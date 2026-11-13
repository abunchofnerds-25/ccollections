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

#include <internal/cpintable.h>
#include <internal/test_helpers/cpintable.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tau/tau.h"

TAU_MAIN()

// Builds the handle of (idx, gen) the way the owning modules do, so tests can
// name slots.
static uint64_t mk(uint32_t idx, uint32_t gen) {
  return ((uint64_t)idx << 32) | (uint64_t)gen;
}

// Disposes of *t; the cleanup function of SCOPED_PINTABLE. A failing REQUIRE
// returns at once, and without it the table's chunks would leak on exactly
// those runs. A second dispose at the end of a test finds nothing to free.
static void dispose_pintable(ccol_pintable *t) { ccol_pintable_dispose(t); }

// Gives how many chunks t holds, so a test can prove that a refusal made no
// allocation; the return value alone can't show that.
static size_t chunks_held(const ccol_pintable *t) {
  size_t n = 0;
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c)
    if (atomic_load_explicit(&t->chunks[c], memory_order_acquire)) n++;
  return n;
}

// Declares the table name, disposed of on every way out of the scope, so a
// failing test doesn't leak it.
#define SCOPED_PINTABLE(name) \
  ccol_pintable name __attribute__((cleanup(dispose_pintable)))

TEST(pintable, published_handle_resolves_to_its_object) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  void *p = ccol_pintable_pin(&t, mk(0, 1));
  REQUIRE_EQ(p, (void *)&obj);
  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_zero_handle_never_resolves) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  REQUIRE_EQ(ccol_pintable_pin(&t, 0), NULL);
  ccol_pintable_dispose(&t);
}

// An owner that hasn't written its self-handle yet holds 0, and slot 0 usually
// holds a real object. Charging the unpin to slot 0 would let a later drain
// finish while a caller holds that object.
TEST(pintable,
     a_zero_handle_is_ignored_by_unpin_rather_than_charged_to_slot_zero) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_unpin(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  REQUIRE_EQ(ccol_pintable_pins_for(&t, 0), (size_t)0);

  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

// A fork child handler resets an owner's pins by handle; a zero handle must not
// drop the pins of the unrelated object in slot 0.
TEST(pintable,
     a_zero_handle_is_ignored_by_reset_rather_than_clearing_slot_zero) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_reset_for(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_reset_for(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

// A caller can pass a stale or made-up handle before anything is published.
TEST(pintable, a_handle_against_an_empty_table_resolves_to_nothing) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(12345, 9)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, an_index_past_the_ceiling_is_refused_rather_than_written) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  uint32_t too_big = CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS;
  REQUIRE_FALSE(ccol_pintable_publish(&t, too_big, 1, &obj));
  REQUIRE_EQ(chunks_held(&t), (size_t)0);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(too_big, 1)), NULL);
  ccol_pintable_dispose(&t);
}

TEST(pintable, generation_zero_is_rejected_because_it_is_the_invalid_sentinel) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_FALSE(ccol_pintable_publish(&t, 0, 0, &obj));
  REQUIRE_EQ(chunks_held(&t), (size_t)0);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 0)), NULL);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_stale_generation_does_not_resolve) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int first = 1, second = 2;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 3, 1, &first));
  ccol_pintable_retire(&t, 3);
  REQUIRE_TRUE(ccol_pintable_publish(&t, 3, 2, &second));

  REQUIRE_EQ(ccol_pintable_pin(&t, mk(3, 1)), NULL);
  void *p = ccol_pintable_pin(&t, mk(3, 2));
  REQUIRE_EQ(p, (void *)&second);
  ccol_pintable_unpin(&t, mk(3, 2));
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_retired_handle_does_not_resolve) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 1, 5, &obj));
  REQUIRE_NE(ccol_pintable_pin(&t, mk(1, 5)), NULL);
  ccol_pintable_unpin(&t, mk(1, 5));

  ccol_pintable_retire(&t, 1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(1, 5)), NULL);
  ccol_pintable_dispose(&t);
}

// A retire stops new pins, but a pin already granted keeps the count above 0
// until its release. That's what keeps the object alive for a caller already
// inside a call.
TEST(pintable, an_outstanding_pin_is_visible_to_the_drain_after_a_retire) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 2, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(2, 1)), NULL);
  ccol_pintable_retire(&t, 2);
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(2, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);

  ccol_pintable_unpin(&t, mk(2, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, nested_pins_on_one_handle_are_counted_individually) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_NE(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)2);
  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);
  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

// A fork child depends on this: The threads that would release these pins
// don't exist there, and its first destroy would wait forever.
TEST(pintable, reset_drops_every_outstanding_pin) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 4, 1, &obj));
  REQUIRE_NE(ccol_pintable_pin(&t, mk(4, 1)), NULL);
  REQUIRE_NE(ccol_pintable_pin(&t, mk(4, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 4), (size_t)2);

  ccol_pintable_reset(&t, 4);
  REQUIRE_EQ(ccol_pintable_pins(&t, 4), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, pins_for_a_handle_matches_pins_for_its_index) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 9, 3, &obj));
  REQUIRE_NE(ccol_pintable_pin(&t, mk(9, 3)), NULL);
  REQUIRE_EQ(ccol_pintable_pins_for(&t, mk(9, 3)), ccol_pintable_pins(&t, 9));
  REQUIRE_EQ(ccol_pintable_pins_for(&t, mk(9, 3)), (size_t)1);
  ccol_pintable_unpin(&t, mk(9, 3));
  ccol_pintable_dispose(&t);
}

TEST(pintable, an_index_in_a_later_chunk_resolves) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  uint32_t idx = CCOL_PIN_CHUNK_SLOTS + 5;
  REQUIRE_TRUE(ccol_pintable_publish(&t, idx, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(idx, 1)), (void *)&obj);
  ccol_pintable_unpin(&t, mk(idx, 1));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(5, 1)), NULL);
  ccol_pintable_dispose(&t);
}

// Arguments of pin_worker().
typedef struct {
  ccol_pintable *t;
  uint64_t handle;
  size_t iterations;
  _Atomic size_t *resolved;
} pin_worker_arg_t;

// Thread body of the balance test: Pins and unpins a->handle a->iterations
// times and counts the successful resolves in *a->resolved.
static void *pin_worker(void *p) {
  pin_worker_arg_t *a = p;
  for (size_t i = 0; i < a->iterations; i++) {
    void *obj = ccol_pintable_pin(a->t, a->handle);
    if (obj) {
      atomic_fetch_add_explicit(a->resolved, 1, memory_order_relaxed);
      ccol_pintable_unpin(a->t, a->handle);
    }
  }
  return NULL;
}

// A mistake in a stripe's accounting shows up as a residue that nothing else
// would show. The sizes stay small for valgrind; such a mistake is systematic,
// so it shows at this size too.
TEST(pintable, concurrent_pins_balance_to_zero) {
  enum { THREADS = 8, ITERS = 2000 };
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));

  pthread_t th[THREADS];
  pin_worker_arg_t args[THREADS];
  _Atomic size_t resolved = 0;
  int started = 0;
  for (int i = 0; i < THREADS; i++) {
    args[i] = (pin_worker_arg_t){.t = &t,
                                 .handle = mk(0, 1),
                                 .iterations = ITERS,
                                 .resolved = &resolved};
    if (pthread_create(&th[i], NULL, pin_worker, &args[i]) != 0) break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(th[i], NULL);

  REQUIRE_GT(started, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  REQUIRE_EQ(atomic_load(&resolved), (size_t)started * ITERS);
  ccol_pintable_dispose(&t);
}

// The value a pinned object must hold. A pin granted after the free reads freed
// memory, which ASan and valgrind report and the magic check may also see.
#define PIN_OBJ_MAGIC 0x5eaf00du

// The object behind the handle in the race test.
typedef struct {
  unsigned magic;
} pin_obj_t;

// Keeps every worker of a round from pinning until all of them exist. Under
// valgrind a thread created while the others spin on the slot starves for
// minutes. The race is between a pin and the retire, both after the gate.
typedef struct {
  _Atomic int ready;
  _Atomic bool go;
} pin_start_gate_t;

// Sleeps 1 us while a thread waits at the start gate or the drain. A real
// sleep, because sched_yield() doesn't hand over the CPU when valgrind runs
// one thread at a time.
static void pin_gate_park(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
  nanosleep(&ts, NULL);
}

// Arguments of race_worker().
typedef struct {
  ccol_pintable *t;
  uint64_t handle;
  _Atomic bool *stop;
  _Atomic size_t *violations;
  pin_start_gate_t *gate;
} race_arg_t;

// Thread body of the drain race: Waits at the gate, then pins a->handle until
// *a->stop, counting in *a->violations every pinned object whose magic is
// wrong. After the retire every pin fails, and it sleeps between attempts so
// it doesn't starve the retiring thread under valgrind.
static void *race_worker(void *p) {
  race_arg_t *a = p;
  atomic_fetch_add_explicit(&a->gate->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&a->gate->go, memory_order_acquire))
    pin_gate_park();
  while (!atomic_load_explicit(a->stop, memory_order_relaxed)) {
    pin_obj_t *obj = (pin_obj_t *)ccol_pintable_pin(a->t, a->handle);
    if (obj) {
      if (obj->magic != PIN_OBJ_MAGIC)
        atomic_fetch_add_explicit(a->violations, 1, memory_order_relaxed);
      ccol_pintable_unpin(a->t, a->handle);
    } else {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

// The race that the seq_cst pairing of pin and retire closes. Each round
// retires the slot, waits (bounded by the clock, as a hang guard) for the pins
// to drain, and frees the object while the workers keep pinning, so a late pin
// is a real use-after-free. When the drain times out the workers are joined
// before the free, so the failure is reported instead of crashing. Assertions
// come after every join. x86-64 rarely catches a weaker order; the aarch64,
// arm32 and sanitizer jobs do.
TEST(pintable, a_pin_is_never_granted_against_a_drained_slot) {
  enum { THREADS = 6, ROUNDS = 60 };
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));

  _Atomic size_t violations = 0;
  size_t drain_saw_nonzero = 0;

  for (int round = 0; round < ROUNDS; round++) {
    uint32_t gen = (uint32_t)(round + 1);
    pin_obj_t *obj = malloc(sizeof(*obj));
    if (!obj) break;
    obj->magic = PIN_OBJ_MAGIC;
    bool published = ccol_pintable_publish(&t, 0, gen, obj);
    if (!published) free(obj);
    REQUIRE_TRUE(published);

    _Atomic bool stop = false;
    pin_start_gate_t gate = {0};
    pthread_t th[THREADS];
    race_arg_t args[THREADS];
    int started = 0;
    for (int i = 0; i < THREADS; i++) {
      args[i] = (race_arg_t){.t = &t,
                             .handle = mk(0, gen),
                             .stop = &stop,
                             .violations = &violations,
                             .gate = &gate};
      if (pthread_create(&th[i], NULL, race_worker, &args[i]) != 0) break;
      started++;
    }

    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < started)
      pin_gate_park();
    atomic_store_explicit(&gate.go, true, memory_order_release);

    ccol_pintable_retire(&t, 0);
    struct timespec drain_start, drain_now;
    bool timed_out = false;
    clock_gettime(CLOCK_MONOTONIC, &drain_start);
    while (ccol_pintable_pins(&t, 0) > 0) {
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
      nanosleep(&ts, NULL);
      clock_gettime(CLOCK_MONOTONIC, &drain_now);
      if (drain_now.tv_sec - drain_start.tv_sec > 30) {
        timed_out = true;
        break;
      }
    }

    bool drained = !timed_out;
    bool joined = false;
    if (!drained) {
      drain_saw_nonzero++;
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      joined = true;
    }

    obj->magic = 0xdeadbeefu;
    free(obj);

    if (!joined) {
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      if (ccol_pintable_pins(&t, 0) != 0) drain_saw_nonzero++;
    }
  }

  REQUIRE_EQ(atomic_load(&violations), (size_t)0);
  REQUIRE_EQ(drain_saw_nonzero, (size_t)0);
  ccol_pintable_dispose(&t);
}

// Arguments of unpin_on_another_thread().
typedef struct {
  ccol_pintable *t;
  uint64_t handle;
} unpin_arg_t;

// Thread body of the split-pin test: Releases a pin on a thread other than the
// one that took it.
static void *unpin_on_another_thread(void *p) {
  unpin_arg_t *a = p;
  ccol_pintable_unpin(a->t, a->handle);
  return NULL;
}

// Releasing on another thread breaks the unpin contract, and the unsafe window
// in between isn't measured here. This pins only that the signed counters sum
// to exactly 0 once both sides are visible, so a split leaves no residue that
// would stall later drains.
TEST(pintable, a_split_pin_still_sums_to_zero_once_both_sides_are_visible) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  unpin_arg_t arg = {.t = &t, .handle = mk(0, 1)};
  pthread_t th;
  bool spawned =
      (pthread_create(&th, NULL, unpin_on_another_thread, &arg) == 0);
  if (spawned) pthread_join(th, NULL);

  REQUIRE_TRUE(spawned);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

// The one refusal after a chunk is published. The chunk stays, because a
// reader may hold a pointer into it, and the slot must not go live. The NULL
// pin also checks that a slot without stripes never resolves.
TEST(pintable,
     a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;

  ccol__pintable_force_next_stripe_alloc_failure_for_tests();
  REQUIRE_FALSE(ccol_pintable_publish(&t, 0, 1, &obj));

  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);

  REQUIRE_EQ(chunks_held(&t), (size_t)1);

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 2, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 2)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);
  ccol_pintable_unpin(&t, mk(0, 2));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}
