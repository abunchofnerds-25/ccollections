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

/* A handle is (index << 32) | generation, which is the form every module
 * that uses this type builds. */

static uint64_t mk(uint32_t idx, uint32_t gen) {
  return ((uint64_t)idx << 32) | (uint64_t)gen;
}

/* Tau's REQUIRE_* macros return from the test function as soon as one fails,
 * so a table that only the last statement frees keeps its chunks and stripe
 * blocks on exactly the runs that matter, and memtest then reports a leak on
 * top of the assertion that caused it. A table with a cleanup attribute is
 * freed on every way out. Each test also disposes of the table explicitly at
 * its end, and since ccol_pintable_dispose unpublishes a chunk before it
 * frees it, the second call finds nothing left to do. */
static void dispose_pintable(ccol_pintable *t) { ccol_pintable_dispose(t); }

/* The number of chunks the table holds right now. A refusal that comes
 * before any allocation must leave this at zero. The return value alone
 * can't tell "refused" apart from "allocated a chunk, stored a pointer and
 * then reported a failure", so this count checks the half of those tests'
 * claims that nothing else checks. It doesn't hold for every refusal though:
 * a stripe allocation that fails after its chunk is published keeps that
 * chunk on purpose, and
 * a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable pins
 * that. */
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

/* Slot 0 is an ordinary slot and it usually holds an object, so a zero
 * handle that reaches unpin must not name it. An owner that hasn't written
 * its own self-handle field yet holds zero there, because such structs start
 * as all zero bytes. Decrementing slot 0 for that owner pushes a real
 * object's count below the truth, and a later drain then finishes while a
 * caller still holds the object.
 *
 * Remove either guard from ccol_pintable_unpin or from
 * ccol_pintable_pins_for and both tests fail here.
 * Without the guards the code corrupts the count of an unrelated owner in a
 * build that looks perfectly healthy. */
TEST(pintable,
     a_zero_handle_is_ignored_by_unpin_rather_than_charged_to_slot_zero) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* The pin above belongs to slot 0 and this call must not touch it. */
  ccol_pintable_unpin(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* A zero handle doesn't name a slot to report on either. */
  REQUIRE_EQ(ccol_pintable_pins_for(&t, 0), (size_t)0);

  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable,
     a_zero_handle_is_ignored_by_reset_rather_than_clearing_slot_zero) {
  /* A fork() child handler drops an owner's pins by handle, and an owner
   * that hasn't written its handle field yet carries zero. Clearing slot 0
   * for that owner drops the pin of a caller that really is in flight on an
   * unrelated object, which is exactly what the count is there to prevent.
   *
   * Code that derives the index from the handle without the guard fails
   * here. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_reset_for(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* With the right name the clear does happen. */
  ccol_pintable_reset_for(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_handle_against_an_empty_table_resolves_to_nothing) {
  /* A module resolves a handle before it ever publishes one, since a caller
   * can pass a stale or made-up handle, so a table of all zero bytes needs to
   * answer instead of faulting. */
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
  /* The refusal comes before the code builds anything, so the slot the call
     names stays empty and a later publish into it starts from nothing. */
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
  /* A different object reuses the slot, the way a module's free-index list
   * does. */
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

TEST(pintable, an_outstanding_pin_is_visible_to_the_drain_after_a_retire) {
  /* A destroy depends on this: a retire stops every new pin, but a pin the
   * table already granted keeps the count above zero until it's released.
   * That's what keeps the object alive for a caller who is already inside a
   * call. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 2, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(2, 1)), NULL);
  ccol_pintable_retire(&t, 2);
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(2, 1)), NULL); /* no new pin granted */
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);  /* and the refusal left
                                                        no trace */

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

TEST(pintable, reset_drops_every_outstanding_pin) {
  /* A fork() child handler depends on this. The threads that would release
   * these pins don't exist in the child, so the count has to be cleared, and
   * without the clear the child's first destroy waits forever. */
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
  /* This crosses the chunk boundary, so the test exercises the directory's
   * index arithmetic, beyond the first chunk. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  uint32_t idx = CCOL_PIN_CHUNK_SLOTS + 5;
  REQUIRE_TRUE(ccol_pintable_publish(&t, idx, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(idx, 1)), (void *)&obj);
  ccol_pintable_unpin(&t, mk(idx, 1));
  /* And the first chunk wasn't touched. */
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
  /* Every thread releases every pin it takes, so the count goes back to
   * exactly zero. A mistake in a stripe's accounting shows up here as a
   * non-zero residue, which no functional assertion anywhere else would show.
   *
   * These numbers give enough concurrency to interleave the stripes while
   * keeping the suite cheap under valgrind, which instruments every atomic
   * operation and runs one thread at a time. A stripe accounting error is
   * systematic rather than rare, so it shows up at this size just as it
   * would at a larger one. */
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

/* The object a worker reaches through its pin. A pin promises that this
 * memory stays valid, so the worker reads the object while it holds one. If
 * the table granted a pin after the owner freed the object, that read would
 * be a use-after-free and the magic value wouldn't match. AddressSanitizer
 * and valgrind also report such a read directly, and that's where most of
 * this test's strength comes from. */
#define PIN_OBJ_MAGIC 0x5eaf00du

typedef struct {
  unsigned magic;
} pin_obj_t;

/* No worker takes a pin until every thread of the round exists. Valgrind
 * runs one thread at a time, so a thread that the test creates while the
 * existing ones hammer this slot starves. Measured without this gate, the
 * creation loop of a single round took 99 seconds and the test's duration
 * swung between two seconds and several minutes depending on the scheduler.
 * The gate costs the race nothing, because the race is between a pin and
 * the retire that follows it, and both happen after the gate opens. */
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
      /* Once the slot is retired every attempt fails. Without this sleep
       * the worker would spin at full speed until the test tells it to
       * stop, and since valgrind runs one thread at a time that spin starves
       * the thread doing the retire.
       *
       * It's a real sleep and not sched_yield, because a yield doesn't
       * reliably hand over the scheduler when only one thread runs at a
       * time. With a yield here the duration of this test under valgrind is
       * bimodal (a few seconds or several minutes), depending on whether the
       * six workers starve the drain. The drain loop below sleeps for the
       * same reason. The race isn't affected: the code only gets to this
       * branch after a pin has failed, which is after the retire the race is
       * against. */
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

TEST(pintable, a_pin_is_never_granted_against_a_drained_slot) {
  /* This is the race that the ordering between the pin side and the retire
   * side closes. An owner retires the slot and waits for the pin count to
   * reach zero, and from that moment on it treats the object as its own to
   * free. A pin the table grants after that point hands a caller memory that
   * is already gone.
   *
   * The free is real, so a violation is a true use-after-free and not just a
   * bookkeeping mismatch. On x86-64 a weaker memory order passes this test
   * most of the time, so most of its value comes from the aarch64 and arm32
   * jobs and the sanitizers, but it's cheap enough to run everywhere.
   *
   * The numbers are kept small so the whole suite stays practical under
   * valgrind, where every round pays for six spinning threads. The property
   * is a race, so what finds it is the number of rounds times the number of
   * runs across CI, not a single run. */
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
    /* The free comes before the assertion, because a failing REQUIRE_
     * returns from the test function at once. The object belongs to this
     * loop until the table takes it. */
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

    /* The gate opens once every thread that really started has arrived, and
       it opens on every path, so after a partial start each started thread
       can still finish and be joined and nothing parks here forever. */
    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < started)
      pin_gate_park();
    atomic_store_explicit(&gate.go, true, memory_order_release);

    /* This is exactly what an owning module's destroy does. */
    ccol_pintable_retire(&t, 0);
    /* This loop sleeps rather than spinning or yielding. Under valgrind
     * only one thread runs at a time and sched_yield() doesn't reliably hand
     * over the scheduler, so a spin with a yield waits out whole quanta
     * while the workers it depends on can't run, and a real sleep lets them
     * go at once. The library's drain loops have the same shape.
     *
     * The wait is bounded, but the bound is only a safety net against a
     * hang, it's not the property under test. A stripe count that doesn't
     * balance is exactly what this test is hunting for, and an unbounded
     * wait for that count to reach zero would hang the whole binary under
     * valgrind or on a loaded runner, so the drain_saw_nonzero check below
     * could never fail.
     *
     * The bound comes from the clock and not from counting iterations. A
     * sleep this short costs the scheduler's granularity, tens of
     * microseconds rather than the one microsecond the code asks for, so
     * counting iterations as microseconds would overstate the bound by more
     * than ten times and the net would catch nothing. */
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

    /* Freeing the object while the workers are still racing is the whole
       point of this test. The slot is retired and drained, so a pin after
       that point is a true use-after-free, which AddressSanitizer and
       valgrind report while a check of the bookkeeping alone sees nothing.
       That only holds once the count really reaches zero though, and the
       bounded wait above can also end with a pin still outstanding, which is
       exactly the accounting failure we are hunting. Freeing at that moment
       would destroy the object with a worker provably inside it and kill
       the binary before the check below could report anything, so on that
       path we stop the workers, join them first and record the failure. */
    bool drained = !timed_out;
    bool joined = false;
    if (!drained) {
      drain_saw_nonzero++;
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      joined = true;
    }

    obj->magic = 0xdeadbeefu; /* poisoned before the free, so a late read
                                 that gets past the allocator is still
                                 visible */
    free(obj);

    if (!joined) {
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      if (ccol_pintable_pins(&t, 0) != 0) drain_saw_nonzero++;
    }
  }

  /* These assertions run after every thread is joined and never inside the
   * loop, because a failing REQUIRE_ returns from the test function at once
   * and would leave the workers running on a fixture that is already freed. */
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
  /* ccol_pintable_unpin's contract is that the thread which takes a pin is
   * the one that releases it, and this test doesn't allow anything else. A
   * release on another thread splits one pin across two stripes, and since
   * ccol_pintable_pins reads the stripes one at a time instead of as one
   * snapshot, a split pin can sum to zero while a caller still holds it,
   * which is what would let a drain free an object with a caller inside it.
   *
   * What this test pins is the narrower property the drain's convergence
   * rests on: the counters are signed, so once both sides are visible a split
   * sums to exactly zero and leaves no permanent residue that would stall
   * every later drain on this slot. The release is joined before we read the
   * sum, so both sides are visible by construction, and the unsafe window in
   * between isn't what we measure here. Whether the two threads land on
   * different stripes depends on how many ids the process has handed out,
   * so we assert on the sum and never on the split. */
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

/* This is the one refusal that happens after the table has published a
 * chunk. The chunk stays on purpose. It is type-stable storage and a
 * concurrent reader may already hold a pointer into it, so nothing frees it
 * before dispose and the next publish into any of its slots reuses it. What
 * must not happen is the slot becoming live, because the caller was told the
 * publish failed.
 *
 * The chunk part catches a real mistake: a failure path that unpublished
 * the chunk makes the reuse assertions fail. The assertion that the pin
 * returns NULL isn't what tells those two cases apart, it's here for another
 * reason. ccol_pintable_pin refuses every slot whose stripe block is NULL,
 * and that guard is what makes a half-published slot impossible to resolve
 * whatever its state word says, which deserves a check of its own. */
TEST(pintable,
     a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;

  _ccol_pintable_force_next_stripe_alloc_failure_for_tests();
  REQUIRE_FALSE(ccol_pintable_publish(&t, 0, 1, &obj));

  /* The publish was refused, so nothing resolves and nothing is charged. */
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), NULL);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);

  /* The table kept the chunk and didn't unpublish it. */
  REQUIRE_EQ(chunks_held(&t), (size_t)1);

  /* And the slot is fully usable. */
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 2, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 2)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);
  ccol_pintable_unpin(&t, mk(0, 2));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}
