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

// glibc needs this for posix_openpt(3) and its companions.
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <clogger.h>
#include <common.h>
#include <dirent.h>
#include <errno.h>
#include <internal/clogderive.h>
#include <internal/test_helpers/clogger.h>
#include <internal/test_helpers/cpintable.h>
// musl has no backtrace(), so the probes below capture no frames there.
#if __has_include(<execinfo.h>)
#include <execinfo.h>
#define TEST_HAS_EXECINFO 1
#else
#define TEST_HAS_EXECINFO 0
#endif
#include <fcntl.h>
#include <ftw.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__) || defined(__OpenBSD__)
#include <pthread_np.h>
#endif

// Returns the calling thread's OS thread ID, the number that ps(1) shows, so
// tests can compare it with the TID a record's proc field carries.
static int _test_os_tid(void) {
#if defined(__linux__)
  return (int)syscall(SYS_gettid);
#elif defined(__FreeBSD__)
  return pthread_getthreadid_np();
#elif defined(__OpenBSD__)
  return getthrid();
#elif defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  return (int)tid;
#endif
}

// Reads the calling thread's name as the system holds it into buf (at most 16
// bytes with the terminator), so tests can compare it with a record's proc
// field. An unnamed thread reads as the program name, as the library reports
// it.
static void _test_get_thread_name(char buf[17]) {
#if defined(__linux__)
  prctl(PR_GET_NAME, buf);
#elif defined(__OpenBSD__)
  pthread_get_name_np(pthread_self(), buf, 17);
  if (buf[0] == '\0') snprintf(buf, 17, "%.15s", getprogname());
#else
  if (pthread_getname_np(pthread_self(), buf, 17) != 0 || buf[0] == '\0')
    snprintf(buf, 17, "%.15s", getprogname());
#endif
}

// Renames the calling thread without going through the library, to show that
// the library's identity cache doesn't see such a rename.
static void _test_set_thread_name(const char *name) {
#if defined(__linux__)
  prctl(PR_SET_NAME, name);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#elif defined(__OpenBSD__)
  pthread_set_name_np(pthread_self(), name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_fds.h>
#include <test_sanitizer.h>
#include <test_signals.h>

#include "consumer_view.h"

TAU_MAIN()

// Resets the signal state that the binary inherits before any test runs, since
// the forked children report through their wait status.
__attribute__((constructor)) static void _reset_inherited_signals(void) {
  test_reset_inherited_signals();
}

// Helpers

// Returns from the calling test with a SKIP line when this build of the
// library captures no backtrace (clog__test_backtrace_supported() is false).
// It's the first statement of every test of a successful capture, so those
// tests don't fail on musl, where the library writes the "backtrace
// unavailable" marker instead (other tests cover that).
#define BT_SKIP_WITHOUT_SUPPORT()                            \
  do {                                                       \
    if (!clog__test_backtrace_supported()) {                 \
      fprintf(stderr,                                        \
              "SKIP: this build of the library captures no " \
              "backtrace\n");                                \
      return;                                                \
    }                                                        \
  } while (0)

// Tells whether backtrace() can unwind this process at all, so a test that
// needs visible frames (or enough frames to overflow a buffer) can return
// early where it can't. Under qemu-arm a plain backtrace() three frames deep
// reports depth 0, and the library then appends no frames by design.
static bool backtrace_capture_genuinely_available(void) {
#if TEST_HAS_EXECINFO
  void *frames[8];
  int depth = backtrace(frames, 8);
  return depth > 2;
#else
  return false;
#endif
}

// Closes an fd-based logger (which flushes it) and the pipe's write end, then
// reads everything the logger wrote, so a pipe test can check the whole
// output. Fills buf with a NUL terminator and returns the length.
static size_t drain_pipe(clog lg, int rfd, int wfd, char *buf, size_t cap) {
  clog_close(lg);
  close(wfd);
  ssize_t n = read(rfd, buf, cap - 1);
  close(rfd);
  if (n < 0) n = 0;
  buf[n] = '\0';
  return (size_t)n;
}

// Reads up to cap - 1 bytes of the file at path into buf with one read() and
// adds a NUL terminator, so tests can check what a logger wrote. Returns the
// length (0 when the file can't be opened).
static size_t read_file(const char *path, char *buf, size_t cap) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) {
    buf[0] = '\0';
    return 0;
  }
  ssize_t n = read(fd, buf, cap - 1);
  close(fd);
  if (n < 0) n = 0;
  buf[n] = '\0';
  return (size_t)n;
}

// Counts the files in dir whose name starts with prefix, so rotation tests can
// check how many generations exist.
static int count_files_with_prefix(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) count++;
  }
  closedir(d);
  return count;
}

// Removes every file in dir whose name starts with prefix, then the directory,
// so a test leaves nothing behind on its usual path.
static void cleanup_dir(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  char path[1024];
  while ((e = readdir(d)) != NULL) {
    if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
    if (strncmp(e->d_name, prefix, strlen(prefix)) == 0) {
      snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
      unlink(path);
    }
  }
  closedir(d);
  rmdir(dir);
}

// Counts the files in dir whose name ends with suffix (".gz", for example),
// so compression tests can check what was compressed.
static int count_files_with_suffix(const char *dir, const char *suffix) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int count = 0;
  size_t slen = strlen(suffix);
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen >= slen && strcmp(e->d_name + nlen - slen, suffix) == 0) count++;
  }
  closedir(d);
  return count;
}

// Runs "gunzip -t" on gz_path and returns its exit status, so compression
// tests can check an archive with an independent tool; 0 means valid.
static int gunzip_test(const char *gz_path) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "gunzip -t '%s' 2>/dev/null", gz_path);
  return system(cmd);
}

// Decompresses gz_path into buf (up to cap - 1 bytes, NUL terminated) with
// gunzip, so a test can check the text of an archive. Returns the number of
// bytes, or 0 on an error.
static size_t gunzip_read(const char *gz_path, char *buf, size_t cap) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "gunzip -c '%s' 2>/dev/null", gz_path);
  FILE *f = popen(cmd, "r");
  if (!f) return 0;
  size_t got = fread(buf, 1, cap - 1, f);
  pclose(f);
  buf[got] = '\0';
  return got;
}

// Finds the first file in dir with the given suffix and copies its path into
// out (outsz bytes), so a test can open a generation whose name has a stamp.
// Returns 0 when found, -1 otherwise.
static int find_file_with_suffix(const char *dir, const char *suffix, char *out,
                                 size_t outsz) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  size_t slen = strlen(suffix);
  struct dirent *e;
  int found = 0;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen >= slen && strcmp(e->d_name + nlen - slen, suffix) == 0) {
      snprintf(out, outsz, "%s/%s", dir, e->d_name);
      found = 1;
      break;
    }
  }
  closedir(d);
  return found ? 0 : -1;
}

// Returns milliseconds on CLOCK_MONOTONIC. Every bounded wait of this file
// measures its bound with it, so sleeps that run long on a loaded runner don't
// stretch the bound.
static long long test_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000LL + ts.tv_nsec / 1000000L;
}

// Every directory a test makes through make_tmpdir() or make_cwd_tmpdir(), by
// absolute path, and the pid of the process that made them. A REQUIRE that
// returns early skips a test's own cleanup, so the destructor below removes
// what is left at exit. It's defined ahead of every other destructor of this
// file, so it runs after each of them that has no priority, and a forked
// child leaves the parent's list alone.
#define TEST_TMPDIRS_MAX 2048
static char g_test_tmpdirs[TEST_TMPDIRS_MAX][512];
static size_t g_test_tmpdirs_len;
static pid_t g_test_tmpdirs_pid;

// Records path, resolved with realpath(), in g_test_tmpdirs, so the destructor
// can remove it when a REQUIRE returns before the test's own cleanup. The
// first call stores this process in g_test_tmpdirs_pid, and a call from
// another process, or a name too long for a slot, adds nothing.
static void register_test_tmpdir(const char *path) {
  if (g_test_tmpdirs_len == 0) g_test_tmpdirs_pid = getpid();
  if (g_test_tmpdirs_pid != getpid() || g_test_tmpdirs_len >= TEST_TMPDIRS_MAX)
    return;
  char *real = realpath(path, NULL);
  if (!real) return;
  size_t len = strlen(real);
  if (real[0] == '/' && len < sizeof(g_test_tmpdirs[0])) {
    memcpy(g_test_tmpdirs[g_test_tmpdirs_len], real, len + 1);
    g_test_tmpdirs_len++;
  }
  free(real);
}

// Gives a directory that we own mode 0700 during the first nftw() walk of the
// leftover cleanup, because a test can leave a directory without write
// permission and the second walk must be able to empty it.
static int _tmpdir_make_writable(const char *p, const struct stat *st, int type,
                                 struct FTW *f) {
  (void)f;
  if (type == FTW_D && st->st_uid == geteuid()) (void)chmod(p, 0700);
  return 0;
}

// Removes one entry during the second, depth-first nftw() walk of the leftover
// cleanup.
static int _tmpdir_remove(const char *p, const struct stat *st, int type,
                          struct FTW *f) {
  (void)st;
  (void)f;
  if (type == FTW_DP)
    (void)rmdir(p);
  else
    (void)unlink(p);
  return 0;
}

// Removes every directory in g_test_tmpdirs that still exists, at exit of the
// process that registered them, so a test that returned early leaves no
// directory behind. A forked child's copy of the list is left alone.
__attribute__((destructor)) static void remove_leftover_test_tmpdirs(void) {
  if (g_test_tmpdirs_len == 0 || g_test_tmpdirs_pid != getpid()) return;
  for (size_t i = 0; i < g_test_tmpdirs_len; i++) {
    struct stat st;
    if (lstat(g_test_tmpdirs[i], &st) != 0 || !S_ISDIR(st.st_mode)) continue;
    (void)nftw(g_test_tmpdirs[i], _tmpdir_make_writable, 16, FTW_PHYS);
    (void)nftw(g_test_tmpdirs[i], _tmpdir_remove, 16, FTW_PHYS | FTW_DEPTH);
  }
}

// Makes a unique directory under /tmp for one test and registers it for the
// leftover cleanup. Writes its path into out (outsz bytes) and returns 0, or
// -1 on failure.
static int make_tmpdir(char *out, size_t outsz) {
  snprintf(out, outsz, "/tmp/clogger_test_XXXXXX");
  if (!mkdtemp(out)) return -1;
  register_test_tmpdir(out);
  return 0;
}

static void forget_parent_fixtures_in_child(void);

// Counts the descriptors this process has open (see test_fds.h), so a test can
// compare two counts and find a descriptor the logger leaked.
static int count_open_fds(void) { return test_count_open_fds(); }

// Reads /proc/<pid>/<name> into buf with a NUL terminator, for the dump of a
// stuck child. It runs only after a bounded wait timed out, so any failure (a
// reaped process, a permission problem) just returns false for the caller to
// print, and nothing here may hang or crash. Returns true on success.
static bool _clog_test_read_proc_file(pid_t pid, const char *name, char *buf,
                                      size_t buf_cap) {
  char path[64];
  snprintf(path, sizeof(path), "/proc/%d/%s", (int)pid, name);
  int fd = open(path, O_RDONLY);
  if (fd < 0) return false;
  ssize_t n = read(fd, buf, buf_cap - 1);
  close(fd);
  if (n < 0) return false;
  buf[n] = '\0';
  return true;
}

static void _clog_test_dump_child_stacks(pid_t pid);

// Dumps status, wchan, syscall and stat of pid and of each of its threads from
// /proc (each file on its own, so one unreadable file hides nothing), then
// every thread's stack. wait_child_bounded() calls it before it kills a child
// that timed out, so the CI log can tell why the child got stuck. A State S
// with a real syscall entry and a SigBlk mask that blocks nearly every signal
// matches the qemu-user fd_trans_lock hang (qemu issue 2846); a State R with
// an empty wchan points at a spin instead.
static void _clog_test_dump_stuck_child_diagnostics(pid_t pid) {
  char buf[4096];
  fprintf(stderr,
          "[STUCK_CHILD_DIAG] parent pid=%d timed out waiting for child "
          "pid=%d; dumping diagnostics\n",
          (int)getpid(), (int)pid);
  char proc_dir[32];
  snprintf(proc_dir, sizeof(proc_dir), "/proc/%d", (int)pid);
  if (access(proc_dir, F_OK) != 0) {
    fprintf(stderr, "[STUCK_CHILD_DIAG] no %s on this system\n", proc_dir);
    _clog_test_dump_child_stacks(pid);
    return;
  }

  static const char *const files[] = {"status", "wchan", "syscall", "stat"};
  for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
    if (_clog_test_read_proc_file(pid, files[i], buf, sizeof(buf))) {
      fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid,
              files[i], buf);
    } else {
      fprintf(stderr,
              "[STUCK_CHILD_DIAG] /proc/%d/%s: unreadable (errno=%d %s)\n",
              (int)pid, files[i], errno, strerror(errno));
    }
  }

  char task_dir[64];
  snprintf(task_dir, sizeof(task_dir), "/proc/%d/task", (int)pid);
  DIR *td = opendir(task_dir);
  if (!td) {
    fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/task: unreadable\n", (int)pid);
  } else {
    struct dirent *ent;
    while ((ent = readdir(td)) != NULL) {
      if (ent->d_name[0] == '.') continue;
      char rel[16 + sizeof(ent->d_name)];
      snprintf(rel, sizeof(rel), "task/%s/status", ent->d_name);
      if (_clog_test_read_proc_file(pid, rel, buf, sizeof(buf))) {
        fprintf(stderr, "[STUCK_CHILD_DIAG] /proc/%d/%s:\n%s\n", (int)pid, rel,
                buf);
      }
    }
    closedir(td);
  }
  fflush(stderr);
  _clog_test_dump_child_stacks(pid);
}

// Prints the stack of every thread of pid to stderr, so a timed-out child can
// be told stuck (the same frames in a lock or a wait) from slow. It uses the
// sampler the system has (sample(1) on macOS, gdb on Linux, lldb otherwise,
// plus procstat -kk on FreeBSD), each under timeout(1) where one is
// installed, so a hung attach can't hang the test binary.
static void _clog_test_dump_child_stacks(pid_t pid) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd),
           "{ p=%d; t=''; "
           "if command -v timeout >/dev/null 2>&1; then t='timeout 60'; "
           "elif command -v gtimeout >/dev/null 2>&1; then t='gtimeout 60'; "
           "fi; "
           "if command -v sample >/dev/null 2>&1; then $t sample $p 1; "
           "elif command -v gdb >/dev/null 2>&1; then "
           "$t gdb -p $p -batch -nx -ex 'thread apply all bt'; "
           "elif command -v lldb >/dev/null 2>&1; then "
           "$t lldb --batch -p $p -o 'thread backtrace all'; "
           "else echo 'no stack sampler (sample, gdb or lldb) found'; fi; "
           "if command -v procstat >/dev/null 2>&1; then procstat -kk $p; fi; "
           "} 1>&2 2>&1",
           (int)pid);
  fprintf(stderr, "[STUCK_CHILD_DIAG] thread stacks of pid=%d:\n", (int)pid);
  fflush(stdout);
  fflush(stderr);
  int rc = system(cmd);
  fprintf(stderr, "[STUCK_CHILD_DIAG] stack sampler finished (status %d)\n",
          rc);
  fflush(stderr);
}

// Lifecycle
TEST(lifecycle, open_fd_and_close) {
  clog lg = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_close(lg);
}

TEST(lifecycle, open_file_and_close) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);

  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_close(lg);

  struct stat st;
  REQUIRE_EQ(stat(path, &st), 0);

  cleanup_dir(dir, "app.log");
}

TEST(lifecycle, invalid_path_returns_null) {
  clog lg = clog_create_from_file_mp("/nonexistent/deeply/nested/path/app.log",
                                     CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_EQ(lg, CLOG_NULL);
}

TEST(lifecycle, open_fd_rejects_invalid_and_readonly_fds) {
  REQUIRE_EQ(clog_create_from_fd(-1, CLOG_INFO, NULL), CLOG_NULL);

  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_EQ(clog_create_from_fd(fds[1], CLOG_INFO, NULL), CLOG_NULL);

  int ro = open("/dev/null", O_RDONLY);
  REQUIRE_NE(ro, -1);
  REQUIRE_EQ(clog_create_from_fd(ro, CLOG_INFO, NULL), CLOG_NULL);
  close(ro);

  clog lg = clog_create_from_fd(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_close(lg);
}

// Under the default CLOG_SIGNALS_AUTO policy the first pipe write from this
// thread sets SIGPIPE to SIG_IGN for the process; without that the write
// kills the whole test binary. tests_sigpipe.c covers the policy itself.
TEST(lifecycle, writing_to_a_broken_pipe_does_not_crash_the_process) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  close(pipefd[0]);

  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "should not crash the process");

  clog_close(lg);
  close(pipefd[1]);
}

// The logger that full_pipe_log_thread() writes to, and a flag it sets once
// its log call has returned.
typedef struct {
  clog lg;
  atomic_bool done;
} full_pipe_log_ctx;

// Logs one record that can't fit in the full pipe of c->lg and then sets
// c->done, so the test can watch from the main thread whether the log call
// ever returns.
static void *full_pipe_log_thread(void *arg) {
  full_pipe_log_ctx *c = (full_pipe_log_ctx *)arg;
  clog_info(c->lg, "a record that does not fit in the full pipe");
  atomic_store(&c->done, true);
  return NULL;
}

// A synchronous logger waits for room in a full non-blocking pipe, and then
// the reader closes; the wait and the log call must end. OpenBSD never wakes a
// POLLOUT wait on an anonymous pipe when the reader closes, so the library asks
// for POLLIN there as well. Without that the log call waits for ever, which the
// bounded wait reports.
TEST(lifecycle, a_log_call_on_a_full_pipe_returns_when_the_reader_closes) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  bool nonblocking =
      fcntl(pipefd[1], F_SETFL, fcntl(pipefd[1], F_GETFL) | O_NONBLOCK) == 0;
  char chunk[4096];
  memset(chunk, 'x', sizeof(chunk));
  while (nonblocking && write(pipefd[1], chunk, sizeof(chunk)) > 0) {
  }
  full_pipe_log_ctx ctx = {.lg =
                               clog_create_from_fd(pipefd[1], CLOG_INFO, NULL)};
  atomic_init(&ctx.done, false);
  pthread_t tid;
  bool started = nonblocking && ctx.lg != CLOG_NULL &&
                 pthread_create(&tid, NULL, full_pipe_log_thread, &ctx) == 0;
  bool returned = false;
  if (started) {
    usleep(200000);
    close(pipefd[0]);
    pipefd[0] = -1;
    for (long long deadline_ms = test_now_ms() + 10000;
         !atomic_load(&ctx.done) && test_now_ms() < deadline_ms;)
      usleep(10000);
    returned = atomic_load(&ctx.done);
    if (returned)
      pthread_join(tid, NULL);
    else
      pthread_detach(tid);
  }
  if (returned && ctx.lg != CLOG_NULL) clog_close(ctx.lg);
  if (pipefd[0] >= 0) close(pipefd[0]);
  if (returned) close(pipefd[1]);
  REQUIRE_TRUE(nonblocking);
  REQUIRE_NE(ctx.lg, CLOG_NULL);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(returned);
}

// Output format
TEST(output, logfmt_fields_present) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "level="), NULL);
  REQUIRE_NE(strstr(buf, "proc="), NULL);
  REQUIRE_NE(strstr(buf, "src="), NULL);
  REQUIRE_NE(strstr(buf, "func="), NULL);
  REQUIRE_NE(strstr(buf, "msg="), NULL);
  REQUIRE_NE(strstr(buf, "INFO"), NULL);
  REQUIRE_NE(strstr(buf, "hello world"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, message_with_special_chars_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "key=\"value\"");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "msg="), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, all_levels_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_trace(lg, "t");
  clog_debug(lg, "d");
  clog_info(lg, "i");
  clog_warn(lg, "w");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "TRACE"), NULL);
  REQUIRE_NE(strstr(buf, "DEBUG"), NULL);
  REQUIRE_NE(strstr(buf, "INFO"), NULL);
  REQUIRE_NE(strstr(buf, "WARN"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, error_produce_backtrace) {
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_error(lg, "something bad");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

// Proc field

// Logs one record from a new thread through the logger at arg, so a test can
// compare the TID of that record with one from the main thread.
static void *_proc_test_thread(void *arg) {
  clog lg = *(clog *)arg;
  clog_info(lg, "from spawned thread");
  return NULL;
}

TEST(proc_field, logfmt_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  clog_info(lg, "proc test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  char *proc_start = strstr(buf, "proc=");
  REQUIRE_NE(proc_start, NULL);

  char *val = proc_start + 5;

  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(val, pid_part), NULL);

  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(val, tid_part), NULL);

  char *val_end = strchr(val, ' ');
  REQUIRE_NE(val_end, NULL);
  size_t val_len = (size_t)(val_end - val);
  char proc_val[256];
  REQUIRE_EQ((val_len < sizeof(proc_val)), 1);
  memcpy(proc_val, val, val_len);
  proc_val[val_len] = '\0';

  char *colon = strchr(proc_val, ':');
  REQUIRE_NE(colon, NULL);
  REQUIRE_EQ(strchr(colon + 1, ':'), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(proc_field, json_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  clog_info(lg, "proc json test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"proc\":"), NULL);

  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(buf, tid_part), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(proc_field, syslog_proc_in_sd_element) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  clog_info(lg, "proc syslog test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "proc=\""), NULL);

  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(buf, tid_part), NULL);
}

TEST(proc_field, different_threads_have_different_tids) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "main thread");

  pthread_t thr;
  int create_rv = pthread_create(&thr, NULL, _proc_test_thread, &lg);
  if (create_rv == 0) {
    pthread_join(thr, NULL);
  } else {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_rv, 0);

  clog_close(lg);

  char buf[131072];
  read_file(path, buf, sizeof(buf));

  char *main_proc = strstr(buf, "proc=");
  REQUIRE_NE(main_proc, NULL);
  char *second_proc = strstr(main_proc + 1, "proc=");
  REQUIRE_NE(second_proc, NULL);

  char get_tid_str[2][32];
  char *procs[2] = {main_proc, second_proc};
  for (int i = 0; i < 2; i++) {
    char *v = procs[i] + 5;
    char *colon = strchr(v, ':');
    REQUIRE_NE(colon, NULL);
    char *open = strchr(colon, '(');
    REQUIRE_NE(open, NULL);
    char *close = strchr(open, ')');
    REQUIRE_NE(close, NULL);
    size_t len = (size_t)(close - (open + 1));
    REQUIRE_EQ((len < sizeof(get_tid_str[i])), 1);
    memcpy(get_tid_str[i], open + 1, len);
    get_tid_str[i][len] = '\0';
  }

  REQUIRE_NE(strcmp(get_tid_str[0], get_tid_str[1]), 0);

  cleanup_dir(dir, "app.log");
}

// Level filtering
TEST(filtering, messages_below_min_level_dropped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_trace(lg, "should not appear");
  clog_debug(lg, "should not appear");
  clog_info(lg, "should not appear");
  clog_warn(lg, "this should appear");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "should not appear"), NULL);
  REQUIRE_NE(strstr(buf, "this should appear"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(filtering, off_suppresses_all_log_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_OFF, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_error(lg, "suppressed");
  clog_alert(lg, "suppressed");

  clog_close(lg);

  struct stat st;
  stat(path, &st);
  REQUIRE_EQ(st.st_size, (off_t)0);

  cleanup_dir(dir, "app.log");
}

TEST(filtering, set_level_changes_filter) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "before change - dropped");

  clog_set_level(lg, CLOG_TRACE);
  REQUIRE_EQ(clog_get_level(lg), CLOG_TRACE);

  clog_info(lg, "after change - visible");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "before change"), NULL);
  REQUIRE_NE(strstr(buf, "after change"), NULL);

  cleanup_dir(dir, "app.log");
}

// clog_set_level() refuses a value clog_level doesn't name with
// ccol_invalid_args and keeps the level, and both create calls give CLOG_NULL
// for it. A setter that stores any value makes -3 read back above CLOG_OFF,
// which drops the INFO record.
TEST(filtering, a_level_outside_the_enum_is_refused) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog bad_file =
      clog_create_from_file_mp(path, (clog_level)-3, NULL, NULL, NULL);
  clog bad_fd = clog_create_from_fd(STDERR_FILENO, (clog_level)8, NULL);
  if (bad_file != CLOG_NULL) clog_close(bad_file);
  if (bad_fd != CLOG_NULL) clog_close(bad_fd);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  ccol_retval rv_minus_3 = clog_set_level(lg, (clog_level)-3);
  clog_level after_minus_3 = clog_get_level(lg);
  ccol_retval rv_8 = clog_set_level(lg, (clog_level)8);
  clog_level after_8 = clog_get_level(lg);
  ccol_retval rv_warn = clog_set_level(lg, CLOG_WARN);
  clog_level after_warn = clog_get_level(lg);
  ccol_retval rv_info = clog_set_level(lg, CLOG_INFO);
  clog_info(lg, "still visible");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(bad_file, CLOG_NULL);
  REQUIRE_EQ(bad_fd, CLOG_NULL);
  REQUIRE_EQ(rv_minus_3, ccol_invalid_args);
  REQUIRE_EQ(rv_8, ccol_invalid_args);
  REQUIRE_EQ(rv_warn, ccol_success);
  REQUIRE_EQ(rv_info, ccol_success);
  REQUIRE_EQ(after_minus_3, CLOG_INFO);
  REQUIRE_EQ(after_8, CLOG_INFO);
  REQUIRE_EQ(after_warn, CLOG_WARN);
  REQUIRE_NE(strstr(buf, "still visible"), NULL);
}

// Structured fields
TEST(fields, set_field_appears_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "service", "auth");
  clog_set_field(lg, "env", "prod");

  clog_info(lg, "request handled");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "service=auth"), NULL);
  REQUIRE_NE(strstr(buf, "env=prod"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, update_existing_field) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  ccol_retval rv_first = clog_set_field(lg, "req_id", "aaa");
  ccol_retval rv_second = clog_set_field(lg, "req_id", "bbb");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(rv_first, ccol_success);
  REQUIRE_EQ(rv_second, ccol_success);
  REQUIRE_EQ(strstr(buf, "req_id=aaa"), NULL);
  REQUIRE_NE(strstr(buf, "req_id=bbb"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, remove_field_disappears) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "trace_id", "xyz");
  clog_remove_field(lg, "trace_id");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "trace_id"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, clear_removes_all) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "a", "1");
  clog_set_field(lg, "b", "2");
  clog_set_field(lg, "c", "3");
  clog_clear_fields(lg);

  clog_info(lg, "empty fields");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, " a="), NULL);
  REQUIRE_EQ(strstr(buf, " b="), NULL);
  REQUIRE_EQ(strstr(buf, " c="), NULL);
  REQUIRE_NE(strstr(buf, "empty fields"), NULL);

  cleanup_dir(dir, "app.log");
}

// Keys with an empty name, a space, '=', a control byte, DEL, ']', '\', '"'
// or a byte above 0x7f are refused with ccol_invalid_args and leave the fields
// as they are.
TEST(fields, invalid_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  const char *bad_keys[] = {
      "",        "bad key",  "bad=key",  "bad\x01key",  "bad\177key",
      "bad]key", "bad\\key", "bad\"key", "bad\xc3\xa9",
  };
  size_t n_bad = sizeof(bad_keys) / sizeof(bad_keys[0]);
  size_t n_refused = 0;
  for (size_t i = 0; i < n_bad; i++)
    if (clog_set_field(lg, bad_keys[i], "v") == ccol_invalid_args) n_refused++;
  ccol_retval rv_good = clog_set_field(lg, "good_key", "ok");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(n_refused, n_bad);
  REQUIRE_EQ(rv_good, ccol_success);

  REQUIRE_EQ(strstr(buf, "=v"), NULL);
  REQUIRE_EQ(strstr(buf, "bad key"), NULL);
  REQUIRE_EQ(strstr(buf, "bad=key="), NULL);
  REQUIRE_EQ(strstr(buf, "bad]key"), NULL);
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

// Every key that clog__write() always writes itself is refused; one that got
// through would give a duplicate JSON key, logfmt token or SD-PARAM-NAME.
TEST(fields, reserved_key_names_are_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *reserved[] = {"ts",   "level", "proc", "src",
                            "func", "msg",   "bt",   "bt_error"};
  size_t n_reserved = sizeof(reserved) / sizeof(reserved[0]);
  size_t n_refused = 0;
  for (size_t i = 0; i < n_reserved; i++)
    if (clog_set_field(lg, reserved[i], "should-not-appear") ==
        ccol_invalid_args)
      n_refused++;
  ccol_retval rv_good = clog_set_field(lg, "good_key", "ok");

  clog_info(lg, "reserved key test");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(n_refused, n_reserved);
  REQUIRE_EQ(rv_good, ccol_success);
  REQUIRE_EQ(strstr(buf, "should-not-appear"), NULL);
  REQUIRE_NE(strstr(buf, "\"good_key\":\"ok\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, value_with_spaces_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "host", "web server 01");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "host=\"web server 01\""), NULL);

  cleanup_dir(dir, "app.log");
}

// A value whose only special byte is '=' is quoted, because a reader would
// otherwise take its tail for a key of its own (query=a=1&b=2).
TEST(fields, value_with_an_equals_sign_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  ccol_retval set_rv = clog_set_field(lg, "query", "a=1&b=2");
  clog_info(lg, "x=y");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_NE(strstr(buf, "query=\"a=1&b=2\""), NULL);
  REQUIRE_NE(strstr(buf, "msg=\"x=y\""), NULL);
}

TEST(fields, del_byte_in_value_is_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  clog_info(lg,
            "msg\x7f"
            "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  REQUIRE_NE(strstr(buf, "\\x7f"), NULL);

  cleanup_dir(dir, "app.log");
}

// DEL is outside PRINTUSASCII, and a key goes into logfmt unchanged, so a DEL
// key would corrupt the output; it's refused like a C0 control byte.
TEST(fields, del_byte_in_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");
  clog_set_field(lg, "good_key", "ok");

  clog_info(lg, "test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  REQUIRE_EQ(strstr(buf, "=v"), NULL);
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

// RFC 5424 allows only PRINTUSASCII in an SD-PARAM-NAME, so a key with a byte
// above 0x7f is refused. Only an fd-based logger can use CLOG_FMT_SYSLOG, so
// this test writes to a pipe.
TEST(fields, non_ascii_byte_in_key_is_rejected) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "bad\xc3\xa9key", "v");
  clog_set_field(lg, "good_key", "ok");

  clog_info(lg, "test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, (int)(unsigned char)0xc3, strlen(buf)), NULL);
  REQUIRE_EQ(strstr(buf, "=\"v\""), NULL);
  REQUIRE_NE(strstr(buf, "good_key=\"ok\""), NULL);
}

TEST(fields, newline_and_cr_in_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "payload", "line1\nline2\rend\ttab");
  clog_info(lg, "escape test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);

  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  size_t first_line_len = (size_t)(nl - buf);
  REQUIRE_EQ(memchr(buf, '\r', first_line_len), NULL);
  REQUIRE_EQ(memchr(buf, '\t', first_line_len), NULL);

  REQUIRE_NE(strstr(buf, "\\n"), NULL);
  REQUIRE_NE(strstr(buf, "\\r"), NULL);
  REQUIRE_NE(strstr(buf, "\\t"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, empty_value_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "empty_field", "");
  clog_info(lg, "empty value test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "empty_field=\"\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, backslash_in_value_is_quoted_and_escaped_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "win_path", "C:\\Users\\foo");
  clog_info(lg, "backslash test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "win_path=\"C:\\\\Users\\\\foo\""), NULL);

  cleanup_dir(dir, "app.log");
}

// Long messages

// The message is longer than the 1024-byte stack buffer of clog__write(), so
// the logger formats it on the heap.
TEST(output, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_msg[2048];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  clog_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, long_msg), NULL);

  cleanup_dir(dir, "app.log");
}

// An allocator that fails every request above 4096 bytes, to reach the "heap
// allocation for the full message failed" path deterministically: Logger
// construction and ordinary records stay below that size, and only the heap
// spill of a large message goes above it.
static void *_size_capped_malloc(size_t sz) {
  return sz > 4096 ? NULL : malloc(sz);
}
static void _size_capped_free(void *p) { free(p); }
static void *_size_capped_calloc(size_t n, size_t sz) {
  return sz != 0 && n > 4096 / sz ? NULL : calloc(n, sz);
}
static void *_size_capped_realloc(void *p, size_t sz) {
  return sz > 4096 ? NULL : realloc(p, sz);
}

TEST(output,
     message_exceeding_stack_buffer_marks_truncation_when_heap_alloc_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_msg[5000];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  clog_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  cleanup_dir(dir, "app.log");
}

// Oversized records

// The message is above the 16 MiB write-buffer cap. The output must hold
// exactly two well-formed lines, the fallback notice and the next record,
// because an oversized message must never desynchronize the stream.
TEST(oversized, logfmt_message_over_buf_cap_falls_back_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  clog_info(lg, "%s", huge);
  clog_info(lg, "normal record after the oversized one");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 2);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "normal record after the oversized one"), NULL);

  char *fallback_line = strstr(buf, "too large to emit");
  REQUIRE_NE(fallback_line, NULL);
  char *line_end = strchr(fallback_line, '\n');
  REQUIRE_NE(line_end, NULL);
  REQUIRE_EQ(*(line_end - 1), '"');

  cleanup_dir(dir, "app.log");
}

TEST(oversized, json_message_over_buf_cap_falls_back_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  clog_info(lg, "%s", huge);
  clog_info(lg, "normal json record");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  int json_lines = 0;
  char *line = buf;
  while (*line == '{') {
    json_lines++;
    char *nl = strchr(line, '\n');
    REQUIRE_NE(nl, NULL);
    REQUIRE_EQ(*(nl - 1), '}');
    line = nl + 1;
  }
  REQUIRE_EQ(json_lines, 2);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "normal json record"), NULL);

  cleanup_dir(dir, "app.log");
}

// When an oversized record that asked for a backtrace falls back to the
// placeholder, the JSON output marks the backtrace as missing (bt_error), so
// it never looks like a record that asked for none.
TEST(oversized, json_fallback_with_backtrace_marks_bt_as_unavailable) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  clog_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "\"bt_error\""), NULL);
  REQUIRE_EQ(strstr(buf, "\"bt\":["), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(oversized, syslog_message_over_buf_cap_falls_back_cleanly) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'C', sz);
  huge[sz] = '\0';

  clog_info(lg, "%s", huge);
  clog_info(lg, "normal syslog record");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  free(huge);

  int rec_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "<14>1 ")) != NULL) {
    rec_count++;
    p++;
  }
  REQUIRE_EQ(rec_count, 2);

  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, 2);

  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "normal syslog record"), NULL);
}

// logfmt writes backtrace frames as separate continuation lines after the
// record, so an oversized message replaces the record with the placeholder and
// the frames follow as usual. Every line must end with a newline.
TEST(oversized,
     logfmt_message_over_buf_cap_with_backtrace_still_writes_backtrace_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  clog_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);

  if (backtrace_capture_genuinely_available()) {
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }

  size_t i = 0;
  while (i < len) {
    REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
    char *nl = memchr(buf + i, '\n', len - i);
    REQUIRE_NE(nl, NULL);
    i = (size_t)(nl - buf) + 1;
  }

  cleanup_dir(dir, "app.log");
}

// Syslog writes each backtrace frame as its own PRI-prefixed record, whether
// or not the main record falls back to the placeholder. PRI 11 is facility
// USER (1) * 8 + severity ERROR (3).
TEST(oversized,
     syslog_message_over_buf_cap_with_backtrace_still_writes_backtrace_lines) {
  BT_SKIP_WITHOUT_SUPPORT();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_ERROR, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_NULL);
  memset(huge, 'D', sz);
  huge[sz] = '\0';

  clog_error(lg, "%s", huge);

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  free(huge);

  int rec_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "<11>1 ")) != NULL) {
    rec_count++;
    p++;
  }
  bool bt_available = backtrace_capture_genuinely_available();
  if (bt_available) {
    REQUIRE_GT(rec_count, 1);
  } else {
    REQUIRE_EQ(rec_count, 1);
  }

  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, rec_count);

  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  if (bt_available) {
    REQUIRE_NE(strstr(buf, " - #0 "), NULL);
  }
}

// Size-based rotation
TEST(rotation, size_rotation_creates_new_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 20; i++)
    clog_info(lg, "rotation test message index=%d padding-padding-padding", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, max_rotated_files_respected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 40; i++)
    clog_info(lg, "rotation pruning test idx=%d extra-data-to-fill-the-buffer",
              i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ((rotated <= 2), 1);

  cleanup_dir(dir, "app.log");
}

// A max_rotated_files of 0 means "not set" and falls back to
// CLOG_DEFAULT_MAX_ROTATED_FILES (7), like the other rotation fields. If 0
// meant "no limit", rotated files would pile up until the disk is full.
TEST(rotation, max_rotated_files_zero_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 40; i++)
    clog_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

// Every value <= 0 falls back to the default, as for max_file_size.
TEST(rotation, max_rotated_files_negative_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = -3,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 40; i++)
    clog_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

// A rotated name is "<base>.<14 digits>", optionally "_NNNN" and ".gz". A file
// with any other text after the 14 digits (a manual dated backup, say) is
// never pruned, however often the pruner runs.
TEST(rotation, prune_never_deletes_unrelated_file_with_similar_name) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  char foreign_path[600];
  snprintf(foreign_path, sizeof(foreign_path),
           "%s/app.log.20260101000000_my_manual_backup", dir);
  {
    int fd = open(foreign_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    const char *content = "do not delete me";
    ssize_t w = write(fd, content, strlen(content));
    (void)w;
    close(fd);
  }

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 40; i++)
    clog_info(lg, "prune safety test idx=%d extra-data-to-fill-the-buffer", i);

  clog_close(lg);

  struct stat st;
  REQUIRE_EQ(stat(foreign_path, &st), 0);
  char buf[64];
  read_file(foreign_path, buf, sizeof(buf));
  REQUIRE_STREQ(buf, "do not delete me");

  cleanup_dir(dir, "app.log");
}

// The log file is deleted while the logger holds it open. The next rotation's
// rename() fails with ENOENT, which rotation treats as a clean slate: It
// creates the file again at the original path and carries on.
TEST(rotation, logger_recovers_after_file_externally_deleted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    char filler[9999];
    memset(filler, 'x', sizeof(filler));
    ssize_t w = write(fd, filler, sizeof(filler));
    (void)w;
    close(fd);
  }

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10000,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  REQUIRE_EQ(unlink(path), 0);

  clog_info(lg, "triggers rotation");

  clog_info(lg, "after recovery");

  clog_close(lg);

  struct stat st;
  REQUIRE_EQ(stat(path, &st), 0);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "after recovery"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, time_based_rotation_creates_rotated_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = false,
      .time_rotation_enabled = true,
      .rotation_interval_us = 1000000,
      .max_rotated_files = 0,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "before rotation");

  sleep(2);

  clog_info(lg, "after rotation");

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "after rotation"), NULL);

  cleanup_dir(dir, "app.log");
}

// Every write starts a rotation (max_file_size 10), so several rotations in
// one UTC second get the suffixes _0001, _0002 and so on.
TEST(rotation, same_second_collision_uses_suffix) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 5; i++) clog_info(lg, "collision test %d", i);

  clog_close(lg);

  REQUIRE_GT(count_files_with_suffix(dir, "_0001"), 0);

  cleanup_dir(dir, "app.log");
}

// After a failed rotation (here the directory loses its write permission) the
// logger backs off for a bounded window, so later writes don't each pay for a
// rename() and an open() that fail again.
TEST(rotation, persistent_failure_does_not_retry_on_every_write) {
  if (geteuid() == 0) {
    fprintf(stderr,
            "[SKIP] persistent_failure_does_not_retry_on_every_write: "
            "running as root, a permission-based rotation failure cannot "
            "be forced\n");
    return;
  }

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  REQUIRE_EQ(chmod(dir, 0555), 0);

  clog__test_reset_rotate_attempt_count();

  for (int i = 0; i < 20; i++) clog_info(lg, "retry backoff test idx=%d", i);

  size_t attempts = clog__test_get_rotate_attempt_count();
  REQUIRE_GT(attempts, (size_t)0);
  REQUIRE_LT(attempts, (size_t)20);

  REQUIRE_EQ(chmod(dir, 0755), 0);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// Derived loggers
TEST(derive, derived_is_not_null) {
  clog parent = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_close(child);
  clog_close(parent);
}

TEST(derive, inherits_parent_fields) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog_set_field(parent, "service", "auth");
  clog_set_field(parent, "env", "prod");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_info(child, "from child");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "service=auth"), NULL);
  REQUIRE_NE(strstr(buf, "env=prod"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_field_does_not_affect_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_set_field(child, "req_id", "child-only");

  clog_info(parent, "parent line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "req_id"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, parent_field_after_derive_does_not_affect_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_set_field(parent, "added_after", "yes");

  clog_info(child, "child line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "added_after"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, shares_output_target) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_info(parent, "parent message");
  clog_info(child, "child message");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "parent message"), NULL);
  REQUIRE_NE(strstr(buf, "child message"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_close_does_not_break_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_info(child, "before child close");
  clog_close(child);

  clog_info(parent, "after child close");
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "before child close"), NULL);
  REQUIRE_NE(strstr(buf, "after child close"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, parent_close_does_not_break_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_info(parent, "before parent close");
  clog_close(parent);

  clog_info(child, "after parent close");
  clog_close(child);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "before parent close"), NULL);
  REQUIRE_NE(strstr(buf, "after parent close"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, independent_level) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_set_level(child, CLOG_DEBUG);

  clog_debug(parent, "parent debug - dropped");
  clog_debug(child, "child debug - visible");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "parent debug"), NULL);
  REQUIRE_NE(strstr(buf, "child debug"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, field_override_in_child_is_independent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog_set_field(parent, "version", "1");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_set_field(child, "version", "2");

  clog_info(parent, "parent write");
  clog_info(child, "child write");

  clog_close(child);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  char *parent_line = strstr(buf, "parent write");
  char *child_line = strstr(buf, "child write");
  REQUIRE_NE(parent_line, NULL);
  REQUIRE_NE(child_line, NULL);

  char *ps = parent_line;
  while (ps > buf && *(ps - 1) != '\n') ps--;
  char *cs = child_line;
  while (cs > buf && *(cs - 1) != '\n') cs--;

  char *pe = strchr(parent_line, '\n');
  char *ce = strchr(child_line, '\n');
  if (pe) *pe = '\0';
  if (ce) *ce = '\0';

  REQUIRE_NE(strstr(ps, "version=1"), NULL);
  REQUIRE_NE(strstr(cs, "version=2"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, grandchild_derive) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);
  clog_set_field(parent, "origin", "parent");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);
  clog_set_field(child, "gen", "child");

  clog grandchild = clog_derive(child);
  REQUIRE_NE(grandchild, CLOG_NULL);

  clog_close(parent);
  clog_close(child);

  clog_info(grandchild, "from grandchild");
  clog_close(grandchild);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "from grandchild"), NULL);
  REQUIRE_NE(strstr(buf, "origin=parent"), NULL);
  REQUIRE_NE(strstr(buf, "gen=child"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, multiple_children_from_one_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);
  clog_set_field(parent, "shared", "yes");

  clog child1 = clog_derive(parent);
  REQUIRE_NE(child1, CLOG_NULL);
  clog_set_field(child1, "name", "c1");

  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_NULL);
  clog_set_field(child2, "name", "c2");

  clog_info(child1, "from child1");
  clog_info(child2, "from child2");

  clog_close(child1);
  clog_close(child2);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "from child1"), NULL);
  REQUIRE_NE(strstr(buf, "from child2"), NULL);

  char *msg1 = strstr(buf, "from child1");
  char *msg2 = strstr(buf, "from child2");
  REQUIRE_NE(msg1, NULL);
  REQUIRE_NE(msg2, NULL);

  char *ls1 = msg1;
  while (ls1 > buf && *(ls1 - 1) != '\n') ls1--;
  char *ls2 = msg2;
  while (ls2 > buf && *(ls2 - 1) != '\n') ls2--;

  char *le1 = strchr(msg1, '\n');
  char *le2 = strchr(msg2, '\n');
  if (le1) *le1 = '\0';
  if (le2) *le2 = '\0';

  REQUIRE_NE(strstr(ls1, "shared=yes"), NULL);
  REQUIRE_NE(strstr(ls2, "shared=yes"), NULL);

  REQUIRE_NE(strstr(ls1, "name=c1"), NULL);
  REQUIRE_EQ(strstr(ls1, "name=c2"), NULL);
  REQUIRE_NE(strstr(ls2, "name=c2"), NULL);
  REQUIRE_EQ(strstr(ls2, "name=c1"), NULL);

  cleanup_dir(dir, "app.log");
}

// Thread safety

// THREAD_COUNT writers each log MSGS_PER_THREAD records through one logger.
#define THREAD_COUNT 8
#define MSGS_PER_THREAD 50

// The logger and the number of one writer thread.
typedef struct {
  clog lg;
  int thread_id;
} thread_arg_t;

// Logs MSGS_PER_THREAD numbered records through a->lg, one of the threads
// that check for garbled lines under concurrent writes.
static void *_writer_thread(void *arg) {
  thread_arg_t *a = (thread_arg_t *)arg;
  for (int i = 0; i < MSGS_PER_THREAD; i++)
    clog_info(a->lg, "thread=%d msg=%d", a->thread_id, i);
  return NULL;
}

TEST(threading, concurrent_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t threads[THREAD_COUNT];
  thread_arg_t args[THREAD_COUNT];
  bool created[THREAD_COUNT];
  int create_failures = 0;

  for (int i = 0; i < THREAD_COUNT; i++) {
    args[i].lg = lg;
    args[i].thread_id = i;
    created[i] =
        (pthread_create(&threads[i], NULL, _writer_thread, &args[i]) == 0);
    if (!created[i]) create_failures++;
  }
  for (int i = 0; i < THREAD_COUNT; i++)
    if (created[i]) pthread_join(threads[i], NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(lg);

  char buf[131072];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_NE(len, (size_t)0);

  int bad_lines = 0;
  char *line = buf;
  while (line < buf + len) {
    char *nl = strchr(line, '\n');
    if (line[0] != '\t' && strncmp(line, "ts=", 3) != 0) bad_lines++;
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

// Custom allocator

// A counting allocator, so a test can check that the logger allocates through
// the procs it was given and frees all of it: _custom_alloc_count counts every
// malloc, calloc and realloc, and _custom_free_count every free of a non-NULL
// pointer.
static size_t _custom_alloc_count = 0;
static size_t _custom_free_count = 0;

// The procs of the counting allocator above.
static void *_custom_malloc(size_t sz) {
  _custom_alloc_count++;
  return malloc(sz);
}
static void _custom_free(void *p) {
  if (p) _custom_free_count++;
  free(p);
}
static void *_custom_calloc(size_t n, size_t sz) {
  _custom_alloc_count++;
  return calloc(n, sz);
}
static void *_custom_realloc(void *p, size_t sz) {
  _custom_alloc_count++;
  return realloc(p, sz);
}

TEST(custom_alloc, open_fd_uses_custom_allocator) {
  _custom_alloc_count = 0;
  _custom_free_count = 0;

  ccol_memmgmt_procs procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "custom allocator test");

  clog_close(lg);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);
}

TEST(custom_alloc, open_file_uses_custom_allocator) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  _custom_alloc_count = 0;
  _custom_free_count = 0;

  ccol_memmgmt_procs procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "env", "test");
  clog_info(lg, "custom allocator file test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "custom allocator file test"), NULL);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);

  cleanup_dir(dir, "app.log");
}

TEST(custom_alloc, derived_logger_uses_parent_allocator) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  _custom_alloc_count = 0;
  _custom_free_count = 0;

  ccol_memmgmt_procs procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_info(child, "child using parent allocator");
  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "child using parent allocator"), NULL);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);

  cleanup_dir(dir, "app.log");
}

TEST(custom_alloc, invalid_mprocs_returns_null) {
  ccol_memmgmt_procs bad_procs = {
      .malloc = _custom_malloc,
      .free = NULL,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &bad_procs);
  REQUIRE_EQ(lg, CLOG_NULL);
}

// An allocator whose malloc, calloc and realloc fail at exactly the
// g_ctor_fail_at'th call (counting from 1 in g_ctor_fail_call_count; <= 0
// never fails), so a test can fail each step of clog_create_from_file_mp()
// after the log file is open.
static _Atomic int g_ctor_fail_call_count = 0;
static _Atomic int g_ctor_fail_at = -1;

// Counts one call of the allocator and tells whether it is the one to fail;
// the procs below use it.
static bool _ctor_fail_should_fail(void) {
  int c = atomic_fetch_add(&g_ctor_fail_call_count, 1) + 1;
  int fa = atomic_load(&g_ctor_fail_at);
  return fa > 0 && c == fa;
}
static void *_ctor_fail_malloc(size_t sz) {
  return _ctor_fail_should_fail() ? NULL : malloc(sz);
}
static void _ctor_fail_free(void *p) { free(p); }
static void *_ctor_fail_calloc(size_t n, size_t sz) {
  return _ctor_fail_should_fail() ? NULL : calloc(n, sz);
}
static void *_ctor_fail_realloc(void *p, size_t sz) {
  return _ctor_fail_should_fail() ? NULL : realloc(p, sz);
}

// clog_create_from_file_mp() opens its file before the rest of the
// construction, so every later failure, the async setup included, must close
// that fd. The test sweeps "fail the Nth allocation" over a wide range, so it
// doesn't depend on the exact number of allocations, and the fd count must
// come back to its start after each iteration.
TEST(custom_alloc, open_file_async_init_failure_does_not_leak_fd) {
  int baseline = count_open_fds();

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _ctor_fail_malloc,
      .free = _ctor_fail_free,
      .calloc = _ctor_fail_calloc,
      .realloc = _ctor_fail_realloc,
  };
  clog_async_cfg acfg = {0};

  for (int fail_at = 1; fail_at <= 60; fail_at++) {
    atomic_store(&g_ctor_fail_call_count, 0);
    atomic_store(&g_ctor_fail_at, fail_at);
    clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
    atomic_store(&g_ctor_fail_at, -1);

    if (lg != CLOG_NULL) clog_close(lg);

    int now = count_open_fds();
    REQUIRE_EQ(now, baseline);
  }

  cleanup_dir(dir, "app.log");
}

// Null handle validation

// Each wrapper below calls one public entry point with CLOG_NULL. They exist
// so _expect_fatal() can run each call in a child and check that the entry
// point asserts on that handle instead of dereferencing it.
static void _call_clog_close_null(void) { clog_close(CLOG_NULL); }
static void _call_clog_derive_null(void) { (void)clog_derive(CLOG_NULL); }
static void _call_clog_set_level_null(void) {
  clog_set_level(CLOG_NULL, CLOG_INFO);
}
static void _call_clog_get_level_null(void) { (void)clog_get_level(CLOG_NULL); }
static void _call_clog_set_format_null(void) {
  clog_set_format(CLOG_NULL, CLOG_FMT_JSON);
}
static void _call_clog_get_format_null(void) {
  (void)clog_get_format(CLOG_NULL);
}
static void _call_clog_set_facility_null(void) {
  clog_set_facility(CLOG_NULL, CLOG_SYSLOG_USER);
}
static void _call_clog_get_facility_null(void) {
  (void)clog_get_facility(CLOG_NULL);
}
static void _call_clog_set_field_null(void) {
  clog_set_field(CLOG_NULL, "key", "value");
}
static void _call_clog_remove_field_null(void) {
  clog_remove_field(CLOG_NULL, "key");
}
static void _call_clog_clear_fields_null(void) { clog_clear_fields(CLOG_NULL); }
static void _call_clog_flush_null(void) { clog_flush(CLOG_NULL); }
static void _call_log_info_null(void) { clog_info(CLOG_NULL, "message"); }

// Runs fn in a forked child with its output sent to /dev/null and requires
// that the child dies by SIGABRT, for the checks that an invalid handle ends
// the process with an assertion.
static void _expect_fatal(void (*fn)(void)) {
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    fn();
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

TEST(null_handle, close_is_fatal) { _expect_fatal(_call_clog_close_null); }
TEST(null_handle, derive_is_fatal) { _expect_fatal(_call_clog_derive_null); }
TEST(null_handle, set_level_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_set_level_null);
}
TEST(null_handle, get_level_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_get_level_null);
}
TEST(null_handle, set_format_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_set_format_null);
}
TEST(null_handle, get_format_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_get_format_null);
}
TEST(null_handle, set_facility_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_set_facility_null);
}
TEST(null_handle, get_facility_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_get_facility_null);
}
TEST(null_handle, set_field_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_set_field_null);
}
TEST(null_handle, remove_field_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_remove_field_null);
}
TEST(null_handle, clear_fields_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_clog_clear_fields_null);
}
TEST(null_handle, flush_is_fatal) { _expect_fatal(_call_clog_flush_null); }
TEST(null_handle, write_via_log_macro_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  _expect_fatal(_call_log_info_null);
}

// Robustness under allocation failure

// An allocator whose realloc() fails at exactly the g_realloc_fail_at'th call
// (counting from 1 in g_realloc_call_count; <= 0 never fails), so a test can
// sweep one failure over every buffer growth of a clog_error() with a
// backtrace.
static int g_realloc_fail_at = -1;
static int g_realloc_call_count = 0;

// The procs of the realloc-failing allocator above; only realloc() counts.
static void *_counting_malloc(size_t sz) { return malloc(sz); }
static void _counting_free(void *p) { free(p); }
static void *_counting_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void *_counting_realloc(void *p, size_t sz) {
  g_realloc_call_count++;
  if (g_realloc_fail_at > 0 && g_realloc_call_count == g_realloc_fail_at)
    return NULL;
  return realloc(p, sz);
}

// The realloc failure lands in the construction, the record or a backtrace
// frame. Each must degrade cleanly: Every line written starts with the right
// prefix and ends with a newline, so no partial write merges into the next.
TEST(robustness, error_with_backtrace_never_writes_malformed_lines_under_oom) {
  ccol_memmgmt_procs procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };

  for (int fail_at = 1; fail_at <= 80; fail_at++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    g_realloc_call_count = 0;
    g_realloc_fail_at = fail_at;

    clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      cleanup_dir(dir, "app.log");
      continue;
    }

    clog_error(lg, "robustness check %d", fail_at);

    g_realloc_fail_at = -1;
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof(buf));

    size_t i = 0;
    while (i < len) {
      REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
      if (buf[i] == 't') REQUIRE_EQ(strncmp(buf + i, "ts=", 3), 0);
      char *nl = memchr(buf + i, '\n', len - i);
      REQUIRE_NE(nl, NULL);
      i = (size_t)(nl - buf) + 1;
    }

    cleanup_dir(dir, "app.log");
  }

  g_realloc_fail_at = -1;
}

// JSON embeds the backtrace inside the record. When an allocation or
// backtrace_symbols() fails, the record keeps a (maybe shorter) "bt" array,
// gets a "bt_error" marker, or becomes the oversized-record fallback. A normal
// record with neither "bt" nor "bt_error" must never appear.
TEST(robustness, json_backtrace_failure_is_never_silently_dropped) {
  ccol_memmgmt_procs procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };

  for (int fail_at = 1; fail_at <= 80; fail_at++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    g_realloc_call_count = 0;
    g_realloc_fail_at = fail_at;

    clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      cleanup_dir(dir, "app.log");
      continue;
    }
    clog_set_format(lg, CLOG_FMT_JSON);

    clog_error(lg, "json backtrace robustness check %d", fail_at);

    g_realloc_fail_at = -1;
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof(buf));
    if (len == 0) {
      cleanup_dir(dir, "app.log");
      continue;
    }

    REQUIRE_EQ(buf[0], '{');
    char *nl = strchr(buf, '\n');
    REQUIRE_NE(nl, NULL);
    REQUIRE_EQ(*(nl - 1), '}');

    bool has_bt = strstr(buf, "\"bt\":[") != NULL;
    bool has_bt_error = strstr(buf, "\"bt_error\"") != NULL;
    bool has_fallback = strstr(buf, "too large to emit") != NULL ||
                        strstr(buf, "transient allocation failure") != NULL;
    REQUIRE_TRUE(has_bt || has_bt_error || has_fallback);

    cleanup_dir(dir, "app.log");
  }

  g_realloc_fail_at = -1;
}

// An allocator whose calloc() fails once when g_fail_next_calloc is set (and
// clears it), so a test can fail one calloc() during a record while the
// construction and the field-map growth of clog_set_field() succeed first.
static int g_fail_next_calloc = 0;

// The procs of the calloc-failing allocator above.
static void *_fail_next_calloc_malloc(size_t sz) { return malloc(sz); }
static void _fail_next_calloc_free(void *p) { free(p); }
static void *_fail_next_calloc_calloc(size_t n, size_t sz) {
  if (g_fail_next_calloc) {
    g_fail_next_calloc = 0;
    return NULL;
  }
  return calloc(n, sz);
}
static void *_fail_next_calloc_realloc(void *p, size_t sz) {
  return realloc(p, sz);
}

// clog__derive_checked() reports a closed, stale or invalid parent as
// ccol_invalid_args and a failed allocation as ccol_not_enough_memory, without
// ending the process.
TEST(derive, the_checked_derive_reports_a_bad_parent_as_an_error) {
  ccol_memmgmt_procs procs = {
      .malloc = _fail_next_calloc_malloc,
      .free = _fail_next_calloc_free,
      .calloc = _fail_next_calloc_calloc,
      .realloc = _fail_next_calloc_realloc,
  };
  clog parent = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &procs);
  REQUIRE_NE(parent, CLOG_NULL);

  ccol_retval rv_valid = ccol_unexpected_failure;
  clog child = clog__derive_checked(parent, &rv_valid);
  bool child_level_ok =
      child != CLOG_NULL && clog_get_level(child) == CLOG_INFO;

  g_fail_next_calloc = 1;
  ccol_retval rv_oom = ccol_unexpected_failure;
  clog no_memory = clog__derive_checked(parent, &rv_oom);
  g_fail_next_calloc = 0;

  ccol_retval rv_none = ccol_unexpected_failure;
  clog from_none = clog__derive_checked(CLOG_NULL, &rv_none);

  clog_close(parent);
  ccol_retval rv_closed = ccol_unexpected_failure;
  clog from_closed = clog__derive_checked(parent, &rv_closed);
  ccol_retval rv_bogus = ccol_unexpected_failure;
  clog from_bogus =
      clog__derive_checked((clog)0x7fffffff00000001ULL, &rv_bogus);

  ccol_retval rv_grandchild = ccol_unexpected_failure;
  clog grandchild = child != CLOG_NULL
                        ? clog__derive_checked(child, &rv_grandchild)
                        : CLOG_NULL;
  if (grandchild != CLOG_NULL) clog_close(grandchild);
  if (child != CLOG_NULL) clog_close(child);

  REQUIRE_EQ(rv_valid, ccol_success);
  REQUIRE_TRUE(child_level_ok);
  REQUIRE_EQ(rv_oom, ccol_not_enough_memory);
  REQUIRE_EQ(no_memory, CLOG_NULL);
  REQUIRE_EQ(rv_none, ccol_invalid_args);
  REQUIRE_EQ(from_none, CLOG_NULL);
  REQUIRE_EQ(rv_closed, ccol_invalid_args);
  REQUIRE_EQ(from_closed, CLOG_NULL);
  REQUIRE_EQ(rv_bogus, ccol_invalid_args);
  REQUIRE_EQ(from_bogus, CLOG_NULL);
  REQUIRE_EQ(rv_grandchild, ccol_success);
  REQUIRE_NE(grandchild, CLOG_NULL);
}

// A record walks its fields without allocating, so a calloc() that fails while
// it is built can't turn it into the "transient allocation failure"
// placeholder.
TEST(robustness, a_failed_calloc_during_a_record_keeps_its_fields) {
  ccol_memmgmt_procs procs = {
      .malloc = _fail_next_calloc_malloc,
      .free = _fail_next_calloc_free,
      .calloc = _fail_next_calloc_calloc,
      .realloc = _fail_next_calloc_realloc,
  };

  const clog_format formats[] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON,
                                 CLOG_FMT_SYSLOG};
  for (size_t fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
    REQUIRE_NE(lg, CLOG_NULL);
    clog_set_format(lg, formats[fi]);

    clog_set_field(lg, "service", "auth");
    clog_set_field(lg, "env", "prod");

    g_fail_next_calloc = 1;
    clog_info(lg, "message that must not silently lose its fields");
    g_fail_next_calloc = 0;

    clog_close(lg);

    char buf[4096];
    size_t len = read_file(path, buf, sizeof(buf));
    REQUIRE_GT(len, (size_t)0);

    bool has_service = strstr(buf, "service") != NULL;
    bool has_env = strstr(buf, "env") != NULL;
    bool has_message =
        strstr(buf, "message that must not silently lose its fields") != NULL;
    bool has_alloc_failure_notice =
        strstr(buf, "transient allocation failure") != NULL;
    cleanup_dir(dir, "app.log");
    REQUIRE_TRUE(has_service);
    REQUIRE_TRUE(has_env);
    REQUIRE_TRUE(has_message);
    REQUIRE_FALSE(has_alloc_failure_notice);
  }
}

// An allocator whose malloc() fails once when g_fail_next_malloc is set (and
// clears it), so a test can fail one heap allocation inside one clog__write()
// while construction succeeds.
static int g_fail_next_malloc = 0;
static void *_fail_next_malloc_malloc(size_t sz) {
  if (g_fail_next_malloc) {
    g_fail_next_malloc = 0;
    return NULL;
  }
  return malloc(sz);
}
static void _fail_next_malloc_free(void *p) { free(p); }
static void *_fail_next_malloc_calloc(size_t n, size_t sz) {
  return calloc(n, sz);
}
static void *_fail_next_malloc_realloc(void *p, size_t sz) {
  return realloc(p, sz);
}

// clog_build_header() spills a long file path to the heap. When that malloc()
// fails, the record must name an allocation failure, never "log record too
// large to emit". Only a direct clog__write() call with a long synthetic file
// argument (512 bytes or more) reaches that path.
TEST(robustness, header_build_allocation_failure_reported_accurately) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _fail_next_malloc_malloc,
      .free = _fail_next_malloc_free,
      .calloc = _fail_next_malloc_calloc,
      .realloc = _fail_next_malloc_realloc,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_file[600];
  memset(long_file, 'A', sizeof(long_file));
  long_file[sizeof(long_file) - 1] = '\0';

  g_fail_next_malloc = 1;
  clog__write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello world");
  g_fail_next_malloc = 0;

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "transient allocation failure"), NULL);
  REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);

  cleanup_dir(dir, "app.log");
}

// A src value longer than the 512-byte stack buffer, holding a space and '=',
// must still be quoted, so the record stays one well-formed logfmt line. A
// direct clog__write() call controls the file argument's length.
TEST(robustness, long_file_path_in_src_field_is_still_quoted_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_file[600];
  memset(long_file, 'A', sizeof(long_file));
  long_file[0] = ' ';
  long_file[100] = '=';
  long_file[sizeof(long_file) - 1] = '\0';

  clog__write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello %s",
              "world");

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);

  REQUIRE_EQ(strstr(buf, " src= "), NULL);
  REQUIRE_NE(strstr(buf, " src=\""), NULL);
  REQUIRE_NE(strstr(buf, "msg=\"hello world\""), NULL);

  cleanup_dir(dir, "app.log");
}

// A width and precision whose output passes INT_MAX make vsnprintf() fail with
// no content (glibc: -1, EOVERFLOW). The logger writes a placeholder that names
// the cause instead of nothing. The width comes from a volatile read, so the
// compiler can't detect the overflow.
TEST(robustness, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  volatile int huge_width_source = 2000000000;
  int huge_width = huge_width_source;
  clog__write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "log message formatting failed"), NULL);

  cleanup_dir(dir, "app.log");
}

// JSON output format
TEST(json, default_format_is_logfmt) {
  clog lg = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);
  clog_close(lg);
}

TEST(json, get_set_round_trip) {
  clog lg = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);
  clog_set_format(lg, CLOG_FMT_JSON);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_JSON);
  clog_set_format(lg, CLOG_FMT_LOGFMT);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);

  clog_close(lg);
}

TEST(json, basic_json_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  REQUIRE_NE(strstr(buf, "\"ts\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"level\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"proc\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"src\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"func\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"msg\":"), NULL);
  REQUIRE_NE(strstr(buf, "\"INFO\""), NULL);
  REQUIRE_NE(strstr(buf, "hello world"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, all_levels_in_json_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_trace(lg, "t");
  clog_debug(lg, "d");
  clog_info(lg, "i");
  clog_warn(lg, "w");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"TRACE\""), NULL);
  REQUIRE_NE(strstr(buf, "\"DEBUG\""), NULL);
  REQUIRE_NE(strstr(buf, "\"INFO\""), NULL);
  REQUIRE_NE(strstr(buf, "\"WARN\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, fields_are_top_level_json_keys) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "env", "prod");
  clog_set_field(lg, "service", "auth");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"env\":\"prod\""), NULL);
  REQUIRE_NE(strstr(buf, "\"service\":\"auth\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, special_chars_escaped_in_message) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_info(lg, "say \"hi\"\nworld");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\\\""), NULL);
  REQUIRE_NE(strstr(buf, "\\n"), NULL);
  char *first_nl = strchr(buf, '\n');
  REQUIRE_NE(first_nl, NULL);
  REQUIRE_EQ(*(first_nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

TEST(json, special_chars_escaped_in_field_value) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "path", "C:\\Users\\foo");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "C:\\\\Users\\\\foo"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, del_byte_escaped_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  clog_info(lg,
            "msg\x7f"
            "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  REQUIRE_NE(strstr(buf, "\\u007f"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, valid_multibyte_utf8_passed_through_unescaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *msg =
      "caf\xc3\xa9 costs \xe2\x82\xac"
      "1 \xf0\x9f\x98\x80";
  clog_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, msg), NULL);
  REQUIRE_EQ(strstr(buf, "\\ufffd"), NULL);

  cleanup_dir(dir, "app.log");
}

// Logs, in format fmt, one message and one field value that hold an encoded C1
// control (U+009B), an overlong '/' (0xc0 0xaf) and a well-formed e-acute, and
// reads the whole output into buf (cap bytes), so the UTF-8 test can check
// every format with the same input.
static void log_c1_and_invalid_utf8(clog_format fmt, char *buf, size_t cap) {
  int pipefd[2];
  if (pipe(pipefd) != 0) {
    buf[0] = '\0';
    return;
  }
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(pipefd[0]);
    close(pipefd[1]);
    buf[0] = '\0';
    return;
  }
  clog_set_format(lg, fmt);
  clog_set_field(lg, "k",
                 "v\xc2\x9b"
                 "w\xc0\xaf"
                 "x\xc3\xa9"
                 "y");
  clog_set_field(lg, "plain", "caf\xc3\xa9");
  clog_info(lg, "%s",
            "a\xc2\x9b"
            "b\xc0\xaf"
            "c\xc3\xa9"
            "d");
  drain_pipe(lg, pipefd[0], pipefd[1], buf, cap);
}

// Every format writes a C1 control as \u0080 to \u009f, a byte that isn't
// well-formed UTF-8 as U+FFFD, and passes a well-formed character through.
TEST(utf8, c1_controls_are_escaped_and_invalid_bytes_replaced_in_every_format) {
  char lf[4096], js[4096], sl[4096];
  log_c1_and_invalid_utf8(CLOG_FMT_LOGFMT, lf, sizeof(lf));
  log_c1_and_invalid_utf8(CLOG_FMT_JSON, js, sizeof(js));
  log_c1_and_invalid_utf8(CLOG_FMT_SYSLOG, sl, sizeof(sl));

  const char *outs[] = {lf, js, sl};
  for (size_t i = 0; i < 3; i++) {
    REQUIRE_EQ(strstr(outs[i], "\xc2\x9b"), NULL);
    REQUIRE_EQ(strchr(outs[i], '\xc0'), NULL);
    REQUIRE_EQ(strchr(outs[i], '\xaf'), NULL);
  }

  REQUIRE_NE(strstr(lf,
                    "msg=\"a\\u009bb\xef\xbf\xbd\xef\xbf\xbd"
                    "c\xc3\xa9"
                    "d\""),
             NULL);
  REQUIRE_NE(strstr(lf,
                    " k=\"v\\u009bw\xef\xbf\xbd\xef\xbf\xbd"
                    "x\xc3\xa9"
                    "y\""),
             NULL);
  REQUIRE_NE(strstr(lf, " plain=caf\xc3\xa9 "), NULL);

  REQUIRE_NE(strstr(js,
                    "\"msg\":\"a\\u009bb\\ufffd\\ufffdc\xc3\xa9"
                    "d\""),
             NULL);
  REQUIRE_NE(strstr(js,
                    "\"k\":\"v\\u009bw\\ufffd\\ufffdx\xc3\xa9"
                    "y\""),
             NULL);

  REQUIRE_NE(strstr(sl,
                    "a\\u009bb\xef\xbf\xbd\xef\xbf\xbd"
                    "c\xc3\xa9"
                    "d"),
             NULL);
  REQUIRE_NE(strstr(sl,
                    "k=\"v\\u009bw\xef\xbf\xbd\xef\xbf\xbd"
                    "x\xc3\xa9"
                    "y\""),
             NULL);
}

// The escaper of a syslog MSG and of a backtrace frame escapes a C1 control at
// either end of the range, replaces an invalid byte with U+FFFD and passes
// U+00A0 through, as the value escapers do.
TEST(utf8, ctrl_escaper_handles_c1_range_bounds) {
  char out[256];
  clog__test_append_ctrl_escaped("\xc2\x80|\xc2\x9f|\xc2\xa0|\xc2|\xff", out,
                                 sizeof(out));
  REQUIRE_STREQ(out, "\\u0080|\\u009f|\xc2\xa0|\xef\xbf\xbd|\xef\xbf\xbd");
}

TEST(json, invalid_utf8_lone_continuation_byte_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *msg =
      "before\x80"
      "after";
  clog_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "before\\ufffdafter"), NULL);
  REQUIRE_EQ(memchr(buf, 0x80, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_overlong_encoding_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *msg =
      "before\xc0\x80"
      "after";
  clog_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xc0, strlen(buf)), NULL);
  REQUIRE_EQ(memchr(buf, 0x80, strlen(buf)), NULL);
  REQUIRE_NE(strstr(buf, "before"), NULL);
  REQUIRE_NE(strstr(buf, "after"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_surrogate_half_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *msg =
      "before\xed\xa0\x80"
      "after";
  clog_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xed, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_truncated_sequence_at_end_of_message_replaced) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const char *msg = "trunc\xe2";
  clog_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "trunc\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xe2, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_in_field_value_is_also_sanitized) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "k",
                 "v\xff"
                 "end");

  clog_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"k\":\"v\\ufffdend\""), NULL);
  REQUIRE_EQ(memchr(buf, 0xff, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, error_has_inline_bt_array) {
  BT_SKIP_WITHOUT_SUPPORT();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_error(lg, "something failed");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"bt\":["), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

// A capture that succeeds with depth <= CLOG_BT_INITIAL_FRAME is a real, empty
// backtrace: The record holds "bt":[] and no bt_error. The
// clog__test_force_shallow_backtrace_depth() hook forces that depth.
TEST(json, error_with_shallow_backtrace_capture_still_yields_empty_bt_array) {
  BT_SKIP_WITHOUT_SUPPORT();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog__test_force_shallow_backtrace_depth(true);
  clog_error(lg, "something failed");
  clog__test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"bt\":[]"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, format_shared_with_derived_logger) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog_set_format(parent, CLOG_FMT_JSON);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  REQUIRE_EQ(clog_get_format(child), CLOG_FMT_JSON);

  clog_info(child, "child json");
  clog_info(parent, "parent json");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  int json_lines = 0;
  char *line = buf;
  while (*line) {
    if (*line == '{') json_lines++;
    char *nl = strchr(line, '\n');
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(json_lines, 2);

  cleanup_dir(dir, "app.log");
}

TEST(output, alert_level_appears_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_alert(lg, "alert message");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "alert message"), NULL);
  if (backtrace_capture_genuinely_available()) {
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }

  cleanup_dir(dir, "app.log");
}

TEST(output, error_and_alert_level_strings_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_error(lg, "error event");
  clog_alert(lg, "alert event");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ERROR"), NULL);
  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "error event"), NULL);
  REQUIRE_NE(strstr(buf, "alert event"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, alert_produces_backtrace) {
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_alert(lg, "critical failure");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "critical failure"), NULL);
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, set_format_on_derived_affects_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  clog_set_format(child, CLOG_FMT_JSON);
  REQUIRE_EQ(clog_get_format(parent), CLOG_FMT_JSON);

  clog_info(parent, "parent line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(buf[0], '{');

  cleanup_dir(dir, "app.log");
}

TEST(json, error_and_alert_level_strings_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_error(lg, "e");
  clog_alert(lg, "a");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"ERROR\""), NULL);
  REQUIRE_NE(strstr(buf, "\"ALERT\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, alert_has_inline_bt_array) {
  BT_SKIP_WITHOUT_SUPPORT();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_alert(lg, "alert event");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));

  int bt_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "\"bt\":[")) != NULL) {
    bt_count++;
    p++;
  }
  REQUIRE_EQ(bt_count, 1);

  REQUIRE_EQ(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, tab_and_cr_in_field_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_set_field(lg, "data", "col1\tcol2\r\n");

  clog_info(lg, "tab and cr test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, '\t', strlen(buf)), NULL);
  REQUIRE_EQ(memchr(buf, '\r', strlen(buf)), NULL);

  REQUIRE_NE(strstr(buf, "\\t"), NULL);
  REQUIRE_NE(strstr(buf, "\\r"), NULL);

  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

// RFC 5424 syslog format

// The logger of a child that tests clog_fatal, which calls exit(). Only a
// child sets it, after the fork, so _fatal_child_cleanup() does nothing in the
// parent.
static clog _g_fatal_child_lg = CLOG_NULL;
// Closes _g_fatal_child_lg when it is set. A child registers it with atexit(),
// so valgrind doesn't report the logger's allocations as still reachable.
static void _fatal_child_cleanup(void) {
  if (_g_fatal_child_lg) {
    clog_close(_g_fatal_child_lg);
    _g_fatal_child_lg = CLOG_NULL;
  }
}

TEST(syslog, basic_structure) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_info(lg, "hello syslog");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<14>1 "), NULL);
  REQUIRE_NE(strstr(buf, "[ccol "), NULL);
  REQUIRE_NE(strstr(buf, "proc=\""), NULL);
  REQUIRE_NE(strstr(buf, "src=\""), NULL);
  REQUIRE_NE(strstr(buf, "func=\""), NULL);
  REQUIRE_NE(strstr(buf, " INFO "), NULL);
  REQUIRE_NE(strstr(buf, "hello syslog"), NULL);
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

// Facility USER (1): WARN 4 gives PRI 12, ERROR 3 gives 11, ALERT 1 gives 9.
TEST(syslog, severity_encoding) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_warn(lg, "w");
  clog_error(lg, "e");
  clog_alert(lg, "a");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<12>"), NULL);
  REQUIRE_NE(strstr(buf, "<11>"), NULL);
  REQUIRE_NE(strstr(buf, "<9>"), NULL);
}

TEST(syslog, facility_change) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  REQUIRE_EQ(clog_get_facility(lg), CLOG_SYSLOG_USER);

  clog_set_facility(lg, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(clog_get_facility(lg), CLOG_SYSLOG_DAEMON);

  clog_info(lg, "daemon log");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<30>"), NULL);
  REQUIRE_NE(strstr(buf, "daemon log"), NULL);
}

// A facility clog_syslog_facility doesn't name is refused with
// ccol_invalid_args and the facility stays, so the PRI stays inside RFC 5424's
// 0 to 191.
TEST(syslog, a_facility_outside_the_enum_is_refused) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_retval rv_daemon = clog_set_facility(lg, CLOG_SYSLOG_DAEMON);

  ccol_retval rv_99 = clog_set_facility(lg, (clog_syslog_facility)99);
  clog_syslog_facility after_99 = clog_get_facility(lg);
  ccol_retval rv_minus_1 = clog_set_facility(lg, (clog_syslog_facility)-1);
  clog_syslog_facility after_minus_1 = clog_get_facility(lg);
  ccol_retval rv_24 = clog_set_facility(lg, (clog_syslog_facility)24);
  clog_syslog_facility after_24 = clog_get_facility(lg);
  ccol_retval rv_13 = clog_set_facility(lg, (clog_syslog_facility)13);
  clog_syslog_facility after_13 = clog_get_facility(lg);

  clog_info(lg, "daemon log");
  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_EQ(rv_daemon, ccol_success);
  REQUIRE_EQ(rv_99, ccol_invalid_args);
  REQUIRE_EQ(rv_minus_1, ccol_invalid_args);
  REQUIRE_EQ(rv_24, ccol_invalid_args);
  REQUIRE_EQ(rv_13, ccol_invalid_args);
  REQUIRE_EQ(after_99, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(after_minus_1, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(after_24, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(after_13, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(strncmp(buf, "<30>1 ", 6), 0);
}

// A format clog_format doesn't name is refused with ccol_invalid_args and the
// format stays as it is.
TEST(syslog, a_format_outside_the_enum_is_refused) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  ccol_retval rv_json = clog_set_format(lg, CLOG_FMT_JSON);

  ccol_retval rv_9 = clog_set_format(lg, (clog_format)9);
  clog_format after_9 = clog_get_format(lg);
  ccol_retval rv_minus_1 = clog_set_format(lg, (clog_format)-1);
  clog_format after_minus_1 = clog_get_format(lg);

  clog_info(lg, "still json");
  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_EQ(rv_json, ccol_success);
  REQUIRE_EQ(rv_9, ccol_invalid_args);
  REQUIRE_EQ(rv_minus_1, ccol_invalid_args);
  REQUIRE_EQ(after_9, CLOG_FMT_JSON);
  REQUIRE_EQ(after_minus_1, CLOG_FMT_JSON);
  REQUIRE_EQ(buf[0], '{');
}

TEST(syslog, fields_in_sd) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_set_field(lg, "service", "auth");
  clog_set_field(lg, "env", "prod");

  clog_info(lg, "structured");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "service=\"auth\""), NULL);
  REQUIRE_NE(strstr(buf, "env=\"prod\""), NULL);
  REQUIRE_NE(strstr(buf, "] structured"), NULL);
}

// RFC 5424 limits an SD-PARAM-NAME to 32 characters. A longer key becomes its
// first 23 characters, '~' and an 8-digit hex hash of the whole key, so two
// long keys with a common prefix don't collide.
TEST(syslog, sd_param_name_over_32_chars_is_shortened_with_a_stable_suffix) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456789", "val");

  clog_info(lg, "truncation");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  char *name_start = strstr(buf, "abcdefghijklmnopqrstuv");
  REQUIRE_NE(name_start, NULL);

  char *eq = strchr(name_start, '=');
  REQUIRE_NE(eq, NULL);
  REQUIRE_TRUE((size_t)(eq - name_start) <= 32);
  REQUIRE_EQ(strstr(buf, "abcdefghijklmnopqrstuvwxyz_123456789="), NULL);
}

TEST(syslog, long_keys_sharing_prefix_get_distinct_sd_param_names) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_ONE", "one");
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_TWO", "two");

  clog_info(lg, "collision test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  char *one_val = strstr(buf, "=\"one\"");
  char *two_val = strstr(buf, "=\"two\"");
  REQUIRE_NE(one_val, NULL);
  REQUIRE_NE(two_val, NULL);

  char *one_name_start = one_val;
  while (one_name_start > buf && one_name_start[-1] != ' ') one_name_start--;
  char *two_name_start = two_val;
  while (two_name_start > buf && two_name_start[-1] != ' ') two_name_start--;

  size_t one_len = (size_t)(one_val - one_name_start);
  size_t two_len = (size_t)(two_val - two_name_start);

  bool same = (one_len == two_len) &&
              memcmp(one_name_start, two_name_start, one_len) == 0;
  REQUIRE_FALSE(same);
}

TEST(syslog, file_logger_rejects_syslog_format) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  ccol_retval rv_syslog = clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_format after_syslog = clog_get_format(lg);
  ccol_retval rv_json = clog_set_format(lg, CLOG_FMT_JSON);
  clog_format after_json = clog_get_format(lg);
  ccol_retval rv_logfmt = clog_set_format(lg, CLOG_FMT_LOGFMT);

  clog_info(lg, "still logfmt");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(rv_syslog, ccol_invalid_args);
  REQUIRE_EQ(after_syslog, CLOG_FMT_LOGFMT);
  REQUIRE_EQ(rv_json, ccol_success);
  REQUIRE_EQ(after_json, CLOG_FMT_JSON);
  REQUIRE_EQ(rv_logfmt, ccol_success);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_EQ(strstr(buf, "<14>"), NULL);
}

TEST(syslog, backtrace_as_separate_messages) {
  if (!backtrace_capture_genuinely_available()) return;
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_ERROR, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_error(lg, "with backtrace");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  int msg_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "<11>1 ")) != NULL) {
    msg_count++;
    p++;
  }
  REQUIRE_GT(msg_count, 1);

  REQUIRE_NE(strstr(buf, " - #0 "), NULL);
  REQUIRE_EQ(strchr(buf, '\t'), NULL);
}

// clog_fatal ends the process, so a child logs it to a file (an fd logger that
// doesn't own the fd may use syslog) and the parent reads the record: PRI 8 is
// USER (1) * 8 + severity 0.
TEST(syslog, fatal_severity_encoding) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);

  clog lg = clog_create_from_fd(fd, CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal syslog event");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  waitpid(pid, &status, 0);
  clog_close(lg);
  close(fd);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<8>"), NULL);
  REQUIRE_NE(strstr(buf, "fatal syslog event"), NULL);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_NE(WEXITSTATUS(status), 0);

  cleanup_dir(dir, "app.log");
}

TEST(syslog, sd_param_value_special_chars_escaped) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "bracket", "val]end");
  clog_set_field(lg, "backslash", "C:\\foo");
  clog_set_field(lg, "quote", "say \"hi\"");

  clog_info(lg, "sd escape test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\\]"), NULL);
  REQUIRE_NE(strstr(buf, "C:\\\\foo"), NULL);
  REQUIRE_NE(strstr(buf, "\\\"hi\\\""), NULL);
}

// A raw newline in the message would split one syslog record into two, and the
// second line would pass for a record of its own (log injection).
TEST(syslog, message_with_embedded_newline_stays_single_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_info(lg, "first part\nFAKE-INJECTED-LINE data=1");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, 1);
  REQUIRE_EQ(buf[n - 1], '\n');

  REQUIRE_NE(strstr(buf, "first part\\nFAKE-INJECTED-LINE"), NULL);
  REQUIRE_EQ(strstr(buf, "\nFAKE-INJECTED-LINE"), NULL);
}

TEST(syslog, field_value_with_embedded_newline_stays_single_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "payload", "line1\nFAKE-INJECTED-LINE");
  clog_info(lg, "structured");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, 1);

  REQUIRE_NE(strstr(buf, "payload=\"line1\\nFAKE-INJECTED-LINE\""), NULL);
}

TEST(syslog, sd_value_cr_and_tab_are_escaped) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "data", "a\rb\tc");
  clog_info(lg, "cr and tab test");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_EQ(memchr(buf, '\r', n), NULL);
  REQUIRE_EQ(memchr(buf, '\t', n), NULL);
  REQUIRE_NE(strstr(buf, "a\\rb\\tc"), NULL);
}

// Additional lifecycle
TEST(lifecycle, open_file_appends_to_existing) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg1 = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg1, CLOG_NULL);
  clog_info(lg1, "first open");
  clog_close(lg1);

  clog lg2 = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg2, CLOG_NULL);
  clog_info(lg2, "second open");
  clog_close(lg2);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "first open"), NULL);
  REQUIRE_NE(strstr(buf, "second open"), NULL);

  cleanup_dir(dir, "app.log");
}

// Additional filtering
TEST(filtering, set_level_to_off_suppresses_everything) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "before off - visible");

  clog_set_level(lg, CLOG_OFF);
  REQUIRE_EQ(clog_get_level(lg), CLOG_OFF);

  clog_alert(lg, "after off - suppressed");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "before off - visible"), NULL);
  REQUIRE_EQ(strstr(buf, "after off - suppressed"), NULL);

  cleanup_dir(dir, "app.log");
}

// Additional field edge cases
TEST(fields, remove_nonexistent_field_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_remove_field(lg, "nonexistent");
  clog_remove_field(lg, "also_never_set");

  clog_info(lg, "still works");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "still works"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, clear_empty_fields_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_clear_fields(lg);
  clog_clear_fields(lg);

  clog_info(lg, "still works");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "still works"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, null_key_or_value_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  ccol_retval rv_null_key = clog_set_field(lg, NULL, "v");
  ccol_retval rv_null_value = clog_set_field(lg, "k", NULL);
  ccol_retval rv_null_both = clog_set_field(lg, NULL, NULL);
  clog_remove_field(lg, NULL);

  ccol_retval rv_good = clog_set_field(lg, "good", "yes");
  clog_info(lg, "null field test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(rv_null_key, ccol_invalid_args);
  REQUIRE_EQ(rv_null_value, ccol_invalid_args);
  REQUIRE_EQ(rv_null_both, ccol_invalid_args);
  REQUIRE_EQ(rv_good, ccol_success);
  REQUIRE_EQ(strstr(buf, "k="), NULL);

  REQUIRE_NE(strstr(buf, "good=yes"), NULL);

  cleanup_dir(dir, "app.log");
}

// An allocator that refuses every allocation while g_field_alloc_refuse is set
// and behaves like the C library otherwise, so a test can make the copy of a
// field fail after the logger exists.
static atomic_bool g_field_alloc_refuse = false;
static void *_field_alloc_malloc(size_t sz) {
  return atomic_load(&g_field_alloc_refuse) ? NULL : malloc(sz);
}
static void _field_alloc_free(void *p) { free(p); }
static void *_field_alloc_calloc(size_t n, size_t sz) {
  return atomic_load(&g_field_alloc_refuse) ? NULL : calloc(n, sz);
}
static void *_field_alloc_realloc(void *p, size_t sz) {
  return atomic_load(&g_field_alloc_refuse) ? NULL : realloc(p, sz);
}

// The values are longer than the field map's inline storage, so a new key and
// a replacement both need an allocation; when it fails, a new key isn't added
// and an existing key keeps its value.
TEST(fields, set_field_reports_a_failed_allocation_and_changes_nothing) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _field_alloc_malloc,
      .free = _field_alloc_free,
      .calloc = _field_alloc_calloc,
      .realloc = _field_alloc_realloc,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  ccol_retval rv_old =
      clog_set_field(lg, "kept", "the-old-value-that-must-stay-in-place");

  atomic_store(&g_field_alloc_refuse, true);
  ccol_retval rv_replace =
      clog_set_field(lg, "kept", "a-new-value-that-cannot-be-copied-here");
  ccol_retval rv_new_key = clog_set_field(lg, "added", "no");
  atomic_store(&g_field_alloc_refuse, false);

  clog_info(lg, "after the failed copies");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(rv_old, ccol_success);
  REQUIRE_EQ(rv_replace, ccol_not_enough_memory);
  REQUIRE_EQ(rv_new_key, ccol_not_enough_memory);
  REQUIRE_NE(strstr(buf, "kept=the-old-value-that-must-stay-in-place"), NULL);
  REQUIRE_EQ(strstr(buf, "a-new-value-that-cannot-be-copied-here"), NULL);
  REQUIRE_EQ(strstr(buf, "added="), NULL);

  cleanup_dir(dir, "app.log");
}

// Additional derive
TEST(derive, inherits_min_level_from_parent) {
  clog parent = clog_create_from_fd_mp(STDERR_FILENO, CLOG_WARN, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  REQUIRE_EQ(clog_get_level(child), CLOG_WARN);

  clog_close(child);
  clog_close(parent);
}

// Additional syslog
TEST(syslog, syslog_format_inherited_by_derived_logger) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog parent = clog_create_from_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(parent, CLOG_NULL);
  clog_set_format(parent, CLOG_FMT_SYSLOG);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_NULL);

  REQUIRE_EQ(clog_get_format(child), CLOG_FMT_SYSLOG);

  clog_info(child, "child syslog");

  char buf[4096];
  drain_pipe(parent, pipefd[0], pipefd[1], buf, sizeof(buf));
  clog_close(child);

  REQUIRE_NE(strstr(buf, "<14>1 "), NULL);
  REQUIRE_NE(strstr(buf, "child syslog"), NULL);
}

// The PRINTUSASCII filter of APP-NAME (and HOSTNAME, which shares it) drops
// each bad byte wherever it is instead of stopping at the first one.
TEST(syslog, appname_sanitizer_filters_not_truncates) {
  char out[49];

  clog__test_sanitize_syslog_appname(
      "ab\x01"
      "cd",
      out, sizeof(out));
  REQUIRE_STREQ(out, "abcd");

  clog__test_sanitize_syslog_appname("\x01\x02", out, sizeof(out));
  REQUIRE_STREQ(out, "-");
  clog__test_sanitize_syslog_appname("", out, sizeof(out));
  REQUIRE_STREQ(out, "-");

  char small_out[4];
  clog__test_sanitize_syslog_appname("abcdefgh", small_out, sizeof(small_out));
  REQUIRE_EQ(strlen(small_out), (size_t)3);
  REQUIRE_STREQ(small_out, "abc");
}

// With outsz 0, outsz - 1 wraps to SIZE_MAX, and with outsz 1 the two-byte "-"
// fallback doesn't fit. Every buffer is a heap block of exactly outsz bytes,
// so valgrind reports an overflow that a larger stack array would hide.
TEST(syslog, appname_sanitizer_tolerates_degenerate_output_sizes) {
  char *zero_out = malloc(1);
  REQUIRE_NE(zero_out, NULL);
  zero_out[0] = 'Z';
  clog__test_sanitize_syslog_appname("abc", zero_out, 0);
  REQUIRE_EQ(zero_out[0], 'Z');
  free(zero_out);

  char *one_out = malloc(1);
  REQUIRE_NE(one_out, NULL);
  clog__test_sanitize_syslog_appname("abc", one_out, 1);
  REQUIRE_EQ(one_out[0], '\0');
  free(one_out);

  char *one_out_empty = malloc(1);
  REQUIRE_NE(one_out_empty, NULL);
  clog__test_sanitize_syslog_appname("", one_out_empty, 1);
  REQUIRE_EQ(one_out_empty[0], '\0');
  free(one_out_empty);

  char *two_out = malloc(2);
  REQUIRE_NE(two_out, NULL);
  clog__test_sanitize_syslog_appname("\x01\x02", two_out, 2);
  REQUIRE_STREQ(two_out, "-");
  free(two_out);
}

// The syslog MSG and the backtrace frames of logfmt and syslog share this
// escaper. A test can't put a control byte into backtrace_symbols() output, so
// it drives the escaper directly.
TEST(syslog, ctrl_escaped_helper_neutralizes_control_bytes) {
  char out[64];

  clog__test_append_ctrl_escaped("plain text, no escaping needed", out,
                                 sizeof(out));
  REQUIRE_STREQ(out, "plain text, no escaping needed");

  clog__test_append_ctrl_escaped("line1\nline2", out, sizeof(out));
  REQUIRE_STREQ(out, "line1\\nline2");
  clog__test_append_ctrl_escaped("a\rb\tc", out, sizeof(out));
  REQUIRE_STREQ(out, "a\\rb\\tc");

  clog__test_append_ctrl_escaped("x\\ny", out, sizeof(out));
  REQUIRE_STREQ(out, "x\\\\ny");

  clog__test_append_ctrl_escaped(
      "del\x7f"
      "end",
      out, sizeof(out));
  REQUIRE_STREQ(out, "del\\x7fend");
  clog__test_append_ctrl_escaped(
      "a\x01"
      "b",
      out, sizeof(out));
  REQUIRE_STREQ(out, "a\\x01b");

  char small_out[6];
  clog__test_append_ctrl_escaped("ab\ncd", small_out, sizeof(small_out));
  REQUIRE_EQ(strlen(small_out), (size_t)5);
  REQUIRE_STREQ(small_out, "ab\\nc");
}

// Additional threading

// DERIVED_THREAD_COUNT threads each derive a child and log
// DERIVED_MSGS_PER_THREAD records through it.
#define DERIVED_THREAD_COUNT 4
#define DERIVED_MSGS_PER_THREAD 30

// The parent logger and the number of one derived writer.
typedef struct {
  clog lg;
  int id;
} derived_arg_t;

// Derives a child of a->lg, gives it a field, logs DERIVED_MSGS_PER_THREAD
// records through it and closes it, one of the threads that write beside the
// parent in the garbled-line test.
static void *_derived_writer_thread(void *arg) {
  derived_arg_t *a = (derived_arg_t *)arg;
  clog child = clog_derive(a->lg);
  if (!child) return NULL;
  clog_set_field(child, "writer", "yes");
  for (int i = 0; i < DERIVED_MSGS_PER_THREAD; i++)
    clog_info(child, "derived id=%d msg=%d", a->id, i);
  clog_close(child);
  return NULL;
}

TEST(threading, concurrent_parent_and_derived_writers_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);
  clog_set_field(parent, "role", "parent");

  pthread_t threads[DERIVED_THREAD_COUNT];
  derived_arg_t args[DERIVED_THREAD_COUNT];
  bool created[DERIVED_THREAD_COUNT];
  int create_failures = 0;

  for (int i = 0; i < DERIVED_THREAD_COUNT; i++) {
    args[i].lg = parent;
    args[i].id = i;
    created[i] = (pthread_create(&threads[i], NULL, _derived_writer_thread,
                                 &args[i]) == 0);
    if (!created[i]) create_failures++;
  }

  for (int i = 0; i < DERIVED_MSGS_PER_THREAD; i++)
    clog_info(parent, "parent msg=%d", i);

  for (int i = 0; i < DERIVED_THREAD_COUNT; i++)
    if (created[i]) pthread_join(threads[i], NULL);

  if (create_failures > 0) {
    clog_close(parent);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(parent);

  char buf[524288];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_NE(len, (size_t)0);

  int bad_lines = 0;
  char *line = buf;
  while (line < buf + len) {
    char *nl = strchr(line, '\n');
    if (line[0] != '\t' && strncmp(line, "ts=", 3) != 0) bad_lines++;
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

// Writers that log filtered TRACE records while a toggler flips the level.
#define LEVEL_FASTPATH_WRITER_COUNT 4
#define LEVEL_FASTPATH_ITERATIONS 500

// Logs LEVEL_FASTPATH_ITERATIONS TRACE records through the logger at arg, to
// race the unlocked level check against the toggler.
static void *_level_fastpath_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < LEVEL_FASTPATH_ITERATIONS; i++)
    clog_trace(lg, "filtered %d", i);
  return NULL;
}

// Flips the level of the logger at arg between TRACE and INFO, so the writers'
// unlocked level check races clog_set_level().
static void *_level_fastpath_toggler(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < LEVEL_FASTPATH_ITERATIONS; i++)
    clog_set_level(lg, (i % 2) ? CLOG_TRACE : CLOG_INFO);
  return NULL;
}

// clog__write() checks min_level once without a lock, then again under the
// lock. This stresses that unlocked check against clog_set_level() calls from
// another thread: No crash, and every line that passes is a whole record.
TEST(threading, concurrent_filtered_writes_and_level_changes_no_crash) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t writers[LEVEL_FASTPATH_WRITER_COUNT], toggler;
  bool writer_created[LEVEL_FASTPATH_WRITER_COUNT];
  int create_failures = 0;
  for (int i = 0; i < LEVEL_FASTPATH_WRITER_COUNT; i++) {
    writer_created[i] =
        (pthread_create(&writers[i], NULL, _level_fastpath_writer, &lg) == 0);
    if (!writer_created[i]) create_failures++;
  }
  bool toggler_created =
      (pthread_create(&toggler, NULL, _level_fastpath_toggler, &lg) == 0);
  if (!toggler_created) create_failures++;

  for (int i = 0; i < LEVEL_FASTPATH_WRITER_COUNT; i++)
    if (writer_created[i]) pthread_join(writers[i], NULL);
  if (toggler_created) pthread_join(toggler, NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_set_level(lg, CLOG_INFO);
  clog_info(lg, "final marker");
  clog_close(lg);

  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "final marker"), NULL);

  int bad_lines = 0;
  char *line = buf;
  while (line < buf + len) {
    char *nl = strchr(line, '\n');
    if (line[0] != '\t' && strncmp(line, "ts=", 3) != 0) bad_lines++;
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

// Writers that log while a mutator sets and removes a field on the same
// handle.
#define FIELD_RACE_WRITER_COUNT 4
#define FIELD_RACE_ITERATIONS 500

// Logs FIELD_RACE_ITERATIONS records through the logger at arg while the
// mutator changes its fields.
static void *_field_race_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < FIELD_RACE_ITERATIONS; i++)
    clog_info(lg, "field race %d", i);
  return NULL;
}

// Sets and removes the field "who" on the logger at arg, over and over, to
// race the field reads of the writers.
static void *_field_race_mutator(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < FIELD_RACE_ITERATIONS; i++) {
    clog_set_field(lg, "who", (i % 2) ? "even" : "odd");
    clog_remove_field(lg, "who");
  }
  return NULL;
}

// A record must not read the field map's count before it locks fields_mutex,
// because that read races clog_set_field() and clog_remove_field(). The test
// checks for no crash; the test_tsan run catches the race itself.
TEST(threading, concurrent_set_field_and_write_no_race) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t writers[FIELD_RACE_WRITER_COUNT], mutator;
  bool writer_created[FIELD_RACE_WRITER_COUNT];
  int create_failures = 0;
  for (int i = 0; i < FIELD_RACE_WRITER_COUNT; i++) {
    writer_created[i] =
        (pthread_create(&writers[i], NULL, _field_race_writer, &lg) == 0);
    if (!writer_created[i]) create_failures++;
  }
  bool mutator_created =
      (pthread_create(&mutator, NULL, _field_race_mutator, &lg) == 0);
  if (!mutator_created) create_failures++;

  for (int i = 0; i < FIELD_RACE_WRITER_COUNT; i++)
    if (writer_created[i]) pthread_join(writers[i], NULL);
  if (mutator_created) pthread_join(mutator, NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(lg);

  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  int bad_lines = 0;
  char *line = buf;
  while (line < buf + len) {
    char *nl = strchr(line, '\n');
    if (strncmp(line, "ts=", 3) != 0) bad_lines++;
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

// Writers that resolve one long-lived handle while a deriver keeps taking and
// releasing slots of the handle table.
#define SLOT_CHURN_WRITER_COUNT 4
#define SLOT_CHURN_ITERATIONS 500
#define SLOT_CHURN_DERIVE_ITERATIONS 500

// Logs through the handle at arg and flips and reads its level, over and over,
// so each call resolves and pins the handle while slots churn.
static void *_slot_churn_writer(void *arg) {
  clog *lg = (clog *)arg;
  for (int i = 0; i < SLOT_CHURN_ITERATIONS; i++) {
    clog_info(*lg, "churn writer message %d", i);
    clog_set_level(*lg, (i % 2 == 0) ? CLOG_TRACE : CLOG_INFO);
    (void)clog_get_level(*lg);
  }
  return NULL;
}

// Derives a child of the handle at arg, logs through it and closes it, over
// and over, so slots of the handle table are freed and taken again.
static void *_slot_churn_deriver(void *arg) {
  clog *parent = (clog *)arg;
  for (int i = 0; i < SLOT_CHURN_DERIVE_ITERATIONS; i++) {
    clog child = clog_derive(*parent);
    if (child == CLOG_NULL) continue;
    clog_info(child, "derived churn message %d", i);
    clog_close(child);
  }
  return NULL;
}

// Real, concurrent slot reuse against the slot table and the lock-free pin:
// writers resolve a shared handle while children are derived and closed. It
// asserts nothing about the output; a crash, a hang or a report from valgrind
// or ThreadSanitizer is the failure.
TEST(threading, concurrent_derive_close_churn_stresses_slot_table) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t writers[SLOT_CHURN_WRITER_COUNT], deriver;
  bool writer_created[SLOT_CHURN_WRITER_COUNT];
  int create_failures = 0;
  for (int i = 0; i < SLOT_CHURN_WRITER_COUNT; i++) {
    writer_created[i] =
        (pthread_create(&writers[i], NULL, _slot_churn_writer, &lg) == 0);
    if (!writer_created[i]) create_failures++;
  }
  bool deriver_created =
      (pthread_create(&deriver, NULL, _slot_churn_deriver, &lg) == 0);
  if (!deriver_created) create_failures++;

  for (int i = 0; i < SLOT_CHURN_WRITER_COUNT; i++)
    if (writer_created[i]) pthread_join(writers[i], NULL);
  if (deriver_created) pthread_join(deriver, NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// Fork safety
//
// These tests drive the pthread_atfork() machinery, which the build drops when
// CCOL_FORK_SAFETY_REQUIRED is 0, so they are dropped with it.
#if CCOL_FORK_SAFETY_REQUIRED

// A child that logs through a logger made before the fork() drives the atfork
// protection of the slot table rwlock, shared->mutex and fields_mutex. If the
// child handler only unlocks the rwlock instead of initializing it again, the
// child's next resolve blocks for ever. The child's exit status isn't reliable
// under memtest (valgrind replaces it after its leak check), so only
// WIFEXITED is checked; a hang or an abort still fails.
TEST(fork_safety, child_can_log_after_fork) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = fork();
  if (pid == 0) {
    clog_info(lg, "message from child");
    clog_close(lg);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  REQUIRE_TRUE(WIFEXITED(status));

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "message from child"), NULL);

  cleanup_dir(dir, "app.log");
}

// Writers that log, derive and close while the main thread forks.
#define FORK_CHURN_WRITER_COUNT 4
#define FORK_CHURN_ITERATIONS 300

// Set after the child is reaped. Writers stay alive until then, so no writer
// has finished unjoined at the fork(), which ThreadSanitizer would report in
// the child as a thread leak.
static atomic_int _fork_churn_stop;

// Logs, derives and closes children through the handle at arg, so the fork()
// lands among calls that hold the library's locks, then waits for
// _fork_churn_stop so it isn't a finished thread at the fork.
static void *_fork_churn_writer(void *arg) {
  clog *lg = (clog *)arg;
  for (int i = 0; i < FORK_CHURN_ITERATIONS; i++) {
    clog_info(*lg, "fork churn message %d", i);
    clog child = clog_derive(*lg);
    if (child != CLOG_NULL) clog_close(child);
  }
  struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000};
  while (!atomic_load(&_fork_churn_stop)) nanosleep(&pause, NULL);
  return NULL;
}

// fork() from one thread while others log, derive and close on the same
// target. The parent's writers must finish (clog_atfork_parent unlocks every
// lock that clog_atfork_prepare took), and the child must use the handle.
TEST(fork_safety, concurrent_fork_during_churn_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  atomic_store(&_fork_churn_stop, 0);
  pthread_t writers[FORK_CHURN_WRITER_COUNT];
  bool writer_created[FORK_CHURN_WRITER_COUNT];
  int writer_create_failures = 0;
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++) {
    writer_created[i] =
        (pthread_create(&writers[i], NULL, _fork_churn_writer, &lg) == 0);
    if (!writer_created[i]) writer_create_failures++;
  }
  if (writer_create_failures > 0) {
    atomic_store(&_fork_churn_stop, 1);
    for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
      if (writer_created[i]) pthread_join(writers[i], NULL);
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(writer_create_failures, 0);

  usleep(1000);
  pid_t pid = fork();
  if (pid == 0) {
    clog_info(lg, "post-fork child message");
    test_child_exit(0);
  }
  if (pid == -1) {
    atomic_store(&_fork_churn_stop, 1);
    for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
      pthread_join(writers[i], NULL);
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  atomic_store(&_fork_churn_stop, 1);
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);

  clog_info(lg, "final parent marker");
  clog_close(lg);

  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_TRUE(reaped);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "final parent marker"), NULL);

  cleanup_dir(dir, "app.log");
}

// Appends one line to path, creating the file if needed, so a forked child can
// report how far it got. Its exit code can't do that, because valgrind
// replaces it when it finds a still-reachable block, which a child of a
// running suite always has.
static void _fork_safety_marker_append(const char *path, const char *line) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  ssize_t w = write(fd, line, strlen(line));
  (void)w;
  close(fd);
}

// Makes the first acquisition of a fresh slot fail at the live_shareds
// registration, then forks again, recording each step in marker_path, to
// check that the rollback leaves the slot fork-safe. If it leaves freed ==
// false and ptr == NULL, clog_atfork_prepare() dereferences NULL and the
// process dies inside fork(), so this runs only in a forked child.
static void _fork_safety_rollback_race_child(const char *marker_path) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) test_child_exit(0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog__test_force_next_fresh_slot_registration_failure(true);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_NULL) {
    cleanup_dir(dir, "app.log");
    test_child_exit(0);
  }
  _fork_safety_marker_append(marker_path, "acquire_failed_as_expected\n");

  pid_t pid = fork();
  _fork_safety_marker_append(marker_path, "survived_fork_call\n");

  if (pid == 0) test_child_exit(0);
  if (pid > 0) waitpid(pid, NULL, 0);
  cleanup_dir(dir, "app.log");
  test_child_exit(0);
}

// Makes the publish into the pin index fail after the live_shareds
// registration succeeded, checks that a second open succeeds (so the armed
// failure is what refused the first), then forks, recording each step in
// marker_path. This checks that the rollback unregisters the shared object:
// if it stays in live_shareds, clog_atfork_prepare() locks a mutex inside
// freed memory, so this runs only in a forked child.
static void _fork_safety_publish_rollback_child(const char *marker_path) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) test_child_exit(0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol__pintable_force_next_publish_failure_for_tests();
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_NULL) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
    test_child_exit(0);
  }
  _fork_safety_marker_append(marker_path, "acquire_failed_as_expected\n");

  clog probe = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (probe != CLOG_NULL) {
    _fork_safety_marker_append(marker_path, "forced_failure_was_consumed\n");
    clog_close(probe);
  }

  pid_t pid = fork();
  _fork_safety_marker_append(marker_path, "survived_fork_call\n");

  if (pid == 0) test_child_exit(0);
  if (pid > 0) waitpid(pid, NULL, 0);
  cleanup_dir(dir, "app.log");
  test_child_exit(0);
}

// A failed publish must undo the live_shareds registration, so the count of
// registered shared objects comes back to its earlier value. This runs in the
// parent because reading the count takes the table's read lock, which
// ThreadSanitizer sees as still write-locked in a forked child.
TEST(fork_safety, handle_publish_rollback_leaves_no_registered_shared) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  size_t before = clog__test_live_shareds_count();

  ccol__pintable_force_next_publish_failure_for_tests();
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  bool refused = (lg == CLOG_NULL);
  if (!refused) clog_close(lg);
  size_t after = clog__test_live_shareds_count();

  clog probe = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  bool probe_opened = (probe != CLOG_NULL);
  if (probe_opened) clog_close(probe);

  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(refused);
  REQUIRE_TRUE(probe_opened);
  REQUIRE_EQ(after, before);
}

TEST(fork_safety, handle_publish_rollback_unregisters_the_shared_object) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof(marker_dir)), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof(marker_path), "%s/marker", marker_dir);

  pid_t pid = fork();
  if (pid == 0) {
    _fork_safety_publish_rollback_child(marker_path);
    test_child_exit(127);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  bool exited = WIFEXITED(status);
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  bool saw_acquire_failed =
      len > 0 && strstr(buf, "acquire_failed_as_expected") != NULL;
  bool saw_failure_consumed =
      len > 0 && strstr(buf, "forced_failure_was_consumed") != NULL;
  bool saw_survived_fork = len > 0 && strstr(buf, "survived_fork_call") != NULL;

  cleanup_dir(marker_dir, "marker");

  REQUIRE_TRUE(exited);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_TRUE(saw_acquire_failed);
  REQUIRE_TRUE(saw_failure_consumed);
  REQUIRE_TRUE(saw_survived_fork);
}

// The rollback of a failed live_shareds registration must leave the slot as
// clog_close() retires it, freed == true included. A freshly grown slot starts
// with freed == false and ptr == NULL, and clog_atfork_prepare() skips only a
// freed slot, so a fork() would dereference that NULL ptr. The child dies by
// SIGSEGV in that case, and its marker file shows it got past the fork().
TEST(fork_safety, handle_acquire_rollback_leaves_slot_fork_safe) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof(marker_dir)), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof(marker_path), "%s/marker", marker_dir);

  pid_t pid = fork();
  if (pid == 0) {
    _fork_safety_rollback_race_child(marker_path);
    test_child_exit(127);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  REQUIRE_TRUE(WIFEXITED(status));

  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  bool saw_acquire_failed =
      len > 0 && strstr(buf, "acquire_failed_as_expected") != NULL;
  bool saw_survived_fork = len > 0 && strstr(buf, "survived_fork_call") != NULL;

  cleanup_dir(marker_dir, "marker");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_TRUE(saw_acquire_failed);
  REQUIRE_TRUE(saw_survived_fork);
}

// The forced registration failure applies to clog_derive() too, whose shared
// object is always the parent's, already registered one: The derive must
// return CLOG_NULL, and the next plain derive must succeed.
TEST(fork_safety, handle_acquire_rollback_hook_also_fires_for_derive) {
  clog parent = clog_create_from_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(parent, CLOG_NULL);

  clog__test_force_next_fresh_slot_registration_failure(true);
  clog child = clog_derive(parent);
  REQUIRE_EQ(child, CLOG_NULL);

  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_NULL);

  clog_close(child2);
  clog_close(parent);
}

// Writes one more record in the child of
// pending_compress_not_permanently_exempt_from_pruning_in_child, which starts
// a rotation, and checks that its prune pass removes the first rotation's
// source, so the child doesn't treat that compression as in flight for ever.
// The parent's compressor thread was held in that compression at the fork()
// and doesn't exist here.
static void _fork_pending_compress_child(clog lg, const char *dir,
                                         const char *stale_plain,
                                         const char *marker_path) {
  clog__test_hold_compressions(false);

  clog_info(lg, "generation two content");
  bool pruned = (access(stale_plain, F_OK) != 0);
  clog_close(lg);

  _fork_safety_marker_append(marker_path,
                             pruned ? "pruned\n" : "still_exempt\n");
  cleanup_dir(dir, "app.log");
  test_child_exit(0);
}

// Logs the record that starts the first rotation and its compression, from a
// thread, so the main thread can fork while that compression is held.
static void *_fork_pending_compress_writer(void *arg) {
  clog lg = *(clog *)arg;
  clog_info(lg, "generation one content");
  return NULL;
}

// The compressor marks its job as running while it compresses without
// shared->mutex, so the prune pass leaves those files alone. A fork() in that
// window copies the mark into a child that has no compressor thread to clear
// it, so clog_atfork_child() must forget the running compression; otherwise
// that generation is never pruned in the child.
TEST(fork_safety,
     pending_compress_not_permanently_exempt_from_pruning_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof(marker_dir)), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof(marker_path), "%s/marker", marker_dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);

  pthread_t t;
  int create_rv = pthread_create(&t, NULL, _fork_pending_compress_writer, &lg);
  if (create_rv != 0) {
    clog__test_hold_compressions(false);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_rv, 0);

  bool opened = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       !opened && test_now_ms() < deadline_ms;) {
    opened = clog__test_gz_dest_opened();
    if (!opened) usleep(1000);
  }

  char stale_plain[1024] = {0};
  DIR *d = opened ? opendir(dir) : NULL;
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      size_t nlen = strlen(e->d_name);
      if (nlen > 7 && strcmp(e->d_name + nlen - 7, ".gz.tmp") == 0)
        snprintf(stale_plain, sizeof(stale_plain), "%s/%.*s", dir,
                 (int)(nlen - 7), e->d_name);
    }
    closedir(d);
  }
  if (stale_plain[0] == '\0') {
    clog__test_hold_compressions(false);
    pthread_join(t, NULL);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(stale_plain[0], '\0');

  pid_t pid = fork();
  if (pid == 0) {
    _fork_pending_compress_child(lg, dir, stale_plain, marker_path);
    test_child_exit(127);
  }
  clog__test_hold_compressions(false);
  if (pid == -1) {
    pthread_join(t, NULL);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(pid, -1);

  pthread_join(t, NULL);
  clog_close(lg);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  REQUIRE_TRUE(WIFEXITED(status));

  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "pruned"), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

// Calls clog_close() on the handle at arg, which
// clog__test_hold_close_finalize() holds in the middle of the close, so a
// fork() from another thread lands there.
static void *_fork_close_race_closer(void *arg) {
  clog *lg = (clog *)arg;
  clog_close(*lg);
  return NULL;
}

// A fork() lands while another thread is held in clog_close() after it cleared
// in_use and before it retired the slot. That thread doesn't exist in the
// child, so clog_atfork_child() must finish the close there; otherwise the
// target's reference count never reaches zero in the child.
TEST(fork_safety, fork_during_concurrent_close_does_not_leak_target_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof(marker_dir)), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof(marker_path), "%s/marker", marker_dir);

  size_t baseline = clog__test_live_shareds_count();

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(clog__test_live_shareds_count(), baseline + 1);

  clog__test_hold_close_finalize(true);

  clog to_close = lg;
  pthread_t closer;
  if (pthread_create(&closer, NULL, _fork_close_race_closer, &to_close) != 0) {
    clog__test_hold_close_finalize(false);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
    REQUIRE_TRUE(false);
  }

  bool entered = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    if (clog__test_close_finalize_entered()) {
      entered = true;
      break;
    }
    usleep(1000);
  }

  pid_t pid = entered ? fork() : -1;
  if (pid == 0) {
    char buf[64];
    snprintf(buf, sizeof(buf), "live_shareds=%zu\n",
             clog__test_live_shareds_count());
    _fork_safety_marker_append(marker_path, buf);
    test_child_exit(0);
  }

  int status = 0;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       pid > 0 && test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (pid > 0 && !reaped) {
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
  }

  clog__test_hold_close_finalize(false);
  pthread_join(closer, NULL);
  if (pid <= 0 || !reaped) {
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_TRUE(entered);
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(reaped);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(clog__test_live_shareds_count(), baseline);

  char expect[64];
  snprintf(expect, sizeof(expect), "live_shareds=%zu", baseline);
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, expect), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

#endif

// Async logging

// Polls path every 5 ms, for up to bound_ms, until needle shows up in its
// content. The moment the writer thread handles a message isn't
// deterministic, so the async tests use this instead of a fixed sleep.
// Returns true when found.
static bool _poll_for_substring(const char *path, const char *needle,
                                int bound_ms) {
  char buf[1 << 16];
  for (long long deadline_ms = test_now_ms() + bound_ms;
       test_now_ms() < deadline_ms;) {
    size_t len = read_file(path, buf, sizeof(buf));
    (void)len;
    if (strstr(buf, needle) != NULL) return true;
    usleep(5000);
  }
  return false;
}

TEST(async, construct_and_close_unbounded_queue) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "hello async unbounded");
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "hello async unbounded"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(async, construct_and_close_bounded_queue) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.queue_size = 8};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "hello async bounded");
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "hello async bounded"), NULL);

  cleanup_dir(dir, "app.log");
}

// A non-NULL clog_async_cfg with every field 0 turns async mode on, with each
// field at its default; the struct has no "enabled" flag.
TEST(async, degenerate_all_zero_config_falls_back_to_defaults) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "degenerate config still works");
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "degenerate config still works"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, messages_eventually_reach_target_without_explicit_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_interval_us = 20000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "eventually visible");
  REQUIRE_TRUE(_poll_for_substring(path, "eventually visible", 20000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// A small flush_buffer_size and a long interval, so only the size trigger can
// flush the record.
TEST(async, size_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 32, .flush_interval_us = 60000000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 20; i++) clog_info(lg, "size trigger filler %d", i);

  REQUIRE_TRUE(_poll_for_substring(path, "size trigger filler", 20000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// A large flush_buffer_size and a short interval, so only the timer can flush
// the record.
TEST(async, duration_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                        .flush_interval_us = 30000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "duration trigger marker");
  REQUIRE_TRUE(_poll_for_substring(path, "duration trigger marker", 20000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// A job that outlasts the flush interval makes the writer's next receive wait
// 0 microseconds, which answers an empty queue with ccol_container_empty. The
// writer must flush on that answer as on an expired interval, not hold the
// record until clog_close().
TEST(async, an_interval_that_ran_out_during_a_job_still_flushes) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                        .flush_interval_us = 20000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  bool seen = false;
  if (lg != CLOG_NULL) {
    clog__test_set_writer_job_delay_us(100000);
    clog_info(lg, "late interval marker");
    seen = _poll_for_substring(path, "late interval marker", 20000);
    clog__test_set_writer_job_delay_us(0);
    clog_close(lg);
  }
  cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(seen);
}

// Async message formatting

// These mirror the synchronous tests for clog_write_async(), whose inline
// message buffer (256 bytes) is separate from and smaller than the stack
// buffer of clog_write_sync().
TEST(async, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_msg[2048];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  clog_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, long_msg), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// With the _size_capped_* allocator only the heap spill of the message fails
// (the envelope, the 4096-byte async buffer and the queue nodes fit), so the
// output must show the truncation.
TEST(async,
     message_exceeding_inline_buffer_marks_truncation_when_heap_alloc_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  char long_msg[5000];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  clog_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  volatile int huge_width_source = 2000000000;
  int huge_width = huge_width_source;
  clog__write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "log message formatting failed"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// A field changed or removed between submission and flush must show its value
// at submission, so the logger takes a snapshot instead of letting the writer
// thread read the fields live.
TEST(async, fields_snapshot_reflects_submission_time_not_flush_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                        .flush_interval_us = 60000000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_set_field(lg, "stage", "before");
  clog_info(lg, "field snapshot check");
  clog_set_field(lg, "stage", "after");
  clog_remove_field(lg, "stage");

  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "stage=before"), NULL);
  REQUIRE_EQ(strstr(buf, "stage=after"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// The logger that _clog_bt_uniquely_named_worker_fn() logs to. A backtrace
// must show the calling thread's stack, captured at submission, never the
// writer thread's.
static clog g_bt_test_lg;
// Logs an error with a backtrace from a function whose name appears nowhere
// else, so the test can find that name in the captured frames.
static void *_clog_bt_uniquely_named_worker_fn(void *arg) {
  (void)arg;
  clog_error(g_bt_test_lg, "backtrace from worker thread");
  return NULL;
}

TEST(async, backtrace_captured_on_calling_thread_not_writer_thread) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  g_bt_test_lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(g_bt_test_lg, CLOG_NULL);

  pthread_t th;
  int create_rv =
      pthread_create(&th, NULL, _clog_bt_uniquely_named_worker_fn, NULL);
  if (create_rv == 0) {
    pthread_join(th, NULL);
  } else {
    clog_close(g_bt_test_lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_rv, 0);

  clog_flush(g_bt_test_lg);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "backtrace from worker thread"), NULL);
  REQUIRE_NE(strstr(buf, "_clog_bt_uniquely_named_worker_fn"), NULL);
  REQUIRE_EQ(strstr(buf, "clog_writer_thread_main"), NULL);

  clog_close(g_bt_test_lg);
  cleanup_dir(dir, "app.log");
}

// emit_backtrace_syslog_lines() must leave its scratch buffer (the async
// buffer itself, for the writer thread) empty after the last frame. Otherwise
// the next flush, here the shutdown drain of clog_close(), writes the last
// frame a second time.
TEST(async, syslog_backtrace_not_duplicated_on_shutdown_drain) {
  if (!backtrace_capture_genuinely_available()) return;
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg cfg = {.flush_interval_us = 60000000};
  clog lg = clog_create_from_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_error(lg, "with backtrace via async");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  const char *starts[64];
  int n = 0;
  for (const char *p = buf; (p = strstr(p, "<11>1 ")) != NULL && n < 64; p++)
    starts[n++] = p;
  REQUIRE_GT(n, 1);

  for (int i = 0; i < n; i++) {
    size_t len_i =
        (i + 1 < n) ? (size_t)(starts[i + 1] - starts[i]) : strlen(starts[i]);
    for (int j = i + 1; j < n; j++) {
      size_t len_j =
          (j + 1 < n) ? (size_t)(starts[j + 1] - starts[j]) : strlen(starts[j]);
      bool same = len_i == len_j && memcmp(starts[i], starts[j], len_i) == 0;
      REQUIRE_FALSE(same);
    }
  }
}

// The same hazard through the other return path: A forced capture failure
// writes the "#error backtrace unavailable" record, which must appear once.
TEST(async,
     syslog_backtrace_capture_failure_marker_not_duplicated_on_shutdown_drain) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg cfg = {.flush_interval_us = 60000000};
  clog lg = clog_create_from_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_force_backtrace_capture_failure(true);
  clog_error(lg, "boom");
  clog__test_force_backtrace_capture_failure(false);

  char buf[8192];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  int occurrences = 0;
  const char *p = buf;
  while ((p = strstr(p, "#error backtrace unavailable")) != NULL) {
    occurrences++;
    p++;
  }
  REQUIRE_EQ(occurrences, 1);
}

// The writer thread may handle a job long after its submission, so every
// backtrace frame record must take its TIMESTAMP from the job, as the main
// record does, and they must all agree byte for byte.
TEST(async, syslog_backtrace_timestamp_matches_primary_record) {
  if (!backtrace_capture_genuinely_available()) return;
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_error(lg, "timestamp correlation check");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  char timestamps[64][64];
  int n = 0;
  const char *p = buf;
  while ((p = strstr(p, "<11>1 ")) != NULL && n < 64) {
    const char *ts_start = p + 6;
    const char *ts_end = strchr(ts_start, ' ');
    REQUIRE_NE((void *)ts_end, (void *)NULL);
    size_t ts_len = (size_t)(ts_end - ts_start);
    REQUIRE_LT(ts_len, (size_t)64);
    memcpy(timestamps[n], ts_start, ts_len);
    timestamps[n][ts_len] = '\0';
    n++;
    p = ts_end;
  }
  REQUIRE_GT(n, 1);

  for (int i = 1; i < n; i++) REQUIRE_STREQ(timestamps[i], timestamps[0]);
}

// A job reads its format live, so a logfmt record may sit in the batch buffer
// when the format switches to syslog. The syslog branch must flush that batch
// before it builds its own record, or the earlier record is lost.
TEST(async, format_switch_to_syslog_does_not_drop_buffered_batch) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg cfg = {.flush_interval_us = 60000000};
  clog lg = clog_create_from_fd_mp(pipefd[1], CLOG_INFO, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "pre-switch logfmt message");
  usleep(50000);

  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "post-switch syslog message");

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(n, (size_t)0);

  REQUIRE_NE(strstr(buf, "pre-switch logfmt message"), NULL);
  REQUIRE_NE(strstr(buf, "post-switch syslog message"), NULL);
}

// CLOG_FATAL drains the queue before it writes the fatal record and exits, so
// records logged just before it aren't lost. The child keeps its output and
// prints checkpoints, and a timeout dumps /proc, because this test hung once
// on qemu-user for a reason that isn't known (see
// _clog_test_dump_stuck_child_diagnostics() for what to look for).
TEST(async, fatal_drains_queue_before_writing_and_terminating) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  pid_t pid = fork();
  if (pid == 0) {
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d about to clog_create_from_file_mp\n",
            (int)getpid());
    forget_parent_fixtures_in_child();
    clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 60000000};
    clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
    if (lg == CLOG_NULL) test_child_exit(2);
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d opened, about to log queued "
            "message one\n",
            (int)getpid());
    clog_info(lg, "queued message one");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d logged message one, about to log "
            "queued message two\n",
            (int)getpid());
    clog_info(lg, "queued message two");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d logged message two, about to "
            "clog_fatal\n",
            (int)getpid());
    clog_fatal(lg, "the fatal record itself");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d clog_fatal returned (should be "
            "unreachable!)\n",
            (int)getpid());
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    _clog_test_dump_stuck_child_diagnostics(pid);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  REQUIRE_FALSE(WIFSIGNALED(status));

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  char *p1 = strstr(buf, "queued message one");
  char *p2 = strstr(buf, "queued message two");
  char *pf = strstr(buf, "the fatal record itself");
  REQUIRE_NE(p1, NULL);
  REQUIRE_NE(p2, NULL);
  REQUIRE_NE(pf, NULL);
  REQUIRE_LT((long)(p1 - buf), (long)(p2 - buf));
  REQUIRE_LT((long)(p2 - buf), (long)(pf - buf));

  cleanup_dir(dir, "app.log");
}

// A time rotation starts at the first write after the interval ends. An idle
// async logger must never rotate, and its writer must stay asleep, however
// many intervals pass; the next write rotates exactly once. The test moves the
// library's wall clock instead of sleeping.
TEST(async, idle_logger_does_not_rotate_until_next_write_after_interval) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg rcfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
  };
  clog_async_cfg acfg = {.flush_interval_us = 50000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "first message");
  bool first_written = _poll_for_substring(path, "first message", 20000);

  size_t wakeups_before = clog__test_writer_timeout_wakeups(lg);
  clog__test_set_wall_clock_offset(350);
  usleep(300000);
  int rotated_idle = count_files_with_prefix(dir, "app.log.");
  size_t wakeups_idle = clog__test_writer_timeout_wakeups(lg) - wakeups_before;

  clog_info(lg, "second message");
  clog_close(lg);
  clog__test_set_wall_clock_offset(0);
  int rotated_after = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(first_written);
  REQUIRE_EQ(wakeups_idle, (size_t)0);
  REQUIRE_EQ(rotated_idle, 0);
  REQUIRE_EQ(rotated_after, 1);
}

TEST(async, flush_drains_a_slow_to_self_flush_setup) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                        .flush_interval_us = 60000000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "needs an explicit flush");

  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "needs an explicit flush"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// An allocator whose malloc() fails while g_flush_oom_fail_malloc is set; it
// makes the unbounded queue's node allocation fail inside cdynmq_send().
static _Atomic bool g_flush_oom_fail_malloc = false;
static void *_flush_oom_malloc(size_t sz) {
  if (atomic_load(&g_flush_oom_fail_malloc)) return NULL;
  return malloc(sz);
}
static void _flush_oom_free(void *p) { free(p); }
static void *_flush_oom_calloc(size_t n, size_t sz) {
  if (atomic_load(&g_flush_oom_fail_malloc)) return NULL;
  return calloc(n, sz);
}
static void *_flush_oom_realloc(void *p, size_t sz) {
  if (atomic_load(&g_flush_oom_fail_malloc)) return NULL;
  return realloc(p, sz);
}

TEST(async, flush_returns_promptly_when_enqueue_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  atomic_store(&g_flush_oom_fail_malloc, true);

  long long t0 = test_now_ms();
  clog_flush(lg);
  long long elapsed_ms = test_now_ms() - t0;

  atomic_store(&g_flush_oom_fail_malloc, false);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
  REQUIRE_LT(elapsed_ms, 2000LL);
}

// clog_flush() must check the init of its own mutex and condition variable;
// when one fails it enqueues nothing and waits for nothing, and the logger
// keeps working afterwards.
TEST(async, flush_returns_promptly_when_mutex_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "before forced ccol__mutex_init failure");

  clog__test_force_flush_mutex_init_failure(true);

  long long t0 = test_now_ms();
  clog_flush(lg);
  long long elapsed_ms = test_now_ms() - t0;

  clog_info(lg, "after forced ccol__mutex_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_LT(elapsed_ms, 2000LL);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "before forced ccol__mutex_init failure"), NULL);
  REQUIRE_NE(strstr(buf, "after forced ccol__mutex_init failure"), NULL);

  cleanup_dir(dir, "app.log");
}

// When the condition variable init fails after the mutex init succeeded, the
// mutex must be destroyed and nothing waits on the condition variable.
TEST(async, flush_returns_promptly_when_condvar_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "before forced condvar_init failure");

  clog__test_force_flush_condvar_init_failure(true);

  long long t0 = test_now_ms();
  clog_flush(lg);
  long long elapsed_ms = test_now_ms() - t0;

  clog_info(lg, "after forced condvar_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_LT(elapsed_ms, 2000LL);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "before forced condvar_init failure"), NULL);
  REQUIRE_NE(strstr(buf, "after forced condvar_init failure"), NULL);

  cleanup_dir(dir, "app.log");
}

// The logger that _teardown_retry_closer() closes, and the flag it sets when
// clog_close() returns.
static _Atomic bool g_teardown_retry_close_done = false;
static clog g_teardown_retry_lg;

// Closes g_teardown_retry_lg and sets g_teardown_retry_close_done, so the test
// can bound a close that would hang without the sentinel retry.
static void *_teardown_retry_closer(void *arg) {
  (void)arg;
  clog_close(g_teardown_retry_lg);
  atomic_store(&g_teardown_retry_close_done, true);
  return NULL;
}

// The shutdown sentinel's send can fail under memory pressure. Without a retry
// the writer thread waits for a sentinel that never comes and clog_close()
// hangs in the join, so the teardown retries with a bounded backoff. Every
// allocation fails for a while, then the allocator recovers.
TEST(async, close_recovers_from_transient_oom_instead_of_hanging) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg cfg = {0};
  g_teardown_retry_lg =
      clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(g_teardown_retry_lg, CLOG_NULL);

  clog_info(g_teardown_retry_lg, "before oom");

  atomic_store(&g_flush_oom_fail_malloc, true);
  atomic_store(&g_teardown_retry_close_done, false);

  pthread_t th;
  REQUIRE_EQ(pthread_create(&th, NULL, _teardown_retry_closer, NULL), 0);

  usleep(300000);
  atomic_store(&g_flush_oom_fail_malloc, false);

  bool done = false;
  for (long long deadline_ms = test_now_ms() + 10000;
       test_now_ms() < deadline_ms;) {
    if (atomic_load(&g_teardown_retry_close_done)) {
      done = true;
      break;
    }
    usleep(20000);
  }
  REQUIRE_TRUE(done);
  pthread_join(th, NULL);

  cleanup_dir(dir, "app.log");
}

// An allocator that fails exactly its g_enqueue_fail_at'th call, counting
// malloc, calloc and realloc together, so a failure can land on the queue node
// allocation of cdynmq_send() inside clog_write_async() after the envelope
// allocation succeeded.
static _Atomic int g_enqueue_fail_call_count = 0;
static _Atomic int g_enqueue_fail_at = -1;

// Counts one call of the allocator and tells whether it is the one to fail;
// the procs below use it.
static bool _enqueue_fail_should_fail(void) {
  int c = atomic_fetch_add(&g_enqueue_fail_call_count, 1) + 1;
  int fa = atomic_load(&g_enqueue_fail_at);
  return fa > 0 && c == fa;
}
static void *_enqueue_fail_malloc(size_t sz) {
  return _enqueue_fail_should_fail() ? NULL : malloc(sz);
}
static void _enqueue_fail_free(void *p) { free(p); }
static void *_enqueue_fail_calloc(size_t n, size_t sz) {
  return _enqueue_fail_should_fail() ? NULL : calloc(n, sz);
}
static void *_enqueue_fail_realloc(void *p, size_t sz) {
  return _enqueue_fail_should_fail() ? NULL : realloc(p, sz);
}

// When the enqueue itself fails, clog_write_async() writes the record
// directly, and that path must run the rotation checks too. With no fields
// and a short message, allocation 1 is the envelope and allocation 2 the queue
// node.
TEST(async, enqueue_failure_fallback_still_rotates_on_size) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg rcfg = {0};
  rcfg.size_rotation_enabled = true;
  rcfg.max_file_size = 10;

  clog_async_cfg acfg = {0};

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "seed");
  clog_flush(lg);

  int rotated_before = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ(rotated_before, 1);

  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  clog_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  int rotated_after = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated_after, rotated_before);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// The same for time rotation. The interval is 10 s and the test moves the
// library's wall clock, because rotation counts whole seconds from the open
// and a short real interval can pass before the "not rotated yet" check.
TEST(async, enqueue_failure_fallback_still_rotates_on_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg rcfg = {0};
  rcfg.time_rotation_enabled = true;
  rcfg.rotation_interval_us = 10000000;

  clog_async_cfg acfg = {0};

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "seed");
  clog_flush(lg);
  int rotated_before = count_files_with_prefix(dir, "app.log.");
  if (rotated_before != 0) clog_close(lg);
  REQUIRE_EQ(rotated_before, 0);

  clog__test_set_wall_clock_offset(11);

  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  clog_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  int rotated_after = count_files_with_prefix(dir, "app.log.");
  clog_close(lg);
  clog__test_set_wall_clock_offset(0);
  cleanup_dir(dir, "app.log");
  REQUIRE_GT(rotated_after, rotated_before);
}

// Before the enqueue-failure fallback writes its record directly, it drains
// what is already queued, so the file keeps submission order. The flush
// triggers are out of reach, so only that drain can put the first message on
// disk before the second one's clog_info() returns.
TEST(async,
     enqueue_failure_direct_write_does_not_reorder_already_queued_message) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg acfg = {.flush_buffer_size = 1 << 20,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "message A (already queued)");

  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  clog_info(lg, "message B (enqueue fails, written directly)");
  atomic_store(&g_enqueue_fail_at, -1);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  char *pos_a = strstr(buf, "message A");
  char *pos_b = strstr(buf, "message B");
  REQUIRE_NE(pos_a, NULL);
  REQUIRE_NE(pos_b, NULL);
  REQUIRE_TRUE(pos_a < pos_b);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// When the field pool's growth fails in snapshot_fields(), the pool must not
// be destroyed there, because the job release destroys it again. A long field
// forces a growth, and a sweep of single allocation failures hits it; glibc or
// valgrind catches a double free.
TEST(async, field_snapshot_growbuf_oom_does_not_double_free) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg acfg = {0};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  char big_value[400];
  memset(big_value, 'v', sizeof(big_value) - 1);
  big_value[sizeof(big_value) - 1] = '\0';
  clog_set_field(lg, "big", big_value);

  for (int fail_at = 1; fail_at <= 20; fail_at++) {
    atomic_store(&g_enqueue_fail_call_count, 0);
    atomic_store(&g_enqueue_fail_at, fail_at);
    clog_info(lg, "msg %d", fail_at);
    atomic_store(&g_enqueue_fail_at, -1);
    clog_flush(lg);
  }

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

// The shared async buffer can be so close to its ceiling that even the small
// fallback record doesn't fit. The writer then flushes what the buffer holds
// and retries on an empty buffer, so the record is never cut. The ceiling
// follows flush_buffer_size above 16 MiB, so the messages are sized from it
// after a calibration of one record's overhead.
TEST(async,
     fallback_record_still_well_formed_when_batch_buffer_is_nearly_full) {
  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);

  clog calib_lg =
      clog_create_from_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_NULL);
  const char *calib_msg = "CALIBRATION_MESSAGE_CONTENT";
  clog_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);

  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof(calib_buf));
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  const size_t write_buf_cap = 20UL * 1024 * 1024;
  clog_async_cfg acfg = {.flush_buffer_size = write_buf_cap};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  const size_t room = 20;
  size_t first_len = write_buf_cap - room - fixed_overhead;
  char *first_msg = malloc(first_len + 1);
  REQUIRE_NE(first_msg, NULL);
  memset(first_msg, 'B', first_len);
  first_msg[first_len] = '\0';

  size_t huge_len = write_buf_cap + (1UL * 1024 * 1024);
  char *huge_msg = malloc(huge_len + 1);
  REQUIRE_NE(huge_msg, NULL);
  memset(huge_msg, 'D', huge_len);
  huge_msg[huge_len] = '\0';

  clog_info(lg, "%s", first_msg);
  clog_info(lg, "%s", huge_msg);
  clog_flush(lg);
  clog_close(lg);

  free(first_msg);
  free(huge_msg);

  size_t read_cap = write_buf_cap + (2UL * 1024 * 1024);
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  size_t len = read_file(path, buf, read_cap);
  REQUIRE_GT(len, (size_t)0);

  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 2);

  char *fallback_line = strstr(buf, "too large to emit");
  REQUIRE_NE(fallback_line, NULL);
  char *line_end = strchr(fallback_line, '\n');
  REQUIRE_NE(line_end, NULL);
  REQUIRE_EQ(*(line_end - 1), '"');

  free(buf);
  cleanup_dir(dir, "app.log");
}

// A small message whose record doesn't fit in the room a neighbour left, while
// the fallback note would, must be retried on an empty buffer instead of
// being reported as too large. The test sweeps the room left after a filler
// record (calibrated for this test's __FILE__ and function name), flushes
// between rounds and truncates the file through a second fd.
TEST(async,
     ordinary_small_message_not_misreported_as_too_large_by_batch_neighbor) {
  const char *small_msg = "reachable small message";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg =
      clog_create_from_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_NULL);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  clog_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof(calib_buf));
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  const size_t write_buf_cap = 16UL * 1024 * 1024;
  char *filler = malloc(write_buf_cap);
  REQUIRE_NE(filler, NULL);
  memset(filler, 'B', write_buf_cap);

  size_t read_cap = write_buf_cap * 2;
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  int pairs_logged = 0;
  for (size_t room = fixed_overhead + 50; room <= fixed_overhead + 1200;
       room += 25) {
    filler[write_buf_cap - room] = '\0';
    clog_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B';
    clog_info(lg, "%s", small_msg);
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);
    REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);
    REQUIRE_NE(strstr(buf, small_msg), NULL);

    int fd = open(path, O_WRONLY | O_TRUNC);
    REQUIRE_GE(fd, 0);
    close(fd);
  }
  clog_close(lg);
  free(filler);
  free(buf);
  REQUIRE_GT(pairs_logged, 0);

  cleanup_dir(dir, "app.log");
}

// When the buffer has room for a record but for no backtrace frame, logfmt
// writes the "unavailable" marker, so a reader can tell a lost backtrace from
// one never asked for. The frame text isn't predictable, so the test sweeps
// the room as the test above does.
TEST(async, backtrace_never_silently_lost_when_batch_buffer_is_nearly_full) {
  if (!backtrace_capture_genuinely_available()) return;
  const char *small_msg = "reachable small message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg =
      clog_create_from_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_NULL);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  clog_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof(calib_buf));
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  const size_t write_buf_cap = 16UL * 1024 * 1024;
  char *filler = malloc(write_buf_cap);
  REQUIRE_NE(filler, NULL);
  memset(filler, 'B', write_buf_cap);

  size_t read_cap = write_buf_cap * 2;
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  int pairs_logged = 0;
  for (size_t room = fixed_overhead + 50; room <= fixed_overhead + 1200;
       room += 25) {
    filler[write_buf_cap - room] = '\0';
    clog_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B';
    clog_error(lg, "%s", small_msg);
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    bool has_frame = strstr(rec, "\t#") != NULL;
    bool has_marker = strstr(rec, "backtrace unavailable") != NULL;
    REQUIRE_TRUE(has_frame || has_marker);

    int fd = open(path, O_WRONLY | O_TRUNC);
    REQUIRE_GE(fd, 0);
    close(fd);
  }
  clog_close(lg);
  free(filler);
  free(buf);
  REQUIRE_GT(pairs_logged, 0);

  cleanup_dir(dir, "app.log");
}

// When the buffer has room to open and close "bt" but not for one frame, JSON
// must write bt_error, because "bt":[] reads as a real shallow backtrace and
// counts as a successful build that the writer never retries.
TEST(async, json_backtrace_never_silently_becomes_an_empty_array) {
  if (!backtrace_capture_genuinely_available()) return;
  const char *small_msg = "reachable small json message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg =
      clog_create_from_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_NULL);
  clog_set_format(calib_lg, CLOG_FMT_JSON);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  clog_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof(calib_buf));
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  const size_t write_buf_cap = 16UL * 1024 * 1024;
  char *filler = malloc(write_buf_cap);
  REQUIRE_NE(filler, NULL);
  memset(filler, 'B', write_buf_cap);

  size_t read_cap = write_buf_cap * 2;
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  int pairs_logged = 0;
  for (size_t room = fixed_overhead + 50; room <= fixed_overhead + 1200;
       room += 25) {
    filler[write_buf_cap - room] = '\0';
    clog_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B';
    clog_error(lg, "%s", small_msg);
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    bool has_real_frame = strstr(rec, "\"bt\":[\"") != NULL;
    bool has_bt_error = strstr(rec, "\"bt_error\"") != NULL;
    bool has_empty_bt_array = strstr(rec, "\"bt\":[]") != NULL;
    REQUIRE_FALSE(has_empty_bt_array);
    REQUIRE_TRUE(has_real_frame || has_bt_error);

    int fd = open(path, O_WRONLY | O_TRUNC);
    REQUIRE_GE(fd, 0);
    close(fd);
  }
  clog_close(lg);
  free(filler);
  free(buf);
  REQUIRE_GT(pairs_logged, 0);

  cleanup_dir(dir, "app.log");
}

// clog_write_unrepresentable_record() is the last resort when not even the
// fallback placeholder fits, and it must still signal a requested backtrace
// as missing. Ordinary use can't reach it, so the test calls the test hook
// directly.
TEST(robustness,
     unrepresentable_record_logfmt_still_signals_missing_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR,
                                          true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  char *primary = strstr(buf, "log record dropped");
  REQUIRE_NE(primary, NULL);

  REQUIRE_NE(strstr(primary, "#error backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(robustness, unrepresentable_record_logfmt_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR,
                                          false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

// The JSON form: The innermost fallback also embeds bt_error when a backtrace
// was requested.
TEST(robustness, unrepresentable_record_json_embeds_bt_error_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_NE(strstr(buf, "\"bt_error\":\"unavailable\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(robustness, unrepresentable_record_json_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

// When the capture fails and the append of the "backtrace unavailable" marker
// fails too, a last resort that needs no allocation still writes a well-formed
// record. Both failures are forced through test hooks.
TEST(robustness, syslog_backtrace_unavailable_marker_survives_append_failure) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_force_next_buf_ensure_failure(true);
  clog__test_emit_backtrace_syslog_unavailable_marker(lg, CLOG_ERROR);

  char buf[4096];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "backtrace unavailable"), NULL);
  REQUIRE_EQ(buf[0], '<');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

// logfmt and syslog have no bt_error key, so a failed capture must write a
// marker line; otherwise a reader can't tell an omitted backtrace from one
// nobody asked for.
TEST(robustness, logfmt_backtrace_capture_failure_emits_marker_line) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_force_backtrace_capture_failure(true);
  clog_error(lg, "boom");
  clog__test_force_backtrace_capture_failure(false);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_NE(strstr(buf, "\t#error backtrace unavailable\n"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(robustness, syslog_backtrace_capture_failure_emits_marker_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_force_backtrace_capture_failure(true);
  clog_error(lg, "boom");
  clog__test_force_backtrace_capture_failure(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_NE(strstr(buf, "#error backtrace unavailable"), NULL);
}

// When every frame of a real capture fails to append (forced by
// clog__test_force_all_syslog_backtrace_frames_failure()), the marker record
// is written instead, and no frame line.
TEST(robustness,
     syslog_backtrace_all_frames_failing_still_emits_marker_record) {
  if (!backtrace_capture_genuinely_available()) return;
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_force_all_syslog_backtrace_frames_failure(true);
  clog_error(lg, "boom");
  clog__test_force_all_syslog_backtrace_frames_failure(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_NE(strstr(buf, "#error backtrace unavailable"), NULL);
  REQUIRE_EQ(strstr(buf, "\t#0 "), NULL);
}

// A capture that succeeds with depth <= CLOG_BT_INITIAL_FRAME is not "every
// frame failed to fit", so it gets no "unavailable" marker and no invented
// frame. clog__test_force_shallow_backtrace_depth() forces the depth.
TEST(robustness, logfmt_shallow_backtrace_capture_emits_no_marker) {
  BT_SKIP_WITHOUT_SUPPORT();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_force_shallow_backtrace_depth(true);
  clog_error(lg, "boom");
  clog__test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(robustness, syslog_shallow_backtrace_capture_emits_no_marker_record) {
  BT_SKIP_WITHOUT_SUPPORT();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_create_from_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_force_shallow_backtrace_depth(true);
  clog_error(lg, "boom");
  clog__test_force_shallow_backtrace_depth(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

// Writers that drive one async logger at the same time.
#define ASYNC_STRESS_WRITER_COUNT 4
#define ASYNC_STRESS_ITERATIONS 500

// Logs ASYNC_STRESS_ITERATIONS records through the logger at arg, one of the
// threads that drive one async logger at once.
static void *_async_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ASYNC_STRESS_ITERATIONS; i++)
    clog_info(lg, "async stress line %d", i);
  return NULL;
}

TEST(threading, concurrent_async_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg cfg = {.flush_buffer_size = 4096, .flush_interval_us = 20000};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t writers[ASYNC_STRESS_WRITER_COUNT];
  bool created[ASYNC_STRESS_WRITER_COUNT];
  int create_failures = 0;
  for (int i = 0; i < ASYNC_STRESS_WRITER_COUNT; i++) {
    created[i] =
        (pthread_create(&writers[i], NULL, _async_stress_writer, &lg) == 0);
    if (!created[i]) create_failures++;
  }
  for (int i = 0; i < ASYNC_STRESS_WRITER_COUNT; i++)
    if (created[i]) pthread_join(writers[i], NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(lg);

  int fd = open(path, O_RDONLY);
  REQUIRE_NE(fd, -1);
  FILE *f = fdopen(fd, "r");
  REQUIRE_NE((void *)f, (void *)NULL);
  char line[512];
  int total_lines = 0;
  int bad_lines = 0;
  while (fgets(line, sizeof(line), f)) {
    total_lines++;
    if (strncmp(line, "ts=", 3) != 0 ||
        strstr(line, "async stress line") == NULL) {
      bad_lines++;
    }
  }
  fclose(f);

  REQUIRE_EQ(total_lines, ASYNC_STRESS_WRITER_COUNT * ASYNC_STRESS_ITERATIONS);
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

// Compression
TEST(compression, rotated_file_is_valid_gzip) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 20; i++)
    clog_info(lg, "compression test message index=%d padding-to-force-rotation",
              i);

  clog_close(lg);

  REQUIRE_GT(count_files_with_suffix(dir, ".gz"), 0);

  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"),
             count_files_with_prefix(dir, "app.log."));

  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)), 0);
  REQUIRE_EQ(gunzip_test(gz_path), 0);

  cleanup_dir(dir, "app.log");
}

TEST(compression, decompressed_content_matches_written_log) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 400,
      .time_rotation_enabled = false,
      .max_rotated_files = 10,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(
      lg,
      "sentinel_marker_abc123 index=0 pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  for (int i = 1; i < 15; i++)
    clog_info(
        lg,
        "sentinel_marker_abc123 index=%d pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        i);

  clog_close(lg);

  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)), 0);

  char decomp[65536];
  size_t dlen = gunzip_read(gz_path, decomp, sizeof(decomp));
  REQUIRE_GT(dlen, (size_t)0);

  REQUIRE_TRUE(strstr(decomp, "sentinel_marker_abc123") != NULL);

  int bad = 0;
  char *l = decomp;
  while (l < decomp + dlen) {
    char *nl = strchr(l, '\n');
    if (l[0] != '\t' && strncmp(l, "ts=", 3) != 0) bad++;
    if (!nl) break;
    l = nl + 1;
  }
  REQUIRE_EQ(bad, 0);

  cleanup_dir(dir, "app.log");
}

TEST(compression, no_compression_when_disabled) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = false,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 20; i++)
    clog_info(lg, "no-compress test message index=%d padding-padding-padding",
              i);

  clog_close(lg);

  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 0);

  cleanup_dir(dir, "app.log");
}

// The prune runs before the compression and keys on the stamped name, so at
// most max_rotated_files rotated files stay; one more is allowed because the
// last rotation can race with the count.
TEST(compression, max_rotated_files_respected_with_compression) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  for (int i = 0; i < 60; i++)
    clog_info(lg, "pruning+compress test idx=%d extra-data-to-fill-the-buffer",
              i);

  clog_close(lg);

  int gz_count = count_files_with_suffix(dir, ".gz");
  REQUIRE_LE(gz_count, cfg.max_rotated_files + 1);
  REQUIRE_GT(gz_count, 0);

  cleanup_dir(dir, "app.log");
}

// A rotation of a nearly empty file must give a valid .gz, or no .gz if the
// write was too small to rotate.
TEST(compression, empty_file_compresses_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);
  close(fd);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "trigger rotation");
  clog_close(lg);

  char gz_path[512];
  int found = find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path));
  if (found == 0) {
    REQUIRE_EQ(gunzip_test(gz_path), 0);
  }

  cleanup_dir(dir, "app.log");
}

// When gzip_compress_file() fails before it opens its destination (here the
// source is missing), it must not unlink a file that already sits at the
// destination path.
TEST(compression,
     compress_failure_before_dst_created_leaves_existing_dst_untouched) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);

  char src[512];
  snprintf(src, sizeof(src), "%s/does-not-exist.log", dir);

  char dst[512];
  snprintf(dst, sizeof(dst), "%s/preexisting.log.gz", dir);
  int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);
  const char *marker = "unrelated pre-existing content";
  REQUIRE_EQ(write(fd, marker, strlen(marker)), (ssize_t)strlen(marker));
  close(fd);

  REQUIRE_FALSE(clog__test_gzip_compress_file(src, dst));

  char buf[256];
  size_t len = read_file(dst, buf, sizeof(buf));
  REQUIRE_EQ(len, strlen(marker));
  REQUIRE_EQ(memcmp(buf, marker, strlen(marker)), 0);

  unlink(dst);
  rmdir(dir);
}

// Many writers with a tiny max_file_size and compression on, so rotations and
// compressions overlap. A compression runs outside shared->mutex, and a
// concurrent prune must not delete the file it reads, or that generation is
// lost and gunzip -t fails.
#define ROTATE_STRESS_THREAD_COUNT 6
#define ROTATE_STRESS_MSGS_PER_THREAD 150

// Logs ROTATE_STRESS_MSGS_PER_THREAD padded records through the logger at arg,
// so rotations and compressions overlap.
static void *_rotate_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ROTATE_STRESS_MSGS_PER_THREAD; i++)
    clog_info(lg, "rotate stress padding-padding-padding-padding idx=%d", i);
  return NULL;
}

TEST(compression, concurrent_rotations_never_corrupt_or_lose_a_gz_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 150,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pthread_t threads[ROTATE_STRESS_THREAD_COUNT];
  bool created[ROTATE_STRESS_THREAD_COUNT];
  int create_failures = 0;
  for (int i = 0; i < ROTATE_STRESS_THREAD_COUNT; i++) {
    created[i] =
        (pthread_create(&threads[i], NULL, _rotate_stress_writer, &lg) == 0);
    if (!created[i]) create_failures++;
  }
  for (int i = 0; i < ROTATE_STRESS_THREAD_COUNT; i++)
    if (created[i]) pthread_join(threads[i], NULL);

  if (create_failures > 0) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(lg);

  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  int gz_checked = 0;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof(gz_path), "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    gz_checked++;
  }
  closedir(d);
  REQUIRE_GT(gz_checked, 0);

  cleanup_dir(dir, "app.log");
}

// Logs the record that starts the first rotation of the gz race tests, from a
// thread, so the test can hold that compression and rotate again.
static void *_gzrace_first_rotation_writer(void *arg) {
  clog lg = *(clog *)arg;
  clog_info(lg, "first rotation content");
  return NULL;
}

// The deterministic form of the stress test. clog__test_hold_compressions()
// holds the first compression right after it creates its output, and a second
// rotation's prune must leave both the source and the temporary output alone,
// because unlinking an open file loses its content silently.
TEST(compression,
     prune_never_deletes_a_gz_file_still_being_written_by_another_rotation) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);

  pthread_t t;
  bool started =
      (pthread_create(&t, NULL, _gzrace_first_rotation_writer, &lg) == 0);

  bool opened = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       started && !opened && test_now_ms() < deadline_ms;) {
    opened = clog__test_gz_dest_opened();
    if (!opened) usleep(1000);
  }

  int gz_during = -1, tmp_during = -1, rotated_during = -1;
  if (opened) {
    clog_info(lg, "second rotation content");
    gz_during = count_files_with_suffix(dir, ".gz");
    tmp_during = count_files_with_suffix(dir, ".gz.tmp");
    rotated_during = count_files_with_prefix(dir, "app.log.");
  }

  clog__test_hold_compressions(false);
  if (started) pthread_join(t, NULL);
  clog_close(lg);

  REQUIRE_TRUE(started);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(gz_during, 0);
  REQUIRE_EQ(tmp_during, 1);
  REQUIRE_EQ(rotated_during, 3);
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 1);

  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  bool found_first = false, found_second = false;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof(gz_path), "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    char content[512];
    gunzip_read(gz_path, content, sizeof(content));
    if (strstr(content, "first rotation content")) found_first = true;
    if (strstr(content, "second rotation content")) found_second = true;
  }
  closedir(d);
  REQUIRE_FALSE(found_first);
  REQUIRE_TRUE(found_second);

  cleanup_dir(dir, "app.log");
}

// The same race for a logger opened with a bare filename in the working
// directory, so the names the prune lists must match the compression's names
// exactly. The test changes the cwd and restores it before any REQUIRE.
TEST(compression,
     prune_survives_gz_race_with_a_bare_relative_filename_no_slash) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);

  char oldcwd[1024];
  REQUIRE_NE((void *)getcwd(oldcwd, sizeof(oldcwd)), (void *)NULL);
  REQUIRE_EQ(chdir(dir), 0);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };

  clog lg = clog_create_from_file_mp("app.log", CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) {
    if (chdir(oldcwd) != 0) perror("chdir back to the original directory");
    REQUIRE_NE(lg, CLOG_NULL);
  }

  clog__test_hold_compressions(true);

  pthread_t t;
  int prv = pthread_create(&t, NULL, _gzrace_first_rotation_writer, &lg);

  bool opened = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       prv == 0 && !opened && test_now_ms() < deadline_ms;) {
    opened = clog__test_gz_dest_opened();
    if (!opened) usleep(1000);
  }

  int gz_during = -1, tmp_during = -1, rotated_during = -1;
  if (opened) {
    clog_info(lg, "second rotation content");
    gz_during = count_files_with_suffix(dir, ".gz");
    tmp_during = count_files_with_suffix(dir, ".gz.tmp");
    rotated_during = count_files_with_prefix(dir, "app.log.");
  }

  clog__test_hold_compressions(false);
  if (prv == 0) pthread_join(t, NULL);
  clog_close(lg);

  REQUIRE_EQ(chdir(oldcwd), 0);

  REQUIRE_EQ(prv, 0);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(gz_during, 0);
  REQUIRE_EQ(tmp_during, 1);
  REQUIRE_EQ(rotated_during, 3);
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 1);

  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  bool found_first = false, found_second = false;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof(gz_path), "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    char content[512];
    gunzip_read(gz_path, content, sizeof(content));
    if (strstr(content, "first rotation content")) found_first = true;
    if (strstr(content, "second rotation content")) found_second = true;
  }
  closedir(d);
  REQUIRE_FALSE(found_first);
  REQUIRE_TRUE(found_second);

  cleanup_dir(dir, "app.log");
}

// Fatal level (process termination)
TEST(fatal, terminates_process) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "process must die");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(WIFEXITED(wstatus));
  REQUIRE_NE(WEXITSTATUS(wstatus), 0);
}

TEST(fatal, writes_log_before_terminating) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal condition encountered");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_TRUE(WIFEXITED(wstatus));
  REQUIRE_NE(WEXITSTATUS(wstatus), 0);
  REQUIRE_NE(strstr(buf, "FATAL"), NULL);
  REQUIRE_NE(strstr(buf, "fatal condition encountered"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fatal, writes_backtrace_before_terminating) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal with trace");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

// clog_fatal goes past the level filter, so even at CLOG_OFF it writes its
// record and stops the process.
TEST(fatal, writes_and_terminates_with_clog_off) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_OFF, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal bypasses filter");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_TRUE(WIFEXITED(wstatus));
  REQUIRE_NE(WEXITSTATUS(wstatus), 0);
  REQUIRE_NE(strstr(buf, "FATAL"), NULL);
  REQUIRE_NE(strstr(buf, "fatal bypasses filter"), NULL);

  cleanup_dir(dir, "app.log");
}

// Fatal level (JSON format)
TEST(json, fatal_level_string_in_json) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal json message");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"FATAL\""), NULL);
  REQUIRE_NE(strstr(buf, "fatal json message"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, fatal_has_inline_bt_array) {
  BT_SKIP_WITHOUT_SUPPORT();
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_FATAL, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    forget_parent_fixtures_in_child();
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    clog_fatal(lg, "fatal event");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"bt\":["), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

// Framing around a retained continuation

// Tells whether line begins where a record of this module begins (the "ts="
// of a logfmt record, the "level=" of a logfmt marker, a syslog PRI, the
// brace of a JSON record or the tab of a backtrace line), so the framing
// tests can find a line that continues a cut record.
static bool line_starts_a_record(const char *line) {
  return strncmp(line, "ts=", 3) == 0 || strncmp(line, "level=", 6) == 0 ||
         line[0] == '<' || line[0] == '{' || line[0] == '\t';
}

// Counts the records that begin inside line past its start, so the framing
// tests can find a record written into the middle of another: A "ts=" beyond
// offset 0, or a syslog "<PRI>1 " (whose ">1 " sits at offset 2 to 4).
static int count_interior_record_starts(const char *line) {
  int n = 0;
  if (line[0] == '\0') return 0;
  for (const char *p = strstr(line + 1, "ts="); p; p = strstr(p + 1, "ts="))
    n++;
  for (const char *p = strstr(line, ">1 "); p; p = strstr(p + 1, ">1 "))
    if (p - line > 4) n++;
  return n;
}

// Walks buf line by line and stores the number of lines, the lines that don't
// begin a record and the lines that hold a record beginning inside another,
// which is the whole verdict of a framing test. It NUL-terminates each line
// in place and restores the byte afterwards, so buf reads the same after.
static void inspect_record_framing(char *buf, int *out_lines, int *out_unframed,
                                   int *out_interior) {
  int lines = 0, unframed = 0, interior = 0;
  char *line = buf;
  while (*line) {
    char *nl = strchr(line, '\n');
    if (nl) *nl = '\0';
    lines++;
    if (line[0] == '\0' || !line_starts_a_record(line)) unframed++;
    if (count_interior_record_starts(line) > 0) interior++;
    if (nl) *nl = '\n';
    if (!nl) break;
    line = nl + 1;
  }
  *out_lines = lines;
  *out_unframed = unframed;
  *out_interior = interior;
}

// The cap of the short writes these tests force. The test bodies are long
// enough that two capped writes in a row both stop inside the first record.
#define SPLIT_WRITE_CAP ((size_t)100)

// Fills body (bodysz bytes) with marker and then filler that needs no logfmt
// escape, so the record carries the marker verbatim and a test can find each
// record whatever the cut.
static void make_split_test_body(char *body, size_t bodysz,
                                 const char *marker) {
  memset(body, 'A', bodysz - 1);
  body[bodysz - 1] = '\0';
  memcpy(body, marker, strlen(marker));
}

// The writer can hold the continuation of a record whose first half is on
// disk. A syslog record goes out in its own write(), so the library first
// settles the continuation: It writes the missing newline and a marker naming
// the loss. Without that, the syslog record lands in the middle of the logfmt
// record.
TEST(split_record_framing, a_syslog_record_never_lands_inside_a_retained_one) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  REQUIRE_NE(fd, -1);

  clog_async_cfg acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_create_from_fd_mp(fd, CLOG_INFO, &acfg, NULL);
  if (lg == CLOG_NULL) close(fd);
  REQUIRE_NE(lg, CLOG_NULL);

  char body1[320], body2[320], body3[320];
  make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
  make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
  make_split_test_body(body3, sizeof(body3), "RECORDTHREEBODY");

  clog_info(lg, "%s", body1);
  clog_info(lg, "%s", body2);
  clog_info(lg, "%s", body3);

  clog__test_force_short_writes(SPLIT_WRITE_CAP, 2);
  clog_flush(lg);

  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "SYSLOGRECORDBODY");
  clog_flush(lg);

  clog_close(lg);
  close(fd);

  char buf[16384];
  size_t len = read_file(path, buf, sizeof(buf));
  int lines = 0, unframed = 0, interior = 0;
  inspect_record_framing(buf, &lines, &unframed, &interior);
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_second = strstr(buf, "RECORDTWOBODY") != NULL;
  bool has_third = strstr(buf, "RECORDTHREEBODY") != NULL;
  bool has_syslog = strstr(buf, "SYSLOGRECORDBODY") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_EQ(interior, 0);
  REQUIRE_EQ(unframed, 0);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_third);
  REQUIRE_TRUE(has_syslog);
}

// An allocator whose calloc() fails once each time _fail_one_calloc is set;
// every other allocation, the writer's and the teardown's included, works.
static _Atomic bool _fail_one_calloc = false;
static void *_failing_calloc(size_t n, size_t sz) {
  if (atomic_exchange(&_fail_one_calloc, false)) return NULL;
  return calloc(n, sz);
}
static void *_failing_calloc_malloc(size_t sz) { return malloc(sz); }
static void _failing_calloc_free(void *p) { free(p); }
static void *_failing_calloc_realloc(void *p, size_t sz) {
  return realloc(p, sz);
}

// A record whose job envelope can't be allocated takes the synchronous path.
// That path first flushes the queue and the batch, so the split record and
// every earlier record go out whole before it.
TEST(split_record_framing,
     a_synchronous_record_never_lands_inside_a_retained_one) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs procs = {
      .malloc = _failing_calloc_malloc,
      .free = _failing_calloc_free,
      .calloc = _failing_calloc,
      .realloc = _failing_calloc_realloc,
  };

  clog_async_cfg acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_NULL);

  char body1[320], body2[320], body3[320];
  make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
  make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
  make_split_test_body(body3, sizeof(body3), "RECORDTHREEBODY");

  clog_info(lg, "%s", body1);
  clog_info(lg, "%s", body2);
  clog_info(lg, "%s", body3);

  clog__test_force_short_writes(SPLIT_WRITE_CAP, 2);
  clog_flush(lg);
  atomic_store(&_fail_one_calloc, true);
  clog_info(lg, "SYNCHRONOUSRECORDBODY");
  atomic_store(&_fail_one_calloc, false);

  clog_flush(lg);
  clog_close(lg);

  char buf[16384];
  size_t len = read_file(path, buf, sizeof(buf));
  int lines = 0, unframed = 0, interior = 0;
  inspect_record_framing(buf, &lines, &unframed, &interior);
  const char *third = strstr(buf, "RECORDTHREEBODY");
  const char *sync = strstr(buf, "SYNCHRONOUSRECORDBODY");
  bool has_second = strstr(buf, "RECORDTWOBODY") != NULL;
  bool has_third = third != NULL;
  bool has_sync = sync != NULL;
  bool in_order = third && sync && third < sync;
  bool lost = strstr(buf, "log record truncated") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_EQ(interior, 0);
  REQUIRE_EQ(unframed, 0);
  REQUIRE_TRUE(in_order);
  REQUIRE_FALSE(lost);
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_third);
  REQUIRE_TRUE(has_sync);
}

// CLOG_FATAL must never wait: It gives a retained continuation one bounded
// attempt, then repairs the framing instead of writing into the middle of the
// record. It runs in a child, which only has to stop without a signal (valgrind
// replaces its exit status), and the child forgets the parent's fixtures
// first (see forget_parent_fixtures_in_child()).
TEST(split_record_framing, a_fatal_record_never_lands_inside_a_retained_one) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    clog_async_cfg cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 60000000};
    clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
    if (lg == CLOG_NULL) test_child_exit(2);

    char body1[320], body2[320];
    make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
    make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
    clog_info(lg, "%s", body1);
    clog_info(lg, "%s", body2);

    clog__test_force_short_writes(SPLIT_WRITE_CAP, 3);
    clog_flush(lg);

    clog_fatal(lg, "FATALRECORDBODY");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    pid_t r = waitpid(pid, &status, WNOHANG);
    if (r == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    _clog_test_dump_stuck_child_diagnostics(pid);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    REQUIRE_TRUE(false);
  }
  REQUIRE_FALSE(WIFSIGNALED(status));

  char buf[16384];
  size_t len = read_file(path, buf, sizeof(buf));
  int lines = 0, unframed = 0, interior = 0;
  inspect_record_framing(buf, &lines, &unframed, &interior);
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_fatal = strstr(buf, "FATALRECORDBODY") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_EQ(interior, 0);
  REQUIRE_EQ(unframed, 0);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_fatal);
}

// Write errors that no retry outlasts, and the last delivery attempt

// The async configuration of these tests: The buffer is big enough, and the
// interval long enough, that only explicit flushes, the close and the fatal
// path write anything.
static const clog_async_cfg g_undelivered_acfg = {
    .queue_size = 0,
    .flush_buffer_size = 1024UL * 1024UL,
    .flush_interval_us = 60000000,
};

// A flush that fails with an error no retry outlasts (EPIPE) drops the batch
// and writes a marker naming the loss, so later records don't pile up behind
// a batch that can never go out.
TEST(undelivered_batch, an_error_no_retry_outlasts_drops_it_with_a_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg,
                                     NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "BATCHRECORDONE");
  clog_info(lg, "BATCHRECORDTWO");
  clog_info(lg, "BATCHRECORDTHREE");
  clog__test_force_write_errors(EPIPE, 1);
  clog_flush(lg);
  clog__test_force_write_errors(0, 0);
  clog_info(lg, "RECORDAFTERTHEDROP");
  clog_close(lg);

  char buf[16384];
  size_t len = read_file(path, buf, sizeof(buf));
  char expected[96];
  snprintf(expected, sizeof(expected),
           "undelivered batch dropped (records=3 bytes=");
  bool has_marker = strstr(buf, expected) != NULL;
  char why[32];
  snprintf(why, sizeof(why), "write error %d", EPIPE);
  bool has_why = strstr(buf, why) != NULL;
  bool has_batch = strstr(buf, "BATCHRECORD") != NULL;
  bool has_after = strstr(buf, "RECORDAFTERTHEDROP") != NULL;
  int lines = 0, unframed = 0, interior = 0;
  inspect_record_framing(buf, &lines, &unframed, &interior);
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_why);
  REQUIRE_FALSE(has_batch);
  REQUIRE_TRUE(has_after);
  REQUIRE_EQ(lines, 2);
  REQUIRE_EQ(unframed, 0);
  REQUIRE_EQ(interior, 0);
}

TEST(undelivered_batch, a_transient_error_keeps_it_for_the_next_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg,
                                     NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "BATCHRECORDONE");
  clog_info(lg, "BATCHRECORDTWO");
  clog__test_force_write_errors(ENOSPC, 1);
  clog_flush(lg);
  clog__test_force_write_errors(0, 0);
  char mid[16384];
  size_t mid_len = read_file(path, mid, sizeof(mid));
  clog_flush(lg);
  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_one = strstr(buf, "BATCHRECORDONE") != NULL;
  bool has_two = strstr(buf, "BATCHRECORDTWO") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(mid_len, (size_t)0);
  REQUIRE_FALSE(has_marker);
  REQUIRE_TRUE(has_one);
  REQUIRE_TRUE(has_two);
}

TEST(undelivered_batch, a_close_names_what_its_last_flush_could_not_deliver) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg,
                                     NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "BATCHRECORDONE");
  clog__test_force_write_errors(ENOSPC, 1);
  clog_close(lg);
  clog__test_force_write_errors(0, 0);

  char buf[16384];
  read_file(path, buf, sizeof(buf));
  bool has_marker =
      strstr(buf, "undelivered batch dropped (records=1 bytes=") != NULL;
  bool has_why = strstr(buf, "the logger closed") != NULL;
  bool has_batch = strstr(buf, "BATCHRECORDONE") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_why);
  REQUIRE_FALSE(has_batch);
}

// After the drain ahead of a fatal record, what the batch still holds gets one
// more bounded attempt, and a marker ahead of the fatal record names what it
// couldn't deliver. It runs in a child, as the split framing fatal test does.
TEST(undelivered_batch, a_fatal_record_names_what_the_batch_could_not_deliver) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL,
                                       &g_undelivered_acfg, NULL);
    if (lg == CLOG_NULL) test_child_exit(2);
    clog_info(lg, "BATCHRECORDONE");
    clog__test_force_write_errors(ENOSPC, 2);
    clog_fatal(lg, "FATALRECORDBODY");
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    if (waitpid(pid, &status, WNOHANG) == pid) {
      reaped = true;
      break;
    }
    usleep(20000);
  }
  if (!reaped) {
    _clog_test_dump_stuck_child_diagnostics(pid);
    kill(pid, SIGKILL);
    waitpid(pid, &status, 0);
    cleanup_dir(dir, "app.log");
    REQUIRE_TRUE(false);
  }

  char buf[16384];
  read_file(path, buf, sizeof(buf));
  const char *marker = strstr(buf, "undelivered batch dropped (records=1");
  const char *why = strstr(buf, "the process stopped");
  const char *fatal = strstr(buf, "FATALRECORDBODY");
  bool has_batch = strstr(buf, "BATCHRECORDONE") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_FALSE(WIFSIGNALED(status));
  REQUIRE_TRUE(marker != NULL);
  REQUIRE_TRUE(why != NULL);
  REQUIRE_TRUE(fatal != NULL);
  REQUIRE_TRUE(marker < fatal);
  REQUIRE_FALSE(has_batch);
}

// Records that span several lines

// Logs the logfmt ERROR record, with its backtrace as continuation lines, that
// every multi-line test uses. Keeping one call site gives the primary line one
// length whichever logger writes it, which the tests measure.
static __attribute__((noinline)) void log_multi_line_record(clog lg) {
  clog_error(lg, "MULTILINERECORDBODY");
}

// Measures, with a synchronous logger in a directory under dir, the length
// (newline included) of the primary line of log_multi_line_record()'s record,
// so a test can aim a short write at that line's end. Returns 0 on a
// failure.
static size_t multi_line_primary_len(const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/probe.log", dir);
  clog probe = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (probe == CLOG_NULL) return 0;
  log_multi_line_record(probe);
  clog_close(probe);
  char buf[16384];
  read_file(path, buf, sizeof(buf));
  unlink(path);
  const char *nl = strchr(buf, '\n');
  if (!nl || nl[1] != '\t') return 0;
  return (size_t)(nl - buf) + 1;
}

// Counts in *out_orphans the continuation lines of buf that follow neither
// log_multi_line_record()'s record nor another continuation line, and in
// *out_empty the empty lines, the two framing defects a repair can leave.
static void inspect_continuation_lines(const char *buf, int *out_orphans,
                                       int *out_empty) {
  int orphans = 0, empty = 0;
  bool prev_owns = false;
  const char *line = buf;
  while (*line) {
    const char *nl = strchr(line, '\n');
    size_t n = nl ? (size_t)(nl - line) : strlen(line);
    if (n == 0) empty++;
    bool cont = n > 0 && line[0] == '\t';
    if (cont && !prev_owns) orphans++;
    bool is_owner = false;
    const char *hit = strstr(line, "MULTILINERECORDBODY");
    if (hit && (size_t)(hit - line) < n) is_owner = true;
    prev_owns = cont || is_owner;
    if (!nl) break;
    line = nl + 1;
  }
  *out_orphans = orphans;
  *out_empty = empty;
}

// A short write that ends exactly at the newline before the backtrace lines
// still splits the record. Before the syslog record goes out, the library
// gives up on those backtrace lines with a marker, so none of them lands
// under the syslog record.
TEST(split_record_framing, a_split_at_a_backtrace_newline_is_still_a_split) {
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  size_t primary = multi_line_primary_len(dir);
  if (primary == 0) cleanup_dir(dir, "app.log");
  REQUIRE_GT(primary, (size_t)0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);

  clog lg = clog_create_from_fd_mp(fd, CLOG_INFO, &g_undelivered_acfg, NULL);
  if (lg == CLOG_NULL) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_NULL);

  log_multi_line_record(lg);
  clog_info(lg, "SECONDRECORDBODY");
  clog__test_force_short_writes(primary, 1);
  clog_flush(lg);
  clog__test_force_write_errors(ENOSPC, 1);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "SYSLOGRECORDBODY");
  clog_flush(lg);
  clog__test_force_write_errors(0, 0);
  clog_close(lg);
  close(fd);

  char buf[65536];
  read_file(path, buf, sizeof(buf));
  int orphans = 0, empty = 0;
  inspect_continuation_lines(buf, &orphans, &empty);
  bool has_marker = strstr(buf, "undelivered bytes dropped") != NULL;
  bool has_second = strstr(buf, "SECONDRECORDBODY") != NULL;
  bool has_syslog = strstr(buf, "SYSLOGRECORDBODY") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(orphans, 0);
  REQUIRE_EQ(empty, 0);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_syslog);
}

// Giving up on a split record drops its backtrace lines too, while the next
// record goes out whole.
TEST(split_record_framing, a_repair_drops_the_backtrace_of_the_split_record) {
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);

  clog lg = clog_create_from_fd_mp(fd, CLOG_INFO, &g_undelivered_acfg, NULL);
  if (lg == CLOG_NULL) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_NULL);

  log_multi_line_record(lg);
  clog_info(lg, "SECONDRECORDBODY");
  clog__test_force_short_writes(20, 2);
  clog_flush(lg);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "SYSLOGRECORDBODY");
  clog_flush(lg);
  clog_close(lg);
  close(fd);

  char buf[65536];
  read_file(path, buf, sizeof(buf));
  int orphans = 0, empty = 0;
  inspect_continuation_lines(buf, &orphans, &empty);
  bool has_tab = strchr(buf, '\t') != NULL;
  bool has_marker = strstr(buf, "undelivered bytes dropped") != NULL;
  bool has_second = strstr(buf, "SECONDRECORDBODY") != NULL;
  bool has_syslog = strstr(buf, "SYSLOGRECORDBODY") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(orphans, 0);
  REQUIRE_EQ(empty, 0);
  REQUIRE_FALSE(has_tab);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_syslog);
}

// Message-oriented sockets
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

// What a receiver thread saw on a datagram socket: The number of datagrams,
// those that aren't exactly one line, those with a loss marker and those that
// carry body. json_ok stays true while every datagram starts with '{', and the
// receiver reads nothing for start_delay_ms first.
typedef struct {
  int fd;
  _Atomic bool stop;
  size_t datagrams;
  size_t not_one;
  size_t markers;
  size_t with_body;
  const char *body;
  bool json_ok;
  int start_delay_ms;
} dgram_rx_t;

// Reads datagrams from rx->fd and counts them in rx, as the receiver thread of
// the datagram tests, until the stop flag is set and a whole receive timeout
// passes after it. The BSDs deliver loopback UDP later from a thread of their
// own, so that last timeout gives a datagram still on its way the time to
// arrive.
static void *dgram_rx_main(void *arg) {
  dgram_rx_t *rx = arg;
  const size_t cap = 70000;
  char *buf = malloc(cap);
  rx->json_ok = true;
  if (!buf) {
    rx->not_one++;
    return NULL;
  }
  if (rx->start_delay_ms > 0) {
    struct timespec d = {.tv_sec = rx->start_delay_ms / 1000,
                         .tv_nsec = (rx->start_delay_ms % 1000) * 1000000L};
    nanosleep(&d, NULL);
  }
  bool stop_seen = false;
  for (;;) {
    ssize_t n = recv(rx->fd, buf, cap - 1, 0);
    if (n < 0) {
      if (errno == EINTR) continue;
      if (errno != EAGAIN && errno != EWOULDBLOCK) break;
      if (stop_seen) break;
      stop_seen = atomic_load(&rx->stop);
      continue;
    }
    buf[n] = '\0';
    rx->datagrams++;
    size_t lines = 0;
    for (ssize_t i = 0; i < n; i++) lines += buf[i] == '\n';
    if (lines != 1 || n == 0 || buf[n - 1] != '\n') rx->not_one++;
    if (strstr(buf, "log record truncated")) rx->markers++;
    if (rx->body && strstr(buf, rx->body)) rx->with_body++;
    if (buf[0] != '{') rx->json_ok = false;
  }
  free(buf);
  return NULL;
}

// Opens a connected pair of datagram sockets of domain for the datagram tests,
// out[0] receiving and out[1] sending (AF_INET on a loopback UDP port the
// kernel picks). The receive buffer is raised as far as the system allows
// (the BSDs refuse a size above their limit, so it halves until accepted),
// and the receiver wakes every 50 ms. Returns 0, or -1 on failure.
static int open_dgram_pair(int domain, int out[2]) {
  if (domain == AF_UNIX) {
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, out) != 0) return -1;
  } else {
    out[0] = socket(AF_INET, SOCK_DGRAM, 0);
    out[1] = socket(AF_INET, SOCK_DGRAM, 0);
    struct sockaddr_in a;
    memset(&a, 0, sizeof(a));
    a.sin_family = AF_INET;
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t alen = sizeof(a);
    if (out[0] < 0 || out[1] < 0 ||
        bind(out[0], (struct sockaddr *)&a, sizeof(a)) != 0 ||
        getsockname(out[0], (struct sockaddr *)&a, &alen) != 0 ||
        test_connect(out[1], (struct sockaddr *)&a, sizeof(a)) != 0) {
      if (out[0] >= 0) close(out[0]);
      if (out[1] >= 0) close(out[1]);
      return -1;
    }
  }
  for (int big = 4 << 20; big >= 64 << 10; big /= 2)
    if (setsockopt(out[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big)) == 0)
      break;
  struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
  (void)setsockopt(out[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return 0;
}

// Logs count records of fmt through an async logger on a datagram socket of
// domain, first one record too large for a UDP datagram when oversize is set,
// and fills rx with what the receiver saw, which every datagram test checks.
// The receiver reads nothing for rx_delay_ms first. Everything is closed when
// it returns. Returns false when the setup failed.
static bool run_dgram_case_delayed(int domain, clog_format fmt, int count,
                                   bool oversize, int rx_delay_ms,
                                   dgram_rx_t *rx) {
  int sv[2];
  if (open_dgram_pair(domain, sv) != 0) return false;
  memset(rx, 0, sizeof(*rx));
  rx->start_delay_ms = rx_delay_ms;
  rx->fd = sv[0];
  rx->body = "DATAGRAMRECORDBODY";
  pthread_t tid;
  if (pthread_create(&tid, NULL, dgram_rx_main, rx) != 0) {
    close(sv[0]);
    close(sv[1]);
    return false;
  }
  clog_async_cfg acfg = {0};
  clog lg = clog_create_from_fd(sv[1], CLOG_INFO, &acfg);
  if (lg != CLOG_NULL) {
    clog_set_format(lg, fmt);
    if (oversize) {
      size_t big_len = 66000;
      char *big = malloc(big_len + 1);
      if (big) {
        memset(big, 'x', big_len);
        big[big_len] = '\0';
        clog_info(lg, "%s", big);
        free(big);
      }
    }
    for (int i = 0; i < count; i++) clog_info(lg, "DATAGRAMRECORDBODY %d", i);
    clog_close(lg);
  }
  atomic_store(&rx->stop, true);
  pthread_join(tid, NULL);
  close(sv[0]);
  close(sv[1]);
  return lg != CLOG_NULL;
}

// Runs run_dgram_case_delayed() with a receiver that starts at once, the case
// most datagram tests need.
static bool run_dgram_case(int domain, clog_format fmt, int count,
                           bool oversize, dgram_rx_t *rx) {
  return run_dgram_case_delayed(domain, fmt, count, oversize, 0, rx);
}

// An async logger on a message-oriented socket writes each record with its
// own write, so each datagram carries one record. The count stays small
// enough for a loopback UDP receive buffer, which drops what doesn't fit.
TEST(datagram_sink, udp_logfmt_sends_one_record_per_datagram) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_INET, CLOG_FMT_LOGFMT, 64, false, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)64);
  REQUIRE_EQ(rx.with_body, (size_t)64);
  REQUIRE_EQ(rx.not_one, (size_t)0);
  REQUIRE_EQ(rx.markers, (size_t)0);
}

TEST(datagram_sink, udp_json_sends_one_record_per_datagram) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_INET, CLOG_FMT_JSON, 64, false, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)64);
  REQUIRE_EQ(rx.with_body, (size_t)64);
  REQUIRE_EQ(rx.not_one, (size_t)0);
  REQUIRE_TRUE(rx.json_ok);
}

// The logger waits for room on an AF_UNIX datagram socket and never drops, so
// this sends far more than one stream batch would hold.
TEST(datagram_sink, unix_logfmt_sends_one_record_per_datagram) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_UNIX, CLOG_FMT_LOGFMT, 2000, false, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)2000);
  REQUIRE_EQ(rx.with_body, (size_t)2000);
  REQUIRE_EQ(rx.not_one, (size_t)0);
}

// A receiver that reads nothing for a while fills the datagram queue. Linux
// blocks the send and FreeBSD refuses it with ENOBUFS, which the logger waits
// out; either way every record arrives with no loss marker.
TEST(datagram_sink, unix_full_receiver_queue_loses_no_record) {
  dgram_rx_t rx;
  bool ok =
      run_dgram_case_delayed(AF_UNIX, CLOG_FMT_LOGFMT, 2000, false, 300, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)2000);
  REQUIRE_EQ(rx.with_body, (size_t)2000);
  REQUIRE_EQ(rx.not_one, (size_t)0);
  REQUIRE_EQ(rx.markers, (size_t)0);
}

TEST(datagram_sink, unix_json_sends_one_record_per_datagram) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_UNIX, CLOG_FMT_JSON, 2000, false, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)2000);
  REQUIRE_EQ(rx.with_body, (size_t)2000);
  REQUIRE_EQ(rx.not_one, (size_t)0);
  REQUIRE_TRUE(rx.json_ok);
}

// A record too large for a UDP datagram fails alone, a marker names it, and
// the records after it arrive.
TEST(datagram_sink, udp_oversize_record_is_named_and_the_rest_arrive) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_INET, CLOG_FMT_LOGFMT, 8, true, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.markers, (size_t)1);
  REQUIRE_EQ(rx.with_body, (size_t)8);
  REQUIRE_EQ(rx.datagrams, (size_t)9);
  REQUIRE_EQ(rx.not_one, (size_t)0);
}

// RFC 5424 escaping

// A syslog MSG escapes a backslash too, so a real newline and the two
// characters backslash and n stay distinguishable.
TEST(syslog, msg_escaping_keeps_a_backslash_distinct_from_an_escape) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "%s", "FIRSTA\\nB");
  clog_info(lg, "%s", "SECONDA\nB");
  clog_close(lg);
  close(fd);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  bool first = strstr(buf, "FIRSTA\\\\nB\n") != NULL;
  bool second = strstr(buf, "SECONDA\\nB\n") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(first);
  REQUIRE_TRUE(second);
}

TEST(syslog, sd_param_value_is_valid_utf8) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_set_field(lg, "k",
                 "a\xff"
                 "b\xc3\xa9"
                 "c\xe2\x82");
  clog_info(lg, "utf8 check");
  clog_close(lg);
  close(fd);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  bool sanitized = strstr(buf,
                          "k=\"a\xef\xbf\xbd"
                          "b\xc3\xa9"
                          "c\xef\xbf\xbd\xef\xbf\xbd\"") != NULL;
  bool raw_ff = strchr(buf, '\xff') != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(sanitized);
  REQUIRE_FALSE(raw_ff);
}

// Deferred slot-table release

// Two loggers that the test leaves open for the destructor below. The release
// of the slot table fires when the last live slot goes away, so only the
// fixture torn down last reaches it; that is why one pair covers both the
// plain and the racing case, and why one more logger held by a destructor of
// this file would make the test check nothing. This file links ahead of
// src/clogger.c and destructors run in reverse link order, so the destructor
// below closes into a table whose release is already pending.
static clog g_deferred_release_a = CLOG_NULL;
static clog g_deferred_release_b = CLOG_NULL;

// Opens the pair, each with a derived handle that it closes, so each final
// close drops the last reference to its shared object and walks
// live_shareds. The pair is published only once fully set up. If the release
// runs ahead of a close still in flight, the binary aborts at exit in
// cvector_elem_count, after the summary; make test sees the exit status.
TEST(clogger_deferred_release, a_racing_pair_left_open_defers_then_releases) {
  int fd = open("/dev/null", O_WRONLY);
  REQUIRE_NE(fd, -1);
  clog a = clog_create_from_fd(fd, CLOG_INFO, NULL);
  clog b = clog_create_from_fd(fd, CLOG_INFO, NULL);
  clog da = (a != CLOG_NULL) ? clog_derive(a) : CLOG_NULL;
  clog db = (b != CLOG_NULL) ? clog_derive(b) : CLOG_NULL;
  if (da != CLOG_NULL) clog_close(da);
  if (db != CLOG_NULL) clog_close(db);

  bool opened =
      (a != CLOG_NULL && b != CLOG_NULL && da != CLOG_NULL && db != CLOG_NULL);
  if (opened) {
    g_deferred_release_a = a;
    g_deferred_release_b = b;
  } else {
    if (a != CLOG_NULL) clog_close(a);
    if (b != CLOG_NULL) clog_close(b);
  }
  REQUIRE_TRUE(opened);
}

// Closes g_deferred_release_b from a second thread, where the window hook
// holds it after it cleared its slot and before it reads the table, so the
// destructor can close A into that window.
static void *_close_b_in_the_window(void *unused) {
  (void)unused;
  clog_close(g_deferred_release_b);
  g_deferred_release_b = CLOG_NULL;
  return NULL;
}

// Closes A while B is held inside its release window, which is the race the
// deferred-release test exists for: A finds no live slot and would release the
// table under B unless the close in flight is counted. It waits for the hook
// to report that B is inside rather than sleeping, says on stderr when the
// race didn't happen, and closes both at once when the thread can't start.
__attribute__((destructor)) static void _close_the_deferred_release_pair(void) {
  if (g_deferred_release_a == CLOG_NULL || g_deferred_release_b == CLOG_NULL) {
    if (g_deferred_release_a != CLOG_NULL) clog_close(g_deferred_release_a);
    if (g_deferred_release_b != CLOG_NULL) clog_close(g_deferred_release_b);
    g_deferred_release_a = CLOG_NULL;
    g_deferred_release_b = CLOG_NULL;
    return;
  }

  clog__test_set_close_release_window_us(200000);
  pthread_t t;
  if (pthread_create(&t, NULL, _close_b_in_the_window, NULL) != 0) {
    clog__test_set_close_release_window_us(0);
    clog_close(g_deferred_release_a);
    clog_close(g_deferred_release_b);
    g_deferred_release_a = CLOG_NULL;
    g_deferred_release_b = CLOG_NULL;
    return;
  }

  for (long long deadline_ms = test_now_ms() + 20000;
       !clog__test_close_release_window_entered() &&
       test_now_ms() < deadline_ms;) {
    struct timespec ts = {0, 100L * 1000L};
    nanosleep(&ts, NULL);
  }
  if (!clog__test_close_release_window_entered())
    fprintf(stderr,
            "clogger_deferred_release: the close of B never entered its "
            "release window, so the deferred release was not raced\n");
  clog__test_set_close_release_window_us(0);
  clog_close(g_deferred_release_a);
  g_deferred_release_a = CLOG_NULL;
  pthread_join(t, NULL);
}

// Rotation pruning robustness

// Creates path and writes text into it, so a test can plant a rotated file.
// Returns 0 on success.
static int write_text_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  size_t len = strlen(text);
  ssize_t w = write(fd, text, len);
  close(fd);
  return (w == (ssize_t)len) ? 0 : -1;
}

// Moves path's modification time by seconds (back when negative), so a test
// can give planted files the times a stepped clock leaves. Returns 0 on
// success.
static int shift_file_mtime(const char *path, long seconds) {
  struct timeval tv[2];
  gettimeofday(&tv[0], NULL);
  tv[0].tv_sec += seconds;
  tv[1] = tv[0];
  return utimes(path, tv);
}

// Tells whether any file in dir whose name starts with prefix holds needle, so
// a test can check that a record survived somewhere among the generations.
static bool any_file_with_prefix_contains(const char *dir, const char *prefix,
                                          const char *needle) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  char path[1024];
  char buf[8192];
  while (!found && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    read_file(path, buf, sizeof(buf));
    if (strstr(buf, needle)) found = true;
  }
  closedir(d);
  return found;
}

// After the clock steps back, every name on disk sorts after the one a
// rotation creates, so ordering by name alone would make the new file look
// oldest and its own prune pass would delete it. The prune pass excludes the
// destination it just created (but counts it against the quota).
TEST(rotation, a_rotation_never_prunes_the_file_it_just_created) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  char ahead_of_now[3][32];
  for (int i = 0; i < 3; i++) {
    time_t t = time(NULL) + 24 * 60 * 60 + i;
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(ahead_of_now[i], sizeof(ahead_of_now[i]), "%Y%m%d%H%M%S", &tm);
  }
  char planted[1024];
  for (size_t i = 0; i < sizeof(ahead_of_now) / sizeof(ahead_of_now[0]); i++) {
    snprintf(planted, sizeof(planted), "%s.%s", path, ahead_of_now[i]);
    REQUIRE_EQ(write_text_file(planted, "older generation\n"), 0);
    REQUIRE_EQ(shift_file_mtime(planted, 3600), 0);
  }

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "PAYLOAD-MARKER-ROTATED %s",
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
            "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  clog_close(lg);

  bool payload_survived =
      any_file_with_prefix_contains(dir, "app.log", "PAYLOAD-MARKER-ROTATED");
  int rotated = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(payload_survived);
  REQUIRE_EQ((rotated <= 2), 1);
}

// Consumer-side layout agreement

// consumer_view.c is built without the feature-test macros of the suite and
// the library, as an ordinary application sees <clogger.h>, and both views of
// clog_rotation_cfg must have the same layout. On LP64 the test can't tell the
// cases apart; on ILP32 it does real work.
TEST(rotation_cfg_abi, a_consumer_sees_the_same_layout_the_library_does) {
  clogger_consumer_view_t consumer;
  clogger_consumer_fill_view(&consumer);

  REQUIRE_EQ(consumer.struct_size, sizeof(clog_rotation_cfg));
  REQUIRE_EQ(consumer.struct_align, _Alignof(clog_rotation_cfg));
  REQUIRE_EQ(consumer.off_size_rotation_enabled,
             offsetof(clog_rotation_cfg, size_rotation_enabled));
  REQUIRE_EQ(consumer.off_max_file_size,
             offsetof(clog_rotation_cfg, max_file_size));
  REQUIRE_EQ(consumer.off_time_rotation_enabled,
             offsetof(clog_rotation_cfg, time_rotation_enabled));
  REQUIRE_EQ(consumer.off_rotation_interval_us,
             offsetof(clog_rotation_cfg, rotation_interval_us));
  REQUIRE_EQ(consumer.off_max_rotated_files,
             offsetof(clog_rotation_cfg, max_rotated_files));
  REQUIRE_EQ(consumer.off_compress_rotated,
             offsetof(clog_rotation_cfg, compress_rotated));
  REQUIRE_EQ(consumer.sizeof_max_file_size, (size_t)8);
  REQUIRE_EQ(consumer.sizeof_rotation_interval_us, (size_t)8);
}

// The same agreement through the library: A config filled in by the consumer
// view must rotate as written and must not turn on gzip, which nobody set.
TEST(rotation_cfg_abi, a_consumer_built_config_rotates_as_it_was_written) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clogger_consumer_open_rotating(path, 200, 2);
  REQUIRE_NE(lg, CLOG_NULL);
  for (int i = 0; i < 20; i++)
    clog_info(lg, "consumer-built rotation config idx=%d padding-padding", i);
  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  int compressed = count_files_with_suffix(dir, ".gz");
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= 2), 1);
  REQUIRE_EQ(compressed, 0);
}

// Short-write record framing

// Counts the lines in buf that end with a newline, so a test can tell whether
// a fragment got joined to the next record.
static int count_lines(const char *buf) {
  int n = 0;
  for (const char *p = buf; *p; p++)
    if (*p == '\n') n++;
  return n;
}

// When the kernel takes only part of a write (a full disk, RLIMIT_FSIZE, a
// peer that goes away), the logger writes the missing newline itself and a
// marker naming the loss, so the next record starts a line of its own.
TEST(short_write, a_cut_short_record_is_terminated_and_the_loss_reported) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "FIRST-RECORD");
  clog__test_force_short_writes(30, 1);
  clog_info(lg, "SECOND-RECORD-cut-in-half");
  clog_info(lg, "THIRD-RECORD");
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_third = strstr(buf, "THIRD-RECORD") != NULL;
  int lines = count_lines(buf);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_third);
  REQUIRE_EQ(lines, 4);
}

// The bytes a short batch write didn't take continue what did land, so the
// next flush offers them again: That restores the cut record and delivers the
// records behind it instead of losing the whole batch.
TEST(short_write, an_async_batch_keeps_what_the_write_did_not_take) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "ASYNC-BATCHED-ONE");
  clog_info(lg, "ASYNC-BATCHED-TWO");
  clog_info(lg, "ASYNC-BATCHED-THREE");

  clog__test_force_short_writes(20, 1);
  clog_flush(lg);

  clog_info(lg, "ASYNC-BATCHED-FOUR");
  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));
  bool has_one = strstr(buf, "ASYNC-BATCHED-ONE") != NULL;
  bool has_two = strstr(buf, "ASYNC-BATCHED-TWO") != NULL;
  bool has_three = strstr(buf, "ASYNC-BATCHED-THREE") != NULL;
  bool has_four = strstr(buf, "ASYNC-BATCHED-FOUR") != NULL;
  int lines = count_lines(buf);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(has_one);
  REQUIRE_TRUE(has_two);
  REQUIRE_TRUE(has_three);
  REQUIRE_TRUE(has_four);
  REQUIRE_EQ(lines, 4);
}

// Counts the files in dir whose name starts with prefix and whose content
// holds needle, so a test can check that a record sits whole in one file. Each
// file is read on its own, so a string split across two files is found in
// neither.
static int count_files_containing(const char *dir, const char *prefix,
                                  const char *needle) {
  DIR *d = opendir(dir);
  if (!d) return 0;
  int count = 0;
  struct dirent *e;
  char path[1024];
  char contents[8192];
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
    read_file(path, contents, sizeof(contents));
    if (strstr(contents, needle)) count++;
  }
  closedir(d);
  return count;
}

// While the buffer holds the rest of a record cut by a short write, neither
// rotation check may rotate, or the two halves end up in two files. The flush
// that drains the buffer takes the due rotation instead. The test measures
// the record's exact length with a synchronous logger first.
TEST(short_write, rotation_waits_for_the_retained_half_of_a_record) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  char body[1025];
  memset(body, 'A', sizeof(body) - 1);
  body[sizeof(body) - 1] = '\0';
  memcpy(body, "SPLITME-HEAD", 12);
  memcpy(body + sizeof(body) - 1 - 12, "SPLITME-TAIL", 12);

  char probe_path[512];
  snprintf(probe_path, sizeof(probe_path), "%s/probe.log", dir);
  clog probe = clog_create_from_file(probe_path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(probe, CLOG_NULL);
  clog_info(probe, "%s", body);
  clog_close(probe);
  char probe_buf[8192];
  size_t record_len = read_file(probe_path, probe_buf, sizeof(probe_buf));
  unlink(probe_path);
  size_t cut = record_len > sizeof(body) ? record_len - (sizeof(body)) / 2 : 0;

  clog_rotation_cfg rcfg = {
      .size_rotation_enabled = true,
      .max_file_size = 64,
      .time_rotation_enabled = false,
      .rotation_interval_us = 0,
      .max_rotated_files = 8,
      .compress_rotated = false,
  };
  clog_async_cfg acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "%s", body);

  clog__test_force_short_writes(cut, 1);
  clog_flush(lg);
  clog_flush(lg);
  clog_close(lg);

  int whole = count_files_containing(dir, "app.log", body);
  int head = count_files_containing(dir, "app.log", "SPLITME-HEAD");
  int tail = count_files_containing(dir, "app.log", "SPLITME-TAIL");
  cleanup_dir(dir, "app.log");

  REQUIRE_GT((long)cut, 0L);
  REQUIRE_EQ(whole, 1);
  REQUIRE_EQ(head, 1);
  REQUIRE_EQ(tail, 1);
}

// Compression runs outside the logger mutex, so a later generation can carry
// an older mtime than an earlier one. Retention orders generations by the
// stamp in their names; ordering by mtime deletes the newest one.
TEST(rotation, retention_orders_rotated_files_by_name_not_modification_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  const char *names[3] = {"app.log.20260101120000.gz",
                          "app.log.20260101120005.gz",
                          "app.log.20260101120010.gz"};
  char full[3][800];
  for (int i = 0; i < 3; i++) {
    snprintf(full[i], sizeof(full[i]), "%s/%s", dir, names[i]);
    int fd = open(full[i], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    ssize_t w = write(fd, "x", 1);
    (void)w;
    close(fd);
    struct timespec ts[2];
    ts[0].tv_sec = 1000000 + (3 - i) * 1000;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    REQUIRE_EQ(utimensat(AT_FDCWD, full[i], ts, 0), 0);
  }

  {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    char filler[4095];
    memset(filler, 'x', sizeof(filler));
    ssize_t w = write(fd, filler, sizeof(filler));
    (void)w;
    close(fd);
  }

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 4096,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "one message to cross the size threshold");
  clog_close(lg);

  struct stat st;
  bool oldest_name_deleted = (stat(full[0], &st) != 0);
  bool newest_name_kept = (stat(full[2], &st) == 0);

  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(oldest_name_deleted);
  REQUIRE_TRUE(newest_name_kept);
}

// Retention keeps the newest generations
#include <zlib.h>

// The limits of the retention scenarios, which size the seen[][] table.
#define RTN_MAX_THREADS 6
#define RTN_MAX_RECORDS 600

// One writer of a retention scenario: Its logger, its index, how many records
// it writes, and the start gate (each writer counts itself in arrived and
// waits for go).
typedef struct {
  clog handle;
  int thread_index;
  int records;
  _Atomic int *arrived;
  _Atomic bool *go;
} rtn_writer_arg_t;

// Sleeps 100 microseconds, the poll step of the retention writers' gate.
static void _rtn_nap(void) {
  struct timespec ts = {0, 100000};
  nanosleep(&ts, NULL);
}

// Arrives at the gate, waits for go, then logs its numbered "rtn" records, so
// all writers of a retention scenario start together.
static void *_rtn_writer(void *p) {
  rtn_writer_arg_t *a = (rtn_writer_arg_t *)p;
  atomic_fetch_add(a->arrived, 1);
  while (!atomic_load(a->go)) _rtn_nap();
  for (int n = 0; n < a->records; n++)
    clog_info(a->handle, "rtn t=%d n=%d", a->thread_index, n);
  return NULL;
}

// Reads a whole log file into a heap buffer that the caller frees, or NULL,
// so tests can search any generation. gzread() reads a file that isn't
// compressed as it is, so this works for the live file and for plain and
// compressed rotated files.
static char *_rtn_slurp(const char *path) {
  gzFile f = gzopen(path, "rb");
  if (!f) return NULL;
  size_t cap = 4096, len = 0;
  char *buf = malloc(cap);
  if (!buf) {
    gzclose(f);
    return NULL;
  }
  for (;;) {
    if (cap - len < 2048) {
      char *nb = realloc(buf, cap * 2);
      if (!nb) break;
      buf = nb;
      cap *= 2;
    }
    int got = gzread(f, buf + len, (unsigned)(cap - len - 1));
    if (got <= 0) break;
    len += (size_t)got;
  }
  gzclose(f);
  buf[len] = '\0';
  return buf;
}

// Reads every file of the logger in dir and marks each "rtn t=<t> n=<n>"
// record in seen[t][n], so a scenario can see which records survived
// retention. Returns the number of distinct rotated generations (names
// counted once with or without ".gz").
static int _rtn_collect(const char *dir, bool seen[][RTN_MAX_RECORDS]) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  static char gens[512][sizeof(((struct dirent *)0)->d_name)];
  int ngens = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
    char *text = _rtn_slurp(full);
    if (text) {
      const char *p = text;
      while ((p = strstr(p, "rtn t=")) != NULL) {
        int t = -1, n = -1;
        if (sscanf(p, "rtn t=%d n=%d", &t, &n) == 2 && t >= 0 &&
            t < RTN_MAX_THREADS && n >= 0 && n < RTN_MAX_RECORDS)
          seen[t][n] = true;
        p++;
      }
      free(text);
    }
    if (strncmp(e->d_name, "app.log.", 8) != 0) continue;
    char g[sizeof(e->d_name)];
    snprintf(g, sizeof(g), "%s", e->d_name);
    size_t gl = strlen(g);
    if (gl > 3 && strcmp(g + gl - 3, ".gz") == 0) g[gl - 3] = '\0';
    bool dup = false;
    for (int i = 0; i < ngens; i++)
      if (strcmp(gens[i], g) == 0) dup = true;
    if (!dup && ngens < 512) snprintf(gens[ngens++], sizeof(gens[0]), "%s", g);
  }
  closedir(d);
  return ngens;
}

// Checks that each thread's surviving records in seen form one run that ends
// at its last record (or nothing, for a thread whose records all went out with
// older generations), since retention keeps the newest records. Returns the
// number of threads whose oldest records were pruned, or -1 on a gap or when
// nothing survived.
static int _rtn_check_suffixes(bool seen[][RTN_MAX_RECORDS], int threads,
                               int records) {
  int pruned_threads = 0;
  bool any_survivor = false;
  for (int t = 0; t < threads; t++) {
    int lo = 0;
    while (lo < records && !seen[t][lo]) lo++;
    if (lo < records) any_survivor = true;
    for (int n = lo; n < records; n++)
      if (!seen[t][n]) {
        fprintf(stderr, "retention gap: thread %d kept n=%d, lost n=%d\n", t,
                lo, n);
        return -1;
      }
    if (lo > 0) pruned_threads++;
  }
  return any_survivor ? pruned_threads : -1;
}

// One retention scenario: Compression, async mode, writer and record counts,
// and whether writers log through derived handles.
typedef struct {
  bool compress;
  bool async;
  int threads;
  int records;
  bool use_derived;
} rtn_case_t;

// Drives one retention scenario c and checks the newest-generations rule,
// storing the number of generations in *out_gens. Returns 0, or a distinct
// nonzero code for each failure, so the caller asserts once everything is
// joined and closed. The scenario must really prune, or it proves nothing.
static int _rtn_run_case(const rtn_case_t *c, int *out_gens) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return 1;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1024,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = c->compress,
  };
  clog_async_cfg acfg = {
      .queue_size = 0, .flush_buffer_size = 256, .flush_interval_us = 5000};

  clog root = clog_create_from_file_mp(path, CLOG_INFO, &cfg,
                                       c->async ? &acfg : NULL, NULL);
  if (root == CLOG_NULL) {
    cleanup_dir(dir, "app.log");
    return 2;
  }
  clog derived = c->use_derived ? clog_derive(root) : CLOG_NULL;

  _Atomic int arrived = 0;
  _Atomic bool go = false;
  pthread_t tids[RTN_MAX_THREADS];
  rtn_writer_arg_t args[RTN_MAX_THREADS];
  int started = 0;
  for (int i = 0; i < c->threads; i++) {
    args[i].handle = (c->use_derived && (i % 2)) ? derived : root;
    args[i].thread_index = i;
    args[i].records = c->records;
    args[i].arrived = &arrived;
    args[i].go = &go;
    if (pthread_create(&tids[i], NULL, _rtn_writer, &args[i]) != 0) break;
    started++;
  }
  while (atomic_load(&arrived) < started) _rtn_nap();
  atomic_store(&go, true);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);

  if (derived != CLOG_NULL) clog_close(derived);
  clog_close(root);

  int rc = 0;
  if (started != c->threads) rc = 3;

  static bool seen[RTN_MAX_THREADS][RTN_MAX_RECORDS];
  memset(seen, 0, sizeof(seen));
  int gens = _rtn_collect(dir, seen);
  if (out_gens) *out_gens = gens;
  if (rc == 0 && gens < 0) rc = 4;
  if (rc == 0 && gens > cfg.max_rotated_files + 1) rc = 5;
  int pruned = _rtn_check_suffixes(seen, c->threads, c->records);
  if (rc == 0 && pruned < 0) rc = 6;
  if (rc == 0 && pruned == 0) rc = 7;

  cleanup_dir(dir, "app.log");
  return rc;
}

TEST(retention, newest_generations_survive_sync_plain) {
  rtn_case_t c = {false, false, 1, 400, false};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_sync_gzip) {
  rtn_case_t c = {true, false, 1, 400, false};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_async_plain) {
  rtn_case_t c = {false, true, 1, 400, false};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_async_gzip) {
  rtn_case_t c = {true, true, 1, 400, false};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_threads_root_and_derived_plain) {
  rtn_case_t c = {false, false, 4, 150, true};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_threads_root_and_derived_gzip) {
  rtn_case_t c = {true, false, 4, 150, true};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

TEST(retention, newest_generations_survive_async_threads_derived_gzip) {
  rtn_case_t c = {true, true, 4, 150, true};
  REQUIRE_EQ(_rtn_run_case(&c, NULL), 0);
}

// A logger reopened on the same path (a restart within the same second)
// numbers its rotations above every name on disk. Otherwise it takes a name
// that retention freed, that name sorts as the oldest, and the next pass
// deletes the newest records.
TEST(retention, a_reopened_logger_numbers_above_the_names_on_disk) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1024,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
  };

  int open_failures = 0;
  for (int round = 0; round < 2; round++) {
    clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
    if (lg == CLOG_NULL) {
      open_failures++;
      continue;
    }
    int first = round == 0 ? 0 : 200, last = round == 0 ? 200 : 230;
    for (int n = first; n < last; n++) clog_info(lg, "rtn t=0 n=%d", n);
    clog_close(lg);
  }

  static bool seen[RTN_MAX_THREADS][RTN_MAX_RECORDS];
  memset(seen, 0, sizeof(seen));
  int gens = _rtn_collect(dir, seen);
  int pruned = _rtn_check_suffixes(seen, 1, 230);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(open_failures, 0);
  REQUIRE_LE(gens, 4);
  REQUIRE_EQ(pruned, 1);
}

// Writes a one-byte file at dir/name, so a test can plant a generation.
// Returns 0 on success.
static int _rtn_touch(const char *dir, const char *name) {
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", dir, name);
  int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  ssize_t w = write(fd, "x", 1);
  close(fd);
  return w == 1 ? 0 : -1;
}

// Tells whether dir/name exists, so a test can check what retention kept.
static bool _rtn_exists(const char *dir, const char *name) {
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", dir, name);
  struct stat st;
  return stat(full, &st) == 0;
}

// A new name is above every generation on disk, even one stamped a day ahead
// of the clock. Suffixes compare as numbers, so "_10000" is newer than
// "_9999".
TEST(retention, a_new_name_sorts_above_every_existing_name) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  time_t ahead = time(NULL) + 24 * 60 * 60;
  struct tm tm;
  gmtime_r(&ahead, &tm);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
  char n9998[64], n9999[64], n10000[64], n10001[64];
  snprintf(n9998, sizeof(n9998), "app.log.%s_9998", stamp);
  snprintf(n9999, sizeof(n9999), "app.log.%s_9999", stamp);
  snprintf(n10000, sizeof(n10000), "app.log.%s_10000", stamp);
  snprintf(n10001, sizeof(n10001), "app.log.%s_10001", stamp);

  int touch_failures = 0;
  if (_rtn_touch(dir, n9998) != 0) touch_failures++;
  if (_rtn_touch(dir, n9999) != 0) touch_failures++;

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  bool opened = (lg != CLOG_NULL);
  bool after_first_10000 = false, after_first_9998_gone = false;
  if (opened) {
    clog_info(lg, "first");
    after_first_10000 = _rtn_exists(dir, n10000);
    after_first_9998_gone = !_rtn_exists(dir, n9998);
    clog_info(lg, "second");
    clog_close(lg);
  }
  bool kept_10000 = _rtn_exists(dir, n10000);
  bool kept_10001 = _rtn_exists(dir, n10001);
  bool dropped_9999 = !_rtn_exists(dir, n9999);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(touch_failures, 0);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(after_first_10000);
  REQUIRE_TRUE(after_first_9998_gone);
  REQUIRE_TRUE(kept_10000);
  REQUIRE_TRUE(kept_10001);
  REQUIRE_TRUE(dropped_9999);
}

// Retention bound under concurrent compression

// Writers and records per writer of the tests below, which rotate on every
// record with each compression held for 20 ms.
#define RTN_BOUND_THREADS 6
#define RTN_BOUND_RECORDS 6

// A deletion pass counts the generations under compression against
// max_rotated_files and deletes every older one not under compression, so the
// directory holds at most max_rotated_files plus the one compression that
// runs, and every .gz is valid. A pass that skips them without counting them
// keeps too many.
TEST(retention, concurrent_compressions_keep_the_documented_bound) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_reset_max_rotated_generations();
  clog__test_set_pending_compress_delay_us(20000);

  _Atomic int arrived = 0;
  _Atomic bool go = false;
  pthread_t tids[RTN_BOUND_THREADS];
  rtn_writer_arg_t args[RTN_BOUND_THREADS];
  int started = 0;
  for (int i = 0; i < RTN_BOUND_THREADS; i++) {
    args[i].handle = lg;
    args[i].thread_index = i;
    args[i].records = RTN_BOUND_RECORDS;
    args[i].arrived = &arrived;
    args[i].go = &go;
    if (pthread_create(&tids[i], NULL, _rtn_writer, &args[i]) != 0) break;
    started++;
  }
  while (atomic_load(&arrived) < started) _rtn_nap();
  atomic_store(&go, true);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);

  clog__test_set_pending_compress_delay_us(0);
  clog_close(lg);
  int max_gens = clog__test_get_max_rotated_generations();
  int max_beyond = clog__test_get_max_generations_beyond_old_compressions();

  static bool seen[RTN_MAX_THREADS][RTN_MAX_RECORDS];
  memset(seen, 0, sizeof(seen));
  int gens_at_end = _rtn_collect(dir, seen);

  int invalid_gz = 0;
  DIR *d = opendir(dir);
  if (d) {
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      size_t nlen = strlen(e->d_name);
      if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
      char gz_path[1024];
      snprintf(gz_path, sizeof(gz_path), "%s/%s", dir, e->d_name);
      if (gunzip_test(gz_path) != 0) invalid_gz++;
    }
    closedir(d);
  }
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(started, RTN_BOUND_THREADS);
  REQUIRE_GT(max_gens, 0);
  REQUIRE_LE(max_beyond, cfg.max_rotated_files);
  REQUIRE_LE(gens_at_end, cfg.max_rotated_files + 1);
  REQUIRE_EQ(invalid_gz, 0);
}

// When rotations don't wait for their compressions, the deletion passes see
// several outstanding at once; a rotation that waited would cap that at one.
// The test reads that count instead of timing anything.
TEST(retention, a_rotation_never_waits_for_a_compression) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_reset_max_rotated_generations();
  clog__test_set_pending_compress_delay_us(20000);

  _Atomic int arrived = 0;
  _Atomic bool go = false;
  pthread_t tids[RTN_BOUND_THREADS];
  rtn_writer_arg_t args[RTN_BOUND_THREADS];
  int started = 0;
  for (int i = 0; i < RTN_BOUND_THREADS; i++) {
    args[i].handle = lg;
    args[i].thread_index = i;
    args[i].records = RTN_BOUND_RECORDS;
    args[i].arrived = &arrived;
    args[i].go = &go;
    if (pthread_create(&tids[i], NULL, _rtn_writer, &args[i]) != 0) break;
    started++;
  }
  while (atomic_load(&arrived) < started) _rtn_nap();
  atomic_store(&go, true);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);

  clog__test_set_pending_compress_delay_us(0);
  clog_close(lg);
  int max_in_flight = clog__test_get_max_compressions_in_flight();
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(started, RTN_BOUND_THREADS);
  REQUIRE_GE(max_in_flight, 3);
}

// Thread identity

// The name, PID and TID that a record's proc field carries.
typedef struct {
  char name[64];
  int pid;
  int tid;
} tid_proc_t;

// Parses proc=<progname>(<pid>):<name>(<tid>) of the logfmt record whose
// message is msg in text into out, so the identity tests can compare the
// fields. Returns true when found.
static bool _tid_parse_proc(const char *text, const char *msg,
                            tid_proc_t *out) {
  const char *m = strstr(text, msg);
  if (!m) return false;
  const char *line = m;
  while (line > text && line[-1] != '\n') line--;
  const char *proc = strstr(line, "proc=");
  if (!proc || proc > m) return false;
  const char *pid_paren = strchr(proc, '(');
  if (!pid_paren || pid_paren > m) return false;
  out->pid = atoi(pid_paren + 1);
  const char *colon = strstr(pid_paren, "):");
  if (!colon || colon > m) return false;
  const char *name = colon + 2;
  const char *tid_paren = strchr(name, '(');
  if (!tid_paren || tid_paren > m) return false;
  size_t nlen = (size_t)(tid_paren - name);
  if (nlen >= sizeof(out->name)) return false;
  memcpy(out->name, name, nlen);
  out->name[nlen] = '\0';
  out->tid = atoi(tid_paren + 1);
  return true;
}

// One worker of each_thread_reads_its_identity_once: What it renamed itself
// to, its TID and how many times it read its identity.
typedef struct {
  clog lg;
  int index;
  int tid;
  unsigned fills;
  ccol_retval set_rv;
} tid_worker_t;

// Renames itself through ccol_set_thread_name(), logs 50 records and stores
// its TID and its identity fill count in w, so the test can check that each
// thread read its identity once and logs its own.
static void *_tid_worker(void *p) {
  tid_worker_t *w = (tid_worker_t *)p;
  char name[16];
  snprintf(name, sizeof(name), "tid-worker-%d", w->index);
  w->set_rv = ccol_set_thread_name(name);
  w->tid = _test_os_tid();
  for (int i = 0; i < 50; i++)
    clog_info(w->lg, "tid worker %d record %d", w->index, i);
  w->fills = clog__test_thread_identity_fill_count();
  return NULL;
}

// A thread reads its identity from the system once however many records it
// writes, and each record carries its own name, PID and TID.
TEST(thread_identity, each_thread_reads_its_identity_once) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  enum { N = 4 };
  pthread_t th[N];
  tid_worker_t w[N];
  int started = 0;
  for (int i = 0; i < N; i++) {
    memset(&w[i], 0, sizeof(w[i]));
    w[i].lg = lg;
    w[i].index = i;
    if (pthread_create(&th[i], NULL, _tid_worker, &w[i]) != 0) break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(th[i], NULL);
  clog_close(lg);

  static char text[1 << 20];
  read_file(path, text, sizeof(text));
  bool all_ok = true;
  for (int i = 0; i < started; i++) {
    char name[32], msg0[64], msg49[64];
    snprintf(name, sizeof(name), "tid-worker-%d", i);
    snprintf(msg0, sizeof(msg0), "tid worker %d record 0\"", i);
    snprintf(msg49, sizeof(msg49), "tid worker %d record 49\"", i);
    tid_proc_t a, b;
    bool pa = _tid_parse_proc(text, msg0, &a);
    bool pb = _tid_parse_proc(text, msg49, &b);
    if (!pa || !pb || strcmp(a.name, name) != 0 || strcmp(b.name, name) != 0 ||
        a.tid != w[i].tid || b.tid != w[i].tid || a.pid != (int)getpid() ||
        w[i].fills != 1 || w[i].set_rv != ccol_success) {
      fprintf(stderr, "thread %d: parsed %d/%d name '%s' tid %d/%d fills %u\n",
              i, pa, pb, pa ? a.name : "", pa ? a.tid : -1, w[i].tid,
              w[i].fills);
      all_ok = false;
    }
  }
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(started, N);
  REQUIRE_TRUE(all_ok);
}

// What _tid_rename_worker() saw: The results of each rename and the name the
// kernel holds after the long and the refused renames.
typedef struct {
  clog lg;
  ccol_retval rv_rename;
  ccol_retval rv_long;
  ccol_retval rv_null;
  ccol_retval rv_empty;
  char kernel_after_long[17];
  char kernel_after_invalid[17];
} tid_rename_t;

// Logs around renames (a valid one, one longer than 15 bytes, NULL, "" and one
// that goes around the library) and stores each result and the kernel's name
// in r, so the test can check what each rename does to the next record.
static void *_tid_rename_worker(void *p) {
  tid_rename_t *r = (tid_rename_t *)p;
  clog_info(r->lg, "rename before");
  r->rv_rename = ccol_set_thread_name("renamed-thread");
  clog_info(r->lg, "rename after");
  r->rv_long = ccol_set_thread_name("abcdefghijklmnopqrstuvwxyz");
  _test_get_thread_name(r->kernel_after_long);
  clog_info(r->lg, "rename long");
  r->rv_null = ccol_set_thread_name(NULL);
  r->rv_empty = ccol_set_thread_name("");
  _test_get_thread_name(r->kernel_after_invalid);
  clog_info(r->lg, "rename invalid");
  _test_set_thread_name("other-means");
  clog_info(r->lg, "rename other means");
  return NULL;
}

// A rename through ccol_set_thread_name() shows in the next record. A name
// longer than 15 bytes keeps its first 15, as the kernel does, and NULL and ""
// are refused and change nothing.
TEST(thread_identity, set_thread_name_renames_and_updates_records) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  tid_rename_t r;
  memset(&r, 0, sizeof(r));
  r.lg = lg;
  pthread_t th;
  int prv = pthread_create(&th, NULL, _tid_rename_worker, &r);
  if (prv == 0) pthread_join(th, NULL);
  clog_close(lg);

  static char text[1 << 16];
  read_file(path, text, sizeof(text));
  tid_proc_t before, after, lng, inv, other;
  bool ok_before = _tid_parse_proc(text, "rename before", &before);
  bool ok_after = _tid_parse_proc(text, "rename after", &after);
  bool ok_long = _tid_parse_proc(text, "rename long", &lng);
  bool ok_inv = _tid_parse_proc(text, "rename invalid", &inv);
  bool ok_other = _tid_parse_proc(text, "rename other means", &other);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(prv, 0);
  REQUIRE_TRUE(ok_before && ok_after && ok_long && ok_inv && ok_other);
  REQUIRE_EQ(r.rv_rename, ccol_success);
  REQUIRE_EQ(r.rv_long, ccol_success);
  REQUIRE_EQ(r.rv_null, ccol_invalid_args);
  REQUIRE_EQ(r.rv_empty, ccol_invalid_args);
  REQUIRE_STRNE(before.name, "renamed-thread");
  REQUIRE_STREQ(after.name, "renamed-thread");
  REQUIRE_STREQ(r.kernel_after_long, "abcdefghijklmno");
  REQUIRE_STREQ(lng.name, "abcdefghijklmno");
  REQUIRE_STREQ(r.kernel_after_invalid, "abcdefghijklmno");
  REQUIRE_STREQ(inv.name, "abcdefghijklmno");
  REQUIRE_STREQ(other.name, "abcdefghijklmno");
  REQUIRE_EQ(before.tid, after.tid);
}

// Renames itself before any logger exists, then logs under that name, to show
// that ccol_set_thread_name() needs no logger.
static void *_tid_early_worker(void *p) {
  tid_rename_t *r = (tid_rename_t *)p;
  r->rv_rename = ccol_set_thread_name("early-name");
  return NULL;
}

TEST(thread_identity, set_thread_name_works_before_any_logger) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  tid_rename_t r;
  memset(&r, 0, sizeof(r));
  pthread_t th;
  int prv = pthread_create(&th, NULL, _tid_early_worker, &r);
  if (prv == 0) pthread_join(th, NULL);

  char saved[17] = {0};
  _test_get_thread_name(saved);
  ccol_retval main_rv = ccol_set_thread_name("early-main");
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_NULL) {
    clog_info(lg, "early record");
    clog_close(lg);
  }
  ccol_retval restore_rv = ccol_set_thread_name(saved);

  static char text[1 << 16];
  read_file(path, text, sizeof(text));
  tid_proc_t rec;
  bool parsed = _tid_parse_proc(text, "early record", &rec);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(prv, 0);
  REQUIRE_EQ(r.rv_rename, ccol_success);
  REQUIRE_EQ(main_rv, ccol_success);
  REQUIRE_EQ(restore_rv, ccol_success);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(parsed);
  REQUIRE_STREQ(rec.name, "early-main");
  REQUIRE_EQ(rec.tid, _test_os_tid());
  REQUIRE_EQ(rec.pid, (int)getpid());
}

// The forking thread's cached identity must not follow it into the child,
// which opens its own logger after the fork(). The parent logs first, so its
// identity is cached at the fork.
TEST(thread_identity, a_forked_child_logs_its_own_pid_and_tid) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "parent record");
  clog_close(lg);

  pid_t pid = fork();
  if (pid == 0) {
    clog clg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
    if (clg != CLOG_NULL) {
      clog_info(clg, "child record");
      clog_close(clg);
    }
    test_child_exit(0);
  }
  int status = 0;
  bool reaped = false;
  if (pid > 0) {
    for (long long deadline_ms = test_now_ms() + 30000;
         test_now_ms() < deadline_ms;) {
      if (waitpid(pid, &status, WNOHANG) == pid) {
        reaped = true;
        break;
      }
      usleep(20000);
    }
    if (!reaped) {
      kill(pid, SIGKILL);
      waitpid(pid, &status, 0);
    }
  }

  static char text[1 << 16];
  read_file(path, text, sizeof(text));
  tid_proc_t parent_rec, child_rec;
  bool parsed_parent = _tid_parse_proc(text, "parent record", &parent_rec);
  bool parsed_child = _tid_parse_proc(text, "child record", &child_rec);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(reaped);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(parsed_parent);
  REQUIRE_TRUE(parsed_child);
  REQUIRE_EQ(parent_rec.pid, (int)getpid());
  REQUIRE_EQ(child_rec.pid, (int)pid);
#if defined(__linux__)
  REQUIRE_EQ(child_rec.tid, (int)pid);
#else
  REQUIRE_NE(child_rec.tid, parent_rec.tid);
#endif
}

// Rotation directory, background compression, writer wait
#include <limits.h>

// Makes a unique directory under the working directory and registers it for
// the leftover cleanup, so the files of these tests land on the filesystem the
// tests run from. Writes its relative path into out and returns 0, or -1.
static int make_cwd_tmpdir(char *out, size_t outsz) {
  snprintf(out, outsz, "clogger_cwd_test_XXXXXX");
  if (!mkdtemp(out)) return -1;
  register_test_tmpdir(out);
  return 0;
}

// Counts the rotated files "<base>.<...>" in dir into *plain and *gz, and the
// .gz files gunzip -t refuses into *invalid_gz, so a compression test can
// check what each rotation left.
static void count_rotated(const char *dir, const char *base, int *plain,
                          int *gz, int *invalid_gz) {
  *plain = 0;
  *gz = 0;
  *invalid_gz = 0;
  char prefix[128];
  snprintf(prefix, sizeof(prefix), "%s.", base);
  size_t plen = strlen(prefix);
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, plen) != 0) continue;
    size_t nlen = strlen(e->d_name);
    if (nlen >= 3 && strcmp(e->d_name + nlen - 3, ".gz") == 0) {
      (*gz)++;
      char gz_path[1024];
      snprintf(gz_path, sizeof(gz_path), "%s/%s", dir, e->d_name);
      if (gunzip_test(gz_path) != 0) (*invalid_gz)++;
    } else {
      (*plain)++;
    }
  }
  closedir(d);
}

// Tells whether a file in dir whose name starts with prefix holds needle (read
// as plain text), so a test can find where a record went.
static bool dir_files_contain(const char *dir, const char *prefix,
                              const char *needle) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  static char text[1 << 14];
  while (!found && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    read_file(p, text, sizeof(text));
    found = strstr(text, needle) != NULL;
  }
  closedir(d);
  return found;
}

// Waits, for at most 30 seconds, until a held compression has created its
// destination, so a test rotates again only once that compression is really
// running. Returns false on timeout.
static bool _wait_for_gz_dest_opened(void) {
  for (long long deadline_ms = test_now_ms() + 30000;
       test_now_ms() < deadline_ms;) {
    if (clog__test_gz_dest_opened()) return true;
    usleep(1000);
  }
  return false;
}

// readdir() can miss both names of the running compression while it publishes
// the .gz and unlinks the source. A pass that misses them must still count
// that generation, and ask for a second pass when the compression ends. A hook
// hides the names on every pass.
TEST(retention, a_pass_that_misses_the_running_compression_still_counts_it) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);
  clog_info(lg, "rtn t=0 n=0");
  clog_info(lg, "rtn t=0 n=1");
  bool held = _wait_for_gz_dest_opened();
  clog__test_set_prune_misses_running(true);
  for (int i = 2; i < 6; i++) clog_info(lg, "rtn t=0 n=%d", i);
  clog__test_set_prune_misses_running(false);
  clog__test_hold_compressions(false);
  clog_close(lg);

  static bool seen[RTN_MAX_THREADS][RTN_MAX_RECORDS];
  memset(seen, 0, sizeof(seen));
  int gens_at_end = _rtn_collect(dir, seen);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(held);
  REQUIRE_EQ(gens_at_end, cfg.max_rotated_files);
}

// Every rename, open and deletion of a rotation works relative to the
// directory where the file was opened, so a later chdir() doesn't move the
// log. The test restores the cwd before any REQUIRE.
TEST(rotation_dir, relative_path_keeps_its_directory_after_chdir) {
  char root[64];
  REQUIRE_EQ(make_cwd_tmpdir(root, sizeof(root)), 0);
  char a[128], b[128];
  snprintf(a, sizeof(a), "%s/A", root);
  snprintf(b, sizeof(b), "%s/B", root);
  bool made = mkdir(a, 0755) == 0 && mkdir(b, 0755) == 0;
  int oldcwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  bool moved_in = made && oldcwd >= 0 && chdir(a) == 0;
  clog lg = CLOG_NULL;
  bool moved_on = false;
  if (moved_in) {
    clog_rotation_cfg cfg = {
        .size_rotation_enabled = true,
        .max_file_size = 200,
        .max_rotated_files = 3,
    };
    lg = clog_create_from_file("rel.log", CLOG_INFO, &cfg, NULL);
    if (lg != CLOG_NULL) {
      clog_info(lg, "before chdir");
      moved_on = chdir("../B") == 0;
      for (int i = 0; i < 8; i++)
        clog_info(lg, "after chdir %d padding-padding-padding-padding", i);
    }
  }
  bool restored = oldcwd >= 0 && fchdir(oldcwd) == 0;
  if (oldcwd >= 0) close(oldcwd);
  if (lg != CLOG_NULL) clog_close(lg);

  char live_a[256], live_b[256];
  snprintf(live_a, sizeof(live_a), "%s/rel.log", a);
  snprintf(live_b, sizeof(live_b), "%s/rel.log", b);
  bool live_in_a = access(live_a, F_OK) == 0;
  bool stray_in_b = access(live_b, F_OK) == 0;
  int rotated_in_a = count_files_with_prefix(a, "rel.log.");
  int files_in_b = count_files_with_prefix(b, "rel.log");
  bool last_record_in_a = dir_files_contain(a, "rel.log", "after chdir 7");
  cleanup_dir(a, "rel.log");
  cleanup_dir(b, "rel.log");
  rmdir(root);

  REQUIRE_TRUE(restored);
  REQUIRE_TRUE(moved_in);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(moved_on);
  REQUIRE_TRUE(live_in_a);
  REQUIRE_TRUE(last_record_in_a);
  REQUIRE_FALSE(stray_in_b);
  REQUIRE_EQ(files_in_b, 0);
  REQUIRE_GT(rotated_in_a, 0);
  REQUIRE_LE(rotated_in_a, 3);
}

// A flush interval of UINT64_MAX ("flush on size only") must not become a
// negative signed value and a zero wait, so the idle writer's count of timed
// waits that found no job stays at zero.
TEST(async, flush_interval_above_llong_max_does_not_spin_the_writer) {
  const uint64_t intervals[] = {UINT64_MAX, (uint64_t)LLONG_MAX + 1u};
  for (size_t k = 0; k < sizeof(intervals) / sizeof(intervals[0]); k++) {
    int pipefd[2];
    REQUIRE_EQ(pipe(pipefd), 0);
    clog_async_cfg acfg = {.flush_interval_us = intervals[k]};
    clog lg = clog_create_from_fd(pipefd[1], CLOG_INFO, &acfg);
    if (lg == CLOG_NULL) {
      close(pipefd[0]);
      close(pipefd[1]);
    }
    REQUIRE_NE(lg, CLOG_NULL);

    clog_info(lg, "size-only flushing %zu", k);
    clog_flush(lg);
    size_t before = clog__test_writer_timeout_wakeups(lg);
    usleep(200000);
    size_t after = clog__test_writer_timeout_wakeups(lg);

    char out[4096];
    drain_pipe(lg, pipefd[0], pipefd[1], out, sizeof(out));
    char expected[64];
    snprintf(expected, sizeof(expected), "size-only flushing %zu", k);

    REQUIRE_EQ(after - before, (size_t)0);
    REQUIRE_NE(strstr(out, expected), NULL);
  }
}

// One producer of the sustained-load test: Its logger, the start gate
// (arrived and release) and the stop flag.
typedef struct {
  clog lg;
  atomic_int *arrived;
  atomic_bool *release;
  atomic_bool *stop;
} interval_load_arg_t;

// Waits at the gate, then logs as fast as it can until stop is set, so the
// bounded queue of the sustained-load test stays full.
static void *interval_load_producer(void *p) {
  interval_load_arg_t *a = (interval_load_arg_t *)p;
  atomic_fetch_add(a->arrived, 1);
  while (!atomic_load(a->release)) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&pause, NULL);
  }
  unsigned long n = 0;
  while (!atomic_load(a->stop)) clog_info(a->lg, "sustained load %lu", n++);
  return NULL;
}

// Producers keep a small queue full, the writer takes 200 us per record and
// the buffer never fills, so only the interval can start a write. The writer
// checks the interval before each receive; otherwise the file wouldn't grow.
// Gaps are measured so a pause of the whole process doesn't count.
TEST(async, flush_interval_holds_under_sustained_load) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/load.log", dir);

  clog_async_cfg acfg = {.queue_size = 64,
                         .flush_buffer_size = 64u * 1024u * 1024u,
                         .flush_interval_us = 50000};
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_NULL) cleanup_dir(dir, "load.log");
  REQUIRE_NE(lg, CLOG_NULL);
  clog__test_set_writer_job_delay_us(200);

  enum { PRODUCERS = 2 };
  atomic_int arrived = 0;
  atomic_bool release = false;
  atomic_bool stop = false;
  interval_load_arg_t arg = {lg, &arrived, &release, &stop};
  pthread_t tids[PRODUCERS];
  int started = 0;
  for (int i = 0; i < PRODUCERS; i++) {
    if (pthread_create(&tids[i], NULL, interval_load_producer, &arg) != 0)
      break;
    started++;
  }
  while (atomic_load(&arrived) < started) {
    struct timespec pause = {.tv_sec = 0, .tv_nsec = 1000000L};
    nanosleep(&pause, NULL);
  }
  atomic_store(&release, true);

  usleep(100000);
  size_t flushes_before = clog__test_writer_interval_flushes(lg);
  struct stat st;
  off_t last_size = stat(path, &st) == 0 ? st.st_size : 0;
  long long start_ms = test_now_ms();
  long long last_growth_ms = start_ms;
  long long last_sample_ms = start_ms;
  long long longest_gap_ms = 0;
  int growths = 0;
  for (int samples = 0;; samples++) {
    long long now_ms = test_now_ms();
    if ((now_ms - start_ms >= 800 && samples >= 100) ||
        now_ms - start_ms >= 20000)
      break;
    if (now_ms - last_sample_ms > 100) {
      if (last_sample_ms - last_growth_ms > longest_gap_ms)
        longest_gap_ms = last_sample_ms - last_growth_ms;
      last_growth_ms = now_ms;
    }
    if (stat(path, &st) == 0 && st.st_size > last_size) {
      last_size = st.st_size;
      growths++;
      if (last_sample_ms - last_growth_ms > longest_gap_ms)
        longest_gap_ms = last_sample_ms - last_growth_ms;
      last_growth_ms = now_ms;
    }
    last_sample_ms = now_ms;
    usleep(5000);
  }
  if (last_sample_ms - last_growth_ms > longest_gap_ms)
    longest_gap_ms = last_sample_ms - last_growth_ms;
  size_t flushes = clog__test_writer_interval_flushes(lg) - flushes_before;

  atomic_store(&stop, true);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  clog__test_set_writer_job_delay_us(0);
  clog_close(lg);
  cleanup_dir(dir, "load.log");

  REQUIRE_EQ(started, PRODUCERS);
  REQUIRE_GE(flushes, (size_t)4);
  REQUIRE_GE(growths, 4);
  REQUIRE_LT(longest_gap_ms, 400LL);
}

// A log call that causes a rotation returns while the compression is held,
// because gzip runs on the compressor thread.
TEST(compression, gzip_never_runs_on_the_logging_thread) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);
  size_t fin0 = clog__test_compressions_finished();
  clog_info(lg, "rotated while the gate is held");
  size_t fin_after_log = clog__test_compressions_finished() - fin0;
  bool opened = _wait_for_gz_dest_opened();
  size_t fin_while_held = clog__test_compressions_finished() - fin0;
  clog__test_hold_compressions(false);
  clog_close(lg);

  int plain, gz, invalid_gz;
  count_rotated(dir, "app.log", &plain, &gz, &invalid_gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(fin_after_log, (size_t)0);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(fin_while_held, (size_t)0);
  REQUIRE_EQ(gz, 1);
  REQUIRE_EQ(plain, 0);
  REQUIRE_EQ(invalid_gz, 0);
}

// clog_flush() waits for the batch and its rotation, but returns while the
// queued compression is held, so neither the writer thread nor a producer
// waits for gzip.
TEST(compression, gzip_never_runs_on_the_async_writer_thread) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };
  clog_async_cfg acfg = {.queue_size = 4};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, &acfg);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);
  size_t fin0 = clog__test_compressions_finished();
  clog_info(lg, "rotated by the writer while the gate is held");
  clog_flush(lg);
  size_t fin_after_flush = clog__test_compressions_finished() - fin0;
  bool opened = _wait_for_gz_dest_opened();
  size_t fin_while_held = clog__test_compressions_finished() - fin0;
  clog__test_hold_compressions(false);
  clog_close(lg);

  int plain, gz, invalid_gz;
  count_rotated(dir, "app.log", &plain, &gz, &invalid_gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(fin_after_flush, (size_t)0);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(fin_while_held, (size_t)0);
  REQUIRE_EQ(gz, 1);
  REQUIRE_EQ(plain, 0);
  REQUIRE_EQ(invalid_gz, 0);
}

// Closes the logger at arg from a thread, so the test can release the held
// compression while the close waits for it.
static void *_close_in_background(void *arg) {
  clog_close(*(clog *)arg);
  return NULL;
}

// The last clog_close() of a target returns only after every queued
// compression has finished: The close starts while one compression is held
// and two wait in the queue, and afterwards every rotated file is a valid
// .gz with no plain file next to it.
TEST(compression, close_finishes_every_queued_compression) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 10,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);
  for (int i = 0; i < 3; i++) clog_info(lg, "generation %d", i);
  bool opened = _wait_for_gz_dest_opened();

  pthread_t t;
  bool closer_started =
      pthread_create(&t, NULL, _close_in_background, &lg) == 0;
  bool close_waited = false;
  for (long long deadline_ms = test_now_ms() + 30000;
       closer_started && !close_waited && test_now_ms() < deadline_ms;) {
    close_waited = clog__test_compressor_teardown_entered();
    if (!close_waited) usleep(1000);
  }
  clog__test_hold_compressions(false);
  if (closer_started)
    pthread_join(t, NULL);
  else
    clog_close(lg);

  int plain, gz, invalid_gz;
  count_rotated(dir, "app.log", &plain, &gz, &invalid_gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(closer_started);
  REQUIRE_TRUE(close_waited);
  REQUIRE_EQ(gz, 3);
  REQUIRE_EQ(plain, 0);
  REQUIRE_EQ(invalid_gz, 0);
}

static bool wait_child_bounded(pid_t pid, int bound_ms, int *status);

// A CLOG_FATAL record that causes a rotation waits for the compression it
// queued before it stops the process. The child holds each compression for
// 300 ms and registers no close at exit, so only the fatal path can wait.
TEST(compression, fatal_waits_for_queued_compressions_before_exit) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  char err_path[160];
  snprintf(err_path, sizeof(err_path), "%s.child_err", dir);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    int dn = open(err_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
    if (lg == CLOG_NULL) test_child_exit(0);
    clog__test_set_pending_compress_delay_us(300000);
    clog_fatal(lg, "fatal record that rotates");
    test_child_exit(0);
  }
  int status = 0;
  if (pid > 0) (void)wait_child_bounded(pid, 60000, &status);
  if (pid > 0 && !WIFEXITED(status)) {
    char err_text[8192] = {0};
    read_file(err_path, err_text, sizeof(err_text));
    fprintf(stderr, "child ended by signal %d; its output:\n%s\n",
            WIFSIGNALED(status) ? WTERMSIG(status) : -1, err_text);
  }
  unlink(err_path);

  int plain, gz, invalid_gz;
  count_rotated(dir, "app.log", &plain, &gz, &invalid_gz);
  char gz_path[512];
  char content[4096] = {0};
  if (find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)) == 0)
    gunzip_read(gz_path, content, sizeof(content));
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_NE(WEXITSTATUS(status), 0);
  REQUIRE_EQ(gz, 1);
  REQUIRE_EQ(plain, 0);
  REQUIRE_EQ(invalid_gz, 0);
  REQUIRE_NE(strstr(content, "fatal record that rotates"), NULL);
}

// Writes text to path and sets both of its timestamps to when, so a time
// rotation test can plant a file that looks old. Returns true on success.
static bool _write_file_with_mtime(const char *path, const char *text,
                                   time_t when) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  if (fd < 0) return false;
  size_t len = strlen(text);
  bool ok = write(fd, text, len) == (ssize_t)len;
  close(fd);
  struct timespec ts[2] = {{.tv_sec = when}, {.tv_sec = when}};
  return ok && utimensat(AT_FDCWD, path, ts, 0) == 0;
}

// glibc and musl declare statx in <sys/stat.h>, and musl then refuses the
// kernel header next to it, while an older C library needs the kernel header.
#if defined(__linux__) && !defined(STATX_BTIME)
#include <linux/stat.h>
#endif

// Returns path's birth time in seconds, or -1 when the kernel and filesystem
// report none, so the time rotation tests can expect what this filesystem
// does. It uses the statx system call on Linux (so no C library wrapper is
// needed) and struct stat on macOS and FreeBSD.
static time_t _file_birth_time(const char *path) {
#if defined(__linux__) && defined(STATX_BTIME) && defined(SYS_statx)
  struct statx stx;
  memset(&stx, 0, sizeof(stx));
  if (syscall(SYS_statx, AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 &&
      (stx.stx_mask & STATX_BTIME))
    return (time_t)stx.stx_btime.tv_sec;
  return -1;
#elif defined(__APPLE__) || defined(__FreeBSD__)
  struct stat st;
  if (stat(path, &st) != 0) return -1;
#if defined(__APPLE__)
  time_t b = st.st_birthtimespec.tv_sec;
#else
  time_t b = st.st_birthtim.tv_sec;
#endif
  return b > 0 ? b : -1;
#else
  (void)path;
  return -1;
#endif
}

// A file that holds records counts its time interval from the newest rotated
// generation's stamp, which is when the current file began, so a process that
// restarts more often than the interval still rotates.
TEST(rotation_time, interval_counts_from_the_newest_rotated_generation) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  time_t now = time(NULL);
  time_t then = now - 1000;
  struct tm tm;
  gmtime_r(&then, &tm);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
  char rotated_path[192];
  snprintf(rotated_path, sizeof(rotated_path), "%s.%s", path, stamp);
  bool prepared =
      _write_file_with_mtime(rotated_path, "older generation\n", then) &&
      _write_file_with_mtime(path, "current record\n", now);

  clog_rotation_cfg cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    clog_info(lg, "new record");
    clog_close(lg);
  }

  int rotated = count_files_with_prefix(dir, "app.log.");
  bool current_rotated = dir_files_contain(dir, "app.log.", "current record");
  static char live_text[4096];
  read_file(path, live_text, sizeof(live_text));
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(rotated, 2);
  REQUIRE_TRUE(current_rotated);
  REQUIRE_NE(strstr(live_text, "new record"), NULL);
  REQUIRE_EQ(strstr(live_text, "current record"), NULL);
}

// With no rotated generation the interval counts from the birth time where the
// filesystem reports one, else from the mtime. The file is new but carries an
// old mtime, so the two answers differ; FreeBSD moves the birth time back with
// the mtime, so the test expects what this filesystem reports.
TEST(rotation_time, interval_counts_from_birth_time_else_mtime) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  bool prepared =
      _write_file_with_mtime(path, "old record\n", time(NULL) - 1000);
  time_t btime = prepared ? _file_birth_time(path) : -1;
  bool recent_btime = btime >= 0 && btime > time(NULL) - 100;

  clog_rotation_cfg cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    clog_info(lg, "new record");
    clog_close(lg);
  }

  int rotated = count_files_with_prefix(dir, "app.log.");
  bool old_rotated = dir_files_contain(dir, "app.log.", "old record");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_NULL);
  if (recent_btime) {
    REQUIRE_EQ(rotated, 0);
  } else {
    REQUIRE_EQ(rotated, 1);
    REQUIRE_TRUE(old_rotated);
  }
}

// A file whose birth time is older than the interval rotates on the first
// write although it was just modified. Linux keeps the real, recent birth
// time, so the test expects what this filesystem reports.
TEST(rotation_time, old_birth_time_rotates_a_recently_modified_file) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  bool prepared =
      _write_file_with_mtime(path, "old record\n", time(NULL) - 1000);
  if (prepared) {
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC);
    prepared = fd >= 0;
    if (prepared) {
      prepared = write(fd, "newer record\n", 13) == 13;
      close(fd);
    }
  }
  time_t btime = prepared ? _file_birth_time(path) : -1;
  bool old_btime = btime >= 0 && btime <= time(NULL) - 100;

  clog_rotation_cfg cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    clog_info(lg, "new record");
    clog_close(lg);
  }

  int rotated = count_files_with_prefix(dir, "app.log.");
  bool old_rotated = dir_files_contain(dir, "app.log.", "old record");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_NULL);
  if (old_btime) {
    REQUIRE_EQ(rotated, 1);
    REQUIRE_TRUE(old_rotated);
  } else {
    REQUIRE_EQ(rotated, 0);
  }
}

// An empty file holds no record that could be older than the interval, so it
// starts the interval at the open whatever its mtime and the generations next
// to it.
TEST(rotation_time, empty_file_starts_the_interval_at_open) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  time_t then = time(NULL) - 1000;
  struct tm tm;
  gmtime_r(&then, &tm);
  char stamp[32];
  strftime(stamp, sizeof(stamp), "%Y%m%d%H%M%S", &tm);
  char rotated_path[192];
  snprintf(rotated_path, sizeof(rotated_path), "%s.%s", path, stamp);
  bool prepared =
      _write_file_with_mtime(rotated_path, "older generation\n", then) &&
      _write_file_with_mtime(path, "", then);

  clog_rotation_cfg cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    clog_info(lg, "first record");
    clog_close(lg);
  }
  int rotated = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(rotated, 1);
}

// A clock that steps back past the start of the interval restarts it at the
// new time, so the next rotation comes one interval after the correction. The
// second rotated name keeps the first one's stamp and takes the next number.
TEST(rotation_time, a_clock_that_steps_back_restarts_the_interval) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 10000000,
      .max_rotated_files = 10,
  };
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  int after_skew = -1, after_correction = -1, after_interval = -1;
  if (lg != CLOG_NULL) {
    clog__test_set_wall_clock_offset(3600);
    clog_info(lg, "clock an hour ahead");
    after_skew = count_files_with_prefix(dir, "app.log.");
    clog__test_set_wall_clock_offset(0);
    clog_info(lg, "clock corrected");
    after_correction = count_files_with_prefix(dir, "app.log.");
    clog__test_set_wall_clock_offset(11);
    clog_info(lg, "one interval after the correction");
    after_interval = count_files_with_prefix(dir, "app.log.");
    clog__test_set_wall_clock_offset(0);
    clog_close(lg);
  }
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(after_skew, 1);
  REQUIRE_EQ(after_correction, 1);
  REQUIRE_EQ(after_interval, 2);
}

// The backoff after a failed rotation stays at most one second when the clock
// steps back after the failure.
TEST(rotation_time,
     a_clock_that_steps_back_does_not_stretch_the_retry_backoff) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10,
      .max_rotated_files = 10,
  };
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  size_t attempts = 0;
  int rotated = -1;
  if (lg != CLOG_NULL) {
    clog__test_reset_rotate_attempt_count();
    clog__test_set_wall_clock_offset(3600);
    clog__test_force_rotate_failures(1);
    clog_info(lg, "the rotation after this record fails");
    clog__test_set_wall_clock_offset(0);
    clog_info(lg, "clock corrected");
    attempts = clog__test_get_rotate_attempt_count();
    rotated = count_files_with_prefix(dir, "app.log.");
    clog__test_force_rotate_failures(0);
    clog_close(lg);
  }
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(attempts, (size_t)2);
  REQUIRE_EQ(rotated, 1);
}

// Counts the distinct rotated generations of "app.log" in dir (a file and its
// .gz form count once), so a test can check retention while compressions
// run.
static int count_app_log_generations(const char *dir) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  char seen[64][40];
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log.", 8) != 0) continue;
    char key[40];
    snprintf(key, sizeof(key), "%s", e->d_name + 8);
    size_t klen = strlen(key);
    if (klen >= 3 && strcmp(key + klen - 3, ".gz") == 0) key[klen - 3] = '\0';
    bool dup = false;
    for (int i = 0; i < n && !dup; i++) dup = strcmp(seen[i], key) == 0;
    if (!dup && n < 64) snprintf(seen[n++], sizeof(seen[0]), "%s", key);
  }
  closedir(d);
  return n;
}

// A pass keeps an old generation while its compression runs, and once that
// compression ends the pass runs again and the old generation goes, so exactly
// the newest max_rotated_files stay with no older one left beside them.
TEST(compression, old_generation_kept_for_compression_goes_when_it_ends) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_hold_compressions(true);
  clog_info(lg, "generation one");
  bool opened = _wait_for_gz_dest_opened();
  clog_info(lg, "generation two");
  clog_info(lg, "generation three");
  clog__test_hold_compressions(false);
  clog_close(lg);

  int gens = count_app_log_generations(dir);
  bool has_one = false, has_three = false;
  char gz_path[512];
  char content[4096] = {0};
  if (find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)) == 0) {
    gunzip_read(gz_path, content, sizeof(content));
    has_one = strstr(content, "generation one") != NULL;
    has_three = strstr(content, "generation three") != NULL;
  }
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(opened);
  REQUIRE_EQ(gens, 1);
  REQUIRE_FALSE(has_one);
  REQUIRE_TRUE(has_three);
}

// Process exit with loggers still open

// Marks in seen (threads * per entries) every record "SEQ t=<thread>
// i=<index>" that dir holds, in the live file and in rotated files, plain or
// compressed, so an exit test can check that every record arrived once.
// Returns the number of records found, duplicates included, or -1 when a file
// can't be read.
static long collect_seq_records(const char *dir, int threads, int per,
                                unsigned char *seen) {
  memset(seen, 0, (size_t)threads * (size_t)per);
  DIR *d = opendir(dir);
  if (!d) return -1;
  long found = 0;
  size_t cap = 64u * 1024u * 1024u;
  char *buf = malloc(cap);
  if (!buf) {
    closedir(d);
    return -1;
  }
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[1024];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    size_t nlen = strlen(e->d_name);
    size_t got = 0;
    if (nlen >= 3 && strcmp(e->d_name + nlen - 3, ".gz") == 0) {
      gzFile g = gzopen(p, "rb");
      if (!g) {
        found = -1;
        break;
      }
      int r;
      while (got < cap - 1 &&
             (r = gzread(g, buf + got, (unsigned)(cap - 1 - got))) > 0)
        got += (size_t)r;
      gzclose(g);
    } else {
      int fd = open(p, O_RDONLY);
      if (fd < 0) {
        found = -1;
        break;
      }
      ssize_t r;
      while (got < cap - 1 && (r = read(fd, buf + got, cap - 1 - got)) > 0)
        got += (size_t)r;
      close(fd);
    }
    buf[got] = '\0';
    for (const char *q = strstr(buf, "SEQ t="); q;
         q = strstr(q + 1, "SEQ t=")) {
      int t = -1, i = -1;
      if (sscanf(q, "SEQ t=%d i=%d", &t, &i) == 2 && t >= 0 && t < threads &&
          i >= 0 && i < per) {
        seen[(size_t)t * (size_t)per + (size_t)i] = 1;
        found++;
      }
    }
  }
  free(buf);
  closedir(d);
  return found;
}

// Waits at most bound_ms for the child pid and stores its wait status. A child
// that doesn't finish gets its state and thread stacks dumped (so a CI log
// shows whether it was stuck or slow) and is killed, and the call returns
// false.
static bool wait_child_bounded(pid_t pid, int bound_ms, int *status) {
  for (long long deadline_ms = test_now_ms() + bound_ms;
       test_now_ms() < deadline_ms;) {
    if (waitpid(pid, status, WNOHANG) == pid) return true;
    usleep(10000);
  }
  _clog_test_dump_stuck_child_diagnostics(pid);
  kill(pid, SIGKILL);
  waitpid(pid, status, 0);
  return false;
}

// Forgets the loggers that the deferred-release fixture published in the
// parent. A child that ends with exit() calls it first, because this file's
// destructor would otherwise close those loggers in the child, racing a thread
// of its own, which is outside the supported use of fork().
static void forget_parent_fixtures_in_child(void) {
  g_deferred_release_a = CLOG_NULL;
  g_deferred_release_b = CLOG_NULL;
}

// Creates "<log_path>ready" when a child reaches its exit(). The parent reads
// that marker instead of the exit status, which valgrind or a sanitizer may
// replace.
static void mark_child_ready(const char *log_path) {
  char p[512];
  snprintf(p, sizeof(p), "%sready", log_path);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) close(fd);
}

// Tells whether the child created "<dir>/app.logready", that is, whether it
// reached its exit().
static bool child_was_ready(const char *dir) {
  char p[512];
  snprintf(p, sizeof(p), "%s/app.logready", dir);
  return access(p, F_OK) == 0;
}

// A flush interval that never fires within a test, so only the exit drain can
// deliver a record.
#define EXIT_TEST_NO_TIMER_US 3600000000ULL

// One writer of an exit test: Its logger, its number and its record count.
typedef struct {
  clog lg;
  int t;
  int per;
} exit_seq_arg_t;

// Logs a->per "SEQ" records through a->lg, a logger the child's main thread
// derived. Workers only log; every open, derive and close runs on the main
// thread, because ThreadSanitizer misreads the handle table's lock (made fresh
// in the child) when a second thread of the child takes it.
static void *exit_seq_writer(void *arg) {
  exit_seq_arg_t *a = (exit_seq_arg_t *)arg;
  for (int i = 0; i < a->per; i++) clog_info(a->lg, "SEQ t=%d i=%d", a->t, i);
  return NULL;
}

// Runs the child of the exit tests: An async logger in fmt with queue size
// qsize, threads writers of per records each, then exit(1) with the logger
// open. The drain gets 60 s instead of 5 s, because draining thousands of
// records takes seconds under valgrind and these tests are about what the
// drain delivers.
static void exit_drain_child(const char *path, clog_format fmt, size_t qsize,
                             int threads, int per) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(60000);
  clog_async_cfg acfg = {.queue_size = qsize,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_set_format(lg, fmt);
  pthread_t tids[8];
  exit_seq_arg_t args[8];
  int started = 0;
  for (int t = 0; t < threads && t < 8; t++) {
    args[t] = (exit_seq_arg_t){.lg = clog_derive(lg), .t = t, .per = per};
    if (args[t].lg == CLOG_NULL) break;
    if (pthread_create(&tids[t], NULL, exit_seq_writer, &args[t]) != 0) {
      clog_close(args[t].lg);
      break;
    }
    started++;
  }
  for (int t = 0; t < started; t++) pthread_join(tids[t], NULL);
  for (int t = 0; t < started; t++) clog_close(args[t].lg);
  if (started != threads) test_child_exit(4);
  exit(1);
}

// Runs exit_drain_child() in a child and returns how many of its records are
// in the files, or -1. It flushes stdio first, because the child's exit()
// would flush our buffers a second time.
static long run_exit_drain_child(const char *dir, clog_format fmt, size_t qsize,
                                 int threads, int per) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_drain_child(path, fmt, qsize, threads, per);
  if (pid < 0) return -1;
  int status = 0;
  if (!wait_child_bounded(pid, 120000, &status)) return -1;
  if (!WIFEXITED(status)) return -1;
  unsigned char *seen = malloc((size_t)threads * (size_t)per);
  if (!seen) return -1;
  long found = collect_seq_records(dir, threads, per, seen);
  long distinct = 0;
  for (size_t i = 0; i < (size_t)threads * (size_t)per; i++)
    distinct += seen[i];
  free(seen);
  if (found < 0) return -1;
  return found == distinct ? distinct : -1;
}

// Runs the child of the full-queue test: A queue of one record and a writer
// that spends 3 s per record, so record B fills the queue behind A. The drain
// then sends its flush request without a wait into the full queue. Writes '1'
// to report_fd when the drain left the target alone, '0' when it didn't and
// 'x' on a setup failure.
static void exit_drain_full_queue_child(const char *path, int report_fd) {
  forget_parent_fixtures_in_child();
  char verdict = 'x';
  clog_async_cfg acfg = {.queue_size = 1,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  if (lg != CLOG_NULL) {
    clog__test_set_writer_job_delay_us(3000000);
    clog_info(lg, "record A");
    clog_info(lg, "record B");
    clog__test_force_exit_drain_send_without_wait(true);
    clog__test_run_exit_drain();
    verdict = clog__test_exit_drain_left_target(lg) ? '1' : '0';
  }
  ssize_t w;
  do {
    w = write(report_fd, &verdict, 1);
  } while (w < 0 && errno == EINTR);
  test_child_exit(0);
}

// A drain whose budget is spent sends its flush request without a wait, and a
// full bounded queue answers ccol_container_full. The drain must leave that
// target alone instead of taking the queue for drained and turning sending
// off with records still in it.
TEST(exit_drain, a_full_queue_at_the_end_of_the_budget_is_left_alone) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int pfd[2];
  bool piped = pipe(pfd) == 0;
  fflush(NULL);
  pid_t pid = piped ? fork() : -1;
  if (pid == 0) {
    close(pfd[0]);
    exit_drain_full_queue_child(path, pfd[1]);
  }
  char verdict = 0;
  int status = 0;
  bool waited = false;
  if (piped) {
    close(pfd[1]);
    if (pid > 0) {
      ssize_t r;
      do {
        r = read(pfd[0], &verdict, 1);
      } while (r < 0 && errno == EINTR);
      waited = wait_child_bounded(pid, 60000, &status);
    }
    close(pfd[0]);
  }
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(piped);
  REQUIRE_GT(pid, 0);
  REQUIRE_TRUE(waited);
  REQUIRE_EQ(verdict, '1');
}

// exit() with an async logger open keeps every record it logged, for every
// format and for both queue kinds, because the exit drain delivers the queue
// and the batch.
TEST(exit_drain, exit_keeps_every_record_of_an_async_logger) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  const clog_format fmts[3] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON, CLOG_FMT_SYSLOG};
  const size_t qsizes[2] = {0, 16};
  long got[3][2];
  for (int f = 0; f < 3; f++) {
    for (int q = 0; q < 2; q++) {
      char dir[64];
      got[f][q] = -2;
      if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) continue;
      got[f][q] = run_exit_drain_child(dir, fmts[f], qsizes[q], 1, 300);
      cleanup_dir(dir, "app.log");
    }
  }
  for (int f = 0; f < 3; f++)
    for (int q = 0; q < 2; q++) REQUIRE_EQ(got[f][q], 300L);
}

// Threads log through derived loggers and finish, then the process exits (a
// return from main() with the root logger open), and every record is on disk
// once.
TEST(exit_drain, exit_after_threads_finish_keeps_every_record) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long unbounded = run_exit_drain_child(dir, CLOG_FMT_LOGFMT, 0, 4, 1000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long bounded = run_exit_drain_child(dir, CLOG_FMT_JSON, 64, 4, 1000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(unbounded, 4000L);
  REQUIRE_EQ(bounded, 4000L);
}

// Runs the child of the compression exit tests: A rotating, compressing logger
// that rotates once and calls exit(0) while that compression runs, or while it
// is held when hold is true.
static void exit_compress_child(const char *path, bool hold) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  if (hold) {
    clog__test_set_exit_drain_budget_ms(200);
    clog__test_hold_compressions(true);
  }
  clog_info(lg, "COMPRESSEDRECORDBODY");
  if (hold) {
    bool opened = false;
    for (long long deadline_ms = test_now_ms() + 20000;
         !opened && test_now_ms() < deadline_ms;) {
      opened = clog__test_gz_dest_opened();
      if (!opened) usleep(1000);
    }
    if (!opened) test_child_exit(4);
  }
  mark_child_ready(path);
  exit(0);
}

// What a directory holds after exit_compress_child(): Plain rotated files, .gz
// files, .gz files that gunzip -t refuses, unfinished outputs, and where the
// record ended up.
typedef struct {
  int plain;
  int gz;
  int invalid_gz;
  int tmp;
  bool body_in_plain;
  bool body_in_gz;
} exit_compress_state_t;

// Fills st with what dir holds after exit_compress_child(), which is what the
// compression exit tests check.
static void inspect_exit_compress_dir(const char *dir,
                                      exit_compress_state_t *st) {
  memset(st, 0, sizeof(*st));
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log.", 8) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    size_t nlen = strlen(e->d_name);
    char content[4096];
    if (nlen > 7 && strcmp(e->d_name + nlen - 7, ".gz.tmp") == 0) {
      st->tmp++;
    } else if (nlen > 3 && strcmp(e->d_name + nlen - 3, ".gz") == 0) {
      st->gz++;
      if (gunzip_test(p) != 0) st->invalid_gz++;
      gunzip_read(p, content, sizeof(content));
      if (strstr(content, "COMPRESSEDRECORDBODY")) st->body_in_gz = true;
    } else {
      st->plain++;
      read_file(p, content, sizeof(content));
      if (strstr(content, "COMPRESSEDRECORDBODY")) st->body_in_plain = true;
    }
  }
  closedir(d);
}

// Runs exit_compress_child() in a child and returns true when it reached its
// exit() within 60 s.
static bool run_exit_compress_child(const char *dir, bool hold) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_compress_child(path, hold);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, 60000, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

// exit() while a compression runs finishes it within the drain's budget, so
// the record is in one valid .gz and the source is gone.
TEST(exit_drain, exit_finishes_a_running_compression) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool ran = run_exit_compress_child(dir, false);
  exit_compress_state_t st;
  inspect_exit_compress_dir(dir, &st);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ran);
  REQUIRE_EQ(st.gz, 1);
  REQUIRE_EQ(st.invalid_gz, 0);
  REQUIRE_EQ(st.tmp, 0);
  REQUIRE_EQ(st.plain, 0);
  REQUIRE_TRUE(st.body_in_gz);
}

// A compression that can't finish within the budget is abandoned cleanly: No
// .gz and no unfinished output, and the plain source keeps the record.
TEST(exit_drain, exit_abandons_a_compression_that_cannot_finish) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool ran = run_exit_compress_child(dir, true);
  exit_compress_state_t st;
  inspect_exit_compress_dir(dir, &st);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ran);
  REQUIRE_EQ(st.gz, 0);
  REQUIRE_EQ(st.tmp, 0);
  REQUIRE_EQ(st.plain, 1);
  REQUIRE_TRUE(st.body_in_plain);
}

// While g_exit_on_foreign_free is set, exit_on_foreign_free() calls exit() once
// on any thread but g_exit_free_owner. That puts exit() on a writer or
// compressor thread, the only other threads that call a logger's allocator.
static _Atomic bool g_exit_on_foreign_free;
static pthread_t g_exit_free_owner;

// Frees p, or ends the process with exit(0) when g_exit_on_foreign_free is set
// and the caller isn't g_exit_free_owner (clearing the flag first), to put
// exit() on a logger's own thread.
static void exit_on_foreign_free(void *p) {
  if (atomic_load(&g_exit_on_foreign_free) &&
      !pthread_equal(pthread_self(), g_exit_free_owner)) {
    atomic_store(&g_exit_on_foreign_free, false);
    exit(0);
  }
  free(p);
}

// Runs a child whose async or compressing logger uses exit_on_foreign_free(),
// so exit() runs on its writer or compressor thread. The budget is far longer
// than the parent's bound, so a drain that waits for itself fails the test.
static void exit_from_worker_child(const char *path, bool compress) {
  forget_parent_fixtures_in_child();
  ccol_memmgmt_procs mp = {.malloc = malloc,
                           .free = exit_on_foreign_free,
                           .calloc = calloc,
                           .realloc = realloc};
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, compress ? &cfg : NULL,
                                     compress ? NULL : &acfg, &mp);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog__test_set_exit_drain_budget_ms(600000);
  mark_child_ready(path);
  g_exit_free_owner = pthread_self();
  atomic_store(&g_exit_on_foreign_free, true);
  clog_info(lg, "WORKEREXITBODY");
  for (;;) pause();
}

// Runs exit_from_worker_child() in a child and returns true when it reached
// its exit() within 60 s.
static bool run_exit_from_worker_child(const char *dir, bool compress) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_from_worker_child(path, compress);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, 60000, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

// The exit drain skips the target the calling thread serves; otherwise it
// waits for its own flush request on the writer thread, or locks a mutex the
// compressor thread already holds.
TEST(exit_drain, exit_from_the_writer_or_compressor_thread_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool writer_ok = run_exit_from_worker_child(dir, false);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool compressor_ok = run_exit_from_worker_child(dir, true);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(writer_ok);
  REQUIRE_TRUE(compressor_ok);
}

// The lock that exit_lock_holder() takes: lg's mutex, or the table lock when
// lg is CLOG_NULL. locked is set once it holds it.
typedef struct {
  clog lg;
  _Atomic bool locked;
} exit_lock_holder_t;

// Takes the lock h names, sets h->locked and never lets it go, so the exit
// drain meets a lock that is held for ever.
static void *exit_lock_holder(void *arg) {
  exit_lock_holder_t *h = (exit_lock_holder_t *)arg;
  if (h->lg == CLOG_NULL)
    clog__test_lock_table_forever();
  else
    clog__test_lock_target_forever(h->lg);
  atomic_store(&h->locked, true);
  for (;;) pause();
  return NULL;
}

// Runs a child with a logger that arms the exit drain (compressing and
// rotating, or async), a thread that holds that logger's mutex or the table
// lock for ever, and exit(). The drain's budget is short and the parent's
// bound far above it.
static void exit_with_held_lock_child(const char *path, bool table_lock) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, table_lock ? NULL : &cfg,
                                     table_lock ? &acfg : NULL, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "HELDLOCKBODY");
  clog__test_set_exit_drain_budget_ms(300);
  static exit_lock_holder_t holder;
  holder.lg = table_lock ? CLOG_NULL : lg;
  atomic_store(&holder.locked, false);
  pthread_t tid;
  if (pthread_create(&tid, NULL, exit_lock_holder, &holder) != 0)
    test_child_exit(4);
  for (long long deadline_ms = test_now_ms() + 20000;
       !atomic_load(&holder.locked) && test_now_ms() < deadline_ms;)
    usleep(1000);
  if (!atomic_load(&holder.locked)) test_child_exit(5);
  mark_child_ready(path);
  exit(0);
}

// Runs exit_with_held_lock_child() in a child and returns true when it ended
// with exit() within bound_ms.
static bool run_exit_with_held_lock_child(const char *dir, bool table_lock,
                                          int bound_ms) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_with_held_lock_child(path, table_lock);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, bound_ms, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

// The exit drain takes every lock with a bound, so a lock held for ever by
// another thread makes it give up within its budget and the process ends.
TEST(exit_drain, exit_with_a_lock_held_for_ever_ends_within_the_budget) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool target_ok = run_exit_with_held_lock_child(dir, false, 30000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool table_ok = run_exit_with_held_lock_child(dir, true, 30000);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(target_ok);
  REQUIRE_TRUE(table_ok);
}

// Unfinished compressions of an earlier process

// Runs a child whose compression is held once it has created its output, and
// then dies without any exit handler, as a crash or a kill ends a process.
static void die_during_compression_child(const char *path) {
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog__test_hold_compressions(true);
  clog_info(lg, "COMPRESSEDRECORDBODY");
  for (long long deadline_ms = test_now_ms() + 20000;
       test_now_ms() < deadline_ms;) {
    if (clog__test_gz_dest_opened()) {
      mark_child_ready(path);
      test_child_exit(0);
    }
    usleep(1000);
  }
  test_child_exit(4);
}

// The output takes the .gz name only once complete, so a death in the middle
// leaves no .gz. The next open removes the unfinished output and compresses
// the source again.
TEST(compression, a_death_during_compression_leaves_no_gz_and_open_redoes_it) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) die_during_compression_child(path);
  int status = 0;
  bool ran = pid > 0 && wait_child_bounded(pid, 60000, &status) &&
             WIFEXITED(status) && child_was_ready(dir);
  exit_compress_state_t died;
  inspect_exit_compress_dir(dir, &died);

  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg != CLOG_NULL) clog_close(lg);
  exit_compress_state_t reopened;
  inspect_exit_compress_dir(dir, &reopened);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(ran);
  REQUIRE_EQ(died.gz, 0);
  REQUIRE_EQ(died.tmp, 1);
  REQUIRE_EQ(died.plain, 1);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(reopened.gz, 1);
  REQUIRE_EQ(reopened.invalid_gz, 0);
  REQUIRE_EQ(reopened.tmp, 0);
  REQUIRE_EQ(reopened.plain, 0);
  REQUIRE_TRUE(reopened.body_in_gz);
}

// Writes len bytes of data to path, so a test can plant what an earlier
// process left. Returns true on success.
static bool write_whole_file(const char *path, const char *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  bool ok = write(fd, data, len) == (ssize_t)len;
  close(fd);
  return ok;
}

// An open removes a truncated .gz and an unfinished output next to their
// sources and compresses each source again, so every rotated file ends up as
// one valid .gz.
TEST(compression, open_redoes_compressions_that_an_earlier_process_left) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char p[256];
  bool setup = true;
  snprintf(p, sizeof(p), "%s/app.log.20200101000000", dir);
  setup &= write_whole_file(p, "OLDGENONE\n", 10);
  snprintf(p, sizeof(p), "%s/app.log.20200101000000.gz", dir);
  setup &= write_whole_file(p, "\x1f\x8b\x08\x00", 4);
  snprintf(p, sizeof(p), "%s/app.log.20200101000001", dir);
  setup &= write_whole_file(p, "OLDGENTWO\n", 10);
  snprintf(p, sizeof(p), "%s/app.log.20200101000001.gz.tmp", dir);
  setup &= write_whole_file(p, "\x1f\x8b", 2);

  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) clog_close(lg);

  exit_compress_state_t st;
  inspect_exit_compress_dir(dir, &st);
  char one[256] = {0}, two[256] = {0};
  snprintf(p, sizeof(p), "%s/app.log.20200101000000.gz", dir);
  gunzip_read(p, one, sizeof(one));
  snprintf(p, sizeof(p), "%s/app.log.20200101000001.gz", dir);
  gunzip_read(p, two, sizeof(two));
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(st.tmp, 0);
  REQUIRE_EQ(st.plain, 0);
  REQUIRE_EQ(st.gz, 2);
  REQUIRE_EQ(st.invalid_gz, 0);
  REQUIRE_STREQ(one, "OLDGENONE\n");
  REQUIRE_STREQ(two, "OLDGENTWO\n");
}

TEST(compression, compressed_file_keeps_the_mode_of_its_source) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  mode_t old_mask = umask(022);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  bool chmod_ok = lg != CLOG_NULL && chmod(path, 0640) == 0;
  if (lg != CLOG_NULL) {
    clog_info(lg, "MODERECORD");
    clog_close(lg);
  }
  umask(old_mask);
  char gz[512];
  struct stat st;
  bool have_gz = find_file_with_suffix(dir, ".gz", gz, sizeof(gz)) == 0 &&
                 stat(gz, &st) == 0;
  mode_t mode = have_gz ? (st.st_mode & 0777) : 0;
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(chmod_ok);
  REQUIRE_TRUE(have_gz);
  REQUIRE_EQ((unsigned)mode, 0640u);
}

// One file opened twice in one process

// One writer of the same-file tests: Its handle, its number and its record
// count.
typedef struct {
  clog lg;
  int t;
  int per;
} same_file_arg_t;

// Logs a->per "SEQ" records through a->lg; thread 1 pauses after each one, so
// if the two handles had two targets its records would keep landing in a file
// the other one rotated away.
static void *same_file_writer(void *arg) {
  same_file_arg_t *a = (same_file_arg_t *)arg;
  for (int i = 0; i < a->per; i++) {
    clog_info(a->lg, "SEQ t=%d i=%d", a->t, i);
    if (a->t == 1) usleep(200);
  }
  return NULL;
}

// Two opens of one rotating, compressing file through two spellings of the
// path share one target, so records of both handles all end up on disk once
// each.
TEST(same_file, two_rotating_opens_share_one_target_and_lose_nothing) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], path2[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(path2, sizeof(path2), "./%s/../%s/app.log", dir, dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 4096,
                           .max_rotated_files = 1000,
                           .compress_rotated = true};
  clog a = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  clog b = clog_create_from_file(path2, CLOG_INFO, &cfg, NULL);
  enum { PER = 2000 };
  bool started[2] = {false, false};
  pthread_t tids[2];
  same_file_arg_t args[2] = {{.lg = a, .t = 0, .per = PER},
                             {.lg = b, .t = 1, .per = PER}};
  if (a != CLOG_NULL && b != CLOG_NULL)
    for (int t = 0; t < 2; t++)
      started[t] =
          pthread_create(&tids[t], NULL, same_file_writer, &args[t]) == 0;
  for (int t = 0; t < 2; t++)
    if (started[t]) pthread_join(tids[t], NULL);
  if (a != CLOG_NULL) clog_close(a);
  if (b != CLOG_NULL) clog_close(b);

  unsigned char *seen = malloc(2 * PER);
  long found = seen ? collect_seq_records(dir, 2, PER, seen) : -1;
  long distinct = 0;
  for (int i = 0; seen && i < 2 * PER; i++) distinct += seen[i];
  free(seen);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_NULL);
  REQUIRE_NE(b, CLOG_NULL);
  REQUIRE_TRUE(started[0] && started[1]);
  REQUIRE_EQ(distinct, (long)(2 * PER));
  REQUIRE_EQ(found, (long)(2 * PER));
}

// A second open joins the first only when the configurations match after the
// defaults are applied, so an open that spells out the default rotation or
// flush interval joins one that gave 0, and another interval doesn't.
TEST(same_file, a_zero_interval_is_the_default_interval) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg zero = {.time_rotation_enabled = true,
                            .rotation_interval_us = 0,
                            .max_rotated_files = 3};
  clog_rotation_cfg spelled = zero;
  spelled.rotation_interval_us = CLOG_DEFAULT_ROTATION_INTERVAL_US;
  clog_rotation_cfg other = zero;
  other.rotation_interval_us = CLOG_DEFAULT_ROTATION_INTERVAL_US + 1000000u;
  clog_async_cfg azero = {.flush_interval_us = 0};
  clog_async_cfg aspelled = {.flush_interval_us =
                                 CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US};
  clog_async_cfg aother = {.flush_interval_us =
                               CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US + 1u};

  clog a = clog_create_from_file(path, CLOG_INFO, &zero, &azero);
  clog b = clog_create_from_file(path, CLOG_INFO, &spelled, &aspelled);
  clog diff_rot = clog_create_from_file(path, CLOG_INFO, &other, &azero);
  clog diff_flush = clog_create_from_file(path, CLOG_INFO, &zero, &aother);
  if (diff_flush != CLOG_NULL) clog_close(diff_flush);
  if (diff_rot != CLOG_NULL) clog_close(diff_rot);
  if (b != CLOG_NULL) clog_close(b);
  if (a != CLOG_NULL) clog_close(a);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_NULL);
  REQUIRE_NE(b, CLOG_NULL);
  REQUIRE_EQ(diff_rot, CLOG_NULL);
  REQUIRE_EQ(diff_flush, CLOG_NULL);
}

// A second open of a file a rotating logger writes fails when the
// configuration differs. Two opens that don't rotate stay independent, and an
// open after the last close works again.
TEST(same_file, a_second_open_with_another_configuration_fails) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 4096,
                           .max_rotated_files = 3,
                           .compress_rotated = true};
  clog_rotation_cfg other = cfg;
  other.max_rotated_files = 4;
  clog_async_cfg acfg = {0};

  clog a = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  clog diff_rot = clog_create_from_file(path, CLOG_INFO, &other, NULL);
  clog no_rot = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  clog async = clog_create_from_file(path, CLOG_INFO, &cfg, &acfg);
  clog same = clog_create_from_file(path, CLOG_DEBUG, &cfg, NULL);
  clog_level same_level = same != CLOG_NULL ? clog_get_level(same) : 0;
  clog_level a_level = a != CLOG_NULL ? clog_get_level(a) : 0;
  if (same != CLOG_NULL) clog_close(same);
  if (a != CLOG_NULL) clog_close(a);
  clog again = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  clog again2 = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  if (again != CLOG_NULL) clog_close(again);
  if (again2 != CLOG_NULL) clog_close(again2);
  if (diff_rot != CLOG_NULL) clog_close(diff_rot);
  if (no_rot != CLOG_NULL) clog_close(no_rot);
  if (async != CLOG_NULL) clog_close(async);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_NULL);
  REQUIRE_EQ(diff_rot, CLOG_NULL);
  REQUIRE_EQ(no_rot, CLOG_NULL);
  REQUIRE_EQ(async, CLOG_NULL);
  REQUIRE_NE(same, CLOG_NULL);
  REQUIRE_EQ((int)same_level, (int)CLOG_DEBUG);
  REQUIRE_EQ((int)a_level, (int)CLOG_INFO);
  REQUIRE_NE(again, CLOG_NULL);
  REQUIRE_NE(again2, CLOG_NULL);
}

// An allocator for the join tests that counts live blocks in g_join_live and
// trips at call g_join_trip of its malloc, calloc and realloc: That call fails,
// or, with g_join_trip_arms_slot set, succeeds and arms the hook that fails the
// next handle acquisition. While g_join_other_threads_fail is set, every
// allocation fails on every thread but g_join_main_thread.
static _Atomic long g_join_calls = 0;
static _Atomic long g_join_live = 0;
static _Atomic long g_join_trip = -1;
static _Atomic bool g_join_trip_arms_slot = false;
static _Atomic bool g_join_other_threads_fail = false;
static pthread_t g_join_main_thread;

// Counts one call of the join allocator and tells whether it must fail (or,
// with g_join_trip_arms_slot, arms the slot failure and succeeds); the procs
// below use it and keep g_join_live up to date.
static bool _join_trips(void) {
  if (atomic_load(&g_join_other_threads_fail) &&
      !pthread_equal(pthread_self(), g_join_main_thread))
    return true;
  long c = atomic_fetch_add(&g_join_calls, 1) + 1;
  if (c != atomic_load(&g_join_trip)) return false;
  if (!atomic_load(&g_join_trip_arms_slot)) return true;
  clog__test_force_next_fresh_slot_registration_failure(true);
  return false;
}
static void *_join_malloc(size_t n) {
  void *p = _join_trips() ? NULL : malloc(n);
  if (p) atomic_fetch_add(&g_join_live, 1);
  return p;
}
static void *_join_calloc(size_t n, size_t sz) {
  void *p = _join_trips() ? NULL : calloc(n, sz);
  if (p) atomic_fetch_add(&g_join_live, 1);
  return p;
}
static void *_join_realloc(void *old, size_t n) {
  void *p = _join_trips() ? NULL : realloc(old, n);
  if (p && !old) atomic_fetch_add(&g_join_live, 1);
  return p;
}
static void _join_free(void *p) {
  if (p) atomic_fetch_sub(&g_join_live, 1);
  free(p);
}

// Opens path a second time with g_join_trip at every call number in turn,
// once with failing allocations and once with a failing handle acquisition,
// while another logger already writes the file. Every open that works logs a
// record and closes. Returns how many opens failed.
static int join_sweep(const char *path, const clog_rotation_cfg *cfg,
                      ccol_memmgmt_procs *procs) {
  int refused = 0;
  for (int arms_slot = 0; arms_slot < 2; arms_slot++) {
    atomic_store(&g_join_trip_arms_slot, arms_slot != 0);
    for (long trip = 1; trip <= 40; trip++) {
      atomic_store(&g_join_calls, 0);
      atomic_store(&g_join_trip, trip);
      clog b = clog_create_from_file_mp(path, CLOG_INFO, cfg, NULL, procs);
      atomic_store(&g_join_trip, -1);
      clog__test_force_next_fresh_slot_registration_failure(false);
      if (b == CLOG_NULL) {
        refused++;
        continue;
      }
      clog_info(b, "JOINED %d %ld", arms_slot, trip);
      clog_close(b);
    }
  }
  return refused;
}

// A join that fails for lack of memory or of a handle slot returns CLOG_NULL
// and gives back its reference to the target: The first logger keeps writing,
// every block comes back, and after the first logger closes an open with
// another configuration works.
TEST(same_file, a_join_that_fails_gives_back_its_reference) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 3};
  atomic_store(&g_join_live, 0);
  clog a = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &procs);
  int refused = a != CLOG_NULL ? join_sweep(path, &cfg, &procs) : -1;
  if (a != CLOG_NULL) {
    clog_info(a, "FIRST STILL WRITES");
    clog_close(a);
  }
  long live = atomic_load(&g_join_live);
  clog other = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  if (other != CLOG_NULL) clog_close(other);
  static char buf[65536];
  read_file(path, buf, sizeof(buf));
  bool first_wrote = strstr(buf, "FIRST STILL WRITES") != NULL;
  bool joins_wrote =
      strstr(buf, "JOINED 0 40") != NULL && strstr(buf, "JOINED 1 40") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_NULL);
  REQUIRE_GT(refused, 0);
  REQUIRE_LT(refused, 80);
  REQUIRE_EQ(live, 0L);
  REQUIRE_NE(other, CLOG_NULL);
  REQUIRE_TRUE(first_wrote);
  REQUIRE_TRUE(joins_wrote);
}

// The joined handle allocates through the target, so a join needs the same
// allocator functions (another struct with the same functions is fine).
TEST(same_file, a_second_open_with_another_allocator_fails) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  ccol_memmgmt_procs same = procs;
  ccol_memmgmt_procs other = procs;
  other.free = _custom_free;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 3};
  clog a = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &procs);
  clog plain = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  clog diff = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &other);
  clog joined = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &same);
  if (joined != CLOG_NULL) clog_close(joined);
  if (diff != CLOG_NULL) clog_close(diff);
  if (plain != CLOG_NULL) clog_close(plain);
  if (a != CLOG_NULL) clog_close(a);
  clog plain_first =
      clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  clog procs_second =
      clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &procs);
  if (procs_second != CLOG_NULL) clog_close(procs_second);
  if (plain_first != CLOG_NULL) clog_close(plain_first);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_NULL);
  REQUIRE_EQ(plain, CLOG_NULL);
  REQUIRE_EQ(diff, CLOG_NULL);
  REQUIRE_NE(joined, CLOG_NULL);
  REQUIRE_NE(plain_first, CLOG_NULL);
  REQUIRE_EQ(procs_second, CLOG_NULL);
}

// Each failing step of an async clog_create_from_fd_mp() (logger, queue,
// batch buffer, writer thread, handle slot) returns CLOG_NULL with every block
// back and the caller's fd open, and a later logger on that fd works.
TEST(custom_alloc, an_fd_logger_that_cannot_start_gives_everything_back) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  int refused = 0, worked = 0, leaked = 0, fd_lost = 0;
  for (int arms_slot = 0; arms_slot < 2; arms_slot++) {
    atomic_store(&g_join_trip_arms_slot, arms_slot != 0);
    for (size_t queue_size = 0; queue_size <= 8; queue_size += 8) {
      clog_async_cfg acfg = {.queue_size = queue_size};
      for (long trip = 1; trip <= 40; trip++) {
        atomic_store(&g_join_live, 0);
        atomic_store(&g_join_calls, 0);
        atomic_store(&g_join_trip, trip);
        clog lg = clog_create_from_fd_mp(p[1], CLOG_INFO, &acfg, &procs);
        atomic_store(&g_join_trip, -1);
        clog__test_force_next_fresh_slot_registration_failure(false);
        if (lg == CLOG_NULL) {
          refused++;
        } else {
          worked++;
          clog_close(lg);
        }
        if (atomic_load(&g_join_live) != 0) leaked++;
        if (fcntl(p[1], F_GETFD) == -1) fd_lost++;
      }
    }
  }
  clog after = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  if (after != CLOG_NULL) {
    clog_info(after, "AFTER THE SWEEP");
    clog_close(after);
  }
  close(p[1]);
  static char buf[16384];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof(buf) - 1 &&
         (r = read(p[0], buf + n, sizeof(buf) - 1 - n)) > 0)
    n += (size_t)r;
  buf[n] = '\0';
  close(p[0]);

  REQUIRE_GT(refused, 0);
  REQUIRE_GT(worked, 0);
  REQUIRE_EQ(leaked, 0);
  REQUIRE_EQ(fd_lost, 0);
  REQUIRE_NE(after, CLOG_NULL);
  REQUIRE_TRUE(strstr(buf, "AFTER THE SWEEP") != NULL);
}

// While g_join_growth_fails is set, _grow_realloc() refuses to grow a block
// it already handed out, so a record buffer can't grow. With
// g_join_growth_spares_main set it still grows blocks for g_join_main_thread,
// so only the async writer thread finds no memory.
static _Atomic bool g_join_growth_fails = false;
static _Atomic bool g_join_growth_spares_main = false;

// Grows a block through _join_realloc(), unless g_join_growth_fails refuses
// growth for this thread; it's the realloc of the tests whose record buffer
// must not grow.
static void *_grow_realloc(void *old, size_t n) {
  if (old && atomic_load(&g_join_growth_fails) &&
      !(atomic_load(&g_join_growth_spares_main) &&
        pthread_equal(pthread_self(), g_join_main_thread)))
    return NULL;
  return _join_realloc(old, n);
}

// Logs a record with a 9000-byte field while the record buffer can't grow,
// then one with memory to spare, through a sync logger (acfg NULL) or an
// async one, and reads the output into buf. Returns false when the setup
// failed. The first record must become the "transient allocation failure"
// record, never one that silently misses the field.
static bool run_field_without_room(const clog_async_cfg *acfg, char *buf,
                                   size_t cap) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return false;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _grow_realloc};
  static char big[9001];
  memset(big, 'v', sizeof(big) - 1);
  big[sizeof(big) - 1] = '\0';
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, acfg, &procs);
  bool ok = lg != CLOG_NULL;
  if (ok) {
    ok = clog_set_field(lg, "big", big) == ccol_success;
    g_join_main_thread = pthread_self();
    atomic_store(&g_join_growth_spares_main, acfg != NULL);
    atomic_store(&g_join_growth_fails, true);
    clog_info(lg, "FIRSTRECORD");
    clog_flush(lg);
    atomic_store(&g_join_growth_fails, false);
    atomic_store(&g_join_growth_spares_main, false);
    clog_info(lg, "SECONDRECORD");
    clog_close(lg);
  }
  read_file(path, buf, cap);
  cleanup_dir(dir, "app.log");
  return ok;
}

TEST(robustness, a_field_that_cannot_grow_the_buffer_drops_the_record) {
  static char sync_out[65536];
  static char async_out[65536];
  clog_async_cfg acfg = {0};
  bool ok_sync = run_field_without_room(NULL, sync_out, sizeof(sync_out));
  bool ok_async = run_field_without_room(&acfg, async_out, sizeof(async_out));
  REQUIRE_TRUE(ok_sync);
  REQUIRE_TRUE(ok_async);
  const char *outs[] = {sync_out, async_out};
  for (int i = 0; i < 2; i++) {
    const char *o = outs[i];
    const char *dropped = strstr(o, "transient allocation failure");
    const char *second = strstr(o, "SECONDRECORD");
    REQUIRE_TRUE(dropped != NULL);
    REQUIRE_TRUE(strstr(o, "FIRSTRECORD") == NULL);
    REQUIRE_TRUE(second != NULL);
    REQUIRE_TRUE(dropped < second);
    const char *val = strstr(o, "big=");
    REQUIRE_TRUE(val != NULL);
    REQUIRE_TRUE(val > dropped);
    REQUIRE_EQ(strspn(val + 4, "v"), (size_t)9000);
  }
}

// Logs msg at ERROR from one call site. Every format's timestamp has a fixed
// width, so only the message changes the length of these records.
static void log_error_from_one_site(clog lg, const char *msg) {
  clog_error(lg, "%s", msg);
}

// Logs, through a logger in fmt whose record buffer can't grow, an ERROR
// record sized so that its backtrace finds room free bytes in the 4096-byte
// buffer, and leaves the output after the measuring records in buf. When
// shortest_frame isn't NULL, an ordinary ERROR record from the same call site
// goes first and *shortest_frame gets the length of its shortest logfmt frame
// line (SIZE_MAX when it has none).
static bool run_backtrace_without_room(clog_format fmt, size_t room, char *buf,
                                       size_t cap, size_t *shortest_frame) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return false;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _grow_realloc};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  if (lg == CLOG_NULL) {
    cleanup_dir(dir, "app.log");
    return false;
  }
  bool ok = clog_set_format(lg, fmt) == ccol_success;
  clog__test_force_backtrace_capture_failure(true);
  log_error_from_one_site(lg, "aaaaaaaaaa");
  clog__test_force_backtrace_capture_failure(false);
  size_t skip = read_file(path, buf, cap);
  size_t first = strcspn(buf, "\n") + 1;
  size_t before = fmt == CLOG_FMT_JSON
                      ? first - strlen(",\"bt_error\":\"unavailable\"}\n")
                      : first;
  if (shortest_frame) {
    *shortest_frame = SIZE_MAX;
    log_error_from_one_site(lg, "bbbbbbbbbb");
    size_t n = read_file(path, buf, cap);
    for (size_t at = skip; at < n;) {
      size_t len = strcspn(buf + at, "\n") + 1;
      if (buf[at] == '\t' && buf[at + 1] == '#' &&
          strncmp(buf + at, "\t#error ", 8) != 0 && len < *shortest_frame)
        *shortest_frame = len;
      at += len;
    }
    skip = n;
  }
  size_t want = 4096 - room;
  static char msg[4096];
  size_t mlen = 10 + want - before;
  ok = ok && want > before && mlen < sizeof(msg);
  if (ok) {
    memset(msg, 'a', mlen);
    msg[mlen] = '\0';
    atomic_store(&g_join_growth_fails, true);
    log_error_from_one_site(lg, msg);
    atomic_store(&g_join_growth_fails, false);
    clog_info(lg, "AFTERRECORD");
  }
  clog_close(lg);
  static char all[65536];
  size_t n = read_file(path, all, sizeof(all));
  size_t keep = n > skip ? n - skip : 0;
  if (keep >= cap) keep = cap - 1;
  memcpy(buf, all + skip, keep);
  buf[keep] = '\0';
  cleanup_dir(dir, "app.log");
  return ok;
}

// Counts the lines of text that start with a tab and '#', the backtrace frame
// lines of logfmt, so a test can check that no frame was written.
static int count_frame_lines(const char *text) {
  int n = text[0] == '\t' && text[1] == '#';
  for (const char *p = strchr(text, '\n'); p; p = strchr(p + 1, '\n'))
    if (p[1] == '\t' && p[2] == '#') n++;
  return n;
}

// When no frame fits and the buffer can't grow, the record keeps its message
// and shows the "unavailable" marker (bt_error in JSON), with no frame written
// in part. A frame line's length depends on the build, so the room comes from
// the shortest frame line of this call site.
TEST(robustness, a_backtrace_without_room_shows_the_marker_and_no_frame) {
  if (clog__test_backtrace_supported() &&
      !backtrace_capture_genuinely_available()) {
    fprintf(stderr, "SKIP: backtrace() captures no frames here\n");
    return;
  }
  static char logfmt[16384];
  static char json[16384];
  const size_t marker_len = strlen("\t#error backtrace unavailable\n");
  size_t shortest = 0;
  bool ok_probe = run_backtrace_without_room(CLOG_FMT_LOGFMT, 32, logfmt,
                                             sizeof(logfmt), &shortest);
  REQUIRE_TRUE(ok_probe);
  size_t room = shortest < 32 ? shortest : 32;
  if (room > marker_len) {
    bool ok_logfmt = run_backtrace_without_room(CLOG_FMT_LOGFMT, room, logfmt,
                                                sizeof(logfmt), NULL);
    REQUIRE_TRUE(ok_logfmt);
    size_t line = strcspn(logfmt, "\n");
    REQUIRE_EQ(line + 1, 4096 - room);
    REQUIRE_EQ(strncmp(logfmt + line + 1, "\t#error backtrace unavailable\n",
                       marker_len),
               0);
    REQUIRE_EQ(count_frame_lines(logfmt), 1);
    REQUIRE_TRUE(strstr(logfmt, "AFTERRECORD") != NULL);
  } else {
    fprintf(stderr,
            "SKIP (logfmt half): a frame line here is %zu bytes, no longer "
            "than the marker\n",
            shortest);
  }

  bool ok_json =
      run_backtrace_without_room(CLOG_FMT_JSON, 28, json, sizeof(json), NULL);
  REQUIRE_TRUE(ok_json);
  const char *marker = ",\"bt_error\":\"unavailable\"}\n";
  size_t line = strcspn(json, "\n") + 1;
  REQUIRE_EQ(line, (size_t)(4096 - 28) + strlen(marker));
  REQUIRE_EQ(strncmp(json + line - strlen(marker), marker, strlen(marker)), 0);
  REQUIRE_TRUE(strstr(json, "\"bt\":[") == NULL);
  REQUIRE_TRUE(strstr(json + line, "AFTERRECORD") != NULL);
}

// A JSON ERROR record with no room even to open "bt" becomes the "transient
// allocation failure" record with bt_error, so it never reads as a record that
// asked for no backtrace.
TEST(robustness, a_json_record_with_no_room_to_open_its_backtrace_is_replaced) {
  static char out[16384];
  bool ok =
      run_backtrace_without_room(CLOG_FMT_JSON, 4, out, sizeof(out), NULL);
  REQUIRE_TRUE(ok);
  size_t line = strcspn(out, "\n");
  REQUIRE_TRUE(memmem(out, line, "transient allocation failure",
                      strlen("transient allocation failure")) != NULL);
  REQUIRE_TRUE(memmem(out, line, "\"bt_error\":\"unavailable\"",
                      strlen("\"bt_error\":\"unavailable\"")) != NULL);
  REQUIRE_TRUE(memmem(out, line, "aaaaaaaaaa", 10) == NULL);
  REQUIRE_TRUE(strstr(out + line, "AFTERRECORD") != NULL);
}

// A logfmt ERROR record with no room even for the marker becomes the shorter
// "transient allocation failure" record, and the backtrace follows that.
TEST(robustness, a_record_with_no_room_for_its_backtrace_marker_is_replaced) {
  if (clog__test_backtrace_supported() &&
      !backtrace_capture_genuinely_available()) {
    fprintf(stderr, "SKIP: backtrace() captures no frames here\n");
    return;
  }
  static char out[16384];
  bool ok =
      run_backtrace_without_room(CLOG_FMT_LOGFMT, 20, out, sizeof(out), NULL);
  REQUIRE_TRUE(ok);
  size_t line = strcspn(out, "\n");
  REQUIRE_TRUE(memmem(out, line, "transient allocation failure",
                      strlen("transient allocation failure")) != NULL);
  REQUIRE_TRUE(memmem(out, line, "aaaaaaaaaa", 10) == NULL);
  REQUIRE_EQ(strncmp(out + line + 1, "\t#", 2), 0);
  REQUIRE_GE(count_frame_lines(out), 1);
  REQUIRE_TRUE(strstr(out, "AFTERRECORD") != NULL);
}

// Writes dir/app.log.1, plants a stale file (or a symbolic link when link_it
// is set) at its compression's temporary name and compresses it, for the test
// of what a compression does with that name. Returns whether the compression
// succeeded and reports whether the source and the temporary name are left,
// whether the destination is gzip, and its text (heap, caller frees).
static bool compress_over_planted_tmp(const char *dir, bool link_it,
                                      bool *has_src, char **dst_text,
                                      bool *dst_is_gzip, bool *tmp_left) {
  char src[256], dst[300], tmp[320];
  snprintf(src, sizeof(src), "%s/app.log.1", dir);
  snprintf(dst, sizeof(dst), "%s.gz", src);
  snprintf(tmp, sizeof(tmp), "%s.tmp", dst);
  int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool prepared = fd >= 0 && write(fd, "THE RECORD\n", 11) == 11;
  if (fd >= 0) close(fd);
  if (link_it) {
    prepared = prepared && symlink("elsewhere", tmp) == 0;
  } else {
    fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    prepared = prepared && fd >= 0 && write(fd, "half a gzip", 11) == 11;
    if (fd >= 0) close(fd);
  }
  bool ok = prepared && clog__test_gzip_compress_file(src, dst);
  *has_src = access(src, F_OK) == 0;
  struct stat st;
  *tmp_left = lstat(tmp, &st) == 0;
  unsigned char magic[2] = {0, 0};
  fd = open(dst, O_RDONLY);
  *dst_is_gzip = fd >= 0 && read(fd, magic, 2) == 2 && magic[0] == 0x1f &&
                 magic[1] == 0x8b;
  if (fd >= 0) close(fd);
  *dst_text = *dst_is_gzip ? _rtn_slurp(dst) : NULL;
  unlink(tmp);
  return ok;
}

// A regular file of our own at the temporary name is the leftover of an
// interrupted compression, so it's replaced and the archive is published.
// Anything else there, such as a link, is left alone and the source stays
// uncompressed.
TEST(compression, a_stale_temporary_of_its_own_is_replaced) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool own_src, own_gzip, own_tmp, link_src, link_gzip, link_tmp;
  char *own_text = NULL, *link_text = NULL;
  bool own_ok = compress_over_planted_tmp(dir, false, &own_src, &own_text,
                                          &own_gzip, &own_tmp);
  bool own_text_ok = own_text && strcmp(own_text, "THE RECORD\n") == 0;
  free(own_text);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool link_ok = compress_over_planted_tmp(dir, true, &link_src, &link_text,
                                           &link_gzip, &link_tmp);
  free(link_text);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(own_ok);
  REQUIRE_FALSE(own_src);
  REQUIRE_TRUE(own_gzip);
  REQUIRE_TRUE(own_text_ok);
  REQUIRE_FALSE(own_tmp);
  REQUIRE_FALSE(link_ok);
  REQUIRE_TRUE(link_src);
  REQUIRE_FALSE(link_gzip);
  REQUIRE_TRUE(link_tmp);
}

// Tells whether the line of text that holds the position at also holds
// needle, so a test can check a value on the record it belongs to.
static bool line_holds(const char *text, const char *at, const char *needle) {
  const char *start = at;
  while (start > text && start[-1] != '\n') start--;
  const char *end = strchr(at, '\n');
  size_t n = strlen(needle);
  for (const char *p = start; p + n <= (end ? end : at + strlen(at)); p++)
    if (memcmp(p, needle, n) == 0) return true;
  return false;
}

// Every allocation and slot of a derive can fail; clog__derive_checked() then
// reports ccol_not_enough_memory, the parent keeps its fields and target, and
// every block comes back after the last close.
TEST(derive, a_derive_without_memory_reports_it_and_gives_everything_back) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  atomic_store(&g_join_live, 0);
  clog parent = clog_create_from_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(parent, CLOG_NULL);
  bool fields_set = true;
  char key[16], value[48];
  for (int i = 0; i < 12; i++) {
    snprintf(key, sizeof(key), "k%02d", i);
    snprintf(value, sizeof(value), "value-%02d-of-the-parent-logger", i);
    fields_set =
        fields_set && clog_set_field(parent, key, value) == ccol_success;
  }
  int refused = 0, worked = 0, wrong_code = 0;
  for (int arms_slot = 0; arms_slot < 2; arms_slot++) {
    atomic_store(&g_join_trip_arms_slot, arms_slot != 0);
    for (long trip = 1; trip <= 60; trip++) {
      ccol_retval rv = ccol_success;
      atomic_store(&g_join_calls, 0);
      atomic_store(&g_join_trip, trip);
      clog child = clog__derive_checked(parent, &rv);
      atomic_store(&g_join_trip, -1);
      clog__test_force_next_fresh_slot_registration_failure(false);
      if (child == CLOG_NULL) {
        refused++;
        if (rv != ccol_not_enough_memory) wrong_code++;
        continue;
      }
      worked++;
      if (rv != ccol_success) wrong_code++;
      clog_info(child, "CHILD %d %ld", arms_slot, trip);
      clog_close(child);
    }
  }
  clog_info(parent, "PARENTAFTER");
  clog_close(parent);
  long live = atomic_load(&g_join_live);
  static char buf[1 << 17];
  read_file(path, buf, sizeof(buf));
  const char *last_child = strstr(buf, "CHILD 1 60");
  const char *parent_after = strstr(buf, "PARENTAFTER");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(fields_set);
  REQUIRE_GT(refused, 0);
  REQUIRE_GT(worked, 0);
  REQUIRE_EQ(wrong_code, 0);
  REQUIRE_EQ(live, 0L);
  REQUIRE_TRUE(last_child != NULL);
  REQUIRE_TRUE(parent_after != NULL);
  REQUIRE_TRUE(
      line_holds(buf, last_child, "k11=value-11-of-the-parent-logger"));
  REQUIRE_TRUE(
      line_holds(buf, parent_after, "k11=value-11-of-the-parent-logger"));
}

// The async writer keeps SIGPIPE and SIGXFSZ blocked under CLOG_SIGNALS_AUTO
// and unblocks both before its next write after a switch to
// CLOG_SIGNALS_UNTOUCHED. The writer inherits the opener's mask, so the test
// opens the logger with both signals unblocked.
TEST(async, the_writer_unblocks_its_signals_under_the_untouched_policy) {
  sigset_t both_set, old_mask;
  sigemptyset(&both_set);
  sigaddset(&both_set, SIGPIPE);
  sigaddset(&both_set, SIGXFSZ);
  pthread_sigmask(SIG_UNBLOCK, &both_set, &old_mask);
  char dir[256];
  int dir_rv = make_tmpdir(dir, sizeof(dir));
  if (dir_rv != 0) pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
  REQUIRE_EQ(dir_rv, 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_async_cfg acfg = {0};
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_NULL) {
    cleanup_dir(dir, "app.log");
    pthread_sigmask(SIG_SETMASK, &old_mask, NULL);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "UNDER AUTO");
  clog_flush(lg);
  bool auto_pipe = false, auto_xfsz = false;
  bool read_auto =
      clog__test_writer_blocked_signals(lg, &auto_pipe, &auto_xfsz);
  ccol_retval set_rv = clog_set_signal_policy(CLOG_SIGNALS_UNTOUCHED);
  clog_info(lg, "UNDER UNTOUCHED");
  clog_flush(lg);
  bool untouched_pipe = true, untouched_xfsz = true;
  bool read_untouched =
      clog__test_writer_blocked_signals(lg, &untouched_pipe, &untouched_xfsz);
  clog_set_signal_policy(CLOG_SIGNALS_AUTO);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
  pthread_sigmask(SIG_SETMASK, &old_mask, NULL);

  REQUIRE_TRUE(read_auto);
  REQUIRE_TRUE(read_untouched);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(auto_pipe);
  REQUIRE_TRUE(auto_xfsz);
  REQUIRE_FALSE(untouched_pipe);
  REQUIRE_FALSE(untouched_xfsz);
}

// The logger close_in_thread() closes, and the flag it sets when the close
// returns.
typedef struct {
  clog lg;
  _Atomic bool done;
} closer_arg_t;

// Closes a->lg and sets a->done, so a test can watch a close that has to wait
// from another thread.
static void *close_in_thread(void *arg) {
  closer_arg_t *a = (closer_arg_t *)arg;
  clog_close(a->lg);
  atomic_store(&a->done, true);
  return NULL;
}

// Tells whether the live file or any rotated file of the logger in dir,
// compressed or not, holds needle; a record can rotate into any of them.
static bool any_log_file_holds(const char *dir, const char *needle) {
  DIR *d = opendir(dir);
  if (!d) return false;
  bool found = false;
  struct dirent *e;
  while (!found && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", dir, e->d_name);
    char *text = _rtn_slurp(full);
    found = text && strstr(text, needle) != NULL;
    free(text);
  }
  closedir(d);
  return found;
}

// Releases the held compressions once the open has backed off from the
// closing target more often than the count at arg, so the open is surely
// waiting when the close can finish. It gives up after 30 s and releases
// anyway, and the test then fails on what the open saw.
static void *release_compressions_later(void *arg) {
  size_t waits_before = *(const size_t *)arg;
  for (long long end = test_now_ms() + 30000;
       clog__test_closing_target_waits() == waits_before &&
       test_now_ms() < end;)
    usleep(1000);
  clog__test_hold_compressions(false);
  return NULL;
}

// An open of a file whose last close is still finishing (waiting for its last
// compression) waits for that close and opens a new target, instead of joining
// a target on its way out or failing.
TEST(same_file, an_open_waits_for_the_last_close_of_the_same_file) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  clog__test_hold_compressions(true);
  clog_info(lg, "ROTATED AWAY");
  clog_info(lg, "SECOND");
  closer_arg_t ca = {.lg = lg, .done = false};
  pthread_t closer, releaser;
  bool closer_started =
      pthread_create(&closer, NULL, close_in_thread, &ca) == 0;
  bool waiting = false;
  for (long long end = test_now_ms() + 30000;
       closer_started && !waiting && test_now_ms() < end;) {
    waiting = clog__test_compressor_teardown_entered();
    if (!waiting) usleep(1000);
  }
  size_t waits_before = clog__test_closing_target_waits();
  bool releaser_started =
      waiting && pthread_create(&releaser, NULL, release_compressions_later,
                                &waits_before) == 0;
  clog again = CLOG_NULL;
  int gz_at_open = 0;
  if (releaser_started) {
    again = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
    gz_at_open = count_files_with_suffix(dir, ".gz");
    pthread_join(releaser, NULL);
  }
  bool open_waited = clog__test_closing_target_waits() > waits_before;
  clog__test_hold_compressions(false);
  if (closer_started)
    pthread_join(closer, NULL);
  else
    clog_close(lg);
  if (again != CLOG_NULL) {
    clog_info(again, "NEW TARGET");
    clog_close(again);
  }
  int gz = count_files_with_suffix(dir, ".gz");
  bool wrote = any_log_file_holds(dir, "NEW TARGET");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(closer_started);
  REQUIRE_TRUE(waiting);
  REQUIRE_TRUE(releaser_started);
  REQUIRE_NE(again, CLOG_NULL);
  REQUIRE_TRUE(open_waited);
  REQUIRE_GE(gz_at_open, 1);
  REQUIRE_GE(gz, 1);
  REQUIRE_TRUE(wrote);
}

// A generation whose plain and .gz files are both on disk counts once and is
// retired with both files together.
TEST(retention, a_generation_with_both_files_counts_once_and_goes_whole) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512], plain[600], gz[608];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(plain, sizeof(plain), "%s/app.log.20200101000000", dir);
  snprintf(gz, sizeof(gz), "%s.gz", plain);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 2};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_NULL);
  bool planted =
      write_whole_file(plain, "OLD\n", 4) && write_whole_file(gz, "OLD\n", 4);
  clog_info(lg, "FIRST ROTATION");
  bool both_after_first = access(plain, F_OK) == 0 && access(gz, F_OK) == 0;
  clog_info(lg, "SECOND ROTATION");
  bool plain_after_second = access(plain, F_OK) == 0;
  bool gz_after_second = access(gz, F_OK) == 0;
  clog_close(lg);
  int rotated = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(planted);
  REQUIRE_TRUE(both_after_first);
  REQUIRE_FALSE(plain_after_second);
  REQUIRE_FALSE(gz_after_second);
  REQUIRE_EQ(rotated, 2);
}

// A pass that can't allocate its listing gives up and frees what it took; the
// next pass with memory brings the count back to max_rotated_files.
TEST(retention, a_deletion_pass_without_memory_leaks_nothing) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 2};
  atomic_store(&g_join_live, 0);
  atomic_store(&g_join_trip_arms_slot, false);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &cfg, NULL, &procs);
  REQUIRE_NE(lg, CLOG_NULL);
  for (int i = 0; i < 3; i++) clog_info(lg, "FILL %d", i);
  for (long trip = 1; trip <= 40; trip++) {
    atomic_store(&g_join_calls, 0);
    atomic_store(&g_join_trip, trip);
    clog_info(lg, "SWEEP %02ld", trip);
    atomic_store(&g_join_trip, -1);
  }
  for (int i = 0; i < 3; i++) clog_info(lg, "LAST %d", i);
  clog_close(lg);
  long live = atomic_load(&g_join_live);
  int generations = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(live, 0L);
  REQUIRE_EQ(generations, 2);
}

// Bounded ends of the process
#include <sys/ioctl.h>

// Measures the capacity of the empty pipe (rd, wr) by filling it in
// non-blocking mode and reading everything back, so tests can size records
// against it. Leaves the pipe empty. Returns 0 on failure.
static size_t pipe_capacity(int rd, int wr) {
  int wfl = fcntl(wr, F_GETFL);
  int rfl = fcntl(rd, F_GETFL);
  if (wfl < 0 || rfl < 0) return 0;
  if (fcntl(wr, F_SETFL, wfl | O_NONBLOCK) != 0) return 0;
  static char chunk[4096];
  size_t cap = 0;
  for (;;) {
    ssize_t w = write(wr, chunk, sizeof(chunk));
    if (w > 0) {
      cap += (size_t)w;
      continue;
    }
    if (w < 0 && errno == EINTR) continue;
    if (w < 0 && errno == EAGAIN) {
      while (write(wr, chunk, 1) == 1) cap++;
    }
    break;
  }
  (void)fcntl(wr, F_SETFL, wfl);
  (void)fcntl(rd, F_SETFL, rfl | O_NONBLOCK);
  while (read(rd, chunk, sizeof(chunk)) > 0) {
  }
  (void)fcntl(rd, F_SETFL, rfl);
  return cap;
}

// Fills the empty pipe (rd, wr) completely and leaves wr in blocking mode, so
// the next write blocks. Returns false on failure.
static bool fill_pipe(int rd, int wr) {
  size_t cap = pipe_capacity(rd, wr);
  if (cap == 0) return false;
  int wfl = fcntl(wr, F_GETFL);
  if (wfl < 0 || fcntl(wr, F_SETFL, wfl | O_NONBLOCK) != 0) return false;
  static char chunk[4096];
  size_t put = 0;
  while (put < cap) {
    size_t n = cap - put < sizeof(chunk) ? cap - put : sizeof(chunk);
    ssize_t w = write(wr, chunk, n);
    if (w <= 0) break;
    put += (size_t)w;
  }
  (void)fcntl(wr, F_SETFL, wfl);
  return put == cap;
}

// The logger and record of a thread that logs into a full pipe. done is set
// once the call returns; a reader must drain until then, because FreeBSD hands
// a large pipe write over in pieces and the pipe can read empty while the
// write still waits.
typedef struct {
  clog lg;
  const char *rec;
  atomic_bool done;
} pinned_writer_arg_t;

// Logs a->rec once and sets a->done, so a test can hold a record inside its
// write to a full pipe.
static void *log_one_record(void *arg) {
  pinned_writer_arg_t *a = (pinned_writer_arg_t *)arg;
  clog_info(a->lg, "%s", a->rec);
  atomic_store(&a->done, true);
  return NULL;
}

// Waits, for at most 20 seconds, until a synchronous record of lg holds the
// logger's mutex for its write; with the pipe full, that record then stays
// pinned there until the test reads the pipe. Returns false on timeout.
static bool wait_for_record_in_write(clog lg) {
  for (long long end = test_now_ms() + 20000; test_now_ms() < end;) {
    if (clog__test_sync_records_locked(lg) >= 1) return true;
    usleep(1000);
  }
  return false;
}

// Waits, for at most 20 seconds, until a synchronous record of lg waits for
// the logger's mutex past the unlocked level check, so a test can change the
// level at that point. Returns false on timeout.
static bool wait_for_record_waiting_for_logger(clog lg) {
  for (long long end = test_now_ms() + 20000; test_now_ms() < end;) {
    if (clog__test_sync_lock_waiters(lg) >= 1) return true;
    usleep(1000);
  }
  return false;
}

// clog_close() waits for a call on the same handle that still runs (a record
// blocked on a full pipe), so the record goes out whole from memory that
// still belongs to the logger.
TEST(lifecycle, a_close_waits_for_a_call_still_running_on_the_handle) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  bool full = fill_pipe(p[0], p[1]);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  static char rec[8192];
  memset(rec, 'r', sizeof(rec) - 1);
  rec[sizeof(rec) - 1] = '\0';
  pinned_writer_arg_t wa = {.lg = lg, .rec = rec};
  closer_arg_t ca = {.lg = lg, .done = false};
  pthread_t writer, closer;
  bool writer_started = full && lg != CLOG_NULL &&
                        pthread_create(&writer, NULL, log_one_record, &wa) == 0;
  bool writer_in_write = writer_started && wait_for_record_in_write(lg);
  size_t pin_waits_before = clog__test_close_pin_waits();
  bool closer_started =
      writer_in_write &&
      pthread_create(&closer, NULL, close_in_thread, &ca) == 0;
  bool close_waited = false;
  for (long long end = test_now_ms() + 20000; closer_started && !close_waited &&
                                              !atomic_load(&ca.done) &&
                                              test_now_ms() < end;) {
    close_waited = clog__test_close_pin_waits() > pin_waits_before;
    if (!close_waited) usleep(1000);
  }
  bool closed_while_blocked = atomic_load(&ca.done);
  static char out[1 << 17];
  size_t n = 0;
  if (writer_started) {
    int rfl = fcntl(p[0], F_GETFL);
    (void)fcntl(p[0], F_SETFL, rfl | O_NONBLOCK);
    for (long long end = test_now_ms() + 20000;
         !atomic_load(&ca.done) && test_now_ms() < end;) {
      ssize_t r = read(p[0], out + n, sizeof(out) - 1 - n);
      if (r > 0)
        n += (size_t)r;
      else
        usleep(1000);
    }
    pthread_join(writer, NULL);
  }
  if (closer_started) pthread_join(closer, NULL);
  if (!closer_started && lg != CLOG_NULL) clog_close(lg);
  close(p[1]);
  ssize_t r;
  while ((r = read(p[0], out + n, sizeof(out) - 1 - n)) > 0) n += (size_t)r;
  out[n] = '\0';
  close(p[0]);
  const char *ts = memmem(out, n, "ts=", 3);
  const char *msg = ts ? strstr(ts, " msg=") : NULL;

  REQUIRE_TRUE(full);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(writer_started);
  REQUIRE_TRUE(writer_in_write);
  REQUIRE_TRUE(closer_started);
  REQUIRE_FALSE(closed_while_blocked);
  REQUIRE_TRUE(close_waited);
  REQUIRE_TRUE(atomic_load(&ca.done));
  REQUIRE_TRUE(msg != NULL);
  REQUIRE_EQ(strspn(msg + 5, "r"), sizeof(rec) - 1);
  REQUIRE_EQ(msg[5 + sizeof(rec) - 1], '\n');
}

// Logs one INFO record and sets a->done; it's the call that waits for the
// logger while the level changes.
static void *log_one_info_record(void *arg) {
  pinned_writer_arg_t *a = (pinned_writer_arg_t *)arg;
  clog_info(a->lg, "LATE INFO RECORD");
  atomic_store(&a->done, true);
  return NULL;
}

// A level change while a call waits for the logger applies to that call, so
// the waiting INFO record is dropped at WARN; the record already in its write
// goes out whole.
TEST(filtering, a_level_change_applies_to_a_call_that_waits_for_the_logger) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  bool full = fill_pipe(p[0], p[1]);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  static char rec[8192];
  memset(rec, 'r', sizeof(rec) - 1);
  rec[sizeof(rec) - 1] = '\0';
  pinned_writer_arg_t wa = {.lg = lg, .rec = rec};
  pinned_writer_arg_t la = {.lg = lg, .rec = NULL};
  pthread_t writer, late;
  bool writer_started = full && lg != CLOG_NULL &&
                        pthread_create(&writer, NULL, log_one_record, &wa) == 0;
  bool writer_in_write = writer_started && wait_for_record_in_write(lg);
  bool late_started =
      writer_in_write &&
      pthread_create(&late, NULL, log_one_info_record, &la) == 0;
  bool late_waits = late_started && wait_for_record_waiting_for_logger(lg);
  ccol_retval lrv =
      lg != CLOG_NULL ? clog_set_level(lg, CLOG_WARN) : ccol_invalid_args;
  static char out[1 << 17];
  size_t n = 0;
  if (writer_started) {
    int rfl = fcntl(p[0], F_GETFL);
    (void)fcntl(p[0], F_SETFL, rfl | O_NONBLOCK);
    for (long long end = test_now_ms() + 20000;
         !(atomic_load(&wa.done) && (!late_started || atomic_load(&la.done))) &&
         test_now_ms() < end;) {
      ssize_t r = read(p[0], out + n, sizeof(out) - 1 - n);
      if (r > 0)
        n += (size_t)r;
      else
        usleep(1000);
    }
    pthread_join(writer, NULL);
  }
  if (late_started) pthread_join(late, NULL);
  if (lg != CLOG_NULL) clog_close(lg);
  close(p[1]);
  ssize_t r;
  while ((r = read(p[0], out + n, sizeof(out) - 1 - n)) > 0) n += (size_t)r;
  out[n] = '\0';
  close(p[0]);
  const char *ts = memmem(out, n, "ts=", 3);
  bool late_written = memmem(out, n, "LATE INFO RECORD", 16) != NULL;

  REQUIRE_TRUE(full);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(writer_started);
  REQUIRE_TRUE(writer_in_write);
  REQUIRE_TRUE(late_started);
  REQUIRE_TRUE(late_waits);
  REQUIRE_EQ(lrv, ccol_success);
  REQUIRE_TRUE(ts != NULL);
  REQUIRE_NE(strstr(ts, "rrrrrrrr\n"), NULL);
  REQUIRE_FALSE(late_written);
}

// The logger and record of blocked_pipe_writer().
typedef struct {
  clog lg;
  const char *rec;
} blocked_writer_arg_t;

// Logs a record larger than the pipe, for ever, so the thread blocks inside a
// write with the logger's mutex held once the pipe is full.
static void *blocked_pipe_writer(void *arg) {
  blocked_writer_arg_t *a = (blocked_writer_arg_t *)arg;
  for (;;) clog_info(a->lg, "%s", a->rec);
  return NULL;
}

// Runs a child where one thread blocks in a write to a pipe nobody reads,
// holding that logger's mutex, while a CLOG_FATAL record goes to a second
// logger's file at path.
static void fatal_beside_blocked_pipe_child(const char *path) {
  forget_parent_fixtures_in_child();
  int p[2];
  if (pipe(p) != 0) test_child_exit(3);
  size_t cap = pipe_capacity(p[0], p[1]);
  if (cap == 0) test_child_exit(3);
  char *rec = malloc(2 * cap + 1);
  if (!rec) test_child_exit(3);
  memset(rec, 'x', 2 * cap);
  rec[2 * cap] = '\0';
  clog pipe_lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  clog file_lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  if (pipe_lg == CLOG_NULL || file_lg == CLOG_NULL) test_child_exit(3);
  blocked_writer_arg_t arg = {.lg = pipe_lg, .rec = rec};
  pthread_t tid;
  if (pthread_create(&tid, NULL, blocked_pipe_writer, &arg) != 0)
    test_child_exit(3);
  for (long long deadline_ms = test_now_ms() + 20000;
       test_now_ms() < deadline_ms;) {
    int avail = 0;
    if (ioctl(p[0], FIONREAD, &avail) == 0 && (size_t)avail >= cap) {
      mark_child_ready(path);
      clog_fatal(file_lg, "FATALBESIDEBLOCKEDPIPE");
    }
    usleep(1000);
  }
  test_child_exit(4);
}

// CLOG_FATAL ends the process although another thread is blocked for ever in
// a write holding another logger's mutex; a fatal path that took every
// logger's mutex would wait for ever.
TEST(fatal, a_logger_blocked_on_a_pipe_does_not_keep_fatal_from_ending) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) fatal_beside_blocked_pipe_child(path);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  char buf[8192] = {0};
  read_file(path, buf, sizeof(buf));
  bool has_fatal = strstr(buf, "FATALBESIDEBLOCKEDPIPE") != NULL;
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(has_fatal);
}

// Makes a connected stream socket pair whose p[1] end, in blocking mode, has
// no room left because nothing reads p[0], for the stalled-socket exit test.
// The fill ends with single bytes, since FreeBSD refuses a send below its
// low-water mark. Returns false on failure.
static bool make_full_stream_socket(int p[2]) {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, p) != 0) return false;
  int fl = fcntl(p[1], F_GETFL);
  if (fl < 0 || fcntl(p[1], F_SETFL, fl | O_NONBLOCK) != 0) return false;
  static char chunk[4096];
  while (send(p[1], chunk, sizeof(chunk), 0) > 0) {
  }
  if (errno == EAGAIN || errno == EWOULDBLOCK)
    while (send(p[1], chunk, 1, 0) > 0) {
    }
  bool full = errno == EAGAIN || errno == EWOULDBLOCK;
  return fcntl(p[1], F_SETFL, fl) == 0 && full;
}

// Runs a child whose async logger writes to a full pipe (a full stream socket
// when socket is set) in blocking mode that nobody reads. A forced ENOSPC on
// the exit drain's flush makes the writer keep the batch, so the settle after
// it meets a descriptor with no room. Ends with exit(), or with a CLOG_FATAL
// record on a second logger when via_fatal is set.
static void exit_with_stalled_pipe_child(const char *path, bool via_fatal,
                                         bool socket) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(2000);
  int p[2];
  if (socket ? !make_full_stream_socket(p)
             : pipe(p) != 0 || !fill_pipe(p[0], p[1]))
    test_child_exit(3);
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog pipe_lg = clog_create_from_fd(p[1], CLOG_INFO, &acfg);
  if (pipe_lg == CLOG_NULL) test_child_exit(3);
  clog null_lg = CLOG_NULL;
  if (via_fatal) {
    int nfd = open("/dev/null", O_WRONLY);
    null_lg = nfd >= 0 ? clog_create_from_fd(nfd, CLOG_INFO, NULL) : CLOG_NULL;
    if (null_lg == CLOG_NULL) test_child_exit(3);
  }
  clog_info(pipe_lg, "STALLEDPIPERECORD");
  mark_child_ready(path);
  if (!via_fatal) {
    clog__test_force_write_errors(ENOSPC, 1);
    exit(0);
  }
  clog__test_force_write_errors(ENOSPC, 3);
  clog_fatal(null_lg, "FATALRECORD");
  test_child_exit(4);
}

// Runs exit_with_stalled_pipe_child() in a child and returns true when it
// reached its exit() within bound_ms.
static bool run_exit_with_stalled_pipe_child(const char *dir, bool via_fatal,
                                             bool socket, int bound_ms) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_with_stalled_pipe_child(path, via_fatal, socket);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, bound_ms, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

// The last delivery attempt and the loss marker are bounded writes, so exit()
// and CLOG_FATAL end within the drain's budget beside a full blocking pipe. A
// bounded write that only polled after EAGAIN would block in write(2) for
// ever.
TEST(exit_drain, a_stalled_blocking_pipe_ends_within_the_budget) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool exit_ok = run_exit_with_stalled_pipe_child(dir, false, false, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool fatal_ok = run_exit_with_stalled_pipe_child(dir, true, false, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(exit_ok);
  REQUIRE_TRUE(fatal_ok);
}

// A full stream socket is written without waiting and polled for room only
// until the deadline, so exit() and CLOG_FATAL end within the budget.
TEST(exit_drain, a_stalled_stream_socket_ends_within_the_budget) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool exit_ok = run_exit_with_stalled_pipe_child(dir, false, true, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool fatal_ok = run_exit_with_stalled_pipe_child(dir, true, true, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(exit_ok);
  REQUIRE_TRUE(fatal_ok);
}

// Rotation and the permissions of the log

// A rotation creates the new live file with the mode of the file it replaces,
// whatever the umask, so a log restricted to 0600 stays 0600 in every
// generation.
TEST(rotation, every_generation_keeps_the_mode_of_the_log) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool created = fd >= 0 && fchmod(fd, 0600) == 0;
  if (fd >= 0) close(fd);
  mode_t old_mask = umask(022);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 100,
                           .compress_rotated = true};
  clog lg =
      created ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 6; i++) clog_info(lg, "SECRETRECORD %d", i);
    clog_close(lg);
  }
  umask(old_mask);
  int files = 0, gz = 0, wrong = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    files++;
    size_t n = strlen(e->d_name);
    if (n > 3 && strcmp(e->d_name + n - 3, ".gz") == 0) gz++;
    if ((st.st_mode & 0777) != 0600) wrong++;
  }
  if (d) closedir(d);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(created);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_GE(files, 5);
  REQUIRE_GE(gz, 4);
  REQUIRE_EQ(wrong, 0);
}

// The owner and the group of a compressed generation
#include <poll.h>

// Tells whether the process can give a file in the working directory to uid
// and gid, by trying it on a scratch file, so an owner test can skip when it
// can't. Root can, except in a user namespace that maps no other id, where
// chown fails with EINVAL.
static bool can_give_a_file_to(uid_t uid, gid_t gid) {
  char probe[] = "clogger_chown_probe_XXXXXX";
  int fd = mkstemp(probe);
  if (fd < 0) return false;
  bool ok = fchown(fd, uid, gid) == 0;
  close(fd);
  (void)unlink(probe);
  return ok;
}

// Picks into *out a group other than the effective one that the process may
// give a file (any group for root, otherwise a supplementary group), for the
// owner and group tests. Returns false when there's none.
static bool pick_foreign_group(gid_t *out) {
  gid_t own = getegid();
  if (geteuid() == 0) {
    *out = own == 1 ? 2 : 1;
    return true;
  }
  gid_t groups[256];
  int n = getgroups(256, groups);
  for (int i = 0; i < n; i++) {
    if (groups[i] != own) {
      *out = groups[i];
      return true;
    }
  }
  return false;
}

// A compressed generation gets the owner, group and mode of its source, so a
// log given to a group with 0640 stays readable by that group alone. A process
// with no second group to give a file has nothing to observe and says so.
TEST(compression,
     a_compressed_generation_keeps_the_owner_and_group_of_its_source) {
  gid_t group;
  if (!pick_foreign_group(&group)) {
    printf("  (skipped: the process has no second group to give a file)\n");
    return;
  }
  uid_t owner = geteuid() == 0 ? 1 : geteuid();
  if (!can_give_a_file_to(owner, group)) {
    printf("  (skipped: the process can't give a file to uid %lu gid %lu)\n",
           (unsigned long)owner, (unsigned long)group);
    return;
  }
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char src[256], dst[300];
  snprintf(src, sizeof(src), "%s/app.log.1", dir);
  snprintf(dst, sizeof(dst), "%s.gz", src);
  int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool prepared = fd >= 0 && write(fd, "RECORD\n", 7) == 7 &&
                  fchown(fd, owner, group) == 0 && fchmod(fd, 0640) == 0;
  if (fd >= 0) close(fd);
  bool compressed =
      prepared && clog__test_gzip_compress_file_for_owner(src, dst, owner);
  struct stat st;
  bool has_dst = stat(dst, &st) == 0;
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(prepared);
  REQUIRE_TRUE(compressed);
  REQUIRE_TRUE(has_dst);
  REQUIRE_EQ((unsigned long)st.st_gid, (unsigned long)group);
  REQUIRE_EQ((unsigned long)st.st_uid, (unsigned long)owner);
  REQUIRE_EQ((unsigned)(st.st_mode & 0777), 0640u);
}

// An fsync that the filesystem does not offer

// Compresses a fresh file while the next fsync() reports err, for the test of
// which fsync() errors fail a compression. Returns whether the compression
// succeeded and reports whether the .gz exists and the source is still
// there.
static bool compress_with_fsync_error(int err, bool *has_dst, bool *has_src) {
  char dir[64];
  *has_dst = false;
  *has_src = false;
  if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) return false;
  char src[256], dst[300];
  snprintf(src, sizeof(src), "%s/app.log.1", dir);
  snprintf(dst, sizeof(dst), "%s.gz", src);
  int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool prepared = fd >= 0 && write(fd, "RECORD\n", 7) == 7;
  if (fd >= 0) close(fd);
  clog__test_force_next_fsync_error(err);
  bool ok = prepared && clog__test_gzip_compress_file(src, dst);
  clog__test_force_next_fsync_error(0);
  *has_dst = access(dst, F_OK) == 0;
  *has_src = access(src, F_OK) == 0;
  cleanup_dir(dir, "app.log");
  return ok;
}

// EINVAL or ENOSYS from fsync() means the filesystem can't be asked to sync,
// so the compression succeeds; EIO fails it and keeps the source.
TEST(compression,
     an_fsync_that_the_filesystem_does_not_offer_still_compresses) {
  bool einval_dst, einval_src, enosys_dst, enosys_src, eio_dst, eio_src;
  bool einval_ok = compress_with_fsync_error(EINVAL, &einval_dst, &einval_src);
  bool enosys_ok = compress_with_fsync_error(ENOSYS, &enosys_dst, &enosys_src);
  bool eio_ok = compress_with_fsync_error(EIO, &eio_dst, &eio_src);
  REQUIRE_TRUE(einval_ok);
  REQUIRE_TRUE(einval_dst);
  REQUIRE_FALSE(einval_src);
  REQUIRE_TRUE(enosys_ok);
  REQUIRE_TRUE(enosys_dst);
  REQUIRE_FALSE(enosys_src);
  REQUIRE_FALSE(eio_ok);
  REQUIRE_FALSE(eio_dst);
  REQUIRE_TRUE(eio_src);
}

// A fatal record on a terminal whose reader stopped

// Opens a pseudo-terminal whose master never reads and fills it until less
// than one PIPE_BUF write fits, so POLLOUT on the slave is set but a blocking
// PIPE_BUF write would block. It reads the master in small steps until the
// slave reports writable, since FreeBSD needs the queue below its low-water
// mark. Returns false on failure.
static bool open_nearly_full_pty(int *master, int *slave) {
  *master = posix_openpt(O_RDWR | O_NOCTTY);
  if (*master < 0) return false;
  char name[64];
  const char *pts = NULL;
  if (grantpt(*master) != 0 || unlockpt(*master) != 0 ||
      !(pts = ptsname(*master)) || strlen(pts) >= sizeof(name)) {
    close(*master);
    return false;
  }
  memcpy(name, pts, strlen(pts) + 1);
  *slave = open(name, O_RDWR | O_NOCTTY);
  int filler = open(name, O_WRONLY | O_NOCTTY | O_NONBLOCK);
  if (*slave < 0 || filler < 0) {
    if (*slave >= 0) close(*slave);
    if (filler >= 0) close(filler);
    close(*master);
    return false;
  }
  char block[256];
  memset(block, 'x', sizeof(block));
  while (write(filler, block, sizeof(block)) > 0) {
  }
  close(filler);
  char drop[64];
  for (int reads = 0; reads < 256; reads++) {
    if (read(*master, drop, sizeof(drop)) <= 0) break;
    for (int i = 0; i < (reads == 0 ? 2000 : 20); i++) {
      struct pollfd pfd = {.fd = *slave, .events = POLLOUT, .revents = 0};
      if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLOUT)) return true;
      usleep(1000);
    }
  }
  close(*slave);
  close(*master);
  *slave = -1;
  *master = -1;
  return false;
}

// Runs a child that logs a 6000-byte CLOG_FATAL record to the nearly full
// terminal slave, for the stalled-terminal test.
static void fatal_on_stalled_tty_child(int slave, const char *path) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(1000);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) dup2(devnull, STDERR_FILENO);
  clog lg = clog_create_from_fd(slave, CLOG_INFO, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  static char big[6000];
  memset(big, 'y', sizeof(big) - 1);
  mark_child_ready(path);
  clog_fatal(lg, "%s", big);
  test_child_exit(4);
}

// POLLOUT on a terminal only says some room is free, so a bounded write that
// wrote PIPE_BUF bytes after it would block for ever on a frozen session.
// CLOG_FATAL must end within the budget.
TEST(fatal, a_stalled_terminal_does_not_keep_fatal_from_ending) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int master = -1, slave = -1;
  bool pty = open_nearly_full_pty(&master, &slave);
  pid_t pid = -1;
  int status = 0;
  bool ended = false;
  if (pty) {
    fflush(NULL);
    pid = fork();
    if (pid == 0) fatal_on_stalled_tty_child(slave, path);
    ended = pid > 0 && wait_child_bounded(pid, 15000, &status);
  }
  bool ready = child_was_ready(dir);
  if (slave >= 0) close(slave);
  if (master >= 0) close(master);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(pty);
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
}

// A fatal record behind a busy writer thread

// Runs a child whose async writer spends 20 ms per record on a backlog of 200,
// then logs a CLOG_FATAL record to the same file.
static void fatal_behind_busy_writer_child(const char *path) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(100);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) dup2(devnull, STDERR_FILENO);
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog__test_set_writer_job_delay_us(20000);
  for (int i = 0; i < 200; i++) clog_info(lg, "BACKLOG %d", i);
  mark_child_ready(path);
  clog_fatal(lg, "FATALBEHINDBACKLOG");
  test_child_exit(4);
}

// The writer takes the target's mutex again at once for each record, yet the
// fatal call gets it: The writer steps aside while a fatal call waits, and the
// fatal call waits at least long enough for the record in hand to finish.
TEST(fatal, a_busy_writer_thread_does_not_cost_the_fatal_record) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) fatal_behind_busy_writer_child(path);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  size_t cap = 1u << 20;
  char *buf = malloc(cap);
  bool has_fatal = false;
  if (buf) {
    read_file(path, buf, cap);
    has_fatal = strstr(buf, "FATALBEHINDBACKLOG") != NULL;
    free(buf);
  }
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_TRUE(has_fatal);
}

// A record logged after the exit drain

// The async logger that _log_after_the_exit_drain() logs to in a child, and
// whether it closes it too; CLOG_NULL everywhere else.
static clog g_after_drain_logger = CLOG_NULL;
static bool g_after_drain_close;
// How many records that destructor logs (1 when 0), and whether it flushes.
static int g_after_drain_records;
static bool g_after_drain_flush;

// A program's destructors run after every exit handler, so after the exit
// drain. OpenBSD runs no destructor of a program that links libpthread, so
// EXIT_TEST_PROGRAM_DESTRUCTORS_RUN is 0 there and the tests that log or open
// from a destructor skip.
#if defined(__OpenBSD__)
#define EXIT_TEST_PROGRAM_DESTRUCTORS_RUN 0
#else
#define EXIT_TEST_PROGRAM_DESTRUCTORS_RUN 1
#endif
// Returns from the calling test with a SKIP line where program destructors
// don't run, because the tests that log or open from a destructor have
// nothing to observe there.
#define EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS()                     \
  do {                                                                   \
    if (!EXIT_TEST_PROGRAM_DESTRUCTORS_RUN) {                            \
      fprintf(stderr, "SKIP: this system runs no program destructor\n"); \
      return;                                                            \
    }                                                                    \
  } while (0)
// Set by a child whose output has no room at exit, so the destructor gives the
// room back before it logs.
static bool g_after_drain_room_back;
// Set by the destructor shortly before it closes the logger, so the child's
// logging threads start no more calls (a call on a closed handle is fatal).
static atomic_bool g_loggers_stop;

// Logs to g_after_drain_logger after the library's exit drain has run, which
// is what the after-drain tests need, then flushes or closes it as the globals
// ask. Before a close it sets g_loggers_stop and waits a little, so the
// child's logging threads finish or block inside the library first.
__attribute__((destructor)) static void _log_after_the_exit_drain(void) {
  if (g_after_drain_logger == CLOG_NULL) return;
  if (g_after_drain_room_back) clog__test_set_output_room(0, 0, 0);
  int records = g_after_drain_records > 0 ? g_after_drain_records : 1;
  for (int i = 0; i < records; i++)
    clog_info(g_after_drain_logger, "AFTERDRAINRECORD");
  if (g_after_drain_flush) clog_flush(g_after_drain_logger);
  if (g_after_drain_close) {
    atomic_store(&g_loggers_stop, true);
    usleep(100000);
    clog_close(g_after_drain_logger);
  }
  g_after_drain_logger = CLOG_NULL;
}

// The write end of a pipe in a child of the test below, -1 everywhere else;
// _open_after_the_release() reports its verdict there.
static int g_late_open_report_fd = -1;

// Opens a logger after the library's module destructor released the handle
// table (a destructor with a priority runs after those without one), and
// writes 'R' (refused) or 'O' (opened) to g_late_open_report_fd.
__attribute__((destructor(101))) static void _open_after_the_release(void) {
  if (g_late_open_report_fd < 0) return;
  int fd = open("/dev/null", O_WRONLY);
  clog lg = fd >= 0 ? clog_create_from_fd(fd, CLOG_INFO, NULL) : CLOG_NULL;
  char verdict = lg == CLOG_NULL ? 'R' : 'O';
  if (lg != CLOG_NULL) clog_close(lg);
  if (fd >= 0) close(fd);

  if (write(g_late_open_report_fd, &verdict, 1) != 1) test_child_exit(4);
  test_child_exit(0);
}

TEST(lifecycle, an_open_after_the_table_is_released_is_refused) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int nfd = open("/dev/null", O_WRONLY);
    clog first =
        nfd >= 0 ? clog_create_from_fd(nfd, CLOG_INFO, NULL) : CLOG_NULL;
    if (first == CLOG_NULL) test_child_exit(5);
    clog_close(first);
    close(nfd);
    if (g_deferred_release_a != CLOG_NULL) clog_close(g_deferred_release_a);
    if (g_deferred_release_b != CLOG_NULL) clog_close(g_deferred_release_b);
    forget_parent_fixtures_in_child();
    g_late_open_report_fd = p[1];
    exit(0);
  }
  close(p[1]);
  char verdict = 0;
  ssize_t got = pid > 0 ? read(p[0], &verdict, 1) : -1;
  close(p[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_EQ(got, (ssize_t)1);
  REQUIRE_EQ(verdict, 'R');
  REQUIRE_TRUE(WIFEXITED(status));
}

// Runs a child that logs one record through an async logger, hands the logger
// to _log_after_the_exit_drain() (closing it there when close_after is set)
// and exits.
static void log_after_drain_child(const char *path, size_t qsize,
                                  bool close_after) {
  forget_parent_fixtures_in_child();
  clog_async_cfg acfg = {.queue_size = qsize,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "BEFOREEXITRECORD");
  g_after_drain_logger = lg;
  g_after_drain_close = close_after;
  mark_child_ready(path);
  exit(0);
}

// Runs log_after_drain_child() and tells whether both records are in the
// file, the child ended and it reached its exit().
static bool run_log_after_drain_child(size_t qsize, bool close_after) {
  char dir[64];
  if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) return false;
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) log_after_drain_child(path, qsize, close_after);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  char buf[8192];
  read_file(path, buf, sizeof(buf));
  bool before = strstr(buf, "BEFOREEXITRECORD") != NULL;
  bool after = strstr(buf, "AFTERDRAINRECORD") != NULL;
  cleanup_dir(dir, "app.log");
  return ended && WIFEXITED(status) && ready && before && after;
}

// exit_drain_race_child()'s logger, for its second thread.
static clog drain_race_lg;

// Logs three records while the exit drain on the child's main thread waits for
// the writer, which spends 1 s on the record ahead of them.
static void *drain_race_logger(void *arg) {
  (void)arg;
  usleep(200000);
  for (int i = 1; i <= 3; i++)
    clog_info(drain_race_lg, "DURINGDRAINRECORD%d", i);
  return NULL;
}

// Runs a child where one record keeps the writer busy, the exit drain starts,
// and another thread logs while it waits. The child ends with _exit(), so only
// that drain can have written the records.
static void exit_drain_race_child(const char *path, size_t qsize) {
  forget_parent_fixtures_in_child();
  clog_async_cfg acfg = {.queue_size = qsize,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  drain_race_lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  if (drain_race_lg == CLOG_NULL) test_child_exit(3);
  clog__test_set_writer_unlocked_job_delay_us(1000000);
  clog_info(drain_race_lg, "BEFOREDRAINRECORD");
  pthread_t t;
  if (pthread_create(&t, NULL, drain_race_logger, NULL) != 0)
    test_child_exit(4);
  clog__test_run_exit_drain();
  pthread_join(t, NULL);
  test_child_exit(0);
}

// Runs exit_drain_race_child() and tells whether every record is in the file
// in the order it was logged. The exit status carries no verdict, because
// valgrind reports the child's leaks through it.
static bool run_exit_drain_race_child(size_t qsize) {
  char dir[64];
  if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) return false;
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_drain_race_child(path, qsize);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 60000, &status);
  char buf[8192];
  read_file(path, buf, sizeof(buf));
  const char *prev = strstr(buf, "BEFOREDRAINRECORD");
  bool in_order = prev != NULL;
  for (int i = 1; i <= 3 && in_order; i++) {
    char name[32];
    snprintf(name, sizeof(name), "DURINGDRAINRECORD%d", i);
    const char *at = strstr(buf, name);
    in_order = at != NULL && at > prev;
    prev = at;
  }
  cleanup_dir(dir, "app.log");
  return ended && WIFEXITED(status) && in_order;
}

// The drain turns sends off before it queues its flush request, so a record
// another thread logs meanwhile isn't accepted behind that request and thrown
// away at the end of the process.
TEST(exit_drain, a_record_logged_while_the_drain_runs_is_written) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  bool unbounded = run_exit_drain_race_child(0);
  bool bounded = run_exit_drain_race_child(16);
  REQUIRE_TRUE(unbounded);
  REQUIRE_TRUE(bounded);
}

// A record an async logger gets after the exit drain (from a destructor) is
// written by the thread that logs it, instead of waiting in the batch for a
// flush that never comes.
TEST(exit_drain, a_record_logged_after_the_drain_is_written) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  bool unbounded = run_log_after_drain_child(0, false);
  bool bounded = run_log_after_drain_child(16, false);
  REQUIRE_TRUE(unbounded);
  REQUIRE_TRUE(bounded);
}

// After the drain the queue refuses the close's shutdown request, so a close
// that retried for ever would hang.
TEST(exit_drain, a_close_after_the_drain_ends) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  bool unbounded = run_log_after_drain_child(0, true);
  bool bounded = run_log_after_drain_child(16, true);
  REQUIRE_TRUE(unbounded);
  REQUIRE_TRUE(bounded);
}

// A rotated file whose .gz name doesn't fit in the directory (253 bytes, 256
// with .gz) stays uncompressed, and a record in the new live file says so.
TEST(compression, a_name_too_long_to_compress_is_reported) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long name_max = pathconf(dir, _PC_NAME_MAX);
  char base[240];
  memset(base, 'b', 238);
  base[238] = '\0';
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, base);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg = name_max == 255 ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL)
                            : CLOG_NULL;
  if (lg != CLOG_NULL) {
    clog_info(lg, "LONGNAMERECORD");
    clog_close(lg);
  }
  char buf[4096] = {0};
  read_file(path, buf, sizeof(buf));
  bool reported = strstr(buf, "stays uncompressed") != NULL &&
                  strstr(buf, "level=WARN") != NULL;
  int rotated = count_files_with_prefix(dir, "bbbbbbbbbb");
  int gz = count_files_with_suffix(dir, ".gz");
  cleanup_dir(dir, "bbbbbbbbbb");
  if (name_max != 255) return;
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(reported);
  REQUIRE_EQ(rotated, 2);
  REQUIRE_EQ(gz, 0);
}

// The path and config of recover_opener() and the logger it opens.
typedef struct {
  const char *path;
  const clog_rotation_cfg *cfg;
  clog lg;
} recover_open_arg_t;

// Opens a->path with a->cfg into a->lg from a thread, to race a second open
// against the recovery of the first one.
static void *recover_opener(void *arg) {
  recover_open_arg_t *a = (recover_open_arg_t *)arg;
  a->lg = clog_create_from_file(a->path, CLOG_INFO, a->cfg, NULL);
  return NULL;
}

// The first open runs its recovery before the target is published, so a
// second open can't join, rotate and start a compression that the recovery
// would take for a leftover; every rotated file is compressed once.
TEST(compression, open_recovery_leaves_a_running_compression_alone) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  size_t finished_before = clog__test_compressions_finished();
  clog__test_hold_compressions(true);
  clog__test_hold_compression_recovery(true);
  recover_open_arg_t first = {.path = path, .cfg = &cfg, .lg = CLOG_NULL};
  pthread_t tid;
  bool started = pthread_create(&tid, NULL, recover_opener, &first) == 0;
  bool entered = false;
  for (long long deadline_ms = test_now_ms() + 10000;
       started && !entered && test_now_ms() < deadline_ms;) {
    entered = clog__test_compression_recovery_entered();
    if (!entered) usleep(1000);
  }
  clog second =
      started ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (second != CLOG_NULL) clog_info(second, "RECOVERYRACERECORD");
  bool held = false;
  for (long long deadline_ms = test_now_ms() + 10000;
       second != CLOG_NULL && !held && test_now_ms() < deadline_ms;) {
    held = clog__test_gz_dest_opened();
    if (!held) usleep(1000);
  }
  clog__test_hold_compression_recovery(false);
  if (started) pthread_join(tid, NULL);
  clog__test_hold_compressions(false);
  if (second != CLOG_NULL) clog_close(second);
  if (first.lg != CLOG_NULL) clog_close(first.lg);
  size_t runs = clog__test_compressions_finished() - finished_before;
  int gz = count_files_with_suffix(dir, ".gz");
  int tmp = count_files_with_suffix(dir, ".gz.tmp");
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(entered);
  REQUIRE_NE(first.lg, CLOG_NULL);
  REQUIRE_NE(second, CLOG_NULL);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(runs, (size_t)1);
  REQUIRE_EQ(gz, 1);
  REQUIRE_EQ(tmp, 0);
}

// The enqueue-failure fallback and records that a batch keeps

// Logs RECORD_A and keeps it in the batch with errors failed writes, then logs
// RECORD_B through the enqueue-failure fallback and closes. Stores the file
// offsets of both records in off_a and off_b, or -1.
static void run_fallback_order_case(unsigned int errors, long *off_a,
                                    long *off_b) {
  *off_a = -1;
  *off_b = -1;
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  if (lg != CLOG_NULL) {
    clog_info(lg, "RECORD_A");
    clog__test_force_write_errors(ENOSPC, errors);
    atomic_store(&g_enqueue_fail_call_count, 0);
    atomic_store(&g_enqueue_fail_at, 2);
    clog_info(lg, "RECORD_B");
    atomic_store(&g_enqueue_fail_at, -1);
    clog__test_force_write_errors(0, 0);
    clog_close(lg);
  }
  char buf[4096] = {0};
  read_file(path, buf, sizeof(buf));
  const char *a = strstr(buf, "RECORD_A");
  const char *b = strstr(buf, "RECORD_B");
  if (a) *off_a = (long)(a - buf);
  if (b) *off_b = (long)(b - buf);
  cleanup_dir(dir, "app.log");
}

// The batch gets one more attempt before the fallback writes its record, and
// if that fails too the record joins the batch behind the kept one, so it
// never lands ahead of it.
TEST(async, enqueue_failure_fallback_keeps_the_order_of_kept_records) {
  long a1, b1, a2, b2;
  run_fallback_order_case(1, &a1, &b1);
  run_fallback_order_case(2, &a2, &b2);
  REQUIRE_GE(a1, 0L);
  REQUIRE_GT(b1, a1);
  REQUIRE_GE(a2, 0L);
  REQUIRE_GT(b2, a2);
}

// Runs a child whose logger (sync, or async when async is set) writes to a
// full blocking pipe that nobody reads, and logs a CLOG_FATAL record on that
// same logger. Its stderr goes to "<path>err" for the parent to read.
static void fatal_on_stalled_own_pipe_child(const char *path, bool async) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(1000);
  char errp[512];
  snprintf(errp, sizeof(errp), "%serr", path);
  int efd = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (efd < 0 || dup2(efd, STDERR_FILENO) < 0) test_child_exit(3);
  int p[2];
  if (pipe(p) != 0 || !fill_pipe(p[0], p[1])) test_child_exit(3);
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, async ? &acfg : NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  if (async) clog_info(lg, "QUEUEDBEFOREFATAL");
  mark_child_ready(path);
  clog_fatal(lg, "FATALONSTALLEDPIPE");
  test_child_exit(4);
}

// Runs fatal_on_stalled_own_pipe_child() and tells whether the child exited
// with EXIT_FAILURE within 20 s after marking itself ready. *reported tells
// whether its stderr names the lost fatal record.
static bool run_fatal_on_stalled_own_pipe(const char *dir, bool async,
                                          bool *reported) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) fatal_on_stalled_own_pipe_child(path, async);
  if (pid < 0) return false;
  int status = 0;
  bool ended = wait_child_bounded(pid, 20000, &status);
  char errp[512], buf[1024] = {0};
  snprintf(errp, sizeof(errp), "%serr", path);
  read_file(errp, buf, sizeof(buf));
  *reported = strstr(buf, "CLOG_FATAL record could not be written") != NULL;
  return ended && WIFEXITED(status) && WEXITSTATUS(status) == EXIT_FAILURE &&
         child_was_ready(dir);
}

// A CLOG_FATAL record whose own output is a full blocking pipe nobody reads
// still stops the process within the budget, sync and async; the record is
// lost and a line on stderr says so.
TEST(fatal,
     a_stalled_output_of_its_own_logger_does_not_keep_fatal_from_ending) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  bool sync_reported = false, async_reported = false;
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool sync_ok = run_fatal_on_stalled_own_pipe(dir, false, &sync_reported);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool async_ok = run_fatal_on_stalled_own_pipe(dir, true, &async_reported);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(sync_ok);
  REQUIRE_TRUE(async_ok);
  REQUIRE_TRUE(sync_reported);
  REQUIRE_TRUE(async_reported);
}

// Rotated-looking entries that are not generations of the log

// The rotation hooks below run once, in the window where the live name names
// neither the old file nor the new one, and plant an entry there as another
// user who can write the directory would. g_rh_hook_fired makes them act once,
// g_rh_link_planted is set only when the link really exists (a failed plant
// would check nothing), and g_rh_fifo_reader holds the planted FIFO's reader
// so an open of it doesn't wait.
static atomic_int g_rh_hook_fired;
static atomic_int g_rh_link_planted;
static int g_rh_fifo_reader = -1;

// Plants a symbolic link at the live name, once, for the link hijack test.
static void rh_plant_link(int dir_fd, const char *base) {
  if (atomic_exchange(&g_rh_hook_fired, 1)) return;
  if (symlinkat("app.log-victim", dir_fd, base) == 0)
    atomic_store(&g_rh_link_planted, 1);
}

// Plants a FIFO at the live name, once, and opens its reader into
// g_rh_fifo_reader, for the FIFO hijack test.
static void rh_plant_fifo(int dir_fd, const char *base) {
  if (atomic_exchange(&g_rh_hook_fired, 1)) return;
  if (mkfifoat(dir_fd, base, 0600) == 0)
    g_rh_fifo_reader = openat(dir_fd, base, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

// Returns the kind of the entry at path without following a link (S_IFREG,
// S_IFLNK, S_IFIFO and so on), or 0 when nothing is there, so a test can
// check what sits at a name.
static mode_t rh_entry_kind(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  return st.st_mode & S_IFMT;
}

// Counts the occurrences of needle in the file at path, to find where the
// records went.
static int rh_count_lines_with(const char *path, const char *needle) {
  char buf[16384];
  read_file(path, buf, sizeof(buf));
  int n = 0;
  for (const char *p = buf; (p = strstr(p, needle)) != NULL; p++) n++;
  return n;
}

// The new live file is one the rotation made, and it takes the live name with
// a rename that replaces whatever sits there, so a planted link gets no
// record.
TEST(rotation_hijack, a_link_put_at_the_live_name_gets_no_record) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], victim[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(victim, sizeof(victim), "%s/app.log-victim", dir);
  bool setup = write_whole_file(victim, "VICTIM\n", 7);

  atomic_store(&g_rh_hook_fired, 0);
  atomic_store(&g_rh_link_planted, 0);
  clog__test_set_rotate_window_hook(rh_plant_link);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 200,
                           .max_rotated_files = 3};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 40; i++) clog_info(lg, "RHRECORD %d", i);
    clog_close(lg);
  }
  clog__test_set_rotate_window_hook(NULL);

  bool fired = atomic_load(&g_rh_hook_fired) != 0;
  bool planted = atomic_load(&g_rh_link_planted) != 0;
  char vbuf[256];
  read_file(victim, vbuf, sizeof(vbuf));
  mode_t live_kind = rh_entry_kind(path);
  bool last_record_kept =
      any_file_with_prefix_contains(dir, "app.log", "RHRECORD 39");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(fired);
  REQUIRE_TRUE(planted);
  REQUIRE_STREQ(vbuf, "VICTIM\n");
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
  REQUIRE_TRUE(last_record_kept);
}

// A FIFO planted at the live name during a rotation never blocks it and never
// gets a record.
TEST(rotation_hijack, a_fifo_put_at_the_live_name_gets_no_record) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  atomic_store(&g_rh_hook_fired, 0);
  g_rh_fifo_reader = -1;
  clog__test_set_rotate_window_hook(rh_plant_fifo);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 200,
                           .max_rotated_files = 3};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 4; i++) clog_info(lg, "RHFIFO %d padding", i);
    clog_close(lg);
  }
  clog__test_set_rotate_window_hook(NULL);

  bool fired = atomic_load(&g_rh_hook_fired) != 0;
  int reader = g_rh_fifo_reader;
  g_rh_fifo_reader = -1;
  char fbuf[256];
  ssize_t got = reader >= 0 ? read(reader, fbuf, sizeof(fbuf)) : -1;
  if (reader >= 0) close(reader);
  mode_t live_kind = rh_entry_kind(path);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(fired);
  REQUIRE_NE(reader, -1);
  REQUIRE_LE(got, 0);
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
}

// Runs a child that opens a compressing logger next to a FIFO at a rotated
// name, logs and closes, and marks that the close returned, for the test that
// the FIFO is never opened.
static void rh_fifo_at_rotated_name_child(const char *path) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 1,
                           .compress_rotated = true};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "RHGEN");
  clog_close(lg);
  mark_child_ready(path);
  test_child_exit(0);
}

// A FIFO at a rotated name isn't a generation, so the open's recovery never
// queues it and the compressor never opens it; otherwise clog_close() would
// wait for ever behind a compressor waiting for a writer.
TEST(rotation_foreign, a_fifo_at_a_rotated_name_never_blocks_close) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], fifo[256], gz[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(fifo, sizeof(fifo), "%s/app.log.20200101000000", dir);
  snprintf(gz, sizeof(gz), "%s/app.log.20200101000000.gz", dir);
  bool setup = mkfifo(fifo, 0600) == 0;

  fflush(NULL);
  pid_t pid = setup ? fork() : -1;
  if (pid == 0) rh_fifo_at_rotated_name_child(path);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  mode_t fifo_kind = rh_entry_kind(fifo);
  mode_t gz_kind = rh_entry_kind(gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(ready);
  REQUIRE_EQ((unsigned)fifo_kind, (unsigned)S_IFIFO);
  REQUIRE_EQ((unsigned)gz_kind, 0u);
}

// A link at a rotated name isn't a generation, so nothing reads, archives or
// deletes it, and the file it names is never copied into a .gz.
TEST(rotation_foreign, a_link_at_a_rotated_name_is_never_read_or_removed) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], link[256], gz[256], secret_dir[256], secret[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(link, sizeof(link), "%s/app.log.20200101000000", dir);
  snprintf(gz, sizeof(gz), "%s/app.log.20200101000000.gz", dir);
  snprintf(secret_dir, sizeof(secret_dir), "%s/secretdir", dir);
  snprintf(secret, sizeof(secret), "%s/secretdir/secret", dir);
  bool setup = mkdir(secret_dir, 0700) == 0 &&
               write_whole_file(secret, "TOPSECRET\n", 10) &&
               symlink("secretdir/secret", link) == 0;

  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 300,
                           .max_rotated_files = 1,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 30; i++) clog_info(lg, "RHGEN %d padding-padding", i);
    clog_close(lg);
  }

  mode_t link_kind = rh_entry_kind(link);
  mode_t gz_kind = rh_entry_kind(gz);
  char sbuf[64];
  read_file(secret, sbuf, sizeof(sbuf));
  unlink(secret);
  rmdir(secret_dir);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ((unsigned)link_kind, (unsigned)S_IFLNK);
  REQUIRE_EQ((unsigned)gz_kind, 0u);
  REQUIRE_STREQ(sbuf, "TOPSECRET\n");
}

// A regular file whose rotated name has a stamp far in the future and a
// suffix nothing can follow isn't a generation: Naming doesn't start above it
// (which would refuse every rotation), and it takes no retention slot.
TEST(rotation_foreign, a_far_future_name_neither_stops_rotation_nor_counts) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], stuck[256], future[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(stuck, sizeof(stuck), "%s/app.log.99991231235959_999999999", dir);
  snprintf(future, sizeof(future), "%s/app.log.99991231235959", dir);
  bool setup =
      write_whole_file(stuck, "", 0) && write_whole_file(future, "F\n", 2);

  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 300,
                           .max_rotated_files = 2};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 60; i++)
      clog_info(lg, "RHSTALL %d padding-padding-padding", i);
    clog_close(lg);
  }

  int generations = count_files_with_prefix(dir, "app.log.2");
  mode_t stuck_kind = rh_entry_kind(stuck);
  mode_t future_kind = rh_entry_kind(future);
  struct stat live_st;
  bool live_bounded = stat(path, &live_st) == 0 && live_st.st_size < 2048;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(generations, 2);
  REQUIRE_EQ((unsigned)stuck_kind, (unsigned)S_IFREG);
  REQUIRE_EQ((unsigned)future_kind, (unsigned)S_IFREG);
  REQUIRE_TRUE(live_bounded);
}

// Every rotated-looking entry that isn't a generation gets a WARN record, at
// most eight per minute.
TEST(rotation_foreign, ignored_entries_are_reported_at_a_bounded_rate) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  bool setup = true;
  for (int i = 0; i < 20; i++) {
    char l[256];
    snprintf(l, sizeof(l), "%s/app.log.202001010000%02d", dir, i);
    setup &= symlink("nowhere", l) == 0;
  }
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) clog_close(lg);
  int warns = rh_count_lines_with(
      path,
      "ignored an entry that is not a rotated file of this log: app.log.");
  int links = count_files_with_prefix(dir, "app.log.2020");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(warns, 8);
  REQUIRE_EQ(links, 20);
}

TEST(utf8, four_byte_characters_pass_and_values_past_the_last_are_replaced) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "%s", "x\xf0\x9f\x98\x80y");
  clog_info(lg, "%s",
            "a\xf4\x90\x80\x80"
            "b");
  char buf[4096];
  drain_pipe(lg, p[0], p[1], buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, " msg=x\xf0\x9f\x98\x80y\n"), NULL);
  REQUIRE_EQ(strchr(buf, '\xf4'), NULL);
  REQUIRE_NE(strstr(buf, " msg=\"a\xef\xbf\xbd"), NULL);
}

TEST(rotation, a_max_file_size_of_zero_is_the_default_size) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 0,
                           .max_rotated_files = 3};
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 40; i++) clog_info(lg, "ZERO LIMIT %02d", i);
    clog_close(lg);
  }
  int rotated = count_files_with_prefix(dir, "app.log.");
  static char buf[16384];
  read_file(path, buf, sizeof(buf));
  bool all = strstr(buf, "ZERO LIMIT 00") && strstr(buf, "ZERO LIMIT 39");
  cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ(rotated, 0);
  REQUIRE_TRUE(all);
}

// A rotating logger opens its directory first, so a directory part longer than
// PATH_MAX fails with ENAMETOOLONG.
TEST(lifecycle, a_rotating_open_of_a_too_long_directory_fails) {
  static char path[PATH_MAX * 2 + 16];
  size_t n = 0;
  while (n + 2 < PATH_MAX + 64) {
    path[n++] = 'd';
    path[n++] = '/';
  }
  memcpy(path + n, "app.log", sizeof("app.log"));
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 4096,
                           .max_rotated_files = 3};
  errno = 0;
  clog lg = clog_create_from_file(path, CLOG_INFO, &cfg, NULL);
  int err = errno;
  if (lg != CLOG_NULL) clog_close(lg);
  REQUIRE_EQ(lg, CLOG_NULL);
  REQUIRE_EQ(err, ENAMETOOLONG);
}

// The last-resort syslog record is one RFC 5424 line with the record's
// priority and every other header field nil.
TEST(robustness, unrepresentable_record_syslog_is_one_header_line) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  ccol_retval frv = clog_set_facility(lg, CLOG_SYSLOG_LOCAL3);
  clog__test_write_unrepresentable_record(lg, CLOG_FMT_SYSLOG, CLOG_ERROR,
                                          false);
  char buf[4096];
  size_t len = drain_pipe(lg, p[0], p[1], buf, sizeof(buf));
  char want[64];
  snprintf(want, sizeof(want), "<%d>1 - - - - - - log record dropped",
           (int)CLOG_SYSLOG_LOCAL3 * 8 + 3);
  REQUIRE_EQ(frv, ccol_success);
  REQUIRE_EQ(strncmp(buf, want, strlen(want)), 0);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_EQ(buf[len - 1], '\n');
  REQUIRE_EQ(strchr(buf, '\n'), buf + len - 1);
}

// The WARN about a foreign entry shows at most 96 bytes of its name and then
// "...", so a long planted name keeps the record short.
TEST(rotation_foreign, a_long_ignored_name_is_cut_in_its_warning) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char base[160];
  memset(base, 'b', 120);
  memcpy(base + 120, ".log", sizeof(".log"));
  char path[256], link[300];
  snprintf(path, sizeof(path), "%s/%s", dir, base);
  snprintf(link, sizeof(link), "%s/%s.20200101000000", dir, base);
  bool setup = symlink("nowhere", link) == 0;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 5,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) clog_close(lg);
  static char buf[8192];
  read_file(path, buf, sizeof(buf));
  char shown[120];
  memset(shown, 'b', 96);
  memcpy(shown + 96, "...", sizeof("..."));
  bool cut = strstr(buf, shown) != NULL;
  bool whole = strstr(buf, ".20200101000000") != NULL;
  unlink(link);
  cleanup_dir(dir, "b");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(cut);
  REQUIRE_FALSE(whole);
}

// The state of drain_pipe_later(): The read end, the buffer it fills, the
// writable-wait count before the test and whether a wait was seen.
typedef struct {
  int rd;
  char *buf;
  size_t cap;
  size_t len;
  size_t waits_before;
  bool saw_wait;
} pipe_drainer_t;

// Drains the pipe into d->buf once a write waits for room in it, so the record
// really meets a full pipe whatever the scheduling. It waits at most 20 s,
// then drains anyway, and the test fails on saw_wait.
static void *drain_pipe_later(void *arg) {
  pipe_drainer_t *d = (pipe_drainer_t *)arg;
  for (long long end = test_now_ms() + 20000;
       !d->saw_wait && test_now_ms() < end;) {
    d->saw_wait = clog__test_writable_waits() > d->waits_before;
    if (!d->saw_wait) usleep(1000);
  }
  ssize_t r;
  while (d->len < d->cap - 1 &&
         (r = read(d->rd, d->buf + d->len, d->cap - 1 - d->len)) > 0)
    d->len += (size_t)r;
  d->buf[d->len] = '\0';
  return NULL;
}

TEST(output, a_record_to_a_full_non_blocking_pipe_waits_for_room) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  bool full = fill_pipe(p[0], p[1]);
  int fl = fcntl(p[1], F_GETFL);
  bool nonblocking = fl >= 0 && fcntl(p[1], F_SETFL, fl | O_NONBLOCK) == 0;
  clog__test_watch_writable(p[1]);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  static char out[1 << 17];
  pipe_drainer_t d = {.rd = p[0],
                      .buf = out,
                      .cap = sizeof(out),
                      .len = 0,
                      .waits_before = clog__test_writable_waits(),
                      .saw_wait = false};
  pthread_t t;
  bool started = full && nonblocking && lg != CLOG_NULL &&
                 pthread_create(&t, NULL, drain_pipe_later, &d) == 0;
  if (started) clog_info(lg, "WAITEDFORROOM %s", "zzzzzzzzzzzzzzzzzzzzzzzzzz");
  if (lg != CLOG_NULL) clog_close(lg);
  close(p[1]);
  if (started) pthread_join(t, NULL);
  clog__test_watch_writable(-1);
  close(p[0]);
  const char *rec = memmem(out, d.len, "ts=", 3);
  REQUIRE_TRUE(full);
  REQUIRE_TRUE(nonblocking);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(d.saw_wait);
  REQUIRE_TRUE(rec != NULL);
  REQUIRE_NE(strstr(rec, " msg=\"WAITEDFORROOM zzzzzzzzzzzzzzzzzzzzzzzzzz\"\n"),
             NULL);
}

// A file at a rotated name owned by another user isn't a generation. Only the
// superuser can create one, so for anyone else the test does nothing.
TEST(rotation_foreign, a_file_another_user_owns_is_never_compressed) {
  if (geteuid() != 0) {
    printf("  (needs root to create a file another user owns; not run)\n");
    return;
  }
  if (!can_give_a_file_to(65534, 65534)) {
    printf("  (this root can't give a file to uid 65534; not run)\n");
    return;
  }
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], foreign[256], gz[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(foreign, sizeof(foreign), "%s/app.log.20200101000000", dir);
  snprintf(gz, sizeof(gz), "%s/app.log.20200101000000.gz", dir);
  bool setup = write_whole_file(foreign, "THEIRS\n", 7) &&
               chown(foreign, 65534, 65534) == 0;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 300,
                           .max_rotated_files = 1,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 30; i++) clog_info(lg, "RHGEN %d padding-padding", i);
    clog_close(lg);
  }
  char fbuf[64];
  read_file(foreign, fbuf, sizeof(fbuf));
  mode_t gz_kind = rh_entry_kind(gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_STREQ(fbuf, "THEIRS\n");
  REQUIRE_EQ((unsigned)gz_kind, 0u);
}

// The next open removes a rotation's private file that a stopped process left
// empty, and only that: A file with content or a link of that name shape
// stays.
TEST(rotation_foreign, open_removes_only_an_empty_private_file_of_a_rotation) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], empty[256], full[256], link[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(empty, sizeof(empty), "%s/app.log.tmp0123456789a", dir);
  snprintf(full, sizeof(full), "%s/app.log.tmp0123456789b", dir);
  snprintf(link, sizeof(link), "%s/app.log.tmp0123456789c", dir);
  bool setup = write_whole_file(empty, "", 0) &&
               write_whole_file(full, "DATA\n", 5) &&
               symlink("nowhere", link) == 0;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1 << 20,
                           .max_rotated_files = 5};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) clog_close(lg);
  mode_t empty_kind = rh_entry_kind(empty);
  mode_t full_kind = rh_entry_kind(full);
  mode_t link_kind = rh_entry_kind(link);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_EQ((unsigned)empty_kind, 0u);
  REQUIRE_EQ((unsigned)full_kind, (unsigned)S_IFREG);
  REQUIRE_EQ((unsigned)link_kind, (unsigned)S_IFLNK);
}

// A logger whose path is a link writes through it until the first rotation
// moves the link to a rotated name; after that the link is never followed,
// compressed or removed.
TEST(rotation_foreign, a_live_path_that_is_a_link_is_moved_aside_once) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], target[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(target, sizeof(target), "%s/app.log-target", dir);
  bool setup =
      write_whole_file(target, "", 0) && symlink("app.log-target", path) == 0;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 300,
                           .max_rotated_files = 50,
                           .compress_rotated = true};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 30; i++)
      clog_info(lg, "RHLINK %02d padding-padding", i);
    clog_close(lg);
  }
  char tbuf[4096];
  read_file(target, tbuf, sizeof(tbuf));
  bool first_in_target = strstr(tbuf, "RHLINK 00") != NULL;
  bool last_in_target = strstr(tbuf, "RHLINK 29") != NULL;
  mode_t live_kind = rh_entry_kind(path);
  int links = 0;
  char link_gz[256 + sizeof(((struct dirent *)0)->d_name) + 3] = {0};
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    if (strncmp(e->d_name, "app.log.", 8) == 0 && rh_entry_kind(p) == S_IFLNK) {
      links++;
      snprintf(link_gz, sizeof(link_gz), "%s.gz", p);
    }
  }
  if (d) closedir(d);
  mode_t link_gz_kind = link_gz[0] ? rh_entry_kind(link_gz) : 0;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_TRUE(first_in_target);
  REQUIRE_FALSE(last_in_target);
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
  REQUIRE_EQ(links, 1);
  REQUIRE_EQ((unsigned)link_gz_kind, 0u);
}

// An output with no room

// Counts the record starts in the line [line, end) (a logfmt "ts=", a JSON
// "{\"", a syslog ">1 ", a logfmt marker), to find a record written onto
// another one's fragment: A well-framed line holds at most one.
static int nr_record_starts(const char *line, const char *end) {
  static const char *const starts[] = {"ts=", "{\"", ">1 ",
                                       "level=ERROR msg=\"log record"};
  int n = 0;
  for (size_t k = 0; k < sizeof(starts) / sizeof(starts[0]); k++) {
    size_t slen = strlen(starts[k]);
    for (const char *p = line; p + slen <= end; p++)
      if (memcmp(p, starts[k], slen) == 0) n++;
  }
  return n;
}

// Counts the lines of buf that hold more than one record start, that is, the
// records written onto another record's fragment.
static int count_joined_lines(const char *buf) {
  int joined = 0;
  const char *p = buf;
  while (*p) {
    const char *nl = strchr(p, '\n');
    const char *end = nl ? nl : p + strlen(p);
    if (nr_record_starts(p, end) > 1) joined++;
    if (!nl) break;
    p = nl + 1;
  }
  return joined;
}

// Logs, to a synchronous logger in fmt, a record cut after 30 bytes, one while
// the output has no room, and one once room is back, then reads the file into
// buf, for the test that a record never joins a fragment. Returns false when
// the setup failed.
static bool run_sync_no_room(clog_format fmt, char *buf, size_t cap) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return false;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  if (lg == CLOG_NULL) {
    cleanup_dir(dir, "app.log");
    return false;
  }
  clog_set_format(lg, fmt);
  clog_info(lg, "BEFOREFULLRECORD");
  clog__test_set_output_room(ENOSPC, 30, 0);
  clog_info(lg, "CUTRECORD %s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  clog_info(lg, "LOSTRECORD");
  clog__test_set_output_room(0, 0, 0);
  clog_info(lg, "AFTERFULLRECORD");
  clog_close(lg);
  read_file(path, buf, cap);
  cleanup_dir(dir, "app.log");
  return true;
}

// When the newline and marker that would close a fragment fail on the same
// full disk, the logger remembers both, writes nothing that would continue the
// fragment, and puts the newline and one marker for both losses in front of
// the next record once there is room.
TEST(output_without_room, a_sync_record_never_joins_a_fragment) {
  static char logfmt[16384];
  static char json[16384];
  bool ok_logfmt = run_sync_no_room(CLOG_FMT_LOGFMT, logfmt, sizeof(logfmt));
  bool ok_json = run_sync_no_room(CLOG_FMT_JSON, json, sizeof(json));
  REQUIRE_TRUE(ok_logfmt);
  REQUIRE_TRUE(ok_json);
  char want[96];
  snprintf(want, sizeof(want), "): write error %d", ENOSPC);
  const char *bufs[] = {logfmt, json};
  for (int i = 0; i < 2; i++) {
    const char *b = bufs[i];
    const char *marker =
        strstr(b, "log record truncated: 2 earlier losses (records=2 bytes=");
    const char *after = strstr(b, "AFTERFULLRECORD");
    REQUIRE_EQ(count_joined_lines(b), 0);
    REQUIRE_TRUE(strstr(b, "BEFOREFULLRECORD") != NULL);
    REQUIRE_TRUE(marker != NULL);
    REQUIRE_TRUE(strstr(marker, want) != NULL);
    REQUIRE_TRUE(after != NULL);
    REQUIRE_TRUE(marker < after);
    REQUIRE_TRUE(strstr(b, "LOSTRECORD") == NULL);
  }
}

// The async form: When the newline that closes a fragment fails, the next
// flush must not write the batch onto the fragment; once there's room the
// newline and the marker go first.
TEST(output_without_room, an_async_batch_never_joins_a_fragment) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  int rfl = fcntl(p[0], F_GETFL);
  (void)fcntl(p[0], F_SETFL, rfl | O_NONBLOCK);
  clog_async_cfg acfg = {.flush_buffer_size = 1024UL * 1024UL,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, &acfg);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "BATCHONE %s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  clog_info(lg, "BATCHTWO");
  clog__test_set_output_room(ENOSPC, 30, 0);
  clog_flush(lg);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_info(lg, "SYSLOGWHILEFULL");
  clog_flush(lg);
  clog__test_set_output_room(0, 0, 0);
  clog_info(lg, "SYSLOGAFTER");
  clog_flush(lg);
  clog_close(lg);
  close(p[1]);
  static char buf[65536];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof(buf) - 1 &&
         (r = read(p[0], buf + n, sizeof(buf) - 1 - n)) > 0)
    n += (size_t)r;
  buf[n] = '\0';
  close(p[0]);

  const char *marker = strstr(buf, "log record truncated: ");
  const char *two = strstr(buf, "BATCHTWO");
  const char *after = strstr(buf, "SYSLOGAFTER");
  REQUIRE_EQ(count_joined_lines(buf), 0);
  REQUIRE_TRUE(marker != NULL);
  REQUIRE_TRUE(two != NULL);
  REQUIRE_TRUE(after != NULL);
  REQUIRE_TRUE(marker < two);
  REQUIRE_TRUE(two < after);
  REQUIRE_TRUE(strstr(buf, "SYSLOGWHILEFULL") == NULL);
}

// When the writer can't allocate the private buffer a syslog record needs
// beside the undelivered bytes, the record becomes the last-resort record and
// the undelivered batch stays for the next flush.
TEST(output_without_room, a_writer_without_memory_keeps_the_undelivered_batch) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  int rfl = fcntl(p[0], F_GETFL);
  (void)fcntl(p[0], F_SETFL, rfl | O_NONBLOCK);
  ccol_memmgmt_procs procs = {.malloc = _join_malloc,
                              .free = _join_free,
                              .calloc = _join_calloc,
                              .realloc = _join_realloc};
  clog_async_cfg acfg = {.flush_buffer_size = 1024UL * 1024UL,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_fd_mp(p[1], CLOG_INFO, &acfg, &procs);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "BATCHONE %s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  clog_info(lg, "BATCHTWO");
  clog__test_set_output_room(ENOSPC, 30, 0);
  clog_flush(lg);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  g_join_main_thread = pthread_self();
  atomic_store(&g_join_other_threads_fail, true);
  clog_info(lg, "SYSLOGWHILEFULL");
  clog_flush(lg);
  atomic_store(&g_join_other_threads_fail, false);
  clog__test_set_output_room(0, 0, 0);
  clog_info(lg, "SYSLOGAFTER");
  clog_flush(lg);
  clog_close(lg);
  close(p[1]);
  static char buf[65536];
  size_t n = 0;
  ssize_t r;
  while (n < sizeof(buf) - 1 &&
         (r = read(p[0], buf + n, sizeof(buf) - 1 - n)) > 0)
    n += (size_t)r;
  buf[n] = '\0';
  close(p[0]);

  const char *two = strstr(buf, "BATCHTWO");
  const char *after = strstr(buf, "SYSLOGAFTER");
  REQUIRE_EQ(count_joined_lines(buf), 0);
  REQUIRE_TRUE(two != NULL);
  REQUIRE_TRUE(after != NULL);
  REQUIRE_TRUE(two < after);
  REQUIRE_TRUE(strstr(buf, "SYSLOGWHILEFULL") == NULL);
}

// A marker that fails because the output has no room is kept and written in
// front of the first record the output accepts later.
TEST(output_without_room, a_loss_is_named_once_the_output_has_room) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg,
                                     NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "BATCHRECORDONE");
  clog_info(lg, "BATCHRECORDTWO");
  clog__test_set_output_room(ENOSPC, 0, 0);
  clog__test_force_write_errors(EPIPE, 1);
  clog_flush(lg);
  clog__test_force_write_errors(0, 0);
  char mid[4096];
  size_t mid_len = read_file(path, mid, sizeof(mid));
  clog__test_set_output_room(0, 0, 0);
  clog_info(lg, "AFTERRECOVERY");
  clog_flush(lg);
  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));
  char want[96];
  snprintf(want, sizeof(want), "write error %d", EPIPE);
  const char *marker =
      strstr(buf, "undelivered batch dropped (records=2 bytes=");
  const char *after = strstr(buf, "AFTERRECOVERY");
  bool has_why = marker != NULL && strstr(marker, want) != NULL;
  bool has_batch = strstr(buf, "BATCHRECORD") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(mid_len, (size_t)0);
  REQUIRE_TRUE(marker != NULL);
  REQUIRE_TRUE(has_why);
  REQUIRE_TRUE(after != NULL);
  REQUIRE_TRUE(marker < after);
  REQUIRE_FALSE(has_batch);
}

// Counts the rotated generations of app.log in dir, for the rotation tests on
// a full output, and stores in *max_size the size of its largest file, the
// live one included.
static int rotated_generations(const char *dir, off_t *max_size) {
  int n = 0;
  if (max_size) *max_size = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    struct stat st;
    if (stat(p, &st) != 0) continue;
    if (max_size && st.st_size > *max_size) *max_size = st.st_size;
    if (e->d_name[7] == '.') n++;
  }
  if (d) closedir(d);
  return n;
}

// Reads every file of app.log in dir one after another into buf and returns
// the length, so a test can check the whole log across its generations. Each
// file's text starts on a line of its own, because a generation can end in a
// fragment that belongs to that file alone.
static size_t read_all_log_files(const char *dir, char *buf, size_t cap) {
  size_t n = 0;
  buf[0] = '\0';
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    if (n + 2 >= cap) break;
    size_t got = read_file(p, buf + n, cap - n - 1);
    n += got;
    if (got > 0 && buf[n - 1] != '\n') {
      buf[n++] = '\n';
      buf[n] = '\0';
    }
  }
  if (d) closedir(d);
  return n;
}

// Counts the files of app.log in dir that start with an empty line, which a
// repair that writes a stray newline would leave.
static int log_files_starting_empty(const char *dir) {
  int n = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    char head[2];
    if (read_file(p, head, sizeof(head)) == 1 && head[0] == '\n') n++;
  }
  if (d) closedir(d);
  return n;
}

// When the file stops taking bytes in the middle of a record (EFBIG, which no
// retry outlasts) and a size rotation is due, the async logger gives up the
// continuation and rotates, so rotation and retention keep working while the
// error lasts.
TEST(output_without_room, a_refused_continuation_does_not_stop_rotation) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 2000,
                            .max_rotated_files = 2};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg,
                                     &g_undelivered_acfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "FIRSTBATCH %02d %s", 0, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  clog_flush(lg);
  struct stat first;
  size_t rec_len = stat(path, &first) == 0 ? (size_t)first.st_size : 200;
  for (int i = 1; i < 30; i++)
    clog_info(lg, "FIRSTBATCH %02d %s", i, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  clog__test_set_output_room(EFBIG, rec_len * 10 + rec_len / 2, 0);
  clog_flush(lg);
  for (int i = 0; i < 30; i++) clog_info(lg, "WHILEFULL %02d", i);
  clog_flush(lg);
  int gens_while_full = rotated_generations(dir, NULL);
  clog__test_set_output_room(0, 0, 0);
  clog_flush(lg);
  static char all[1 << 20];
  read_all_log_files(dir, all, sizeof(all));
  bool marker = strstr(all, "log record truncated: ") != NULL;
  bool kept = strstr(all, "WHILEFULL 29") != NULL;
  int joined = count_joined_lines(all);
  int empty_starts = log_files_starting_empty(dir);
  for (int i = 0; i < 300; i++)
    clog_info(lg, "LATER %03d %s", i, "zzzzzzzzzzzzzzzzzzzzzzzzzzzzzz");
  clog_close(lg);
  int gens_end = rotated_generations(dir, NULL);
  read_all_log_files(dir, all, sizeof(all));
  bool last = strstr(all, "LATER 299") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_GE(gens_while_full, 1);
  REQUIRE_TRUE(marker);
  REQUIRE_TRUE(kept);
  REQUIRE_EQ(joined, 0);
  REQUIRE_EQ(empty_starts, 0);
  REQUIRE_EQ(gens_end, 2);
  REQUIRE_TRUE(last);
}

// Runs, in a forked child, a rotating logger (async when async is set) whose
// RLIMIT_FSIZE sits between max_file_size and one record more, so the record
// that crosses max_file_size is cut and a size rotation follows. Writes to
// fd_out the number of rotated generations, the number of files whose last
// byte isn't a newline, and whether a marker names the loss.
static void fsize_cut_child(const char *dir, bool async, int fd_out) {
  int res[3] = {-1, -1, -1};
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  signal(SIGXFSZ, SIG_IGN);
  char probe[512];
  snprintf(probe, sizeof(probe), "%s/probe.txt", dir);
  clog pl = clog_create_from_file(probe, CLOG_INFO, NULL, NULL);
  size_t rec = 0;
  if (pl != CLOG_NULL) {
    clog_info(pl, "FSZCUT %03d %s", 0, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
    clog_close(pl);
    struct stat st;
    if (stat(probe, &st) == 0) rec = (size_t)st.st_size;
  }
  unlink(probe);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = (int64_t)(rec * 10 + rec / 4),
                            .max_rotated_files = 50};
  struct rlimit rl = {.rlim_cur = rec * 10 + rec * 3 / 4,
                      .rlim_max = rec * 10 + rec * 3 / 4};
  clog lg = CLOG_NULL;
  if (rec > 0 && setrlimit(RLIMIT_FSIZE, &rl) == 0)
    lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg,
                                  async ? &g_undelivered_acfg : NULL, NULL);
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 60; i++) {
      clog_info(lg, "FSZCUT %03d %s", i, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
      clog_flush(lg);
    }
    clog_close(lg);
    res[0] = rotated_generations(dir, NULL);
    res[1] = 0;
    res[2] = 0;
    DIR *d = opendir(dir);
    struct dirent *e;
    while (d && (e = readdir(d)) != NULL) {
      if (strncmp(e->d_name, "app.log", 7) != 0) continue;
      char p[256 + sizeof(e->d_name)];
      snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
      static char body[1 << 16];
      size_t n = read_file(p, body, sizeof(body));
      if (n > 0 && body[n - 1] != '\n') res[1]++;
      if (strstr(body, "log record truncated: ") != NULL) res[2] = 1;
    }
    if (d) closedir(d);
  }
  if (write(fd_out, res, sizeof(res)) != (ssize_t)sizeof(res))
    test_child_exit(2);
  test_child_exit(0);
}

// Runs fsize_cut_child() and stores its three results in res. Returns false
// when the child couldn't run or report.
static bool run_fsize_cut(bool async, int res[3]) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return false;
  int p[2];
  if (pipe(p) != 0) {
    cleanup_dir(dir, "app.log");
    return false;
  }
  pid_t pid = fork();
  if (pid == 0) {
    close(p[0]);
    fsize_cut_child(dir, async, p[1]);
  }
  close(p[1]);
  ssize_t got = -1;
  if (pid > 0) {
    got = read(p[0], res, 3 * sizeof(int));
    int status = 0;
    waitpid(pid, &status, 0);
  }
  close(p[0]);
  cleanup_dir(dir, "app.log");
  return got == (ssize_t)(3 * sizeof(int));
}

// A record cut by a file size limit right before a size rotation leaves a
// fragment in the retired file, and the rotation still closes it with a
// newline, so reading generations one after another never glues the marker
// onto the fragment.
TEST(output_without_room, every_generation_ends_with_a_newline) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int sync_res[3] = {0};
  int async_res[3] = {0};
  bool ok_sync = run_fsize_cut(false, sync_res);
  bool ok_async = run_fsize_cut(true, async_res);
  REQUIRE_TRUE(ok_sync);
  REQUIRE_TRUE(ok_async);
  REQUIRE_GE(sync_res[0], 2);
  REQUIRE_EQ(sync_res[1], 0);
  REQUIRE_EQ(sync_res[2], 1);
  REQUIRE_GE(async_res[0], 2);
  REQUIRE_EQ(async_res[1], 0);
  REQUIRE_EQ(async_res[2], 1);
}

// A backlog many times max_file_size goes out in pieces that end on record
// boundaries, with a rotation between them, so no file grows past
// max_file_size by more than one record.
TEST(output_without_room, a_backlog_is_split_across_generations) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 4096,
                            .max_rotated_files = 50};
  clog_async_cfg acfg = {.flush_buffer_size = 1024,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_file(path, CLOG_INFO, &rcfg, &acfg);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_set_output_room(ENOSPC, 0, 0);
  for (int i = 0; i < 200; i++)
    clog_info(lg, "BACKLOG %03d %s", i, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
  clog_flush(lg);
  clog__test_set_output_room(0, 0, 0);
  clog_flush(lg);
  off_t biggest = 0;
  int gens = rotated_generations(dir, &biggest);
  clog_close(lg);
  cleanup_dir(dir, "app.log");

  REQUIRE_GE(gens, 4);
  REQUIRE_LE(biggest, (off_t)(4096 + 512));
}

// Counts the files of dir whose name starts with prefix and whose content
// starts with a tab, so a test can find a generation that begins with a torn
// backtrace.
static int files_starting_with_a_tab(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    char first[2] = {0, 0};
    read_file(p, first, sizeof(first));
    if (first[0] == '\t') n++;
  }
  closedir(d);
  return n;
}

// Counts the backtrace lines (lines that start with a tab) in the files of dir
// whose name starts with prefix, so a test can tell whether any backtrace
// existed to tear.
static int tab_lines_in_files(const char *dir, const char *prefix) {
  DIR *d = opendir(dir);
  if (!d) return -1;
  int n = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, prefix, strlen(prefix)) != 0) continue;
    char p[256 + sizeof(e->d_name)];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    char *text = _rtn_slurp(p);
    for (const char *s = text; s && *s; s++)
      if (*s == '\t' && (s == text || s[-1] == '\n')) n++;
    free(text);
  }
  closedir(d);
  return n;
}

// A backlog is split only at a record boundary, never between a record and
// its backtrace lines. Without backtrace lines the test can't tell the two
// apart and checks nothing.
TEST(output_without_room,
     a_split_backlog_keeps_each_backtrace_with_its_record) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 4096,
                            .max_rotated_files = 100};
  clog_async_cfg acfg = {.flush_buffer_size = 1024,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_file(path, CLOG_INFO, &rcfg, &acfg);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_set_output_room(ENOSPC, 0, 0);
  for (int i = 0; i < 60; i++) clog_error(lg, "BACKLOG ERROR %03d", i);
  clog_flush(lg);
  clog__test_set_output_room(0, 0, 0);
  clog_flush(lg);
  int gens = rotated_generations(dir, NULL);
  clog_close(lg);
  int torn = files_starting_with_a_tab(dir, "app.log");
  int tab_lines = tab_lines_in_files(dir, "app.log");
  cleanup_dir(dir, "app.log");

  if (tab_lines == 0) {
    fprintf(stderr, "SKIP: the ERROR records carry no backtrace here\n");
    return;
  }
  REQUIRE_GE(gens, 3);
  REQUIRE_EQ(torn, 0);
}

// A WARN about a foreign entry that the output takes only part of is closed
// and named by a marker once there is room, like any other record.
TEST(rotation_foreign, a_cut_warning_is_named_as_a_loss) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], link[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(link, sizeof(link), "%s/app.log.20200101000000", dir);
  bool setup = symlink("nowhere", link) == 0;
  clog_rotation_cfg cfg = {.size_rotation_enabled = true,
                           .max_file_size = 1,
                           .max_rotated_files = 10};
  clog lg =
      setup ? clog_create_from_file(path, CLOG_INFO, &cfg, NULL) : CLOG_NULL;
  off_t rec_len = 0;
  static char live[8192];
  if (lg != CLOG_NULL) {
    for (int i = 0; i < 2; i++) {
      if (i == 1) {
        DIR *d = opendir(dir);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL)
          if (strncmp(e->d_name, "app.log.2", 9) == 0 &&
              strcmp(e->d_name, "app.log.20200101000000") != 0) {
            char p[256 + sizeof(e->d_name)];
            snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
            static char gen[4096];
            read_file(p, gen, sizeof(gen));
            rec_len = (off_t)(strcspn(gen, "\n") + 1);
          }
        if (d) closedir(d);
        clog__test_set_output_room(ENOSPC, (size_t)rec_len + 5, 1);
      }
      clog_info(lg, "RECORD BEFORE A WARNING %d", i);
    }
    clog__test_set_output_room(0, 0, 0);
    read_file(path, live, sizeof(live));
    clog_close(lg);
  }
  unlink(link);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_NULL);
  REQUIRE_GT(rec_len, (off_t)0);
  REQUIRE_EQ(strcspn(live, "\n"), (size_t)5);
  REQUIRE_NE(strstr(live, "log record truncated: 5 of "), NULL);
  REQUIRE_EQ(count_joined_lines(live), 0);
}

// When the backlog starts with the rest of a cut record, the split state ends
// once the first piece delivers it, so size rotations run between the later
// pieces.
TEST(output_without_room, a_backlog_that_starts_mid_record_is_split_too) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 4096,
                            .max_rotated_files = 50};
  clog_async_cfg acfg = {.flush_buffer_size = 1024,
                         .flush_interval_us = 60000000};
  clog lg = clog_create_from_file(path, CLOG_INFO, &rcfg, &acfg);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_set_output_room(ENOSPC, 1000, 0);
  for (int i = 0; i < 200; i++)
    clog_info(lg, "BACKLOG %03d %s", i, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
  clog_flush(lg);
  clog__test_set_output_room(0, 0, 0);
  clog_flush(lg);
  off_t biggest = 0;
  int gens = rotated_generations(dir, &biggest);
  clog_close(lg);
  cleanup_dir(dir, "app.log");

  REQUIRE_GE(gens, 4);
  REQUIRE_LE(biggest, (off_t)(4096 + 512));
}

// A synchronous logger that owes a repair and never writes again pays it at
// exit: The file ends with a newline and a marker.
TEST(output_without_room, an_owed_repair_is_paid_at_exit) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog__test_set_output_room(ENOSPC, 150, 0);
  for (int i = 0; i < 5; i++)
    clog_info(lg, "OWED %d %s", i, "wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
  clog__test_set_output_room(0, 0, 0);
  clog__test_run_exit_drain();
  char buf[8192];
  size_t n = read_file(path, buf, sizeof(buf));
  bool ends_whole = n > 0 && buf[n - 1] == '\n';
  bool named = strstr(buf, "earlier losses") != NULL ||
               strstr(buf, "log record truncated") != NULL;
  clog_close(lg);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(ends_whole);
  REQUIRE_TRUE(named);
}

// A syslog record written directly and cut leaves a repair owed with an empty
// batch behind it; the writer's next flush pays it once there is room,
// without waiting for another record or the exit.
TEST(output_without_room,
     an_owed_repair_with_an_empty_batch_is_paid_by_a_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_GE(fd, 0);
  clog_async_cfg acfg = {0};
  clog lg = clog_create_from_fd(fd, CLOG_INFO, &acfg);
  if (lg == CLOG_NULL) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog__test_set_output_room(ENOSPC, 60, 0);
  clog_info(lg, "OWED %s", "wwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwwww");
  clog_flush(lg);
  clog__test_set_output_room(0, 0, 0);
  clog_flush(lg);
  char buf[8192];
  size_t n = read_file(path, buf, sizeof(buf));
  bool ends_whole = n > 0 && buf[n - 1] == '\n';
  bool named = strstr(buf, "log record truncated") != NULL;
  clog_close(lg);
  close(fd);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(ends_whole);
  REQUIRE_TRUE(named);
}

// Logs one record from a new thread and returns whether that thread got an
// identity cache of its own, for the test of records after the module
// destructor.
static void *late_identity_thread(void *arg) {
  clog lg = *(clog *)arg;
  clog_info(lg, "LATERECORD");
  return (void *)(uintptr_t)(clog__test_thread_identity_block() != NULL);
}

// A record logged after the module destructor ran (a later destructor of a
// statically linked program) gets an identity and allocates no cache that
// nothing would free.
TEST(thread_identity, a_record_after_the_module_destructor_allocates_nothing) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    forget_parent_fixtures_in_child();
    char verdict = 'x';
    clog lg = clog_create_from_file(path, CLOG_INFO, NULL, NULL);
    if (lg != CLOG_NULL) {
      clog__test_run_identity_fini();
      pthread_t t;
      void *got = (void *)1;
      if (pthread_create(&t, NULL, late_identity_thread, &lg) == 0) {
        pthread_join(t, &got);
        verdict = got == NULL ? '1' : '0';
      }
      clog_close(lg);
    }
    ssize_t w;
    do {
      w = write(pfd[1], &verdict, 1);
    } while (w < 0 && errno == EINTR);
    test_child_exit(0);
  }
  close(pfd[1]);
  char verdict = 0;
  if (pid > 0) {
    ssize_t r;
    do {
      r = read(pfd[0], &verdict, 1);
    } while (r < 0 && errno == EINTR);
  }
  close(pfd[0]);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  char buf[4096];
  read_file(path, buf, sizeof(buf));
  bool logged = strstr(buf, "LATERECORD") != NULL;
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(verdict, '1');
  REQUIRE_TRUE(logged);
}

// Logs a record that a full datagram socket refuses, so the loss marker after
// it blocks in its write while this thread holds the target's mutex.
static void *blocked_marker_thread(void *arg) {
  static char big[300000];
  memset(big, 'x', sizeof(big) - 1);
  clog_error(*(clog *)arg, "%s", big);
  return NULL;
}

// The exit drain only tries the lock of a target that owes a repair, so a
// thread blocked for ever in a write on it doesn't delay the exit; the 20 s
// budget is above the parent's 10 s bound.
TEST(exit_drain, a_blocked_writer_that_owes_a_repair_delays_no_exit) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    clog__test_set_exit_drain_budget_ms(20000);
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_DGRAM, 0, sv) != 0) test_child_exit(3);
    int sb = 4096;
    (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
    while (send(sv[0], "fill\n", 5, MSG_DONTWAIT) == 5) {
    }
    static clog lg;
    lg = clog_create_from_fd(sv[0], CLOG_INFO, NULL);
    if (lg == CLOG_NULL) test_child_exit(3);
    pthread_t t;
    if (pthread_create(&t, NULL, blocked_marker_thread, &lg) != 0)
      test_child_exit(4);
    usleep(300000);
    mark_child_ready(path);
    exit(0);
  }
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 10000, &status);
  bool ready = child_was_ready(dir);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
}

// A failed publish in a directory that also refuses the removal leaves the
// temporary output to the compressor, which removes it before its next
// compression and at close.
TEST(compression, a_temporary_output_left_behind_is_removed_later) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 1,
                            .max_rotated_files = 10,
                            .compress_rotated = true};
  clog lg = clog_create_from_file(path, CLOG_INFO, &rcfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  size_t fin0 = clog__test_compressions_finished();
  clog__test_force_gz_publish_failures(1);
  clog_info(lg, "FIRSTROTATION");
  for (long long deadline_ms = test_now_ms() + 20000;
       clog__test_compressions_finished() == fin0 &&
       test_now_ms() < deadline_ms;)
    usleep(1000);
  int tmp_after_failure = count_files_with_suffix(dir, ".gz.tmp");
  size_t fin1 = clog__test_compressions_finished();
  clog_info(lg, "SECONDROTATION");
  for (long long deadline_ms = test_now_ms() + 20000;
       clog__test_compressions_finished() == fin1 &&
       test_now_ms() < deadline_ms;)
    usleep(1000);
  int tmp_after_next = count_files_with_suffix(dir, ".gz.tmp");
  size_t fin2 = clog__test_compressions_finished();
  clog__test_force_gz_publish_failures(1);
  clog_info(lg, "THIRDROTATION");
  for (long long deadline_ms = test_now_ms() + 20000;
       clog__test_compressions_finished() == fin2 &&
       test_now_ms() < deadline_ms;)
    usleep(1000);
  int tmp_before_close = count_files_with_suffix(dir, ".gz.tmp");
  clog_close(lg);
  clog__test_force_gz_publish_failures(0);
  int tmp_after_close = count_files_with_suffix(dir, ".gz.tmp");
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(tmp_after_failure, 1);
  REQUIRE_EQ(tmp_after_next, 0);
  REQUIRE_EQ(tmp_before_close, 1);
  REQUIRE_EQ(tmp_after_close, 0);
}

// Runs a child whose async writer blocks writing to a pipe nobody reads, so
// the exit drain must leave the target, after which this file's destructor
// logs to it.
static void exit_with_blocked_writer_child(const char *path, size_t qsize,
                                           int records, bool flush,
                                           bool close_it) {
  forget_parent_fixtures_in_child();
  clog__test_set_exit_drain_budget_ms(300);
  int p[2];
  if (pipe(p) != 0 || !fill_pipe(p[0], p[1])) test_child_exit(3);
  clog_async_cfg acfg = {.queue_size = qsize,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, &acfg);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "BLOCKEDWRITERRECORD");
  g_after_drain_logger = lg;
  g_after_drain_close = close_it;
  g_after_drain_records = records;
  g_after_drain_flush = flush;
  mark_child_ready(path);
  exit(0);
}

// A target the drain left keeps taking records, so a destructor that logs to
// it returns at once; with its sends left off the record would take the
// synchronous fallback and wait for ever for the writer's mutex.
TEST(exit_drain, a_target_the_drain_leaves_keeps_taking_records) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_with_blocked_writer_child(path, 0, 1, false, false);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
}

// After the drain left a target, more records than a bounded queue holds, a
// flush or a close from a destructor never wait for the blocked writer; a
// record that a full bounded queue can't take is dropped.
TEST(exit_drain, a_left_target_never_waits_for_its_writer) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  bool ended[4] = {false, false, false, false};
  bool ready[4] = {false, false, false, false};
  int status[4] = {0, 0, 0, 0};
  for (int k = 0; k < 4; k++) {
    char dir[64];
    if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) continue;
    char path[256];
    snprintf(path, sizeof(path), "%s/app.log", dir);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) {
      if (k == 0)
        exit_with_blocked_writer_child(path, 2, 6, false, false);
      else if (k == 1)
        exit_with_blocked_writer_child(path, 0, 1, true, false);
      else
        exit_with_blocked_writer_child(path, k == 2 ? 0 : 2, 1, false, true);
    }
    ended[k] = pid > 0 && wait_child_bounded(pid, 20000, &status[k]);
    ready[k] = child_was_ready(dir);
    cleanup_dir(dir, "app.log");
  }
  for (int k = 0; k < 4; k++) {
    REQUIRE_TRUE(ready[k]);
    REQUIRE_TRUE(ended[k]);
    REQUIRE_TRUE(WIFEXITED(status[k]));
  }
}

// Logs to the child's logger every millisecond until g_loggers_stop is set, so
// calls are under way when the process exits. The pause keeps the main thread
// running under valgrind.
static void *log_forever_thread(void *arg) {
  clog lg = *(clog *)arg;
  while (!atomic_load(&g_loggers_stop)) {
    clog_info(lg, "LOGGINGTHROUGHTHEEXIT");
    usleep(1000);
  }
  for (;;) pause();
  return NULL;
}

// Runs a child like exit_with_blocked_writer_child() with four threads that
// keep logging through the exit and a close from this file's destructor.
static void exit_with_blocked_writer_and_loggers_child(const char *path,
                                                       size_t qsize) {
  forget_parent_fixtures_in_child();
  int p[2];
  if (pipe(p) != 0 || !fill_pipe(p[0], p[1])) test_child_exit(3);
  clog_async_cfg acfg = {.queue_size = qsize,
                         .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  static clog lg;
  lg = clog_create_from_fd(p[1], CLOG_INFO, &acfg);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "BLOCKEDWRITERRECORD");
  g_after_drain_logger = lg;
  g_after_drain_close = true;
  g_after_drain_records = 1;
  g_after_drain_flush = false;
  mark_child_ready(path);
  for (int i = 0; i < 4; i++) {
    pthread_t t;
    if (pthread_create(&t, NULL, log_forever_thread, &lg) == 0)
      pthread_detach(t);
  }
  usleep(100000);
  exit(0);
}

// Threads that keep logging find sends off and wait for the target's mutex
// only as long as the drain does, so a destructor's close, which waits until
// no call holds the handle, returns.
TEST(exit_drain, loggers_through_the_exit_do_not_hold_a_destructor_close) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  bool ended[2] = {false, false};
  bool ready[2] = {false, false};
  int status[2] = {0, 0};
  for (int k = 0; k < 2; k++) {
    char dir[64];
    if (make_cwd_tmpdir(dir, sizeof(dir)) != 0) continue;
    char path[256];
    snprintf(path, sizeof(path), "%s/app.log", dir);
    fflush(NULL);
    pid_t pid = fork();
    if (pid == 0) exit_with_blocked_writer_and_loggers_child(path, k ? 16 : 0);
    ended[k] = pid > 0 && wait_child_bounded(pid, 20000, &status[k]);
    ready[k] = child_was_ready(dir);
    cleanup_dir(dir, "app.log");
  }
  for (int k = 0; k < 2; k++) {
    REQUIRE_TRUE(ready[k]);
    REQUIRE_TRUE(ended[k]);
    REQUIRE_TRUE(WIFEXITED(status[k]));
  }
}

// Runs a child whose async output takes the first 30 bytes of the drain's
// flush and then has no room, leaving a fragment the drain can't close; this
// file's destructor then gives the room back and logs.
static void exit_with_full_output_child(const char *path) {
  forget_parent_fixtures_in_child();
  clog_async_cfg acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_create_from_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_NULL) test_child_exit(3);
  clog_info(lg, "DRAINEDRECORD %s", "xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  clog_info(lg, "SECONDDRAINEDRECORD");
  mark_child_ready(path);
  g_after_drain_logger = lg;
  g_after_drain_close = false;
  g_after_drain_room_back = true;
  clog__test_set_output_room(ENOSPC, 30, 0);
  exit(0);
}

// A destructor that logs once the output has room writes the owed newline and
// marker first, so its record doesn't land on the fragment the drain left.
TEST(exit_drain, a_record_after_the_drain_never_joins_a_fragment) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_with_full_output_child(path);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  bool ready = child_was_ready(dir);
  char buf[8192];
  read_file(path, buf, sizeof(buf));
  cleanup_dir(dir, "app.log");
  const char *marker = strstr(buf, "log record truncated: ");
  const char *after = strstr(buf, "AFTERDRAINRECORD");
  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(ended);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(count_joined_lines(buf), 0);
  REQUIRE_TRUE(marker != NULL);
  REQUIRE_TRUE(after != NULL);
  REQUIRE_TRUE(marker < after);
}

// A small record that doesn't fit behind a backlog filling the whole batch
// buffer is a loss that a marker names, never a "too large" placeholder.
TEST(output_without_room, a_record_behind_a_full_backlog_is_a_named_loss) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg,
                                     NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);
  size_t big_len = 60000;
  char *big = malloc(big_len + 1);
  if (big) {
    memset(big, 'b', big_len);
    big[big_len] = '\0';
    clog__test_set_output_room(ENOSPC, 0, 0);
    for (int i = 0; i < 320; i++) clog_info(lg, "BIG %03d %s", i, big);
    clog_flush(lg);
    clog__test_set_output_room(0, 0, 0);
    clog_info(lg, "AFTERBACKLOG");
    clog_flush(lg);
  }
  clog_close(lg);
  free(big);
  size_t cap = 32u << 20;
  char *buf = malloc(cap);
  bool too_large = true, named = false, after = false;
  if (buf) {
    read_file(path, buf, cap);
    too_large = strstr(buf, "too large to emit") != NULL;
    named = strstr(buf, "did not fit behind the undelivered batch") != NULL ||
            strstr(buf, "earlier losses (records=") != NULL;
    after = strstr(buf, "AFTERBACKLOG") != NULL;
    free(buf);
  }
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(big != NULL);
  REQUIRE_FALSE(too_large);
  REQUIRE_TRUE(named);
  REQUIRE_TRUE(after);
}

// The last flush of a closing logger gives up a cut continuation, rotates and
// offers its kept records to the new file in the same flush, so the close
// loses only the continuation.
TEST(output_without_room, a_rotation_gives_kept_records_another_attempt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg rcfg = {.size_rotation_enabled = true,
                            .max_file_size = 2000,
                            .max_rotated_files = 5};
  clog lg = clog_create_from_file_mp(path, CLOG_INFO, &rcfg,
                                     &g_undelivered_acfg, NULL);
  if (lg == CLOG_NULL) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_NULL);

  clog_info(lg, "KEPTRECORD %02d %s", 0, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  clog_flush(lg);
  struct stat first;
  size_t rec_len = stat(path, &first) == 0 ? (size_t)first.st_size : 200;
  for (int i = 1; i < 30; i++)
    clog_info(lg, "KEPTRECORD %02d %s", i, "yyyyyyyyyyyyyyyyyyyyyyyyyyyyyy");
  clog__test_set_output_room(EFBIG, rec_len * 10 + rec_len / 2, 2);
  clog_close(lg);
  clog__test_set_output_room(0, 0, 0);
  static char all[1 << 20];
  read_all_log_files(dir, all, sizeof(all));
  bool last = strstr(all, "KEPTRECORD 29") != NULL;
  bool closed_drop = strstr(all, "the logger closed") != NULL;
  bool marker = strstr(all, "undelivered bytes dropped") != NULL;
  int joined = count_joined_lines(all);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(last);
  REQUIRE_FALSE(closed_drop);
  REQUIRE_TRUE(marker);
  REQUIRE_EQ(joined, 0);
}

// macOS releases thread-local storage before key destructors run. A record
// logged then takes the identity cache back from the key's value instead of
// allocating a second cache that would leak the first.
TEST(thread_identity, a_cleared_pointer_takes_the_cache_back_from_its_key) {
  int fd = open("/dev/null", O_WRONLY);
  REQUIRE_GE(fd, 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  if (lg == CLOG_NULL) close(fd);
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "IDENTITYONE");
  const void *before = clog__test_thread_identity_block();
  clog__test_forget_thread_identity_pointer();
  clog_info(lg, "IDENTITYTWO");
  const void *after = clog__test_thread_identity_block();
  clog_close(lg);
  close(fd);
  REQUIRE_TRUE(before != NULL);
  REQUIRE_TRUE(after == before);
}

// Allocation-free fields and the idle writer

// A counting allocator: g_count_allocs counts every malloc, calloc and
// realloc, so a test can check that a record allocates nothing.
static size_t g_count_allocs = 0;
static void *_count_malloc(size_t sz) {
  g_count_allocs++;
  return malloc(sz);
}
// The frees of non-NULL pointers made through the counting allocator.
static size_t g_count_frees = 0;
static void _count_free(void *p) {
  if (p) g_count_frees++;
  free(p);
}
static void *_count_calloc(size_t n, size_t sz) {
  g_count_allocs++;
  return calloc(n, sz);
}
static void *_count_realloc(void *p, size_t sz) {
  g_count_allocs++;
  return realloc(p, sz);
}

// Once its buffers have grown, a synchronous record with fields allocates
// nothing, in logfmt and in JSON.
TEST(fields, a_record_with_fields_allocates_nothing) {
  ccol_memmgmt_procs procs = {.malloc = _count_malloc,
                              .free = _count_free,
                              .calloc = _count_calloc,
                              .realloc = _count_realloc};
  const clog_format formats[] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON};
  size_t per_format[2] = {1, 1};
  for (size_t fi = 0; fi < 2; fi++) {
    int fd = open("/dev/null", O_WRONLY);
    REQUIRE_GE(fd, 0);
    clog lg = clog_create_from_fd_mp(fd, CLOG_INFO, NULL, &procs);
    if (lg == CLOG_NULL) close(fd);
    REQUIRE_NE(lg, CLOG_NULL);
    (void)clog_set_format(lg, formats[fi]);
    (void)clog_set_field(lg, "service", "auth");
    (void)clog_set_field(lg, "env", "prod");
    (void)clog_set_field(lg, "region", "eu-west");
    for (int i = 0; i < 10; i++) clog_info(lg, "warm up %d", i);
    g_count_allocs = 0;
    for (int i = 0; i < 100; i++) clog_info(lg, "record %d", i);
    per_format[fi] = g_count_allocs;
    clog_close(lg);
    close(fd);
  }
  REQUIRE_EQ(per_format[0], (size_t)0);
  REQUIRE_EQ(per_format[1], (size_t)0);
}

TEST(fields, fields_come_out_in_the_order_they_were_set) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, NULL);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  (void)clog_set_field(lg, "first", "1");
  (void)clog_set_field(lg, "second", "2");
  (void)clog_set_field(lg, "third", "3");
  (void)clog_set_field(lg, "first", "one");
  clog child = clog_derive(lg);
  clog_info(lg, "ORDERPARENT");
  if (child != CLOG_NULL) clog_info(child, "ORDERCHILD");
  if (child != CLOG_NULL) clog_close(child);
  char out[4096];
  drain_pipe(lg, p[0], p[1], out, sizeof(out));
  char *a = strstr(out, " first=one");
  char *b = strstr(out, " second=2");
  char *c = strstr(out, " third=3");
  char *child_line = strchr(out, '\n');
  char *ca = child_line ? strstr(child_line, " first=one") : NULL;
  char *cb = child_line ? strstr(child_line, " second=2") : NULL;
  char *cc = child_line ? strstr(child_line, " third=3") : NULL;
  REQUIRE_NE(child, CLOG_NULL);
  REQUIRE_TRUE(a != NULL && b != NULL && c != NULL);
  REQUIRE_TRUE(a < b && b < c);
  REQUIRE_TRUE(ca != NULL && cb != NULL && cc != NULL);
  REQUIRE_TRUE(ca < cb && cb < cc);
}

// A writer with nothing to deliver sleeps until the next record, so its count
// of timed waits with no job stays at zero however short the interval; a
// record after the idle time still goes out within the interval.
TEST(async, an_idle_writer_thread_does_not_wake) {
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  clog_async_cfg acfg = {.flush_interval_us = 5000};
  clog lg = clog_create_from_fd(p[1], CLOG_INFO, &acfg);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "BEFOREIDLE");
  clog_flush(lg);
  usleep(20000);
  size_t before = clog__test_writer_timeout_wakeups(lg);
  usleep(200000);
  size_t after = clog__test_writer_timeout_wakeups(lg);

  clog_info(lg, "AFTERIDLE");
  char out[4096];
  size_t got = 0;
  int fl = fcntl(p[0], F_GETFL);
  (void)fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
  bool seen = false;
  for (long long deadline_ms = test_now_ms() + 20000;
       !seen && test_now_ms() < deadline_ms;) {
    ssize_t r = read(p[0], out + got, sizeof(out) - 1 - got);
    if (r > 0) {
      got += (size_t)r;
      out[got] = '\0';
      seen = strstr(out, "AFTERIDLE") != NULL;
    } else {
      usleep(1000);
    }
  }
  clog_close(lg);
  close(p[0]);
  close(p[1]);
  REQUIRE_EQ(after - before, (size_t)0);
  REQUIRE_TRUE(seen);
}

// A record whose envelope can't be allocated is written directly; when that
// write and its marker fail, the debt wakes the idle writer, which pays it
// within the interval.
TEST(async, a_repair_left_by_a_direct_write_wakes_the_idle_writer) {
  ccol_memmgmt_procs procs = {
      .malloc = _fail_next_calloc_malloc,
      .free = _fail_next_calloc_free,
      .calloc = _fail_next_calloc_calloc,
      .realloc = _fail_next_calloc_realloc,
  };
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  clog_async_cfg acfg = {.flush_interval_us = 5000};
  clog lg = clog_create_from_fd_mp(p[1], CLOG_INFO, &acfg, &procs);
  if (lg == CLOG_NULL) {
    close(p[0]);
    close(p[1]);
  }
  REQUIRE_NE(lg, CLOG_NULL);
  clog_info(lg, "BEFOREIDLE");
  clog_flush(lg);
  usleep(20000);
  g_fail_next_calloc = 1;
  clog__test_force_write_errors(ENOSPC, 2);
  clog_info(lg, "LOSTRECORD");
  clog__test_force_write_errors(0, 0);
  g_fail_next_calloc = 0;

  char out[8192];
  size_t got = 0;
  int fl = fcntl(p[0], F_GETFL);
  (void)fcntl(p[0], F_SETFL, fl | O_NONBLOCK);
  bool seen = false;
  for (long long deadline_ms = test_now_ms() + 20000;
       !seen && test_now_ms() < deadline_ms;) {
    ssize_t r = read(p[0], out + got, sizeof(out) - 1 - got);
    if (r > 0) {
      got += (size_t)r;
      out[got] = '\0';
      seen = strstr(out, "log record truncated") != NULL;
    } else {
      usleep(1000);
    }
  }
  clog_close(lg);
  close(p[0]);
  close(p[1]);
  REQUIRE_TRUE(seen);
}

// Without MSG_NOSIGNAL a socket sink takes the pipe path, and the bounded path
// of CLOG_FATAL must still send a message socket each record in one write
// instead of PIPE_BUF pieces.
TEST(fatal, a_long_fatal_record_reaches_a_datagram_socket_whole) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int sv[2];
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_DGRAM, 0, sv), 0);
  int big = 1 << 20;
  (void)setsockopt(sv[0], SOL_SOCKET, SO_SNDBUF, &big, sizeof(big));
  (void)setsockopt(sv[1], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    close(sv[1]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    clog__test_classify_sockets_as_pipes(true);
    clog lg = clog_create_from_fd(sv[0], CLOG_INFO, NULL);
    if (lg == CLOG_NULL) test_child_exit(3);
    static char msg[3 * PIPE_BUF];
    memset(msg, 'm', sizeof(msg) - 1);
    memcpy(msg + sizeof(msg) - 9, "ENDOFMSG", 8);
    clog_fatal(lg, "%s", msg);
    test_child_exit(4);
  }
  close(sv[0]);
  int status = 0;
  bool ended = pid > 0 && wait_child_bounded(pid, 20000, &status);
  static char dgram[1 << 16];
  ssize_t n = recv(sv[1], dgram, sizeof(dgram) - 1, MSG_DONTWAIT);
  close(sv[1]);
  if (n > 0) dgram[n] = '\0';
  REQUIRE_NE(pid, -1);
  REQUIRE_TRUE(ended);
  REQUIRE_GT(n, (ssize_t)(2 * PIPE_BUF));
  REQUIRE_NE(strstr(dgram, "ENDOFMSG"), NULL);
}

// The handle that _close_after_the_release() closes in a child, and the
// descriptor its stderr goes to; -1 everywhere else.
static clog g_stale_close_handle = CLOG_NULL;
static int g_stale_close_report_fd = -1;

// Closes g_stale_close_handle after the module destructor released the handle
// table, with stderr on g_stale_close_report_fd, so the test can read the
// stale-handle error it must reach.
__attribute__((destructor(101))) static void _close_after_the_release(void) {
  if (g_stale_close_report_fd < 0) return;
  dup2(g_stale_close_report_fd, STDERR_FILENO);
  clog_close(g_stale_close_handle);
  test_child_exit(0);
}

TEST(lifecycle, a_close_after_the_table_is_released_is_the_stale_handle_error) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  EXIT_TEST_SKIP_WITHOUT_PROGRAM_DESTRUCTORS();
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    close(p[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    int nfd = open("/dev/null", O_WRONLY);
    clog first =
        nfd >= 0 ? clog_create_from_fd(nfd, CLOG_INFO, NULL) : CLOG_NULL;
    if (first == CLOG_NULL) test_child_exit(5);
    clog_close(first);
    close(nfd);
    if (g_deferred_release_a != CLOG_NULL) clog_close(g_deferred_release_a);
    if (g_deferred_release_b != CLOG_NULL) clog_close(g_deferred_release_b);
    forget_parent_fixtures_in_child();
    g_stale_close_handle = first;
    g_stale_close_report_fd = p[1];
    exit(0);
  }
  close(p[1]);
  char out[2048];
  size_t got = 0;
  for (;;) {
    ssize_t r = pid > 0 ? read(p[0], out + got, sizeof(out) - 1 - got) : -1;
    if (r <= 0) break;
    got += (size_t)r;
    if (got >= sizeof(out) - 1) break;
  }
  out[got] = '\0';
  close(p[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_NE(strstr(out,
                    "clog_close: clog handle is invalid, stale, or "
                    "already closed"),
             NULL);
  REQUIRE_TRUE(WIFSIGNALED(status) || WIFEXITED(status));
}

// The flush request the exit drain queues comes from the target's allocator
// and goes back to it; a bounded queue makes no allocation per send.
TEST(exit_drain, the_flush_request_uses_the_allocator_of_the_target) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  int p[2];
  REQUIRE_EQ(pipe(p), 0);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    forget_parent_fixtures_in_child();
    close(p[0]);
    ccol_memmgmt_procs procs = {.malloc = _count_malloc,
                                .free = _count_free,
                                .calloc = _count_calloc,
                                .realloc = _count_realloc};
    int fd = open("/dev/null", O_WRONLY);
    clog_async_cfg acfg = {.queue_size = 16, .flush_interval_us = UINT64_MAX};
    clog lg = fd >= 0 ? clog_create_from_fd_mp(fd, CLOG_INFO, &acfg, &procs)
                      : CLOG_NULL;
    if (lg == CLOG_NULL) test_child_exit(3);
    clog_info(lg, "BEFOREDRAIN");
    clog_flush(lg);
    size_t a0 = g_count_allocs, f0 = g_count_frees;
    clog__test_run_exit_drain();
    size_t counts[2] = {g_count_allocs - a0, g_count_frees - f0};
    if (write(p[1], counts, sizeof(counts)) != (ssize_t)sizeof(counts))
      test_child_exit(4);
    test_child_exit(0);
  }
  close(p[1]);
  size_t counts[2] = {0, 0};
  ssize_t got = pid > 0 ? read(p[0], counts, sizeof(counts)) : -1;
  close(p[0]);
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);
  REQUIRE_NE(pid, -1);
  REQUIRE_EQ(got, (ssize_t)sizeof(counts));
  REQUIRE_GE(counts[0], (size_t)1);
  REQUIRE_EQ(counts[0], counts[1]);
}
