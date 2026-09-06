#include <common.h>
#include <common_invariants.h>
#include <cthreadpool.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

extern struct cthread_pool *_ctpool_resolve_for_tests(ctpool h);
extern size_t _ctpool_slot_table_capacity_for_tests(void);
extern size_t _ctpool_task_free_list_size_for_tests(struct cthread_pool *pool);
extern size_t _ctpool_task_free_list_cap_for_tests(struct cthread_pool *pool);

/* ========================================================================== */
/*                         SHARED TASK FUNCTIONS                              */
/* ========================================================================== */

static void sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* Increment the atomic_int pointed to by arg. */
static void inc_counter(void *arg) { atomic_fetch_add((atomic_int *)arg, 1); }

/* Sleep 20 ms then increment. */
static void slow_inc(void *arg) {
  sleep_ms(20);
  atomic_fetch_add((atomic_int *)arg, 1);
}

/* Spin-wait on a gate. */
typedef struct {
  atomic_int *gate;
  atomic_int *started;
} gate_ctx_t;

static void blocker_fn(void *arg) {
  gate_ctx_t *ctx = (gate_ctx_t *)arg;
  atomic_store(ctx->started, 1);
  while (!atomic_load(ctx->gate)) sleep_ms(1);
}

/*
 * Helper used by shutdown_immediate/queued_tasks_discarded: a pthread start
 * routine (not a pool task) that releases a gate after a brief delay so the
 * blocked worker can exit once the queue has been discarded by
 * ctpool_shutdown_immediate running on the main thread.
 */
static void *release_gate_fn(void *arg) {
  sleep_ms(20);
  atomic_store((atomic_int *)arg, 1);
  return NULL;
}

/* Call ctpool_wait on the pool passed as arg; used to block a background
 * thread so the main thread can race ctpool_shutdown_immediate against it.
 * arg is a `ctpool *` (the address of the caller's own local handle
 * variable), not the handle value itself: ctpool is now a uint64_t value
 * handle, not a pointer, so it can no longer be round-tripped through
 * void* by value the way a raw pointer handle could. */
static void *pool_wait_thread(void *arg) {
  ctpool *p = (ctpool *)arg;
  ctpool_wait(*p);
  return NULL;
}

/* Submit n tasks to pool, incrementing *counter per task; used by
 * load/concurrent_producers to exercise multi-threaded submission. */
typedef struct {
  ctpool pool;
  atomic_int *counter;
  int n;
} producer_arg_t;

static void *producer_thread(void *arg) {
  producer_arg_t *a = (producer_arg_t *)arg;
  for (int i = 0; i < a->n; i++) {
    ctpool_submit(a->pool, inc_counter, a->counter, NULL);
  }
  return NULL;
}

/* Return the arg pointer unchanged (identity future). */
static void *identity_fn(void *arg) { return arg; }

/* Sleep 20 ms then return arg. */
static void *slow_identity(void *arg) {
  sleep_ms(20);
  return arg;
}

/* Gate-blocked future task: spins until gate is released, then returns result.
 * Used to guarantee a future cannot complete before an explicit release. */
typedef struct {
  atomic_int *gate;
  void *result;
} gated_future_ctx_t;

static void *gated_identity(void *arg) {
  gated_future_ctx_t *ctx = (gated_future_ctx_t *)arg;
  while (!atomic_load(ctx->gate)) sleep_ms(1);
  return ctx->result;
}

/* No-op task: ignores its argument.  Lets on_complete be tested independently
 * of what the task function does to the same arg. */
static void noop_fn(void *arg) { (void)arg; }

/* Sleep 10 ms then increment (phase-1 task). */
static void phase1_fn(void *arg) {
  sleep_ms(10);
  atomic_fetch_add((atomic_int *)arg, 1);
}

typedef struct {
  atomic_int *p1_count;
  atomic_int *ok;
  int expected_p1;
} phase2_ctx_t;

static void phase2_fn(void *arg) {
  phase2_ctx_t *c = (phase2_ctx_t *)arg;
  if (atomic_load(c->p1_count) < c->expected_p1) {
    atomic_store(c->ok, 0);
  }
}

typedef struct {
  atomic_int *fn_done;
  atomic_int *cb_done;
} cb_ctx_t;

static void cb_fn(void *arg) { atomic_store(((cb_ctx_t *)arg)->fn_done, 1); }

static void cb_cb(void *arg) {
  REQUIRE_EQ(atomic_load(((cb_ctx_t *)arg)->fn_done), 1);
  atomic_store(((cb_ctx_t *)arg)->cb_done, 1);
}

/* ========================================================================== */
/*                         CUSTOM ALLOCATOR TRACKING                         */
/* ========================================================================== */

static atomic_int g_alloc_count = 0;
static atomic_int g_free_count = 0;

static void *tracked_malloc(size_t size) {
  atomic_fetch_add(&g_alloc_count, 1);
  return malloc(size);
}
static void tracked_free(void *ptr) {
  if (ptr) atomic_fetch_add(&g_free_count, 1);
  free(ptr);
}
static void *tracked_calloc(size_t n, size_t size) {
  atomic_fetch_add(&g_alloc_count, 1);
  return calloc(n, size);
}
static void *tracked_realloc(void *ptr, size_t size) {
  atomic_fetch_add(&g_alloc_count, 1);
  return realloc(ptr, size);
}

/* ========================================================================== */
/*                         OOM ALLOCATOR                                      */
/* ========================================================================== */

/*
 * When g_oom_enabled is non-zero every allocation via the pool's custom procs
 * returns NULL.  Used to exercise ccol_not_enough_memory paths.  Enable AFTER
 * pool creation so the pool itself is set up with real memory.
 */
static atomic_int g_oom_enabled = 0;

static void *oom_malloc(size_t size) {
  if (atomic_load(&g_oom_enabled)) return NULL;
  return malloc(size);
}
static void oom_free(void *ptr) { free(ptr); }
static void *oom_calloc(size_t n, size_t size) {
  if (atomic_load(&g_oom_enabled)) return NULL;
  return calloc(n, size);
}
static void *oom_realloc(void *ptr, size_t size) {
  if (atomic_load(&g_oom_enabled)) return NULL;
  return realloc(ptr, size);
}

/* ========================================================================== */
/*                         CONSTRUCTION / DESTRUCTION                         */
/* ========================================================================== */

TEST(construction, unbounded_default) {
  ctpool_construct(pool, 2, 0);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, bounded) {
  ctpool_construct(pool, 4, 64);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, ccol_invalid_size_means_unbounded) {
  char *err = NULL;
  ctpool pool = create_cthread_pool(2, ccol_invalid_size, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, zero_threads_fails) {
  char *err = NULL;
  ctpool pool = create_cthread_pool(0, 0, &err);
  REQUIRE_EQ(pool, CTPOOL_INVALID);
  REQUIRE_NE((void *)err, NULL);
}

TEST(construction, destroy_without_explicit_shutdown_drains) {
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 0);
  ctpool_submit(pool, inc_counter, &counter, NULL);
  ctpool_submit(pool, inc_counter, &counter, NULL);
  ctpool_destroy(pool);
  REQUIRE_EQ(atomic_load(&counter), 2);
}

TEST(construction, construct_scoped_auto_destroys) {
  /* ctpool_construct_scoped must drain and free the pool automatically when
   * the enclosing block exits.  Verified by checking tasks completed and by
   * valgrind for leaks. */
  atomic_int counter = 0;
  {
    ctpool_construct_scoped(pool, 2, 0);
    ctpool_submit(pool, inc_counter, &counter, NULL);
    ctpool_submit(pool, inc_counter, &counter, NULL);
  } /* pool is auto-destroyed here */
  REQUIRE_EQ(atomic_load(&counter), 2);
}

TEST(construction, declare_scoped_auto_destroys) {
  atomic_int counter = 0;
  {
    ctpool_declare_scoped(pool);
    char *err = NULL;
    pool = create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);
    ctpool_submit(pool, inc_counter, &counter, NULL);
  } /* pool is auto-destroyed here */
  REQUIRE_EQ(atomic_load(&counter), 1);
}

/* ========================================================================== */
/*                         BASIC SUBMISSION                                   */
/* ========================================================================== */

TEST(submit, tasks_execute) {
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  for (int i = 0; i < 100; i++) {
    ccol_retval_t r = ctpool_submit(pool, inc_counter, &counter, NULL);
    REQUIRE_EQ(r, ccol_success);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 100);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(submit, on_complete_fires_after_fn) {
  atomic_int fn_done = 0;
  atomic_int cb_done = 0;
  cb_ctx_t ctx = {.fn_done = &fn_done, .cb_done = &cb_done};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, cb_fn, &ctx, cb_cb);
  ctpool_wait(pool);

  REQUIRE_EQ(atomic_load(&fn_done), 1);
  REQUIRE_EQ(atomic_load(&cb_done), 1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(submit, rejected_after_shutdown_drain) {
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_drain(pool);
  ccol_retval_t r = ctpool_submit(pool, inc_counter, NULL, NULL);
  REQUIRE_EQ(r, ccol_not_permitted);
  ctpool_destroy(pool);
}

TEST(submit, rejected_after_shutdown_immediate) {
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_immediate(pool);
  ccol_retval_t r = ctpool_submit(pool, inc_counter, NULL, NULL);
  REQUIRE_EQ(r, ccol_not_permitted);
  ctpool_destroy(pool);
}

TEST(submit, invalid_args) {
  ctpool_construct(pool, 2, 0);
  REQUIRE_EQ(ctpool_submit(CTPOOL_INVALID, inc_counter, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(ctpool_try_submit(CTPOOL_INVALID, inc_counter, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_try_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  REQUIRE_EQ(ctpool_timed_submit(CTPOOL_INVALID, inc_counter, NULL, NULL, &ts),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_timed_submit(pool, NULL, NULL, NULL, &ts),
             ccol_invalid_args);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(submit, returns_not_enough_memory) {
  ccol_memmgmt_procs_t mp = {.malloc = oom_malloc,
                             .free = oom_free,
                             .calloc = oom_calloc,
                             .realloc = oom_realloc};
  char *err = NULL;
  ctpool pool = create_cthread_pool_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  atomic_int counter = 0;
  atomic_store(&g_oom_enabled, 1);
  ccol_retval_t r = ctpool_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  atomic_store(&g_oom_enabled, 0);

  /* Pool must still accept work once memory is available again. */
  r = ctpool_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         TRY_SUBMIT / TIMED_SUBMIT                         */
/* ========================================================================== */

TEST(try_submit, returns_container_full_when_bounded_queue_saturated) {
  /* Single worker, 1-slot queue.  Block the worker so the queue fills up,
   * then try_submit must return ccol_container_full without blocking. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);

  /* Occupy the worker with a gate-blocked task. */
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  /* Fill the 1-slot queue. */
  ctpool_submit(pool, inc_counter, &counter, NULL);

  /* Queue is full: try_submit must return immediately. */
  ccol_retval_t r = ctpool_try_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_container_full);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(try_submit, succeeds_when_space_available) {
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 16);
  ccol_retval_t r = ctpool_try_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, times_out_on_full_bounded_queue) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL);

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L}; /* 50 ms */
  ccol_retval_t r = ctpool_timed_submit(pool, inc_counter, &counter, NULL, &ts);
  REQUIRE_EQ(r, ccol_timed_out);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, succeeds_before_timeout) {
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 16);
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  ccol_retval_t r = ctpool_timed_submit(pool, inc_counter, &counter, NULL, &ts);
  REQUIRE_EQ(r, ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, null_timeout_acts_as_try_submit) {
  /* With NULL timeout ctpool_timed_submit must return ccol_container_full
   * immediately on a full bounded queue, not block. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* fill the 1-slot queue */

  ccol_retval_t r =
      ctpool_timed_submit(pool, inc_counter, &counter, NULL, NULL);
  REQUIRE_EQ(r, ccol_container_full);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, null_timeout_with_invalid_args_still_reports_invalid_args) {
  /* Coverage gap found while factoring ctpool_submit/_try_submit/
   * _timed_submit into a shared internal helper: no existing test combined
   * a NULL timeout (the delegation path straight to ctpool_try_submit) with
   * an invalid pool or a NULL fn. Both must still report ccol_invalid_args
   * via the delegated call, exactly as they would via a direct
   * ctpool_try_submit call. */
  ctpool_construct(pool, 2, 0);

  REQUIRE_EQ(ctpool_timed_submit(CTPOOL_INVALID, inc_counter, NULL, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_timed_submit(pool, NULL, NULL, NULL, NULL),
             ccol_invalid_args);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, unbounded_queue_never_blocks_on_capacity) {
  /* For an unbounded queue the timeout is irrelevant: ctpool_timed_submit must
   * always succeed immediately regardless of how short the timeout is. */
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 0);

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1}; /* 1 ns; effectively zero */
  for (int i = 0; i < 20; i++) {
    ccol_retval_t r =
        ctpool_timed_submit(pool, inc_counter, &counter, NULL, &ts);
    REQUIRE_EQ(r, ccol_success);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 20);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, malformed_negative_tv_nsec_normalized_correctly) {
  /* Regression coverage for a real bug in make_abs_deadline: a
   * caller-supplied timeout whose tv_nsec is negative (a malformed,
   * non-normalised struct timespec that POSIX never itself produces, but
   * which nothing here previously rejected; e.g. the result of
   * subtracting two timespecs to compute a remaining budget without
   * separately normalising that subtraction's own result) used to flow
   * straight through into the absolute deadline handed to
   * cond_var_timedwait with no correction, leaving that deadline's own
   * tv_nsec also possibly negative: undefined behaviour per POSIX. This
   * mirrors the identical hazard already found and fixed for
   * add_duration_to_timespec in cthreadcomm.c.
   *
   * {1, -500000000} means "0.5 seconds" once correctly normalised (borrow
   * one second, add it back as +1e9 ns): with a 1-slot bounded queue kept
   * full by a task sleeping much longer than that, this must time out at
   * around 500ms, not fail immediately with some platform-specific error
   * arising from handing an out-of-range tv_nsec to the underlying pthread
   * call, and not hang. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* fills the 1-slot queue */

  struct timespec bad_timeout = {.tv_sec = 1, .tv_nsec = -500000000L};
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval_t r =
      ctpool_timed_submit(pool, inc_counter, &counter, NULL, &bad_timeout);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_EQ(r, ccol_timed_out);
  /* Comfortably wide bounds around the intended ~500ms: must not return
   * near-instantly (proving the malformed value was actually normalised
   * and used to compute a real, future deadline, not merely rejected or
   * swallowed as an immediate error) and must not take anywhere near the
   * full, un-normalised 1 second either. */
  REQUIRE_GT(elapsed_ms, 200L);
  REQUIRE_LT(elapsed_ms, 900L);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         FUTURES                                            */
/* ========================================================================== */

TEST(futures, get_result) {
  ctpool_construct(pool, 2, 0);

  int value = 42;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value);
  REQUIRE_NE((void *)f, NULL);

  void *result = ctpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);
  REQUIRE_FALSE(ctpool_future_cancelled(f));
  REQUIRE_TRUE(ctpool_future_done(f));

  ctpool_future_free(f);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, get_blocks_until_done) {
  ctpool_construct(pool, 1, 0);

  int value = 99;
  ctpool_future *f = ctpool_submit_future(pool, slow_identity, &value);
  REQUIRE_NE((void *)f, NULL);

  void *result = ctpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);

  ctpool_future_free(f);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, fan_out_fan_in) {
  enum { N = 8 };
  ctpool_construct(pool, 4, 0);

  int values[N];
  ctpool_future *futures[N];

  for (int i = 0; i < N; i++) {
    values[i] = i * 10;
    futures[i] = ctpool_submit_future(pool, identity_fn, &values[i]);
    REQUIRE_NE((void *)futures[i], NULL);
  }

  for (int i = 0; i < N; i++) {
    int *res = (int *)ctpool_future_get(futures[i]);
    REQUIRE_EQ(*res, values[i]);
    ctpool_future_free(futures[i]);
  }

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, free_before_get_no_leak) {
  /* Drop the caller's reference before the future is done.  The task still
   * executes and the future is freed by the worker. Verified by valgrind. */
  ctpool_construct(pool, 1, 0);

  int value = 7;
  ctpool_future *f = ctpool_submit_future(pool, slow_identity, &value);
  REQUIRE_NE((void *)f, NULL);

  ctpool_future_free(f); /* caller drops reference; no future_get call */

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, try_submit_future_succeeds) {
  int value = 42;
  ctpool_construct(pool, 2, 16);

  ctpool_future *f = NULL;
  ccol_retval_t r = ctpool_try_submit_future(pool, identity_fn, &value, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = ctpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, try_submit_future_container_full) {
  /* Single worker, 1-slot queue.  Block the worker so the queue fills, then
   * try_submit_future must return ccol_container_full without blocking. */
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f1, NULL); /* fills the 1-slot queue */

  ctpool_future *f2 = NULL;
  ccol_retval_t r = ctpool_try_submit_future(pool, identity_fn, &dummy, &f2);
  REQUIRE_EQ(r, ccol_container_full);
  REQUIRE_EQ((void *)f2, NULL);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_succeeds) {
  int value = 99;
  ctpool_construct(pool, 2, 16);

  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  ctpool_future *f = NULL;
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &value, &ts, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = ctpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_times_out) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f1, NULL); /* fills the 1-slot queue */

  ctpool_future *f2 = NULL;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 50 * 1000000L}; /* 50 ms */
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &dummy, &ts, &f2);
  REQUIRE_EQ(r, ccol_timed_out);
  REQUIRE_EQ((void *)f2, NULL);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_malformed_negative_tv_nsec_normalized) {
  /* Same make_abs_deadline fix as timed_submit's own
   * malformed_negative_tv_nsec_normalized_correctly test, exercised through
   * ctpool_timed_submit_future specifically, since it computes its own
   * absolute deadline through the identical shared helper. */
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f1, NULL); /* fills the 1-slot queue */

  ctpool_future *f2 = NULL;
  struct timespec bad_timeout = {.tv_sec = 1, .tv_nsec = -500000000L};
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &dummy, &bad_timeout, &f2);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_EQ(r, ccol_timed_out);
  REQUIRE_EQ((void *)f2, NULL);
  REQUIRE_GT(elapsed_ms, 200L);
  REQUIRE_LT(elapsed_ms, 900L);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_null_timeout_acts_as_try) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f1, NULL); /* fills the 1-slot queue */

  ctpool_future *f2 = NULL;
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &dummy, NULL, &f2);
  REQUIRE_EQ(r, ccol_container_full);
  REQUIRE_EQ((void *)f2, NULL);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_null_timeout_with_invalid_args) {
  /* Same coverage gap as timed_submit's identical companion test, for the
   * future variant's own NULL-timeout delegation path. */
  ctpool_construct(pool, 2, 0);
  int value = 0;

  ctpool_future *f = (ctpool_future *)0x1;
  REQUIRE_EQ(
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, NULL, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = (ctpool_future *)0x1;
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, NULL, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_unbounded_never_blocks_on_capacity) {
  /* Unbounded queue: timed_submit_future must always succeed even with a
   * near-zero timeout because no capacity check is needed. */
  ctpool_construct(pool, 2, 0);
  int value = 7;

  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1}; /* 1 ns */
  ctpool_future *f = NULL;
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &value, &ts, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = ctpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, done_is_false_before_completion) {
  /* ctpool_future_done must return false before the task finishes.
   * Use a gate to guarantee the task cannot complete before we check,
   * making the assertion deterministic rather than timing-dependent. */
  atomic_int gate = 0;
  int value = 0;
  gated_future_ctx_t ctx = {.gate = &gate, .result = &value};

  ctpool_construct(pool, 1, 0);
  ctpool_future *f = ctpool_submit_future(pool, gated_identity, &ctx);
  REQUIRE_NE((void *)f, NULL);

  /* Task is pinned behind the gate: done cannot be true yet. */
  REQUIRE_FALSE(ctpool_future_done(f));

  atomic_store(&gate, 1); /* release the task */
  void *res = ctpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  REQUIRE_TRUE(ctpool_future_done(f));

  ctpool_future_free(f);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, invalid_args) {
  ctpool_construct(pool, 2, 16);
  int value = 0;
  ctpool_future *f = NULL;
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};

  /* ctpool_try_submit_future: NULL pool, NULL fn, NULL out */
  REQUIRE_EQ(ctpool_try_submit_future(CTPOOL_INVALID, identity_fn, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_try_submit_future(pool, NULL, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_try_submit_future(pool, identity_fn, &value, NULL),
             ccol_invalid_args);

  /* ctpool_timed_submit_future: NULL pool, NULL fn, NULL out */
  REQUIRE_EQ(
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, &ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, &ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, identity_fn, &value, &ts, NULL),
             ccol_invalid_args);

  /* ctpool_submit_future: NULL pool, NULL fn */
  REQUIRE_EQ((void *)ctpool_submit_future(CTPOOL_INVALID, identity_fn, &value),
             NULL);
  REQUIRE_EQ((void *)ctpool_submit_future(pool, NULL, &value), NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* Regression coverage for a real contract violation: ctpool_try_submit_future
 * and ctpool_timed_submit_future both document "out ... set to NULL on
 * failure", unconditionally, but used to leave *out completely untouched on
 * their two earliest failure paths (an invalid/stale pool handle, or a NULL
 * fn); only their later failure paths (a full queue, OOM, ...) actually
 * zeroed it first. futures.invalid_args above cannot catch this: it always
 * pre-initialises f to NULL before every call, so a garbage-*out regression
 * would still read back as NULL by coincidence. This test instead poisons
 * *out with a distinctive non-NULL sentinel immediately before each call
 * that must fail on one of those two earliest paths, so the failure would be
 * visible even though the return code alone is unaffected either way. */
TEST(futures, out_param_zeroed_even_on_earliest_failure_paths) {
  ctpool_construct(pool, 2, 16);
  int value = 0;
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  ctpool_future *const poison = (ctpool_future *)(uintptr_t)0xdeadbeefu;

  ctpool_future *f = poison;
  REQUIRE_EQ(ctpool_try_submit_future(CTPOOL_INVALID, identity_fn, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(ctpool_try_submit_future(pool, NULL, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, &ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, &ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, submit_future_returns_null_on_oom) {
  /* With a custom OOM allocator, task allocation fails inside
   * alloc_future_task; the partially-constructed future must be cleaned up
   * and the function must return NULL / ccol_not_enough_memory.
   *
   * Note: the ctpool_future struct itself is allocated with plain calloc (not
   * the pool's custom allocator) so that its lifetime is fully independent of
   * the pool.  Failure of that allocation is therefore not exercised here; it
   * would require intercepting the system allocator (e.g. LD_PRELOAD), which
   * is outside the scope of these unit tests. */
  ccol_memmgmt_procs_t mp = {.malloc = oom_malloc,
                             .free = oom_free,
                             .calloc = oom_calloc,
                             .realloc = oom_realloc};
  char *err = NULL;
  ctpool pool = create_cthread_pool_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  int value = 0;

  /* ctpool_submit_future returns NULL on OOM. */
  atomic_store(&g_oom_enabled, 1);
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value);
  REQUIRE_EQ((void *)f, NULL);

  /* ctpool_try_submit_future returns ccol_not_enough_memory on OOM. */
  ctpool_future *f2 = NULL;
  ccol_retval_t r = ctpool_try_submit_future(pool, identity_fn, &value, &f2);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f2, NULL);
  atomic_store(&g_oom_enabled, 0);

  /* Pool must recover once memory is available. */
  f = ctpool_submit_future(pool, identity_fn, &value);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         DETACHED FUTURES                                   */
/* ========================================================================== */

/* pthread start routine that fulfills a detached future after a short delay,
 * used to prove ctpool_future_get() blocks until an external (non-worker)
 * producer thread calls ctpool_future_fulfill(). */
typedef struct {
  ctpool_future *f;
  void *result;
  int delay_ms;
} detached_producer_ctx_t;

static void *detached_producer_fn(void *arg) {
  detached_producer_ctx_t *ctx = (detached_producer_ctx_t *)arg;
  if (ctx->delay_ms > 0) sleep_ms(ctx->delay_ms);
  ctpool_future_fulfill(ctx->f, ctx->result);
  return NULL;
}

TEST(detached_futures, create_and_fulfill_no_pool) {
  /* No cthread_pool involved at all; proves the future is genuinely
   * standalone. */
  char *err = NULL;
  ctpool_future *f = ctpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_FALSE(ctpool_future_done(f));

  int value = 123;
  ccol_retval_t r = ctpool_future_fulfill(f, &value);
  REQUIRE_EQ(r, ccol_success);

  REQUIRE_TRUE(ctpool_future_done(f));
  REQUIRE_FALSE(ctpool_future_cancelled(f));
  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);

  ctpool_future_free(f);
}

TEST(detached_futures, get_blocks_until_external_fulfill) {
  char *err = NULL;
  ctpool_future *f = ctpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  int value = 55;
  detached_producer_ctx_t ctx = {.f = f, .result = &value, .delay_ms = 20};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, detached_producer_fn, &ctx), 0);

  /* Blocks until detached_producer_fn calls ctpool_future_fulfill. */
  void *result = ctpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);

  pthread_join(tid, NULL);
  ctpool_future_free(f);
}

TEST(detached_futures, free_before_fulfill_no_leak) {
  /* Caller drops its reference before the producer fulfills; the future must
   * stay alive (producer still holds a reference) and be freed once fulfill
   * runs. Verified by valgrind; no leak, no use-after-free. */
  char *err = NULL;
  ctpool_future *f = ctpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  ctpool_future_free(f); /* caller drops its reference first */

  int value = 7;
  ccol_retval_t r = ctpool_future_fulfill(f, &value); /* producer's turn */
  REQUIRE_EQ(r, ccol_success);
}

TEST(detached_futures, double_fulfill_returns_not_permitted) {
  char *err = NULL;
  ctpool_future *f = ctpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  int v1 = 1, v2 = 2;
  REQUIRE_EQ(ctpool_future_fulfill(f, &v1), ccol_success);
  REQUIRE_EQ(ctpool_future_fulfill(f, &v2), ccol_not_permitted);

  /* First result wins; second call must not have overwritten it. */
  REQUIRE_EQ(ctpool_future_get(f), (void *)&v1);

  ctpool_future_free(f);
}

TEST(detached_futures, fulfill_null_returns_invalid_args) {
  REQUIRE_EQ(ctpool_future_fulfill(NULL, NULL), ccol_invalid_args);
}

TEST(detached_futures, independent_of_any_pool) {
  /* Create/fulfill/free several detached futures with no ctpool ever
   * constructed in this test, alongside a real pool doing unrelated work, to
   * prove there is no hidden coupling between the two. */
  ctpool_construct(pool, 2, 0);
  atomic_int counter = 0;
  ctpool_submit(pool, inc_counter, &counter, NULL);

  char *err = NULL;
  ctpool_future *f = ctpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);
  int value = 9;
  REQUIRE_EQ(ctpool_future_fulfill(f, &value), ccol_success);
  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         CTPOOL_WAIT                                        */
/* ========================================================================== */

TEST(wait, phase_synchronisation) {
  atomic_int phase1_done = 0;
  atomic_int phase2_ok = 1;

  phase2_ctx_t p2ctx = {
      .p1_count = &phase1_done, .ok = &phase2_ok, .expected_p1 = 4};

  ctpool_construct(pool, 4, 0);

  for (int i = 0; i < 4; i++) {
    ctpool_submit(pool, phase1_fn, &phase1_done, NULL);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&phase1_done), 4);

  for (int i = 0; i < 4; i++) {
    ctpool_submit(pool, phase2_fn, &p2ctx, NULL);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&phase2_ok), 1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(wait, returns_when_idle_after_burst) {
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  for (int i = 0; i < 50; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 50);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

// ========================================================================
// INVARIANT TESTS (randomized operation sequences)
// ========================================================================

// Runs several bursts of a random number of tasks (each incrementing a
// shared atomic counter), draining with ctpool_wait after every burst and
// checking three things hold every time: ctpool_pending_count and
// ctpool_active_count are both back to zero, and the counter's cumulative
// total exactly matches the cumulative number of tasks submitted so far -
// i.e. every submitted task runs exactly once, no more, no less, and the
// pool always reports fully idle once drained.
TEST(cthreadpool, invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("cthreadpool.invariants_random_ops", seed);

  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  int total_submitted = 0;
  const int num_bursts = 30;
  for (int b = 0; b < num_bursts; ++b) {
    int burst_size = (int)ccol_invariants_next_bounded(&rng, 100) + 1;
    for (int i = 0; i < burst_size; ++i) {
      ccol_retval_t r = ctpool_submit(pool, inc_counter, &counter, NULL);
      REQUIRE_EQ(r, ccol_success);
    }
    total_submitted += burst_size;

    ctpool_wait(pool);

    REQUIRE_EQ(ctpool_pending_count(pool), (size_t)0);
    REQUIRE_EQ(ctpool_active_count(pool), (size_t)0);
    REQUIRE_EQ(atomic_load(&counter), total_submitted);
  }

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(wait, pool_accepts_work_after_wait) {
  /* ctpool_wait must not shut down the pool; workers must remain alive and
   * accept new tasks after it returns. */
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 0);

  for (int i = 0; i < 5; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 5);

  /* Submit a second batch and wait again. */
  for (int i = 0; i < 5; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 10);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(wait, returns_immediately_after_shutdown_immediate) {
  /* ctpool_wait on a pool that was immediately shut down (idle, no tasks)
   * must return without blocking. */
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_immediate(pool);
  ctpool_wait(pool);
  ctpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_immediate_unblocks_wait) {
  /* A thread blocked in ctpool_wait must be unblocked when another thread
   * calls ctpool_shutdown_immediate and the queue is discarded while
   * active_count is zero.
   *
   * Setup: one worker executes a gate-blocked task (active_count=1) while
   * five tasks queue up.  A waiter thread enters ctpool_wait.  Then the gate
   * is released so the worker exits blocker_fn; active_count drops to zero
   * for a brief window before the worker picks up the next queued task.  The
   * subsequent ctpool_shutdown_immediate observes this window (active_count==0,
   * queue non-empty), discards the queue, and must broadcast idle_cv so the
   * waiter returns.  Without the fix the waiter deadlocks. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started))
    sleep_ms(1); /* worker executing blocker_fn; active_count==1 */

  for (int i = 0; i < 5; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  pthread_t waiter;
  pthread_create(&waiter, NULL, pool_wait_thread, &pool);
  sleep_ms(10); /* let waiter enter cond_var_wait inside ctpool_wait */

  /* Release the gate, then poll until active_count drops to 0.  The moment
   * we observe 0, the worker has finished blocker_fn but has not yet
   * re-incremented active_count for the next queued task.  Calling
   * ctpool_shutdown_immediate immediately after maximises the chance that
   * it observes active_count==0 with a non-empty queue; the condition
   * that would deadlock ctpool_wait without the idle_cv broadcast fix. */
  atomic_store(&gate, 1);
  while (ctpool_active_count(pool) > 0) sleep_ms(1);
  ctpool_shutdown_immediate(pool);

  pthread_join(waiter, NULL); /* deadlocks without the idle_cv fix */
  ctpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_drain_unblocks_wait) {
  /* A thread blocked in ctpool_wait must be unblocked by the natural idle_cv
   * broadcast when the last task completes, even when ctpool_shutdown_drain is
   * called concurrently from another thread while tasks are still executing.
   * Unlike shutdown_immediate, the drain path has no explicit idle_cv signal;
   * it relies entirely on the broadcast emitted by the last completing task. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  for (int i = 0; i < 5; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  pthread_t waiter;
  pthread_create(&waiter, NULL, pool_wait_thread, &pool);
  sleep_ms(10); /* let waiter block inside ctpool_wait */

  /* A helper thread releases the gate after a delay so the worker exits
   * blocker_fn and drains the queued tasks while shutdown_drain is already
   * in progress. */
  pthread_t releaser;
  pthread_create(&releaser, NULL, release_gate_fn, &gate);

  ctpool_shutdown_drain(pool);
  pthread_join(releaser, NULL);
  pthread_join(waiter, NULL); /* must not deadlock */
  REQUIRE_EQ(atomic_load(&counter), 5);
  ctpool_destroy(pool);
}

TEST(wait, returns_immediately_when_pool_already_drained) {
  /* ctpool_wait called after ctpool_shutdown_drain has already joined all
   * workers must return immediately: active_count==0 and queue_size==0, so
   * the while condition is false on entry and no cond_var_wait is reached. */
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 0);

  for (int i = 0; i < 10; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  ctpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 10);

  ctpool_wait(pool); /* must not deadlock */

  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         SHUTDOWN DRAIN                                     */
/* ========================================================================== */

TEST(shutdown_drain, all_tasks_complete) {
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  for (int i = 0; i < 200; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  ctpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 200);
  ctpool_destroy(pool);
}

TEST(shutdown_drain, idempotent_second_call) {
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_drain(pool);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         SHUTDOWN IMMEDIATE                                 */
/* ========================================================================== */

TEST(shutdown_immediate, queued_tasks_discarded) {
  /* Block the single worker so tasks accumulate, then call
   * ctpool_shutdown_immediate while the gate is still closed.  The queue is
   * discarded atomically under the pool mutex before the worker can dequeue
   * anything.  A helper thread releases the gate after a brief delay so the
   * worker can exit and the join inside ctpool_shutdown_immediate completes.
   * All 10 queued tasks must be discarded: counter stays 0. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  for (int i = 0; i < 10; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  pthread_t releaser;
  pthread_create(&releaser, NULL, release_gate_fn, &gate);

  /* Queue is discarded here (worker is in blocker_fn, not dequeuing).
   * Internally blocks until the worker joins; the helper thread releases
   * the gate so the worker exits. */
  ctpool_shutdown_immediate(pool);
  pthread_join(releaser, NULL);

  REQUIRE_EQ(atomic_load(&counter), 0);
  ctpool_destroy(pool);
}

TEST(shutdown_immediate, futures_become_cancelled) {
  /* Block the single worker, queue two futures, then call
   * ctpool_shutdown_immediate while the gate is still closed (gate=0).
   * The queue is discarded atomically before the worker can dequeue
   * anything, so both futures must be cancelled.  A helper thread
   * releases the gate after a brief delay so the worker can exit. */
  atomic_int gate = 0;
  atomic_int started = 0;
  int dummy = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  ctpool_future *f1 = ctpool_submit_future(pool, slow_identity, &dummy);
  ctpool_future *f2 = ctpool_submit_future(pool, slow_identity, &dummy);
  REQUIRE_NE((void *)f1, NULL);
  REQUIRE_NE((void *)f2, NULL);

  pthread_t releaser;
  pthread_create(&releaser, NULL, release_gate_fn, &gate);

  ctpool_shutdown_immediate(pool); /* discards f1 and f2 while gate==0 */
  pthread_join(releaser, NULL);

  REQUIRE_TRUE(ctpool_future_cancelled(f1));
  REQUIRE_TRUE(ctpool_future_done(f1));
  REQUIRE_EQ(ctpool_future_get(f1), NULL);

  REQUIRE_TRUE(ctpool_future_cancelled(f2));
  REQUIRE_EQ(ctpool_future_get(f2), NULL);

  ctpool_future_free(f1);
  ctpool_future_free(f2);
  ctpool_destroy(pool);
}

TEST(shutdown_immediate, idempotent_second_call) {
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_immediate(pool);
  ctpool_shutdown_immediate(pool);
  ctpool_destroy(pool);
}

TEST(shutdown_immediate, on_complete_not_called_for_discarded) {
  /* Tasks cancelled by ctpool_shutdown_immediate must not have their
   * on_complete callback invoked.  noop_fn is the task function (arg is
   * ignored); inc_counter is the on_complete (would increment the counter
   * if called).  After immediate shutdown the counter must remain 0. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int on_complete_count = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  for (int i = 0; i < 5; i++) {
    ctpool_submit(pool, noop_fn, &on_complete_count, inc_counter);
  }

  pthread_t releaser;
  pthread_create(&releaser, NULL, release_gate_fn, &gate);
  ctpool_shutdown_immediate(pool); /* discards all 5 queued tasks */
  pthread_join(releaser, NULL);

  REQUIRE_EQ(atomic_load(&on_complete_count), 0);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         COUNTERS                                           */
/* ========================================================================== */

TEST(counters, pending_and_active) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);

  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  ctpool_submit(pool, inc_counter, &counter, NULL);

  REQUIRE_EQ(ctpool_active_count(pool), (size_t)1);
  REQUIRE_EQ(ctpool_pending_count(pool), (size_t)1);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);

  REQUIRE_EQ(ctpool_active_count(pool), (size_t)0);
  REQUIRE_EQ(ctpool_pending_count(pool), (size_t)0);

  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         CUSTOM MEMORY MANAGEMENT                          */
/* ========================================================================== */

TEST(custom_mprocs, allocations_go_through_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};

  char *err = NULL;
  ctpool pool = create_cthread_pool_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  atomic_int counter = 0;
  for (int i = 0; i < 8; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 8);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Regression/documentation-contract coverage: cthreadpool.h's own "Result
 * ownership" doc comment and alloc_future_task's own implementation comment
 * both document that a ctpool_future is always heap-allocated with plain
 * malloc/calloc, independent of the pool's custom allocator; only the
 * internal ctpool_task node it is submitted with goes through
 * pool->m_procs. No existing test actually distinguishes this from the
 * alternative design (the future ALSO going through the custom procs):
 * futures.submit_future_returns_null_on_oom's own comment explicitly notes
 * it cannot exercise the future's own allocation at all, since an
 * OOM-failing custom allocator makes *f == NULL either way, regardless of
 * which of the two designs is actually implemented. A COUNTING (rather than
 * failing) custom allocator closes this gap directly: one
 * ctpool_submit_future call must increase g_alloc_count by exactly 1 (the
 * task node only), not 2, which is what a regression routing the future
 * itself through the custom calloc would produce instead. */
TEST(custom_mprocs, future_struct_uses_plain_allocator_not_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = create_cthread_pool_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  int before_alloc = atomic_load(&g_alloc_count);
  int value = 42;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value);
  REQUIRE_NE((void *)f, NULL);
  int after_submit_alloc = atomic_load(&g_alloc_count);

  REQUIRE_EQ(after_submit_alloc - before_alloc, 1); /* task node only */

  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  /* Every tracked allocation this whole test made (pool construction, the
   * one task node) must have a matching tracked free; the future's own
   * alloc/free never appear in either count at all. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Direct verification of the task-node recycling optimization: submitting
 * tasks one at a time, waiting for each to fully complete (and therefore be
 * freed, i.e. recycled into the pool's own free list, since ctpool_wait
 * cannot observe active_count reach 0 until after the worker's own
 * task_free call has already returned) before submitting the next, must
 * NOT trigger a fresh custom-allocator call for any submission after the
 * very first one, which alone finds the free list empty. */
TEST(custom_mprocs, task_nodes_are_recycled_not_reallocated_each_time) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = create_cthread_pool_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  atomic_int counter = 0;
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  ctpool_wait(pool);
  int alloc_after_first = atomic_load(&g_alloc_count);
  REQUIRE_GT(alloc_after_first, 0); /* pool construction plus this one node */

  enum { N = 20 };
  for (int i = 0; i < N; i++) {
    REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
    ctpool_wait(pool);
  }
  REQUIRE_EQ(atomic_load(&counter), N + 1);

  /* No further tracked allocation should have happened: every one of the
   * N later submissions reused the exact node the previous submission's own
   * task_free recycled. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), alloc_after_first);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Direct verification that the free list is actually bounded by
 * task_free_list_cap, using the white-box accessors rather than only
 * inferring it indirectly through allocator call counts: a burst of tasks
 * queued up behind a gate-blocked single worker, released all at once so
 * the worker frees them back-to-back with no intervening submission to
 * reuse any of them, must never leave more than the configured cap sitting
 * in the free list. */
TEST(custom_mprocs, task_free_list_is_bounded_by_cap) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  size_t cap = _ctpool_task_free_list_cap_for_tests(raw);
  REQUIRE_EQ(cap, (size_t)4); /* num_threads(1) * 4, per the constructor */

  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  /* Queue comfortably more tasks than the cap while the sole worker is
   * still blocked on the gate, so none of them can be reused by a later
   * submission before the whole burst is freed. */
  enum { BURST = 3 * 4 /* 3x cap */ };
  atomic_int counter = 0;
  for (int i = 0; i < BURST; i++) {
    REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  }

  atomic_store(&gate, 1);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), BURST);

  REQUIRE_EQ(_ctpool_task_free_list_size_for_tests(raw), cap);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                         CONCURRENT LOAD                                   */
/* ========================================================================== */

TEST(load, many_tasks_many_threads) {
  enum { NTASKS = 1000 };
  atomic_int counter = 0;
  ctpool_construct(pool, 8, 0);

  for (int i = 0; i < NTASKS; i++) {
    ctpool_submit(pool, inc_counter, &counter, NULL);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), NTASKS);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(load, bounded_queue_backpressure) {
  /* 2 slow workers and a 16-slot queue; 64 tasks.  ctpool_submit must block
   * when the queue fills and all tasks must complete. */
  enum { NTASKS = 64 };
  atomic_int counter = 0;

  ctpool_construct(pool, 2, 16);

  for (int i = 0; i < NTASKS; i++) {
    ccol_retval_t r = ctpool_submit(pool, slow_inc, &counter, NULL);
    REQUIRE_EQ(r, ccol_success);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), NTASKS);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(load, concurrent_producers) {
  /* Multiple threads submit tasks concurrently.  All tasks must execute
   * exactly once, verifying that the queue and counter operations are
   * race-free under simultaneous submission from many producers. */
  enum { NPRODUCERS = 4, TASKS_PER = 250 };
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  producer_arg_t args[NPRODUCERS];
  pthread_t threads[NPRODUCERS];
  for (int i = 0; i < NPRODUCERS; i++) {
    args[i] =
        (producer_arg_t){.pool = pool, .counter = &counter, .n = TASKS_PER};
    pthread_create(&threads[i], NULL, producer_thread, &args[i]);
  }
  for (int i = 0; i < NPRODUCERS; i++) {
    pthread_join(threads[i], NULL);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), NPRODUCERS * TASKS_PER);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*          CTPOOL HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)            */
/* ========================================================================== */

/* Mirrors the already-implemented, already-verified chttpcli_handle_lifecycle
 * / event_loop_handle_lifecycle / clrucache_handle_lifecycle test groups,
 * adapted for ctpool's own lock-protected pin mechanism (see
 * src/cthreadpool.c's own struct cthread_pool.pending_resolve_count/pin_cv
 * field comments: like clru_cache, ctpool reuses the chttpcli/chttpsvr-style
 * lock-protected decrement+broadcast, since this module is already a
 * single-global-mutex design). */

/* A fully completed destroy, followed later by a second destroy call on an
 * independently-held copy of the same original handle value, must be a
 * fatal error. Run in a forked child (mirroring tests/clogger/tests.c's own
 * fork-test precedent for process-terminating misuse) since fatal_err
 * aborts the whole process. */
TEST(ctpool_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    ctpool pool = create_cthread_pool(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctpool stale = pool;     /* an independently-held copy of the handle value,
            distinct from the local the macro below invalidates */
    ctpool_destroy(pool);    /* completes normally (implicit drain shutdown);
           the local `pool` is now CTPOOL_INVALID, but `stale` still holds the
           original value */
    __ctpool_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  ctpool h;
} ctp_concurrent_destroy_arg_t;

static void *ctp_concurrent_destroy_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  __ctpool_destroy(a->h);
  return NULL;
}

/* Two threads calling destroy on two independently-held copies of the SAME,
 * still-valid handle at (as close to) the same moment as possible must also
 * be fatal; regression coverage for the same class of concurrent double-free
 * this whole redesign exists to close for chttpcli/chttpsvr/event_loop/
 * clru_cache. */
TEST(ctpool_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    ctpool pool = create_cthread_pool(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_concurrent_destroy_arg_t a1 = {.h = pool};
    ctp_concurrent_destroy_arg_t a2 = {.h = pool};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, ctp_concurrent_destroy_thread, &a1);
    pthread_create(&t2, NULL, ctp_concurrent_destroy_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  ctpool h;
  atomic_int *counter;
  ccol_retval_t rv;
} ctp_blocked_submit_arg_t;

static void *ctp_blocked_submit_thread(void *arg) {
  ctp_blocked_submit_arg_t *a = (ctp_blocked_submit_arg_t *)arg;
  a->rv = ctpool_submit(a->h, inc_counter, a->counter, NULL);
  return NULL;
}

/* The resolve-then-use race fix actually works, exercising the specific
 * reversed wait-ordering __ctpool_teardown_raw uses (shutdown-drain BEFORE
 * waiting on pending_resolve_count; see that function's own comment in
 * src/cthreadpool.c): a one-worker, queue_cap==1 pool with its worker stuck
 * in a long-running first task and a second task already filling the
 * queue, so a third ctpool_submit call genuinely blocks inside
 * submit_internal's cond_var_wait(not_full, ...) (holding a real,
 * resolved pin on the handle for the entire blocked duration). A concurrent
 * ctpool_destroy must (1) not crash / not free the pool out from under
 * that still-pinned blocked submitter (the actual UAF this whole redesign
 * exists to close: without the fix, submit_internal's blocked wait can
 * only ever be released by shutdown's own not_full broadcast, so waiting
 * on pending_resolve_count BEFORE running shutdown would deadlock
 * destroy forever instead), (2) let the blocked submit return
 * ccol_not_permitted once shutdown starts, and (3) still actually block
 * until the worker thread has been joined (a real wait, not an instant
 * return); proven by racing it against a release_gate_fn thread with a
 * known, fixed 20ms delay. */
TEST(ctpool_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  /* Fill the bounded queue (cap 1) so the next submit genuinely blocks. */
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);

  ctp_blocked_submit_arg_t blocked_arg = {
      .h = pool, .counter = &counter, .rv = ccol_success};
  pthread_t blocked_thread;
  REQUIRE_EQ(pthread_create(&blocked_thread, NULL, ctp_blocked_submit_thread,
                            &blocked_arg),
             0);
  /* Give the blocked-submit thread a head start so its resolve (and
   * therefore its pin) has definitely already happened before destroy
   * fires. */
  sleep_ms(10);

  pthread_t gate_thread;
  REQUIRE_EQ(pthread_create(&gate_thread, NULL, release_gate_fn, &gate), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ctpool_destroy(pool); /* must block until the worker (stuck until the gate
                            thread's ~20ms release) has actually exited */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(blocked_thread, NULL);
  pthread_join(gate_thread, NULL);

  REQUIRE_EQ(blocked_arg.rv, ccol_not_permitted);
  /* The worker was stuck for ~20ms (the gate thread's own fixed delay);
   * destroy returning in well under that would mean it did NOT actually
   * wait for the worker to be joined. */
  REQUIRE_GT(elapsed_ms, 10L);
}

typedef struct {
  ctpool h;
} ctp_pending_count_arg_t;

static void *ctp_pending_count_thread(void *arg) {
  ctp_pending_count_arg_t *a = (ctp_pending_count_arg_t *)arg;
  /* Return value intentionally ignored: a legitimate race with a concurrent
   * destroy can make this resolve fail (returning 0) instead of succeeding;
   * both outcomes are correct. This thread exists purely to generate
   * resolve/pin/unpin traffic concurrent with the destroy thread below. */
  ctpool_pending_count(a->h);
  return NULL;
}

/* Distinct from resolve_then_use_race_destroy_waits above, and not
 * redundant with it: that test's blocked submit guarantees
 * pending_resolve_count > 0 for a long, deterministic window; this test
 * needs the opposite shape: a fast, non-blocking entry point
 * (ctpool_pending_count: resolve, one mutex-protected field read, unpin,
 * return) raced against a concurrent destroy, repeated under stress, since
 * the failure window for a fast pin/unpin pair is only a handful of
 * instructions wide and will not reproduce reliably under a single
 * unstressed run. A fresh pool is used each iteration so every repetition
 * gets its own independent race rather than reusing one already-destroyed
 * handle. */
TEST(ctpool_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);

    ctp_pending_count_arg_t pending_arg = {.h = pool};
    ctp_concurrent_destroy_arg_t destroy_arg = {.h = pool};
    pthread_t pending_tid, destroy_tid;
    REQUIRE_EQ(pthread_create(&pending_tid, NULL, ctp_pending_count_thread,
                              &pending_arg),
               0);
    REQUIRE_EQ(pthread_create(&destroy_tid, NULL, ctp_concurrent_destroy_thread,
                              &destroy_arg),
               0);
    pthread_join(pending_tid, NULL);
    pthread_join(destroy_tid, NULL);
  }
}

static void *ctp_concurrent_shutdown_immediate_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  ctpool_shutdown_immediate(a->h);
  return NULL;
}

/* Regression coverage for a real data race _ctpool_teardown_raw used to
 * have: it peeked at pool->shutdown_started with no lock held at all
 * before deciding whether to run a drain shutdown, racing a concurrently
 * pinned, in-flight ctpool_shutdown_immediate/_drain call's own, properly
 * locked write to that same field (see src/cthreadpool.c's own
 * _ctpool_teardown_raw comment for the full account; fixed by always
 * calling the already-idempotent _ctpool_shutdown_drain_internal
 * unconditionally instead of peeking first). Races an explicit
 * ctpool_shutdown_immediate call on one thread against __ctpool_destroy on
 * another, on the same still-live handle, repeated under stress: must not
 * crash, hang, or corrupt state regardless of which thread reaches
 * pool->mu first. */
TEST(ctpool_handle_lifecycle,
     shutdown_immediate_races_concurrent_destroy_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);

    ctp_concurrent_destroy_arg_t shutdown_arg = {.h = pool};
    ctp_concurrent_destroy_arg_t destroy_arg = {.h = pool};
    pthread_t shutdown_tid, destroy_tid;
    REQUIRE_EQ(
        pthread_create(&shutdown_tid, NULL,
                       ctp_concurrent_shutdown_immediate_thread, &shutdown_arg),
        0);
    REQUIRE_EQ(pthread_create(&destroy_tid, NULL, ctp_concurrent_destroy_thread,
                              &destroy_arg),
               0);
    pthread_join(shutdown_tid, NULL);
    pthread_join(destroy_tid, NULL);
  }
}

/* Legitimate slot reuse must never be confused with a stale handle to the
 * slot's previous occupant; the whole point of the generation counter. */
TEST(ctpool_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  char *err = NULL;
  ctpool a = create_cthread_pool(2, 0, &err);
  REQUIRE_NE(a, CTPOOL_INVALID);
  ctpool stale_a = a;
  ctpool_destroy(a);

  ctpool b = create_cthread_pool(2, 0, &err);
  REQUIRE_NE(b, CTPOOL_INVALID);

  /* B's operations must succeed normally regardless of whether the
   * allocator happened to reuse A's exact address for B. */
  REQUIRE_EQ(ctpool_pending_count(b), (size_t)0);

  /* A's stale handle must never resolve to B, even if it reused the same
   * underlying address; the whole point of the generation counter. */
  REQUIRE_EQ((void *)_ctpool_resolve_for_tests(stale_a), NULL);

  ctpool_destroy(b);
}

/* The slot table is bounded, not ever-growing: a create/destroy churn loop
 * with only a single slot ever in flight at a time must reuse that one
 * freed slot on every iteration rather than growing the table further.
 * Captures capacity right after the first create/destroy pair (rather than
 * asserting a fixed absolute value like 1) since other tests earlier in
 * this same process may have already grown the table to some N > 1; what
 * this test actually needs to prove is that ITS OWN churn adds no further
 * growth, not what the table's absolute size happens to be when it runs. */
TEST(ctpool_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  char *err = NULL;
  ctpool pool0 = create_cthread_pool(2, 0, &err);
  REQUIRE_NE(pool0, CTPOOL_INVALID);
  ctpool_destroy(pool0);
  size_t capacity_after_first = _ctpool_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ctpool pool = create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);
    ctpool_destroy(pool);
  }

  REQUIRE_EQ(_ctpool_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*                    SELF-CALL FROM WITHIN A TASK                           */
/* ========================================================================== */

/* Regression coverage for a real, deterministic use-after-free: a task (or
 * its on_complete callback) calling ctpool_destroy on the very pool it is
 * executing on used to free the pool's mutex/condvars/struct while the
 * calling worker thread was still on its way back through worker_thread_fn
 * (task_free, then a lock/decrement/broadcast/unlock against the just-freed
 * object); pthread_join on the calling thread's own id returns
 * EDEADLK immediately instead of blocking, and that return value was never
 * checked, so the shutdown's own "join every worker before freeing anything"
 * guarantee silently did not apply to the calling worker itself. Now a fatal
 * error, mirroring how a stale/already-destroyed handle is already fatal.
 * Run in a forked child since fatal_err aborts the whole process. */
typedef struct {
  ctpool pool;
} ctp_self_destroy_ctx_t;

static void self_destroy_task(void *arg) {
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  ctpool_destroy(ctx->pool); /* the actual misuse under test */
  /* Unreachable if fatal_err() aborted as expected; if it somehow is
   * reached, worker_thread_fn's own post-task code (task_free, then
   * pool->mu-protected bookkeeping) would otherwise run against a freed
   * pool immediately after this function returns. */
}

TEST(ctpool_handle_lifecycle, destroy_from_within_own_task_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's own lifetime in case the fix somehow regressed
     * into a hang rather than a crash (see the ctpool_wait call below); the
     * parent below only ever checks WTERMSIG against SIGABRT, so a SIGALRM
     * termination here still fails the test cleanly rather than hanging the
     * whole suite. */
    alarm(2);

    char *err = NULL;
    ctpool pool = create_cthread_pool(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, self_destroy_task, &ctx, NULL);
    /* Deterministic rather than a fixed sleep for the expected (fix holds)
     * case: the worker aborts the whole process via SIGABRT well before
     * this could ever return, so it does not matter what this thread is
     * doing at that instant. Guarded by alarm() above, not relied upon
     * alone, in case a future regression corrupts pool in some way that
     * makes ctpool_wait itself misbehave rather than cleanly resolving it
     * as stale. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Same hazard, reached via on_complete instead of the task function itself:
 * on_complete runs on the same worker thread, before task_free, so it is
 * exactly as much "within" the pool's own worker as fn is. */
static void self_destroy_on_complete(void *arg) {
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  ctpool_destroy(ctx->pool);
}

TEST(ctpool_handle_lifecycle, destroy_from_within_own_on_complete_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* See destroy_from_within_own_task_is_fatal's identical alarm() comment
     * above. */
    alarm(2);

    char *err = NULL;
    ctpool pool = create_cthread_pool(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, noop_fn, &ctx, self_destroy_on_complete);
    /* See destroy_from_within_own_task_is_fatal's identical comment above:
     * deterministic either way, not a fixed-sleep guess, and bounded by
     * alarm() above regardless. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Regression coverage for the related, milder self-shutdown_drain hazard: a
 * task calling ctpool_shutdown_drain on its own pool must be a complete
 * no-op (not merely non-crashing) rather than actually setting
 * shutdown_drain/shutdown_started; setting those from within the self-call
 * would (a) reject this test's own subsequent, legitimate ctpool_submit call
 * with ccol_not_permitted, and (b) permanently prevent any LATER, real
 * external shutdown/destroy call from ever retrying the join this worker's
 * own self-join silently skipped (thread_join on one's own id returns
 * EDEADLK instead of blocking), leaking that worker thread's OS resources
 * for the remaining life of the process. This test would fail on point (a)
 * alone without the fix. */
typedef struct {
  ctpool pool;
  atomic_int *task_returned;
} ctp_self_shutdown_ctx_t;

static void self_shutdown_drain_task(void *arg) {
  ctp_self_shutdown_ctx_t *ctx = (ctp_self_shutdown_ctx_t *)arg;
  ctpool_shutdown_drain(ctx->pool); /* must be a no-op from within own task */
  atomic_store(ctx->task_returned, 1);
}

TEST(ctpool_handle_lifecycle, shutdown_drain_from_within_own_task_is_a_noop) {
  ctpool_construct(pool, 1, 0);
  atomic_int task_returned = 0;
  ctp_self_shutdown_ctx_t ctx = {.pool = pool, .task_returned = &task_returned};

  REQUIRE_EQ(ctpool_submit(pool, self_shutdown_drain_task, &ctx, NULL),
             ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&task_returned), 1);

  /* The pool must still be fully alive and accept new work: a self-call
   * must not have actually shut anything down. */
  atomic_int counter = 0;
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  /* A real, external shutdown/destroy afterward must still work cleanly,
   * proving no worker thread was left permanently un-joinable by the
   * earlier self-call. */
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* Same no-op requirement for ctpool_shutdown_immediate. */
static void self_shutdown_immediate_task(void *arg) {
  ctp_self_shutdown_ctx_t *ctx = (ctp_self_shutdown_ctx_t *)arg;
  ctpool_shutdown_immediate(
      ctx->pool); /* must be a no-op from within own task */
  atomic_store(ctx->task_returned, 1);
}

TEST(ctpool_handle_lifecycle,
     shutdown_immediate_from_within_own_task_is_a_noop) {
  ctpool_construct(pool, 1, 0);
  atomic_int task_returned = 0;
  ctp_self_shutdown_ctx_t ctx = {.pool = pool, .task_returned = &task_returned};

  REQUIRE_EQ(ctpool_submit(pool, self_shutdown_immediate_task, &ctx, NULL),
             ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&task_returned), 1);

  atomic_int counter = 0;
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* Regression coverage for the analogous ctpool_wait self-call hazard: a task
 * waiting on its own pool would otherwise deadlock forever, since the
 * calling task is itself still counted in active_count until it returns.
 * Run in a forked child, bounded by alarm(), so a regression here fails the
 * test rather than hanging the whole suite. */
typedef struct {
  ctpool pool;
  atomic_int *waited_ok;
} ctp_self_wait_ctx_t;

static void self_wait_task(void *arg) {
  ctp_self_wait_ctx_t *ctx = (ctp_self_wait_ctx_t *)arg;
  ctpool_wait(ctx->pool); /* must return immediately (no-op), not deadlock */
  atomic_store(ctx->waited_ok, 1);
}

TEST(ctpool_handle_lifecycle, wait_from_within_own_task_does_not_hang) {
  /* The child reports its own outcome through a pipe rather than through its
   * own process exit code: under make memtest, valgrind overrides a forked
   * child's real exit code with its own --error-exitcode the instant it
   * finds ANY "still reachable" allocation in that child's inherited process
   * image at exit time (which every child forked mid-suite always has,
   * since the rest of this suite has not quiesced yet), so the exit code
   * cannot reliably carry this result; see
   * fork_safety.destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future's
   * own identical reasoning a few tests below, and
   * tests/clogger/tests.c's own fork_safety group for the original,
   * independently-confirmed account of this exact valgrind behaviour. */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  pid_t pid = fork();
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2); /* bounds this child's own lifetime if the fix regresses */

    char *err = NULL;
    ctpool pool = create_cthread_pool(1, 0, &err);
    char ok = 0;
    if (pool != CTPOOL_INVALID) {
      atomic_int waited_ok = 0;
      ctp_self_wait_ctx_t ctx = {.pool = pool, .waited_ok = &waited_ok};
      ctpool_submit(pool, self_wait_task, &ctx, NULL);

      ctpool_wait(pool); /* from the main (non-worker) thread; must not hang */
      ok = (atomic_load(&waited_ok) == 1) ? 1 : 0;
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  REQUIRE_EQ(waited, pid);
  /* Not WEXITSTATUS (see this test's own comment above); WIFEXITED alone
   * still catches a real regression re-hanging (turns into WIFSIGNALED via
   * the alarm above) or a genuine crash. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* Regression coverage for a genuinely distinct interaction between the
 * self-call guard above and the fork-safety machinery elsewhere in this
 * file: a task that calls fork() itself (not the test's own top-level
 * fork()) leaves the child's sole surviving thread as an exact continuation
 * of that same worker call frame, with pool now marked foreign_since_fork
 * (since it was in_use at the instant of fork(), walked by
 * _ctpool_atfork_prepare/_release like every other live pool) but with this
 * thread's own ctpool_worker_key_bundle TLS value still pointing at pool
 * (inherited via fork(), which duplicates the calling thread's entire
 * state, TLS included). Calling ctpool_destroy(pool) from there must still
 * be detected as a self-call and rejected: __ctpool_destroy checks this
 * before ever dispatching to _ctpool_teardown_raw's foreign-vs-non-foreign
 * branches, so a real self-destroy is never masked by the pool's own
 * foreign status. Confirmed via a standalone reproduction outside this
 * suite before being added here: reverting just the self-call check (while
 * leaving the foreign-pool machinery untouched) turns this into a real
 * use-after-free in the child, since _ctpool_teardown_raw's foreign branch
 * frees pool's own struct while this exact thread is still on its way back
 * through worker_thread_fn's tail code. */
typedef struct {
  ctpool pool;
  _Atomic pid_t grandchild_pid; /* -1 until the task's own fork() returns in
                                    the parent side; reported back since a
                                    task has no return value of its own */
} ctp_fork_from_task_ctx_t;

static void fork_from_task_then_self_destroy(void *arg) {
  ctp_fork_from_task_ctx_t *ctx = (ctp_fork_from_task_ctx_t *)arg;
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2); /* bounds this grandchild's own lifetime if the fix regresses */
    ctpool_destroy(ctx->pool); /* self-destroy of a now-foreign pool, from
        within the exact worker call frame that was executing pre-fork */
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  /* Parent side: still the pool's own genuine worker thread, unaffected by
   * anything the child does to its own, independent post-fork copy of pool.
   */
  atomic_store(&ctx->grandchild_pid, pid);
}

TEST(ctpool_handle_lifecycle,
     destroy_from_within_own_task_is_fatal_even_after_forking_first) {
  ctpool_construct(pool, 1, 0);
  ctp_fork_from_task_ctx_t ctx = {.pool = pool, .grandchild_pid = -1};
  REQUIRE_EQ(ctpool_submit(pool, fork_from_task_then_self_destroy, &ctx, NULL),
             ccol_success);

  pid_t pid = -1;
  for (int i = 0; i < 500 && pid == -1; i++) {
    pid = atomic_load(&ctx.grandchild_pid);
    if (pid == -1) sleep_ms(2);
  }
  REQUIRE_NE(pid, -1);

  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);

  /* The parent's own copy of pool is completely unaffected by whatever the
   * grandchild did to its own, independent (post-fork COW) copy; the
   * original task itself (fork_from_task_then_self_destroy) has already
   * returned by this point in the parent, since fork() itself returns
   * immediately there. */
  ctpool_wait(pool);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                              FORK SAFETY                                   */
/* ========================================================================== */

/* The entire remainder of this file exercises the pthread_atfork()-based
 * fork() safety machinery in src/cthreadpool.c, which is itself compiled out
 * when FORK_SAFETY_REQUIRED is 0 (see that macro's own doc comment in
 * common.h); without that machinery these tests' own premises (a forked
 * child never inheriting a locked ctpool mutex, a pool surviving as
 * "foreign" rather than being torn down unsafely) no longer hold, so they
 * are compiled out along with it rather than left in to fail or hang. */
#if FORK_SAFETY_REQUIRED

typedef struct {
  _Atomic int stop;
} ctp_fork_stop_arg_t;

/* Continuously creates and destroys throwaway ctpool instances, completely
 * unrelated to the pool the main test thread keeps busy below; its only
 * purpose is to keep SOME thread inside ctpool_slot_table's own mutex (via
 * create_cthread_pool/ctpool_destroy) as often as possible, racing this
 * test's own repeated fork() calls. Mirrors
 * tests/cthreadcomm/tests.c's own fork_safety_churn_thread exactly, adapted
 * to ctpool instead of event_loop. */
static void *ctp_fork_churn_thread(void *arg) {
  ctp_fork_stop_arg_t *a = (ctp_fork_stop_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ctpool p = create_cthread_pool(1, 0, &err);
    if (p != CTPOOL_INVALID) ctpool_destroy(p);
  }
  return NULL;
}

typedef struct {
  ctpool pool;
  _Atomic int stop;
} ctp_fork_feeder_arg_t;

/* A single background thread repeatedly querying the busy pool the main
 * test thread forks against below, widening the pool->mu contention window
 * beyond what the trial loop's own submit-burst-then-fork technique alone
 * provides. sched_yield() after every call is load-bearing, not a nicety:
 * a bare `while (!stop) ctpool_pending_count(pool);` loop (tried first)
 * reproduces the pre-fix hang just as reliably natively, but iterates fast
 * enough to execute many millions of times even over a short test run,
 * and valgrind's memcheck does not give threads true multi-core
 * parallelism (it time-slices every thread through one single instrumented
 * execution engine instead), so that many iterations of anything, however
 * cheap each one is, still made this test catastrophically slow under
 * valgrind (confirmed directly: tens of seconds to multiple minutes for
 * this one test alone). Yielding after every call caps this thread's own
 * achievable call rate to whatever the scheduler's own time-slice
 * granularity allows, several orders of magnitude fewer calls for the same
 * wall-clock window, while still contending often enough in practice to
 * keep the race reproducible (see the test's own comment below for the
 * measured trade-off this lands on). */
static void *ctp_fork_feeder_thread(void *arg) {
  ctp_fork_feeder_arg_t *a = (ctp_fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    (void)ctpool_pending_count(a->pool);
    sched_yield();
  }
  return NULL;
}

/* Regression test for a real fork-safety hang this module's own
 * ctpool_slot_table.mutex and every live pool's own mu previously had no
 * protection against (see src/cthreadpool.c's own _ctpool_atfork_prepare
 * doc comment for the full mechanism): fork() duplicates only the calling
 * thread, so a pool's own mu (taken by ctpool_submit/_try_submit, by a
 * worker thread picking up or finishing a task, and by ctpool_destroy's own
 * shutdown/teardown sequence) could previously be inherited by a child
 * already locked, with no thread left alive in that child that could ever
 * unlock it. The same class of hazard already has a fix and regression test
 * for event_loop's own locks (see tests/cthreadcomm/tests.c's
 * fork_does_not_inherit_a_locked_event_loop_mutex); this module needed the
 * identical fix independently, since event_loop's own dispatch_pool
 * (created whenever a caller configures num_reactor_threads > 1) is exactly
 * such a ctpool, reachable through this exact mechanism, and event_loop has
 * no way to reach into this module's own opaque internals to protect it
 * from outside.
 *
 * A churn thread continuously creating/destroying throwaway ctpool
 * instances races ctpool_slot_table.mutex in the background, mirroring
 * fork_does_not_inherit_a_locked_event_loop_mutex's own identically-shaped
 * churn thread exactly (confirmed cheap under valgrind on its own: that
 * existing test, doing the same real create/destroy work at the same
 * duration, runs in under a second there). A single yield-throttled feeder
 * thread (see ctp_fork_feeder_thread's own comment) separately races the
 * busy pool's own mu in the background. Each trial then additionally
 * bursts several real task submissions to that same pool immediately
 * before forking, deliberately timing the fork() to land while some
 * worker thread may still be mid-dequeue (racing to acquire pool->mu after
 * one of the burst's own wakeup signals): several submissions, not one,
 * because a single submit's own pool->mu window proved too narrow to
 * reliably overlap a fork() called immediately afterward by itself
 * (confirmed directly: one-submit-per-trial with no feeder thread at all
 * reproduced zero hangs across 300 trials).
 *
 * Two earlier drafts of this test used a differently-shaped feeder thread
 * (or several of them) and no burst at all: first one or more threads
 * calling real, allocating ctpool_try_submit continuously; then, after
 * that proved even worse, one or more threads calling allocation-free
 * ctpool_pending_count/_active_count continuously, with no sched_yield()
 * between iterations. Every one of those reliably reproduced the pre-fix
 * hang in well under a second natively, but each turned catastrophically
 * slow under valgrind (tens of seconds to multiple minutes for this one
 * test alone, even with the real fix in place and zero hangs to report).
 * Root cause, confirmed by direct experimentation rather than assumed:
 * valgrind's memcheck does not give threads true multi-core parallelism at
 * all, it time-slices all of them through its own single instrumented
 * execution engine, so an unthrottled loop iterating as fast as the CPU
 * allows for the test's own full duration does not divide that work across
 * cores the way it would natively, it just hands valgrind an astronomically
 * larger total instrumented instruction count to simulate for the exact
 * same wall-clock window. The current design bounds that cost two ways:
 * the one feeder thread yields after every single lock/unlock, capping its
 * own achievable call rate to whatever the scheduler's own time-slice
 * granularity allows rather than the CPU's raw instruction rate; and the
 * trial loop's own burst is a small, fixed-size, non-spinning sequence of
 * blocking calls per trial, so that half of the contention scales with
 * TRIALS, not with wall-clock duration, and pays no such multiplier
 * either. */
TEST(fork_safety, fork_does_not_inherit_a_locked_ctpool_mutex) {
  ctp_fork_stop_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  REQUIRE_EQ(pthread_create(&churn_tid, NULL, ctp_fork_churn_thread, &churn),
             0);

  char *err = NULL;
  ctpool pool = create_cthread_pool(3, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  atomic_int counter = 0;

  ctp_fork_feeder_arg_t feeder = {.pool = pool, .stop = 0};
  pthread_t feeder_tid;
  REQUIRE_EQ(pthread_create(&feeder_tid, NULL, ctp_fork_feeder_thread, &feeder),
             0);

  /* 60 trials, not hundreds: this configuration's own empirically-measured
   * pre-fix hang rate (roughly 10-20% of trials, confirmed directly by
   * temporarily neutering _ctpool_atfork_prepare/_release's bodies while
   * keeping them registered, then rerunning this exact test dozens of
   * times) already makes the odds of a full run seeing zero hangs purely by
   * chance well under 1% without paying for hundreds of trials' worth of
   * fork()+waitpid() overhead on every ordinary (post-fix, zero-hang) test
   * run. */
  enum { TRIALS = 60 };
  /* Several submissions right before each fork(), not just one: a single
   * submit's own pool->mu window is too narrow to reliably overlap a fork()
   * called immediately afterward (confirmed directly: one-submit-per-trial
   * alone reproduced zero hangs across 300 trials); bursting several gives
   * several independent workers a near-simultaneous dequeue race to win,
   * multiplying the odds at least one is still inside pool->mu at the exact
   * fork() instant, without needing a perpetually-spinning background
   * thread (tried first; see this test's own comment above for why that
   * made it too slow under valgrind). */
  enum { BURST = 8 };
  int hangs = 0;
  for (int i = 0; i < TRIALS; i++) {
    for (int b = 0; b < BURST; b++) {
      REQUIRE_EQ(ctpool_try_submit(pool, inc_counter, &counter, NULL),
                 ccol_success);
    }

    pid_t pid = fork();
    REQUIRE_NE(pid, -1);
    if (pid == 0) {
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      /* Bounds this child's own lifetime in case the hazard this test
       * guards against somehow still fires, rather than hanging the whole
       * suite; the parent below distinguishes this from a clean exit via
       * WIFEXITED. */
      alarm(1);

      /* The exact call shape (ctpool_submit/_try_submit -> submit_internal
       * -> mutex_lock(pool->mu)) a real application would use right after
       * inheriting a pool across a fork, so it is what this test should
       * actually prove is safe. */
      atomic_int local_counter = 0;
      ctpool_try_submit(pool, inc_counter, &local_counter, NULL);
      _exit(0); /* reached only if the call above returned at all */
    }

    int status = 0;
    REQUIRE_EQ(waitpid(pid, &status, 0), pid);
    if (!WIFEXITED(status)) hangs++;
  }

  REQUIRE_EQ(hangs, 0);

  atomic_store(&feeder.stop, 1);
  pthread_join(feeder_tid, NULL);
  atomic_store(&churn.stop, 1);
  pthread_join(churn_tid, NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* Regression coverage for a real bug in _ctpool_teardown_raw's own
 * foreign_since_fork branch (see src/cthreadpool.c's own comment there for
 * the full account): it used to free pool->threads and the pool struct
 * itself without ever touching pool->head/pool->tail, silently leaking
 * every still-queued ctpool_task; and, for a queued FUTURE task
 * specifically, never calling future_cancel on it, so a caller in the
 * child still holding that future's pointer and calling
 * ctpool_future_get() on it would block forever waiting for a worker that
 * will never exist in this process. Fixed by discarding the queue (and
 * cancelling any attached futures) exactly like ctpool_shutdown_immediate
 * already does for a live pool, before freeing anything. This is also the
 * exact call shape __ctpool_destroy's own doc comment promises to support
 * ("If neither ctpool_shutdown_drain nor ctpool_shutdown_immediate was
 * called beforehand, a drain shutdown runs first"): destroying a foreign
 * pool directly, with no explicit prior shutdown call. */
TEST(
    fork_safety,
    destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = create_cthread_pool(1, 1, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f, NULL); /* queued behind the blocked worker */

  /* The child reports its own outcome through a pipe rather than through
   * its own process exit code: under make memtest, valgrind overrides a
   * forked child's real exit code with its own --error-exitcode the
   * instant it finds ANY "still reachable" allocation in that child's
   * inherited process image at exit time (which every child forked
   * mid-suite always has, since the rest of this suite has not quiesced
   * yet), so the exit code cannot reliably carry this result; see
   * tests/clogger/tests.c's own fork_safety group for the identical,
   * already-established reasoning, and this file's own
   * fork_does_not_inherit_a_locked_ctpool_mutex test above, which checks
   * only WIFEXITED for the same reason. */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's own lifetime in case the hazard this test guards
     * against somehow still fires, rather than hanging the whole suite. */
    alarm(2);

    ctpool_destroy(pool); /* no explicit shutdown_immediate call first */

    void *res = ctpool_future_get(f); /* must return promptly (cancelled),
                                          not hang forever */
    char ok = (ctpool_future_cancelled(f) && res == NULL) ? 1 : 0;
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  /* Release the parent's own worker and tear down its own, still-live pool
   * before any assertion below, so a genuine regression here does not also
   * leak a permanently-blocked worker thread into the rest of the suite;
   * its own copy of pool/f is completely unaffected by whatever the child
   * did to its own, independent (post-fork COW) copy. */
  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f);
  ctpool_future_free(f);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(res, (void *)&dummy);
}

/* Regression coverage for a real bug in ctpool_wait: unlike
 * _ctpool_shutdown_drain_internal/_ctpool_shutdown_immediate_internal (both
 * of which skip trying to join a foreign pool's own, nonexistent worker
 * threads), ctpool_wait had no foreign_since_fork guard at all, so calling
 * it in a forked child on an inherited pool whose active_count/queue_size
 * was nonzero at the instant of fork() hung forever: no worker thread
 * exists in this process to ever decrement active_count, drain queue_size,
 * or broadcast idle_cv again. Fixed by treating a foreign pool as vacuously
 * idle. */
TEST(fork_safety, wait_on_foreign_pool_with_pending_work_does_not_hang) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = create_cthread_pool(1, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* queues behind blocker */

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2); /* bounds this child's own lifetime if the fix regresses */
    ctpool_wait(pool); /* must return promptly, not hang forever */
    _exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  /* Release the parent's own worker and tear down its own, still-live pool
   * before the assertions below, so a genuine regression here does not
   * also leak a permanently-blocked worker thread into the rest of the
   * suite; its own worker is still genuinely blocked, unaffected by the
   * child. */
  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  int final_counter = atomic_load(&counter);
  ctpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  /* Not WEXITSTATUS: under make memtest, valgrind overrides a forked
   * child's real exit code with its own --error-exitcode the instant it
   * finds any "still reachable" allocation in that child's inherited
   * process image at exit time (which every child forked mid-suite
   * always has), so WIFEXITED alone (still catches a real regression
   * re-hanging, which turns into WIFSIGNALED via the alarm above, or a
   * genuine crash) is the only part of the observable exit status this
   * test can rely on; see tests/clogger/tests.c's own fork_safety group
   * for the identical, already-established reasoning, and this file's own
   * fork_does_not_inherit_a_locked_ctpool_mutex test above, which checks
   * only WIFEXITED for the same reason. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(final_counter, 1);
}

#endif /* FORK_SAFETY_REQUIRED */
