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

/* This binary checks what clogger does about SIGXFSZ, which a write to a
 * regular file raises at the process's file size limit (RLIMIT_FSIZE). It
 * runs with SIGXFSZ at its default disposition, which ends the process, and
 * it's a binary of its own because tests.c ignores SIGXFSZ for its whole
 * process, and an ignored SIGXFSZ can't be seen.
 *
 * Opening a regular file under CLOG_SIGNALS_AUTO sets SIG_IGN once for the
 * process, and a policy change and a file size limit are process state as
 * well, so every scenario runs in a forked child of a process that never
 * opens a logger itself. The child reports back over a pipe: 0 for a pass, a
 * positive source line for the check that failed and a negative marker for
 * each phase it completed. */

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

/* Puts SIGXFSZ back to its default disposition and unblocks it in the
 * calling thread. */
static void _sigxfsz_default(void) {
  signal(SIGXFSZ, SIG_DFL);
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGXFSZ);
  pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}

__attribute__((constructor)) static void _setup(void) {
  _sigxfsz_default();
  test_reset_inherited_signals();
}

/* ========================================================================== */
/*                         HELPERS                                            */
/* ========================================================================== */

static bool _sigxfsz_is(void (*handler)(int)) {
  struct sigaction sa;
  if (sigaction(SIGXFSZ, NULL, &sa) != 0) return false;
  return !(sa.sa_flags & SA_SIGINFO) && sa.sa_handler == handler;
}

/* Sets the process's soft file size limit and keeps the hard one.
 * RLIM_INFINITY lifts the soft limit up to the hard one. */
static bool _set_file_size_limit(rlim_t soft) {
  struct rlimit rl;
  if (getrlimit(RLIMIT_FSIZE, &rl) != 0) return false;
  rl.rlim_cur = soft == RLIM_INFINITY ? rl.rlim_max : soft;
  return setrlimit(RLIMIT_FSIZE, &rl) == 0;
}

static off_t _file_size(const char *path) {
  struct stat st;
  return stat(path, &st) == 0 ? st.st_size : -1;
}

/* Reads at most cap - 1 bytes of path into buf, terminated, and returns the
 * count, or -1. */
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

/* The log file of one scenario. It goes in the working directory so it ends
 * up on the filesystem the tests normally use. */
static void _log_path(char *buf, size_t cap, const char *tag) {
  snprintf(buf, cap, "sigxfsz_%s.log", tag);
  (void)unlink(buf);
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

/* The file ends with a newline and holds `last`, the record the logger took
 * once the limit was lifted. A synchronous logger names the records that the
 * limit refused with a marker (lost is true), while an async logger keeps
 * them and delivers every one of them once there is room. */
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
    _sigxfsz_default();
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

/* A child passed when the last thing it reported is 0 and no signal killed
 * it. We don't read its exit status since valgrind can replace the exit
 * status of a forked child. */
static bool _child_passed(const child_outcome_t *o) {
  return o->forked && !WIFSIGNALED(o->status) && o->nvalues > 0 &&
         o->values[o->nvalues - 1] == 0;
}

static bool _child_died_of(const child_outcome_t *o, int sig) {
  return o->forked && WIFSIGNALED(o->status) && WTERMSIG(o->status) == sig;
}

static void _print_child(const char *name, const child_outcome_t *o) {
  if (_child_passed(o)) return;
  fprintf(stderr, "%s: forked=%d status=0x%x", name, o->forked, o->status);
  if (o->forked && WIFSIGNALED(o->status))
    fprintf(stderr, " (signal %d)", WTERMSIG(o->status));
  fprintf(stderr, " reports:");
  for (int i = 0; i < o->nvalues; ++i) fprintf(stderr, " %d", o->values[i]);
  fprintf(stderr, "\n");
}

/* ========================================================================== */
/*                         SCENARIOS                                          */
/* ========================================================================== */

/* A synchronous file logger under CLOG_SIGNALS_AUTO, whose open sets SIG_IGN
 * for SIGXFSZ. `extra` is how far past the size of the first record the
 * limit lies. With 0 the next write starts exactly at the limit and fails
 * right away, and with a few bytes the kernel takes part of a record first.
 * Once the limit is lifted the logger closes the fragment and names the
 * loss. */
static void _sync_auto_body(off_t extra) {
  char path[128];
  _log_path(path, sizeof(path), "sync");
  CHILD_CHECK(_sigxfsz_is(SIG_DFL));
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  CHILD_CHECK(lg != CLOG_NULL);
  CHILD_CHECK(_sigxfsz_is(SIG_IGN)); /* at the open, before any limit */
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

/* The same thing for an async logger, where the writer thread hits the
 * limit. */
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

/* The writer thread takes back the SIGXFSZ that its own failed write left
 * pending on it. The logger opens under CLOG_SIGNALS_UNTOUCHED so the
 * disposition stays the default one, and then the main thread blocks
 * SIGXFSZ, so a signal the kernel raises on the whole process (FreeBSD) can
 * only reach the writer thread. Phase 1 switches to CLOG_SIGNALS_AUTO, so the
 * writer thread blocks SIGXFSZ before its write at the limit. Phase 2 lifts
 * the limit and switches back to CLOG_SIGNALS_UNTOUCHED, so the writer thread
 * unblocks SIGXFSZ before its next write, and a SIGXFSZ left pending from
 * phase 1 would kill the process there. Phase 3 puts the limit back, and this
 * time the write must raise SIGXFSZ with its default disposition. */
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
  clog_flush(lg); /* raises SIGXFSZ on the writer thread */
  _child_mark(3);
  clog_close(lg);
  _child_pass();
}

TEST(sigxfsz, writer_thread_consumes_its_sigxfsz_and_follows_a_policy_change) {
  child_outcome_t o;
  _run_child(_scenario_writer_consumes_and_follows_policy, &o);
  /* The child dies before it can remove its file. */
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

/* Under CLOG_SIGNALS_UNTOUCHED opening a regular file changes nothing, so a
 * write at the limit raises SIGXFSZ with its default disposition. */
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
  /* We remove the file before the limit, because the process dies at it. */
  CHILD_CHECK(unlink(path) == 0);
  CHILD_CHECK(_set_file_size_limit((rlim_t)size));
  _child_mark(1);
  clog_info(lg, "record at the limit"); /* raises SIGXFSZ */
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

/* A handler the application installed before the open stays in place and
 * sees the SIGXFSZ of a write at the limit. */
static volatile sig_atomic_t _handler_calls;

static void _count_sigxfsz(int sig) {
  (void)sig;
  _handler_calls = _handler_calls + 1;
}

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

/* An ignore the application set stays an ignore, and switching the policy to
 * CLOG_SIGNALS_UNTOUCHED doesn't undo the ignore that an open under
 * CLOG_SIGNALS_AUTO set. */
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
