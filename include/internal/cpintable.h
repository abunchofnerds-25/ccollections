/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file cpintable.h
 * @brief INTERNAL ONLY. Lock-free handle-to-pointer index with per-slot pins.
 *
 * Some modules give out an opaque uint64_t handle. A generation-tagged slot
 * table backs each such handle. Each public call resolves the handle, and
 * must keep the object alive until the call ends. This type is the hot half
 * of that work. It turns a handle into a pointer, and it holds the object
 * against a concurrent destroy. It does all of this with no shared write.
 *
 * The cost of a wrong design here is large, and one thread does not show it.
 * Consider a resolve that takes the read side of a process-global lock and
 * increments a counter that every caller shares. It does several
 * read-modify-write atomic operations on two cache lines that every core
 * wants. Each line then moves between the cores, and the call becomes slower
 * with each new thread, even when the call does no other work.
 *
 * In this type a resolve does plain acquire loads. A pin touches only the
 * stripe of the thread that calls it. This is why the throughput rises with
 * the thread count, and does not collapse.
 *
 * This header is internal. It carries no visibility block. make install does
 * not install it. Its symbols are absent from the dynamic symbol table of the
 * shared library.
 *
 * Scope: the owning module keeps its own slot table for everything that is
 * cold (iteration, fork handlers, bookkeeping for that module). This type
 * holds a copy of only what a resolve needs. The paths that write it already
 * hold the writer lock of that module.
 *
 * The handle encoding is the same one that every module already uses:
 * (index << 32) | generation. The generation is never 0. This is why a handle
 * of all zero bits is always invalid.
 */

#ifndef CCOL_CPINTABLE_H
#define CCOL_CPINTABLE_H

#include <common.h>
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* C23 has bool, true and false as keywords */
#endif
#include <stddef.h>
#include <stdint.h>

/** The number of slots in each chunk. The library never moves a chunk and
 *  never frees a chunk. This is what lets a reader index one chunk with no
 *  lock held. */
#define CCOL_PIN_CHUNK_SLOTS 64u

/** The number of chunk pointers in the directory. This is also the ceiling on
 *  the live indices. */
#define CCOL_PIN_MAX_CHUNKS 1024u

/** The number of indices that this table can hold. This is also the ceiling
 *  that each owning module must respect when it claims a slot of its own. The
 *  table cannot publish an index at or above this value, and the handle is
 *  then impossible to resolve. A module must refuse such an index. It must
 *  not claim the index and then roll back. An index that can never publish
 *  must never reach a free list that a later acquire pops from. If it does,
 *  every later acquire pops the same unusable index, and the module gives out
 *  no more handles for the rest of the process. */
#define CCOL_PIN_MAX_SLOTS ((size_t)CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS)

/** The number of independent pin counters for each slot. Each counter is on
 *  its own cache line. A thread pins on the stripe that its own id selects.
 *  This is why concurrent pins of one object write different lines and never
 *  contend. */
#define CCOL_PIN_STRIPES 8u

/** The assumed size of a cache line. A value that is too large costs a small
 *  quantity of memory for each live handle. A value that is too small puts
 *  two stripes on one line. That brings back the exact sharing that this type
 *  exists to remove.
 *
 *  The STRIDE that this padding makes is what separates two stripes. The
 *  alignment of the block that holds them does not separate them. This
 *  difference is important, because plain calloc allocates the block, and the
 *  block usually does not start on a line boundary. The code writes only the
 *  count at the front of each stripe. Two consecutive counts are exactly this
 *  distance apart for any alignment of the block, so no two counts share a
 *  line. Only padding that nobody reads crosses a line boundary.
 *
 *  A stronger alignment for the block is not worth an allocator that can give
 *  it. A measurement against a build that is identical in every other way, on
 *  one shared slot, at 1, 4, 8 and 12 threads, shows no change outside the
 *  variation between runs. */
#define CCOL_PIN_LINE 64u

/* The counter is signed, and this is deliberate. A stripe reads negative when
 * a release lands on a stripe that is not the one of the pin. Only the sum
 * across the stripes has a meaning. An unsigned counter would wrap to an
 * enormous value, and a destroy would then wait for a pin that does not
 * exist. Atomic arithmetic on a signed integer type is defined to wrap. This
 * is why the intermediate negative value is well defined, and not only
 * correct by chance.
 *
 * That tolerance makes the drain robust. It does not make it general. It
 * keeps the sum of a count that is temporarily split correct. It is NOT
 * permission to release a pin from another thread. See
 * ccol_pintable_unpin(). */
typedef struct {
  _Atomic ptrdiff_t count;
  char pad[CCOL_PIN_LINE - sizeof(_Atomic ptrdiff_t)];
} ccol_pin_stripe;

typedef struct {
  /* (generation << 1) | live. The two values share one word. A reader takes a
   * consistent snapshot of both with one acquire load. The validation after
   * the pin below is then a plain comparison. */
  _Atomic uint64_t state;
  /* The code publishes this before the slot becomes live. It does not write
   * it again while the slot is live. This is why an acquire load of a live
   * state also sees this value. */
  void *_Atomic ptr;
  /* The first publish into this slot allocates this block. The block then
   * stays for the life of the process. A reader can already hold this
   * pointer, and nothing must free the block under that reader. A slot is
   * also reused much more often than it is created. */
  ccol_pin_stripe *_Atomic stripes;
} ccol_pin_slot;

typedef struct {
  ccol_pin_slot slots[CCOL_PIN_CHUNK_SLOTS];
} ccol_pin_chunk;

typedef struct {
  ccol_pin_chunk *_Atomic chunks[CCOL_PIN_MAX_CHUNKS];
} ccol_pintable;

/**
 * @brief Publish an object at an index so that a resolve can find it.
 *
 * Call this function with the writer lock of the owning module held. Call it
 * from the same place that marks the slot of that module in use. The function
 * allocates the chunk and the stripe block at the first use of the index.
 *
 * @param gen The generation for this occupancy. It must not be 0.
 * @return false if the index is above the ceiling of the table, or if an
 *         allocation failed. The function then publishes nothing.
 */
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr);

/**
 * @brief Stop a new resolve from finding the object at this index.
 *
 * Call this function with the writer lock of the owning module held. Call it
 * from the same place that clears the in-use flag of that module. The
 * function does not change a pin that the table already granted. Wait for
 * those pins with ccol_pintable_pins() before you free the object.
 */
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx);

/**
 * @brief Resolve a handle and pin the object against a concurrent destroy.
 *
 * This function is lock-free. It writes to no memory that another thread
 * reads.
 *
 * @return The published pointer. The result is NULL if the handle is 0, if
 *         the handle is out of range, or if the handle names a slot that is
 *         retired or on a different generation. On a NULL result the function
 *         takes no pin, and you must not call unpin.
 */
void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle);

/**
 * @brief Release a pin taken by ccol_pintable_pin(). Lock-free.
 *
 * This function takes the same handle as the pin. It must run on the thread
 * that took the pin. Each thread selects its own stripe. A release on another
 * thread splits one pin across two stripes.
 *
 * ccol_pintable_pins() reads the stripes one at a time, and not as one
 * snapshot. It can read the increment on the first stripe before the split,
 * and the decrement on the second stripe after the split. The sum is then
 * zero while a caller still holds the pin. A destroy that waits on that count
 * frees the object while a caller is inside it.
 *
 * The counters are signed, so a split still sums correctly after both sides
 * are visible. This is what makes the drain converge. It does not make the
 * intermediate reads safe.
 *
 * A zero handle is a no-op. This is the same as ccol_pintable_pin(), which
 * never grants a pin for a zero handle.
 */
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle);

/**
 * @brief The number of pins that are outstanding against an index, summed
 *        across the stripes.
 *
 * This is for a destroy that already retired the index. That destroy now
 * waits for the callers that are in flight to finish. A result of zero means
 * that every pin that a caller took before this call is released. Together
 * with the retire, this means that no new pin can appear. This is true
 * because the thread that takes a pin is the thread that releases it. No
 * single pin is ever split across two of the stripes that this function sums
 * one at a time. See ccol_pintable_unpin().
 */
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_pins() for the index that a handle names. The result
 *         is zero for a zero handle, because that handle names no slot. */
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle);

/**
 * @brief Free every chunk and every stripe block that this table owns.
 *
 * The library never frees the slot storage while the table is in use. This is
 * deliberate, because a reader indexes that storage with no lock held. The
 * storage stays reachable at process exit. A leak checker that treats
 * still-reachable memory as an error reports it.
 *
 * This is why the owner of the table disposes of the storage after nothing
 * can resolve against it. A resolve that starts after that point finds no
 * chunk and answers NULL. This is how it already answers for any handle that
 * it does not recognise.
 *
 * Call this function only after no call into the owning module can still be
 * in flight. The function unpublishes each chunk before it frees that chunk.
 * A resolve that did not yet load the chunk pointer then answers NULL, and
 * does not read freed memory. But nothing here protects a resolve that is
 * already past that load. This is an entry point for process teardown, and
 * not a concurrent one.
 */
void ccol_pintable_dispose(ccol_pintable *t);

/**
 * @brief Drop every outstanding pin against an index.
 *
 * This function is only for a fork() child handler. fork() copies only the
 * thread that calls it. Another thread can hold a pin at the instant of the
 * fork. Nothing in the child can release that pin, because the thread that
 * would release it does not exist there. A destroy that waits for the count
 * to drain would wait forever. The child has one thread, so no caller that is
 * truly in flight can survive and be counted wrongly.
 */
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_reset() for the index that a handle names. A zero
 *         handle is a no-op, because it names no slot. Slot 0 is an ordinary
 *         slot, and an unrelated object probably occupies it. */
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle);

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Force the next ccol_pintable_publish() call to fail, once.
 *
 * A publish is the last step of every handle acquisition. Its failure path is
 * the rollback that the owning module runs to put a half-claimed slot back on
 * its free list. Nothing else reaches that path without a real allocation
 * failure inside this file. No allocator that a caller gives can reach that
 * failure. Plain calloc makes the chunk and the stripe blocks, by design,
 * because a reader indexes them with no lock held.
 *
 * The forced failure happens before the function stores anything. A rollback
 * sees exactly the state that a real allocation failure leaves. The arm is
 * one-shot. The next publish consumes the flag, and it does this whether that
 * publish succeeds or fails.
 *
 * The build compiles this only under RUNNING_UNIT_TESTS. It is absent from
 * the shipped library and from the dynamic symbol table of that library.
 */
void _ccol_pintable_force_next_publish_failure_for_tests(void);

/**
 * @brief Force the next stripe-block allocation to fail.
 *
 * This is the one refusal that can happen after the table already published a
 * chunk. It is the only way to reach a table that holds a chunk whose slot
 * never became live. The build compiles this only under RUNNING_UNIT_TESTS.
 */
void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);
#endif /* RUNNING_UNIT_TESTS */

#endif /* CCOL_CPINTABLE_H */
