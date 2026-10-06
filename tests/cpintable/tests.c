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
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "tau/tau.h"

TAU_MAIN()

/* A handle is (index << 32) | generation. This is the same form that every
 * module that uses this type makes. */
extern void _ccol_pintable_force_next_stripe_alloc_failure_for_tests(void);

static uint64_t mk(uint32_t idx, uint32_t gen) {
  return ((uint64_t)idx << 32) | (uint64_t)gen;
}

/* The REQUIRE_* macros of Tau return from the test function the moment one of
 * them fails. A table that only a last statement frees therefore keeps its
 * chunks and its stripe blocks on exactly the runs that matter. memtest then
 * reports a leak on top of the assertion that caused it. A table that carries
 * a cleanup attribute is freed on every path out. Each test also disposes of
 * the table explicitly at its end. ccol_pintable_dispose unpublishes a chunk
 * before it frees the chunk, so that second call finds nothing to do. */
static void dispose_pintable(ccol_pintable *t) { ccol_pintable_dispose(t); }

/* The number of chunks that the table holds now. A refusal that happens
 * before any allocation must leave this at zero. The return value alone
 * cannot separate "refused" from "allocated a chunk, stored a pointer, and
 * then reported a failure". That is the half of the claims of those tests
 * that nothing else checks. This is not true of every refusal. A stripe
 * allocation that fails after its chunk is published keeps that chunk on
 * purpose, and
 * a_failed_stripe_allocation_keeps_the_chunk_and_leaves_the_slot_reusable
 * pins that. */
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

/* Slot 0 is an ordinary slot, and it usually holds an object. A zero handle
 * that reaches unpin must therefore not name it. An owner that did not yet
 * write its own self-handle field holds zero, because such structs start as
 * all zero bytes. A decrement of slot 0 for that owner drives the count of a
 * real object below the truth. A later drain then finishes while a caller
 * still holds the object.
 *
 * These two tests are not vacuous. Remove either guard from
 * ccol_pintable_unpin or from ccol_pintable_pins_for, and the tests fail
 * here. Without them, the code corrupts the count of an unrelated owner in a
 * build that looks healthy. */
TEST(pintable,
     a_zero_handle_is_ignored_by_unpin_rather_than_charged_to_slot_zero) {
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* The pin above belongs to slot 0. This call must not touch it. */
  ccol_pintable_unpin(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* A zero handle also names no slot to report on. */
  REQUIRE_EQ(ccol_pintable_pins_for(&t, 0), (size_t)0);

  ccol_pintable_unpin(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable,
     a_zero_handle_is_ignored_by_reset_rather_than_clearing_slot_zero) {
  /* A fork() child handler drops the pins of an owner by handle. An owner
   * that did not yet write its handle field carries zero. A clear of slot 0
   * for that owner drops the pin of a caller that is truly in flight, on an
   * unrelated object. That is the one thing that the count exists to prevent.
   *
   * This test is not vacuous. Code that derives the index from the handle
   * without the guard fails here. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 7;

  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 1)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  ccol_pintable_reset_for(&t, 0);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);

  /* With the correct name, the clear happens. */
  ccol_pintable_reset_for(&t, mk(0, 1));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}

TEST(pintable, a_handle_against_an_empty_table_resolves_to_nothing) {
  /* A module resolves a handle before it publishes one, because a caller can
   * pass a stale handle or an invented one. A table of all zero bytes must
   * therefore answer, and must not fault. */
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
  /* The refusal happens before the code builds anything. The slot that the
     call names stays empty, and a later publish into it starts from
     nothing. */
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
  /* A different object reuses the slot, in the same way as the free-index
   * list of a module. */
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
  /* This is the property that a destroy depends on. A retire stops every new
   * pin. A pin that the table already granted keeps the count above zero
   * until its release. That is what holds the object alive for a caller that
   * is already inside a call. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  REQUIRE_TRUE(ccol_pintable_publish(&t, 2, 1, &obj));

  REQUIRE_NE(ccol_pintable_pin(&t, mk(2, 1)), NULL);
  ccol_pintable_retire(&t, 2);
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(2, 1)), NULL); /* no new pin granted */
  REQUIRE_EQ(ccol_pintable_pins(&t, 2), (size_t)1);  /* the refusal left
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
  /* This is what a fork() child handler depends on. The threads that would
   * release these pins do not exist in the child. The count must therefore be
   * cleared. Without that clear, the first destroy in the child waits
   * forever. */
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
  /* This crosses the chunk boundary. The test therefore exercises the index
   * arithmetic of the directory, and not only the first chunk. */
  SCOPED_PINTABLE(t);
  memset(&t, 0, sizeof(t));
  int obj = 1;
  uint32_t idx = CCOL_PIN_CHUNK_SLOTS + 5;
  REQUIRE_TRUE(ccol_pintable_publish(&t, idx, 1, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(idx, 1)), (void *)&obj);
  ccol_pintable_unpin(&t, mk(idx, 1));
  /* That did not touch the first chunk. */
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
  /* Every thread releases every pin that it takes, so the count returns to
   * exactly zero. An error in the accounting of a stripe appears here as a
   * non-zero residue. No functional assertion anywhere else shows it.
   *
   * These numbers give enough concurrency to interleave the stripes. They
   * also keep the suite cheap under valgrind, which instruments every atomic
   * operation and runs one thread at a time. An error in the accounting of a
   * stripe is systematic, and not rare, so it appears at this size as well as
   * at a larger one. */
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

/* The object that a worker reaches through its pin. A pin is a promise that
 * this memory stays valid, so a worker reads the object while it holds a pin.
 * A pin that the table grants after the owner freed the object turns that read
 * into a use-after-free, and the magic value no longer matches.
 * AddressSanitizer and valgrind also report such a read directly, and that is
 * where this test has most of its strength. */
#define PIN_OBJ_MAGIC 0x5eaf00du

typedef struct {
  unsigned magic;
} pin_obj_t;

/* No worker takes a pin until every thread of the round exists. Valgrind runs
 * one thread at a time. A thread that the test creates while the threads that
 * already exist hammer this slot therefore starves. Measured, the creation
 * loop of one round took 99 seconds that way, and the whole duration of the
 * test moved between two seconds and several minutes with the decisions of the
 * scheduler. The gate costs the race nothing. The race is between a pin and
 * the retire after it, and both happen after the gate opens. */
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
      /* After the retire of the slot, every attempt fails. Without this
       * sleep, the worker spins at full speed until the test tells it to
       * stop. Valgrind runs one thread at a time, and that spin starves the
       * thread that does the retire.
       *
       * This is a real sleep, and not sched_yield. A yield does not reliably
       * hand the scheduler over when only one thread runs at a time. A yield
       * here makes the duration of this test bimodal under valgrind, either a
       * few seconds or several minutes, and that depends on whether the six
       * workers starve the drain. The drain loop below already sleeps, for
       * the same reason. This does not change the race. The code reaches this
       * branch only after a pin already fails, which is after the retire that
       * the race is against. */
      struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000};
      nanosleep(&ts, NULL);
    }
  }
  return NULL;
}

TEST(pintable, a_pin_is_never_granted_against_a_drained_slot) {
  /* This is the race that the order between the pin side and the retire side
   * closes. An owner retires the slot and waits for the pin count to reach
   * zero. From that moment the owner treats the object as its own to free. A
   * pin that the table grants after that point gives a caller memory that is
   * already gone.
   *
   * The free is real, so a violation is a true use-after-free, and not a
   * mismatch in the bookkeeping. On x86-64 a weaker memory order passes this
   * test most of the time. The test therefore earns most of its value on the
   * aarch64 and arm32 jobs, and under the sanitizers. It stays cheap enough
   * to run everywhere.
   *
   * These numbers stay small, so that the whole suite stays practical under
   * valgrind, where each round pays for six threads that spin. The property
   * is a race. The number of rounds, times the number of runs across CI, is
   * what finds it. One run alone does not. */
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
    /* The free happens before the assertion, and not after it. A REQUIRE_
     * that fails returns from the test function at once. The object belongs
     * to this loop until the table takes it. */
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

    /* The gate opens after every thread that truly started arrives, and it
       opens on every path. A partial start therefore still lets each started
       thread finish and be joined. Nothing parks here forever. */
    while (atomic_load_explicit(&gate.ready, memory_order_acquire) < started)
      pin_gate_park();
    atomic_store_explicit(&gate.go, true, memory_order_release);

    /* Exactly what the destroy of an owning module does. */
    ccol_pintable_retire(&t, 0);
    /* This loop sleeps, and does not spin or yield. Under valgrind only one
     * thread runs at a time, and sched_yield() does not reliably hand the
     * scheduler over. A spin with a yield therefore waits out whole quanta
     * while the workers that it depends on cannot run. A real sleep releases
     * it at once. The drain loops of the library have the same shape.
     *
     * The wait has a bound. That bound is a safety net against a hang, and is
     * not the property under test. A stripe count that does not balance is
     * exactly what this test hunts for. A wait with no bound for that count
     * to reach zero hangs the whole binary under valgrind or on a loaded
     * runner, and the drain_saw_nonzero check below never fails.
     *
     * The bound comes from the clock, and not from a count of loop
     * iterations. A sleep this short costs the granularity of the scheduler,
     * which is tens of microseconds and not the one microsecond that the
     * code asks for. A count of iterations as microseconds therefore
     * overstates the bound by more than a factor of ten, and the net catches
     * nothing. */
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

    /* The free of the object while the workers still race is the point of
       this test, and not an oversight. The slot is retired and drained, so a
       pin after that point is a true use-after-free. AddressSanitizer and
       valgrind report it, where a check of the bookkeeping alone sees
       nothing.
       That reasoning holds only after the count truly reaches zero. The
       bounded wait above can also end with a pin still outstanding. That is
       exactly the accounting failure that this test hunts. A free at that
       moment destroys the object with a worker provably inside it, and kills
       the binary before the check below can report anything. That path
       therefore stops the workers, joins them first, and records the
       failure. */
    bool drained = !timed_out;
    bool joined = false;
    if (!drained) {
      drain_saw_nonzero++;
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      joined = true;
    }

    obj->magic = 0xdeadbeefu; /* poisoned before the free, so a late read
                                 that escapes the allocator is still
                                 visible */
    free(obj);

    if (!joined) {
      atomic_store_explicit(&stop, true, memory_order_relaxed);
      for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
      if (ccol_pintable_pins(&t, 0) != 0) drain_saw_nonzero++;
    }
  }

  /* These assertions run after every thread is joined, and never inside the
   * loop. A REQUIRE_ that fails returns from the test function at once, and
   * would leave the workers at work on a fixture that is already freed. */
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
  /* The contract of ccol_pintable_unpin is that the thread that takes a pin
   * is the thread that releases it. This test does not permit anything else.
   * A release on another thread splits one pin across two stripes.
   * ccol_pintable_pins reads the stripes one at a time, and not as one
   * snapshot. A split pin can therefore sum to zero while a caller still
   * holds it. That is what would let a drain free an object with a caller
   * inside it.
   *
   * This test pins the narrower property that the convergence of the drain
   * rests on. The counters are signed, so a split sums to exactly zero after
   * both sides are visible. It leaves no permanent residue, which would stall
   * every later drain on this slot. The test joins the release before it
   * reads the sum, so both sides are visible by construction. The unsafe
   * window in the middle is not what this test measures. Whether these two
   * threads land on different stripes depends on how many ids the process
   * gave out. The assertion is therefore on the sum, and never on the
   * split. */
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

/* This is the one refusal that happens after the table already published a
 * chunk. The chunk stays, and this is deliberate. It is type-stable storage,
 * and a concurrent reader can already hold a pointer into it. Nothing frees it
 * before dispose, and the next publish into any of its slots reuses it. The
 * one thing that must not happen is the slot becoming live, because the
 * caller was told that the publish failed.
 *
 * This test is not vacuous about the chunk. A failure path that unpublished
 * the chunk makes the reuse assertions fail. The assertion that the pin
 * returns NULL is not what separates those two cases. It is here for a
 * different reason: ccol_pintable_pin refuses every slot whose stripe block is
 * still NULL. That guard is what makes a half-published slot impossible to
 * resolve, whatever its state word says, and that is worth a check of its
 * own. */
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

  /* The table kept the chunk, and did not unpublish it. */
  REQUIRE_EQ(chunks_held(&t), (size_t)1);

  /* The slot is also still fully usable. */
  REQUIRE_TRUE(ccol_pintable_publish(&t, 0, 2, &obj));
  REQUIRE_EQ(ccol_pintable_pin(&t, mk(0, 2)), (void *)&obj);
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)1);
  ccol_pintable_unpin(&t, mk(0, 2));
  REQUIRE_EQ(ccol_pintable_pins(&t, 0), (size_t)0);
  ccol_pintable_dispose(&t);
}
