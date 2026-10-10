#include <common.h>
#include <common_invariants.h>
#include <cthreadpool.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <internal/cdebuglog.h>
#include <poll.h>
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
#include <test_sanitizer.h>
#pragma GCC diagnostic pop

TAU_MAIN()

extern struct cthread_pool *_ctpool_resolve_for_tests(ctpool h);
extern size_t _ctpool_slot_table_capacity_for_tests(void);
extern bool _ctpool_is_self_call_for_tests(struct cthread_pool *pool);
extern size_t _ctpool_free_index_count_for_tests(void);
extern atomic_bool _ctpool_skip_worker_mark_for_tests;
extern size_t _ctpool_task_free_list_size_for_tests(struct cthread_pool *pool);
extern size_t _ctpool_task_free_list_cap_for_tests(struct cthread_pool *pool);
extern size_t _ctpool_pending_resolve_count_for_tests(
    struct cthread_pool *pool);
extern int _ctpool_future_refcount_for_tests(ctpool_future *f);
extern void ctpool_test_wrlock_slot_table_for_tests(void);
extern void ctpool_test_wrunlock_slot_table_for_tests(void);
extern void _ctpool_set_task_free_list_window_for_tests(uint32_t idle_points);
extern int _ctpool_timedwait_not_full_for_tests(struct cthread_pool *pool,
                                                const struct timespec *abs);
extern void _ctpool_shutdown_state_for_tests(struct cthread_pool *pool,
                                             bool *started, bool *immediate,
                                             bool *complete);

/* ========================================================================== */
/*                    FORK-HANG DIAGNOSTIC CAPTURE                            */
/* ========================================================================== */

/* Every function in the diagnostic block below has its only callers inside
 * the "#if CCOL_FORK_SAFETY_REQUIRED" block further down in this file. A
 * build with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles that block out, so
 * these helpers would have no caller left, and because this file builds with
 * -Werror, -Wunused-function would turn that into a hard build failure for a
 * configuration that the library documents as supported. The guard keeps the
 * helpers and their callers on the same switch. It is not a suppression: a
 * helper that loses its LAST caller in the default build is an error.
 */
#if CCOL_FORK_SAFETY_REQUIRED
/* This is the diagnostic aid for fork_does_not_inherit_a_locked_ctpool_mutex
 * below. The alarm(5) bound of that test can fire in a forked child, where
 * the default disposition for SIGALRM kills the child and reports only THAT
 * the child hung, never WHERE. A custom handler instead captures the raw call
 * stack of the child and writes it down a pipe to the parent as plain
 * addresses. It resolves no symbols, because a resolver can call malloc() and
 * dlopen() inside itself, which would go back into the exact kind of lock
 * that this diagnostic investigates, when that lock is the stuck one.
 *
 * The parent runs in an ordinary context (not in a signal handler, and not
 * hung), so it resolves the addresses with backtrace_symbols(), which is
 * safe there, and prints them. A real hang then shows the stuck call site of
 * the child directly in the output of this test; without this, the output
 * carries only the bare verdict "it did not return".
 *
 * _diag_warm_up_backtrace warms backtrace() up once, and the test below calls
 * it at the top, well before any fork(). The unwinder of glibc dlopen()s, and
 * therefore malloc()s, its own internal unwind-info machinery on its OWN
 * first call in the process, and a child can be stuck on the malloc lock that
 * this diagnostic tries to catch. If that setup ran for the first time from
 * inside a signal handler in such a child, the diagnostic would recreate the
 * same hang inside itself and report nothing. */
static volatile sig_atomic_t diag_write_fd = -1;

static void _diag_warm_up_backtrace(void) {
  void *dummy[4];
  int n = backtrace(dummy, 4);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) {
    backtrace_symbols_fd(dummy, n, devnull);
    close(devnull);
  }
}

/* This handler is async-signal-safe: backtrace() only reads stack frames that
 * it unwinds into a buffer of the caller, and allocates nothing, because the
 * warm-up above already forced its one-time lazy setup, and write() is
 * async-signal-safe by the POSIX definition. This handler resolves no
 * symbols, on purpose, for the reason that the comment at the top of this
 * section gives. */
static void _diag_alarm_handler(int sig) {
  (void)sig;
  int fd = (int)diag_write_fd;
  if (fd >= 0) {
    void *frames[32];
    int n = backtrace(frames, 32);
    ssize_t written = write(fd, frames, (size_t)n * sizeof(frames[0]));
    (void)written;
  }
  _exit(66); /* A distinct sentinel, which means that the diagnostic capture
                fired, not that a plain unhandled signal killed the child, so
                the parent can tell the two apart and knows that a backtrace
                waits in the pipe. */
}

/* This function arms the diagnostic SIGALRM handler and starts the alarm(5)
 * bound that this test runs under. Call it from the child, right after
 * fork(), and before the operation under test. write_fd is the write end of
 * the pipe that the handler of this child reports through, while the read
 * end belongs to the caller of this function, in the parent, after waitpid. */
static void _diag_arm(int write_fd) {
  diag_write_fd = write_fd;
  struct sigaction sa = {0};
  sa.sa_handler = _diag_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* No SA_RESTART: a syscall that this signal interrupts
                      must not retry on its own, because with a retry the
                      handler fires and the stuck call does not stop. */
  sigaction(SIGALRM, &sa, NULL);
  alarm(5);
}

/* This function runs in the parent and reads whatever raw addresses
 * _diag_alarm_handler wrote (a plain unhandled-signal kill, or a clean exit,
 * leaves the pipe empty, which is fine and gives got <= 0 below). It then
 * resolves the addresses with backtrace_symbols() and prints them, which is
 * safe here, because the parent is an ordinary process that is not stuck. */
static void _diag_report(int read_fd) {
  void *frames[32];
  ssize_t got = read(read_fd, frames, sizeof(frames));
  if (got <= 0) return;
  int n = (int)(got / (ssize_t)sizeof(frames[0]));
  if (n <= 0) return;
  char **syms = backtrace_symbols(frames, n);
  fprintf(stderr, "  [diagnostic] child was stuck at:\n");
  for (int i = 0; i < n; i++) {
    fprintf(stderr, "    %s\n", syms && syms[i] ? syms[i] : "???");
  }
  if (syms) free(syms);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

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
 * shutdown_immediate and queued_tasks_discarded use this helper, a pthread
 * start routine rather than a pool task, which releases a gate after a short
 * delay so that the blocked worker can exit once ctpool_shutdown_immediate on
 * the main thread discards the queue.
 */
static void *release_gate_fn(void *arg) {
  sleep_ms(20);
  atomic_store((atomic_int *)arg, 1);
  return NULL;
}

/* This function calls ctpool_wait on the pool in arg, blocking a background
 * thread so that the main thread can race ctpool_shutdown_immediate against
 * it. arg is a `ctpool *`, the address of the local handle variable of the
 * caller, not the handle value itself, because ctpool is a uint64_t value
 * handle, not a pointer, so a void* cannot carry it by value. */
static void *pool_wait_thread(void *arg) {
  ctpool *p = (ctpool *)arg;
  ctpool_wait(*p);
  return NULL;
}

/* This function submits n tasks to the pool, each of which increments
 * *counter; load/concurrent_producers uses it to exercise submission from
 * more than one thread. */
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

/* The future task that a gate blocks: it spins until something releases the
 * gate and then returns result, which guarantees that a future cannot
 * complete before an explicit release. */
typedef struct {
  atomic_int *gate;
  void *result;
} gated_future_ctx_t;

static void *gated_identity(void *arg) {
  gated_future_ctx_t *ctx = (gated_future_ctx_t *)arg;
  while (!atomic_load(ctx->gate)) sleep_ms(1);
  return ctx->result;
}

/*
 * The fixture for the multi-waiter future tests below. The task of the
 * future does not return until the test sets `release`, so every waiter
 * thread parks inside ctpool_future_get for as long as the test needs.
 */
typedef struct {
  ctpool_future *f;
  void *expected_result;
  atomic_int release;    /* lets the task function return */
  atomic_int results_ok; /* waiters that saw the right result */
} future_waiters_ctx_t;

static void *waiters_gated_fn(void *arg) {
  future_waiters_ctx_t *c = (future_waiters_ctx_t *)arg;
  while (!atomic_load(&c->release)) sleep_ms(1);
  return c->expected_result;
}

/* The waiter thread, which blocks in ctpool_future_get on the shared future
 * and then records whether it saw the expected result. */
static void *future_waiter_thread(void *arg) {
  future_waiters_ctx_t *c = (future_waiters_ctx_t *)arg;
  void *result = ctpool_future_get(c->f);
  if (result == c->expected_result) atomic_fetch_add(&c->results_ok, 1);
  return NULL;
}

/*
 * A bounded wait for the reference count of f to reach `target`, which
 * returns the last value that it read. The tests below need the references
 * of the waiter threads to exist before they drop every other reference.
 * Because a waiter takes its reference inside a call that then blocks, it
 * can announce nothing for itself from the outside, so asking the future
 * instead makes the tests independent of how long a thread takes to get
 * there.
 *
 * The bound is a safety net against a hang, never the mechanism: it lets a
 * build that does not take the reference reach its assertions and fail.
 * Without it, such a build drops the last reference under threads that are
 * still inside the future.
 */
static int wait_for_future_refcount(ctpool_future *f, int target, int max_ms) {
  int observed = _ctpool_future_refcount_for_tests(f);
  for (int waited = 0; observed != target && waited < max_ms; waited++) {
    sleep_ms(1);
    observed = _ctpool_future_refcount_for_tests(f);
  }
  return observed;
}

/* The no-op task, which ignores its argument, so that a test can check
 * on_complete on its own, apart from what the task function does to the same
 * arg. */
static void noop_fn(void *arg) { (void)arg; }

/*
 * The fixture for the self-submit tests below, where a task, or the
 * on_complete callback of that task, submits back into the pool that it runs
 * on.
 */
typedef struct {
  ctpool pool;
  atomic_int first_rc;
  atomic_int second_rc;
  ctpool_future *first_future;
  ctpool_future *second_future;
} self_submit_ctx_t;

static void self_submit_twice(self_submit_ctx_t *c) {
  atomic_store(&c->first_rc, (int)ctpool_submit(c->pool, noop_fn, NULL, NULL));
  atomic_store(&c->second_rc, (int)ctpool_submit(c->pool, noop_fn, NULL, NULL));
}

static void self_submit_task_fn(void *arg) {
  self_submit_twice((self_submit_ctx_t *)arg);
}

static void self_submit_complete_fn(void *arg, bool ran) {
  (void)ran;
  self_submit_twice((self_submit_ctx_t *)arg);
}

static void self_submit_future_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  c->first_future = ctpool_submit_future(c->pool, identity_fn, NULL);
  c->second_future = ctpool_submit_future(c->pool, identity_fn, NULL);
}

/* A bounded wait for an atomic flag to become non-zero, which returns what it
 * last read. Every wait that a test makes on a flag from another thread is
 * bounded, so a precondition that never becomes true fails an assertion
 * instead of hanging the whole binary. */
static int wait_for_flag(atomic_int *flag, int max_ms) {
  int value = atomic_load(flag);
  for (int waited = 0; !value && waited < max_ms; waited++) {
    sleep_ms(1);
    value = atomic_load(flag);
  }
  return value;
}

/* The fixture for the cross-pool control test, where a task on one pool makes
 * a blocking submission into a different pool that is full for a moment. */
typedef struct {
  ctpool other;
  atomic_int submitting;
  atomic_int rc;
} cross_pool_ctx_t;

static void cross_pool_task_fn(void *arg) {
  cross_pool_ctx_t *c = (cross_pool_ctx_t *)arg;
  atomic_store(&c->submitting, 1);
  atomic_store(&c->rc, (int)ctpool_submit(c->other, noop_fn, NULL, NULL));
}

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

/* This runs on a worker, so it records what it saw instead of asserting: a
 * Tau assertion from a thread other than the test thread is a data race. 1
 * means that fn had finished and ran was true; 2 means anything else. */
static void cb_cb(void *arg, bool ran) {
  bool ok = ran && atomic_load(((cb_ctx_t *)arg)->fn_done) == 1;
  atomic_store(((cb_ctx_t *)arg)->cb_done, ok ? 1 : 2);
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
 * While g_oom_enabled is non-zero, every allocation through the custom procs
 * of the pool returns NULL, which exercises the ccol_not_enough_memory paths.
 * Turn it on AFTER the pool exists, so that the pool itself gets real memory.
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

/* ccol_invalid_size is the error value of a size (a caller that wants an
 * unbounded queue passes 0), so the constructor refuses it before it
 * allocates anything. */
TEST(construction, ccol_invalid_size_is_refused) {
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(2, ccol_invalid_size, &err);
  bool refused = pool == CTPOOL_INVALID;
  if (!refused) ctpool_destroy(pool);
  REQUIRE_TRUE(refused);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "ccol_invalid_size"), NULL);
}

TEST(construction, ccol_invalid_size_is_refused_before_any_allocation) {
  atomic_store(&g_alloc_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc,
                             .free = tracked_free};
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(2, ccol_invalid_size, &mp, &err);
  bool refused = pool == CTPOOL_INVALID;
  if (!refused) ctpool_destroy(pool);
  REQUIRE_TRUE(refused);
  REQUIRE_EQ(atomic_load(&g_alloc_count), 0);
}

/* The largest capacity below ccol_invalid_size is a valid bounded queue. */
TEST(construction, largest_finite_capacity_is_bounded) {
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(1, ccol_invalid_size - 1, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, zero_threads_fails) {
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(0, 0, &err);
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
   * the enclosing block exits, which the count of completed tasks and
   * valgrind (for leaks) verify. */
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
    pool = ccol_create_cthread_pool(2, 0, &err);
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
  uint64_t ts = 1000000;
  REQUIRE_EQ(ctpool_timed_submit(CTPOOL_INVALID, inc_counter, NULL, NULL, ts),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_timed_submit(pool, NULL, NULL, NULL, ts),
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
  ctpool pool = ccol_create_cthread_pool_mp(2, 0, &mp, &err);
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

TEST(submit, blocking_submit_from_within_a_task_reports_container_full) {
  /*
   * A blocking submit parks on the not-full condition of the queue until a
   * worker dequeues. When the caller is itself a worker of that pool, it is
   * one of the few threads that could make that dequeue, and a wait there
   * takes it out of the set of threads that could satisfy its own wait, so
   * the library must report a full queue to such a caller. Without that
   * report, this test never returns, because the one worker of the pool parks
   * for ever on a slot that only it could free.
   *
   * The pool has one worker and a queue capacity of one, so the sequence is
   * fixed: the first submit of the running task finds the queue empty and
   * succeeds, and the second submit finds the queue at capacity.
   */
  ctpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval_t submitted =
      ctpool_submit(pool, self_submit_task_fn, &ctx, NULL);
  ctpool_wait(pool);

  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

TEST(submit, a_worker_without_its_thread_mark_is_still_recognised) {
  /* pthread_setspecific can fail (glibc allocates the storage for a key
   * index of 32 or above lazily for each thread). A worker whose mark is
   * missing must still be recognised as a worker of its pool, or the
   * container-full report of the test above turns into a worker that parks
   * for ever on a slot that only it could free. The hook skips the mark for
   * the workers of this one pool only.
   *
   * This test is non-vacuous: without the fallback over the worker IDs, the
   * second submit never returns, second_rc stays -1 past the bound below, and
   * the test fails (the stuck worker then also keeps the pool from
   * draining). */
  atomic_store(&_ctpool_skip_worker_mark_for_tests, true);
  ctpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval_t submitted =
      ctpool_submit(pool, self_submit_task_fn, &ctx, NULL);

  /* Bounded poll; the bound only guards against a hang. */
  for (int i = 0; i < 1000 && atomic_load(&ctx.second_rc) == -1; i++) {
    struct timespec ts = {0, 10 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  /* The worker read the hook when it started, before it ran the task, so the
   * hook can be cleared now, and it must not stay set for the pools of later
   * tests. */
  atomic_store(&_ctpool_skip_worker_mark_for_tests, false);
  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);
  REQUIRE_NE(second_rc, -1);

  ctpool_wait(pool);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

/* The thread ID of a worker that a shutdown already joined names a free
 * descriptor. glibc caches the stacks of joined threads and hands them to the
 * next threads that start, so a thread that the application starts after the
 * shutdown commonly carries exactly that ID. The self-call check must never
 * mistake such a thread for a worker of the pool: a destroy from it is an
 * ordinary destroy, and must not end the process as a destroy from inside a
 * task of the pool does.
 *
 * The child keeps starting threads until one carries the ID of the joined
 * worker, and only that thread calls ctpool_destroy. A child in which no
 * thread reuses the ID reports that, and the test fails rather than passing
 * without having examined the case. This test is non-vacuous: when the scan
 * over the worker IDs also looks at workers that have exited, the destroy
 * ends the child with SIGABRT, and no byte reaches the pipe. The child
 * reports through a pipe instead of its exit status, because valgrind
 * replaces the exit status of a forked child that holds reachable memory. */
typedef struct {
  pthread_t worker_id;
  ctpool pool;
  atomic_int outcome; /* 0: ID not reused, 1: reused and destroyed */
} ctp_reused_id_ctx_t;

static void ctp_record_worker_id(void *arg) {
  ((ctp_reused_id_ctx_t *)arg)->worker_id = pthread_self();
}

static void *ctp_destroy_if_reused_id(void *arg) {
  ctp_reused_id_ctx_t *ctx = (ctp_reused_id_ctx_t *)arg;
  if (pthread_equal(pthread_self(), ctx->worker_id)) {
    ctpool_destroy(ctx->pool);
    atomic_store(&ctx->outcome, 1);
  }
  return NULL;
}

TEST(submit, a_thread_that_reuses_a_joined_worker_id_is_not_a_worker) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(20);
    char verdict = 'F';
    char *err = NULL;
    ctp_reused_id_ctx_t ctx;
    atomic_init(&ctx.outcome, 0);
    ctx.pool = ccol_create_cthread_pool(1, 0, &err);
    if (ctx.pool != CTPOOL_INVALID &&
        ctpool_submit(ctx.pool, ctp_record_worker_id, &ctx, NULL) ==
            ccol_success) {
      ctpool_wait(ctx.pool);
      /* Joins the one worker, whose ID then names a free descriptor. */
      ctpool_shutdown_drain(ctx.pool);
      for (int i = 0; i < 64 && atomic_load(&ctx.outcome) == 0; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, ctp_destroy_if_reused_id, &ctx) != 0)
          break;
        pthread_join(t, NULL);
      }
      verdict = atomic_load(&ctx.outcome) == 1 ? 'R' : 'N';
      if (verdict == 'N') ctpool_destroy(ctx.pool);
    }
    ssize_t w = write(fds[1], &verdict, 1);
    (void)w;
    _exit(0);
  }
  close(fds[1]);
  REQUIRE_NE(pid, -1);
  char got = 0;
  ssize_t n = read(fds[0], &got, 1);
  close(fds[0]);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  /* 'N' means that no thread reused the ID, so the case went unexamined. */
  REQUIRE_EQ(got, 'R');
}

TEST(submit, blocking_submit_from_on_complete_reports_container_full) {
  /* An on_complete callback runs on the worker thread that ran its task, so
   * it meets the identical hazard as the task itself; see
   * submit.blocking_submit_from_within_a_task_reports_container_full. */
  ctpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval_t submitted =
      ctpool_submit(pool, noop_fn, &ctx, self_submit_complete_fn);
  ctpool_wait(pool);

  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

TEST(submit, blocking_submit_from_a_task_into_another_pool_still_waits) {
  /*
   * The full-queue report is keyed to the pool of the caller. A task that
   * submits into a DIFFERENT pool is not a worker of that pool, and nothing
   * about its wait defeats itself, so it must block until a slot frees, and
   * the library must not refuse it. The target pool is saturated at the
   * moment of the submission (its single worker is held on a gate, and its
   * one queue slot is taken), so only a release of that gate lets the
   * submission through.
   */
  ctpool_construct(target, 1, 1);
  ctpool_construct(source, 1, 0);

  atomic_int gate;
  atomic_int blocker_started;
  atomic_init(&gate, 0);
  atomic_init(&blocker_started, 0);
  gate_ctx_t blocker_ctx = {&gate, &blocker_started};

  ccol_retval_t held = ctpool_submit(target, blocker_fn, &blocker_ctx, NULL);
  int blocker_running = wait_for_flag(&blocker_started, 5000);
  ccol_retval_t queued = ctpool_submit(target, noop_fn, NULL, NULL);

  cross_pool_ctx_t ctx;
  ctx.other = target;
  atomic_init(&ctx.submitting, 0);
  atomic_init(&ctx.rc, -1);

  ccol_retval_t dispatched =
      ctpool_submit(source, cross_pool_task_fn, &ctx, NULL);
  int reached_submit = wait_for_flag(&ctx.submitting, 5000);
  /* Widens the window in which the submission is genuinely waiting when the
   * gate opens. Nothing here depends on it: releasing the gate early only
   * makes the submission find a slot sooner. */
  sleep_ms(5);

  atomic_store(&gate, 1);
  ctpool_wait(source);
  int cross_rc = atomic_load(&ctx.rc);

  ctpool_shutdown_drain(source);
  ctpool_destroy(source);
  ctpool_shutdown_drain(target);
  ctpool_destroy(target);

  REQUIRE_EQ(held, ccol_success);
  REQUIRE_EQ(blocker_running, 1);
  REQUIRE_EQ(queued, ccol_success);
  REQUIRE_EQ(dispatched, ccol_success);
  REQUIRE_EQ(reached_submit, 1);
  REQUIRE_EQ(cross_rc, (int)ccol_success);
}

TEST(try_submit, returns_container_full_when_bounded_queue_saturated) {
  /* Single worker, 1-slot queue: block the worker so that the queue fills
   * up, and try_submit must then return ccol_container_full without
   * blocking. */
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

  uint64_t ts = 50000; /* 50 ms */
  ccol_retval_t r = ctpool_timed_submit(pool, inc_counter, &counter, NULL, ts);
  REQUIRE_EQ(r, ccol_timed_out);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, succeeds_before_timeout) {
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 16);
  uint64_t ts = 1000000;
  ccol_retval_t r = ctpool_timed_submit(pool, inc_counter, &counter, NULL, ts);
  REQUIRE_EQ(r, ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, zero_timeout_acts_as_try_submit) {
  /* With a timeout of 0, ctpool_timed_submit must return ccol_container_full
   * immediately on a full bounded queue, not block, and not report
   * ccol_timed_out. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* fill the 1-slot queue */

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval_t r = ctpool_timed_submit(pool, inc_counter, &counter, NULL, 0);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  REQUIRE_EQ(r, ccol_container_full);
  REQUIRE_LT(elapsed_ms, 1000L);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, zero_timeout_with_invalid_args_still_reports_invalid_args) {
  /* A timeout of 0 delegates straight to ctpool_try_submit, and an invalid
   * pool and a NULL fn must both report ccol_invalid_args through that
   * delegation, exactly as they do for a direct call to
   * ctpool_try_submit. */
  ctpool_construct(pool, 2, 0);

  REQUIRE_EQ(ctpool_timed_submit(CTPOOL_INVALID, inc_counter, NULL, NULL, 0),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_timed_submit(pool, NULL, NULL, NULL, 0), ccol_invalid_args);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, unbounded_queue_never_blocks_on_capacity) {
  /* For an unbounded queue the timeout is irrelevant: ctpool_timed_submit must
   * always succeed immediately regardless of how short the timeout is. */
  atomic_int counter = 0;
  ctpool_construct(pool, 2, 0);

  uint64_t ts = 1; /* 1 us; effectively zero */
  for (int i = 0; i < 20; i++) {
    ccol_retval_t r =
        ctpool_timed_submit(pool, inc_counter, &counter, NULL, ts);
    REQUIRE_EQ(r, ccol_success);
  }

  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 20);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, a_small_timeout_waits_about_that_long) {
  /* A timeout of 500000 us is half a second. The queue here is bounded to
   * one slot, and a task that sleeps much longer than that keeps it full, so
   * the call must time out at about 500 ms: not at once, and not after a
   * much longer wait. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* fills the 1-slot queue */

  uint64_t bad_timeout = 500000;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval_t r =
      ctpool_timed_submit(pool, inc_counter, &counter, NULL, bad_timeout);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_EQ(r, ccol_timed_out);
  /* These bounds are wide around the intended 500 ms, for a loaded machine:
   * the call must not return almost at once, and it must not take several
   * times the timeout. */
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
  /* Drop the caller's reference before the future is done; the task
   * executes anyway, and the worker frees the future, which valgrind
   * verifies. */
  ctpool_construct(pool, 1, 0);

  int value = 7;
  ctpool_future *f = ctpool_submit_future(pool, slow_identity, &value);
  REQUIRE_NE((void *)f, NULL);

  ctpool_future_free(f); /* caller drops reference; no future_get call */

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, waiters_keep_the_future_alive_when_every_other_reference_goes) {
  /*
   * This test pins one hazard: a thread that blocks in ctpool_future_get
   * blocks inside the mutex and the condition variable of the future, and
   * nothing may destroy the future while that thread is there. This test
   * releases the caller reference first, and the reference of the task goes
   * with the completion of that task; between them, those two account for
   * every reference that a future has, apart from the references of the
   * waiters. Without one reference for each waiting ctpool_future_get call,
   * that completion destroys the mutex and frees the future under eight
   * threads that still use it.
   *
   * The test reads the reference count directly, so a failure is an
   * assertion and not a hang. A build that does not take the waiter reference
   * never reaches the count that the test waits for, and the test then holds
   * the caller reference until every waiter joins, so the run stays safe
   * enough to report what it found.
   */
  enum { WAITERS = 8 };
  int expected_result_storage = 0;
  ctpool_construct(pool, 2, 0);

  future_waiters_ctx_t ctx;
  ctx.f = NULL;
  ctx.expected_result = &expected_result_storage;
  atomic_init(&ctx.release, 0);
  atomic_init(&ctx.results_ok, 0);

  ctpool_future *f = ctpool_submit_future(pool, waiters_gated_fn, &ctx);
  ctx.f = f;

  pthread_t tids[WAITERS];
  int started = 0;
  if (f) {
    for (int i = 0; i < WAITERS; i++) {
      if (pthread_create(&tids[i], NULL, future_waiter_thread, &ctx) != 0)
        break;
      started++;
    }
  }

  /* caller reference + task reference + one per waiter */
  int expected_refcount = 2 + started;
  int observed_refcount =
      f ? wait_for_future_refcount(f, expected_refcount, 5000) : 0;
  bool waiters_hold_references = (f && observed_refcount == expected_refcount);

  /* Drop the caller reference while every waiter is still inside the future,
   * leaving the task's completion to release the last non-waiter reference. */
  if (waiters_hold_references) ctpool_future_free(f);
  atomic_store(&ctx.release, 1);

  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  int results_ok = atomic_load(&ctx.results_ok);
  if (f && !waiters_hold_references) ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(started, (int)WAITERS);
  REQUIRE_EQ(observed_refcount, expected_refcount);
  REQUIRE_EQ(results_ok, started);
}

TEST(futures, many_threads_get_the_same_future_while_the_owner_frees_it) {
  /*
   * The documented multi-waiter pattern from end to end: several threads
   * call ctpool_future_get on one future, something releases the single
   * caller reference exactly once in total, and every waiter sees the same
   * result. Because each waiter holds its own reference for the whole of its
   * call, the thread that leaves the future last is the one that destroys it.
   */
  enum { WAITERS = 16 };
  int expected_result_storage = 0;
  ctpool_construct(pool, 2, 0);

  future_waiters_ctx_t ctx;
  ctx.f = NULL;
  ctx.expected_result = &expected_result_storage;
  atomic_init(&ctx.release, 0);
  atomic_init(&ctx.results_ok, 0);

  ctpool_future *f = ctpool_submit_future(pool, waiters_gated_fn, &ctx);
  ctx.f = f;

  pthread_t tids[WAITERS];
  int started = 0;
  if (f) {
    for (int i = 0; i < WAITERS; i++) {
      if (pthread_create(&tids[i], NULL, future_waiter_thread, &ctx) != 0)
        break;
      started++;
    }
  }

  int expected_refcount = 2 + started;
  int observed_refcount =
      f ? wait_for_future_refcount(f, expected_refcount, 5000) : 0;
  bool waiters_hold_references = (f && observed_refcount == expected_refcount);

  atomic_store(&ctx.release, 1);
  void *owner_result = f ? ctpool_future_get(f) : NULL;
  /* The owner races the waiters out of the future; freeing here is safe
   * precisely because each of them still holds a reference of its own. */
  if (waiters_hold_references) ctpool_future_free(f);

  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  int results_ok = atomic_load(&ctx.results_ok);
  if (f && !waiters_hold_references) ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(started, (int)WAITERS);
  REQUIRE_EQ(observed_refcount, expected_refcount);
  REQUIRE_EQ(owner_result, (void *)&expected_result_storage);
  REQUIRE_EQ(results_ok, started);
}

TEST(futures, blocking_submit_future_from_within_a_task_reports_a_full_queue) {
  /*
   * A worker of a pool can make a blocking future submission into that same
   * pool, and such a call returns NULL once the queue is full, for the reason
   * that submit.blocking_submit_from_within_a_task_reports_container_full
   * gives: the thread that calls is one of the few threads that could free
   * the slot that it would wait for.
   */
  ctpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval_t submitted =
      ctpool_submit(pool, self_submit_future_task_fn, &ctx, NULL);
  ctpool_wait(pool);

  ctpool_future *first = ctx.first_future;
  ctpool_future *second = ctx.second_future;
  if (first) ctpool_future_free(first);
  if (second) ctpool_future_free(second);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_NE((void *)first, NULL);
  REQUIRE_EQ((void *)second, NULL);
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
  /* Single worker, 1-slot queue: block the worker so that the queue fills,
   * and try_submit_future must then return ccol_container_full without
   * blocking. */
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

  uint64_t ts = 1000000;
  ctpool_future *f = NULL;
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &value, ts, &f);
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
  uint64_t ts = 50000; /* 50 ms */
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &dummy, ts, &f2);
  REQUIRE_EQ(r, ccol_timed_out);
  REQUIRE_EQ((void *)f2, NULL);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_small_timeout_waits_about_that_long) {
  /* The same wait as timed_submit.a_small_timeout_waits_about_that_long,
   * through ctpool_timed_submit_future, which computes its own absolute
   * deadline through the same helper. */
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
  uint64_t bad_timeout = 500000;
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &dummy, bad_timeout, &f2);
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

TEST(futures, timed_submit_future_zero_timeout_acts_as_try) {
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
      ctpool_timed_submit_future(pool, identity_fn, &dummy, 0, &f2);
  REQUIRE_EQ(r, ccol_container_full);
  REQUIRE_EQ((void *)f2, NULL);

  atomic_store(&gate, 1);
  void *res = ctpool_future_get(f1);
  REQUIRE_EQ(res, (void *)&dummy);
  ctpool_future_free(f1);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_zero_timeout_with_invalid_args) {
  /* Mirrors timed_submit's identical companion test, for the future
   * variant's own zero-timeout delegation path. */
  ctpool_construct(pool, 2, 0);
  int value = 0;

  ctpool_future *f = (ctpool_future *)0x1;
  REQUIRE_EQ(
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, 0, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = (ctpool_future *)0x1;
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, 0, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, timed_submit_future_unbounded_never_blocks_on_capacity) {
  /* Unbounded queue: timed_submit_future must always succeed, even with a
   * near-zero timeout, because no capacity check is needed. */
  ctpool_construct(pool, 2, 0);
  int value = 7;

  uint64_t ts = 1; /* 1 us */
  ctpool_future *f = NULL;
  ccol_retval_t r =
      ctpool_timed_submit_future(pool, identity_fn, &value, ts, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = ctpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, done_is_false_before_completion) {
  /* ctpool_future_done must return false before the task finishes. A gate
   * guarantees that the task cannot complete before the check, which makes
   * the assertion deterministic rather than timing-dependent. */
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
  uint64_t ts = 1000000;

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
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(ctpool_timed_submit_future(pool, identity_fn, &value, ts, NULL),
             ccol_invalid_args);

  /* ctpool_submit_future: NULL pool, NULL fn */
  REQUIRE_EQ((void *)ctpool_submit_future(CTPOOL_INVALID, identity_fn, &value),
             NULL);
  REQUIRE_EQ((void *)ctpool_submit_future(pool, NULL, &value), NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ctpool_try_submit_future and ctpool_timed_submit_future both document that
 * they set out to NULL on any failure, so neither may leave *out untouched on
 * its two earliest failure paths (an invalid or stale pool handle, and a NULL
 * fn); it is not enough to zero *out only on the later failure paths, such as
 * a full queue or an out-of-memory condition.
 *
 * futures.invalid_args above cannot catch this, because it sets f to NULL
 * before every call, so a regression that leaves garbage in *out reads back
 * as NULL by coincidence. This test instead poisons *out with a distinctive
 * non-NULL sentinel right before each call that must fail on one of those
 * two earliest paths, which makes the failure visible, although the return
 * code alone does not change either way. */
TEST(futures, out_param_zeroed_even_on_earliest_failure_paths) {
  ctpool_construct(pool, 2, 16);
  int value = 0;
  uint64_t ts = 1000000;
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
      ctpool_timed_submit_future(CTPOOL_INVALID, identity_fn, &value, ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(ctpool_timed_submit_future(pool, NULL, &value, ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, submit_future_returns_null_on_oom) {
  /* With a custom out-of-memory allocator, the allocation of the task fails
   * inside alloc_future_task, and the library must then clean up the future
   * that it built in part; the function must return NULL and
   * ccol_not_enough_memory.
   *
   * Note: plain calloc, not the custom allocator of the pool, allocates the
   * ctpool_future struct itself, so the lifetime of that struct is fully
   * independent of the pool. This test does not exercise a failure of that
   * allocation, because such a test would have to intercept the system
   * allocator (for example with LD_PRELOAD), which is outside the scope of
   * these unit tests. */
  ccol_memmgmt_procs_t mp = {.malloc = oom_malloc,
                             .free = oom_free,
                             .calloc = oom_calloc,
                             .realloc = oom_realloc};
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(2, 0, &mp, &err);
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

/* A pthread start routine that fulfills a detached future after a short
 * delay, used to prove that ctpool_future_get() blocks until an external
 * (non-worker) producer thread calls ctpool_future_fulfill(). */
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
  /* No cthread_pool is involved at all, which proves that the future is
   * genuinely standalone. */
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
  /* The caller drops its reference before the producer fulfills, so the
   * future must stay alive (the producer holds a reference) and be freed once
   * fulfill runs; valgrind verifies that there is no leak and no
   * use-after-free. */
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
  /* This test creates, fulfills and frees several detached futures that no
   * ctpool builds, while a real pool does unrelated work beside them, which
   * proves that there is no hidden coupling between the two. */
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

// This test runs several bursts of tasks, each of a random size, and each
// task increments one shared atomic counter. The test drains with
// ctpool_wait after every burst and then checks that three things hold
// every time: ctpool_pending_count and ctpool_active_count are back to
// zero, and the total in the counter matches the total number of tasks
// submitted so far. So every submitted task runs exactly once, no more and
// no less, and the pool always reports itself fully idle once it is drained.
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
  /* ctpool_wait must not shut down the pool: the workers must remain alive
   * and accept new tasks after it returns. */
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
   * The setup: one worker runs a task that a gate blocks, so active_count is
   * 1, while five tasks queue up, and a waiter thread enters ctpool_wait. The
   * test then releases the gate, and the worker leaves blocker_fn, so
   * active_count drops to zero for a short window before the worker picks up
   * the next queued task. The ctpool_shutdown_immediate call after that sees
   * this window, where active_count is 0 and the queue is not empty; it
   * discards the queue, and it must broadcast idle_cv so that the waiter
   * returns. Without that broadcast the waiter deadlocks. */
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
  REQUIRE_EQ(pthread_create(&waiter, NULL, pool_wait_thread, &pool), 0);
  sleep_ms(10); /* let waiter enter ccol_cond_var_wait inside ctpool_wait */

  /* Release the gate, then poll until active_count drops to 0. At the moment
   * the poll reads 0, the worker has finished blocker_fn but has not yet
   * incremented active_count again for the next queued task, so calling
   * ctpool_shutdown_immediate immediately after maximises the chance that it
   * sees active_count==0 with a non-empty queue: the condition that would
   * deadlock ctpool_wait without the idle_cv broadcast. */
  atomic_store(&gate, 1);
  while (ctpool_active_count(pool) > 0) sleep_ms(1);
  ctpool_shutdown_immediate(pool);

  pthread_join(waiter, NULL); /* deadlocks without the idle_cv broadcast */
  ctpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_drain_unblocks_wait) {
  /* The idle_cv broadcast at the completion of the last task must unblock a
   * thread that waits in ctpool_wait, also when another thread calls
   * ctpool_shutdown_drain at the same time, while tasks run. Unlike
   * shutdown_immediate, the drain path sends no idle_cv signal of its own, so
   * it depends only on the broadcast from the last task to complete. */
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
  bool started_waiter =
      (pthread_create(&waiter, NULL, pool_wait_thread, &pool) == 0);
  sleep_ms(10); /* let waiter block inside ctpool_wait */

  /* A helper thread releases the gate after a delay, so that the worker
   * exits blocker_fn and drains the queued tasks while shutdown_drain is
   * already in progress. */
  pthread_t releaser;
  bool started_releaser =
      (pthread_create(&releaser, NULL, release_gate_fn, &gate) == 0);

  ctpool_shutdown_drain(pool);
  if (started_releaser) pthread_join(releaser, NULL);
  REQUIRE_TRUE(started_waiter);
  REQUIRE_TRUE(started_releaser);
  if (started_waiter) pthread_join(waiter, NULL); /* must not deadlock */
  REQUIRE_EQ(atomic_load(&counter), 5);
  ctpool_destroy(pool);
}

TEST(wait, returns_immediately_when_pool_already_drained) {
  /* ctpool_shutdown_drain joins every worker, so a ctpool_wait call after
   * that must return at once: active_count is 0 and queue_size is 0, so the
   * while condition is false on entry and the call reaches no
   * ccol_cond_var_wait. */
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
  /* Block the single worker so that tasks accumulate, then call
   * ctpool_shutdown_immediate while the gate is closed, which discards the
   * queue atomically under the pool mutex before the worker can dequeue
   * anything. A helper thread releases the gate after a brief delay, so that
   * the worker can exit and the join inside ctpool_shutdown_immediate
   * completes. All 10 queued tasks must be discarded, so the counter stays 0.
   */
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
  REQUIRE_EQ(pthread_create(&releaser, NULL, release_gate_fn, &gate), 0);

  /* The queue is discarded here (the worker is in blocker_fn, not
   * dequeuing), and the call blocks inside until the worker joins; the helper
   * thread releases the gate so that the worker exits. */
  ctpool_shutdown_immediate(pool);
  pthread_join(releaser, NULL);

  REQUIRE_EQ(atomic_load(&counter), 0);
  ctpool_destroy(pool);
}

TEST(shutdown_immediate, futures_become_cancelled) {
  /* Block the single worker, queue two futures, then call
   * ctpool_shutdown_immediate while the gate is closed (gate=0). The queue
   * is discarded atomically before the worker can dequeue anything, so both
   * futures must be cancelled. A helper thread releases the gate after a
   * brief delay, so that the worker can exit. */
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
  REQUIRE_EQ(pthread_create(&releaser, NULL, release_gate_fn, &gate), 0);

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

/* One heap argument for each task of the discard tests below. on_complete
 * records the ran flag that it got and frees the argument, so a second call
 * for the same task is a double free that valgrind and the allocator report,
 * and a missing call is a leak. */
typedef struct {
  atomic_int *completions; /* on_complete calls, over every task */
  atomic_int *ran_true;    /* of which with ran == true */
  atomic_int *fn_runs;     /* fn calls, over every task */
  int index;
  atomic_int *seen; /* on_complete calls for each index */
} discard_arg_t;

static void discard_fn(void *arg) {
  atomic_fetch_add(((discard_arg_t *)arg)->fn_runs, 1);
}

static void discard_on_complete(void *arg, bool ran) {
  discard_arg_t *a = (discard_arg_t *)arg;
  atomic_fetch_add(&a->seen[a->index], 1);
  if (ran) atomic_fetch_add(a->ran_true, 1);
  atomic_fetch_add(a->completions, 1);
  free(a);
}

#define DISCARD_TASKS 16

TEST(shutdown_immediate, on_complete_reports_discarded_tasks_as_not_run) {
  /* ctpool_shutdown_immediate discards the DISCARD_TASKS queued tasks, and
   * each one gets exactly one on_complete call, with ran == false, on the
   * thread that calls ctpool_shutdown_immediate and before that call
   * returns. Because each on_complete frees its heap argument, the memtest
   * run of this suite also proves that no argument is freed twice or leaked.
   */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ccol_retval_t blocker_rv = ctpool_submit(pool, blocker_fn, &ctx, NULL);
  if (blocker_rv == ccol_success)
    while (!atomic_load(&started)) sleep_ms(1);

  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (ctpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }

  pthread_t releaser;
  int created = pthread_create(&releaser, NULL, release_gate_fn, &gate);
  if (created != 0) atomic_store(&gate, 1);
  ctpool_shutdown_immediate(pool); /* discards every queued task */
  int completions_at_return = atomic_load(&completions);
  if (created == 0) pthread_join(releaser, NULL);
  ctpool_destroy(pool);

  REQUIRE_EQ(blocker_rv, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(accepted, DISCARD_TASKS);
  REQUIRE_EQ(completions_at_return, DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&completions), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&ran_true), 0);
  REQUIRE_EQ(atomic_load(&fn_runs), 0);
  for (int i = 0; i < DISCARD_TASKS; i++) REQUIRE_EQ(atomic_load(&seen[i]), 1);
}

/* An on_complete for a discarded task that calls back into its own pool.
 * There, the wait and both shutdowns are no-ops, exactly as from a worker,
 * and a submit is refused because the pool is in shutdown; otherwise a wait
 * on the shutdown that this very thread has yet to finish would hang. */
typedef struct {
  ctpool pool;
  atomic_int calls;
  atomic_int submit_rv;
} discard_reentry_ctx_t;

static void discard_reentry_on_complete(void *arg, bool ran) {
  discard_reentry_ctx_t *c = (discard_reentry_ctx_t *)arg;
  if (!ran) {
    ctpool_wait(c->pool);
    ctpool_shutdown_drain(c->pool);
    ctpool_shutdown_immediate(c->pool);
    atomic_store(&c->submit_rv,
                 (int)ctpool_submit(c->pool, noop_fn, NULL, NULL));
  }
  atomic_fetch_add(&c->calls, 1);
}

TEST(shutdown_immediate, discarded_on_complete_can_call_back_into_its_pool) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  discard_reentry_ctx_t rc = {.pool = pool};
  atomic_init(&rc.calls, 0);
  atomic_init(&rc.submit_rv, -1);
  ccol_retval_t blocker_rv = ctpool_submit(pool, blocker_fn, &ctx, NULL);
  if (blocker_rv == ccol_success)
    while (!atomic_load(&started)) sleep_ms(1);
  ccol_retval_t rv =
      ctpool_submit(pool, noop_fn, &rc, discard_reentry_on_complete);

  pthread_t releaser;
  int created = pthread_create(&releaser, NULL, release_gate_fn, &gate);
  if (created != 0) atomic_store(&gate, 1);
  ctpool_shutdown_immediate(pool);
  if (created == 0) pthread_join(releaser, NULL);
  ctpool_destroy(pool);

  REQUIRE_EQ(blocker_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(atomic_load(&rc.calls), 1);
  REQUIRE_EQ(atomic_load(&rc.submit_rv), (int)ccol_not_permitted);
}

/* Two pools whose discarded on_complete callbacks run nested on one thread:
 * the callback of a task discarded from outer shuts down inner, and the
 * callback of a task discarded from inner then runs inside it. There, the
 * thread counts as a worker of outer too, so a wait on outer, a shutdown of
 * outer and a submit to outer behave exactly as from a callback of outer
 * itself. If outer were forgotten while inner discards, the shutdown of
 * outer from there would wait for ever on a shutdown that this very thread
 * owns and has not finished, so the callback asks the self-call check first
 * and calls into outer only when the check says yes.
 *
 * Because a shutdown runs the discard callbacks before it joins the workers,
 * each callback opens the gate of the blocked worker of its own pool, so the
 * queued task of a pool cannot run before the shutdown discards it, however
 * slow the host is. */
typedef struct {
  ctpool outer;
  ctpool inner;
  atomic_int *gate_outer;
  atomic_int *gate_inner;
  struct cthread_pool *outer_raw;
  struct cthread_pool *inner_raw;
  atomic_int outer_calls;
  atomic_int inner_calls;
  atomic_int inner_sees_outer;
  atomic_int inner_sees_inner;
  atomic_int outer_sees_outer_after;
  atomic_int submit_rv;
} nested_discard_ctx_t;

static void nested_discard_inner_on_complete(void *arg, bool ran) {
  nested_discard_ctx_t *c = (nested_discard_ctx_t *)arg;
  if (!ran) {
    bool outer_self = _ctpool_is_self_call_for_tests(c->outer_raw);
    atomic_store(&c->inner_sees_outer, outer_self ? 1 : 0);
    atomic_store(&c->inner_sees_inner,
                 _ctpool_is_self_call_for_tests(c->inner_raw) ? 1 : 0);
    if (outer_self) {
      ctpool_wait(c->outer);
      ctpool_shutdown_drain(c->outer);
      ctpool_shutdown_immediate(c->outer);
      atomic_store(&c->submit_rv,
                   (int)ctpool_submit(c->outer, noop_fn, NULL, NULL));
    }
  }
  atomic_fetch_add(&c->inner_calls, 1);
  atomic_store(c->gate_inner, 1);
}

static void nested_discard_outer_on_complete(void *arg, bool ran) {
  nested_discard_ctx_t *c = (nested_discard_ctx_t *)arg;
  if (!ran) {
    ctpool_shutdown_immediate(c->inner);
    /* The frame of inner is gone, and the one of outer remains. */
    atomic_store(&c->outer_sees_outer_after,
                 _ctpool_is_self_call_for_tests(c->outer_raw) ? 1 : 0);
  }
  atomic_fetch_add(&c->outer_calls, 1);
  atomic_store(c->gate_outer, 1);
}

/* A bound on a test whose callbacks open their own gates: when a callback
 * does not run, this thread opens the gate after 10 s, so that the test fails
 * instead of hanging. It returns as soon as the gate is open. */
static void *release_gate_late_fn(void *arg) {
  atomic_int *gate = (atomic_int *)arg;
  for (int i = 0; i < 10000 && !atomic_load(gate); i++) sleep_ms(1);
  atomic_store(gate, 1);
  return NULL;
}

TEST(shutdown_immediate, nested_discards_keep_the_outer_pool_a_self_call) {
  atomic_int gate_outer = 0, gate_inner = 0;
  atomic_int started_outer = 0, started_inner = 0;
  gate_ctx_t ctx_outer = {.gate = &gate_outer, .started = &started_outer};
  gate_ctx_t ctx_inner = {.gate = &gate_inner, .started = &started_inner};

  ctpool_construct(outer, 1, 0);
  ctpool_construct(inner, 1, 0);
  nested_discard_ctx_t nc = {.outer = outer,
                             .inner = inner,
                             .gate_outer = &gate_outer,
                             .gate_inner = &gate_inner,
                             .outer_raw = _ctpool_resolve_for_tests(outer),
                             .inner_raw = _ctpool_resolve_for_tests(inner)};
  atomic_init(&nc.outer_calls, 0);
  atomic_init(&nc.inner_calls, 0);
  atomic_init(&nc.inner_sees_outer, -1);
  atomic_init(&nc.inner_sees_inner, -1);
  atomic_init(&nc.outer_sees_outer_after, -1);
  atomic_init(&nc.submit_rv, -1);

  /* Each single worker is held busy, so the next task of each pool stays in
   * the queue until a shutdown discards it. */
  ccol_retval_t b1 = ctpool_submit(outer, blocker_fn, &ctx_outer, NULL);
  ccol_retval_t b2 = ctpool_submit(inner, blocker_fn, &ctx_inner, NULL);
  if (b1 == ccol_success)
    while (!atomic_load(&started_outer)) sleep_ms(1);
  if (b2 == ccol_success)
    while (!atomic_load(&started_inner)) sleep_ms(1);
  ccol_retval_t r1 =
      ctpool_submit(outer, noop_fn, &nc, nested_discard_outer_on_complete);
  ccol_retval_t r2 =
      ctpool_submit(inner, noop_fn, &nc, nested_discard_inner_on_complete);

  pthread_t rel_outer, rel_inner;
  int c1 = pthread_create(&rel_outer, NULL, release_gate_late_fn, &gate_outer);
  if (c1 != 0) atomic_store(&gate_outer, 1);
  int c2 = pthread_create(&rel_inner, NULL, release_gate_late_fn, &gate_inner);
  if (c2 != 0) atomic_store(&gate_inner, 1);
  ctpool_shutdown_immediate(outer);
  if (c1 == 0) pthread_join(rel_outer, NULL);
  if (c2 == 0) pthread_join(rel_inner, NULL);
  bool after_outside = _ctpool_is_self_call_for_tests(nc.outer_raw);
  ctpool_destroy(inner);
  ctpool_destroy(outer);

  REQUIRE_EQ(b1, ccol_success);
  REQUIRE_EQ(b2, ccol_success);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(c1, 0);
  REQUIRE_EQ(c2, 0);
  REQUIRE_EQ(atomic_load(&nc.outer_calls), 1);
  REQUIRE_EQ(atomic_load(&nc.inner_calls), 1);
  REQUIRE_EQ(atomic_load(&nc.inner_sees_inner), 1);
  REQUIRE_EQ(atomic_load(&nc.inner_sees_outer), 1);
  REQUIRE_EQ(atomic_load(&nc.outer_sees_outer_after), 1);
  REQUIRE_EQ(atomic_load(&nc.submit_rv), (int)ccol_not_permitted);
  REQUIRE_FALSE(after_outside);
}

/* A pthread start routine that drains the pool whose handle arg points to. */
static void *discard_drain_thread_fn(void *arg) {
  ctpool_shutdown_drain(*(ctpool *)arg);
  return NULL;
}

TEST(shutdown_immediate, escalated_drain_reports_discarded_tasks_as_not_run) {
  /* A drain is under way on another thread, held up by a blocked task, and a
   * ctpool_shutdown_immediate escalates it: every task that is queued gets
   * on_complete(arg, false) exactly once, and none of them runs. */
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ccol_retval_t blocker_rv = ctpool_submit(pool, blocker_fn, &ctx, NULL);
  if (blocker_rv == ccol_success)
    while (!atomic_load(&started)) sleep_ms(1);
  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (ctpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }

  /* The drain starts on its own thread and stays blocked behind the gate,
   * and the immediate call below starts only once the drain owns the
   * shutdown. */
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  pthread_t drainer;
  int drainer_created =
      pthread_create(&drainer, NULL, discard_drain_thread_fn, &pool);
  bool drain_started = false;
  if (drainer_created == 0 && raw) {
    for (int i = 0; i < 5000 && !drain_started; i++) {
      _ctpool_shutdown_state_for_tests(raw, &drain_started, NULL, NULL);
      if (!drain_started) sleep_ms(1);
    }
  }
  pthread_t releaser;
  int created = pthread_create(&releaser, NULL, release_gate_fn, &gate);
  if (created != 0) atomic_store(&gate, 1);
  ctpool_shutdown_immediate(pool);
  int completions_at_return = atomic_load(&completions);
  if (created == 0) pthread_join(releaser, NULL);
  if (drainer_created == 0) pthread_join(drainer, NULL);
  ctpool_destroy(pool);

  REQUIRE_EQ(blocker_rv, ccol_success);
  REQUIRE_EQ(drainer_created, 0);
  REQUIRE_TRUE(drain_started);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(accepted, DISCARD_TASKS);
  REQUIRE_EQ(completions_at_return, DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&ran_true), 0);
  REQUIRE_EQ(atomic_load(&fn_runs), 0);
  for (int i = 0; i < DISCARD_TASKS; i++) REQUIRE_EQ(atomic_load(&seen[i]), 1);
}

TEST(submit, on_complete_reports_ran_for_every_task_that_ran) {
  /* A drain runs every queued task, so every on_complete gets ran == true. */
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);

  ctpool_construct(pool, 2, 0);
  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (ctpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(accepted, DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&completions), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&ran_true), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&fn_runs), DISCARD_TASKS);
  for (int i = 0; i < DISCARD_TASKS; i++) REQUIRE_EQ(atomic_load(&seen[i]), 1);
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
  ctpool pool = ccol_create_cthread_pool_mp(2, 0, &mp, &err);
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

/* This test covers a documented contract, which the "Result ownership" doc
 * comment in cthreadpool.h and the implementation comment on
 * alloc_future_task both state: plain malloc or calloc always allocates a
 * ctpool_future, and the custom allocator of the pool never does. Only the
 * internal ctpool_task node that carries the submission goes through
 * pool->m_procs.
 *
 * No other test separates this from the other design, where the future ALSO
 * goes through the custom procs: the comment on
 * futures.submit_future_returns_null_on_oom says that it cannot exercise the
 * allocation of the future at all, and a custom allocator that fails makes
 * *f == NULL either way, whichever of the two designs the library
 * implements.
 *
 * A custom allocator that COUNTS, instead of failing, closes that gap
 * directly: one ctpool_submit_future call must raise g_alloc_count by
 * exactly 1 (the task node alone), not by 2, while a regression that routes
 * the future itself through the custom calloc raises it by 2. */
TEST(custom_mprocs, future_struct_uses_plain_allocator_not_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(1, 0, &mp, &err);
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

  /* Every tracked allocation of this whole test (the construction of the pool
   * and the one task node) must have a matching tracked free, while the
   * allocation and the free of the future appear in neither count. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* This test verifies the reuse of a task node directly. It submits tasks one
 * at a time and waits for each task to complete fully before it submits the
 * next one. A completed task is freed, which means that it goes back onto
 * the free list of the pool, and ctpool_wait cannot see active_count reach 0
 * until the task_free call of the worker has returned. So no submission
 * after the very first one may make a new call to the custom allocator; the
 * first submission alone finds the free list empty. */
TEST(custom_mprocs, task_nodes_are_recycled_not_reallocated_each_time) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(1, 0, &mp, &err);
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

  /* No more tracked allocation may happen: every one of the N later
   * submissions reuses the exact node that the task_free of the submission
   * before it put back. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), alloc_after_first);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Queues burst tasks behind the one worker of pool, which a gate blocks, and
 * then lets the whole burst run. Because the queue is burst deep at its peak,
 * the burst needs burst + 1 task nodes at once. */
static bool ctp_gated_burst(ctpool pool, int burst) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  atomic_int counter = 0;
  bool ok = (ctpool_submit(pool, blocker_fn, &ctx, NULL) == ccol_success);
  while (ok && !atomic_load(&started)) sleep_ms(1);
  for (int i = 0; i < burst && ok; i++)
    ok = (ctpool_submit(pool, inc_counter, &counter, NULL) == ccol_success);
  atomic_store(&gate, 1);
  ctpool_wait(pool);
  return ok && atomic_load(&counter) == burst;
}

/* The free list keeps every node that a burst needed, up to its ceiling. A
 * trim keeps the whole list when the traffic since the previous trim emptied
 * it, and otherwise gives back half of what the list holds above
 * num_threads * 4. The numbers come from the white-box accessors, while the
 * expectations are written out instead of being read back from the library.
 */
TEST(custom_mprocs, task_free_list_keeps_what_the_last_burst_used) {
  ctpool_construct(pool, 1, 0);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(_ctpool_task_free_list_cap_for_tests(raw), (size_t)32768);

  /* A burst of 12 behind the blocker needs 13 nodes and keeps all 13; since
   * the burst emptied the list, the first trim after it gives nothing back. */
  bool first = ctp_gated_burst(pool, 12);
  size_t after_deep = _ctpool_task_free_list_size_for_tests(raw);

  /* With a trim at every idle point, one task at a time: the first trim
   * keeps 13, and every later one halves the excess over 4 (rounding the
   * half up): 13, 8, 6, 5, 4. */
  atomic_int counter = 0;
  _ctpool_set_task_free_list_window_for_tests(1);
  bool second = true;
  size_t sizes[5] = {0};
  for (int i = 0; i < 5 && second; i++) {
    second = (ctpool_submit(pool, inc_counter, &counter, NULL) == ccol_success);
    ctpool_wait(pool);
    sizes[i] = _ctpool_task_free_list_size_for_tests(raw);
  }
  _ctpool_set_task_free_list_window_for_tests(0);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
  REQUIRE_TRUE(first);
  REQUIRE_TRUE(second);
  REQUIRE_EQ(after_deep, (size_t)13);
  REQUIRE_EQ(sizes[0], (size_t)13);
  REQUIRE_EQ(sizes[1], (size_t)8);
  REQUIRE_EQ(sizes[2], (size_t)6);
  REQUIRE_EQ(sizes[3], (size_t)5);
  REQUIRE_EQ(sizes[4], (size_t)4);
}

/* A burst deeper than the ceiling leaves exactly the ceiling on the list. */
TEST(custom_mprocs, task_free_list_never_exceeds_its_ceiling) {
  ctpool_construct(pool, 1, 0);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  bool ok = ctp_gated_burst(pool, 32768 + 100);
  size_t kept = _ctpool_task_free_list_size_for_tests(raw);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(kept, (size_t)32768);
}

/* A deep queue costs no allocation once the pool has seen that depth: the
 * second and every later burst of the same depth reuse the nodes of the
 * first, so the custom allocator sees no call at all, even with a trim at
 * every idle point, because each burst empties the list, so no trim gives
 * anything back. This test is non-vacuous: with the list capped at
 * num_threads * 4, each later burst of 500 allocates 497 nodes again. */
TEST(custom_mprocs, repeated_deep_bursts_allocate_no_task_nodes) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  _ctpool_set_task_free_list_window_for_tests(1);
  bool ok = ctp_gated_burst(pool, 500);
  int after_first = atomic_load(&g_alloc_count);
  for (int round = 0; round < 5 && ok; round++) ok = ctp_gated_burst(pool, 500);
  int after_rest = atomic_load(&g_alloc_count);
  _ctpool_set_task_free_list_window_for_tests(0);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(after_rest, after_first);
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
  /* 2 slow workers, a 16-slot queue and 64 tasks: ctpool_submit must block
   * when the queue fills, and all tasks must complete. */
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
  /* Multiple threads submit tasks concurrently, and all tasks must execute
   * exactly once, which verifies that the queue and counter operations are
   * race-free under simultaneous submission from many producers. */
  enum { NPRODUCERS = 4, TASKS_PER = 250 };
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  producer_arg_t args[NPRODUCERS];
  pthread_t threads[NPRODUCERS];
  int created = 0;
  for (int i = 0; i < NPRODUCERS; i++) {
    args[i] =
        (producer_arg_t){.pool = pool, .counter = &counter, .n = TASKS_PER};
    /* A failure part way through must not leave the join loop below with an
     * uninitialized threads[i] slot to join, which is undefined behavior and
     * can hang on garbage pthread_t data, so the loop joins only the threads
     * that it really created. */
    if (pthread_create(&threads[i], NULL, producer_thread, &args[i]) != 0)
      break;
    created++;
  }
  REQUIRE_EQ(created, (int)NPRODUCERS);
  for (int i = 0; i < created; i++) {
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

/* These tests mirror the chttpcli_handle_lifecycle,
 * ccol_event_loop_handle_lifecycle and clrucache_handle_lifecycle test
 * groups, adapted for the lock-protected pin mechanism of ctpool (see the
 * comments on the pending_resolve_count and pin_cv fields of struct
 * cthread_pool in src/cthreadpool.c). Like clru_cache, ctpool reuses the
 * lock-protected decrement and broadcast of chttpcli and chttpsvr; this
 * module already uses one global mutex. */

/* Once a destroy completes fully, a second destroy call later, on a separate
 * copy of the same original handle value, must be a fatal error. This test
 * runs in a forked child, because ccol_fatal_err aborts the whole process,
 * and it follows the fork-test precedent in tests/clogger/tests.c for misuse
 * that ends the process. */

/* The publication of the handle into the pin index is the last step of pool
 * creation, and it can fail, because the index allocates a chunk and a stripe
 * block on the first use of an index, with plain calloc that no allocator of
 * a caller reaches. The rollback of that failure must put the slot back on
 * the free list; without that, the index is lost for the life of the process.
 *
 * The rollback also restores the slot to the shape that a destroy leaves it
 * in, which the fork below exercises as a smoke test rather than as a check
 * that can tell the two apart: this rollback never sets in_use, and
 * _ctpool_atfork_prepare skips every slot where neither in_use nor torn_down
 * is set, so the walk cannot reach a rolled-back slot, whatever its ptr
 * holds. Without the hook below, that path needs a real out-of-memory
 * condition, which an ordinary test run cannot reach. */
extern void _ccol_pintable_force_next_publish_failure_for_tests(void);

TEST(ctpool_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* The test repeats the cycle and asserts on the growth of the table over
   * the whole run, because a single cycle cannot tell a rollback apart from no
   * rollback: one lost slot makes the next pool grow the table by one, which
   * looks the same as a table with no free slot to start with. Over CYCLES
   * rounds a working rollback grows the table by nothing at all, while a
   * missing push onto the free list grows it by one in each round. */
  enum { CYCLES = 8 };
  size_t before = _ctpool_slot_table_capacity_for_tests();
  size_t free_before = _ctpool_free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  for (int i = 0; i < CYCLES; i++) {
    _ccol_pintable_force_next_publish_failure_for_tests();
    ctpool bad = ccol_create_cthread_pool(2, 0, NULL);
    if (bad != CTPOOL_INVALID) {
      all_failed = false;
      __ctpool_destroy(bad);
      break;
    }
    ctpool good = ccol_create_cthread_pool(2, 0, NULL);
    if (good == CTPOOL_INVALID) {
      all_created = false;
      break;
    }
    __ctpool_destroy(good);
  }

  _ccol_pintable_force_next_publish_failure_for_tests();
  char *err = NULL;
  ctpool failed = ccol_create_cthread_pool(2, 0, &err);
  /* The test captures these instead of asserting here. The first one is false
     only when the forced failure did not apply, and `failed` is then a live
     pool with two worker threads, so a return now would leak it, together
     with everything else that the rest of this test has yet to clean up. */
  bool forced_failure_applied = (failed == CTPOOL_INVALID);
  bool err_reported = (err != NULL);
  if (!forced_failure_applied) __ctpool_destroy(failed);

  /* A fork while that slot sits on the free list. The prepare handler walks
   * every slot, and it would dereference a rolled-back slot here if it
   * considered that slot live. The child does nothing but exit, so the whole
   * result under check is that the child reaches an exit at all.
   *
   * The test asserts only WIFEXITED, never a particular exit code. A leak
   * checker can run with an error exit code and treat still-reachable memory
   * as an error, and it then replaces the status of a forked child with that
   * code; since the child inherits the whole live image of the parent and
   * reports all of it at exit, a WEXITSTATUS check here would fail under
   * memtest and pass everywhere else, for a reason that has nothing to do
   * with the property under test. A crash in the prepare handler shows up
   * anyway, because WIFEXITED is then false. */
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  int status = 0;
  bool forked = (pid != -1);
  bool reaped = forked && (waitpid(pid, &status, 0) == pid);

  /* The slot went back on the free list, so the next pool takes it again,
   * and the table does not grow for the failed attempt. That pool must also
   * be fully usable: a stale self_handle left behind breaks it on its first
   * call. */
  ctpool pool = ccol_create_cthread_pool(2, 0, NULL);
  bool pool_ok = (pool != CTPOOL_INVALID);
  size_t after = _ctpool_slot_table_capacity_for_tests();
  size_t free_after = _ctpool_free_index_count_for_tests();
  size_t pending = 0;
  if (pool_ok) {
    pending = ctpool_pending_count(pool);
    ctpool_destroy(pool);
  }

  REQUIRE_TRUE(forked);
  REQUIRE_TRUE(reaped);
  REQUIRE_TRUE(forced_failure_applied);
  REQUIRE_TRUE(err_reported);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(pool_ok);
  REQUIRE_EQ(pending, (size_t)0);
  REQUIRE_TRUE(all_failed);
  REQUIRE_TRUE(all_created);
  /* Growth alone is not enough: a lost slot only grows the table while the free
     list is empty, so a run in which earlier tests left several free indices
     behind would absorb every loss silently. The free list has to come back to
     where it started too, which holds however deep it was. */
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

TEST(ctpool_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctpool stale = pool;     /* an independently-held copy of the handle value,
            distinct from the local the macro below invalidates */
    ctpool_destroy(pool);    /* completes normally (implicit drain shutdown);
           the local `pool` is now CTPOOL_INVALID, but `stale` still holds the
           original value */
    __ctpool_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

extern void _ctpool_run_exit_cleanup_for_tests(void);
extern bool _ctpool_worker_key_live_for_tests(void);

/* The process-exit cleanup of the slot table defers its release while a pool
 * is live, and the destroy of the last pool then performs it. After that
 * release the worker key is deleted, a new pool is refused with an error, and
 * a destroy of the old handle ends the process with the documented
 * stale-handle message. The child runs all of it, because the release is
 * final for the process. This test is non-vacuous: a deferred release that
 * leaves the key live lets the creation reach the released table, and the
 * child exits with 4 or aborts through an assertion in the vector module
 * instead of printing the stale-handle message. */
TEST(ctpool_handle_lifecycle, deferred_exit_release_retires_the_key) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    ctpool pool = ccol_create_cthread_pool(1, 0, NULL);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctpool stale = pool;
    _ctpool_run_exit_cleanup_for_tests();
    if (!_ctpool_worker_key_live_for_tests()) _exit(5);
    ctpool_destroy(pool);
    if (_ctpool_worker_key_live_for_tests()) _exit(3);
    char *err = NULL;
    ctpool again = ccol_create_cthread_pool(1, 0, &err);
    if (again != CTPOOL_INVALID || !err) _exit(4);
    __ctpool_destroy(stale);
    _exit(0); /* unreachable when ccol_fatal_err() aborts as expected */
  }
  close(fds[1]);
  char buf[2048];
  size_t got = 0;
  for (;;) {
    ssize_t r = pid == -1 ? 0 : read(fds[0], buf + got, sizeof(buf) - 1 - got);
    if (r <= 0) break;
    got += (size_t)r;
    if (got == sizeof(buf) - 1) break;
  }
  buf[got] = '\0';
  close(fds[0]);
  int status = 0;
  bool reaped = pid != -1 && waitpid(pid, &status, 0) == pid;
  REQUIRE_TRUE(reaped);
  REQUIRE_FALSE(WIFEXITED(status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
  REQUIRE_NE((void *)strstr(buf, "ctpool_destroy: handle is stale"), NULL);
}

typedef struct {
  ctpool h;
} ctp_concurrent_destroy_arg_t;

static void *ctp_concurrent_destroy_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  __ctpool_destroy(a->h);
  return NULL;
}

/* Two threads can call destroy on two separate copies of the SAME valid
 * handle, as close to the same moment as the test can arrange, and that must
 * also be fatal: it is the same class of concurrent double free that the
 * generation-tagged slot table closes for chttpcli, chttpsvr, ccol_event_loop
 * and clru_cache. */
TEST(ctpool_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_concurrent_destroy_arg_t a1 = {.h = pool};
    ctp_concurrent_destroy_arg_t a2 = {.h = pool};
    pthread_t t1, t2;
    /* This code runs inside a forked child, where REQUIRE_* is unsafe,
     * because its early return would skip the _exit() of this branch and fall
     * back into the test loop of the harness a second time. So a failed
     * create falls through to a distinct exit that is not SIGABRT, which the
     * WIFSIGNALED and SIGABRT check of the parent below turns into a clean
     * test failure, and the child does not join a garbage pthread_t that
     * nothing created. */
    if (pthread_create(&t1, NULL, ctp_concurrent_destroy_thread, &a1) != 0)
      _exit(2);
    if (pthread_create(&t2, NULL, ctp_concurrent_destroy_thread, &a2) != 0)
      _exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* Unreachable: whichever of the two destroy calls loses the
                 race must reach ccol_fatal_err(). */
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

typedef struct {
  ctpool h;
  _Atomic bool entered;
  _Atomic bool returned;
} ctp_destroy_thread_arg_t;

/* This function runs ctpool_destroy on its own thread instead of the main
 * test thread, and sets entered to true as its very first action and
 * returned to true as its very last. The main thread can then see two things
 * with no assumption about timing: that something called destroy, because
 * entered is true, and that destroy did NOT return while the test holds the
 * worker of the pool stuck on purpose. resolve_then_use_race_destroy_waits
 * below uses this, and the doc comment of that test explains why that order
 * is a real proof, not a heuristic. */
static void *ctp_destroy_thread_fn(void *arg) {
  ctp_destroy_thread_arg_t *a = (ctp_destroy_thread_arg_t *)arg;
  atomic_store(&a->entered, true);
  ctpool_destroy(a->h);
  atomic_store(&a->returned, true);
  return NULL;
}

/* The diagnostic aid for resolve_then_use_race_destroy_waits below. That
 * test hangs in one environment, linux-arm32-clang under real qemu-user
 * 8.2.2, where ctpool_destroy never returns. Unlike the fork-related hangs
 * elsewhere in this codebase, the test has no forked child to point /proc
 * at: the whole race lives between threads of this same process. So instead
 * of the /proc/<pid> files of one pid, this function dumps status, wchan,
 * syscall and stat for every thread of THIS process, from the
 * /proc/self/task/<tid> entries, when ctpool_destroy did not return inside a
 * generous, fixed bound.
 *
 * This function does NOT try to cancel the stuck call, or to recover it in
 * any other way, on purpose: a pthread_cancel that lands while a thread holds
 * a mutex can leave that lock held for the rest of the life of this process,
 * which corrupts every later use and reports no clean failure. The function
 * instead ends the whole process outright after it flushes the dump, with a
 * distinct exit code, so that the exit reads as a deliberate diagnostic exit
 * rather than an ordinary crash. That turns an uninformative 45-minute
 * timeout at the job level into a fast and informative one.
 *
 * The dump itself goes through plain, buffered fprintf(stderr, ...) instead
 * of cdebuglog_write(), because it runs only once the bounded window has
 * closed, well after the timing that mattered, so real write(2) syscalls cost
 * nothing here; _dump_stuck_child_diagnostics in tests/cthreadcomm/tests.c
 * reasons in the same way. The function calls cdebuglog_flush() first all the
 * same, so that every [DEBUG_TEST] checkpoint that is already buffered
 * appears in the correct order in time, ahead of the raw output of this dump.
 * Without that flush, the checkpoints stay buffered until process exit, and
 * the _exit() below skips that exit path and loses them for good. */
static bool _ctp_test_read_proc_task_file(pid_t tid, const char *name,
                                          char *buf, size_t buf_cap) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/self/task/%d/%s", (int)tid, name);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t n = read(fd, buf, buf_cap - 1);
  close(fd);
  if (n < 0) return false;
  buf[n] = '\0';
  return true;
}

static void _ctp_test_dump_all_threads_and_die(void) {
  char buf[4096];
  /* Flush whatever [DEBUG_TEST] content is buffered before anything else
   * prints, so that it appears in the correct order in time, ahead of the raw
   * fprintf() output of this function. Without this flush it is lost
   * completely, because the _exit() below skips the flush that cdebuglog
   * registers with atexit. */
  cdebuglog_flush();
  fprintf(stderr,
          "[WATCHDOG_DIAG] pid=%d: ctpool_destroy did not return within the "
          "bounded window; dumping every thread of this process\n",
          (int)getpid());
  DIR *td = opendir("/proc/self/task");
  if (!td) {
    fprintf(stderr, "[WATCHDOG_DIAG] /proc/self/task: unreadable\n");
  } else {
    struct dirent *ent;
    static const char *const files[] = {"status", "wchan", "syscall", "stat"};
    while ((ent = readdir(td)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      pid_t tid = (pid_t)atoi(ent->d_name);
      for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        if (_ctp_test_read_proc_task_file(tid, files[i], buf, sizeof(buf))) {
          fprintf(stderr, "[WATCHDOG_DIAG] /proc/self/task/%d/%s:\n%s\n",
                  (int)tid, files[i], buf);
        } else {
          fprintf(stderr,
                  "[WATCHDOG_DIAG] /proc/self/task/%d/%s: unreadable "
                  "(errno=%d %s)\n",
                  (int)tid, files[i], errno, strerror(errno));
        }
      }
    }
    closedir(td);
  }
  fflush(stderr);
  _exit(97); /* A distinct sentinel, which means that the watchdog found a
                stuck ctpool_destroy, not that an ordinary crash happened, so
                a CI log or an exit-code check can tell the two apart. */
}

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool done;
} ctp_destroy_watchdog_ctx_t;

/* A bounded condvar wait rather than a blind poll, because this codebase
 * prefers a real timed wait over a poll at a fixed interval. The wait wakes
 * when the main thread signals done or after the fixed bound, whichever
 * comes first.
 *
 * The test creates this thread at the very start of
 * resolve_then_use_race_destroy_waits, before both bounded safety-net polls
 * of that test. Each of those polls runs for up to 10s, neither can hang on
 * its own, and both run BEFORE the pthread_join that this watchdog protects.
 * The bound of 60 real seconds leaves a margin of three times over that
 * combined worst case of 20s, so a slow machine that is not hung can need
 * almost the full budget on both polls and still cannot trip this watchdog
 * on its own.
 *
 * Even at 60s this is a pure safety net against a hang, not a correctness
 * threshold that this test depends on: real synchronization drives every
 * step that it brackets, as the doc comment of
 * resolve_then_use_race_destroy_waits details. Only a real hang should ever
 * reach the timeout branch, and ordinary scheduling variance never
 * should. */
static void *ctp_destroy_watchdog_fn(void *arg) {
  ctp_destroy_watchdog_ctx_t *w = (ctp_destroy_watchdog_ctx_t *)arg;
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 60;
  pthread_mutex_lock(&w->mu);
  while (!w->done) {
    int rc = pthread_cond_timedwait(&w->cv, &w->mu, &deadline);
    if (rc == ETIMEDOUT) break;
  }
  bool timed_out = !w->done;
  pthread_mutex_unlock(&w->mu);
  if (timed_out) _ctp_test_dump_all_threads_and_die();
  return NULL;
}

/* A pin that a resolve takes must outlive the call that took it. This test
 * exercises that against the reversed order of waits that
 * __ctpool_teardown_raw uses: that function runs the shutdown drain BEFORE it
 * waits on pending_resolve_count (see its own comment in src/cthreadpool.c).
 *
 * The pool here has one worker and a queue_cap of 1. Its worker is stuck in
 * a first task that runs for a long time, and a second task already fills the
 * queue, so a third ctpool_submit call blocks inside the
 * ccol_cond_var_wait(not_full, ...) of submit_internal, holding a real,
 * resolved pin on the handle for the whole time that it blocks.
 *
 * A concurrent ctpool_destroy must do three things. First, it must not crash,
 * and it must not free the pool under that blocked submitter, which is still
 * pinned: that is the use-after-free that the generation-tagged slot table
 * and its pin counter close. Because only the not_full broadcast of the
 * shutdown can release the blocked wait of submit_internal, a wait on
 * pending_resolve_count BEFORE the shutdown would deadlock destroy for ever
 * instead. Second, it must let the blocked submit return ccol_not_permitted
 * once the shutdown starts. Third, it must block until something has joined
 * the worker thread, which is a real wait, not an instant return.
 *
 * The test proves all three properties below with NO assumption about a
 * fixed sleep, on purpose. One alternative takes a fixed head start of about
 * 10ms before the test creates a background thread, which releases the gate
 * after about 20ms, and treats a ctpool_destroy of more than 10ms as proof of
 * the third property. That is not a safe substitute, because both delays are
 * assumptions that depend on load, and they break under valgrind (on memtest
 * in a native linux-x86_64-gcc job, with no emulation at all) as soon as
 * unrelated thread-creation overhead elsewhere in the test eats into that
 * budget. Something then releases the gate before the test even calls
 * ctpool_destroy, and that call returns in about 0ms, or the blocked submit
 * comes back ccol_success instead of ccol_not_permitted, from the same cause.
 * Neither symptom is a product bug; both are that timing assumption under
 * load.
 *
 * The structure below uses a real synchronization point instead of any fixed
 * sleep. The entered and returned flags of ctp_destroy_thread_fn let the main
 * thread observe two things with no clock at all: that something called
 * ctpool_destroy, because entered is true, and that the call has provably NOT
 * returned while gate is 0. It structurally CANNOT have returned at that
 * point, however much wall-clock time passed, because the one worker of the
 * pool cannot leave the spin in blocker_fn, which is its only exit condition,
 * until this test sets gate itself, and ctpool_destroy cannot return before
 * something has joined that worker. The check below is therefore a real
 * proof of the third property, not a timing heuristic.
 *
 * _ctpool_pending_resolve_count_for_tests takes the place of any head start
 * in the same way: the test polls it, bounded only as a safety net against a
 * hang and never as a correctness threshold, which confirms that the blocked
 * submitter really pinned the handle before the test goes on, however long
 * that takes on the machine that runs it.
 *
 * This test also hangs in one environment, linux-arm32-clang under real
 * qemu-user 8.2.2, where the destroy thread below never finishes inside the
 * 45-minute timeout of that job. Because this test never calls fork(), the
 * confirmed, external, already-filed qemu-user fd_trans_lock bug cannot be
 * the cause. That bug is about fd_trans_lock, an internal process-wide
 * pthread mutex of linux-user, which stays locked for ever in a forked child
 * when another thread in the parent held it at the instant of the fork() (see
 * gitlab.com/qemu-project/qemu/-/issues/2846). That emulator bug has no way
 * to trigger without a fork() call, while the race of this test lives
 * entirely between ordinary threads of one process. The real cause remains
 * open.
 *
 * Two things exist so that a recurrence carries enough information to
 * diagnose it. The first is the [DEBUG_TEST] checkpoints below, which
 * cdebuglog_write() logs instead of a direct fprintf(stderr, ...), because a
 * buffer for these checkpoints matters for a timing-sensitive mystery like
 * this one, and a real write(2) syscall for each checkpoint does not (see
 * cdebuglog.h for the reason). The second is the watchdog thread that
 * brackets the join of the destroy thread below, which the comment on that
 * section above describes. The checkpoint that printed last narrows down how
 * far the main thread got, and the /proc dump of the watchdog shows the real
 * kernel-level wait state of every thread, as wchan and syscall, at the
 * moment of the hang. Without them there is only a bare timeout with no
 * information. */
TEST(ctpool_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);

  /* The test creates this thread at once. Its bounded 60s wait is a pure
   * safety net against a hang, which the doc comment of its own section above
   * explains, including the choice of 60s. That wait is independent of every
   * timing concern in the rest of this test, so its exact creation point does
   * not matter. */
  ctp_destroy_watchdog_ctx_t watchdog = {.mu = PTHREAD_MUTEX_INITIALIZER,
                                         .cv = PTHREAD_COND_INITIALIZER,
                                         .done = false};
  pthread_t watchdog_thread;
  int watchdog_rc = pthread_create(&watchdog_thread, NULL,
                                   ctp_destroy_watchdog_fn, &watchdog);
  bool watchdog_created = (watchdog_rc == 0);

  cdebuglog_write("[DEBUG_TEST] pid=%d about to submit blocker_fn\n",
                  (int)getpid());
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  cdebuglog_write(
      "[DEBUG_TEST] pid=%d blocker_fn started, about to fill queue\n",
      (int)getpid());

  /* Every step from here on can fail, and the worker of the pool is already
   * alive and spinning inside blocker_fn, so no step may carry a direct
   * assertion. Unlike ctpool_construct_scoped, ctpool_construct does not
   * destroy pool on scope exit, so an early return from a failed REQUIRE_*
   * here would leak that running worker, and every thread already created,
   * for the rest of the life of this test binary. The test therefore captures
   * every outcome into a local, on every path, and every REQUIRE_* runs only
   * after the test has joined every thread that it created and destroyed
   * pool. This codebase has a test-hygiene lesson on this exact class of gap.
   */

  /* Fill the bounded queue (cap 1), so that the next submit really blocks. */
  ccol_retval_t fill_rv = ctpool_submit(pool, inc_counter, &counter, NULL);

  ctp_blocked_submit_arg_t blocked_arg = {
      .h = pool, .counter = &counter, .rv = ccol_success};
  pthread_t blocked_thread;
  cdebuglog_write(
      "[DEBUG_TEST] pid=%d queue filled, about to create blocked_thread\n",
      (int)getpid());
  int blocked_rc = pthread_create(&blocked_thread, NULL,
                                  ctp_blocked_submit_thread, &blocked_arg);
  bool blocked_created = (blocked_rc == 0);
  /* Poll until the resolve of blocked_thread really pinned the handle. The
   * bound is only a safety net against a hang, never a correctness
   * threshold, and the test does not assume that a fixed sleep was long
   * enough; the doc comment of this test above says why a fixed sleep is not
   * a safe substitute. */
  if (blocked_created) {
    for (int i = 0;
         i < 10000 && _ctpool_pending_resolve_count_for_tests(raw) == 0; i++)
      sleep_ms(1);
  }

  cdebuglog_write("[DEBUG_TEST] pid=%d about to create destroy_thread\n",
                  (int)getpid());
  ctp_destroy_thread_arg_t destroy_arg = {
      .h = pool, .entered = false, .returned = false};
  pthread_t destroy_thread;
  int destroy_rc = pthread_create(&destroy_thread, NULL, ctp_destroy_thread_fn,
                                  &destroy_arg);
  bool destroy_created = (destroy_rc == 0);

  bool destroy_returned_before_gate_released = false;
  if (destroy_created) {
    /* Poll until destroy_thread really entered ctpool_destroy; the bound is
     * only a safety net against a hang. gate is 0 here, because this test has
     * not set it yet, so the one worker of the pool cannot have left the spin
     * in blocker_fn, and ctpool_destroy cannot have returned either, however
     * long entered took to become true. The doc comment of this test above
     * says why that makes the read below a real proof, not a timing
     * heuristic. */
    for (int i = 0; i < 10000 && !atomic_load(&destroy_arg.entered); i++)
      sleep_ms(1);
    destroy_returned_before_gate_released = atomic_load(&destroy_arg.returned);
  }

  cdebuglog_write("[DEBUG_TEST] pid=%d about to release gate\n", (int)getpid());
  atomic_store(&gate, 1);

  if (destroy_created) pthread_join(destroy_thread, NULL);
  cdebuglog_write("[DEBUG_TEST] pid=%d destroy_thread joined\n", (int)getpid());

  if (watchdog_created) {
    pthread_mutex_lock(&watchdog.mu);
    watchdog.done = true;
    pthread_cond_signal(&watchdog.cv);
    pthread_mutex_unlock(&watchdog.mu);
    pthread_join(watchdog_thread, NULL);
  }
  /* A statically initialized mutex or condition variable needs its destroy
   * too, because FreeBSD allocates the object behind it on first use. */
  pthread_mutex_destroy(&watchdog.mu);
  pthread_cond_destroy(&watchdog.cv);

  if (blocked_created) pthread_join(blocked_thread, NULL);

  REQUIRE_EQ(fill_rv, ccol_success);
  REQUIRE_EQ(blocked_rc, 0);
  REQUIRE_EQ(destroy_rc, 0);
  REQUIRE_EQ(watchdog_rc, 0);
  REQUIRE_FALSE(destroy_returned_before_gate_released);
  REQUIRE_EQ(blocked_arg.rv, ccol_not_permitted);
  REQUIRE_TRUE(atomic_load(&destroy_arg.returned));
}

typedef struct {
  ctpool h;
} ctp_pending_count_arg_t;

static void *ctp_pending_count_thread(void *arg) {
  ctp_pending_count_arg_t *a = (ctp_pending_count_arg_t *)arg;
  /* The return value is ignored on purpose: a legitimate race with a
   * concurrent destroy can make this resolve fail (returning 0) instead of
   * succeeding, and both outcomes are correct. This thread exists only to
   * generate resolve/pin/unpin traffic concurrent with the destroy thread
   * below. */
  ctpool_pending_count(a->h);
  return NULL;
}

/* This test differs from resolve_then_use_race_destroy_waits above, and is
 * not redundant with it. The blocked submit of that test guarantees
 * pending_resolve_count > 0 for a long, deterministic window, while this test
 * needs the opposite shape: a fast entry point that does not block, raced
 * against a concurrent destroy. ctpool_pending_count is such an entry point:
 * it resolves, reads one field under a mutex, unpins and returns.
 *
 * The test repeats that race under stress, because the failure window for a
 * fast pin and unpin pair is only a few instructions wide, so one run without
 * stress does not reproduce it reliably. Each iteration uses a fresh pool, so
 * every repetition gets its own independent race, and no iteration reuses a
 * handle that is already destroyed. */
TEST(ctpool_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);

    ctp_pending_count_arg_t pending_arg = {.h = pool};
    ctp_concurrent_destroy_arg_t destroy_arg = {.h = pool};
    pthread_t pending_tid, destroy_tid;
    bool started_pending_tid =
        (pthread_create(&pending_tid, NULL, ctp_pending_count_thread,
                        &pending_arg) == 0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, ctp_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    if (started_pending_tid) pthread_join(pending_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    REQUIRE_TRUE(started_pending_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

static void *ctp_concurrent_shutdown_immediate_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  ctpool_shutdown_immediate(a->h);
  return NULL;
}

/* _ctpool_teardown_raw must not read pool->shutdown_started with no lock held
 * before it decides whether to run a drain shutdown, because that read races
 * the write to the same field from an in-flight, pinned
 * ctpool_shutdown_immediate or ctpool_shutdown_drain call, which holds the
 * right lock (see the comment on _ctpool_teardown_raw in src/cthreadpool.c
 * for the full account). The requirement is to always call
 * _ctpool_shutdown_drain_internal, which is idempotent, instead of reading
 * the field first.
 *
 * This test races an explicit ctpool_shutdown_immediate call on one thread
 * against __ctpool_destroy on another, both on the same live handle, and
 * repeats the race under stress. Nothing may crash, hang or corrupt state,
 * whichever thread reaches pool->mu first. */
TEST(ctpool_handle_lifecycle,
     shutdown_immediate_races_concurrent_destroy_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);

    ctp_concurrent_destroy_arg_t shutdown_arg = {.h = pool};
    ctp_concurrent_destroy_arg_t destroy_arg = {.h = pool};
    pthread_t shutdown_tid, destroy_tid;
    REQUIRE_EQ(
        pthread_create(&shutdown_tid, NULL,
                       ctp_concurrent_shutdown_immediate_thread, &shutdown_arg),
        0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, ctp_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    pthread_join(shutdown_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

/* Valid reuse of a slot must never look like a stale handle to the object
 * that held that slot before, which is the whole point of the generation
 * counter. */
TEST(ctpool_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  char *err = NULL;
  ctpool a = ccol_create_cthread_pool(2, 0, &err);
  REQUIRE_NE(a, CTPOOL_INVALID);
  ctpool stale_a = a;
  ctpool_destroy(a);

  ctpool b = ccol_create_cthread_pool(2, 0, &err);
  REQUIRE_NE(b, CTPOOL_INVALID);

  /* The operations of B must succeed as usual, whether or not the allocator
   * reused the exact address of A for B. */
  REQUIRE_EQ(ctpool_pending_count(b), (size_t)0);

  /* The stale handle of A must never resolve to B, even when B reused the
   * same underlying address: that is the whole point of the generation
   * counter. */
  REQUIRE_EQ((void *)_ctpool_resolve_for_tests(stale_a), NULL);

  ctpool_destroy(b);
}

/* The slot table is bounded and does not grow for ever: a churn loop of
 * creates and destroys keeps only one slot in flight at a time, so it must
 * reuse that one freed slot on every iteration instead of growing the table
 * further.
 *
 * The test captures the capacity right after the first create and destroy
 * pair instead of asserting a fixed value such as 1, because other tests
 * earlier in this same process may already have grown the table to some N
 * above 1. This test must prove that ITS OWN churn adds no more growth, not
 * what the absolute size of the table happens to be when it runs. */
TEST(ctpool_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  char *err = NULL;
  ctpool pool0 = ccol_create_cthread_pool(2, 0, &err);
  REQUIRE_NE(pool0, CTPOOL_INVALID);
  ctpool_destroy(pool0);
  size_t capacity_after_first = _ctpool_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ctpool pool = ccol_create_cthread_pool(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);
    ctpool_destroy(pool);
  }

  REQUIRE_EQ(_ctpool_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*                    SELF-CALL FROM WITHIN A TASK                           */
/* ========================================================================== */

/* This guards against a use-after-free that happens every time. A task, or
 * the on_complete callback of that task, can call ctpool_destroy on the pool
 * that it runs on, and the mutex, the condition variables and the struct of
 * the pool must survive that call, because the worker thread that made the
 * call is on its way back through worker_thread_fn, which runs task_free and
 * then a lock, a decrement, a broadcast and an unlock against the freed
 * object.
 *
 * pthread_join on the id of the calling thread returns EDEADLK at once
 * instead of blocking. The shutdown guarantees that it joins every worker
 * before it frees anything, so an unchecked return value there would exempt
 * the calling worker from that guarantee. Such a self-call is a fatal error,
 * in the same way as a stale or already destroyed handle, and this test runs
 * in a forked child, because ccol_fatal_err aborts the whole process. */
typedef struct {
  ctpool pool;
} ctp_self_destroy_ctx_t;

static void self_destroy_task(void *arg) {
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  ctpool_destroy(ctx->pool); /* the actual misuse under test */
  /* This is unreachable when ccol_fatal_err() aborts, which is the expected
   * outcome. If the code does reach it, the post-task code of
   * worker_thread_fn (task_free, and then bookkeeping under pool->mu) runs
   * against a freed pool right after this function returns. */
}

TEST(ctpool_handle_lifecycle, destroy_from_within_own_task_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* This bounds the lifetime of the child, in case a regression turns the
     * crash into a hang (see the ctpool_wait call below). Because the parent
     * below only checks WTERMSIG against SIGABRT, an end through SIGALRM here
     * fails the test cleanly instead of hanging the whole suite. */
    alarm(2);

    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, self_destroy_task, &ctx, NULL);
    /* This is deterministic for the expected case, not a fixed sleep: the
     * worker ends the whole process with SIGABRT well before this call could
     * return, so what this thread does at that instant does not matter. The
     * alarm() above guards this call, so the test does not depend on this call
     * alone: a future regression could corrupt pool in a way that makes
     * ctpool_wait itself misbehave, instead of resolving pool cleanly as
     * stale. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* The same hazard, reached through on_complete instead of the task function
 * itself: on_complete runs on the same worker thread, before task_free, so it
 * is just as much inside the worker of the pool as fn is. */
static void self_destroy_on_complete(void *arg, bool ran) {
  (void)ran;
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  ctpool_destroy(ctx->pool);
}

TEST(ctpool_handle_lifecycle, destroy_from_within_own_on_complete_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
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
    ctpool pool = ccol_create_cthread_pool(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, noop_fn, &ctx, self_destroy_on_complete);
    /* See the identical comment in destroy_from_within_own_task_is_fatal
     * above. This is deterministic either way, not a guess based on a fixed
     * sleep, and the alarm() above bounds it in any case. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* This test covers the related, milder hazard of a self-call to
 * shutdown_drain. A task that calls ctpool_shutdown_drain on its own pool
 * must be a complete no-op: it is not enough that the call does not crash,
 * because it must also not set shutdown_drain or shutdown_started.
 *
 * A self-call that sets those two fields does two things. It makes the next
 * valid ctpool_submit call of this test fail with ccol_not_permitted. And the
 * self-join of this worker skips a join silently, which no LATER, real
 * external shutdown or destroy call can then retry, because
 * ccol_thread_join on the id of a thread returns EDEADLK instead of
 * blocking, so the OS resources of that worker thread leak for the rest of
 * the life of the process.
 *
 * Without this guard, this test fails on the first of those two points
 * alone. */
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

  /* The pool must be fully alive and accept new work, because a self-call
   * must shut nothing down. */
  atomic_int counter = 0;
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  /* A real, external shutdown/destroy afterward must work cleanly, which
   * proves that the earlier self-call left no worker thread permanently
   * un-joinable. */
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

/* This test covers the matching hazard of a self-call to ctpool_wait. Without
 * the guard, a task that waits on its own pool deadlocks for ever, because
 * the calling task itself counts in active_count until it returns. This test
 * runs in a forked child that alarm() bounds, so a regression here fails the
 * test instead of hanging the whole suite. */
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
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* The child reports its outcome through a pipe instead of its own process
   * exit code, because under make memtest, valgrind replaces the real exit
   * code of a forked child with its own --error-exitcode the instant it finds
   * ANY still reachable allocation in the inherited process image of that
   * child at exit time, and every child forked in the middle of the suite
   * always has one, since the rest of this suite is not quiet yet. So the
   * exit code cannot carry this result reliably. The test
   * fork_safety.destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future
   * a few tests below reasons in the same way, and the fork_safety group in
   * tests/clogger/tests.c holds the original, independently confirmed account
   * of this exact valgrind behaviour. */
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
    alarm(2); /* bounds this child's own lifetime if this regresses */

    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool(1, 0, &err);
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
  /* Not WEXITSTATUS, for the reason that the comment of this test above
   * gives. WIFEXITED alone catches a real regression that hangs, because the
   * alarm above turns such a hang into WIFSIGNALED, and it also catches a
   * real crash. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* This test covers a separate interaction between the self-call guard above
 * and the fork-safety machinery elsewhere in this file. A task can call
 * fork() itself (not the top-level fork() of the test), and the one surviving
 * thread of that child is then an exact continuation of the same worker call
 * frame. pool then carries the mark foreign_since_fork, because it was in_use
 * at the instant of the fork(), and both _ctpool_atfork_prepare and
 * _ctpool_atfork_release walked it, like every other live pool. The
 * ctpool_worker_key_bundle TLS value of this thread points at pool, because
 * it inherited that value through the fork(), which duplicates the whole
 * state of the calling thread, TLS included.
 *
 * A ctpool_destroy(pool) call from there must count as a self-call, and the
 * library must reject it. __ctpool_destroy checks this before it dispatches
 * to the foreign and non-foreign branches of _ctpool_teardown_raw, so the
 * foreign status of the pool never hides a real self-destroy.
 *
 * This test is not vacuous: revert only the self-call check, leaving the
 * foreign-pool machinery alone, and the test becomes a real use-after-free in
 * the child, because the foreign branch of _ctpool_teardown_raw frees the
 * struct of pool while this exact thread is on its way back through the tail
 * code of worker_thread_fn. */
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
    alarm(2); /* bounds this grandchild's own lifetime if this regresses */
    ctpool_destroy(ctx->pool); /* self-destroy of a now-foreign pool, from
        within the exact worker call frame that was executing pre-fork */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  /* Parent side: this is the pool's own genuine worker thread, which nothing
   * that the child does to its own, independent post-fork copy of pool can
   * affect. */
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

  /* Whatever the grandchild did to its own copy of pool does not touch the
   * copy of the parent, because the two copies are independent after the
   * fork, through copy on write. The original task itself,
   * fork_from_task_then_self_destroy, has already returned at this point in
   * the parent, because fork() returns there at once. */
  ctpool_wait(pool);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                              FORK SAFETY                                   */
/* ========================================================================== */

/* The whole rest of this file exercises the fork() safety machinery in
 * src/cthreadpool.c, which is built on pthread_atfork(), and which a build
 * with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles out (see the doc comment
 * of that macro in common.h). The premises of these tests (that a forked
 * child never inherits a locked ctpool mutex, and that a pool survives as
 * "foreign" instead of an unsafe teardown) do not hold without that
 * machinery, so these tests are compiled out with it instead of being left
 * in to fail or to hang. */
#if CCOL_FORK_SAFETY_REQUIRED

typedef struct {
  _Atomic int stop;
} ctp_fork_stop_arg_t;

/* This thread creates and destroys throwaway ctpool instances without a
 * break; they have nothing to do with the pool that the main test thread
 * keeps busy below. Its only purpose is to keep SOME thread inside the mutex
 * of ctpool_slot_table as often as it can, through ccol_create_cthread_pool
 * and ctpool_destroy, which races the repeated fork() calls of this test. It
 * mirrors fork_safety_churn_thread in tests/cthreadcomm/tests.c exactly,
 * adapted to ctpool instead of ccol_event_loop. */
static void *ctp_fork_churn_thread(void *arg) {
  ctp_fork_stop_arg_t *a = (ctp_fork_stop_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ctpool p = ccol_create_cthread_pool(1, 0, &err);
    if (p != CTPOOL_INVALID) ctpool_destroy(p);
  }
  return NULL;
}

typedef struct {
  ctpool pool;
  _Atomic int stop;
} ctp_fork_feeder_arg_t;

/* One background thread that queries, again and again, the busy pool that
 * the main test thread forks against below, which widens the contention
 * window on pool->mu beyond what the submit-burst-then-fork technique of the
 * trial loop gives on its own.
 *
 * The sched_yield() after every call is load-bearing, not a nicety. A bare
 * `while (!stop) ctpool_pending_count(pool);` loop reproduces the hang just
 * as reliably on a native run, but it also iterates fast enough to run many
 * millions of times, even over a short test run. Because the memcheck tool
 * of valgrind gives threads no true parallelism across cores, and
 * time-slices every thread through one single instrumented execution engine,
 * that many iterations of anything, however cheap each one is, make this
 * test extremely slow under valgrind: tens of seconds to several minutes for
 * this one test alone.
 *
 * A yield after every call caps the call rate of this thread at whatever the
 * time-slice granularity of the scheduler lets it reach, which is several
 * orders of magnitude fewer calls for the same window of wall-clock time,
 * and in practice it contends often enough to keep the race reproducible.
 * The comment on the test below records the measured trade-off that this
 * lands on. */
static void *ctp_fork_feeder_thread(void *arg) {
  ctp_fork_feeder_arg_t *a = (ctp_fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    (void)ctpool_pending_count(a->pool);
    sched_yield();
  }
  return NULL;
}

/* The regression test for the fork-safety hang, against which the mutex of
 * ctpool_slot_table in this module, and the mu of every live pool, must be
 * protected (see the doc comment of _ctpool_atfork_prepare in
 * src/cthreadpool.c for the full mechanism). fork() duplicates only the
 * calling thread, while several places take the mu of a pool: ctpool_submit,
 * ctpool_try_submit, a worker thread that picks up or finishes a task, and
 * the shutdown and teardown sequence of ctpool_destroy. Without the
 * protection, a child inherits that mu already locked, with no thread left
 * alive in that child that could unlock it.
 *
 * The same class of hazard has its own guard and regression test for the
 * locks of ccol_event_loop (see fork_does_not_inherit_a_locked_event_loop_mutex
 * in tests/cthreadcomm/tests.c), and this module carries the identical
 * protection on its own. The dispatch_pool of ccol_event_loop is exactly such
 * a ctpool, which a caller creates whenever it configures num_reactor_threads
 * above 1, so it is reachable through this exact mechanism, and
 * ccol_event_loop has no way to reach into the opaque internals of this
 * module and protect it from outside.
 *
 * A churn thread creates and destroys throwaway ctpool instances without a
 * break, racing the mutex of ctpool_slot_table in the background; it mirrors
 * the churn thread of fork_does_not_inherit_a_locked_event_loop_mutex
 * exactly. That churn thread is cheap under valgrind on its own: the test
 * that exists does the same real create and destroy work for the same
 * duration and runs there in under a second.
 *
 * One feeder thread, throttled with a yield, separately races the mu of the
 * busy pool in the background (see the comment on ctp_fork_feeder_thread).
 * Each trial then also bursts several real task submissions to that same
 * pool right before it forks, which times the fork() to land while some
 * worker thread may be in the middle of a dequeue, racing to take pool->mu
 * after one of the wakeup signals of the burst.
 *
 * The burst holds several submissions, not one, because the pool->mu window
 * of a single submit is too narrow to overlap a fork() right after it
 * reliably: one submit for each trial, with no feeder thread at all,
 * reproduces zero hangs across 300 trials.
 *
 * A feeder thread of a different shape, or several of them, with no burst
 * at all, does not work either. That covers one or more threads that call
 * the real, allocating ctpool_try_submit without a break, and one or more
 * threads that call ctpool_pending_count or ctpool_active_count, which
 * allocate nothing, without a sched_yield() between iterations. Every one of
 * those reproduces the hang reliably in well under a second on a native run,
 * but turns extremely slow under valgrind: tens of seconds to several minutes
 * for this one test alone, even with the protection in place and zero hangs
 * to report.
 *
 * Direct experiment, not an assumption, confirmed the reason: the memcheck
 * tool of valgrind gives threads no true parallelism across cores at all,
 * and time-slices all of them through its own single instrumented execution
 * engine. A loop with no throttle iterates as fast as the CPU lets it, for
 * the full duration of the test, and that work does not divide across cores
 * the way it would on a native run; it only hands valgrind an enormously
 * larger total of instrumented instructions to simulate, for the same window
 * of wall-clock time.
 *
 * The current design bounds that cost in two ways. The one feeder thread
 * yields after every single lock and unlock, so its achievable call rate is
 * whatever the time-slice granularity of the scheduler lets it reach, not
 * the raw instruction rate of the CPU. And the burst of the trial loop is a
 * small, fixed-size sequence of blocking calls for each trial that spins on
 * nothing, so that half of the contention scales with TRIALS instead of with
 * wall-clock duration, and pays no such multiplier either. */
TEST(fork_safety, fork_does_not_inherit_a_locked_ctpool_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  ctp_fork_stop_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  bool started_churn_tid =
      (pthread_create(&churn_tid, NULL, ctp_fork_churn_thread, &churn) == 0);

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(3, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  atomic_int counter = 0;

  ctp_fork_feeder_arg_t feeder = {.pool = pool, .stop = 0};
  pthread_t feeder_tid;
  bool started_feeder_tid =
      (pthread_create(&feeder_tid, NULL, ctp_fork_feeder_thread, &feeder) == 0);

  /* There are 60 trials here, not hundreds. With the protection turned off,
   * the measured hang rate of this configuration is roughly 10 to 20 percent
   * of trials (to measure it, empty the bodies of _ctpool_atfork_prepare and
   * _ctpool_atfork_release, keep both registered, and run this exact test
   * dozens of times). At that rate, the odds that a full run sees zero hangs
   * by chance alone are already well under 1 percent, while hundreds of
   * trials would add the cost of that many fork() and waitpid() pairs to
   * every ordinary test run that finds no hang. */
  enum { TRIALS = 60 };
  /* There are several submissions right before each fork(), not one, because
   * the pool->mu window of a single submit is too narrow to overlap a fork()
   * right after it reliably: one submit for each trial alone reproduces zero
   * hangs across 300 trials. A burst of several gives several independent
   * workers a dequeue race that they start at nearly the same instant, which
   * multiplies the odds that at least one of them is inside pool->mu at the
   * exact instant of the fork(), with no background thread that spins for
   * ever (the comment on this test above says why such a thread is too slow
   * under valgrind). */
  enum { BURST = 8 };
  int hangs = 0;
  _diag_warm_up_backtrace();
  for (int i = 0; i < TRIALS; i++) {
    for (int b = 0; b < BURST; b++) {
      REQUIRE_EQ(ctpool_try_submit(pool, inc_counter, &counter, NULL),
                 ccol_success);
    }

    int diagfd[2];
    REQUIRE_EQ(pipe(diagfd), 0);

    pid_t pid = fork();
    REQUIRE_NE(pid, -1);
    if (pid == 0) {
      close(diagfd[0]);
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      /* This bounds the lifetime of the child in case the hazard that this
       * test guards against fires, so that it fails instead of hanging the
       * whole suite; the parent below tells this apart from a clean exit with
       * WIFEXITED.
       *
       * The bound is 5 seconds, not 1. A real deadlock on an inherited locked
       * mutex hangs for ever, whatever bound the test picks, so the larger
       * bound buys headroom against ordinary scheduling delay after a fork: a
       * busy, oversubscribed or virtualized CI host can leave a freshly forked
       * child unscheduled for more than a second with nothing wrong. The
       * larger bound weakens nothing that this test catches.
       *
       * _diag_arm installs a diagnostic SIGALRM handler instead of relying on
       * the default disposition that kills on SIGALRM (see the doc comment of
       * that section), so a real hang reports WHERE the child was stuck, not
       * only THAT it was stuck. */
      _diag_arm(diagfd[1]);

      /* The exact call shape that a real application uses right after it
       * inherits a pool across a fork (ctpool_submit or ctpool_try_submit,
       * then submit_internal, then ccol_mutex_lock(pool->mu)), and therefore
       * what this test must prove is safe. */
      atomic_int local_counter = 0;
      ctpool_try_submit(pool, inc_counter, &local_counter, NULL);
      _exit(0); /* reached only if the call above returned at all */
    }
    close(diagfd[1]);

    int status = 0;
    REQUIRE_EQ(waitpid(pid, &status, 0), pid);
    bool diag_fired = WIFEXITED(status) && WEXITSTATUS(status) == 66;
    if (!WIFEXITED(status) || diag_fired) {
      hangs++;
      fprintf(stderr,
              "trial %d: hang detected (WIFEXITED=%d WEXITSTATUS=%d "
              "WIFSIGNALED=%d WTERMSIG=%d)\n",
              i, WIFEXITED(status),
              WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status),
              WIFSIGNALED(status) ? WTERMSIG(status) : -1);
      if (diag_fired) _diag_report(diagfd[0]);
    }
    close(diagfd[0]);
  }

  REQUIRE_EQ(hangs, 0);

  atomic_store(&feeder.stop, 1);
  if (started_feeder_tid) pthread_join(feeder_tid, NULL);
  atomic_store(&churn.stop, 1);
  if (started_churn_tid) pthread_join(churn_tid, NULL);
  REQUIRE_TRUE(started_churn_tid);
  REQUIRE_TRUE(started_feeder_tid);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

typedef struct {
  _Atomic bool locked;
  int hold_ms;
} ctpool_slot_table_fork_lock_arg_t;

static void *ctpool_slot_table_fork_lock_thread(void *arg) {
  ctpool_slot_table_fork_lock_arg_t *a =
      (ctpool_slot_table_fork_lock_arg_t *)arg;
  ctpool_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  /* This thread releases on a fixed schedule of its own, independent of
   * everything that the thread which forks does below (see
   * queue_fork_lock_thread in tests/cthreadcomm/tests.c for the same
   * reasoning). The thread that forks must never be the one that signals
   * this thread to let go, because the two would then wait on each other in
   * a real cycle. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  ctpool_test_wrunlock_slot_table_for_tests();
  return NULL;
}

/* The regression test for the write-lock hazard of
 * ctpool_slot_table.rwlock, which glibc tracks by thread ID (TID), as the
 * comment on the in_child branch of _ctpool_atfork_release_impl describes.
 *
 * The lock of ctpool_slot_table is a ccol_rw_lock_t, which keeps concurrent
 * _ctpool_resolve calls (the hottest path of this module under a caller that
 * submits a task for each request, such as chttpserver) off one shared lock.
 *
 * Any thread that calls ccol_create_cthread_pool_mp or __ctpool_destroy can
 * take the write side of that lock, not only the thread that later calls
 * fork(). Because the write lock of a glibc rwlock tracks its owner by TID,
 * a plain ccol_rw_lock_unlock from the surviving thread of the child, whose
 * TID differs, would fail silently: it would not release a lock that a
 * different thread took, which is gone, and every later _ctpool_resolve in
 * that child would then hang.
 *
 * This mirrors fork_does_not_inherit_a_write_locked_reg_slot_rwlock of
 * ccol_event_loop, in tests/cthreadcomm/tests.c, exactly, with
 * ctpool_slot_table.rwlock in place of the reg_slot_rwlock of
 * ccol_event_loop. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_ctpool_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(2, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  enum { HOLD_MS = 300 };
  ctpool_slot_table_fork_lock_arg_t arg = {.locked = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(
      pthread_create(&holder, NULL, ctpool_slot_table_fork_lock_thread, &arg),
      0);

  while (!atomic_load(&arg.locked)) {
    /* See fork_does_not_inherit_a_locked_ctpool_mutex's own identical
     * spin-wait reasoning for why sched_yield(), not a bare spin, matters
     * under valgrind. */
    sched_yield();
  }

  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` does not exist here, because fork() duplicates only the
     * thread that calls it. This child can exist only after the fork() call
     * of the parent returned, which needs the
     * ccol_rw_lock_wrlock(ctpool_slot_table.rwlock) inside
     * _ctpool_atfork_prepare to have succeeded first; that is, the holder
     * thread, which is gone in this process, must already have released the
     * lock.
     *
     * With a plain ccol_rw_lock_unlock in the child in place of the reinit,
     * this process would inherit ctpool_slot_table.rwlock in a write-locked
     * state that no thread could ever release, and the resolve inside
     * ctpool_try_submit below would hang until alarm(3) kills this
     * child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    atomic_int local_counter = 0;
    ccol_retval_t rv =
        ctpool_try_submit(pool, inc_counter, &local_counter, NULL);
    char byte = (rv == ccol_success) ? 1 : 0;
    ssize_t written = write(result_pipe[1], &byte, 1);
    (void)written;
    close(result_pipe[1]);
    _exit(0);
  }
  close(result_pipe[1]);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000LL;
  /* This proves that the atfork prepare handler really blocks: fork() must
   * have waited for close to the HOLD_MS of the holder before it returned,
   * instead of returning almost at once while the holder held the lock. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));

  /* This read is short and bounded: if the child hangs past alarm(3) and is
   * killed before it writes anything, its copy of the write end closes with
   * it, so this read returns 0 (end of file) instead of blocking for ever,
   * since the parent already closed its own copy of the write end above. */
  char byte = 0;
  ssize_t n = read(result_pipe[0], &byte, 1);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFEXITED(status));

  pthread_join(holder, NULL);

  /* The ctpool_slot_table.rwlock of the parent must be usable after
   * everything above. The release path of the parent is a plain
   * ccol_rw_lock_unlock, not the reinit that the child runs, and since the
   * fork() call of this same thread validly released that lock, that unlock
   * must work. */
  atomic_int counter_after = 0;
  REQUIRE_EQ(ctpool_try_submit(pool, inc_counter, &counter_after, NULL),
             ccol_success);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* The regression coverage for the foreign_since_fork branch of
 * _ctpool_teardown_raw (see the comment there in src/cthreadpool.c for the
 * full account).
 *
 * That branch must not free pool->threads and the pool struct itself while
 * leaving pool->head and pool->tail untouched, because that silently leaks
 * every ctpool_task that is in the queue, and for a FUTURE task in the queue
 * it also never calls future_cancel, so a caller in the child that holds the
 * pointer to that future and calls ctpool_future_get() on it blocks for
 * ever, waiting for a worker that will never exist in this process.
 *
 * The branch therefore discards the queue before it frees anything, and
 * cancels any future attached to it, exactly as ctpool_shutdown_immediate
 * does for a live pool.
 *
 * This is also the exact call shape that the doc comment of
 * __ctpool_destroy promises to support, where a drain shutdown runs first
 * when neither ctpool_shutdown_drain nor ctpool_shutdown_immediate ran
 * beforehand: here the test destroys a foreign pool directly, with no
 * shutdown call before it. */
TEST(
    fork_safety,
    destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(1, 1, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &dummy);
  REQUIRE_NE((void *)f, NULL); /* queued behind the blocked worker */

  /* The child reports its outcome through a pipe instead of its own process
   * exit code, because under make memtest, valgrind replaces the real exit
   * code of a forked child with its own --error-exitcode the instant it finds
   * ANY still reachable allocation in the inherited image of that child at
   * exit, and every child forked in the middle of a suite has one, since the
   * rest of this suite has not settled yet. So the exit code cannot carry
   * this result reliably.
   *
   * See the fork_safety group of tests/clogger/tests.c, where that file
   * establishes the same reasoning, and the
   * fork_does_not_inherit_a_locked_ctpool_mutex test above in this file,
   * which checks only WIFEXITED for the same reason. */
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
    /* Bounds this child's own lifetime in case the hazard that this test
     * guards against somehow fires, rather than hanging the whole suite. */
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

  /* Release the worker of the parent and tear down the live pool of the
   * parent, both before any assertion below; without that, a real regression
   * here would also leak a worker thread that blocks for ever into the rest
   * of the suite. Whatever the child did leaves the copies of pool and f that
   * the parent holds untouched, because the child works on its own
   * independent copy, which copy on write made. */
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

/* ctpool_wait needs a foreign_since_fork guard of its own, just as
 * _ctpool_shutdown_drain_internal and _ctpool_shutdown_immediate_internal each
 * have one, with which they skip the join of the worker threads of a foreign
 * pool, which do not exist.
 *
 * Without that guard, a ctpool_wait in a forked child, on a pool that the
 * child inherited, whose active_count or queue_size was not zero at the
 * instant of the fork(), hangs for ever, because no worker thread exists in
 * this process to decrement active_count, to drain queue_size, or to
 * broadcast idle_cv again. This is why the library treats a foreign pool as
 * idle by default. */
TEST(fork_safety, wait_on_foreign_pool_with_pending_work_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = ccol_create_cthread_pool(1, 0, &err);
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
    alarm(2);          /* This bounds the life of the child on a regression */
    ctpool_wait(pool); /* This must return at once, and not hang for ever */
    _exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  /* Release the worker of the parent and tear down the live pool of the
   * parent, both before the assertions below; without that, a real
   * regression here would also leak a worker thread that blocks for ever
   * into the rest of the suite. The worker of the parent is blocked at this
   * point, and the child did not touch it. */
  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  int final_counter = atomic_load(&counter);
  ctpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  /* This check does not use WEXITSTATUS, because under make memtest,
   * valgrind replaces the real exit code of a forked child with its own
   * --error-exitcode the instant it finds any still reachable allocation in
   * the inherited image of that child at exit, and every child forked in the
   * middle of a suite has one.
   *
   * WIFEXITED alone is therefore the only part of the exit status that this
   * test can trust. It catches a real regression that hangs, because the
   * alarm above turns that into WIFSIGNALED, and it also catches a real
   * crash.
   *
   * See the fork_safety group of tests/clogger/tests.c, where that file
   * establishes the same reasoning, and the
   * fork_does_not_inherit_a_locked_ctpool_mutex test above in this file,
   * which checks only WIFEXITED for the same reason. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(final_counter, 1);
}

#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/* Allocation-failure sweep over pool construction                            */
/*                                                                            */
/* ccol_create_cthread_pool_mp builds four things before it returns: a */
/* slot entry, a queue, a free list and a worker array, and each failure */
/* point unwinds a different amount of that work. The g_oom_enabled */
/* allocator above fails every allocation at once, so it reaches */
/* only the first of those branches, while a sweep that fails the Nth */
/* allocation in turn walks the rest. */
/*                                                                            */
/* The counter spans all four procs on purpose: the pool struct and the */
/* worker array both come from calloc, so an injector that failed only */
/* malloc could not reach the code that unwinds them. */
/* ========================================================================== */

static atomic_int g_ctp_alloc_seen = 0;
static atomic_int g_ctp_fail_at = 0; /* 0 disarms */

static bool _ctp_should_fail(void) {
  int at = atomic_load(&g_ctp_fail_at);
  if (at == 0) return false;
  return (atomic_fetch_add(&g_ctp_alloc_seen, 1) + 1) == at;
}
static void *_ctp_sweep_malloc(size_t n) {
  return _ctp_should_fail() ? NULL : malloc(n);
}
static void _ctp_sweep_free(void *p) { free(p); }
static void *_ctp_sweep_calloc(size_t a, size_t b) {
  return _ctp_should_fail() ? NULL : calloc(a, b);
}
static void *_ctp_sweep_realloc(void *p, size_t n) {
  return _ctp_should_fail() ? NULL : realloc(p, n);
}
static ccol_memmgmt_procs_t g_ctp_sweep_mp = {
    _ctp_sweep_malloc, _ctp_sweep_free, _ctp_sweep_calloc, _ctp_sweep_realloc};

static void _ctp_arm(int nth) {
  atomic_store(&g_ctp_alloc_seen, 0);
  atomic_store(&g_ctp_fail_at, nth);
}
static void _ctp_disarm(void) { atomic_store(&g_ctp_fail_at, 0); }

/* Deep enough to walk past the last allocation construction makes. */
#define CTP_SWEEP_DEPTH 24

TEST(ctpool_oom, bounded_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= CTP_SWEEP_DEPTH; n++) {
    _ctp_arm(n);
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool_mp(2, 8, &g_ctp_sweep_mp, &err);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID) {
      /* Construction got past the failed allocation, so the pool must be a
       * really working one rather than a half-built handle. */
      if (ctpool_submit(pool, NULL, NULL, NULL) == ccol_success)
        all_handled = false;
      ctpool_destroy(pool);
    } else if (!err) {
      all_handled = false;
    }
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctpool_oom, unbounded_pool_construction_unwinds_at_every_allocation) {
  /* A zero capacity takes the unbounded-queue branch, which allocates a
   * different shape from the bounded one above. */
  bool all_handled = true;
  for (int n = 1; n <= CTP_SWEEP_DEPTH; n++) {
    _ctp_arm(n);
    char *err = NULL;
    ctpool pool = ccol_create_cthread_pool_mp(1, 0, &g_ctp_sweep_mp, &err);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID)
      ctpool_destroy(pool);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctpool_oom, many_worker_pool_construction_unwinds_at_every_allocation) {
  /* More workers mean more per-thread allocation, which reaches failure
   * points that the two-thread sweeps above do not get to. */
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _ctp_arm(n);
    ctpool pool = ccol_create_cthread_pool_mp(8, 16, &g_ctp_sweep_mp, NULL);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID) ctpool_destroy(pool);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctpool_oom, a_pool_built_under_a_late_failure_still_runs_work) {
  /* Whatever survives the sweep must be a usable pool, not merely a non-NULL
   * handle: this is what separates "unwound correctly" from "returned a
   * corpse". */
  bool built = false, ran = false;
  for (int n = 30; n <= 60 && !built; n++) {
    _ctp_arm(n);
    ctpool pool = ccol_create_cthread_pool_mp(2, 4, &g_ctp_sweep_mp, NULL);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID) {
      built = true;
      ctpool_wait(pool); /* returns cleanly on a genuinely working pool */
      ran = true;
      ctpool_destroy(pool);
    }
  }
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(ran);
}

/* ========================================================================== */
/*                CONCURRENT SHUTDOWN CALLS ON ONE POOL                       */
/* ========================================================================== */

extern void _ctpool_force_next_free_index_push_failure_for_tests(void);

/* Sleeps for ms milliseconds, which paces the bounded poll loops and, in one
 * place below, also gives an implementation that is wrong the time to
 * return. No property under test depends on it as a mechanism. */
static void ctp_sd_sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

typedef struct ctp_sd_fixture {
  _Atomic int gate; /* only the main test thread ever sets this */
  _Atomic int t0_running;
  _Atomic int t0_ran;
  _Atomic int extra_ran;
} ctp_sd_fixture;

static ctp_sd_fixture g_ctp_sd;

/* This task occupies the one worker of the pool until the main test thread
 * opens the gate. The bound of 60 real seconds is only a safety net against
 * a hang, never the mechanism for correctness.
 *
 * Every test below asserts that no shutdown call can have returned while the
 * gate is shut, which follows from this worker being here, not from any
 * elapsed time. */
static void ctp_sd_blocking_task(void *arg) {
  (void)arg;
  atomic_store(&g_ctp_sd.t0_running, 1);
  for (int i = 0; i < 60000 && atomic_load(&g_ctp_sd.gate) == 0; i++)
    ctp_sd_sleep_ms(1);
  atomic_fetch_add(&g_ctp_sd.t0_ran, 1);
}

static void ctp_sd_extra_task(void *arg) {
  (void)arg;
  atomic_fetch_add(&g_ctp_sd.extra_ran, 1);
}

static void *ctp_sd_extra_future_task(void *arg) {
  (void)arg;
  atomic_fetch_add(&g_ctp_sd.extra_ran, 1);
  return NULL;
}

typedef struct ctp_sd_caller {
  ctpool pool;
  bool immediate;
  _Atomic int entered;
  _Atomic int returned;
} ctp_sd_caller;

static void *ctp_sd_caller_thread(void *arg) {
  ctp_sd_caller *c = (ctp_sd_caller *)arg;
  atomic_store(&c->entered, 1);
  if (c->immediate)
    ctpool_shutdown_immediate(c->pool);
  else
    ctpool_shutdown_drain(c->pool);
  atomic_store(&c->returned, 1);
  return NULL;
}

static void ctp_sd_fixture_reset(void) {
  atomic_store(&g_ctp_sd.gate, 0);
  atomic_store(&g_ctp_sd.t0_running, 0);
  atomic_store(&g_ctp_sd.t0_ran, 0);
  atomic_store(&g_ctp_sd.extra_ran, 0);
}

/* Waits, bounded, for *flag to become 1. */
static bool ctp_sd_wait_flag(_Atomic int *flag, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited++) {
    if (atomic_load(flag) == 1) return true;
    ctp_sd_sleep_ms(1);
  }
  return atomic_load(flag) == 1;
}

TEST(ctpool_shutdown, a_second_concurrent_shutdown_blocks_until_workers_exit) {
  /* Both shutdown entry points document that they block until the pool
   * really stops. The library sets the flag that says "a shutdown started"
   * before it wakes any worker, and long before it joins one, so a call that
   * sees that flag and returns hands application code a pool whose workers
   * all run, and a caller that then frees the state that its tasks use has a
   * use-after-free in its own memory, with no return value to warn it.
   *
   * The proof here is structural, not a margin of time. The one worker of
   * the pool is inside a task whose only exit condition is a gate that this
   * test has not yet opened, so no shutdown call on this pool can have
   * joined that worker, and none of them may have returned. The pause below
   * only gives an implementation that returns early the time to do so; a
   * longer pause can never make a correct implementation fail.
   *
   * This test is not vacuous: make either entry point return as soon as it
   * finds a shutdown that already started, and the test fails. */
  ctp_sd_fixture_reset();
  ctpool pool = ccol_create_cthread_pool(1, 0, NULL);
  bool pool_ok = (pool != CTPOOL_INVALID);
  bool submitted = false, running = false;
  if (pool_ok) {
    submitted =
        (ctpool_submit(pool, ctp_sd_blocking_task, NULL, NULL) == ccol_success);
    running = submitted && ctp_sd_wait_flag(&g_ctp_sd.t0_running, 5000);
  }

  ctp_sd_caller a = {.pool = pool, .immediate = false};
  ctp_sd_caller b = {.pool = pool, .immediate = false};
  atomic_store(&a.entered, 0);
  atomic_store(&a.returned, 0);
  atomic_store(&b.entered, 0);
  atomic_store(&b.returned, 0);

  pthread_t th[2];
  int started = 0;
  if (running) {
    if (pthread_create(&th[0], NULL, ctp_sd_caller_thread, &a) == 0) started++;
    if (pthread_create(&th[1], NULL, ctp_sd_caller_thread, &b) == 0) started++;
  }

  bool both_entered = (started == 2) && ctp_sd_wait_flag(&a.entered, 5000) &&
                      ctp_sd_wait_flag(&b.entered, 5000);
  if (both_entered) ctp_sd_sleep_ms(300);
  int a_returned_early = atomic_load(&a.returned);
  int b_returned_early = atomic_load(&b.returned);

  /* Released unconditionally and before any assertion, so that every thread
   * that actually started can finish and be joined even if something above
   * went wrong. */
  atomic_store(&g_ctp_sd.gate, 1);
  int join_failures = 0;
  for (int i = 0; i < started; i++)
    if (pthread_join(th[i], NULL) != 0) join_failures++;

  int a_returned = atomic_load(&a.returned);
  int b_returned = atomic_load(&b.returned);
  int t0_ran = atomic_load(&g_ctp_sd.t0_ran);
  if (pool_ok) ctpool_destroy(pool);

  REQUIRE_TRUE(pool_ok);
  REQUIRE_TRUE(submitted);
  REQUIRE_TRUE(running);
  REQUIRE_EQ(started, 2);
  REQUIRE_TRUE(both_entered);
  REQUIRE_EQ(join_failures, 0);
  REQUIRE_EQ(a_returned_early, 0);
  REQUIRE_EQ(b_returned_early, 0);
  REQUIRE_EQ(a_returned, 1);
  REQUIRE_EQ(b_returned, 1);
  REQUIRE_EQ(t0_ran, 1);
}

TEST(ctpool_shutdown, immediate_escalates_a_drain_already_in_progress) {
  /* A request for an immediate shutdown while a drain runs must discard what
   * the queue holds and cancel the futures of those tasks. Delivering the
   * behavior of a drain instead is silent: both entry points return void, so
   * a caller that asked for the queue to be thrown away has no way to learn
   * that the library ran it instead.
   *
   * The drain is made to arrive first by waiting for it to take ownership,
   * read directly off the pool, before the immediate call is started.
   *
   * This test is non-vacuous: making the immediate entry point return as
   * soon as it finds a shutdown already started makes it fail. */
  enum { EXTRA_TASKS = 4 };
  ctp_sd_fixture_reset();
  ctpool pool = ccol_create_cthread_pool(1, 0, NULL);
  bool pool_ok = (pool != CTPOOL_INVALID);
  struct cthread_pool *raw = pool_ok ? _ctpool_resolve_for_tests(pool) : NULL;

  bool submitted = false, running = false;
  if (pool_ok) {
    submitted =
        (ctpool_submit(pool, ctp_sd_blocking_task, NULL, NULL) == ccol_success);
    running = submitted && ctp_sd_wait_flag(&g_ctp_sd.t0_running, 5000);
  }

  ctpool_future *futures[EXTRA_TASKS] = {NULL};
  bool all_queued = running;
  for (int i = 0; i < EXTRA_TASKS && all_queued; i++) {
    futures[i] = ctpool_submit_future(pool, ctp_sd_extra_future_task, NULL);
    if (!futures[i]) all_queued = false;
  }

  ctp_sd_caller drain = {.pool = pool, .immediate = false};
  ctp_sd_caller immediate = {.pool = pool, .immediate = true};
  atomic_store(&drain.entered, 0);
  atomic_store(&drain.returned, 0);
  atomic_store(&immediate.entered, 0);
  atomic_store(&immediate.returned, 0);

  pthread_t th[2];
  int started = 0;
  bool drain_owns = false;
  if (all_queued && raw) {
    if (pthread_create(&th[0], NULL, ctp_sd_caller_thread, &drain) == 0) {
      started++;
      for (int i = 0; i < 5000 && !drain_owns; i++) {
        bool shutdown_started = false;
        _ctpool_shutdown_state_for_tests(raw, &shutdown_started, NULL, NULL);
        if (shutdown_started) {
          drain_owns = true;
          break;
        }
        ctp_sd_sleep_ms(1);
      }
    }
  }
  if (drain_owns) {
    if (pthread_create(&th[1], NULL, ctp_sd_caller_thread, &immediate) == 0)
      started++;
  }

  /* The queue empties while the first task holds the one worker of the pool,
   * which proves that the escalation happened, because nothing else can
   * consume those tasks yet. */
  bool queue_discarded = false;
  if (started == 2) {
    for (int i = 0; i < 5000 && !queue_discarded; i++) {
      if (ctpool_pending_count(pool) == 0) {
        queue_discarded = true;
        break;
      }
      ctp_sd_sleep_ms(1);
    }
  }

  atomic_store(&g_ctp_sd.gate, 1);
  int join_failures = 0;
  for (int i = 0; i < started; i++)
    if (pthread_join(th[i], NULL) != 0) join_failures++;

  int cancelled = 0;
  for (int i = 0; i < EXTRA_TASKS; i++) {
    if (futures[i]) {
      if (ctpool_future_cancelled(futures[i])) cancelled++;
      ctpool_future_free(futures[i]);
    }
  }
  int extra_ran = atomic_load(&g_ctp_sd.extra_ran);
  int t0_ran = atomic_load(&g_ctp_sd.t0_ran);
  if (pool_ok) ctpool_destroy(pool);

  REQUIRE_TRUE(pool_ok);
  REQUIRE_TRUE(submitted);
  REQUIRE_TRUE(running);
  REQUIRE_TRUE(all_queued);
  REQUIRE_TRUE(drain_owns);
  REQUIRE_EQ(started, 2);
  REQUIRE_EQ(join_failures, 0);
  REQUIRE_TRUE(queue_discarded);
  REQUIRE_EQ(cancelled, (int)EXTRA_TASKS);
  REQUIRE_EQ(extra_ran, 0);
  REQUIRE_EQ(t0_ran, 1);
}

TEST(ctpool_handle_lifecycle, a_free_list_push_failure_does_not_strand_a_slot) {
  /* The slot index of a pool that a destroy took down goes back onto the
   * free list of the handle table. When that push cannot allocate, the index
   * names a slot that is fully released and that nothing references, and
   * without recovery that slot is unusable for the rest of the process, so
   * every later create grows the table by one more.
   *
   * This test runs enough cycles that the free list it starts with cannot
   * absorb them. With recovery, the table grows by at most one, which comes
   * from the first cycle, and only when the free list was empty and nothing
   * had been lost yet; without recovery, it grows by one for each cycle.
   *
   * This test is not vacuous: drop a failed push on the floor, and the table
   * grows by CYCLES and the test fails. */
  size_t before = _ctpool_slot_table_capacity_for_tests();
  size_t free_before = _ctpool_free_index_count_for_tests();
  size_t cycles = free_before + 8;

  bool all_ok = true;
  for (size_t i = 0; i < cycles && all_ok; i++) {
    ctpool pool = ccol_create_cthread_pool(1, 0, NULL);
    if (pool == CTPOOL_INVALID) {
      all_ok = false;
      break;
    }
    _ctpool_force_next_free_index_push_failure_for_tests();
    ctpool_destroy(pool);
  }

  /* This pool must come from a slot that the recovery reclaimed, and it must
   * be fully usable: a reclaimed index that carried stale records would break
   * on its first call. */
  ctp_sd_fixture_reset();
  ctpool pool = ccol_create_cthread_pool(1, 0, NULL);
  bool pool_ok = (pool != CTPOOL_INVALID);
  bool ran = false;
  if (pool_ok) {
    if (ctpool_submit(pool, ctp_sd_extra_task, NULL, NULL) == ccol_success) {
      ctpool_wait(pool);
      ran = (atomic_load(&g_ctp_sd.extra_ran) == 1);
    }
    ctpool_destroy(pool);
  }

  size_t after = _ctpool_slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(pool_ok);
  REQUIRE_TRUE(ran);
  REQUIRE_LE(after, before + 1);
}

/* ==========================================================================
 * The submit path under contention
 * ========================================================================== */

typedef struct ctp_gate {
  _Atomic bool open;
  _Atomic int entered;
  _Atomic long ran;
} ctp_gate;

static void ctp_gate_nap(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  nanosleep(&ts, NULL);
}

/* A task that parks until its gate opens, with a bound, so that a test that
 * fails early can never leave a worker parked for good. */
static void ctp_gate_task(void *arg) {
  ctp_gate *g = (ctp_gate *)arg;
  atomic_fetch_add(&g->entered, 1);
  for (int waited_ms = 0; !atomic_load(&g->open) && waited_ms < 30000;
       waited_ms++)
    ctp_gate_nap();
  atomic_fetch_add(&g->ran, 1);
}

static void ctp_count_task(void *arg) {
  atomic_fetch_add(&((ctp_gate *)arg)->ran, 1);
}

static bool ctp_gate_wait_entered(ctp_gate *g, int target) {
  for (int waited_ms = 0; atomic_load(&g->entered) < target; waited_ms++) {
    if (waited_ms >= 30000) return false;
    ctp_gate_nap();
  }
  return true;
}

typedef struct ctp_stress_submitter {
  ctpool pool;
  ctp_gate *g;
  int mode; /* 0 blocking, 1 timed with a short budget, 2 try */
  long count;
  _Atomic bool *abort_run;
  long submitted;
} ctp_stress_submitter;

static void *ctp_stress_submitter_thread(void *arg) {
  ctp_stress_submitter *a = (ctp_stress_submitter *)arg;
  uint64_t tiny = 1000;
  while (a->submitted < a->count && !atomic_load(a->abort_run)) {
    ccol_retval_t rv;
    if (a->mode == 0) {
      rv = ctpool_submit(a->pool, ctp_count_task, a->g, NULL);
    } else if (a->mode == 1) {
      rv = ctpool_timed_submit(a->pool, ctp_count_task, a->g, NULL, tiny);
    } else {
      rv = ctpool_try_submit(a->pool, ctp_count_task, a->g, NULL);
    }
    if (rv == ccol_success) {
      a->submitted++;
    } else if (rv == ccol_container_full || rv == ccol_timed_out) {
      sched_yield();
    } else {
      return NULL; /* ccol_not_permitted after an abort */
    }
  }
  return NULL;
}

/* Blocking, timed and try submitters race many workers on a bounded and on
 * an unbounded queue. Every task must run, and no submitter and no worker
 * may stay asleep with work for it. Because a lost wake parks a thread for
 * good, the wait for the full count is bounded, and on a timeout an
 * immediate shutdown wakes every parked thread before the test joins them. */
static bool ctp_signal_stress(size_t queue_cap, long per_submitter) {
  enum { SUBMITTERS = 6 };
  ctp_gate g = {0};
  _Atomic bool abort_run = false;
  ctpool pool = ccol_create_cthread_pool(8, queue_cap, NULL);
  if (pool == CTPOOL_INVALID) return false;
  ctp_stress_submitter subs[SUBMITTERS];
  pthread_t tids[SUBMITTERS];
  bool started[SUBMITTERS];
  for (int i = 0; i < SUBMITTERS; i++) {
    subs[i] = (ctp_stress_submitter){.pool = pool,
                                     .g = &g,
                                     .mode = i % 3,
                                     .count = per_submitter,
                                     .abort_run = &abort_run,
                                     .submitted = 0};
    started[i] = (pthread_create(&tids[i], NULL, ctp_stress_submitter_thread,
                                 &subs[i]) == 0);
  }
  long expected = 0;
  for (int i = 0; i < SUBMITTERS; i++)
    if (started[i]) expected += per_submitter;
  bool done = false;
  for (int waited_ms = 0; waited_ms < 120000; waited_ms++) {
    if (atomic_load(&g.ran) >= expected) {
      done = true;
      break;
    }
    ctp_gate_nap();
  }
  if (!done) {
    atomic_store(&abort_run, true);
    ctpool_shutdown_immediate(pool);
  }
  long submitted = 0;
  for (int i = 0; i < SUBMITTERS; i++) {
    if (!started[i]) continue;
    pthread_join(tids[i], NULL);
    submitted += subs[i].submitted;
  }
  if (done) ctpool_wait(pool);
  ctpool_destroy(pool);
  bool all_started = true;
  for (int i = 0; i < SUBMITTERS; i++) all_started = all_started && started[i];
  return all_started && done && submitted == expected &&
         atomic_load(&g.ran) == expected;
}

TEST(ctpool_submit_cost, no_task_and_no_wake_is_lost_under_contention) {
  bool bounded = ctp_signal_stress(4, 3000);
  bool unbounded = ctp_signal_stress(0, 3000);
  REQUIRE_TRUE(bounded);
  REQUIRE_TRUE(unbounded);
}

/* ==========================================================================
 * The deadline of a timed submit
 * ========================================================================== */

typedef struct ctp_timed_arg {
  ctpool pool;
  ctp_gate *g;
  uint64_t timeout_us;
  _Atomic bool returned;
  ccol_retval_t rv;
} ctp_timed_arg;

static void *ctp_timed_submit_thread(void *arg) {
  ctp_timed_arg *a = (ctp_timed_arg *)arg;
  a->rv =
      ctpool_timed_submit(a->pool, ctp_count_task, a->g, NULL, a->timeout_us);
  atomic_store(&a->returned, true);
  return NULL;
}

/* Runs one timed submit against a full queue of capacity 1, with the one
 * worker parked, and opens the gate after hold_ms. It returns the result of
 * the timed submit and reports whether that submit returned before the gate
 * opened. */
static ccol_retval_t ctp_timed_submit_on_full_queue(uint64_t timeout_us,
                                                    int hold_ms,
                                                    bool *returned_early) {
  *returned_early = false;
  ctp_gate g = {0};
  ctpool pool = ccol_create_cthread_pool(1, 1, NULL);
  if (pool == CTPOOL_INVALID) return ccol_unexpected_failure;
  ccol_retval_t rv = ccol_unexpected_failure;
  bool ready = (ctpool_submit(pool, ctp_gate_task, &g, NULL) == ccol_success) &&
               ctp_gate_wait_entered(&g, 1) &&
               (ctpool_submit(pool, ctp_count_task, &g, NULL) == ccol_success);
  if (ready) {
    ctp_timed_arg a = {.pool = pool,
                       .g = &g,
                       .timeout_us = timeout_us,
                       .returned = false,
                       .rv = ccol_unexpected_failure};
    pthread_t tid;
    if (pthread_create(&tid, NULL, ctp_timed_submit_thread, &a) == 0) {
      for (int waited_ms = 0; waited_ms < hold_ms; waited_ms++) {
        if (atomic_load(&a.returned)) break;
        ctp_gate_nap();
      }
      *returned_early = atomic_load(&a.returned);
      atomic_store(&g.open, true);
      pthread_join(tid, NULL);
      rv = a.rv;
    }
  }
  atomic_store(&g.open, true);
  ctpool_wait(pool);
  ctpool_destroy(pool);
  return rv;
}

/* A timeout that reaches past the largest time_t is a wait for ever, not a
 * deadline that wrapped into the past. This test is non-vacuous: with an
 * unsaturated sum the submit returns ccol_timed_out at once. */
TEST(ctpool_timed_submit, a_timeout_past_the_end_of_time_waits_for_room) {
  bool early = true;
  ccol_retval_t rv = ctp_timed_submit_on_full_queue(UINT64_MAX, 200, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);

  rv = ctp_timed_submit_on_full_queue(UINT64_MAX - 999999, 200, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);
}

/* A budget that is not a whole number of seconds lasts for all of it:
 * 1500000 us is 1.5 seconds, so the gate that opens after 50 ms lets the
 * submit through. A timeout of 0 is the try variant, which reports a full
 * queue at once. */
TEST(ctpool_timed_submit, a_fractional_budget_waits_and_zero_does_not) {
  bool early = true;
  ccol_retval_t rv = ctp_timed_submit_on_full_queue(1500000, 50, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);

  rv = ctp_timed_submit_on_full_queue(0, 10000, &early);
  REQUIRE_TRUE(early);
  REQUIRE_EQ(rv, ccol_container_full);
}

/* ==========================================================================
 * The clock of a timed submit
 * ========================================================================== */

static long long ctp_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

/* Waits on not_full of raw until a deadline 100 ms ahead on CLOCK_MONOTONIC,
 * and returns how long the wait took, or -1 when it did not time out. A
 * condition variable on CLOCK_REALTIME would read that deadline as a moment
 * decades in the past and return at once. */
static long long ctp_not_full_wait_ms(struct cthread_pool *raw) {
  struct timespec abs;
  clock_gettime(CLOCK_MONOTONIC, &abs);
  abs.tv_nsec += 100000000L;
  if (abs.tv_nsec >= 1000000000L) {
    abs.tv_sec++;
    abs.tv_nsec -= 1000000000L;
  }
  long long start = ctp_mono_ms();
  int rc = _ctpool_timedwait_not_full_for_tests(raw, &abs);
  long long elapsed = ctp_mono_ms() - start;
  return rc == ETIMEDOUT ? elapsed : -1;
}

/* not_full measures an absolute deadline on CLOCK_MONOTONIC, so a wall clock
 * that an administrator or NTP steps backwards cannot lengthen a timed
 * submit. This test is non-vacuous: on a condition variable with the default
 * CLOCK_REALTIME the wait returns at once. */
TEST(ctpool_timed_submit, not_full_measures_deadlines_on_clock_monotonic) {
  ctpool pool = ccol_create_cthread_pool(1, 1, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  long long elapsed = ctp_not_full_wait_ms(_ctpool_resolve_for_tests(pool));
  ctpool_destroy(pool);
  REQUIRE_GE(elapsed, 80LL);
}

/* A timed submit on a full queue returns ccol_timed_out when its budget runs
 * out on CLOCK_MONOTONIC. This test is non-vacuous: a deadline computed on
 * CLOCK_REALTIME against a CLOCK_MONOTONIC condition variable lies decades
 * ahead, and the submit then waits until the test opens the gate. */
TEST(ctpool_timed_submit, a_timed_submit_times_out_on_its_budget) {
  bool early = false;
  long long start = ctp_mono_ms();
  ccol_retval_t rv = ctp_timed_submit_on_full_queue(200000, 10000, &early);
  long long elapsed = ctp_mono_ms() - start;
  REQUIRE_TRUE(early);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed, 150LL);
}

#if CCOL_FORK_SAFETY_REQUIRED
/* The child of a fork() initialises not_full of an inherited pool again, and
 * must use the same clock as the ordinary init. The child reports through a
 * pipe instead of its exit status, which valgrind can replace: '1' when the
 * wait timed out on the CLOCK_MONOTONIC deadline, '0' otherwise. */
TEST(ctpool_timed_submit, not_full_keeps_clock_monotonic_in_a_forked_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  ctpool pool = ccol_create_cthread_pool(1, 1, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  int pfd[2];
  bool piped = (pipe(pfd) == 0);
  pid_t pid = piped ? fork() : -1;
  if (pid == 0) {
    char verdict = ctp_not_full_wait_ms(raw) >= 80LL ? '1' : '0';
    ssize_t w;
    do {
      w = write(pfd[1], &verdict, 1);
    } while (w < 0 && errno == EINTR);
    _exit(0);
  }
  char verdict = 'x';
  bool reaped = false;
  if (pid > 0) {
    close(pfd[1]);
    pfd[1] = -1;
    struct pollfd p = {.fd = pfd[0], .events = POLLIN};
    if (poll(&p, 1, 30000) == 1 && read(pfd[0], &verdict, 1) != 1)
      verdict = 'x';
    for (int waited_ms = 0; waited_ms < 30000; waited_ms++) {
      if (waitpid(pid, NULL, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      ctp_gate_nap();
    }
    if (!reaped) {
      kill(pid, SIGKILL);
      waitpid(pid, NULL, 0);
    }
  }
  if (piped) {
    close(pfd[0]);
    if (pfd[1] >= 0) close(pfd[1]);
  }
  ctpool_destroy(pool);
  REQUIRE_TRUE(piped);
  REQUIRE_TRUE(pid > 0);
  REQUIRE_TRUE(reaped);
  REQUIRE_EQ(verdict, '1');
}
#endif

/* ctpool_destroy evaluates its argument once, so a walk backwards over an
 * array with `ctpool_destroy(a[--k])` destroys and clears every pool. This
 * test is non-vacuous: a macro that evaluates its argument twice destroys
 * only every second pool and clears the others without destroying them. */
TEST(ctpool_macros, destroy_evaluates_its_argument_once) {
  ctpool pools[2] = {ccol_create_cthread_pool(1, 4, NULL),
                     ccol_create_cthread_pool(1, 4, NULL)};
  bool created = pools[0] != CTPOOL_INVALID && pools[1] != CTPOOL_INVALID;
  int k = 2;
  while (k > 0) ctpool_destroy(pools[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(pools[0], CTPOOL_INVALID);
  REQUIRE_EQ(pools[1], CTPOOL_INVALID);
}
