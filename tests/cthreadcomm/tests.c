#include <assert.h>
#include <cdebuglog.h>
#include <cthreadcomm.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <time.h>
#include <unistd.h>
TAU_MAIN()  // sets up Tau (+ main function)

/* Every [DEBUG_TEST] print in this file goes through cdebuglog_write()
 * (src/cdebuglog.c, an opt-in RUNNING_UNIT_TESTS-only module explicitly
 * added to this suite's own Makefile) rather than fprintf(stderr, ...)
 * directly: stderr is unbuffered by default, so each call would otherwise
 * cost a real write(2) syscall on the spot, and under qemu-user emulation
 * (this instrumentation exists specifically to chase CI-only, qemu-only
 * timing/hang mysteries) every syscall pays real, measurable translation
 * overhead that risks perturbing the exact timing being observed. See
 * cdebuglog.h for the full rationale.
 *
 * cdebuglog_flush() is called explicitly by _test_flush_and_exit() below, at
 * every _exit() call site in the hang-prone tests that log through this
 * buffer (since _exit() bypasses cdebuglog's own atexit-registered flush),
 * and as the first action of _sigalrm_backtrace_handler() below, for correct
 * chronological ordering relative to that handler's own raw-write() backtrace
 * output. */

/* Flushes the shared cdebuglog buffer before calling _exit(): a forked
 * child in this file that calls plain _exit() bypasses cdebuglog's own
 * atexit-registered flush, silently losing whatever it had accumulated
 * since its last automatic flush point (see cdebuglog.h). Every _exit()
 * call site in a test that logs through cdebuglog_write() must go through
 * this instead of a bare _exit(). */
static void _test_flush_and_exit(int code) {
  cdebuglog_flush();
  _exit(code);
}

/* Set by a hang-prone test right after creating its own background waiter
 * thread, so _sigalrm_backtrace_handler() below can additionally ask THAT
 * thread for its own backtrace (via SIGUSR1) rather than only ever seeing
 * whichever thread SIGALRM itself happened to be delivered to (POSIX leaves
 * that choice unspecified for a process-directed signal). 0 means "no
 * secondary thread registered for this test." */
static _Atomic uintptr_t g_hang_watch_waiter_thread = 0;

/* Diagnostic-only SIGUSR1 handler: dumps the current thread's own call stack
 * and returns (does NOT terminate), so a thread targeted by
 * pthread_kill(..., SIGUSR1) from _sigalrm_backtrace_handler below can be
 * inspected without disturbing whatever it was doing. */
static void _sigusr1_backtrace_handler(int signo) {
  (void)signo;
  char hdr[160];
  int hlen = snprintf(hdr, sizeof hdr,
                      "[DEBUG_TEST] SIGUSR1 (on-demand dump) pid=%d tid=%ld; "
                      "dumping backtrace of the signal-receiving thread:\n",
                      (int)getpid(), (long)syscall(SYS_gettid));
  if (hlen > 0) {
    ssize_t _wn = write(STDERR_FILENO, hdr, (size_t)hlen);
    (void)_wn; /* best-effort diagnostic write; nothing to do on failure */
  }
  void *frames[64];
  int depth = backtrace(frames, 64);
  backtrace_symbols_fd(frames, depth, STDERR_FILENO);
}

/* Diagnostic-only SIGALRM handler for the fork()+destroy() hang scenarios
 * below: the default SIGALRM disposition (a bare alarm() with no handler)
 * terminates the process with no indication of where it was stuck, leaving
 * a CI hang with nothing more to go on than "it took until the alarm fired."
 * This prints the signal-receiving thread's tid and its own call stack (via
 * backtrace()/backtrace_symbols_fd(), the standard, widely-used approach for
 * this exact diagnostic purpose despite backtrace() not being on the strict
 * POSIX async-signal-safe list), additionally asks the test's registered
 * background waiter thread (if any) for its own backtrace via SIGUSR1, then
 * restores the default disposition and re-raises, so the process still
 * terminates via a real SIGALRM exactly as it did before (preserving every
 * existing WIFSIGNALED/WTERMSIG(SIGALRM) check downstream), just with both
 * threads' backtraces now printed first. Only useful against a genuine
 * in-process deadlock; it cannot fire at all against the already-documented
 * qemu-user translation-lock hang, since that failure mode wedges signal
 * delivery itself, not merely the thread the signal targets. */
static void _sigalrm_backtrace_handler(int signo) {
  (void)signo;
  /* Flush whatever [DEBUG_TEST] content is still buffered before printing
   * anything else, so it appears in correct chronological order ahead of
   * this handler's own raw-write() backtrace output rather than being lost
   * entirely once the process terminates via the re-raise below. */
  cdebuglog_flush();
  char hdr[160];
  int hlen = snprintf(hdr, sizeof hdr,
                      "[DEBUG_TEST] SIGALRM fired, pid=%d tid=%ld; dumping "
                      "backtrace of the signal-receiving thread:\n",
                      (int)getpid(), (long)syscall(SYS_gettid));
  if (hlen > 0) {
    ssize_t _wn = write(STDERR_FILENO, hdr, (size_t)hlen);
    (void)_wn; /* best-effort diagnostic write; nothing to do on failure */
  }
  void *frames[64];
  int depth = backtrace(frames, 64);
  backtrace_symbols_fd(frames, depth, STDERR_FILENO);

  uintptr_t waiter = atomic_load(&g_hang_watch_waiter_thread);
  if (waiter != 0) {
    const char *msg =
        "[DEBUG_TEST] requesting registered waiter thread's own backtrace "
        "via SIGUSR1:\n";
    ssize_t _wn = write(STDERR_FILENO, msg, strlen(msg));
    (void)_wn; /* best-effort diagnostic write; nothing to do on failure */
    pthread_kill((pthread_t)waiter, SIGUSR1);
    /* Give the signal a brief, bounded window to actually be delivered and
     * printed before this handler goes on to terminate the process below;
     * best-effort only, never itself a source of an unbounded wait. */
    struct timespec wait_ts = {0, 200000000}; /* 200ms */
    nanosleep(&wait_ts, NULL);
  }

  signal(SIGALRM, SIG_DFL);
  raise(SIGALRM);
}

/* Installs both handlers above and forces backtrace()'s own lazy one-time
 * setup (unwind-table/dl_iterate_phdr caching) to happen now, in a normal
 * calling context, rather than risking that lazy init's own internal
 * allocations happening for the first time from inside a signal handler
 * later. Call this once, before alarm(), in every hang-prone forked-child
 * test that wants an actual backtrace instead of a silent SIGALRM
 * termination. A test whose background waiter thread does not exist yet at
 * this point registers it afterward via _register_hang_watch_waiter()
 * below, once pthread_create() actually returns it. */
static void _arm_sigalrm_backtrace_handler(void) {
  void *warm[4];
  backtrace(warm, 4);
  signal(SIGUSR1, _sigusr1_backtrace_handler);
  signal(SIGALRM, _sigalrm_backtrace_handler);
}

/* See _arm_sigalrm_backtrace_handler()'s own doc comment. */
static void _register_hang_watch_waiter(pthread_t t) {
  atomic_store(&g_hang_watch_waiter_thread, (uintptr_t)t);
}

extern struct event_loop_s *_event_loop_resolve_for_tests(event_loop h);
extern size_t _event_loop_slot_table_capacity_for_tests(void);
extern bool _event_loop_resolve_pin_and_sleep_for_tests(event_loop h, int ms);

extern void add_duration_to_timespec(struct timespec *target,
                                     struct timespec *duration);

/* Every write(2) call in this file is a small (a handful of bytes), single
 * fixed-size best-effort signal into a pipe this same test already owns
 * (a result/notify fd), never a partial-transfer-prone bulk transfer; so,
 * like src/cthreadcomm.c's own _eventfd_notify(), the only failure worth
 * looping on is EINTR. A bare (void) cast used to be enough to document
 * "deliberately unchecked" here, but _FORTIFY_SOURCE's fortified write(2)
 * wrapper marks itself warn_unused_result in a way a (void) cast does not
 * reliably suppress, so the return value must be genuinely consumed. */
static void test_write_retry_eintr(int fd, const void *buf, size_t n) {
  ssize_t rv;
  do {
    rv = write(fd, buf, n);
  } while (rv < 0 && errno == EINTR);
}

TEST(add_duration_to_timespec, edge_cases) {
  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 0);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 3);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 1400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 1400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 5);
    REQUIRE_EQ(t.tv_nsec, 0);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 1599999999;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 4);
    REQUIRE_EQ(t.tv_nsec, 999999999);
  }

  {
    struct timespec t;
    struct timespec duration;

    t.tv_sec = 1;
    t.tv_nsec = 1600000000;

    duration.tv_sec = 2;
    duration.tv_nsec = 400000000;

    add_duration_to_timespec(&t, &duration);

    REQUIRE_EQ(t.tv_sec, 5);
    REQUIRE_EQ(t.tv_nsec, 0);
  }
}

TEST(add_duration_to_timespec, duration_not_mutated) {
  // The function must not modify the caller's duration struct even when
  // duration->tv_nsec is overflowed and would normally be normalised.
  struct timespec t = {.tv_sec = 1, .tv_nsec = 600000000};
  struct timespec d = {.tv_sec = 2, .tv_nsec = 1500000000};
  add_duration_to_timespec(&t, &d);
  // Verify the result is correct (function was actually called).
  REQUIRE_EQ(t.tv_sec, 5);
  REQUIRE_EQ(t.tv_nsec, 100000000);
  // Duration struct must be unchanged.
  REQUIRE_EQ(d.tv_sec, 2);
  REQUIRE_EQ(d.tv_nsec, 1500000000);
}

/* Regression tests: a caller-supplied duration (or an already-computed
 * target) with a negative tv_nsec (a malformed, non-normalised struct
 * timespec no legitimate internal call in this file ever produces, but
 * nothing previously rejected either) used to flow straight through
 * un-normalised, since the pre-fix code only ever checked tv_nsec >=
 * 1000000000, never tv_nsec < 0. A negative tv_nsec surviving into the
 * result would reach cond_var_timedwait (via circq_timed_send_zc/
 * circq_timed_recv_zc/dynmq_timed_recv_zc, all of which pass a caller's own
 * timeout straight into this function), itself undefined behaviour per
 * POSIX for a struct timespec outside [0, 999999999]. Fixed by borrowing
 * whole seconds until tv_nsec is non-negative, the mirror image of the
 * pre-existing >= max_nsecs normalisation. */
TEST(add_duration_to_timespec, negative_tv_nsec_in_duration_normalised) {
  struct timespec t = {.tv_sec = 5, .tv_nsec = 0};
  struct timespec d = {.tv_sec = 2, .tv_nsec = -1};
  add_duration_to_timespec(&t, &d);
  REQUIRE_EQ(t.tv_sec, 6);
  REQUIRE_EQ(t.tv_nsec, 999999999);
  REQUIRE_GE(t.tv_nsec, 0L);
}

TEST(add_duration_to_timespec, negative_tv_nsec_in_target_normalised) {
  struct timespec t = {.tv_sec = 5, .tv_nsec = -500000000};
  struct timespec d = {.tv_sec = 0, .tv_nsec = 0};
  add_duration_to_timespec(&t, &d);
  REQUIRE_EQ(t.tv_sec, 4);
  REQUIRE_EQ(t.tv_nsec, 500000000);
  REQUIRE_GE(t.tv_nsec, 0L);
}

TEST(add_duration_to_timespec,
     negative_tv_nsec_spanning_multiple_seconds_normalised) {
  /* -1500000000ns == -2s + 500000000ns: exercises the general borrow-count
   * computation (more than a single second's worth of borrowing), not just
   * the single-second case above. Also confirms the duration struct itself
   * stays unmutated by this function's own copy-before-normalise
   * discipline, mirroring duration_not_mutated above. */
  struct timespec t = {.tv_sec = 10, .tv_nsec = 0};
  struct timespec d = {.tv_sec = 0, .tv_nsec = -1500000000};
  add_duration_to_timespec(&t, &d);
  REQUIRE_EQ(t.tv_sec, 8);
  REQUIRE_EQ(t.tv_nsec, 500000000);
  REQUIRE_EQ(d.tv_sec, 0);
  REQUIRE_EQ(d.tv_nsec, -1500000000);
}

// CIRCULAR_QUEUE TESTS

TEST(circular_queues, create_fails) {
  char *err_str = NULL;

  circular_queue *cq = circular_queue_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = circular_queue_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  cq = circular_queue_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

/* Regression test for a real, reproduced heap-buffer-overflow: prior to the
 * max_size > SIZE_MAX / sizeof(c_message_t) guard in
 * verify_circular_queue_create_inputs, any max_size in roughly
 * [max_elem_count / 16, max_elem_count] (i.e. the top slice of the
 * documented-valid "1 to max_elem_count" range) made
 * max_size * sizeof(c_message_t) wrap size_t, so the queue believed it had
 * room for max_size messages while msg_array's real allocation was tiny (or
 * exactly 0 bytes for max_size == 2^60); the very first send corrupted the
 * heap. Confirmed via a standalone AddressSanitizer repro against the
 * pre-fix code before this test was written. Both rejected values below are
 * well within max_elem_count, so only the new overflow guard (not the
 * pre-existing max_size > max_elem_count check) can be what rejects them;
 * NULL mmgmt_procs (the default allocator) is used throughout, since the
 * guard must reject both before ever calling malloc(3) at all. */
TEST(circular_queues,
     create_rejects_max_size_that_would_overflow_the_backing_array_size) {
  char *err_str = NULL;

  size_t smallest_overflowing = SIZE_MAX / sizeof(c_message_t) + 1;
  REQUIRE_LT(smallest_overflowing, max_elem_count);
  circular_queue *cq =
      circular_queue_create_with_mprocs(smallest_overflowing, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  /* The exact value confirmed, before this guard existed, to wrap
   * max_size * sizeof(c_message_t) to exactly 0 and corrupt the heap on the
   * very first send; still comfortably inside max_elem_count. Only
   * constructible on a platform where size_t is wider than 32 bits: `(size_t)1
   * << 60` is a shift by more than the width of a 32-bit size_t (e.g. i386),
   * undefined behavior and a compile error under -Werror=shift-count-overflow
   * regardless of what runtime branch would have contained it, so this half
   * of the test is guarded with a compile-time #if rather than skipped at
   * runtime. smallest_overflowing above already covers the identical overflow
   * guard on every platform, size_t width included, so no coverage is lost
   * for a 32-bit build; this second case only adds a second, independently
   * confirmed data point that happens to require a wider size_t to express. */
#if SIZE_MAX > 0xFFFFFFFFu
  size_t known_bad = (size_t)1 << 60;
  REQUIRE_LT(known_bad, max_elem_count);
  err_str = NULL;
  cq = circular_queue_create_with_mprocs(known_bad, NULL, &err_str);
  REQUIRE_EQ((void *)cq, NULL);
  REQUIRE_NE((void *)err_str, NULL);
#endif
}

/* A max_size large enough that max_size * sizeof(c_message_t) is itself a
 * sizeable allocation, but nowhere near overflowing, must still be
 * accepted; the overflow guard must not be over-strict. Mirrors
 * tests/cvector/tests.c's own create_succeeds_with_large_elem_size
 * precedent for the identical class of guard. */
TEST(circular_queues, create_succeeds_with_large_non_overflowing_max_size) {
  char *err_str = NULL;
  size_t large_max_size = 1024 * 1024; /* 16 MiB of c_message_t slots */
  circular_queue *cq =
      circular_queue_create_with_mprocs(large_max_size, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);
  circular_queue_destroy(cq);
}

TEST(circular_queues, create_and_destroy_no_mem_procs) {
  char *err_str = "";

  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, create_and_destroy_with_mem_procs) {
  char *err_str = "";

  circular_queue *cq = circular_queue_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)cq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  circular_queue_destroy(cq);
  REQUIRE_EQ((void *)cq, NULL);
}

TEST(circular_queues, basic_send_and_receive_no_mem_procs) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_with_mem_procs) {
  circular_queue *cq = circular_queue_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2;
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');
  REQUIRE_EQ(m2.size, 16);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, msg_count) {
  circular_queue *cq = circular_queue_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(circq_msg_count(cq), i);
    circq_send_zc(cq, &m1);
    REQUIRE_EQ(circq_msg_count(cq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(circq_msg_count(cq), i);
    circq_recv_zc(cq, &m1);
    REQUIRE_EQ(circq_msg_count(cq), i - 1);
  }

  circular_queue_destroy(cq);
}

TEST(circular_queues, basic_send_and_receive_NULL_msg) {
  circular_queue *cq = circular_queue_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = (void *)0xabcdef01, .size = 0x35};
  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);
  REQUIRE_EQ(m2.size, 0);

  circular_queue_destroy(cq);
}

TEST(circular_queues, try_send_and_try_receive) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(circq_try_recv_zc(cq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  circular_queue_destroy(cq);
}

#define getWallTime(A) clock_gettime(CLOCK_REALTIME, &A);
#define diffTimeUSec(A, B) \
  (B.tv_sec - A.tv_sec) * 1000000 + (B.tv_nsec - A.tv_nsec) / 1000

TEST(circular_queues, timed_send_and_timed_receive) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  struct timespec timeout;
  timeout.tv_sec = 0;           // 0  secs
  timeout.tv_nsec = 100000000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  getWallTime(before);
  REQUIRE_EQ(circq_timed_send_zc(cq, &m1, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getWallTime(before);
  REQUIRE_EQ(circq_timed_send_zc(cq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, enable_disable_sending) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  circq_disable_sending(cq);

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(circq_try_send_zc(cq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  REQUIRE_EQ(circq_timed_send_zc(cq, &m1,
                                 &(struct timespec){.tv_sec = 1, .tv_nsec = 0}),
             ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  circq_enable_sending(cq);

  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(circq_recv_zc(cq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, ring_wrap_around) {
  // With capacity 2, sending and receiving 5 messages forces head and tail
  // indices to wrap around the ring boundary multiple times.
  circular_queue *cq = circular_queue_create_with_mprocs(2, NULL, NULL);

  for (int i = 0; i < 5; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = 'A' + i;
    c_message_t out = {.data = buf, .size = 1};
    REQUIRE_EQ(circq_send_zc(cq, &out), ccol_success);
    REQUIRE_EQ(out.data, NULL);

    c_message_t in = {.data = NULL, .size = 0};
    REQUIRE_EQ(circq_recv_zc(cq, &in), ccol_success);
    REQUIRE_EQ(*(char *)in.data, 'A' + i);
    free(in.data);
  }

  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_args_null_timeout) {
  // NULL timeout must return ccol_invalid_args without crashing.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  c_message_t m = {.data = malloc(sizeof(char)), .size = 1};
  REQUIRE_EQ(circq_timed_send_zc(cq, &m, NULL), ccol_invalid_args);
  REQUIRE_NE(m.data, NULL);  // caller retains ownership on failure

  c_message_t recv_m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_timed_recv_zc(cq, &recv_m, NULL), ccol_invalid_args);

  free(m.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_send_reports_unexpected_failure_on_condvar_error) {
  /* Regression test: circq_timed_send_zc's wait loop must not report
   * ccol_unexpected_failure unconditionally on any non-ETIMEDOUT
   * cond_var_timedwait return without first re-checking whether space
   * actually became available; that re-check is exercised separately by
   * the racing test right below. This test just confirms the ordinary,
   * still-genuinely-full case: a forced error with nothing racing it must
   * still be reported as ccol_unexpected_failure, not silently ignored or
   * retried forever (a large configured timeout with a tight elapsed-time
   * bound proves it returns promptly on the forced error). */
  circular_queue *cq = circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(1), .size = 1};
  REQUIRE_EQ(circq_try_send_zc(cq, &filler), ccol_success);

  circq_test_force_next_send_condvar_wait_error();

  c_message_t m = {.data = malloc(1), .size = 1};
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(circq_timed_send_zc(cq, &m, &timeout), ccol_unexpected_failure);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_NE(m.data, NULL);  // caller retains ownership on failure
  free(m.data);

  c_message_t drained = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &drained), ccol_success);
  free(drained.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_send_condvar_error_racing_freed_slot_still_sends) {
  /* Regression test for the same class of bug already fixed in
   * ccol_select's own _sel_wait_condvar (see
   * ccol_select.timed_wait_ready_racing_condvar_error_still_succeeds):
   * circq_timed_send_zc's FAILURE branch used to report
   * ccol_unexpected_failure unconditionally, even when a concurrent
   * consumer's own receive had already freed a slot in the very same
   * instant (cond_var_timedwait always re-acquires cq->mutex before
   * returning, success or failure, so this interleaving is genuinely
   * possible in production). Uses the dedicated test hook to simulate
   * exactly that interleaving deterministically, since a real consumer
   * thread cannot actually race into this exact window on its own (the
   * hook replaces cond_var_timedwait outright rather than releasing
   * cq->mutex). Before the fix, this fails with ccol_unexpected_failure
   * and the message is never sent at all. */
  circular_queue *cq = circular_queue_create(1, NULL);
  c_message_t filler = {.data = malloc(sizeof(int)), .size = sizeof(int)};
  *(int *)filler.data = 42;
  REQUIRE_EQ(circq_try_send_zc(cq, &filler), ccol_success);

  circq_test_force_next_send_condvar_wait_error_racing_ready();

  c_message_t m = {.data = malloc(1), .size = 1};
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  REQUIRE_EQ(circq_timed_send_zc(cq, &m, &timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // ownership transferred on success

  c_message_t raced_out = circq_test_take_race_freed_msg();
  REQUIRE_NE(raced_out.data, NULL);
  REQUIRE_EQ(*(int *)raced_out.data, 42);
  free(raced_out.data);

  REQUIRE_EQ(circq_msg_count(cq), (size_t)1);
  c_message_t recv_m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_m), ccol_success);
  free(recv_m.data);
  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  /* Mirrors timed_send_reports_unexpected_failure_on_condvar_error for the
   * receive side: a forced error on an empty queue, with nothing racing
   * it, must still be reported as ccol_unexpected_failure promptly. */
  circular_queue *cq = circular_queue_create(1, NULL);

  circq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_unexpected_failure);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* Regression test for the receive-side analogue of
   * timed_send_condvar_error_racing_freed_slot_still_sends: a concurrent
   * producer's own send completing in the same instant an unrelated
   * cond_var_timedwait error is (forced to be) reported must still be
   * received, not discarded and reported as ccol_unexpected_failure.
   * Before the fix, this fails with ccol_unexpected_failure and the
   * sentinel message the hook enqueued is silently lost inside the queue
   * forever (msg_count would stay at 1 with nothing ever able to receive
   * it, since the caller already gave up). */
  circular_queue *cq = circular_queue_create(4, NULL);

  circq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // clobbered on success
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel the hook enqueued
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(circq_msg_count(cq), (size_t)0);

  circular_queue_destroy(cq);
}

TEST(circular_queues, recv_drains_successfully_after_disable) {
  // Messages already in the queue must still be receivable after sending is
  // disabled. Once the queue is empty a timed recv must time out rather than
  // return ccol_not_permitted (disabling only affects senders).
  circular_queue *cq = circular_queue_create_with_mprocs(2, NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);

  circq_disable_sending(cq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(circq_recv_zc(cq, &m), ccol_success);
  REQUIRE_EQ(circq_recv_zc(cq, &m), ccol_success);

  // Queue is now empty with sending disabled: timed recv must time out,
  // not return ccol_not_permitted.
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};  // 50 ms
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_timed_out);

  circular_queue_destroy(cq);
}

typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} cq_disable_recv_args;

void *cq_blocking_recv_thread(void *raw) {
  cq_disable_recv_args *a = (cq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = circq_recv_zc(a->cq, &m);
  return NULL;
}

TEST(circular_queues, disable_sending_does_not_unblock_recv) {
  // A thread blocked in circq_recv_zc must NOT be woken by
  // circq_disable_sending. It must stay blocked and only return once sending
  // is re-enabled and a message arrives.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  cq_disable_recv_args args = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  circq_disable_sending(cq);
  usleep(20000);  // receiver must still be blocked at this point

  // Re-enable and send a message to unblock the receiver.
  circq_enable_sending(cq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  circular_queue_destroy(cq);
}

TEST(circular_queues, timed_recv_times_out_when_disabled) {
  // circq_timed_recv_zc must return ccol_timed_out (not ccol_not_permitted)
  // when the queue is empty and sending is disabled. The disable state must not
  // cause an early return before the deadline.
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  circq_disable_sending(cq);

  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(circq_timed_recv_zc(cq, &m, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  circular_queue_destroy(cq);
}

void *cq_helper_thread(void *args) {
  circular_queue *cq = (circular_queue *)args;
  // Let's make the sender block while sending the second message.
  usleep(50000);

  c_message_t m = {.data = NULL, .size = 0};
  assert(circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  assert(circq_recv_zc(cq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'B');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(circular_queues, send_and_receive_thread) {
  circular_queue *cq = circular_queue_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, cq_helper_thread, cq), 0);

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  m = (c_message_t){.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'B';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(circq_send_zc(cq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  circular_queue_destroy(cq);
}

/* Reads /proc/<pid>/<name> (a single-line-ish pseudo-file) into buf, NUL-
 * terminated, returning true on success. Best-effort diagnostic-only: false
 * on any failure (process already fully reaped, permission, etc.), which
 * the caller reports as part of the dump rather than treating as fatal --
 * this runs only after a bounded wait has already timed out, so it must
 * never itself introduce a new way to hang or crash the test binary. */
static bool _read_proc_file(pid_t pid, const char *name, char *buf,
                            size_t buf_cap) {
  char path[64];
  snprintf(path, sizeof path, "/proc/%d/%s", (int)pid, name);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t n = read(fd, buf, buf_cap - 1);
  close(fd);
  if (n < 0) return false;
  buf[n] = '\0';
  return true;
}

/* Diagnostic-only: dumps whatever the host kernel's /proc still says about
 * pid (and each of its own threads, if any) after a bounded wait for it has
 * already timed out -- see _wait_for_forked_child_bounded's own doc comment
 * for why an unbounded parent-side waitpid() is no longer trusted here.
 * Deliberately reads real files via plain, buffered stdio/read() calls
 * rather than anything signal-handler-safe or cdebuglog-buffered: unlike
 * the now-removed child-side SIGALRM instrumentation this mirrors in spirit
 * (see this suite's own git history), this only ever runs on the already-
 * failing path, well after the timing window that mattered has closed, so
 * perturbing it further costs nothing.
 *
 * /proc/<pid>/status's own State: line is the single most useful field
 * here: Z (zombie) would mean the child has genuinely already exited and
 * the kernel is just waiting to be reaped, pointing at a bug in how
 * waitpid() itself observes that under emulation rather than anything the
 * child was actually doing; D/R/S with a real /proc/<pid>/syscall entry
 * points at the child (or a specific one of its threads) genuinely still
 * being alive and stuck somewhere identifiable. status/wchan/syscall/stat
 * are each read independently so one missing/unreadable file (e.g. wchan
 * requiring a config the running kernel might lack) does not suppress the
 * others. */
static void _dump_stuck_child_diagnostics(pid_t pid) {
  char buf[4096];
  fprintf(stderr,
          "[STUCK_CHILD_DIAG] parent pid=%d timed out waiting for child "
          "pid=%d; dumping /proc diagnostics\n",
          (int)getpid(), (int)pid);

  static const char *const files[] = {"status", "wchan", "syscall", "stat"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (_read_proc_file(pid, files[i], buf, sizeof buf)) {
      fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid,
              files[i], buf);
    } else {
      fprintf(stderr,
              "[STUCK_CHILD_DIAG] /proc/%d/%s: unreadable (errno=%d %s)\n",
              (int)pid, files[i], errno, strerror(errno));
    }
  }

  char task_dir[64];
  snprintf(task_dir, sizeof task_dir, "/proc/%d/task", (int)pid);
  DIR *td = opendir(task_dir);
  if (!td) {
    fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/task: unreadable\n", (int)pid);
  } else {
    struct dirent *ent;
    while ((ent = readdir(td)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      char rel[16 + sizeof(ent->d_name)];
      snprintf(rel, sizeof rel, "task/%s/status", ent->d_name);
      if (_read_proc_file(pid, rel, buf, sizeof buf)) {
        fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid, rel,
                buf);
      }
    }
    closedir(td);
  }
  fflush(stderr);
}

/* Bounded replacement for a blocking waitpid(pid, status_out, 0): polls with
 * WNOHANG up to timeout_ms (wall-clock), returning true and setting
 * *status_out the moment pid is reaped, or false if timeout_ms elapses
 * first. A genuinely reproduced qemu-user hang can leave a forked child's
 * own alarm()-bounded lifetime irrelevant: the child completes and even
 * visibly aborts (SIGABRT, a core dump), yet the parent's own waitpid()
 * still never returns -- see this suite's own git history for the
 * SIGALRM-in-the-child instrumentation that was built to chase this and
 * removed once it looked resolved, and this function's own sibling
 * (_dump_stuck_child_diagnostics) for why that removed tooling was aimed
 * at the wrong side. An unbounded parent-side wait, unlike an unbounded
 * child-side one, has no alarm() of its own to fall back on, so it can hang
 * this entire test binary (and whatever CI job runs it) indefinitely
 * regardless of how well-bounded every forked child in this file already
 * is. On timeout, dumps diagnostics and makes a best-effort attempt to
 * SIGKILL and reap pid anyway (so a merely-slow-not-actually-stuck child
 * does not outlive this test as an orphan); that reap attempt is itself
 * bounded and its own outcome does not change this function's own false
 * return, since the timeout already establishes the failure this test
 * needs to report. */
static bool _wait_for_forked_child_bounded(pid_t pid, int *status_out,
                                           int timeout_ms) {
  enum { POLL_INTERVAL_MS = 50 };
  int elapsed_ms = 0;
  while (elapsed_ms < timeout_ms) {
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid) return true;
    if (r == -1) return false; /* e.g. ECHILD: nothing left to wait for */
    struct timespec ts = {.tv_sec = POLL_INTERVAL_MS / 1000,
                          .tv_nsec = (POLL_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    elapsed_ms += POLL_INTERVAL_MS;
  }

  _dump_stuck_child_diagnostics(pid);

  kill(pid, SIGKILL);
  for (int i = 0; i < 20; i++) { /* up to 1s, best-effort only */
    pid_t r = waitpid(pid, status_out, WNOHANG);
    if (r == pid || r == -1) break;
    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);
  }
  return false;
}

/* Bounded alternative to a plain blocking read() on a forked child's result
 * pipe. Every fork_safety test below that uses a result pipe relies on the
 * forked child's own alarm() to eventually close its write end (either by
 * writing the result byte and exiting normally, or by the default SIGALRM
 * disposition terminating it on a hang), so a bare read() there was assumed
 * safe: it can only ever block as long as the child's own alarm bound
 * allows. A qemu-user binary-translation lock held across the guest fork()
 * (the same class this project's CI infrastructure notes already document
 * for a hung linux-aarch64-clang run) can wedge a forked child's own
 * itimer/signal delivery right along with everything else it depends on,
 * silently disabling that alarm bound entirely and leaving the write end of
 * the pipe open indefinitely with nothing ever written or closed. Bounding
 * the read itself with poll() closes that gap independently of whatever
 * bound the child's own alarm() call was supposed to provide, mirroring
 * _wait_for_forked_child_bounded's own reasoning above for why an unbounded
 * parent-side wait is never trusted alone in this file. Returns the same
 * value read() would (1 on a byte actually observed, 0 on a genuine EOF)
 * within timeout_ms; also returns 0 (as if EOF) on a timeout, so every
 * existing REQUIRE_EQ((int)n, 1) call site downstream still fails that one
 * test cleanly instead of hanging the whole binary. */
static ssize_t _read_result_byte_bounded(int fd, char *out_byte,
                                         int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec += 1;
    deadline.tv_nsec -= 1000000000L;
  }

  for (;;) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    long remaining_ms = (deadline.tv_sec - now.tv_sec) * 1000L +
                        (deadline.tv_nsec - now.tv_nsec) / 1000000L;
    if (remaining_ms <= 0) return 0; /* timed out; treat like EOF */

    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    int r = poll(&pfd, 1, (int)remaining_ms);
    if (r == 0) return 0; /* timed out; treat like EOF */
    if (r < 0) {
      if (errno == EINTR) continue;
      return -1;
    }
    return read(fd, out_byte, 1);
  }
}

/* Regression tests for a real use-after-free hazard: destroying a queue
 * while it still has a linked ccol_select()/event_loop waiter left that
 * waiter's own node holding a dangling pointer into the queue's
 * about-to-be-freed mutex, with nothing to catch it (unlike the existing
 * msg_count > 0 check, which only ever caught leftover messages). Both
 * __circular_queue_destroy and __dynamic_queue_destroy now assert if either
 * sel_read_waiters_head or sel_write_waiters_head is still non-NULL. Run in
 * a forked child since the resulting ccol_assert()/abort() aborts the whole
 * process. */
TEST(circular_queues, destroy_with_live_event_loop_registration_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's own lifetime so an unexpected hang here fails this
     * one test loudly and fast (the default SIGALRM disposition terminates
     * the process, which the parent's own bounded wait below then simply
     * observes as WIFSIGNALED/SIGALRM rather than SIGABRT), independently
     * of the parent's own bound below. */
    alarm(10);
    circular_queue *cq = circular_queue_create(4, NULL);
    if (!cq) _exit(2);
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);
    event_handlers_t handlers = {0};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers, NULL, &err);
    if (reg == EVENT_REG_INVALID) _exit(2);
    /* The actual misuse under test: the registration above is never removed
     * (and the loop never destroyed) before this call. */
    circular_queue_destroy(cq);
    _exit(0); /* unreachable if the assert fired as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* 20s: comfortably longer than the child's own alarm(10), leaving margin
   * for the parent to actually observe and reap it even under emulation;
   * see _wait_for_forked_child_bounded's own doc comment for why this
   * bound exists at all, not just the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct cq_select_waiter_args {
  circular_queue *cq;
} cq_select_waiter_args;

static void *cq_select_waiter_thread(void *arg) {
  cq_select_waiter_args *a = (cq_select_waiter_args *)arg;
  size_t idx;
  /* Long enough that the main thread's own destroy call (racing this) is
   * always what ends the process first; this call itself never needs to
   * return. */
  ccol_select_timed_va(&idx, 5000,
                       selectable_from_circq(a->cq, ccol_select_read));
  return NULL;
}

TEST(circular_queues, destroy_while_ccol_select_is_watching_is_fatal) {
  cdebuglog_write("[DEBUG_TEST] pid=%d about to fork()\n", (int)getpid());
  pid_t pid = fork();
  cdebuglog_write("[DEBUG_TEST] pid=%d fork() returned %d\n", (int)getpid(),
                  (int)pid);
  if (pid == 0) {
    /* Bounds this child's own lifetime so an unexpected hang here fails
     * this one test loudly and fast instead of hanging the entire test
     * binary (and whatever CI job is running it) indefinitely; see this
     * alarm's identical use in the sibling test above for the full
     * rationale, and _wait_for_forked_child_bounded's own doc comment for
     * why the parent below has its own, independent bound too. */
    _arm_sigalrm_backtrace_handler();
    alarm(10);
    cdebuglog_write("[DEBUG_TEST] child pid=%d alarm set, creating queue\n",
                    (int)getpid());
    circular_queue *cq = circular_queue_create(4, NULL);
    if (!cq) _test_flush_and_exit(2);
    cdebuglog_write(
        "[DEBUG_TEST] child pid=%d queue created, spawning waiter\n",
        (int)getpid());

    cq_select_waiter_args wargs = {.cq = cq};
    pthread_t waiter;
    /* Checked via an if-guard, not REQUIRE_EQ: this runs inside the forked
     * child above (pid == 0), where REQUIRE_EQ's failure path (an early
     * `return` out of this Tau test function) would return out of THIS
     * test function while still running as the forked child, skipping the
     * exit calls below and falling into the harness's own subsequent
     * test-running loop a second time in a process only ever meant to run
     * this one child-side branch; see fork_does_not_deadlock_with_queue_
     * registered_before_event_loop's own identical fix/comment further
     * down in this file for the full reasoning. A failed create here is
     * already bounded by the poll loop below (linked never becomes true
     * with no thread to link it), but exiting immediately with a distinct
     * code makes the failure's actual cause visible in the exit status
     * rather than surfacing 2 seconds later as an indistinguishable
     * REQUIRE_TRUE(linked) failure. */
    if (pthread_create(&waiter, NULL, cq_select_waiter_thread, &wargs) != 0)
      _test_flush_and_exit(4);
    _register_hang_watch_waiter(waiter);
    cdebuglog_write("[DEBUG_TEST] child pid=%d waiter thread created\n",
                    (int)getpid());

    /* Poll for the waiter thread to have ACTUALLY linked itself into cq's
     * own read-waiter list (real synchronization on the condition this test
     * needs, not a fixed sleep guessing at it): pthread_create() returning
     * gives no guarantee the new thread has been scheduled at all yet, let
     * alone reached its own first mutex_lock/link step inside ccol_select_
     * timed's Phase 1. A fixed-sleep hand-off here (this test's own former
     * design) previously lost this race under qemu-user emulation's much
     * higher and more variable thread-start scheduling latency: destroy()
     * ran first, correctly saw no waiters linked yet (nothing to catch),
     * froze cq, and the late-arriving waiter thread then dereferenced that
     * freed memory the moment it was finally scheduled; a genuine
     * use-after-free whose undefined behaviour manifested as the whole
     * child hanging forever, which this test's own then-unbounded waitpid
     * below had no way to ever notice. 200 iterations * 10ms = 2s bound,
     * comfortably above any realistic scheduling delay while still being a
     * hard bound; if it's never satisfied, the REQUIRE_TRUE below fails
     * this test cleanly instead of proceeding into the same race. */
    bool linked = false;
    for (int i = 0; i < 200 && !linked; i++) {
      linked = circq_test_has_sel_read_waiter_for_tests(cq);
      if (!linked) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, NULL);
      }
    }
    /* unreachable in practice; see REQUIRE_TRUE below */
    if (!linked) _test_flush_and_exit(3);
    cdebuglog_write(
        "[DEBUG_TEST] child pid=%d confirmed linked, calling destroy\n",
        (int)getpid());
    /* Flush now, immediately before the one call in this test that is
     * EXPECTED to terminate the process via ccol_assert()/abort() (not a
     * controlled _exit() this file can wrap): abort() bypasses atexit()
     * exactly like _exit() does, so without this explicit flush here, every
     * [DEBUG_TEST] checkpoint logged since the last flush point would
     * silently vanish on every ordinary, PASSING run of this test, leaving
     * only the SIGALRM-handler path (the abnormal, hanging case) able to
     * ever see this test's own diagnostic trail. */
    cdebuglog_flush();

    /* The actual misuse under test: the waiter thread above is confirmed
     * still linked into cq's own waiter list when this destroys cq. */
    circular_queue_destroy(cq);
    cdebuglog_write(
        "[DEBUG_TEST] child pid=%d destroy returned (should be unreachable!)\n",
        (int)getpid());
    _test_flush_and_exit(0); /* unreachable if the assert fired as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* See _wait_for_forked_child_bounded's own doc comment and the sibling
   * test above for why 20s and why this bound exists at all. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// DYNAMIC_QUEUE TESTS

TEST(dynamic_queues, create_fails) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = NULL},
      &err_str);
  REQUIRE_EQ((void *)dq, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_no_mprocs) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, create_and_destroy_with_mprocs) {
  char *err_str = "";

  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)dq, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  dynamic_queue_destroy(dq);
  REQUIRE_EQ((void *)dq, NULL);
}

TEST(dynamic_queues, basic_send_and_receive_no_mprocs) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_with_mprocs) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);  // The ownership of the message is lost.

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);

  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, msg_count) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(dynmq_msg_count(dq), i);
    dynmq_send_zc(dq, &m1);
    REQUIRE_EQ(dynmq_msg_count(dq), i + 1);
  }

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(dynmq_msg_count(dq), i);
    dynmq_recv_zc(dq, &m1);
    REQUIRE_EQ(dynmq_msg_count(dq), i - 1);
  }

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, basic_send_and_receive_NULL_msg) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 0;
  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_invalid_args);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_EQ(m2.data, NULL);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, fifo_ordering) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  const char labels[] = {'A', 'B', 'C'};
  for (int i = 0; i < 3; ++i) {
    char *buf = malloc(sizeof(char));
    *buf = labels[i];
    c_message_t m = {.data = buf, .size = 1};
    REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
    REQUIRE_EQ(m.data, NULL);
  }
  REQUIRE_EQ(dynmq_msg_count(dq), 3);

  for (int i = 0; i < 3; ++i) {
    c_message_t m = {.data = NULL, .size = 0};
    REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);
    REQUIRE_EQ(*(char *)m.data, labels[i]);
    free(m.data);
  }

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_try_receive) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  REQUIRE_EQ(dynmq_try_recv_zc(dq, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, send_and_timed_receive) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  struct timespec timeout;
  timeout.tv_sec = 0;           // 0  secs
  timeout.tv_nsec = 100000000;  // 100 msecs

  struct timespec before;
  struct timespec after;

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);

  c_message_t m2 = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_reports_unexpected_failure_on_condvar_error) {
  /* Mirrors circular_queues.timed_recv_reports_unexpected_failure_on_
   * condvar_error: a forced error on an empty queue, with nothing racing
   * it, must still be reported as ccol_unexpected_failure promptly. */
  dynamic_queue *dq = dynamic_queue_create(NULL);

  dynmq_test_force_next_recv_condvar_wait_error();

  c_message_t m = {.data = NULL, .size = 0};
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_unexpected_failure);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_condvar_error_racing_message_still_receives) {
  /* Regression test for the same class of bug already fixed in
   * ccol_select's own _sel_wait_condvar: dynmq_timed_recv_zc's FAILURE
   * branch used to report ccol_unexpected_failure unconditionally, even
   * when a concurrent producer's own send had already enqueued a message
   * in the very same instant (cond_var_timedwait always re-acquires
   * dq->mutex before returning, success or failure, so this interleaving
   * is genuinely possible in production). Uses the dedicated test hook to
   * simulate exactly that interleaving deterministically, since a real
   * producer thread cannot actually race into this exact window on its
   * own. Before the fix, this fails with ccol_unexpected_failure and the
   * sentinel message the hook enqueued is silently lost inside the queue
   * forever. */
  dynamic_queue *dq = dynamic_queue_create(NULL);

  dynmq_test_force_next_recv_condvar_wait_error_racing_ready();

  c_message_t m = {.data = (void *)1, .size = 1};  // clobbered on success
  struct timespec timeout = {.tv_sec = 5, .tv_nsec = 0};
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // the {NULL, 0} sentinel the hook enqueued
  REQUIRE_EQ(m.size, (size_t)0);
  REQUIRE_EQ(dynmq_msg_count(dq), (size_t)0);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, enable_disable_sending) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m1 = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m1.data))[0] = 'A';
  ((char *)(m1.data))[1] = '\0';

  dynmq_disable_sending(dq);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  dynmq_enable_sending(dq);

  REQUIRE_EQ(dynmq_send_zc(dq, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(dynmq_recv_zc(dq, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(((char *)(m2.data))[0], 'A');
  REQUIRE_EQ(((char *)(m2.data))[1], '\0');

  free(m2.data);
  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_null_timeout) {
  // NULL timeout must return ccol_invalid_args without crashing.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, NULL), ccol_invalid_args);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, recv_drains_successfully_after_disable) {
  // Messages already in the queue must still be receivable after sending is
  // disabled. Once the queue is empty a timed recv must time out rather than
  // return ccol_not_permitted (disabling only affects senders).
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);

  dynmq_disable_sending(dq);

  // Existing messages must still drain successfully.
  REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);
  REQUIRE_EQ(dynmq_recv_zc(dq, &m), ccol_success);

  // Queue is now empty with sending disabled: timed recv must time out,
  // not return ccol_not_permitted.
  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 50000000};  // 50 ms
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_timed_out);

  dynamic_queue_destroy(dq);
}

typedef struct {
  dynamic_queue *dq;
  ccol_retval_t result;
} dq_disable_recv_args;

void *dq_blocking_recv_thread(void *raw) {
  dq_disable_recv_args *a = (dq_disable_recv_args *)raw;
  c_message_t m = {.data = NULL, .size = 0};
  a->result = dynmq_recv_zc(a->dq, &m);
  return NULL;
}

TEST(dynamic_queues, disable_sending_does_not_unblock_recv) {
  // A thread blocked in dynmq_recv_zc must NOT be woken by
  // dynmq_disable_sending. It must stay blocked and only return once sending
  // is re-enabled and a message arrives.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  dq_disable_recv_args args = {.dq = dq, .result = ccol_unexpected_failure};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_blocking_recv_thread, &args), 0);

  usleep(20000);  // let the receiver block on the empty queue
  dynmq_disable_sending(dq);
  usleep(20000);  // receiver must still be blocked at this point

  // Re-enable and send a message to unblock the receiver.
  dynmq_enable_sending(dq);
  c_message_t m = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  pthread_join(tid, NULL);

  REQUIRE_EQ(args.result, ccol_success);

  dynamic_queue_destroy(dq);
}

TEST(dynamic_queues, timed_recv_times_out_when_disabled) {
  // dynmq_timed_recv_zc must return ccol_timed_out (not ccol_not_permitted)
  // when the queue is empty and sending is disabled. The disable state must not
  // cause an early return before the deadline.
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  dynmq_disable_sending(dq);

  struct timespec timeout = {.tv_sec = 0, .tv_nsec = 100000000};  // 100 ms
  struct timespec before, after;
  c_message_t m = {.data = NULL, .size = 0};

  getWallTime(before);
  REQUIRE_EQ(dynmq_timed_recv_zc(dq, &m, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 100000);  // must wait out the timeout

  dynamic_queue_destroy(dq);
}

void *dq_helper_thread(void *args) {
  dynamic_queue *dq = (dynamic_queue *)args;

  c_message_t m = {.data = NULL, .size = 0};
  assert(dynmq_recv_zc(dq, &m) == ccol_success);
  assert(((char *)(m.data))[0] == 'A');
  assert(((char *)(m.data))[1] == '\0');
  free(m.data);
  m.data = NULL;

  return NULL;
}

TEST(dynamic_queues, send_and_receive_thread) {
  dynamic_queue *dq = dynamic_queue_create_with_mprocs(NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, dq_helper_thread, dq), 0);

  usleep(50000);  // Let's make the receiver wait

  c_message_t m = {.data = malloc(16 * sizeof(char)), .size = 16};
  ((char *)(m.data))[0] = 'A';
  ((char *)(m.data))[1] = '\0';

  REQUIRE_EQ(dynmq_send_zc(dq, &m), ccol_success);
  REQUIRE_EQ(m.data, NULL);  // The ownership of the message is lost.

  pthread_join(tid, NULL);

  dynamic_queue_destroy(dq);
}

/* dynamic_queue counterpart of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above;
 * see that test's own comment. */
TEST(dynamic_queues, destroy_with_live_event_loop_registration_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    dynamic_queue *dq = dynamic_queue_create(NULL);
    if (!dq) _exit(2);
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);
    event_handlers_t handlers = {0};
    char *err = NULL;
    event_reg reg = event_loop_add(
        loop, selectable_from_dynq(dq, ccol_select_read), handlers, NULL, &err);
    if (reg == EVENT_REG_INVALID) _exit(2);
    /* The actual misuse under test: the registration above is never removed
     * (and the loop never destroyed) before this call. */
    dynamic_queue_destroy(dq);
    _exit(0); /* unreachable if the assert fired as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// CHANNEL TESTS

TEST(channels, create_fails) {
  char *err_str = NULL;

  channel *ch = channel_create_with_mprocs(0, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = channel_create_with_mprocs(-1, NULL, &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);

  ch = channel_create_with_mprocs(
      (size_t)INT32_MAX,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = NULL, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_EQ((void *)ch, NULL);
  REQUIRE_NE((void *)err_str, NULL);
}

TEST(channels, create_and_destroy_no_mprocs) {
  char *err_str = "";

  channel *ch = channel_create_with_mprocs(1, NULL, &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

TEST(channels, create_and_destroy_with_mprocs) {
  char *err_str = "";

  channel *ch = channel_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      &err_str);
  REQUIRE_NE((void *)ch, NULL);
  REQUIRE_EQ((void *)err_str, NULL);

  channel_destroy(ch);
  REQUIRE_EQ((void *)ch, NULL);
}

void *thr_for_channels_basic_send_and_receive(void *args) {
  // Using direct assertions in helper threads
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  assert(chan_recv_zc(ch, &msg) == ccol_success);

  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  assert(chan_send_zc(ch, &msg) == ccol_success);

  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, basic_send_and_receive_no_mprocs) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

TEST(channels, basic_send_and_receive_with_mprocs) {
  channel *ch = channel_create_with_mprocs(
      1,
      &(ccol_memmgmt_procs_t){
          .calloc = calloc, .free = free, .malloc = malloc, .realloc = realloc},
      NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_basic_send_and_receive, ch),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *((char *)m1.data) = 'A';
  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*((char *)m2.data), 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_msg_count(void *args) {
  // Using direct assertions in helper threads
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};

  for (size_t i = 3; i > 0; --i) {
    assert(chan_msg_count(ch, owner_to_workers) == i);
    chan_recv_zc(ch, &msg);
    assert(chan_msg_count(ch, owner_to_workers) == i - 1);
  }

  for (size_t i = 0; i < 3; ++i) {
    assert(chan_msg_count(ch, workers_to_owner) == i);
    chan_send_zc(ch, &msg);
    assert(chan_msg_count(ch, workers_to_owner) == i + 1);
  }

  return NULL;
}

TEST(channels, msg_count) {
  channel *ch = channel_create_with_mprocs(3, NULL, NULL);

  c_message_t m1 = {.data = NULL, .size = 0};

  for (size_t i = 0; i < 3; ++i) {
    REQUIRE_EQ(chan_msg_count(ch, owner_to_workers), i);
    chan_send_zc(ch, &m1);
    REQUIRE_EQ(chan_msg_count(ch, owner_to_workers), i + 1);
  }

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_channels_msg_count, ch), 0);

  usleep(100000);

  for (size_t i = 3; i > 0; --i) {
    REQUIRE_EQ(chan_msg_count(ch, workers_to_owner), i);
    chan_recv_zc(ch, &m1);
    REQUIRE_EQ(chan_msg_count(ch, workers_to_owner), i - 1);
  }

  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_try_send_and_try_receive(void *args) {
  channel *ch = (channel *)args;

  usleep(50000);  // 50 msecs

  c_message_t msg = {.data = NULL, .size = 0};
  assert(chan_try_recv_zc(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  assert(chan_try_recv_zc(ch, &m2) == ccol_container_empty);
  assert(m2.data == NULL);

  assert(chan_try_send_zc(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  assert(chan_try_send_zc(ch, &m2) == ccol_container_full);
  assert(m2.data != NULL);
  free(m2.data);

  return NULL;
}

TEST(channels, try_send_and_try_receive) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_for_channels_try_send_and_try_receive, ch),
      0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';
  REQUIRE_EQ(chan_try_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  REQUIRE_EQ(chan_try_send_zc(ch, &m1), ccol_container_full);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  usleep(100000);  // 100 msecs

  c_message_t m2 = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_try_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  REQUIRE_EQ(chan_try_recv_zc(ch, &m1), ccol_container_empty);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_channels_timed_send_and_timed_receive(void *args) {
  channel *ch = (channel *)args;

  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10000000;  // 10 msecs

  usleep(40000);  // 40 msecs

  struct timespec before;
  struct timespec after;

  c_message_t msg = {.data = NULL, .size = 0};
  getWallTime(before);
  assert(chan_timed_recv_zc(ch, &msg, &timeout) == ccol_success);
  getWallTime(after);
  assert(diffTimeUSec(before, after) < 4000);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');

  *((char *)msg.data) = 'B';

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  assert(chan_timed_recv_zc(ch, &m2, &timeout) == ccol_timed_out);
  getWallTime(after);
  assert(diffTimeUSec(before, after) >= 10000);
  assert(m2.data == NULL);

  getWallTime(before);
  assert(chan_timed_send_zc(ch, &msg, &timeout) == ccol_success);
  getWallTime(after);
  assert(diffTimeUSec(before, after) < 4000);
  assert(msg.data == NULL);

  m2 = (c_message_t){.data = malloc(sizeof(char)), .size = 1};
  getWallTime(before);
  assert(chan_timed_send_zc(ch, &m2, &timeout) == ccol_timed_out);
  getWallTime(after);
  assert(diffTimeUSec(before, after) >= 10000);
  assert(m2.data != NULL);
  free(m2.data);

  return NULL;
}

TEST(channels, timed_send_and_timed_receive) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL,
                            thr_for_channels_timed_send_and_timed_receive, ch),
             0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  *(char *)m1.data = 'A';

  struct timespec before;
  struct timespec after;

  struct timespec timeout;
  timeout.tv_sec = 0;
  timeout.tv_nsec = 10000000;  // 10 msecs

  getWallTime(before);
  REQUIRE_EQ(chan_timed_send_zc(ch, &m1, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 4000);
  REQUIRE_EQ(m1.data, NULL);

  m1.data = malloc(sizeof(char));
  m1.size = 1;
  getWallTime(before);
  REQUIRE_EQ(chan_timed_send_zc(ch, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 10000);
  REQUIRE_NE(m1.data, NULL);
  free(m1.data);
  m1.data = NULL;

  usleep(90000);  // 90 msecs

  c_message_t m2 = {.data = NULL, .size = 0};
  getWallTime(before);
  REQUIRE_EQ(chan_timed_recv_zc(ch, &m2, &timeout), ccol_success);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 4000);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  getWallTime(before);
  REQUIRE_EQ(chan_timed_recv_zc(ch, &m1, &timeout), ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 10000);
  REQUIRE_EQ(m1.data, NULL);

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

void *thr_for_enable_disable_sending(void *args) {
  channel *ch = (channel *)args;

  c_message_t msg = {.data = NULL, .size = 0};
  assert(chan_recv_zc(ch, &msg) == ccol_success);
  assert(msg.data != NULL);
  assert(*((char *)msg.data) == 'A');
  *((char *)msg.data) = 'B';

  chan_disable_sending(ch, workers_to_owner);
  assert(chan_send_zc(ch, &msg) == ccol_not_permitted);
  assert(msg.data != NULL);

  chan_enable_sending(ch, workers_to_owner);
  assert(chan_send_zc(ch, &msg) == ccol_success);
  assert(msg.data == NULL);

  return NULL;
}

TEST(channels, enable_disable_sending) {
  channel *ch = channel_create_with_mprocs(1, NULL, NULL);

  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_for_enable_disable_sending, ch), 0);

  c_message_t m1 = {.data = malloc(sizeof(char)), .size = 1};
  m1.size = 1;
  ((char *)(m1.data))[0] = 'A';

  chan_disable_sending(ch, owner_to_workers);

  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_not_permitted);
  REQUIRE_NE(m1.data, NULL);

  chan_enable_sending(ch, owner_to_workers);

  REQUIRE_EQ(chan_send_zc(ch, &m1), ccol_success);
  REQUIRE_EQ(m1.data, NULL);

  c_message_t m2 = {.data = NULL, .size = 0};

  REQUIRE_EQ(chan_recv_zc(ch, &m2), ccol_success);
  REQUIRE_NE(m2.data, NULL);
  REQUIRE_EQ(*(char *)m2.data, 'B');

  free(m2.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

/* channel counterpart of
 * circular_queues.destroy_with_live_event_loop_registration_is_fatal above:
 * __channel_destroy destroys both underlying circular queues, so it
 * inherits the identical assert via whichever direction still has a live
 * event_loop registration watching it. */
TEST(channels, destroy_with_live_event_loop_registration_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    channel *ch = channel_create(4, NULL);
    if (!ch) _exit(2);
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);
    event_handlers_t handlers = {0};
    char *err = NULL;
    /* This thread is the channel's owner, so ccol_select_read here resolves
     * to workers_to_owner_cq (see selectable_from_chan's own doc comment). */
    event_reg reg = event_loop_add(
        loop, selectable_from_chan(ch, ccol_select_read), handlers, NULL, &err);
    if (reg == EVENT_REG_INVALID) _exit(2);
    /* The actual misuse under test: the registration above is never removed
     * (and the loop never destroyed) before this call. */
    channel_destroy(ch);
    _exit(0); /* unreachable if the assert fired as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// CCOL_SELECT TESTS

// helpers
typedef struct {
  circular_queue *cq;
  int delay_us;
  int value;
} sel_circq_args;

typedef struct {
  dynamic_queue *dq;
  int delay_us;
  int value;
} sel_dynq_args;

typedef struct {
  circular_queue *cq0;
  circular_queue *cq1;
  int delay_us;
  int value;
} sel_disable_reenable_args;

typedef struct {
  channel *ch;
  int delay_us;
  int value;
} sel_chan_args;

static void *thr_send_to_circq(void *arg) {
  sel_circq_args *a = (sel_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(circq_send_zc(a->cq, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_to_dynq(void *arg) {
  sel_dynq_args *a = (sel_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(dynmq_send_zc(a->dq, &msg) == ccol_success);
  return NULL;
}

/* Disables both queues, waits another delay, then re-enables cq0 and sends
 * one message to it.  Used to verify ccol_select stays blocked through the
 * disable phase and only wakes on the subsequent message. */
static void *thr_disable_then_reenable_and_send(void *arg) {
  sel_disable_reenable_args *a = (sel_disable_reenable_args *)arg;
  usleep((useconds_t)a->delay_us);
  circq_disable_sending(a->cq0);
  circq_disable_sending(a->cq1);
  usleep((useconds_t)a->delay_us);
  circq_enable_sending(a->cq0);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(circq_send_zc(a->cq0, &msg) == ccol_success);
  return NULL;
}

static void *thr_send_via_channel(void *arg) {
  sel_chan_args *a = (sel_chan_args *)arg;
  usleep((useconds_t)a->delay_us);
  int *data = malloc(sizeof(int));
  assert(data);
  *data = a->value;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  assert(chan_send_zc(a->ch, &msg) == ccol_success);
  return NULL;
}

// tests
TEST(ccol_select, returns_invalid_args_on_bad_inputs) {
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 0;
  ccol_selectable sel = selectable_from_circq(cq, ccol_select_read);

  REQUIRE_EQ(ccol_select(NULL, 1, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 0, &sel), ccol_invalid_args);
  REQUIRE_EQ(ccol_select(&idx, 1, NULL), ccol_invalid_args);

  ccol_selectable null_sel = selectable_from_circq(NULL, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &null_sel), ccol_invalid_args);

  circular_queue_destroy(cq);
}

TEST(ccol_select,
     timed_n_too_large_rejected_before_any_allocation_or_oob_read) {
  /* Regression test for the size_t-overflow guard in _sel_validate_args:
   * ccol_select_timed() backs its own per-selectable waiter-node array with
   * a single, plain malloc(n * sizeof(internal waiter node)) call with no
   * calloc-style overflow checking of its own. n == SIZE_MAX is guaranteed
   * to exceed SIZE_MAX / sizeof(that struct) for any struct larger than one
   * byte (it has several pointer-sized fields), so this must be rejected
   * with ccol_invalid_args, and, just as importantly, rejected BEFORE the
   * overflowing multiplication is ever formed and before selectables[i] is
   * ever indexed for any i > 0: a genuinely 1-element selectables array is
   * passed deliberately (n claims far more elements than the array
   * actually holds), so a regression that checked n only after starting to
   * scan the array would read out of bounds here rather than merely
   * mis-sizing an allocation. */
  circular_queue *cq = circular_queue_create(4, NULL);
  ccol_selectable sel = selectable_from_circq(cq, ccol_select_read);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed(&idx, SIZE_MAX, &sel, 0), ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  circular_queue_destroy(cq);
}

TEST(ccol_select,
     timed_n_exceeding_int_max_rejected_before_any_allocation_or_oob_read) {
  /* Regression test for the n > INT_MAX guard in _sel_validate_args:
   * _sel_phase1_scan_register narrows a matched selectable's own index (a
   * size_t, 0..n-1) into a plain `int` (its `found` local, doubling as the
   * -1/-2 "nothing found yet"/"system error" sentinels), which
   * ccol_select_timed then widens back via `(size_t)found`. An n large
   * enough to let a match land beyond INT_MAX would silently narrow to a
   * negative or wrapped value there, corrupting *ready_index. This n value
   * is far below the pre-existing SIZE_MAX / sizeof(internal waiter node)
   * guard tested above (INT_MAX is ~2^31, that guard's own ceiling is
   * several orders of magnitude higher on any 64-bit platform), so this
   * exercises the INT_MAX-specific guard in isolation, not the earlier one.
   * Just as importantly, rejected BEFORE selectables[i] is ever indexed for
   * any i > 0: a genuinely 1-element selectables array is passed
   * deliberately (n claims far more elements than the array actually
   * holds), so a regression that checked n only after starting to scan the
   * array would read out of bounds here rather than merely mis-sizing an
   * allocation. */
  circular_queue *cq = circular_queue_create(4, NULL);
  ccol_selectable sel = selectable_from_circq(cq, ccol_select_read);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed(&idx, (size_t)INT_MAX + 1, &sel, 0),
             ccol_invalid_args);
  REQUIRE_EQ(idx, (size_t)99);

  circular_queue_destroy(cq);
}

TEST(ccol_select, receives_from_first_queue_when_message_already_present) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 7;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q0, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 7);

  free(recv_msg.data);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, receives_from_second_queue_when_message_already_present) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  int *data = malloc(sizeof(int));
  *data = 42;
  c_message_t send_msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q1, &send_msg), ccol_success);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);

  free(recv_msg.data);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_circq) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = q1, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q1, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, timed_returns_timed_out_when_all_queues_disabled) {
  // Disabling sending must not affect read-direction waiters: ccol_select_timed
  // must wait out the full timeout rather than returning ccol_not_permitted.
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  circq_disable_sending(q0);
  circq_disable_sending(q1);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(q0, ccol_select_read),
                                  selectable_from_circq(q1, ccol_select_read)),
             ccol_timed_out);

  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select,
     stays_blocked_through_disable_then_wakes_after_reenable_and_send) {
  // ccol_select must remain blocked when sending is disabled on all queues.
  // Once sending is re-enabled and a message arrives, it must return success.
  circular_queue *q0 = circular_queue_create(4, NULL);
  circular_queue *q1 = circular_queue_create(4, NULL);

  sel_disable_reenable_args args = {
      .cq0 = q0, .cq1 = q1, .delay_us = 15000, .value = 42};
  pthread_t tid;
  REQUIRE_EQ(
      pthread_create(&tid, NULL, thr_disable_then_reenable_and_send, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_circq(q1, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q0, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 42);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  circular_queue_destroy(q1);
}

TEST(ccol_select, blocks_until_message_arrives_on_dynq) {
  circular_queue *q0 = circular_queue_create(4, NULL);
  dynamic_queue *dq = dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 55};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(q0, ccol_select_read),
                            selectable_from_dynq(dq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 55);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  circular_queue_destroy(q0);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, channel_direction_resolved_correctly_for_owner_thread) {
  channel *ch = channel_create_with_mprocs(4, NULL, NULL);

  /* Worker thread sends via chan_send_zc, which routes to workers_to_owner_cq.
   * selectable_from_chan() called from the owner thread here also
   * resolves to workers_to_owner_cq, so ccol_select watches the correct queue.
   */
  sel_chan_args args = {.ch = ch, .delay_us = 15000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_via_channel, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_chan(ch, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(chan_try_recv_zc(ch, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  channel_destroy(ch);
}

// write-wait helpers
typedef struct {
  circular_queue *cq;
  int delay_us;
} sel_recv_circq_args;

static void *thr_recv_from_circq(void *arg) {
  sel_recv_circq_args *a = (sel_recv_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  c_message_t msg = {.data = NULL, .size = 0};
  assert(circq_recv_zc(a->cq, &msg) == ccol_success);
  free(msg.data);
  return NULL;
}

typedef struct {
  circular_queue *cq;
  int delay_us;
} sel_enable_circq_args;

static void *thr_enable_circq_sending(void *arg) {
  sel_enable_circq_args *a = (sel_enable_circq_args *)arg;
  usleep((useconds_t)a->delay_us);
  circq_enable_sending(a->cq);
  return NULL;
}

typedef struct {
  dynamic_queue *dq;
  int delay_us;
} sel_enable_dynq_args;

static void *thr_enable_dynq_sending(void *arg) {
  sel_enable_dynq_args *a = (sel_enable_dynq_args *)arg;
  usleep((useconds_t)a->delay_us);
  dynmq_enable_sending(a->dq);
  return NULL;
}

// write-wait tests
TEST(ccol_select, write_circq_writable_immediately) {
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_blocks_until_reader_frees_space) {
  circular_queue *cq = circular_queue_create(2, NULL);

  /* Fill the queue to capacity */
  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 10;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 20;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);

  sel_recv_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_recv_from_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);

  /* Drain the remaining message to satisfy destroy's assert */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_wakes_when_sending_reenabled) {
  circular_queue *cq = circular_queue_create(4, NULL);
  circq_disable_sending(cq);

  sel_enable_circq_args args = {.cq = cq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_enable_circq_sending, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_circq(cq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  circular_queue_destroy(cq);
}

TEST(ccol_select, write_dynq_writable_immediately) {
  dynamic_queue *dq = dynamic_queue_create(NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_dynq(dq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_dynq_wakes_when_sending_reenabled) {
  dynamic_queue *dq = dynamic_queue_create(NULL);
  dynmq_disable_sending(dq);

  sel_enable_dynq_args args = {.dq = dq, .delay_us = 15000};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_enable_dynq_sending, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_dynq(dq, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  pthread_join(tid, NULL);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, write_mixed_full_circq_and_readable_circq) {
  /* q_write is full; q_read is empty and will receive a message.
   * ccol_select should pick up q_read (read-direction) first. */
  circular_queue *q_write = circular_queue_create(1, NULL);
  circular_queue *q_read = circular_queue_create(4, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 42;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(q_write, &fill_msg), ccol_success);

  sel_circq_args args = {.cq = q_read, .delay_us = 15000, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, selectable_from_circq(q_write, ccol_select_write),
                     selectable_from_circq(q_read, ccol_select_read)),
      ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(q_read, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 88);

  free(recv_msg.data);
  pthread_join(tid, NULL);

  /* Drain q_write's fill message */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(q_write, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(q_write);
  circular_queue_destroy(q_read);
}

TEST(ccol_select, write_channel_owner_resolves_send_direction) {
  channel *ch = channel_create_with_mprocs(4, NULL, NULL);

  /* Owner calls selectable_from_chan with ccol_select_write: resolves
   * to owner_to_workers_cq.  That queue is empty and writable, so ccol_select
   * must return immediately. */
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_chan(ch, ccol_select_write)),
             ccol_success);
  REQUIRE_EQ(idx, 0);

  channel_destroy(ch);
}

// fd selectable helpers
typedef struct {
  int write_fd;
  int delay_us;
  int value;
} sel_fd_write_args;

static void *thr_write_to_fd(void *arg) {
  sel_fd_write_args *a = (sel_fd_write_args *)arg;
  usleep((useconds_t)a->delay_us);
  int v = a->value;
  assert(write(a->write_fd, &v, sizeof(v)) == sizeof(v));
  return NULL;
}

// fd selectable tests
TEST(ccol_select, fd_readable_immediately) {
  /* Write data to the pipe before calling ccol_select; it must return without
   * blocking and report the correct index. The caller then reads the fd
   * itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 42;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 42);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_blocks_until_data_arrives) {
  /* Thread writes to pipe after a delay; ccol_select must block and then wake
   * up when data arrives. The caller then reads the fd itself. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  sel_fd_write_args args = {.write_fd = pfd[1], .delay_us = 15000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_write_to_fd, &args), 0);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_success);
  getWallTime(after);
  REQUIRE_EQ(idx, 0);
  REQUIRE_GE(diffTimeUSec(before, after), 10000); /* actually blocked */

  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 77);

  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_writable_immediately) {
  /* A fresh pipe write-end is always writable; must return without blocking. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 99;
  REQUIRE_EQ(
      ccol_select_va(&idx, selectable_from_fd(pfd[1], ccol_select_write)),
      ccol_success);
  REQUIRE_EQ(idx, 0);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_and_queue_fd_wins) {
  /* Pipe already has data; circular queue is empty.  fd must win at index 0. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  int val = 55;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 0);
  int got = 0;
  REQUIRE_EQ((ssize_t)sizeof(got), read(pfd[0], &got, sizeof(got)));
  REQUIRE_EQ(got, 55);

  close(pfd[0]);
  close(pfd[1]);
  circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_queue_queue_wins) {
  /* Pipe has no data; a thread sends to the queue after a delay.  The queue
   * must win, demonstrating that the eventfd bridge correctly wakes epoll_wait
   * for a queue event in a mixed fd+queue selectable array. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  circular_queue *cq = circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 15000, .value = 33};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 33);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  circular_queue_destroy(cq);
}

TEST(ccol_select, fd_and_dynq_dynq_wins) {
  /* Same as above but with a dynamic_queue, ensuring the eventfd bridge works
   * for dynq selectables in epoll mode. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  dynamic_queue *dq = dynamic_queue_create(NULL);

  sel_dynq_args args = {.dq = dq, .delay_us = 15000, .value = 99};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_dynq, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_va(&idx, selectable_from_fd(pfd[0], ccol_select_read),
                            selectable_from_dynq(dq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, 1);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(dynmq_try_recv_zc(dq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 99);

  free(recv_msg.data);
  pthread_join(tid, NULL);
  close(pfd[0]);
  close(pfd[1]);
  dynamic_queue_destroy(dq);
}

TEST(ccol_select, fd_invalid_fd_returns_invalid_args) {
  size_t idx = 99;
  ccol_selectable bad = selectable_from_fd(-1, ccol_select_read);
  REQUIRE_EQ(ccol_select(&idx, 1, &bad), ccol_invalid_args);
}

TEST(ccol_select, timed_circq_returns_timed_out) {
  /* Queue is empty with writing enabled; ccol_select_timed must return
   * ccol_timed_out after the deadline, not block indefinitely. */
  circular_queue *cq = circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);
  /* idx must be untouched on timeout */
  REQUIRE_EQ(idx, (size_t)99);
  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_fd_returns_timed_out) {
  /* Read end of a pipe with no data written; must time out. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_fd(pfd[0], ccol_select_read)),
             ccol_timed_out);
  REQUIRE_EQ(idx, (size_t)99);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, timed_poll_zero_ms_circq_empty) {
  /* timeout_ms == 0: non-blocking poll; empty queue -> immediate timed_out. */
  circular_queue *cq = circular_queue_create(4, NULL);
  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 0,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);
  circular_queue_destroy(cq);
}

typedef struct helper_thread_args {
  circular_queue *cq;
  c_message_t msg;
} helper_thread_args;

void *helper_thread_main(void *arg) {
  helper_thread_args *a = arg;
  struct timespec ts = {.tv_sec = 0, .tv_nsec = 20 * 1000000L};
  nanosleep(&ts, NULL);
  circq_send_zc(a->cq, &a->msg);
  return NULL;
}

TEST(ccol_select, timed_succeeds_before_deadline) {
  /* Producer sends before the 500 ms deadline; select must return success. */
  circular_queue *cq = circular_queue_create(4, NULL);

  pthread_t tid;
  c_message_t send_msg = {.data = malloc(4), .size = 4};
  *(int *)send_msg.data = 1234;

  /* A helper thread that sleeps 20 ms then sends. */
  helper_thread_args args = {cq, send_msg};

  REQUIRE_EQ(pthread_create(&tid, NULL, helper_thread_main, &args), 0);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 500,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, (size_t)0);
  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 1234);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_out_deregisters_waiter_node) {
  /* Regression test for the use-after-free bug: after ccol_select_timed times
   * out, the waiter node must be removed from the queue's waiter list before
   * the node is freed.  If deregistration is missing, a subsequent send will
   * dereference freed memory, which Valgrind or ASan would catch.  Running
   * cleanly here confirms that deregistration happens on the timeout path. */
  circular_queue *cq = circular_queue_create(4, NULL);

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 30 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_timed_out);

  /* Send to the queue AFTER the timed-out select has returned.  If the waiter
   * node was not deregistered, circq_send_zc -> notify_one_sel_waiter will
   * dereference the freed node here. */
  c_message_t msg = {.data = malloc(4), .size = 4};
  *(int *)msg.data = 42;
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  /* Drain so the queue is empty before destroy. */
  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_wait_reports_unexpected_failure_on_condvar_error) {
  /* Regression test: in ccol_select_timed's no-fd-selectables wait path,
   * _sel_wait_condvar's deadline branch must not spin forever retrying the
   * same call when cond_var_timedwait itself returns an error other than 0
   * or ETIMEDOUT; it must report ccol_unexpected_failure instead, the same
   * way circq_timed_send_zc/circq_timed_recv_zc/dynmq_timed_recv_zc already
   * do for the identical class of failure. Uses a test-only hook to force
   * exactly one such error deterministically, since no legitimate deadline
   * this library computes internally can ever trigger a genuine EINVAL from
   * cond_var_timedwait. A large configured timeout (5s) with a tight
   * elapsed-time bound proves the call returns promptly on the forced
   * error rather than either waiting out the full deadline or hanging
   * indefinitely (the pre-fix behavior, which this test would otherwise
   * never return from at all). */
  circular_queue *cq = circular_queue_create(4, NULL);

  ccol_select_test_force_next_condvar_wait_error();

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_timed_va(&idx, 5000 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_unexpected_failure);
  getWallTime(after);
  REQUIRE_LT(diffTimeUSec(before, after), 500000);
  REQUIRE_EQ(idx, (size_t)99);

  circular_queue_destroy(cq);
}

TEST(ccol_select, timed_wait_ready_racing_condvar_error_still_succeeds) {
  /* Regression test for a real bug in _sel_wait_condvar's deadline branch:
   * its FAILURE outcome was reported unconditionally on any non-zero,
   * non-ETIMEDOUT return from cond_var_timedwait, unlike the sibling
   * ETIMEDOUT branch right above it, which already re-checks *ready before
   * declaring a real timeout. Since cond_var_timedwait always re-acquires
   * its mutex before returning (success or failure), a producer's own
   * notify can legitimately complete and set *ready = true an instant
   * before an unrelated, spurious wait error is also reported; without the
   * FAILURE branch's own matching re-check, that already-delivered wakeup
   * was silently discarded and ccol_select_timed reported
   * ccol_unexpected_failure instead of resuming and eventually succeeding.
   *
   * Uses the dedicated test hook to simulate exactly that interleaving
   * (forcing both the error and *ready = true in the same instant), since a
   * real producer thread cannot actually race into this exact window on its
   * own (the hook replaces cond_var_timedwait outright rather than
   * releasing sel_mtx, so nothing else could acquire it meanwhile). A
   * second, genuine background send (racing the call's own re-scan after
   * the hook fires) is what lets the call actually complete successfully
   * either way: if the fixed code correctly resumes instead of bailing out,
   * it either sees this message immediately or falls through to an
   * ordinary, already-well-tested real wait for it. Before the fix, this
   * test fails deterministically with ccol_unexpected_failure, never
   * ccol_success. */
  circular_queue *cq = circular_queue_create(4, NULL);

  sel_circq_args args = {.cq = cq, .delay_us = 50000, .value = 77};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, thr_send_to_circq, &args), 0);

  ccol_select_test_force_next_condvar_wait_error_racing_ready();

  size_t idx = 99;
  REQUIRE_EQ(ccol_select_timed_va(&idx, 2000 /* ms */,
                                  selectable_from_circq(cq, ccol_select_read)),
             ccol_success);
  REQUIRE_EQ(idx, (size_t)0);

  c_message_t recv_msg = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_try_recv_zc(cq, &recv_msg), ccol_success);
  REQUIRE_EQ(*(int *)recv_msg.data, 77);
  free(recv_msg.data);

  pthread_join(tid, NULL);
  circular_queue_destroy(cq);
}

TEST(ccol_select, write_circq_timed_out_when_sending_disabled) {
  /* A disabled queue must cause a write-direction ccol_select_timed to wait
   * out the full timeout rather than return ccol_not_permitted immediately.
   * Disabling only blocks new sends; it must not short-circuit the select. */
  circular_queue *cq = circular_queue_create(4, NULL);
  circq_disable_sending(cq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_circq(cq, ccol_select_write)),
             ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  circular_queue_destroy(cq);
}

TEST(ccol_select, write_dynq_timed_out_when_sending_disabled) {
  /* Same contract as above for dynamic_queue. */
  dynamic_queue *dq = dynamic_queue_create(NULL);
  dynmq_disable_sending(dq);

  size_t idx = 99;
  struct timespec before, after;
  getWallTime(before);
  REQUIRE_EQ(ccol_select_timed_va(&idx, 50 /* ms */,
                                  selectable_from_dynq(dq, ccol_select_write)),
             ccol_timed_out);
  getWallTime(after);
  REQUIRE_GE(diffTimeUSec(before, after), 50000);
  REQUIRE_EQ(idx, (size_t)99);

  dynamic_queue_destroy(dq);
}

// concurrent write-waiters helpers
typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} sel_write_wait_result;

static void *thr_circq_write_wait(void *arg) {
  sel_write_wait_result *a = (sel_write_wait_result *)arg;
  size_t idx = 0;
  a->result = ccol_select_timed_va(
      &idx, 500 /* ms */, selectable_from_circq(a->cq, ccol_select_write));
  return NULL;
}

TEST(ccol_select, write_circq_two_concurrent_waiters_both_wake_on_slot_free) {
  /* Regression test for the cascade bug: when two threads are simultaneously
   * waiting for write-readiness on a full capacity-1 queue, a single dequeue
   * must cascade through the waiter list and wake BOTH threads, not just the
   * head waiter. Without the fix the second waiter would time out. */
  circular_queue *cq = circular_queue_create(1, NULL);

  int *fill = malloc(sizeof(int));
  assert(fill);
  *fill = 0;
  c_message_t fill_msg = {.data = fill, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &fill_msg), ccol_success);

  sel_write_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_write_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  REQUIRE_EQ(pthread_create(&t1, NULL, thr_circq_write_wait, &a1), 0);
  REQUIRE_EQ(pthread_create(&t2, NULL, thr_circq_write_wait, &a2), 0);

  usleep(30000); /* let both threads register as write-waiters */

  c_message_t drain = {.data = NULL, .size = 0};
  REQUIRE_EQ(circq_recv_zc(cq, &drain), ccol_success);
  free(drain.data);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  circular_queue_destroy(cq);
}

// concurrent read-waiters helper
typedef struct {
  circular_queue *cq;
  ccol_retval_t result;
} sel_read_wait_result;

static void *thr_circq_read_wait(void *arg) {
  sel_read_wait_result *a = (sel_read_wait_result *)arg;
  size_t idx = 0;
  a->result = ccol_select_timed_va(
      &idx, 500 /* ms */, selectable_from_circq(a->cq, ccol_select_read));
  if (a->result == ccol_success) {
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = circq_try_recv_zc(a->cq, &msg);
    a->result = rv;
    if (rv == ccol_success) free(msg.data);
  }
  return NULL;
}

TEST(ccol_select,
     read_circq_two_concurrent_waiters_both_wake_on_message_available) {
  /* Regression test for the read-direction cascade: once ccol_select() stops
   * consuming internally (peek-only, mirroring the write-direction branch's
   * existing peek+forward-notify shape), a thread that finds itself ready
   * must still forward the notify to the next waiter; otherwise, with two
   * threads simultaneously waiting to read from the same empty queue, only
   * the first ever wakes even though two messages are actually available. */
  circular_queue *cq = circular_queue_create(4, NULL);

  sel_read_wait_result a1 = {.cq = cq, .result = ccol_unexpected_failure};
  sel_read_wait_result a2 = {.cq = cq, .result = ccol_unexpected_failure};
  pthread_t t1, t2;
  REQUIRE_EQ(pthread_create(&t1, NULL, thr_circq_read_wait, &a1), 0);
  REQUIRE_EQ(pthread_create(&t2, NULL, thr_circq_read_wait, &a2), 0);

  usleep(30000); /* let both threads register as read waiters */

  int *d0 = malloc(sizeof(int));
  assert(d0);
  *d0 = 1;
  int *d1 = malloc(sizeof(int));
  assert(d1);
  *d1 = 2;
  c_message_t m0 = {.data = d0, .size = sizeof(int)};
  c_message_t m1 = {.data = d1, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &m0), ccol_success);
  REQUIRE_EQ(circq_send_zc(cq, &m1), ccol_success);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  REQUIRE_EQ(a1.result, ccol_success);
  REQUIRE_EQ(a2.result, ccol_success);

  circular_queue_destroy(cq);
}

/* Regression tests for a real bug: _sel_setup_epoll used to call
 * epoll_ctl(EPOLL_CTL_ADD) once per fd selectable with no de-duplication,
 * so two selectables sharing one real fd (any mix of directions, including
 * an outright duplicate) always failed the second EPOLL_CTL_ADD with
 * EEXIST, making the whole ccol_select/ccol_select_timed call fail with
 * ccol_unexpected_failure, even for the ordinary, common pattern of
 * watching one connected socket for both readability and writability at
 * once. Fixed by grouping fd selectables by fd (via a sort, not a linear
 * scan, to stay O(n log n) rather than degrading to O(n^2) for the much
 * more common case of many distinct fds) and registering one combined
 * epoll_ctl call per unique fd, then resolving a fired combined event back
 * to whichever member selectable's own direction it actually satisfies. */
TEST(ccol_select, fd_same_fd_two_directions_writable_wins) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      selectable_from_fd(sv[0], ccol_select_read),
      selectable_from_fd(sv[0], ccol_select_write),
  };
  /* A freshly connected socket is immediately writable and has nothing to
   * read yet, so this must resolve to the write-direction selectable
   * specifically, not merely succeed. */
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)1);

  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_select, fd_same_fd_two_directions_both_ready_resolves_to_a_member) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  char byte = 'x';
  REQUIRE_EQ(write(sv[1], &byte, 1), (ssize_t)1);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      selectable_from_fd(sv[0], ccol_select_read),
      selectable_from_fd(sv[0], ccol_select_write),
  };
  /* Both directions are genuinely ready now (one byte queued to read, send
   * buffer still has room); either member may legitimately win, but the
   * call itself must succeed and pick one of the two real members, not the
   * EEXIST failure this combining fix exists to close. */
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  char buf[8];
  REQUIRE_EQ(read(sv[0], buf, sizeof(buf)), (ssize_t)1);
  close(sv[0]);
  close(sv[1]);
}

TEST(ccol_select, fd_duplicate_selectable_same_direction_still_succeeds) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  ccol_selectable sels[2] = {
      selectable_from_fd(pfd[1], ccol_select_write),
      selectable_from_fd(pfd[1], ccol_select_write),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 500);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(idx == 0 || idx == 1);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_duplicate_selectable_neither_ready_times_out) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  size_t idx = 999;
  /* Nothing was ever written; watching the read end for read-readiness
   * twice must still behave like an ordinary single registration and time
   * out, not spuriously succeed or fail with an unrelated error. */
  ccol_selectable sels[2] = {
      selectable_from_fd(pfd[0], ccol_select_read),
      selectable_from_fd(pfd[0], ccol_select_read),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 2, sels, 100);
  REQUIRE_EQ(rv, ccol_timed_out);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(ccol_select, fd_grouped_registration_coexists_with_ready_queue) {
  /* A not-ready fd registered twice (sharing one epoll_ctl group under the
   * fix) alongside a genuinely ready queue selectable in the same call:
   * confirms the fd-grouping change didn't disturb ccol_select's existing,
   * unrelated queue-selectable handling. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  circular_queue *cq = circular_queue_create(4, NULL);
  REQUIRE_NE((void *)cq, NULL);
  int *data = malloc(sizeof(int));
  REQUIRE_NE((void *)data, NULL);
  *data = 42;
  c_message_t msg = {.data = data, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  size_t idx = 999;
  ccol_selectable sels[3] = {
      selectable_from_fd(pfd[0], ccol_select_read),
      selectable_from_fd(pfd[0], ccol_select_read),
      selectable_from_circq(cq, ccol_select_read),
  };
  ccol_retval_t rv = ccol_select_timed(&idx, 3, sels, 500);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idx, (size_t)2);

  c_message_t out;
  REQUIRE_EQ(circq_try_recv_zc(cq, &out), ccol_success);
  free(out.data);
  circular_queue_destroy(cq);
  close(pfd[0]);
  close(pfd[1]);
}

// event_loop test helpers
typedef struct evl_sync_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  int readable_count;
  int writable_count;
  int error_count;
  c_message_t last_msg;
  bool last_msg_valid;
  ccol_select_dir last_dir_seen;
  event_reg self_reg; /* for self-removal tests */
  event_loop self_loop;
} evl_sync_ctx;

static void evl_sync_ctx_init(evl_sync_ctx *c) {
  /* Checked: an unchecked failure here would leave c->mtx/c->cond
   * uninitialized, undefined behavior for every later pthread_mutex_lock/
   * pthread_cond_wait call this file makes on them, up to and including a
   * silent, indefinite hang if the uninitialized bytes happen to look
   * already-locked. This helper runs only from ordinary (non-forked-child)
   * test bodies in this file, so asserting is safe here, unlike a
   * pthread_create inside a forked child elsewhere in this file. */
  assert(pthread_mutex_init(&c->mtx, NULL) == 0);
  assert(pthread_cond_init(&c->cond, NULL) == 0);
  c->readable_count = 0;
  c->writable_count = 0;
  c->error_count = 0;
  c->last_msg = (c_message_t){.data = NULL, .size = 0};
  c->last_msg_valid = false;
  c->self_reg = EVENT_REG_INVALID;
  c->self_loop = EVENT_LOOP_INVALID;
}

static void evl_sync_ctx_destroy(evl_sync_ctx *c) {
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  pthread_mutex_destroy(&c->mtx);
  pthread_cond_destroy(&c->cond);
}

/* event_loop never performs the receive itself, for any selectable type
 * (mirrors _event_loop_run_callback's own contract): a queue-backed
 * registration must perform its own explicit circq_try_recv_zc/
 * dynmq_try_recv_zc here, exactly as a real caller would. A non-success
 * result (a concurrent consumer already claimed the message, the same
 * TOCTOU write-direction wins have always had) leaves last_msg_valid
 * false rather than asserting. */
static void evl_on_readable(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  bool got_msg = false;
  if (sel->type == ccol_selectable_circq) {
    got_msg = (circq_try_recv_zc(sel->cq, &msg) == ccol_success);
  } else if (sel->type == ccol_selectable_dynq) {
    got_msg = (dynmq_try_recv_zc(sel->dq, &msg) == ccol_success);
  }
  pthread_mutex_lock(&c->mtx);
  c->readable_count++;
  c->last_dir_seen = sel->dir;
  if (c->last_msg_valid && c->last_msg.data) free(c->last_msg.data);
  if (got_msg) {
    c->last_msg = msg;
    c->last_msg_valid = true;
  } else {
    c->last_msg = (c_message_t){.data = NULL, .size = 0};
    c->last_msg_valid = false;
  }
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_writable(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->writable_count++;
  c->last_dir_seen = sel->dir;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

static void evl_on_error(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)sel;
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->error_count++;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

/* Self-removing on_readable: removes its own registration from inside the
 * callback, then behaves exactly like evl_on_readable. */
static void evl_on_readable_self_remove(event_loop loop, ccol_selectable *sel,
                                        void *arg) {
  evl_sync_ctx *c = (evl_sync_ctx *)arg;
  /* assert(), not REQUIRE_EQ: this callback runs on event_loop's own
   * reactor/dispatch thread, concurrently with the main test thread's own
   * REQUIRE_EQ calls. Tau's assertion machinery is backed by plain,
   * unlocked, non-thread-local globals (see tau.h), so calling it from
   * here would be a genuine data race the instant this assertion ever
   * actually fails (exactly the scenario it exists to catch), and would
   * not even abort the enclosing TEST() the way a top-level REQUIRE_EQ
   * does (only this function's own stack frame would return early).
   * Every other background-thread helper in this file already follows
   * this same assert()-only convention for exactly this reason. */
  assert(event_loop_remove(c->self_loop, c->self_reg) == ccol_success);
  evl_on_readable(loop, sel, arg);
}

/* Waits until *counter_field >= target or timeout_ms elapses. Returns true
 * if the target was reached before the deadline. */
static bool evl_wait_for(evl_sync_ctx *c, int *counter_field, int target,
                         int timeout_ms) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += timeout_ms / 1000;
  deadline.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
  if (deadline.tv_nsec >= 1000000000L) {
    deadline.tv_sec++;
    deadline.tv_nsec -= 1000000000L;
  }
  pthread_mutex_lock(&c->mtx);
  while (*counter_field < target) {
    int r = pthread_cond_timedwait(&c->cond, &c->mtx, &deadline);
    if (r == ETIMEDOUT) break;
  }
  bool ok = (*counter_field >= target);
  pthread_mutex_unlock(&c->mtx);
  return ok;
}

/* Several of the multi-thread tests below have their on_readable callback
 * perform its own read() directly (rather than deferring consumption to the
 * single-threaded test driver, as evl_on_readable's own fd-selectable path
 * already documents doing for exactly this reason), specifically to mimic
 * how a real production callback behaves under genuinely concurrent
 * dispatch. That makes a *blocking* read unsafe: event_loop's own
 * documentation already warns a callback's receive call "may find nothing,
 * and must handle that gracefully"; a plain blocking read() on an fd with
 * nothing left to read, and no writer left to ever produce more (e.g. once
 * every feeder/driver thread in a test has already finished), is not
 * graceful, it hangs the reactor thread that called it forever, which in
 * turn hangs event_loop_shutdown's join on that thread; a real deadlock
 * reproduced (via gdb thread-apply-all-bt on a stuck test process) during
 * this test suite's own development. Setting O_NONBLOCK makes "nothing
 * available" return -1/EAGAIN immediately instead. */
static void evl_set_nonblocking(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

// event_loop tests
TEST(event_loop, create_destroy) {
  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  event_loop_destroy(loop);
  REQUIRE_EQ(loop, EVENT_LOOP_INVALID);
}

TEST(event_loop, create_destroy_scoped) {
  {
    event_loop_construct_scoped(loop, 8, 1, 1);
    REQUIRE_NE(loop, EVENT_LOOP_INVALID);
  }
  /* loop was destroyed at scope exit; nothing to assert beyond "no crash,
   * clean under valgrind" (checked by the memtest target). */
}

TEST(event_loop, fd_on_readable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  bool last_msg_valid;

  {
    /* Nested block: see the multi-thread tests' identical pattern (e.g.
     * fd_modify_flips_direction_and_updates_sel's own comment for the full
     * explanation). evl_on_readable never drains an fd selectable's data
     * (by design; the caller reads sel->fd itself, as this test does
     * below), so once pfd[0] becomes readable it stays level-triggered-
     * ready and the reactor thread keeps re-dispatching indefinitely until
     * the registration is removed; ctx must not be destroyed, nor may this
     * function return, until that's guaranteed to have stopped, which only
     * this block's join guarantees. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

    int val = 42;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
    /* Read under ctx.mtx, not unprotected; see fd_modify_flips_direction_
     * and_updates_sel's identical last_dir_seen fix for why. */
    pthread_mutex_lock(&ctx.mtx);
    last_msg_valid = ctx.last_msg_valid;
    pthread_mutex_unlock(&ctx.mtx);

    char rbuf[16];
    ssize_t n = read(pfd[0], rbuf, sizeof(rbuf));
    REQUIRE_EQ(n, (ssize_t)sizeof(val));
    REQUIRE_EQ(*(int *)rbuf, 42);

    event_loop_remove(loop, reg);

    /* loop shuts down and its one reactor thread is joined here, at block
     * exit; ctx is guaranteed quiescent from this point on. */
  }

  /* fd selectables never get msg populated; caller reads sel->fd itself. */
  REQUIRE_FALSE(last_msg_valid);

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_on_writable_fires) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment.
     * A fresh pipe write end stays writable indefinitely (nothing ever
     * fills its buffer), so the reactor thread keeps re-dispatching until
     * removed. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[1], ccol_select_write),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    /* A fresh pipe write end is always immediately writable. */
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    event_loop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_both_directions_combine_and_recombine) {
  /* A socketpair fd gives a genuinely bidirectional fd, unlike a pipe;
   * needed to register both read and write interest on the SAME fd, which
   * exercises the EPOLL_CTL_ADD-then-MOD combining path (and MOD-back-down
   * on partial removal). */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment;
     * both directions here stay level-triggered-ready indefinitely (sv[0]'s
     * read side is never drained by evl_on_readable; its write side never
     * fills up), so both ctx's must outlive every reactor thread, not just
     * their own event_loop_remove call. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg rreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write), wh,
                       &write_ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)2);

    /* sv[0] is immediately writable (empty send buffer). */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    /* Remove the write direction (MOD-back-down path); read direction must
     * keep working afterward. */
    REQUIRE_EQ(event_loop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

    event_loop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_simultaneous_readable_and_writable) {
  /* Both directions registered on the same fd; write from the peer so sv[0]
   * becomes readable while it is still writable; both callbacks must fire
   * for the SAME epoll_wait batch. An earlier version of the dispatch design
   * stored a bare event_reg* in ev.data.ptr instead of the shared
   * event_entry, which would have silently delivered only one of the two. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg rreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write), wh,
                       &write_ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);

    /* Drain the initial "immediately writable" dispatch before writing data,
     * so the later wait unambiguously observes the combined batch. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    int val = 99;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));

    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));
    /* sv[0] remains writable the whole time (nothing filled its send
     * buffer), so the writable callback should have fired again too. */
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 2, 2000));

    event_loop_remove(loop, rreg);
    event_loop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_on_error_fires_for_both_directions) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment;
     * an EPOLLHUP/ERR condition from a hung-up peer persists (level-
     * triggered) until the fd is removed, exactly like an undrained
     * readable/writable fd, so the reactor thread keeps re-dispatching
     * on_error until then. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t rh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = NULL, .on_error = evl_on_error};
    char *err = NULL;
    event_reg rreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write), wh,
                       &write_ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);

    close(sv[1]); /* peer hangs up */

    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.error_count, 1, 2000));
    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.error_count, 1, 2000));

    event_loop_remove(loop, rreg);
    event_loop_remove(loop, wreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
}

/* Regression test for a real, asymmetric dispatch-priority bug (see
 * src/cthreadcomm.c's own has_writer comment in _event_loop_handle_event
 * for the full mechanism): EPOLLOUT and EPOLLERR/EPOLLHUP are not mutually
 * exclusive any more than EPOLLIN and EPOLLERR/EPOLLHUP are (see
 * fd_on_error_fires_for_both_directions's own setup just above, which this
 * mirrors): a socket whose peer has just hung up is reported both
 * writable (the local send buffer still has room, so write(2) on it would
 * return immediately with an error rather than block) and erroring at
 * once, confirmed directly against this exact socketpair shape via a
 * standalone epoll probe before writing this test. A write-only
 * registration (on_writable set, on_error == NULL; an explicitly
 * documented supported pattern; see event_handlers_t's own doc comment,
 * "a write-only producer that never expects on_error may pass NULL
 * there") must still see on_writable fire for such an event, not be
 * silently, permanently starved of it with nothing to report the
 * co-occurring error either. num_reactor_threads == 1 here exercises
 * _event_loop_handle_event's own copy of the fix; see the _multi_thread
 * sibling test below for _event_loop_poller_collect's identical copy. */
TEST(event_loop,
     fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment;
     * a hung-up peer's EPOLLHUP condition persists (level-triggered) until
     * the fd is removed, so the reactor thread keeps re-dispatching
     * on_writable until then. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg wreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_write), wh, &ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);

    close(sv[1]); /* peer hangs up: sv[0] becomes both EPOLLOUT and EPOLLHUP */

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    event_loop_remove(loop, wreg);
  }

  /* on_error is NULL on this registration; nothing was ever there to
   * consume the co-occurring error condition with, so it must never have
   * been dispatched. */
  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

/* Identical to the test just above, except num_reactor_threads == 3, which
 * routes dispatch through _event_loop_poller_collect/
 * _event_loop_dispatch_job_fn instead of _event_loop_handle_event; that
 * path carries its own, separate copy of the identical has_writer fix
 * (deliberately near-duplicated, not shared, to keep the
 * num_reactor_threads == 1 path byte-for-byte unchanged; see this module's
 * own standing design note on that duplication), which needs its own
 * regression coverage rather than being assumed correct by extension. */
TEST(
    event_loop,
    fd_write_only_registration_still_fires_on_writable_when_peer_hangs_up_multi_thread) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  int error_count_seen;

  {
    event_loop_construct_scoped(loop, 8, 1, 3);

    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg wreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_write), wh, &ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);

    close(sv[1]);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    pthread_mutex_lock(&ctx.mtx);
    error_count_seen = ctx.error_count;
    pthread_mutex_unlock(&ctx.mtx);

    event_loop_remove(loop, wreg);
  }

  REQUIRE_EQ(error_count_seen, 0);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
}

TEST(event_loop, fd_duplicate_direction_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg1 = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg1, EVENT_REG_INVALID);

  err = NULL;
  event_reg reg2 = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_EQ(reg2, EVENT_REG_INVALID);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

  event_loop_remove(loop, reg1);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_modify_rejects_occupied_direction) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg rreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(rreg, EVENT_REG_INVALID);
  event_reg wreg = event_loop_add(
      loop, selectable_from_fd(sv[0], ccol_select_write), handlers, NULL, &err);
  REQUIRE_NE(wreg, EVENT_REG_INVALID);

  /* Flipping rreg to write would collide with wreg. */
  REQUIRE_EQ(event_loop_modify(loop, rreg, ccol_select_write),
             ccol_not_permitted);

  event_loop_remove(loop, rreg);
  event_loop_remove(loop, wreg);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_flips_direction_and_updates_sel) {
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ccol_select_dir last_dir_seen;

  {
    /* Nested block: see the other multi-thread tests' identical pattern.
     * evl_on_readable never drains sv[0] for this fd selectable (nothing in
     * this test reads it), so once sv[0] becomes readable it stays
     * level-triggered-ready and the reactor thread keeps re-dispatching
     * indefinitely until the registration is removed; ctx must not be
     * destroyed, nor may this function return (freeing ctx's stack slot),
     * until that's guaranteed to have actually stopped, which only this
     * block's join guarantees. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers = {.on_readable = evl_on_readable,
                                 .on_writable = evl_on_writable,
                                 .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_read), ccol_success);

    int val = 5;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    /* Read under ctx.mtx directly rather than through evl_wait_for's
     * counter-only contract: evl_on_readable can (and, since it never
     * drains sv[0] here, will) keep re-writing last_dir_seen for as long as
     * the registration stays live and the reactor thread keeps
     * re-dispatching, so a plain unprotected read immediately after
     * evl_wait_for returns races those ongoing writes; a real,
     * pre-existing bug ThreadSanitizer caught here despite
     * num_reactor_threads == 1 (this has nothing to do with multi-threaded
     * dispatch; a single reactor thread re-dispatching a never-drained
     * level-triggered fd is enough on its own). */
    pthread_mutex_lock(&ctx.mtx);
    last_dir_seen = ctx.last_dir_seen;
    pthread_mutex_unlock(&ctx.mtx);

    event_loop_remove(loop, reg);

    /* loop shuts down and its one reactor thread is joined here, at block
     * exit; ctx is guaranteed quiescent from this point on. */
  }

  REQUIRE_EQ((int)last_dir_seen, (int)ccol_select_read);

  evl_sync_ctx_destroy(&ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_after_remove_returns_invalid_args) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_modify_rejects_queue_selectable) {
  /* event_loop_modify is fd-only, mirroring event_loop_pause/_resume's
   * identical restriction (see pause_and_resume_reject_queue_selectable):
   * a queue/channel registration's direction is part of its identity
   * (remove and re-add instead of flipping it in place), so this must be
   * rejected with ccol_invalid_args rather than silently doing nothing or
   * corrupting the registration. The type check runs unconditionally,
   * before event_loop_modify's own "already in the requested direction"
   * idempotence check, so even a same-direction call is rejected the same
   * way, not treated as a no-op success. This exact negative path had no
   * direct test before, unlike its pause/resume siblings. */
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);
  /* Even requesting the direction the registration already has must be
   * rejected the same way, not treated as a same-direction no-op. */
  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_read), ccol_invalid_args);

  event_loop_remove(loop, reg);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_circq_readable_delivers_message) {
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int *payload = malloc(sizeof(int));
  *payload = 123;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(ctx.last_msg.size, sizeof(int));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 123);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_circq_writable_fires_without_consuming) {
  circular_queue *cq = circular_queue_create(1, NULL);

  /* Fill the queue so write-direction isn't immediately satisfiable. */
  int *filler = malloc(sizeof(int));
  *filler = 1;
  c_message_t fmsg = {.data = filler, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &fmsg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment.
     * evl_on_writable never consumes or produces anything on cq (by design,
     * to prove the reactor never does that on the callback's own behalf),
     * so the circq_try_send_zc/circq_recv_zc round trip below, performed
     * by the test itself after the first wait already succeeded, re-opens
     * write-readiness on cq; a second on_writable dispatch can therefore
     * still be genuinely in flight on the reactor thread at the exact
     * moment this function would otherwise go on to remove the
     * registration and tear ctx/cq down (event_loop_remove only blocks a
     * FUTURE dispatch from starting; it does not wait for one already
     * in flight to finish - see _event_loop_run_callback's own comment).
     * Neither ctx nor cq may be destroyed, nor may this function return,
     * until that's guaranteed to have stopped, which only this block's
     * join (event_loop_construct_scoped's own scope-exit destructor)
     * guarantees. Confirmed as a real, reproducible race, not a
     * theoretical one: ThreadSanitizer caught ctx.mtx/ctx.cond being
     * destroyed by the main thread while evl_on_writable was still using
     * them on the reactor thread, in roughly 1 of every 4-9 runs, before
     * this nested-block fix was applied. */
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_write),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    /* Free the slot; on_writable must fire, but must NOT have consumed
     * anything (queue still has room, not a message); the callback itself
     * is responsible for the actual send. */
    c_message_t recvd;
    REQUIRE_EQ(circq_recv_zc(cq, &recvd), ccol_success);
    free(recvd.data);

    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, 1, 2000));

    int *payload = malloc(sizeof(int));
    *payload = 55;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(circq_try_send_zc(cq, &msg), ccol_success);
    REQUIRE_EQ(circq_msg_count(cq), (size_t)1);

    c_message_t out;
    REQUIRE_EQ(circq_recv_zc(cq, &out), ccol_success);
    REQUIRE_EQ(*(int *)out.data, 55);
    free(out.data);

    event_loop_remove(loop, reg);

    /* loop shuts down and its one reactor thread is joined here, at block
     * exit; ctx/cq are guaranteed quiescent from this point on. */
  }

  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_dynq_readable_delivers_message) {
  event_loop_construct_scoped(loop, 8, 1, 1);
  dynamic_queue *dq = dynamic_queue_create(NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_dynq(dq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int *payload = malloc(sizeof(int));
  *payload = 321;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(dynmq_send_zc(dq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 321);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  dynamic_queue_destroy(dq);
}

typedef struct evl_chan_sender_args {
  channel *ch;
  int value;
} evl_chan_sender_args;

static void *evl_chan_sender_thread(void *arg) {
  evl_chan_sender_args *a = (evl_chan_sender_args *)arg;
  int *payload = malloc(sizeof(int));
  *payload = a->value;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  /* Called from a spawned thread, so get_thread_id() differs from the
   * channel's owner (the test thread that called channel_create); this
   * routes to workers_to_owner_cq, exactly what the owner-side
   * ccol_select_read registration below watches. */
  chan_send_zc(a->ch, &msg);
  return NULL;
}

TEST(event_loop, queue_channel_selectable) {
  event_loop_construct_scoped(loop, 8, 1, 1);
  channel *ch = channel_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  /* This thread is the channel's owner; owner reads from workers_to_owner,
   * so a worker (a separate thread) must send for the owner-side read
   * registration to fire. */
  event_reg reg = event_loop_add(
      loop, selectable_from_chan(ch, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  evl_chan_sender_args sargs = {.ch = ch, .value = 88};
  pthread_t tid;
  REQUIRE_EQ(pthread_create(&tid, NULL, evl_chan_sender_thread, &sargs), 0);
  pthread_join(tid, NULL);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_TRUE(ctx.last_msg_valid);
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 88);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  channel_destroy(ch);
}

TEST(event_loop, queue_persistent_across_multiple_cycles) {
  /* The core "persistent, not per-call" property: a single registration
   * keeps firing across many independent send/recv cycles without ever
   * being re-added. */
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  for (int i = 0; i < 5; i++) {
    int *payload = malloc(sizeof(int));
    *payload = i;
    c_message_t msg = {.data = payload, .size = sizeof(int)};
    REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, i + 1, 2000));
    REQUIRE_EQ(*(int *)ctx.last_msg.data, i);
  }

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

TEST(event_loop, queue_already_pending_message_at_registration_time) {
  /* A message sent BEFORE event_loop_add is called must still be delivered
   *; the bridge eventfd has no prior notify to rely on, so event_loop_add
   * must self-trigger when the queue is already in the target state at
   * registration time. */
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  int *payload = malloc(sizeof(int));
  *payload = 999;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(*(int *)ctx.last_msg.data, 999);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

typedef struct evl_two_readers_ctx {
  _Atomic size_t calls;
} evl_two_readers_ctx;

/* Deliberately slower than a continuously-refilling feeder can keep up
 * with (same shape as evl_bounded_hot_queue_on_readable further below), so
 * a real backlog of 2+ pending messages reliably exists behind this
 * registration by the time it dispatches; this is what gives
 * _event_loop_queue_cascade_notify_next something real to forward toward
 * whichever registration is linked behind this one. */
static void evl_two_readers_slow_on_readable(event_loop loop,
                                             ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)circq_try_recv_zc(sel->cq, &msg);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

static void evl_two_readers_fast_on_readable(event_loop loop,
                                             ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_two_readers_ctx *c = (evl_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)circq_try_recv_zc(sel->cq, &msg);
  atomic_fetch_add(&c->calls, 1);
}

typedef struct evl_two_readers_feeder_args {
  circular_queue *cq;
  int iterations;
} evl_two_readers_feeder_args;

static void *evl_two_readers_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    /* Blocking: the queue's own finite capacity is what reliably keeps this
     * feeder from racing arbitrarily far ahead of both registrations, while
     * still building up a real backlog against the slow registration. */
    (void)circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

TEST(event_loop, queue_second_registration_on_same_queue_is_not_starved) {
  /* Regression test for a real, silent starvation bug found via manual code
   * review: notify_one_sel_waiter() always wakes only the CURRENT head of a
   * queue's own waiter list, and a ccol_select() waiter only "cascades"
   * that wake onward correctly because it always unlinks itself before
   * re-checking readiness. An event_loop registration's waiter_node stays
   * linked into the list PERMANENTLY (until event_loop_remove), so before
   * _event_loop_queue_cascade_notify_next existed, a second live
   * registration on the same queue+direction, linked BEHIND another one
   * that never unlinks, could receive ZERO notifications forever,
   * regardless of how much traffic the queue saw.
   *
   * r1 is registered FIRST here specifically so it ends up linked behind
   * r2 (the most-recently-added registration always becomes the new list
   * head; see event_loop_add's own "prepend to head" linking). Without the
   * fix, ctx1.calls is deterministically stuck at 0 forever in this exact
   * scenario, not merely flaky, since nothing but a cascade forward could
   * ever reach a registration sitting behind a permanently-linked head;
   * confirmed by temporarily reverting the fix and re-running this test,
   * which then hangs against the bounded wait below until it times out
   * with ctx1.calls still 0, rather than occasionally passing. */
  circular_queue *cq = circular_queue_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    event_reg r1 =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers1, &ctx1, &err);
    REQUIRE_NE(r1, EVENT_REG_INVALID);

    /* r2 is added second, so it becomes the list's new head; its own
     * deliberately slow callback is what reliably lets a backlog build up
     * for it to then cascade-forward toward r1. */
    event_handlers_t handlers2 = {
        .on_readable = evl_two_readers_slow_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    event_reg r2 =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers2, &ctx2, &err);
    REQUIRE_NE(r2, EVENT_REG_INVALID);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 300};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_two_readers_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    /* Bounded wait (r2, the slow head, is expected to dominate the count;
     * the fix's own contribution being tested is that r1, the tail, gets
     * ANY turn at all rather than none). */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 3;
    for (;;) {
      if (atomic_load(&ctx1.calls) > 0 && atomic_load(&ctx2.calls) > 0) break;
      struct timespec now;
      clock_gettime(CLOCK_REALTIME, &now);
      if (now.tv_sec > deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec)) {
        break;
      }
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    REQUIRE_GT(atomic_load(&ctx1.calls), (size_t)0);
    REQUIRE_GT(atomic_load(&ctx2.calls), (size_t)0);

    event_loop_remove(loop, r1);
    event_loop_remove(loop, r2);

    /* loop shuts down and its one reactor thread is joined here, at block
     * exit; cq is only safe to touch/destroy after this point, since an
     * already-in-flight dispatch (an ordinary on_readable callback, or this
     * fix's own cascade forward) could otherwise still be using it, exactly
     * the same pre-existing hazard already documented and guarded against
     * by remove_from_different_thread_concurrent_with_dispatch above. */
  }

  /* Drain whatever's left (a small leftover is expected and fine, matching
   * the already-documented, unrelated coalescing characteristic; see
   * multi_thread_hot_queue_dispatch_pool_pending_stays_bounded's own
   * comment); circular_queue_destroy asserts on any remaining message. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  circular_queue_destroy(cq);
}

/* Feeds one message at a time, sleeping between sends so the queue always
 * has time to drain fully before the next one arrives; deliberately the
 * OPPOSITE traffic shape from evl_two_readers_feeder_thread above (which
 * floods the queue as fast as possible specifically to build up a backlog).
 * No backlog ever existing is exactly the condition under which the
 * readiness-contingent cascade (_event_loop_queue_cascade_notify_next) has
 * nothing to forward, so this is what actually exercises the round-robin
 * rotor fix in notify_one_sel_waiter rather than the cascade. */
static void *evl_two_readers_no_backlog_feeder_thread(void *arg) {
  evl_two_readers_feeder_args *a = (evl_two_readers_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    (void)circq_send_zc(a->cq, &msg);
    struct timespec ts = {0, 200000}; /* 0.2ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

TEST(event_loop, queue_multiple_registrations_share_traffic_fairly) {
  /* Regression test for a real, silent starvation bug found via manual code
   * review and confirmed with a standalone reproduction against the built
   * library: notify_one_sel_waiter() used to always wake the CURRENT HEAD of
   * a queue's waiter list, and since an event_loop registration's
   * waiter_node stays linked PERMANENTLY (never re-links, unlike a
   * ccol_select() caller's own transient node), whichever registration was
   * added LAST (and therefore became, and permanently stayed, head) was the
   * ONLY one ever directly notified for as long as it remained registered.
   * The pre-existing cascade mechanism only forwards a wake when the queue
   * is STILL ready right after the notified registration's own callback
   * returns; a callback that keeps up with traffic (even a plain,
   * non-looping single circq_try_recv_zc() per call, an explicitly
   * documented, ordinary pattern; exactly what both registrations below
   * do) routinely leaves nothing to forward, so every OTHER live
   * registration received ZERO callbacks, for as long as traffic kept
   * flowing, directly contradicting event_loop_add's own documented "no
   * live listener is ever passed over indefinitely" guarantee. Confirmed
   * via a standalone repro before the fix: two registrations, 2000 messages
   * fed one at a time, produced calls1=0 calls2=2000 every single run, not
   * merely occasionally. Fixed by giving each queue+direction's waiter list
   * its own round-robin rotor (see notify_one_sel_waiter's own doc comment),
   * so every live registration is revisited once per full rotation instead
   * of the same one winning forever. */
  circular_queue *cq = circular_queue_create(64, NULL);

  evl_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    event_loop_construct_scoped(loop, 8, 1, 1);

    /* Both registrations use the same "fast", non-looping, single-recv
     * callback deliberately: this is the exact pairing under which the
     * pre-fix code produced total starvation (a "slow" head, as the
     * sibling test above uses, artificially builds a backlog the old
     * cascade mechanism could still ride to reach r1; two equally-fast
     * callbacks build no such backlog at all). */
    event_handlers_t handlers1 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    event_reg r1 =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers1, &ctx1, &err);
    REQUIRE_NE(r1, EVENT_REG_INVALID);

    event_handlers_t handlers2 = {
        .on_readable = evl_two_readers_fast_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    event_reg r2 =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers2, &ctx2, &err);
    REQUIRE_NE(r2, EVENT_REG_INVALID);

    evl_two_readers_feeder_args feeder_args = {.cq = cq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_two_readers_no_backlog_feeder_thread,
                       &feeder_args),
        0);
    pthread_join(feeder, NULL);

    /* Bounded wait for any final in-flight dispatch to settle. */
    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);

    /* Both registrations must have received a genuinely fair share, not
     * merely "more than zero": the pre-fix bug produced an exact 0/N split,
     * so a loose ">0" bound alone would already be a meaningful regression
     * check, but asserting each side got at least a quarter of a perfectly-
     * even (1/2 each) split gives real margin against scheduling jitter
     * while still failing hard against the old all-or-nothing behavior. */
    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    event_loop_remove(loop, r1);
    event_loop_remove(loop, r2);
  }

  c_message_t leftover = {.data = NULL, .size = 0};
  while (circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  circular_queue_destroy(cq);
}

/* dynamic_queue analogue of the circular_queue test above: the rotor fix
 * applies identically to dynamic_queue's own sel_read_rotor/sel_write_rotor
 * fields via the exact same notify_one_sel_waiter()/_sel_unlink_waiter()
 * code paths, so this exercises that half of the fix directly rather than
 * relying on circular_queue coverage alone. */
typedef struct evl_dynq_two_readers_ctx {
  _Atomic size_t calls;
} evl_dynq_two_readers_ctx;

static void evl_dynq_two_readers_on_readable(event_loop loop,
                                             ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_dynq_two_readers_ctx *c = (evl_dynq_two_readers_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  if (dynmq_try_recv_zc(sel->dq, &msg) == ccol_success) {
    atomic_fetch_add(&c->calls, 1);
  }
}

typedef struct evl_dynq_feeder_args {
  dynamic_queue *dq;
  int iterations;
} evl_dynq_feeder_args;

static void *evl_dynq_no_backlog_feeder_thread(void *arg) {
  evl_dynq_feeder_args *a = (evl_dynq_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    (void)dynmq_send_zc(a->dq, &msg);
    struct timespec ts = {0, 200000}; /* 0.2ms */
    nanosleep(&ts, NULL);
  }
  return NULL;
}

TEST(event_loop, dynq_multiple_registrations_share_traffic_fairly) {
  dynamic_queue *dq = dynamic_queue_create(NULL);

  evl_dynq_two_readers_ctx ctx1, ctx2;
  atomic_init(&ctx1.calls, (size_t)0);
  atomic_init(&ctx2.calls, (size_t)0);

  {
    event_loop_construct_scoped(loop, 8, 1, 1);

    event_handlers_t handlers1 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    event_reg r1 =
        event_loop_add(loop, selectable_from_dynq(dq, ccol_select_read),
                       handlers1, &ctx1, &err);
    REQUIRE_NE(r1, EVENT_REG_INVALID);

    event_handlers_t handlers2 = {
        .on_readable = evl_dynq_two_readers_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    event_reg r2 =
        event_loop_add(loop, selectable_from_dynq(dq, ccol_select_read),
                       handlers2, &ctx2, &err);
    REQUIRE_NE(r2, EVENT_REG_INVALID);

    evl_dynq_feeder_args feeder_args = {.dq = dq, .iterations = 400};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_dynq_no_backlog_feeder_thread,
                              &feeder_args),
               0);
    pthread_join(feeder, NULL);

    struct timespec ts = {0, 50000000}; /* 50ms */
    nanosleep(&ts, NULL);

    size_t c1 = atomic_load(&ctx1.calls);
    size_t c2 = atomic_load(&ctx2.calls);
    REQUIRE_GE(c1 + c2, (size_t)350);
    REQUIRE_GT(c1, (size_t)(feeder_args.iterations / 8));
    REQUIRE_GT(c2, (size_t)(feeder_args.iterations / 8));

    event_loop_remove(loop, r1);
    event_loop_remove(loop, r2);
  }

  c_message_t leftover = {.data = NULL, .size = 0};
  while (dynmq_try_recv_zc(dq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }
  dynamic_queue_destroy(dq);
}

TEST(event_loop, remove_from_within_callback) {
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  ctx.self_loop = loop;
  event_handlers_t handlers = {.on_readable = evl_on_readable_self_remove,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);
  ctx.self_reg = reg;

  int *payload = malloc(sizeof(int));
  *payload = 1;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);

  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

  /* A second message must NOT be delivered (registration removed itself). */
  int *payload2 = malloc(sizeof(int));
  *payload2 = 2;
  c_message_t msg2 = {.data = payload2, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg2), ccol_success);
  usleep(50000);
  REQUIRE_EQ(ctx.readable_count, 1);

  c_message_t drained;
  REQUIRE_EQ(circq_recv_zc(cq, &drained), ccol_success);
  free(drained.data);

  evl_sync_ctx_destroy(&ctx);
  circular_queue_destroy(cq);
}

typedef struct evl_remover_args {
  event_loop loop;
  event_reg reg;
  int delay_us;
} evl_remover_args;

static void *evl_remover_thread(void *arg) {
  evl_remover_args *a = (evl_remover_args *)arg;
  usleep((useconds_t)a->delay_us);
  event_loop_remove(a->loop, a->reg);
  return NULL;
}

TEST(event_loop, remove_from_different_thread_concurrent_with_dispatch) {
  /* Repeated add -> notify -> concurrent-remove-from-another-thread cycles,
   * targeted stress for the refcount design; must never crash or leak
   * (verified separately under valgrind by the memtest target).
   *
   * ctx is heap-allocated per iteration and logged rather than destroyed
   * inline: event_loop_remove is documented to return while an in-flight
   * dispatch for the removed reg may still be running (it defers only the
   * library's OWN memory reclamation, not how long the callback itself
   * takes); so the remover thread joining does not, by itself, prove
   * evl_on_readable has finished touching ctx. Destroying ctx.mtx (or
   * reusing its stack slot next iteration, before this fix) could race
   * that still-in-flight callback; a real bug ThreadSanitizer caught
   * here despite num_reactor_threads == 1: this hazard has nothing to do
   * with multi-threaded dispatch, it's inherent to event_loop_remove's own
   * documented contract and was already present before this feature. Every
   * logged ctx is freed only after the whole loop (and the one reactor
   * thread that could still be mid-callback) has been joined, at this
   * test's own nested block below. cq itself has the identical hazard and
   * fix: the ORIGINAL version of this test called circular_queue_destroy(cq)
   * before the scoped loop's own automatic destructor (which only runs at
   * this function's closing brace) had joined the reactor thread, meaning a
   * stale, already-collected dispatch for the last iteration's just-removed
   * reg could still call circq_try_recv_zc(sel->cq, ...) on an
   * already-freed queue; a real, pre-existing use-after-free hazard this
   * fix also closes, not merely the ctx one. */
  evl_sync_ctx *ctx_log[50];
  int ctx_log_count = 0;
  circular_queue *cq = circular_queue_create(4, NULL);

  {
    event_loop_construct_scoped(loop, 8, 1, 1);

    for (int iter = 0; iter < 50; iter++) {
      evl_sync_ctx *ctx = malloc(sizeof(*ctx));
      evl_sync_ctx_init(ctx);
      ctx_log[ctx_log_count++] = ctx;
      event_handlers_t handlers = {.on_readable = evl_on_readable,
                                   .on_writable = NULL,
                                   .on_error = NULL};
      char *err = NULL;
      event_reg reg =
          event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                         handlers, ctx, &err);
      REQUIRE_NE(reg, EVENT_REG_INVALID);

      evl_remover_args rargs = {.loop = loop, .reg = reg, .delay_us = 0};
      pthread_t rtid;
      REQUIRE_EQ(pthread_create(&rtid, NULL, evl_remover_thread, &rargs), 0);

      int *payload = malloc(sizeof(int));
      *payload = iter;
      c_message_t msg = {.data = payload, .size = sizeof(int)};
      circq_send_zc(cq, &msg); /* may or may not be delivered; that's fine */

      pthread_join(rtid, NULL);

      /* Drain whatever's left so the queue doesn't grow unbounded across
       * iterations (the message may not have been consumed if removal won
       * the race before dispatch). */
      c_message_t leftover;
      while (circq_try_recv_zc(cq, &leftover) == ccol_success) {
        free(leftover.data);
      }
    }

    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

    /* loop shuts down and its one reactor thread is joined here, at block
     * exit; cq and every logged ctx are guaranteed quiescent from this
     * point on. */
  }

  circular_queue_destroy(cq);
  for (int i = 0; i < ctx_log_count; i++) {
    evl_sync_ctx_destroy(ctx_log[i]);
    free(ctx_log[i]);
  }
}

TEST(event_loop, shutdown_with_pending_registrations) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);
  /* Idempotent: calling again must not hang or double-join. */
  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);

  event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, destroy_while_queue_registration_pending_queue_outlives_loop) {
  /* Regression test for the destroy-path fix: freeing a queue-backed
   * event_reg without first unlinking its waiter_node from the queue's own
   * waiter list would leave the queue holding a dangling pointer, a
   * use-after-free the next time anyone sends/receives on it; especially
   * dangerous here since the queue is intentionally NOT destroyed until
   * after the event_loop is. */
  circular_queue *cq = circular_queue_create(4, NULL);

  char *err = NULL;
  event_loop loop = event_loop_create(8, 1, 1, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  /* Destroy the loop with the registration still pending; must unlink
   * from cq's waiter list before freeing, not after. */
  event_loop_destroy(loop);

  /* The queue must still be perfectly usable afterward. */
  int *payload = malloc(sizeof(int));
  *payload = 42;
  c_message_t msg = {.data = payload, .size = sizeof(int)};
  REQUIRE_EQ(circq_send_zc(cq, &msg), ccol_success);
  c_message_t out;
  REQUIRE_EQ(circq_recv_zc(cq, &out), ccol_success);
  REQUIRE_EQ(*(int *)out.data, 42);
  free(out.data);

  circular_queue_destroy(cq);
}

TEST(event_loop, reg_count_tracks_add_remove) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);
  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

  close(pfd[0]);
  close(pfd[1]);
}

/* Regression test for a real, reasoned-through (not previously crash-
 * reproduced) hazard fixed in event_loop_add: reg used to be wired into the
 * fd/queue registry (making it fully live and dispatchable, purely through
 * the raw event_reg_s* the registry stores) BEFORE its own public handle
 * was minted via _event_reg_slot_acquire. If slot acquisition then failed
 * (its only failure mode, an allocation failure growing loop->reg_slots),
 * event_loop_add reported EVENT_REG_INVALID to the caller, who, believing
 * no registration was ever created, had no reason to think a callback
 * might already be running (or, for num_reactor_threads > 1, queued on a
 * ctpool worker) against the arg pointer it had just supplied, and could
 * free it immediately: a genuine use-after-free, needing only an ordinary
 * allocation failure to coincide with the target fd already being ready.
 * Fixed by acquiring the slot BEFORE ever wiring reg into the registry, so
 * a slot-acquire failure now always finds reg fully unwired, with no
 * dispatch possible.
 *
 * The primary assertion below (last_forced_..._reg_count == 0) proves the
 * ORDERING directly and deterministically, independent of any actual
 * thread-scheduling race: event_loop_test_last_forced_slot_acquire_failure_
 * reg_count() reports event_loop_reg_count()'s own value as it stood at the
 * exact moment inside _event_reg_slot_acquire that the forced failure
 * fired, before any rollback could run. A pre-fix ordering (wire first,
 * acquire the slot last) would have already incremented reg_count for this
 * registration by that point, so this snapshot would read 1, not 0; a
 * post-return check of event_loop_reg_count() alone cannot distinguish the
 * two orderings, since a post-wiring failure's own rollback also restores
 * it to 0 by the time event_loop_add returns either way. This is why a
 * naive live-dispatch race (arm the hook, keep a genuinely ready fd sitting
 * next to it, and check whether a callback fires) is not by itself a
 * reliable regression guard here: confirmed directly, by temporarily
 * reordering event_loop_add back to the pre-fix "wire, then acquire" shape
 * while keeping this same hook; the window between releasing the stripe
 * lock and the (immediately following, unforced-work) slot-acquire call
 * turned out to be too narrow in practice for even an already-running,
 * already-blocked-in-epoll_wait poller thread on a separate core to
 * reliably win, across 5 consecutive full runs with num_reactor_threads =
 * 3 and pfd[0] already holding data before the call. The reg_count
 * snapshot below has no such timing dependency: it reads the ordering
 * directly off event_loop_add's own already-completed work at the instant
 * the hook fires, not a race between two threads. The live-dispatch checks
 * further down are kept anyway, as a secondary, best-effort corroboration
 * (and because they exercise the module's actual end-to-end behavior),
 * but the reg_count snapshot is what this test's own correctness rests on. */
TEST(event_loop, add_slot_acquire_failure_leaves_nothing_wired_or_dispatched) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int one = 1;
  REQUIRE_EQ(write(pfd[1], &one, sizeof(one)), (ssize_t)sizeof(one));

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* num_reactor_threads = 3: exercises the strictly more dangerous
     * async-dispatch-pool path (a ctpool worker could run the callback
     * well after event_loop_add's own caller has already moved on),
     * not just the inline num_reactor_threads == 1 path. */
    event_loop_construct_scoped(loop, 8, 1, 3);

    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

    event_loop_test_force_next_reg_slot_acquire_failure();

    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_EQ(reg, EVENT_REG_INVALID);
    REQUIRE_NE((void *)err, NULL);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);

    /* The deterministic proof: see this test's own leading comment. */
    REQUIRE_EQ(event_loop_test_last_forced_slot_acquire_failure_reg_count(),
               (size_t)0);

    /* Best-effort corroboration: pfd[0] is already, and remains, genuinely
     * readable; if the failed registration had left anything wired, the
     * poller (and, on this num_reactor_threads > 1 loop, a dispatch_pool
     * worker) would have every opportunity to dispatch it during this
     * bounded wait. Not itself relied upon to catch a regression (see this
     * test's own leading comment); a real bug here would still show up as
     * a crash/UAF under valgrind/ASan long before this assertion's own
     * timing margin became the limiting factor. */
    REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));
    pthread_mutex_lock(&ctx.mtx);
    REQUIRE_EQ(ctx.readable_count, 0);
    pthread_mutex_unlock(&ctx.mtx);

    /* The hook is one-shot: a normal call right after must succeed,
     * confirming it only ever affected the single call above. */
    err = NULL;
    event_reg reg2 =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg2, EVENT_REG_INVALID);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    event_loop_remove(loop, reg2);

    /* loop shuts down and every one of its threads is joined here, at
     * block exit; ctx is guaranteed quiescent from this point on. */
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, high_add_remove_churn_stress) {
  event_loop_construct_scoped(loop, 32, 1, 1);
  const int n = 200;
  int pfds[200][2];
  event_reg regs[200];

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(pipe(pfds[i]), 0);
    char *err = NULL;
    regs[i] =
        event_loop_add(loop, selectable_from_fd(pfds[i][0], ccol_select_read),
                       handlers, NULL, &err);
    REQUIRE_NE(regs[i], EVENT_REG_INVALID);
  }
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)n);

  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(event_loop_remove(loop, regs[i]), ccol_success);
    close(pfds[i][0]);
    close(pfds[i][1]);
  }
  REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
}

TEST(event_loop, multiple_independent_instances) {
  /* No shared global state between separate event_loop instances, including
   * their lock-stripe arrays: loop_a uses 1 stripe (the original
   * single-lock-equivalent behavior) and loop_b uses 8, deliberately
   * different, to confirm num_lock_stripes is a genuinely per-instance
   * setting with no cross-instance interference. */
  int pfd_a[2], pfd_b[2];
  REQUIRE_EQ(pipe(pfd_a), 0);
  REQUIRE_EQ(pipe(pfd_b), 0);

  evl_sync_ctx ctx_a, ctx_b;
  evl_sync_ctx_init(&ctx_a);
  evl_sync_ctx_init(&ctx_b);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment;
     * both fds here stay level-triggered-ready (evl_on_readable never
     * drains an fd selectable), so both loops' reactor threads must be
     * joined before either ctx is destroyed. */
    event_loop_construct_scoped(loop_a, 8, 1, 1);
    event_loop_construct_scoped(loop_b, 8, 8, 1);

    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    event_reg reg_a =
        event_loop_add(loop_a, selectable_from_fd(pfd_a[0], ccol_select_read),
                       handlers, &ctx_a, &err);
    REQUIRE_NE(reg_a, EVENT_REG_INVALID);
    event_reg reg_b =
        event_loop_add(loop_b, selectable_from_fd(pfd_b[0], ccol_select_read),
                       handlers, &ctx_b, &err);
    REQUIRE_NE(reg_b, EVENT_REG_INVALID);

    int val_a = 1, val_b = 2;
    REQUIRE_EQ((ssize_t)sizeof(val_a), write(pfd_a[1], &val_a, sizeof(val_a)));
    REQUIRE_TRUE(evl_wait_for(&ctx_a, &ctx_a.readable_count, 1, 2000));
    /* loop_b's registration must not have fired for loop_a's fd. Safe to
     * read unprotected: nothing has ever been written to pfd_b at this
     * point, so there is no possible concurrent writer of ctx_b.
     * readable_count to race; that's precisely the property being
     * tested. */
    REQUIRE_EQ(ctx_b.readable_count, 0);

    REQUIRE_EQ((ssize_t)sizeof(val_b), write(pfd_b[1], &val_b, sizeof(val_b)));
    REQUIRE_TRUE(evl_wait_for(&ctx_b, &ctx_b.readable_count, 1, 2000));

    event_loop_remove(loop_a, reg_a);
    event_loop_remove(loop_b, reg_b);

    /* both loops shut down and their reactor threads are joined here, at
     * block exit; ctx_a/ctx_b are guaranteed quiescent from this point
     * on. */
  }

  evl_sync_ctx_destroy(&ctx_a);
  evl_sync_ctx_destroy(&ctx_b);
  close(pfd_a[0]);
  close(pfd_a[1]);
  close(pfd_b[0]);
  close(pfd_b[1]);
}

// lock-striping tests
TEST(event_loop, num_lock_stripes_zero_returns_null) {
  char *err = NULL;
  event_loop loop = event_loop_create(8, 0, 1, &err);
  REQUIRE_EQ(loop, EVENT_LOOP_INVALID);
}

/* Regression tests for a real gap: max_events_per_wait had no upper-bound
 * validation at all, even though it is narrowed to a plain `int` for
 * epoll_wait(2)'s own maxevents parameter and used, unchecked, to size the
 * poller thread's events buffer allocation (max_events_per_wait *
 * sizeof(struct epoll_event)), both in _event_loop_thread_fn. A value
 * whose low 32 bits, reinterpreted as a signed int, were non-positive made
 * epoll_wait fail with EINVAL on the very first call; a merely huge value
 * could instead make the events-buffer allocation fail. Either way, the
 * poller thread's own error handling treated this as an ordinary, silent,
 * permanent thread exit (nothing distinguishes it from any other
 * unexpected epoll_wait/allocation failure): the constructor still
 * returned what looked like a perfectly valid, live event_loop handle
 * (event_loop_add kept succeeding), but no callback would ever fire
 * again, with nothing surfaced to the caller. Fixed by rejecting any
 * max_events_per_wait above INT_MAX, and any value that would overflow
 * size_t when multiplied by sizeof(struct epoll_event) (the latter is the
 * binding guard specifically on an ILP32 platform, where INT_MAX alone
 * does not rule out that multiplication overflowing a 32-bit size_t). */
TEST(event_loop, create_rejects_max_events_per_wait_exceeding_int_max) {
  char *err = NULL;
  event_loop loop = event_loop_create((size_t)INT_MAX + 1, 1, 1, &err);
  REQUIRE_EQ(loop, EVENT_LOOP_INVALID);
  REQUIRE_NE((void *)err, NULL);
}

TEST(
    event_loop,
    create_rejects_max_events_per_wait_that_would_overflow_events_buffer_size) {
  char *err = NULL;
  size_t smallest_overflowing = SIZE_MAX / sizeof(struct epoll_event) + 1;
  event_loop loop = event_loop_create(smallest_overflowing, 1, 1, &err);
  REQUIRE_EQ(loop, EVENT_LOOP_INVALID);
  REQUIRE_NE((void *)err, NULL);
}

/* A max_events_per_wait comfortably within both new guards must still be
 * accepted, and the resulting loop must actually dispatch, so the fix
 * above is confirmed not to be over-strict. */
TEST(event_loop, create_succeeds_with_a_large_but_valid_max_events_per_wait) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  event_loop loop = event_loop_create(4096, 1, 1, NULL);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int val = 1;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, fd_both_directions_combine_and_recombine_multi_stripe) {
  /* Same scenario as fd_both_directions_combine_and_recombine, but with
   * num_lock_stripes > 1: read and write directions on the SAME fd must
   * hash to (_stripe_index_for_fd is a pure function of the fd) and stay
   * serialized within the SAME stripe, so the EPOLL_CTL_ADD-then-MOD
   * combining path (and MOD-back-down on partial removal) must behave
   * identically to the single-stripe case. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);

  evl_sync_ctx read_ctx, write_ctx;
  evl_sync_ctx_init(&read_ctx);
  evl_sync_ctx_init(&write_ctx);

  {
    /* Nested block: see fd_on_readable_fires's identical pattern/comment. */
    event_loop_construct_scoped(loop, 8, 8, 1);

    event_handlers_t rh = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    event_handlers_t wh = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    char *err = NULL;
    event_reg rreg = event_loop_add(
        loop, selectable_from_fd(sv[0], ccol_select_read), rh, &read_ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write), wh,
                       &write_ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)2);

    REQUIRE_TRUE(evl_wait_for(&write_ctx, &write_ctx.writable_count, 1, 2000));

    REQUIRE_EQ(event_loop_remove(loop, wreg), ccol_success);
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)1);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(sv[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&read_ctx, &read_ctx.readable_count, 1, 2000));

    event_loop_remove(loop, rreg);
  }

  evl_sync_ctx_destroy(&read_ctx);
  evl_sync_ctx_destroy(&write_ctx);
  close(sv[0]);
  close(sv[1]);
}

TEST(event_loop, fd_modify_after_remove_returns_invalid_args_multi_stripe) {
  /* Same scenario as fd_modify_after_remove_returns_invalid_args, but with
   * num_lock_stripes > 1: event_loop_modify must still key its stripe
   * lookup off reg->stripe_idx (read lock-free, safe unconditionally for
   * any reg* the caller legitimately holds) and gracefully return
   * ccol_invalid_args, not crash or misbehave, when reg is already removed.
   *
   * Deliberately does NOT try to force reg's own underlying memory to
   * actually be freed before calling modify (e.g. by forcing several
   * reactor drain cycles to elapse first): reg is only guaranteed to
   * remain valid, stale, memory across the *short* window before the next
   * drain claims it (see _event_loop_defer_reg_free's doc comment);
   * forcing extra drain cycles here would let that window close and make
   * reg itself genuinely freed, which is no longer "gracefully handled
   * stale reg*" territory but real, out-of-contract use-after-free. An
   * earlier draft of this test tried exactly that and was caught by
   * valgrind flagging a real UAF; correctly so, since the bug was in the
   * test's own premise, not in event_loop_modify/_remove. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 8, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_modify(loop, reg, ccol_select_write),
             ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

/* ========================================================================== */
/*                    event_loop_pause / event_loop_resume                    */
/* ========================================================================== */

TEST(event_loop, pause_stops_delivery_then_resume_restores_it) {
  /* Pauses BEFORE the fd is ever made ready, deliberately, rather than
   * waiting for one delivery and then racing a pause call against it: a
   * plain pipe read end, once made ready, stays ready until drained, and
   * (with num_reactor_threads > 1, where EPOLLONESHOT is in play) the
   * dispatch job's own post-callback re-arm runs on a totally separate
   * thread from the one calling event_loop_pause, with no synchronization
   * between "callback observed to have run once" and "pause has taken
   * effect before the next re-arm"; an earlier draft of this test tried
   * exactly that ordering and was genuinely racy (failed intermittently,
   * not deterministically). Pausing first sidesteps the race entirely: no
   * delivery can happen before pause takes effect, because none has
   * happened yet. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);

  /* Now make the fd genuinely ready while paused; no on_readable callback
   * must fire no matter how long we wait; a bounded wait standing in for
   * "never", per this file's own established convention for asserting a
   * negative. */
  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 1, 300));

  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, pause_write_direction) {
  /* Same "pause before anything can fire" shape as
   * pause_stops_delivery_then_resume_restores_it above, for the identical
   * race-avoidance reason; doubly necessary here, since a pipe's write
   * end never stops being writable on its own, so waiting for one delivery
   * and then trying to pause before a second would be racing an
   * unboundedly-fast-refiring condition, not a one-shot event.
   *
   * A pipe's write end is writable from the instant it exists, including
   * during the brief window between event_loop_add returning and this
   * test's own very next line calling event_loop_pause (unlike the read
   * side above, genuinely not ready until this test's own later write()
   * call), so a dispatch racing ahead of the pause call here is a real,
   * if narrow, possibility, not just a theoretical one. The assertions
   * below are baseline-relative (count must not advance past whatever it
   * already was the instant pause() returned) specifically so this test
   * passes deterministically either way, rather than assuming the count is
   * exactly 0 at that point. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
  char *err = NULL;
  /* A pipe's write end is writable the moment it has room, which it does
   * immediately; no priming needed, unlike the circq_writable test above
   * (which had to fill the queue first to make write-readiness meaningful). */
  event_reg reg =
      event_loop_add(loop, selectable_from_fd(pfd[1], ccol_select_write),
                     handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  int baseline = ctx.writable_count;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, 300));

  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.writable_count, baseline + 1, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, pause_is_idempotent) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);

  event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, resume_never_paused_is_idempotent_and_harmless) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  /* Never paused; must succeed as a no-op and not disturb delivery. */
  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, pause_and_resume_reject_queue_selectable) {
  event_loop_construct_scoped(loop, 8, 1, 1);
  circular_queue *cq = circular_queue_create(4, NULL);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_circq(cq, ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  /* Same fd-only restriction as event_loop_modify: a queue/channel
   * registration's bridge eventfd has no "temporarily stop caring, but keep
   * the registration" use case worth exposing today. */
  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_invalid_args);
  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_invalid_args);

  event_loop_remove(loop, reg);
  circular_queue_destroy(cq);
}

TEST(event_loop, pause_and_resume_null_args_rejected) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(EVENT_LOOP_INVALID, reg), ccol_invalid_args);
  REQUIRE_EQ(event_loop_pause(loop, EVENT_REG_INVALID), ccol_invalid_args);
  REQUIRE_EQ(event_loop_resume(EVENT_LOOP_INVALID, reg), ccol_invalid_args);
  REQUIRE_EQ(event_loop_resume(loop, EVENT_REG_INVALID), ccol_invalid_args);

  event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, resume_after_remove_returns_invalid_args) {
  /* Mirrors fd_modify_after_remove_returns_invalid_args_multi_stripe's own
   * timing exactly (remove immediately followed by the operation under
   * test, no drain-forcing): reg is only guaranteed to remain valid, stale
   * memory across the short window before the next reclamation drain
   * claims it, so event_loop_resume must key its stripe lookup off reg's
   * own stripe_idx and gracefully report ccol_invalid_args, never crash. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, sequential_double_remove_returns_invalid_args_not_success) {
  /* Pins the documented contract precisely: a SECOND event_loop_remove()
   * call made strictly after an earlier one on the same reg has already
   * returned is not a no-op success; that earlier call already released
   * reg's own slot before returning (see _event_reg_slot_release, called
   * unconditionally on the successful-removal path before this function
   * returns), so reg_h no longer resolves at all by the time this second,
   * purely sequential call runs, and it reports ccol_invalid_args instead,
   * the same outcome event_loop_modify()/_pause()/_resume()/event_loop_
   * reg_generation() already give for an already-removed reg. Only a call
   * that genuinely RACES the first one, resolving reg before that first
   * call's own slot release, can observe ccol_success from a second
   * removal; see reg_handle_survives_concurrent_remove_vs_accessor_race
   * below for that separate, narrower window. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 1);

  event_handlers_t handlers = {0};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_remove(loop, reg), ccol_invalid_args);

  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, pause_resume_preserve_reg_count_and_generation) {
  /* Pause/resume must never look like a remove-then-add to any external
   * observer: the whole point is that the registration itself never goes
   * away, so both of these caller-visible identities must stay exactly the
   * same across a pause/resume cycle. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 1);

  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  size_t count_before = event_loop_reg_count(loop);
  uint64_t gen_before = event_loop_reg_generation(loop, reg);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), count_before);
  REQUIRE_EQ(event_loop_reg_generation(loop, reg), gen_before);

  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);
  REQUIRE_EQ(event_loop_reg_count(loop), count_before);
  REQUIRE_EQ(event_loop_reg_generation(loop, reg), gen_before);

  event_loop_remove(loop, reg);
  close(pfd[0]);
  close(pfd[1]);
}

/* Regression test for a real, previously-undetected bug: EPOLLERR/EPOLLHUP
 * are reported by the kernel unconditionally, regardless of the registered
 * interest mask (even mask 0 still gets them), so a registration paused
 * via event_loop_pause could not, before the fix, ever be fully silenced
 * against its own fd entering (or already being in) an error/hangup
 * condition: level-triggered epoll_wait kept reporting it on every single
 * call, forever, with the callback itself correctly skipped by reg->paused's
 * own re-check but nothing else stopping the reactor from re-observing and
 * re-collecting it; an unbounded, silent CPU-spin busy loop, directly
 * contradicting event_loop_pause's own documented "no callback fires,
 * exactly as if it had been removed" contract: an actually-removed
 * registration produces zero further wakeups (EPOLL_CTL_DEL), while a
 * merely mask-narrowed paused one could not, since ERR/HUP cannot be opted
 * out of via the interest mask. Fixed in _event_loop_rearm_entry_locked by
 * actually removing the fd from the epoll interest set (EPOLL_CTL_DEL)
 * whenever every live direction on it is currently paused (tracked via the
 * new event_entry.epoll_added field, since a fd removed this way must be
 * re-added via EPOLL_CTL_ADD, not EPOLL_CTL_MOD, once some direction wants
 * real interest again).
 *
 * Verified via event_loop_poller_iterations_for_tests (a direct count of
 * completed epoll_wait calls) rather than wall-clock/CPU measurement: with
 * the bug present, this counter races into the thousands within the sleep
 * window below; with the fix, the poller is genuinely blocked in
 * epoll_wait for the whole window (nothing else is registered with this
 * loop), so the count barely moves. Confirmed to fail (a delta orders of
 * magnitude larger than the bound below) against a scratch revert of just
 * the epoll_added-based DEL/ADD logic before this fix was reapplied. */
TEST(event_loop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_single_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {.on_readable = evl_on_readable,
                               .on_writable = NULL,
                               .on_error = evl_on_error};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);

  /* Put the fd into a persistent, level-triggered hangup condition while
   * paused: closing the write end makes the read end report EPOLLHUP. */
  close(pfd[1]);

  /* Give a pre-fix busy loop a real chance to run away before the first
   * sample, then measure the delta across a further, generous window. */
  usleep(50000);
  uint64_t before = event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* Multi-threaded reactor counterpart: with dispatch_pool present, the
 * fully-paused mask includes EPOLLONESHOT alone (still no real interest
 * bits), and the spin, before the fix, took the shape of a continuous
 * epoll_wait -> ctpool submit -> worker dequeue -> skip -> re-arm cycle
 * instead of a tight single-thread loop, but was just as unbounded; the
 * same poller_iterations_for_tests counter catches it identically, since
 * it counts completed epoll_wait calls on the one and only poller thread
 * regardless of num_reactor_threads. */
TEST(event_loop,
     pause_does_not_spin_when_fd_hangs_up_while_paused_multi_thread) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 3);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {.on_readable = evl_on_readable,
                               .on_writable = NULL,
                               .on_error = evl_on_error};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);

  close(pfd[1]);

  usleep(50000);
  uint64_t before = event_loop_poller_iterations_for_tests(loop);
  usleep(150000);
  uint64_t after = event_loop_poller_iterations_for_tests(loop);

  REQUIRE_LT(after - before, (uint64_t)20);

  pthread_mutex_lock(&ctx.mtx);
  REQUIRE_EQ(ctx.readable_count, 0);
  REQUIRE_EQ(ctx.error_count, 0);
  pthread_mutex_unlock(&ctx.mtx);

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

/* Companion to the two spin-regression tests above, covering the other
 * half of the same fix: once every direction on a fully-paused fd is
 * removed from the epoll interest set (EPOLL_CTL_DEL), resuming it must
 * re-add it via EPOLL_CTL_ADD, not EPOLL_CTL_MOD (MOD on a fd not
 * currently registered fails with ENOENT); see event_entry.epoll_added's
 * own field comment. Confirms the still-persistent hangup condition is
 * correctly observed and dispatched once resumed, proving the re-add path
 * actually works rather than merely leaving the fd silently unregistered. */
TEST(event_loop, resume_after_hangup_while_paused_delivers_dispatch) {
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 1, 1);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t handlers = {.on_readable = evl_on_readable,
                               .on_writable = NULL,
                               .on_error = evl_on_error};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  REQUIRE_EQ(event_loop_pause(loop, reg), ccol_success);
  close(pfd[1]);
  /* Let the (now fixed) reactor settle with the fd fully de-registered
   * before resuming. */
  usleep(100000);

  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);

  /* An empty pipe whose write end has been closed reports EPOLLHUP alone
   * (confirmed directly against this kernel via a standalone epoll_ctl/
   * epoll_wait reproduction; no EPOLLIN/EPOLLRDHUP bit accompanies it here,
   * unlike the socket-oriented "peer writes then closes" scenario
   * documented on _event_loop_handle_event's own has_reader comment), so
   * this dispatches as an error, not a readable, event. */
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.error_count, 1, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
}

typedef struct {
  evl_sync_ctx *ctx;
  event_loop loop;
  /* _Atomic: reg is only known once event_loop_add (below) returns, so it is
   * necessarily published to this struct AFTER the registration is already
   * live and could in principle be dispatched from another thread. In this
   * test's own exact sequence the callback can never actually observe reg
   * unset (the fd genuinely has nothing to read until this test's own later
   * write() call, issued strictly after reg is published), but a plain,
   * unsynchronized struct field read on one thread with no happens-before
   * edge to the write on another is still a real data race by the C memory
   * model regardless of that data-dependent timing guarantee; caught by
   * ThreadSanitizer, not by inspection. Atomic store/load establishes the
   * missing synchronization without changing any actual behavior. */
  _Atomic(event_reg) reg;
} evl_pause_from_callback_args;

/* Pauses its own registration from inside the callback, exactly matching
 * chttpserver.c's own _conn_start_diverted -> event_loop_pause call site
 * (called synchronously from within an in-flight on_readable dispatch). */
static void evl_on_readable_pause_self(event_loop loop, ccol_selectable *sel,
                                       void *arg) {
  evl_pause_from_callback_args *a = (evl_pause_from_callback_args *)arg;
  /* assert(), not REQUIRE_EQ: see evl_on_readable_self_remove's own
   * comment above for why calling a Tau assertion macro from a callback
   * running on event_loop's own reactor/dispatch thread (concurrently
   * with the main test thread's own REQUIRE_* calls) is unsafe. */
  assert(event_loop_pause(a->loop, atomic_load(&a->reg)) == ccol_success);
  evl_on_readable(loop, sel, a->ctx);
}

TEST(event_loop, pause_from_within_callback_then_resume_from_another_thread) {
  /* The exact pattern chttpserver.c relies on: on_readable pauses its own
   * registration synchronously from within the dispatch callback (mirroring
   * _conn_start_diverted), then a completely separate thread (mirroring
   * chttpserver's own worker_pool, distinct from event_loop's internal
   * reactor/dispatch threads) resumes it later. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  event_loop_construct_scoped(loop, 8, 4, 2);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  evl_pause_from_callback_args cb_args = {.ctx = &ctx, .loop = loop};
  event_handlers_t handlers = {.on_readable = evl_on_readable_pause_self,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg reg =
      event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                     handlers, &cb_args, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);
  atomic_store(&cb_args.reg, reg);

  int val = 1;
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  /* Paused inside the callback above; drain and write again to prove no
   * further delivery happens while paused, exactly like the single-thread
   * pause test. */
  int drain;
  REQUIRE_EQ(read(pfd[0], &drain, sizeof(drain)), (ssize_t)sizeof(drain));
  REQUIRE_EQ(write(pfd[1], &val, sizeof(val)), (ssize_t)sizeof(val));
  REQUIRE_FALSE(evl_wait_for(&ctx, &ctx.readable_count, 2, 300));

  /* Resume from the TEST's own thread; neither of event_loop's own
   * internal threads (the poller or a dispatch_pool worker). */
  REQUIRE_EQ(event_loop_resume(loop, reg), ccol_success);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 2, 2000));

  event_loop_remove(loop, reg);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct {
  event_loop loop;
  int thread_id;
  int iterations;
  /* Opened once before this worker's loop, closed once after; not per
   * iteration. Closing per iteration, while the loop (and its other 7
   * concurrently-running workers) stays alive, raced a legitimate
   * thundering-herd-adjacent stale dispatch's read() against this worker's
   * own close(), the same close()-vs-read() hazard
   * multi_thread_fd_reuse_generation_stays_consistent's own comment
   * documents catching via ThreadSanitizer. */
  int pfd[2];
  /* Every iteration's ctx is logged here instead of destroyed inline, for
   * the same reason as evl_reuse_driver_args's own identical field:
   * event_loop_remove can return while an in-flight callback for the
   * removed reg is still running, so destroying ctx.mtx (or reusing its
   * stack slot next iteration) immediately after remove() returns can race
   * that callback; a real, pre-existing bug (present even with a single
   * reactor thread; nothing about it is specific to multi-threaded
   * dispatch) ThreadSanitizer caught here. Freed by the TEST function only
   * after the whole loop, and every worker thread, has been joined. */
  evl_sync_ctx **ctx_log;
  int ctx_log_count;
  /* Queue-worker only: the circular_queue paired with ctx_log[j] at the
   * same index j, deferred and freed alongside it for the identical
   * reason (see ctx_log's own comment above and
   * evl_stripe_stress_queue_worker's own use of this field). Unused
   * (left NULL) by the fd worker. */
  circular_queue **cq_log;
} evl_stripe_stress_args;

static void *evl_stripe_stress_fd_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    event_reg reg =
        event_loop_add(a->loop, selectable_from_fd(a->pfd[0], ccol_select_read),
                       handlers, ctx, &err);
    if (reg) {
      a->ctx_log[a->ctx_log_count++] = ctx;
      int val = a->thread_id;
      test_write_retry_eintr(a->pfd[1], &val, sizeof(val));
      evl_wait_for(ctx, &ctx->readable_count, 1, 2000);
      event_loop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
    }
  }
  return NULL;
}

TEST(event_loop, multi_threaded_multi_fd_stress_with_stripes) {
  /* Many threads, many DISTINCT fds (unlike high_add_remove_churn_stress,
   * which is single-threaded and stripe-count 1), num_lock_stripes well
   * above 1; exercises genuinely concurrent event_loop_add/_remove/
   * dispatch across different stripes running in parallel, not just churn
   * on a single lock. */
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* Non-blocking: see evl_set_nonblocking's own comment; a legitimate
     * duplicate/stale dispatch reading this fd after this worker's own
     * feeding has moved on to a later iteration must not block forever. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  {
    /* Nested block: see the other multi-thread tests' identical pattern;
     * this loop's destructor joins every reactor thread at this block's
     * closing brace, which is what makes freeing every logged ctx
     * afterward, below, actually safe. */
    event_loop_construct_scoped(loop, 32, 16, 1);
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      REQUIRE_EQ(pthread_create(&threads[i], NULL, evl_stripe_stress_fd_worker,
                                &args[i]),
                 0);
    }
    for (int i = 0; i < n_threads; i++) {
      pthread_join(threads[i], NULL);
    }

    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  }

  for (int i = 0; i < n_threads; i++) {
    close(args[i].pfd[0]);
    close(args[i].pfd[1]);
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      evl_sync_ctx_destroy(args[i].ctx_log[j]);
      free(args[i].ctx_log[j]);
    }
    free(args[i].ctx_log);
  }
}

static void *evl_stripe_stress_queue_worker(void *arg) {
  evl_stripe_stress_args *a = (evl_stripe_stress_args *)arg;
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  for (int i = 0; i < a->iterations; i++) {
    circular_queue *cq = circular_queue_create(4, NULL);
    evl_sync_ctx *ctx = malloc(sizeof(*ctx));
    evl_sync_ctx_init(ctx);
    char *err = NULL;
    event_reg reg =
        event_loop_add(a->loop, selectable_from_circq(cq, ccol_select_read),
                       handlers, ctx, &err);
    if (reg) {
      /* Logged, not destroyed inline: event_loop_remove can return while
       * an in-flight callback for the removed reg is still running (see
       * evl_stripe_stress_args's own ctx_log/cq_log field comments), so
       * destroying ctx's mutex/cond or freeing cq immediately after
       * remove() returns can race that callback, exactly like
       * evl_stripe_stress_fd_worker's own identical deferred-cleanup
       * pattern. Freed by the TEST function only after the whole loop,
       * and every worker thread, has been joined. */
      a->ctx_log[a->ctx_log_count] = ctx;
      a->cq_log[a->ctx_log_count] = cq;
      a->ctx_log_count++;
      int *payload = malloc(sizeof(int));
      *payload = a->thread_id;
      c_message_t msg = {.data = payload, .size = sizeof(int)};
      circq_send_zc(cq, &msg);
      evl_wait_for(ctx, &ctx->readable_count, 1, 2000);
      event_loop_remove(a->loop, reg);
    } else {
      evl_sync_ctx_destroy(ctx);
      free(ctx);
      circular_queue_destroy(cq);
    }
  }
  return NULL;
}

TEST(event_loop, multi_threaded_multi_queue_stress_with_stripes) {
  /* Queue-selectable equivalent of the fd stress test above: many distinct
   * queues (hence many distinct bridge_efds, each round-robin-assigned to
   * a stripe via loop->next_queue_stripe) registered/removed concurrently
   * across threads. */
  const int n_threads = 8;
  const int iterations = 25;
  pthread_t threads[8];
  evl_stripe_stress_args args[8];

  for (int i = 0; i < n_threads; i++) {
    args[i].thread_id = i;
    args[i].iterations = iterations;
    args[i].ctx_log = malloc(sizeof(evl_sync_ctx *) * (size_t)iterations);
    args[i].cq_log = malloc(sizeof(circular_queue *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  {
    /* Nested block: see the other multi-thread tests' identical pattern;
     * this loop's destructor joins every reactor thread at this block's
     * closing brace, which is what makes freeing every logged ctx/cq
     * afterward, below, actually safe. */
    event_loop_construct_scoped(loop, 32, 16, 1);
    for (int i = 0; i < n_threads; i++) {
      args[i].loop = loop;
      REQUIRE_EQ(pthread_create(&threads[i], NULL,
                                evl_stripe_stress_queue_worker, &args[i]),
                 0);
    }
    for (int i = 0; i < n_threads; i++) {
      pthread_join(threads[i], NULL);
    }

    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)0);
  }

  for (int i = 0; i < n_threads; i++) {
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      evl_sync_ctx_destroy(args[i].ctx_log[j]);
      free(args[i].ctx_log[j]);
      circular_queue_destroy(args[i].cq_log[j]);
    }
    free(args[i].ctx_log);
    free(args[i].cq_log);
  }
}

TEST(event_loop, destroy_frees_registrations_across_multiple_stripes) {
  /* Registers enough distinct fds and queues, with num_lock_stripes > 1, to
   * spread live registrations across multiple stripes, then destroys the
   * loop WITHOUT removing them first; exercises __event_loop_destroy's
   * per-stripe walk (fd_index chmap + queue_regs_head unlinking) for every
   * stripe, not just stripe 0. */
  const int n = 20;
  int pfds[20][2];
  circular_queue *queues[20];
  event_handlers_t handlers = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};

  {
    event_loop_construct_scoped(loop, 32, 8, 1);
    char *err = NULL;

    for (int i = 0; i < n; i++) {
      REQUIRE_EQ(pipe(pfds[i]), 0);
      event_reg reg =
          event_loop_add(loop, selectable_from_fd(pfds[i][0], ccol_select_read),
                         handlers, NULL, &err);
      REQUIRE_NE(reg, EVENT_REG_INVALID);

      queues[i] = circular_queue_create(4, NULL);
      event_reg qreg = event_loop_add(
          loop, selectable_from_circq(queues[i], ccol_select_read), handlers,
          NULL, &err);
      REQUIRE_NE(qreg, EVENT_REG_INVALID);
    }
    REQUIRE_EQ(event_loop_reg_count(loop), (size_t)(2 * n));

    /* loop destroyed here (scope exit) with every registration still live;
     * __event_loop_destroy must walk and free all of them across every
     * stripe. */
  }

  for (int i = 0; i < n; i++) {
    close(pfds[i][0]);
    close(pfds[i][1]);
    circular_queue_destroy(queues[i]);
  }
}

/* Multi-threaded reactor (num_reactor_threads > 1) tests below */

TEST(event_loop, multi_thread_basic_smoke) {
  /* Plain single-fd readable dispatch, but with several reactor threads
   * sharing the epoll instance; confirms ordinary dispatch still works
   * correctly (not just "doesn't crash") once more than one thread is
   * calling epoll_wait on the same epfd. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* Nested block, same reasoning as the other multi-thread tests below:
     * evl_sync_ctx_destroy (like every other caller of it in this file)
     * assumes no callback can still be touching ctx by the time it runs;
     * true by construction with the single-reactor-thread tests elsewhere
     * in this file, but only an assumption here with 6 reactor threads
     * unless this block's join actually guarantees it. */
    event_loop_construct_scoped(loop, 8, 4, 6);

    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    int val = 7;
    REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));
    REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

    event_loop_remove(loop, reg);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct evl_no_double_dispatch_ctx {
  pthread_mutex_t mtx;
  int active;
  bool violation;
  _Atomic int total_calls;
} evl_no_double_dispatch_ctx;

/* Deliberately holds entry->dispatch_lock's protected region open for a
 * moment (usleep) after checking/updating `active`, widening the window in
 * which a second reactor thread (if dispatch_lock were broken or absent)
 * could receive this same still-ready fd from its own concurrent epoll_wait
 * call (epoll's default level-triggered, non-EPOLLEXCLUSIVE semantics
 * genuinely allow this) and enter this callback concurrently. */
static void evl_no_double_dispatch_on_readable(event_loop loop,
                                               ccol_selectable *sel,
                                               void *arg) {
  (void)loop;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;

  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  struct timespec ts = {0, 200000}; /* 0.2ms */
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

typedef struct evl_feeder_args {
  int write_fd;
  int iterations;
} evl_feeder_args;

static void *evl_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n; /* a full pipe buffer blocking briefly is fine and even
              * desirable here: it keeps the read end continuously ready,
              * maximizing the chance several reactor threads' concurrent
              * epoll_wait calls all observe it ready at once. */
  }
  return NULL;
}

TEST(event_loop, multi_thread_no_double_dispatch_same_fd) {
  /* Many reactor threads, ONE hot fd continuously fed by a writer thread;
   * exactly the thundering-herd scenario entry->dispatch_lock exists to
   * close (see its own field comment in cthreadcomm.c). Without that lock,
   * this test reliably catches active > 1 under stress; with it, active
   * never exceeds 1 regardless of how many reactor threads race for the
   * same entry. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  /* Non-blocking: the callback below performs its own read() directly (to
   * mimic real usage under genuinely concurrent dispatch), and once the
   * feeder thread stops writing, a legitimate thundering-herd duplicate
   * dispatch reading an already-drained, permanently-quiet pipe would
   * otherwise block forever on a blocking read; a real, reproduced
   * deadlock; see evl_set_nonblocking's own comment. */
  evl_set_nonblocking(pfd[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* Nested block: see multi_thread_cross_direction_serialization's
     * identical pattern and comment; ctx must not be destroyed, and pfd
     * must not be closed, until every reactor thread has actually been
     * joined (this block's closing brace), not merely after a heuristic
     * grace period. */
    event_loop_construct_scoped(loop, 8, 4, 12);

    event_handlers_t handlers = {
        .on_readable = evl_no_double_dispatch_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 4000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_feeder_thread, &feeder_args),
               0);
    pthread_join(feeder, NULL);

    /* Give the reactor threads a brief grace period to finish draining
     * whatever's left in the pipe after the feeder stops; purely to let
     * total_calls approach feeder_args.iterations before the assertion
     * below, not relied on for safety (the nested block's join is what
     * provides that). */
    for (int spins = 0;
         spins < 400 && atomic_load(&ctx.total_calls) < feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    event_loop_remove(loop, reg);

    /* loop shuts down and every reactor thread is joined here, at block
     * exit; ctx and pfd are guaranteed quiescent from this point on. */
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(pfd[0]);
  close(pfd[1]);
}

/* Same active-counter protocol as evl_no_double_dispatch_ctx, but shared
 * between a read-direction AND a write-direction callback on the SAME fd,
 * to verify entry->dispatch_lock's stricter cross-direction guarantee (not
 * just self-exclusion). */
static void evl_cross_dir_on_readable(event_loop loop, ccol_selectable *sel,
                                      void *arg) {
  (void)loop;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

static void evl_cross_dir_on_writable(event_loop loop, ccol_selectable *sel,
                                      void *arg) {
  (void)loop;
  (void)sel;
  evl_no_double_dispatch_ctx *c = (evl_no_double_dispatch_ctx *)arg;
  pthread_mutex_lock(&c->mtx);
  c->active++;
  if (c->active > 1) c->violation = true;
  pthread_mutex_unlock(&c->mtx);

  struct timespec ts = {0, 200000};
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->active--;
  pthread_mutex_unlock(&c->mtx);
  atomic_fetch_add(&c->total_calls, 1);
}

TEST(event_loop, multi_thread_cross_direction_serialization) {
  /* A stream socketpair gives one fd with both directions independently
   * live: sv[0]'s write direction stays ready indefinitely (nothing ever
   * fills its send buffer, since nothing here writes from sv[0] to sv[1]),
   * while a peer thread continuously feeds sv[1] to keep sv[0]'s read
   * direction ready too; both directions genuinely, concurrently
   * dispatchable across many reactor threads for the whole test. */
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
  /* Non-blocking for the same reason as multi_thread_no_double_dispatch_
   * same_fd's pfd[0]; see evl_set_nonblocking's own comment. */
  evl_set_nonblocking(sv[0]);

  evl_no_double_dispatch_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);

  {
    /* Nested block: event_loop_construct_scoped's destructor fires at this
     * block's closing brace, which shuts down AND JOINS every reactor
     * thread before control leaves it. That join is a real, non-heuristic
     * guarantee that no callback referencing ctx can possibly still be
     * running afterward; unlike a fixed nanosleep-based grace period
     * (which an earlier version of this test used and which ThreadSanitizer
     * still caught a real race through: event_loop_remove's own documented
     * contract only guarantees an in-flight callback for the reg being
     * removed will finish, not that no OTHER, independently-collected
     * dispatch for the same still-live entry, a legitimate, expected
     * thundering-herd duplicate, exactly the scenario
     * multi_thread_no_double_dispatch_same_fd exists to prove is
     * lock-serialized, not eliminated, won't still be running). Destroying
     * ctx.mtx or letting this function return (freeing ctx's stack slot)
     * before that join happened is exactly the bug this restructuring
     * avoids. */
    event_loop_construct_scoped(loop, 8, 4, 12);

    event_handlers_t read_handlers = {.on_readable = evl_cross_dir_on_readable,
                                      .on_writable = NULL,
                                      .on_error = NULL};
    event_handlers_t write_handlers = {.on_readable = NULL,
                                       .on_writable = evl_cross_dir_on_writable,
                                       .on_error = NULL};
    char *err = NULL;
    event_reg rreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_read),
                       read_handlers, &ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(sv[0], ccol_select_write),
                       write_handlers, &ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);

    evl_feeder_args feeder_args = {.write_fd = sv[1], .iterations = 4000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_feeder_thread, &feeder_args),
               0);
    pthread_join(feeder, NULL);

    for (int spins = 0; spins < 400 && atomic_load(&ctx.total_calls) < 500;
         spins++) {
      struct timespec ts = {0, 2000000};
      nanosleep(&ts, NULL);
    }

    event_loop_remove(loop, rreg);
    event_loop_remove(loop, wreg);

    /* loop shuts down and every reactor thread is joined here, at block
     * exit; ctx is guaranteed quiescent from this point on. */
  }

  REQUIRE_FALSE(ctx.violation);
  REQUIRE_GT(atomic_load(&ctx.total_calls), 0);

  pthread_mutex_destroy(&ctx.mtx);
  close(sv[0]);
  close(sv[1]);
}

typedef struct evl_reuse_ctx {
  event_reg reg;
  uint64_t expected_generation;
  _Atomic int mismatch_count;
  _Atomic int call_count;
} evl_reuse_ctx;

static void evl_reuse_on_readable(event_loop loop, ccol_selectable *sel,
                                  void *arg) {
  evl_reuse_ctx *c = (evl_reuse_ctx *)arg;
  char buf[16];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;
  /* If a stale batch entry from a PREVIOUS (already-removed) registration
   * on a recycled fd number ever misdispatched into this callback with the
   * WRONG ctx/reg pairing, this would observe a generation mismatch. */
  if (event_loop_reg_generation(loop, c->reg) != c->expected_generation) {
    atomic_fetch_add(&c->mismatch_count, 1);
  }
  atomic_fetch_add(&c->call_count, 1);
}

typedef struct evl_reuse_driver_args {
  event_loop loop;
  int iterations;
  _Atomic int mismatch_total;
  /* Every iteration's ctx is logged here instead of freed inline:
   * event_loop_remove's own documented contract is "safe to call
   * concurrently with an in-flight dispatch; teardown is deferred until any
   * in-progress callback returns"; it does NOT promise that no more
   * callback invocations for this reg can possibly still be in flight (e.g.
   * a second reactor thread that independently collected this same
   * still-registered entry from its own epoll_wait batch, via the
   * documented thundering-herd behavior other tests in this file also
   * exercise) by the time remove() returns. Freeing ctx inline, or reusing
   * its memory on the next iteration, would race a stale, still-running
   * callback's reads/writes against this iteration's own writes/frees; a
   * real bug an earlier version of this test had, caught by valgrind
   * (definitely-lost, from a first fix's deliberate never-free) and then
   * ThreadSanitizer (an actual use-after-free once that leak was
   * reasonably closed with a fixed-duration grace-period sleep instead of a
   * real join-based guarantee). The test function frees every logged ctx
   * only after the whole event_loop (every reactor thread) has been
   * shut down and joined; see its own nested-block comment for why that's
   * a real guarantee and a sleep never was. */
  evl_reuse_ctx **ctx_log;
  int ctx_log_count;
  /* Kept open for this driver's ENTIRE run rather than closed and reopened
   * every iteration. Re-adding the SAME still-open fd after removing it
   * still mints a fresh event_entry and hence a fresh generation each time
   * (event_loop_remove deletes the fd's registry entry before returning,
   * so a subsequent event_loop_add on that same fd number always takes the
   * new-entry path); enough to exercise this module's core generation
   * guarantee under heavy concurrent add/remove/dispatch/reclaim churn
   * across drivers sharing one event_loop, without ALSO needing to close()
   * a fd that a legitimate (if rare) thundering-herd duplicate dispatch
   * might still be reading; the exact close()-vs-read() race
   * ThreadSanitizer caught when this test did close per iteration. */
  int pfd[2];
} evl_reuse_driver_args;

static void *evl_reuse_driver_thread(void *arg) {
  evl_reuse_driver_args *a = (evl_reuse_driver_args *)arg;
  event_handlers_t handlers = {.on_readable = evl_reuse_on_readable,
                               .on_writable = NULL,
                               .on_error = NULL};

  for (int i = 0; i < a->iterations; i++) {
    evl_reuse_ctx *ctx = malloc(sizeof(*ctx));
    memset(ctx, 0, sizeof(*ctx));
    char *err = NULL;
    event_reg reg =
        event_loop_add(a->loop, selectable_from_fd(a->pfd[0], ccol_select_read),
                       handlers, ctx, &err);
    if (!reg) {
      free(ctx);
      continue;
    }
    ctx->reg = reg;
    ctx->expected_generation = event_loop_reg_generation(a->loop, reg);
    a->ctx_log[a->ctx_log_count++] = ctx;

    int val = i;
    ssize_t wn = write(a->pfd[1], &val, sizeof(val));
    (void)wn;

    for (int spins = 0; spins < 500 && atomic_load(&ctx->call_count) == 0;
         spins++) {
      struct timespec ts = {0, 500000};
      nanosleep(&ts, NULL);
    }

    event_loop_remove(a->loop, reg);

    atomic_fetch_add(&a->mismatch_total, atomic_load(&ctx->mismatch_count));
  }
  return NULL;
}

TEST(event_loop, multi_thread_fd_reuse_generation_stays_consistent) {
  /* Several driver threads, each rapidly re-registering/writing/removing
   * against its own fd in a tight loop, concurrently, sharing one
   * event_loop; exactly the kind of high-churn add/remove/dispatch/
   * reclaim workload under which a fresh generation must be minted (and
   * observed correctly) every single time, the core guarantee
   * event_loop_reg_generation exists to make safe by construction (see its
   * own doc comment; this is the same bug CLASS (stale identity confusion
   * across a fd's registration lifecycle) that historically bit this
   * codebase's fd-reuse-across-a-redirect-hand-off scenario, exercised
   * here as repeated same-fd re-registration under concurrent load rather
   * than an actual OS-level close+reopen, specifically to avoid racing a
   * legitimate thundering-herd duplicate dispatch against a close() call;
   * see evl_reuse_driver_args's own comment). Asserts every dispatch
   * observed its OWN registration's generation, never a stale/mismatched
   * one. */
  const int n_drivers = 4;
  const int iterations = 150;
  pthread_t drivers[4];
  evl_reuse_driver_args args[4];

  for (int i = 0; i < n_drivers; i++) {
    REQUIRE_EQ(pipe(args[i].pfd), 0);
    /* Non-blocking for the same reason as multi_thread_no_double_dispatch_
     * same_fd's pfd[0]; see evl_set_nonblocking's own comment; here it
     * additionally covers the window after a driver's last iteration
     * removes its registration but before the whole loop is torn down. */
    evl_set_nonblocking(args[i].pfd[0]);
    args[i].iterations = iterations;
    atomic_init(&args[i].mismatch_total, 0);
    args[i].ctx_log = malloc(sizeof(evl_reuse_ctx *) * (size_t)iterations);
    args[i].ctx_log_count = 0;
  }

  {
    /* Nested block: see multi_thread_cross_direction_serialization's
     * identical pattern and comment; this loop's destructor shuts down
     * and joins every reactor thread at this block's closing brace, which
     * is what makes freeing every logged ctx afterward, below, actually
     * safe rather than a timing guess. */
    event_loop_construct_scoped(loop, 8, 4, 8);
    for (int i = 0; i < n_drivers; i++) {
      args[i].loop = loop;
      REQUIRE_EQ(
          pthread_create(&drivers[i], NULL, evl_reuse_driver_thread, &args[i]),
          0);
    }
    for (int i = 0; i < n_drivers; i++) {
      pthread_join(drivers[i], NULL);
    }
  }

  int total_mismatches = 0;
  for (int i = 0; i < n_drivers; i++) {
    close(args[i].pfd[0]);
    close(args[i].pfd[1]);
    for (int j = 0; j < args[i].ctx_log_count; j++) {
      free(args[i].ctx_log[j]);
    }
    free(args[i].ctx_log);
    total_mismatches += atomic_load(&args[i].mismatch_total);
  }

  REQUIRE_EQ(total_mismatches, 0);
}

TEST(event_loop, multi_thread_shutdown_joins_poller_promptly) {
  /* Many reactor threads configured, nothing ever registered. Unlike before
   * the poller/dispatch_pool split, only ONE thread (poller_thread) is ever
   * actually blocked in epoll_wait here; the rest are idle ctpool workers
   * with nothing queued (see multi_thread_shutdown_drains_idle_dispatch_
   * pool_promptly below for that half). event_loop_shutdown's single
   * write() to shutdown_efd must still wake and join poller_thread
   * promptly: shutdown_efd is deliberately never drained (see
   * event_loop_shutdown's own comment for the real hang an earlier version
   * of this code hit by draining it), so poller_thread's epoll_wait call
   * keeps seeing it ready, however long it takes to actually return and
   * observe shutting_down. Bounds how long shutdown is allowed to take,
   * rather than merely asserting it eventually returns, so a regression
   * that leaves poller_thread stuck would show up as a slow/hung test, not
   * a silent pass. */
  char *err = NULL;
  event_loop loop = event_loop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  event_loop_destroy(loop);
}

TEST(event_loop, multi_thread_shutdown_drains_idle_dispatch_pool_promptly) {
  /* The other half of the shutdown story the poller/dispatch_pool split
   * introduced: with num_reactor_threads > 1, event_loop_shutdown also
   * calls ctpool_shutdown_drain on dispatch_pool, a completely different
   * mechanism (a condvar broadcast waking idle ctpool workers, not an
   * epoll_wait wakeup) from the poller-wake path covered above. Bounds how
   * long that call takes with a large, entirely idle worker pool, the same
   * way the test above bounds the poller half; a regression that left
   * some idle worker un-woken would show up here as a slow/hung test. */
  char *err = NULL;
  event_loop loop = event_loop_create(8, 4, 16, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  struct timespec start, end;
  clock_gettime(CLOCK_MONOTONIC, &start);
  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);
  clock_gettime(CLOCK_MONOTONIC, &end);

  double elapsed_ms = (double)(end.tv_sec - start.tv_sec) * 1000.0 +
                      (double)(end.tv_nsec - start.tv_nsec) / 1e6;
  REQUIRE_LT(elapsed_ms, 2000.0);

  event_loop_destroy(loop);
}

typedef struct evl_shutdown_drain_ctx {
  pthread_mutex_t mtx;
  pthread_cond_t cond;
  bool started;
  bool finished;
} evl_shutdown_drain_ctx;

/* Signals "started" as soon as this callback begins running, then sleeps
 * before signaling "finished"; giving the test driver a reliable way to
 * call event_loop_shutdown while this job is provably still in flight,
 * rather than racing shutdown against the poller ever noticing the
 * triggering write at all. */
static void evl_shutdown_drain_on_readable(event_loop loop,
                                           ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_shutdown_drain_ctx *c = (evl_shutdown_drain_ctx *)arg;
  char buf[8];
  ssize_t n = read(sel->fd, buf, sizeof(buf));
  (void)n;

  pthread_mutex_lock(&c->mtx);
  c->started = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);

  struct timespec ts = {0, 100000000}; /* 100ms */
  nanosleep(&ts, NULL);

  pthread_mutex_lock(&c->mtx);
  c->finished = true;
  pthread_cond_broadcast(&c->cond);
  pthread_mutex_unlock(&c->mtx);
}

TEST(event_loop, multi_thread_shutdown_drains_in_flight_dispatch_job) {
  /* Unlike the two tests above (idle pool), this one puts a real job in
   * flight (a callback deliberately sleeping, synchronized so the test
   * driver knows it has genuinely started before calling
   * event_loop_shutdown) directly exercising ctpool_shutdown_drain's
   * "finish in-flight work" contract (chosen over ctpool_shutdown_immediate
   * specifically to preserve event_loop_shutdown's own documented "no
   * dispatch can be in flight once this returns" guarantee; see that
   * function's own comment). Asserts the callback actually completed (not
   * merely that shutdown returned), which immediate-cancel semantics would
   * not guarantee. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_shutdown_drain_ctx ctx;
  memset(&ctx, 0, sizeof(ctx));
  assert(pthread_mutex_init(&ctx.mtx, NULL) == 0);
  assert(pthread_cond_init(&ctx.cond, NULL) == 0);

  event_loop loop = event_loop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  event_handlers_t handlers = {.on_readable = evl_shutdown_drain_on_readable,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 2;
  pthread_mutex_lock(&ctx.mtx);
  while (!ctx.started) {
    if (pthread_cond_timedwait(&ctx.cond, &ctx.mtx, &deadline) == ETIMEDOUT)
      break;
  }
  bool started = ctx.started;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_TRUE(started);

  REQUIRE_EQ(event_loop_shutdown(loop), ccol_success);

  pthread_mutex_lock(&ctx.mtx);
  bool finished = ctx.finished;
  pthread_mutex_unlock(&ctx.mtx);
  REQUIRE_TRUE(finished);

  event_loop_destroy(loop);
  pthread_mutex_destroy(&ctx.mtx);
  pthread_cond_destroy(&ctx.cond);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop, reg_generation_semantics) {
  /* NULL reg reads as generation 0 (reserved, never minted for a real
   * registration). */
  REQUIRE_EQ(event_loop_reg_generation(EVENT_LOOP_INVALID, EVENT_REG_INVALID),
             (uint64_t)0);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  int pfd2[2];
  REQUIRE_EQ(pipe(pfd2), 0);

  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);

  {
    /* Nested block, same reasoning as the other multi-thread tests in this
     * file: wreg's write direction dispatches almost immediately (a fresh
     * pipe write end is always writable), and with 4 reactor threads
     * evl_sync_ctx_destroy must not run (nor may this function return,
     * freeing ctx's stack slot) until that dispatch (and any other one
     * still in flight) is guaranteed finished, which only this block's
     * join actually guarantees. */
    event_loop_construct_scoped(loop, 8, 1, 4);
    event_handlers_t handlers = {
        .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
    char *err = NULL;

    /* Both directions on the same fd share one generation. */
    event_reg rreg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(rreg, EVENT_REG_INVALID);
    uint64_t rgen = event_loop_reg_generation(loop, rreg);
    REQUIRE_GT(rgen, (uint64_t)0);

    event_handlers_t write_handlers = {
        .on_readable = NULL, .on_writable = evl_on_writable, .on_error = NULL};
    event_reg wreg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_write),
                       write_handlers, &ctx, &err);
    REQUIRE_NE(wreg, EVENT_REG_INVALID);
    REQUIRE_EQ(event_loop_reg_generation(loop, wreg), rgen);

    /* event_loop_modify (direction flip) keeps the same generation; it's
     * the same underlying fd/connection, just a different direction. */
    event_loop_remove(loop, wreg);
    REQUIRE_EQ(event_loop_modify(loop, rreg, ccol_select_write), ccol_success);
    REQUIRE_EQ(event_loop_reg_generation(loop, rreg), rgen);
    REQUIRE_EQ(event_loop_modify(loop, rreg, ccol_select_read), ccol_success);

    /* A different fd gets a different generation. */
    event_reg reg2 =
        event_loop_add(loop, selectable_from_fd(pfd2[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg2, EVENT_REG_INVALID);
    REQUIRE_NE(event_loop_reg_generation(loop, reg2), rgen);

    event_loop_remove(loop, rreg);
    event_loop_remove(loop, reg2);
  }

  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
  close(pfd2[0]);
  close(pfd2[1]);
}

/* No-op: registered purely to keep the poller looping (epoll_wait
 * returning, poller_batch_gen advancing, epoll_wait being re-entered)
 * continuously and as fast as possible for the duration of
 * reg_handle_survives_concurrent_remove_vs_accessor_race below; a pipe's
 * write end is writable from the instant it exists and stays that way
 * forever, so registering several of them for write-interest is enough to
 * keep the reactor busy without needing a second thread to keep feeding
 * it. */
static void evl_reg_race_hot_writable(event_loop loop, ccol_selectable *sel,
                                      void *arg) {
  (void)loop;
  (void)sel;
  (void)arg;
}

typedef struct evl_reg_race_args {
  event_loop loop;
  event_reg reg;
} evl_reg_race_args;

static void *evl_reg_race_remover_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  event_loop_remove(a->loop, a->reg);
  return NULL;
}

static void *evl_reg_race_accessor_thread(void *arg) {
  evl_reg_race_args *a = (evl_reg_race_args *)arg;
  /* Genuinely racing event_loop_remove on the other thread for the exact
   * same reg: any return value from any of these five calls is acceptable
   * (ccol_success/ccol_not_permitted/ccol_invalid_args, or generation 0),
   * entirely dependent on which thread the scheduler lets win. What this
   * test actually verifies (via a clean run, and especially via a clean
   * `make memtest` run) is that none of them ever touches memory the
   * reclaimer has already freed out from under them; see struct
   * event_loop_s's own reg_slots field comment. */
  event_loop_modify(a->loop, a->reg, ccol_select_write);
  event_loop_pause(a->loop, a->reg);
  event_loop_resume(a->loop, a->reg);
  (void)event_loop_reg_generation(a->loop, a->reg);
  event_loop_remove(a->loop, a->reg);
  return NULL;
}

TEST(event_loop, reg_handle_survives_concurrent_remove_vs_accessor_race) {
  /* Regression test for a real bug: event_loop_modify/_pause/_resume/
   * _remove/event_loop_reg_generation all used to dereference the
   * caller-supplied reg pointer directly (starting with reg->stripe_idx,
   * just to find out which stripe lock would protect the rest of the
   * call) with no protection at all against a concurrent poller reclaim
   * of that exact reg, which could complete on the very next poller loop
   * iteration after a DIFFERENT thread's event_loop_remove() deferred it;
   * a genuine use-after-free for two threads racing on the same reg, not
   * merely a theoretical concern. Fixed by making event_reg an opaque,
   * generation-checked VALUE handle resolved through its own loop's
   * registration table before anything is dereferenced, exactly
   * mirroring how event_loop's own handle already works; see struct
   * event_loop_s's own reg_slots field comment for the full design.
   *
   * A handful of always-ready write-direction "hot" registrations keep
   * the poller cycling continuously for this whole test, maximizing how
   * often the old, unprotected window would actually get exercised; many
   * short-lived fd registrations are then raced, back to back, between
   * one thread that removes each one and a second thread that
   * concurrently calls every other reg-accessor entry point on that
   * exact same reg. This is the single most direct way to catch a
   * regression of the fix: run under `make memtest`, a reintroduced
   * version of the bug reliably reports a real, valgrind-detected
   * use-after-free here, not just an intermittent failure. */
  event_loop_construct_scoped(loop, 64, 4, 4);

  enum { N_HOT = 4 };
  int hot_pfd[N_HOT][2];
  event_reg hot_reg[N_HOT];
  event_handlers_t hot_handlers = {.on_readable = NULL,
                                   .on_writable = evl_reg_race_hot_writable,
                                   .on_error = NULL};
  for (int i = 0; i < N_HOT; i++) {
    REQUIRE_EQ(pipe(hot_pfd[i]), 0);
    char *err = NULL;
    hot_reg[i] = event_loop_add(
        loop, selectable_from_fd(hot_pfd[i][1], ccol_select_write),
        hot_handlers, NULL, &err);
    REQUIRE_NE(hot_reg[i], EVENT_REG_INVALID);
  }

  const int iterations = 300;
  event_handlers_t reg_handlers = {0};
  for (int i = 0; i < iterations; i++) {
    int pfd[2];
    REQUIRE_EQ(pipe(pfd), 0);
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       reg_handlers, NULL, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    evl_reg_race_args shared = {loop, reg};
    pthread_t remover, accessor;
    REQUIRE_EQ(
        pthread_create(&remover, NULL, evl_reg_race_remover_thread, &shared),
        0);
    REQUIRE_EQ(
        pthread_create(&accessor, NULL, evl_reg_race_accessor_thread, &shared),
        0);
    pthread_join(remover, NULL);
    pthread_join(accessor, NULL);

    close(pfd[0]);
    close(pfd[1]);
  }

  for (int i = 0; i < N_HOT; i++) {
    event_loop_remove(loop, hot_reg[i]);
    close(hot_pfd[i][0]);
    close(hot_pfd[i][1]);
  }
}

typedef struct evl_bounded_ctx {
  _Atomic size_t calls;
} evl_bounded_ctx;

/* Deliberately slower than a continuously-refilling feeder can keep up
 * with, maximizing the poller's own chances to loop back to epoll_wait
 * while a dispatch job for this exact fd is still queued or executing;
 * precisely the scenario that would mint an unbounded stream of redundant
 * dispatch jobs without EPOLLONESHOT's re-arm-after-dispatch fix (see
 * _event_loop_poller_collect's own comment in cthreadcomm.c). */
static void evl_bounded_hot_fd_on_readable(event_loop loop,
                                           ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  char buf[64];
  ssize_t n;
  do {
    n = read(sel->fd, buf, sizeof(buf));
  } while (n > 0);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

static void *evl_bounded_feeder_thread(void *arg) {
  evl_feeder_args *a = (evl_feeder_args *)arg;
  char byte = 'x';
  for (int i = 0; i < a->iterations; i++) {
    ssize_t n = write(a->write_fd, &byte, 1);
    (void)n;
  }
  return NULL;
}

TEST(event_loop, multi_thread_hot_fd_dispatch_pool_pending_stays_bounded) {
  /* Direct regression test for the EPOLLONESHOT re-arm fix: with collection
   * (poller_thread) decoupled from dispatch (a ctpool worker), a still-
   * ready fd not yet re-armed must never be re-collected; if it were,
   * dispatch_pool's own pending-job count would grow without bound for the
   * whole duration a slow callback lags behind a fast feeder. Samples
   * event_loop_dispatch_pool_pending_count_for_tests while a feeder thread
   * writes continuously and asserts it never exceeds a small bound (well
   * under num_reactor_threads - 1 workers plus one more queued; the
   * livelock this test guards against would blow far past that, not
   * merely nudge over it). */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    event_loop_construct_scoped(loop, 8, 4, 8);

    event_handlers_t handlers = {.on_readable = evl_bounded_hot_fd_on_readable,
                                 .on_writable = NULL,
                                 .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    /* Deliberately modest: at ~0.5ms/dispatch this fully drains within the
     * grace period below, matching multi_thread_no_double_dispatch_same_
     * fd's own established pattern; the sampling window while the feeder
     * is still running is what actually stresses the mechanism, not the
     * total byte count. */
    evl_feeder_args feeder_args = {.write_fd = pfd[1], .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(
        pthread_create(&feeder, NULL, evl_bounded_feeder_thread, &feeder_args),
        0);

    for (int i = 0; i < 200; i++) {
      size_t pending = event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* Brief, best-effort settle time before tearing anything down; purely
     * test hygiene (nothing here asserts on leftover unread pipe bytes,
     * unlike circular_queue_destroy in the queue version of this test
     * below), not relied on for the max_pending assertion itself; kept
     * short since a full drain isn't needed and isn't worth this test's
     * wall-clock time. */
    for (int spins = 0; spins < 100 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    event_loop_remove(loop, reg);
  }

  REQUIRE_LT(max_pending, (size_t)10);
}

typedef struct evl_bounded_queue_feeder_args {
  circular_queue *cq;
  int iterations;
} evl_bounded_queue_feeder_args;

static void *evl_bounded_queue_feeder_thread(void *arg) {
  evl_bounded_queue_feeder_args *a = (evl_bounded_queue_feeder_args *)arg;
  for (int i = 0; i < a->iterations; i++) {
    c_message_t msg = {.data = NULL, .size = 0};
    /* Blocking, not circq_try_send_zc: a full queue should throttle this
     * feeder exactly like a full pipe buffer already throttles
     * evl_bounded_feeder_thread's plain write() above, giving both tests
     * the same "every iteration eventually gets delivered" guarantee
     * rather than silently dropping sends past capacity (which would make
     * ctx.calls never reliably reach iterations below). */
    (void)circq_send_zc(a->cq, &msg);
  }
  return NULL;
}

/* Same slow-callback shape as evl_bounded_hot_fd_on_readable, for the
 * bridge-eventfd/queue-selectable path; the design note in
 * _event_loop_dispatch_job_fn flags this case as having had NO natural
 * throttle at all before the EPOLLONESHOT fix (a bridge eventfd has no
 * finite kernel buffer the way a pipe does), so this is the higher-value
 * of the two regression tests, not a redundant mirror of the fd one. */
static void evl_bounded_hot_queue_on_readable(event_loop loop,
                                              ccol_selectable *sel, void *arg) {
  (void)loop;
  evl_bounded_ctx *c = (evl_bounded_ctx *)arg;
  c_message_t msg = {.data = NULL, .size = 0};
  (void)circq_try_recv_zc(sel->cq, &msg);
  struct timespec ts = {0, 500000}; /* 0.5ms */
  nanosleep(&ts, NULL);
  atomic_fetch_add(&c->calls, 1);
}

TEST(event_loop, multi_thread_hot_queue_dispatch_pool_pending_stays_bounded) {
  circular_queue *cq = circular_queue_create(64, NULL);
  REQUIRE_NE((void *)cq, NULL);

  evl_bounded_ctx ctx;
  atomic_init(&ctx.calls, (size_t)0);

  size_t max_pending = 0;
  {
    event_loop_construct_scoped(loop, 8, 4, 8);

    event_handlers_t handlers = {
        .on_readable = evl_bounded_hot_queue_on_readable,
        .on_writable = NULL,
        .on_error = NULL};
    char *err = NULL;
    event_reg reg =
        event_loop_add(loop, selectable_from_circq(cq, ccol_select_read),
                       handlers, &ctx, &err);
    REQUIRE_NE(reg, EVENT_REG_INVALID);

    evl_bounded_queue_feeder_args feeder_args = {.cq = cq, .iterations = 1000};
    pthread_t feeder;
    REQUIRE_EQ(pthread_create(&feeder, NULL, evl_bounded_queue_feeder_thread,
                              &feeder_args),
               0);

    for (int i = 0; i < 200; i++) {
      size_t pending = event_loop_dispatch_pool_pending_count_for_tests(loop);
      if (pending > max_pending) max_pending = pending;
      struct timespec ts = {0, 1000000}; /* 1ms */
      nanosleep(&ts, NULL);
    }

    pthread_join(feeder, NULL);

    /* Best-effort grace period, not asserted on: confirmed (by direct
     * comparison against num_reactor_threads == 1, the unmodified code
     * path) that a queue reader with only ONE registration can legitimately
     * finish with a handful of messages still unconsumed once the feeder
     * stops sending; a pre-existing characteristic of the bridge
     * eventfd's own notify-on-send coalescing, unrelated to this test's own
     * EPOLLONESHOT regression coverage. Draining directly below (after the
     * registration is removed) is what actually guarantees
     * circular_queue_destroy never asserts, not this loop. */
    for (int spins = 0; spins < 500 && atomic_load(&ctx.calls) <
                                           (size_t)feeder_args.iterations;
         spins++) {
      struct timespec ts = {0, 2000000}; /* 2ms */
      nanosleep(&ts, NULL);
    }

    event_loop_remove(loop, reg);
  }

  /* Drain whatever the event_loop-driven consumption above didn't get to
   * (see the grace-period loop's own comment) directly, now that reg has
   * been removed and the loop's own scope has closed; circular_queue_
   * destroy asserts on any remaining message (see its own comment in
   * cthreadcomm.c), so this is required for a clean teardown, not
   * optional hygiene. */
  c_message_t leftover = {.data = NULL, .size = 0};
  while (circq_try_recv_zc(cq, &leftover) == ccol_success) {
    /* nothing to free: every sent message here has data == NULL */
  }

  circular_queue_destroy(cq);
  REQUIRE_LT(max_pending, (size_t)10);
}

/* on_readable that never returns on its own; used only to hold a job
 * in flight on a dispatch_pool worker (or, for num_reactor_threads == 1,
 * to occupy the sole thread) long enough for the test driver to reliably
 * call event_loop_shutdown from within it. */
typedef struct evl_self_shutdown_ctx {
  event_loop loop;
  _Atomic int observed_rv; /* holds a ccol_retval_t; _Atomic since the test
                            * driver polls this from the main thread while
                            * the callback (poller_thread or a dispatch_pool
                            * worker) writes it with no other synchronization
                            * between them; a real, TSan-caught race in an
                            * earlier version of this test that used a plain
                            * ccol_retval_t field here. */
} evl_self_shutdown_ctx;

static void evl_self_shutdown_on_readable(event_loop loop, ccol_selectable *sel,
                                          void *arg) {
  (void)sel;
  evl_self_shutdown_ctx *c = (evl_self_shutdown_ctx *)arg;
  atomic_store(&c->observed_rv, (int)event_loop_shutdown(loop));
}

TEST(event_loop,
     shutdown_from_within_callback_returns_not_permitted_single_thread) {
  /* num_reactor_threads == 1: the sole thread both polls and dispatches, so
   * a self-call here would join itself (EDEADLK) without the guard. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  event_loop loop = event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);
  ctx.loop = loop;

  event_handlers_t handlers = {.on_readable = evl_self_shutdown_on_readable,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  /* Bounded poll instead of evl_wait_for: this ctx has no mutex/condvar of
   * its own (deliberately minimal), and observed_rv's default sentinel
   * value is what's being waited on. */
  for (int i = 0;
       i < 500 && atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure;
       i++) {
    struct timespec ts = {0, 2000000}; /* 2ms */
    nanosleep(&ts, NULL);
  }
  REQUIRE_EQ(atomic_load(&ctx.observed_rv), (int)ccol_not_permitted);

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

TEST(event_loop,
     shutdown_from_within_callback_returns_not_permitted_multi_thread) {
  /* num_reactor_threads > 1: the callback runs on a dispatch_pool worker,
   * not poller_thread; a self-call here would call
   * ctpool_shutdown_drain from within one of the pool's own workers
   * (thread_join'ing itself) without the guard. */
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);

  evl_self_shutdown_ctx ctx;
  atomic_init(&ctx.observed_rv, (int)ccol_unexpected_failure);

  event_loop loop = event_loop_create(8, 4, 4, NULL);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);
  ctx.loop = loop;

  event_handlers_t handlers = {.on_readable = evl_self_shutdown_on_readable,
                               .on_writable = NULL,
                               .on_error = NULL};
  char *err = NULL;
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), handlers, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  int val = 7;
  REQUIRE_EQ((ssize_t)sizeof(val), write(pfd[1], &val, sizeof(val)));

  for (int i = 0;
       i < 500 && atomic_load(&ctx.observed_rv) == (int)ccol_unexpected_failure;
       i++) {
    struct timespec ts = {0, 2000000}; /* 2ms */
    nanosleep(&ts, NULL);
  }
  REQUIRE_EQ(atomic_load(&ctx.observed_rv), (int)ccol_not_permitted);

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

/* ========================================================================== */
/*         EVENT_LOOP HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)         */
/* ========================================================================== */

/* Mirrors the already-implemented, already-verified chttpcli_handle_
 * lifecycle test group in tests/chttpclient/tests.c, adapted for
 * event_loop's own fully-lock-free pin mechanism (see src/cthreadcomm.c's
 * own struct event_loop_s.pending_resolve_count field comment for why the
 * unpin side here is a bare atomic decrement rather than chttpcli/
 * chttpsvr's lock-protected one, and __event_loop_destroy's own comment for
 * why that makes destroy wait by polling rather than a condvar). */

/* A fully completed destroy, followed later by a second destroy call on an
 * independently-held copy of the same original handle value, must be a
 * fatal error. Run in a forked child (mirroring tests/clogger/tests.c's own
 * fork-test precedent for process-terminating misuse) since fatal_err
 * aborts the whole process. */
TEST(event_loop_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);
    event_loop stale = loop;     /* an independently-held copy of the handle
            value, distinct from the local the macro below invalidates */
    event_loop_destroy(loop);    /* completes normally; the local `loop` is now
           EVENT_LOOP_INVALID, but `stale` still holds the original value */
    __event_loop_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

static void evl_self_destroy_on_readable(event_loop loop, ccol_selectable *sel,
                                         void *arg) {
  (void)sel;
  (void)arg;
  event_loop_destroy(loop); /* the actual misuse under test */
}

/* Calling event_loop_destroy on a loop from within a callback currently
 * dispatching on that loop's own thread (the poller thread for
 * num_reactor_threads == 1, or a dispatch_pool worker for > 1) must be a
 * fatal error, exactly like the sequential/concurrent double-destroy cases
 * above, not a silently-skipped teardown: __event_loop_destroy's own
 * internal event_loop_shutdown call already detects and rejects a self-join
 * here (see shutdown_from_within_callback_returns_not_permitted_* above),
 * but that alone is not enough, since __event_loop_destroy has no way to
 * propagate that rejection to its own void-returning, macro-driven
 * contract; without an explicit guard of its own, it would free every live
 * registration and the loop struct itself out from under the
 * still-executing callback regardless, a genuine use-after-free rather than
 * a mere deadlock. Run in a forked child since fatal_err aborts the whole
 * process. */
TEST(event_loop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_single_thread) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) _exit(2);
    evl_set_nonblocking(pfd[0]);

    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);

    event_handlers_t handlers = {.on_readable = evl_self_destroy_on_readable,
                                 .on_writable = NULL,
                                 .on_error = NULL};
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, NULL, NULL);
    if (reg == EVENT_REG_INVALID) _exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val)) _exit(2);

    /* Bounds the child's own lifetime in case fatal_err somehow does not
     * fire as expected, rather than hanging the whole suite. */
    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(event_loop_handle_lifecycle,
     destroy_from_within_callback_is_fatal_multi_thread) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int pfd[2];
    if (pipe(pfd) != 0) _exit(2);
    evl_set_nonblocking(pfd[0]);

    event_loop loop = event_loop_create(8, 4, 4, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);

    event_handlers_t handlers = {.on_readable = evl_self_destroy_on_readable,
                                 .on_writable = NULL,
                                 .on_error = NULL};
    event_reg reg =
        event_loop_add(loop, selectable_from_fd(pfd[0], ccol_select_read),
                       handlers, NULL, NULL);
    if (reg == EVENT_REG_INVALID) _exit(2);

    int val = 7;
    if (write(pfd[1], &val, sizeof(val)) != (ssize_t)sizeof(val)) _exit(2);

    struct timespec ts = {1, 0};
    nanosleep(&ts, NULL);
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  event_loop h;
} evl_concurrent_destroy_arg_t;

static void *evl_concurrent_destroy_thread(void *arg) {
  evl_concurrent_destroy_arg_t *a = (evl_concurrent_destroy_arg_t *)arg;
  __event_loop_destroy(a->h);
  return NULL;
}

/* Two threads calling destroy on two independently-held copies of the SAME,
 * still-valid handle at (as close to) the same moment as possible must also
 * be fatal; regression coverage for the same class of concurrent double-free
 * this whole redesign exists to close for chttpcli/chttpsvr. */
TEST(event_loop_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    if (loop == EVENT_LOOP_INVALID) _exit(2);
    evl_concurrent_destroy_arg_t a1 = {.h = loop};
    evl_concurrent_destroy_arg_t a2 = {.h = loop};
    pthread_t t1, t2;
    /* Checked via if-guards, not REQUIRE_EQ: this runs inside the forked
     * child above (pid == 0); see cq_select_waiter_thread's own identical
     * fix/comment earlier in this file for why REQUIRE_EQ's early-return
     * failure path is unsafe here. Joining a pthread_t that pthread_create
     * never actually initialized is undefined behavior (a hang, a crash,
     * or, worse, a stray SIGABRT from something unrelated that would make
     * this test appear to pass for the wrong reason); exit distinctly
     * instead so a create failure is reported as itself, not conflated
     * with the fatal_err() this test actually expects. */
    if (pthread_create(&t1, NULL, evl_concurrent_destroy_thread, &a1) != 0)
      _exit(3);
    if (pthread_create(&t2, NULL, evl_concurrent_destroy_thread, &a2) != 0)
      _exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 20000);
  REQUIRE_TRUE(reaped);
  if (!reaped) return;
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  event_loop h;
  int sleep_ms;
  bool resolved;
} evl_pin_sleep_arg_t;

static void *evl_pin_sleep_thread(void *arg) {
  evl_pin_sleep_arg_t *a = (evl_pin_sleep_arg_t *)arg;
  a->resolved = _event_loop_resolve_pin_and_sleep_for_tests(a->h, a->sleep_ms);
  return NULL;
}

/* The resolve-then-use race fix actually works: races a thread that resolves
 * and pins the handle for a deliberately long, directly-controlled duration
 * (via _event_loop_resolve_pin_and_sleep_for_tests, since, unlike
 * chttpcli's chttpclient_do against a slow endpoint, event_loop has no
 * naturally-occurring slow public entry point to borrow for this) against a
 * concurrent event_loop_destroy on the same handle. destroy must block until
 * the pin is released, not race ahead and free the loop out from under the
 * still-resolved pointer. */
TEST(event_loop_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  event_loop_construct(loop, 8, 1, 1);

  evl_pin_sleep_arg_t pin_arg = {.h = loop, .sleep_ms = 100, .resolved = false};
  pthread_t pin_thread;
  REQUIRE_EQ(pthread_create(&pin_thread, NULL, evl_pin_sleep_thread, &pin_arg),
             0);

  /* Give the pin thread a brief head start so its resolve (and therefore its
   * pin) has definitely already happened before destroy fires. */
  struct timespec startup = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
  nanosleep(&startup, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  event_loop_destroy(loop); /* must block until the pin thread's 100ms sleep
                                (still holding the pin) has fully elapsed */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(pin_thread, NULL);
  REQUIRE_TRUE(pin_arg.resolved);
  /* The pin thread slept ~100ms while pinned; destroy returning in well
   * under that would mean it did NOT actually wait for the pin, i.e. the
   * resolve-then-use protection failed. */
  REQUIRE_GT(elapsed_ms, 50L);
}

typedef struct {
  event_loop h;
} evl_reg_count_arg_t;

static void *evl_reg_count_thread(void *arg) {
  evl_reg_count_arg_t *a = (evl_reg_count_arg_t *)arg;
  /* Return value intentionally ignored: a legitimate race with a concurrent
   * destroy can make this resolve fail (returning (size_t)-1) instead of
   * succeeding; both outcomes are correct. This thread exists purely to
   * generate resolve/pin/unpin traffic concurrent with the destroy thread
   * below. */
  event_loop_reg_count(a->h);
  return NULL;
}

/* Distinct from resolve_then_use_race_destroy_waits above, and not
 * redundant with it: that test's long, deliberately-held pin guarantees
 * pending_resolve_count > 0 for the whole race window, so destroy's
 * poll-wait always finds it nonzero on its first check there; this test
 * needs the opposite shape: a fast, non-blocking entry point
 * (event_loop_reg_count: resolve, one atomic read, unpin, return; no
 * sleeping at all) raced against a concurrent destroy, repeated under
 * stress, since the failure window for a genuinely lock-free pin/unpin pair
 * is only a handful of instructions wide and will not reproduce reliably
 * under a single unstressed run. A fresh loop is used each iteration so
 * every repetition gets its own independent race rather than reusing one
 * already-destroyed handle. */
TEST(event_loop_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, EVENT_LOOP_INVALID);

    evl_reg_count_arg_t reg_count_arg = {.h = loop};
    evl_concurrent_destroy_arg_t destroy_arg = {.h = loop};
    pthread_t reg_count_tid, destroy_tid;
    REQUIRE_EQ(pthread_create(&reg_count_tid, NULL, evl_reg_count_thread,
                              &reg_count_arg),
               0);
    REQUIRE_EQ(pthread_create(&destroy_tid, NULL, evl_concurrent_destroy_thread,
                              &destroy_arg),
               0);
    pthread_join(reg_count_tid, NULL);
    pthread_join(destroy_tid, NULL);
  }
}

/* Legitimate slot reuse must never be confused with a stale handle to the
 * slot's previous occupant; the whole point of the generation counter. */
TEST(event_loop_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  event_loop a = event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(a, EVENT_LOOP_INVALID);
  event_loop stale_a = a;
  event_loop_destroy(a);

  event_loop b = event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(b, EVENT_LOOP_INVALID);

  /* B's operations must succeed normally regardless of whether the
   * allocator happened to reuse A's exact address for B. */
  REQUIRE_EQ(event_loop_reg_count(b), (size_t)0);

  /* A's stale handle must never resolve to B, even if it reused the same
   * underlying address; the whole point of the generation counter. */
  REQUIRE_EQ((void *)_event_loop_resolve_for_tests(stale_a), NULL);

  event_loop_destroy(b);
}

/* The slot table is bounded, not ever-growing: a create/destroy churn loop
 * with only a single slot ever in flight at a time must reuse that one
 * freed slot on every iteration rather than growing the table further.
 * Captures capacity right after the first create/destroy pair (rather than
 * asserting a fixed absolute value like 1) since other tests earlier in
 * this same process may have already grown the table to some N > 1; what
 * this test actually needs to prove is that ITS OWN churn adds no further
 * growth, not what the table's absolute size happens to be when it runs. */
TEST(event_loop_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  event_loop loop0 = event_loop_create(8, 1, 1, NULL);
  REQUIRE_NE(loop0, EVENT_LOOP_INVALID);
  event_loop_destroy(loop0);
  size_t capacity_after_first = _event_loop_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    event_loop loop = event_loop_create(8, 1, 1, NULL);
    REQUIRE_NE(loop, EVENT_LOOP_INVALID);
    event_loop_destroy(loop);
  }

  REQUIRE_EQ(_event_loop_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*                       FORK SAFETY (pthread_atfork)                         */
/* ========================================================================== */

/* This entire remainder of the file exercises src/cthreadcomm.c's own
 * pthread_atfork()-based fork() safety machinery (both event_loop's own and
 * the merged circular_queue/dynamic_queue mutex registry), which is itself
 * compiled out when FORK_SAFETY_REQUIRED is 0 (see that macro's own doc
 * comment in common.h); without that machinery these tests' own premises (a
 * forked child never inheriting a locked event_loop/queue mutex, and never
 * hitting the AB-BA lock-ordering hazard two independent registrations used
 * to have) no longer hold, so they are compiled out along with it rather
 * than left in to hang or fail. */
#if FORK_SAFETY_REQUIRED

static void fork_safety_hot_fd_on_readable(event_loop loop,
                                           ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)arg;
  char buf[64];
  while (read(sel->fd, buf, sizeof(buf)) > 0) {
  }
}

typedef struct {
  /* _Atomic, not plain volatile: volatile alone guarantees neither
   * atomicity nor any C11 memory-model ordering between the main test
   * thread's write (churn.stop = 1;) and this thread's read, only that
   * the compiler won't cache the read in a register. A genuine, if
   * low-impact, data race under the C11 memory model; every other
   * cross-thread handshake flag in this file already uses _Atomic for
   * exactly this reason. */
  _Atomic int stop;
} fork_safety_churn_arg_t;

/* Continuously creates and destroys throwaway event_loop instances,
 * completely unrelated to the loop the main test thread keeps busy below;
 * its only purpose is to keep SOME thread inside event_loop_slot_table's
 * own mutex (via event_loop_create/_destroy) as often as possible, racing
 * this test's own repeated fork() calls. */
static void *fork_safety_churn_thread(void *arg) {
  fork_safety_churn_arg_t *a = (fork_safety_churn_arg_t *)arg;
  cdebuglog_write("[DEBUG_TEST] pid=%d churn thread ENTER\n", (int)getpid());
  unsigned long churn_iters = 0;
  while (!atomic_load(&a->stop)) {
    char *err = NULL;
    /* Only every 200th iteration is logged, both before AND after: this
     * thread spins as fast as it can for the whole test, and logging every
     * single iteration would burn through the shared buffer's own
     * auto-flush threshold purely on churn-thread noise, crowding out the
     * far rarer, far more diagnostic [DEBUG_ATFORK]/[DEBUG_TEST] fork()
     * checkpoints from the main test thread. A before/after pair around a
     * skipped create+destroy cycle still bounds "was the churn thread
     * inside event_loop_create/_destroy (and therefore inside
     * event_loop_slot_table's own mutex) at the moment things stopped
     * progressing" to within 200 iterations either way. */
    bool log_this_iter = (churn_iters % 200) == 0;
    if (log_this_iter) {
      cdebuglog_write(
          "[DEBUG_TEST] pid=%d churn thread iter=%lu about to "
          "event_loop_create\n",
          (int)getpid(), churn_iters);
    }
    event_loop l = event_loop_create_with_mprocs(4, 1, 1, NULL, &err);
    if (l != EVENT_LOOP_INVALID) {
      int pfd[2];
      if (pipe(pfd) == 0) {
        event_handlers_t h = {0};
        event_reg r = event_loop_add(
            l, selectable_from_fd(pfd[0], ccol_select_read), h, NULL, NULL);
        (void)r;
        close(pfd[0]);
        close(pfd[1]);
      }
      if (log_this_iter) {
        cdebuglog_write(
            "[DEBUG_TEST] pid=%d churn thread iter=%lu about to "
            "event_loop_destroy\n",
            (int)getpid(), churn_iters);
      }
      event_loop_destroy(l);
    }
    if (log_this_iter) {
      cdebuglog_write("[DEBUG_TEST] pid=%d churn thread iter=%lu cycle done\n",
                      (int)getpid(), churn_iters);
    }
    churn_iters++;
  }
  cdebuglog_write("[DEBUG_TEST] pid=%d churn thread EXIT after %lu iters\n",
                  (int)getpid(), churn_iters);
  return NULL;
}

/* Regression test for a real, reproducible fork-safety hang (see
 * src/cthreadcomm.c's own _event_loop_atfork_prepare doc comment for the
 * full mechanism): fork() duplicates only the calling thread, so
 * event_loop_slot_table's own mutex and any live loop's own
 * shutdown_lock/reg_slot_rwlock/stripes[].lock could previously be
 * inherited by a child already locked, with no thread left alive in that
 * child that could ever unlock it.
 *
 * Recreates both demonstrated shapes of this hazard at once, in a single
 * bounded test: a churn thread continuously creating/destroying throwaway
 * event_loop instances (touching event_loop_slot_table's own mutex) races
 * repeated fork() calls of a process that ALSO keeps one single-stripe,
 * multi-threaded event_loop continuously busy dispatching a hot fd (so its
 * poller/dispatch threads are constantly acquiring and releasing that
 * loop's one and only stripe lock). Each forked child immediately tries
 * one more event_loop_add on the exact loop it just inherited, guarded by
 * alarm(3): before the fix, this reproduced a hung child in the large
 * majority of trials (a single stripe maximizes the odds fork() lands
 * mid-critical-section on it), which this test would otherwise never
 * finish at all rather than fail visibly. */
TEST(fork_safety, fork_does_not_inherit_a_locked_event_loop_mutex) {
  /* Unlike every OTHER hang-prone test in this file, the one genuinely
   * UNBOUNDED call here is fork() itself, on the MAIN test thread: a
   * forked child's own lifetime is bounded by alarm(3), and the parent's
   * own reap is bounded by _wait_for_forked_child_bounded()'s WNOHANG poll
   * loop, but nothing bounds the fork() syscall (and the atfork prepare()/
   * parent() handlers it runs synchronously before returning) if THAT is
   * where a lock-ordering hazard wedges. This outer alarm, armed for the
   * whole test body and disarmed just before normal completion, is what
   * catches that specific case: 30 trials * 10s worst-case reap bound each,
   * comfortably under this. */
  _arm_sigalrm_backtrace_handler();
  alarm(360);
  cdebuglog_write("[DEBUG_TEST] pid=%d fork_does_not_inherit... ENTER\n",
                  (int)getpid());
  fork_safety_churn_arg_t churn = {.stop = 0};
  pthread_t churn_tid;
  REQUIRE_EQ(pthread_create(&churn_tid, NULL, fork_safety_churn_thread, &churn),
             0);
  /* Registered as the SIGALRM handler's secondary backtrace target: if the
   * main thread's own fork() call wedges inside _cthreadcomm_atfork_prepare
   * waiting for a lock, the churn thread (the only other actor touching
   * event_loop_slot_table's own mutex/stripe locks in this process) is the
   * single most likely thread to be found holding it. */
  _register_hang_watch_waiter(churn_tid);
  cdebuglog_write("[DEBUG_TEST] pid=%d churn thread created tid=%lu\n",
                  (int)getpid(), (unsigned long)churn_tid);

  char *err = NULL;
  event_loop loop = event_loop_create_with_mprocs(16, 1, 3, NULL, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  int hotfd[2];
  REQUIRE_EQ(pipe(hotfd), 0);
  evl_set_nonblocking(hotfd[0]);
  event_handlers_t h = {.on_readable = fork_safety_hot_fd_on_readable,
                        .on_writable = NULL,
                        .on_error = NULL};
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(hotfd[0], ccol_select_read), h, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  enum { TRIALS = 30 };
  int hangs = 0;
  for (int i = 0; i < TRIALS; i++) {
    char byte = 'x';
    REQUIRE_EQ(write(hotfd[1], &byte, 1), 1);

    cdebuglog_write("[DEBUG_TEST] pid=%d trial=%d about to fork()\n",
                    (int)getpid(), i);
    pid_t pid = fork();
    cdebuglog_write("[DEBUG_TEST] pid=%d trial=%d fork() returned %d\n",
                    (int)getpid(), i, (int)pid);
    REQUIRE_NE(pid, -1);
    if (pid == 0) {
      /* Deliberately does NOT redirect STDOUT_FILENO/STDERR_FILENO to
       * /dev/null: cdebuglog_flush() and this handler's own backtrace dump
       * both write to STDERR_FILENO, and silencing it here would defeat the
       * entire purpose of chasing a hang with this instrumentation
       * active. */
      _arm_sigalrm_backtrace_handler();
      cdebuglog_write("[DEBUG_TEST] child pid=%d trial=%d alarm set\n",
                      (int)getpid(), i);
      /* Bounds this child's own lifetime in case the hazard this test
       * guards against somehow still fires, rather than hanging the whole
       * suite; the parent below distinguishes this from a clean exit via
       * WIFEXITED. */
      alarm(3);

      int fd2[2];
      if (pipe(fd2) != 0) _exit(2);
      evl_set_nonblocking(fd2[0]);
      event_handlers_t h2 = {0};
      event_reg r2 = event_loop_add(
          loop, selectable_from_fd(fd2[0], ccol_select_read), h2, NULL, NULL);
      int rc = r2 ? 0 : 1;
      if (r2 != EVENT_REG_INVALID) event_loop_remove(loop, r2);
      cdebuglog_write(
          "[DEBUG_TEST] child pid=%d trial=%d event_loop_add rc=%d, "
          "exiting\n",
          (int)getpid(), i, rc);
      _test_flush_and_exit(rc);
    }

    int status = 0;
    /* 10s: comfortably longer than the child's own alarm(3); see
     * _wait_for_forked_child_bounded's own doc comment for why the parent
     * needs its own bound here too, independent of the child's. */
    bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
    REQUIRE_TRUE(reaped);
    if (!reaped) return;
    if (!WIFEXITED(status)) hangs++;
    cdebuglog_write(
        "[DEBUG_TEST] pid=%d trial=%d reaped=%d WIFEXITED=%d hangs=%d\n",
        (int)getpid(), i, (int)reaped, WIFEXITED(status), hangs);
  }

  REQUIRE_EQ(hangs, 0);

  atomic_store(&churn.stop, 1);
  cdebuglog_write("[DEBUG_TEST] pid=%d about to join churn thread\n",
                  (int)getpid());
  pthread_join(churn_tid, NULL);
  cdebuglog_write("[DEBUG_TEST] pid=%d churn thread joined\n", (int)getpid());

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  close(hotfd[0]);
  close(hotfd[1]);

  alarm(0);
  cdebuglog_write("[DEBUG_TEST] pid=%d fork_does_not_inherit... EXIT\n",
                  (int)getpid());
}

typedef struct {
  circular_queue *cq;
  _Atomic bool locked;
  int hold_ms;
} queue_fork_lock_arg_t;

static void *queue_fork_lock_thread(void *arg) {
  queue_fork_lock_arg_t *a = (queue_fork_lock_arg_t *)arg;
  circq_test_lock_mutex_for_tests(a->cq);
  atomic_store(&a->locked, true);
  /* Releases on its own fixed schedule, entirely independent of anything
   * the forking thread does below: pthread_atfork's own prepare() handler
   * is what MUST block on this exact mutex until this thread releases it
   * (see _queue_atfork_prepare), so the forking thread must never be the
   * one signalling this thread to let go; that would make the two
   * threads wait on each other in a genuine cycle (fork() blocked in
   * prepare() waiting for this thread to unlock; this thread waiting for a
   * signal the forking thread can only send once fork() has returned), a
   * self-inflicted deadlock in the test itself, not anything to do with
   * the fix under test. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  circq_test_unlock_mutex_for_tests(a->cq);
  return NULL;
}

/* Regression test for the queue-mutex fork-safety gap fixed alongside this
 * test: circular_queue/dynamic_queue/channel's own internal mutex was never
 * walked by any pthread_atfork() handler, unlike every lock event_loop
 * itself owns (see fork_does_not_inherit_a_locked_event_loop_mutex above
 * and _queue_atfork_prepare's own doc comment in src/cthreadcomm.c). A
 * cq->mutex held by some thread OTHER than the one calling fork() at the
 * exact instant of fork() would previously be inherited by the child
 * already locked, with no thread left alive there to ever unlock it,
 * hanging every future operation on that same queue in the child.
 *
 * Unlike the event_loop regression test above (which relies on many
 * repeated fork() trials racing a real, naturally-short critical section),
 * this reproduces the hazard deterministically: circq_test_lock_mutex_for_
 * tests/circq_test_unlock_mutex_for_tests (RUNNING_UNIT_TESTS-only) let a
 * dedicated holder thread keep cq->mutex locked for a fixed, much-longer-
 * than-any-real-critical-section window (HOLD_MS) before releasing it on
 * its own schedule. The forking thread only calls fork() once it has
 * confirmed the lock is genuinely held; with the fix's atfork prepare()
 * handler in place, fork() itself must then BLOCK until the holder
 * releases (its own mutex_lock(cq->mutex) cannot return before then),
 * which is asserted below via a wall-clock lower bound on fork()'s own
 * duration, proving the fix's blocking behaviour actually engaged this
 * run, not merely that the race happened not to matter. */
TEST(fork_safety, fork_does_not_inherit_a_locked_circular_queue_mutex) {
  char *err = NULL;
  circular_queue *cq = circular_queue_create(4, &err);
  REQUIRE_NE((void *)cq, NULL);

  enum { HOLD_MS = 300 };
  queue_fork_lock_arg_t arg = {.cq = cq, .locked = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, queue_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* Wait for the holder thread to confirm it has acquired cq->mutex
     * before forking; a short, bounded spin (the holder thread does
     * nothing else before this store) natively, but a bare atomic-load
     * spin with no yield is not actually cheap under valgrind: memcheck
     * time-slices every thread through one single instrumented execution
     * engine rather than giving them true multi-core parallelism (see
     * tests/cthreadpool/tests.c's own ctp_fork_feeder_thread for the
     * identical finding), so this loop's own iteration count, however
     * cheap each one is natively, still made this test's own `make
     * memtest` run take tens of seconds to multiple minutes rather than a
     * fraction of a second. sched_yield() caps this thread's own
     * achievable spin rate to whatever the scheduler's own time-slice
     * granularity allows, letting the holder thread actually get
     * scheduled promptly instead of being starved by this thread
     * continuously re-winning the single instrumented engine's turn. */
    sched_yield();
  }

  /* The child reports its own result over a pipe rather than via its own
   * process exit status: under make memtest, a forked child's own
   * WEXITSTATUS as observed by the parent's waitpid() is not reliably the
   * value the child itself passed to _exit(): valgrind's own
   * --errors-for-leak-kinds=all/--error-exitcode machinery can override it
   * based on whatever it finds "reachable" in the child's own inherited
   * process image at exit time, unrelated to this test's own logic
   * (confirmed directly: a debug build proved the child's own rv was
   * genuinely ccol_success on every run, yet WEXITSTATUS still
   * intermittently came back non-zero under valgrind). This mirrors
   * clogger.c's own documented, independently-confirmed precedent for the
   * identical mechanism (see its fork_safety section's
   * child_can_log_after_fork history); only WIFEXITED is asserted below
   * for the same reason: it still reliably distinguishes a genuine
   * regression (re-hanging past alarm(3), or a real ccol_assert()/
   * fatal_err() abort raising SIGABRT) from a clean exit, unlike
   * WEXITSTATUS. */
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` does not exist here (fork() duplicates only the calling
     * thread). This child process could only come into existence once the
     * parent's own fork() call returned, which, per the fix, requires
     * _queue_atfork_prepare's own mutex_lock(cq->mutex) to have already
     * succeeded, i.e. the (vanished, in this process) holder thread must
     * have already released it. cq->mutex is therefore unlocked here; this
     * call returns immediately. Without the fix, cq->mutex was never
     * touched by any atfork handler at all, so fork() would have returned
     * near-instantly regardless of the still-live parent-side holder
     * thread, handing this child a mutex snapshot that was still genuinely
     * locked, hanging this exact call until alarm(3) kills the child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    c_message_t msg = {.data = NULL, .size = 0};
    ccol_retval_t rv = circq_try_send_zc(cq, &msg);
    char byte = (rv == ccol_success) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  close(result_pipe[1]);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000LL;
  /* Proves the fix's own blocking behaviour actually engaged: fork() must
   * have waited for close to the holder's own HOLD_MS before returning,
   * not returned near-instantly while the lock was still genuinely held. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));

  /* Bounded, not a bare blocking read(): see _read_result_byte_bounded's own
   * doc comment for why a plain read() here cannot be trusted to return
   * even on a killed/hung child. 10s matches this test's own parent-side
   * _wait_for_forked_child_bounded call below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s: comfortably longer than the child's own alarm(3); see
   * _wait_for_forked_child_bounded's own doc comment for why the parent
   * needs its own bound here too, independent of the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  pthread_join(holder, NULL);
  circular_queue_destroy(cq);
}

typedef struct {
  event_loop loop;
  _Atomic bool locked;
  int hold_ms;
} reg_slot_fork_lock_arg_t;

static void *reg_slot_fork_lock_thread(void *arg) {
  reg_slot_fork_lock_arg_t *a = (reg_slot_fork_lock_arg_t *)arg;
  /* The returned opaque pointer, not a->loop itself, must be handed to the
   * matching unlock call below; see event_loop_test_wrlock_reg_slot_for_
   * tests's own doc comment for why re-resolving a->loop a second time
   * from this thread, at unlock time, is a real deadlock hazard against a
   * concurrent fork(). */
  void *resolved = event_loop_test_wrlock_reg_slot_for_tests(a->loop);
  atomic_store(&a->locked, true);
  /* Releases on its own fixed schedule, entirely independent of anything
   * the forking thread does below; see queue_fork_lock_thread's own
   * identical comment for why (the same reasoning applies verbatim, this
   * time against _cthreadcomm_atfork_prepare's Phase 1 reg_slot_rwlock
   * write-lock instead of _queue_atfork_prepare's cq->mutex lock). */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  event_loop_test_wrunlock_reg_slot_for_tests(resolved);
  return NULL;
}

/* Regression test for the reg_slot_rwlock TID-tracked write-lock hazard
 * described in _cthreadcomm_atfork_release_impl's own comment on its
 * in_child branch for reg_slot_rwlock: converting reg_slot_mutex to a
 * rw_lock_t (so concurrent _event_reg_resolve calls, event_loop's own
 * hottest path under a per-request pause/resume caller like chttpserver,
 * no longer serialize behind one lock) means the write side can now be
 * acquired by any thread calling event_loop_add/_remove, not necessarily
 * the thread that later calls fork(); glibc's rwlock write-lock tracks
 * ownership by TID, so a plain rw_lock_unlock from the child's own
 * differently-TID'd surviving thread would silently fail to release a lock
 * a different, now-vanished thread actually locked, hanging every
 * subsequent resolve in that child. Mirrors fork_does_not_inherit_a_
 * locked_circular_queue_mutex's own structure exactly, substituting
 * event_loop's reg_slot_rwlock for circular_queue's cq->mutex. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_reg_slot_rwlock) {
  char *err = NULL;
  event_loop loop = event_loop_create_with_mprocs(4, 1, 1, NULL, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  event_handlers_t h = {.on_readable = fork_safety_hot_fd_on_readable,
                        .on_writable = NULL,
                        .on_error = NULL};
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), h, NULL, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  enum { HOLD_MS = 300 };
  reg_slot_fork_lock_arg_t arg = {
      .loop = loop, .locked = false, .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL, reg_slot_fork_lock_thread, &arg), 0);

  while (!atomic_load(&arg.locked)) {
    /* See fork_does_not_inherit_a_locked_circular_queue_mutex's own
     * identical spin-wait comment for why sched_yield(), not a bare spin,
     * matters under valgrind. */
    sched_yield();
  }

  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    /* `holder` does not exist here (fork() duplicates only the calling
     * thread). This child could only come into existence once the parent's
     * own fork() call returned, which requires _cthreadcomm_atfork_
     * prepare's own rw_lock_wrlock(loop->reg_slot_rwlock) to have already
     * succeeded, i.e. the (vanished, in this process) holder thread must
     * have already released it. Without the fix (a plain rw_lock_unlock
     * in the child instead of a reinit), this process would inherit
     * reg_slot_rwlock in a write-locked state with no thread that could
     * ever release it, hanging the resolve inside event_loop_reg_
     * generation below until alarm(3) kills this child. */
    close(result_pipe[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(3);
    uint64_t gen = event_loop_reg_generation(loop, reg);
    char byte = (gen != 0) ? 1 : 0;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }
  close(result_pipe[1]);

  clock_gettime(CLOCK_MONOTONIC, &t1);
  long long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000LL + (t1.tv_nsec - t0.tv_nsec) / 1000000LL;
  /* Proves the fix's own blocking behaviour actually engaged: fork() must
   * have waited for close to the holder's own HOLD_MS before returning. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));

  /* Bounded, not a bare blocking read(): see _read_result_byte_bounded's own
   * doc comment for why a plain read() here cannot be trusted to return
   * even on a killed/hung child. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  int status = 0;
  /* 10s: comfortably longer than the child's own alarm(3); see
   * _wait_for_forked_child_bounded's own doc comment for why the parent
   * needs its own bound here too, independent of the child's. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  pthread_join(holder, NULL);

  /* The parent's own reg_slot_rwlock must still be genuinely usable after
   * all of the above: a plain rw_lock_unlock (the parent's own release
   * path, unlike the child's reinit) on a lock this same thread's fork()
   * call validly released is exactly what is expected to work. */
  uint64_t gen_after = event_loop_reg_generation(loop, reg);
  REQUIRE_NE(gen_after, (uint64_t)0);

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  close(pfd[0]);
  close(pfd[1]);
}

typedef struct {
  _Atomic bool locked;
  int hold_ms;
} event_loop_slot_table_fork_lock_arg_t;

static void *event_loop_slot_table_fork_lock_thread(void *arg) {
  event_loop_slot_table_fork_lock_arg_t *a =
      (event_loop_slot_table_fork_lock_arg_t *)arg;
  event_loop_test_wrlock_slot_table_for_tests();
  atomic_store(&a->locked, true);
  /* Releases on its own fixed schedule, entirely independent of anything
   * the forking thread does below; see queue_fork_lock_thread's own
   * identical reasoning elsewhere in this file. */
  struct timespec ts = {.tv_sec = a->hold_ms / 1000,
                        .tv_nsec = (long)(a->hold_ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
  event_loop_test_wrunlock_slot_table_for_tests();
  return NULL;
}

/* Regression test for the event_loop_slot_table.rwlock TID-tracked
 * write-lock hazard described in _cthreadcomm_atfork_release_impl's own
 * comment on its in_child branch for event_loop_slot_table.rwlock: this is
 * the process-wide LOOP table (distinct from a single loop's own
 * reg_slot_rwlock, covered by fork_does_not_inherit_a_write_locked_reg_
 * slot_rwlock above), acquired by every event_loop_create/_destroy call
 * and also, on its read side, by the very first thing event_loop_pause/
 * _resume/_modify/_remove/event_loop_reg_generation do. Its write side can
 * be acquired by any thread calling event_loop_create/_destroy, not
 * necessarily the thread that later calls fork(); glibc's rwlock
 * write-lock tracks ownership by TID, so a plain rw_lock_unlock from the
 * child's own differently-TID'd surviving thread would silently fail to
 * release a lock a different, now-vanished thread actually locked, hanging
 * every subsequent _event_loop_resolve (and therefore every event_loop_
 * create/_destroy/_add/_remove/_pause/_resume/_modify call) in that
 * child. */
TEST(fork_safety, fork_does_not_inherit_a_write_locked_event_loop_slot_table) {
  enum { HOLD_MS = 300 };
  event_loop_slot_table_fork_lock_arg_t arg = {.locked = false,
                                               .hold_ms = HOLD_MS};
  pthread_t holder;
  REQUIRE_EQ(pthread_create(&holder, NULL,
                            event_loop_slot_table_fork_lock_thread, &arg),
             0);

  while (!atomic_load(&arg.locked)) {
    /* See fork_does_not_inherit_a_locked_circular_queue_mutex's own
     * identical spin-wait reasoning for why sched_yield(), not a bare
     * spin, matters under valgrind. */
    sched_yield();
  }

  /* Every fallible step from here on runs with holder already alive and
   * holding event_loop_slot_table.rwlock, so NONE of them may be asserted
   * on directly: a REQUIRE_* failure returns from this test function
   * immediately, and holder's own lifetime does not depend on any of
   * pipe()/fork()/the timing check/the child's response ever succeeding.
   * An earlier version of this test asserted on pipe(), fork(), and the
   * elapsed_ms check directly, each capable of leaking holder un-joined on
   * its own failure path (missed in an earlier pass that only fixed the
   * LATER assertions, below the read/reap steps -- caught on review, not
   * by inspection). Every outcome is therefore captured into a local
   * below, unconditionally, and the corresponding REQUIRE_* only runs
   * after pthread_join(holder, NULL) -- and, when a child was actually
   * forked, after _wait_for_forked_child_bounded too, for the identical
   * reason (see that call's own comment: this is the one test in this
   * file where skipping _dump_stuck_child_diagnostics on a genuine hang
   * would matter most). */
  int result_pipe[2] = {-1, -1};
  bool pipe_ok = (pipe(result_pipe) == 0);

  pid_t pid = -1;
  long long elapsed_ms = -1;
  char byte = 0;
  ssize_t n = -1;
  bool reaped = false;
  int status = 0;

  if (pipe_ok) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    pid = fork();
    if (pid == 0) {
      /* `holder` does not exist here (fork() duplicates only the calling
       * thread). This child could only come into existence once the
       * parent's own fork() call returned, which requires
       * _cthreadcomm_atfork_prepare's own
       * rw_lock_wrlock(event_loop_slot_table.rwlock) to have already
       * succeeded, i.e. the (vanished, in this process) holder thread
       * must have already released it. Without the fix (a plain
       * rw_lock_unlock in the child instead of a reinit), this process
       * would inherit event_loop_slot_table.rwlock in a write-locked
       * state with no thread that could ever release it, hanging the
       * resolve inside event_loop_create below until alarm(3) kills this
       * child. */
      close(result_pipe[0]);
      /* Deliberately does NOT redirect STDOUT_FILENO/STDERR_FILENO to
       * /dev/null: cdebuglog_flush() writes to STDERR_FILENO, and
       * silencing it here would defeat the entire purpose of the
       * checkpoints below -- this is precisely the test whose child went
       * silent on a real CI run (a qemu-arm and an aarch64-clang job both
       * failed here) with no checkpoint of any kind to show how far it
       * got, unlike every sibling fork_safety test in this file. */
      _arm_sigalrm_backtrace_handler();
      alarm(3);
      cdebuglog_write(
          "[DEBUG_TEST] child pid=%d alarm set, about to "
          "event_loop_create\n",
          (int)getpid());
      char *err = NULL;
      event_loop l = event_loop_create(4, 1, 1, &err);
      cdebuglog_write(
          "[DEBUG_TEST] child pid=%d event_loop_create returned %s\n",
          (int)getpid(), (l != EVENT_LOOP_INVALID) ? "valid" : "invalid");
      char b = (l != EVENT_LOOP_INVALID) ? 1 : 0;
      if (l != EVENT_LOOP_INVALID) event_loop_destroy(l);
      cdebuglog_write("[DEBUG_TEST] child pid=%d writing result, exiting\n",
                      (int)getpid());
      test_write_retry_eintr(result_pipe[1], &b, 1);
      close(result_pipe[1]);
      _test_flush_and_exit(0);
    }

    if (pid > 0) {
      close(result_pipe[1]);
      clock_gettime(CLOCK_MONOTONIC, &t1);
      elapsed_ms = (t1.tv_sec - t0.tv_sec) * 1000LL +
                   (t1.tv_nsec - t0.tv_nsec) / 1000000LL;

      /* Bounded, not a bare blocking read(): see _read_result_byte_bounded's
       * own doc comment for why a plain read() here cannot be trusted to
       * return even on a killed/hung child; this is precisely the test
       * where that exact gap was confirmed to hang an entire CI job (a
       * qemu-arm run went silent right after this fork()'s own
       * child-side atfork release completed, with neither process ever
       * heard from again until the job's own external timeout fired). */
      n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
      close(result_pipe[0]);

      /* Bounded, not a bare blocking waitpid(): matches every sibling
       * fork_safety test's own established convention (see
       * _wait_for_forked_child_bounded's own doc comment for why an
       * unbounded parent-side wait is never used here, even though the
       * child's own alarm(3) above already bounds the ordinary case). */
      reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
    } else {
      /* fork() itself failed: nothing was ever forked, so there is no
       * child to reap, only this process's own two pipe fds to close. */
      close(result_pipe[0]);
      close(result_pipe[1]);
    }
  }

  /* Joined unconditionally, after every step above, regardless of which
   * (if any) of them failed: holder's own lifetime never depended on any
   * of pipe()/fork()/the child's response succeeding. */
  pthread_join(holder, NULL);

  REQUIRE_TRUE(pipe_ok);
  REQUIRE_NE(pid, -1);
  /* Proves the fix's own blocking behaviour actually engaged: fork() must
   * have waited for close to the holder's own HOLD_MS before returning. */
  REQUIRE_GE(elapsed_ms, (long long)(HOLD_MS / 2));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
  REQUIRE_TRUE(reaped);
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));

  /* The parent's own event_loop_slot_table.rwlock must still be genuinely
   * usable after all of the above: a plain rw_lock_unlock (the parent's
   * own release path, unlike the child's reinit) on a lock this same
   * thread's fork() call validly released is exactly what is expected to
   * work. */
  char *err2 = NULL;
  event_loop l2 = event_loop_create(4, 1, 1, &err2);
  REQUIRE_NE(l2, EVENT_LOOP_INVALID);
  event_loop_destroy(l2);
}

static void fork_safety2_noop_readable(event_loop loop, ccol_selectable *sel,
                                       void *arg) {
  (void)loop;
  (void)sel;
  (void)arg;
}

typedef struct {
  circular_queue *cq;
  _Atomic bool started;
} fork_safety2_sender_arg_t;

static void *fork_safety2_sender_thread(void *arg) {
  fork_safety2_sender_arg_t *a = (fork_safety2_sender_arg_t *)arg;
  atomic_store(&a->started, true);
  c_message_t msg = {.data = NULL, .size = 0};
  circq_send_zc(a->cq, &msg);
  return NULL;
}

/* Regression test for a real, reproduced AB-BA deadlock in an EARLIER
 * version of the queue-mutex fork-safety fix above: that version registered
 * its own queue-mutex atfork handling via a SEPARATE, independent
 * at_fork() call from event_loop's own pre-existing one. pthread_atfork's
 * prepare handlers run in REVERSE registration order, so which of two
 * independent handler sets runs first at fork() time is purely an accident
 * of which subsystem (a circular_queue, or an event_loop) happens to be
 * used first in a given process; with a queue created before the first
 * event_loop (registering the queue's own atfork triple first, so
 * event_loop's own prepare (registered second) ran FIRST at fork()
 * time), event_loop's prepare locked a queue-backed registration's own
 * wait_mtx, and the queue registry's own (separate) prepare then tried to
 * lock that SAME queue's cq->mutex: the reverse of the order
 * _notify_waiter (called from any ordinary circq_send_zc/recv_zc from a
 * concurrently running, unrelated thread) always uses. Confirmed via a
 * standalone reproduction, gdb-verified: the forking thread deadlocked
 * inside fork() itself (blocked locking cq->mutex from what was then a
 * second, independent atfork prepare), while a concurrent sender thread
 * was simultaneously blocked locking wait_mtx from inside _notify_waiter,
 * a textbook cycle. Fixed by merging both subsystems' locking into ONE
 * at_fork() registration (_cthreadcomm_atfork_prepare/_release/
 * _child_release; see that function's own three-phase design comment in
 * src/cthreadcomm.c), so there is no "which of two independent handler
 * sets happens to run first" accident left to depend on.
 *
 * This test forces exactly the dangerous construction order (a circular_
 * queue created before the first event_loop in a fresh process) and uses
 * _notify_waiter_test_set_delay_us (RUNNING_UNIT_TESTS-only) to widen the
 * real, otherwise only a handful of instructions long, window inside
 * _notify_waiter between "cq->mutex already held" and "about to lock
 * wait_mtx" to a duration a concurrent fork() call reliably lands inside,
 * rather than relying on timing luck against a window this narrow.
 *
 * The entire scenario runs inside its own forked, alarm-bounded child
 * process (mirroring this file's own established "isolate a risky
 * operation, bound it from the parent via alarm+waitpid" precedent used
 * elsewhere for fatal_err()-triggering tests): unlike every OTHER
 * fork_safety test in this file, a regression here deadlocks the FORKING
 * THREAD ITSELF, inside fork()'s own prepare() handler, before fork() ever
 * returns to userspace, not merely a spawned child, which a plain
 * alarm(3) inside that child could bound on its own. Isolating the whole
 * scenario in its own child process, bounded by ITS OWN alarm(30), keeps a
 * regression from ever hanging the outer test suite itself.
 *
 * Reports success over a pipe rather than via the child's own exit status,
 * for the identical, independently-confirmed reason
 * fork_does_not_inherit_a_locked_circular_queue_mutex's own doc comment
 * above already documents (a forked child's WEXITSTATUS as observed by
 * waitpid() is not reliably what the child itself passed to _exit() under
 * make memtest's own --errors-for-leak-kinds=all/--error-exitcode
 * machinery); only WIFEXITED is meaningful and even that is only used to
 * reap the process here, since a hung child's own alarm(30) makes it exit
 * via SIGALRM (WIFSIGNALED), and the pipe read (bounded by that same
 * alarm, since the parent already closed its own write-end copy) is what
 * actually distinguishes success from a reproduced regression. */
TEST(fork_safety,
     fork_does_not_deadlock_with_queue_registered_before_event_loop) {
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  pid_t outer_pid = fork();
  REQUIRE_NE(outer_pid, -1);
  if (outer_pid == 0) {
    close(result_pipe[0]);
    /* A generous bound, not a tight one: this widened window (matching this
     * project's own established precedent for valgrind-specific timing
     * margin, e.g. chttpclient's async tests) must comfortably absorb
     * valgrind's own real, substantial per-fork() overhead under a fully
     * loaded test run (168 other tests' worth of accumulated heap/shadow-
     * memory state ahead of this one), confirmed to occasionally approach
     * several real seconds on its own with no bug involved at all; a
     * regression this test exists to catch is expected to hang
     * indefinitely regardless, so widening this costs nothing but a
     * slower failure report on an actual regression. */
    alarm(180);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }

    char byte = 0; /* 0 = failure/regression, unless proven otherwise below */

    /* Forces the dangerous construction order: this is the first-ever
     * circular_queue/event_loop use in this freshly forked process, so
     * creating the queue before the loop deterministically makes the
     * queue-mutex registry's own lazy init (and, pre-fix, its own separate
     * at_fork() registration) happen first. */
    char *err = NULL;
    circular_queue *cq = circular_queue_create(4, &err);
    event_loop loop =
        cq ? event_loop_create(16, 1, 1, &err) : EVENT_LOOP_INVALID;
    event_reg reg = EVENT_REG_INVALID;
    if (cq && loop) {
      event_handlers_t h = {.on_readable = fork_safety2_noop_readable};
      reg = event_loop_add(loop, selectable_from_circq(cq, ccol_select_read), h,
                           NULL, &err);
    }

    if (cq && loop && reg) {
      _notify_waiter_test_set_delay_us(300000);

      fork_safety2_sender_arg_t sarg = {.cq = cq, .started = false};
      pthread_t sender;
      /* Checked, unlike a bare fire-and-forget call: this runs inside the
       * forked child above (outer_pid == 0), where a failed create cannot
       * be reported via REQUIRE_EQ the way every sibling fork_safety test's
       * own identical spin-wait setup does (their own pthread_create is
       * called by the PARENT, before ever forking, so REQUIRE_EQ's failure
       * path -- an early `return` out of this Tau test function -- lands
       * safely back in the harness that called it; here, that same early
       * `return` would instead return out of THIS test function while
       * still running as the forked child, skipping the _exit(0) below and
       * falling into the harness's own subsequent test-running loop a
       * second time, in a process that was only ever supposed to run this
       * one child-side branch and exit). A failed create is therefore
       * handled exactly like a failed cq/loop/reg creation just above:
       * skip the risky section entirely and fall through with byte still
       * 0, reporting this run as a failure via the existing pipe protocol
       * rather than ever entering the wait loop below with no thread
       * created to ever satisfy it. Confirmed, via reproduction under a
       * deliberately tightened process/thread ulimit, that an unchecked
       * failure here left sarg.started permanently false, spinning the
       * sched_yield() loop below forever with nothing left to log: the
       * exact hang this file's own instrumentation was added to chase. */
      if (pthread_create(&sender, NULL, fork_safety2_sender_thread, &sarg) ==
          0) {
        while (!atomic_load(&sarg.started)) {
          /* Wait for the sender to confirm it is about to call
           * circq_send_zc, before forking below. A bare atomic-load spin
           * with no yield is not actually cheap under valgrind: memcheck
           * time-slices every thread through one single instrumented
           * execution engine rather than giving them true multi-core
           * parallelism (see fork_does_not_inherit_a_locked_circular_
           * queue_mutex's own identical fix above, and tests/cthreadpool/
           * tests.c's own ctp_fork_feeder_thread), so this loop's own
           * iteration count, however cheap each one is natively, made this
           * test's own `make memtest` run take well over a minute (up to,
           * and sometimes past, this test's own 30-second alarm) rather
           * than a fraction of a second. sched_yield() caps this thread's
           * own achievable spin rate to whatever the scheduler's own
           * time-slice granularity allows, letting the sender thread
           * actually get scheduled promptly instead of being starved by
           * this thread continuously re-winning the single instrumented
           * engine's turn. */
          sched_yield();
        }
        /* Give the sender a moment to acquire cq->mutex and enter the
         * widened _notify_waiter delay before the risky fork() call. */
        usleep(50000);

        pid_t inner_pid = fork();
        if (inner_pid == 0) {
          _exit(0);
        } else if (inner_pid > 0) {
          /* Reaching here at all (rather than hanging until alarm(30)
           * above kills this process) is the actual thing under test.
           * Bounded, not a bare blocking waitpid(): _wait_for_forked_child_
           * bounded has no Tau macro in it, so it is safe to call from
           * this already-forked child too, and doing so keeps this call
           * consistent with every other forked-child wait in this file
           * rather than being the one bare exception. */
          int inner_status = 0;
          (void)_wait_for_forked_child_bounded(inner_pid, &inner_status, 10000);
          byte = 1;
        }

        _notify_waiter_test_set_delay_us(0);
        pthread_join(sender, NULL);
      }
    }

    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);

    if (reg != EVENT_REG_INVALID) event_loop_remove(loop, reg);
    if (cq) {
      c_message_t drain = {0};
      circq_try_recv_zc(cq, &drain);
      circular_queue_destroy(cq);
    }
    if (loop != EVENT_LOOP_INVALID) event_loop_destroy(loop);
    _exit(0);
  }

  close(result_pipe[1]);
  /* Bounded, not a bare blocking read(): see _read_result_byte_bounded's own
   * doc comment (and fork_does_not_inherit_a_write_locked_event_loop_slot_
   * table's identical fix above) for why a plain read() here cannot be
   * trusted to return even on a killed/hung child. 200s matches this test's
   * own outer_pid bound below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 200000);
  close(result_pipe[0]);

  int status = 0;
  /* Bounded, not a bare blocking waitpid(): see fork_does_not_inherit_a_
   * write_locked_event_loop_slot_table's own identical fix/comment above
   * for why, even though the child's own alarm(180) already bounds the
   * ordinary case. */
  bool reaped = _wait_for_forked_child_bounded(outer_pid, &status, 200000);
  REQUIRE_TRUE(reaped);

  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);
}

/* Regression test closing a real, previously-untested gap: no test in this
 * suite ever actually called event_loop_shutdown/event_loop_destroy on an
 * event_loop INHERITED across fork() (every existing fork test either only
 * event_loop_add/_remove's on the inherited loop, or destroys a loop
 * created fresh inside the child itself). This is exactly the scenario
 * struct event_loop_s's own foreign_since_fork field exists to fix: fork()
 * duplicates only the calling thread, so a forked child's own
 * poller_thread field names a pthread_t this process never created and can
 * never join, and loop->shutdown_efd is a real, kernel-level object still
 * shared (not copied) with the parent's own genuinely-live poller thread.
 * Per this module's own history, an event_loop_destroy() of an inherited,
 * num_reactor_threads > 1 loop in a forked child reliably (5/5)
 * SIGSEGV'd inside glibc's own __pthread_clockjoin_ex before the
 * foreign_since_fork fixup existed, reached via ctpool_shutdown_drain's own
 * worker-thread join loop; that reproduction was, at the time, only ever a
 * standalone throwaway script, never a permanent regression test, leaving
 * this exact crash unguarded against a future regression (e.g. a change to
 * where the foreign_since_fork check sits relative to the self-call guard,
 * or to cthreadpool.c's own analogous fixup this mechanism depends on).
 *
 * Also verifies the other half of the fix: destroying the loop in the
 * child must not disturb the PARENT's still-live loop at all (in
 * particular, it must never write loop->shutdown_efd, a kernel object
 * shared across fork(), which would otherwise incorrectly wake the
 * parent's own poller thread); checked by confirming the parent's
 * identical, still-registered fd selectable keeps dispatching normally
 * after the child has fully torn its own copy down. */
TEST(fork_safety, event_loop_destroy_of_inherited_loop_in_child_is_safe) {
  int result_pipe[2];
  REQUIRE_EQ(pipe(result_pipe), 0);

  char *err = NULL;
  event_loop loop = event_loop_create_with_mprocs(8, 2, 3, NULL, &err);
  REQUIRE_NE(loop, EVENT_LOOP_INVALID);

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  evl_set_nonblocking(pfd[0]);
  evl_sync_ctx ctx;
  evl_sync_ctx_init(&ctx);
  event_handlers_t h = {
      .on_readable = evl_on_readable, .on_writable = NULL, .on_error = NULL};
  event_reg reg = event_loop_add(
      loop, selectable_from_fd(pfd[0], ccol_select_read), h, &ctx, &err);
  REQUIRE_NE(reg, EVENT_REG_INVALID);

  pid_t pid = fork();
  REQUIRE_NE(pid, -1);
  if (pid == 0) {
    close(result_pipe[0]);
    close(pfd[0]);
    close(pfd[1]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's own lifetime in case the hazard this test guards
     * against somehow still fires (a hang rather than a crash), rather
     * than hanging the whole suite; the parent below distinguishes this
     * from a clean exit via WIFEXITED, mirroring this file's own
     * established convention for these fork-safety tests. */
    alarm(5);

    /* The actual misuse under test: destroying the exact, fully inherited
     * loop handle, with its dispatch_pool workers and poller thread
     * existing in this process only as inert, copy-on-write memory. */
    event_loop_destroy(loop);

    char byte = 1;
    test_write_retry_eintr(result_pipe[1], &byte, 1);
    close(result_pipe[1]);
    _exit(0);
  }

  close(result_pipe[1]);
  /* Bounded, not a bare blocking read(): see _read_result_byte_bounded's own
   * doc comment for why a plain read() here cannot be trusted to return
   * even on a killed/hung child. 10s matches this test's own alarm(5)-plus-
   * margin bound below. */
  char byte = 0;
  ssize_t n = _read_result_byte_bounded(result_pipe[0], &byte, 10000);
  close(result_pipe[0]);

  int status = 0;
  /* 10s: comfortably longer than the child's own alarm(5); see
   * _wait_for_forked_child_bounded's own doc comment for why the parent
   * needs its own bound here too, independent of the child's. Not an
   * early-return on timeout, unlike this file's simpler fork-safety
   * tests: the parent's own loop/pfd/ctx below still need tearing down
   * regardless of whether the child was successfully reaped. */
  bool reaped = _wait_for_forked_child_bounded(pid, &status, 10000);
  REQUIRE_TRUE(reaped);

  /* Reaching a clean exit with the byte actually written is the real
   * assertion: a SIGSEGV (the historically-reproduced regression) makes
   * both fail (WIFSIGNALED, and the read() above returns 0 once the pipe's
   * only writer dies without ever writing). Only WIFEXITED is checked on
   * status itself, not WEXITSTATUS, mirroring this file's own established
   * precedent elsewhere (a forked child's WEXITSTATUS as observed by the
   * parent's waitpid() is not reliably what the child itself passed to
   * _exit() under make memtest's own --errors-for-leak-kinds=all/
   * --error-exitcode machinery; see e.g.
   * fork_does_not_inherit_a_locked_circular_queue_mutex's own comment). */
  if (reaped) REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ((int)n, 1);
  REQUIRE_EQ((int)byte, 1);

  /* The parent's OWN loop must still be fully alive and dispatching
   * normally: the child's destroy of its own inherited copy must not have
   * written the shared shutdown_efd (which would have woken the parent's
   * still-live poller) or otherwise disturbed the parent's kernel-level
   * epoll instance. */
  char val = 'x';
  REQUIRE_EQ(write(pfd[1], &val, 1), 1);
  REQUIRE_TRUE(evl_wait_for(&ctx, &ctx.readable_count, 1, 2000));

  event_loop_remove(loop, reg);
  event_loop_destroy(loop);
  evl_sync_ctx_destroy(&ctx);
  close(pfd[0]);
  close(pfd[1]);
}

#endif /* FORK_SAFETY_REQUIRED */
