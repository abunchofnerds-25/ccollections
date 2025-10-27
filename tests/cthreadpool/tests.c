#include <common.h>
#include <cthreadpool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

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
 * thread so the main thread can race ctpool_shutdown_immediate against it. */
static void *pool_wait_thread(void *arg) {
  ctpool_wait((ctpool)arg);
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
  REQUIRE_NE((void *)pool, NULL);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, bounded) {
  ctpool_construct(pool, 4, 64);
  REQUIRE_NE((void *)pool, NULL);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, ccol_invalid_size_means_unbounded) {
  char *err = NULL;
  ctpool pool = create_cthread_pool(2, ccol_invalid_size, &err);
  REQUIRE_NE((void *)pool, NULL);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, zero_threads_fails) {
  char *err = NULL;
  ctpool pool = create_cthread_pool(0, 0, &err);
  REQUIRE_EQ((void *)pool, NULL);
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
    REQUIRE_NE((void *)pool, NULL);
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
  REQUIRE_EQ(ctpool_submit(NULL, inc_counter, NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(ctpool_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(ctpool_try_submit(NULL, inc_counter, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_try_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  struct timespec ts = {.tv_sec = 1, .tv_nsec = 0};
  REQUIRE_EQ(ctpool_timed_submit(NULL, inc_counter, NULL, NULL, &ts),
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
  REQUIRE_NE((void *)pool, NULL);

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
  REQUIRE_EQ(ctpool_try_submit_future(NULL, identity_fn, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_try_submit_future(pool, NULL, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_try_submit_future(pool, identity_fn, &value, NULL),
             ccol_invalid_args);

  /* ctpool_timed_submit_future: NULL pool, NULL fn, NULL out */
  REQUIRE_EQ(ctpool_timed_submit_future(NULL, identity_fn, &value, &ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, &ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, identity_fn, &value, &ts, NULL),
             ccol_invalid_args);

  /* ctpool_submit_future: NULL pool, NULL fn */
  REQUIRE_EQ((void *)ctpool_submit_future(NULL, identity_fn, &value), NULL);
  REQUIRE_EQ((void *)ctpool_submit_future(pool, NULL, &value), NULL);

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
  REQUIRE_NE((void *)pool, NULL);

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
  pthread_create(&waiter, NULL, pool_wait_thread, pool);
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
  pthread_create(&waiter, NULL, pool_wait_thread, pool);
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
  REQUIRE_NE((void *)pool, NULL);

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
