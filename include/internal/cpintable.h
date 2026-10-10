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
 * Some modules give out an opaque uint64_t handle, backed by a
 * generation-tagged slot table. Each public call resolves the handle and
 * must keep the object alive until the call ends. This type is the hot half
 * of that work: it turns a handle into a pointer and holds the object
 * against a concurrent destroy, all with no shared write.
 *
 * A wrong design here is costly, and one thread does not show the cost.
 * Consider a resolve that takes the read side of a process-global lock and
 * increments a counter that every caller shares: it does several
 * read-modify-write atomic operations on two cache lines that every core
 * wants, so each line moves between the cores and the call becomes slower
 * with each new thread, even when it does no other work.
 *
 * In this type a resolve does plain acquire loads, and a pin touches only
 * the stripe of the calling thread, which is why the throughput rises with
 * the thread count instead of collapsing.
 *
 * This header is internal: it carries no visibility block, make install does
 * not install it, and its symbols are absent from the dynamic symbol table
 * of the shared library.
 *
 * Scope: the owning module keeps its own slot table for everything that is
 * cold (iteration, fork handlers, bookkeeping for that module). This type
 * holds a copy of only what a resolve needs, and the paths that write it
 * already hold the writer lock of that module.
 *
 * The handle encoding is the one that every module uses:
 * (index << 32) | generation. The generation is never 0, so a handle of all
 * zero bits is always invalid.
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

/** The number of slots in each chunk. The library never moves or frees a
 *  chunk, which is what lets a reader index one chunk with no lock held. */
#define CCOL_PIN_CHUNK_SLOTS 64u

/** The number of chunk pointers in the directory, which is also the ceiling
 *  on the live indices. */
#define CCOL_PIN_MAX_CHUNKS 1024u

/** The number of indices that this table can hold. This is also the ceiling
 *  that each owning module must respect when it claims a slot of its own:
 *  the table cannot publish an index at or above this value, so such a
 *  handle could never be resolved. A module must refuse such an index rather
 *  than claim it and then roll back. An index that can never publish must
 *  never reach a free list that a later acquire pops from; otherwise every
 *  later acquire pops the same unusable index, and the module gives out no
 *  more handles for the rest of the process. */
#define CCOL_PIN_MAX_SLOTS ((size_t)CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS)

/** The number of independent pin counters for each slot. Each counter sits
 *  on its own cache line, and a thread pins on the stripe that its own id
 *  selects, so concurrent pins of one object write different lines and
 *  never contend. */
#define CCOL_PIN_STRIPES 8u

/** The assumed size of a cache line. A value that is too large costs a
 *  little memory for each live handle; a value that is too small puts two
 *  stripes on one line, which brings back exactly the sharing that this type
 *  exists to remove.
 *
 *  What separates two stripes is the STRIDE that this padding makes, not the
 *  alignment of the block that holds them. This matters because plain calloc
 *  allocates the block, which usually does not start on a line boundary. The
 *  code writes only the count at the front of each stripe, and two
 *  consecutive counts are exactly this distance apart for any alignment of
 *  the block, so no two counts share a line; only padding that nobody reads
 *  crosses a line boundary.
 *
 *  A stronger alignment for the block is not worth an allocator that can
 *  give it: a measurement against a build that is identical in every other
 *  way, on one shared slot, at 1, 4, 8 and 12 threads, shows no change
 *  outside the variation between runs. */
#define CCOL_PIN_LINE 64u

/* The counter is signed on purpose. A stripe reads negative when a release
 * lands on a stripe other than the one of the pin, and only the sum across
 * the stripes has a meaning. An unsigned counter would wrap to an enormous
 * value, and a destroy would then wait for a pin that does not exist. Atomic
 * arithmetic on a signed integer type is defined to wrap, so the
 * intermediate negative value is well defined and not merely correct by
 * chance.
 *
 * That tolerance makes the drain robust, not general: it keeps the sum of a
 * temporarily split count correct, but it is NOT permission to release a pin
 * from another thread. See ccol_pintable_unpin(). */
typedef struct {
  _Atomic ptrdiff_t count;
  char pad[CCOL_PIN_LINE - sizeof(_Atomic ptrdiff_t)];
} ccol_pin_stripe;

typedef struct {
  /* (generation << 1) | live. The two values share one word, so a reader
   * takes a consistent snapshot of both with one acquire load, and the
   * validation after the pin below is a plain comparison. */
  _Atomic uint64_t state;
  /* The code publishes this before the slot becomes live and does not write
   * it again while the slot is live, so an acquire load of a live state also
   * sees this value. */
  void *_Atomic ptr;
  /* The first publish into this slot allocates this block, which then stays
   * for the life of the process: a reader can already hold this pointer, and
   * nothing must free the block under that reader. A slot is also reused
   * much more often than it is created. */
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
 * Call this function with the writer lock of the owning module held, from
 * the same place that marks the slot of that module in use. The function
 * allocates the chunk and the stripe block at the first use of the index.
 *
 * @param gen The generation for this occupancy. It must not be 0.
 * @return false if the index is above the ceiling of the table or an
 *         allocation failed; the function then publishes nothing.
 */
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr);

/**
 * @brief Stop a new resolve from finding the object at this index.
 *
 * Call this function with the writer lock of the owning module held, from
 * the same place that clears the in-use flag of that module. The function
 * does not change a pin that the table has already granted, so wait for
 * those pins with ccol_pintable_pins() before you free the object.
 */
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx);

/**
 * @brief Resolve a handle and pin the object against a concurrent destroy.
 *
 * This function is lock-free and writes to no memory that another thread
 * reads.
 *
 * @return The published pointer, or NULL if the handle is 0, is out of
 *         range, or names a slot that is retired or on a different
 *         generation. On a NULL result the function takes no pin, and you
 *         must not call unpin.
 */
void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle);

/**
 * @brief Release a pin taken by ccol_pintable_pin(). Lock-free.
 *
 * This function takes the same handle as the pin, and it must run on the
 * thread that took the pin. Each thread selects its own stripe, so a release
 * on another thread splits one pin across two stripes.
 *
 * ccol_pintable_pins() reads the stripes one at a time rather than as one
 * snapshot. It can read the increment on the first stripe before the split
 * and the decrement on the second stripe after it, and the sum is then zero
 * while a caller still holds the pin. A destroy that waits on that count
 * frees the object while a caller is inside it.
 *
 * The counters are signed, so a split sums correctly once both sides are
 * visible. That is what makes the drain converge; it does not make the
 * intermediate reads safe.
 *
 * A zero handle is a no-op, matching ccol_pintable_pin(), which never grants
 * a pin for a zero handle.
 */
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle);

/**
 * @brief The number of pins that are outstanding against an index, summed
 *        across the stripes.
 *
 * This is for a destroy that has retired the index and waits for the
 * callers in flight to finish. A result of zero means that every pin that a
 * caller took before this call is released, and together with the retire
 * this means that no new pin can appear. This holds because the thread that
 * takes a pin is the thread that releases it, so no single pin is ever
 * split across two of the stripes that this function sums one at a time.
 * See ccol_pintable_unpin().
 */
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_pins() for the index that a handle names. The result
 *         is zero for a zero handle, because that handle names no slot. */
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle);

/**
 * @brief Free every chunk and every stripe block that this table owns.
 *
 * The library deliberately never frees the slot storage while the table is
 * in use, because a reader indexes that storage with no lock held. The
 * storage therefore stays reachable at process exit, and a leak checker that
 * treats still-reachable memory as an error reports it.
 *
 * For this reason the owner of the table disposes of the storage once
 * nothing can resolve against it. A resolve that starts after that point
 * finds no chunk and answers NULL, which is how it answers for any handle
 * that it does not recognise.
 *
 * Call this function only when no call into the owning module can still be
 * in flight. The function unpublishes each chunk before it frees that chunk,
 * so a resolve that has not yet loaded the chunk pointer answers NULL
 * instead of reading freed memory. Nothing here protects a resolve that is
 * already past that load, however: this is an entry point for process
 * teardown, not a concurrent one.
 */
void ccol_pintable_dispose(ccol_pintable *t);

/**
 * @brief Drop every outstanding pin against an index.
 *
 * This function is only for a fork() child handler. fork() copies only the
 * calling thread, and another thread can hold a pin at the instant of the
 * fork. Nothing in the child can release that pin, because the thread that
 * would release it does not exist there, so a destroy that waits for the
 * count to drain would wait forever. The child has one thread, so no caller
 * that is truly in flight can survive and be counted wrongly.
 */
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_reset() for the index that a handle names. A zero
 *         handle is a no-op, because it names no slot: slot 0 is an
 *         ordinary slot, and an unrelated object probably occupies it. */
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle);

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Force the next ccol_pintable_publish() call to fail, once.
 *
 * A publish is the last step of every handle acquisition, and its failure
 * path is the rollback that the owning module runs to put a half-claimed
 * slot back on its free list. Nothing else reaches that path without a real
 * allocation failure inside this file, and no allocator that a caller gives
 * can cause that failure: plain calloc makes the chunk and the stripe blocks
 * by design, because a reader indexes them with no lock held.
 *
 * The forced failure happens before the function stores anything, so a
 * rollback sees exactly the state that a real allocation failure leaves. The
 * arm is one-shot: the next publish consumes the flag, whether that publish
 * succeeds or fails.
 *
 * The build compiles this only under RUNNING_UNIT_TESTS, so it is absent
 * from the shipped library and from the dynamic symbol table of that
 * library.
 */
void _ccol_pintable_force_next_publish_failure_for_tests(void);

/**
 * @brief Force the next stripe-block allocation to fail.
 *
 * This is the one refusal that can happen after the table has published a
 * chunk, and the only way to reach a table that holds a chunk whose slot
 * never became live. The build compiles this only under RUNNING_UNIT_TESTS.
 */
void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);
#endif /* RUNNING_UNIT_TESTS */

#endif /* CCOL_CPINTABLE_H */
