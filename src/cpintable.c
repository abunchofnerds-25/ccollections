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
 * @file cpintable.c
 * @brief INTERNAL ONLY. Lock-free handle-to-pointer index with per-slot pins.
 */

#include "cpintable.h"

#include <stdlib.h>

/* Which stripe this thread pins on. Assigned once per thread from a global
 * counter, so concurrent threads land on different stripes and their pins
 * write different cache lines.
 *
 * initial-exec rather than the default general model, matching cmempool's own
 * thread-local state and for the same reason: under the general model every
 * access to a thread-local in a shared library goes through a __tls_get_addr
 * call, which would put a function call on the very path this type exists to
 * keep cheap. The cost is a slot in the static thread-local block, which only
 * affects a library dlopen()ed into a process that has already exhausted it. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread unsigned _pin_stripe_id;
#else
static __thread unsigned _pin_stripe_id
    __attribute__((tls_model("initial-exec")));
#endif
static _Atomic unsigned _pin_stripe_next = 1;

/* Claims this thread's id, once. Out of line deliberately: it runs on a
 * thread's first pin and never again, while pin_stripe_self() below is inlined
 * into both of this file's hot entry points, and code that never executes still
 * costs them through register allocation and code layout. Keeping the whole
 * claim behind a call leaves those two functions the shape they would have with
 * no assignment path at all.
 *
 * The counter wraps after 2^32 assignments, and a thread that took 0 out of it
 * would store the unassigned marker: it would then re-derive its stripe on
 * every call, contending the counter on the hot path and releasing pins on a
 * different stripe from the one it took them on. */
static __attribute__((noinline)) unsigned pin_stripe_claim(void) {
  unsigned id =
      atomic_fetch_add_explicit(&_pin_stripe_next, 1, memory_order_relaxed);
  if (id == 0) id = 1;
  _pin_stripe_id = id;
  return id;
}

/* Zero means "not yet assigned", so ids start at 1 and the stored value is one
 * more than the stripe it selects. */
static inline unsigned pin_stripe_self(void) {
  unsigned id = _pin_stripe_id;
  if (id == 0) id = pin_stripe_claim();
  return (id - 1u) % CCOL_PIN_STRIPES;
}

/* A live slot's state word carries its generation and its liveness together.
 * Keeping them in one word is what lets a resolve validate both with a single
 * load, and lets the post-pin re-check be one comparison rather than a pair of
 * reads that could straddle a retire. */
static inline uint64_t pin_state(uint32_t gen, bool live) {
  return ((uint64_t)gen << 1) | (live ? 1u : 0u);
}

static ccol_pin_slot *pin_slot_lookup(ccol_pintable *t, uint32_t idx) {
  uint32_t chunk_idx = idx / CCOL_PIN_CHUNK_SLOTS;
  if (chunk_idx >= CCOL_PIN_MAX_CHUNKS) return NULL;
  ccol_pin_chunk *chunk =
      atomic_load_explicit(&t->chunks[chunk_idx], memory_order_acquire);
  if (!chunk) return NULL;
  return &chunk->slots[idx % CCOL_PIN_CHUNK_SLOTS];
}

#ifdef RUNNING_UNIT_TESTS
/* See the declaration in cpintable.h. Consumed by the next publish, and read
 * before that publish stores anything, so a rollback sees the same state a
 * genuine allocation failure would leave behind. */
static atomic_bool _pin_force_publish_failure;

/* Forces the stripe block's allocation to fail on the next publish that would
 * perform one, which is the only refusal that can happen after a chunk has
 * already been published. Nothing else reaches that state. */
static _Atomic bool _pin_force_stripe_alloc_failure = false;

void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void) {
  atomic_store(&_pin_force_stripe_alloc_failure, true);
}

void _ccol_pintable_force_next_publish_failure_for_tests(void) {
  atomic_store(&_pin_force_publish_failure, true);
}
#endif

bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr) {
  if (gen == 0) return false;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&_pin_force_publish_failure, false)) return false;
#endif
  uint32_t chunk_idx = idx / CCOL_PIN_CHUNK_SLOTS;
  if (chunk_idx >= CCOL_PIN_MAX_CHUNKS) return false;

  /* The caller holds its own writer lock, so no two publishes race here and a
   * relaxed load is enough to find out whether the chunk already exists. The
   * store below is still a release, because a concurrent reader acquires it. */
  ccol_pin_chunk *chunk =
      atomic_load_explicit(&t->chunks[chunk_idx], memory_order_relaxed);
  if (!chunk) {
    chunk = calloc(1, sizeof(*chunk));
    if (!chunk) return false;
    atomic_store_explicit(&t->chunks[chunk_idx], chunk, memory_order_release);
  }

  ccol_pin_slot *slot = &chunk->slots[idx % CCOL_PIN_CHUNK_SLOTS];
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_relaxed);
  if (!stripes) {
#ifdef RUNNING_UNIT_TESTS
    if (atomic_exchange(&_pin_force_stripe_alloc_failure, false)) return false;
#endif
    stripes = calloc(CCOL_PIN_STRIPES, sizeof(*stripes));
    if (!stripes) return false;
    atomic_store_explicit(&slot->stripes, stripes, memory_order_release);
  }

  atomic_store_explicit(&slot->ptr, ptr, memory_order_relaxed);
  /* Release, and last: a reader that acquires a live state must see the
   * pointer that goes with it. */
  atomic_store_explicit(&slot->state, pin_state(gen, true),
                        memory_order_release);
  return true;
}

void ccol_pintable_retire(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  uint64_t st = atomic_load_explicit(&slot->state, memory_order_relaxed);
  /* Generation is preserved so a handle naming this occupancy stays
   * distinguishable from one naming the next; only liveness is cleared.
   *
   * Sequentially consistent, and it must be. This store and the pin count read
   * that follows it form one half of a store-then-load pair whose other half
   * is in ccol_pintable_pin(); see the note there for why release ordering
   * alone would let both halves miss each other and free an object a caller
   * had already been handed. */
  atomic_store_explicit(&slot->state, st & ~(uint64_t)1, memory_order_seq_cst);
}

void *ccol_pintable_pin(ccol_pintable *t, uint64_t handle) {
  if (handle == 0) return NULL;
  uint32_t idx = (uint32_t)(handle >> 32);
  uint32_t gen = (uint32_t)(handle & 0xFFFFFFFFu);

  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return NULL;

  uint64_t want = pin_state(gen, true);
  if (atomic_load_explicit(&slot->state, memory_order_acquire) != want)
    return NULL;

  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return NULL;

  unsigned stripe = pin_stripe_self();

  /* This increment and the state load after it are sequentially consistent, as
   * are the retiring side's own store and its read of the pin count. All four
   * being seq_cst is what makes the two sides unable to miss each other, and
   * nothing weaker will do.
   *
   * The two sides race in opposite directions: this one writes the pin then
   * reads the liveness, the retiring one writes the liveness then reads the
   * pin. Under acquire and release alone, each side's read may still be
   * satisfied from before the other side's write, so a retiring destroy can
   * read a pin count of zero while this call reads a state that is still live,
   * and the object is then freed with a caller already holding the pointer.
   * With every one of the four operations sequentially consistent there is a
   * single total order over them: whichever write lands first in that order is
   * observed by the other side's read, so either the destroy sees this pin and
   * waits for it, or this call sees the retire and backs out. Exactly one, and
   * never neither.
   *
   * The cost is nothing on x86-64, where the increment is already a locked
   * read-modify-write and a sequentially consistent load is an ordinary load.
   */
  atomic_fetch_add_explicit(&stripes[stripe].count, 1, memory_order_seq_cst);

  if (atomic_load_explicit(&slot->state, memory_order_seq_cst) != want) {
    atomic_fetch_sub_explicit(&stripes[stripe].count, 1, memory_order_release);
    return NULL;
  }

  return atomic_load_explicit(&slot->ptr, memory_order_relaxed);
}

void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle) {
  /* Mirrors ccol_pintable_pin's own guard. A zero handle never yields a pin
   * there, so releasing one here must do nothing: slot 0 is an ordinary,
   * usually occupied slot, and decrementing it on behalf of a pin that was
   * never taken drives its count below the truth, which lets a later drain
   * finish while a real caller still holds the object. The realistic way to
   * arrive here with zero is an owner whose self-handle field has not been
   * written yet, since those structs are normally zero-initialised. */
  if (handle == 0) return;
  uint32_t idx = (uint32_t)(handle >> 32);
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return;
  atomic_fetch_sub_explicit(&stripes[pin_stripe_self()].count, 1,
                            memory_order_release);
}

size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return 0;
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return 0;
  /* Summed signed, then clamped: an individual stripe can legitimately read
   * negative while a pin taken on one stripe is released on another, and only
   * the total is meaningful.
   *
   * Accumulated in an unsigned type and converted once at the end. A workload
   * that systematically pins on one thread and releases on another drives the
   * two stripes apart without bound while their sum stays right, so a partial
   * sum can exceed what a signed accumulator holds long before the total does;
   * unsigned arithmetic wraps by definition, where signed overflow is
   * undefined. */
  uintptr_t accumulated = 0;
  for (unsigned i = 0; i < CCOL_PIN_STRIPES; ++i) {
    accumulated += (uintptr_t)atomic_load_explicit(&stripes[i].count,
                                                   memory_order_seq_cst);
  }
  ptrdiff_t total = (ptrdiff_t)accumulated;
  return total > 0 ? (size_t)total : 0;
}

size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle) {
  /* Same guard as unpin above: a zero handle names no slot, and answering
   * with slot 0's count would report some unrelated object's pins. */
  if (handle == 0) return 0;
  return ccol_pintable_pins(t, (uint32_t)(handle >> 32));
}

void ccol_pintable_dispose(ccol_pintable *t) {
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c) {
    ccol_pin_chunk *chunk =
        atomic_load_explicit(&t->chunks[c], memory_order_acquire);
    if (!chunk) continue;
    /* Unpublished before it is freed, so a resolve racing this finds no chunk
     * and answers NULL rather than reading freed memory. */
    atomic_store_explicit(&t->chunks[c], NULL, memory_order_release);
    for (unsigned i = 0; i < CCOL_PIN_CHUNK_SLOTS; ++i) {
      ccol_pin_stripe *stripes =
          atomic_load_explicit(&chunk->slots[i].stripes, memory_order_acquire);
      atomic_store_explicit(&chunk->slots[i].stripes, NULL,
                            memory_order_release);
      free(stripes);
    }
    free(chunk);
  }
}

void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle) {
  /* Same guard as unpin and pins_for above: a zero handle names no slot, and
   * clearing slot 0's counters on its behalf would drop pins belonging to
   * whichever object happens to occupy that slot. */
  if (handle == 0) return;
  ccol_pintable_reset(t, (uint32_t)(handle >> 32));
}

void ccol_pintable_reset(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return;
  for (unsigned i = 0; i < CCOL_PIN_STRIPES; ++i)
    atomic_store_explicit(&stripes[i].count, 0, memory_order_release);
}
