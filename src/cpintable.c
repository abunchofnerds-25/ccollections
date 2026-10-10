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

#include "internal/cpintable.h"

#include <internal/ctlsmodel.h>
#include <stdlib.h>

/* The stripe that this thread pins on. Each thread takes its value one time
 * from a global counter, so concurrent threads land on different stripes and
 * their pins write different cache lines.
 *
 * The model is initial-exec instead of the default general model, as for the
 * thread-local state of cmempool and for the same reason: under the general
 * model, every access to a thread-local in a shared library goes through a
 * __tls_get_addr call, which would sit on the exact path that this type
 * exists to keep cheap. The cost is one slot in the static thread-local
 * block, which only affects a library that a process dlopen()s after that
 * process already used every slot. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread unsigned _pin_stripe_id;
#else
static __thread unsigned _pin_stripe_id
    __attribute__((tls_model("initial-exec")));
#endif
static _Atomic unsigned _pin_stripe_next = 1;

/* Claims the id of this thread, one time. This function is out of line on
 * purpose, because it runs on the first pin of a thread and never again. The
 * compiler inlines pin_stripe_self() below into both hot entry points of this
 * file, and code that never runs costs those two functions through register
 * allocation and code layout all the same. The call keeps the whole claim out
 * of them, so they have the shape that they would have with no assignment
 * path at all.
 *
 * The counter wraps after 2^32 assignments. A thread that takes 0 from it
 * would store the marker for "not yet assigned", and would then derive its
 * stripe again on every call: it would contend the counter on the hot path,
 * and it would release a pin on a stripe that is not the stripe that it took
 * the pin on. */
static __attribute__((noinline)) unsigned pin_stripe_claim(void) {
  unsigned id =
      atomic_fetch_add_explicit(&_pin_stripe_next, 1, memory_order_relaxed);
  if (id == 0) id = 1;
  _pin_stripe_id = id;
  return id;
}

/* Zero means "not yet assigned", which is why an id starts at 1: the stored
 * value is one more than the stripe that it selects. */
static inline unsigned pin_stripe_self(void) {
  unsigned id = _pin_stripe_id;
  if (id == 0) id = pin_stripe_claim();
  return (id - 1u) % CCOL_PIN_STRIPES;
}

/* The state word of a live slot carries its generation and its liveness
 * together. One word is what lets a resolve validate both with one load, and
 * it also makes the re-check after the pin one comparison, while two separate
 * reads could fall on both sides of a retire. */
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
/* See the declaration in cpintable.h. The next publish consumes this flag
 * and reads it before it stores anything, so a rollback sees the same state
 * that a real allocation failure leaves. */
static atomic_bool _pin_force_publish_failure;

/* Forces the allocation of the stripe block to fail on the next publish that
 * does one. This is the only refusal that can happen after the table already
 * published a chunk, and nothing else reaches that state. */
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

  /* The caller holds its own writer lock, so no two publishes race here and
   * a relaxed load is enough to find out whether the chunk already exists.
   * The store below is a release all the same, because a concurrent reader
   * acquires it. */
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
  /* This store is a release, and it is the last one, because a reader that
   * acquires a live state must see the pointer that belongs to it. */
  atomic_store_explicit(&slot->state, pin_state(gen, true),
                        memory_order_release);
  return true;
}

void ccol_pintable_retire(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  uint64_t st = atomic_load_explicit(&slot->state, memory_order_relaxed);
  /* This store keeps the generation and clears only the liveness, because a
   * handle that names this occupancy must stay different from a handle that
   * names the next one.
   *
   * The store is sequentially consistent, and it must be: this store and the
   * read of the pin count after it are one half of a store-then-load pair,
   * whose other half is in ccol_pintable_pin(). The note there explains why
   * release ordering alone lets the two halves miss each other, which results
   * in a free of an object that a caller already holds. */
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

  /* This increment and the state load after it are sequentially consistent,
   * and so are the store on the retire side and its read of the pin count.
   * All four operations must be seq_cst, because that is what makes it
   * impossible for the two sides to miss each other; nothing weaker is
   * enough.
   *
   * The two sides race in opposite directions: this side writes the pin and
   * then reads the liveness, while the retire side writes the liveness and
   * then reads the pin. Under acquire and release alone, the read of each
   * side can come from before the write of the other side. A destroy that
   * retires can then read a pin count of zero while this call reads a state
   * that is live, and the object is freed while a caller already holds the
   * pointer.
   *
   * With all four operations sequentially consistent there is one total order
   * over them, and the read of one side sees whichever write comes first in
   * that order. So either the destroy sees this pin and waits for it, or this
   * call sees the retire and backs out: exactly one of the two happens, and
   * never neither of them.
   *
   * The cost is nothing on x86-64, where the increment is already a locked
   * read-modify-write and a sequentially consistent load is an ordinary
   * load. */
  atomic_fetch_add_explicit(&stripes[stripe].count, 1, memory_order_seq_cst);

  if (atomic_load_explicit(&slot->state, memory_order_seq_cst) != want) {
    atomic_fetch_sub_explicit(&stripes[stripe].count, 1, memory_order_release);
    return NULL;
  }

  return atomic_load_explicit(&slot->ptr, memory_order_relaxed);
}

void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle) {
  /* This guard is the same as the guard in ccol_pintable_pin: a zero handle
   * never gives a pin there, so a release of one here must do nothing. Slot 0
   * is an ordinary slot that usually holds an object, and a decrement for a
   * pin that nobody took drives the count of that slot below the truth, so a
   * later drain finishes while a real caller holds the object. The realistic
   * way to arrive here with zero is an owner that did not yet write its own
   * self-handle field, because such structs normally start as all zero
   * bytes. */
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
  /* The sum is signed, and the code clamps it afterwards: one stripe can
   * correctly read negative while a release for a pin of one stripe lands on
   * another stripe, and only the total has a meaning.
   *
   * The accumulator has an unsigned type, and the code converts it one time
   * at the end. A workload can always pin on one thread and release on
   * another, so the two stripes move apart without a bound while their sum
   * stays correct, and a partial sum can pass the limit of a signed
   * accumulator long before the total does. Unsigned arithmetic wraps by
   * definition, while signed overflow is undefined. */
  uintptr_t accumulated = 0;
  for (unsigned i = 0; i < CCOL_PIN_STRIPES; ++i) {
    accumulated += (uintptr_t)atomic_load_explicit(&stripes[i].count,
                                                   memory_order_seq_cst);
  }
  ptrdiff_t total = (ptrdiff_t)accumulated;
  return total > 0 ? (size_t)total : 0;
}

size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle) {
  /* This guard is the same as the one in unpin above: a zero handle names no
   * slot, and an answer that used the count of slot 0 would report the pins
   * of an unrelated object. */
  if (handle == 0) return 0;
  return ccol_pintable_pins(t, (uint32_t)(handle >> 32));
}

void ccol_pintable_dispose(ccol_pintable *t) {
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c) {
    ccol_pin_chunk *chunk =
        atomic_load_explicit(&t->chunks[c], memory_order_acquire);
    if (!chunk) continue;
    /* The loop unpublishes the chunk before it frees it, so a resolve that
     * races this loop finds no chunk and answers NULL instead of reading freed
     * memory. */
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
  /* This guard is the same as the ones in unpin and pins_for above: a zero
   * handle names no slot, and a clear of the counters of slot 0 would drop
   * the pins of whichever object occupies that slot. */
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
