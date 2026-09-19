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
 * Every module that hands out an opaque uint64_t handle backed by a
 * generation-tagged slot table resolves that handle on each public call, and
 * must keep the object alive for the duration of the call. This type is the
 * hot half of that: turning a handle into a pointer, and holding the object
 * against a concurrent destroy, without any shared write.
 *
 * The cost of getting that wrong is not small and does not show up on one
 * thread. A resolve that read-acquires a process-global lock and increments a
 * counter shared by every caller performs several read-modify-write atomics on
 * two cache lines that every core wants, so the line ping-pongs between them
 * and the call gets slower as threads are added, whether or not it goes on to
 * do any work. Here a resolve performs plain acquire loads, and a pin touches
 * only the calling thread's own stripe, so throughput rises with thread count
 * instead of collapsing.
 *
 * This header is internal. It carries no visibility block, is excluded from
 * make install, and its symbols are absent from the shared library's dynamic
 * symbol table.
 *
 * Scope: the owning module keeps its own slot table for everything cold
 * (iteration, fork handlers, per-module bookkeeping). This type mirrors only
 * what a resolve needs, and is written from the paths that already hold that
 * module's own writer lock.
 *
 * Handle encoding matches what every module already uses: (index << 32) |
 * generation, with generation never 0, so a zeroed handle is always invalid.
 */

#ifndef CCOL_CPINTABLE_H
#define CCOL_CPINTABLE_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "common.h"

/** Slots per chunk. Chunks are never moved or freed, which is what lets a
 *  reader index one with no lock held. */
#define CCOL_PIN_CHUNK_SLOTS 64u

/** Chunk pointers in the directory, and so the ceiling on live indices. */
#define CCOL_PIN_MAX_CHUNKS 1024u

/** Indices this table can hold, and so the ceiling every owning module has to
 *  respect when it claims a slot of its own. An index at or above this cannot
 *  be published, which makes the handle unresolvable, so a module must refuse
 *  to claim one rather than claim it and roll back: an index that can never
 *  publish must never reach a free list a later acquire pops from, or every
 *  later acquire pops the same unusable index and the module stops handing out
 *  handles for the rest of the process. */
#define CCOL_PIN_MAX_SLOTS ((size_t)CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS)

/** Independent pin counters per slot, each on its own cache line. A thread
 *  pins on the stripe its own id selects, so concurrent pins of one object
 *  write different lines and never contend. */
#define CCOL_PIN_STRIPES 8u

/** Assumed cache line. Over-estimating costs a little memory per live handle;
 *  under-estimating would put two stripes on one line and reintroduce exactly
 *  the sharing this type exists to remove.
 *
 *  What separates two stripes is the STRIDE this padding creates, not the
 *  alignment of the block holding them, and the distinction is worth stating
 *  because the block is allocated with plain calloc and so usually does not
 *  begin on a line boundary. Only the count at the front of each stripe is
 *  ever written, and consecutive counts are exactly this far apart whatever
 *  the block's own alignment, so no two of them ever share a line; what
 *  straddles a boundary is padding nobody reads. Over-aligning the block is
 *  therefore not worth an allocator that can honour it: measured against an
 *  otherwise identical build on a single shared slot at 1, 4, 8 and 12
 *  threads, it changes nothing outside run-to-run variation. */
#define CCOL_PIN_LINE 64u

/* Signed, and deliberately so. A stripe reads negative whenever a release lands
 * on a different one from the pin, and only the sum across stripes is
 * meaningful; an unsigned counter would wrap to an enormous value instead and a
 * destroy would wait out a pin that does not exist. Atomic arithmetic on a
 * signed integer type is defined to wrap, so the intermediate negative value is
 * well defined rather than merely working in practice.
 *
 * That tolerance makes the drain robust, not general: it is what keeps a
 * transiently split count summing correctly, and it is NOT a licence to release
 * a pin from another thread. See ccol_pintable_unpin(). */
typedef struct {
  _Atomic ptrdiff_t count;
  char pad[CCOL_PIN_LINE - sizeof(_Atomic ptrdiff_t)];
} ccol_pin_stripe;

typedef struct {
  /* (generation << 1) | live. One word so a reader takes a consistent
   * snapshot of both with a single acquire load, and so the validate-after-pin
   * below is a plain comparison. */
  _Atomic uint64_t state;
  /* Published before the slot goes live and not written again while it is, so
   * an acquire load of a live state also observes this. */
  void *_Atomic ptr;
  /* Allocated on first publish into this slot and then kept for the life of
   * the process: a reader that has already loaded this pointer must not have
   * it freed underneath, and a slot is reused far more often than it is
   * created. */
  ccol_pin_stripe *_Atomic stripes;
} ccol_pin_slot;

typedef struct {
  ccol_pin_slot slots[CCOL_PIN_CHUNK_SLOTS];
} ccol_pin_chunk;

typedef struct {
  ccol_pin_chunk *_Atomic chunks[CCOL_PIN_MAX_CHUNKS];
} ccol_pintable;

/**
 * @brief Publish an object at an index so resolves can find it.
 *
 * Call with the owning module's writer lock held, from the same place that
 * marks its own slot in use. Allocates the chunk and stripe block if this is
 * the index's first use.
 *
 * @param gen Generation for this occupancy; must not be 0.
 * @return false if the index is beyond the table's ceiling or an allocation
 *         failed, in which case nothing was published.
 */
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr);

/**
 * @brief Stop new resolves from finding the object at this index.
 *
 * Call with the owning module's writer lock held, from the same place that
 * clears its own in-use flag. Already-granted pins are unaffected; wait for
 * them with ccol_pintable_pins() before freeing the object.
 */
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx);

/**
 * @brief Resolve a handle and pin the object against a concurrent destroy.
 *
 * Lock-free, and performs no write to any memory another thread reads.
 *
 * @return The published pointer, or NULL if the handle is 0, out of range, or
 *         names a slot that is retired or on a different generation. On NULL
 *         no pin was taken and unpin must not be called.
 */
void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle);

/**
 * @brief Release a pin taken by ccol_pintable_pin(). Lock-free.
 *
 * Takes the same handle the pin did, and must run on the thread that pinned.
 * The stripe is chosen per thread, so releasing elsewhere splits one pin across
 * two stripes; ccol_pintable_pins() reads the stripes one at a time rather than
 * as one snapshot, so a split pin can be observed as the increment on one
 * stripe before it, and the decrement on the other after it, summing to zero
 * while the pin is still held. A destroy waiting on that count would then free
 * the object with a caller inside it. The counters are signed so that a split
 * still sums correctly once both sides are visible, which is what makes the
 * drain converge; it does not make the intermediate reads safe.
 *
 * A zero handle is a no-op, mirroring ccol_pintable_pin(), which never grants
 * a pin for one.
 */
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle);

/**
 * @brief Pins currently outstanding against an index, summed across stripes.
 *
 * For a destroy that has already retired the index and now waits for in-flight
 * callers to finish. Reading zero means every pin taken before this call has
 * been released, which together with the retire means no new one can appear.
 * That holds because every pin is released on the thread that took it, so no
 * single pin is ever split across two of the stripes this sums one at a time;
 * see ccol_pintable_unpin().
 */
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_pins() for the index a handle names. Zero for a zero
 *         handle, which names no slot. */
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle);

/**
 * @brief Release every chunk and stripe block this table owns.
 *
 * Slot storage is deliberately never freed while the table is in use, because
 * a reader indexes it with no lock held. That leaves it reachable at process
 * exit, which a leak checker configured to treat still-reachable memory as an
 * error reports, so the owner disposes of it once nothing can resolve against
 * it any more. A resolve that starts afterwards finds no chunk and answers
 * NULL, which is how it already answers for any handle it does not recognise.
 *
 * Call it only once no call into the owning module can still be in flight.
 * Each chunk is unpublished before it is freed, which is what makes a resolve
 * that has not yet loaded the chunk pointer answer NULL rather than read freed
 * memory, but a resolve already past that load is not covered by anything
 * here: this is a process-teardown entry point, not a concurrent one.
 */
void ccol_pintable_dispose(ccol_pintable *t);

/**
 * @brief Drop every outstanding pin against an index.
 *
 * Only for a fork() child handler. fork() duplicates the calling thread alone,
 * so a pin another thread held at the instant of the fork can never be released
 * in the child: the thread that would release it does not exist there, and a
 * destroy waiting for the count to drain would wait forever. The child has one
 * thread, so no genuinely in-flight caller can survive to be miscounted.
 */
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx);

/** @brief ccol_pintable_reset() for the index a handle names. A zero handle is
 *         a no-op: it names no slot, and slot 0 is an ordinary slot that some
 *         unrelated object is likely to occupy. */
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle);

#ifdef RUNNING_UNIT_TESTS
/**
 * @brief Force the next ccol_pintable_publish() call to fail, once.
 *
 * Publishing is the last step of every handle acquisition, so its failure path
 * is the rollback the owning module runs to put a half-claimed slot back on its
 * free list. That path is otherwise unreachable without a real allocation
 * failure inside this file, which no caller-supplied allocator can reach: the
 * chunk and stripe blocks come from plain calloc by design, since a reader
 * indexes them with no lock held.
 *
 * The forced failure happens before anything at all is stored, so a rollback
 * observes exactly the state a genuine allocation failure would leave. Arming
 * is one-shot: the flag is consumed by the next publish, successful or not.
 *
 * Compiled only under RUNNING_UNIT_TESTS, so it is absent from the shipped
 * library and from its dynamic symbol table.
 */
void _ccol_pintable_force_next_publish_failure_for_tests(void);

/**
 * @brief Force the next stripe-block allocation to fail.
 *
 * The one refusal that can happen after a chunk has already been published,
 * and so the only way to reach a table holding a chunk whose slot was never
 * made live. Compiled only under RUNNING_UNIT_TESTS.
 */
void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);
#endif /* RUNNING_UNIT_TESTS */

#endif /* CCOL_CPINTABLE_H */
