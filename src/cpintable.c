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
#include <internal/test_helpers/cpintable.h>
#include <stdlib.h>

/* The stripe this thread pins on. Each thread takes its value once from a
 * global counter, so concurrent threads end up on different stripes and
 * their pins write different cache lines.
 *
 * The TLS model comes from include/internal/ctlsmodel.h, same as the
 * thread-local state of cmempool and for the same reason. With glibc and on
 * FreeBSD it's initial-exec, because under the general model every access to
 * a thread-local in a shared library goes through a __tls_get_addr call, and
 * that call would sit on exactly the path this type is meant to keep cheap.
 * What it costs is one slot in the static thread-local block, which only
 * hurts a library that a process dlopen()s after it has already used up
 * every slot. Every other C library gets the general model, since musl
 * refuses to dlopen() an object with initial-exec thread-locals. */
#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread unsigned _pin_stripe_id;
#else
static __thread unsigned _pin_stripe_id
    __attribute__((tls_model("initial-exec")));
#endif
static _Atomic unsigned _pin_stripe_next = 1;

/* Claims this thread's id, once. It's out of line on purpose because it
 * runs on the thread's first pin and never again. pin_stripe_self() below is
 * inlined into both hot entry points of this file, and code that never runs
 * still costs those two functions through register allocation and code
 * layout. Keeping the claim behind a call gives them the same shape they
 * would have with no assignment path at all.
 *
 * The counter wraps after 2^32 assignments. A thread that got 0 from it
 * would store the "not assigned yet" marker and then work out its stripe
 * again on every call. It would contend on the counter in the hot path, and
 * it would release a pin on a different stripe than the one it pinned on. */
static __attribute__((noinline)) unsigned pin_stripe_claim(void) {
  unsigned id =
      atomic_fetch_add_explicit(&_pin_stripe_next, 1, memory_order_relaxed);
  if (id == 0) id = 1;
  _pin_stripe_id = id;
  return id;
}

/* Zero means "not assigned yet", which is why ids start at 1: the stored
 * value is one more than the stripe it selects. */
static inline unsigned pin_stripe_self(void) {
  unsigned id = _pin_stripe_id;
  if (id == 0) id = pin_stripe_claim();
  return (id - 1u) % CCOL_PIN_STRIPES;
}

/* A live slot's state word holds its generation and its liveness together.
 * With one word a resolve can validate both with a single load, and the
 * re-check after the pin is one comparison; two separate reads could fall on
 * both sides of a retire. */
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
 * and reads it before storing anything, so the rollback sees the same state
 * as after a real allocation failure. */
static atomic_bool _pin_force_publish_failure;

/* Makes the stripe block allocation fail on the next publish that does one.
 * It's the only refusal that can happen after the table has published a
 * chunk, and nothing else gets us into that state. */
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

  /* The caller holds its own writer lock, so two publishes never race here
   * and a relaxed load is enough to see whether the chunk already exists.
   * The store below is still a release, because a concurrent reader acquires
   * it. */
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
  /* This store is a release and it comes last, because a reader that
   * acquires a live state must see the pointer that goes with it. */
  atomic_store_explicit(&slot->state, pin_state(gen, true),
                        memory_order_release);
  return true;
}

void ccol_pintable_retire(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  uint64_t st = atomic_load_explicit(&slot->state, memory_order_relaxed);
  /* We keep the generation and clear only the liveness, because a handle for
   * this occupancy must stay different from a handle for the next one.
   *
   * The store must be sequentially consistent. Together with the read of
   * the pin count after it, it is one half of a store-then-load pair whose
   * other half is in ccol_pintable_pin(). The note there explains how, with
   * release ordering alone, the two halves can miss each other and we end up
   * freeing an object that a caller already holds. */
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
   * and so are the retire side's store and its read of the pin count. All
   * four have to be seq_cst, since that's what makes it impossible for the
   * two sides to miss each other; nothing weaker will do.
   *
   * The two sides race in opposite directions. This side writes the pin and
   * then reads the liveness, the retire side writes the liveness and then
   * reads the pin. With only acquire and release, each side's read can come
   * from before the other side's write. A destroy that retires can then read
   * a pin count of zero while this call reads a live state, and the object
   * gets freed while a caller already holds the pointer.
   *
   * With all four seq_cst there is one total order over them, and each
   * side's read sees whichever write comes first in that order. So either
   * the destroy sees this pin and waits for it, or this call sees the retire
   * and backs out. Exactly one of the two happens, never neither.
   *
   * On x86-64 this costs nothing, since the increment is a locked
   * read-modify-write anyway and a seq_cst load is an ordinary load. Tests
   * on x86-64 will also pass a weaker version almost every time, so don't
   * take a green run as a reason to relax any of the four. */
  atomic_fetch_add_explicit(&stripes[stripe].count, 1, memory_order_seq_cst);

  if (atomic_load_explicit(&slot->state, memory_order_seq_cst) != want) {
    atomic_fetch_sub_explicit(&stripes[stripe].count, 1, memory_order_release);
    return NULL;
  }

  return atomic_load_explicit(&slot->ptr, memory_order_relaxed);
}

void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle) {
  /* Same guard as in ccol_pintable_pin: a zero handle never gets a pin
   * there, so releasing one here must do nothing. Slot 0 is an ordinary slot
   * that usually holds an object, and a decrement for a pin nobody took drives
   * its count below the truth, so a later drain finishes while a real caller
   * still holds the object. The realistic way to get here with zero is an
   * owner that hasn't written its own self-handle field yet, since such
   * structs normally start as all zero bytes. */
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
  /* The sum is signed and clamped afterwards. One stripe can rightly read
   * negative while a release for a pin taken on one stripe lands on another,
   * and only the total means anything.
   *
   * The accumulator is unsigned and we convert it once at the end. A
   * workload can always pin on one thread and release on another, so two
   * stripes can drift apart without bound while their sum stays right, and a
   * partial sum can overflow a signed accumulator long before the total
   * does. Unsigned arithmetic wraps by definition; signed overflow is
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
  /* Same guard as in unpin above: a zero handle names no slot, and answering
   * with slot 0's count would report the pins of some unrelated object. */
  if (handle == 0) return 0;
  return ccol_pintable_pins(t, (uint32_t)(handle >> 32));
}

void ccol_pintable_dispose(ccol_pintable *t) {
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c) {
    ccol_pin_chunk *chunk =
        atomic_load_explicit(&t->chunks[c], memory_order_acquire);
    if (!chunk) continue;
    /* The chunk is unpublished before it's freed, so a resolve racing this
     * loop finds no chunk and answers NULL instead of reading freed memory. */
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
  /* Same guard as in unpin and pins_for above: a zero handle names no slot,
   * and clearing slot 0's counters would drop the pins of whatever object
   * occupies it. */
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
