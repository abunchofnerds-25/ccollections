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

#include <cpintable.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tau/tau.h"

TAU_MAIN()

/* A handle is (index << 32) | generation, matching what every module that uses
 * this type mints. */
extern void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);

static uint64_t mk(uint32_t idx, uint32_t gen) {
  return ((uint64_t)idx << 32) | (uint64_t)gen;
}

/* Tau's REQUIRE_* macros return from the test function the moment one fails,
 * so a table released only by a trailing statement keeps its chunks and stripe
 * blocks on exactly the runs that matter, and memtest then reports a leak on
 * top of the assertion that caused it. Declaring the table with a cleanup
 * attribute releases it on every path out. Each test also disposes explicitly
 * at its end; ccol_pintable_dispose unpublishes a chunk before freeing it, so
 * that second call finds nothing to do. */
static void dispose_pintable(ccol_pintable *t) { ccol_pintable_dispose(t); }

/* How many chunks the table currently holds. A refusal that happens before any
 * allocation must leave this at zero: the return value alone cannot distinguish
 * "refused" from "allocated a chunk, stored a pointer, and then reported
 * failure", which is the half of those tests' claims that would otherwise go
 * unchecked. It is not true of every refusal; a stripe allocation that fails
 * after its chunk is published keeps the chunk deliberately, which
 * a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable
 * pins. */
static size_t chunks_held(const ccol_pintable *t) {
  size_t n = 0;
  for (unsigned c = 0; c < CCOL_PIN_MAX_CHUNKS; ++c)
    if (atomic_load_explicit(&t->chunks[c], memory_order_acquire)) n++;
  return n;
}

#define SCOPED_PINTABLE(name) \
  ccol_pintable name __attribute__((cleanup(dispose_pintable)))

TEST(pintable, published_handle_resolves_to_its_object) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
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
  memset(&t, 0, sizeof t);
  REQUIRE_EQ(ccol_pintable_pin(&t, 0), NULL);
  ccol_pintable_dispose(&t);
}

/* Slot 0 is an ordinary slot and is usually occupied, so a zero handle reaching
 * unpin must not be treated as naming it. An owner whose self-handle field has
 * not been written yet holds zero, because those structs are zero-initialised,
 * and decrementing slot 0 on its behalf would drive a real object's count below
 * the truth and let a later drain finish while a caller still holds it.
 *
 * These two are non-vacuous: removing either guard from ccol_pintable_unpin or
 * ccol_pintable_pins_for makes them fail here rather than corrupting some
 * unrelated owner's count in a build that looks healthy. */
TEST(pintable,
     a_zero_handle_is_ignored_by_unpin_rather_than_charged_to_slot_zero) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* The pin above belongs to slot 0. This must leave it alone. */
  ccol_pintable_unpin(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* And a zero handle names no slot to report on. */
  REQUIRE_EQ(ccol_pintable_pins_for(&t, 0), (size_t)0);

  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable,
     a_zero_handle_is_ignored_by_reset_rather_than_clearing_slot_zero) {
  /* A fork() child handler drops an owner's pins by handle, and an owner whose
   * handle field has not been written yet carries zero. Clearing slot 0 on its
   * behalf would drop a genuinely in-flight caller's pin on some unrelated
   * object, which is the one thing the count exists to prevent.
   *
   * This test is non-vacuous: deriving the index from the handle without the
   * guard makes it fail here. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_reset_for(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* Named properly, it does clear. */
  ccol_pintable_reset_for(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_handle_against_an_empty_table_resolves_to_nothing) {
  /* A module resolves handles before it has ever published one (a caller
   * passing a stale or fabricated handle), so an all-zero table must answer
   * rather than fault. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(12345, 9)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, an_index_past_the_ceiling_is_refused_rather_than_written) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;
  uint32_t too_big = CCOL_PIN_MAX_CHUNKS * CCOL_PIN_CHUNK_SLOTS;
  REQUIRE_FALSE(ccol_pintable_publish(&t, too_big, 1, &obj));
  REQUIRE_EQ(chunks_held(&t), (size_t)0);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(too_big, 1)), NULL);
  ccol_pintable_dispose(&t);
}

TEST(pintable, generation_zero_is_rejected_because_it_is_the_invalid_sentinel) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;
  REQUIRE_FALSE(ccol_pintable_publish(&t, 0, 0, &obj));
  /* Refused before anything is built, so the slot it names stays empty and a
     later publish into it starts from nothing. */
  REQUIRE_EQ(chunks_held(&t), (size_t)0);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 0)), NULL);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_stale_generation_does_not_resolve) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int first = 1, second = 2;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 3, 1, &first));
  ccol_pintable_retire(&t, 3);
  /* The slot is reused by a different object, as a module's own free-index
   * list would do. */
  REQUIRE_TRUE(ccol_pintable_publish(&t, 3, 2, &second));

  REQUIRE_EQ(ccol_pintable_pin(&t, mk(3, 1)), NULL);
  void *p = ccol_pintable_pin(&t, mk(3, 2));
  REQUIRE_EQ(p, (void *)&second);
  ccol_pintable_unpin(&t, mk(3, 2));
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_retired_handle_does_not_resolve) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 1, 5, &obj));
  REQUIRE_NE(ccol_pintable_pin(&t, mk(1, 5)), NULL);
  ccol_pintable_unpin(&t, mk(1, 5));

  ccol_pintable_retire(&t, 1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(1, 5)), NULL);
  ccol_pintable_dispose(&t);
}

TEST(pintable, an_outstanding_pin_is_visible_to_the_drain_after_a_retire) {
  /* The property a destroy depends on: retiring stops new pins, but a pin
   * already granted keeps the count above zero until it is released, which is
   * what holds the object alive for a caller already inside a call. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 2, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(2, 1)), NULL);
  ccol_pintable_retire(&t, 2);
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(2, 1)), NULL); /* no new pin granted */
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);  /* refusal left no trace */

  ccol_pintable_unpin(&t, mk(2, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, nested_pins_on_one_handle_are_counted_individually) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
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

TEST(pintable, reset_drops_every_outstanding_pin) {
  /* What a fork() child handler relies on: the threads that would have
   * released these pins do not exist in the child, so the count has to be
   * cleared or the child's first destroy would wait forever. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
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
  memset(&t, 0, sizeof t);
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 9, 3, &obj));
  REQUIRE_NE(ccol_pintable_pin(&t, mk(9, 3)), NULL);
  REQUIRE_EQ(ccol_pintable_pins_for(&t, mk(9, 3)), ccol_pintable_pins(&t, 9));
  REQUIRE_EQ(ccol_pintable_pins_for(&t, mk(9, 3)), (size_t)1);
  ccol_pintable_unpin(&t, mk(9, 3));
  ccol_pintable_dispose(&t);
}

TEST(pintable, an_index_in_a_later_chunk_resolves) {
  /* Crosses the chunk boundary, so the directory's own indexing is exercised
   * rather than only the first chunk. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;
  uint32_t idx = CCOL_PIN_CHUNK_SLOTS + 5;
  REQUIRE_TRUE(ccol_pintable_publish(&t, idx, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(idx, 1)), (void *)&obj);
  ccol_pintable_unpin(&t, mk(idx, 1));
  /* The first chunk stayed untouched by that. */
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(5, 1)), NULL);
  ccol_pintable_dispose(&t);
}

/* ------------------------------------------------------------------------ */
/* Concurrency                                                               */
/* ------------------------------------------------------------------------ */

typedef struct {
  ccol_pintable *t;
  uint64_t handle;
  size_t iterations;
  _Atomic size_t *resolved;
} pin_worker_arg_t;

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

TEST(pintable, concurrent_pins_balance_to_zero) {
  /* Every pin taken across every thread is released, so the count returns to
   * exactly zero. A stripe accounting error shows up here as a non-zero
   * residue, which no functional assertion elsewhere would reveal. */
  /* Enough concurrency to interleave the stripes without making the suite
   * expensive under valgrind, where every atomic is instrumented and threads
   * are serialised. A stripe accounting error is systematic rather than rare,
   * so it shows up at this size just as well as at a larger one. */
  enum { THREADS = 8, ITERS = 2000 };
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
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

/* The object a worker reaches through its pin. Holding a pin is a promise that
 * this memory stays valid, so a worker reads it while pinned; if a pin were
 * ever granted after the owner freed the object, that read is a use-after-free
 * and the magic no longer matches (and AddressSanitizer or valgrind report it
 * outright, which is where this test has most of its teeth). */
#define PIN_OBJ_MAGIC 0x5eaf00du

typedef struct {
  unsigned magic;
} pin_obj_t;

/* Workers must not start pinning until every thread of the round exists.
 * Creating a thread while the ones already created hammer this slot starves the
 * creating thread under valgrind, which runs one thread at a time: measured, a
 * single round's creation loop took 99 seconds that way, and the test's whole
 * duration swung between two seconds and several minutes depending on how the
 * scheduler happened to fall. The gate costs the race nothing, because the race
 * is between pinning and the retire that follows it, and both happen after the
 * gate opens. */
typedef struct {
  _Atomic int ready;
  _Atomic bool go;
} pin_start_gate_t;

static void pin_gate_park(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
  nanosleep(&ts, NULL);
}

typedef struct {
  ccol_pintable *t;
  uint64_t handle;
  _Atomic bool *stop;
  _Atomic size_t *violations;
  pin_start_gate_t *gate;
} race_arg_t;

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
      /* Once the slot is retired every attempt fails, so without this the
       * worker spins at full tilt until it is told to stop, which under
       * valgrind's serialised scheduling starves the thread doing the
       * retiring.
       *
       * A real sleep rather than sched_yield, which does not reliably hand the
       * scheduler over when only one thread runs at a time: yielding here
       * leaves this test's own duration bimodal under valgrind, either a couple
       * of seconds or minutes, depending on whether the six workers happen to
       * starve the drain. Sleeping is what the drain loop below already does,
       * for the same reason. The race is unaffected: this branch is only
       * reached once pinning already fails, which is after the retire the race
       * is against. */
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

TEST(pintable, a_pin_is_never_granted_against_a_drained_slot) {
  /* The race the pin and retire sides are ordered against each other to close.
   * An owner retires the slot, waits for the pin count to reach zero, and from
   * that moment treats the object as its own to free. A pin granted after that
   * point hands a caller memory that is already gone.
   *
   * The free is real, so a violation is a genuine use-after-free rather than a
   * bookkeeping mismatch. On x86-64 a weaker memory ordering passes this most
   * of the time, so the test earns most of its value on the aarch64 and arm32
   * jobs and under the sanitizers; it is kept cheap enough to run everywhere.
   */
  /* Kept modest so the whole suite stays practical under valgrind, where
   * every round pays for six spinning threads. The property is a race, so it
   * is the number of rounds times the number of runs across CI that finds it,
   * not any single run. */
  enum { THREADS = 6, ROUNDS = 60 };
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);

  _Atomic size_t violations = 0;
  size_t drain_saw_nonzero = 0;

  for (int round = 0; round < ROUNDS; round++) {
    uint32_t gen = (uint32_t)(round + 1);
    pin_obj_t *obj = malloc(sizeof *obj);
    if (!obj) break;
    obj->magic = PIN_OBJ_MAGIC;
    /* Released before the assertion rather than after it: a failing REQUIRE_
     * returns from the test function on the spot, and the object is this
     * loop's own to free until the table has taken it. */
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

    /* Released once every thread that actually started has arrived, and
       unconditionally, so a partial start still lets each started thread run to
       completion and be joined rather than parking here forever. */
    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < started)
      pin_gate_park();
    atomic_store_explicit(&gate.go, true, memory_order_release);

    /* Exactly what an owning module's destroy does. */
    ccol_pintable_retire(&t, 0);
    /* Sleeps rather than spinning or yielding. Under valgrind only one thread
     * runs at a time and sched_yield() does not reliably hand the scheduler
     * over, so a yield-spin waits out whole quanta while the workers it depends
     * on cannot run; a real sleep releases it immediately. This is the same
     * shape the library's own drain loops use. */
    /* Bounded, though the bound is a hang-safety net and not the property
     * being tested: an unbalanced stripe count is exactly what this test hunts
     * for, and an unbounded wait for it to reach zero would hang the whole
     * binary under valgrind or a loaded runner instead of failing the
     * drain_saw_nonzero check below.
     *
     * Read off the clock rather than counted in loop iterations. A sleep this
     * short costs whatever the scheduler's granularity is, tens of microseconds
     * rather than the one it asked for, so counting iterations as microseconds
     * overstates the bound by more than an order of magnitude and the net stops
     * catching anything. */
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

    /* Releasing the object while the workers still race is the point of this
       test rather than an oversight: the slot is retired and drained, so a pin
       granted after that is a genuine use-after-free that AddressSanitizer and
       valgrind report, where a bookkeeping-only check would see nothing.
       That reasoning holds only once the count has actually reached zero. The
       bounded wait above can also end with a pin still outstanding, which is
       exactly the accounting failure this test hunts, and freeing then would
       destroy the object with a worker provably inside it, killing the binary
       before the check below can report anything. So that path stops and joins
       first, and records the failure. */
    bool drained = !timed_out;
    bool joined = false;
    if (!drained) {
      drain_saw_nonzero++;
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      joined = true;
    }

    obj->magic = 0xdeadbeefu; /* poisoned before the free, so a late read that
                                 escapes the allocator still shows up */
    free(obj);

    if (!joined) {
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      if (ccol_pintable_pins(&t, 0) != 0) drain_saw_nonzero++;
    }
  }

  /* Asserted after every thread is joined, never inside the loop: a failing
   * REQUIRE_ returns from the test function immediately and would leave
   * workers running against a freed fixture. */
  REQUIRE_EQ(atomic_load(&violations), (size_t)0);
  REQUIRE_EQ(drain_saw_nonzero, (size_t)0);
  ccol_pintable_dispose(&t);
}

typedef struct {
  ccol_pintable *t;
  uint64_t handle;
} unpin_arg_t;

static void *unpin_on_another_thread(void *p) {
  unpin_arg_t *a = p;
  ccol_pintable_unpin(a->t, a->handle);
  return NULL;
}

TEST(pintable, a_split_pin_still_sums_to_zero_once_both_sides_are_visible) {
  /* ccol_pintable_unpin's contract is that a pin is released on the thread that
   * took it, and this test does not license doing otherwise: releasing
   * elsewhere splits one pin across two stripes, and ccol_pintable_pins reads
   * the stripes one at a time rather than as a snapshot, so a split pin can be
   * observed summing to zero while it is still held, which is what would let a
   * drain free an object with a caller inside it.
   *
   * What is pinned here is the narrower property the drain's convergence rests
   * on: the counters are signed, so a split reconciles to exactly zero once
   * both sides are visible, rather than leaving a permanent residue that would
   * stall every later drain on this slot. The release is joined before the sum
   * is read, so both sides are visible by construction and the unsafe
   * intermediate window is not what is being measured. Whether these two
   * threads land on different stripes at all depends on how many ids the
   * process has handed out, so the assertion is on the sum, never the split. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
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

/* The one refusal that happens after a chunk has already been published. The
 * chunk stays, deliberately: it is type-stable storage a concurrent reader may
 * already hold a pointer into, so it is never freed before dispose, and the
 * next publish into any of its slots reuses it. What must not happen is the
 * slot going live, since the caller was told the publish failed.
 *
 * This test is non-vacuous about the chunk: a failure path that unpublished it
 * instead makes the reuse assertions fail. The pin-returns-NULL assertion is
 * not what distinguishes that, and is kept for a different reason:
 * ccol_pintable_pin refuses any slot whose stripe block is still NULL, which is
 * the guard that makes a half-published slot unresolvable however its state
 * word reads, and that is worth pinning in its own right. */
TEST(pintable,
     a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof t);
  int obj = 1;

  _ccol_pintable_force_next_stripe_alloc_failure_for_tests();
  REQUIRE_FALSE(ccol_pintable_publish(&t, 0, 1, &obj));

  /* Refused, so nothing resolves and nothing is charged. */
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);

  /* The chunk was kept rather than unpublished. */
  REQUIRE_EQ(chunks_held(&t), (size_t)1);

  /* And the slot is still fully usable. */
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 2, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 2)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);
  ccol_pintable_unpin(&t, mk(0, 2));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}
