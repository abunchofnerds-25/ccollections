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

/* This binary checks clogger's SIGPIPE policy. It runs with SIGPIPE at its
 * default disposition, which ends the process, and it must stay a binary
 * of its own because tests.c writes to a broken pipe, which sets SIGPIPE to
 * SIG_IGN for that whole process, and an ignored SIGPIPE can't be seen.
 *
 * The ignore that a pipe write installs under CLOG_SIGNALS_AUTO sticks for
 * the whole process and so does a policy change, so every scenario that can
 * make either change runs in a forked child that reports back over a pipe: 0
 * for a pass, a positive source line for the check that failed and a
 * negative marker for each phase it completed. The tests that run in this
 * process change neither, and each of them first asserts that SIGPIPE is at
 * its default disposition here. */

#include <clogger.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

__attribute__((constructor)) static void _setup(void) {
  signal(SIGPIPE, SIG_DFL);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
  test_reset_inherited_signals();
}

/* ========================================================================== */
/*                         HELPERS                                            */
/* ========================================================================== */

static bool _sigpipe_is(void (*handler)(int)) {
  struct sigaction sa;
  if (sigaction(SIGPIPE, NULL, &sa) != 0) return false;
  return !(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == handler;
}

static void _block_sigpipe(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, old);
}

/* Tells whether a SIGPIPE is pending, consumes every pending one (FreeBSD
 * queues one per failed write, Linux keeps just one) and restores the signal
 * mask. */
static bool _take_pending_sigpipe_and_restore(const sigset_t *old) {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  bool raised = sigismember(&pending, SIGPIPE) == 1;
  if (raised) test_consume_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, old, NULL);
  return raised;
}

/* A pipe whose reader is already gone. Returns the write end, or -1. */
static int _broken_pipe(void) {
  int fds[2];
  if (pipe(fds) != 0) return -1;
  close(fds[0]);
  return fds[1];
}

/* A stream socket whose peer is already gone. Returns the open end, or
 * -1. */
static int _broken_socket(void) {
  int fds[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return -1;
  close(fds[1]);
  return fds[0];
}

static const clog_async_cfg_t _async_cfg = {0};

/* ---- child side ---------------------------------------------------------- */

static int _result_fd = -1;

static void _child_send(int v) {
  ssize_t n;
  do {
    n = write(_result_fd, &v, sizeof(v));
  } while (n < 0 && errno == EINTR);
}

#define CHILD_CHECK(cond)    \
  do {                       \
    if (!(cond)) {           \
      _child_send(__LINE__); \
      test_child_exit(1);    \
    }                        \
  } while (0)

static void _child_pass(void) {
  _child_send(0);
  test_child_exit(0);
}

static void _child_mark(int phase) { _child_send(-phase); }

/* ---- parent side --------------------------------------------------------- */

typedef struct {
  int status;    /* from waitpid() */
  int values[8]; /* what the child sent, in order */
  int nvalues;
  bool forked;
} child_outcome_t;

/* Runs fn in a forked child, collects what it reports and reaps it. The child
 * arms an alarm, so a scenario that hangs dies by SIGALRM and this wait can't
 * last for ever. */
static void _run_child(void (*fn)(void), child_outcome_t *out) {
  memset(out, 0, sizeof(*out));
  int p[2];
  if (pipe(p) != 0) return;
  pid_t pid = fork();
  if (pid < 0) {
    close(p[0]);
    close(p[1]);
    return;
  }
  if (pid == 0) {
    test_child_no_core();
    close(p[0]);
    _result_fd = p[1];
    alarm(120);
    fn();
    _child_send(-1000); /* a scenario must end in _child_pass() */
    test_child_exit(1);
  }
  out->forked = true;
  close(p[1]);
  int v;
  for (;;) {
    ssize_t n = read(p[0], &v, sizeof(v));
    if (n < 0 && errno == EINTR) continue;
    if (n != (ssize_t)sizeof(v)) break;
    if (out->nvalues < 8) out->values[out->nvalues++] = v;
  }
  close(p[0]);
  while (waitpid(pid, &out->status, 0) < 0 && errno == EINTR) {
  }
}

/* A child passed when the last thing it reported is 0. We don't read its
 * exit status since valgrind can replace the exit status of a forked child. */
static bool _child_passed(const child_outcome_t *o) {
  return o->forked && o->nvalues > 0 && o->values[o->nvalues - 1] == 0;
}

static bool _child_died_of(const child_outcome_t *o, int sig) {
  return o->forked && WIFSIGNALED(o->status) && WTERMSIG(o->status) == sig;
}

static void _print_child(const char *name, const child_outcome_t *o) {
  if (_child_passed(o)) return;
  fprintf(stderr, "%s: forked=%d status=0x%x reports:", name, o->forked,
          o->status);
  for (int i = 0; i < o->nvalues; ++i) fprintf(stderr, " %d", o->values[i]);
  fprintf(stderr, "\n");
}

/* ========================================================================== */
/*                         IN THIS PROCESS                                    */
/* ========================================================================== */

TEST(sigpipe, regular_file_logger_leaves_the_disposition_alone) {
  REQUIRE_TRUE(_sigpipe_is(SIG_DFL));
  const char *path = "sigpipe_regular_file.log";
  unlink(path);

  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  clog alg = clog_create_from_file(path, CLOG_INFO, NULL, &_async_cfg);
  bool opened = lg != CLOG_NULL && alg != CLOG_NULL;
  if (lg != CLOG_NULL) clog_info(lg, "synchronous record");
  if (alg != CLOG_NULL) {
    clog_info(alg, "async record");
    clog_flush(alg);
    clog_close(alg);
  }
  if (lg != CLOG_NULL) clog_close(lg);
  bool still_default = _sigpipe_is(SIG_DFL);

  char buf[4096] = {0};
  int fd = open(path, O_RDONLY);
  ssize_t n = fd >= 0 ? read(fd, buf, sizeof(buf) - 1) : -1;
  if (fd >= 0) close(fd);
  unlink(path);

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(still_default);
  REQUIRE_GT(n, 0);
  REQUIRE_NE(strstr(buf, "synchronous record"), NULL);
  REQUIRE_NE(strstr(buf, "async record"), NULL);
}

/* A write to a socket whose peer is gone must use MSG_NOSIGNAL. We block
 * SIGPIPE on this thread around the log call, so a plain write(2) there
 * would leave one pending here instead of ending the process. */
TEST(sigpipe, socket_logger_with_a_gone_peer_raises_nothing) {
  REQUIRE_TRUE(_sigpipe_is(SIG_DFL));
  int fd = _broken_socket();
  REQUIRE_GE(fd, 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  bool opened = lg != CLOG_NULL;

  sigset_t old;
  _block_sigpipe(&old);
  if (opened) {
    clog_info(lg, "first record");
    clog_info(lg, "second record");
  }
  bool raised = _take_pending_sigpipe_and_restore(&old);

  if (opened) clog_close(lg);
  close(fd);

  REQUIRE_TRUE(opened);
  REQUIRE_FALSE(raised);
  REQUIRE_TRUE(_sigpipe_is(SIG_DFL));
}

TEST(sigpipe, an_invalid_policy_is_refused) {
  REQUIRE_EQ(clog_set_signal_policy((clog_signal_policy_t)2),
             ccol_invalid_args);
  REQUIRE_EQ(clog_set_signal_policy((clog_signal_policy_t)-1),
             ccol_invalid_args);
  REQUIRE_EQ(clog_set_signal_policy((clog_signal_policy_t)99),
             ccol_invalid_args);
  REQUIRE_EQ(clog_set_signal_policy(CLOG_SIGNALS_AUTO), ccol_success);
}

/* ========================================================================== */
/*                         IN A FORKED CHILD                                  */
/* ========================================================================== */

/* A synchronous logger on a pipe writes on the calling thread, so under
 * CLOG_SIGNALS_AUTO its first write sets SIG_IGN for the process. Opening the
 * logger alone must not do that. */
static void _scenario_sync_pipe_auto(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigpipe_is(SIG_DFL)); /* not at open */
  clog_info(lg, "to a pipe whose reader is gone");
  CHILD_CHECK(_sigpipe_is(SIG_IGN)); /* after the first write */
  clog_info(lg, "again");
  clog_close(lg);
  close(fd);
  _child_pass();
}

TEST(sigpipe, sync_pipe_logger_ignores_sigpipe_only_from_its_first_write) {
  child_outcome_t o;
  _run_child(_scenario_sync_pipe_auto, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* The async writer thread keeps SIGPIPE blocked in its own mask, so the
 * process's disposition stays the default one. */
static void _scenario_async_pipe_auto(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);
  for (int i = 0; i < 3; ++i) {
    clog_info(lg, "async record %d", i);
    clog_flush(lg);
  }
  clog_close(lg);
  close(fd);
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  _child_pass();
}

TEST(sigpipe, async_pipe_logger_leaves_the_disposition_at_default) {
  child_outcome_t o;
  _run_child(_scenario_async_pipe_auto, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* The writer thread takes back the SIGPIPE that its own failed write left
 * pending on it. In phase 1 it writes to a broken pipe with SIGPIPE blocked.
 * Phase 2 puts a healthy pipe under the same descriptor and switches to
 * CLOG_SIGNALS_UNTOUCHED, so the writer thread unblocks SIGPIPE before its
 * next write, and a SIGPIPE left pending from phase 1 would kill the process
 * there. Phase 3 puts a broken pipe back, and this time the write must raise
 * SIGPIPE with its default disposition. */
static void _scenario_writer_consumes_and_follows_policy(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);

  clog_info(lg, "phase 1");
  clog_flush(lg);
  _child_mark(1);

  int healthy[2];
  CHILD_CHECK(pipe(healthy) == 0);
  CHILD_CHECK(dup2(healthy[1], fd) == fd);
  close(healthy[1]);
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  clog_info(lg, "phase 2");
  clog_flush(lg);
  char buf[4096];
  ssize_t n = read(healthy[0], buf, sizeof(buf) - 1);
  CHILD_CHECK(n > 0);
  buf[n] = '\0';
  CHILD_CHECK(strstr(buf, "phase 2") != NULL);
  _child_mark(2);

  close(healthy[0]); /* the pipe under fd is broken again */
  clog_info(lg, "phase 3");
  clog_flush(lg); /* raises SIGPIPE on the writer thread */
  _child_mark(3);
  clog_close(lg);
  _child_pass();
}

TEST(sigpipe, writer_thread_consumes_its_sigpipe_and_follows_a_policy_change) {
  child_outcome_t o;
  _run_child(_scenario_writer_consumes_and_follows_policy, &o);
  bool ok = _child_died_of(&o, SIGPIPE) && o.nvalues == 2 &&
            o.values[0] == -1 && o.values[1] == -2;
  if (!ok) {
    fprintf(stderr, "%s: ", __func__);
    _print_child("outcome", &o);
    if (_child_passed(&o)) fprintf(stderr, "the child was not ended\n");
  }
  REQUIRE_TRUE(ok);
}

/* Under CLOG_SIGNALS_UNTOUCHED a synchronous write to a broken pipe raises
 * SIGPIPE on the calling thread and the disposition stays the default. */
static void _scenario_sync_pipe_untouched(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  sigset_t old;
  _block_sigpipe(&old);
  clog_info(lg, "to a pipe whose reader is gone");
  bool raised = _take_pending_sigpipe_and_restore(&old);
  CHILD_CHECK(raised);
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  clog_close(lg);
  close(fd);
  _child_pass();
}

TEST(sigpipe, untouched_policy_leaves_sigpipe_to_the_application_sync) {
  child_outcome_t o;
  _run_child(_scenario_sync_pipe_untouched, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* Under CLOG_SIGNALS_UNTOUCHED the async writer thread keeps the mask it
 * inherited, which doesn't block SIGPIPE here, so its write to a broken pipe
 * ends the process by the default disposition. */
static void _scenario_async_pipe_untouched(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);
  _child_mark(1);
  clog_info(lg, "to a pipe whose reader is gone");
  clog_flush(lg);
  _child_mark(2);
  clog_close(lg);
  _child_pass();
}

TEST(sigpipe, untouched_policy_leaves_sigpipe_to_the_application_async) {
  child_outcome_t o;
  _run_child(_scenario_async_pipe_untouched, &o);
  bool ok = _child_died_of(&o, SIGPIPE) && o.nvalues == 1 && o.values[0] == -1;
  if (!ok) _print_child(__func__, &o);
  REQUIRE_TRUE(ok);
}

/* MSG_NOSIGNAL changes no signal state, so it applies under
 * CLOG_SIGNALS_UNTOUCHED as well, on the calling thread and on the writer
 * thread alike. With the default disposition any SIGPIPE would end the
 * child. */
static void _scenario_socket_untouched(void) {
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  int sfd = _broken_socket();
  int afd = _broken_socket();
  CHILD_CHECK(sfd >= 0 && afd >= 0);
  clog lg = clog_create_from_fd(sfd, CLOG_INFO, NULL);
  clog alg = clog_create_from_fd(afd, CLOG_INFO, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL && alg != CLOG_NULL);
  clog_info(lg, "synchronous record");
  clog_info(alg, "async record");
  clog_flush(alg);
  clog_close(alg);
  clog_close(lg);
  close(sfd);
  close(afd);
  CHILD_CHECK(_sigpipe_is(SIG_DFL));
  _child_pass();
}

TEST(sigpipe, socket_logger_raises_nothing_under_the_untouched_policy) {
  child_outcome_t o;
  _run_child(_scenario_socket_untouched, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* A handler the application installed is the application's decision, so a
 * write from the calling thread under CLOG_SIGNALS_AUTO leaves it in place
 * and the handler sees the SIGPIPE. */
static volatile sig_atomic_t _handler_hits;
static void _count_sigpipe(int sig) {
  (void)sig;
  _handler_hits++;
}

static void _scenario_application_handler_stays(void) {
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _count_sigpipe;
  sigemptyset(&sa.sa_mask);
  CHILD_CHECK(sigaction(SIGPIPE, &sa, NULL) == 0);
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  clog_info(lg, "to a pipe whose reader is gone");
  CHILD_CHECK(_sigpipe_is(_count_sigpipe));
  /* The failed record raises one and so does the loss marker behind it. */
  CHILD_CHECK(_handler_hits >= 1);
  clog_close(lg);
  close(fd);
  _child_pass();
}

TEST(sigpipe, auto_policy_keeps_a_handler_of_the_application) {
  child_outcome_t o;
  _run_child(_scenario_application_handler_stays, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* A refused value changes nothing. CLOG_SIGNALS_AUTO stays in force, so the
 * first synchronous pipe write sets SIG_IGN as usual. */
static void _scenario_invalid_policy_changes_nothing(void) {
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_AUTO) == ccol_success);
  CHILD_CHECK(clog_set_signal_policy((clog_signal_policy_t)2) ==
              ccol_invalid_args);
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  clog_info(lg, "to a pipe whose reader is gone");
  CHILD_CHECK(_sigpipe_is(SIG_IGN));
  clog_close(lg);
  close(fd);
  _child_pass();
}

TEST(sigpipe, an_invalid_policy_leaves_the_current_one_in_force) {
  child_outcome_t o;
  _run_child(_scenario_invalid_policy_changes_nothing, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

/* CLOG_FATAL on an async logger writes its record on the calling thread,
 * which sets SIG_IGN under CLOG_SIGNALS_AUTO, so the process ends through
 * exit() and not through SIGPIPE. */
static void _scenario_fatal_on_async_pipe(void) {
  int fd = _broken_pipe();
  CHILD_CHECK(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);
  _child_mark(1);
  clog_fatal(lg, "fatal record to a pipe whose reader is gone");
  _child_mark(2); /* not reached: CLOG_FATAL ends the process */
  _child_pass();
}

TEST(sigpipe, fatal_record_on_an_async_pipe_logger_exits_normally) {
  child_outcome_t o;
  _run_child(_scenario_fatal_on_async_pipe, &o);
  bool ok =
      o.forked && WIFEXITED(o.status) && o.nvalues == 1 && o.values[0] == -1;
  if (!ok) _print_child(__func__, &o);
  REQUIRE_TRUE(ok);
}
