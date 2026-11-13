/*
MIT License

Copyright (c) 2026 - A bunch of nerds

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include <common.h>
#include <common_invariants.h>
#include <cthreadpool.h>
#include <dirent.h>
#include <errno.h>
// TEST_HAS_EXECINFO is 1 when backtrace() exists, so the fork hang diagnostics
// can capture frames; musl has none, and the diagnostics then capture
// nothing.
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
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <test_threads.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Resets the signal state inherited from the parent process, since the forked
// children of this binary report through their wait status.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// Fork hang diagnostics for fork_does_not_inherit_a_locked_cthrpool_mutex. Its
// alarm can fire in a forked child; the handler then writes the raw stack
// addresses down a pipe (diag_write_fd) and the parent resolves them, because a
// symbol resolver could take the very lock the child is stuck on. The block
// shares the CCOL_FORK_SAFETY_REQUIRED switch with its only callers.
#if CCOL_FORK_SAFETY_REQUIRED
static volatile sig_atomic_t diag_write_fd = -1;

// Calls backtrace() once so its lazy setup is done before the signal handler
// needs it.
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

// The SIGALRM handler of a stuck child: Writes the raw frames to diag_write_fd
// and exits with 66. It only calls async-signal-safe functions.
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
  _exit(66);
}

// Installs the SIGALRM handler and starts a 5 s alarm in the child; write_fd is
// the pipe it reports through.
static void _diag_arm(int write_fd) {
  diag_write_fd = write_fd;
  struct sigaction sa = {0};
  sa.sa_handler = _diag_alarm_handler;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGALRM, &sa, NULL);
  alarm(5);
}

// Reads the frames a stuck child wrote to read_fd, resolves and prints them.
// Runs in the parent; an empty pipe prints nothing.
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
#endif

// Sleeps ms milliseconds; the slow tasks and the polls of the tests use it.
static void sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// A task that increments the atomic_int at arg, so a test can count the
// tasks that ran.
static void inc_counter(void *arg) { atomic_fetch_add((atomic_int *)arg, 1); }

// A task that sleeps 20 ms and then increments the atomic_int at arg, so tasks
// are still queued when a shutdown or a wait starts.
static void slow_inc(void *arg) {
  sleep_ms(20);
  atomic_fetch_add((atomic_int *)arg, 1);
}

// A task that sets started and then spins until gate opens.
typedef struct {
  atomic_int *gate;
  atomic_int *started;
} gate_ctx_t;

// Sets started and spins until the gate opens, to hold a worker as long as a
// test needs.
static void blocker_fn(void *arg) {
  gate_ctx_t *ctx = (gate_ctx_t *)arg;
  atomic_store(ctx->started, 1);
  while (!atomic_load(ctx->gate)) sleep_ms(1);
}

// A thread that opens the gate at arg after 20 ms. The test doesn't depend on
// the delay, since the drain runs every queued task whenever the gate opens.
static void *release_gate_fn(void *arg) {
  sleep_ms(20);
  atomic_store((atomic_int *)arg, 1);
  return NULL;
}

// The releaser of the shutdown_immediate tests: It opens the gate only once the
// shutdown has set its immediate flag. The pool sets that flag and takes the
// queue in one critical section, so the worker can't dequeue a task that the
// shutdown must discard. After 10 s it opens the gate anyway.
typedef struct {
  atomic_int *gate;
  struct cthread_pool *raw;
} immediate_release_ctx_t;

// Waits (bounded) for the immediate flag, then opens the gate, so the held
// worker can't take a task the shutdown must discard.
static void *release_gate_after_immediate_fn(void *arg) {
  immediate_release_ctx_t *ctx = (immediate_release_ctx_t *)arg;
  bool immediate = false;
  for (int i = 0; i < 10000 && ctx->raw && !immediate; i++) {
    cthrpool__shutdown_state_for_tests(ctx->raw, NULL, &immediate, NULL);
    if (!immediate) sleep_ms(1);
  }
  atomic_store(ctx->gate, 1);
  return NULL;
}

// A thread that calls cthrpool_wait on the pool whose handle arg points to (a
// cthrpool is a value, so it travels by address).
static void *pool_wait_thread(void *arg) {
  cthrpool *p = (cthrpool *)arg;
  cthrpool_wait(*p);
  return NULL;
}

// A producer thread that submits n increments of counter to pool.
typedef struct {
  cthrpool pool;
  atomic_int *counter;
  int n;
} producer_arg_t;

// Submits n inc_counter tasks, so several threads can submit at once.
static void *producer_thread(void *arg) {
  producer_arg_t *a = (producer_arg_t *)arg;
  for (int i = 0; i < a->n; i++) {
    cthrpool_submit(a->pool, inc_counter, a->counter, NULL);
  }
  return NULL;
}

// A future task that gives back arg, so a test can check the result it gets.
static void *identity_fn(void *arg) { return arg; }

// A future task that sleeps 20 ms and gives back arg, so a get has to block.
static void *slow_identity(void *arg) {
  sleep_ms(20);
  return arg;
}

// A future task held by a gate: It gives result only after the gate opens.
typedef struct {
  atomic_int *gate;
  void *result;
} gated_future_ctx_t;

// Spins until the gate opens and gives result, so the future can't complete
// before the test releases it.
static void *gated_identity(void *arg) {
  gated_future_ctx_t *ctx = (gated_future_ctx_t *)arg;
  while (!atomic_load(ctx->gate)) sleep_ms(1);
  return ctx->result;
}

// The multi-waiter future tests: The task returns only once release is set, so
// every waiter stays inside cthrpool_future_get as long as the test needs.
// results_ok counts the waiters that got expected_result.
typedef struct {
  cthrpool_future *f;
  void *expected_result;
  atomic_int release;
  atomic_int results_ok;
} future_waiters_ctx_t;

// The task of the multi-waiter tests: Waits for release, then gives
// expected_result.
static void *waiters_gated_fn(void *arg) {
  future_waiters_ctx_t *c = (future_waiters_ctx_t *)arg;
  while (!atomic_load(&c->release)) sleep_ms(1);
  return c->expected_result;
}

// Waits on the shared future and counts a correct result, as one of many
// concurrent waiters.
static void *future_waiter_thread(void *arg) {
  future_waiters_ctx_t *c = (future_waiters_ctx_t *)arg;
  void *result = cthrpool_future_get(c->f);
  if (result == c->expected_result) atomic_fetch_add(&c->results_ok, 1);
  return NULL;
}

// Waits up to max_ms for f's refcount to reach target and gives the last value
// read. A waiter takes its reference inside a call that blocks, so asking the
// future is the only way to know it's there. The bound is a safety net only.
static int wait_for_future_refcount(cthrpool_future *f, int target,
                                    int max_ms) {
  int observed = cthrpool__future_refcount_for_tests(f);
  for (int waited = 0; observed != target && waited < max_ms; waited++) {
    sleep_ms(1);
    observed = cthrpool__future_refcount_for_tests(f);
  }
  return observed;
}

// A task that does nothing, so a test can watch on_complete alone.
static void noop_fn(void *arg) { (void)arg; }

// The self-submit tests: A task or on_complete submits twice into its own
// pool, and the two results (and futures) are recorded here.
typedef struct {
  cthrpool pool;
  atomic_int first_rc;
  atomic_int second_rc;
  cthrpool_future *first_future;
  cthrpool_future *second_future;
} self_submit_ctx_t;

// Makes the two plain submits and records their results; the first fills the
// one-slot queue and the second must not wait.
static void self_submit_twice(self_submit_ctx_t *c) {
  atomic_store(&c->first_rc,
               (int)cthrpool_submit(c->pool, noop_fn, NULL, NULL));
  atomic_store(&c->second_rc,
               (int)cthrpool_submit(c->pool, noop_fn, NULL, NULL));
}

// A task that submits twice into its own pool.
static void self_submit_task_fn(void *arg) {
  self_submit_twice((self_submit_ctx_t *)arg);
}

// An on_complete that submits twice into its own pool.
static void self_submit_complete_fn(void *arg, bool ran) {
  (void)ran;
  self_submit_twice((self_submit_ctx_t *)arg);
}

// A task that makes two future submits into its own pool.
static void self_submit_future_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  ccol_retval rv = ccol_unexpected_failure;
  c->first_future = cthrpool_submit_future(c->pool, identity_fn, NULL, &rv);
  atomic_store(&c->first_rc, (int)rv);
  rv = ccol_unexpected_failure;
  c->second_future = cthrpool_submit_future(c->pool, identity_fn, NULL, &rv);
  atomic_store(&c->second_rc, (int)rv);
}

// Milliseconds on CLOCK_MONOTONIC, for the deadlines of bounded waits.
static long long ctp_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

// Waits up to max_ms on the clock for *flag to become non-zero and gives the
// last value read, so a precondition that never comes true fails an assertion
// instead of hanging.
static int wait_for_flag(atomic_int *flag, int max_ms) {
  long long end = ctp_now_ms() + max_ms;
  int value = atomic_load(flag);
  while (!value && ctp_now_ms() < end) {
    sleep_ms(1);
    value = atomic_load(flag);
  }
  return value;
}

// Waits up to 10 s for *flag and tells whether it was set.
static bool ctp_await_flag(atomic_int *flag) {
  return wait_for_flag(flag, 10000) != 0;
}

// The way out of a test whose held task didn't start in time: Opens the gate,
// discards the queue and destroys the pool before the test's frame goes away.
static void ctp_abandon_gated_pool(cthrpool *pool, atomic_int *gate) {
  atomic_store(gate, 1);
  cthrpool_shutdown_immediate(*pool);
  cthrpool_destroy(*pool);
}

// The cross-pool test: A task on one pool makes a blocking submit into another
// pool that is full for a while.
typedef struct {
  cthrpool other;
  atomic_int submitting;
  atomic_int rc;
} cross_pool_ctx_t;

// Signals that it's submitting and makes a blocking submit into the other
// pool, which must still wait for room.
static void cross_pool_task_fn(void *arg) {
  cross_pool_ctx_t *c = (cross_pool_ctx_t *)arg;
  atomic_store(&c->submitting, 1);
  atomic_store(&c->rc, (int)cthrpool_submit(c->other, noop_fn, NULL, NULL));
}

// A phase 1 task: Sleeps 10 ms and increments the counter, so a wait between
// the phases has something to wait for.
static void phase1_fn(void *arg) {
  sleep_ms(10);
  atomic_fetch_add((atomic_int *)arg, 1);
}

// A phase 2 task, which checks that every phase 1 task has finished.
typedef struct {
  atomic_int *p1_count;
  atomic_int *ok;
  int expected_p1;
} phase2_ctx_t;

// Clears ok when fewer than expected_p1 phase 1 tasks are done, which shows a
// wait that returned early.
static void phase2_fn(void *arg) {
  phase2_ctx_t *c = (phase2_ctx_t *)arg;
  if (atomic_load(c->p1_count) < c->expected_p1) {
    atomic_store(c->ok, 0);
  }
}

// A task and an on_complete that check their order.
typedef struct {
  atomic_int *fn_done;
  atomic_int *cb_done;
} cb_ctx_t;

// Marks fn as done, so on_complete can check that it runs after fn.
static void cb_fn(void *arg) { atomic_store(((cb_ctx_t *)arg)->fn_done, 1); }

// Records 1 when fn had finished and ran is true, 2 otherwise. It runs on a
// worker, so it records instead of asserting.
static void cb_cb(void *arg, bool ran) {
  bool ok = ran && atomic_load(((cb_ctx_t *)arg)->fn_done) == 1;
  atomic_store(((cb_ctx_t *)arg)->cb_done, ok ? 1 : 2);
}

// An allocator that counts its allocations and frees, to see which calls reach
// the pool's custom procs.
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

// An allocator that fails every allocation while g_oom_enabled is set. Turn it
// on after the pool exists, so the pool itself gets real memory.
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

TEST(construction, unbounded_default) {
  cthrpool_construct(pool, 2, 0);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(construction, bounded) {
  cthrpool_construct(pool, 4, 64);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(construction, ccol_invalid_size_is_refused) {
  char *err = NULL;
  cthrpool pool = cthrpool_create(2, ccol_invalid_size, &err);
  bool refused = pool == CTHRPOOL_NULL;
  if (!refused) cthrpool_destroy(pool);
  REQUIRE_TRUE(refused);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "ccol_invalid_size"), NULL);
}

TEST(construction, ccol_invalid_size_is_refused_before_any_allocation) {
  atomic_store(&g_alloc_count, 0);
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc,
                           .free = tracked_free};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(2, ccol_invalid_size, &mp, &err);
  bool refused = pool == CTHRPOOL_NULL;
  if (!refused) cthrpool_destroy(pool);
  REQUIRE_TRUE(refused);
  REQUIRE_EQ(atomic_load(&g_alloc_count), 0);
}

TEST(construction, largest_finite_capacity_is_bounded) {
  char *err = NULL;
  cthrpool pool = cthrpool_create(1, ccol_invalid_size - 1, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(construction, zero_threads_fails) {
  char *err = NULL;
  cthrpool pool = cthrpool_create(0, 0, &err);
  REQUIRE_EQ(pool, CTHRPOOL_NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(construction, destroy_without_explicit_shutdown_drains) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 0);
  cthrpool_submit(pool, inc_counter, &counter, NULL);
  cthrpool_submit(pool, inc_counter, &counter, NULL);
  cthrpool_destroy(pool);
  REQUIRE_EQ(atomic_load(&counter), 2);
}

TEST(construction, construct_scoped_auto_destroys) {
  atomic_int counter = 0;
  {
    cthrpool_construct_scoped(pool, 2, 0);
    cthrpool_submit(pool, inc_counter, &counter, NULL);
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }
  REQUIRE_EQ(atomic_load(&counter), 2);
}

TEST(construction, declare_scoped_auto_destroys) {
  atomic_int counter = 0;
  {
    cthrpool_declare_scoped(pool);
    char *err = NULL;
    pool = cthrpool_create(2, 0, &err);
    REQUIRE_NE(pool, CTHRPOOL_NULL);
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }
  REQUIRE_EQ(atomic_load(&counter), 1);
}

// Reads the thread count until it falls back to want, for at most 5 s; a joined
// thread can stay listed for a moment after its join.
static int thread_count_settled(int want) {
  long long end = ctp_now_ms() + 5000;
  int n = test_thread_count();
  while (n != want && ctp_now_ms() < end) {
    sleep_ms(1);
    n = test_thread_count();
  }
  return n;
}

// Reads the thread count until it holds still for 50 ms (at most 5 s), as a
// baseline that the previous test's joined workers don't disturb.
static int thread_count_baseline(void) {
  long long end = ctp_now_ms() + 5000;
  int n = test_thread_count();
  long long still_since = ctp_now_ms();
  while (ctp_now_ms() - still_since < 50 && ctp_now_ms() < end) {
    sleep_ms(1);
    int m = test_thread_count();
    if (m != n) {
      n = m;
      still_since = ctp_now_ms();
    }
  }
  return n;
}

// Each failure must give CTHRPOOL_NULL with its own err_str, start no worker
// and return every byte to the caller's allocator; the next create works.
TEST(construction, a_failed_primitive_init_fails_cleanly) {
  static const struct {
    cthrpool__fault step;
    const char *err;
  } cases[] = {
      {CTHRPOOL__FAULT_POOL_MUTEX, "mutex init failed"},
      {CTHRPOOL__FAULT_POOL_NOT_EMPTY, "cond_var init failed"},
      {CTHRPOOL__FAULT_POOL_NOT_FULL, "cond_var init failed"},
      {CTHRPOOL__FAULT_POOL_IDLE_CV, "cond_var init failed"},
      {CTHRPOOL__FAULT_POOL_SHUTDOWN_CV, "cond_var init failed"},
  };
  enum { N = sizeof(cases) / sizeof(cases[0]) };
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc,
                           .free = tracked_free};
  int threads_before = thread_count_baseline();
  bool refused[N];
  bool err_ok[N];
  int allocs[N];
  int frees[N];
  int threads_after[N];
  for (size_t i = 0; i < N; i++) {
    atomic_store(&g_alloc_count, 0);
    atomic_store(&g_free_count, 0);
    char *err = NULL;
    cthrpool__arm_fault_for_tests(cases[i].step);
    cthrpool pool = cthrpool_create_mp(2, 8, &mp, &err);
    cthrpool__arm_fault_for_tests(CTHRPOOL__FAULT_NONE);
    refused[i] = pool == CTHRPOOL_NULL;
    if (!refused[i]) cthrpool_destroy(pool);
    err_ok[i] = err && strstr(err, cases[i].err) != NULL;
    allocs[i] = atomic_load(&g_alloc_count);
    frees[i] = atomic_load(&g_free_count);
    threads_after[i] = thread_count_settled(threads_before);
  }
  atomic_int counter = 0;
  cthrpool pool = cthrpool_create_mp(2, 8, &mp, NULL);
  ccol_retval r = ccol_unexpected_failure;
  if (pool != CTHRPOOL_NULL) {
    r = cthrpool_submit(pool, inc_counter, &counter, NULL);
    cthrpool_wait(pool);
    cthrpool_destroy(pool);
  }

  for (size_t i = 0; i < N; i++) {
    REQUIRE_TRUE(refused[i]);
    REQUIRE_TRUE(err_ok[i]);
    REQUIRE_GT(allocs[i], 0);
    REQUIRE_EQ(allocs[i], frees[i]);
    REQUIRE_EQ(threads_after[i], threads_before);
  }
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(atomic_load(&counter), 1);
}

// Each worker pauses 200 ms before its last statement, so a create that left
// the started workers running would return while they are still counted.
TEST(construction, a_failed_worker_start_joins_the_started_workers) {
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc,
                           .free = tracked_free};
  int threads_before = thread_count_baseline();
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  char *err = NULL;
  long running_before = atomic_load(&cthrpool__running_workers_for_tests);
  atomic_store(&cthrpool__worker_exit_delay_us_for_tests, 200000u);
  cthrpool__fail_worker_create_at_for_tests(3);
  cthrpool pool = cthrpool_create_mp(5, 0, &mp, &err);
  cthrpool__fail_worker_create_at_for_tests(SIZE_MAX);
  long running_at_return = atomic_load(&cthrpool__running_workers_for_tests);
  atomic_store(&cthrpool__worker_exit_delay_us_for_tests, 0u);
  bool refused = pool == CTHRPOOL_NULL;
  if (!refused) cthrpool_destroy(pool);
  int allocs = atomic_load(&g_alloc_count);
  int frees = atomic_load(&g_free_count);
  int threads_after = thread_count_settled(threads_before);

  atomic_int counter = 0;
  cthrpool again = cthrpool_create_mp(5, 0, &mp, NULL);
  ccol_retval r = ccol_unexpected_failure;
  if (again != CTHRPOOL_NULL) {
    r = cthrpool_submit(again, inc_counter, &counter, NULL);
    cthrpool_wait(again);
    cthrpool_destroy(again);
  }

  REQUIRE_TRUE(refused);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "pthread_create failed"), NULL);
  REQUIRE_EQ(running_at_return, running_before);
  REQUIRE_GT(allocs, 0);
  REQUIRE_EQ(allocs, frees);
  REQUIRE_EQ(threads_after, threads_before);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_EQ(atomic_load(&counter), 1);
}

TEST(submit, tasks_execute) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 4, 0);

  for (int i = 0; i < 100; i++) {
    ccol_retval r = cthrpool_submit(pool, inc_counter, &counter, NULL);
    REQUIRE_EQ(r, ccol_success);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 100);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(submit, on_complete_fires_after_fn) {
  atomic_int fn_done = 0;
  atomic_int cb_done = 0;
  cb_ctx_t ctx = {.fn_done = &fn_done, .cb_done = &cb_done};

  cthrpool_construct(pool, 1, 0);
  cthrpool_submit(pool, cb_fn, &ctx, cb_cb);
  cthrpool_wait(pool);

  REQUIRE_EQ(atomic_load(&fn_done), 1);
  REQUIRE_EQ(atomic_load(&cb_done), 1);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(submit, rejected_after_shutdown_drain) {
  cthrpool_construct(pool, 2, 0);
  cthrpool_shutdown_drain(pool);
  ccol_retval r = cthrpool_submit(pool, inc_counter, NULL, NULL);
  REQUIRE_EQ(r, ccol_not_permitted);
  cthrpool_destroy(pool);
}

TEST(submit, rejected_after_shutdown_immediate) {
  cthrpool_construct(pool, 2, 0);
  cthrpool_shutdown_immediate(pool);
  ccol_retval r = cthrpool_submit(pool, inc_counter, NULL, NULL);
  REQUIRE_EQ(r, ccol_not_permitted);
  cthrpool_destroy(pool);
}

TEST(submit, invalid_args) {
  cthrpool_construct(pool, 2, 0);
  REQUIRE_EQ(cthrpool_submit(CTHRPOOL_NULL, inc_counter, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(cthrpool_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(cthrpool_try_submit(CTHRPOOL_NULL, inc_counter, NULL, NULL),
             ccol_invalid_args);
  REQUIRE_EQ(cthrpool_try_submit(pool, NULL, NULL, NULL), ccol_invalid_args);
  uint64_t ts = 1000000;
  REQUIRE_EQ(cthrpool_timed_submit(CTHRPOOL_NULL, inc_counter, NULL, NULL, ts),
             ccol_invalid_args);
  REQUIRE_EQ(cthrpool_timed_submit(pool, NULL, NULL, NULL, ts),
             ccol_invalid_args);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(submit, returns_not_enough_memory) {
  ccol_memmgmt_procs mp = {.malloc = oom_malloc,
                           .free = oom_free,
                           .calloc = oom_calloc,
                           .realloc = oom_realloc};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  atomic_int counter = 0;
  atomic_store(&g_oom_enabled, 1);
  ccol_retval r = cthrpool_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  atomic_store(&g_oom_enabled, 0);

  r = cthrpool_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(submit, blocking_submit_from_within_a_task_reports_container_full) {
  cthrpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval submitted =
      cthrpool_submit(pool, self_submit_task_fn, &ctx, NULL);
  cthrpool_wait(pool);

  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

// The counter of the tasks that self_timed_submit_task_fn submits.
static atomic_int self_timed_counter;

// A task that makes two timed submits with no limit into its own pool: One into
// the empty queue and one into the full one.
static void self_timed_submit_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  atomic_store(&c->first_rc, (int)cthrpool_timed_submit(c->pool, inc_counter,
                                                        &self_timed_counter,
                                                        NULL, UINT64_MAX));
  atomic_store(&c->second_rc, (int)cthrpool_timed_submit(c->pool, inc_counter,
                                                         &self_timed_counter,
                                                         NULL, UINT64_MAX));
}

// The same with two timed future submits.
static void self_timed_future_task_fn(void *arg) {
  self_submit_ctx_t *c = (self_submit_ctx_t *)arg;
  atomic_store(&c->first_rc,
               (int)cthrpool_timed_submit_future(c->pool, identity_fn, NULL,
                                                 UINT64_MAX, &c->first_future));
  atomic_store(&c->second_rc,
               (int)cthrpool_timed_submit_future(
                   c->pool, identity_fn, NULL, UINT64_MAX, &c->second_future));
}

TEST(timed_submit, a_timed_submit_from_within_a_task_reports_container_full) {
  void (*fns[2])(void *) = {self_timed_submit_task_fn,
                            self_timed_future_task_fn};
  int first_rc[2], second_rc[2];
  ccol_retval submitted[2];
  for (int k = 0; k < 2; k++) {
    cthrpool_construct(pool, 1, 1);
    self_submit_ctx_t ctx;
    ctx.pool = pool;
    atomic_init(&ctx.first_rc, -1);
    atomic_init(&ctx.second_rc, -1);
    ctx.first_future = NULL;
    ctx.second_future = NULL;
    submitted[k] = cthrpool_submit(pool, fns[k], &ctx, NULL);
    for (int i = 0; i < 500 && atomic_load(&ctx.second_rc) == -1; i++) {
      struct timespec ts = {0, 10 * 1000 * 1000};
      nanosleep(&ts, NULL);
    }
    first_rc[k] = atomic_load(&ctx.first_rc);
    second_rc[k] = atomic_load(&ctx.second_rc);
    if (second_rc[k] == -1) {
      cthrpool_shutdown_immediate(pool);
    }
    cthrpool_wait(pool);
    if (ctx.first_future) {
      (void)cthrpool_future_get(ctx.first_future);
      cthrpool_future_free(ctx.first_future);
    }
    if (ctx.second_future) cthrpool_future_free(ctx.second_future);
    cthrpool_shutdown_drain(pool);
    cthrpool_destroy(pool);
  }
  for (int k = 0; k < 2; k++) {
    REQUIRE_EQ(submitted[k], ccol_success);
    REQUIRE_EQ(first_rc[k], (int)ccol_success);
    REQUIRE_EQ(second_rc[k], (int)ccol_container_full);
  }
}

TEST(submit, a_worker_without_its_thread_mark_is_still_recognised) {
  atomic_store(&cthrpool__skip_worker_mark_for_tests, true);
  cthrpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval submitted =
      cthrpool_submit(pool, self_submit_task_fn, &ctx, NULL);

  for (int i = 0; i < 1000 && atomic_load(&ctx.second_rc) == -1; i++) {
    struct timespec ts = {0, 10 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }
  atomic_store(&cthrpool__skip_worker_mark_for_tests, false);
  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);
  if (second_rc == -1) {
    cthrpool_shutdown_immediate(pool);
  }

  cthrpool_wait(pool);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_NE(second_rc, -1);
  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

// A thread whose ID is that of an already joined worker must not count as a
// worker of the pool. The child starts threads until one carries the joined ID,
// and only that one destroys the pool.
typedef struct {
  pthread_t worker_id;
  cthrpool pool;
  atomic_int outcome;
} ctp_reused_id_ctx_t;

// A task that records the ID of the worker running it, the ID a later thread
// may inherit.
static void ctp_record_worker_id(void *arg) {
  ((ctp_reused_id_ctx_t *)arg)->worker_id = pthread_self();
}

// Destroys the pool when this thread carries the recorded worker ID; that
// destroy must not count as a self-call.
static void *ctp_destroy_if_reused_id(void *arg) {
  ctp_reused_id_ctx_t *ctx = (ctp_reused_id_ctx_t *)arg;
  if (pthread_equal(pthread_self(), ctx->worker_id)) {
    cthrpool_destroy(ctx->pool);
    atomic_store(&ctx->outcome, 1);
  }
  return NULL;
}

// A child where no thread reuses the ID reports that, so the test fails instead
// of passing without having looked at the case. With exited workers in the
// scan, the destroy would end the child with SIGABRT.
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
    ctx.pool = cthrpool_create(1, 0, &err);
    if (ctx.pool != CTHRPOOL_NULL &&
        cthrpool_submit(ctx.pool, ctp_record_worker_id, &ctx, NULL) ==
            ccol_success) {
      cthrpool_wait(ctx.pool);
      cthrpool_shutdown_drain(ctx.pool);
      for (int i = 0; i < 4096 && atomic_load(&ctx.outcome) == 0; i++) {
        pthread_t t;
        if (pthread_create(&t, NULL, ctp_destroy_if_reused_id, &ctx) != 0)
          break;
        pthread_join(t, NULL);
      }
      verdict = atomic_load(&ctx.outcome) == 1 ? 'R' : 'N';
      if (verdict == 'N') cthrpool_destroy(ctx.pool);
    }
    ssize_t w = write(fds[1], &verdict, 1);
    (void)w;
    test_child_exit(0);
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
  REQUIRE_EQ(got, 'R');
}

TEST(submit, blocking_submit_from_on_complete_reports_container_full) {
  cthrpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval submitted =
      cthrpool_submit(pool, noop_fn, &ctx, self_submit_complete_fn);
  cthrpool_wait(pool);

  int first_rc = atomic_load(&ctx.first_rc);
  int second_rc = atomic_load(&ctx.second_rc);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_EQ(first_rc, (int)ccol_success);
  REQUIRE_EQ(second_rc, (int)ccol_container_full);
}

TEST(submit, blocking_submit_from_a_task_into_another_pool_still_waits) {
  cthrpool_construct(target, 1, 1);
  cthrpool_construct(source, 1, 0);

  atomic_int gate;
  atomic_int blocker_started;
  atomic_init(&gate, 0);
  atomic_init(&blocker_started, 0);
  gate_ctx_t blocker_ctx = {&gate, &blocker_started};

  ccol_retval held = cthrpool_submit(target, blocker_fn, &blocker_ctx, NULL);
  int blocker_running = wait_for_flag(&blocker_started, 5000);
  ccol_retval queued = cthrpool_submit(target, noop_fn, NULL, NULL);

  cross_pool_ctx_t ctx;
  ctx.other = target;
  atomic_init(&ctx.submitting, 0);
  atomic_init(&ctx.rc, -1);

  ccol_retval dispatched =
      cthrpool_submit(source, cross_pool_task_fn, &ctx, NULL);
  int reached_submit = wait_for_flag(&ctx.submitting, 5000);
  sleep_ms(5);

  atomic_store(&gate, 1);
  cthrpool_wait(source);
  int cross_rc = atomic_load(&ctx.rc);

  cthrpool_shutdown_drain(source);
  cthrpool_destroy(source);
  cthrpool_shutdown_drain(target);
  cthrpool_destroy(target);

  REQUIRE_EQ(held, ccol_success);
  REQUIRE_EQ(blocker_running, 1);
  REQUIRE_EQ(queued, ccol_success);
  REQUIRE_EQ(dispatched, ccol_success);
  REQUIRE_EQ(reached_submit, 1);
  REQUIRE_EQ(cross_rc, (int)ccol_success);
}

// The result of one submit against a one-slot queue that a held task keeps
// full, with what the pool recorded about its wait.
typedef struct {
  bool ready;
  ccol_retval rv;
  bool no_future;
  bool queued_ran;
  int64_t call_started_ns;
  int64_t call_ended_ns;
  cthrpool__timed_wait_record wait;
} ctp_full_queue_submit_t;

// A timespec in nanoseconds, for comparing the instants of a wait record.
static int64_t ctp_ts_ns(const struct timespec *t) {
  return (int64_t)t->tv_sec * INT64_C(1000000000) + (int64_t)t->tv_nsec;
}

// Makes one timed submit (or a try, with try_only), plain or of a future,
// against a one-slot queue that a held task keeps full. The gate opens and the
// pool is destroyed before it returns, so the caller can assert freely.
static ctp_full_queue_submit_t ctp_submit_on_full_queue(uint64_t timeout_us,
                                                        bool future,
                                                        bool try_only) {
  ctp_full_queue_submit_t m = {.ready = false,
                               .rv = ccol_unexpected_failure,
                               .no_future = false,
                               .queued_ran = false};
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  int dummy = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  cthrpool pool = cthrpool_create(1, 1, NULL);
  if (pool == CTHRPOOL_NULL) return m;

  cthrpool_future *const poison = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  cthrpool_future *f1 = NULL;
  cthrpool_future *f2 = poison;
  m.ready = cthrpool_submit(pool, blocker_fn, &ctx, NULL) == ccol_success &&
            wait_for_flag(&started, 10000);
  if (m.ready) {
    if (future) {
      f1 = cthrpool_submit_future(pool, identity_fn, &dummy, NULL);
      m.ready = f1 != NULL;
    } else {
      m.ready =
          cthrpool_submit(pool, inc_counter, &counter, NULL) == ccol_success;
    }
  }
  if (m.ready) {
    struct timespec t0, t1;
    (void)cthrpool__timed_wait_record_for_tests();
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (future && try_only)
      m.rv = cthrpool_try_submit_future(pool, identity_fn, &dummy, &f2);
    else if (future)
      m.rv = cthrpool_timed_submit_future(pool, identity_fn, &dummy, timeout_us,
                                          &f2);
    else if (try_only)
      m.rv = cthrpool_try_submit(pool, inc_counter, &counter, NULL);
    else
      m.rv =
          cthrpool_timed_submit(pool, inc_counter, &counter, NULL, timeout_us);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    m.wait = cthrpool__timed_wait_record_for_tests();
    m.call_started_ns = ctp_ts_ns(&t0);
    m.call_ended_ns = ctp_ts_ns(&t1);
    if (future) m.no_future = f2 == NULL;
  }

  atomic_store(&gate, 1);
  if (f1) {
    m.queued_ran = cthrpool_future_get(f1) == (void *)&dummy;
    cthrpool_future_free(f1);
  }
  if (f2 && f2 != poison) {
    (void)cthrpool_future_get(f2);
    cthrpool_future_free(f2);
  }
  cthrpool_shutdown_drain(pool);
  if (!future) m.queued_ran = atomic_load(&counter) >= 1;
  cthrpool_destroy(pool);
  return m;
}

// Checks the record of a timed submit that gave up: Its deadline is timeout_us
// after an instant inside the call and before the first wait, and it decided no
// earlier than that deadline (2 ms of slack for a platform that waits on
// another clock). The wall time serves only as a hang guard.
static void ctp_check_timed_out_wait(const ctp_full_queue_submit_t *m,
                                     uint64_t timeout_us, bool *waited,
                                     bool *deadline_ok, bool *not_early,
                                     bool *no_hang) {
  const int64_t timeout_ns = (int64_t)timeout_us * 1000;
  const int64_t deadline_ns = ctp_ts_ns(&m->wait.deadline);
  const int64_t started_ns = ctp_ts_ns(&m->wait.wait_started);
  const int64_t ended_ns = ctp_ts_ns(&m->wait.wait_ended);
  *waited = m->wait.waited && m->wait.timed_out;
  *deadline_ok = deadline_ns - timeout_ns >= m->call_started_ns &&
                 deadline_ns - timeout_ns <= started_ns;
  *not_early = ended_ns + INT64_C(2000000) >= deadline_ns &&
               ended_ns <= m->call_ended_ns;
  *no_hang = m->call_ended_ns - m->call_started_ns < INT64_C(60000000000);
}

TEST(try_submit, returns_container_full_when_bounded_queue_saturated) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(0, false, true);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_container_full);
  REQUIRE_FALSE(m.wait.waited);
  REQUIRE_TRUE(m.queued_ran);
}

TEST(try_submit, succeeds_when_space_available) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 16);
  ccol_retval r = cthrpool_try_submit(pool, inc_counter, &counter, NULL);
  REQUIRE_EQ(r, ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(timed_submit, times_out_on_full_bounded_queue) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(50000, false, false);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_timed_out);
  REQUIRE_TRUE(m.wait.timed_out);
  REQUIRE_TRUE(m.queued_ran);
}

// A timed submit on its own thread, with its result.
typedef struct {
  cthrpool pool;
  atomic_int *counter;
  ccol_retval result;
} timed_submit_arg_t;

// Makes a timed submit of 10 s and stores the result, so the main thread can
// act while it waits.
static void *timed_submit_thread(void *p) {
  timed_submit_arg_t *a = p;
  a->result =
      cthrpool_timed_submit(a->pool, inc_counter, a->counter, NULL, 10000000);
  return NULL;
}

// The hook makes the wait report ETIMEDOUT after it consumed a dequeue's
// signal. The submit must still take the free slot; otherwise a blocking
// submitter waiting for the same slot never gets a signal.
TEST(timed_submit, a_wait_that_times_out_after_a_wake_takes_the_free_slot) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 1);
  cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);
  cthrpool_submit(pool, inc_counter, &counter, NULL);

  atomic_store(&cthrpool__timeout_after_wake_waiting_for_tests, false);
  atomic_store(&cthrpool__timeout_after_wake_for_tests, true);
  timed_submit_arg_t arg = {
      .pool = pool, .counter = &counter, .result = ccol_unexpected_failure};
  pthread_t t;
  bool created = pthread_create(&t, NULL, timed_submit_thread, &arg) == 0;
  bool waited = false;
  for (int i = 0; created && i < 10000; i++) {
    if (atomic_load(&cthrpool__timeout_after_wake_waiting_for_tests)) {
      waited = true;
      break;
    }
    sleep_ms(1);
  }
  atomic_store(&gate, 1);
  if (created) pthread_join(t, NULL);
  atomic_store(&cthrpool__timeout_after_wake_for_tests, false);
  cthrpool_wait(pool);
  int count = atomic_load(&counter);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(waited);
  REQUIRE_EQ(arg.result, ccol_success);
  REQUIRE_EQ(count, 2);
}

TEST(timed_submit, succeeds_before_timeout) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 16);
  uint64_t ts = 1000000;
  ccol_retval r = cthrpool_timed_submit(pool, inc_counter, &counter, NULL, ts);
  REQUIRE_EQ(r, ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(timed_submit, zero_timeout_acts_as_try_submit) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(0, false, false);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_container_full);
  REQUIRE_FALSE(m.wait.waited);
  REQUIRE_LT(m.call_ended_ns - m.call_started_ns, INT64_C(60000000000));
  REQUIRE_TRUE(m.queued_ran);
}

TEST(timed_submit, zero_timeout_with_invalid_args_still_reports_invalid_args) {
  cthrpool_construct(pool, 2, 0);

  REQUIRE_EQ(cthrpool_timed_submit(CTHRPOOL_NULL, inc_counter, NULL, NULL, 0),
             ccol_invalid_args);
  REQUIRE_EQ(cthrpool_timed_submit(pool, NULL, NULL, NULL, 0),
             ccol_invalid_args);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(timed_submit, unbounded_queue_never_blocks_on_capacity) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 0);

  uint64_t ts = 1;
  for (int i = 0; i < 20; i++) {
    ccol_retval r =
        cthrpool_timed_submit(pool, inc_counter, &counter, NULL, ts);
    REQUIRE_EQ(r, ccol_success);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 20);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(timed_submit, a_small_timeout_waits_about_that_long) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(500000, false, false);
  bool waited, deadline_ok, not_early, no_hang;
  ctp_check_timed_out_wait(&m, 500000, &waited, &deadline_ok, &not_early,
                           &no_hang);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_timed_out);
  REQUIRE_TRUE(waited);
  REQUIRE_TRUE(deadline_ok);
  REQUIRE_TRUE(not_early);
  REQUIRE_TRUE(no_hang);
  REQUIRE_TRUE(m.queued_ran);
}

// A future task that increments the counter at arg and gives arg back, so a
// test sees both that it ran and its result.
static void *count_and_return_fn(void *arg) {
  atomic_fetch_add((atomic_int *)arg, 1);
  return arg;
}

TEST(futures, a_future_that_cannot_be_built_fails_the_submit) {
  static const cthrpool__fault steps[] = {CTHRPOOL__FAULT_FUTURE_ALLOC,
                                          CTHRPOOL__FAULT_FUTURE_MUTEX,
                                          CTHRPOOL__FAULT_FUTURE_CV};
  enum { N = sizeof(steps) / sizeof(steps[0]) };
  cthrpool pool = cthrpool_create(1, 0, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  atomic_int ran = 0;
  ccol_retval rv_ok = ccol_unexpected_failure;
  cthrpool_future *ok =
      cthrpool_submit_future(pool, count_and_return_fn, &ran, &rv_ok);
  void *result = ok ? cthrpool_future_get(ok) : NULL;
  cthrpool_future_free(ok);
  cthrpool_wait(pool);
  long live_before = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_future *f[N];
  ccol_retval rv[N];
  long block_delta[N];
  for (size_t i = 0; i < N; i++) {
    rv[i] = ccol_success;
    long blocks_before = atomic_load(&cthrpool__future_blocks_for_tests);
    cthrpool__arm_fault_for_tests(steps[i]);
    f[i] = cthrpool_submit_future(pool, count_and_return_fn, &ran, &rv[i]);
    cthrpool__arm_fault_for_tests(CTHRPOOL__FAULT_NONE);
    block_delta[i] =
        atomic_load(&cthrpool__future_blocks_for_tests) - blocks_before;
  }
  cthrpool_wait(pool);
  int ran_after_failures = atomic_load(&ran);
  long live_after = atomic_load(&cthrpool__live_futures_for_tests);
  bool refused[N];
  for (size_t i = 0; i < N; i++) {
    refused[i] = f[i] == NULL;
    if (f[i]) {
      (void)cthrpool_future_get(f[i]);
      cthrpool_future_free(f[i]);
    }
  }
  cthrpool_destroy(pool);

  for (size_t i = 0; i < N; i++) {
    REQUIRE_TRUE(refused[i]);
    REQUIRE_EQ(rv[i], ccol_not_enough_memory);
    REQUIRE_EQ(block_delta[i], 0L);
  }
  REQUIRE_EQ(rv_ok, ccol_success);
  REQUIRE_EQ(result, (void *)&ran);
  REQUIRE_EQ(ran_after_failures, 1);
  REQUIRE_EQ(live_after, live_before);
}

TEST(futures, a_detached_future_that_cannot_be_built_reports_why) {
  static const struct {
    cthrpool__fault step;
    const char *err;
  } cases[] = {
      {CTHRPOOL__FAULT_FUTURE_ALLOC, "failed to allocate future"},
      {CTHRPOOL__FAULT_FUTURE_MUTEX, "future mutex init failed"},
      {CTHRPOOL__FAULT_FUTURE_CV, "future cond_var init failed"},
  };
  enum { N = sizeof(cases) / sizeof(cases[0]) };
  long live_before = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_future *f[N];
  bool err_ok[N];
  long block_delta[N];
  for (size_t i = 0; i < N; i++) {
    char *err = NULL;
    long blocks_before = atomic_load(&cthrpool__future_blocks_for_tests);
    cthrpool__arm_fault_for_tests(cases[i].step);
    f[i] = cthrpool_future_create_detached(&err);
    cthrpool__arm_fault_for_tests(CTHRPOOL__FAULT_NONE);
    block_delta[i] =
        atomic_load(&cthrpool__future_blocks_for_tests) - blocks_before;
    err_ok[i] = err && strstr(err, cases[i].err) != NULL;
  }
  long live_after = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_future *ok = cthrpool_future_create_detached(NULL);
  int value = 7;
  if (ok) cthrpool_future_fulfill(ok, &value);
  void *result = ok ? cthrpool_future_get(ok) : NULL;
  cthrpool_future_free(ok);

  for (size_t i = 0; i < N; i++) {
    REQUIRE_EQ((void *)f[i], NULL);
    REQUIRE_TRUE(err_ok[i]);
    REQUIRE_EQ(block_delta[i], 0L);
  }
  REQUIRE_EQ(live_after, live_before);
  REQUIRE_EQ(result, (void *)&value);
}

TEST(futures, get_result) {
  cthrpool_construct(pool, 2, 0);

  int value = 42;
  ccol_retval rv = ccol_unexpected_failure;
  cthrpool_future *f = cthrpool_submit_future(pool, identity_fn, &value, &rv);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_success);

  void *result = cthrpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);
  REQUIRE_FALSE(cthrpool_future_cancelled(f));
  REQUIRE_TRUE(cthrpool_future_done(f));

  cthrpool_future_free(f);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, get_blocks_until_done) {
  cthrpool_construct(pool, 1, 0);

  int value = 99;
  cthrpool_future *f =
      cthrpool_submit_future(pool, slow_identity, &value, NULL);
  REQUIRE_NE((void *)f, NULL);

  void *result = cthrpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);

  cthrpool_future_free(f);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, fan_out_fan_in) {
  enum { N = 8 };
  cthrpool_construct(pool, 4, 0);

  int values[N];
  cthrpool_future *futures[N];

  for (int i = 0; i < N; i++) {
    values[i] = i * 10;
    futures[i] = cthrpool_submit_future(pool, identity_fn, &values[i], NULL);
    REQUIRE_NE((void *)futures[i], NULL);
  }

  for (int i = 0; i < N; i++) {
    int *res = (int *)cthrpool_future_get(futures[i]);
    REQUIRE_EQ(*res, values[i]);
    cthrpool_future_free(futures[i]);
  }

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, free_before_get_no_leak) {
  cthrpool_construct(pool, 1, 0);

  int value = 7;
  cthrpool_future *f =
      cthrpool_submit_future(pool, slow_identity, &value, NULL);
  REQUIRE_NE((void *)f, NULL);

  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, waiters_keep_the_future_alive_when_every_other_reference_goes) {
  enum { WAITERS = 8 };
  int expected_result_storage = 0;
  cthrpool_construct(pool, 2, 0);

  future_waiters_ctx_t ctx;
  ctx.f = NULL;
  ctx.expected_result = &expected_result_storage;
  atomic_init(&ctx.release, 0);
  atomic_init(&ctx.results_ok, 0);

  cthrpool_future *f =
      cthrpool_submit_future(pool, waiters_gated_fn, &ctx, NULL);
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

  if (waiters_hold_references) cthrpool_future_free(f);
  atomic_store(&ctx.release, 1);

  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  int results_ok = atomic_load(&ctx.results_ok);
  if (f && !waiters_hold_references) cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(started, (int)WAITERS);
  REQUIRE_EQ(observed_refcount, expected_refcount);
  REQUIRE_EQ(results_ok, started);
}

TEST(futures, many_threads_get_the_same_future_while_the_owner_frees_it) {
  enum { WAITERS = 16 };
  int expected_result_storage = 0;
  cthrpool_construct(pool, 2, 0);

  future_waiters_ctx_t ctx;
  ctx.f = NULL;
  ctx.expected_result = &expected_result_storage;
  atomic_init(&ctx.release, 0);
  atomic_init(&ctx.results_ok, 0);

  cthrpool_future *f =
      cthrpool_submit_future(pool, waiters_gated_fn, &ctx, NULL);
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
  void *owner_result = f ? cthrpool_future_get(f) : NULL;
  if (waiters_hold_references) cthrpool_future_free(f);

  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  int results_ok = atomic_load(&ctx.results_ok);
  if (f && !waiters_hold_references) cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(started, (int)WAITERS);
  REQUIRE_EQ(observed_refcount, expected_refcount);
  REQUIRE_EQ(owner_result, (void *)&expected_result_storage);
  REQUIRE_EQ(results_ok, started);
}

TEST(futures, blocking_submit_future_from_within_a_task_reports_a_full_queue) {
  cthrpool_construct(pool, 1, 1);

  self_submit_ctx_t ctx;
  ctx.pool = pool;
  atomic_init(&ctx.first_rc, -1);
  atomic_init(&ctx.second_rc, -1);
  ctx.first_future = NULL;
  ctx.second_future = NULL;

  ccol_retval submitted =
      cthrpool_submit(pool, self_submit_future_task_fn, &ctx, NULL);
  cthrpool_wait(pool);

  cthrpool_future *first = ctx.first_future;
  cthrpool_future *second = ctx.second_future;
  if (first) cthrpool_future_free(first);
  if (second) cthrpool_future_free(second);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(submitted, ccol_success);
  REQUIRE_NE((void *)first, NULL);
  REQUIRE_EQ((void *)second, NULL);
  REQUIRE_EQ(atomic_load(&ctx.first_rc), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&ctx.second_rc), (int)ccol_container_full);
}

TEST(futures, try_submit_future_succeeds) {
  int value = 42;
  cthrpool_construct(pool, 2, 16);

  cthrpool_future *f = NULL;
  ccol_retval r = cthrpool_try_submit_future(pool, identity_fn, &value, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = cthrpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, try_submit_future_container_full) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(0, true, true);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_container_full);
  REQUIRE_TRUE(m.no_future);
  REQUIRE_FALSE(m.wait.waited);
  REQUIRE_TRUE(m.queued_ran);
}

TEST(futures, timed_submit_future_succeeds) {
  int value = 99;
  cthrpool_construct(pool, 2, 16);

  uint64_t ts = 1000000;
  cthrpool_future *f = NULL;
  ccol_retval r =
      cthrpool_timed_submit_future(pool, identity_fn, &value, ts, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = cthrpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, timed_submit_future_times_out) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(50000, true, false);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_timed_out);
  REQUIRE_TRUE(m.no_future);
  REQUIRE_TRUE(m.wait.timed_out);
  REQUIRE_TRUE(m.queued_ran);
}

TEST(futures, timed_submit_future_small_timeout_waits_about_that_long) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(500000, true, false);
  bool waited, deadline_ok, not_early, no_hang;
  ctp_check_timed_out_wait(&m, 500000, &waited, &deadline_ok, &not_early,
                           &no_hang);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_timed_out);
  REQUIRE_TRUE(m.no_future);
  REQUIRE_TRUE(waited);
  REQUIRE_TRUE(deadline_ok);
  REQUIRE_TRUE(not_early);
  REQUIRE_TRUE(no_hang);
  REQUIRE_TRUE(m.queued_ran);
}

TEST(futures, timed_submit_future_zero_timeout_acts_as_try) {
  ctp_full_queue_submit_t m = ctp_submit_on_full_queue(0, true, false);
  REQUIRE_TRUE(m.ready);
  REQUIRE_EQ(m.rv, ccol_container_full);
  REQUIRE_TRUE(m.no_future);
  REQUIRE_FALSE(m.wait.waited);
  REQUIRE_TRUE(m.queued_ran);
}

TEST(futures, timed_submit_future_zero_timeout_with_invalid_args) {
  cthrpool_construct(pool, 2, 0);
  int value = 0;

  cthrpool_future *f = (cthrpool_future *)0x1;
  REQUIRE_EQ(
      cthrpool_timed_submit_future(CTHRPOOL_NULL, identity_fn, &value, 0, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = (cthrpool_future *)0x1;
  REQUIRE_EQ(cthrpool_timed_submit_future(pool, NULL, &value, 0, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, timed_submit_future_unbounded_never_blocks_on_capacity) {
  cthrpool_construct(pool, 2, 0);
  int value = 7;

  uint64_t ts = 1;
  cthrpool_future *f = NULL;
  ccol_retval r =
      cthrpool_timed_submit_future(pool, identity_fn, &value, ts, &f);
  REQUIRE_EQ(r, ccol_success);
  REQUIRE_NE((void *)f, NULL);

  void *res = cthrpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, done_is_false_before_completion) {
  atomic_int gate = 0;
  int value = 0;
  gated_future_ctx_t ctx = {.gate = &gate, .result = &value};

  cthrpool_construct(pool, 1, 0);
  cthrpool_future *f = cthrpool_submit_future(pool, gated_identity, &ctx, NULL);
  REQUIRE_NE((void *)f, NULL);

  REQUIRE_FALSE(cthrpool_future_done(f));

  atomic_store(&gate, 1);
  void *res = cthrpool_future_get(f);
  REQUIRE_EQ(res, (void *)&value);
  REQUIRE_TRUE(cthrpool_future_done(f));

  cthrpool_future_free(f);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, invalid_args) {
  cthrpool_construct(pool, 2, 16);
  int value = 0;
  cthrpool_future *f = NULL;
  uint64_t ts = 1000000;

  REQUIRE_EQ(cthrpool_try_submit_future(CTHRPOOL_NULL, identity_fn, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(cthrpool_try_submit_future(pool, NULL, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(cthrpool_try_submit_future(pool, identity_fn, &value, NULL),
             ccol_invalid_args);

  REQUIRE_EQ(
      cthrpool_timed_submit_future(CTHRPOOL_NULL, identity_fn, &value, ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(cthrpool_timed_submit_future(pool, NULL, &value, ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(cthrpool_timed_submit_future(pool, identity_fn, &value, ts, NULL),
             ccol_invalid_args);

  ccol_retval rv = ccol_unexpected_failure;
  REQUIRE_EQ(
      (void *)cthrpool_submit_future(CTHRPOOL_NULL, identity_fn, &value, &rv),
      NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  rv = ccol_unexpected_failure;
  REQUIRE_EQ((void *)cthrpool_submit_future(pool, NULL, &value, &rv), NULL);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)cthrpool_submit_future(pool, NULL, &value, NULL), NULL);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// *out is poisoned before each call, so a failure path that leaves it untouched
// shows up (futures.invalid_args presets it to NULL and can't see that).
TEST(futures, out_param_zeroed_even_on_earliest_failure_paths) {
  cthrpool_construct(pool, 2, 16);
  int value = 0;
  uint64_t ts = 1000000;
  cthrpool_future *const poison = (cthrpool_future *)(uintptr_t)0xdeadbeefu;

  cthrpool_future *f = poison;
  REQUIRE_EQ(cthrpool_try_submit_future(CTHRPOOL_NULL, identity_fn, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(cthrpool_try_submit_future(pool, NULL, &value, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(
      cthrpool_timed_submit_future(CTHRPOOL_NULL, identity_fn, &value, ts, &f),
      ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  f = poison;
  REQUIRE_EQ(cthrpool_timed_submit_future(pool, NULL, &value, ts, &f),
             ccol_invalid_args);
  REQUIRE_EQ((void *)f, NULL);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(futures, every_future_submit_reports_not_permitted_after_shutdown) {
  for (int mode = 0; mode < 2; mode++) {
    for (size_t cap = 0; cap <= 4; cap += 4) {
      cthrpool pool = cthrpool_create(2, cap, NULL);
      REQUIRE_NE(pool, CTHRPOOL_NULL);
      if (mode == 0)
        cthrpool_shutdown_drain(pool);
      else
        cthrpool_shutdown_immediate(pool);

      int value = 0;
      ccol_retval rv = ccol_unexpected_failure;
      cthrpool_future *f =
          cthrpool_submit_future(pool, identity_fn, &value, &rv);
      cthrpool_future *f2 = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
      ccol_retval r2 =
          cthrpool_try_submit_future(pool, identity_fn, &value, &f2);
      cthrpool_future *f3 = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
      ccol_retval r3 =
          cthrpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
      cthrpool_destroy(pool);

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
  ccol_memmgmt_procs mp = {.malloc = oom_malloc,
                           .free = oom_free,
                           .calloc = oom_calloc,
                           .realloc = oom_realloc};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  int value = 0;

  atomic_store(&g_oom_enabled, 1);
  ccol_retval rv = ccol_unexpected_failure;
  cthrpool_future *f = cthrpool_submit_future(pool, identity_fn, &value, &rv);
  cthrpool_future *f_null_rv =
      cthrpool_submit_future(pool, identity_fn, &value, NULL);

  cthrpool_future *f2 = NULL;
  ccol_retval r = cthrpool_try_submit_future(pool, identity_fn, &value, &f2);
  cthrpool_future *f3 = NULL;
  ccol_retval r3 =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
  atomic_store(&g_oom_enabled, 0);
  REQUIRE_EQ((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_null_rv, NULL);
  REQUIRE_EQ(r, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f2, NULL);
  REQUIRE_EQ(r3, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f3, NULL);

  f = cthrpool_submit_future(pool, identity_fn, &value, NULL);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(cthrpool_future_get(f), (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// What one round of every_future_submit_locks_the_pool_once measured: Results
// and lock counts of each kind of submit.
typedef struct {
  bool created;
  size_t free_nodes;
  ccol_retval r_try, r_block, r_timed;
  unsigned long locks_try, locks_block, locks_timed;
  bool results_ok;
  ccol_retval p_try, p_block, p_timed;
  unsigned long plain_try, plain_block, plain_timed;
  int plain_ran;
} future_lock_count_t;

// Fills the free list of a pool with capacity cap, then makes one try, blocking
// and timed submit of each kind and counts the locks each takes on this
// thread.
static future_lock_count_t measure_future_submit_locks(size_t cap) {
  future_lock_count_t m = {.r_try = ccol_unexpected_failure,
                           .r_block = ccol_unexpected_failure,
                           .r_timed = ccol_unexpected_failure,
                           .p_try = ccol_unexpected_failure,
                           .p_block = ccol_unexpected_failure,
                           .p_timed = ccol_unexpected_failure};
  cthrpool pool = cthrpool_create(1, cap, NULL);
  if (pool == CTHRPOOL_NULL) return m;
  m.created = true;
  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);
  int value = 7;

  atomic_int warm_gate = 0;
  atomic_int warm_started = 0;
  gate_ctx_t warm_ctx = {.gate = &warm_gate, .started = &warm_started};
  if (cthrpool_submit(pool, blocker_fn, &warm_ctx, NULL) == ccol_success) {
    (void)wait_for_flag(&warm_started, 10000);
  }
  atomic_int warm_counter = 0;
  for (int i = 0; i < 6; i++)
    cthrpool_submit(pool, inc_counter, &warm_counter, NULL);
  atomic_store(&warm_gate, 1);
  cthrpool_wait(pool);
  for (int i = 0; i < 5000 && cthrpool__task_free_list_size_for_tests(raw) < 7;
       i++)
    sleep_ms(1);

  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  if (cthrpool_submit(pool, blocker_fn, &ctx, NULL) == ccol_success) {
    (void)wait_for_flag(&started, 10000);
  }
  m.free_nodes = cthrpool__task_free_list_size_for_tests(raw);

  cthrpool_future *f1 = NULL, *f2 = NULL, *f3 = NULL;
  unsigned long l0 = cthrpool__mu_locks_for_tests();
  m.r_try = cthrpool_try_submit_future(pool, identity_fn, &value, &f1);
  unsigned long l1 = cthrpool__mu_locks_for_tests();
  f2 = cthrpool_submit_future(pool, identity_fn, &value, &m.r_block);
  unsigned long l2 = cthrpool__mu_locks_for_tests();
  m.r_timed =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 1000000, &f3);
  unsigned long l3 = cthrpool__mu_locks_for_tests();
  m.locks_try = l1 - l0;
  m.locks_block = l2 - l1;
  m.locks_timed = l3 - l2;

  atomic_int plain_counter = 0;
  unsigned long p0 = cthrpool__mu_locks_for_tests();
  m.p_try = cthrpool_try_submit(pool, inc_counter, &plain_counter, NULL);
  unsigned long p1 = cthrpool__mu_locks_for_tests();
  m.p_block = cthrpool_submit(pool, inc_counter, &plain_counter, NULL);
  unsigned long p2 = cthrpool__mu_locks_for_tests();
  m.p_timed =
      cthrpool_timed_submit(pool, inc_counter, &plain_counter, NULL, 1000000);
  unsigned long p3 = cthrpool__mu_locks_for_tests();
  m.plain_try = p1 - p0;
  m.plain_block = p2 - p1;
  m.plain_timed = p3 - p2;

  atomic_store(&gate, 1);
  m.results_ok = true;
  cthrpool_future *fs[3] = {f1, f2, f3};
  for (int i = 0; i < 3; i++) {
    if (!fs[i]) {
      m.results_ok = false;
      continue;
    }
    if (cthrpool_future_get(fs[i]) != (void *)&value) m.results_ok = false;
    cthrpool_future_free(fs[i]);
  }
  cthrpool_wait(pool);
  m.plain_ran = atomic_load(&plain_counter);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
  return m;
}

// A submit that took its node, checked for room and enqueued in separate
// critical sections would count two or three locks here.
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

// The armed allocation failure stays armed, which shows that the refused try
// didn't even attempt to allocate a future.
TEST(futures, a_refused_future_try_allocates_nothing_and_takes_no_lock) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  cthrpool pool = cthrpool_create_mp(1, 1, &mp, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  ccol_retval r_blocker = cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  (void)wait_for_flag(&started, 10000);
  ccol_retval r_fill = cthrpool_submit(pool, inc_counter, &counter, NULL);
  int value = 0;

  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, true);
  int allocs0 = atomic_load(&g_alloc_count);
  unsigned long l0 = cthrpool__mu_locks_for_tests();
  cthrpool_future *f_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_try);
  cthrpool_future *f_zero = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_zero =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 0, &f_zero);
  unsigned long full_locks = cthrpool__mu_locks_for_tests() - l0;
  int full_allocs = atomic_load(&g_alloc_count) - allocs0;
  bool armed_after_full =
      atomic_load(&cthrpool__fail_next_future_alloc_for_tests);

  atomic_store(&gate, 1);
  cthrpool_shutdown_drain(pool);

  int allocs1 = atomic_load(&g_alloc_count);
  unsigned long l1 = cthrpool__mu_locks_for_tests();
  cthrpool_future *f_sd_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_sd_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_sd_try);
  cthrpool_future *f_sd_zero = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_sd_zero =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 0, &f_sd_zero);
  unsigned long sd_locks = cthrpool__mu_locks_for_tests() - l1;
  int sd_allocs = atomic_load(&g_alloc_count) - allocs1;
  bool armed_after_shutdown =
      atomic_load(&cthrpool__fail_next_future_alloc_for_tests);
  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, false);
  cthrpool_destroy(pool);

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

TEST(futures, a_failed_future_allocation_takes_nothing) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  cthrpool pool = cthrpool_create_mp(1, 4, &mp, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  long live_before = atomic_load(&cthrpool__live_futures_for_tests);
  int allocs_before = atomic_load(&g_alloc_count);
  int value = 0;

  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, true);
  ccol_retval r_block = ccol_unexpected_failure;
  cthrpool_future *f_block =
      cthrpool_submit_future(pool, identity_fn, &value, &r_block);
  bool flag_used_block =
      !atomic_load(&cthrpool__fail_next_future_alloc_for_tests);

  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, true);
  cthrpool_future *f_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_try);
  bool flag_used_try =
      !atomic_load(&cthrpool__fail_next_future_alloc_for_tests);

  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, true);
  cthrpool_future *f_timed = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_timed = cthrpool_timed_submit_future(pool, identity_fn, &value,
                                                     1000000, &f_timed);
  bool flag_used_timed =
      !atomic_load(&cthrpool__fail_next_future_alloc_for_tests);
  atomic_store(&cthrpool__fail_next_future_alloc_for_tests, false);

  int allocs_after = atomic_load(&g_alloc_count);
  long live_after = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

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

TEST(futures, a_failed_task_node_allocation_frees_the_future) {
  ccol_memmgmt_procs mp = {.malloc = oom_malloc,
                           .free = oom_free,
                           .calloc = oom_calloc,
                           .realloc = oom_realloc};
  cthrpool pool = cthrpool_create_mp(1, 4, &mp, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  size_t free_nodes = cthrpool__task_free_list_size_for_tests(
      cthrpool__resolve_for_tests(pool));
  long live_before = atomic_load(&cthrpool__live_futures_for_tests);
  int value = 0;

  atomic_store(&g_oom_enabled, 1);
  ccol_retval r_block = ccol_unexpected_failure;
  cthrpool_future *f_block =
      cthrpool_submit_future(pool, identity_fn, &value, &r_block);
  cthrpool_future *f_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_try);
  cthrpool_future *f_timed = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_timed = cthrpool_timed_submit_future(pool, identity_fn, &value,
                                                     1000000, &f_timed);
  atomic_store(&g_oom_enabled, 0);
  long live_after = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(free_nodes, (size_t)0);
  REQUIRE_EQ((void *)f_block, NULL);
  REQUIRE_EQ(r_block, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_try, NULL);
  REQUIRE_EQ(r_try, ccol_not_enough_memory);
  REQUIRE_EQ((void *)f_timed, NULL);
  REQUIRE_EQ(r_timed, ccol_not_enough_memory);
  REQUIRE_EQ(live_after, live_before);
}

TEST(futures, a_refused_future_submit_leaves_no_future_and_no_node) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  cthrpool pool = cthrpool_create_mp(1, 1, &mp, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  long live_start = atomic_load(&cthrpool__live_futures_for_tests);

  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  ccol_retval r_blocker = cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  (void)wait_for_flag(&started, 10000);
  int value = 3;
  cthrpool_future *f0 = cthrpool_submit_future(pool, identity_fn, &value, NULL);
  long live_full = atomic_load(&cthrpool__live_futures_for_tests);

  cthrpool_future *f_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_try);
  cthrpool_future *f_zero = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_zero =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 0, &f_zero);
  cthrpool_future *f_timed = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_timed =
      cthrpool_timed_submit_future(pool, identity_fn, &value, 20000, &f_timed);
  long live_refused = atomic_load(&cthrpool__live_futures_for_tests);

  atomic_store(&gate, 1);
  void *res0 = f0 ? cthrpool_future_get(f0) : NULL;
  cthrpool_future_free(f0);
  cthrpool_shutdown_drain(pool);

  long live_drained = atomic_load(&cthrpool__live_futures_for_tests);
  ccol_retval r_sd_block = ccol_unexpected_failure;
  cthrpool_future *f_sd_block =
      cthrpool_submit_future(pool, identity_fn, &value, &r_sd_block);
  cthrpool_future *f_sd_try = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_sd_try =
      cthrpool_try_submit_future(pool, identity_fn, &value, &f_sd_try);
  cthrpool_future *f_sd_timed = (cthrpool_future *)(uintptr_t)0xdeadbeefu;
  ccol_retval r_sd_timed = cthrpool_timed_submit_future(
      pool, identity_fn, &value, 1000000, &f_sd_timed);
  long live_after_shutdown = atomic_load(&cthrpool__live_futures_for_tests);
  cthrpool_destroy(pool);

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

// A timed future submit on its own thread, with its future and result.
typedef struct {
  cthrpool pool;
  cthrpool_future *f;
  ccol_retval r;
} timed_future_waiter_t;

// Makes a timed future submit of 10 s that a full queue holds, so a shutdown
// can wake it.
static void *timed_future_waiter(void *p) {
  timed_future_waiter_t *a = p;
  static int value = 5;
  a->r = cthrpool_timed_submit_future(a->pool, identity_fn, &value, 10000000,
                                      &a->f);
  return NULL;
}

// Runs cthrpool_shutdown_immediate on the pool whose handle p points to, so a
// test can shut down from another thread.
static void *shutdown_immediate_thread(void *p) {
  cthrpool_shutdown_immediate(*(cthrpool *)p);
  return NULL;
}

// The hook makes the wait return ETIMEDOUT after it consumed the shutdown's
// wake; the submit must still report ccol_not_permitted and free its future.
TEST(futures, a_timed_future_submit_woken_by_shutdown_frees_its_future) {
  cthrpool pool = cthrpool_create(1, 1, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  long live_start = atomic_load(&cthrpool__live_futures_for_tests);
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  (void)wait_for_flag(&started, 10000);
  cthrpool_submit(pool, inc_counter, &counter, NULL);

  atomic_store(&cthrpool__timeout_after_wake_waiting_for_tests, false);
  atomic_store(&cthrpool__timeout_after_wake_for_tests, true);
  timed_future_waiter_t arg = {
      .pool = pool, .f = NULL, .r = ccol_unexpected_failure};
  pthread_t waiter, stopper;
  bool waiter_created =
      pthread_create(&waiter, NULL, timed_future_waiter, &arg) == 0;
  bool waited = false;
  for (int i = 0; waiter_created && i < 10000; i++) {
    if (atomic_load(&cthrpool__timeout_after_wake_waiting_for_tests)) {
      waited = true;
      break;
    }
    sleep_ms(1);
  }
  bool stopper_created =
      pthread_create(&stopper, NULL, shutdown_immediate_thread, &pool) == 0;
  if (!stopper_created) atomic_store(&gate, 1);
  if (waiter_created) pthread_join(waiter, NULL);
  atomic_store(&gate, 1);
  if (stopper_created) pthread_join(stopper, NULL);
  atomic_store(&cthrpool__timeout_after_wake_for_tests, false);
  long live_after = atomic_load(&cthrpool__live_futures_for_tests);
  bool no_future = arg.f == NULL;
  if (arg.f) {
    (void)cthrpool_future_get(arg.f);
    cthrpool_future_free(arg.f);
  }
  cthrpool_destroy(pool);

  REQUIRE_TRUE(waiter_created);
  REQUIRE_TRUE(stopper_created);
  REQUIRE_TRUE(waited);
  REQUIRE_EQ(arg.r, ccol_not_permitted);
  REQUIRE_TRUE(no_future);
  REQUIRE_EQ(live_after, live_start);
}

// An outside producer thread that fulfils a detached future after a delay.
typedef struct {
  cthrpool_future *f;
  void *result;
  int delay_ms;
} detached_producer_ctx_t;

// Sleeps delay_ms and fulfils f with result, to show that
// cthrpool_future_get() waits for a producer that isn't a worker.
static void *detached_producer_fn(void *arg) {
  detached_producer_ctx_t *ctx = (detached_producer_ctx_t *)arg;
  if (ctx->delay_ms > 0) sleep_ms(ctx->delay_ms);
  cthrpool_future_fulfill(ctx->f, ctx->result);
  return NULL;
}

TEST(detached_futures, create_and_fulfill_no_pool) {
  char *err = NULL;
  cthrpool_future *f = cthrpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_FALSE(cthrpool_future_done(f));

  int value = 123;
  ccol_retval r = cthrpool_future_fulfill(f, &value);
  REQUIRE_EQ(r, ccol_success);

  REQUIRE_TRUE(cthrpool_future_done(f));
  REQUIRE_FALSE(cthrpool_future_cancelled(f));
  REQUIRE_EQ(cthrpool_future_get(f), (void *)&value);

  cthrpool_future_free(f);
}

TEST(detached_futures, get_blocks_until_external_fulfill) {
  char *err = NULL;
  cthrpool_future *f = cthrpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  int value = 55;
  detached_producer_ctx_t ctx = {.f = f, .result = &value, .delay_ms = 20};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, detached_producer_fn, &ctx), 0);

  void *result = cthrpool_future_get(f);
  REQUIRE_EQ(result, (void *)&value);

  pthread_join(tid, NULL);
  cthrpool_future_free(f);
}

TEST(detached_futures, free_before_fulfill_no_leak) {
  char *err = NULL;
  cthrpool_future *f = cthrpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  cthrpool_future_free(f);

  int value = 7;
  ccol_retval r = cthrpool_future_fulfill(f, &value);
  REQUIRE_EQ(r, ccol_success);
}

TEST(detached_futures, double_fulfill_returns_not_permitted) {
  char *err = NULL;
  cthrpool_future *f = cthrpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);

  int v1 = 1, v2 = 2;
  REQUIRE_EQ(cthrpool_future_fulfill(f, &v1), ccol_success);
  REQUIRE_EQ(cthrpool_future_fulfill(f, &v2), ccol_not_permitted);

  REQUIRE_EQ(cthrpool_future_get(f), (void *)&v1);

  cthrpool_future_free(f);
}

TEST(detached_futures, fulfill_null_returns_invalid_args) {
  REQUIRE_EQ(cthrpool_future_fulfill(NULL, NULL), ccol_invalid_args);
}

TEST(detached_futures, independent_of_any_pool) {
  cthrpool_construct(pool, 2, 0);
  atomic_int counter = 0;
  cthrpool_submit(pool, inc_counter, &counter, NULL);

  char *err = NULL;
  cthrpool_future *f = cthrpool_future_create_detached(&err);
  REQUIRE_NE((void *)f, NULL);
  int value = 9;
  REQUIRE_EQ(cthrpool_future_fulfill(f, &value), ccol_success);
  REQUIRE_EQ(cthrpool_future_get(f), (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);
  cthrpool_destroy(pool);
}

TEST(wait, phase_synchronisation) {
  atomic_int phase1_done = 0;
  atomic_int phase2_ok = 1;

  phase2_ctx_t p2ctx = {
      .p1_count = &phase1_done, .ok = &phase2_ok, .expected_p1 = 4};

  cthrpool_construct(pool, 4, 0);

  for (int i = 0; i < 4; i++) {
    cthrpool_submit(pool, phase1_fn, &phase1_done, NULL);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&phase1_done), 4);

  for (int i = 0; i < 4; i++) {
    cthrpool_submit(pool, phase2_fn, &p2ctx, NULL);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&phase2_ok), 1);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(wait, returns_when_idle_after_burst) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 4, 0);

  for (int i = 0; i < 50; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 50);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// Bursts of random size; after each drain the pending and active counts are
// zero and every submitted task ran exactly once.
TEST(cthreadpool, invariants_random_ops) {
  ccol_invariants_rng_t rng;
  uint64_t seed = CCOL_INVARIANTS_DEFAULT_SEED;
  ccol_invariants_seed(&rng, seed);
  ccol_invariants_print_seed("cthreadpool.invariants_random_ops", seed);

  atomic_int counter = 0;
  cthrpool_construct(pool, 4, 0);

  int total_submitted = 0;
  const int num_bursts = 30;
  for (int b = 0; b < num_bursts; ++b) {
    int burst_size = (int)ccol_invariants_next_bounded(&rng, 100) + 1;
    for (int i = 0; i < burst_size; ++i) {
      ccol_retval r = cthrpool_submit(pool, inc_counter, &counter, NULL);
      REQUIRE_EQ(r, ccol_success);
    }
    total_submitted += burst_size;

    cthrpool_wait(pool);

    REQUIRE_EQ(cthrpool_pending_count(pool), (size_t)0);
    REQUIRE_EQ(cthrpool_active_count(pool), (size_t)0);
    REQUIRE_EQ(atomic_load(&counter), total_submitted);
  }

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(wait, pool_accepts_work_after_wait) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 0);

  for (int i = 0; i < 5; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 5);

  for (int i = 0; i < 5; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 10);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(wait, returns_immediately_after_shutdown_immediate) {
  cthrpool_construct(pool, 2, 0);
  cthrpool_shutdown_immediate(pool);
  cthrpool_wait(pool);
  cthrpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_immediate_unblocks_wait) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  cthrpool_submit(pool, blocker_fn, &gctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  for (int i = 0; i < 5; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  pthread_t waiter;
  bool started_waiter =
      (pthread_create(&waiter, NULL, pool_wait_thread, &pool) == 0);
  if (!started_waiter) {
    atomic_store(&gate, 1);
    cthrpool_destroy(pool);
  }
  REQUIRE_TRUE(started_waiter);
  sleep_ms(10);

  atomic_store(&gate, 1);
  while (cthrpool_active_count(pool) > 0) sleep_ms(1);
  cthrpool_shutdown_immediate(pool);

  pthread_join(waiter, NULL);
  cthrpool_destroy(pool);
}

TEST(wait, concurrent_shutdown_drain_unblocks_wait) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  cthrpool_submit(pool, blocker_fn, &gctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  for (int i = 0; i < 5; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  pthread_t waiter;
  bool started_waiter =
      (pthread_create(&waiter, NULL, pool_wait_thread, &pool) == 0);
  sleep_ms(10);

  pthread_t releaser;
  bool started_releaser =
      (pthread_create(&releaser, NULL, release_gate_fn, &gate) == 0);
  if (!started_releaser) atomic_store(&gate, 1);

  cthrpool_shutdown_drain(pool);
  if (started_releaser) pthread_join(releaser, NULL);
  if (started_waiter) pthread_join(waiter, NULL);
  int ran = atomic_load(&counter);
  cthrpool_destroy(pool);
  REQUIRE_TRUE(started_waiter);
  REQUIRE_TRUE(started_releaser);
  REQUIRE_EQ(ran, 5);
}

TEST(wait, returns_immediately_when_pool_already_drained) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 2, 0);

  for (int i = 0; i < 10; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  cthrpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 10);

  cthrpool_wait(pool);

  cthrpool_destroy(pool);
}

TEST(shutdown_drain, all_tasks_complete) {
  atomic_int counter = 0;
  cthrpool_construct(pool, 4, 0);

  for (int i = 0; i < 200; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  cthrpool_shutdown_drain(pool);
  REQUIRE_EQ(atomic_load(&counter), 200);
  cthrpool_destroy(pool);
}

TEST(shutdown_drain, idempotent_second_call) {
  cthrpool_construct(pool, 2, 0);
  cthrpool_shutdown_drain(pool);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(shutdown_immediate, queued_tasks_discarded) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  for (int i = 0; i < 10; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = cthrpool__resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);

  cthrpool_shutdown_immediate(pool);
  if (created == 0) pthread_join(releaser, NULL);
  int counter_after = atomic_load(&counter);
  cthrpool_destroy(pool);

  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(counter_after, 0);
}

TEST(shutdown_immediate, futures_become_cancelled) {
  atomic_int gate = 0;
  atomic_int started = 0;
  int dummy = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  cthrpool_future *f1 =
      cthrpool_submit_future(pool, slow_identity, &dummy, NULL);
  cthrpool_future *f2 =
      cthrpool_submit_future(pool, slow_identity, &dummy, NULL);
  REQUIRE_NE((void *)f1, NULL);
  REQUIRE_NE((void *)f2, NULL);

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = cthrpool__resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);

  cthrpool_shutdown_immediate(pool);
  if (created == 0) pthread_join(releaser, NULL);

  REQUIRE_EQ(created, 0);
  REQUIRE_TRUE(cthrpool_future_cancelled(f1));
  REQUIRE_TRUE(cthrpool_future_done(f1));
  REQUIRE_EQ(cthrpool_future_get(f1), NULL);

  REQUIRE_TRUE(cthrpool_future_cancelled(f2));
  REQUIRE_EQ(cthrpool_future_get(f2), NULL);

  cthrpool_future_free(f1);
  cthrpool_future_free(f2);
  cthrpool_destroy(pool);
}

TEST(shutdown_immediate, idempotent_second_call) {
  cthrpool_construct(pool, 2, 0);
  cthrpool_shutdown_immediate(pool);
  cthrpool_shutdown_immediate(pool);
  cthrpool_destroy(pool);
}

// One heap argument per task of the discard tests. on_complete records ran and
// frees the argument, so a second call for a task is a double free and a
// missing call is a leak, both of which the tools report.
typedef struct {
  atomic_int *completions;
  atomic_int *ran_true;
  atomic_int *fn_runs;
  int index;
  atomic_int *seen;
} discard_arg_t;

// Counts a run of a task that should have been discarded; it should never
// run.
static void discard_fn(void *arg) {
  atomic_fetch_add(((discard_arg_t *)arg)->fn_runs, 1);
}

// Records one completion of task index, counts ran == true and frees the
// argument.
static void discard_on_complete(void *arg, bool ran) {
  discard_arg_t *a = (discard_arg_t *)arg;
  atomic_fetch_add(&a->seen[a->index], 1);
  if (ran) atomic_fetch_add(a->ran_true, 1);
  atomic_fetch_add(a->completions, 1);
  free(a);
}

// How many tasks the discard tests queue behind the held worker.
#define DISCARD_TASKS 16

TEST(shutdown_immediate, on_complete_reports_discarded_tasks_as_not_run) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  ccol_retval blocker_rv = cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = blocker_rv == ccol_success && ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (cthrpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = cthrpool__resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);
  cthrpool_shutdown_immediate(pool);
  int completions_at_return = atomic_load(&completions);
  if (created == 0) pthread_join(releaser, NULL);
  cthrpool_destroy(pool);

  REQUIRE_EQ(blocker_rv, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(accepted, DISCARD_TASKS);
  REQUIRE_EQ(completions_at_return, DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&completions), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&ran_true), 0);
  REQUIRE_EQ(atomic_load(&fn_runs), 0);
  for (int i = 0; i < DISCARD_TASKS; i++) REQUIRE_EQ(atomic_load(&seen[i]), 1);
}

// An on_complete of a discarded task that calls back into its own pool: The
// wait and both shutdowns must be no-ops there and a submit must be refused,
// or the thread would wait on the shutdown it is running itself.
typedef struct {
  cthrpool pool;
  atomic_int calls;
  atomic_int submit_rv;
} discard_reentry_ctx_t;

// On a discard, waits on, shuts down and submits to its own pool and records
// the submit result.
static void discard_reentry_on_complete(void *arg, bool ran) {
  discard_reentry_ctx_t *c = (discard_reentry_ctx_t *)arg;
  if (!ran) {
    cthrpool_wait(c->pool);
    cthrpool_shutdown_drain(c->pool);
    cthrpool_shutdown_immediate(c->pool);
    atomic_store(&c->submit_rv,
                 (int)cthrpool_submit(c->pool, noop_fn, NULL, NULL));
  }
  atomic_fetch_add(&c->calls, 1);
}

TEST(shutdown_immediate, discarded_on_complete_can_call_back_into_its_pool) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  discard_reentry_ctx_t rc = {.pool = pool};
  atomic_init(&rc.calls, 0);
  atomic_init(&rc.submit_rv, -1);
  ccol_retval blocker_rv = cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = blocker_rv == ccol_success && ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);
  ccol_retval rv =
      cthrpool_submit(pool, noop_fn, &rc, discard_reentry_on_complete);

  immediate_release_ctx_t rctx = {.gate = &gate,
                                  .raw = cthrpool__resolve_for_tests(pool)};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);
  cthrpool_shutdown_immediate(pool);
  if (created == 0) pthread_join(releaser, NULL);
  cthrpool_destroy(pool);

  REQUIRE_EQ(blocker_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(created, 0);
  REQUIRE_EQ(atomic_load(&rc.calls), 1);
  REQUIRE_EQ(atomic_load(&rc.submit_rv), (int)ccol_not_permitted);
}

// Two pools whose discard callbacks nest on one thread: A callback discarded
// from outer shuts down inner, whose discard callback then runs inside it.
// There the thread must still count as a worker of outer, so calls into outer
// behave as from one of its own callbacks instead of waiting for ever on a
// shutdown that this thread owns.
typedef struct {
  cthrpool outer;
  cthrpool inner;
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

// Records whether it counts as a worker of outer and of inner, and calls into
// outer only when the self-call check says yes.
static void nested_discard_inner_on_complete(void *arg, bool ran) {
  nested_discard_ctx_t *c = (nested_discard_ctx_t *)arg;
  if (!ran) {
    bool outer_self = cthrpool__is_self_call_for_tests(c->outer_raw);
    atomic_store(&c->inner_sees_outer, outer_self ? 1 : 0);
    atomic_store(&c->inner_sees_inner,
                 cthrpool__is_self_call_for_tests(c->inner_raw) ? 1 : 0);
    if (outer_self) {
      cthrpool_wait(c->outer);
      cthrpool_shutdown_drain(c->outer);
      cthrpool_shutdown_immediate(c->outer);
      atomic_store(&c->submit_rv,
                   (int)cthrpool_submit(c->outer, noop_fn, NULL, NULL));
    }
  }
  atomic_fetch_add(&c->inner_calls, 1);
  atomic_store(c->gate_inner, 1);
}

// Shuts down inner from outer's discard callback and records that the thread
// still counts as a worker of outer afterwards.
static void nested_discard_outer_on_complete(void *arg, bool ran) {
  nested_discard_ctx_t *c = (nested_discard_ctx_t *)arg;
  if (!ran) {
    cthrpool_shutdown_immediate(c->inner);
    atomic_store(&c->outer_sees_outer_after,
                 cthrpool__is_self_call_for_tests(c->outer_raw) ? 1 : 0);
  }
  atomic_fetch_add(&c->outer_calls, 1);
  atomic_store(c->gate_outer, 1);
}

// Opens the gate after 10 s if no callback did, so a callback that never runs
// fails the test instead of hanging it.
static void *release_gate_late_fn(void *arg) {
  atomic_int *gate = (atomic_int *)arg;
  (void)wait_for_flag(gate, 10000);
  atomic_store(gate, 1);
  return NULL;
}

TEST(shutdown_immediate, nested_discards_keep_the_outer_pool_a_self_call) {
  atomic_int gate_outer = 0, gate_inner = 0;
  atomic_int started_outer = 0, started_inner = 0;
  gate_ctx_t ctx_outer = {.gate = &gate_outer, .started = &started_outer};
  gate_ctx_t ctx_inner = {.gate = &gate_inner, .started = &started_inner};

  cthrpool_construct(outer, 1, 0);
  cthrpool_construct(inner, 1, 0);
  nested_discard_ctx_t nc = {.outer = outer,
                             .inner = inner,
                             .gate_outer = &gate_outer,
                             .gate_inner = &gate_inner,
                             .outer_raw = cthrpool__resolve_for_tests(outer),
                             .inner_raw = cthrpool__resolve_for_tests(inner)};
  atomic_init(&nc.outer_calls, 0);
  atomic_init(&nc.inner_calls, 0);
  atomic_init(&nc.inner_sees_outer, -1);
  atomic_init(&nc.inner_sees_inner, -1);
  atomic_init(&nc.outer_sees_outer_after, -1);
  atomic_init(&nc.submit_rv, -1);

  ccol_retval b1 = cthrpool_submit(outer, blocker_fn, &ctx_outer, NULL);
  ccol_retval b2 = cthrpool_submit(inner, blocker_fn, &ctx_inner, NULL);
  bool held_outer = b1 == ccol_success && ctp_await_flag(&started_outer);
  bool held_inner = b2 == ccol_success && ctp_await_flag(&started_inner);
  if (!held_outer || !held_inner) {
    ctp_abandon_gated_pool(&outer, &gate_outer);
    ctp_abandon_gated_pool(&inner, &gate_inner);
  }
  REQUIRE_TRUE(held_outer);
  REQUIRE_TRUE(held_inner);
  ccol_retval r1 =
      cthrpool_submit(outer, noop_fn, &nc, nested_discard_outer_on_complete);
  ccol_retval r2 =
      cthrpool_submit(inner, noop_fn, &nc, nested_discard_inner_on_complete);

  pthread_t rel_outer, rel_inner;
  int c1 = pthread_create(&rel_outer, NULL, release_gate_late_fn, &gate_outer);
  if (c1 != 0) atomic_store(&gate_outer, 1);
  int c2 = pthread_create(&rel_inner, NULL, release_gate_late_fn, &gate_inner);
  if (c2 != 0) atomic_store(&gate_inner, 1);
  cthrpool_shutdown_immediate(outer);
  if (c1 == 0) pthread_join(rel_outer, NULL);
  if (c2 == 0) pthread_join(rel_inner, NULL);
  bool after_outside = cthrpool__is_self_call_for_tests(nc.outer_raw);
  cthrpool_destroy(inner);
  cthrpool_destroy(outer);

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

// Drains the pool whose handle arg points to from its own thread, so an
// immediate shutdown can escalate the drain.
static void *discard_drain_thread_fn(void *arg) {
  cthrpool_shutdown_drain(*(cthrpool *)arg);
  return NULL;
}

TEST(shutdown_immediate, escalated_drain_reports_discarded_tasks_as_not_run) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);
  ccol_retval blocker_rv = cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = blocker_rv == ccol_success && ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);
  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (cthrpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }

  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);
  pthread_t drainer;
  int drainer_created =
      pthread_create(&drainer, NULL, discard_drain_thread_fn, &pool);
  bool drain_started = false;
  if (drainer_created == 0 && raw) {
    for (int i = 0; i < 5000 && !drain_started; i++) {
      cthrpool__shutdown_state_for_tests(raw, &drain_started, NULL, NULL);
      if (!drain_started) sleep_ms(1);
    }
  }
  immediate_release_ctx_t rctx = {.gate = &gate, .raw = raw};
  pthread_t releaser;
  int created =
      pthread_create(&releaser, NULL, release_gate_after_immediate_fn, &rctx);
  if (created != 0) atomic_store(&gate, 1);
  cthrpool_shutdown_immediate(pool);
  int completions_at_return = atomic_load(&completions);
  if (created == 0) pthread_join(releaser, NULL);
  if (drainer_created == 0) pthread_join(drainer, NULL);
  cthrpool_destroy(pool);

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
  atomic_int completions = 0;
  atomic_int ran_true = 0;
  atomic_int fn_runs = 0;
  atomic_int seen[DISCARD_TASKS];
  for (int i = 0; i < DISCARD_TASKS; i++) atomic_init(&seen[i], 0);

  cthrpool_construct(pool, 2, 0);
  int accepted = 0;
  for (int i = 0; i < DISCARD_TASKS; i++) {
    discard_arg_t *a = malloc(sizeof(*a));
    if (!a) break;
    *a = (discard_arg_t){.completions = &completions,
                         .ran_true = &ran_true,
                         .fn_runs = &fn_runs,
                         .index = i,
                         .seen = seen};
    if (cthrpool_submit(pool, discard_fn, a, discard_on_complete) !=
        ccol_success) {
      free(a);
      break;
    }
    accepted++;
  }
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(accepted, DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&completions), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&ran_true), DISCARD_TASKS);
  REQUIRE_EQ(atomic_load(&fn_runs), DISCARD_TASKS);
  for (int i = 0; i < DISCARD_TASKS; i++) REQUIRE_EQ(atomic_load(&seen[i]), 1);
}

TEST(counters, pending_and_active) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 0);

  cthrpool_submit(pool, blocker_fn, &ctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  cthrpool_submit(pool, inc_counter, &counter, NULL);

  REQUIRE_EQ(cthrpool_active_count(pool), (size_t)1);
  REQUIRE_EQ(cthrpool_pending_count(pool), (size_t)1);

  atomic_store(&gate, 1);
  cthrpool_shutdown_drain(pool);

  REQUIRE_EQ(cthrpool_active_count(pool), (size_t)0);
  REQUIRE_EQ(cthrpool_pending_count(pool), (size_t)0);

  cthrpool_destroy(pool);
}

TEST(custom_mprocs, allocations_go_through_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};

  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(2, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  atomic_int counter = 0;
  for (int i = 0; i < 8; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 8);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

// A future always comes from plain malloc; only the task node goes through the
// pool's allocator. The counting allocator tells the two designs apart, which
// a failing allocator can't.
TEST(custom_mprocs, future_struct_uses_plain_allocator_not_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  int before_alloc = atomic_load(&g_alloc_count);
  int value = 42;
  cthrpool_future *f = cthrpool_submit_future(pool, identity_fn, &value, NULL);
  REQUIRE_NE((void *)f, NULL);
  int after_submit_alloc = atomic_load(&g_alloc_count);

  REQUIRE_EQ(after_submit_alloc - before_alloc, 1);

  REQUIRE_EQ(cthrpool_future_get(f), (void *)&value);
  cthrpool_future_free(f);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

// Each task completes before the next submit, so only the first submit finds
// the free list empty and calls the allocator.
TEST(custom_mprocs, task_nodes_are_recycled_not_reallocated_each_time) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  atomic_int counter = 0;
  REQUIRE_EQ(cthrpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  cthrpool_wait(pool);
  int alloc_after_first = atomic_load(&g_alloc_count);
  REQUIRE_GT(alloc_after_first, 0);

  enum { N = 20 };
  for (int i = 0; i < N; i++) {
    REQUIRE_EQ(cthrpool_submit(pool, inc_counter, &counter, NULL),
               ccol_success);
    cthrpool_wait(pool);
  }
  REQUIRE_EQ(atomic_load(&counter), N + 1);

  REQUIRE_EQ(atomic_load(&g_alloc_count), alloc_after_first);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

// Queues burst tasks behind the pool's only worker, held by a gate, and lets
// them all run, so the queue peaks at burst and the burst needs burst + 1 task
// nodes at once. True when every task ran.
static bool ctp_gated_burst(cthrpool pool, int burst) {
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t ctx = {.gate = &gate, .started = &started};
  atomic_int counter = 0;
  bool ok = (cthrpool_submit(pool, blocker_fn, &ctx, NULL) == ccol_success);
  ok = ok && ctp_await_flag(&started);
  for (int i = 0; i < burst && ok; i++)
    ok = (cthrpool_submit(pool, inc_counter, &counter, NULL) == ccol_success);
  atomic_store(&gate, 1);
  cthrpool_wait(pool);
  return ok && atomic_load(&counter) == burst;
}

TEST(custom_mprocs, task_free_list_keeps_what_the_last_burst_used) {
  cthrpool_construct(pool, 1, 0);
  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(cthrpool__task_free_list_cap_for_tests(raw), (size_t)32768);

  bool first = ctp_gated_burst(pool, 12);
  size_t after_deep = cthrpool__task_free_list_size_for_tests(raw);

  atomic_int counter = 0;
  cthrpool__set_task_free_list_window_for_tests(1);
  bool second = true;
  size_t sizes[5] = {0};
  for (int i = 0; i < 5 && second; i++) {
    second =
        (cthrpool_submit(pool, inc_counter, &counter, NULL) == ccol_success);
    cthrpool_wait(pool);
    sizes[i] = cthrpool__task_free_list_size_for_tests(raw);
  }
  cthrpool__set_task_free_list_window_for_tests(0);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
  REQUIRE_TRUE(first);
  REQUIRE_TRUE(second);
  REQUIRE_EQ(after_deep, (size_t)13);
  REQUIRE_EQ(sizes[0], (size_t)13);
  REQUIRE_EQ(sizes[1], (size_t)8);
  REQUIRE_EQ(sizes[2], (size_t)6);
  REQUIRE_EQ(sizes[3], (size_t)5);
  REQUIRE_EQ(sizes[4], (size_t)4);
}

TEST(custom_mprocs, task_free_list_never_exceeds_its_ceiling) {
  cthrpool_construct(pool, 1, 0);
  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);
  REQUIRE_NE((void *)raw, NULL);
  bool ok = ctp_gated_burst(pool, 32768 + 100);
  size_t kept = cthrpool__task_free_list_size_for_tests(raw);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(kept, (size_t)32768);
}

// Each burst empties the free list, so no trim gives anything back and later
// bursts of the same depth reuse the first burst's nodes.
TEST(custom_mprocs, repeated_deep_bursts_allocate_no_task_nodes) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);
  ccol_memmgmt_procs mp = {.malloc = tracked_malloc,
                           .free = tracked_free,
                           .calloc = tracked_calloc,
                           .realloc = tracked_realloc};
  char *err = NULL;
  cthrpool pool = cthrpool_create_mp(1, 0, &mp, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool__set_task_free_list_window_for_tests(1);
  bool ok = ctp_gated_burst(pool, 500);
  int after_first = atomic_load(&g_alloc_count);
  for (int round = 0; round < 5 && ok; round++) ok = ctp_gated_burst(pool, 500);
  int after_rest = atomic_load(&g_alloc_count);
  cthrpool__set_task_free_list_window_for_tests(0);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(after_rest, after_first);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

TEST(load, many_tasks_many_threads) {
  enum { NTASKS = 1000 };
  atomic_int counter = 0;
  cthrpool_construct(pool, 8, 0);

  for (int i = 0; i < NTASKS; i++) {
    cthrpool_submit(pool, inc_counter, &counter, NULL);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), NTASKS);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(load, bounded_queue_backpressure) {
  enum { NTASKS = 64 };
  atomic_int counter = 0;

  cthrpool_construct(pool, 2, 16);

  for (int i = 0; i < NTASKS; i++) {
    ccol_retval r = cthrpool_submit(pool, slow_inc, &counter, NULL);
    REQUIRE_EQ(r, ccol_success);
  }

  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), NTASKS);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

TEST(load, concurrent_producers) {
  enum { NPRODUCERS = 4, TASKS_PER = 250 };
  atomic_int counter = 0;
  cthrpool_construct(pool, 4, 0);

  producer_arg_t args[NPRODUCERS];
  pthread_t threads[NPRODUCERS];
  int created = 0;
  for (int i = 0; i < NPRODUCERS; i++) {
    args[i] =
        (producer_arg_t){.pool = pool, .counter = &counter, .n = TASKS_PER};
    if (pthread_create(&threads[i], NULL, producer_thread, &args[i]) != 0)
      break;
    created++;
  }
  for (int i = 0; i < created; i++) {
    pthread_join(threads[i], NULL);
  }

  cthrpool_wait(pool);
  int ran = atomic_load(&counter);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(created, (int)NPRODUCERS);
  REQUIRE_EQ(ran, NPRODUCERS * TASKS_PER);
}

// The publish into the pin table can fail on the first use of an index. The
// rollback must put the slot back on the free list in the shape a destroy
// leaves it, or the index is lost for good.
TEST(cthrpool_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  enum { CYCLES = 8 };
  size_t before = cthrpool__slot_table_capacity_for_tests();
  size_t free_before = cthrpool__free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  for (int i = 0; i < CYCLES; i++) {
    ccol__pintable_force_next_publish_failure_for_tests();
    cthrpool bad = cthrpool_create(2, 0, NULL);
    if (bad != CTHRPOOL_NULL) {
      all_failed = false;
      cthrpool__destroy(bad);
      break;
    }
    cthrpool good = cthrpool_create(2, 0, NULL);
    if (good == CTHRPOOL_NULL) {
      all_created = false;
      break;
    }
    cthrpool__destroy(good);
  }

  ccol__pintable_force_next_publish_failure_for_tests();
  char *err = NULL;
  cthrpool failed = cthrpool_create(2, 0, &err);
  bool forced_failure_applied = (failed == CTHRPOOL_NULL);
  bool err_reported = (err != NULL);
  if (!forced_failure_applied) cthrpool__destroy(failed);

  pid_t pid = fork();
  if (pid == 0) test_child_exit(0);
  int status = 0;
  bool forked = (pid != -1);
  bool reaped = forked && (waitpid(pid, &status, 0) == pid);

  cthrpool pool = cthrpool_create(2, 0, NULL);
  bool pool_ok = (pool != CTHRPOOL_NULL);
  size_t after = cthrpool__slot_table_capacity_for_tests();
  size_t free_after = cthrpool__free_index_count_for_tests();
  size_t pending = 0;
  if (pool_ok) {
    pending = cthrpool_pending_count(pool);
    cthrpool_destroy(pool);
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
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

TEST(cthrpool_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    cthrpool pool = cthrpool_create(2, 0, &err);
    if (pool == CTHRPOOL_NULL) test_child_exit(2);
    cthrpool stale = pool;
    cthrpool_destroy(pool);
    cthrpool__destroy(stale);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The exit cleanup defers its release while a pool lives, and destroying the
// last pool does it. After that the key is gone, a create is refused and a
// destroy of the old handle gives the stale-handle message. A release that left
// the key alive would let the create reach the released table.
TEST(cthrpool_handle_lifecycle, deferred_exit_release_retires_the_key) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(fds[0]);
    dup2(fds[1], STDERR_FILENO);
    close(fds[1]);
    cthrpool pool = cthrpool_create(1, 0, NULL);
    if (pool == CTHRPOOL_NULL) test_child_exit(2);
    cthrpool stale = pool;
    cthrpool__run_exit_cleanup_for_tests();
    if (!cthrpool__worker_key_live_for_tests()) test_child_exit(5);
    cthrpool_destroy(pool);
    if (cthrpool__worker_key_live_for_tests()) test_child_exit(3);
    char *err = NULL;
    cthrpool again = cthrpool_create(1, 0, &err);
    if (again != CTHRPOOL_NULL || !err) test_child_exit(4);
    cthrpool__destroy(stale);
    test_child_exit(0);
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
  REQUIRE_NE((void *)strstr(buf, "cthrpool_destroy: handle is stale"), NULL);
}

// A thread that destroys handle h, so two threads can race on one handle.
typedef struct {
  cthrpool h;
} ctp_concurrent_destroy_arg_t;

// Destroys a->h, as one side of a destroy race.
static void *ctp_concurrent_destroy_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  cthrpool__destroy(a->h);
  return NULL;
}

TEST(cthrpool_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    char *err = NULL;
    cthrpool pool = cthrpool_create(2, 0, &err);
    if (pool == CTHRPOOL_NULL) test_child_exit(2);
    ctp_concurrent_destroy_arg_t a1 = {.h = pool};
    ctp_concurrent_destroy_arg_t a2 = {.h = pool};
    pthread_t t1, t2;
    if (pthread_create(&t1, NULL, ctp_concurrent_destroy_thread, &a1) != 0)
      test_child_exit(2);
    if (pthread_create(&t2, NULL, ctp_concurrent_destroy_thread, &a2) != 0)
      test_child_exit(2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A blocking submit on its own thread, with its result.
typedef struct {
  cthrpool h;
  atomic_int *counter;
  ccol_retval rv;
} ctp_blocked_submit_arg_t;

// Submits an increment that waits on a full queue, so it holds a pin while a
// destroy runs.
static void *ctp_blocked_submit_thread(void *arg) {
  ctp_blocked_submit_arg_t *a = (ctp_blocked_submit_arg_t *)arg;
  a->rv = cthrpool_submit(a->h, inc_counter, a->counter, NULL);
  return NULL;
}

// A destroy on its own thread with entered and returned flags, so a test can
// see that the destroy started and didn't return while the worker is held.
typedef struct {
  cthrpool h;
  _Atomic bool entered;
  _Atomic bool returned;
} ctp_destroy_thread_arg_t;

// Sets entered, destroys h, sets returned, so the test can see the destroy
// start and wait.
static void *ctp_destroy_thread_fn(void *arg) {
  ctp_destroy_thread_arg_t *a = (ctp_destroy_thread_arg_t *)arg;
  atomic_store(&a->entered, true);
  cthrpool_destroy(a->h);
  atomic_store(&a->returned, true);
  return NULL;
}

// Reads /proc/self/task/<tid>/<name> into buf for the watchdog dump; false when
// it can't.
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

// Dumps status, wchan, syscall and stat of every thread of the process and
// exits with 97, so a destroy that hangs shows where every thread is instead of
// using up the job's time.
static void _ctp_test_dump_all_threads_and_die(void) {
  char buf[4096];
  cdebuglog_flush();
  fprintf(stderr,
          "[WATCHDOG_DIAG] pid=%d: cthrpool_destroy did not return within the "
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
  _exit(97);
}

// The watchdog of resolve_then_use_race_destroy_waits: done is set under mu
// once the destroy thread is joined.
typedef struct {
  pthread_mutex_t mu;
  pthread_cond_t cv;
  bool done;
} ctp_destroy_watchdog_ctx_t;

// Waits up to 60 s for done and dumps every thread when it doesn't come. The
// test's own bounded waits add up to 40 s at most, so the bound leaves room.
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

// One worker held by a task, the one-slot queue full, and a third submit
// blocked inside the wait for room with a real pin. A concurrent destroy must
// neither free the pool under that submit nor return while the worker is held;
// its drain's broadcast releases the submit, which reports
// ccol_not_permitted.
TEST(cthrpool_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  cthrpool_construct(pool, 1, 1);
  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);

  ctp_destroy_watchdog_ctx_t watchdog = {.mu = PTHREAD_MUTEX_INITIALIZER,
                                         .cv = PTHREAD_COND_INITIALIZER,
                                         .done = false};
  pthread_t watchdog_thread;
  int watchdog_rc = pthread_create(&watchdog_thread, NULL,
                                   ctp_destroy_watchdog_fn, &watchdog);
  bool watchdog_created = (watchdog_rc == 0);

  cdebuglog_write("[DEBUG_TEST] pid=%d about to submit blocker_fn\n",
                  (int)getpid());
  cthrpool_submit(pool, blocker_fn, &gctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) {
    ctp_abandon_gated_pool(&pool, &gate);
    if (watchdog_created) {
      pthread_mutex_lock(&watchdog.mu);
      watchdog.done = true;
      pthread_cond_signal(&watchdog.cv);
      pthread_mutex_unlock(&watchdog.mu);
      pthread_join(watchdog_thread, NULL);
    }
    pthread_mutex_destroy(&watchdog.mu);
    pthread_cond_destroy(&watchdog.cv);
  }
  REQUIRE_TRUE(held);
  cdebuglog_write(
      "[DEBUG_TEST] pid=%d blocker_fn started, about to fill queue\n",
      (int)getpid());

  ccol_retval fill_rv = cthrpool_submit(pool, inc_counter, &counter, NULL);

  ctp_blocked_submit_arg_t blocked_arg = {
      .h = pool, .counter = &counter, .rv = ccol_success};
  pthread_t blocked_thread;
  cdebuglog_write(
      "[DEBUG_TEST] pid=%d queue filled, about to create blocked_thread\n",
      (int)getpid());
  int blocked_rc = pthread_create(&blocked_thread, NULL,
                                  ctp_blocked_submit_thread, &blocked_arg);
  bool blocked_created = (blocked_rc == 0);
  if (blocked_created) {
    for (long long end = ctp_now_ms() + 10000;
         cthrpool__pending_resolve_count_for_tests(raw) == 0 &&
         ctp_now_ms() < end;)
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
  bool shutdown_started = false;
  if (destroy_created) {
    for (long long end = ctp_now_ms() + 10000;
         !atomic_load(&destroy_arg.entered) && ctp_now_ms() < end;)
      sleep_ms(1);
    for (long long end = ctp_now_ms() + 10000;
         !shutdown_started && ctp_now_ms() < end;) {
      cthrpool__shutdown_state_for_tests(raw, &shutdown_started, NULL, NULL);
      if (!shutdown_started) sleep_ms(1);
    }
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
  pthread_mutex_destroy(&watchdog.mu);
  pthread_cond_destroy(&watchdog.cv);

  if (blocked_created) pthread_join(blocked_thread, NULL);

  REQUIRE_EQ(fill_rv, ccol_success);
  REQUIRE_EQ(blocked_rc, 0);
  REQUIRE_EQ(destroy_rc, 0);
  REQUIRE_EQ(watchdog_rc, 0);
  REQUIRE_TRUE(shutdown_started);
  REQUIRE_FALSE(destroy_returned_before_gate_released);
  REQUIRE_EQ(blocked_arg.rv, ccol_not_permitted);
  REQUIRE_TRUE(atomic_load(&destroy_arg.returned));
}

// A thread that calls cthrpool_pending_count on h, a fast call that races a
// destroy.
typedef struct {
  cthrpool h;
} ctp_pending_count_arg_t;

// Calls cthrpool_pending_count once, as the fast side of a race with a
// destroy.
static void *ctp_pending_count_thread(void *arg) {
  ctp_pending_count_arg_t *a = (ctp_pending_count_arg_t *)arg;
  cthrpool_pending_count(a->h);
  return NULL;
}

// A fast resolve and unpin raced against a destroy; the window is a few
// instructions wide, so it repeats with a fresh pool each time.
TEST(cthrpool_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    cthrpool pool = cthrpool_create(2, 0, &err);
    REQUIRE_NE(pool, CTHRPOOL_NULL);

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
    if (!started_destroy_tid) cthrpool_destroy(pool);
    REQUIRE_TRUE(started_pending_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

// Runs cthrpool_shutdown_immediate on a->h, as one side of a race with a
// destroy.
static void *ctp_concurrent_shutdown_immediate_thread(void *arg) {
  ctp_concurrent_destroy_arg_t *a = (ctp_concurrent_destroy_arg_t *)arg;
  cthrpool_shutdown_immediate(a->h);
  return NULL;
}

// The teardown must not read shutdown_started without the lock while a pinned
// shutdown writes it; nothing may crash or hang whichever thread gets mu
// first.
TEST(cthrpool_handle_lifecycle,
     shutdown_immediate_races_concurrent_destroy_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    char *err = NULL;
    cthrpool pool = cthrpool_create(2, 0, &err);
    REQUIRE_NE(pool, CTHRPOOL_NULL);

    ctp_concurrent_destroy_arg_t shutdown_arg = {.h = pool};
    ctp_concurrent_destroy_arg_t destroy_arg = {.h = pool};
    pthread_t shutdown_tid, destroy_tid;
    bool started_shutdown_tid =
        (pthread_create(&shutdown_tid, NULL,
                        ctp_concurrent_shutdown_immediate_thread,
                        &shutdown_arg) == 0);
    bool started_destroy_tid =
        (pthread_create(&destroy_tid, NULL, ctp_concurrent_destroy_thread,
                        &destroy_arg) == 0);
    if (started_shutdown_tid) pthread_join(shutdown_tid, NULL);
    if (started_destroy_tid) pthread_join(destroy_tid, NULL);
    if (!started_destroy_tid) cthrpool_destroy(pool);
    REQUIRE_TRUE(started_shutdown_tid);
    REQUIRE_TRUE(started_destroy_tid);
  }
}

TEST(cthrpool_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  char *err = NULL;
  cthrpool a = cthrpool_create(2, 0, &err);
  REQUIRE_NE(a, CTHRPOOL_NULL);
  cthrpool stale_a = a;
  cthrpool_destroy(a);

  cthrpool b = cthrpool_create(2, 0, &err);
  REQUIRE_NE(b, CTHRPOOL_NULL);

  REQUIRE_EQ(cthrpool_pending_count(b), (size_t)0);

  REQUIRE_EQ((void *)cthrpool__resolve_for_tests(stale_a), NULL);

  cthrpool_destroy(b);
}

// The capacity is read after the first cycle, since earlier tests may have
// grown the table; this test's own churn must add nothing.
TEST(cthrpool_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  char *err = NULL;
  cthrpool pool0 = cthrpool_create(2, 0, &err);
  REQUIRE_NE(pool0, CTHRPOOL_NULL);
  cthrpool_destroy(pool0);
  size_t capacity_after_first = cthrpool__slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    cthrpool pool = cthrpool_create(2, 0, &err);
    REQUIRE_NE(pool, CTHRPOOL_NULL);
    cthrpool_destroy(pool);
  }

  REQUIRE_EQ(cthrpool__slot_table_capacity_for_tests(), capacity_after_first);
}

// A task or on_complete that destroys its own pool. That must be fatal: The
// worker touches the pool again on its way back through its loop.
typedef struct {
  cthrpool pool;
} ctp_self_destroy_ctx_t;

// Destroys the pool it runs on, which must be fatal.
static void self_destroy_task(void *arg) {
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  cthrpool_destroy(ctx->pool);
}

TEST(cthrpool_handle_lifecycle, destroy_from_within_own_task_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2);

    char *err = NULL;
    cthrpool pool = cthrpool_create(1, 0, &err);
    if (pool == CTHRPOOL_NULL) test_child_exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    cthrpool_submit(pool, self_destroy_task, &ctx, NULL);
    cthrpool_wait(pool);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// Destroys the pool from on_complete, which runs on the same worker and must
// be fatal too.
static void self_destroy_on_complete(void *arg, bool ran) {
  (void)ran;
  ctp_self_destroy_ctx_t *ctx = (ctp_self_destroy_ctx_t *)arg;
  cthrpool_destroy(ctx->pool);
}

TEST(cthrpool_handle_lifecycle, destroy_from_within_own_on_complete_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2);

    char *err = NULL;
    cthrpool pool = cthrpool_create(1, 0, &err);
    if (pool == CTHRPOOL_NULL) test_child_exit(2);
    ctp_self_destroy_ctx_t ctx = {.pool = pool};
    cthrpool_submit(pool, noop_fn, &ctx, self_destroy_on_complete);
    cthrpool_wait(pool);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A task that shuts down its own pool. That must be a full no-op that leaves
// the shutdown flags unset: Otherwise the next submit fails, and the worker's
// failed self-join would never be retried.
typedef struct {
  cthrpool pool;
  atomic_int *task_returned;
} ctp_self_shutdown_ctx_t;

// Drains its own pool and records that it returned, which a no-op must let it
// do.
static void self_shutdown_drain_task(void *arg) {
  ctp_self_shutdown_ctx_t *ctx = (ctp_self_shutdown_ctx_t *)arg;
  cthrpool_shutdown_drain(ctx->pool);
  atomic_store(ctx->task_returned, 1);
}

TEST(cthrpool_handle_lifecycle, shutdown_drain_from_within_own_task_is_a_noop) {
  cthrpool_construct(pool, 1, 0);
  atomic_int task_returned = 0;
  ctp_self_shutdown_ctx_t ctx = {.pool = pool, .task_returned = &task_returned};

  REQUIRE_EQ(cthrpool_submit(pool, self_shutdown_drain_task, &ctx, NULL),
             ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&task_returned), 1);

  atomic_int counter = 0;
  REQUIRE_EQ(cthrpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// Shuts its own pool down immediately and records that it returned.
static void self_shutdown_immediate_task(void *arg) {
  ctp_self_shutdown_ctx_t *ctx = (ctp_self_shutdown_ctx_t *)arg;
  cthrpool_shutdown_immediate(ctx->pool);
  atomic_store(ctx->task_returned, 1);
}

TEST(cthrpool_handle_lifecycle,
     shutdown_immediate_from_within_own_task_is_a_noop) {
  cthrpool_construct(pool, 1, 0);
  atomic_int task_returned = 0;
  ctp_self_shutdown_ctx_t ctx = {.pool = pool, .task_returned = &task_returned};

  REQUIRE_EQ(cthrpool_submit(pool, self_shutdown_immediate_task, &ctx, NULL),
             ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&task_returned), 1);

  atomic_int counter = 0;
  REQUIRE_EQ(cthrpool_submit(pool, inc_counter, &counter, NULL), ccol_success);
  cthrpool_wait(pool);
  REQUIRE_EQ(atomic_load(&counter), 1);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// A task that waits on its own pool, which would deadlock without the guard.
// The test runs it in a forked child under alarm().
typedef struct {
  cthrpool pool;
  atomic_int *waited_ok;
} ctp_self_wait_ctx_t;

// Waits on its own pool and records that it returned instead of deadlocking.
static void self_wait_task(void *arg) {
  ctp_self_wait_ctx_t *ctx = (ctp_self_wait_ctx_t *)arg;
  cthrpool_wait(ctx->pool);
  atomic_store(ctx->waited_ok, 1);
}

TEST(cthrpool_handle_lifecycle, wait_from_within_own_task_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
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
    alarm(2);

    char *err = NULL;
    cthrpool pool = cthrpool_create(1, 0, &err);
    char ok = 0;
    if (pool != CTHRPOOL_NULL) {
      atomic_int waited_ok = 0;
      ctp_self_wait_ctx_t ctx = {.pool = pool, .waited_ok = &waited_ok};
      cthrpool_submit(pool, self_wait_task, &ctx, NULL);

      cthrpool_wait(pool);
      ok = (atomic_load(&waited_ok) == 1) ? 1 : 0;
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

// A task that forks: The grandchild is a copy of the worker with the pool
// marked foreign and the worker key still naming the pool. A destroy there must
// still count as a self-call, or the foreign teardown frees the pool under the
// worker's own frame.
typedef struct {
  cthrpool pool;
  _Atomic pid_t grandchild_pid;
} ctp_fork_from_task_ctx_t;

// Forks from inside a task; the grandchild destroys the pool, which must end it
// with the self-call fatal error. The parent records the grandchild's pid.
static void fork_from_task_then_self_destroy(void *arg) {
  ctp_fork_from_task_ctx_t *ctx = (ctp_fork_from_task_ctx_t *)arg;
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2);
    cthrpool_destroy(ctx->pool);
    test_child_exit(0);
  }
  atomic_store(&ctx->grandchild_pid, pid);
}

TEST(cthrpool_handle_lifecycle,
     destroy_from_within_own_task_is_fatal_even_after_forking_first) {
  cthrpool_construct(pool, 1, 0);
  ctp_fork_from_task_ctx_t ctx = {.pool = pool, .grandchild_pid = -1};
  REQUIRE_EQ(
      cthrpool_submit(pool, fork_from_task_then_self_destroy, &ctx, NULL),
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

  cthrpool_wait(pool);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// The fork safety tests. They depend on the atfork machinery, so they are
// compiled out with it.
#if CCOL_FORK_SAFETY_REQUIRED

// Tells a background thread of a fork test to stop.
typedef struct {
  _Atomic int stop;
} ctp_fork_stop_arg_t;

// Keeps creating and destroying throwaway pools, so some thread holds the slot
// table lock as often as possible while the test forks.
static void *ctp_fork_churn_thread(void *arg) {
  ctp_fork_stop_arg_t *a = (ctp_fork_stop_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    cthrpool p = cthrpool_create(1, 0, &err);
    if (p != CTHRPOOL_NULL) cthrpool_destroy(p);
  }
  return NULL;
}

// A thread that keeps querying the busy pool, to widen the window in which
// another thread holds its mu at a fork.
typedef struct {
  cthrpool pool;
  _Atomic int stop;
} ctp_fork_feeder_arg_t;

// Calls cthrpool_pending_count in a loop until stop. The sched_yield keeps the
// loop from running millions of times under valgrind, which runs one thread at
// a time.
static void *ctp_fork_feeder_thread(void *arg) {
  ctp_fork_feeder_arg_t *a = (ctp_fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    (void)cthrpool_pending_count(a->pool);
    sched_yield();
  }
  return NULL;
}

// Forks again and again while other threads take the slot table lock and the
// pool's mu. Without the atfork handlers a child inherits one of them locked
// and hangs; the diagnostics above then print where.
TEST(fork_safety, fork_does_not_inherit_a_locked_cthrpool_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  ctp_fork_stop_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  bool started_churn_tid =
      (pthread_create(&churn_tid, NULL, ctp_fork_churn_thread, &churn) == 0);

  char *err = NULL;
  cthrpool pool = cthrpool_create(3, 0, &err);
  bool pool_ok = (pool != CTHRPOOL_NULL);
  atomic_int counter = 0;

  ctp_fork_feeder_arg_t feeder = {.pool = pool, .stop = 0};
  pthread_t feeder_tid;
  bool started_feeder_tid =
      pool_ok &&
      (pthread_create(&feeder_tid, NULL, ctp_fork_feeder_thread, &feeder) == 0);

  enum { TRIALS = 60 };
  enum { BURST = 8 };
  int hangs = 0;
  bool setup_ok = pool_ok;
  _diag_warm_up_backtrace();
  for (int i = 0; i < TRIALS && setup_ok; i++) {
    for (int b = 0; b < BURST && setup_ok; b++) {
      setup_ok = (cthrpool_try_submit(pool, inc_counter, &counter, NULL) ==
                  ccol_success);
    }

    int diagfd[2], okfd[2];
    if (!setup_ok || pipe(diagfd) != 0) {
      setup_ok = false;
      break;
    }
    if (pipe(okfd) != 0) {
      close(diagfd[0]);
      close(diagfd[1]);
      setup_ok = false;
      break;
    }

    pid_t pid = fork();
    if (pid == -1) {
      close(diagfd[0]);
      close(diagfd[1]);
      close(okfd[0]);
      close(okfd[1]);
      setup_ok = false;
      break;
    }
    if (pid == 0) {
      close(diagfd[0]);
      close(okfd[0]);
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      _diag_arm(diagfd[1]);

      atomic_int local_counter = 0;
      cthrpool_try_submit(pool, inc_counter, &local_counter, NULL);
      char ok = 'K';
      ssize_t w = write(okfd[1], &ok, 1);
      (void)w;
      test_child_exit(0);
    }
    close(diagfd[1]);
    close(okfd[1]);

    int status = 0;
    bool reaped = (waitpid(pid, &status, 0) == pid);
    char ok = 0;
    bool returned = reaped && read(okfd[0], &ok, 1) == 1 && ok == 'K';
    close(okfd[0]);
    if (!returned) {
      hangs++;
      fprintf(stderr,
              "trial %d: hang detected (reaped=%d WIFEXITED=%d "
              "WEXITSTATUS=%d WIFSIGNALED=%d WTERMSIG=%d)\n",
              i, reaped, WIFEXITED(status),
              WIFEXITED(status) ? WEXITSTATUS(status) : -1, WIFSIGNALED(status),
              WIFSIGNALED(status) ? WTERMSIG(status) : -1);
      _diag_report(diagfd[0]);
    }
    close(diagfd[0]);
  }

  atomic_store(&feeder.stop, 1);
  if (started_feeder_tid) pthread_join(feeder_tid, NULL);
  atomic_store(&churn.stop, 1);
  if (started_churn_tid) pthread_join(churn_tid, NULL);
  if (pool_ok) {
    cthrpool_shutdown_drain(pool);
    cthrpool_destroy(pool);
  }

  REQUIRE_TRUE(pool_ok);
  REQUIRE_TRUE(setup_ok);
  REQUIRE_EQ(hangs, 0);
  REQUIRE_TRUE(started_churn_tid);
  REQUIRE_TRUE(started_feeder_tid);
}

// A thread that holds the slot table's write lock for hold_ms while the test
// forks.
typedef struct {
  _Atomic bool locked;
  _Atomic bool released;
  int hold_ms;
} cthrpool_slot_table_fork_lock_arg_t;

// Takes the write lock, sets locked, holds it hold_ms, sets released and
// unlocks.
static void *cthrpool_slot_table_fork_lock_thread(void *arg) {
  cthrpool_slot_table_fork_lock_arg_t *a =
      (cthrpool_slot_table_fork_lock_arg_t *)arg;
  cthrpool_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  atomic_store(&a->released, true);
  cthrpool_test_wrunlock_slot_table_for_tests();
  return NULL;
}

// glibc's rwlock records its writer's thread ID, so a plain unlock in the child
// releases nothing; the child must reinitialize the lock or its next create
// hangs.
TEST(fork_safety, fork_does_not_inherit_a_write_locked_cthrpool_slot_table) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char *err = NULL;
  cthrpool pool = cthrpool_create(2, 0, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);

  enum { HOLD_MS = 300 };
  cthrpool_slot_table_fork_lock_arg_t arg = {
      .locked = false, .released = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(
      pthread_create(&holder, NULL, cthrpool_slot_table_fork_lock_thread, &arg),
      0);

  while (!atomic_load(&arg.locked)) {
    sched_yield();
  }

  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    atomic_int local_counter = 0;
    ccol_retval rv =
        cthrpool_try_submit(pool, inc_counter, &local_counter, NULL);
    char byte = (rv == ccol_success) ? 1 : 0;
    ssize_t written = write(result_pipe[1], &byte, 1);
    (void)written;
    close(result_pipe[1]);
    test_child_exit(0);
  }
  close(result_pipe[1]);

  bool released_before_fork_returned = atomic_load(&arg.released);
  pthread_join(holder, NULL);

  char byte = 0;
  ssize_t n = read(result_pipe[0], &byte, 1);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(released_before_fork_returned);

  atomic_int counter_after = 0;
  REQUIRE_EQ(cthrpool_try_submit(pool, inc_counter, &counter_after, NULL),
             ccol_success);

  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);
}

// The foreign teardown must discard the queue and cancel its futures; otherwise
// the tasks leak and cthrpool_future_get in the child waits for a worker that
// doesn't exist here. The pool is destroyed with no shutdown call first.
TEST(
    fork_safety,
    destroy_of_foreign_pool_with_queued_future_frees_queue_and_cancels_future) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  cthrpool pool = cthrpool_create(1, 1, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool_submit(pool, blocker_fn, &gctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);

  int dummy = 0;
  cthrpool_future *f = cthrpool_submit_future(pool, identity_fn, &dummy, NULL);
  if (!f) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_NE((void *)f, NULL);

  int pipefd[2];
  bool piped = pipe(pipefd) == 0;
  pid_t pid = piped ? fork() : -1;
  if (pid == -1) {
    if (piped) {
      close(pipefd[0]);
      close(pipefd[1]);
    }
    atomic_store(&gate, 1);
    (void)cthrpool_future_get(f);
    cthrpool_future_free(f);
    cthrpool_shutdown_drain(pool);
    cthrpool_destroy(pool);
  }
  REQUIRE_TRUE(piped);
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2);

    cthrpool_destroy(pool);

    void *res = cthrpool_future_get(f);
    char ok = (cthrpool_future_cancelled(f) && res == NULL) ? 1 : 0;
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  atomic_store(&gate, 1);
  void *res = cthrpool_future_get(f);
  cthrpool_future_free(f);
  cthrpool_shutdown_drain(pool);
  cthrpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
  REQUIRE_EQ(res, (void *)&dummy);
}

// No worker of an inherited pool exists in the child, so the wait must treat
// the pool as idle.
TEST(fork_safety, wait_on_foreign_pool_with_pending_work_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  atomic_int gate = 0;
  atomic_int started = 0;
  atomic_int counter = 0;
  gate_ctx_t gctx = {.gate = &gate, .started = &started};

  char *err = NULL;
  cthrpool pool = cthrpool_create(1, 0, &err);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  cthrpool_submit(pool, blocker_fn, &gctx, NULL);
  bool held = ctp_await_flag(&started);
  if (!held) ctp_abandon_gated_pool(&pool, &gate);
  REQUIRE_TRUE(held);
  cthrpool_submit(pool, inc_counter, &counter, NULL);

  pid_t pid = fork();
  if (pid == -1) {
    atomic_store(&gate, 1);
    cthrpool_shutdown_drain(pool);
    cthrpool_destroy(pool);
  }
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(2);
    cthrpool_wait(pool);
    test_child_exit(0);
  }

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  atomic_store(&gate, 1);
  cthrpool_shutdown_drain(pool);
  int final_counter = atomic_load(&counter);
  cthrpool_destroy(pool);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(final_counter, 1);
}

#endif

// An allocator that fails the Nth allocation of any kind (g_ctp_fail_at, 0
// disarms), so a sweep reaches every unwind point of pool construction.
// g_ctp_alloc_seen counts the allocations since the last arm.
static atomic_int g_ctp_alloc_seen = 0;
static atomic_int g_ctp_fail_at = 0;

// Counts one allocation and tells whether it's the one to fail; every sweep
// procs function asks it.
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
// The procs table of the sweep allocator.
static ccol_memmgmt_procs g_ctp_sweep_mp = {
    _ctp_sweep_malloc, _ctp_sweep_free, _ctp_sweep_calloc, _ctp_sweep_realloc};

// Arms the sweep allocator to fail allocation number nth, one step of the
// sweep.
static void _ctp_arm(int nth) {
  atomic_store(&g_ctp_alloc_seen, 0);
  atomic_store(&g_ctp_fail_at, nth);
}
// Disarms the sweep allocator, so later allocations succeed.
static void _ctp_disarm(void) { atomic_store(&g_ctp_fail_at, 0); }

// Deep enough to get past the last allocation of a pool construction.
#define CTP_SWEEP_DEPTH 24

TEST(cthrpool_oom, bounded_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= CTP_SWEEP_DEPTH; n++) {
    _ctp_arm(n);
    char *err = NULL;
    cthrpool pool = cthrpool_create_mp(2, 8, &g_ctp_sweep_mp, &err);
    _ctp_disarm();
    if (pool != CTHRPOOL_NULL) {
      if (cthrpool_submit(pool, NULL, NULL, NULL) == ccol_success)
        all_handled = false;
      cthrpool_destroy(pool);
    } else if (!err) {
      all_handled = false;
    }
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cthrpool_oom, unbounded_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= CTP_SWEEP_DEPTH; n++) {
    _ctp_arm(n);
    char *err = NULL;
    cthrpool pool = cthrpool_create_mp(1, 0, &g_ctp_sweep_mp, &err);
    _ctp_disarm();
    if (pool != CTHRPOOL_NULL)
      cthrpool_destroy(pool);
    else if (!err)
      all_handled = false;
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cthrpool_oom, many_worker_pool_construction_unwinds_at_every_allocation) {
  bool all_handled = true;
  for (int n = 1; n <= 40; n++) {
    _ctp_arm(n);
    cthrpool pool = cthrpool_create_mp(8, 16, &g_ctp_sweep_mp, NULL);
    _ctp_disarm();
    if (pool != CTHRPOOL_NULL) cthrpool_destroy(pool);
  }
  REQUIRE_TRUE(all_handled);
}

TEST(cthrpool_oom, a_pool_built_under_a_late_failure_still_runs_work) {
  bool built = false, ran = false;
  for (int n = 30; n <= 60 && !built; n++) {
    _ctp_arm(n);
    cthrpool pool = cthrpool_create_mp(2, 4, &g_ctp_sweep_mp, NULL);
    _ctp_disarm();
    if (pool != CTHRPOOL_NULL) {
      built = true;
      cthrpool_wait(pool);
      ran = true;
      cthrpool_destroy(pool);
    }
  }
  REQUIRE_TRUE(built);
  REQUIRE_TRUE(ran);
}

// Sleeps ms milliseconds, to pace the bounded polls of the concurrent shutdown
// tests.
static void ctp_sd_sleep_ms(int ms) {
  struct timespec ts = {.tv_sec = ms / 1000,
                        .tv_nsec = (long)(ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

// The concurrent shutdown tests: A task holds the pool's only worker until gate
// opens, so no shutdown call can have joined it and none may have returned
// while the gate is shut.
typedef struct ctp_sd_fixture {
  _Atomic int gate;
  _Atomic int t0_running;
  _Atomic int t0_ran;
  _Atomic int extra_ran;
} ctp_sd_fixture;

// The shared state of the concurrent shutdown tests.
static ctp_sd_fixture g_ctp_sd;

// Holds the only worker until the gate opens (60 s at most, as a hang guard).
static void ctp_sd_blocking_task(void *arg) {
  (void)arg;
  atomic_store(&g_ctp_sd.t0_running, 1);
  for (long long end = ctp_now_ms() + 60000;
       atomic_load(&g_ctp_sd.gate) == 0 && ctp_now_ms() < end;)
    ctp_sd_sleep_ms(1);
  atomic_fetch_add(&g_ctp_sd.t0_ran, 1);
}

// Counts a run of a queued task, so a test sees whether a shutdown ran or
// discarded it.
static void ctp_sd_extra_task(void *arg) {
  (void)arg;
  atomic_fetch_add(&g_ctp_sd.extra_ran, 1);
}

// Counts a run of a queued future task, for the same reason.
static void *ctp_sd_extra_future_task(void *arg) {
  (void)arg;
  atomic_fetch_add(&g_ctp_sd.extra_ran, 1);
  return NULL;
}

// One shutdown call (drain or immediate) on its own thread, with entered and
// returned flags.
typedef struct ctp_sd_caller {
  cthrpool pool;
  bool immediate;
  _Atomic int entered;
  _Atomic int returned;
} ctp_sd_caller;

// Makes the shutdown call between the two flags, so a test sees whether it
// returned while the worker was held.
static void *ctp_sd_caller_thread(void *arg) {
  ctp_sd_caller *c = (ctp_sd_caller *)arg;
  atomic_store(&c->entered, 1);
  if (c->immediate)
    cthrpool_shutdown_immediate(c->pool);
  else
    cthrpool_shutdown_drain(c->pool);
  atomic_store(&c->returned, 1);
  return NULL;
}

// Resets g_ctp_sd, so each test starts from a clean fixture.
static void ctp_sd_fixture_reset(void) {
  atomic_store(&g_ctp_sd.gate, 0);
  atomic_store(&g_ctp_sd.t0_running, 0);
  atomic_store(&g_ctp_sd.t0_ran, 0);
  atomic_store(&g_ctp_sd.extra_ran, 0);
}

// Waits up to timeout_ms for *flag to become 1, so a missing event fails the
// test instead of hanging it.
static bool ctp_sd_wait_flag(_Atomic int *flag, int timeout_ms) {
  for (int waited = 0; waited < timeout_ms; waited++) {
    if (atomic_load(flag) == 1) return true;
    ctp_sd_sleep_ms(1);
  }
  return atomic_load(flag) == 1;
}

// The shutdown flag is set long before any worker is joined, so a second call
// that returned on the flag would hand back a pool whose workers still run. The
// held worker makes the proof structural, not a time margin.
TEST(cthrpool_shutdown,
     a_second_concurrent_shutdown_blocks_until_workers_exit) {
  ctp_sd_fixture_reset();
  cthrpool pool = cthrpool_create(1, 0, NULL);
  bool pool_ok = (pool != CTHRPOOL_NULL);
  bool submitted = false, running = false;
  if (pool_ok) {
    submitted = (cthrpool_submit(pool, ctp_sd_blocking_task, NULL, NULL) ==
                 ccol_success);
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

  atomic_store(&g_ctp_sd.gate, 1);
  int join_failures = 0;
  for (int i = 0; i < started; i++)
    if (pthread_join(th[i], NULL) != 0) join_failures++;

  int a_returned = atomic_load(&a.returned);
  int b_returned = atomic_load(&b.returned);
  int t0_ran = atomic_load(&g_ctp_sd.t0_ran);
  if (pool_ok) cthrpool_destroy(pool);

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

// The drain is made to own the shutdown first. The queue must empty while the
// only worker is held, which only the escalation can do.
TEST(cthrpool_shutdown, immediate_escalates_a_drain_already_in_progress) {
  enum { EXTRA_TASKS = 4 };
  ctp_sd_fixture_reset();
  cthrpool pool = cthrpool_create(1, 0, NULL);
  bool pool_ok = (pool != CTHRPOOL_NULL);
  struct cthread_pool *raw = pool_ok ? cthrpool__resolve_for_tests(pool) : NULL;

  bool submitted = false, running = false;
  if (pool_ok) {
    submitted = (cthrpool_submit(pool, ctp_sd_blocking_task, NULL, NULL) ==
                 ccol_success);
    running = submitted && ctp_sd_wait_flag(&g_ctp_sd.t0_running, 5000);
  }

  cthrpool_future *futures[EXTRA_TASKS] = {NULL};
  bool all_queued = running;
  for (int i = 0; i < EXTRA_TASKS && all_queued; i++) {
    futures[i] =
        cthrpool_submit_future(pool, ctp_sd_extra_future_task, NULL, NULL);
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
        cthrpool__shutdown_state_for_tests(raw, &shutdown_started, NULL, NULL);
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

  bool queue_discarded = false;
  if (started == 2) {
    for (int i = 0; i < 5000 && !queue_discarded; i++) {
      if (cthrpool_pending_count(pool) == 0) {
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
      if (cthrpool_future_cancelled(futures[i])) cancelled++;
      cthrpool_future_free(futures[i]);
    }
  }
  int extra_ran = atomic_load(&g_ctp_sd.extra_ran);
  int t0_ran = atomic_load(&g_ctp_sd.t0_ran);
  if (pool_ok) cthrpool_destroy(pool);

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

// Enough cycles with a failing free list push that the starting free list
// can't absorb them: With recovery the table grows by one at most, without it
// by one per cycle. The reclaimed slot must give a working pool.
TEST(cthrpool_handle_lifecycle,
     a_free_list_push_failure_does_not_strand_a_slot) {
  size_t before = cthrpool__slot_table_capacity_for_tests();
  size_t free_before = cthrpool__free_index_count_for_tests();
  size_t cycles = free_before + 8;

  bool all_ok = true;
  for (size_t i = 0; i < cycles && all_ok; i++) {
    cthrpool pool = cthrpool_create(1, 0, NULL);
    if (pool == CTHRPOOL_NULL) {
      all_ok = false;
      break;
    }
    cthrpool__force_next_free_index_push_failure_for_tests();
    cthrpool_destroy(pool);
  }

  ctp_sd_fixture_reset();
  cthrpool pool = cthrpool_create(1, 0, NULL);
  bool pool_ok = (pool != CTHRPOOL_NULL);
  bool ran = false;
  if (pool_ok) {
    if (cthrpool_submit(pool, ctp_sd_extra_task, NULL, NULL) == ccol_success) {
      cthrpool_wait(pool);
      ran = (atomic_load(&g_ctp_sd.extra_ran) == 1);
    }
    cthrpool_destroy(pool);
  }

  size_t after = cthrpool__slot_table_capacity_for_tests();

  REQUIRE_TRUE(all_ok);
  REQUIRE_TRUE(pool_ok);
  REQUIRE_TRUE(ran);
  REQUIRE_LE(after, before + 1);
}

// A gate for the contention tests: Tasks count themselves in entered, park
// until open (30 s at most) and count their run in ran.
typedef struct ctp_gate {
  _Atomic bool open;
  _Atomic int entered;
  _Atomic long ran;
} ctp_gate;

// Sleeps 1 ms, the step of the gate's bounded waits.
static void ctp_gate_nap(void) {
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000L};
  nanosleep(&ts, NULL);
}

// Counts itself in, parks until the gate opens and counts its run, to hold a
// worker or fill a queue.
static void ctp_gate_task(void *arg) {
  ctp_gate *g = (ctp_gate *)arg;
  atomic_fetch_add(&g->entered, 1);
  for (int waited_ms = 0; !atomic_load(&g->open) && waited_ms < 30000;
       waited_ms++)
    ctp_gate_nap();
  atomic_fetch_add(&g->ran, 1);
}

// Counts a run in the gate, so a test can check that every task ran.
static void ctp_count_task(void *arg) {
  atomic_fetch_add(&((ctp_gate *)arg)->ran, 1);
}

// Waits up to 30 s until target tasks have entered the gate, so a test knows
// the workers are held.
static bool ctp_gate_wait_entered(ctp_gate *g, int target) {
  for (int waited_ms = 0; atomic_load(&g->entered) < target; waited_ms++) {
    if (waited_ms >= 30000) return false;
    ctp_gate_nap();
  }
  return true;
}

// A submitter of the contention test: mode 0 blocks, 1 makes 1 ms timed
// submits, 2 tries. It retries a full queue until count tasks are in or the run
// is aborted.
typedef struct ctp_stress_submitter {
  cthrpool pool;
  ctp_gate *g;
  int mode;
  long count;
  _Atomic bool *abort_run;
  long submitted;
} ctp_stress_submitter;

// Submits count tasks with its mode and counts the successes, as one of the
// competing submitters.
static void *ctp_stress_submitter_thread(void *arg) {
  ctp_stress_submitter *a = (ctp_stress_submitter *)arg;
  uint64_t tiny = 1000;
  while (a->submitted < a->count && !atomic_load(a->abort_run)) {
    ccol_retval rv;
    if (a->mode == 0) {
      rv = cthrpool_submit(a->pool, ctp_count_task, a->g, NULL);
    } else if (a->mode == 1) {
      rv = cthrpool_timed_submit(a->pool, ctp_count_task, a->g, NULL, tiny);
    } else {
      rv = cthrpool_try_submit(a->pool, ctp_count_task, a->g, NULL);
    }
    if (rv == ccol_success) {
      a->submitted++;
    } else if (rv == ccol_container_full || rv == ccol_timed_out) {
      sched_yield();
    } else {
      return NULL;
    }
  }
  return NULL;
}

// Races six submitters of every mode against eight workers on a queue of
// capacity queue_cap. True when every task ran: A lost wake leaves a thread
// asleep for good, so the wait is bounded and a timeout shuts the pool down
// immediately before the join.
static bool ctp_signal_stress(size_t queue_cap, long per_submitter) {
  enum { SUBMITTERS = 6 };
  ctp_gate g = {0};
  _Atomic bool abort_run = false;
  cthrpool pool = cthrpool_create(8, queue_cap, NULL);
  if (pool == CTHRPOOL_NULL) return false;
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
    cthrpool_shutdown_immediate(pool);
  }
  long submitted = 0;
  for (int i = 0; i < SUBMITTERS; i++) {
    if (!started[i]) continue;
    pthread_join(tids[i], NULL);
    submitted += subs[i].submitted;
  }
  if (done) cthrpool_wait(pool);
  cthrpool_destroy(pool);
  bool all_started = true;
  for (int i = 0; i < SUBMITTERS; i++) all_started = all_started && started[i];
  return all_started && done && submitted == expected &&
         atomic_load(&g.ran) == expected;
}

TEST(cthrpool_submit_cost, no_task_and_no_wake_is_lost_under_contention) {
  bool bounded = ctp_signal_stress(4, 3000);
  bool unbounded = ctp_signal_stress(0, 3000);
  REQUIRE_TRUE(bounded);
  REQUIRE_TRUE(unbounded);
}

// A timed submit on its own thread, with its result.
typedef struct ctp_timed_arg {
  cthrpool pool;
  ctp_gate *g;
  uint64_t timeout_us;
  _Atomic bool returned;
  ccol_retval rv;
} ctp_timed_arg;

// Makes the timed submit and sets returned, so the main thread sees when it
// returned.
static void *ctp_timed_submit_thread(void *arg) {
  ctp_timed_arg *a = (ctp_timed_arg *)arg;
  a->rv =
      cthrpool_timed_submit(a->pool, ctp_count_task, a->g, NULL, a->timeout_us);
  atomic_store(&a->returned, true);
  return NULL;
}

// Makes one timed submit against a full one-slot queue with the only worker
// held, and opens the gate after hold_ms. Gives the submit's result, and tells
// through *returned_early whether it returned before the gate opened.
static ccol_retval ctp_timed_submit_on_full_queue(uint64_t timeout_us,
                                                  int hold_ms,
                                                  bool *returned_early) {
  *returned_early = false;
  ctp_gate g = {0};
  cthrpool pool = cthrpool_create(1, 1, NULL);
  if (pool == CTHRPOOL_NULL) return ccol_unexpected_failure;
  ccol_retval rv = ccol_unexpected_failure;
  bool ready =
      (cthrpool_submit(pool, ctp_gate_task, &g, NULL) == ccol_success) &&
      ctp_gate_wait_entered(&g, 1) &&
      (cthrpool_submit(pool, ctp_count_task, &g, NULL) == ccol_success);
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
  cthrpool_wait(pool);
  cthrpool_destroy(pool);
  return rv;
}

// A deadline that wrapped into the past would make the submit time out at
// once.
TEST(cthrpool_timed_submit, a_timeout_past_the_end_of_time_waits_for_room) {
  bool early = true;
  ccol_retval rv = ctp_timed_submit_on_full_queue(UINT64_MAX, 200, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);

  rv = ctp_timed_submit_on_full_queue(UINT64_MAX - 999999, 200, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);
}

TEST(cthrpool_timed_submit, a_fractional_budget_waits_and_zero_does_not) {
  bool early = true;
  ccol_retval rv = ctp_timed_submit_on_full_queue(1500000, 50, &early);
  REQUIRE_FALSE(early);
  REQUIRE_EQ(rv, ccol_success);

  rv = ctp_timed_submit_on_full_queue(0, 10000, &early);
  REQUIRE_TRUE(early);
  REQUIRE_EQ(rv, ccol_container_full);
}

// Waits on raw's not_full until 100 ms ahead on CLOCK_MONOTONIC and gives how
// long it took, or -1 when it didn't time out. On CLOCK_REALTIME that deadline
// lies decades in the past, so the wait would return at once.
static long long ctp_not_full_wait_ms(struct cthread_pool *raw) {
  struct timespec abs;
  clock_gettime(CLOCK_MONOTONIC, &abs);
  abs.tv_nsec += 100000000L;
  if (abs.tv_nsec >= 1000000000L) {
    abs.tv_sec++;
    abs.tv_nsec -= 1000000000L;
  }
  long long start = ctp_now_ms();
  int rc = cthrpool__timedwait_not_full_for_tests(raw, &abs);
  long long elapsed = ctp_now_ms() - start;
  return rc == ETIMEDOUT ? elapsed : -1;
}

TEST(cthrpool_timed_submit, not_full_measures_deadlines_on_clock_monotonic) {
  cthrpool pool = cthrpool_create(1, 1, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  long long elapsed = ctp_not_full_wait_ms(cthrpool__resolve_for_tests(pool));
  cthrpool_destroy(pool);
  REQUIRE_GE(elapsed, 80LL);
}

// A deadline computed on CLOCK_REALTIME against the CLOCK_MONOTONIC condition
// lies decades ahead, so the submit would wait until the gate opens.
TEST(cthrpool_timed_submit, a_timed_submit_times_out_on_its_budget) {
  bool early = false;
  long long start = ctp_now_ms();
  ccol_retval rv = ctp_timed_submit_on_full_queue(200000, 10000, &early);
  long long elapsed = ctp_now_ms() - start;
  REQUIRE_TRUE(early);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_GE(elapsed, 150LL);
}

#if CCOL_FORK_SAFETY_REQUIRED
// The child reinitializes not_full and must keep CLOCK_MONOTONIC. It reports
// through a pipe, since valgrind can replace its exit status.
TEST(cthrpool_timed_submit, not_full_keeps_clock_monotonic_in_a_forked_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  cthrpool pool = cthrpool_create(1, 1, NULL);
  REQUIRE_NE(pool, CTHRPOOL_NULL);
  struct cthread_pool *raw = cthrpool__resolve_for_tests(pool);
  int pfd[2];
  bool piped = (pipe(pfd) == 0);
  pid_t pid = piped ? fork() : -1;
  if (pid == 0) {
    char verdict = ctp_not_full_wait_ms(raw) >= 80LL ? '1' : '0';
    ssize_t w;
    do {
      w = write(pfd[1], &verdict, 1);
    } while (w < 0 && errno == EINTR);
    test_child_exit(0);
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
  cthrpool_destroy(pool);
  REQUIRE_TRUE(piped);
  REQUIRE_TRUE(pid > 0);
  REQUIRE_TRUE(reaped);
  REQUIRE_EQ(verdict, '1');
}
#endif

// cthrpool_destroy(a[--k]) must destroy and clear every pool; evaluating the
// argument twice would skip every second one.
TEST(cthrpool_macros, destroy_evaluates_its_argument_once) {
  cthrpool pools[2] = {cthrpool_create(1, 4, NULL),
                       cthrpool_create(1, 4, NULL)};
  bool created = pools[0] != CTHRPOOL_NULL && pools[1] != CTHRPOOL_NULL;
  int k = 2;
  while (k > 0) cthrpool_destroy(pools[--k]);
  REQUIRE_TRUE(created);
  REQUIRE_EQ(k, 0);
  REQUIRE_EQ(pools[0], CTHRPOOL_NULL);
  REQUIRE_EQ(pools[1], CTHRPOOL_NULL);
}
