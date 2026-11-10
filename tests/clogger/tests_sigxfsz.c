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

// Checks what clogger does about SIGXFSZ, which a write to a regular file
// raises at RLIMIT_FSIZE, with SIGXFSZ at its default disposition. It's a
// binary of its own because tests.c ignores SIGXFSZ for its whole process. An
// open under AUTO, a policy change and a size limit are all process state, so
// every scenario runs in a forked child of a process that never opens a logger.
// The child reports over a pipe: 0 for a pass, a positive source line for a
// failed check, a negative marker per completed phase.
#include <clogger.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_coverage.h>
#include <test_signals.h>
#include <unistd.h>

TAU_MAIN()

// Puts SIGXFSZ back to its default disposition and unblocks it on the calling
// thread, since the binary inherits whatever its parent set.
static void _sigxfsz_default(void) {
  signal(SIGXFSZ, SIG_DFL);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGXFSZ);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

// Resets SIGXFSZ and the other inherited signal state before any test.
__attribute__((constructor)) static void _setup(void) {
  _sigxfsz_default();
  test_reset_inherited_signals();
}

// Tells whether SIGXFSZ's disposition is handler, so a test can see what the
// library changed.
static bool _sigxfsz_is(void (*handler)(int)) {
  struct sigaction sa;
  if (sigaction(SIGXFSZ, NULL, &sa) != 0) return false;
  return !(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == handler;
}

// Sets the soft file size limit to soft and keeps the hard one (RLIM_INFINITY
// lifts it to the hard limit), so a scenario can put a log at the limit.
// Returns whether it worked.
static bool _set_file_size_limit(rlim_t soft) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_FSIZE, &rl) != 0) return false;
  rl.rlim_cur = soft == RLIM_INFINITY ? rl.rlim_max : soft;
  return setrlimit(RLIMIT_FSIZE, &rl) == 0;
}

// Gives the size of path, or -1, so a scenario can set the limit relative to
// what the log holds.
static off_t _file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_size : -1;
}

// Reads at most cap - 1 bytes of path into buf, NUL-terminated, and gives the
// count, or -1.
static ssize_t _read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return -1;
  size_t got = 0;
  while (got + 1 < cap) {
    ssize_t n = read(fd, buf + got, cap - 1 - got);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) break;
    got += (size_t)n;
  }
  close(fd);
  buf[got] = '\0';
  return (ssize_t)got;
}

// Writes the log path of the scenario tag into buf. The file goes in the
// working directory, on the filesystem the tests normally use.
static void _log_path(char *buf, size_t cap, const char *tag) {
  snprintf(buf, cap, "sigxfsz_%s.log", tag);
  (void)unlink(buf);
}

// An all-default async config for the async scenarios.
static const clog_async_cfg _async_cfg = {0};

// The write end of the pipe a forked child reports through.
static int _result_fd = -1;

// Sends one report value v from a child to the parent.
static void _child_send(int v) {
  ssize_t n;
  do {
    n = write(_result_fd, &v, sizeof(v));
  } while (n < 0 && errno == EINTR);
}

// Checks cond in a child; on failure it reports the source line and ends the
// child, since a tau assertion can't run there.
#define CHILD_CHECK(cond)    \
  do {                       \
    if (!(cond)) {           \
      _child_send(__LINE__); \
      test_child_exit(1);    \
    }                        \
  } while (0)

// Reports a pass (0) and ends the child.
static void _child_pass(void) {
  _child_send(0);
  test_child_exit(0);
}

// Reports that the child completed phase, so a failure shows how far it got.
static void _child_mark(int phase) { _child_send(-phase); }

// Checks that the log at path ends with a newline and holds last, the record
// taken once the limit was lifted. With lost true (a synchronous logger) it
// also wants the loss marker for the refused records; an async logger keeps
// them and delivers them all once there is room.
static void _child_check_repaired(const char *path, const char *last,
                                  bool lost) {
  static char buf[1 << 16];
  ssize_t n = _read_file(path, buf, sizeof(buf));
  CHILD_CHECK(n > 0);
  CHILD_CHECK(buf[n - 1] == '\n');
  CHILD_CHECK(strstr(buf, last) != NULL);
  if (lost) {
    CHILD_CHECK(strstr(buf, "log record truncated") != NULL);
    return;
  }
  char rec[64];
  for (int i = 0; i < 5; ++i) {
    snprintf(rec, sizeof(rec), "record past the limit %d\"\n", i);
    CHILD_CHECK(strstr(buf, rec) != NULL);
  }
}

// What a forked scenario left: its wait status, the values it reported in
// order, and whether the fork happened.
typedef struct {
  int status;
  int values[8];
  int nvalues;
  bool forked;
} child_outcome_t;

// Runs scenario fn in a forked child, collects its reports into *out and reaps
// it. The child arms an alarm, so a scenario that hangs dies by SIGALRM and the
// wait ends.
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
    _sigxfsz_default();
    alarm(120);
    fn();
    _child_send(-1000);
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

// Tells whether a child passed: its last report is 0 and no signal killed it.
// The exit status isn't used, because valgrind can replace a forked child's
// status.
static bool _child_passed(const child_outcome_t *o) {
  return o->forked && !WIFSIGNALED(o->status) && o->nvalues > 0 &&
         o->values[o->nvalues - 1] == 0;
}

// Tells whether a child was killed by sig, for the scenarios where SIGXFSZ must
// end it.
static bool _child_died_of(const child_outcome_t *o, int sig) {
  return o->forked && WIFSIGNALED(o->status) && WTERMSIG(o->status) == sig;
}

// Prints a failed child's status and reports to stderr, so a failure says what
// happened.
static void _print_child(const char *name, const child_outcome_t *o) {
  if (_child_passed(o)) return;
  fprintf(stderr, "%s: forked=%d status=0x%x", name, o->forked, o->status);
  if (o->forked && WIFSIGNALED(o->status))
    fprintf(stderr, " (signal %d)", WTERMSIG(o->status));
  fprintf(stderr, " reports:");
  for (int i = 0; i < o->nvalues; ++i) fprintf(stderr, " %d", o->values[i]);
  fprintf(stderr, "\n");
}

// Scenario body: a synchronous file logger under AUTO, whose open sets SIGXFSZ
// to SIG_IGN. The limit lies extra bytes past the first record: 0 makes the
// next write start at the limit and fail at once, a few bytes make the kernel
// take part of a record first. Once the limit is lifted the logger closes the
// fragment and names the loss.
static void _sync_auto_body(off_t extra) {
  char path[128];
  _log_path(path, sizeof(path), "sync");
  CHILD_CHECK(_sigxfsz_is(SIG_DFL));
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_IGN));
  clog_info(lg, "first record");
  off_t size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)(size + extra)));
  _child_mark(1);
  for (int i = 0; i < 5; ++i) clog_info(lg, "record past the limit %d", i);
  _child_mark(2);
  CHILD_CHECK(_set_file_size_limit(RLIM_INFINITY));
  clog_info(lg, "record after the limit was lifted");
  clog_close(lg);
  _child_check_repaired(path, "record after the limit was lifted", true);
  (void)unlink(path);
  _child_pass();
}

// The two variants of the synchronous scenario: a short write, and a write that
// starts at the limit.
static void _scenario_sync_short_write(void) { _sync_auto_body(10); }
static void _scenario_sync_write_at_the_limit(void) { _sync_auto_body(0); }

TEST(sigxfsz, sync_file_logger_survives_a_short_write_at_the_limit) {
  child_outcome_t o;
  _run_child(_scenario_sync_short_write, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

TEST(sigxfsz, sync_file_logger_survives_a_write_that_starts_at_the_limit) {
  child_outcome_t o;
  _run_child(_scenario_sync_write_at_the_limit, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

// Scenario: the same for an async logger, whose writer thread hits the limit.
static void _scenario_async_auto(void) {
  char path[128];
  _log_path(path, sizeof(path), "async");
  CHILD_CHECK(_sigxfsz_is(SIG_DFL));
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_IGN));
  clog_info(lg, "first record");
  clog_flush(lg);
  off_t size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)(size + 10)));
  _child_mark(1);
  for (int i = 0; i < 5; ++i) {
    clog_info(lg, "record past the limit %d", i);
    clog_flush(lg);
  }
  _child_mark(2);
  CHILD_CHECK(_set_file_size_limit(RLIM_INFINITY));
  clog_info(lg, "record after the limit was lifted");
  clog_close(lg);
  _child_check_repaired(path, "record after the limit was lifted", false);
  (void)unlink(path);
  _child_pass();
}

TEST(sigxfsz, async_file_logger_survives_the_limit) {
  child_outcome_t o;
  _run_child(_scenario_async_auto, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

// Scenario: the writer thread takes back the SIGXFSZ its own failed write left
// pending. The logger opens under UNTOUCHED (default disposition) and the main
// thread blocks SIGXFSZ, so a process-wide signal (FreeBSD) can only reach the
// writer. Phase 1 switches to AUTO, so the writer blocks SIGXFSZ before its
// write at the limit; phase 2 lifts the limit and switches back, so a pending
// SIGXFSZ would kill the child at the next write; phase 3 puts the limit back
// and the write must kill the child with SIGXFSZ.
static void _scenario_writer_consumes_and_follows_policy(void) {
  char path[128];
  _log_path(path, sizeof(path), "consume");
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &_async_cfg);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_DFL));
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGXFSZ);
  CHILD_CHECK(pthread_sigmask(SIG_BLOCK, &set, NULL) == 0);

  clog_info(lg, "first record");
  clog_flush(lg);
  off_t size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)size));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_AUTO) == ccol_success);
  clog_info(lg, "phase 1");
  clog_flush(lg);
  _child_mark(1);

  CHILD_CHECK(_set_file_size_limit(RLIM_INFINITY));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  clog_info(lg, "phase 2");
  clog_flush(lg);
  static char buf[1 << 16];
  CHILD_CHECK(_read_file(path, buf, sizeof(buf)) > 0);
  CHILD_CHECK(strstr(buf, "phase 2") != NULL);
  _child_mark(2);

  size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)size));
  clog_info(lg, "phase 3");
  clog_flush(lg);
  _child_mark(3);
  clog_close(lg);
  _child_pass();
}

TEST(sigxfsz, writer_thread_consumes_its_sigxfsz_and_follows_a_policy_change) {
  child_outcome_t o;
  _run_child(_scenario_writer_consumes_and_follows_policy, &o);
  char path[128];
  _log_path(path, sizeof(path), "consume");
  bool ok = _child_died_of(&o, SIGXFSZ) && o.nvalues == 2 &&
            o.values[0] == -1 && o.values[1] == -2;
  if (!ok) {
    _print_child(__func__, &o);
    if (_child_passed(&o)) fprintf(stderr, "the child was not ended\n");
  }
  REQUIRE_TRUE(ok);
}

// Scenario: under UNTOUCHED opening a regular file changes nothing, so a write
// at the limit kills the child with SIGXFSZ. The file is removed before the
// limit, since the child dies there.
static void _scenario_untouched(void) {
  char path[128];
  _log_path(path, sizeof(path), "untouched");
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_DFL));
  clog_info(lg, "first record");
  off_t size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(unlink(path) == 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)size));
  _child_mark(1);
  clog_info(lg, "record at the limit");
  _child_mark(2);
  clog_close(lg);
  _child_pass();
}

TEST(sigxfsz, untouched_policy_leaves_sigxfsz_to_the_application) {
  child_outcome_t o;
  _run_child(_scenario_untouched, &o);
  bool ok = _child_died_of(&o, SIGXFSZ) && o.nvalues == 1 && o.values[0] == -1;
  if (!ok) {
    _print_child(__func__, &o);
    if (_child_passed(&o)) fprintf(stderr, "the child was not ended\n");
  }
  REQUIRE_TRUE(ok);
}

// SIGXFSZs the application's handler saw.
static volatile sig_atomic_t _handler_calls;

// The application's SIGXFSZ handler; counts each signal.
static void _count_sigxfsz(int sig) {
  (void)sig;
  _handler_calls = _handler_calls + 1;
}

// Scenario: a handler the application installed before the open stays, and it
// sees the SIGXFSZ of a write at the limit.
static void _scenario_application_handler_stays(void) {
  char path[128];
  _log_path(path, sizeof(path), "handler");
  struct sigaction sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _count_sigxfsz;
  sigemptyset(&sa.sa_mask);
  CHILD_CHECK(sigaction(SIGXFSZ, &sa, NULL) == 0);
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(_count_sigxfsz));
  clog_info(lg, "first record");
  off_t size = _file_size(path);
  CHILD_CHECK(size > 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)size));
  clog_info(lg, "record at the limit");
  CHILD_CHECK(_handler_calls > 0);
  CHILD_CHECK(_sigxfsz_is(_count_sigxfsz));
  CHILD_CHECK(_set_file_size_limit(RLIM_INFINITY));
  clog_info(lg, "record after the limit was lifted");
  clog_close(lg);
  _child_check_repaired(path, "record after the limit was lifted", true);
  (void)unlink(path);
  _child_pass();
}

TEST(sigxfsz, auto_policy_keeps_a_handler_of_the_application) {
  child_outcome_t o;
  _run_child(_scenario_application_handler_stays, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}

// Scenario: an application's ignore stays an ignore, and switching to UNTOUCHED
// doesn't undo the ignore that an open under AUTO set.
static void _scenario_ignores_stay(void) {
  char path[128];
  _log_path(path, sizeof(path), "ignores");
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_IGN));
  CHILD_CHECK(clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED) == ccol_success);
  clog_info(lg, "first record");
  CHILD_CHECK(_sigxfsz_is(SIG_IGN));
  clog_close(lg);
  (void)unlink(path);
  _child_pass();
}

TEST(sigxfsz, an_untouched_policy_keeps_the_ignore_that_an_open_set) {
  child_outcome_t o;
  _run_child(_scenario_ignores_stay, &o);
  _print_child(__func__, &o);
  REQUIRE_TRUE(_child_passed(&o));
}
