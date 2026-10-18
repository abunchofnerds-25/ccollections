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
 * Some modules give out an opaque uint64_t handle backed by a
 * generation-tagged slot table. Every public call resolves the handle and
 * has to keep the object alive until the call ends. This type is the hot
 * half of that work: it turns a handle into a pointer and holds the object
 * against a concurrent destroy, without any shared write.
 *
 * Getting this wrong is expensive, and you won't see the cost with one
 * thread. Take a resolve that takes the read side of a process-global lock
 * and increments a counter that every caller shares. That's several
 * read-modify-write atomics on two cache lines that every core wants, so the
 * lines bounce between cores and the call gets slower with every new thread,
 * even when it does nothing else.
 *
 * Here a resolve does plain acquire loads and a pin only touches the calling
 * thread's stripe, so throughput goes up with the thread count instead of
 * collapsing.
 *
 * The header is internal: it carries no visibility block, make install
 * leaves it out, and its symbols aren't in the dynamic symbol table of the
 * shared library.
 *
 * Scope: the owning module keeps its own slot table for all the cold work
 * (iteration, fork handlers, its own bookkeeping). This type holds a copy of
 * only what a resolve needs, and the paths that write it already hold the
 * module's writer lock.
 *
 * The handle encoding is the one every module uses:
 * (index << 32) | generation. The generation is never 0, so a handle whose
 * bits are all zero is always invalid.
 */

#ifndef CCOL_CPINTABLE_H
#define CCOL_CPINTABLE_H

#include <common.h>
#include <stdatomic.h>
#if !defined(__STDC_VERSION__) || __STDC_VERSION__ < 202311L
#include <stdbool.h> /* bool, true and false are keywords in C23 */
#endif
#include <stddef.h>
#include <stdint.h>

/** Slots per chunk. A chunk is never moved or freed, and that's what lets a
 *  reader index into it without holding a lock. */
#define CCOL_PIN_CHUNK_SLOTS 64u

/** Number of chunk pointers in the directory, which is also the ceiling on
 *  live indices. */
#define CCOL_PIN_MAX_CHUNKS 1024u

/** How many indices the table can hold. Each owning module has to respect
 *  this ceiling when it claims a slot of its own, since the table can't
 *  publish an index at or above it and such a handle could never resolve.
 *  The module must refuse such an index instead of claiming it and rolling
 *  back. An index that can never publish must never reach a free list that
 *  a later acquire pops from; otherwise every later acquire pops the same
 *  useless index and the module gives out no more handles for the rest of
 *  the process. */
#define CCOL_PIN_MAX_SLOTS ((size_t)CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS)

/** Independent pin counters per slot. Each counter sits on its own cache
 *  line and a thread pins on the stripe its own id selects, so concurrent
 *  pins of one object write different lines and never contend. */
#define CCOL_PIN_STRIPES 8u

/** The cache line size we assume. Too large costs a bit of memory per live
 *  handle. Too small puts two stripes on one line, which brings back exactly
 *  the sharing this type is there to remove.
 *
 *  What keeps two stripes apart is the STRIDE this padding gives, not the
 *  alignment of the block holding them. That matters because the block comes
 *  from plain calloc, which usually doesn't start it on a line boundary. We
 *  only write the count at the front of each stripe, and two consecutive
 *  counts are exactly this far apart whatever the block's alignment, so no
 *  two counts share a line; only padding that nobody reads crosses a line
 *  boundary.
 *
 *  A stronger block alignment isn't worth an allocator that can give it. We
 *  measured it against a build identical in every other way, on one shared
 *  slot at 1, 4, 8 and 12 threads, and saw no change beyond the run-to-run
 *  variation. */
#define CCOL_PIN_LINE 64u

/* The counter is signed on purpose. A stripe goes negative when a release
 * lands on a different stripe than its pin, and only the sum over all
 * stripes means anything. With an unsigned counter it would wrap to a huge
 * value and a destroy would wait for a pin that doesn't exist. Atomic
 * arithmetic on a signed integer type is defined to wrap, so the negative
 * intermediate value is well defined and not correct by luck.
 *
 * This tolerance makes the drain robust but not general. It keeps the sum
 * of a temporarily split count right, but it does NOT allow releasing a pin
 * from another thread. See ccol_pintable_unpin(). */
typedef struct {
  _Atomic ptrdiff_t count;
  char pad[CCOL_PIN_LINE - sizeof(_Atomic ptrdiff_t)];
} ccol_pin_stripe;

typedef struct {
  /* (generation << 1) | live. Both live in one word, so a reader gets a
   * consistent snapshot of the two with one acquire load, and the check
   * after the pin below is a plain comparison. */
  _Atomic uint64_t state;
  /* Published before the slot goes live and not written again while it is
   * live, so an acquire load of a live state sees this value too. */
  void *_Atomic ptr;
  /* Allocated by the first publish into this slot and kept for the life of
   * the process. A reader may already hold this pointer, so nothing may free
   * the block under it, and a slot is reused far more often than it is
   * created anyway. */
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
 * Call it with the owning module's writer lock held, from the same place
 * that marks the module's slot in use. The chunk and the stripe block are
 * allocated the first time the index is used.
 *
 * @param gen The generation for this occupancy. It must not be 0.
 * @return false if the index is above the table's ceiling or an allocation
 *         failed, and in that case nothing is published.
 */
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr);

/**
 * @brief Stop a new resolve from finding the object at this index.
 *
 * Call it with the owning module's writer lock held, from the same place
 * that clears the module's in-use flag. Pins the table has already granted
 * aren't touched, so wait for them with ccol_pintable_pins() before you free
 * the object.
 */
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx);

/**
 * @brief Resolve a handle and pin the object against a concurrent destroy.
 *
 * Lock-free, and it writes no memory that another thread reads.
 *
 * @return The published pointer, or NULL if the handle is 0, is out of
 *         range, or names a slot that is retired or on a different
 *         generation. A NULL result takes no pin, and you must not call
 *         unpin after it.
 */
void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle);

/**
 * @brief Release a pin taken by ccol_pintable_pin(). Lock-free.
 *
 * Pass the same handle as the pin, and call it on the thread that took the
 * pin. Every thread picks its own stripe, so a release on another thread
 * splits one pin across two stripes. Handing a pin to another thread to
 * release looks harmless, and it isn't, for the reason below.
 *
 * ccol_pintable_pins() reads the stripes one by one and not as a single
 * snapshot. It can see the increment on the first stripe before the split
 * and the decrement on the second stripe after it, and then the sum is zero
 * while a caller still holds the pin. A destroy waiting on that count would
 * free the object while a caller is inside it.
 *
 * Because the counters are signed, a split sums correctly once both sides
 * are visible. That's what makes the drain converge, but it doesn't make the
 * intermediate reads safe.
 *
 * A zero handle does nothing, matching ccol_pintable_pin(), which never
 * grants a pin for a zero handle.
 */
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle);

/**
 * @brief The number of pins outstanding against an index, summed over the
 *        stripes.
 *
 * Meant for a destroy that has retired the index and waits for the callers
 * in flight to finish. Zero means every pin a caller took before this call
 * has been released, and together with the retire it means no new pin can
 * appear. That holds because the thread that takes a pin is the one that
 * releases it, so no single pin is ever split across two of the stripes we
 * sum one by one. See ccol_pintable_unpin().
 */
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_pins() for the index a handle names. It gives zero
 *         for a zero handle, because that handle names no slot. */
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle);

/**
 * @brief Free every chunk and every stripe block that this table owns.
 *
 * We never free the slot storage while the table is in use, on purpose,
 * since readers index it without a lock. So the storage is still reachable
 * at process exit, and a leak checker that counts still-reachable memory as
 * an error reports it.
 *
 * That's why the table's owner disposes of the storage once nothing can
 * resolve against it any more. A resolve that starts after that finds no
 * chunk and answers NULL, the same answer it gives for any handle it doesn't
 * know.
 *
 * Only call this when no call into the owning module can be in flight. Each
 * chunk is unpublished before it is freed, so a resolve that hasn't loaded
 * the chunk pointer yet answers NULL instead of reading freed memory.
 * Nothing protects a resolve that is already past that load though; this is
 * an entry point for process teardown, not a concurrent one.
 */
void ccol_pintable_dispose(ccol_pintable *t);

/**
 * @brief Drop every outstanding pin against an index.
 *
 * Only for a fork() child handler. fork() copies just the calling thread,
 * and another thread may hold a pin at the moment of the fork. Nothing in
 * the child can release that pin since the thread that would do it doesn't
 * exist there, so a destroy waiting for the count to drain would wait
 * forever. The child has a single thread, so no caller that is really in
 * flight can survive and be miscounted.
 */
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_reset() for the index a handle names. A zero handle
 *         does nothing because it names no slot: slot 0 is an ordinary
 *         slot, and some unrelated object probably sits in it. */
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle);

#endif /* CCOL_CPINTABLE_H */
