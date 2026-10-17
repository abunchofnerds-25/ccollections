#include <common.h>
#include <common_invariants.h>
#include <cthreadpool.h>
#include <dirent.h>
#include <errno.h>
/* musl has no backtrace(); the diagnostic and the probes below then capture
 * no frames. */
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define TEST_HAS_EXECINFO 1
#else
#define TEST_HAS_EXECINFO 0
#endif
#include <fcntl.h>
#include <internal/cdebuglog.h>
#include <internal/test_helpers/cpintable.h>
#include <internal/test_helpers/cthreadpool.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_sanitizer.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

/* ========================================================================== */
/*                    FORK-HANG DIAGNOSTIC CAPTURE                            */
/* ========================================================================== */

/* Every function in the diagnostic block below is called only from inside
 * the "#if CCOL_FORK_SAFETY_REQUIRED" block further down this file. A build
 * with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles that block out and the
 * helpers would have no caller left, and since this file builds with
 * -Werror, -Wunused-function would turn that into a hard build failure for
 * a configuration the library documents as supported. So the guard keeps
 * the helpers and their callers on the same switch. It doesn't suppress
 * anything, a helper that loses its LAST caller in the default build is
 * still an error.
 */
#if CCOL_FORK_SAFETY_REQUIRED
/* The diagnostic aid for fork_does_not_inherit_a_locked_ctpool_mutex below.
 * That test's alarm(5) bound can fire in a forked child, and the default
 * SIGALRM disposition kills the child and only tells us THAT it hung, never
 * WHERE. So a custom handler captures the child's raw call stack and writes
 * it down a pipe to the parent as plain addresses. It doesn't resolve any
 * symbols, because a resolver can call malloc() and dlopen() internally and
 * that would go right back into the kind of lock this diagnostic is looking
 * at, in case that lock is the stuck one.
 *
 * The parent runs in an ordinary context (no signal handler, not hung), so
 * it can resolve the addresses with backtrace_symbols() safely and print
 * them. A real hang then shows the child's stuck call site directly in this
 * test's output, where otherwise we would only get the bare verdict "it did
 * not return".
 *
 * _diag_warm_up_backtrace warms backtrace() up once, and the test below
 * calls it at the top, long before any fork(). glibc's unwinder dlopen()s
 * (and so malloc()s) its internal unwind-info machinery on its OWN first
 * call in the process, and a child can be stuck on exactly the malloc lock
 * this diagnostic tries to catch. If that setup ran for the first time from
 * a signal handler in such a child, the diagnostic would hang the same way
 * itself and report nothing. */
static volatile sig_atomic_t diag_write_fd = -1;

static void _diag_warm_up_backtrace(void) {
#if TEST_HAS_EXECINFO
  void *dummy[4];
  int n = backtrace(dummy, 4);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) {
    backtrace_symbols_fd(dummy, n, devnull);
    close(devnull);
  }
#endif
}

/* This handler is async-signal-safe. backtrace() only reads the stack
 * frames it unwinds into the caller's buffer and allocates nothing, because
 * the warm-up above already forced its one-time lazy setup, and POSIX
 * defines write() as async-signal-safe. The handler resolves no symbols on
 * purpose, for the reason given in the comment at the top of this
 * section. */
static void _diag_alarm_handler(int sig) {
  (void)sig;
  int fd = (int)diag_write_fd;
  if (fd >= 0) {
#if TEST_HAS_EXECINFO
    void *frames[32];
    int n = backtrace(frames, 32);
    ssize_t written = write(fd, frames, (size_t)n * sizeof(frames[0]));
    (void)written;
#endif
  }
  _exit(66); /* A sentinel of its own. It says that the diagnostic capture
                fired and not that a plain unhandled signal killed the child,
                so the parent can tell the two apart and knows a backtrace
                is waiting in the pipe. */
}

/* Arms the diagnostic SIGALRM handler and starts the alarm(5) bound the test
 * runs under. Call it from the child right after fork() and before the
 * operation under test. write_fd is the write end of the pipe this child's
 * handler reports through, and the read end belongs to the caller in the
 * parent, after waitpid. */
static void _diag_arm(int write_fd) {
  diag_write_fd = write_fd;
  struct sigaction sa = {0};
  sa.sa_handler = _diag_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0; /* No SA_RESTART. A syscall this signal interrupts must
                      not be retried, otherwise the handler fires and
                      the stuck call just carries on. */
  sigaction(SIGALRM, &sa, NULL);
  alarm(5);
}

/* Runs in the parent and reads whatever raw addresses _diag_alarm_handler
 * wrote (a plain kill by an unhandled signal or a clean exit leaves the
 * pipe empty, which is fine and gives got <= 0 below). It then resolves the
 * addresses with backtrace_symbols() and prints them, which is safe here
 * because the parent is an ordinary process and isn't stuck. */
static void _diag_report(int read_fd) {
  void *frames[32];
  ssize_t got = read(read_fd, frames, sizeof(frames));
  if (got <= 0) return;
  int n = (int)(got / (ssize_t)sizeof(frames[0]));
  if (n <= 0) return;
#if TEST_HAS_EXECINFO
  char **syms = backtrace_symbols(frames, n);
#else
  char **syms = NULL;
#endif
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
 * wait.concurrent_shutdown_drain_unblocks_wait uses this helper, a pthread
 * start routine and not a pool task, which opens a gate after a short delay
 * so the blocked worker can exit while a drain is running. The result
 * doesn't depend on the delay as a margin, because the drain runs every
 * queued task whenever the gate opens.
 */
static void *release_gate_fn(void *arg) {
  sleep_ms(20);
  atomic_store((atomic_int *)arg, 1);
  return NULL;
}

/*
 * The releaser of the shutdown_immediate tests. It opens the blocked
 * worker's gate only once ctpool_shutdown_immediate has set the pool's
 * immediate flag. The pool sets that flag and takes the whole queue in one
 * critical section, so by the time the flag is visible the queue is gone
 * and the worker can't dequeue a task the shutdown has to discard. The wait
 * is bounded at 10 s, after which the gate opens anyway, so a broken
 * shutdown makes the test fail rather than hang.
 */
typedef struct {
  atomic_int *gate;
  struct cthread_pool *raw;
} immediate_release_ctx_t;

static void *release_gate_after_immediate_fn(void *arg) {
  immediate_release_ctx_t *ctx = (immediate_release_ctx_t *)arg;
  bool immediate = false;
  for (int i = 0; i < 10000 && ctx->raw && !immediate; i++) {
    _ctpool_shutdown_state_for_tests(ctx->raw, NULL, &immediate, NULL);
    if (!immediate) sleep_ms(1);
  }
  atomic_store(ctx->gate, 1);
  return NULL;
}

/* Calls ctpool_wait on the pool in arg and blocks a background thread with
 * it, so the main thread can race ctpool_shutdown_immediate against it. arg
 * is a `ctpool *`, the address of the caller's local handle variable and not
 * the handle value, because ctpool is a uint64_t value handle and a void*
 * can't carry it by value. */
static void *pool_wait_thread(void *arg) {
  ctpool *p = (ctpool *)arg;
  ctpool_wait(*p);
  return NULL;
}

/* Submits n tasks to the pool, each of which increments *counter.
 * load/concurrent_producers uses it to submit from more than one
 * thread. */
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

/* A future task held by a gate. It spins until something opens the gate and
 * then returns result, so the future can't complete before an explicit
 * release. */
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
 * The fixture of the multi-waiter future tests below. The future's task
 * doesn't return until the test sets `release`, so every waiter thread
 * stays parked inside ctpool_future_get for as long as the test needs.
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

/* The waiter thread. It blocks in ctpool_future_get on the shared future and
 * then records whether it got the expected result. */
static void *future_waiter_thread(void *arg) {
  future_waiters_ctx_t *c = (future_waiters_ctx_t *)arg;
  void *result = ctpool_future_get(c->f);
  if (result == c->expected_result) atomic_fetch_add(&c->results_ok, 1);
  return NULL;
}

/*
 * A bounded wait for f's reference count to reach `target`, returning the
 * last value it read. The tests below need the waiter threads' references
 * to exist before they drop every other reference. A waiter takes its
 * reference inside a call that then blocks, so it can't announce anything
 * to the outside, and asking the future instead keeps the tests
 * independent of how long a thread takes to get there.
 *
 * The bound is only a safety net against a hang and never the mechanism. It
 * lets a build that doesn't take the reference reach its assertions and
 * fail, and without it such a build would drop the last reference under
 * threads that are still inside the future.
 */
static int wait_for_future_refcount(ctpool_future *f, int target, int max_ms) {
  int observed = _ctpool_future_refcount_for_tests(f);
  for (int waited = 0; observed != target && waited < max_ms; waited++) {
    sleep_ms(1);
    observed = _ctpool_future_refcount_for_tests(f);
  }
  return observed;
}

/* The no-op task, which ignores its argument, so a test can check
 * on_complete alone, apart from what the task function does to the same
 * arg. */
static void noop_fn(void *arg) { (void)arg; }

/*
 * The fixture of the self-submit tests below, where a task (or its
 * on_complete callback) submits back into the pool it runs on.
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
  ccol_retval_t rv = ccol_unexpected_failure;
  c->first_future = ctpool_submit_future(c->pool, identity_fn, NULL, &rv);
  atomic_store(&c->first_rc, (int)rv);
  rv = ccol_unexpected_failure;
  c->second_future = ctpool_submit_future(c->pool, identity_fn, NULL, &rv);
  atomic_store(&c->second_rc, (int)rv);
}

/* A bounded wait for an atomic flag to become non-zero, returning what it
 * last read. Every wait a test makes on a flag set by another thread is
 * bounded, so a precondition that never comes true fails an assertion
 * instead of hanging the whole binary. */
static int wait_for_flag(atomic_int *flag, int max_ms) {
  int value = atomic_load(flag);
  for (int waited = 0; !value && waited < max_ms; waited++) {
    sleep_ms(1);
    value = atomic_load(flag);
  }
  return value;
}

/* The fixture of the cross-pool control test, where a task on one pool makes
 * a blocking submission into a different pool that is full for a while. */
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

/* This runs on a worker, so it records what it saw instead of asserting,
 * since a Tau assertion from a thread other than the test thread is a data
 * race. 1 means fn had finished and ran was true, 2 means anything else. */
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
 * While g_oom_enabled is non-zero, every allocation through the pool's
 * custom procs returns NULL, which drives the ccol_not_enough_memory paths.
 * Turn it on AFTER the pool exists so the pool itself gets real memory.
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
 * allocates anything at all. */
TEST(construction, ccol_invalid_size_is_refused) {
  char *err = NULL;
  ctpool pool = ctpool_create(2, ccol_invalid_size, &err);
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
  ctpool pool = ctpool_create_mp(2, ccol_invalid_size, &mp, &err);
  bool refused = pool == CTPOOL_INVALID;
  if (!refused) ctpool_destroy(pool);
  REQUIRE_TRUE(refused);
  REQUIRE_EQ(atomic_load(&g_alloc_count), 0);
}

/* The largest capacity below ccol_invalid_size is a valid bounded queue. */
TEST(construction, largest_finite_capacity_is_bounded) {
  char *err = NULL;
  ctpool pool = ctpool_create(1, ccol_invalid_size - 1, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(construction, zero_threads_fails) {
  char *err = NULL;
  ctpool pool = ctpool_create(0, 0, &err);
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
  /* ctpool_construct_scoped should drain and free the pool automatically when
   * the enclosing block exits. The count of completed tasks checks the
   * drain and valgrind checks for leaks. */
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
    pool = ctpool_create(2, 0, &err);
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
  ctpool pool = ctpool_create_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  atomic_int counter = 0;
  atomic_store(&g_oom_enabled, 1);
  ccol_retval_t r = ctpool_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  atomic_store(&g_oom_enabled, 0);

  /* The pool accepts work again once memory is available. */
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
   * A blocking submit parks on the queue's not-full condition until a worker
   * dequeues. When the caller is itself a worker of that pool, it is one of
   * the few threads that could do that dequeue, and waiting there removes it
   * from the set of threads that could satisfy its own wait, so the library
   * has to report a full queue to such a caller. Without that report this
   * test never returns, because the pool's only worker parks for ever on a
   * slot that only it could free.
   *
   * The pool has one worker and a queue capacity of one, so the sequence is
   * fixed. The running task's first submit finds the queue empty and
   * succeeds, and the second one finds the queue at capacity.
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

/* A task that submits twice into its own pool with a timed submit that has
 * no limit, first into an empty queue and then into the full one, and the
 * same with ctpool_timed_submit_future. */
static atomic_int self_timed_counter;

static void self_timed_submit_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  atomic_store(&c->first_rc,
               (int)ctpool_timed_submit(c->pool, inc_counter,
                                        &self_timed_counter, NULL, UINT64_MAX));
  atomic_store(&c->second_rc,
               (int)ctpool_timed_submit(c->pool, inc_counter,
                                        &self_timed_counter, NULL, UINT64_MAX));
}

static void self_timed_future_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  atomic_store(&c->first_rc,
               (int)ctpool_timed_submit_future(c->pool, identity_fn, NULL,
                                               UINT64_MAX, &c->first_future));
  atomic_store(&c->second_rc,
               (int)ctpool_timed_submit_future(c->pool, identity_fn, NULL,
                                               UINT64_MAX, &c->second_future));
}

TEST(timed_submit, a_timed_submit_from_within_a_task_reports_container_full) {
  /*
   * A timed submit from a worker of the same pool into its full queue must
   * not wait either. Only the workers can free a slot, so with UINT64_MAX
   * the wait would never end and the pool would be wedged. Both timed forms
   * report a full queue at once, like the blocking ones.
   *
   * If the timed wait parked, second_rc would stay
   * -1 past the bound below and the stuck worker would keep the pool from
   * draining.
   */
  void (*fns[2])(void *) = {self_timed_submit_task_fn,
                            self_timed_future_task_fn};
  int first_rc[2], second_rc[2];
  ccol_retval_t submitted[2];
  for (int k = 0; k < 2; k++) {
    ctpool_construct(pool, 1, 1);
    self_submit_ctx_t ctx;
    ctx.pool = pool;
    atomic_init(&ctx.first_rc, -1);
    atomic_init(&ctx.second_rc, -1);
    ctx.first_future = NULL;
    ctx.second_future = NULL;
    submitted[k] = ctpool_submit(pool, fns[k], &ctx, NULL);
    /* Bounded poll; the bound only guards against a hang. */
    for (int i = 0; i < 500 && atomic_load(&ctx.second_rc) == -1; i++) {
      struct timespec ts = {0, 10 * 1000 * 1000};
      nanosleep(&ts, NULL);
    }
    first_rc[k] = atomic_load(&ctx.first_rc);
    second_rc[k] = atomic_load(&ctx.second_rc);
    if (second_rc[k] == -1) {
      /* The worker is stuck in its own queue, free it so the pool can go. */
      ctpool_shutdown_immediate(pool);
    }
    ctpool_wait(pool);
    if (ctx.first_future) {
      (void)ctpool_future_get(ctx.first_future);
      ctpool_future_free(ctx.first_future);
    }
    if (ctx.second_future) ctpool_future_free(ctx.second_future);
    ctpool_shutdown_drain(pool);
    ctpool_destroy(pool);
  }
  for (int k = 0; k < 2; k++) {
    REQUIRE_EQ(submitted[k], ccol_success);
    REQUIRE_EQ(first_rc[k], (int)ccol_success);
    REQUIRE_EQ(second_rc[k], (int)ccol_container_full);
  }
}

TEST(submit, a_worker_without_its_thread_mark_is_still_recognised) {
  /* pthread_setspecific can fail (glibc allocates the storage for a key
   * index of 32 or above lazily, per thread). A worker whose mark is missing
   * must still be recognised as a worker of its pool, otherwise the
   * container-full report of the test above becomes a worker that parks for
   * ever on a slot only it could free. The hook skips the mark only for the
   * workers of this one pool.
   *
   * Take out the fallback over the worker IDs and the
   * second submit never returns, second_rc stays -1 past the bound below and
   * the test fails (and the stuck worker also keeps the pool from
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
  /* The worker read the hook when it started, before running the task, so
   * we can clear the hook now, and it must not stay set for the pools of
   * later tests. */
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
 * descriptor. glibc caches the stacks of joined threads and hands them to
 * the next threads that start, and OpenBSD reuses a joined thread's memory
 * at random, so a thread the application starts after the shutdown can
 * carry exactly that ID. The self-call check must never take such a thread
 * for a worker of the pool. A destroy from it is an ordinary destroy and
 * must not end the process the way a destroy from inside one of the pool's
 * tasks does.
 *
 * The child keeps starting threads until one of them carries the joined
 * worker's ID, and only that thread calls ctpool_destroy. A child where no
 * thread reuses the ID reports that, and the test fails instead of passing
 * without having looked at the case. If the scan
 * over the worker IDs also looked at workers that have exited, the destroy
 * would end the child with SIGABRT and no byte would reach the pipe. The
 * child reports through a pipe and not its exit status because valgrind
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
    ctx.pool = ctpool_create(1, 0, &err);
    if (ctx.pool != CTPOOL_INVALID &&
        ctpool_submit(ctx.pool, ctp_record_worker_id, &ctx, NULL) ==
            ccol_success) {
      ctpool_wait(ctx.pool);
      /* Joins the only worker, whose ID then names a free descriptor. */
      ctpool_shutdown_drain(ctx.pool);
      /* glibc hands the ID to the next thread right away, while OpenBSD
       * gives it back at random after 3 to a few hundred starts. */
      for (int i = 0; i < 4096 && atomic_load(&ctx.outcome) == 0; i++) {
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
  /* 'N' means no thread reused the ID, so the case went unexamined. */
  REQUIRE_EQ(got, 'R');
}

TEST(submit, blocking_submit_from_on_complete_reports_container_full) {
  /* An on_complete callback runs on the worker thread that ran its task, so
   * it runs into the same hazard as the task itself; see
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
   * The full-queue report is tied to the caller's pool. A task submitting
   * into a DIFFERENT pool isn't a worker of that pool and nothing about its
   * wait defeats itself, so it must block until a slot frees and the
   * library must not refuse it. When it submits, the target pool is
   * saturated (its only worker is held on a gate and its one queue slot is
   * taken), so only opening that gate lets the submission through.
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
  /* Widens the window in which the submission really is waiting when the
   * gate opens. Nothing depends on it, opening the gate early only makes the
   * submission find a slot sooner. */
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
  /* One worker and a 1-slot queue. We block the worker so the queue fills
   * up, and then try_submit should return ccol_container_full without
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

typedef struct {
  ctpool pool;
  atomic_int *counter;
  ccol_retval_t result;
} timed_submit_arg_t;

static void *timed_submit_thread(void *p) {
  timed_submit_arg_t *a = p;
  a->result =
      ctpool_timed_submit(a->pool, inc_counter, a->counter, NULL, 10000000);
  return NULL;
}

/* A timed wait for room can report ETIMEDOUT after it has consumed a
 * dequeue's signal, and the test hook makes the wait do exactly that. The
 * submit then needs to see the free slot and succeed. Without checking the
 * queue after a failed wait it reports ccol_timed_out next to the free slot,
 * and a blocking submitter waiting for the same slot never gets a signal. */
TEST(timed_submit, a_wait_that_times_out_after_a_wake_takes_the_free_slot) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* the queue is full */

  atomic_store(&_ctpool_timeout_after_wake_waiting_for_tests, false);
  atomic_store(&_ctpool_timeout_after_wake_for_tests, true);
  timed_submit_arg_t arg = {
      .pool = pool, .counter = &counter, .result = ccol_unexpected_failure};
  pthread_t t;
  bool created = pthread_create(&t, NULL, timed_submit_thread, &arg) == 0;
  bool waited = false;
  for (int i = 0; created && i < 10000; i++) {
    if (atomic_load(&_ctpool_timeout_after_wake_waiting_for_tests)) {
      waited = true;
      break;
    }
    sleep_ms(1);
  }
  atomic_store(&gate, 1); /* the worker finishes and dequeues: one signal */
  if (created) pthread_join(t, NULL);
  atomic_store(&_ctpool_timeout_after_wake_for_tests, false);
  ctpool_wait(pool);
  int count = atomic_load(&counter);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(waited);
  REQUIRE_EQ(arg.result, ccol_success);
  REQUIRE_EQ(count, 2);
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
  /* With a timeout of 0 ctpool_timed_submit returns
   * ccol_container_full immediately on a full bounded queue, without blocking
   * and without reporting ccol_timed_out. */
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
  /* A timeout of 0 goes straight to ctpool_try_submit, and an invalid pool
   * and a NULL fn must both report ccol_invalid_args through it, exactly
   * as they do when ctpool_try_submit is called directly. */
  ctpool_construct(pool, 2, 0);

  REQUIRE_EQ(ctpool_timed_submit(CTPOOL_INVALID, inc_counter, NULL, NULL, 0),
             ccol_invalid_args);
  REQUIRE_EQ(ctpool_timed_submit(pool, NULL, NULL, NULL, 0), ccol_invalid_args);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(timed_submit, unbounded_queue_never_blocks_on_capacity) {
  /* For an unbounded queue the timeout doesn't matter, ctpool_timed_submit
   * succeeds without waiting, however short the timeout is. */
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
  /* A timeout of 500000 us is half a second. The queue is bounded to one
   * slot here, and a task that sleeps much longer than that keeps it full,
   * so the call should time out after about 500 ms, not at once and not
   * after a much longer wait. */
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
  /* The bounds around the intended 500 ms are wide, for a loaded machine.
   * The call must not return almost at once and it must not take several
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
  ccol_retval_t rv = ccol_unexpected_failure;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value, &rv);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_success);

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
  ctpool_future *f = ctpool_submit_future(pool, slow_identity, &value, NULL);
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
    futures[i] = ctpool_submit_future(pool, identity_fn, &values[i], NULL);
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
  /* Drop the caller's reference before the future is done. The task runs
   * anyway and the worker frees the future, and valgrind checks that it
   * does. */
  ctpool_construct(pool, 1, 0);

  int value = 7;
  ctpool_future *f = ctpool_submit_future(pool, slow_identity, &value, NULL);
  REQUIRE_NE((void *)f, NULL);

  ctpool_future_free(f); /* caller drops reference; no future_get call */

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

TEST(futures, waiters_keep_the_future_alive_when_every_other_reference_goes) {
  /*
   * This test pins one hazard. A thread blocked in ctpool_future_get sits
   * inside the future's mutex and condition variable, and nothing may
   * destroy the future while that thread is there. The test releases the
   * caller's reference first and the task's reference goes away when the
   * task completes, and apart from the waiters' references those two are
   * every reference a future has. Without one reference per waiting
   * ctpool_future_get call, that completion would destroy the mutex and
   * free the future under eight threads that are still using it.
   *
   * We read the reference count directly so a failure is an assertion and
   * not a hang. A build that doesn't take the waiter reference never reaches
   * the count the test waits for, and then the test holds on to the
   * caller's reference until every waiter has joined, so the run stays safe
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

  ctpool_future *f = ctpool_submit_future(pool, waiters_gated_fn, &ctx, NULL);
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
   * so the task's completion releases the last reference that isn't a
   * waiter's. */
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
   * The documented multi-waiter pattern from end to end. Several threads
   * call ctpool_future_get on one future, the single caller reference is
   * released exactly once in total, and every waiter sees the same result.
   * Each waiter holds its own reference for its whole call, so whichever
   * thread leaves the future last is the one that destroys it.
   */
  enum { WAITERS = 16 };
  int expected_result_storage = 0;
  ctpool_construct(pool, 2, 0);

  future_waiters_ctx_t ctx;
  ctx.f = NULL;
  ctx.expected_result = &expected_result_storage;
  atomic_init(&ctx.release, 0);
  atomic_init(&ctx.results_ok, 0);

  ctpool_future *f = ctpool_submit_future(pool, waiters_gated_fn, &ctx, NULL);
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
  /* The owner races the waiters out of the future, and freeing here is safe
   * exactly because each of them still holds its own reference. */
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
   * A pool's worker can make a blocking future submission into the same
   * pool, and once the queue is full such a call returns NULL, for the
   * reason submit.blocking_submit_from_within_a_task_reports_container_full
   * gives: the calling thread is one of the few threads that could free the
   * slot it would be waiting for.
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
  REQUIRE_EQ(atomic_load(&ctx.first_rc), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&ctx.second_rc), (int)ccol_container_full);
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
  /* One worker and a 1-slot queue. We block the worker so the queue fills,
   * and then try_submit_future returns ccol_container_full without
   * blocking. */
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy, NULL);
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
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy, NULL);
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
  /* The same wait as timed_submit.a_small_timeout_waits_about_that_long but
   * through ctpool_timed_submit_future, which computes its own absolute
   * deadline with the same helper. */
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy, NULL);
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
  ctpool_future *f1 = ctpool_submit_future(pool, identity_fn, &dummy, NULL);
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
  /* The counterpart of the same test of timed_submit, for the zero-timeout
   * delegation path of the future variant. */
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
  /* Unbounded queue, so timed_submit_future should succeed every time, even
   * with a near-zero timeout, since there's no capacity to check. */
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
   * makes sure the task can't complete before the check, so the assertion
   * is deterministic and isn't tied to timing. */
  atomic_int gate = 0;
  int value = 0;
  gated_future_ctx_t ctx = {.gate = &gate, .result = &value};

  ctpool_construct(pool, 1, 0);
  ctpool_future *f = ctpool_submit_future(pool, gated_identity, &ctx, NULL);
  REQUIRE_NE((void *)f, NULL);

  /* The task is held behind the gate, so done can't be true yet. */
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

  /* ctpool_submit_future: NULL pool, NULL fn. rv_out must get its code on
   * these earliest failure paths too, so rv starts as a value this function
   * never stores. */
  ccol_retval_t rv = ccol_unexpected_failure;
  REQUIRE_EQ(
      (void *)ctpool_submit_future(CTPOOL_INVALID, identity_fn, &value, &rv),
      NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  rv = ccol_unexpected_failure;
  REQUIRE_EQ((void *)ctpool_submit_future(pool, NULL, &value, &rv), NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)ctpool_submit_future(pool, NULL, &value, NULL), NULL);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ctpool_try_submit_future and ctpool_timed_submit_future both document that
 * they set out to NULL on any failure, so neither may leave *out untouched on
 * its two earliest failure paths (an invalid or stale pool handle, and a
 * NULL fn). Zeroing *out only on the later failure paths, such as a full
 * queue or running out of memory, isn't enough.
 *
 * futures.invalid_args above can't catch this because it sets f to NULL
 * before every call, so a regression that leaves garbage in *out reads back
 * as NULL by coincidence. This test poisons *out with a distinctive non-NULL
 * sentinel right before each call that must fail on one of those two
 * paths, which makes the failure visible even though the return code is the
 * same either way. */
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

/* After either kind of shutdown, every future submit refuses the pool with
 * ccol_not_permitted, the code ctpool_submit returns for the same condition.
 * ctpool_submit_future reports it through rv_out. */
TEST(futures, every_future_submit_reports_not_permitted_after_shutdown) {
  for (int mode = 0; mode < 2; mode++) {
    for (size_t cap = 0; cap <= 4; cap += 4) {
      ctpool pool = ctpool_create(2, cap, NULL);
      REQUIRE_NE(pool, CTPOOL_INVALID);
      if (mode == 0)
        ctpool_shutdown_drain(pool);
      else
        ctpool_shutdown_immediate(pool);

      int value = 0;
      ccol_retval_t rv = ccol_unexpected_failure;
      ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value, &rv);
      ctpool_future *f2 = (ctpool_future *)(uintptr_t)0xdeadbeefu;
      ccol_retval_t r2 =
          ctpool_try_submit_future(pool, identity_fn, &value, &f2);
      ctpool_future *f3 = (ctpool_future *)(uintptr_t)0xdeadbeefu;
      ccol_retval_t r3 =
          ctpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
      ctpool_destroy(pool);

      REQUIRE_EQ((void *)f, NULL);
      REQUIRE_EQ(rv, ccol_not_permitted);
      REQUIRE_EQ(r2, ccol_not_permitted);
      REQUIRE_EQ((void *)f2, NULL);
      REQUIRE_EQ(r3, ccol_not_permitted);
      REQUIRE_EQ((void *)f3, NULL);
    }
  }
}

TEST(futures, submit_future_returns_null_on_oom) {
  /* With a custom out-of-memory allocator the task allocation fails inside
   * alloc_future_task, and the library then has to clean up the future it
   * has partly built. The function returns NULL and
   * ccol_not_enough_memory.
   *
   * The ctpool_future struct itself comes from plain calloc and not from the
   * pool's custom allocator, so its lifetime is completely independent of
   * the pool. This test doesn't drive a failure of that allocation, since
   * that would mean intercepting the system allocator (for example with
   * LD_PRELOAD), which is outside the scope of these unit tests. */
  ccol_memmgmt_procs_t mp = {.malloc = oom_malloc,
                             .free = oom_free,
                             .calloc = oom_calloc,
                             .realloc = oom_realloc};
  char *err = NULL;
  ctpool pool = ctpool_create_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  int value = 0;

  /* On OOM ctpool_submit_future returns NULL and reports
   * ccol_not_enough_memory through rv_out. */
  atomic_store(&g_oom_enabled, 1);
  ccol_retval_t rv = ccol_unexpected_failure;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value, &rv);
  ctpool_future *f_null_rv =
      ctpool_submit_future(pool, identity_fn, &value, NULL);

  /* On OOM ctpool_try_submit_future and ctpool_timed_submit_future return
   * ccol_not_enough_memory. */
  ctpool_future *f2 = NULL;
  ccol_retval_t r = ctpool_try_submit_future(pool, identity_fn, &value, &f2);
  ctpool_future *f3 = NULL;
  ccol_retval_t r3 =
      ctpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
  atomic_store(&g_oom_enabled, 0);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_null_rv, NULL);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f2, NULL);
  REQUIRE_EQ(r3, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f3, NULL);

  /* Pool must recover once memory is available. */
  f = ctpool_submit_future(pool, identity_fn, &value, NULL);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* What one round of every_future_submit_locks_the_pool_once measured. */
typedef struct {
  bool created;
  size_t free_nodes;
  ccol_retval_t r_try, r_block, r_timed;
  unsigned long locks_try, locks_block, locks_timed;
  bool results_ok;
  ccol_retval_t p_try, p_block, p_timed;
  unsigned long plain_try, plain_block, plain_timed;
  int plain_ran;
} future_lock_count_t;

static future_lock_count_t measure_future_submit_locks(size_t cap) {
  future_lock_count_t m = {.r_try = ccol_unexpected_failure,
                           .r_block = ccol_unexpected_failure,
                           .r_timed = ccol_unexpected_failure,
                           .p_try = ccol_unexpected_failure,
                           .p_block = ccol_unexpected_failure,
                           .p_timed = ccol_unexpected_failure};
  ctpool pool = ctpool_create(1, cap, NULL);
  if (pool == CTPOOL_INVALID) return m;
  m.created = true;
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  int value = 7;

  /* Tasks that sit in the queue at the same time take a node each and give
   * it back to the free list when they complete, so the submits below find
   * a node there. */
  atomic_int warm_gate = 0;
  atomic_int warm_started = 0;
  gate_ctx_t warm_ctx = {.gate = &warm_gate, .started = &warm_started};
  if (ctpool_submit(pool, blocker_fn, &warm_ctx, NULL) == ccol_success) {
    for (int i = 0; i < 10000 && !atomic_load(&warm_started); i++) sleep_ms(1);
  }
  atomic_int warm_counter = 0;
  for (int i = 0; i < 6; i++)
    ctpool_submit(pool, inc_counter, &warm_counter, NULL);
  atomic_store(&warm_gate, 1);
  ctpool_wait(pool);
  for (int i = 0; i < 5000 && _ctpool_task_free_list_size_for_tests(raw) < 7;
       i++)
    sleep_ms(1);

  /* The only worker waits on the gate, so no worker takes the lock while
   * the submits run and every submit below finds room. */
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  if (ctpool_submit(pool, blocker_fn, &ctx, NULL) == ccol_success) {
    for (int i = 0; i < 10000 && !atomic_load(&started); i++) sleep_ms(1);
  }
  m.free_nodes = _ctpool_task_free_list_size_for_tests(raw);

  ctpool_future *f1 = NULL, *f2 = NULL, *f3 = NULL;
  unsigned long l0 = _ctpool_mu_locks_for_tests();
  m.r_try = ctpool_try_submit_future(pool, identity_fn, &value, &f1);
  unsigned long l1 = _ctpool_mu_locks_for_tests();
  f2 = ctpool_submit_future(pool, identity_fn, &value, &m.r_block);
  unsigned long l2 = _ctpool_mu_locks_for_tests();
  m.r_timed =
      ctpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
  unsigned long l3 = _ctpool_mu_locks_for_tests();
  m.locks_try = l1 - l0;
  m.locks_block = l2 - l1;
  m.locks_timed = l3 - l2;

  atomic_int plain_counter = 0;
  unsigned long p0 = _ctpool_mu_locks_for_tests();
  m.p_try = ctpool_try_submit(pool, inc_counter, &plain_counter, NULL);
  unsigned long p1 = _ctpool_mu_locks_for_tests();
  m.p_block = ctpool_submit(pool, inc_counter, &plain_counter, NULL);
  unsigned long p2 = _ctpool_mu_locks_for_tests();
  m.p_timed =
      ctpool_timed_submit(pool, inc_counter, &plain_counter, NULL, 1000000);
  unsigned long p3 = _ctpool_mu_locks_for_tests();
  m.plain_try = p1 - p0;
  m.plain_block = p2 - p1;
  m.plain_timed = p3 - p2;

  atomic_store(&gate, 1);
  m.results_ok = true;
  ctpool_future *fs[3] = {f1, f2, f3};
  for (int i = 0; i < 3; i++) {
    if (!fs[i]) {
      m.results_ok = false;
      continue;
    }
    if (ctpool_future_get(fs[i]) != (void *)&value) m.results_ok = false;
    ctpool_future_free(fs[i]);
  }
  ctpool_wait(pool);
  m.plain_ran = atomic_load(&plain_counter);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
  return m;
}

/* Every submit, plain or of a future, that finds room and a node on the free
 * list takes the pool's lock once on the calling thread. A future is
 * allocated before the critical section, and popping the node, the checks
 * and the enqueue share that one section. A
 * submit that checked room, took its node and enqueued in separate critical
 * sections would count two or three here. */
TEST(futures, every_future_submit_locks_the_pool_once) {
  future_lock_count_t unbounded = measure_future_submit_locks(0);
  future_lock_count_t bounded = measure_future_submit_locks(8);
  future_lock_count_t all[2] = {unbounded, bounded};
  for (int i = 0; i < 2; i++) {
    REQUIRE_TRUE(all[i].created);
    REQUIRE_GE(all[i].free_nodes, (size_t)6);
    REQUIRE_EQ(all[i].r_try, ccol_success);
    REQUIRE_EQ(all[i].r_block, ccol_success);
    REQUIRE_EQ(all[i].r_timed, ccol_success);
    REQUIRE_EQ(all[i].locks_try, 1ul);
    REQUIRE_EQ(all[i].locks_block, 1ul);
    REQUIRE_EQ(all[i].locks_timed, 1ul);
    REQUIRE_TRUE(all[i].results_ok);
    REQUIRE_EQ(all[i].p_try, ccol_success);
    REQUIRE_EQ(all[i].p_block, ccol_success);
    REQUIRE_EQ(all[i].p_timed, ccol_success);
    REQUIRE_EQ(all[i].plain_try, 1ul);
    REQUIRE_EQ(all[i].plain_block, 1ul);
    REQUIRE_EQ(all[i].plain_timed, 1ul);
    REQUIRE_EQ(all[i].plain_ran, 3);
  }
}

/* A try submit of a future that the pool's state refuses (a full queue or a
 * shutdown) returns at once. It allocates no future, takes no task node
 * and doesn't lock the pool, and the armed allocation failure stays armed,
 * which shows that no future allocation was even attempted. */
TEST(futures, a_refused_future_try_allocates_nothing_and_takes_no_lock) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  ctpool pool = ctpool_create_mp(1, 1, &mp, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  ccol_retval_t r_blocker = ctpool_submit(pool, blocker_fn, &ctx, NULL);
  for (int i = 0; i < 10000 && !atomic_load(&started); i++) sleep_ms(1);
  ccol_retval_t r_fill = ctpool_submit(pool, inc_counter, &counter, NULL);
  int value = 0;

  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, true);
  int allocs0 = atomic_load(&g_alloc_count);
  unsigned long l0 = _ctpool_mu_locks_for_tests();
  ctpool_future *f_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_try);
  ctpool_future *f_zero = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_zero =
      ctpool_timed_submit_future(pool, identity_fn, &value, 0, &f_zero);
  unsigned long full_locks = _ctpool_mu_locks_for_tests() - l0;
  int full_allocs = atomic_load(&g_alloc_count) - allocs0;
  bool armed_after_full =
      atomic_load(&_ctpool_fail_next_future_alloc_for_tests);

  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);

  int allocs1 = atomic_load(&g_alloc_count);
  unsigned long l1 = _ctpool_mu_locks_for_tests();
  ctpool_future *f_sd_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_sd_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_sd_try);
  ctpool_future *f_sd_zero = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_sd_zero =
      ctpool_timed_submit_future(pool, identity_fn, &value, 0, &f_sd_zero);
  unsigned long sd_locks = _ctpool_mu_locks_for_tests() - l1;
  int sd_allocs = atomic_load(&g_alloc_count) - allocs1;
  bool armed_after_shutdown =
      atomic_load(&_ctpool_fail_next_future_alloc_for_tests);
  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, false);
  ctpool_destroy(pool);

  REQUIRE_EQ(r_blocker, ccol_success);
  REQUIRE_EQ(r_fill, ccol_success);
  REQUIRE_EQ(r_try, ccol_container_full);
  REQUIRE_EQ((void *)f_try, NULL);
  REQUIRE_EQ(r_zero, ccol_container_full);
  REQUIRE_EQ((void *)f_zero, NULL);
  REQUIRE_EQ(full_locks, 0ul);
  REQUIRE_EQ(full_allocs, 0);
  REQUIRE_TRUE(armed_after_full);
  REQUIRE_EQ(r_sd_try, ccol_not_permitted);
  REQUIRE_EQ((void *)f_sd_try, NULL);
  REQUIRE_EQ(r_sd_zero, ccol_not_permitted);
  REQUIRE_EQ((void *)f_sd_zero, NULL);
  REQUIRE_EQ(sd_locks, 0ul);
  REQUIRE_EQ(sd_allocs, 0);
  REQUIRE_TRUE(armed_after_shutdown);
  REQUIRE_EQ(atomic_load(&counter), 1);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* When the future's allocation fails, every future submit reports
 * ccol_not_enough_memory with *out NULL before it takes a task node, so the
 * pool's allocator sees no call and no future is left behind. */
TEST(futures, a_failed_future_allocation_takes_nothing) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  ctpool pool = ctpool_create_mp(1, 4, &mp, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  long live_before = atomic_load(&_ctpool_live_futures_for_tests);
  int allocs_before = atomic_load(&g_alloc_count);
  int value = 0;

  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, true);
  ccol_retval_t r_block = ccol_unexpected_failure;
  ctpool_future *f_block =
      ctpool_submit_future(pool, identity_fn, &value, &r_block);
  bool flag_used_block =
      !atomic_load(&_ctpool_fail_next_future_alloc_for_tests);

  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, true);
  ctpool_future *f_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_try);
  bool flag_used_try = !atomic_load(&_ctpool_fail_next_future_alloc_for_tests);

  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, true);
  ctpool_future *f_timed = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_timed =
      ctpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f_timed);
  bool flag_used_timed =
      !atomic_load(&_ctpool_fail_next_future_alloc_for_tests);
  atomic_store(&_ctpool_fail_next_future_alloc_for_tests, false);

  int allocs_after = atomic_load(&g_alloc_count);
  long live_after = atomic_load(&_ctpool_live_futures_for_tests);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_TRUE(flag_used_block);
  REQUIRE_TRUE(flag_used_try);
  REQUIRE_TRUE(flag_used_timed);
  REQUIRE_EQ((void *)f_block, NULL);
  REQUIRE_EQ(r_block, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_try, NULL);
  REQUIRE_EQ(r_try, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_timed, NULL);
  REQUIRE_EQ(r_timed, ccol_not_enough_memory);
  REQUIRE_EQ(allocs_after, allocs_before);
  REQUIRE_EQ(live_after, live_before);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* With an empty free list and a failing allocator, every future submit
 * reports ccol_not_enough_memory and frees the future it allocated. */
TEST(futures, a_failed_task_node_allocation_frees_the_future) {
  ccol_memmgmt_procs_t mp = {.malloc = oom_malloc,
                             .free = oom_free,
                             .calloc = oom_calloc,
                             .realloc = oom_realloc};
  ctpool pool = ctpool_create_mp(1, 4, &mp, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  size_t free_nodes =
      _ctpool_task_free_list_size_for_tests(_ctpool_resolve_for_tests(pool));
  long live_before = atomic_load(&_ctpool_live_futures_for_tests);
  int value = 0;

  atomic_store(&g_oom_enabled, 1);
  ccol_retval_t r_block = ccol_unexpected_failure;
  ctpool_future *f_block =
      ctpool_submit_future(pool, identity_fn, &value, &r_block);
  ctpool_future *f_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_try);
  ctpool_future *f_timed = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_timed =
      ctpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f_timed);
  atomic_store(&g_oom_enabled, 0);
  long live_after = atomic_load(&_ctpool_live_futures_for_tests);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(free_nodes, (size_t)0);
  REQUIRE_EQ((void *)f_block, NULL);
  REQUIRE_EQ(r_block, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_try, NULL);
  REQUIRE_EQ(r_try, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_timed, NULL);
  REQUIRE_EQ(r_timed, ccol_not_enough_memory);
  REQUIRE_EQ(live_after, live_before);
}

/* A future submit that is refused for a full queue, a timeout or a shutdown
 * frees the future and the task node it took, and reports the same code the
 * plain submit gives for that condition. */
TEST(futures, a_refused_future_submit_leaves_no_future_and_no_node) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  ctpool pool = ctpool_create_mp(1, 1, &mp, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  long live_start = atomic_load(&_ctpool_live_futures_for_tests);

  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  ccol_retval_t r_blocker = ctpool_submit(pool, blocker_fn, &ctx, NULL);
  for (int i = 0; i < 10000 && !atomic_load(&started); i++) sleep_ms(1);
  int value = 3;
  ctpool_future *f0 = ctpool_submit_future(pool, identity_fn, &value, NULL);
  long live_full = atomic_load(&_ctpool_live_futures_for_tests);

  ctpool_future *f_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_try);
  ctpool_future *f_zero = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_zero =
      ctpool_timed_submit_future(pool, identity_fn, &value, 0, &f_zero);
  ctpool_future *f_timed = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_timed =
      ctpool_timed_submit_future(pool, identity_fn, &value, 20000, &f_timed);
  long live_refused = atomic_load(&_ctpool_live_futures_for_tests);

  atomic_store(&gate, 1);
  void *res0 = f0 ? ctpool_future_get(f0) : NULL;
  ctpool_future_free(f0);
  ctpool_shutdown_drain(pool);

  long live_drained = atomic_load(&_ctpool_live_futures_for_tests);
  ccol_retval_t r_sd_block = ccol_unexpected_failure;
  ctpool_future *f_sd_block =
      ctpool_submit_future(pool, identity_fn, &value, &r_sd_block);
  ctpool_future *f_sd_try = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_sd_try =
      ctpool_try_submit_future(pool, identity_fn, &value, &f_sd_try);
  ctpool_future *f_sd_timed = (ctpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval_t r_sd_timed = ctpool_timed_submit_future(
      pool, identity_fn, &value, 1000000, &f_sd_timed);
  long live_after_shutdown = atomic_load(&_ctpool_live_futures_for_tests);
  ctpool_destroy(pool);

  REQUIRE_EQ(r_blocker, ccol_success);
  REQUIRE_NE((void *)f0, NULL);
  REQUIRE_EQ(res0, (void *)&value);
  REQUIRE_EQ(live_full, live_start + 1);
  REQUIRE_EQ(r_try, ccol_container_full);
  REQUIRE_EQ((void *)f_try, NULL);
  REQUIRE_EQ(r_zero, ccol_container_full);
  REQUIRE_EQ((void *)f_zero, NULL);
  REQUIRE_EQ(r_timed, ccol_timed_out);
  REQUIRE_EQ((void *)f_timed, NULL);
  REQUIRE_EQ(live_refused, live_full);
  REQUIRE_EQ(live_drained, live_start);
  REQUIRE_EQ(r_sd_block, ccol_not_permitted);
  REQUIRE_EQ((void *)f_sd_block, NULL);
  REQUIRE_EQ(r_sd_try, ccol_not_permitted);
  REQUIRE_EQ((void *)f_sd_try, NULL);
  REQUIRE_EQ(r_sd_timed, ccol_not_permitted);
  REQUIRE_EQ((void *)f_sd_timed, NULL);
  REQUIRE_EQ(live_after_shutdown, live_start);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

typedef struct {
  ctpool pool;
  ctpool_future *f;
  ccol_retval_t r;
} timed_future_waiter_t;

static void *timed_future_waiter(void *p) {
  timed_future_waiter_t *a = p;
  static int value = 5;
  a->r =
      ctpool_timed_submit_future(a->pool, identity_fn, &value, 10000000, &a->f);
  return NULL;
}

static void *shutdown_immediate_thread(void *p) {
  ctpool_shutdown_immediate(*(ctpool *)p);
  return NULL;
}

/* A timed future submit that waits for room and gets woken by a shutdown
 * reports ccol_not_permitted and frees its future, even when its wait
 * returns ETIMEDOUT after consuming the wake, which the test hook forces. */
TEST(futures, a_timed_future_submit_woken_by_shutdown_frees_its_future) {
  ctpool pool = ctpool_create(1, 1, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  long live_start = atomic_load(&_ctpool_live_futures_for_tests);
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  for (int i = 0; i < 10000 && !atomic_load(&started); i++) sleep_ms(1);
  ctpool_submit(pool, inc_counter, &counter, NULL); /* the queue is full */

  atomic_store(&_ctpool_timeout_after_wake_waiting_for_tests, false);
  atomic_store(&_ctpool_timeout_after_wake_for_tests, true);
  timed_future_waiter_t arg = {
      .pool = pool, .f = NULL, .r = ccol_unexpected_failure};
  pthread_t waiter, stopper;
  bool waiter_created =
      pthread_create(&waiter, NULL, timed_future_waiter, &arg) == 0;
  bool waited = false;
  for (int i = 0; waiter_created && i < 10000; i++) {
    if (atomic_load(&_ctpool_timeout_after_wake_waiting_for_tests)) {
      waited = true;
      break;
    }
    sleep_ms(1);
  }
  /* The shutdown waits for the worker, which waits on the gate, so it runs
   * on its own thread, and its broadcast of not_full wakes the submitter. */
  bool stopper_created =
      pthread_create(&stopper, NULL, shutdown_immediate_thread, &pool) == 0;
  if (waiter_created) pthread_join(waiter, NULL);
  atomic_store(&gate, 1);
  if (stopper_created) pthread_join(stopper, NULL);
  atomic_store(&_ctpool_timeout_after_wake_for_tests, false);
  long live_after = atomic_load(&_ctpool_live_futures_for_tests);
  ctpool_destroy(pool);

  REQUIRE_TRUE(waiter_created);
  REQUIRE_TRUE(stopper_created);
  REQUIRE_TRUE(waited);
  REQUIRE_EQ(arg.r, ccol_not_permitted);
  REQUIRE_EQ((void *)arg.f, NULL);
  REQUIRE_EQ(live_after, live_start);
}

/* ========================================================================== */
/*                         DETACHED FUTURES                                   */
/* ========================================================================== */

/* A pthread start routine that fulfills a detached future after a short
 * delay. We use it to show that ctpool_future_get() blocks until an external
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
  /* No cthread_pool is involved at all, which shows that the future really
   * stands on its own. */
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
   * future must stay alive (the producer holds a reference) and be freed
   * once fulfill runs. valgrind checks that there's no leak and no
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

  /* The first result wins, the second call must not have overwritten it. */
  REQUIRE_EQ(ctpool_future_get(f), (void *)&v1);

  ctpool_future_free(f);
}

TEST(detached_futures, fulfill_null_returns_invalid_args) {
  REQUIRE_EQ(ctpool_future_fulfill(NULL, NULL), ccol_invalid_args);
}

TEST(detached_futures, independent_of_any_pool) {
  /* Creates, fulfills and frees several detached futures that no ctpool
   * builds while a real pool does unrelated work next to them, to show there
   * is no hidden coupling between the two. */
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

// This test runs several bursts of tasks of random size, and every task
// increments one shared atomic counter. After each burst it drains with
// ctpool_wait and checks that ctpool_pending_count and ctpool_active_count
// are back to zero and that the counter matches the total number of tasks
// submitted so far. So every submitted task runs exactly once, no more and
// no less, and the pool always reports itself fully idle once it's
// drained.
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
  /* ctpool_wait must not shut down the pool. The workers stay alive
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
  /* ctpool_wait on a pool that got an immediate shutdown (idle, no tasks)
   * should return without blocking. */
  ctpool_construct(pool, 2, 0);
  ctpool_shutdown_immediate(pool);
  ctpool_wait(pool);
  ctpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_immediate_unblocks_wait) {
  /* A thread blocked in ctpool_wait must be woken when another thread
   * calls ctpool_shutdown_immediate and the queue is discarded while
   * active_count is zero.
   *
   * The setup is one worker running a task held by a gate, so active_count
   * is 1, while five tasks queue up and a waiter thread enters ctpool_wait.
   * Then the test opens the gate and the worker leaves blocker_fn, so
   * active_count drops to zero for a short window before the worker picks up
   * the next queued task. The ctpool_shutdown_immediate call that follows
   * sees this window, with active_count at 0 and a non-empty queue. It
   * discards the queue and has to broadcast idle_cv so the waiter returns,
   * and without that broadcast the waiter deadlocks. */
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
  sleep_ms(10); /* let waiter enter _ccol_cond_var_wait inside ctpool_wait */

  /* Open the gate and poll until active_count drops to 0. When the poll
   * reads 0 the worker has finished blocker_fn but hasn't incremented
   * active_count again for the next queued task, so calling
   * ctpool_shutdown_immediate right after that gives it the best chance to
   * see active_count==0 with a non-empty queue, which is the condition that
   * would deadlock ctpool_wait without the idle_cv broadcast. */
  atomic_store(&gate, 1);
  while (ctpool_active_count(pool) > 0) sleep_ms(1);
  ctpool_shutdown_immediate(pool);

  pthread_join(waiter, NULL); /* deadlocks without the idle_cv broadcast */
  ctpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_drain_unblocks_wait) {
  /* The idle_cv broadcast when the last task completes must wake a thread
   * waiting in ctpool_wait, also when another thread calls
   * ctpool_shutdown_drain at the same time while tasks are running. Unlike
   * shutdown_immediate the drain path sends no idle_cv signal of its own, so
   * it relies only on the broadcast from the last task to complete. */
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

  /* A helper thread opens the gate after a delay, so the worker leaves
   * blocker_fn and drains the queued tasks while shutdown_drain is already
   * running. */
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
  /* ctpool_shutdown_drain joins every worker, so a ctpool_wait call after it
   * returns at once. active_count is 0 and queue_size is 0, so the
   * while condition is false on entry and the call never gets to a
   * _ccol_cond_var_wait. */
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
  /* Block the only worker so tasks pile up, then call
   * ctpool_shutdown_immediate while the gate is closed. It discards the queue
   * atomically under the pool mutex before the worker can dequeue anything.
   * A helper thread opens the gate once the shutdown has taken the queue, so
   * the worker can exit and the join inside ctpool_shutdown_immediate
   * completes. All 10 queued tasks are discarded, so the counter
   * stays 0.
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

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = _ctpool_resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);

  /* The queue is discarded here (the worker is in blocker_fn and not
   * dequeuing) and the call blocks inside until the worker joins, while the
   * helper thread opens the gate so the worker can exit. */
  ctpool_shutdown_immediate(pool);
  if (created == 0) pthread_join(releaser, NULL);
  int counter_after = atomic_load(&counter);
  ctpool_destroy(pool);

  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(counter_after, 0);
}

TEST(shutdown_immediate, futures_become_cancelled) {
  /* Block the only worker, queue two futures, then call
   * ctpool_shutdown_immediate while the gate is closed (gate=0). The queue is
   * discarded atomically before the worker can dequeue anything, so both
   * futures are cancelled. A helper thread opens the gate once the
   * shutdown has taken the queue, so the worker can exit. */
  atomic_int gate = 0;
  atomic_int started = 0;
  int dummy = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 0);
  ctpool_submit(pool, blocker_fn, &ctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  ctpool_future *f1 = ctpool_submit_future(pool, slow_identity, &dummy, NULL);
  ctpool_future *f2 = ctpool_submit_future(pool, slow_identity, &dummy, NULL);
  REQUIRE_NE((void *)f1, NULL);
  REQUIRE_NE((void *)f2, NULL);

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = _ctpool_resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);

  ctpool_shutdown_immediate(pool); /* discards f1 and f2 while gate==0 */
  if (created == 0) pthread_join(releaser, NULL);

  REQUIRE_EQ(created, 0);
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

/* One heap argument per task of the discard tests below. on_complete
 * records the ran flag it got and frees the argument, so a second call for
 * the same task is a double free that valgrind and the allocator report,
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
   * each of them gets exactly one on_complete call with ran == false, on the
   * thread that calls ctpool_shutdown_immediate and before that call
   * returns. Since each on_complete frees its heap argument, the memtest run
   * of this suite also shows that no argument is freed twice or leaked.
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

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = _ctpool_resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
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

/* An on_complete of a discarded task that calls back into its own pool. In
 * there the wait and both shutdowns are no-ops, just as from a worker, and
 * a submit is refused because the pool is shutting down; otherwise a wait
 * on the shutdown that this very thread hasn't finished yet would hang. */
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

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = _ctpool_resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
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

/* Two pools whose discarded on_complete callbacks run nested on one thread.
 * The callback of a task discarded from outer shuts down inner, and the
 * callback of a task discarded from inner then runs inside it. In there the
 * thread counts as a worker of outer as well, so a wait on outer, a
 * shutdown of outer and a submit to outer behave exactly as they would from
 * one of outer's own callbacks. If outer were forgotten while inner
 * discards, shutting outer down from there would wait for ever on a
 * shutdown that this same thread owns and hasn't finished, so the callback
 * asks the self-call check first and only calls into outer when it says
 * yes.
 *
 * A shutdown runs the discard callbacks before it joins the workers, so
 * each callback opens the gate of its own pool's blocked worker. That way a
 * pool's queued task can't run before the shutdown discards it, however
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
    /* inner's frame is gone and outer's is left. */
    atomic_store(&c->outer_sees_outer_after,
                 _ctpool_is_self_call_for_tests(c->outer_raw) ? 1 : 0);
  }
  atomic_fetch_add(&c->outer_calls, 1);
  atomic_store(c->gate_outer, 1);
}

/* A bound for a test whose callbacks open their own gates. If a callback
 * doesn't run, this thread opens the gate after 10 s so the test fails
 * instead of hanging, and it returns as soon as the gate is open. */
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

  /* Each pool's only worker is kept busy, so each pool's next task stays in
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

/* A pthread start routine that drains the pool arg's handle points to. */
static void *discard_drain_thread_fn(void *arg) {
  ctpool_shutdown_drain(*(ctpool *)arg);
  return NULL;
}

TEST(shutdown_immediate, escalated_drain_reports_discarded_tasks_as_not_run) {
  /* A drain is running on another thread, held up by a blocked task, and a
   * ctpool_shutdown_immediate escalates it. Every queued task gets
   * on_complete(arg, false) exactly once and none of them runs. */
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
   * and the immediate call below only starts once the drain owns the
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
  immediate_release_ctx_t rctx = {.gate = &gate, .raw = raw};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
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
  /* A drain runs every queued task, so each on_complete gets ran == true. */
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
  ctpool pool = ctpool_create_mp(2, 0, &mp, &err);
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

/* This covers a documented contract that both the "Result ownership" doc
 * comment in cthreadpool.h and the implementation comment on
 * alloc_future_task state: a ctpool_future always comes from plain malloc
 * or calloc and never from the pool's custom allocator. Only the internal
 * ctpool_task node that carries the submission goes through
 * pool->m_procs.
 *
 * No other test tells this apart from the other design, where the future
 * ALSO goes through the custom procs. The comment on
 * futures.submit_future_returns_null_on_oom says it can't exercise the
 * future's allocation at all, and with a failing custom allocator *f is
 * NULL either way, whichever of the two designs the library implements.
 *
 * A custom allocator that COUNTS instead of failing closes that gap
 * directly. One ctpool_submit_future call should raise g_alloc_count by
 * exactly 1 (the task node alone) and not by 2, and a regression that sends
 * the future itself through the custom calloc raises it by 2. */
TEST(custom_mprocs, future_struct_uses_plain_allocator_not_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ctpool_create_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  int before_alloc = atomic_load(&g_alloc_count);
  int value = 42;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &value, NULL);
  REQUIRE_NE((void *)f, NULL);
  int after_submit_alloc = atomic_load(&g_alloc_count);

  REQUIRE_EQ(after_submit_alloc - before_alloc, 1); /* task node only */

  REQUIRE_EQ(ctpool_future_get(f), (void *)&value);
  ctpool_future_free(f);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  /* Every tracked allocation of this test (constructing the pool and the one
   * task node) needs a matching tracked free, while the future's allocation
   * and free show up in neither count. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Checks task node reuse directly. Tasks are submitted one at a time and
 * each one completes fully before the next is submitted. A completed
 * task is freed, so it goes back onto the pool's free list, and ctpool_wait
 * can't see active_count reach 0 until the worker's task_free call has
 * returned. So after the very first submission no submission may call the
 * custom allocator again, only the first one finds the free list empty. */
TEST(custom_mprocs, task_nodes_are_recycled_not_reallocated_each_time) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ctpool_create_mp(1, 0, &mp, &err);
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

  /* No further tracked allocation may happen, since each of the N later
   * submissions reuses exactly the node that the previous submission's
   * task_free put back. */
  REQUIRE_EQ(atomic_load(&g_alloc_count), alloc_after_first);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);

  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* Queues burst tasks behind the pool's only worker, which a gate blocks, and
 * then lets the whole burst run. At its peak the queue is burst deep, so the
 * burst needs burst + 1 task nodes at once. */
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

/* The free list keeps every node a burst needed, up to its ceiling. A trim
 * keeps the whole list when the traffic since the previous trim emptied it,
 * and otherwise gives back half of what the list holds above
 * num_threads * 4. The numbers come from the white-box accessors, and the
 * expectations are written out here rather than read back from the
 * library. */
TEST(custom_mprocs, task_free_list_keeps_what_the_last_burst_used) {
  ctpool_construct(pool, 1, 0);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(_ctpool_task_free_list_cap_for_tests(raw), (size_t)32768);

  /* A burst of 12 behind the blocker needs 13 nodes and keeps all 13, and
   * since the burst emptied the list the first trim after it gives nothing
   * back. */
  bool first = ctp_gated_burst(pool, 12);
  size_t after_deep = _ctpool_task_free_list_size_for_tests(raw);

  /* With a trim at every idle point and one task at a time, the first trim
   * keeps 13 and each later one halves the excess over 4 (rounding the half
   * up): 13, 8, 6, 5, 4. */
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

/* A deep queue costs no allocation once the pool has seen that depth. The
 * second and every later burst of the same depth reuse the first burst's
 * nodes, so the custom allocator sees no call at all, even with a trim at
 * every idle point, because each burst empties the list and so no trim
 * gives anything back. With the list capped at
 * num_threads * 4, every later burst of 500 allocates 497 nodes again. */
TEST(custom_mprocs, repeated_deep_bursts_allocate_no_task_nodes) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  ctpool pool = ctpool_create_mp(1, 0, &mp, &err);
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
  /* 2 slow workers, a 16-slot queue and 64 tasks. ctpool_submit must
   * block when the queue fills, and every task completes. */
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
  /* Several threads submit tasks at the same time and every task must run
   * exactly once, which checks that the queue and the counters have no race
   * when many producers submit at once. */
  enum { NPRODUCERS = 4, TASKS_PER = 250 };
  atomic_int counter = 0;
  ctpool_construct(pool, 4, 0);

  producer_arg_t args[NPRODUCERS];
  pthread_t threads[NPRODUCERS];
  int created = 0;
  for (int i = 0; i < NPRODUCERS; i++) {
    args[i] =
        (producer_arg_t){.pool = pool, .counter = &counter, .n = TASKS_PER};
    /* A failure half way through must not leave the join loop below with an
     * uninitialized threads[i] slot to join. That is undefined behavior and
     * can hang on garbage pthread_t data, so the loop only joins the threads
     * it really created. */
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
 * groups, adapted to ctpool's lock-protected pin mechanism (see the
 * comments on the pending_resolve_count and pin_cv fields of struct
 * cthread_pool in src/cthreadpool.c). Like clru_cache, ctpool reuses the
 * lock-protected decrement and broadcast of chttpcli and chttpsvr, since
 * this module already uses one global mutex. */

/* Once a destroy has fully completed, a second destroy later, on a separate
 * copy of the same original handle value, must be a fatal error. It runs
 * in a forked child because _ccol_fatal_err aborts the whole process, which
 * is how tests/clogger/tests.c handles misuse that ends the process as
 * well. */

/* Publishing the handle into the pin index is the last step of creating a
 * pool, and it can fail, because on the first use of an index the pin index
 * allocates a chunk and a stripe block with plain calloc that no caller's
 * allocator reaches. The rollback of that failure has to put the slot back
 * on the free list, otherwise the index is lost for the life of the process.
 *
 * The rollback also gives the slot back the shape a destroy leaves it in,
 * and the fork below exercises that as a smoke test and can't tell the two
 * apart. This rollback never sets in_use, and _ctpool_atfork_prepare skips
 * every slot that has neither in_use nor torn_down set, so the walk can't
 * reach a rolled-back slot whatever its ptr holds. Without
 * _ccol_pintable_force_next_publish_failure_for_tests that path needs a real
 * out-of-memory condition, which an ordinary test run can't produce. */

TEST(ctpool_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* We repeat the cycle and assert on how much the table grew over the whole
   * run, because a single cycle can't tell a rollback from no rollback. One
   * lost slot makes the next pool grow the table by one, which looks exactly
   * like a table that had no free slot to start with. Over CYCLES rounds a
   * working rollback grows the table by nothing, while a missing push onto
   * the free list grows it by one each round. */
  enum { CYCLES = 8 };
  size_t before = _ctpool_slot_table_capacity_for_tests();
  size_t free_before = _ctpool_free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  for (int i = 0; i < CYCLES; i++) {
    _ccol_pintable_force_next_publish_failure_for_tests();
    ctpool bad = ctpool_create(2, 0, NULL);
    if (bad != CTPOOL_INVALID) {
      all_failed = false;
      __ctpool_destroy(bad);
      break;
    }
    ctpool good = ctpool_create(2, 0, NULL);
    if (good == CTPOOL_INVALID) {
      all_created = false;
      break;
    }
    __ctpool_destroy(good);
  }

  _ccol_pintable_force_next_publish_failure_for_tests();
  char *err = NULL;
  ctpool failed = ctpool_create(2, 0, &err);
  /* We capture these instead of asserting here. The first is false only
     when the forced failure didn't apply, and then `failed` is a live pool
     with two worker threads, so returning now would leak it along with
     everything else that the rest of this test still has to clean up. */
  bool forced_failure_applied = (failed == CTPOOL_INVALID);
  bool err_reported = (err != NULL);
  if (!forced_failure_applied) __ctpool_destroy(failed);

  /* A fork while that slot sits on the free list. The prepare handler walks
   * every slot and would dereference a rolled-back slot here if it took it
   * for a live one. The child only exits, so all we check is that the child
   * gets to an exit at all.
   *
   * We assert only WIFEXITED and never a particular exit code. A leak
   * checker can run with an error exit code and treat still-reachable memory
   * as an error, and then it replaces a forked child's status with that
   * code. The child inherits the parent's whole live image and reports all
   * of it at exit, so a WEXITSTATUS check would fail under memtest and pass
   * everywhere else, for a reason that has nothing to do with the property
   * under test. A crash in the prepare handler shows up anyway, because
   * WIFEXITED is false then. */
  pid_t pid = fork();
  if (pid == 0) _exit(0);
  int status = 0;
  bool forked = (pid != -1);
  bool reaped = forked && (waitpid(pid, &status, 0) == pid);

  /* The slot went back on the free list, so the next pool takes it again and
   * the table doesn't grow for the failed attempt. That pool should also be
   * fully usable, since a stale self_handle left behind would break it on
   * its first call. */
  ctpool pool = ctpool_create(2, 0, NULL);
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
  /* Growth alone isn't enough. A lost slot only grows the table while the
     free list is empty, so a run where earlier tests left several free
     indices behind would quietly absorb every loss. The free list also has
     to come back to where it started, and that holds however deep it was. */
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
    ctpool pool = ctpool_create(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctpool stale = pool;     /* our own copy of the handle value, separate
            from the local that the macro below invalidates */
    ctpool_destroy(pool);    /* completes normally (implicit drain shutdown);
           afterwards the local `pool` is CTPOOL_INVALID while `stale` holds
           the original value */
    __ctpool_destroy(stale); /* the misuse under test: a second, purely
        sequential destroy of a handle that is already fully torn down */
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* While a pool is alive, the process-exit cleanup of the slot table defers
 * its release, and destroying the last pool then does it. After that
 * release the worker key is deleted, a new pool is refused with an error and
 * destroying the old handle ends the process with the documented
 * stale-handle message. The child runs all of it since the release is final
 * for the process. A deferred release that left
 * the key alive would let the creation reach the released table, and the
 * child would exit with 4 or abort through an assertion in the vector
 * module instead of printing the stale-handle message. */
TEST(ctpool_handle_lifecycle, deferred_exit_release_retires_the_key) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    ctpool pool = ctpool_create(1, 0, NULL);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctpool stale = pool;
    _ctpool_run_exit_cleanup_for_tests();
    if (!_ctpool_worker_key_live_for_tests()) _exit(5);
    ctpool_destroy(pool);
    if (_ctpool_worker_key_live_for_tests()) _exit(3);
    char *err = NULL;
    ctpool again = ctpool_create(1, 0, &err);
    if (again != CTPOOL_INVALID || !err) _exit(4);
    __ctpool_destroy(stale);
    _exit(0); /* unreachable when _ccol_fatal_err() aborts as expected */
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

/* Two threads calling destroy on two separate copies of the SAME valid
 * handle, as close to the same moment as the test can manage, must be
 * fatal too. That's exactly the concurrent double free that the
 * generation-tagged slot table closes for chttpcli, chttpsvr,
 * ccol_event_loop and clru_cache. */
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
    ctpool pool = ctpool_create(2, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_concurrent_destroy_arg_t a1 = {.h = pool};
    ctp_concurrent_destroy_arg_t a2 = {.h = pool};
    pthread_t t1, t2;
    /* We are inside a forked child here, where REQUIRE_* is unsafe because
     * its early return would skip this branch's _exit() and fall back into
     * the harness's test loop a second time. So a failed create falls
     * through to an exit of its own that isn't SIGABRT, which the parent's
     * WIFSIGNALED and SIGABRT check below turns into a clean test failure,
     * and the child never joins a garbage pthread_t that nothing created. */
    if (pthread_create(&t1, NULL, ctp_concurrent_destroy_thread, &a1) != 0)
      _exit(2);
    if (pthread_create(&t2, NULL, ctp_concurrent_destroy_thread, &a2) != 0)
      _exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* Unreachable, since whichever destroy call loses the race
                 has to end up in _ccol_fatal_err(). */
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

/* Runs ctpool_destroy on its own thread rather than the main test thread,
 * setting entered to true as its very first action and returned to true as
 * its very last. The main thread can then see two things without any
 * timing assumption: that destroy was called, because entered is true, and
 * that destroy did NOT return while the test is deliberately keeping the
 * pool's worker stuck. resolve_then_use_race_destroy_waits below uses it,
 * and that test's doc comment explains why this order is a real proof and
 * not a heuristic. */
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
 * elsewhere in the codebase there's no forked child to point /proc at, the
 * whole race is between threads of this same process. So instead of one
 * pid's /proc/<pid> files, this function dumps status, wchan, syscall and
 * stat for every thread of THIS process from the /proc/self/task/<tid>
 * entries, when ctpool_destroy hasn't returned within a generous fixed
 * bound.
 *
 * It does NOT try to cancel the stuck call or recover from it in any other
 * way, on purpose. A pthread_cancel that lands while a thread holds a mutex
 * can leave that lock held for the rest of this process's life, which
 * corrupts every later use and gives no clean failure. Instead the function
 * ends the whole process right after flushing the dump, with an exit code
 * of its own, so the exit reads as a deliberate diagnostic exit and not an
 * ordinary crash. That turns an uninformative 45-minute timeout at the job
 * level into a fast and informative one.
 *
 * The dump itself uses plain, buffered fprintf(stderr, ...) and not
 * cdebuglog_write(), because it runs only after the bounded window has
 * closed, long after the timing that mattered, so real write(2) syscalls
 * cost nothing here; _dump_stuck_child_diagnostics in
 * tests/cthreadcomm/tests.c reasons the same way. The function does call
 * cdebuglog_flush() first, so every [DEBUG_TEST] checkpoint that is already
 * buffered comes out in the right time order, ahead of the dump's raw
 * output. Without that flush the checkpoints would stay buffered until
 * process exit, and the _exit() below skips that exit path, so they would
 * be lost for good. */
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
   * prints, so it comes out in the right time order, ahead of this
   * function's raw fprintf() output. Without this flush it's lost entirely,
   * because the _exit() below skips the flush that cdebuglog registers with
   * atexit. */
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
  _exit(97); /* A sentinel of its own. It says the watchdog found a stuck
                ctpool_destroy and not that an ordinary crash happened, so a
                CI log or an exit-code check can tell the two apart. */
}

typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool done;
} ctp_destroy_watchdog_ctx_t;

/* A bounded condvar wait rather than a blind poll, since this codebase
 * prefers a real timed wait over polling at a fixed interval. The wait
 * wakes when the main thread signals done or after the fixed bound,
 * whichever comes first.
 *
 * The test creates this thread at the very start of
 * resolve_then_use_race_destroy_waits, before that test's two bounded
 * safety-net polls. Each of those polls runs for up to 10s, neither can hang
 * forever, and both run BEFORE the pthread_join that this watchdog guards.
 * The bound of 60 real seconds is three times that combined worst case of
 * 20s, so a slow machine that isn't hung can use almost the whole budget on
 * both polls and still can't trip this watchdog.
 *
 * Even at 60s this is only a safety net against a hang and not a threshold
 * the test's correctness depends on. Real synchronization drives every step
 * it brackets, as the doc comment of resolve_then_use_race_destroy_waits
 * explains in detail. Only a real hang should ever get to the timeout
 * branch, never ordinary scheduling variance. */
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

/* A pin that a resolve takes must outlive the call that took it. This
 * test checks that against the reversed order of waits that
 * __ctpool_teardown_raw uses, since that function runs the shutdown drain
 * BEFORE it waits on pending_resolve_count (see its own comment in
 * src/cthreadpool.c).
 *
 * The pool has one worker and a queue_cap of 1. Its worker is stuck in a
 * long first task and a second task already fills the queue, so a third
 * ctpool_submit call blocks inside submit_internal's
 * _ccol_cond_var_wait(not_full, ...) and holds a real, resolved pin on the
 * handle for as long as it blocks.
 *
 * A concurrent ctpool_destroy has to get three things right. It must not
 * crash or free the pool under the blocked submitter that is still pinned,
 * which is the use-after-free the generation-tagged slot table and its pin
 * counter close. Only the shutdown's not_full broadcast can release
 * submit_internal's blocked wait, so waiting on pending_resolve_count BEFORE
 * the shutdown would deadlock destroy for ever. It must let the blocked
 * submit return ccol_not_permitted once the shutdown starts. And it must
 * block until the worker thread has been joined, which is a real wait and
 * not an instant return.
 *
 * The test proves all three below without assuming any fixed sleep, on
 * purpose. An alternative would be a fixed head start of about 10ms before
 * creating a background thread that opens the gate after about 20ms, and to
 * take a ctpool_destroy of more than 10ms as proof of the third property.
 * That isn't a safe substitute, because both delays are assumptions that
 * depend on load, and they break under valgrind (on memtest in a native
 * linux-x86_64-gcc job, no emulation at all) as soon as unrelated
 * thread-creation overhead elsewhere in the test eats into that budget. The
 * gate then opens before the test even calls ctpool_destroy, and that call
 * returns in about 0ms, or the blocked submit comes back ccol_success
 * instead of ccol_not_permitted for the same reason. Neither symptom is a
 * product bug, both are that timing assumption breaking under load.
 *
 * So the structure below uses a real synchronization point instead of a
 * fixed sleep. ctp_destroy_thread_fn's entered and returned flags let the
 * main thread see two things with no clock at all: that ctpool_destroy was
 * called, because entered is true, and that the call has provably NOT
 * returned while gate is 0. It structurally CAN'T have returned at that
 * point however much wall-clock time went by, because the pool's only
 * worker can't leave the spin in blocker_fn (its only exit condition) until
 * this test sets gate itself, and ctpool_destroy can't return before that
 * worker has been joined. So the check below is a real proof of the third
 * property and not a timing heuristic.
 *
 * _ctpool_pending_resolve_count_for_tests replaces a head start the same
 * way. The test polls it, bounded only as a safety net against a hang and
 * never as a correctness threshold, to confirm that the blocked submitter
 * really pinned the handle before going on, however long that takes on the
 * machine that runs it.
 *
 * This test also hangs in one environment, linux-arm32-clang under real
 * qemu-user 8.2.2, where the destroy thread below doesn't finish within
 * that job's 45-minute timeout. This test never calls fork(), so the
 * confirmed and already filed qemu-user fd_trans_lock bug can't be the
 * cause. That bug is about fd_trans_lock, an internal process-wide pthread
 * mutex of linux-user, which stays locked for ever in a forked child when
 * another thread of the parent held it at the instant of the fork() (see
 * gitlab.com/qemu-project/qemu/-/issues/2846). That emulator bug can't
 * trigger without a fork() call, and the race of this test is entirely
 * between ordinary threads of one process. The real cause is open.
 *
 * Two things are here so that a recurrence carries enough information to
 * diagnose it. One is the [DEBUG_TEST] checkpoints below, which go through
 * cdebuglog_write() instead of a direct fprintf(stderr, ...), because for a
 * timing-sensitive mystery like this one buffering the checkpoints matters
 * and a real write(2) syscall per checkpoint doesn't (see cdebuglog.h for
 * why). The other is the watchdog thread around the join of the destroy
 * thread below, which the comment on that section above describes. The
 * checkpoint printed last narrows down how far the main thread got, and the
 * watchdog's /proc dump shows the real kernel-level wait state of every
 * thread, as wchan and syscall, at the moment of the hang. Without them all
 * we'd have is a bare timeout with no information. */
TEST(ctpool_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  ctpool_construct(pool, 1, 1);
  struct cthread_pool *raw = _ctpool_resolve_for_tests(pool);

  /* The thread is created first. Its bounded 60s wait is only a safety
   * net against a hang, as the doc comment of its section above explains
   * (including why 60s). That wait isn't tied to any timing concern in
   * the rest of this test, so the exact point where it's created doesn't
   * matter. */
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

  /* Every step from here on can fail and the pool's worker is already alive
   * and spinning in blocker_fn, so no step may carry a direct assertion.
   * Unlike ctpool_construct_scoped, ctpool_construct doesn't destroy pool on
   * scope exit, so an early return from a failed REQUIRE_* here would leak
   * that running worker, and every thread created so far, for the rest of
   * this test binary's life. So the test captures every outcome into a local
   * on every path, and every REQUIRE_* runs only after the test has joined
   * every thread it created and destroyed pool. The codebase has a
   * test-hygiene lesson on exactly this kind of gap.
   */

  /* Fill the bounded queue (cap 1) so the next submit really blocks. */
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
  /* Poll until blocked_thread's resolve has really pinned the handle. The
   * bound is only a safety net against a hang and never a correctness
   * threshold, and the test doesn't assume that some fixed sleep was long
   * enough; this test's doc comment above says why a fixed sleep isn't a
   * safe substitute. */
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
    /* Poll until destroy_thread has really entered ctpool_destroy, and the
     * bound is only a safety net against a hang. gate is 0 here because the
     * test hasn't set it yet, so the pool's only worker can't have left the
     * spin in blocker_fn and ctpool_destroy can't have returned either,
     * however long entered took to become true. This test's doc comment
     * above says why that makes the read below a real proof and not a
     * timing heuristic. */
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
   * as well, because FreeBSD allocates the object behind it on first use. */
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
  /* The return value is ignored on purpose. A legitimate race with a
   * concurrent destroy can make this resolve fail (returning 0) instead of
   * succeeding, and both outcomes are correct. This thread is only here to
   * make resolve/pin/unpin traffic at the same time as the destroy thread
   * below. */
  ctpool_pending_count(a->h);
  return NULL;
}

/* This isn't redundant with resolve_then_use_race_destroy_waits above. That
 * test's blocked submit guarantees pending_resolve_count > 0 for a long,
 * deterministic window, while this one needs the opposite shape, a fast
 * entry point that doesn't block raced against a concurrent destroy.
 * ctpool_pending_count is such an entry point: it resolves, reads one field
 * under a mutex, unpins and returns.
 *
 * The test repeats that race under stress because the failure window for a
 * fast pin and unpin pair is only a few instructions wide, and a single run
 * without stress doesn't reproduce it reliably. Each iteration uses a fresh
 * pool, so every repetition gets its own independent race and no iteration
 * reuses a handle that has already been destroyed. */
TEST(ctpool_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = ctpool_create(2, 0, &err);
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

/* _ctpool_teardown_raw must not read pool->shutdown_started without a lock
 * before deciding whether to run a drain shutdown, because that read races
 * the write to the same field from an in-flight, pinned
 * ctpool_shutdown_immediate or ctpool_shutdown_drain call, which holds the
 * right lock (see the comment on _ctpool_teardown_raw in src/cthreadpool.c
 * for the whole story). What it has to do is always call
 * _ctpool_shutdown_drain_internal, which is idempotent, instead of reading
 * the field first.
 *
 * The test races an explicit ctpool_shutdown_immediate call on one thread
 * against __ctpool_destroy on another, both on the same live handle, and
 * repeats the race under stress. Whichever thread gets pool->mu first,
 * nothing may crash, hang or corrupt state. */
TEST(ctpool_handle_lifecycle,
     shutdown_immediate_races_concurrent_destroy_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    ctpool pool = ctpool_create(2, 0, &err);
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

/* Reusing a slot validly must never look like a stale handle to the object
 * that held that slot before, which is the whole point of the generation
 * counter. */
TEST(ctpool_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  char *err = NULL;
  ctpool a = ctpool_create(2, 0, &err);
  REQUIRE_NE(a, CTPOOL_INVALID);
  ctpool stale_a = a;
  ctpool_destroy(a);

  ctpool b = ctpool_create(2, 0, &err);
  REQUIRE_NE(b, CTPOOL_INVALID);

  /* B's operations should succeed as usual, whether or not the allocator
   * gave B exactly the address A had. */
  REQUIRE_EQ(ctpool_pending_count(b), (size_t)0);

  /* A's stale handle must never resolve to B, even when B reused the same
   * underlying address. That's what the generation counter is there for. */
  REQUIRE_EQ((void *)_ctpool_resolve_for_tests(stale_a), NULL);

  ctpool_destroy(b);
}

/* The slot table is bounded and doesn't grow for ever. A churn loop of
 * creates and destroys keeps only one slot in flight at a time, so it must
 * reuse that one freed slot on every iteration instead of growing the table
 * further.
 *
 * We capture the capacity right after the first create and destroy pair
 * instead of asserting a fixed value such as 1, because earlier tests in the
 * same process may have grown the table to some N above 1 already. What
 * this test needs to show is that ITS OWN churn adds no growth, not what the
 * absolute size of the table happens to be when it runs. */
TEST(ctpool_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  char *err = NULL;
  ctpool pool0 = ctpool_create(2, 0, &err);
  REQUIRE_NE(pool0, CTPOOL_INVALID);
  ctpool_destroy(pool0);
  size_t capacity_after_first = _ctpool_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    ctpool pool = ctpool_create(2, 0, &err);
    REQUIRE_NE(pool, CTPOOL_INVALID);
    ctpool_destroy(pool);
  }

  REQUIRE_EQ(_ctpool_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*                    SELF-CALL FROM WITHIN A TASK                           */
/* ========================================================================== */

/* Guards against a use-after-free that would happen every single time. A
 * task (or its on_complete callback) can call ctpool_destroy on the pool it
 * runs on, and the pool's mutex, condition variables and struct must
 * survive that call, because the worker thread that made it is on its way
 * back through worker_thread_fn, which runs task_free and then a lock, a
 * decrement, a broadcast and an unlock against the freed object.
 *
 * pthread_join on the calling thread's own id returns EDEADLK at once
 * instead of blocking. The shutdown guarantees that it joins every worker
 * before it frees anything, so an unchecked return value there would let
 * the calling worker escape that guarantee. Such a self-call is a fatal
 * error, just like a stale or already destroyed handle, and the test runs in
 * a forked child because _ccol_fatal_err aborts the whole process. */
typedef struct {
  ctpool pool;
} ctp_self_destroy_ctx_t;

static void self_destroy_task(void *arg) {
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  ctpool_destroy(ctx->pool); /* the actual misuse under test */
  /* Unreachable when _ccol_fatal_err() aborts, which is what we expect. If
   * the code does get here, worker_thread_fn's post-task code (task_free and
   * then bookkeeping under pool->mu) runs against a freed pool right after
   * this function returns. */
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
    /* Bounds the child's lifetime in case a regression turns the crash into
     * a hang (see the ctpool_wait call below). The parent below only checks
     * WTERMSIG against SIGABRT, so ending through SIGALRM here fails the test
     * cleanly instead of hanging the whole suite. */
    alarm(2);

    char *err = NULL;
    ctpool pool = ctpool_create(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, self_destroy_task, &ctx, NULL);
    /* Deterministic for the expected case and not a fixed sleep. The worker
     * ends the whole process with SIGABRT well before this call could
     * return, so what this thread does at that instant doesn't matter. The
     * alarm() above guards this call, so the test doesn't rely on this call
     * alone, since a future regression could corrupt pool in a way that makes
     * ctpool_wait itself misbehave instead of cleanly resolving pool as
     * stale. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* The same hazard, reached through on_complete instead of the task function.
 * on_complete runs on the same worker thread before task_free, so it is just
 * as much inside the pool's worker as fn is. */
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
    /* See the same alarm() comment in destroy_from_within_own_task_is_fatal
     * above. */
    alarm(2);

    char *err = NULL;
    ctpool pool = ctpool_create(1, 0, &err);
    if (pool == CTPOOL_INVALID) _exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    ctpool_submit(pool, noop_fn, &ctx, self_destroy_on_complete);
    /* See the same comment in destroy_from_within_own_task_is_fatal above.
     * It's deterministic either way and not a guess based on a fixed sleep,
     * and the alarm() above bounds it in any case. */
    ctpool_wait(pool);
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* The related, milder hazard of a self-call to shutdown_drain. A task that
 * calls ctpool_shutdown_drain on its own pool must be a complete no-op.
 * Not crashing isn't enough, the call must also leave shutdown_drain and
 * shutdown_started unset.
 *
 * A self-call that set those two fields would do two things. The next valid
 * ctpool_submit call of this test would fail with ccol_not_permitted. And
 * this worker's self-join would quietly skip a join that no LATER real
 * external shutdown or destroy can retry, because _ccol_thread_join on a
 * thread's own id returns EDEADLK instead of blocking, so that worker
 * thread's OS resources would leak for the rest of the process's life.
 *
 * Without this guard the test already fails on the first of those two
 * points. */
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

  /* The pool should be fully alive and accept new work, since a self-call
   * must shut nothing down. */
  atomic_int counter = 0;
  REQUIRE_EQ(ctpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  ctpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  /* A real external shutdown/destroy afterwards should work cleanly, which
   * shows that the earlier self-call didn't leave any worker thread
   * un-joinable for good. */
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

/* The matching hazard for a self-call to ctpool_wait. Without the guard a
 * task that waits on its own pool deadlocks for ever, because the calling
 * task itself counts in active_count until it returns. The test runs in a
 * forked child bounded by alarm(), so a regression fails the test instead
 * of hanging the whole suite. */
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
  /* The child reports its outcome through a pipe and not its exit code.
   * Under make memtest valgrind replaces a forked child's real exit code
   * with its own --error-exitcode as soon as it finds ANY still reachable
   * allocation in the child's inherited image at exit, and every child
   * forked in the middle of the suite has one, since the rest of the suite
   * isn't quiet yet. So the exit code can't carry this result reliably. The
   * test
   * fork_safety.destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future
   * a few tests below reasons the same way, and the fork_safety group in
   * tests/clogger/tests.c has the original, independently confirmed account
   * of this valgrind behaviour. */
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
    ctpool pool = ctpool_create(1, 0, &err);
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
  /* Not WEXITSTATUS, for the reason given in this test's comment above.
   * WIFEXITED alone catches a real regression that hangs, since the alarm
   * above turns such a hang into WIFSIGNALED, and it catches a real crash
   * too. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* A separate interaction between the self-call guard above and the
 * fork-safety machinery elsewhere in this file. A task can call fork()
 * itself (not the test's top-level fork()), and the only surviving thread
 * of that child is then an exact continuation of the same worker call
 * frame. pool then carries the foreign_since_fork mark, because it was in_use
 * at the instant of the fork() and both _ctpool_atfork_prepare and
 * _ctpool_atfork_release walked it like every other live pool. This
 * thread's ctpool_worker_key_bundle TLS value points at pool, because it
 * inherited that value through the fork(), which duplicates the calling
 * thread's whole state, TLS included.
 *
 * A ctpool_destroy(pool) call from there must count as a self-call and
 * the library has to reject it. __ctpool_destroy checks this before it
 * dispatches to the foreign and non-foreign branches of
 * _ctpool_teardown_raw, so the pool being foreign never hides a real
 * self-destroy.
 *
 * Revert only the self-call check and leave the
 * foreign-pool machinery alone, and the test turns into a real
 * use-after-free in the child, because the foreign branch of
 * _ctpool_teardown_raw frees pool's struct while this very thread is on its
 * way back through the tail code of worker_thread_fn. */
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
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  /* Parent side. This is the pool's own real worker thread, and nothing the
   * child does to its own, independent post-fork copy of pool can affect
   * it. */
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

  /* Whatever the grandchild did to its copy of pool doesn't touch the
   * parent's copy, because after the fork the two are independent through
   * copy on write. The original task, fork_from_task_then_self_destroy, has
   * already returned in the parent at this point, since fork() returns there
   * immediately. */
  ctpool_wait(pool);
  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* ========================================================================== */
/*                              FORK SAFETY                                   */
/* ========================================================================== */

/* The whole rest of this file exercises the fork() safety machinery in
 * src/cthreadpool.c, which is built on pthread_atfork() and which a build
 * with CCOL_FORK_SAFETY_REQUIRED set to 0 compiles out (see that macro's doc
 * comment in common.h). What these tests assume (that a forked child never
 * inherits a locked ctpool mutex, and that a pool survives as "foreign"
 * instead of going through an unsafe teardown) doesn't hold without that
 * machinery, so the tests are compiled out together with it rather than
 * left in to fail or hang. */
#if CCOL_FORK_SAFETY_REQUIRED

typedef struct {
  _Atomic int stop;
} ctp_fork_stop_arg_t;

/* This thread keeps creating and destroying throwaway ctpool instances that
 * have nothing to do with the pool the main test thread keeps busy below.
 * Its only job is to keep SOME thread inside ctpool_slot_table's mutex as
 * often as it can, through ctpool_create and ctpool_destroy, racing this
 * test's repeated fork() calls. It's an exact mirror of
 * fork_safety_churn_thread in tests/cthreadcomm/tests.c, adapted to ctpool
 * instead of ccol_event_loop. */
static void *ctp_fork_churn_thread(void *arg) {
  ctp_fork_stop_arg_t *a = (ctp_fork_stop_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    ctpool p = ctpool_create(1, 0, &err);
    if (p != CTPOOL_INVALID) ctpool_destroy(p);
  }
  return NULL;
}

typedef struct {
  ctpool pool;
  _Atomic int stop;
} ctp_fork_feeder_arg_t;

/* One background thread that keeps querying the busy pool the main test
 * thread forks against below, which widens the contention window on
 * pool->mu beyond what the trial loop's submit-burst-then-fork technique
 * gives on its own.
 *
 * The sched_yield() after every call is load-bearing and not a nicety. A
 * bare `while (!stop) ctpool_pending_count(pool);` loop reproduces the hang
 * just as reliably on a native run, but it also loops fast enough to run
 * many millions of times even in a short test run. valgrind's memcheck tool
 * gives threads no real parallelism across cores and time-slices all of
 * them through a single instrumented execution engine, so that many
 * iterations of anything, however cheap, make this test extremely slow
 * under valgrind, tens of seconds to several minutes for this one test
 * alone.
 *
 * Yielding after every call caps this thread's call rate at whatever the
 * scheduler's time-slice granularity allows, which is several orders of
 * magnitude fewer calls for the same wall-clock window, and in practice it
 * still contends often enough to keep the race reproducible. The comment on
 * the test below records the measured trade-off this lands on. */
static void *ctp_fork_feeder_thread(void *arg) {
  ctp_fork_feeder_arg_t *a = (ctp_fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    (void)ctpool_pending_count(a->pool);
    sched_yield();
  }
  return NULL;
}

/* The regression test for the fork-safety hang that this module's
 * ctpool_slot_table mutex, and the mu of every live pool, have to be
 * protected against (see the doc comment of _ctpool_atfork_prepare in
 * src/cthreadpool.c for the whole mechanism). fork() duplicates only the
 * calling thread, and several places take a pool's mu: ctpool_submit,
 * ctpool_try_submit, a worker thread picking up or finishing a task, and
 * ctpool_destroy's shutdown and teardown sequence. Without the protection a
 * child inherits that mu already locked, with no thread left alive in the
 * child that could unlock it.
 *
 * The same kind of hazard has its own guard and regression test for the
 * locks of ccol_event_loop (see fork_does_not_inherit_a_locked_event_loop_mutex
 * in tests/cthreadcomm/tests.c), and this module carries the same
 * protection on its own. ccol_event_loop's dispatch_pool is exactly such a
 * ctpool, created whenever a caller configures num_reactor_threads above 1,
 * so it can be reached through this mechanism, and ccol_event_loop has no
 * way into the opaque internals of this module to protect it from outside.
 *
 * A churn thread keeps creating and destroying throwaway ctpool instances,
 * racing ctpool_slot_table's mutex in the background, and it's an exact
 * mirror of the churn thread of
 * fork_does_not_inherit_a_locked_event_loop_mutex. That churn thread by
 * itself is cheap under valgrind, the existing test does the same real
 * create and destroy work for the same duration and runs there in under a
 * second.
 *
 * Separately, one feeder thread throttled with a yield races the busy
 * pool's mu in the background (see the comment on ctp_fork_feeder_thread).
 * Each trial also fires a burst of several real task submissions at that
 * same pool right before it forks, which times the fork() to land while
 * some worker thread may be in the middle of a dequeue, racing to take
 * pool->mu after one of the burst's wakeup signals.
 *
 * The burst is several submissions and not one because a single submit's
 * pool->mu window is too narrow to reliably overlap a fork() right after
 * it: one submit per trial and no feeder thread at all reproduces zero
 * hangs across 300 trials.
 *
 * A feeder thread of another shape, or several of them, with no burst at
 * all doesn't work either. That covers one or more threads calling the
 * real, allocating ctpool_try_submit without a break, and one or more
 * threads calling ctpool_pending_count or ctpool_active_count (which
 * allocate nothing) without a sched_yield() between iterations. Each of
 * those reproduces the hang reliably in well under a second on a native
 * run but gets extremely slow under valgrind, tens of seconds to several
 * minutes for this one test alone, even with the protection in place and
 * no hang to report.
 *
 * A direct experiment, not an assumption, confirmed why. valgrind's
 * memcheck tool gives threads no real parallelism across cores at all and
 * time-slices them all through its own single instrumented execution
 * engine. A loop with no throttle runs as fast as the CPU lets it for the
 * whole test, and that work doesn't spread across cores as it would on a
 * native run; it just hands valgrind a far larger total of instrumented
 * instructions to simulate for the same wall-clock window.
 *
 * The design bounds that cost in two ways. The single feeder thread yields
 * after every lock and unlock, so its call rate is whatever the scheduler's
 * time-slice granularity allows and not the CPU's raw instruction rate.
 * And the trial loop's burst is a small, fixed-size sequence of blocking
 * calls per trial that spins on nothing, so that half of the contention
 * scales with TRIALS rather than with wall-clock duration and pays no such
 * multiplier either. */
TEST(fork_safety, fork_does_not_inherit_a_locked_ctpool_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  ctp_fork_stop_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  bool started_churn_tid =
      (pthread_create(&churn_tid, NULL, ctp_fork_churn_thread, &churn) == 0);

  char *err = NULL;
  ctpool pool = ctpool_create(3, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  atomic_int counter = 0;

  ctp_fork_feeder_arg_t feeder = {.pool = pool, .stop = 0};
  pthread_t feeder_tid;
  bool started_feeder_tid =
      (pthread_create(&feeder_tid, NULL, ctp_fork_feeder_thread, &feeder) == 0);

  /* 60 trials, not hundreds. With the protection turned off the measured
   * hang rate of this configuration is roughly 10 to 20 percent of trials
   * (to measure it, empty the bodies of _ctpool_atfork_prepare and
   * _ctpool_atfork_release, keep both registered and run this exact test
   * dozens of times). At that rate the odds of a full run seeing zero hangs
   * by pure chance are already well under 1 percent, while hundreds of
   * trials would add that many fork() and waitpid() pairs to every ordinary
   * test run that finds no hang. */
  enum { TRIALS = 60 };
  /* Several submissions right before each fork() and not one, because a
   * single submit's pool->mu window is too narrow to reliably overlap a
   * fork() right after it, one submit per trial alone reproduces zero hangs
   * across 300 trials. A burst of several gives several independent workers
   * a dequeue race that starts at almost the same instant, which multiplies
   * the odds that at least one of them is inside pool->mu at the exact
   * instant of the fork(), and needs no background thread that spins for
   * ever (this test's comment above says why such a thread is too slow
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
      /* Bounds the child's lifetime in case the hazard this test guards
       * against fires, so it fails instead of hanging the whole suite, and
       * the parent below tells that apart from a clean exit with
       * WIFEXITED.
       *
       * The bound is 5 seconds and not 1. A real deadlock on an inherited
       * locked mutex hangs for ever whatever bound we pick, so the larger
       * bound only buys headroom against ordinary scheduling delay after a
       * fork. A busy, oversubscribed or virtualized CI host can leave a
       * freshly forked child unscheduled for more than a second with nothing
       * wrong, and the larger bound weakens nothing this test catches.
       *
       * _diag_arm installs a diagnostic SIGALRM handler instead of relying
       * on the default disposition that kills on SIGALRM (see that
       * section's doc comment), so a real hang reports WHERE the child was
       * stuck and not only THAT it was stuck. */
      _diag_arm(diagfd[1]);

      /* Exactly the call shape a real application uses right after it
       * inherits a pool across a fork (ctpool_submit or ctpool_try_submit,
       * then submit_internal, then _ccol_mutex_lock(pool->mu)), and so the
       * thing this test has to prove safe. */
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
  /* Set right before the unlock, so it is true whenever another thread can
   * take the lock. */
  _Atomic bool released;
  int hold_ms;
} ctpool_slot_table_fork_lock_arg_t;

static void *ctpool_slot_table_fork_lock_thread(void *arg) {
  ctpool_slot_table_fork_lock_arg_t *a =
      (ctpool_slot_table_fork_lock_arg_t *)arg;
  ctpool_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  /* This thread lets go on a fixed schedule of its own, independent of
   * anything the forking thread does below (see queue_fork_lock_thread in
   * tests/cthreadcomm/tests.c for the same reasoning). The forking thread
   * must never be the one that tells this thread to let go, because then
   * the two would wait on each other in a real cycle. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&a->released, true);
  ctpool_test_wrunlock_slot_table_for_tests();
  return NULL;
}

/* The regression test for the write-lock hazard of
 * ctpool_slot_table.rwlock, which glibc tracks by thread ID (TID), as the
 * comment on the in_child branch of _ctpool_atfork_release_impl describes.
 *
 * ctpool_slot_table's lock is a _ccol_rw_lock_t, which keeps concurrent
 * _ctpool_resolve calls (the hottest path of this module under a caller
 * that submits a task per request, such as chttpserver) off one shared
 * lock.
 *
 * Any thread that calls ctpool_create_mp or __ctpool_destroy can take the
 * write side of that lock, not only the thread that later calls fork().
 * glibc's rwlock tracks its write lock's owner by TID, so a plain
 * _ccol_rw_lock_unlock from the child's surviving thread, whose TID is
 * different, would fail silently. It wouldn't release a lock that a
 * different, now gone, thread took, and every later _ctpool_resolve in that
 * child would hang.
 *
 * This is an exact mirror of ccol_event_loop's
 * fork_does_not_inherit_a_write_locked_reg_slot_rwlock in
 * tests/cthreadcomm/tests.c, with ctpool_slot_table.rwlock in place of
 * ccol_event_loop's reg_slot_rwlock. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_ctpool_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  ctpool pool = ctpool_create(2, 0, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);

  enum { HOLD_MS = 300 };
  ctpool_slot_table_fork_lock_arg_t arg = {
      .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(
      pthread_create(&holder, NULL, ctpool_slot_table_fork_lock_thread, &arg),
      0);

  while (!atomic_load(&arg.locked)) {
    /* See the same spin-wait reasoning in
     * fork_does_not_inherit_a_locked_ctpool_mutex for why sched_yield() and
     * not a bare spin matters under valgrind. */
    sched_yield();
  }

  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` doesn't exist here, because fork() duplicates only the
     * calling thread. This child can only exist once the parent's fork()
     * call has returned, which needs the
     * _ccol_rw_lock_wrlock(ctpool_slot_table.rwlock) inside
     * _ctpool_atfork_prepare to succeed first, so the holder thread (gone in
     * this process) must already have released the lock.
     *
     * With a plain _ccol_rw_lock_unlock in the child instead of the reinit,
     * this process would inherit ctpool_slot_table.rwlock write-locked with
     * no thread that could ever release it, and the resolve inside
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

  /* Shows that the atfork prepare handler really blocks. The holder took the
   * write lock before this thread called fork(), and the prepare handler
   * can't take it until the holder unlocks, which the holder only does after
   * setting `released`. So when fork() returns `released` is true, however
   * fast or slow either thread runs. If the prepare handler didn't take the
   * lock, fork() would return while the holder is still sleeping out its
   * HOLD_MS and this assertion would fail, unless this thread was so slow
   * itself that it called fork() after the hold ended. We read the value
   * here and assert on it after the child has been reaped. */
  bool released_before_fork_returned = atomic_load(&arg.released);
  /* The holder ends on its own schedule, so the join is bounded, and it runs
   * before the assertions below so none of them leaves the holder
   * unjoined. */
  pthread_join(holder, NULL);

  /* This read is short and bounded. If the child hangs past alarm(3) and is
   * killed before writing anything, its copy of the write end closes with
   * it and this read returns 0 (end of file) instead of blocking for ever,
   * since the parent has already closed its own copy above. */
  char byte = 0;
  ssize_t n = read(result_pipe[0], &byte, 1);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(released_before_fork_returned);

  /* The parent's ctpool_slot_table.rwlock must be usable after all of the
   * above. The parent's release path is a plain _ccol_rw_lock_unlock and not
   * the reinit the child runs, and since this same thread's fork() call
   * validly released that lock, the unlock works. */
  atomic_int counter_after = 0;
  REQUIRE_EQ(ctpool_try_submit(pool, inc_counter, &counter_after, NULL),
             ccol_success);

  ctpool_shutdown_drain(pool);
  ctpool_destroy(pool);
}

/* Regression coverage for the foreign_since_fork branch of
 * _ctpool_teardown_raw (see the comment there in src/cthreadpool.c for the
 * whole story).
 *
 * That branch must not free pool->threads and the pool struct while leaving
 * pool->head and pool->tail alone. That would silently leak every queued
 * ctpool_task, and for a queued FUTURE task it would never call
 * future_cancel, so a caller in the child holding a pointer to that future
 * and calling ctpool_future_get() on it would block for ever, waiting for a
 * worker that will never exist in this process.
 *
 * So the branch discards the queue before it frees anything and cancels any
 * future attached to it, exactly as ctpool_shutdown_immediate does for a
 * live pool.
 *
 * It's also exactly the call shape that __ctpool_destroy's doc comment
 * promises to support, where a drain shutdown runs first when neither
 * ctpool_shutdown_drain nor ctpool_shutdown_immediate ran before: the test
 * destroys a foreign pool directly, with no shutdown call in front of
 * it. */
TEST(
    fork_safety,
    destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = ctpool_create(1, 1, &err);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  ctpool_submit(pool, blocker_fn, &gctx, NULL);
  while (!atomic_load(&started)) sleep_ms(1);

  int dummy = 0;
  ctpool_future *f = ctpool_submit_future(pool, identity_fn, &dummy, NULL);
  REQUIRE_NE((void *)f, NULL); /* queued behind the blocked worker */

  /* The child reports its outcome through a pipe and not its exit code.
   * Under make memtest valgrind replaces a forked child's real exit code
   * with its own --error-exitcode as soon as it finds ANY still reachable
   * allocation in the child's inherited image at exit, and every child
   * forked in the middle of a suite has one, since the rest of this suite
   * hasn't settled yet. So the exit code can't carry this result reliably.
   *
   * See the fork_safety group of tests/clogger/tests.c, which sets out the
   * same reasoning, and the fork_does_not_inherit_a_locked_ctpool_mutex test
   * above in this file, which checks only WIFEXITED for the same reason. */
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
    /* Bounds this child's lifetime in case the hazard this test guards
     * against fires after all, instead of hanging the whole suite. */
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

  /* Release the parent's worker and tear down the parent's live pool, both
   * before any assertion below, otherwise a real regression here would also
   * leak a worker thread that blocks for ever into the rest of the suite.
   * Whatever the child did leaves the parent's copies of pool and f alone,
   * because the child works on its own independent copy made by copy on
   * write. */
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

/* ctpool_wait needs a foreign_since_fork guard of its own, just like
 * _ctpool_shutdown_drain_internal and _ctpool_shutdown_immediate_internal,
 * which each use theirs to skip joining a foreign pool's worker threads
 * that don't exist.
 *
 * Without the guard, a ctpool_wait in a forked child on an inherited pool
 * whose active_count or queue_size wasn't zero at the instant of the fork()
 * hangs for ever, because no worker thread exists in this process to
 * decrement active_count, drain queue_size or broadcast idle_cv again.
 * That's why the library treats a foreign pool as idle by default. */
TEST(fork_safety, wait_on_foreign_pool_with_pending_work_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  ctpool pool = ctpool_create(1, 0, &err);
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

  /* Release the parent's worker and tear down the parent's live pool, both
   * before the assertions below, otherwise a real regression here would
   * also leak a worker thread that blocks for ever into the rest of the
   * suite. The parent's worker is blocked at this point and the child
   * didn't touch it. */
  atomic_store(&gate, 1);
  ctpool_shutdown_drain(pool);
  int final_counter = atomic_load(&counter);
  ctpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  /* No WEXITSTATUS here, because under make memtest valgrind replaces a
   * forked child's real exit code with its own --error-exitcode as soon as
   * it finds any still reachable allocation in the child's inherited image
   * at exit, and every child forked in the middle of a suite has one.
   *
   * So WIFEXITED is the only part of the exit status this test can trust.
   * It catches a real regression that hangs, since the alarm above turns
   * that into WIFSIGNALED, and it catches a real crash as well.
   *
   * See the fork_safety group of tests/clogger/tests.c, which sets out the
   * same reasoning, and the fork_does_not_inherit_a_locked_ctpool_mutex test
   * above in this file, which checks only WIFEXITED for the same reason. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(final_counter, 1);
}

#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/* Allocation-failure sweep over pool construction                            */
/*                                                                            */
/* ctpool_create_mp builds four things before it returns: a */
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

/* Deep enough to get past the last allocation that construction makes. */
#define CTP_SWEEP_DEPTH 24

TEST(ctpool_oom, bounded_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= CTP_SWEEP_DEPTH; n++) {
    _ctp_arm(n);
    char *err = NULL;
    ctpool pool = ctpool_create_mp(2, 8, &g_ctp_sweep_mp, &err);
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
    ctpool pool = ctpool_create_mp(1, 0, &g_ctp_sweep_mp, &err);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID)
      ctpool_destroy(pool);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctpool_oom, many_worker_pool_construction_unwinds_at_every_allocation) {
  /* More workers mean more per-thread allocations, which reach failure
   * points the two-thread sweeps above never get to. */
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _ctp_arm(n);
    ctpool pool = ctpool_create_mp(8, 16, &g_ctp_sweep_mp, NULL);
    _ctp_disarm();
    if (pool != CTPOOL_INVALID) ctpool_destroy(pool);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(ctpool_oom, a_pool_built_under_a_late_failure_still_runs_work) {
  /* Whatever survives the sweep must be a usable pool, not merely a
   * non-NULL handle, that's what tells "unwound correctly" apart from
   * "returned a corpse". */
  bool built = false, ran = false;
  for (int n = 30; n <= 60 && !built; n++) {
    _ctp_arm(n);
    ctpool pool = ctpool_create_mp(2, 4, &g_ctp_sweep_mp, NULL);
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

/* Sleeps for ms milliseconds. It paces the bounded poll loops and, in one
 * place below, gives a wrong implementation the time to return as well, but
 * no property under test depends on it as a mechanism. */
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

/* Keeps the pool's only worker busy until the main test thread opens the
 * gate. The bound of 60 real seconds is only a safety net against a hang
 * and never the mechanism for correctness.
 *
 * Every test below asserts that no shutdown call can have returned while
 * the gate is shut, and that follows from this worker being here, not from
 * any elapsed time. */
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
  /* Both shutdown entry points are documented to block until the pool has
   * really stopped. The library sets the "a shutdown started" flag before it
   * wakes any worker, and long before it joins one, so a call that sees the
   * flag and returns would give application code a pool whose workers are
   * all running. A caller that then frees the state its tasks use has a
   * use-after-free in its own memory, with no return value to warn it.
   *
   * The proof here is structural and not a time margin. The pool's only
   * worker is inside a task that can only exit through a gate this test
   * hasn't opened yet, so no shutdown call on this pool can have joined that
   * worker and none of them may have returned. The pause below only gives an
   * implementation that returns early the time to do it, and a longer pause
   * can never make a correct implementation fail.
   *
   * Make either entry point return as soon as it
   * finds a shutdown already started, and the test fails. */
  ctp_sd_fixture_reset();
  ctpool pool = ctpool_create(1, 0, NULL);
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

  /* Released unconditionally and before any assertion, so every thread that
   * really started can finish and be joined even if something above went
   * wrong. */
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
  /* Asking for an immediate shutdown while a drain runs must discard what
   * the queue holds and cancel those tasks' futures. Behaving like a drain
   * instead would be silent, since both entry points return void and a
   * caller that asked for the queue to be thrown away couldn't find out that
   * the library ran it instead.
   *
   * We make the drain arrive first by waiting for it to take ownership, read
   * directly off the pool, before starting the immediate call.
   *
   * If the immediate entry point returned as soon
   * as it found a shutdown already started, this test would fail. */
  enum { EXTRA_TASKS = 4 };
  ctp_sd_fixture_reset();
  ctpool pool = ctpool_create(1, 0, NULL);
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
    futures[i] =
        ctpool_submit_future(pool, ctp_sd_extra_future_task, NULL, NULL);
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

  /* The queue empties while the first task holds the pool's only worker,
   * which shows the escalation happened, because nothing else can consume
   * those tasks yet. */
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
   * handle table's free list. When that push can't allocate, the index names
   * a slot that is fully released and referenced by nothing, and without
   * recovery that slot stays unusable for the rest of the process, so every
   * later create grows the table by one more.
   *
   * We run enough cycles that the free list we start with can't absorb
   * them. With recovery the table grows by one at most, from the first
   * cycle, and only when the free list was empty and nothing had been lost
   * yet; without recovery it grows by one per cycle.
   *
   * Drop a failed push on the floor and the table
   * grows by CYCLES and the test fails. */
  size_t before = _ctpool_slot_table_capacity_for_tests();
  size_t free_before = _ctpool_free_index_count_for_tests();
  size_t cycles = free_before + 8;

  bool all_ok = true;
  for (size_t i = 0; i < cycles && all_ok; i++) {
    ctpool pool = ctpool_create(1, 0, NULL);
    if (pool == CTPOOL_INVALID) {
      all_ok = false;
      break;
    }
    _ctpool_force_next_free_index_push_failure_for_tests();
    ctpool_destroy(pool);
  }

  /* This pool must come from a slot the recovery reclaimed, and it has to
   * be fully usable, since a reclaimed index carrying stale records would
   * break on its first call. */
  ctp_sd_fixture_reset();
  ctpool pool = ctpool_create(1, 0, NULL);
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

/* A task that parks until its gate opens, with a bound, so a test that fails
 * early can never leave a worker parked for good. */
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
 * an unbounded queue. Every task must run, and no submitter or worker may
 * stay asleep while there is work for it. A lost wake parks a thread for
 * good, so the wait for the full count is bounded, and on a timeout an
 * immediate shutdown wakes every parked thread before the test joins them. */
static bool ctp_signal_stress(size_t queue_cap, long per_submitter) {
  enum { SUBMITTERS = 6 };
  ctp_gate g = {0};
  _Atomic bool abort_run = false;
  ctpool pool = ctpool_create(8, queue_cap, NULL);
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

/* Runs one timed submit against a full queue of capacity 1 with the only
 * worker parked, and opens the gate after hold_ms. Returns the timed
 * submit's result and tells whether that submit returned before the gate
 * opened. */
static ccol_retval_t ctp_timed_submit_on_full_queue(uint64_t timeout_us,
                                                    int hold_ms,
                                                    bool *returned_early) {
  *returned_early = false;
  ctp_gate g = {0};
  ctpool pool = ctpool_create(1, 1, NULL);
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

/* A timeout that reaches past the largest time_t is a wait for ever and not
 * a deadline that wrapped into the past. With an
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

/* A budget that isn't a whole number of seconds lasts for all of it.
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

/* Waits on raw's not_full until a deadline 100 ms ahead on CLOCK_MONOTONIC
 * and returns how long the wait took, or -1 when it didn't time out. A
 * condition variable on CLOCK_REALTIME would read that deadline as a moment
 * decades in the past and return without waiting. */
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
 * that an administrator or NTP steps backwards can't make a timed submit
 * longer. On a condition variable with the default
 * CLOCK_REALTIME the wait returns at once. */
TEST(ctpool_timed_submit, not_full_measures_deadlines_on_clock_monotonic) {
  ctpool pool = ctpool_create(1, 1, NULL);
  REQUIRE_NE(pool, CTPOOL_INVALID);
  long long elapsed = ctp_not_full_wait_ms(_ctpool_resolve_for_tests(pool));
  ctpool_destroy(pool);
  REQUIRE_GE(elapsed, 80LL);
}

/* A timed submit on a full queue returns ccol_timed_out when its budget runs
 * out on CLOCK_MONOTONIC. A deadline computed on
 * CLOCK_REALTIME against a CLOCK_MONOTONIC condition variable lies decades
 * ahead, and then the submit waits until the test opens the gate. */
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
/* In a fork() child the not_full of an inherited pool is initialised again,
 * and it must use the same clock as the ordinary init. The child reports
 * through a pipe and not its exit status, which valgrind can replace: '1'
 * when the wait timed out on the CLOCK_MONOTONIC deadline, '0' otherwise. */
TEST(ctpool_timed_submit, not_full_keeps_clock_monotonic_in_a_forked_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  ctpool pool = ctpool_create(1, 1, NULL);
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

/* ctpool_destroy evaluates its argument once, so walking backwards over an
 * array with `ctpool_destroy(a[--k])` destroys and clears every pool. A
 * macro that evaluated its argument twice would
 * destroy only every second pool and clear the others without destroying
 * them. */
TEST(ctpool_macros, destroy_evaluates_its_argument_once) {
  ctpool pools[2] = {ctpool_create(1, 4, NULL), ctpool_create(1, 4, NULL)};
  bool created = pools[0] != CTPOOL_INVALID && pools[1] != CTPOOL_INVALID;
  int k = 2;
  while (k > 0) ctpool_destroy(pools[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(pools[0], CTPOOL_INVALID);
  REQUIRE_EQ(pools[1], CTPOOL_INVALID);
}
