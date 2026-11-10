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

// Internal: the lock-free handle index with per-slot pins (see cpintable.h).

#include "internal/cpintable.h"

#include <internal/ctlsmodel.h>
#include <internal/test_helpers/cpintable.h>
#include <stdlib.h>

// The stripe this thread pins on, stored as stripe + 1 so 0 means "not
// assigned yet". The TLS model comes from ctlsmodel.h: initial-exec with glibc
// and on FreeBSD keeps __tls_get_addr off the pin path, and other C libraries
// get the general model (musl refuses to dlopen() initial-exec objects).
#if defined(CMEMPOOL_DYNAMIC_TLS) && CMEMPOOL_DYNAMIC_TLS
static __thread unsigned pin_stripe_id;
#else
static __thread unsigned pin_stripe_id
    __attribute__((tls_model("initial-exec")));
#endif
// Hands out stripe ids, so concurrent threads pin on different cache lines.
static _Atomic unsigned pin_stripe_next = 1;

// Claims a stripe id for the calling thread from pin_stripe_next and stores
// it in pin_stripe_id, on the thread's first pin or unpin. Gives the id. It's
// out of line so the two hot callers keep the shape they'd have without it.
// An id of 0 after the counter wraps becomes 1; a stored 0 would make the
// thread claim again on every call and release on another stripe than it
// pinned on.
static __attribute__((noinline)) unsigned pin_stripe_claim(void) {
  unsigned id =
      atomic_fetch_add_explicit(&pin_stripe_next, 1, memory_order_relaxed);
  if (id == 0) id = 1;
  pin_stripe_id = id;
  return id;
}

// Gives the stripe index of the calling thread, claiming an id on first use.
// Pin and unpin use it so concurrent threads write different cache lines.
static inline unsigned pin_stripe_self(void) {
  unsigned id = pin_stripe_id;
  if (id == 0) id = pin_stripe_claim();
  return (id - 1u) % CCOL_PIN_STRIPES;
}

// Packs a generation and the live bit into one state word, so a resolve
// checks both with one load and can't straddle a retire.
static inline uint64_t pin_state(uint32_t gen, bool live) {
  return ((uint64_t)gen << 1) | (live ? 1u : 0u);
}

// Gives the slot of idx, or NULL when idx is out of range or its chunk doesn't
// exist yet. Every entry point except publish starts with it; it reads the
// chunk pointer with acquire.
static ccol_pin_slot *pin_slot_lookup(ccol_pintable *t, uint32_t idx) {
  uint32_t chunk_idx = idx / CCOL_PIN_CHUNK_SLOTS;
  if (chunk_idx >= CCOL_PIN_MAX_CHUNKS) return NULL;
  ccol_pin_chunk *chunk =
      atomic_load_explicit(&t->chunks[chunk_idx], memory_order_acquire);
  if (!chunk) return NULL;
  return &chunk->slots[idx % CCOL_PIN_CHUNK_SLOTS];
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: makes the next publish fail before it stores anything.
static atomic_bool pin_force_publish_failure;

// Test hook: makes the next stripe-block allocation of a publish fail.
static _Atomic bool pin_force_stripe_alloc_failure = false;

// Test hook: makes the next stripe-block allocation of a publish fail, the only
// way to get a published chunk whose slot never went live.
void ccol__pintable_force_next_stripe_alloc_failure_for_tests(void) {
  atomic_store(&pin_force_stripe_alloc_failure, true);
}

// Test hook: makes the next publish fail before it stores anything, so the
// tests of the owning modules reach their rollback paths.
void ccol__pintable_force_next_publish_failure_for_tests(void) {
  atomic_store(&pin_force_publish_failure, true);
}
#endif

// Publishes ptr at idx under generation gen, so a resolve can find it; an
// owning module calls it when it acquires a handle, under its writer lock.
// Allocates the chunk and the stripe block on first use. Gives false, storing
// nothing, for gen 0, an index above the ceiling or a failed allocation. The
// writer lock means two publishes never race, so a relaxed load finds an
// existing chunk; the chunk, the stripes and the state are stored with
// release, the state last, so a reader that sees it live sees ptr.
bool ccol_pintable_publish(ccol_pintable *t, uint32_t idx, uint32_t gen,
                           void *ptr) {
  if (gen == 0) return false;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&pin_force_publish_failure, false)) return false;
#endif
  uint32_t chunk_idx = idx / CCOL_PIN_CHUNK_SLOTS;
  if (chunk_idx >= CCOL_PIN_MAX_CHUNKS) return false;

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
    if (atomic_exchange(&pin_force_stripe_alloc_failure, false)) return false;
#endif
    stripes = calloc(CCOL_PIN_STRIPES, sizeof(*stripes));
    if (!stripes) return false;
    atomic_store_explicit(&slot->stripes, stripes, memory_order_release);
  }

  atomic_store_explicit(&slot->ptr, ptr, memory_order_relaxed);
  atomic_store_explicit(&slot->state, pin_state(gen, true),
                        memory_order_release);
  return true;
}

// Clears the live bit of idx so no new pin can find it; a destroy calls it
// under the owning module's writer lock before waiting for the pins. The
// generation stays, so this occupancy's handle differs from the next one's.
// The store is seq_cst: it pairs with the pin in ccol_pintable_pin().
void ccol_pintable_retire(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  uint64_t st = atomic_load_explicit(&slot->state, memory_order_relaxed);
  atomic_store_explicit(&slot->state, st & ~(uint64_t)1, memory_order_seq_cst);
}

// Resolves a handle and pins its object on the calling thread's stripe, so a
// public call can use the object while a concurrent destroy waits. Gives the
// pointer, or NULL with no pin. The pin increment and the state re-check after
// it are seq_cst, and so are the retire store and the read of the pin count in
// ccol_pintable_pins(). With one total order over the four, either the destroy
// sees this pin and waits, or this call sees the retire and backs out. Acquire
// and release alone let both reads miss the other side's write, and the object
// is freed while a caller holds it. x86-64 passes a weaker version almost
// every time, so don't relax any of the four on the strength of a green run.
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

  atomic_fetch_add_explicit(&stripes[stripe].count, 1, memory_order_seq_cst);

  if (atomic_load_explicit(&slot->state, memory_order_seq_cst) != want) {
    atomic_fetch_sub_explicit(&stripes[stripe].count, 1, memory_order_release);
    return NULL;
  }

  return atomic_load_explicit(&slot->ptr, memory_order_relaxed);
}

// Releases a pin from the calling thread's stripe when a public call ends. A
// zero handle never got a pin, so it does nothing; decrementing slot 0 for it
// would let a drain finish while a real caller holds that object.
void ccol_pintable_unpin(ccol_pintable *t, uint64_t handle) {
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

// Gives the pins outstanding against idx, which a destroy waits to reach 0.
// It sums the stripes (seq_cst loads) in an unsigned accumulator and clamps
// the signed total at 0: one stripe may read negative, and stripes can drift
// apart without bound while their sum stays right, so a signed partial sum
// could overflow.
size_t ccol_pintable_pins(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return 0;
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return 0;
  uintptr_t accumulated = 0;
  for (unsigned i = 0; i < CCOL_PIN_STRIPES; ++i) {
    accumulated += (uintptr_t)atomic_load_explicit(&stripes[i].count,
                                                   memory_order_seq_cst);
  }
  ptrdiff_t total = (ptrdiff_t)accumulated;
  return total > 0 ? (size_t)total : 0;
}

// Gives the pins of the index a handle names, for owners that hold a handle.
// A zero handle names no slot, so it gives 0 and not slot 0's count.
size_t ccol_pintable_pins_for(ccol_pintable *t, uint64_t handle) {
  if (handle == 0) return 0;
  return ccol_pintable_pins(t, (uint32_t)(handle >> 32));
}

// Frees every chunk and stripe block of t at process teardown, so a leak
// checker doesn't report them. Each chunk is unpublished before it's freed, so
// a racing resolve answers NULL instead of reading freed memory.
void ccol_pintable_dispose(ccol_pintable *t) {
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c) {
    ccol_pin_chunk *chunk =
        atomic_load_explicit(&t->chunks[c], memory_order_acquire);
    if (!chunk) continue;
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

// Drops the pins of the index a handle names, for a fork child handler. A
// zero handle names no slot, so it leaves slot 0's pins alone.
void ccol_pintable_reset_for(ccol_pintable *t, uint64_t handle) {
  if (handle == 0) return;
  ccol_pintable_reset(t, (uint32_t)(handle >> 32));
}

// Zeroes every stripe of idx. Only a fork child handler calls it, because a
// pin held by a thread that didn't survive the fork can never be released.
void ccol_pintable_reset(ccol_pintable *t, uint32_t idx) {
  ccol_pin_slot *slot = pin_slot_lookup(t, idx);
  if (!slot) return;
  ccol_pin_stripe *stripes =
      atomic_load_explicit(&slot->stripes, memory_order_acquire);
  if (!stripes) return;
  for (unsigned i = 0; i < CCOL_PIN_STRIPES; ++i)
    atomic_store_explicit(&stripes[i].count, 0, memory_order_release);
}
