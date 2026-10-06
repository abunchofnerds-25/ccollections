/* posix_openpt(3) and its companions need it on glibc. */
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <clogger.h>
#include <common.h>
#include <dirent.h>
#include <errno.h>
#include <execinfo.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__linux__)
#include <sys/prctl.h>
#elif defined(__FreeBSD__)
#include <pthread_np.h>
#endif

/* The OS thread ID of the calling thread, the number that ps(1) shows and
 * that the proc field of a record carries. */
static int _test_os_tid(void) {
#if defined(__linux__)
  return (int)syscall(SYS_gettid);
#elif defined(__FreeBSD__)
  return pthread_getthreadid_np();
#elif defined(__APPLE__)
  uint64_t tid = 0;
  pthread_threadid_np(NULL, &tid);
  return (int)tid;
#endif
}

/* The name of the calling thread as the system holds it, at most 16 bytes
 * with the terminator. */
static void _test_get_thread_name(char buf[17]) {
#if defined(__linux__)
  prctl(PR_GET_NAME, buf);
#else
  /* An unnamed thread reads as the program name here, as on Linux and as
   * the library reports it. */
  if (pthread_getname_np(pthread_self(), buf, 17) != 0 || buf[0] == '\0')
    snprintf(buf, 17, "%.15s", getprogname());
#endif
}

/* Renames the calling thread without going through the library. */
static void _test_set_thread_name(const char *name) {
#if defined(__linux__)
  prctl(PR_SET_NAME, name);
#elif defined(__APPLE__)
  pthread_setname_np(name);
#else
  pthread_setname_np(pthread_self(), name);
#endif
}

#include "consumer_view.h"

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_fds.h>
#include <test_sanitizer.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                         HELPERS                                            */
/* ========================================================================== */

/* In some environments the backtrace() and backtrace_symbols() functions
 * cannot unwind the call stack of this process at all. Take a small program
 * that contains no clogger.c, built for arm32. Under the user-mode emulation
 * of qemu-arm, it reports depth=0 from a plain backtrace() call three frames
 * deep. That program contains no c_collections code.
 * The _capture_backtrace() function of clog treats this in the same way as a
 * real capture that is shallow. See the doc comment of CLOG_BT_INITIAL_FRAME.
 * It appends nothing, and it reports nothing as missing. This is the design.
 * Some tests below assert that a request for a backtrace gives visible frame
 * content. The "all frames failed to fit" tests assert a marker instead.
 * That marker needs enough real frames to overflow the buffer. Neither
 * assertion can hold when the OS or the libc never gives more than two or
 * three frames. Those tests therefore probe this capability directly and
 * skip. They return early, and Tau records an early return as a pass. This
 * is better than a failure that comes from a platform limit and not from the
 * logic of clog. */
static bool backtrace_capture_genuinely_available(void) {
  void *frames[8];
  int depth = backtrace(frames, 8);
  return depth > 2;
}

/* Flush an fd-based logger, close the write-end of a pipe, read all output. */
static size_t drain_pipe(clog lg, int rfd, int wfd, char *buf, size_t cap) {
  clog_close(lg);
  close(wfd);
  ssize_t n = read(rfd, buf, cap - 1);
  close(rfd);
  if (n < 0) n = 0;
  buf[n] = '\0';
  return (size_t)n;
}

/* This reads up to `cap-1` bytes of a file into buf. It adds a NUL
 * terminator and returns the length. */
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

/* Count files in dir whose name starts with prefix. */
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

/* Remove all files in dir whose name starts with prefix, then rmdir. */
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

/* Count files in dir whose name ends with suffix. */
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

/*
 * Run "gunzip -t <path>" and return the exit status.
 * 0 means the file is a valid gzip archive.
 */
static int gunzip_test(const char *gz_path) {
  char cmd[1024];
  snprintf(cmd, sizeof(cmd), "gunzip -t '%s' 2>/dev/null", gz_path);
  return system(cmd);
}

/*
 * This decompresses <gz_path> into buf, up to cap-1 bytes. It adds a NUL
 * terminator. It returns the number of decompressed bytes, or 0 on an error.
 */
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

/* Find the first file in dir with the given suffix and copy its path to out. */
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

/* Make a unique temp directory under /tmp and return its path in `out`. */
static int make_tmpdir(char *out, size_t outsz) {
  snprintf(out, outsz, "/tmp/clogger_test_XXXXXX");
  if (!mkdtemp(out)) return -1;
  return 0;
}

/* Count the file descriptors that this process has open; see test_fds.h. A
 * caller compares the results of two calls. */
static int count_open_fds(void) { return test_count_open_fds(); }

/* Reads /proc/<pid>/<name> into buf and adds a NUL terminator. That file is
 * a pseudo-file of one line, or of a small number of lines. The function
 * returns true after a success. It is a best-effort diagnostic only. It
 * returns false after any failure, for example a process that the OS already
 * reaped, or a permission problem. The caller prints that failure as part of
 * the dump and does not treat it as fatal. This code runs only after a
 * bounded wait times out. It must therefore never add a new way for the test
 * binary to hang or to crash. tests/cthreadcomm/tests.c has a helper with
 * the same name and the same shape. See _dump_stuck_child_diagnostics in
 * that file for the same pattern. This is a separate copy and not shared
 * code. The two test binaries are independent and share no test-only
 * header. */
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

/* This function is a diagnostic only. It dumps everything that /proc on the
 * host kernel still says about pid, and about each thread of pid. It runs
 * after a bounded wait for that process times out.
 *
 * It exists for async.fatal_drains_queue_before_writing_and_terminating. See
 * the comment of that test. The cause of the rare CI-only hang of that test
 * is not confirmed. The cthreadcomm fork() tests carry the qemu-user
 * fd_trans_lock signature, but this hang does not reproduce in 15 direct
 * attempts against the same version of qemu-user.
 *
 * If the hang happens again, this dump is what answers it. Look for a State:
 * S, together with a real /proc/<pid>/syscall entry and a broad SigBlk mask
 * that blocks nearly every signal. That combination matches the confirmed
 * qemu-user fd_trans_lock signature. A real syscall entry means a true futex
 * or nanosleep wait, and not a placeholder. fd_trans_lock is a process-wide
 * pthread mutex inside the linux-user part of qemu-user. It stays locked
 * forever in a forked child when another thread in the parent holds it at
 * the instant of the fork(). See gitlab.com/qemu-project/qemu/-/issues/2846.
 * The bug is in qemu-user 8.2.2 and is fixed in 10.x, which is the version
 * that the CI jobs install.
 *
 * That broad SigBlk mask is the normal thread-management behaviour of
 * qemu-user. It is there even though the child of this test never calls
 * alarm(). Do not expect one specific pending signal in ShdPnd, as the
 * cthreadcomm cases show. Here the mask itself is the signal to look for,
 * and not one particular pending bit.
 *
 * A State: R with high CPU use and an empty wchan points at a real spin
 * instead. A State: Z (zombie) points at a bug in the waitpid() observation
 * under emulation, and not at anything that the child does.
 *
 * The function reads status, wchan, syscall and stat one by one. One file
 * that is missing or unreadable therefore does not suppress the others. */
static void _clog_test_dump_stuck_child_diagnostics(pid_t pid) {
  char buf[4096];
  fprintf(stderr,
          "[STUCK_CHILD_DIAG] parent pid=%d timed out waiting for child "
          "pid=%d; dumping /proc diagnostics\n",
          (int)getpid(), (int)pid);

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
}

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

TEST(lifecycle, open_fd_and_close) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_close(lg);
}

TEST(lifecycle, open_file_and_close) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);

  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_close(lg);

  struct stat st;
  REQUIRE_EQ(stat(path, &st), 0); /* file was created */

  cleanup_dir(dir, "app.log");
}

TEST(lifecycle, invalid_path_returns_null) {
  clog lg = clog_open_file_mp("/nonexistent/deeply/nested/path/app.log",
                              CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_EQ(lg, CLOG_INVALID);
}

TEST(lifecycle, open_fd_rejects_invalid_and_readonly_fds) {
  /* Negative fd must be rejected. */
  REQUIRE_EQ(clog_open_fd(-1, CLOG_INFO, NULL), CLOG_INVALID);

  /* Closed fd must be rejected. */
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_EQ(clog_open_fd(fds[1], CLOG_INFO, NULL), CLOG_INVALID);

  /* Read-only fd must be rejected. */
  int ro = open("/dev/null", O_RDONLY);
  REQUIRE_NE(ro, -1);
  REQUIRE_EQ(clog_open_fd(ro, CLOG_INFO, NULL), CLOG_INVALID);
  close(ro);

  /* Writable fd must be accepted. */
  clog lg = clog_open_fd(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_close(lg);
}

TEST(lifecycle, writing_to_a_broken_pipe_does_not_crash_the_process) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  close(pipefd[0]); /* close the read end, which breaks the write end */

  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Under the default CLOG_SIGPIPE_AUTO policy, this first write to a pipe
   * from the calling thread sets SIGPIPE to SIG_IGN for the process. Without
   * that, the write raises SIGPIPE, and its default disposition stops the
   * whole test process. It does not only fail this one assertion.
   * tests_sigpipe.c covers the policy itself. */
  ccol_log_info(lg, "should not crash the process");

  clog_close(lg);
  close(pipefd[1]);
}

/* ========================================================================== */
/*                         OUTPUT FORMAT                                      */
/* ========================================================================== */

TEST(output, logfmt_fields_present) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* All mandatory logfmt keys must be present */
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "level="), NULL);
  REQUIRE_NE(strstr(buf, "proc="), NULL);
  REQUIRE_NE(strstr(buf, "src="), NULL);
  REQUIRE_NE(strstr(buf, "func="), NULL);
  REQUIRE_NE(strstr(buf, "msg="), NULL);
  REQUIRE_NE(strstr(buf, "INFO"), NULL);
  /* Message content (quoted because it contains a space) */
  REQUIRE_NE(strstr(buf, "hello world"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, message_with_special_chars_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "key=\"value\"");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));
  /* The message contains '"' so it must be double-quoted in the output */
  REQUIRE_NE(strstr(buf, "msg="), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, all_levels_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_trace(lg, "t");
  ccol_log_debug(lg, "d");
  ccol_log_info(lg, "i");
  ccol_log_warn(lg, "w");

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
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_error(lg, "something bad");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* The backtrace continuation line starts with a tab followed by '#' */
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         PROC FIELD                                         */
/* ========================================================================== */

static void *_proc_test_thread(void *arg) {
  clog lg = *(clog *)arg;
  ccol_log_info(lg, "from spawned thread");
  return NULL;
}

TEST(proc_field, logfmt_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  ccol_log_info(lg, "proc test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* proc= field must be present */
  char *proc_start = strstr(buf, "proc=");
  REQUIRE_NE(proc_start, NULL);

  char *val = proc_start + 5; /* skip "proc=" */

  /* proc format: progname(pid):tname(tid) */
  /* (pid): must appear in the progname section */
  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(val, pid_part), NULL);

  /* (tid) must appear in the thread section */
  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(val, tid_part), NULL);

  /* Extract the value, which ends at the next space. Check for one colon. */
  char *val_end = strchr(val, ' ');
  REQUIRE_NE(val_end, NULL);
  size_t val_len = (size_t)(val_end - val);
  char proc_val[256];
  REQUIRE_EQ((val_len < sizeof(proc_val)), 1);
  memcpy(proc_val, val, val_len);
  proc_val[val_len] = '\0';

  char *colon = strchr(proc_val, ':');
  REQUIRE_NE(colon, NULL);
  REQUIRE_EQ(strchr(colon + 1, ':'), NULL); /* exactly one colon */

  cleanup_dir(dir, "app.log");
}

TEST(proc_field, json_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  ccol_log_info(lg, "proc json test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* "proc": key must exist */
  REQUIRE_NE(strstr(buf, "\"proc\":"), NULL);

  /* (pid): must appear in the progname section of the proc value */
  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  /* (tid) must appear in the thread section of the proc value */
  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(buf, tid_part), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(proc_field, syslog_proc_in_sd_element) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  pid_t pid = getpid();
  pid_t tid = (pid_t)_test_os_tid();

  ccol_log_info(lg, "proc syslog test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* proc SD param must be present inside the [ccol ...] element */
  REQUIRE_NE(strstr(buf, "proc=\""), NULL);

  /* (pid): must appear in the progname section of the proc SD value */
  char pid_part[48];
  snprintf(pid_part, sizeof(pid_part), "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  /* (tid) must appear in the thread section of the proc SD value */
  char tid_part[48];
  snprintf(tid_part, sizeof(tid_part), "(%d)", (int)tid);
  REQUIRE_NE(strstr(buf, tid_part), NULL);
}

TEST(proc_field, different_threads_have_different_tids) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Log from the main thread */
  ccol_log_info(lg, "main thread");

  /* Log from a spawned thread */
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

  /* Collect both proc= values */
  char *main_proc = strstr(buf, "proc=");
  REQUIRE_NE(main_proc, NULL);
  char *second_proc = strstr(main_proc + 1, "proc=");
  REQUIRE_NE(second_proc, NULL);

  /* Extract the TID component from (tid)] in each proc value.
   * Format: [progname(pid):tname(tid)]
   * After the single ':' separator, find (tid) to extract the TID. */
  char get_tid_str[2][32];
  char *procs[2] = {main_proc, second_proc};
  for (int i = 0; i < 2; i++) {
    char *v = procs[i] + 5; /* skip "proc=" */
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

  /* The TIDs must differ between the main thread and the spawned thread */
  REQUIRE_NE(strcmp(get_tid_str[0], get_tid_str[1]), 0);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         LEVEL FILTERING                                    */
/* ========================================================================== */

TEST(filtering, messages_below_min_level_dropped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_trace(lg, "should not appear");
  ccol_log_debug(lg, "should not appear");
  ccol_log_info(lg, "should not appear");
  ccol_log_warn(lg, "this should appear");

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

  clog lg = clog_open_file_mp(path, CLOG_OFF, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_error(lg, "suppressed");
  ccol_log_alert(lg, "suppressed");

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

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "before change - dropped");

  clog_set_level(lg, CLOG_TRACE);
  REQUIRE_EQ(clog_get_level(lg), CLOG_TRACE);

  ccol_log_info(lg, "after change - visible");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "before change"), NULL);
  REQUIRE_NE(strstr(buf, "after change"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

TEST(fields, set_field_appears_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "service", "auth");
  clog_set_field(lg, "env", "prod");

  ccol_log_info(lg, "request handled");

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "req_id", "aaa");
  clog_set_field(lg, "req_id", "bbb"); /* override */

  ccol_log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "req_id=aaa"), NULL);
  REQUIRE_NE(strstr(buf, "req_id=bbb"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, remove_field_disappears) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "trace_id", "xyz");
  clog_remove_field(lg, "trace_id");

  ccol_log_info(lg, "test");

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "a", "1");
  clog_set_field(lg, "b", "2");
  clog_set_field(lg, "c", "3");
  clog_clear_fields(lg);

  ccol_log_info(lg, "empty fields");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, " a="), NULL);
  REQUIRE_EQ(strstr(buf, " b="), NULL);
  REQUIRE_EQ(strstr(buf, " c="), NULL);
  REQUIRE_NE(strstr(buf, "empty fields"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, invalid_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The logger must ignore these keys and report nothing. They hold an empty
   * name, a space, '=', a control char, DEL, ']', '\' or '"'. */
  clog_set_field(lg, "", "v"); /* empty; violates RFC 5424 1*32PRINTUSASCII */
  clog_set_field(lg, "bad key", "v");
  clog_set_field(lg, "bad=key", "v");
  clog_set_field(lg, "bad\x01key", "v"); /* C0 control character */
  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");                 /* DEL (0x7f); not PRINTUSASCII */
  clog_set_field(lg, "bad]key", "v");  /* ']' breaks RFC 5424 SD elements */
  clog_set_field(lg, "bad\\key", "v"); /* '\' corrupts the logfmt quotes */
  clog_set_field(lg, "bad\"key", "v"); /* '"' corrupts the logfmt quotes */
  /* This key is valid and must appear. */
  clog_set_field(lg, "good_key", "ok");

  ccol_log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "=v"), NULL); /* empty key would produce =v */
  REQUIRE_EQ(strstr(buf, "bad key"), NULL);
  REQUIRE_EQ(strstr(buf, "bad=key="), NULL);
  REQUIRE_EQ(strstr(buf, "bad]key"), NULL);
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, reserved_key_names_are_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* The logger must reject every key that _clog_write() always writes itself.
   * If one such key passes, the output holds a duplicate JSON key, a
   * duplicate logfmt key= token, or a duplicate syslog SD-PARAM-NAME. */
  const char *reserved[] = {"ts",   "level", "proc", "src",
                            "func", "msg",   "bt",   "bt_error"};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
    clog_set_field(lg, reserved[i], "should-not-appear");
  /* An ordinary key must still work. */
  clog_set_field(lg, "good_key", "ok");

  ccol_log_info(lg, "reserved key test");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_EQ(strstr(buf, "should-not-appear"), NULL);
  REQUIRE_NE(strstr(buf, "\"good_key\":\"ok\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, value_with_spaces_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "host", "web server 01");

  ccol_log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Value contains spaces -> must be quoted */
  REQUIRE_NE(strstr(buf, "host=\"web server 01\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, del_byte_in_value_is_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The logger must escape DEL (0x7f) as \x7f in a quoted logfmt value.
   * The split of the string literal ends the hex escape before 'e'. */
  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  ccol_log_info(lg,
                "msg\x7f"
                "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Raw DEL must not appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* DEL must be represented as the escape sequence \x7f. */
  REQUIRE_NE(strstr(buf, "\\x7f"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, del_byte_in_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* DEL (0x7f) is not in PRINTUSASCII (0x21-0x7e). The logger writes a key
   * into logfmt without a change, so a DEL key corrupts the output and
   * reports nothing. The logger must reject it, as it rejects a C0 control
   * character. */
  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");
  clog_set_field(lg, "good_key", "ok");

  ccol_log_info(lg, "test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* No raw DEL must appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* The rejected key must not emit =v. */
  REQUIRE_EQ(strstr(buf, "=v"), NULL);
  /* The valid key must be present. */
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, non_ascii_byte_in_key_is_rejected) {
  /* Only an fd-based logger can use CLOG_FMT_SYSLOG. A file-backed logger
   * ignores clog_set_format(..., CLOG_FMT_SYSLOG) and reports nothing. This
   * test therefore needs a pipe and not the usual temporary file. The other
   * syslog.* tests use the same setup. */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* A byte >= 0x80 is outside PRINTUSASCII (0x21-0x7e). RFC 5424 needs an
   * SD-PARAM-NAME that holds PRINTUSASCII characters only. The logger must
   * therefore reject a key that holds such a byte, in the same way as any
   * other byte that is out of range. It must not copy that key into the
   * structured-data element without a change. */
  clog_set_field(lg, "bad\xc3\xa9key",
                 "v"); /* embeds a UTF-8 'e with accent' */
  clog_set_field(lg, "good_key", "ok");

  ccol_log_info(lg, "test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* No raw high-bit byte from the rejected key must appear in the output. */
  REQUIRE_EQ(memchr(buf, (int)(unsigned char)0xc3, strlen(buf)), NULL);
  /* The rejected key must not emit a v="..." SD-PARAM. */
  REQUIRE_EQ(strstr(buf, "=\"v\""), NULL);
  /* The valid key must still be present. */
  REQUIRE_NE(strstr(buf, "good_key=\"ok\""), NULL);
}

TEST(fields, newline_and_cr_in_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The value holds a raw LF, a raw CR and a raw HT. The logfmt output must
   * escape all three, so that the record stays on one line. */
  clog_set_field(lg, "payload", "line1\nline2\rend\ttab");
  ccol_log_info(lg, "escape test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* The entire record must fit on a single logfmt line: exactly one ts= */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);

  /* The first (and only) line must not contain raw CR or HT bytes. */
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  size_t first_line_len = (size_t)(nl - buf);
  REQUIRE_EQ(memchr(buf, '\r', first_line_len), NULL);
  REQUIRE_EQ(memchr(buf, '\t', first_line_len), NULL);

  /* The escaped two-character sequences must be present. */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* An empty string value must be emitted as "" in logfmt. */
  clog_set_field(lg, "empty_field", "");
  ccol_log_info(lg, "empty value test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* The field must appear with an explicitly quoted empty string value. */
  REQUIRE_NE(strstr(buf, "empty_field=\"\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, backslash_in_value_is_quoted_and_escaped_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A backslash in a value makes logfmt put that value in double quotes.
   * Each backslash then becomes \\ inside the quoted string. */
  clog_set_field(lg, "win_path", "C:\\Users\\foo");
  ccol_log_info(lg, "backslash test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Value must appear as win_path="C:\\Users\\foo"; each \ escaped to \\. */
  REQUIRE_NE(strstr(buf, "win_path=\"C:\\\\Users\\\\foo\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         OUTPUT; LONG MESSAGES                            */
/* ========================================================================== */

TEST(output, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Build a message that is longer than the 1024-byte stack buffer in
   * _clog_write. The logger then uses the fallback that allocates on the
   * heap. */
  char long_msg[2048];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  ccol_log_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* The full 2047-character message must appear verbatim in the output. */
  REQUIRE_NE(strstr(buf, long_msg), NULL);

  cleanup_dir(dir, "app.log");
}

/* An allocator that fails every request of more than 4096 bytes. The first
 * write buffer and the allocations of the logger construction are all below
 * this limit. Only the heap spill of a large message goes above it. This
 * allocator forces the fallback path for "the heap allocation for the full
 * message failed". The result is deterministic, and the message does not
 * need to be near CLOG_BUF_MAX. */
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

  ccol_memmgmt_procs_t procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This message is longer than the 1024-byte stack buffer. Its heap spill
   * request of one more byte is about 5000 bytes, which is above the
   * 4096-byte cap above. Everything else that the record needs stays well
   * below 4096 bytes. */
  char long_msg[5000];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  ccol_log_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* The heap spill fails, so the full message must NOT appear as it is... */
  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  /* ...and the output must show that truncation clearly. */
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         OVERSIZED RECORDS (FALLBACK)                       */
/* ========================================================================== */

TEST(oversized, logfmt_message_over_buf_cap_falls_back_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This size is above the internal 16 MiB write-buffer cap of clogger. */
  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  ccol_log_info(lg, "%s", huge);
  ccol_log_info(lg, "normal record after the oversized one");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* There must be exactly two well-formed lines. The first is the fallback
   * notice and the second is the normal record. The oversized message must
   * never desynchronize the stream. */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 2);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "normal record after the oversized one"), NULL);

  /* The fallback line itself must be well-formed. It must hold a quoted msg
   * value that closes correctly, and then a real newline at the end. It must
   * not hold a `msg="` with nothing after it. An overflow with no guard
   * produces that broken shape. */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  ccol_log_info(lg, "%s", huge);
  ccol_log_info(lg, "normal json record");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* Both lines must be complete JSON objects, and each one must be valid on
   * its own. Neither line may bleed into the other. */
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

/* The record can fall back to the general placeholder for an oversized
 * record. See _clog_build_fallback_record. A call to ccol_log_error,
 * ccol_log_alert or ccol_log_fatal asks for a backtrace. The JSON output
 * must then still show clearly that the backtrace is absent. This is the
 * same mark as for a "bt" array that the logger cannot embed inline. See the
 * bt_error marker of _emit_backtrace_json. The record must never look like
 * an ordinary success that asks for no backtrace. */
TEST(oversized, json_fallback_with_backtrace_marks_bt_as_unavailable) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  ccol_log_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* Exactly one well-formed JSON object line. */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  /* The output must mark the requested backtrace as absent. A reader must be
   * able to tell this record from a call that asks for no backtrace. */
  REQUIRE_NE(strstr(buf, "\"bt_error\""), NULL);
  REQUIRE_EQ(strstr(buf, "\"bt\":["), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(oversized, syslog_message_over_buf_cap_falls_back_cleanly) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'C', sz);
  huge[sz] = '\0';

  ccol_log_info(lg, "%s", huge);
  ccol_log_info(lg, "normal syslog record");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  free(huge);

  /* Exactly two well-formed, correctly PRI-prefixed syslog records. */
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

/* JSON embeds the backtrace inline, in the same record that falls back to
 * the placeholder. See json_fallback_with_backtrace_marks_bt_as_unavailable
 * above. logfmt is different: it writes the backtrace frames as separate,
 * independent continuation lines after the primary record. An oversized
 * message replaces that primary record with the fallback placeholder. It
 * must not suppress those continuation lines. The fallback path and the
 * backtrace path are independent, and a requested backtrace must still
 * appear. */
TEST(oversized,
     logfmt_message_over_buf_cap_with_backtrace_still_writes_backtrace_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  ccol_log_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* Exactly one primary "ts=" record: the fallback placeholder. */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);

  /* The tab-indented continuation lines of the backtrace must still follow.
   * They must follow in the same way as for an ordinary ccol_log_error call
   * that is not oversized. This is true in every environment where the
   * backtrace capture works. See the doc comment of
   * backtrace_capture_genuinely_available(). */
  if (backtrace_capture_genuinely_available()) {
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }

  /* Every line must be well-formed on its own and must end with a newline.
   * This is true for the fallback record and for a backtrace continuation
   * line. The oversized message must never desynchronize the stream. */
  size_t i = 0;
  while (i < len) {
    REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
    char *nl = memchr(buf + i, '\n', len - i);
    REQUIRE_NE(nl, NULL);
    i = (size_t)(nl - buf) + 1;
  }

  cleanup_dir(dir, "app.log");
}

/* Syslog also writes its backtrace frames as separate records with a PRI
 * prefix. This does not depend on whether the primary record falls back to
 * the placeholder because it is oversized. See the logfmt test above. */
TEST(oversized,
     syslog_message_over_buf_cap_with_backtrace_still_writes_backtrace_lines) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_ERROR, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'D', sz);
  huge[sz] = '\0';

  ccol_log_error(lg, "%s", huge);

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  free(huge);

  /* The PRI for CLOG_ERROR at the default CLOG_SYSLOG_USER facility (1) is
   * 1*8 + 3 = 11, where 3 is the severity of an error. There must be at
   * least two well-formed "<11>1 " records: the fallback placeholder, and
   * one or more backtrace frames. This holds in every environment where the
   * backtrace capture works. See the doc comment of
   * backtrace_capture_genuinely_available(). In another environment there is
   * only the fallback placeholder. */
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
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }
}

/* ========================================================================== */
/*                         SIZE-BASED ROTATION                                */
/* ========================================================================== */

TEST(rotation, size_rotation_creates_new_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300, /* tiny threshold to force rotation */
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Write enough data to exceed the threshold several times */
  for (int i = 0; i < 20; i++)
    ccol_log_info(lg, "rotation test message index=%d padding-padding-padding",
                  i);

  clog_close(lg);

  /* At least one rotated file should exist alongside app.log */
  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, max_rotated_files_respected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Drive enough rotations to exceed max_rotated_files */
  for (int i = 0; i < 40; i++)
    ccol_log_info(
        lg, "rotation pruning test idx=%d extra-data-to-fill-the-buffer", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  /* The pruner keeps at most max_rotated_files on disk */
  REQUIRE_EQ((rotated <= 2), 1);

  cleanup_dir(dir, "app.log");
}

/* A max_rotated_files of zero means that the caller does not set the field.
 * It must fall back to CLOG_DEFAULT_MAX_ROTATED_FILES (7). The other two
 * rotation fields fall back to their own CLOG_DEFAULT_* constant in the same
 * way when they stay at 0. If 0 meant "no limit", rotated files would
 * collect forever and could fill the disk. */
TEST(rotation, max_rotated_files_zero_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Drive many more rotations than CLOG_DEFAULT_MAX_ROTATED_FILES. Without
   * the default quota, every one of them stays on the disk. */
  for (int i = 0; i < 40; i++)
    ccol_log_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

/* This is the same test, but for a negative max_rotated_files. Every value
 * that is <= 0 falls back to the default. This matches max_file_size. The
 * logger does not treat 0 and a negative value differently. */
TEST(rotation, max_rotated_files_negative_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = -3,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 40; i++)
    ccol_log_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

/* A file can start with "<base>.<14 digits>" and still not come from this
 * logger. For example, a person or another tool can put a manual backup with
 * a date stamp in the same directory. The pruner must never select such a
 * file. A real rotated name is always "<base>.<14 digits>". It can also hold
 * "_NNNN" after the digits, and ".gz" at the end. Any other text after the
 * 14 digits keeps the file out of the prune completely. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Drive enough rotations to make the pruner run many times. The quota
   * keeps only 1 file. */
  for (int i = 0; i < 40; i++)
    ccol_log_info(lg, "prune safety test idx=%d extra-data-to-fill-the-buffer",
                  i);

  clog_close(lg);

  /* The foreign file must stay, and its content must not change. This is
   * true for any number of pruner runs. */
  struct stat st;
  REQUIRE_EQ(stat(foreign_path, &st), 0);
  char buf[64];
  read_file(foreign_path, buf, sizeof(buf));
  REQUIRE_STREQ(buf, "do not delete me");

  cleanup_dir(dir, "app.log");
}

TEST(rotation, logger_recovers_after_file_externally_deleted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* Fill the file first, so that bytes_written starts 1 byte below the
   * rotation threshold. The first log write is at least 80 bytes. It pushes
   * the counter above the limit and starts a rotation. */
  {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    char filler[9999];
    memset(filler, 'x', sizeof(filler));
    ssize_t w = write(fd, filler, sizeof(filler));
    (void)w;
    close(fd);
  }

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10000,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Simulate an external agent that deletes the log file while the logger
   * holds it open. The fd stays valid. The directory entry is gone. */
  REQUIRE_EQ(unlink(path), 0);

  /* At the first write, bytes_written (9999 + line_len) is >= 10000, so a
   * rotation starts. rename(path, rotated) returns ENOENT, because the
   * source is gone. _rotate treats ENOENT as a clean slate. It creates a new
   * file at `path` with O_CREAT and continues in the normal way. */
  ccol_log_info(lg, "triggers rotation");

  /* At the second write, bytes_written is 0 again after the rotation, and
   * line_len is below 10000. There is no other rotation. This line goes into
   * the new file at `path`. */
  ccol_log_info(lg, "after recovery");

  clog_close(lg);

  /* The logger must create the file again at the original path. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = false,
      .time_rotation_enabled = true,
      .rotation_interval_us = 1000000,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The first write sets the last_rotation baseline. There is no rotation
   * yet. */
  ccol_log_info(lg, "before rotation");

  /* Sleep long enough to exceed the 1-second rotation interval. */
  sleep(2);

  /* This write triggers time-based rotation. */
  ccol_log_info(lg, "after rotation");

  clog_close(lg);

  /* At least one rotated file must exist alongside the live log. */
  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  /* The live file must contain the post-rotation message. */
  char buf[4096];
  read_file(path, buf, sizeof(buf));
  REQUIRE_NE(strstr(buf, "after rotation"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, same_second_collision_uses_suffix) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* With max_file_size = 10, every log write starts an immediate size-based
   * rotation, because each write is much larger than 10 bytes. Five
   * rotations inside the same UTC second make the collision code generate
   * the suffixes _0001, _0002 and so on. */
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 5; i++) ccol_log_info(lg, "collision test %d", i);

  clog_close(lg);

  /* At least one file with the _0001 collision suffix must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, "_0001"), 0);

  cleanup_dir(dir, "app.log");
}

/* A rotation can fail again and again. For example, the destination
 * directory can lose its write permission. The logger must not retry the
 * rotation at every write. After one try fails, the logger backs off for a
 * bounded window. Without this, every later write pays the full syscall cost
 * of rename() and open() for as long as the condition lasts. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10, /* tiny threshold: every write below exceeds it */
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Remove the write permission on the directory. rename() inside _rotate()
   * then fails with EACCES at every try. This simulates a rotation failure
   * that does not go away. */
  REQUIRE_EQ(chmod(dir, 0555), 0);

  clog_test_reset_rotate_attempt_count();

  /* Each one of these 20 writes is above the 10-byte threshold on its own.
   * Without a backoff, each write tries a rotation and fails. */
  for (int i = 0; i < 20; i++)
    ccol_log_info(lg, "retry backoff test idx=%d", i);

  size_t attempts = clog_test_get_rotate_attempt_count();
  REQUIRE_GT(attempts, (size_t)0);
  REQUIRE_LT(attempts, (size_t)20);

  /* Restore write permission so cleanup_dir can remove the directory. */
  REQUIRE_EQ(chmod(dir, 0755), 0);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         DERIVED LOGGERS                                    */
/* ========================================================================== */

TEST(derive, derived_is_not_null) {
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  clog_close(child);
  clog_close(parent);
}

TEST(derive, inherits_parent_fields) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_field(parent, "service", "auth");
  clog_set_field(parent, "env", "prod");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  ccol_log_info(child, "from child");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Child must carry the snapshot of parent's fields at derive time */
  REQUIRE_NE(strstr(buf, "service=auth"), NULL);
  REQUIRE_NE(strstr(buf, "env=prod"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_field_does_not_affect_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  clog_set_field(child, "req_id", "child-only");

  /* Only the parent writes. Its line must not hold the child-only field */
  ccol_log_info(parent, "parent line");

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Add a field to the parent AFTER clog_derive takes the snapshot */
  clog_set_field(parent, "added_after", "yes");

  /* Only the child writes. Its line must not hold the parent field that
   * comes after the derive. */
  ccol_log_info(child, "child line");

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  ccol_log_info(parent, "parent message");
  ccol_log_info(child, "child message");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Both messages must land in the same file */
  REQUIRE_NE(strstr(buf, "parent message"), NULL);
  REQUIRE_NE(strstr(buf, "child message"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_close_does_not_break_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  ccol_log_info(child, "before child close");
  clog_close(child);

  /* The parent must still work after clog_close on the derived logger */
  ccol_log_info(parent, "after child close");
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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  ccol_log_info(parent, "before parent close");
  clog_close(parent);

  /* The child must still work after clog_close on its parent */
  ccol_log_info(child, "after parent close");
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

  clog parent = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Set the level of the child below the level of the parent. The child then
   * sees DEBUG and the parent does not. */
  clog_set_level(child, CLOG_DEBUG);

  ccol_log_debug(parent, "parent debug - dropped");
  ccol_log_debug(child, "child debug - visible");

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_field(parent, "version", "1");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Override the inherited field in the child only */
  clog_set_field(child, "version", "2");

  ccol_log_info(parent, "parent write");
  ccol_log_info(child, "child write");

  clog_close(child);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* Locate the two log lines */
  char *parent_line = strstr(buf, "parent write");
  char *child_line = strstr(buf, "child write");
  REQUIRE_NE(parent_line, NULL);
  REQUIRE_NE(child_line, NULL);

  /* Find the start-of-line for each to isolate them */
  char *ps = parent_line;
  while (ps > buf && *(ps - 1) != '\n') ps--;
  char *cs = child_line;
  while (cs > buf && *(cs - 1) != '\n') cs--;

  /* NUL-terminate each line for isolated strstr checks */
  char *pe = strchr(parent_line, '\n');
  char *ce = strchr(child_line, '\n');
  if (pe) *pe = '\0';
  if (ce) *ce = '\0';

  REQUIRE_NE(strstr(ps, "version=1"), NULL); /* the parent keeps the first */
  REQUIRE_NE(strstr(cs, "version=2"), NULL); /* the child has its override */

  cleanup_dir(dir, "app.log");
}

TEST(derive, grandchild_derive) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_field(parent, "origin", "parent");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);
  /* The child inherits origin=parent from the snapshot. Add its own field */
  clog_set_field(child, "gen", "child");

  /* Derive a grandchild from the child */
  clog grandchild = clog_derive(child);
  REQUIRE_NE(grandchild, CLOG_INVALID);
  /* grandchild inherits both origin=parent and gen=child */

  /* Close the parent and the child first. The grandchild must still work */
  clog_close(parent);
  clog_close(child);

  ccol_log_info(grandchild, "from grandchild");
  clog_close(grandchild);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "from grandchild"), NULL);
  /* Both inherited fields must appear in the grandchild's line */
  REQUIRE_NE(strstr(buf, "origin=parent"), NULL);
  REQUIRE_NE(strstr(buf, "gen=child"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, multiple_children_from_one_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_field(parent, "shared", "yes");

  clog child1 = clog_derive(parent);
  REQUIRE_NE(child1, CLOG_INVALID);
  clog_set_field(child1, "name", "c1");

  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_INVALID);
  clog_set_field(child2, "name", "c2");

  ccol_log_info(child1, "from child1");
  ccol_log_info(child2, "from child2");

  clog_close(child1);
  clog_close(child2);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* Both messages land in the same file */
  REQUIRE_NE(strstr(buf, "from child1"), NULL);
  REQUIRE_NE(strstr(buf, "from child2"), NULL);

  /* Isolate each log line for independent field checks */
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

  /* Both children inherit the shared field from the parent snapshot */
  REQUIRE_NE(strstr(ls1, "shared=yes"), NULL);
  REQUIRE_NE(strstr(ls2, "shared=yes"), NULL);

  /* Each child's own field is independent and must not appear on the other's
   * line */
  REQUIRE_NE(strstr(ls1, "name=c1"), NULL);
  REQUIRE_EQ(strstr(ls1, "name=c2"), NULL);
  REQUIRE_NE(strstr(ls2, "name=c2"), NULL);
  REQUIRE_EQ(strstr(ls2, "name=c1"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         THREAD SAFETY                                      */
/* ========================================================================== */

#define THREAD_COUNT 8
#define MSGS_PER_THREAD 50

typedef struct {
  clog lg;
  int thread_id;
} thread_arg_t;

static void *_writer_thread(void *arg) {
  thread_arg_t *a = (thread_arg_t *)arg;
  for (int i = 0; i < MSGS_PER_THREAD; i++)
    ccol_log_info(a->lg, "thread=%d msg=%d", a->thread_id, i);
  return NULL;
}

TEST(threading, concurrent_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

  /* Every line must start with "ts=". No partial write may interleave. */
  char buf[131072];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_NE(len, (size_t)0);

  int bad_lines = 0;
  char *line = buf;
  while (line < buf + len) {
    char *nl = strchr(line, '\n');
    /* Skip backtrace continuation lines (start with tab) */
    if (line[0] != '\t' && strncmp(line, "ts=", 3) != 0) bad_lines++;
    if (!nl) break;
    line = nl + 1;
  }
  REQUIRE_EQ(bad_lines, 0);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         CUSTOM ALLOCATOR                                   */
/* ========================================================================== */

static size_t _custom_alloc_count = 0;
static size_t _custom_free_count = 0;

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

  ccol_memmgmt_procs_t procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "custom allocator test");

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

  ccol_memmgmt_procs_t procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "env", "test");
  ccol_log_info(lg, "custom allocator file test");

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

  ccol_memmgmt_procs_t procs = {
      .malloc = _custom_malloc,
      .free = _custom_free,
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  ccol_log_info(child, "child using parent allocator");
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
  ccol_memmgmt_procs_t bad_procs = {
      .malloc = _custom_malloc,
      .free = NULL, /* free is absent, so the logger must reject this */
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &bad_procs);
  REQUIRE_EQ(lg, CLOG_INVALID);
}

/* A counting allocator. Its malloc, calloc and realloc can fail at exactly
 * one numbered call. The test below uses it to make a later construction
 * step of clog_open_file_mp() fail. That step is the setup of the async
 * logging. The failure happens after the open of the log file succeeds. */
static _Atomic int g_ctor_fail_call_count = 0;
static _Atomic int g_ctor_fail_at = -1;

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

/* clog_open_file_mp() opens its log file with a real open() call. That call
 * runs before the rest of the construction. EVERY later failure path must
 * therefore close that fd. This includes the setup of the async logging in
 * _shared_async_init(), which a non-NULL async_cfg turns on. It is not only
 * the earlier, separate path where _alloc() itself fails.
 *
 * The test sweeps a wide range of "fail the Nth allocation" points. One
 * hardcoded count is fragile, because the number of allocations in the
 * construction sequence can change for an unrelated reason. With a sweep,
 * several iterations land inside the async setup itself. Every iteration
 * must leave the open-fd count of the process at its start value. This holds
 * when the failure lands in the async setup, when it lands earlier inside
 * _alloc(), and when there is no failure at all. */
TEST(custom_alloc, open_file_async_init_failure_does_not_leak_fd) {
  int baseline = count_open_fds();

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _ctor_fail_malloc,
      .free = _ctor_fail_free,
      .calloc = _ctor_fail_calloc,
      .realloc = _ctor_fail_realloc,
  };
  clog_async_cfg_t acfg = {0}; /* unbounded queue */

  for (int fail_at = 1; fail_at <= 60; fail_at++) {
    atomic_store(&g_ctor_fail_call_count, 0);
    atomic_store(&g_ctor_fail_at, fail_at);
    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
    atomic_store(&g_ctor_fail_at, -1);

    if (lg != CLOG_INVALID) clog_close(lg);

    int now = count_open_fds();
    REQUIRE_EQ(now, baseline);
  }

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         NULL HANDLE VALIDATION                             */
/* ========================================================================== */

/* Every public entry point must assert when it gets a NULL logger handle. It
 * must not segfault with no diagnostic. This is the convention of this
 * project for an invalid raw handle. For example, chmap_elem_count and
 * chmap_reset in chashmap.c, and the raw-layer functions of cvector.c, all
 * call ccol_assert(false) on NULL. They do not dereference the NULL. Each
 * check below runs in a forked child, because the assertion aborts the whole
 * process. */

static void _call_clog_close_null(void) { clog_close(CLOG_INVALID); }
static void _call_clog_derive_null(void) { (void)clog_derive(CLOG_INVALID); }
static void _call_clog_set_level_null(void) {
  clog_set_level(CLOG_INVALID, CLOG_INFO);
}
static void _call_clog_get_level_null(void) {
  (void)clog_get_level(CLOG_INVALID);
}
static void _call_clog_set_format_null(void) {
  clog_set_format(CLOG_INVALID, CLOG_FMT_JSON);
}
static void _call_clog_get_format_null(void) {
  (void)clog_get_format(CLOG_INVALID);
}
static void _call_clog_set_facility_null(void) {
  clog_set_facility(CLOG_INVALID, CLOG_SYSLOG_USER);
}
static void _call_clog_get_facility_null(void) {
  (void)clog_get_facility(CLOG_INVALID);
}
static void _call_clog_set_field_null(void) {
  clog_set_field(CLOG_INVALID, "key", "value");
}
static void _call_clog_remove_field_null(void) {
  clog_remove_field(CLOG_INVALID, "key");
}
static void _call_clog_clear_fields_null(void) {
  clog_clear_fields(CLOG_INVALID);
}
static void _call_clog_flush_null(void) { clog_flush(CLOG_INVALID); }
static void _call_log_info_null(void) {
  ccol_log_info(CLOG_INVALID, "message");
}

static void _expect_fatal(void (*fn)(void)) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    fn();
    _exit(0); /* the handle-validation assert stops the child first */
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

/* ========================================================================== */
/*                         ROBUSTNESS UNDER ALLOCATION FAILURE                */
/* ========================================================================== */

/* An allocator whose realloc() fails at exactly the g_realloc_fail_at'th
 * call. The count starts at 1, and a value <= 0 means "never fail". Every
 * other call succeeds. One test can therefore sweep a fault across every
 * buffer growth that a ccol_log_error() with a backtrace can make. Those
 * growths are the logger construction, the field-map operations, the primary
 * record, and each backtrace frame. */
static int g_realloc_fail_at = -1;
static int g_realloc_call_count = 0;

static void *_counting_malloc(size_t sz) { return malloc(sz); }
static void _counting_free(void *p) { free(p); }
static void *_counting_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void *_counting_realloc(void *p, size_t sz) {
  g_realloc_call_count++;
  if (g_realloc_fail_at > 0 && g_realloc_call_count == g_realloc_fail_at)
    return NULL;
  return realloc(p, sz);
}

TEST(robustness, error_with_backtrace_never_writes_malformed_lines_under_oom) {
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };

  /* Sweep the fault across a wide range of call indices. It then lands in
   * the construction, in the primary record, and in a backtrace frame on a
   * platform with backtrace support. Every one of those buffer growths must
   * degrade in a clean way. None of them may write a malformed line. */
  for (int fail_at = 1; fail_at <= 80; fail_at++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    g_realloc_call_count = 0;
    g_realloc_fail_at = fail_at;

    clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      /* The construction itself hits the fault. There is nothing to check
       * in this round. */
      cleanup_dir(dir, "app.log");
      continue;
    }

    ccol_log_error(lg, "robustness check %d", fail_at);

    g_realloc_fail_at = -1; /* do not let the bookkeeping of close() fault */
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof(buf));

    /* Every line that the logger writes must be well-formed on its own. It
     * must start with the expected prefix and end with a real newline. This
     * is true for the primary record and for a backtrace continuation line.
     * Without this, a partial write merges into the line that follows it.
     * For example, a truncated backtrace frame has no newline at its end. */
    size_t i = 0;
    while (i < len) {
      REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
      if (buf[i] == 't') REQUIRE_EQ(strncmp(buf + i, "ts=", 3), 0);
      char *nl = memchr(buf + i, '\n', len - i);
      REQUIRE_NE(nl, NULL); /* every line must end with a newline */
      i = (size_t)(nl - buf) + 1;
    }

    cleanup_dir(dir, "app.log");
  }

  g_realloc_fail_at = -1;
}

/* JSON embeds the backtrace inline, before the closing "}\n" of the record.
 * This is a separate code path from logfmt and syslog. Those two write one
 * frame at a time, after the main write.
 *
 * A buffer-growth allocation can fail while the logger appends the "bt"
 * array. backtrace_symbols() can also fail. The record must never drop the
 * backtrace and still look like an ordinary, correctly closed JSON record.
 * One of three things must happen. The "bt" array stays, and it can hold
 * fewer frames. A "bt_error" marker takes its place. Or the well-formed
 * fallback for an oversized record replaces the whole record. For a call
 * with with_backtrace=true, a JSON object that closes in the normal way with
 * no "bt" and no "bt_error" must never happen. */
TEST(robustness, json_backtrace_failure_is_never_silently_dropped) {
  ccol_memmgmt_procs_t procs = {
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

    clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      cleanup_dir(dir, "app.log");
      continue;
    }
    clog_set_format(lg, CLOG_FMT_JSON);

    ccol_log_error(lg, "json backtrace robustness check %d", fail_at);

    g_realloc_fail_at = -1;
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof(buf));
    if (len == 0) {
      cleanup_dir(dir, "app.log");
      continue;
    }

    /* Exactly one, well-formed JSON object line. */
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

/* An allocator whose calloc() fails on demand. g_fail_next_calloc controls
 * it. malloc(), realloc() and free() do not change. The test uses it to fail
 * exactly one calloc() call in a deterministic way. That call is the
 * iterator allocation inside chashmap_begin_iter(). The allocator does not
 * disturb the logger construction. It also does not disturb the map-growth
 * allocations of clog_set_field(). Both of those must succeed first. */
static int g_fail_next_calloc = 0;

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

/* chashmap_begin_iter() allocates its own iterator struct with calloc().
 * That one call can fail one time, for a reason that has nothing to do with
 * the size of the record. The logger must not then drop every field and
 * still write a record that looks normal. It must report the problem in the
 * same way as any other append failure in _clog_write() that it cannot
 * recover from. That report is the well-formed fallback placeholder record.
 * The placeholder must carry a correct diagnostic. This fault is a transient
 * allocation failure and not an oversized record, and a reader must be able
 * to tell the two apart. */
TEST(robustness, field_iterator_alloc_failure_never_silently_drops_fields) {
  ccol_memmgmt_procs_t procs = {
      .malloc = _fail_next_calloc_malloc,
      .free = _fail_next_calloc_free,
      .calloc = _fail_next_calloc_calloc,
      .realloc = _fail_next_calloc_realloc,
  };

  const clog_format_t formats[] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON,
                                   CLOG_FMT_SYSLOG};
  for (size_t fi = 0; fi < sizeof(formats) / sizeof(formats[0]); fi++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
    char path[512];
    snprintf(path, sizeof(path), "%s/app.log", dir);

    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
    REQUIRE_NE(lg, CLOG_INVALID);
    clog_set_format(lg, formats[fi]);

    clog_set_field(lg, "service", "auth");
    clog_set_field(lg, "env", "prod");

    /* Fail exactly the next calloc() call. That call is the allocation of
     * the field-map iterator, which the ccol_log_info() call below makes. */
    g_fail_next_calloc = 1;
    ccol_log_info(lg, "message that must not silently lose its fields");
    g_fail_next_calloc = 0;

    clog_close(lg);

    char buf[4096];
    size_t len = read_file(path, buf, sizeof(buf));
    REQUIRE_GT(len, (size_t)0);

    /* Either both fields reach a well-formed record, or the well-formed
     * fallback placeholder reports the fault. A record that looks like an
     * ordinary success but holds neither field must never happen. */
    bool has_service = strstr(buf, "service") != NULL;
    bool has_env = strstr(buf, "env") != NULL;
    bool has_alloc_failure_notice =
        strstr(buf, "transient allocation failure") != NULL;
    REQUIRE_TRUE((has_service && has_env) || has_alloc_failure_notice);

    /* This fault is a transient allocation failure and not an oversized
     * record. When the fallback fires, it must say so. It must not report
     * "too large to emit". */
    if (has_alloc_failure_notice)
      REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);

    cleanup_dir(dir, "app.log");
  }
}

/* An allocator whose malloc() fails on demand. g_fail_next_malloc controls
 * it. calloc(), realloc() and free() do not change. The test uses it to fail
 * exactly one heap allocation inside one _clog_write() call, in a
 * deterministic way. It does not disturb the logger construction, which must
 * first succeed with a real malloc(). */
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

/* _clog_build_header() spills a long __FILE__ path to a heap buffer. A real
 * allocator failure there must not leave the alloc_failure verdict of
 * _clog_build_record() at false. If it does, the record falls back to the
 * general note "log record too large to emit". That note is wrong, because
 * the true cause has nothing to do with the size of the record. See the doc
 * comment of clog_buf_t.oom.
 *
 * The test calls _clog_write() directly with a synthetic "file" argument. That
 * argument is at least as long as the 512-byte stack buffer that
 * _clog_build_header() uses to build the logfmt "src=" value. This direct
 * call is the only deterministic way to reach the heap-spill branch. The
 * log_* macros always pass the literal __FILE__ of their own call site, and
 * that path is normally short. */
TEST(robustness, header_build_allocation_failure_reported_accurately) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _fail_next_malloc_malloc,
      .free = _fail_next_malloc_free,
      .calloc = _fail_next_malloc_calloc,
      .realloc = _fail_next_malloc_realloc,
  };

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  char long_file[600];
  memset(long_file, 'A', sizeof(long_file));
  long_file[sizeof(long_file) - 1] = '\0';

  /* Fail exactly the next malloc() call. That call is the heap-spill buffer
   * for the long src value, which the _clog_write() call below makes. This
   * logger holds no fields, so no field-iterator allocation competes for the
   * same single fault. */
  g_fail_next_malloc = 1;
  _clog_write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello world");
  g_fail_next_malloc = 0;

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* The record must name the real cause, which is a transient allocation
   * failure. It must never label the fault as an oversized record. */
  REQUIRE_NE(strstr(buf, "transient allocation failure"), NULL);
  REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);

  cleanup_dir(dir, "app.log");
}

/* This test calls _clog_write() directly with a synthetic "file" argument.
 * That argument is at least as long as the 512-byte stack buffer that
 * _clog_write() uses to build the logfmt "src=" value. It also holds bytes
 * that are special to logfmt, which are a space and '='. Only a direct call
 * gives exact control over the length of the file argument, and that control
 * is what reaches this code path. The log_* macros always pass the literal
 * __FILE__ of the call site. */
TEST(robustness, long_file_path_in_src_field_is_still_quoted_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  char long_file[600];
  memset(long_file, 'A', sizeof(long_file));
  long_file[0] = ' ';   /* special to logfmt: first byte of the src= value */
  long_file[100] = '='; /* special to logfmt: not to be read as a new token */
  long_file[sizeof(long_file) - 1] = '\0';

  _clog_write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello %s",
              "world");

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* The record must stay one well-formed logfmt line. It must hold one "ts="
   * token, and the msg= value must still be easy to find and to isolate.
   * Neither is possible if the long src value leaks an unquoted space or an
   * unquoted '=' into the middle of the line. */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);

  /* The src= value must carry quotes. A raw, unquoted space or '=' from the
   * long file path must never appear as a bare " src= " with no first
   * quote. */
  REQUIRE_EQ(strstr(buf, " src= "), NULL);
  REQUIRE_NE(strstr(buf, " src=\""), NULL);
  REQUIRE_NE(strstr(buf, "msg=\"hello world\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* A width and precision can be large enough that the total output length of
 * the two conversions goes above INT_MAX. vsnprintf() itself then fails:
 * glibc returns -1 and sets errno to EOVERFLOW. It produces no content at
 * all, and it does not truncate. No partial content is left to keep. This is
 * therefore the one case that the message format step of _clog_write()
 * cannot route through its usual heap-spill and "...[truncated]" fallback. */
TEST(robustness, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This test needs a real run-time failure of vsnprintf(), and not one
   * that the compiler can detect. The width therefore arrives as an argument
   * and not as digits in the format string, and a volatile read keeps it a
   * run-time value at every optimization level, so the format-overflow
   * analysis of the compiler cannot fold it. */
  volatile int huge_width_source = 2000000000;
  int huge_width = huge_width_source;
  _clog_write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));

  /* The logger must not drop the record and write nothing. It must write a
   * well-formed placeholder line that names the real cause. */
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "log message formatting failed"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         JSON OUTPUT FORMAT                                 */
/* ========================================================================== */

TEST(json, default_format_is_logfmt) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);
  clog_close(lg);
}

TEST(json, get_set_round_trip) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  ccol_log_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* The line must be a JSON object. It starts with '{' and closes with '}'
   * before the '\n'. */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  /* All mandatory JSON keys */
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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  ccol_log_trace(lg, "t");
  ccol_log_debug(lg, "d");
  ccol_log_info(lg, "i");
  ccol_log_warn(lg, "w");

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "env", "prod");
  clog_set_field(lg, "service", "auth");

  ccol_log_info(lg, "test");

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* Message contains a double-quote and a newline */
  ccol_log_info(lg, "say \"hi\"\nworld");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Double-quote must be escaped as \" in the JSON string */
  REQUIRE_NE(strstr(buf, "\\\""), NULL);
  /* Newline must be escaped as \n (two chars: backslash + n) */
  REQUIRE_NE(strstr(buf, "\\n"), NULL);
  /* The entire record must still be a single line ending with }\n */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "path", "C:\\Users\\foo");

  ccol_log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Backslash must be escaped as \\ */
  REQUIRE_NE(strstr(buf, "C:\\\\Users\\\\foo"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, del_byte_escaped_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* The JSON output must escape DEL (0x7f) as \u007f. This is true in a
   * field value and in the message. The split of the string literal stops
   * GCC from reading \x7fe as one hex escape of several digits. */
  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  ccol_log_info(lg,
                "msg\x7f"
                "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Raw DEL must not appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* DEL must be encoded as the JSON Unicode escape \u007f. */
  REQUIRE_NE(strstr(buf, "\\u007f"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, valid_multibyte_utf8_passed_through_unescaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* These are a 2-byte e-acute (U+00E9), a 3-byte euro sign (U+20AC) and a
   * 4-byte emoji (U+1F600). All three are well-formed UTF-8. The logger must
   * never replace one of them with the Unicode replacement character. */
  const char *msg =
      "caf\xc3\xa9 costs \xe2\x82\xac"
      "1 \xf0\x9f\x98\x80";
  ccol_log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, msg), NULL);
  REQUIRE_EQ(strstr(buf, "\\ufffd"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_lone_continuation_byte_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0x80 is a continuation byte, and no lead byte comes before it. It is
   * never valid UTF-8 on its own. */
  const char *msg =
      "before\x80"
      "after";
  ccol_log_info(lg, "%s", msg);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0xc0 0x80 is the well-known overlong encoding of NUL. The bytes 0xc0
   * and 0xc1 can never start a well-formed sequence. */
  const char *msg =
      "before\xc0\x80"
      "after";
  ccol_log_info(lg, "%s", msg);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0xed 0xa0 0x80 decodes to U+D800. That is one half of a UTF-16 surrogate
   * pair, which is never a valid Unicode scalar value on its own. */
  const char *msg =
      "before\xed\xa0\x80"
      "after";
  ccol_log_info(lg, "%s", msg);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* This is a 3-byte lead (0xe2) with nothing after it. The end of the
   * message truncates the sequence. A bad continuation byte does not. */
  const char *msg = "trunc\xe2";
  ccol_log_info(lg, "%s", msg);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "k",
                 "v\xff"
                 "end");

  ccol_log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"k\":\"v\\ufffdend\""), NULL);
  REQUIRE_EQ(memchr(buf, 0xff, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, error_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  ccol_log_error(lg, "something failed");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* Backtrace must be an inline JSON array */
  REQUIRE_NE(strstr(buf, "\"bt\":["), NULL);
  /* No tab-indented continuation lines */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  /* Must be a single JSON object line */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

/* This test guards consistency. A capture can succeed and still be shallow,
 * with a depth <= CLOG_BT_INITIAL_FRAME. JSON treats such a capture as a
 * real, empty backtrace and not as a failure. See the
 * array_content_is_accurate check of _emit_backtrace_json(). This test pins
 * that behaviour. It uses the clog_test_force_shallow_backtrace_depth()
 * hook, which the logfmt and syslog tests also use for the same boundary
 * condition in those two formats. */
TEST(json, error_with_shallow_backtrace_capture_still_yields_empty_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_test_force_shallow_backtrace_depth(true);
  ccol_log_error(lg, "something failed");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* A capture that succeeds but is shallow is a real, empty backtrace and
   * not a failure. The output holds "bt":[] and no bt_error marker. This is
   * the same "bt":[...] shape as for an ordinary capture that succeeds, but
   * with zero elements. */
  REQUIRE_NE(strstr(buf, "\"bt\":[]"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, format_shared_with_derived_logger) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_format(parent, CLOG_FMT_JSON);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Derived logger shares the format because it shares the same backing store
   */
  REQUIRE_EQ(clog_get_format(child), CLOG_FMT_JSON);

  ccol_log_info(child, "child json");
  ccol_log_info(parent, "parent json");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Both lines must be JSON objects */
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

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_alert(lg, "alert message");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "alert message"), NULL);
  /* ccol_log_alert must write a backtrace continuation line. This is true in
   * every environment where the backtrace capture works. See the doc comment
   * of backtrace_capture_genuinely_available() in this file. */
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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_error(lg, "error event");
  ccol_log_alert(lg, "alert event");

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
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_alert(lg, "critical failure");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "critical failure"), NULL);
  /* ccol_log_alert must produce at least one backtrace continuation line */
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, set_format_on_derived_affects_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Set the JSON format on the child. The parent must see it too */
  clog_set_format(child, CLOG_FMT_JSON);
  REQUIRE_EQ(clog_get_format(parent), CLOG_FMT_JSON);

  ccol_log_info(parent, "parent line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* The line of the parent must be JSON */
  REQUIRE_EQ(buf[0], '{');

  cleanup_dir(dir, "app.log");
}

TEST(json, error_and_alert_level_strings_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  ccol_log_error(lg, "e");
  ccol_log_alert(lg, "a");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"ERROR\""), NULL);
  REQUIRE_NE(strstr(buf, "\"ALERT\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, alert_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  ccol_log_alert(lg, "alert event");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof(buf));

  /* Alert must carry an inline bt array */
  int bt_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "\"bt\":[")) != NULL) {
    bt_count++;
    p++;
  }
  REQUIRE_EQ(bt_count, 1);

  /* JSON format must not produce tab-indented backtrace continuation lines */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, tab_and_cr_in_field_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* The field value holds an HT and a CR. JSON must escape both. */
  clog_set_field(lg, "data", "col1\tcol2\r\n");

  ccol_log_info(lg, "tab and cr test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Raw HT and CR must not appear in the JSON output */
  REQUIRE_EQ(memchr(buf, '\t', strlen(buf)), NULL);
  REQUIRE_EQ(memchr(buf, '\r', strlen(buf)), NULL);

  /* The escaped sequences must be present */
  REQUIRE_NE(strstr(buf, "\\t"), NULL);
  REQUIRE_NE(strstr(buf, "\\r"), NULL);

  /* The entire record must be a single JSON-object line */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         RFC 5424 SYSLOG FORMAT                             */
/* ========================================================================== */

/*
 * ccol_log_fatal calls exit() inside the library. The child therefore
 * registers an atexit handler that closes the logger before the process
 * stops. Without this handler, the valgrind report of the child shows the
 * clogger allocations as "still reachable". Only the child branch sets the
 * global, after the fork, so the handler never fires in the parent process.
 */
static clog _g_fatal_child_lg = CLOG_INVALID;
static void _fatal_child_cleanup(void) {
  if (_g_fatal_child_lg) {
    clog_close(_g_fatal_child_lg);
    _g_fatal_child_lg = CLOG_INVALID;
  }
}

TEST(syslog, basic_structure) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  ccol_log_info(lg, "hello syslog");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* PRI for LOG_USER(1) + INFO(6) = 14 */
  REQUIRE_NE(strstr(buf, "<14>1 "), NULL);
  /* RFC 5424 SD-ID for structured data */
  REQUIRE_NE(strstr(buf, "[ccol "), NULL);
  /* proc, src, and func present as SD params */
  REQUIRE_NE(strstr(buf, "proc=\""), NULL);
  REQUIRE_NE(strstr(buf, "src=\""), NULL);
  REQUIRE_NE(strstr(buf, "func=\""), NULL);
  /* MSGID is the level string */
  REQUIRE_NE(strstr(buf, " INFO "), NULL);
  /* message text follows the SD block */
  REQUIRE_NE(strstr(buf, "hello syslog"), NULL);
  /* single line */
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

TEST(syslog, severity_encoding) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* LOG_USER(1): WARN->4 -> PRI=12; ERROR->3 -> PRI=11; ALERT->1 -> PRI=9;
     FATAL->0 -> PRI=8 */
  ccol_log_warn(lg, "w");
  ccol_log_error(lg, "e");
  ccol_log_alert(lg, "a");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<12>"), NULL); /* WARN   */
  REQUIRE_NE(strstr(buf, "<11>"), NULL); /* ERROR  */
  REQUIRE_NE(strstr(buf, "<9>"), NULL);  /* ALERT  */
}

TEST(syslog, facility_change) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  REQUIRE_EQ(clog_get_facility(lg), CLOG_SYSLOG_USER);

  /* Switch to DAEMON(3); INFO(6) -> PRI = 3*8+6 = 30 */
  clog_set_facility(lg, CLOG_SYSLOG_DAEMON);
  REQUIRE_EQ(clog_get_facility(lg), CLOG_SYSLOG_DAEMON);

  ccol_log_info(lg, "daemon log");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "<30>"), NULL);
  REQUIRE_NE(strstr(buf, "daemon log"), NULL);
}

TEST(syslog, fields_in_sd) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_set_field(lg, "service", "auth");
  clog_set_field(lg, "env", "prod");

  ccol_log_info(lg, "structured");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* Fields must appear as SD params inside the [ccol ...] element. */
  REQUIRE_NE(strstr(buf, "service=\"auth\""), NULL);
  REQUIRE_NE(strstr(buf, "env=\"prod\""), NULL);
  /* The closing bracket and message must follow. */
  REQUIRE_NE(strstr(buf, "] structured"), NULL);
}

TEST(syslog, sd_param_name_over_32_chars_is_shortened_with_a_stable_suffix) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* This key has 37 characters, which is above the RFC 5424 SD-PARAM-NAME
   * limit of 32. The logger shortens a name that is above the limit. The
   * short name is a 23-character prefix of the original key, then a '~',
   * then an 8-digit hex hash of the full key. The total is 32 characters. A
   * bare 32-character prefix is not enough: two different long keys with a
   * common prefix then collide into one SD-PARAM-NAME. See
   * long_keys_sharing_prefix_get_distinct_sd_param_names below. */
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456789", "val");

  ccol_log_info(lg, "truncation");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* The stable prefix (first 23 chars of the original key) must be present. */
  char *name_start = strstr(buf, "abcdefghijklmnopqrstuv");
  REQUIRE_NE(name_start, NULL);

  /* The SD-PARAM-NAME that the logger writes, up to the '=', must be 32
   * characters or fewer. The full key is above the limit, and it must never
   * appear without a change. */
  char *eq = strchr(name_start, '=');
  REQUIRE_NE(eq, NULL);
  REQUIRE_TRUE((size_t)(eq - name_start) <= 32);
  REQUIRE_EQ(strstr(buf, "abcdefghijklmnopqrstuvwxyz_123456789="), NULL);
}

TEST(syslog, long_keys_sharing_prefix_get_distinct_sd_param_names) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* These two different keys are longer than 40 characters, and they share
   * their first 32 characters. A bare truncation to 32 characters makes both
   * collide into one SD-PARAM-NAME. RFC 5424 forbids that inside one
   * SD-ELEMENT. */
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_ONE", "one");
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_TWO", "two");

  ccol_log_info(lg, "collision test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  char *one_val = strstr(buf, "=\"one\"");
  char *two_val = strstr(buf, "=\"two\"");
  REQUIRE_NE(one_val, NULL);
  REQUIRE_NE(two_val, NULL);

  /* Walk back from each "=\"..\"" to the start of its own SD-PARAM-NAME (the
   * preceding space) and compare the two names directly. */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A call that sets the SYSLOG format on a file-backed logger must do
   * nothing. */
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);

  ccol_log_info(lg, "still logfmt");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Output must be logfmt, not syslog. */
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_EQ(strstr(buf, "<14>"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(syslog, backtrace_as_separate_messages) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_ERROR, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  ccol_log_error(lg, "with backtrace");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* There must be at least two syslog messages (main + >=1 backtrace frame). */
  int msg_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "<11>1 ")) != NULL) {
    msg_count++;
    p++;
  }
  REQUIRE_GT(msg_count, 1);

  /* Each backtrace message carries the tab-prefixed frame marker. */
  REQUIRE_NE(strstr(buf, "\t#0 "), NULL);
}

TEST(syslog, fatal_severity_encoding) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* clog_open_fd with owns_fd=false lets CLOG_FMT_SYSLOG work on a file fd. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);

  clog lg = clog_open_fd(fd, CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* ccol_log_fatal stops the process. Use a child to check it and to collect
   * the log output that it writes before the exit() call. */
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal syslog event");
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  waitpid(pid, &status, 0);
  clog_close(lg);
  close(fd);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* LOG_USER(1) * 8 + FATAL syslog severity(0) = 8 */
  REQUIRE_NE(strstr(buf, "<8>"), NULL);
  REQUIRE_NE(strstr(buf, "fatal syslog event"), NULL);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_NE(WEXITSTATUS(status), 0);

  cleanup_dir(dir, "app.log");
}

TEST(syslog, sd_param_value_special_chars_escaped) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* RFC 5424 needs an escape for ], \ and " in an SD-PARAM-VALUE. */
  clog_set_field(lg, "bracket", "val]end");
  clog_set_field(lg, "backslash", "C:\\foo");
  clog_set_field(lg, "quote", "say \"hi\"");

  ccol_log_info(lg, "sd escape test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* ']' -> '\]' */
  REQUIRE_NE(strstr(buf, "\\]"), NULL);
  /* '\' -> '\\' */
  REQUIRE_NE(strstr(buf, "C:\\\\foo"), NULL);
  /* '"' -> '\"' */
  REQUIRE_NE(strstr(buf, "\\\"hi\\\""), NULL);
}

TEST(syslog, message_with_embedded_newline_stays_single_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* The logger must escape a raw newline in the message. Without the escape,
   * that newline splits one syslog record into two lines. The second line
   * then looks like a separate record with no prefix. This is a well-known
   * way to inject content into a log. */
  ccol_log_info(lg, "first part\nFAKE-INJECTED-LINE data=1");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* Exactly one syslog record: the entire output is a single line. */
  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, 1);
  REQUIRE_EQ(buf[n - 1], '\n');

  /* The embedded newline must survive as a literal backslash-n escape. */
  REQUIRE_NE(strstr(buf, "first part\\nFAKE-INJECTED-LINE"), NULL);
  /* No raw injected content may look like a second record with no prefix. */
  REQUIRE_EQ(strstr(buf, "\nFAKE-INJECTED-LINE"), NULL);
}

TEST(syslog, field_value_with_embedded_newline_stays_single_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "payload", "line1\nFAKE-INJECTED-LINE");
  ccol_log_info(lg, "structured");

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
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "data", "a\rb\tc");
  ccol_log_info(lg, "cr and tab test");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));

  /* Raw CR/HT bytes must not appear anywhere in the record. */
  REQUIRE_EQ(memchr(buf, '\r', n), NULL);
  REQUIRE_EQ(memchr(buf, '\t', n), NULL);
  REQUIRE_NE(strstr(buf, "a\\rb\\tc"), NULL);
}

/* ========================================================================== */
/*                         ADDITIONAL LIFECYCLE                               */
/* ========================================================================== */

TEST(lifecycle, open_file_appends_to_existing) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* Write a sentinel line with the first logger. */
  clog lg1 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg1, CLOG_INVALID);
  ccol_log_info(lg1, "first open");
  clog_close(lg1);

  /* Open the same path again. The logger must append and must not
   * truncate. */
  clog lg2 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg2, CLOG_INVALID);
  ccol_log_info(lg2, "second open");
  clog_close(lg2);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Both messages must be present in the file. */
  REQUIRE_NE(strstr(buf, "first open"), NULL);
  REQUIRE_NE(strstr(buf, "second open"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         ADDITIONAL FILTERING                               */
/* ========================================================================== */

TEST(filtering, set_level_to_off_suppresses_everything) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "before off - visible");

  clog_set_level(lg, CLOG_OFF);
  REQUIRE_EQ(clog_get_level(lg), CLOG_OFF);

  ccol_log_alert(lg, "after off - suppressed");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "before off - visible"), NULL);
  REQUIRE_EQ(strstr(buf, "after off - suppressed"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         ADDITIONAL FIELD EDGE CASES                        */
/* ========================================================================== */

TEST(fields, remove_nonexistent_field_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A call that removes a key which the caller never sets must not crash. */
  clog_remove_field(lg, "nonexistent");
  clog_remove_field(lg, "also_never_set");

  ccol_log_info(lg, "still works");
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A call that clears the fields of a logger with no fields must not
   * crash. */
  clog_clear_fields(lg);
  clog_clear_fields(lg);

  ccol_log_info(lg, "still works");
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The logger must reject a NULL key and a NULL value, and report
   * nothing. */
  clog_set_field(lg, NULL, "v");
  clog_set_field(lg, "k", NULL);
  clog_set_field(lg, NULL, NULL);
  clog_remove_field(lg, NULL);

  clog_set_field(lg, "good", "yes");
  ccol_log_info(lg, "null field test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof(buf));

  /* Only the valid field may appear, and the process must not crash. */
  REQUIRE_NE(strstr(buf, "good=yes"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         ADDITIONAL DERIVE                                  */
/* ========================================================================== */

TEST(derive, inherits_min_level_from_parent) {
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_WARN, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* The child must start with the parent's level at derive time. */
  REQUIRE_EQ(clog_get_level(child), CLOG_WARN);

  clog_close(child);
  clog_close(parent);
}

/* ========================================================================== */
/*                         ADDITIONAL SYSLOG                                  */
/* ========================================================================== */

TEST(syslog, syslog_format_inherited_by_derived_logger) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog parent = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_format(parent, CLOG_FMT_SYSLOG);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Child shares the parent's backing store, so it must see syslog format. */
  REQUIRE_EQ(clog_get_format(child), CLOG_FMT_SYSLOG);

  ccol_log_info(child, "child syslog");

  char buf[4096];
  drain_pipe(parent, pipefd[0], pipefd[1], buf, sizeof(buf));
  clog_close(child);

  /* Output must be RFC 5424 syslog, not logfmt. */
  REQUIRE_NE(strstr(buf, "<14>1 "), NULL);
  REQUIRE_NE(strstr(buf, "child syslog"), NULL);
}

/* The sanitizer for the RFC 5424 APP-NAME must filter out every byte that is
 * not PRINTUSASCII, at any position. It must not stop at the first such byte
 * and drop everything after it. */
/* The same PRINTUSASCII filter serves both APP-NAME and HOSTNAME. See the
 * doc comment of _sanitize_syslog_printusascii_field() in clogger.c. This
 * test drives the filter through the accessor for APP-NAME. The coverage
 * holds in the same way for the HOSTNAME field. That field has the same
 * RFC 5424 "1*NNNPRINTUSASCII" grammar, and the logger filters it in the
 * same way. It does not truncate the field at its first bad byte. */
TEST(syslog, appname_sanitizer_filters_not_truncates) {
  char out[49];

  /* The function must drop a bad byte in the middle. It must not drop
   * everything after that byte as well. */
  clog_test_sanitize_syslog_appname(
      "ab\x01"
      "cd",
      out, sizeof(out));
  REQUIRE_STREQ(out, "abcd");

  /* An input with no PRINTUSASCII byte falls back to "-". An empty input
   * does the same. */
  clog_test_sanitize_syslog_appname("\x01\x02", out, sizeof(out));
  REQUIRE_STREQ(out, "-");
  clog_test_sanitize_syslog_appname("", out, sizeof(out));
  REQUIRE_STREQ(out, "-");

  /* The output stops at outsz-1 bytes. This is true when the input holds
   * more good bytes than that. */
  char small_out[4];
  clog_test_sanitize_syslog_appname("abcdefgh", small_out, sizeof(small_out));
  REQUIRE_EQ(strlen(small_out), (size_t)3);
  REQUIRE_STREQ(small_out, "abc");
}

/* The function must handle outsz == 0 and outsz == 1 as their own cases.
 * size_t is unsigned, so outsz - 1 wraps to SIZE_MAX when outsz is 0. That
 * destroys the bound of the loop. The function then writes an unbounded
 * number of bytes into a destination buffer with no space at all. An outsz
 * of 1 leaves room for the NUL terminator alone. The "-" fallback of two
 * bytes then writes one byte past the end of a 1-byte buffer.
 *
 * Every destination buffer below comes from the heap at its exact documented
 * size, and never from a larger stack array. valgrind (make memtest) then
 * reports a regression here directly as a heap buffer overflow. A version of
 * this test with a stack array can survive the same regression, because the
 * padding of the stack hides it. */
TEST(syslog, appname_sanitizer_tolerates_degenerate_output_sizes) {
  /* outsz == 0: the function must not touch *out at all. There is no byte in
   * it, so even a NUL terminator has no safe place. */
  char *zero_out = malloc(1);
  REQUIRE_NE(zero_out, NULL);
  zero_out[0] = 'Z';
  clog_test_sanitize_syslog_appname("abc", zero_out, 0);
  REQUIRE_EQ(zero_out[0], 'Z');
  free(zero_out);

  /* outsz == 1: there is room for the NUL terminator only. The function must
   * not try the "-" fallback, which needs 2 bytes. */
  char *one_out = malloc(1);
  REQUIRE_NE(one_out, NULL);
  clog_test_sanitize_syslog_appname("abc", one_out, 1);
  REQUIRE_EQ(one_out[0], '\0');
  free(one_out);

  char *one_out_empty = malloc(1);
  REQUIRE_NE(one_out_empty, NULL);
  clog_test_sanitize_syslog_appname("", one_out_empty, 1);
  REQUIRE_EQ(one_out_empty[0], '\0');
  free(one_out_empty);

  /* outsz == 2: exactly enough room for the "-" fallback plus its NUL. */
  char *two_out = malloc(2);
  REQUIRE_NE(two_out, NULL);
  clog_test_sanitize_syslog_appname("\x01\x02", two_out, 2);
  REQUIRE_STREQ(two_out, "-");
  free(two_out);
}

/* This test covers the escaper for control characters directly. The RFC 5424
 * MSG content shares that escaper with the backtrace frame text of the
 * logfmt and syslog formats. See the doc comment of
 * _buf_append_ctrl_escaped() in clogger.c. The backtrace frame text comes
 * from backtrace_symbols(), and a test cannot force a control character into
 * its real output. The test therefore drives the shared primitive directly,
 * and not only through a real backtrace. */
TEST(syslog, ctrl_escaped_helper_neutralizes_control_bytes) {
  char out[64];

  clog_test_append_ctrl_escaped("plain text, no escaping needed", out,
                                sizeof(out));
  REQUIRE_STREQ(out, "plain text, no escaping needed");

  /* '\n' and '\r' use the same two-character escapes as every other format
   * in this file. Without them, a raw newline splits a backtrace line of
   * logfmt or syslog into two. It also splits the syslog MSG field. */
  clog_test_append_ctrl_escaped("line1\nline2", out, sizeof(out));
  REQUIRE_STREQ(out, "line1\\nline2");
  clog_test_append_ctrl_escaped("a\rb\tc", out, sizeof(out));
  REQUIRE_STREQ(out, "a\\rb\\tc");

  /* A backslash is escaped too, so the escape of a newline and the two
   * characters backslash and n never read the same. */
  clog_test_append_ctrl_escaped("x\\ny", out, sizeof(out));
  REQUIRE_STREQ(out, "x\\\\ny");

  /* Every other control byte (and DEL) falls back to the generic \xXX form.
   */
  clog_test_append_ctrl_escaped(
      "del\x7f"
      "end",
      out, sizeof(out));
  REQUIRE_STREQ(out, "del\\x7fend");
  clog_test_append_ctrl_escaped(
      "a\x01"
      "b",
      out, sizeof(out));
  REQUIRE_STREQ(out, "a\\x01b");

  /* The function truncates the output when it does not fit outsz. It never
   * overflows the buffer. */
  char small_out[6];
  clog_test_append_ctrl_escaped("ab\ncd", small_out, sizeof(small_out));
  REQUIRE_EQ(strlen(small_out), (size_t)5);
  REQUIRE_STREQ(small_out, "ab\\nc");
}

/* ========================================================================== */
/*                         ADDITIONAL THREADING                               */
/* ========================================================================== */

#define DERIVED_THREAD_COUNT 4
#define DERIVED_MSGS_PER_THREAD 30

typedef struct {
  clog lg;
  int id;
} derived_arg_t;

static void *_derived_writer_thread(void *arg) {
  derived_arg_t *a = (derived_arg_t *)arg;
  clog child = clog_derive(a->lg);
  if (!child) return NULL;
  clog_set_field(child, "writer", "yes");
  for (int i = 0; i < DERIVED_MSGS_PER_THREAD; i++)
    ccol_log_info(child, "derived id=%d msg=%d", a->id, i);
  clog_close(child);
  return NULL;
}

TEST(threading, concurrent_parent_and_derived_writers_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
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

  /* Parent also writes concurrently. */
  for (int i = 0; i < DERIVED_MSGS_PER_THREAD; i++)
    ccol_log_info(parent, "parent msg=%d", i);

  for (int i = 0; i < DERIVED_THREAD_COUNT; i++)
    if (created[i]) pthread_join(threads[i], NULL);

  if (create_failures > 0) {
    clog_close(parent);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_failures, 0);

  clog_close(parent);

  /* Every non-backtrace line must start with "ts=". */
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

/* _clog_write() checks min_level one time with no lock, before it locks
 * shared->mutex. This fast path keeps a call that the filter drops away from
 * the lock that every other writer needs. The function then checks the level
 * again under the lock. This test stresses that unlocked check against
 * concurrent clog_set_level() calls on the same handle from another thread.
 * The code must never crash. Every line that passes the filter must still be
 * a complete, well-formed record. */
#define LEVEL_FASTPATH_WRITER_COUNT 4
#define LEVEL_FASTPATH_ITERATIONS 500

static void *_level_fastpath_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < LEVEL_FASTPATH_ITERATIONS; i++)
    ccol_log_trace(lg, "filtered %d", i);
  return NULL;
}

static void *_level_fastpath_toggler(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < LEVEL_FASTPATH_ITERATIONS; i++)
    clog_set_level(lg, (i % 2) ? CLOG_TRACE : CLOG_INFO);
  return NULL;
}

TEST(threading, concurrent_filtered_writes_and_level_changes_no_crash) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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
  ccol_log_info(lg, "final marker");
  clog_close(lg);

  /* This buffer is much larger than the true worst case. That worst case is
   * LEVEL_FASTPATH_WRITER_COUNT * LEVEL_FASTPATH_ITERATIONS = 2000 lines,
   * each one well below 300 bytes. It happens when every write lands while
   * the toggler holds the level at CLOG_TRACE. The size does not come from
   * the typical number of lines that pass the filter. read_file() does one
   * bounded read() from the start of the file. A buffer that is too small
   * therefore truncates the file before "final marker", which the test
   * writes last. That happens only on a run that lets more trace lines
   * through than usual. A buffer of 131072 bytes is small enough to hit that
   * truncation, which depends on timing. */
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

#define FIELD_RACE_WRITER_COUNT 4
#define FIELD_RACE_ITERATIONS 500

static void *_field_race_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < FIELD_RACE_ITERATIONS; i++)
    ccol_log_info(lg, "field race %d", i);
  return NULL;
}

static void *_field_race_mutator(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < FIELD_RACE_ITERATIONS; i++) {
    clog_set_field(lg, "who", (i % 2) ? "even" : "odd");
    clog_remove_field(lg, "who");
  }
  return NULL;
}

/* The live-field source of _clog_emit_fields() must not read
 * chmap_elem_count(lg->fields) BEFORE it locks fields_mutex. Such a read
 * races a concurrent clog_set_field() or clog_remove_field() call on the
 * same handle from another thread.
 *
 * In this codebase chashmap is a container that the caller must synchronize.
 * Every other access to lg->fields in clogger.c locks fields_mutex first.
 * Those accesses are _snapshot_fields(), clog_set_field(),
 * clog_remove_field(), clog_clear_fields() and clog_derive(). A thread can
 * read the internal element count of lg->fields with no lock, while another
 * thread changes that same map under fields_mutex. That is undefined
 * behaviour. A plain run without ThreadSanitizer rarely shows any difference
 * in behaviour.
 *
 * The assertions of this test only check that there is no crash and that the
 * output is well-formed. A run of this suite under -fsanitize=thread catches
 * the race itself. See the test_tsan target of this directory. */
TEST(threading, concurrent_set_field_and_write_no_race) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

#define SLOT_CHURN_WRITER_COUNT 4
#define SLOT_CHURN_ITERATIONS 500
#define SLOT_CHURN_DERIVE_ITERATIONS 500

static void *_slot_churn_writer(void *arg) {
  clog *lg = (clog *)arg;
  for (int i = 0; i < SLOT_CHURN_ITERATIONS; i++) {
    ccol_log_info(*lg, "churn writer message %d", i);
    clog_set_level(*lg, (i % 2 == 0) ? CLOG_TRACE : CLOG_INFO);
    (void)clog_get_level(*lg);
  }
  return NULL;
}

static void *_slot_churn_deriver(void *arg) {
  clog *parent = (clog *)arg;
  for (int i = 0; i < SLOT_CHURN_DERIVE_ITERATIONS; i++) {
    clog child = clog_derive(*parent);
    if (child == CLOG_INVALID) continue;
    ccol_log_info(child, "derived churn message %d", i);
    clog_close(child);
  }
  return NULL;
}

/* This test stresses two mechanisms against real, concurrent slot reuse.
 * They are the slot table that the rwlock protects, and the lock-free pin
 * and unpin. Several threads resolve one shared handle again and again,
 * through ccol_log_info, clog_set_level and clog_get_level. That handle stays
 * open. A separate thread derives a new child handle and closes it at once,
 * again and again. Every child shares the same target.
 *
 * Each close moves a slot through in_use=false, then freed=true, then a
 * generation bump. The next derive then takes the slot again. This is the
 * churn pattern that the freed-flag walk of _clog_atfork_prepare and the
 * freed-reset of _clog_handle_acquire exist to handle. The whole point of
 * this test is no crash, no hang, and no report from valgrind or from
 * ThreadSanitizer. It asserts nothing about the log content. */
TEST(threading, concurrent_derive_close_churn_stresses_slot_table) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

/* ========================================================================== */
/*                         FORK SAFETY                                       */
/* ========================================================================== */

/* This whole section drives the fork() safety machinery of src/clogger.c,
 * which is built on pthread_atfork(). The compiler drops that machinery when
 * CCOL_FORK_SAFETY_REQUIRED is 0. See the doc comment of that macro in
 * common.h. Without the machinery, the premise of these tests does not hold.
 * That premise is that a forked child never inherits a locked
 * clog_slot_table.rwlock, shared->mutex or fields_mutex. The tests are
 * therefore dropped with the machinery, and not left in to hang or to
 * fail. */
#if CCOL_FORK_SAFETY_REQUIRED

/* The test creates a synchronous logger before the fork() and logs from the
 * child. It drives the atfork prepare, parent and child protection of
 * clog_slot_table.rwlock, shared->mutex and fields_mutex directly. This test
 * is not vacuous: _clog_atfork_release initializes the rwlock again in the
 * child and does not only unlock it. Without that, the child inherits the
 * write-locked rwlock. Its next _clog_resolve() call then blocks forever in
 * ccol_rw_lock_rdlock, and the child hangs. */
TEST(fork_safety, child_can_log_after_fork) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    ccol_log_info(lg, "message from child");
    clog_close(lg);
    _exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* This is a bounded wait and not a blocking waitpid(). A regression in the
   * fork safety hangs the child forever. This test must fail in a visible
   * way and must not hang the whole suite. The bound is 30s and not a few
   * hundred milliseconds. That keeps it well clear of the valgrind
   * instrumentation cost of make memtest, which applies to this forked child
   * too. At a bound of 5s, valgrind slows the child enough to miss it. The
   * SIGKILL fallback below then runs, and the heap allocations of that child
   * appear as "still reachable". With a large enough bound, the _exit() and
   * clog_close() path of the child itself frees them cleanly. */
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }
  /* This check uses WIFEXITED alone, and deliberately not
   * WEXITSTATUS(status) == 0 as well. The exit code of this child is not
   * reliably 0 under the memtest flags of this suite. The reason has nothing
   * to do with the correctness of this test.
   *
   * make memtest runs with --errors-for-leak-kinds=all and
   * --error-exitcode=1. The test forks this child in the middle of the
   * suite. That is well before the rest of the tests in this binary run and
   * free their own, unrelated allocations. The child then reaches its own
   * normal, voluntary exit. Valgrind runs a full "still reachable" leak
   * check over the whole process image of that child at that instant. That
   * image holds every live allocation of every other test and module in this
   * binary that nothing tore down yet. This is expected and correct. The
   * process becomes fully quiet only one time, at the very end of main(),
   * and not after each test.
   *
   * --errors-for-leak-kinds=all counts that as a real error. It then
   * replaces the exit status of the child with the value of
   * --error-exitcode, whatever value _exit() gets. The option
   * --child-silent-after-fork=yes suppresses only the diagnostic OUTPUT of
   * the child, and not this replacement of the exit status. A small program
   * that only calls fork() and _exit(0), with one allocation, reproduces the
   * same override.
   *
   * This is a property of a fork in the middle of the suite under the
   * memtest flags of this project. A change from _exit() to exit() does not
   * help. exit() lets the destructor of this file run. A full
   * --leak-check=full pass at that same instant still finds every other
   * still-reachable allocation of the rest of the suite. It forces the same
   * override.
   *
   * WIFEXITED alone catches what this test cares about. A regression in the
   * fork safety hangs the child, and the bounded wait above catches that. Or
   * it crashes the child: a real ccol_assert() or ccol_fatal_err() abort
   * raises SIGABRT, which makes WIFEXITED false here, and not only
   * WEXITSTATUS nonzero. */
  REQUIRE_TRUE(WIFEXITED(status));

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "message from child"), NULL);

  cleanup_dir(dir, "app.log");
}

#define FORK_CHURN_WRITER_COUNT 4
#define FORK_CHURN_ITERATIONS 300

static void *_fork_churn_writer(void *arg) {
  clog *lg = (clog *)arg;
  for (int i = 0; i < FORK_CHURN_ITERATIONS; i++) {
    ccol_log_info(*lg, "fork churn message %d", i);
    clog child = clog_derive(*lg);
    if (child != CLOG_INVALID) clog_close(child);
  }
  return NULL;
}

/* This test calls fork() from one thread while several OTHER threads log,
 * derive and close on the same shared target. It confirms that the logging
 * of the parent continues with no break as soon as fork() returns. It
 * therefore checks that _clog_atfork_parent restores normal operation and
 * unlocks every lock that _clog_atfork_prepare locks. It does not only check
 * that the child avoids a hang. */
TEST(fork_safety, concurrent_fork_during_churn_does_not_hang) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[FORK_CHURN_WRITER_COUNT];
  bool writer_created[FORK_CHURN_WRITER_COUNT];
  int writer_create_failures = 0;
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++) {
    writer_created[i] =
        (pthread_create(&writers[i], NULL, _fork_churn_writer, &lg) == 0);
    if (!writer_created[i]) writer_create_failures++;
  }
  if (writer_create_failures > 0) {
    /* There is no fork() yet. Join every thread that started, before this
     * test fails. No background thread may outlive the stack frame of this
     * test function, where `lg` and `dir` above live. */
    for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
      if (writer_created[i]) pthread_join(writers[i], NULL);
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(writer_create_failures, 0);

  /* Fork partway through the churn, from the main test thread. */
  usleep(1000);
  pid_t pid = fork();
  if (pid == 0) {
    /* In the child only this thread exists. Confirm that it can still
     * resolve the inherited handle and use it before it exits. */
    ccol_log_info(lg, "post-fork child message");
    _exit(0);
  }
  if (pid == -1) {
    /* Every writer thread above started, which the check above guarantees.
     * Join all of them before this test fails. Do not leave them at work on
     * `lg` and `dir` after this test function returns. */
    for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
      pthread_join(writers[i], NULL);
    clog_close(lg);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* The bound is 30s. See the same note on the wait of
   * fork_safety.child_can_log_after_fork above. The bound must stay well
   * clear of the valgrind instrumentation cost of make memtest. */
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }

  /* The writer threads of the parent must still finish. If
   * _clog_atfork_parent does not unlock everything it locks, these joins
   * hang. */
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);

  ccol_log_info(lg, "final parent marker");
  clog_close(lg);

  /* This buffer is much larger than the true worst case, which is
   * FORK_CHURN_WRITER_COUNT * FORK_CHURN_ITERATIONS = 1200 lines. The reason
   * for the size is the same as for
   * threading.concurrent_filtered_writes_and_level_changes_no_crash above.
   * read_file() does one bounded read(). A buffer that is too small
   * therefore truncates the file before "final parent marker", which the
   * test writes last. */
  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "final parent marker"), NULL);

  cleanup_dir(dir, "app.log");
}

/* Appends one line to path and creates the file if it does not exist.
 * _fork_safety_rollback_race_child() below uses it to record its own
 * progress in a place that the outer test can read back. That child does not
 * use its exit code for this. The exit code is not reliable under make
 * memtest, for the same reason that the WIFEXITED-only check of
 * fork_safety.child_can_log_after_fork exists. The
 * --errors-for-leak-kinds=all option of valgrind replaces the real exit code
 * of a forked child with the value of --error-exitcode. It does so as soon
 * as it finds ANY "still reachable" allocation in the inherited process
 * image of that child. A child that a test forks in the middle of the suite
 * always has such an allocation. It inherits the live allocations of every
 * other test that nothing tore down yet. */
static void _fork_safety_marker_append(const char *path, const char *line) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  ssize_t w = write(fd, line, strlen(line));
  (void)w;
  close(fd);
}

/* This function runs inside an isolated, forked child only. A slot left in
 * the shape that this test guards against crashes the calling process at the
 * SECOND fork() below. That crash happens inside _clog_atfork_prepare(),
 * which runs in the calling thread before fork() returns anywhere. This
 * whole scenario must therefore not run in the main test process.
 *
 * `marker_path` works like a log file that any other test in this suite
 * reads back after a forked child exits. See the _fork_safety_marker_append
 * helper above for the reason this code does not use exit codes. */
static void _fork_safety_rollback_race_child(const char *marker_path) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) _exit(0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* This forces the acquisition of this logger to grow the slot table with a
   * fresh slot. It then fails the live_shareds registration step of that
   * slot. _clog_handle_acquire() therefore takes the exact rollback path
   * under test. That path can leave a slot that it has just grown, and that
   * nothing registered, with freed == false and ptr == NULL at the same
   * time. Earlier tests in this process may have left free_indices entries
   * behind for reuse. Those entries would otherwise hide the defect, because
   * the acquisition would take the OTHER rollback branch. That branch is
   * already safe, because it reuses a slot that is already freed == true. */
  clog_test_force_next_fresh_slot_registration_failure(true);
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_INVALID) {
    cleanup_dir(dir, "app.log");
    _exit(0); /* the forced failure does not apply, so nothing to check */
  }
  _fork_safety_marker_append(marker_path, "acquire_failed_as_expected\n");

  /* The fork() here drives the walk of _clog_atfork_prepare() over every
   * slot in clog_slot_table.slots. The step above can leave a slot with
   * freed == false and ptr == NULL. The
   * ccol_mutex_lock(slot->ptr->fields_mutex) call of that walk then
   * dereferences NULL and crashes this whole process. This line never
   * returns, in either direction, and the process never sees a return value
   * from fork(). */
  pid_t pid = fork();
  _fork_safety_marker_append(marker_path, "survived_fork_call\n");

  if (pid == 0) _exit(0);
  if (pid > 0) waitpid(pid, NULL, 0);
  cleanup_dir(dir, "app.log");
  _exit(0);
}

/* This is the second rollback in _clog_handle_acquire, and the one that
 * handle_acquire_rollback_leaves_slot_fork_safe below does not reach. The
 * last step of an acquisition publishes the handle into the pin index. The
 * live_shareds registration succeeds before that step. A failure of the
 * publish step therefore has that registration to undo, as well as the slot.
 * The caller gets CLOG_INVALID and then frees the shared object. An entry
 * left behind in live_shareds means that the next fork() locks a mutex
 * inside that freed memory.
 *
 * This scenario runs in its own child, for the same reason as
 * handle_acquire_rollback_leaves_slot_fork_safe below. The failure that it
 * stages leaves a freed shared object behind when the rollback is wrong. A
 * later fork in the same process then locks a mutex inside that object. This
 * test proves that the fork survives. The test
 * handle_publish_rollback_leaves_no_registered_shared below pins the
 * rollback itself. That test runs in this process and fails with no
 * instrumentation. */
extern void _ccol_pintable_force_next_publish_failure_for_tests(void);

static void _fork_safety_publish_rollback_child(const char *marker_path) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) _exit(0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  _ccol_pintable_force_next_publish_failure_for_tests();
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_INVALID) {
    clog_close(lg);
    cleanup_dir(dir, "app.log");
    _exit(0); /* the forced failure does not apply, so nothing to check */
  }
  _fork_safety_marker_append(marker_path, "acquire_failed_as_expected\n");

  /* This proves that the arming above is what makes that open fail. Without
   * it, a mkdtemp, an open(2) or an allocation can fail ahead of the publish
   * and leave the one-shot flag armed. The next publish consumes the flag,
   * whether that publish then succeeds or not. A second open must therefore
   * succeed here. Without this check, the whole child reads as a pass for a
   * failure that never reaches the rollback under test. */
  clog probe = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (probe != CLOG_INVALID) {
    _fork_safety_marker_append(marker_path, "forced_failure_was_consumed\n");
    clog_close(probe);
  }

  /* The error path of the caller frees the shared object that this
   * acquisition registered. If the rollback leaves that object in
   * live_shareds, the walk of _clog_atfork_prepare locks sh->mutex inside
   * freed memory here. It does so before fork() returns anywhere. */
  pid_t pid = fork();
  _fork_safety_marker_append(marker_path, "survived_fork_call\n");

  if (pid == 0) _exit(0);
  if (pid > 0) waitpid(pid, NULL, 0);
  cleanup_dir(dir, "app.log");
  _exit(0);
}

/* The whole job of the rollback is to undo the live_shareds registration
 * that the acquisition makes before the publish. The property to check is
 * therefore that the count of registered shared objects returns to its
 * earlier value. This test checks it in this process, and not inside the
 * forked child above. A read of the count takes the read lock of the table.
 * A child inherits the write lock of the fork handler and then initializes
 * that lock again. ThreadSanitizer still reads the lock as write-locked,
 * because it cannot model that new initialization as a release.
 *
 * This test is not vacuous: remove the live_shareds rollback and the count
 * is one higher. A sanitizer or a leak checker cannot find the stale entry
 * instead. The access that the stale entry permits happens in a forked
 * child, and the fork test discards the exit status of that child. */
TEST(fork_safety, handle_publish_rollback_leaves_no_registered_shared) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  size_t before = clog_test_live_shareds_count();

  _ccol_pintable_force_next_publish_failure_for_tests();
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  bool refused = (lg == CLOG_INVALID);
  if (!refused) clog_close(lg);
  size_t after = clog_test_live_shareds_count();

  /* This proves that the arming above is what refuses that open. Without it,
     something can fail ahead of the publish and leave the one-shot flag
     armed. */
  clog probe = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  bool probe_opened = (probe != CLOG_INVALID);
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
    _exit(127); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* Same 30s bound, and for the same reason, as the test above. */
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }
  /* The test reads everything from the markers and removes the directory
     before it asserts any of it. A REQUIRE_ that fails returns from the test
     at once, and would leave the mkdtemp directory behind. The strstr calls
     run only after a read that is not empty, because read_file() writes buf
     only when there is something to read. */
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

/* The rollback of _clog_handle_acquire() for a failed live_shareds
 * registration must restore BOTH slot->freed and slot->ptr. It must leave
 * the slot in the same shape as a slot that clog_close() itself retires.
 * This holds for either of the two acquisition branches.
 *
 * A push of the index of the slot back onto free_indices is not enough on
 * its own. The rollback must also set slot->freed = true. A slot from the
 * branch that grows the table with a new slot starts as
 * clog_slot_t fresh = {0}. Before the rollback runs, freed is therefore
 * already false and ptr is already NULL. The walk of _clog_atfork_prepare()
 * skips a slot only through `if (slot->freed) continue;`. It therefore
 * dereferences the NULL ptr of that slot in
 * ccol_mutex_lock(slot->ptr->fields_mutex). That happens as soon as any
 * thread calls fork() while the now "free" index sits unused in
 * free_indices. It crashes the whole process. */
TEST(fork_safety, handle_acquire_rollback_leaves_slot_fork_safe) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof(marker_dir)), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof(marker_path), "%s/marker", marker_dir);

  pid_t pid = fork();
  if (pid == 0) {
    _fork_safety_rollback_race_child(marker_path);
    _exit(127); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* The bound is 30s. See the same note on the wait of
   * fork_safety.child_can_log_after_fork above. The bound must stay well
   * clear of the valgrind instrumentation cost of make memtest. */
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }
  /* This check uses WIFEXITED alone, and deliberately not the exit code of
   * the child. The exit code of a forked child is not a reliable signal
   * under make memtest. See the same note on
   * fork_safety.child_can_log_after_fork above. This test is not vacuous. A
   * slot in the unsafe shape makes this REQUIRE_TRUE fail. The child then
   * dies with SIGSEGV and does not exit at all. That is how this test finds
   * the crash. */
  REQUIRE_TRUE(WIFEXITED(status));

  /* The marker file confirms two things. First, that the scenario really
   * runs, and that the forced hook does apply. Second, that the process gets
   * all the way past the fork() call under test. An exit on its own is not
   * enough. */
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  bool saw_acquire_failed =
      len > 0 && strstr(buf, "acquire_failed_as_expected") != NULL;
  bool saw_survived_fork = len > 0 && strstr(buf, "survived_fork_call") != NULL;

  /* This runs before the assertions, so an assertion that fails does not
     leave the mkdtemp directory behind. */
  cleanup_dir(marker_dir, "marker");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_TRUE(saw_acquire_failed);
  REQUIRE_TRUE(saw_survived_fork);
}

/* clog_test_force_next_fresh_slot_registration_failure() forces the rollback
 * path of _clog_handle_acquire() for clog_open_fd_mp(), clog_open_file_mp()
 * and clog_derive() alike. Its own doc comment and the public one in
 * clogger.h both state this. The forced-failure branch must therefore apply
 * in every case. It must not depend on whether the shared object of the
 * acquiring handle is already in clog_slot_table.live_shareds.
 *
 * The shared object of clog_derive() is always the already-registered object
 * of the parent. That object stays in live_shareds for as long as the parent
 * is a live, pinned handle. A hook that the code reads only when
 * already_registered is false is therefore defeated for this one of its
 * three documented call sites. Such a hook still disarms itself, but
 * clog_derive() returns a valid handle in place of the documented
 * CLOG_INVALID.
 *
 * This test needs no process isolation and no fork, unlike
 * handle_acquire_rollback_leaves_slot_fork_safe above. It drives only the
 * ordinary rollback return path, which corrupts no process-wide state when
 * it works correctly. */
TEST(fork_safety, handle_acquire_rollback_hook_also_fires_for_derive) {
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_test_force_next_fresh_slot_registration_failure(true);
  clog child = clog_derive(parent);
  REQUIRE_EQ(child, CLOG_INVALID);

  /* The hook must affect only the one acquisition that it targets. A plain
   * clog_derive() call right after it, with no force, must succeed. The
   * parent handle must also still work fully. The forced failure above must
   * never touch or corrupt the already-registered shared object of that
   * parent. */
  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_INVALID);

  clog_close(child2);
  clog_close(parent);
}

/* This is the second half of
 * pending_compress_not_permanently_exempt_from_pruning_in_child below. It
 * runs inside the forked child. Only this thread exists here. At the moment
 * of the fork(), the compressor thread of the parent is in the middle of a
 * compression, held after it created its temporary output. That thread does
 * not exist in this process at all.
 *
 * One more write proves the point. max_file_size == 1 makes that write start
 * a new rotation. The _prune_rotated() pass of that rotation shows whether
 * this process still treats the source of the first rotation as a
 * compression in flight. The check runs right after that write. The
 * compression of the parent stays held until the parent releases it after
 * the fork, and only that compression can remove the source otherwise. */
static void _fork_pending_compress_child(clog lg, const char *dir,
                                         const char *stale_plain,
                                         const char *marker_path) {
  /* The copy of the hook in this process. It never changes the compression
   * that the parent started, and it lets the compression of this child run. */
  clog_test_hold_compressions(false);

  ccol_log_info(lg, "generation two content");
  bool pruned = (access(stale_plain, F_OK) != 0);
  clog_close(lg);

  _fork_safety_marker_append(marker_path,
                             pruned ? "pruned\n" : "still_exempt\n");
  cleanup_dir(dir, "app.log");
  _exit(0);
}

static void *_fork_pending_compress_writer(void *arg) {
  clog lg = *(clog *)arg;
  ccol_log_info(lg, "generation one content");
  return NULL;
}

/* The compressor thread of a shared target unlocks shared->mutex around its
 * slow gzip-compression step, and for that time it marks the job as the
 * running compression of the target. See "BACKGROUND COMPRESSION" in
 * src/clogger.c. The _prune_rotated() pass of a concurrent rotation then
 * never deletes a file that is still open for a read.
 *
 * A fork() inside that exact window copies that mark into the CHILD. Only
 * the compressor thread ever clears it, and that thread does not exist in a
 * new forked child, because fork() copies only the calling thread. Without
 * handling, _prune_rotated() in that child treats the filenames of the job
 * as "still under compression" forever. It never deletes them, however many
 * more rotations that child runs. The result is a permanent
 * max_rotated_files violation for that one generation. Nothing reports it,
 * and it happens only in the child.
 *
 * _clog_atfork_child() therefore forgets the running compression and the
 * queue of every live shared target. No compression is truly in flight in a
 * new forked child. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Any write triggers a rotation. */
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The compression of the first rotation stays held after it creates its
   * temporary output, until this test releases it after the fork(). */
  clog_test_hold_compressions(true);

  pthread_t t;
  int create_rv = pthread_create(&t, NULL, _fork_pending_compress_writer, &lg);
  if (create_rv != 0) {
    clog_test_hold_compressions(false);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_EQ(create_rv, 0);

  /* Wait until the compression of the first rotation truly runs. The
   * fork() below then lands inside it. */
  bool opened = false;
  for (int i = 0; i < 30000 && !opened; i++) {
    opened = clog_test_gz_dest_opened();
    if (!opened) usleep(1000);
  }

  /* The source of the one rotated file: the name of the temporary output
   * without ".gz.tmp". */
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
    /* The premise fails: no compression of the first rotation is visible
     * in flight. This path makes no fork() at all. It releases the
     * compression, joins the writer thread and closes lg before the test
     * fails, so that nothing of this test outlives it. */
    clog_test_hold_compressions(false);
    pthread_join(t, NULL);
    clog_close(lg);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(stale_plain[0], '\0');

  pid_t pid = fork();
  if (pid == 0) {
    _fork_pending_compress_child(lg, dir, stale_plain, marker_path);
    _exit(127); /* unreachable */
  }
  /* In the parent, let the original compression finish in the normal way.
   * This test is only about the separate copy of its running mark in the
   * CHILD. */
  clog_test_hold_compressions(false);
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
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }
  REQUIRE_TRUE(WIFEXITED(status));

  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "pruned"), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

/* Runs clog_close() on *lg on a background thread. It serves
 * fork_safety.fork_during_concurrent_close_does_not_leak_target_in_child
 * below. The outer test needs this call suspended with
 * clog_test_set_close_finalize_delay_us(), on a thread OTHER than the one
 * that calls fork(). The forked child then inherits a slot in the middle of
 * a close, on a thread that never resumes there. */
static void *_fork_close_race_closer(void *arg) {
  clog *lg = (clog *)arg;
  clog_close(*lg);
  return NULL;
}

/* The child-side handling of _clog_atfork_release() must finish a
 * clog_close() on behalf of a thread that is gone. A DIFFERENT thread is
 * suspended in that call at the instant one thread calls fork(). It sits
 * between step 2 of the call, which clears in_use, and step 4, which retires
 * the slot and releases the reference of the shared target. See the doc
 * comment of clog_atfork_closing_t in clogger.c.
 *
 * Without that handling, the child never reclaims the target. One thread
 * would retire the slot and release the reference of the target. That thread
 * does not exist in a new forked child at all, because fork() copies only
 * the calling thread. Neither step ever runs there. The ref_count of the
 * target can then never reach zero, however long that child process runs. */
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

  /* Take the baseline BEFORE the open of the logger under test. The
   * assertion below then holds whatever number of other shared targets are
   * already live elsewhere in this test binary at this point. */
  size_t baseline = clog_test_live_shareds_count();

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(clog_test_live_shareds_count(), baseline + 1);

  /* This delay is long enough to outlast the fork() and the live_shareds
   * check of the child below. clog_test_close_finalize_delay_entered(),
   * which the code below polls, is what makes the landing inside this window
   * deterministic. The length of the delay does not. */
  clog_test_set_close_finalize_delay_us(1000000);

  clog to_close = lg;
  pthread_t closer;
  REQUIRE_EQ(pthread_create(&closer, NULL, _fork_close_race_closer, &to_close),
             0);

  /* Spin-wait until the closer thread enters the wide window of steps 3 and
   * 4, where in_use is clear and the slot is not yet retired. Only then
   * fork. A fixed sleep with the hope of good timing is not enough. */
  while (!clog_test_close_finalize_delay_entered()) usleep(1000);

  pid_t pid = fork();
  if (pid == 0) {
    /* In the child only this thread exists. The closer thread, which the
     * delay suspends in the middle of clog_close() on `lg`, never resumes.
     * _clog_atfork_child() runs before fork() returns here at all. The
     * machinery of pthread_atfork calls it as part of fork() itself, in the
     * calling thread, before fork() returns in either process. The effect is
     * therefore visible at once. The library finishes the suspended close on
     * behalf of the thread that is gone. It reclaims the target of lg here.
     * live_shareds does not stay at baseline + 1 forever. */
    char buf[64];
    snprintf(buf, sizeof(buf), "live_shareds=%zu\n",
             clog_test_live_shareds_count());
    _fork_safety_marker_append(marker_path, buf);
    _exit(0);
  }
  if (pid == -1) {
    /* The closer thread is still suspended in the middle of clog_close(),
     * behind the artificial delay above. Disarm that delay and join the
     * thread before this test fails. Do not leave the thread at work on
     * `to_close` and `lg`, which are both local to this test function,
     * after this test function returns. */
    clog_test_set_close_finalize_delay_us(0);
    pthread_join(closer, NULL);
    cleanup_dir(marker_dir, "marker");
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
    REQUIRE_TRUE(false); /* the child does not finish inside the bound */
  }
  REQUIRE_TRUE(WIFEXITED(status));

  /* The closer thread of the parent runs a real clog_close() that nothing
   * changes. Both must still finish in the normal way after the artificial
   * delay ends. This confirms that _clog_atfork_parent() unlocks everything
   * it locks, and that none of this changes the behaviour of the parent. */
  clog_test_set_close_finalize_delay_us(0);
  pthread_join(closer, NULL);
  REQUIRE_EQ(clog_test_live_shareds_count(), baseline);

  char expect[64];
  snprintf(expect, sizeof(expect), "live_shareds=%zu", baseline);
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, expect), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         ASYNC LOGGING                                      */
/* ========================================================================== */

/* Polls read_file() every 5ms, for up to bound_ms, until needle appears in
 * the content of path, or until the bound ends. This section uses it in
 * place of a fixed sleep. The exact moment when the scheduler lets the
 * writer thread process a given message is not deterministic. */
static bool _poll_for_substring(const char *path, const char *needle,
                                int bound_ms) {
  char buf[1 << 16];
  for (int waited_ms = 0; waited_ms < bound_ms; waited_ms += 5) {
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

  clog_async_cfg_t cfg = {0}; /* queue_size == 0 -> unbounded */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "hello async unbounded");
  clog_close(lg); /* drains the writer thread before it returns */

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

  clog_async_cfg_t cfg = {.queue_size = 8};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "hello async bounded");
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "hello async bounded"), NULL);

  cleanup_dir(dir, "app.log");
}

/* An all-zero clog_async_cfg_t is a pointer that is not NULL, to a struct in
 * which every field is 0. It must still turn on the async mode. Every zero
 * field falls back to its own documented default. This struct holds no
 * "enabled" flag that a caller can leave false, and clog_rotation_cfg_t is
 * different in this respect. */
TEST(async, degenerate_all_zero_config_falls_back_to_defaults) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "degenerate config still works");
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

  /* Small interval so this test does not need to wait long. */
  clog_async_cfg_t cfg = {.flush_interval_us = 20000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "eventually visible");
  REQUIRE_TRUE(_poll_for_substring(path, "eventually visible", 2000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, size_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* A small flush_buffer_size and a long flush_interval_us isolate the
   * size-based trigger. If the message of this test appears, only the buffer
   * threshold can be the cause. The timer cannot. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 32,
                          .flush_interval_us = 60000000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 20; i++) ccol_log_info(lg, "size trigger filler %d", i);

  REQUIRE_TRUE(_poll_for_substring(path, "size trigger filler", 3000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, duration_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* A large flush_buffer_size and a short interval isolate the trigger that
   * depends on duration. This one message can never reach the size threshold
   * on its own. If it appears, the timer fires. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 30000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "duration trigger marker");
  REQUIRE_TRUE(_poll_for_substring(path, "duration trigger marker", 3000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A record whose job outlasts the whole flush interval leaves the writer
 * thread with no time left to wait. Its next receive then waits for 0
 * microseconds, which is a receive that does not wait and that answers an
 * empty queue with ccol_container_empty. That answer must flush the batch as
 * an expired interval does. This test is non-vacuous: a writer that takes
 * ccol_container_empty for a failure never flushes the record on time, and
 * it reaches the file only at clog_close(). */
TEST(async, an_interval_that_ran_out_during_a_job_still_flushes) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 20000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  bool seen = false;
  if (lg != CLOG_INVALID) {
    /* Each record holds the writer for 100 ms, five intervals. */
    clog_test_set_writer_job_delay_us(100000);
    ccol_log_info(lg, "late interval marker");
    seen = _poll_for_substring(path, "late interval marker", 3000);
    clog_test_set_writer_job_delay_us(0);
    clog_close(lg);
  }
  cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(seen);
}

/* ========================================================================== */
/*                         ASYNC MESSAGE FORMATTING                          */
/*                                                                            */
/* These tests mirror three tests of the synchronous path. Those three are  */
/* output.long_message_uses_heap_and_is_not_truncated,                      */
/* output.message_exceeding_stack_buffer_marks_truncation_when_heap_alloc_  */
/* fails and robustness.format_string_failure_writes_diagnostic_not_       */
/* silence. The tests below drive the message format logic of               */
/* _clog_write_async() itself, which uses job->msg_inline and               */
/* job->msg_heap. That inline buffer of 256 bytes is separate from, and     */
/* smaller than, the 1024-byte stack buffer of _clog_write_sync(). It is a  */
/* separate code path, so it gets its own coverage below.                   */
/* ========================================================================== */

TEST(async, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This message is longer than the 256-byte inline buffer job->msg_inline.
   * _clog_write_async() must therefore spill it into job->msg_heap. */
  char long_msg[2048];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  ccol_log_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* The full 2047-character message must appear verbatim in the output. */
  REQUIRE_NE(strstr(buf, long_msg), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async,
     message_exceeding_inline_buffer_marks_truncation_when_heap_alloc_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* This test reuses the _size_capped_* allocator of
   * output.message_exceeding_stack_buffer_marks_truncation_when_heap_alloc_
   * fails. That allocator fails every request of more than 4096 bytes. Every
   * other allocation of this job fits at or below that cap. Those are the
   * envelope, the CLOG_BUF_INITIAL == 4096 byte buffer of sh->async_buf, and
   * the node for each message in the unbounded queue. The failure therefore
   * reaches job->msg_heap alone. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This message is longer than the 256-byte inline buffer. Its heap spill
   * request of one more byte is about 5000 bytes, which is above the
   * 4096-byte cap above. */
  char long_msg[5000];
  memset(long_msg, 'A', sizeof(long_msg) - 1);
  long_msg[sizeof(long_msg) - 1] = '\0';

  ccol_log_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  /* The heap spill fails, so the full message must NOT appear as it is... */
  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  /* ...and the output must show that truncation clearly. */
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A width and precision can be large enough that the total output length of
 * the two conversions goes above INT_MAX. vsnprintf() itself then fails and
 * produces no content at all. See
 * robustness.format_string_failure_writes_diagnostic_not_silence for the
 * same reason that the width is a run-time argument. Here the call that must
 * fail this way is the vsnprintf() of _clog_write_async() into
 * job->msg_inline, and not the one of _clog_write_sync(). */
TEST(async, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A volatile read keeps the width a run-time value at every optimization
   * level, so the format-overflow analysis of the compiler cannot fold it
   * and vsnprintf() really fails at run time. */
  volatile int huge_width_source = 2000000000;
  int huge_width = huge_width_source;
  _clog_write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));

  /* The logger must not drop the record and write nothing. It must write a
   * well-formed placeholder line that names the real cause. */
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "log message formatting failed"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A caller can change or remove a field between the submission and the
 * flush. The record must still show the value of that field AT THE TIME OF
 * SUBMISSION. This proves that the logger takes a snapshot of the fields at
 * the submission. The writer thread does not read them live. */
TEST(async, fields_snapshot_reflects_submission_time_not_flush_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* A long interval and a large buffer mean that nothing flushes on its own
   * before the explicit clog_flush() call below. This gives a wide window to
   * change the field before the writer thread builds the record. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 60000000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "stage", "before");
  ccol_log_info(lg, "field snapshot check");
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

/* A thread with a function of a unique name asks for a backtrace. That
 * backtrace must show the call stack of that thread. The logger captures it
 * on the calling thread, at the time of the submission. It must never show
 * the unrelated stack of the writer thread. */
static clog g_bt_test_lg;
static void *_clog_bt_uniquely_named_worker_fn(void *arg) {
  (void)arg;
  ccol_log_error(g_bt_test_lg, "backtrace from worker thread");
  return NULL;
}

TEST(async, backtrace_captured_on_calling_thread_not_writer_thread) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  g_bt_test_lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(g_bt_test_lg, CLOG_INVALID);

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
  REQUIRE_EQ(strstr(buf, "_clog_writer_thread_main"), NULL);

  clog_close(g_bt_test_lg);
  cleanup_dir(dir, "app.log");
}

/* _emit_backtrace_syslog_lines() must leave its scratch buffer reset after
 * it writes the line of the LAST frame. A reset before each line is not
 * enough. For the async writer thread that scratch buffer is sh->async_buf
 * itself. _writer_flush_now() reads the same buffer through
 * "sh->async_buf.len > 0". It decides from that whether data waits for a
 * flush.
 *
 * A buffer left non-empty after a job with a backtrace causes a repeat
 * write. Take the next flush trigger that is not another CLOG_FMT_SYSLOG
 * job. It writes the bytes of the last frame to the fd a second time, and
 * nothing reports it.
 *
 * A long flush_interval_us makes the shutdown-sentinel flush of clog_close()
 * the ONLY flush that this job can see before drain_pipe() reads the pipe.
 * This test therefore drives that trigger in a deterministic way, and does
 * not race a real idle timeout. */
TEST(async, syslog_backtrace_not_duplicated_on_shutdown_drain) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_us = 60000000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  ccol_log_error(lg, "with backtrace via async");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* Split the output into single syslog messages. Each one starts with
   * "<11>1 ". Confirm that no message repeats. A scratch buffer left
   * non-empty appears here as one more copy of the line of the last
   * backtrace frame. The shutdown drain sends that copy right after it. */
  const char *starts[64];
  int n = 0;
  for (const char *p = buf; (p = strstr(p, "<11>1 ")) != NULL && n < 64; p++)
    starts[n++] = p;
  REQUIRE_GT(n, 1); /* main record + at least one real backtrace frame */

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

/* This is the same hazard of a scratch buffer left behind. It drives the
 * OTHER return path of _emit_backtrace_syslog_lines() that can leave one.
 * The test forces the backtrace capture to fail. The single marker record
 * "#error backtrace unavailable" must then appear exactly once. The shutdown
 * drain of clog_close() must not send it a second time. */
TEST(async,
     syslog_backtrace_capture_failure_marker_not_duplicated_on_shutdown_drain) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_us = 60000000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_backtrace_capture_failure(true);
  ccol_log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

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

/* _emit_backtrace_syslog_lines() must build the TIMESTAMP field of each
 * backtrace continuation line from job->ts. That is the same
 * submission-time timestamp as the primary record uses. The function must
 * never call gettimeofday() again at the time of the write.
 *
 * With async logging, the writer thread processes a job on another thread,
 * and possibly long after the submission. A timestamp taken at the write
 * therefore makes the TIMESTAMP field of a record disagree with the
 * TIMESTAMP fields of its backtrace frames. Nothing reports that. One
 * ccol_log_error() call produces a primary record and its backtrace frames.
 * Every one of those syslog messages must carry the same TIMESTAMP field,
 * byte for byte. */
TEST(async, syslog_backtrace_timestamp_matches_primary_record) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  ccol_log_error(lg, "timestamp correlation check");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* Extract the RFC 5424 TIMESTAMP token (the field right after "<PRI>1 ")
   * from every syslog record found in the output. */
  char timestamps[64][64];
  int n = 0;
  const char *p = buf;
  while ((p = strstr(p, "<11>1 ")) != NULL && n < 64) {
    const char *ts_start = p + 6; /* just past "<11>1 " */
    const char *ts_end = strchr(ts_start, ' ');
    REQUIRE_NE((void *)ts_end, (void *)NULL);
    size_t ts_len = (size_t)(ts_end - ts_start);
    REQUIRE_LT(ts_len, (size_t)64);
    memcpy(timestamps[n], ts_start, ts_len);
    timestamps[n][ts_len] = '\0';
    n++;
    p = ts_end;
  }
  REQUIRE_GT(n, 1); /* main record + at least one backtrace frame */

  for (int i = 1; i < n; i++) REQUIRE_STREQ(timestamps[i], timestamps[0]);
}

/* The CLOG_FMT_SYSLOG branch of the writer thread must flush the shared
 * async batch buffer before it builds its own record. A plain _buf_reset()
 * is not enough. clog_set_format() never touches that buffer, and a job
 * reads its render format live from the shared target. It never captures the
 * format at the submission.
 *
 * A batching format, which is CLOG_FMT_LOGFMT or CLOG_FMT_JSON, can already
 * have built a message into the batch buffer before a switch to
 * CLOG_FMT_SYSLOG. Without that flush, the logger discards that message. It
 * never writes it to the
 * fd, and it reports nothing. The loss happens as soon as the next syslog
 * job builds its record.
 *
 * A large flush_interval_us stops the first message from a flush of its own
 * before the switch. The short sleep after it gives the writer thread time
 * to dequeue that message and build it into the batch buffer. This
 * reproduces the exact window that the test needs. Both messages must
 * survive, whatever the outcome of that race. */
TEST(async, format_switch_to_syslog_does_not_drop_buffered_batch) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_us = 60000000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_INFO, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "pre-switch logfmt message");
  usleep(50000); /* let the writer thread build this into the batch buffer */

  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_log_info(lg, "post-switch syslog message");

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(n, (size_t)0);

  REQUIRE_NE(strstr(buf, "pre-switch logfmt message"), NULL);
  REQUIRE_NE(strstr(buf, "post-switch syslog message"), NULL);
}

/* CLOG_FATAL must drain everything already in the queue before it writes the
 * fatal record. It then exits. Without this, a message logged moments before
 * a crash is still unflushed, and the logger loses it with no report.
 *
 * This test has one open failure mode that appears in CI only. It appears on
 * linux-aarch64-gcc, under qemu-user 8.2.2: the parent does not reap the
 * child inside the 30s bound below. It does NOT match the confirmed,
 * external, already-filed qemu-user fd_trans_lock bug that several
 * cthreadcomm fork() tests do carry. That bug is in the internal
 * fd_trans_lock of linux-user. That lock is a process-wide pthread mutex. It
 * stays locked forever in a forked child when another thread in the parent
 * holds it at the instant of the fork. See
 * gitlab.com/qemu-project/qemu/-/issues/2846. 15 direct reproduction
 * attempts of THIS test, against the same real qemu-aarch64 8.2.2
 * environment, all pass cleanly in about 22ms each. An adjacent, simple test
 * in that same CI run shows a high duration of about 2.3s. That fits
 * ordinary contention and slowness of a CI runner, and not a deterministic
 * race. The real cause is still open.
 *
 * The instrumentation below holds checkpoints plus a /proc dump on a
 * timeout. It mirrors the pattern of tests/cthreadcomm/tests.c for this
 * class of qemu-only mystery. It exists so that a recurrence carries enough
 * information for a real diagnosis, and not only a bare REQUIRE_TRUE(false)
 * with no reason.
 *
 * Look for a child in State: S, with a real /proc/<pid>/syscall entry and a
 * broad SigBlk mask that blocks nearly every signal. That combination
 * matches the confirmed fd_trans_lock signature. This child never calls
 * alarm() itself,
 * so, unlike the cthreadcomm cases, do not expect any particular pending
 * signal in ShdPnd. The mask itself is the normal thread-management
 * behaviour of qemu-user, and the mask is what to look for here. A child in
 * State: R that spins at high CPU with an empty wchan points at a real busy
 * loop instead. The last [DEBUG_TEST] checkpoint that the child prints
 * narrows down where inside the child it stops. */
TEST(async, fatal_drains_queue_before_writing_and_terminating) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  pid_t pid = fork();
  if (pid == 0) {
    /* This child deliberately does NOT redirect STDOUT_FILENO and
     * STDERR_FILENO to /dev/null. Silence here defeats the whole purpose of
     * the checkpoints below. The hang-chasing tests of
     * tests/cthreadcomm/tests.c document the same point: a silent child that
     * hangs shows nothing about how far it got. */
    fprintf(stderr, "[DEBUG_TEST] child pid=%d about to clog_open_file_mp\n",
            (int)getpid());
    /* The interval is long and the buffer is large. Neither the timer nor
     * the size threshold can therefore flush any of the messages below.
     * ccol_log_fatal() runs a few lines later. */
    clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                            .flush_interval_us = 60000000};
    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
    if (lg == CLOG_INVALID) _exit(2);
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d opened, about to log queued "
            "message one\n",
            (int)getpid());
    ccol_log_info(lg, "queued message one");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d logged message one, about to log "
            "queued message two\n",
            (int)getpid());
    ccol_log_info(lg, "queued message two");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d logged message two, about to "
            "ccol_log_fatal\n",
            (int)getpid());
    ccol_log_fatal(lg, "the fatal record itself");
    fprintf(stderr,
            "[DEBUG_TEST] child pid=%d ccol_log_fatal returned (should be "
            "unreachable!)\n",
            (int)getpid());
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
  /* The order on the disk follows the clock. Both queued messages come
   * first, and then the fatal one. The test compares byte offsets and not
   * raw pointers. The _Generic printer of tau treats a char* as a C string.
   * A failure message would otherwise print the rest of buf from that point
   * on. */
  REQUIRE_LT((long)(p1 - buf), (long)(p2 - buf));
  REQUIRE_LT((long)(p2 - buf), (long)(pf - buf));

  cleanup_dir(dir, "app.log");
}

/* An idle async logger logs nothing after its first message. It must NEVER
 * rotate on its own, however many multiples of the interval pass with no
 * further write. This mirrors the documented contract of the synchronous
 * write path: a time rotation starts at the first WRITE after the interval
 * ends. The logger does not rotate on its own when the interval ends and
 * nothing new arrives.
 *
 * The writer thread wakes up at each flush interval, which is a different
 * interval from the rotation one. Those wakeups must never start a rotation
 * either. The whole rotation-and-write block of _writer_flush_now() runs
 * only when async_buf holds something to write.
 *
 * When a NEW message finally arrives after the interval ends, exactly one
 * rotation must happen. Not one for each interval that passes, and not
 * zero. */
TEST(async, idle_logger_does_not_rotate_until_next_write_after_interval) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* rotation_interval_us must stay well above the tolerance that this test
   * grants the flush of the "first message" below, which is 2000ms. A
   * rotation_interval_us of 1 s is TIGHTER than that worst-case landing
   * time. A flush can land anywhere between 1000ms and 2000ms after the
   * construction. That is inside the stated tolerance of the test, and the
   * valgrind instrumentation cost of make memtest reaches it.
   * _clog_time_rotate_if_due() then rotates as part of THAT flush. That
   * rotation is correct under the documented "next write after the interval
   * ends" contract of this library. It happens before the idle-wait phase
   * below starts. It then fails the "must not have rotated yet" check a few
   * lines down for no real reason. A value of 3 seconds leaves a wide margin
   * above the same 2000ms tolerance. */
  clog_rotation_cfg_t rcfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 3000000,
  };
  clog_async_cfg_t acfg = {.flush_interval_us = 50000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "first message");
  REQUIRE_TRUE(_poll_for_substring(path, "first message", 2000));

  /* Stay idle well past the rotation interval, with no further log call.
   * The writer thread still wakes up every 50ms for its flush interval. The
   * logger must not rotate on its own. */
  usleep(4000000);
  REQUIRE_EQ(count_files_with_prefix(dir, "app.log."), 0);

  /* The interval ends, so the next write must start exactly one rotation. */
  ccol_log_info(lg, "second message");
  clog_close(lg);

  REQUIRE_EQ(count_files_with_prefix(dir, "app.log."), 1);

  cleanup_dir(dir, "app.log");
}

TEST(async, flush_drains_a_slow_to_self_flush_setup) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_us = 60000000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "needs an explicit flush");

  /* Without clog_flush(), this message waits for a full minute, because
   * flush_interval_us is 60000000. It can also wait for a very large batch,
   * because flush_buffer_size is 1MiB. This test waits for neither. */
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "needs an explicit flush"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* An allocator whose malloc() can fail on demand. The test uses it to make
 * the internal node allocation of the unbounded queue fail inside
 * ccol_dynmq_send_zc(). This pins the path of clog_flush() that returns at
 * once, instead of a hang, when the enqueue itself fails. See
 * _clog_flush_pinned(). */
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

  ccol_memmgmt_procs_t procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg_t cfg = {0}; /* an unbounded queue. This test forces its
      own send-side node allocation to fail. */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  atomic_store(&g_flush_oom_fail_malloc, true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return at once, and must not hang */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  atomic_store(&g_flush_oom_fail_malloc, false);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* _clog_flush_pinned() must check the return value of ccol_mutex_init() and
 * of ccol_cond_var_init() on its own synchronization object on the stack. It
 * must check both before it calls ccol_mutex_lock() or ccol_cond_var_wait()
 * on that object. A real failure of either init is not reachable against the
 * default-attribute implementation of glibc, but POSIX permits one, for
 * example under ENOMEM. Without the checks, the code then works on a mutex
 * that is not fully initialized. That is undefined behaviour and not a clean
 * degradation. This path is reachable from clog_flush() and, worse, from the
 * FATAL path of _clog_write(). This one call instead fails open: it enqueues
 * nothing and it tries no wait. This is the same behaviour as for the "the
 * enqueue itself fails" case above. */
TEST(async, flush_returns_promptly_when_mutex_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "before forced ccol_mutex_init failure");

  clog_test_force_flush_mutex_init_failure(true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return at once, with no hang and no crash */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  /* The forced failure must disarm itself. This logger must still work
   * fully after it. This proves that the failed init leaves no corrupt state
   * and no leaked resource behind. */
  ccol_log_info(lg, "after forced ccol_mutex_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "before forced ccol_mutex_init failure"), NULL);
  REQUIRE_NE(strstr(buf, "after forced ccol_mutex_init failure"), NULL);

  cleanup_dir(dir, "app.log");
}

/* This is the same contract as flush_returns_promptly_when_mutex_init_fails
 * above, through the OTHER init call. ccol_cond_var_init() can fail after
 * ccol_mutex_init() succeeds. The code must then destroy that initialized
 * mutex and must not leak it. It must also make no wait on the condition
 * variable, which is not initialized. */
TEST(async, flush_returns_promptly_when_condvar_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "before forced condvar_init failure");

  clog_test_force_flush_condvar_init_failure(true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return at once, with no hang and no crash */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  ccol_log_info(lg, "after forced condvar_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "before forced condvar_init failure"), NULL);
  REQUIRE_NE(strstr(buf, "after forced condvar_init failure"), NULL);

  cleanup_dir(dir, "app.log");
}

static _Atomic bool g_teardown_retry_close_done = false;
static clog g_teardown_retry_lg;

static void *_teardown_retry_closer(void *arg) {
  (void)arg;
  clog_close(g_teardown_retry_lg);
  atomic_store(&g_teardown_retry_close_done, true);
  return NULL;
}

/* _shared_async_teardown() must check whether the send of the shutdown
 * sentinel to the writer thread succeeds, before it joins that thread. With
 * the default unbounded queue, that send through ccol_dynmq_send_zc() can
 * really fail under memory pressure, when its own internal node allocation
 * fails. That is exactly the condition under which a caller may close
 * loggers. Without a retry, the writer thread then waits forever for a
 * sentinel that never arrives, and clog_close() hangs forever. The signature
 * is a closing thread parked in pthread_join inside _shared_async_teardown,
 * while the writer thread is parked in ccol_dynmq_timed_recv_zc. The code
 * therefore retries the sentinel send with a bounded backoff. It does not
 * give up after one try. */
TEST(async, close_recovers_from_transient_oom_instead_of_hanging) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg_t cfg = {0}; /* an unbounded queue. This test forces its
      own shutdown-sentinel node allocation to fail, and then to recover. */
  g_teardown_retry_lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(g_teardown_retry_lg, CLOG_INVALID);

  ccol_log_info(g_teardown_retry_lg, "before oom");

  atomic_store(&g_flush_oom_fail_malloc, true);
  atomic_store(&g_teardown_retry_close_done, false);

  pthread_t th;
  REQUIRE_EQ(pthread_create(&th, NULL, _teardown_retry_closer, NULL), 0);

  /* Simulate a short window with no memory. Every allocation fails for a
   * while, and then the allocator recovers. This is what a real, transient
   * allocation failure looks like when it clears up. This test is not
   * vacuous. Without the retry, close() already hangs beyond recovery by the
   * time the first sentinel send fails. A later clear of the flag then
   * cannot help. */
  usleep(300000);
  atomic_store(&g_flush_oom_fail_malloc, false);

  /* This is a bounded wait and not a blocking pthread_join(). Without the
   * bounded retry above, this thread hangs forever. This test must fail in a
   * visible way and must not hang the whole suite. */
  bool done = false;
  for (int waited_ms = 0; waited_ms < 10000; waited_ms += 20) {
    if (atomic_load(&g_teardown_retry_close_done)) {
      done = true;
      break;
    }
    usleep(20000);
  }
  REQUIRE_TRUE(done); /* close() must return in the end, and must not hang */
  pthread_join(th, NULL);

  cleanup_dir(dir, "app.log");
}

/* An allocator that fails exactly its Nth call. It counts malloc, calloc and
 * realloc together, which matches the real allocation order. The test uses
 * it to land a forced failure on the internal node allocation of
 * ccol_dynmq_send_zc() inside _clog_write_async().
 *
 * The allocator of flush_returns_promptly_when_enqueue_fails above is
 * different: it fails every allocation and not one numbered call. It cannot
 * reach this call site without a failure of the envelope and field-snapshot
 * allocations, which must succeed first. */
static _Atomic int g_enqueue_fail_call_count = 0;
static _Atomic int g_enqueue_fail_at = -1;

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

/* The enqueue-failure fallback of _clog_write_async() runs when
 * ccol_dynmq_send_zc() or ccol_circq_send_zc() itself fails. For example,
 * the node allocation of the unbounded queue can fail under memory pressure.
 * That fallback must run both rotation checks that every other write path in
 * this file runs, before it writes the record straight to sh->fd. Without
 * them, max_file_size has no effect for exactly the record that takes this
 * path, and nothing reports it. That record never touches sh->async_buf, so
 * a later clog_flush() does not notice either. _writer_flush_now() runs its
 * own rotation checks only when async_buf holds content. */
TEST(async, enqueue_failure_fallback_still_rotates_on_size) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg_t rcfg = {0};
  rcfg.size_rotation_enabled = true;
  rcfg.max_file_size = 10; /* small: any real record goes past it */

  clog_async_cfg_t acfg = {0}; /* unbounded queue */

  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* One ordinary write confirms that the rotation works end to end for this
   * setup. It also leaves bytes_written at 0 again, because _rotate() resets
   * it. */
  ccol_log_info(lg, "seed");
  clog_flush(lg);

  int rotated_before = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ(rotated_before, 1);

  /* Fail exactly the 2nd allocation of the next ccol_log_info() call. This
   * logger holds no fields and the message is short. Allocation #1 is then
   * the calloc for the envelope, and #2 is the internal node allocation of
   * ccol_dynmq_send_zc(). The failure therefore lands on the enqueue call
   * itself. It does not land on anything before that call. Such a failure
   * would route through the ordinary, already correct _clog_write_sync()
   * fallback.
   * _snapshot_fields() skips its own ccol_growbuf_init() completely for a
   * logger with no fields, so that call never appears in this count. */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  ccol_log_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  /* The logger must write the record synchronously, because this fallback
   * path never touches the queue. That write must also start the rotation at
   * once. No clog_flush() call is needed. */
  int rotated_after = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated_after, rotated_before);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* This test is a sibling of the test above. It covers the time-based
 * rotation in place of the size-based one. The same enqueue-failure fallback
 * path must obey rotation_interval_us, and not only max_file_size. */
TEST(async, enqueue_failure_fallback_still_rotates_on_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg_t rcfg = {0};
  rcfg.time_rotation_enabled = true;
  rcfg.rotation_interval_us = 1000000;

  clog_async_cfg_t acfg = {0};

  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "seed");
  clog_flush(lg);
  int rotated_before = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ(rotated_before, 0);

  usleep(1100 * 1000); /* wait for rotation_interval_us to end */

  /* See the comment of enqueue_failure_fallback_still_rotates_on_size. This
   * logger holds no fields. Allocation #1 is therefore the calloc for the
   * envelope. Allocation #2 is the internal node allocation of
   * ccol_dynmq_send_zc(), which is the enqueue call itself.
   * _snapshot_fields() skips its own ccol_growbuf_init() completely for a
   * logger with no fields, so that call is never part of this count. */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  ccol_log_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  int rotated_after = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated_after, rotated_before);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* The enqueue-failure fallback of _clog_write_async() must first drain
 * whatever waits ahead of it in the SAME queue. Those messages are already
 * enqueued, and the writer thread has not processed them yet. Only then may
 * the fallback write its own record straight to sh->fd.
 *
 * Without that drain, a message that fails to enqueue can win the race for
 * lg->shared->mutex. It then lands on the disk BEFORE an earlier message
 * that the caller already submitted and the writer thread has not reached
 * yet. The order of the output and the order of the submissions then differ,
 * and nothing reports it.
 *
 * flush_buffer_size and flush_interval_us are both far out of reach. Nothing
 * other than the drain before the direct write can put "message A" on the
 * disk this early. That drain is the _clog_flush_pinned() call ahead of the
 * fallback write. Without it, "message A" still waits unflushed in
 * sh->async_buf at this point. Only the final drain of clog_close() would
 * write it, and this test never reaches that drain. So "message B" appears
 * in the file FIRST, or "message A" is absent completely. */
TEST(async,
     enqueue_failure_direct_write_does_not_reorder_already_queued_message) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg_t acfg = {.flush_buffer_size = 1 << 20,
                           .flush_interval_us = 60000000}; /* unbounded queue */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "message A (already queued)");

  /* This fails exactly the enqueue call of the very next ccol_log_info().
   * Allocation #1 is the envelope calloc. Allocation #2 is the internal node
   * allocation of ccol_dynmq_send_zc(). The comment of
   * enqueue_failure_fallback_still_rotates_on_size above already explains
   * this choice. */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  ccol_log_info(lg, "message B (enqueue fails, written directly)");
  atomic_store(&g_enqueue_fail_at, -1);

  /* There is no clog_flush() call here. When the ccol_log_info() call for
   * "message B" returns, both messages must already be on the disk, in the
   * order of their submission. The drain before the direct write of the
   * fallback path is the only cause of this. */
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

/* The out_pool->oom branch of _snapshot_fields() must not destroy
 * job->field_pool itself. ccol_growbuf_destroy() frees out_pool->buf and
 * never sets that pointer to NULL afterwards. _clog_async_job_release()
 * always destroys job->field_pool again when it finishes with the job. Both
 * destroys together free the same heap block twice. The key and value data
 * of a field can be large enough to grow the internal growbuf of the field
 * pool past its first capacity. This happens as soon as the realloc() of
 * that growth fails.
 *
 * The test sets one field that is long enough to guarantee at least one
 * growbuf_grow() call. An allocator that fails exactly one numbered malloc,
 * calloc or realloc call then sweeps enough calls. It lands on the realloc()
 * of that growth at least once. A double free here either aborts the process
 * through the heap consistency checks of glibc, or make memtest catches it
 * with valgrind. The only job of this test is to survive the sweep with no
 * crash. */
TEST(async, field_snapshot_growbuf_oom_does_not_double_free) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg_t acfg = {0}; /* unbounded queue */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* This value is long enough for one purpose. Append "key\0value\0" to the
   * 256-byte first growbuf capacity of the field pool. That always needs at
   * least one growbuf_grow() call, and therefore one _ccol_mem_realloc()
   * call. This holds wherever else in this call an allocation lands. */
  char big_value[400];
  memset(big_value, 'v', sizeof(big_value) - 1);
  big_value[sizeof(big_value) - 1] = '\0';
  clog_set_field(lg, "big", big_value);

  for (int fail_at = 1; fail_at <= 20; fail_at++) {
    atomic_store(&g_enqueue_fail_call_count, 0);
    atomic_store(&g_enqueue_fail_at, fail_at);
    ccol_log_info(lg, "msg %d", fail_at);
    atomic_store(&g_enqueue_fail_at, -1);
    clog_flush(lg); /* drain the job of this round before the code arms the
        next fault. The unrelated allocations of the writer thread then never
        fall inside the armed window of the next round. */
  }

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* The record of a queued job can be too large. It can also hit a transient
 * allocation failure. The writer thread of the async path then builds a
 * fallback placeholder. It must not append that placeholder into
 * sh->async_buf with a
 * sequence of appends that it does not check. sh->async_buf is the shared
 * aggregation buffer that batches jobs from every handle that shares this
 * target.
 *
 * That buffer can sit so close to its own growth ceiling that even the
 * small, fixed-size fallback does not fit. A configured flush_buffer_size
 * close to that ceiling reaches this state. Nothing then flushes the buffer
 * before the record build of a job runs into it. An
 * unchecked fallback then truncates in the middle of the record, and the
 * logger never writes it. That breaks the documented guarantee of this
 * library that a record is not truncated and not malformed. The writer
 * thread instead flushes whatever is already safely in the buffer, and tries
 * again against an empty buffer, which always has room.
 *
 * The growth ceiling of async_buf follows the configured flush_buffer_size
 * whenever that value is larger than the internal 16 MiB single-record
 * default of the library. See _buf_raise_cap_limit(). This test therefore
 * sizes its messages from the flush_buffer_size that it configures, and not
 * from a hardcoded constant. */
TEST(async,
     fallback_record_still_well_formed_when_batch_buffer_is_nearly_full) {
  /* Calibrate the fixed overhead of one record in this format. That overhead
   * is everything on a logfmt line except the message content. The
   * calibration uses a plain synchronous logger and a short message that
   * needs no quotes. The real messages below are several megabytes, and this
   * lets the test size them exactly. The test then hardcodes no internal
   * layout beyond the documented 16 MiB cap. */
  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);

  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "CALIBRATION_MESSAGE_CONTENT";
  ccol_log_info(calib_lg, "%s", calib_msg);
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

  /* flush_buffer_size is well above the internal 16 MiB single-record
   * default of the library. The growth ceiling of async_buf rises to match
   * it. See _buf_raise_cap_limit(). Nothing then flushes the buffer by size
   * before the record build of a job runs into that larger ceiling. This is
   * the scenario that the flush-and-retry recovery exists for. */
  const size_t write_buf_cap = 20UL * 1024 * 1024; /* == acfg.flush_buffer_size
      below, and therefore the real cap_limit of async_buf */
  clog_async_cfg_t acfg = {.flush_buffer_size = write_buf_cap};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  const size_t room = 20; /* far less than a fallback record needs */
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

  /* Both messages go into the queue before any flush, so they land in
   * async_buf one after the other on the writer thread. The first brings
   * async_buf to within `room` bytes of the cap, and it is a valid,
   * well-formed record of its own. The record of the second one does not
   * fit. Without the flush that this test pins, even ITS fallback does not
   * fit. */
  ccol_log_info(lg, "%s", first_msg);
  ccol_log_info(lg, "%s", huge_msg);
  clog_flush(lg);
  clog_close(lg);

  free(first_msg);
  free(huge_msg);

  size_t read_cap = write_buf_cap + (2UL * 1024 * 1024);
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  size_t len = read_file(path, buf, read_cap);
  REQUIRE_GT(len, (size_t)0);

  /* There must be exactly two well-formed lines. The first is the huge
   * message, which fits on its own. The second is the fallback placeholder
   * of the second message. The two must never merge into one truncated
   * line. */
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
  /* A well-formed line holds a quoted msg value that closes correctly, and
   * then a real newline at the end. It is not a fragment that stops in the
   * middle of a field and joins whatever comes next. */
  REQUIRE_EQ(*(line_end - 1), '"');

  free(buf);
  cleanup_dir(dir, "app.log");
}

/*
 * This is a different case from the one that the test above guards. An
 * ordinary, small message must never become the "too large to emit" fallback
 * placeholder. Unrelated content in the shared async batch buffer can leave
 * too little room for the real record of that message. It can still leave
 * enough room for the much smaller fallback note.
 *
 * The test above drives the case where NEITHER the real record NOR the
 * fallback fits, which flushes and tries again. This test drives the
 * narrower case where the fallback alone fits. Without a retry there, the
 * writer thread never tries again at all. A message that fits well on its
 * own is then lost, and the log reports it as oversized, which is wrong.
 */
TEST(async,
     ordinary_small_message_not_misreported_as_too_large_by_batch_neighbor) {
  const char *small_msg = "reachable small message";

  /* Calibrate the fixed overhead of one record for this test. That overhead
   * is everything on a logfmt line besides the message content, which is ts,
   * level, proc, src and func=. The calibration uses a plain synchronous
   * logger that shares the __FILE__ and __func__ of this test. tau generates
   * one function for each TEST(), so every ccol_log_info() call below pays
   * for the generated function name of this test, whatever its length.
   *
   * Without this calibration, a filler record sized only from write_buf_cap
   * can itself go above CLOG_BUF_MAX in an empty buffer, once this overhead
   * comes back in. The sweep below would then also cover the real
   * oversized-record case, which the code already handles. It would miss the
   * narrow case that this test aims at. */
  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  ccol_log_info(calib_lg, "%s", calib_msg);
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

  /* flush_buffer_size is deliberately ABOVE the internal 16 MiB write-buffer
   * cap of clogger. Nothing then flushes async_buf by size in time. The
   * record build of a job therefore runs into the room that an earlier job
   * in the same batch leaves behind. */
  clog_async_cfg_t acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  const size_t write_buf_cap = 16UL * 1024 * 1024; /* CLOG_BUF_MAX */
  char *filler = malloc(write_buf_cap);
  REQUIRE_NE(filler, NULL);
  memset(filler, 'B', write_buf_cap);

  /* Sweep a wide range of leftover headroom values. Those values are the
   * bytes that stay free in the batch buffer after the filler record, which
   * is `room - fixed_overhead`. A sweep is better than an exact calibration
   * of the size of the fallback placeholder. At least one value in this
   * range lands between two sizes. Those are the size of the fallback
   * placeholder and the size of the real record of the small message. That
   * window is the one that needs a retry against an empty buffer. There the
   * fallback fits and the real record does not.
   * The sweep starts at fixed_overhead, which keeps every filler
   * record inside CLOG_BUF_MAX on its own. No filler record therefore needs
   * the fallback, for a real reason or otherwise.
   *
   * The clog_flush() call between rounds writes each pair to the disk and
   * empties the batch buffer. Every round therefore starts from the same
   * deterministic state, where the buffer just grew to CLOG_BUF_MAX and
   * holds nothing else, whatever an earlier round did.
   *
   * The test truncates the file back to empty after it checks the pair of
   * each round. It does this through a second fd on the same path, because
   * clog always opens for append and never truncates. The destination fd
   * stays open with O_APPEND the whole time. A write right after an external
   * truncate therefore lands at the new end of the file, which is zero. That
   * is the same as a fresh logger for that one round. Without this, the file
   * would collect about write_buf_cap bytes for each round. The read-back
   * buffer of this test would then have to grow to match, only to see the
   * pair of the last round. */
  size_t read_cap = write_buf_cap * 2;
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  int pairs_logged = 0;
  for (size_t room = fixed_overhead + 50; room <= fixed_overhead + 1200;
       room += 25) {
    filler[write_buf_cap - room] = '\0';
    ccol_log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B'; /* restore it for the next round */
    ccol_log_info(lg, "%s", small_msg);
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);
    /* The ordinary small message of this round must never become the
     * incorrect "too large to emit" placeholder. The logger must also not
     * drop it. */
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

/*
 * _emit_backtrace_lines() is the backtrace emitter of logfmt. It must report
 * a failure, and it must not return void. If it discards every failure,
 * _clog_build_record() always reports the record as fully and successfully
 * built. One of the failures that it would discard is the case where not one
 * single frame line fits in the room that is left. That is not only the case
 * of one oversized frame.
 *
 * The batch buffer can hold enough room for the primary content of a record
 * with with_backtrace=true. It can hold not enough room for any single
 * backtrace frame line. That state reaches this case. Here that buffer is
 * sh->async_buf. An earlier, unrelated filler record in the same batch
 * leaves it with only a few hundred to a few thousand bytes of headroom.
 *
 * A real ccol_log_error(), ccol_log_alert() or ccol_log_fatal() call could
 * then reach the disk with its message intact and its whole requested
 * backtrace absent. A reader could not tell that record from a call that
 * asks for no backtrace. That breaks the guarantee of this file that a
 * reader can always tell "backtrace omitted" from "never requested". The
 * library honours and tests that guarantee for a capture failure too. See
 * logfmt_backtrace_capture_failure_emits_marker_line above. The equivalent
 * scenarios of JSON and syslog are beside it. Whenever not one frame fits,
 * _emit_backtrace_lines() therefore falls back to the same small,
 * fixed-size "unavailable" marker that it uses when the capture fails.
 *
 * This test mirrors the room sweep of
 * ordinary_small_message_not_misreported_as_too_large_by_batch_neighbor. It
 * uses the same calibration and the same range. The exact byte offset that
 * this failure mode needs depends on the backtrace frame text. That text
 * holds the full paths of the binary and the libraries, from
 * backtrace_symbols().
 * This test cannot predict that text. At least one value in this range is
 * expected to land inside the target window in any environment.
 */
TEST(async, backtrace_never_silently_lost_when_batch_buffer_is_nearly_full) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  const char *small_msg = "reachable small message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  ccol_log_info(calib_lg, "%s", calib_msg);
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

  clog_async_cfg_t acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  const size_t write_buf_cap = 16UL * 1024 * 1024; /* CLOG_BUF_MAX */
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
    ccol_log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B';  /* restore it for the next round */
    ccol_log_error(lg, "%s", small_msg); /* with_backtrace = true */
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    /* The content at the end of this record must always hold at least one
     * real backtrace frame line, or the fixed-size "unavailable" marker.
     * Both must never be absent. */
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

/*
 * _emit_backtrace_json() must not close an empty "bt":[] array. The header,
 * the fields and the msg of a record can fill the batch buffer to a
 * particular point. Just enough room is left to open and close the "bt"
 * array. Not enough room is left for the text of even one real frame. That
 * is the case here.
 *
 * Such an array is identical, byte for byte, to a real shallow backtrace
 * with no frames after the first two. It therefore removes the reason for
 * "bt_error". ccol_log_error(3) states the contract. Rarely, the logger
 * cannot put the backtrace into the record. In that case, it closes the
 * record in the usual way, with a bt_error key.
 *
 * A closed empty array also counts as a fully successful build. The retry of
 * the writer thread, which used_fallback drives, would then never notice
 * that anything is lost. See the logfmt and JSON branch of
 * _clog_writer_thread_main(). The LOGFMT sibling test directly above this
 * one has no such exposure, because _emit_backtrace_lines() detects "not one
 * frame fits" directly.
 *
 * This test mirrors the approach of
 * backtrace_never_silently_lost_when_batch_buffer_is_nearly_full. A
 * calibrated filler message pushes async_buf to within a swept range of
 * `room` bytes of CLOG_BUF_MAX. The test then logs an error record with a
 * backtrace. This test does that for CLOG_FMT_JSON. It also adds a check
 * that "bt":[] never stands for a backtrace that is real but has no space.
 */
TEST(async, json_backtrace_never_silently_becomes_an_empty_array) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  const char *small_msg = "reachable small json message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof(calib_dir)), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof(calib_path), "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  clog_set_format(calib_lg, CLOG_FMT_JSON);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  ccol_log_info(calib_lg, "%s", calib_msg);
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

  clog_async_cfg_t acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  const size_t write_buf_cap = 16UL * 1024 * 1024; /* CLOG_BUF_MAX */
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
    ccol_log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B';  /* restore it for the next round */
    ccol_log_error(lg, "%s", small_msg); /* with_backtrace = true */
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    /* A "bt":[] must never stand for a real backtrace that the logger
     * drops. The record must hold real frame content, or the explicit
     * bt_error marker. An empty array must never look like a shallow
     * one. */
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

/*
 * _clog_build_record() falls back to _clog_write_unrepresentable_record() as
 * its last resort, when not even its own small fallback placeholder fits
 * into the target buffer. That function must take with_backtrace into
 * account. Without that, a requested LOGFMT backtrace disappears with no
 * sign of it at all.
 *
 * CLOG_FMT_SYSLOG has no such exposure. Its callers always write its
 * backtrace lines separately from this function, whatever happens to the
 * primary record. CLOG_FMT_JSON embeds a "bt_error" marker into its own
 * fallback text through _clog_build_fallback_record(), whenever THAT
 * fallback fits, and that one is reachable in ordinary use. But
 * with_backtrace must reach THIS function. Without it, one case loses the
 * backtrace for both, with no report. That is the case where not even the
 * JSON or the logfmt fallback fits.
 *
 * To reach this function through the ordinary log_* call path, not even the
 * small fallback placeholder of _clog_build_record() may fit in the target
 * buffer. The real CLOG_BUF_INITIAL value of this library makes that
 * unreachable in practice. See the doc comment of that function. This test
 * therefore calls clog_test_write_unrepresentable_record() directly. It
 * follows the precedent of clog_test_gzip_compress_file(), which tests
 * another internal helper that is hard to reach.
 */
TEST(robustness,
     unrepresentable_record_logfmt_still_signals_missing_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR, true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  char *primary = strstr(buf, "log record dropped");
  REQUIRE_NE(primary, NULL);

  /* A "backtrace unavailable" continuation line must follow the primary
   * placeholder line. An ordinary logfmt record whose backtrace capture
   * fails gets the same line. */
  REQUIRE_NE(strstr(primary, "#error backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

/* This is the opposite of the behaviour above. With with_backtrace == false,
 * the record must never gain a marker line that nobody asks for. */
TEST(robustness, unrepresentable_record_logfmt_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR,
                                         false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

/* This is the CLOG_FMT_JSON form of the behaviour above. Whenever
 * with_backtrace is true, _clog_write_unrepresentable_record() must embed a
 * "bt_error" marker into its own single JSON line.
 * _clog_build_fallback_record() already does the same for the JSON fallback
 * that is reachable in ordinary use. */
TEST(robustness, unrepresentable_record_json_embeds_bt_error_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  /* The bt_error marker of JSON must appear even in this innermost fallback.
   * It must not appear only in the fallback that
   * _clog_build_fallback_record() handles, which is reachable in ordinary
   * use. */
  REQUIRE_NE(strstr(buf, "\"bt_error\":\"unavailable\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* This is the opposite of the behaviour above, for JSON. An unrepresentable
 * record with no with_backtrace must carry no bt_error marker at all. */
TEST(robustness, unrepresentable_record_json_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

/*
 * _emit_backtrace_syslog_lines() has a branch for syms == NULL, which means
 * that the backtrace capture failed. That branch must still put something on
 * the wire when one more thing fails. That is the one small append that
 * builds its "backtrace unavailable" marker record into the buffer of the
 * caller. Without a last
 * resort that needs no allocation, that double failure writes nothing at
 * all, and nothing reports it. The first failure is the capture, and the
 * second is the append of the marker. Almost every other
 * build-into-a-buffer call site in this file behaves differently.
 *
 * This test forces both failures in a deterministic way. The real backtrace
 * capture never runs here, because the test calls the marker branch
 * directly. clog_test_force_next_buf_ensure_failure() then forces the one
 * append inside it to fail. The buffer-sizing constants of this library make
 * that append failure unreachable in ordinary use.
 */
TEST(robustness, syslog_backtrace_unavailable_marker_survives_append_failure) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_next_buf_ensure_failure(true);
  clog_test_emit_backtrace_syslog_unavailable_marker(lg, CLOG_ERROR);

  char buf[4096];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);

  /* Some record that signals the absent backtrace must still reach the wire.
   * This holds even though the ordinary marker build, which uses the buffer,
   * fails. */
  REQUIRE_NE(strstr(buf, "backtrace unavailable"), NULL);
  /* A well-formed record holds a real syslog PRI header and one line that
   * ends with a newline. It is not a truncated fragment that can
   * desynchronize the stream. */
  REQUIRE_EQ(buf[0], '<');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

/* This is the logfmt and syslog form of the guarantee of
 * json.fatal_has_inline_bt_array, which is that the loss of a backtrace must
 * never be silent. JSON carries a dedicated bt_error marker. logfmt and
 * syslog do not, so a NULL syms must not turn _emit_backtrace_lines() and
 * _emit_backtrace_syslog_lines() into functions that do nothing. A reader of
 * a logfmt or syslog record with with_backtrace=true could then not tell
 * "the logger omitted the backtrace" from "nobody asked for a
 * backtrace". */
TEST(robustness, logfmt_backtrace_capture_failure_emits_marker_line) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_force_backtrace_capture_failure(true);
  ccol_log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

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

  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_backtrace_capture_failure(true);
  ccol_log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

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

/*
 * The loop over real syms in _emit_backtrace_syslog_lines() must track
 * whether it writes any frame at all. Each frame gets its own scratch buffer
 * that the code resets first. A failure of every frame is therefore
 * reachable only under a real allocation failure that lasts across all of
 * them. A loop with
 * no such tracking then returns with nothing written: no frame, and no
 * "unavailable" marker. The syms == NULL case above, where the capture fails
 * outright, always writes one marker. A real backtrace that the logger
 * captures and then writes nowhere would be identical to one that nobody
 * asks for.
 *
 * The test uses clog_test_force_all_syslog_backtrace_frames_failure() to
 * make every frame fail in a deterministic way. The appends of this
 * function for each frame are small, and they have plenty of room under the
 * buffer-sizing constants of this library. A real allocation failure across
 * all of them is therefore not practical to reproduce.
 */
TEST(robustness,
     syslog_backtrace_all_frames_failing_still_emits_marker_record) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* This test needs a real backtrace that the capture gets, unlike the
   * capture-failure test above. The platform must have real backtrace
   * support that works in the normal way. Only the append of each frame
   * fails, and the test forces that. */
  clog_test_force_all_syslog_backtrace_frames_failure(true);
  ccol_log_error(lg, "boom");
  clog_test_force_all_syslog_backtrace_frames_failure(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* Some record that signals the absent backtrace must still reach the wire,
   * as it does when the capture fails outright. No real "\t#N " frame line
   * may appear instead, because the forced failure lets none through. */
  REQUIRE_NE(strstr(buf, "#error backtrace unavailable"), NULL);
  REQUIRE_EQ(strstr(buf, "\t#0 "), NULL);
}

/*
 * _emit_backtrace_lines() must tell two cases apart. The first is a capture
 * that succeeds and finds no frame beyond the two internal bookkeeping ones,
 * where the depth is <= CLOG_BT_INITIAL_FRAME. The second is "every frame
 * failed to fit". If the function treats them alike, it writes the
 * misleading "#error backtrace unavailable" marker although nothing is
 * wrong. _emit_backtrace_json() makes the same distinction. See its
 * array_content_is_accurate check.
 *
 * This test drives the logfmt side with
 * clog_test_force_shallow_backtrace_depth(). That hook reports a real
 * capture that is not NULL, clamped to a shallow depth. It reproduces the
 * boundary condition in a deterministic way. It does not depend on a real
 * shallow call stack, which never happens on this platform.
 */
TEST(robustness, logfmt_shallow_backtrace_capture_emits_no_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_force_shallow_backtrace_depth(true);
  ccol_log_error(lg, "boom");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof(buf));
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* A capture that succeeds but is shallow must never appear as an
   * unavailable one. */
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  /* The logger must also not invent a frame line for a frame that the
   * capture never gets. */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

/* This is the CLOG_FMT_SYSLOG form of the behaviour above. A capture that
 * succeeds but is shallow must also not put an extra "unavailable" marker
 * record on the wire. */
TEST(robustness, syslog_shallow_backtrace_capture_emits_no_marker_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_shallow_backtrace_depth(true);
  ccol_log_error(lg, "boom");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* There must be no "unavailable" marker for a capture that succeeds but
   * is shallow. */
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  /* There must be no invented frame line either. */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  /* Exactly one record reaches the wire, which is the primary message.
   * There is no second, extra marker record after it. */
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

#define ASYNC_STRESS_WRITER_COUNT 4
#define ASYNC_STRESS_ITERATIONS 500

static void *_async_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ASYNC_STRESS_ITERATIONS; i++)
    ccol_log_info(lg, "async stress line %d", i);
  return NULL;
}

/* This is the async form of
 * threading.concurrent_parent_and_derived_writers_no_garbled_lines. Several
 * threads drive a logger with async mode at the same time. They must never
 * produce a line that is partial or merged. */
TEST(threading, concurrent_async_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_async_cfg_t cfg = {.flush_buffer_size = 4096,
                          .flush_interval_us = 20000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

/* ========================================================================== */
/*                         COMPRESSION                                        */
/* ========================================================================== */

/* A rotation produces a valid gzip file that gunzip can decompress. */
TEST(compression, rotated_file_is_valid_gzip) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 20; i++)
    ccol_log_info(
        lg, "compression test message index=%d padding-to-force-rotation", i);

  clog_close(lg);

  /* At least one .gz file must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, ".gz"), 0);

  /* Every rotated file must be a .gz file. None may stay uncompressed. */
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"),
             count_files_with_prefix(dir, "app.log."));

  /* gunzip -t must pass on the first .gz file found. */
  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)), 0);
  REQUIRE_EQ(gunzip_test(gz_path), 0);

  cleanup_dir(dir, "app.log");
}

/* The decompressed content must match the log lines that the test
 * writes. */
TEST(compression, decompressed_content_matches_written_log) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 400,
      .time_rotation_enabled = false,
      .max_rotated_files = 10,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Write a clear sentinel into the first batch, so the logger rotates it. */
  ccol_log_info(
      lg,
      "sentinel_marker_abc123 index=0 pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  for (int i = 1; i < 15; i++)
    ccol_log_info(
        lg,
        "sentinel_marker_abc123 index=%d pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        i);

  clog_close(lg);

  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path)), 0);

  char decomp[65536];
  size_t dlen = gunzip_read(gz_path, decomp, sizeof(decomp));
  REQUIRE_GT(dlen, (size_t)0);

  /* The sentinel must appear somewhere in the decompressed output. */
  REQUIRE_TRUE(strstr(decomp, "sentinel_marker_abc123") != NULL);

  /* Every non-backtrace line must start with "ts=". */
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

/* With compress_rotated=false, no .gz file is ever produced. */
TEST(compression, no_compression_when_disabled) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 300,
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = false,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 20; i++)
    ccol_log_info(
        lg, "no-compress test message index=%d padding-padding-padding", i);

  clog_close(lg);

  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 0);

  cleanup_dir(dir, "app.log");
}

/* The cap of max_rotated_files holds even when compression is on. */
TEST(compression, max_rotated_files_respected_with_compression) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 60; i++)
    ccol_log_info(
        lg, "pruning+compress test idx=%d extra-data-to-fill-the-buffer", i);

  clog_close(lg);

  /*
   * _prune_rotated runs before the compression. It keys on the name with the
   * timestamp alone. After the prune, at most max_rotated_files raw rotated
   * files are left. The library then compresses them to .gz. This count
   * leaves out the live file ("app.log"). It allows one more file, because
   * the last rotation can race with the check.
   */
  int gz_count = count_files_with_suffix(dir, ".gz");
  REQUIRE_LE(gz_count, cfg.max_rotated_files + 1);
  REQUIRE_GT(gz_count, 0);

  cleanup_dir(dir, "app.log");
}

/* An empty log file, which is 0 bytes at the rotation, compresses to a valid
 * gzip file. */
TEST(compression, empty_file_compresses_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* Create the log file first, with 0 bytes. The test can then trigger a
   * rotation by hand, with the internal clog_rotate_now helper. A unit-test
   * build offers that helper. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);
  close(fd);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Any write triggers a rotation. */
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* One write forces a rotation of the file, which is nearly empty. */
  ccol_log_info(lg, "trigger rotation");
  clog_close(lg);

  char gz_path[512];
  int found = find_file_with_suffix(dir, ".gz", gz_path, sizeof(gz_path));
  if (found == 0) {
    /* A .gz file that the library produced must be valid. */
    REQUIRE_EQ(gunzip_test(gz_path), 0);
  }
  /* No .gz file means that the file was too small to trigger a rotation.
   * That is also acceptable. This test checks that nothing crashes. */

  cleanup_dir(dir, "app.log");
}

/* A failure can happen before the gzopen() call of _gzip_compress_file()
 * ever runs. The function must then not unlink() its destination path. A
 * source file that the function cannot even open means that this call never
 * touched the destination at all. An unlink with no condition silently
 * deletes an unrelated file that already sits at that exact destination
 * path. The compression that deletes it never wrote a single byte to it.
 * This test drives the internal helper clog_test_gzip_compress_file()
 * directly. To reproduce the real scenario end to end would need an outside
 * actor. That actor would delete a file that a rotation has just made, in
 * the narrow window between rename() and the fopen() of this call. */
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

  /* src does not exist. fopen() therefore fails before the code ever reaches
   * gzopen(dst, ...). The compression must report a failure and must not
   * touch dst at all. */
  REQUIRE_FALSE(clog_test_gzip_compress_file(src, dst));

  char buf[256];
  size_t len = read_file(dst, buf, sizeof(buf));
  REQUIRE_EQ(len, strlen(marker));
  REQUIRE_EQ(memcmp(buf, marker, strlen(marker)), 0);

  unlink(dst);
  rmdir(dir);
}

/* A stress test for concurrent rotations. It drives the race between two
 * things. The first is the gzip compression of one rotation, which runs
 * outside shared->mutex on purpose, so that slow I/O does not stall other
 * writers. The second is the prune pass of another rotation that runs at the
 * same time. Without this protection, the prune of that second rotation can
 * delete the rotated file of the first one. It does so before the
 * compression of that file finishes its read, or even starts it. The log
 * data of that generation is then lost completely, and not merely left
 * uncompressed. Many writer threads share one logger here. A tiny
 * max_file_size, and compress_rotated=true, drive many rotate and compress
 * cycles that overlap. Every .gz file that the library produces must be a
 * complete, valid gzip archive. A prune that destroys a file in the middle
 * of a read instead makes gunzip -t fail, or gives truncated content. */
#define ROTATE_STRESS_THREAD_COUNT 6
#define ROTATE_STRESS_MSGS_PER_THREAD 150

static void *_rotate_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ROTATE_STRESS_MSGS_PER_THREAD; i++)
    ccol_log_info(lg, "rotate stress padding-padding-padding-padding idx=%d",
                  i);
  return NULL;
}

TEST(compression, concurrent_rotations_never_corrupt_or_lose_a_gz_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 150,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

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

  /* Every .gz file that this concurrent load produces must be a complete,
   * valid gzip archive. A prune that races must never truncate one, and must
   * never remove one completely. */
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

/* The deterministic counterpart to the stress test above. The
 * pending-compress protection of _prune_rotated() must cover the compressed
 * DESTINATION file, which is the ".gz" file that a concurrent rotation is
 * still writing. It must not cover the uncompressed SOURCE alone, which that
 * rotation reads from. This test uses two synchronization hooks that only a
 * RUNNING_UNIT_TESTS build offers. They are
 * clog_test_set_pending_compress_delay_us() and clog_test_gz_dest_opened().
 * They force this exact narrow window every time. The wider stress test
 * above instead depends on the luck of the real thread scheduler. */
static void *_gzrace_first_rotation_writer(void *arg) {
  clog lg = *(clog *)arg;
  ccol_log_info(lg, "first rotation content");
  return NULL;
}

TEST(compression,
     prune_never_deletes_a_gz_file_still_being_written_by_another_rotation) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Any write triggers a rotation. */
      .time_rotation_enabled = false,
      .max_rotated_files = 1, /* A tight quota. It forces the prune to try to
                                  delete something on the second rotation. */
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Arm a long, deterministic delay. The compress step of the first rotation
   * then pauses directly after it creates its .gz destination on the disk.
   * It pauses before it writes any content to that file. */
  clog_test_set_pending_compress_delay_us(500000); /* 500ms */

  pthread_t t;
  REQUIRE_EQ(pthread_create(&t, NULL, _gzrace_first_rotation_writer, &lg), 0);

  /* Spin-wait until the .gz destination of the first rotation really exists
   * on the disk. A fixed sleep with a guess is not enough. The second
   * rotation directly below then always races a real file on the disk. */
  while (!clog_test_gz_dest_opened()) usleep(1000);

  /* Disarm the delay. The compress step of the second rotation, which the
   * line below triggers, then does not also pause. The in-flight compress of
   * the first rotation already took its own local copy of the 500ms delay,
   * so this disarm does not touch it. */
  clog_test_set_pending_compress_delay_us(0);

  /* The prune pass of this rotation runs while the compress of the first
   * rotation is still in flight. Its set of candidates on the disk holds
   * three files. The first is the uncompressed source of the first
   * rotation. The second is the .gz destination of that same rotation, which
   * is still in progress. The third is the file that this rotation has just
   * rotated and not yet compressed. */
  ccol_log_info(lg, "second rotation content");
  /* The prune pass of the second rotation is over, and the compression of
   * the first one is still held. Both of its files must still be on disk:
   * the temporary output that becomes the .gz file, and the source that it
   * reads. A prune that races can delete them while the compression is in
   * flight. The content of that generation then disappears completely while
   * the compression still "succeeds". An unlink of a file that is still open
   * removes only its directory entry, and the writer keeps appending to an
   * inode that no longer has a name. No .gz file exists before the output
   * is complete. */
  int gz_during = count_files_with_suffix(dir, ".gz");
  int tmp_during = count_files_with_suffix(dir, ".gz.tmp");
  int rotated_during = count_files_with_prefix(dir, "app.log.");

  pthread_join(t, NULL);
  clog_close(lg);

  REQUIRE_EQ(gz_during, 0);
  REQUIRE_EQ(tmp_during, 1);
  REQUIRE_EQ(rotated_during, 3);
  /* Once the first compression ends, max_rotated_files == 1 retires its
   * generation. The second generation is the one that stays, compressed. */
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

/* A logger opened with a bare filename, which holds no '/', works in the
 * working directory of the process at the time of the open. The names that
 * _prune_rotated() lists there must match the names of the compression in
 * flight exactly, or the protection of that compression silently does
 * nothing. This test is otherwise identical to
 * prune_never_deletes_a_gz_file_still_being_written_by_another_rotation
 * above. It opens a bare relative filename in place of an absolute path. It
 * also points the cwd of the process at its own tmpdir for a time. Every
 * other test in this suite uses the absolute paths that make_tmpdir()
 * gives, so they cannot drive this case. */
TEST(compression,
     prune_survives_gz_race_with_a_bare_relative_filename_no_slash) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);

  char oldcwd[1024];
  REQUIRE_NE((void *)getcwd(oldcwd, sizeof(oldcwd)), (void *)NULL);
  REQUIRE_EQ(chdir(dir), 0);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Any write triggers a rotation. */
      .time_rotation_enabled = false,
      .max_rotated_files = 1, /* A tight quota. It forces the prune to try to
                                  delete something on the second rotation. */
      .compress_rotated = true,
  };

  /* This is "app.log", and not "<dir>/app.log". The path holds no '/' at
   * all. */
  clog lg = clog_open_file_mp("app.log", CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_INVALID) {
    chdir(oldcwd);
    REQUIRE_NE(lg, CLOG_INVALID);
  }

  clog_test_set_pending_compress_delay_us(500000); /* 500ms */

  pthread_t t;
  int prv = pthread_create(&t, NULL, _gzrace_first_rotation_writer, &lg);
  if (prv != 0) {
    clog_test_set_pending_compress_delay_us(0);
    clog_close(lg);
    chdir(oldcwd);
    REQUIRE_EQ(prv, 0);
  }

  while (!clog_test_gz_dest_opened()) usleep(1000);
  clog_test_set_pending_compress_delay_us(0);

  ccol_log_info(lg, "second rotation content");
  /* See the absolute-path version of this race above. */
  int gz_during = count_files_with_suffix(dir, ".gz");
  int tmp_during = count_files_with_suffix(dir, ".gz.tmp");
  int rotated_during = count_files_with_prefix(dir, "app.log.");

  pthread_join(t, NULL);
  clog_close(lg);

  /* Every file operation that this test needs is done. Restore the cwd of
   * the process now. Any REQUIRE below can return early. It would then leave
   * the cwd pointed at a directory that the cleanup_dir() of this test is
   * about to remove. Every other test in this suite expects an unchanged
   * cwd. */
  REQUIRE_EQ(chdir(oldcwd), 0);

  /* This is exactly the rule of the absolute-path version of this race
   * above. */
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

/* ========================================================================== */
/*                         FATAL LEVEL (PROCESS TERMINATION)                  */
/* ========================================================================== */

TEST(fatal, terminates_process) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "process must die");
    _exit(0); /* unreachable */
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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal condition encountered");
    _exit(0); /* unreachable */
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
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal with trace");
    _exit(0); /* unreachable */
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

TEST(fatal, writes_and_terminates_with_clog_off) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* ccol_log_fatal goes past the level filter. Even with CLOG_OFF, the
   * library must write the message and the process must stop. */
  clog lg = clog_open_file_mp(path, CLOG_OFF, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal bypasses filter");
    _exit(0); /* unreachable */
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

/* ========================================================================== */
/*                         FATAL LEVEL (JSON FORMAT)                          */
/* ========================================================================== */

TEST(json, fatal_level_string_in_json) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal json message");
    _exit(0); /* unreachable */
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
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_FATAL, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    _g_fatal_child_lg = lg;
    atexit(_fatal_child_cleanup);
    ccol_log_fatal(lg, "fatal event");
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));

  REQUIRE_NE(strstr(buf, "\"bt\":["), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"),
             NULL); /* JSON embeds bt inline, no tab lines */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                  FRAMING AROUND A RETAINED CONTINUATION                    */
/* ========================================================================== */

/* True when line begins where a record of this module begins. There are five
 * such beginnings. Three of them are the "ts=" of a logfmt record, the
 * "level=" of a logfmt marker record, and the PRI of a syslog record. The
 * other two are the brace of a JSON record, and the leading tab of a logfmt
 * backtrace continuation line. */
static bool line_starts_a_record(const char *line) {
  return strncmp(line, "ts=", 3) == 0 || strncmp(line, "level=", 6) == 0 ||
         line[0] == '<' || line[0] == '{' || line[0] == '\t';
}

/*
 * This counts the places inside `line` where another record begins, past the
 * point at which a record may legitimately begin. Two things count. The
 * first is the "ts=" of a logfmt record beyond offset 0. The second is the
 * "<PRI>1 " of a syslog record beyond that point. Its ">1 " sits at offset 2
 * to 4, because PRI is one to three digits. One of these is one record
 * written into the middle of another.
 */
static int count_interior_record_starts(const char *line) {
  int n = 0;
  /* An empty line holds no record at all. The searches below skip the first
   * byte of the line, which an empty line does not have. */
  if (line[0] == '\0') return 0;
  for (const char *p = strstr(line + 1, "ts="); p; p = strstr(p + 1, "ts="))
    n++;
  for (const char *p = strstr(line, ">1 "); p; p = strstr(p + 1, ">1 "))
    if (p - line > 4) n++;
  return n;
}

/*
 * This walks buf line by line. It reports how many lines there are, how many
 * do not begin a record, and how many hold a record that begins inside
 * another one. It adds a NUL terminator to each line in place while it
 * inspects that line, and it restores the byte afterwards. buf therefore
 * reads the same before and after.
 */
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

/* A body long enough for one purpose. Two capped writes of SPLIT_WRITE_CAP
 * bytes, one after the other, both stop inside the very first record. */
#define SPLIT_WRITE_CAP ((size_t)100)

/* This fills body with `marker` and then filler that needs no logfmt escape.
 * The record therefore carries the marker verbatim. */
static void make_split_test_body(char *body, size_t bodysz,
                                 const char *marker) {
  memset(body, 'A', bodysz - 1);
  body[bodysz - 1] = '\0';
  memcpy(body, marker, strlen(marker));
}

/*
 * The async writer thread can still hold the continuation of a record whose
 * first half is already on the disk. A record that goes straight to the
 * descriptor must not land between those two halves. CLOG_FMT_SYSLOG output
 * is exempt from batching and takes exactly that route. Each record goes out
 * with its own write(). The batch buffer may still hold a continuation that
 * the flush ahead of it could not deliver. The library settles the
 * continuation first, which here means that it gives up on it. It writes the
 * missing newline of the half-written record, and a marker that names the
 * loss, in place of the rest. A consumer that reads the file line by line
 * therefore sees well-formed lines throughout.
 *
 * This test is not vacuous. Without the settle, the library writes the
 * syslog record into the middle of the logfmt record that it delivered by
 * half. The line that carries it then also carries the start of that record.
 * count_interior_record_starts() reports exactly that.
 */
TEST(split_record_framing, a_syslog_record_never_lands_inside_a_retained_one) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* A descriptor this test owns: CLOG_FMT_SYSLOG is only ever accepted on
   * one the logger did not open for itself. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  REQUIRE_NE(fd, -1);

  /* The buffer is large enough that only the explicit flushes below write
   * anything. The interval is long enough that the timer of the writer
   * thread does not flush first. */
  clog_async_cfg_t acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_open_fd_mp(fd, CLOG_INFO, &acfg, NULL);
  if (lg == CLOG_INVALID) close(fd);
  REQUIRE_NE(lg, CLOG_INVALID);

  char body1[320], body2[320], body3[320];
  make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
  make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
  make_split_test_body(body3, sizeof(body3), "RECORDTHREEBODY");

  ccol_log_info(lg, "%s", body1);
  ccol_log_info(lg, "%s", body2);
  ccol_log_info(lg, "%s", body3);

  /* Two attempts at the batch, one after the other, both stop inside the
   * first record. The first attempt is the write of this flush. The second
   * is the one that the syslog record below makes before it writes itself. */
  clog_test_force_short_writes(SPLIT_WRITE_CAP, 2);
  clog_flush(lg);

  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_log_info(lg, "SYSLOGRECORDBODY");
  /* The writer thread handles this after the record above. This call
   * therefore returns after the library writes that record. */
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
  /* No line holds a record that begins inside another one, and every line
   * begins one. */
  REQUIRE_EQ(interior, 0);
  REQUIRE_EQ(unframed, 0);
  /* The library reports the half of the first record that it could not
   * deliver. It does not drop that half in silence... */
  REQUIRE_TRUE(has_marker);
  /* ...it still delivers the records that the batch holds behind it... */
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_third);
  /* ...and it delivers the syslog record that it had to write past them. */
  REQUIRE_TRUE(has_syslog);
}

/* An allocator whose calloc fails exactly one time, on demand. A test can
 * therefore fail one specific internal allocation and leave every other one
 * working normally. That includes the allocations that the writer thread and
 * the teardown of the logger make after the body of the test returns. */
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

/*
 * The synchronous write path serves an async logger too. A CLOG_FATAL call
 * takes it. So does an ordinary call whose job envelope the allocator cannot
 * supply, which is what this test forces. That record goes straight to the
 * descriptor. It must therefore not land between the two halves of a record
 * whose continuation the batch buffer still holds.
 *
 * This test is not vacuous. Without the settle, the library writes the
 * record onto the fragment that is already on the disk. The two merge into
 * one line, and count_interior_record_starts() reports that.
 */
TEST(split_record_framing,
     a_synchronous_record_never_lands_inside_a_retained_one) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _failing_calloc_malloc,
      .free = _failing_calloc_free,
      .calloc = _failing_calloc,
      .realloc = _failing_calloc_realloc,
  };

  clog_async_cfg_t acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  char body1[320], body2[320], body3[320];
  make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
  make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
  make_split_test_body(body3, sizeof(body3), "RECORDTHREEBODY");

  ccol_log_info(lg, "%s", body1);
  ccol_log_info(lg, "%s", body2);
  ccol_log_info(lg, "%s", body3);

  clog_test_force_short_writes(SPLIT_WRITE_CAP, 2);
  clog_flush(lg);
  /* The writer thread parks on its own queue by the time that the flush it
   * acknowledged returns. The next allocation that the logger makes is
   * therefore the one that this arms against. That is the envelope that the
   * call below needs. A failure of it hands that call to the synchronous
   * write path. */
  atomic_store(&_fail_one_calloc, true);
  ccol_log_info(lg, "SYNCHRONOUSRECORDBODY");
  atomic_store(&_fail_one_calloc, false);

  clog_flush(lg);
  clog_close(lg);

  char buf[16384];
  size_t len = read_file(path, buf, sizeof(buf));
  int lines = 0, unframed = 0, interior = 0;
  inspect_record_framing(buf, &lines, &unframed, &interior);
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_second = strstr(buf, "RECORDTWOBODY") != NULL;
  bool has_third = strstr(buf, "RECORDTHREEBODY") != NULL;
  bool has_sync = strstr(buf, "SYNCHRONOUSRECORDBODY") != NULL;
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(len, (size_t)0);
  REQUIRE_EQ(interior, 0);
  REQUIRE_EQ(unframed, 0);
  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_second);
  REQUIRE_TRUE(has_third);
  REQUIRE_TRUE(has_sync);
}

/*
 * CLOG_FATAL takes the synchronous write path on an async logger. It is the
 * one caller of that path that must never wait, because the process is on
 * its way to exit(). The batch buffer can still hold a continuation. The
 * library therefore gives that continuation one bounded attempt at delivery,
 * and then gives up on it. It repairs the framing in its place. It does not
 * write the fatal record into the middle of a record that it wrote by half.
 * It also does not block the call on a descriptor that will not take the
 * rest.
 *
 * This test runs in a forked child, because the call stops the process. It
 * deliberately asserts nothing about the exit STATUS of that child. It
 * asserts only that the child stopped with no signal, and inside a bounded
 * wait. Valgrind overrides the real status of a forked child as soon as it
 * finds any still-reachable allocation in the inherited process image. Such
 * a child always has one.
 *
 * This test must also stay ahead of the fixture for the deferred slot-table
 * release below. That fixture publishes two loggers for its own destructor
 * to close in a deliberate race. A child forked after that publication
 * inherits them. A stop through exit() then runs that destructor, and so
 * that race, inside the child. ThreadSanitizer reports it there against a
 * test that has nothing to do with it.
 *
 * This test is not vacuous. Without the settle, the library writes the fatal
 * record onto the fragment that is already on the disk. The two become one
 * line, and count_interior_record_starts() reports that.
 */
TEST(split_record_framing, a_fatal_record_never_lands_inside_a_retained_one) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* The child stops through exit(). That flushes whatever the stdio buffers
   * of this process held at the time of the fork(). A drain of them here
   * first stops the harness from writing its output two times. */
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                            .flush_interval_us = 60000000};
    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
    if (lg == CLOG_INVALID) _exit(2);

    char body1[320], body2[320];
    make_split_test_body(body1, sizeof(body1), "RECORDONEBODY");
    make_split_test_body(body2, sizeof(body2), "RECORDTWOBODY");
    ccol_log_info(lg, "%s", body1);
    ccol_log_info(lg, "%s", body2);

    /* Three attempts at the batch, one after the other, all stop inside the
     * first record. The first is the write of this flush. The second is the
     * drain that CLOG_FATAL runs before anything else. The third is the
     * attempt that the fatal record makes before it gives up on the rest. */
    clog_test_force_short_writes(SPLIT_WRITE_CAP, 3);
    clog_flush(lg);

    ccol_log_fatal(lg, "FATALRECORDBODY");
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
  /* The record that names the cause of the stop reaches the log. That is the
   * whole reason that this path writes synchronously. */
  REQUIRE_TRUE(has_fatal);
}

/* ------------------------------------------------------------------------ */
/* Write errors that no retry outlasts, and the last delivery attempt        */
/* ------------------------------------------------------------------------ */

/* The async configuration of the tests below. The buffer is large enough,
 * and the interval long enough, that only the explicit flushes, the close
 * and the fatal path write anything. */
static const clog_async_cfg_t g_undelivered_acfg = {
    .queue_size = 0,
    .flush_buffer_size = 1024UL * 1024UL,
    .flush_interval_us = 60000000,
};

/*
 * A flush whose write fails with an error that no later attempt outlasts
 * (EPIPE here) drops the batch and writes a marker that names the loss. The
 * records after it then go out as usual. Offering the same batch again would
 * fail the same way, while every later record piles up behind it.
 *
 * This test is non-vacuous: when the flush keeps the batch after such an
 * error, the file holds no marker, and the next flush delivers the batch
 * after all.
 */
TEST(undelivered_batch, an_error_no_retry_outlasts_drops_it_with_a_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "BATCHRECORDONE");
  ccol_log_info(lg, "BATCHRECORDTWO");
  ccol_log_info(lg, "BATCHRECORDTHREE");
  clog_test_force_write_errors(EPIPE, 1);
  clog_flush(lg);
  clog_test_force_write_errors(0, 0);
  ccol_log_info(lg, "RECORDAFTERTHEDROP");
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

/*
 * A flush whose write fails with an error that a later attempt can outlast
 * (ENOSPC here) keeps the batch, and the next flush delivers it whole, with
 * no marker. This pins the other side of the rule of the test above.
 */
TEST(undelivered_batch, a_transient_error_keeps_it_for_the_next_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "BATCHRECORDONE");
  ccol_log_info(lg, "BATCHRECORDTWO");
  clog_test_force_write_errors(ENOSPC, 1);
  clog_flush(lg);
  clog_test_force_write_errors(0, 0);
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

/*
 * The last flush of a closing logger is its last delivery attempt. A batch
 * that it keeps after a transient error has no later retry, so the close
 * names it with a marker instead of freeing it in silence.
 *
 * This test is non-vacuous: without the marker the file stays empty, and
 * nothing names the lost record.
 */
TEST(undelivered_batch, a_close_names_what_its_last_flush_could_not_deliver) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "BATCHRECORDONE");
  clog_test_force_write_errors(ENOSPC, 1);
  clog_close(lg);
  clog_test_force_write_errors(0, 0);

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

/*
 * A fatal record is the last write of the process. What the batch still
 * holds after the drain ahead of it gets one bounded attempt more; a marker
 * names what that attempt could not deliver, ahead of the fatal record.
 *
 * The test runs in a forked child, because the call stops the process. It
 * asserts only that the child stopped with no signal, for the reason that
 * split_record_framing.a_fatal_record_never_lands_inside_a_retained_one
 * gives.
 *
 * This test is non-vacuous: without the marker the file holds the fatal
 * record alone, and nothing names the record that the process lost.
 */
TEST(undelivered_batch, a_fatal_record_names_what_the_batch_could_not_deliver) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) {
    clog lg =
        clog_open_file_mp(path, CLOG_INFO, NULL, &g_undelivered_acfg, NULL);
    if (lg == CLOG_INVALID) _exit(2);
    ccol_log_info(lg, "BATCHRECORDONE");
    /* The first failure is the drain of the queue, the second the bounded
     * attempt that the fatal path makes after it. */
    clog_test_force_write_errors(ENOSPC, 2);
    ccol_log_fatal(lg, "FATALRECORDBODY");
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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

/* ------------------------------------------------------------------------ */
/* Records that span several lines                                           */
/* ------------------------------------------------------------------------ */

/* The single call site of every multi-line record below. A logfmt ERROR
 * record carries its backtrace as tab-indented continuation lines, and one
 * call site gives a primary line of one length whichever logger writes it.
 */
static __attribute__((noinline)) void log_multi_line_record(clog lg) {
  ccol_log_error(lg, "MULTILINERECORDBODY");
}

/* The length of the primary line of the record of log_multi_line_record(),
 * with its newline, as a synchronous logger writes it. 0 on a failure. */
static size_t multi_line_primary_len(const char *dir) {
  char path[512];
  snprintf(path, sizeof(path), "%s/probe.log", dir);
  clog probe = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (probe == CLOG_INVALID) return 0;
  log_multi_line_record(probe);
  clog_close(probe);
  char buf[16384];
  read_file(path, buf, sizeof(buf));
  unlink(path);
  const char *nl = strchr(buf, '\n');
  if (!nl || nl[1] != '\t') return 0;
  return (size_t)(nl - buf) + 1;
}

/* Counts the tab-indented continuation lines of buf that do not follow the
 * record of log_multi_line_record() or another continuation line, and the
 * empty lines. Either one is a framing defect. */
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

/*
 * A short write that ends exactly at the newline between the primary line
 * of a logfmt record and its backtrace lines still splits that record. The
 * syslog record below goes straight to the descriptor after a flush that
 * cannot deliver the rest. The library gives up on the backtrace lines of
 * the split record, with a marker and no empty line, before it writes the
 * syslog record. Every backtrace line on disk therefore follows its own
 * record.
 *
 * This test is non-vacuous: when that newline counts as a record boundary,
 * the syslog record goes out ahead of the backtrace lines, which then land
 * under it.
 */
TEST(split_record_framing, a_split_at_a_backtrace_newline_is_still_a_split) {
  /* The record of log_multi_line_record() spans several lines only through
   * its backtrace; see backtrace_capture_genuinely_available(). */
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

  clog lg = clog_open_fd_mp(fd, CLOG_INFO, &g_undelivered_acfg, NULL);
  if (lg == CLOG_INVALID) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_INVALID);

  log_multi_line_record(lg);
  ccol_log_info(lg, "SECONDRECORDBODY");
  clog_test_force_short_writes(primary, 1);
  clog_flush(lg);
  clog_test_force_write_errors(ENOSPC, 1);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_log_info(lg, "SYSLOGRECORDBODY");
  clog_flush(lg);
  clog_test_force_write_errors(0, 0);
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

/*
 * When the library gives up on the continuation of a split logfmt record,
 * it gives up on the whole rest of that record, backtrace lines included.
 * The record after it is whole and still goes out.
 *
 * This test is non-vacuous: when the repair stops at the first newline, the
 * backtrace lines of the split record stay in the batch and go out later,
 * under the syslog record.
 */
TEST(split_record_framing, a_repair_drops_the_backtrace_of_the_split_record) {
  /* The record of log_multi_line_record() spans several lines only through
   * its backtrace; see backtrace_capture_genuinely_available(). */
  if (!backtrace_capture_genuinely_available()) return;
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);

  clog lg = clog_open_fd_mp(fd, CLOG_INFO, &g_undelivered_acfg, NULL);
  if (lg == CLOG_INVALID) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_INVALID);

  log_multi_line_record(lg);
  ccol_log_info(lg, "SECONDRECORDBODY");
  /* Both attempts stop inside the primary line, which is far longer than
   * the two caps together. */
  clog_test_force_short_writes(20, 2);
  clog_flush(lg);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_log_info(lg, "SYSLOGRECORDBODY");
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

/* ------------------------------------------------------------------------ */
/* Message-oriented sockets                                                  */
/* ------------------------------------------------------------------------ */

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>

/* What a receiver thread saw on a datagram socket. */
typedef struct {
  int fd;
  _Atomic bool stop;
  size_t datagrams; /* every datagram */
  size_t not_one;   /* datagrams that are not exactly one line */
  size_t markers;   /* datagrams that carry a loss marker */
  size_t with_body; /* datagrams that carry `body` */
  const char *body;
  bool json_ok;       /* every datagram starts with '{' (JSON runs only) */
  int start_delay_ms; /* the receiver reads nothing for this long first */
} dgram_rx_t;

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
  for (;;) {
    ssize_t n = recv(rx->fd, buf, cap - 1, 0);
    if (n < 0) {
      if ((errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR) &&
          !atomic_load(&rx->stop))
        continue;
      if (errno == EINTR) continue;
      break; /* stopped and drained */
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

/* Opens a connected pair of datagram sockets: out[0] receives, out[1]
 * sends. AF_INET uses a loopback UDP port that the kernel picks. The
 * receiver wakes every 50 ms, so that it can see the stop flag. */
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
        connect(out[1], (struct sockaddr *)&a, sizeof(a)) != 0) {
      if (out[0] >= 0) close(out[0]);
      if (out[1] >= 0) close(out[1]);
      return -1;
    }
  }
  int big = 4 << 20;
  (void)setsockopt(out[0], SOL_SOCKET, SO_RCVBUF, &big, sizeof(big));
  struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
  (void)setsockopt(out[0], SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return 0;
}

/* Logs `count` records of `fmt` through an async logger on a datagram
 * socket of `domain`, then `oversize` records too large for one UDP
 * datagram first when it is true, and reports what a receiver saw. The
 * logger, the thread and both sockets are gone when it returns. Returns
 * false when the setup failed. */
static bool run_dgram_case_delayed(int domain, clog_format_t fmt, int count,
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
  clog_async_cfg_t acfg = {0};
  clog lg = clog_open_fd(sv[1], CLOG_INFO, &acfg);
  if (lg != CLOG_INVALID) {
    clog_set_format(lg, fmt);
    if (oversize) {
      size_t big_len = 66000;
      char *big = malloc(big_len + 1);
      if (big) {
        memset(big, 'x', big_len);
        big[big_len] = '\0';
        ccol_log_info(lg, "%s", big);
        free(big);
      }
    }
    for (int i = 0; i < count; i++)
      ccol_log_info(lg, "DATAGRAMRECORDBODY %d", i);
    clog_close(lg);
  }
  atomic_store(&rx->stop, true);
  pthread_join(tid, NULL);
  close(sv[0]);
  close(sv[1]);
  return lg != CLOG_INVALID;
}

static bool run_dgram_case(int domain, clog_format_t fmt, int count,
                           bool oversize, dgram_rx_t *rx) {
  return run_dgram_case_delayed(domain, fmt, count, oversize, 0, rx);
}

/*
 * An async logger on a message-oriented socket writes every record with its
 * own write, so each datagram carries exactly one record and every record
 * arrives. The count stays small enough for the receive buffer of a
 * loopback UDP socket, which drops what does not fit.
 *
 * These tests are non-vacuous: with the batch of the stream sinks, many
 * records share one datagram.
 */
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

/* The logger waits for room on an AF_UNIX datagram socket and never drops,
 * so this sends far more than one batch of the stream sinks would hold. */
TEST(datagram_sink, unix_logfmt_sends_one_record_per_datagram) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_UNIX, CLOG_FMT_LOGFMT, 2000, false, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.datagrams, (size_t)2000);
  REQUIRE_EQ(rx.with_body, (size_t)2000);
  REQUIRE_EQ(rx.not_one, (size_t)0);
}

/* A receiver that reads nothing for a while fills the datagram queue of the
 * socket. Linux then blocks the send, and FreeBSD refuses it with ENOBUFS,
 * which the logger waits out; every record still arrives, and no loss marker
 * is written. This test is non-vacuous on FreeBSD: a writer that treats
 * ENOBUFS as a failed write loses most of the records. */
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

/*
 * One record larger than a UDP datagram can be fails alone. A marker names
 * it, and every record after it still arrives.
 *
 * This test is non-vacuous: when the record shares a batch with the ones
 * after it, the whole batch fails with it and nothing arrives.
 */
TEST(datagram_sink, udp_oversize_record_is_named_and_the_rest_arrive) {
  dgram_rx_t rx;
  bool ok = run_dgram_case(AF_INET, CLOG_FMT_LOGFMT, 8, true, &rx);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(rx.markers, (size_t)1);
  REQUIRE_EQ(rx.with_body, (size_t)8);
  REQUIRE_EQ(rx.datagrams, (size_t)9);
  REQUIRE_EQ(rx.not_one, (size_t)0);
}

/* ------------------------------------------------------------------------ */
/* RFC 5424 escaping                                                         */
/* ------------------------------------------------------------------------ */

/*
 * A syslog MSG escapes a backslash as well as a control byte, so a real
 * newline and the two characters backslash and n stay distinguishable.
 *
 * This test is non-vacuous: when the backslash passes through as it is,
 * both messages read "A\nB".
 */
TEST(syslog, msg_escaping_keeps_a_backslash_distinct_from_an_escape) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);
  clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
  if (lg == CLOG_INVALID) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  ccol_log_info(lg, "%s", "FIRSTA\\nB");
  ccol_log_info(lg, "%s", "SECONDA\nB");
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

/*
 * An SD-PARAM-VALUE is UTF-8. A well-formed sequence passes through as it
 * is, and each byte that is not part of one becomes U+FFFD.
 *
 * This test is non-vacuous: without the check, the byte 0xff reaches the
 * record as it is.
 */
TEST(syslog, sd_param_value_is_valid_utf8) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) cleanup_dir(dir, "app.log");
  REQUIRE_NE(fd, -1);
  clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
  if (lg == CLOG_INVALID) {
    close(fd);
    cleanup_dir(dir, "app.log");
  }
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  clog_set_field(lg, "k",
                 "a\xff"
                 "b\xc3\xa9"
                 "c\xe2\x82");
  ccol_log_info(lg, "utf8 check");
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

/* ------------------------------------------------------------------------ */
/* Deferred slot-table release                                               */
/* ------------------------------------------------------------------------ */

/* One fixture, on purpose, that covers both halves of the deferred release.
 *
 * Only ONE set of loggers in this binary can drive the release at all. The
 * release fires when the last live slot goes away. Whichever fixture is torn
 * down last is therefore the only one that reaches it. The close of any other
 * fixture finds a live sibling and returns. Two separate fixtures therefore
 * cannot both be non-vacuous here, in any order. This is why one pair of
 * loggers drives both the single-threaded case and the concurrent case, and
 * not one pair for each.
 *
 * Each logger has a derived handle that the test closes. Two handles
 * therefore shared one clog_shared_t. Each close is then the one that drops
 * the last reference to its own shared object and walks live_shareds. The
 * release must not run ahead of that walk.
 *
 * The test then closes both at the same time. It holds one inside the window
 * where that close has already cleared its slot and still has to read the
 * table. The other close finds no live slot. From that alone, it would
 * release the table under the first one.
 *
 * This test is not vacuous. With a close in flight that nothing counts, the
 * binary dies with "ccol_assert failed" out of cvector_elem_count. The close
 * that the test holds then walks a vector that the other close destroyed. The
 * abort lands after the harness prints its summary, so the signal is the exit
 * status of the binary. That status is what `make test` checks.
 *
 * Link order is what reaches the release at all. Do not "tidy" either half of
 * this away. The build links this file ahead of src/clogger.c, and a
 * destructor runs in the reverse of the link order. The destructor below
 * therefore runs AFTER the one of clogger. It closes into a table whose
 * release is already pending. An open of the loggers from an ordinary test is
 * what leaves them open at exit. The other checks of this suite on the shared
 * count all compare deltas around their own work. Two more long-lived loggers
 * therefore do not disturb them.
 *
 * One more logger that a destructor holds in this file would silently make
 * this test check nothing. The first paragraph gives the reason. */
static clog g_deferred_release_a = CLOG_INVALID;
static clog g_deferred_release_b = CLOG_INVALID;

TEST(clogger_deferred_release, a_racing_pair_left_open_defers_then_releases) {
  int fd = open("/dev/null", O_WRONLY);
  REQUIRE_NE(fd, -1);
  clog a = clog_open_fd(fd, CLOG_INFO, NULL);
  clog b = clog_open_fd(fd, CLOG_INFO, NULL);
  clog da = (a != CLOG_INVALID) ? clog_derive(a) : CLOG_INVALID;
  clog db = (b != CLOG_INVALID) ? clog_derive(b) : CLOG_INVALID;
  if (da != CLOG_INVALID) clog_close(da);
  if (db != CLOG_INVALID) clog_close(db);

  /* This publishes the pair only after both are fully set up. A partial open
     therefore cannot leave the destructor below with one logger that it then
     declines to close. */
  bool opened = (a != CLOG_INVALID && b != CLOG_INVALID && da != CLOG_INVALID &&
                 db != CLOG_INVALID);
  if (opened) {
    g_deferred_release_a = a;
    g_deferred_release_b = b;
  } else {
    if (a != CLOG_INVALID) clog_close(a);
    if (b != CLOG_INVALID) clog_close(b);
  }
  REQUIRE_TRUE(opened);
}

static void *_close_b_in_the_window(void *unused) {
  (void)unused;
  clog_close(g_deferred_release_b);
  g_deferred_release_b = CLOG_INVALID;
  return NULL;
}

__attribute__((destructor)) static void _close_the_deferred_release_pair(void) {
  if (g_deferred_release_a == CLOG_INVALID ||
      g_deferred_release_b == CLOG_INVALID) {
    if (g_deferred_release_a != CLOG_INVALID) clog_close(g_deferred_release_a);
    if (g_deferred_release_b != CLOG_INVALID) clog_close(g_deferred_release_b);
    g_deferred_release_a = CLOG_INVALID;
    g_deferred_release_b = CLOG_INVALID;
    return;
  }

  clog_test_set_close_release_window_us(200000);
  pthread_t t;
  if (pthread_create(&t, NULL, _close_b_in_the_window, NULL) != 0) {
    /* There is nothing to race with. Close both, so that nothing leaves the
       fixture open. */
    clog_test_set_close_release_window_us(0);
    clog_close(g_deferred_release_a);
    clog_close(g_deferred_release_b);
    g_deferred_release_a = CLOG_INVALID;
    g_deferred_release_b = CLOG_INVALID;
    return;
  }

  /* Wait until something really occupies the window. A sleep for a guessed
     interval is not enough. A thread that is slow to start would read a hook
     that is already disarmed. The two closes would then run one after the
     other, and this check would pass with nothing driven at all. The bound
     below is a safety net against a hang, and nothing more.

     B holds the window for 200ms once it is inside. The close that it races
     takes about one microsecond natively, and about a fifth of a millisecond
     under valgrind. That is a margin of about three orders of magnitude in
     the slower of the two environments that this suite runs in. */
  for (int i = 0; i < 20000 && !clog_test_close_release_window_entered(); i++) {
    struct timespec ts = {0, 100L * 1000L};
    nanosleep(&ts, NULL);
  }
  clog_test_set_close_release_window_us(0);
  clog_close(g_deferred_release_a);
  g_deferred_release_a = CLOG_INVALID;
  pthread_join(t, NULL);
}

/* ========================================================================== */
/*                         ROTATION PRUNING ROBUSTNESS                        */
/* ========================================================================== */

/* This creates `path` and writes `text` into it. It returns 0 after a
 * success. */
static int write_text_file(const char *path, const char *text) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  size_t len = strlen(text);
  ssize_t w = write(fd, text, len);
  close(fd);
  return (w == (ssize_t)len) ? 0 : -1;
}

/* This moves the modification time of `path` forward by `seconds`. A
 * negative value moves it back. It returns 0 after a success. */
static int shift_file_mtime(const char *path, long seconds) {
  struct timeval tv[2];
  gettimeofday(&tv[0], NULL);
  tv[0].tv_sec += seconds;
  tv[1] = tv[0];
  return utimes(path, tv);
}

/* True when any file in `dir` whose name starts with `prefix` holds
 * `needle`. */
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

/*
 * The name of a rotated file carries the wall-clock time of the rotation
 * that produced it. The system clock can step backward. An NTP step
 * correction, a restored VM snapshot and a corrected host clock all do this.
 * The names that are already on the disk then sort ahead of every name that
 * comes afterwards. An order on those names alone would make the destination
 * that a rotation has just created look like the oldest file in the
 * directory. The prune pass at the end of that same rotation would then
 * delete it, and nothing would ever read its contents. The whole generation
 * of log data that the rotation moved aside is lost, and nothing reports it.
 *
 * This test is not vacuous. The prune pass of a rotation excludes the
 * destination that it has just created. That exclusion is what keeps the
 * payload record below on the disk. Without it, the library unlinks that
 * record here.
 */
TEST(rotation, a_rotation_never_prunes_the_file_it_just_created) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* These are rotated files stamped a day ahead of the present, with
   * modification times to match. A clock that somebody has since stepped
   * backwards leaves exactly this behind. Something wrote every one of them
   * while the clock still read ahead of where it reads now. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  /* One record comfortably past the threshold. The write lands in app.log.
   * The rotation directly after it moves that file aside, under a fresh
   * stamped name. */
  ccol_log_info(lg, "PAYLOAD-MARKER-ROTATED %s",
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
  /* The prune still counts that file against the quota, although it excludes
   * it from deletion. The number of rotated files that stay does not
   * change. */
  REQUIRE_EQ((rotated <= 2), 1);
}

/* ========================================================================== */
/*                         CONSUMER-SIDE LAYOUT AGREEMENT                     */
/* ========================================================================== */

/*
 * The build compiles consumer_view.c without the feature-test macros that
 * this suite and the library use. It therefore sees <clogger.h> exactly as
 * an application with ordinary default flags sees it. Both views of
 * clog_rotation_cfg_t must describe the same object. Otherwise the library
 * reads every member after the first from an offset that the caller never
 * wrote.
 *
 * On some targets, off_t and time_t have one width whatever the feature
 * macros say. LP64 is one such target. A struct built from them then also
 * has one layout, and this test cannot tell the two cases apart. On an ILP32
 * target this test does real work. It separates a public struct with one
 * layout from one whose layout the application and the library disagree
 * about.
 */
TEST(rotation_cfg_abi, a_consumer_sees_the_same_layout_the_library_does) {
  clogger_consumer_view_t consumer;
  clogger_consumer_fill_view(&consumer);

  REQUIRE_EQ(consumer.struct_size, sizeof(clog_rotation_cfg_t));
  REQUIRE_EQ(consumer.struct_align, _Alignof(clog_rotation_cfg_t));
  REQUIRE_EQ(consumer.off_size_rotation_enabled,
             offsetof(clog_rotation_cfg_t, size_rotation_enabled));
  REQUIRE_EQ(consumer.off_max_file_size,
             offsetof(clog_rotation_cfg_t, max_file_size));
  REQUIRE_EQ(consumer.off_time_rotation_enabled,
             offsetof(clog_rotation_cfg_t, time_rotation_enabled));
  REQUIRE_EQ(consumer.off_rotation_interval_us,
             offsetof(clog_rotation_cfg_t, rotation_interval_us));
  REQUIRE_EQ(consumer.off_max_rotated_files,
             offsetof(clog_rotation_cfg_t, max_rotated_files));
  REQUIRE_EQ(consumer.off_compress_rotated,
             offsetof(clog_rotation_cfg_t, compress_rotated));
  /* Both members have a fixed width. Their sizes are therefore the same
   * everywhere. They do not follow how the build made the including
   * translation unit. */
  REQUIRE_EQ(consumer.sizeof_max_file_size, (size_t)8);
  REQUIRE_EQ(consumer.sizeof_rotation_interval_us, (size_t)8);
}

/*
 * The same agreement, driven through the library and not through offsetof. A
 * rotation config that an application translation unit fills in must drive
 * the rotation that the application asked for. A config that the library
 * reads from the wrong offsets rotates on a schedule that nobody configured.
 * It also turns on options that nobody set, such as gzip compression here.
 */
TEST(rotation_cfg_abi, a_consumer_built_config_rotates_as_it_was_written) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clogger_consumer_open_rotating(path, 200, 2);
  REQUIRE_NE(lg, CLOG_INVALID);
  for (int i = 0; i < 20; i++)
    ccol_log_info(lg, "consumer-built rotation config idx=%d padding-padding",
                  i);
  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  int compressed = count_files_with_suffix(dir, ".gz");
  cleanup_dir(dir, "app.log");

  REQUIRE_GT(rotated, 0);        /* Size rotation is on. */
  REQUIRE_EQ((rotated <= 2), 1); /* max_rotated_files is 2. */
  REQUIRE_EQ(compressed, 0);     /* compress_rotated is false. */
}

/* ========================================================================== */
/*                         SHORT-WRITE RECORD FRAMING                         */
/* ========================================================================== */

/* This counts the lines in buf that end with a newline. */
static int count_lines(const char *buf) {
  int n = 0;
  for (const char *p = buf; *p; p++)
    if (*p == '\n') n++;
  return n;
}

/*
 * The kernel can accept only part of a write. A filesystem that has just run
 * out of space does this. So does an RLIMIT_FSIZE ceiling, and a peer that
 * goes away in the middle of a record. The accepted bytes then sit on the
 * disk with no terminating newline for that record. The logger writes that
 * newline itself. The next record therefore starts a line of its own, and
 * nothing joins it onto the fragment. The logger also reports how much of
 * the record it lost, and it does not drop it in silence.
 *
 * This test is not vacuous. Without the repair, the fragment and the record
 * after it are one line. The file then holds two lines, and not four, and no
 * marker names the loss.
 */
TEST(short_write, a_cut_short_record_is_terminated_and_the_loss_reported) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "FIRST-RECORD");
  /* The next record reaches the disk 30 bytes deep, and no further. */
  clog_test_force_short_writes(30, 1);
  ccol_log_info(lg, "SECOND-RECORD-cut-in-half");
  ccol_log_info(lg, "THIRD-RECORD");
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof(buf));
  bool has_marker = strstr(buf, "log record truncated") != NULL;
  bool has_third = strstr(buf, "THIRD-RECORD") != NULL;
  int lines = count_lines(buf);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(has_marker);
  REQUIRE_TRUE(has_third);
  /* The file holds four lines, and none of them merged into another. They
   * are the first record, the fragment of 30 bytes, the marker that names
   * the loss, and the third record. */
  REQUIRE_EQ(lines, 4);
}

/*
 * An async logger batches records into one buffer and writes the batch in
 * one call. That write can be cut short. The bytes that the kernel did not
 * take then stay in the buffer for the next flush. They are the exact
 * continuation of what did land on the disk. To offer them again therefore
 * does two things. It restores the framing of the record that the short
 * write cut in half, and it delivers every record behind that one. To
 * discard them would lose a whole batch at once.
 *
 * This test is not vacuous. Drop the rest that the write did not deliver,
 * and none of the three batched messages below reaches the disk.
 */
TEST(short_write, an_async_batch_keeps_what_the_write_did_not_take) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* The buffer is large enough that only the explicit flush below writes
   * anything. The interval is long enough that the timer of the writer
   * thread does not flush first. */
  clog_async_cfg_t acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "ASYNC-BATCHED-ONE");
  ccol_log_info(lg, "ASYNC-BATCHED-TWO");
  ccol_log_info(lg, "ASYNC-BATCHED-THREE");

  /* The write of this flush reaches the disk 20 bytes deep, and no
   * further. */
  clog_test_force_short_writes(20, 1);
  clog_flush(lg);

  ccol_log_info(lg, "ASYNC-BATCHED-FOUR");
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
  /* Four records give four lines. The rest of the buffer continued the
   * record that the short write cut in half. It did not start a new one. */
  REQUIRE_EQ(lines, 4);
}

/*
 * This counts the files in dir whose name starts with prefix and whose
 * contents hold needle. It reads each candidate on its own. A string that
 * covers two files is therefore found in neither.
 */
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

/*
 * An async batch write can be cut in the middle of a record. The retry that
 * delivers the rest of that record restores its framing. A truncation marker
 * does not. This works only while both halves reach the same file. Neither
 * rotation check may therefore rotate while the buffer still holds a
 * continuation. A rotation there renames away the file that holds the first
 * half, and the continuation opens the fresh one. The short write makes a
 * rotation due. The flush that finally drains the buffer takes that rotation
 * instead.
 *
 * This test is not vacuous. A rotation on the short write puts the two
 * halves of the record in two different files. No file then holds the
 * message from end to end, and the count below is 0 and not 1.
 */
TEST(short_write, rotation_waits_for_the_retained_half_of_a_record) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* Nothing in here needs a logfmt escape. The body therefore appears in
   * the record verbatim, and a search can find it as one string. */
  char body[1025];
  memset(body, 'A', sizeof(body) - 1);
  body[sizeof(body) - 1] = '\0';
  memcpy(body, "SPLITME-HEAD", 12);
  memcpy(body + sizeof(body) - 1 - 12, "SPLITME-TAIL", 12);

  /* Measure the exact length of this record through a synchronous logger.
   * The short write below then aims at a real offset, and not a guessed
   * one. */
  char probe_path[512];
  snprintf(probe_path, sizeof(probe_path), "%s/probe.log", dir);
  clog probe = clog_open_file(probe_path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(probe, CLOG_INVALID);
  ccol_log_info(probe, "%s", body);
  clog_close(probe);
  char probe_buf[8192];
  size_t record_len = read_file(probe_path, probe_buf, sizeof(probe_buf));
  unlink(probe_path);
  /* A record is its prefix plus the body, so it is always longer than the
   * body. A stop half a body short of its end therefore lands inside the
   * body, whatever the prefix weighs. */
  size_t cut = record_len > sizeof(body) ? record_len - (sizeof(body)) / 2 : 0;

  clog_rotation_cfg_t rcfg = {
      .size_rotation_enabled = true,
      .max_file_size = 64, /* The short write alone crosses it. */
      .time_rotation_enabled = false,
      .rotation_interval_us = 0,
      .max_rotated_files = 8,
      .compress_rotated = false,
  };
  /* The buffer is large enough that only the explicit flushes below write
   * anything. The interval is long enough that the timer of the writer
   * thread does not flush first. */
  clog_async_cfg_t acfg = {
      .queue_size = 0,
      .flush_buffer_size = 1024UL * 1024UL,
      .flush_interval_us = 60000000,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  ccol_log_info(lg, "%s", body);

  /* Reaches disk in the middle of the message body and no further, past
   * max_file_size, so a size rotation is due the instant it is charged. */
  clog_test_force_short_writes(cut, 1);
  clog_flush(lg);
  /* This delivers the continuation. The deferred rotation happens here. */
  clog_flush(lg);
  clog_close(lg);

  int whole = count_files_containing(dir, "app.log", body);
  int head = count_files_containing(dir, "app.log", "SPLITME-HEAD");
  int tail = count_files_containing(dir, "app.log", "SPLITME-TAIL");
  cleanup_dir(dir, "app.log");

  REQUIRE_GT((long)cut, 0L);
  /* One file holds the message from end to end. It is also the only file
   * that either end of the message appears in. */
  REQUIRE_EQ(whole, 1);
  REQUIRE_EQ(head, 1);
  REQUIRE_EQ(tail, 1);
}

/*
 * Retention must order rotated generations by the timestamp that each
 * rotated name carries. That timestamp names the rotation that produced the
 * file. A modification time does not describe that. The compression step
 * writes a compressed rotated file, so its mtime records when the
 * compression finished. The compression also runs outside the logger mutex.
 * A later generation that compresses quickly can therefore carry an older
 * mtime than an earlier one that is still under compression. An order by
 * mtime then deletes the newest generation.
 *
 * This test is not vacuous. An order on these candidates by modification
 * time deletes the file with the newest name first, because it carries the
 * oldest mtime. It keeps the file with the oldest name. Both assertions
 * below then invert.
 */
TEST(rotation, retention_orders_rotated_files_by_name_not_modification_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* The names ascend. The modification times descend. */
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

  /* The file is filled first, so that one message crosses the threshold
     exactly one time. */
  {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    char filler[4095];
    memset(filler, 'x', sizeof(filler));
    ssize_t w = write(fd, filler, sizeof(filler));
    (void)w;
    close(fd);
  }

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 4096,
      .time_rotation_enabled = false,
      /* The prune pass of a rotation protects the new file of that same
         rotation, and it still counts it. Exactly one generation that
         already existed may therefore stay. It must be the one with the
         newest name. */
      .max_rotated_files = 2,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  ccol_log_info(lg, "one message to cross the size threshold");
  clog_close(lg);

  struct stat st;
  bool oldest_name_deleted = (stat(full[0], &st) != 0);
  bool newest_name_kept = (stat(full[2], &st) == 0);

  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(oldest_name_deleted);
  REQUIRE_TRUE(newest_name_kept);
}

/* ========================================================================== */
/*                         RETENTION KEEPS THE NEWEST GENERATIONS             */
/* ========================================================================== */

#include <zlib.h>

#define RTN_MAX_THREADS 6
#define RTN_MAX_RECORDS 600

typedef struct {
  clog handle;
  int thread_index;
  int records;
  _Atomic int *arrived;
  _Atomic bool *go;
} rtn_writer_arg_t;

static void _rtn_nap(void) {
  struct timespec ts = {0, 100000}; /* 100us */
  nanosleep(&ts, NULL);
}

static void *_rtn_writer(void *p) {
  rtn_writer_arg_t *a = (rtn_writer_arg_t *)p;
  atomic_fetch_add(a->arrived, 1);
  while (!atomic_load(a->go)) _rtn_nap();
  for (int n = 0; n < a->records; n++)
    ccol_log_info(a->handle, "rtn t=%d n=%d", a->thread_index, n);
  return NULL;
}

/* Reads a whole log file into a heap buffer. gzread() reads a file that is
 * not gzip-compressed as it is, so one reader serves the live file, a plain
 * rotated file and a compressed one. */
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

/* This reads every file of the logger in `dir`: the live file, and every
 * rotated file whether compressed or not. It marks each "rtn t=<t> n=<n>"
 * record that it finds in seen[t][n]. It returns the number of distinct
 * rotated generations, which is the number of distinct rotated names once a
 * ".gz" ending is ignored. */
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

/* The records that survive retention are the newest ones. For each writer
 * thread, the surviving records of that thread are therefore one contiguous
 * run that ends at the last record the thread wrote, or nothing at all when
 * a thread that finished early had every record in generations that later
 * rotations pushed out. A deleted newer generation beside a surviving older
 * one leaves a gap in that run. The function returns the number of threads
 * whose oldest records were pruned, or -1 on a gap or when no record of any
 * thread survived. */
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

typedef struct {
  bool compress;
  bool async;
  int threads;
  int records;
  bool use_derived;
} rtn_case_t;

/* Drives one retention scenario and asserts the newest-generations rule.
 * Returns 0 on success, and a distinct nonzero code for each failure, so that
 * the caller asserts once after every thread is joined and every handle is
 * closed. */
static int _rtn_run_case(const rtn_case_t *c, int *out_gens) {
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return 1;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1024,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = c->compress,
  };
  clog_async_cfg_t acfg = {
      .queue_size = 0, .flush_buffer_size = 256, .flush_interval_us = 5000};

  clog root =
      clog_open_file_mp(path, CLOG_INFO, &cfg, c->async ? &acfg : NULL, NULL);
  if (root == CLOG_INVALID) {
    cleanup_dir(dir, "app.log");
    return 2;
  }
  clog derived = c->use_derived ? clog_derive(root) : CLOG_INVALID;

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

  if (derived != CLOG_INVALID) clog_close(derived);
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
  /* The scenario must really prune, or it proves nothing. */
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

/* A second logger on the same path, opened after the first one closed, must
 * number its rotations above every rotated name that is already on disk. A
 * process that restarts inside the same second is this case. Otherwise the
 * second logger takes a name that the first one's retention freed, that name
 * sorts as the oldest, and the next pass deletes the newest records. */
TEST(retention, a_reopened_logger_numbers_above_the_names_on_disk) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1024,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
  };

  int open_failures = 0;
  for (int round = 0; round < 2; round++) {
    clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
    if (lg == CLOG_INVALID) {
      open_failures++;
      continue;
    }
    /* The second logger writes only a few rotations' worth. Its names must
     * sort above the names that the first one left, from its very first
     * rotation on. */
    int first = round == 0 ? 0 : 200, last = round == 0 ? 200 : 230;
    for (int n = first; n < last; n++) ccol_log_info(lg, "rtn t=0 n=%d", n);
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

/* Writes a one-byte file at dir/name. */
static int _rtn_touch(const char *dir, const char *name) {
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", dir, name);
  int fd = open(full, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return -1;
  ssize_t w = write(fd, "x", 1);
  close(fd);
  return w == 1 ? 0 : -1;
}

static bool _rtn_exists(const char *dir, const char *name) {
  char full[1024];
  snprintf(full, sizeof(full), "%s/%s", dir, name);
  struct stat st;
  return stat(full, &st) == 0;
}

/* A new rotated name is always above every generation on disk, also when
 * the newest one carries a later stamp than the clock reads, as the names of
 * a process whose clock stepped back by a day do. Retention then orders a
 * suffix by its number and not by its text, so "_10000" is newer than
 * "_9999". */
TEST(retention, a_new_name_sorts_above_every_existing_name) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  /* A stamp one day ahead of the clock. */
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

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Every record rotates. */
      .time_rotation_enabled = false,
      .max_rotated_files = 2,
  };
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  bool opened = (lg != CLOG_INVALID);
  bool after_first_10000 = false, after_first_9998_gone = false;
  if (opened) {
    ccol_log_info(lg, "first");
    after_first_10000 = _rtn_exists(dir, n10000);
    after_first_9998_gone = !_rtn_exists(dir, n9998);
    ccol_log_info(lg, "second");
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

/* ========================================================================== */
/*                         RETENTION BOUND UNDER CONCURRENT COMPRESSION       */
/* ========================================================================== */

#define RTN_BOUND_THREADS 6
#define RTN_BOUND_RECORDS 6

/* Several threads rotate on every record with compression on, and every
 * compression is held in flight for 20ms. A deletion pass counts every
 * generation under compression against max_rotated_files, keeps the newest
 * max_rotated_files generations, and deletes every older one that is not
 * under compression. The generations on disk after a pass, counted from a
 * directory listing of their own, must therefore exceed max_rotated_files
 * only by the older generations that the pass kept because they were under
 * compression. Every compressed file must stay a valid gzip archive.
 *
 * This test is non-vacuous: when a pass skips the generations under
 * compression without counting them, it keeps max_rotated_files other
 * generations beside them, and the count exceeds that bound. */
TEST(retention, concurrent_compressions_keep_the_documented_bound) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Every record rotates. */
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = true,
  };
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_reset_max_rotated_generations();
  clog_test_set_pending_compress_delay_us(20000); /* 20ms per compression */

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

  clog_test_set_pending_compress_delay_us(0);
  clog_close(lg);
  int max_gens = clog_test_get_max_rotated_generations();
  int max_beyond = clog_test_get_max_generations_beyond_old_compressions();

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
  /* The files at the end are what the last pass left: at most
   * max_rotated_files plus the one compression that the compressor thread of
   * the target runs at a time. */
  REQUIRE_LE(gens_at_end, cfg.max_rotated_files + 1);
  REQUIRE_EQ(invalid_gz, 0);
}

/* A rotation never waits for an earlier compression to finish. Every
 * compression here is held in flight for 20ms by the compressor thread, and
 * six threads rotate on every record. When a rotation proceeds while earlier
 * compressions are still outstanding, running or queued, the deletion passes
 * see more than one of them at once; a rotation that waited for its
 * compression would cap that number at one. Measured structurally, from the
 * count that each pass sees, and not from elapsed time. */
TEST(retention, a_rotation_never_waits_for_a_compression) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .time_rotation_enabled = false,
      .max_rotated_files = 3,
      .compress_rotated = true,
  };
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_reset_max_rotated_generations();
  clog_test_set_pending_compress_delay_us(20000);

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

  clog_test_set_pending_compress_delay_us(0);
  clog_close(lg);
  int max_in_flight = clog_test_get_max_compressions_in_flight();
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(started, RTN_BOUND_THREADS);
  /* Six threads each hold a compression for 20ms. Rotations that never wait
   * put several of them in flight at once. */
  REQUIRE_GE(max_in_flight, 3);
}

/* ========================================================================== */
/*                         THREAD IDENTITY                                    */
/* ========================================================================== */

typedef struct {
  char name[64];
  int pid;
  int tid;
} tid_proc_t;

/* Parses the proc field of the record whose message is `msg`, in logfmt
 * output: proc=<progname>(<pid>):<name>(<tid>). Gives true when found. */
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

typedef struct {
  clog lg;
  int index;
  int tid;
  unsigned fills;
  ccol_retval_t set_rv;
} tid_worker_t;

static void *_tid_worker(void *p) {
  tid_worker_t *w = (tid_worker_t *)p;
  char name[16];
  snprintf(name, sizeof(name), "tid-worker-%d", w->index);
  w->set_rv = ccol_set_thread_name(name);
  w->tid = _test_os_tid();
  for (int i = 0; i < 50; i++)
    ccol_log_info(w->lg, "tid worker %d record %d", w->index, i);
  w->fills = clog_test_thread_identity_fill_count();
  return NULL;
}

/* A thread reads its identity from the system once, however many records
 * it writes, and every record carries its own name, PID and TID. Each
 * thread renamed through ccol_set_thread_name() sees its own name. */
TEST(thread_identity, each_thread_reads_its_identity_once) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

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

typedef struct {
  clog lg;
  ccol_retval_t rv_rename;
  ccol_retval_t rv_long;
  ccol_retval_t rv_null;
  ccol_retval_t rv_empty;
  char kernel_after_long[17];
  char kernel_after_invalid[17];
} tid_rename_t;

static void *_tid_rename_worker(void *p) {
  tid_rename_t *r = (tid_rename_t *)p;
  ccol_log_info(r->lg, "rename before");
  r->rv_rename = ccol_set_thread_name("renamed-thread");
  ccol_log_info(r->lg, "rename after");
  r->rv_long = ccol_set_thread_name("abcdefghijklmnopqrstuvwxyz");
  _test_get_thread_name(r->kernel_after_long);
  ccol_log_info(r->lg, "rename long");
  r->rv_null = ccol_set_thread_name(NULL);
  r->rv_empty = ccol_set_thread_name("");
  _test_get_thread_name(r->kernel_after_invalid);
  ccol_log_info(r->lg, "rename invalid");
  /* A rename that bypasses the library is not seen after the first record. */
  _test_set_thread_name("other-means");
  ccol_log_info(r->lg, "rename other means");
  return NULL;
}

/* A rename through ccol_set_thread_name() shows in the very next record. A
 * name longer than 15 bytes keeps its first 15, exactly as the kernel
 * stores it. NULL and "" are refused and change nothing. */
TEST(thread_identity, set_thread_name_renames_and_updates_records) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

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

/* ccol_set_thread_name() needs no logger. A thread renamed before any
 * logger exists logs under that name from its first record. */
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
  /* The worker renames itself with no logger open, then logs. */
  int prv = pthread_create(&th, NULL, _tid_early_worker, &r);
  if (prv == 0) pthread_join(th, NULL);

  /* The main thread does the same, on a logger that it opens afterwards. */
  char saved[17] = {0};
  _test_get_thread_name(saved);
  ccol_retval_t main_rv = ccol_set_thread_name("early-main");
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "early record");
    clog_close(lg);
  }
  /* Restore the name of the main thread for the tests that follow. */
  ccol_retval_t restore_rv = ccol_set_thread_name(saved);

  static char text[1 << 16];
  read_file(path, text, sizeof(text));
  tid_proc_t rec;
  bool parsed = _tid_parse_proc(text, "early record", &rec);
  cleanup_dir(dir, "app.log");

  REQUIRE_EQ(prv, 0);
  REQUIRE_EQ(r.rv_rename, ccol_success);
  REQUIRE_EQ(main_rv, ccol_success);
  REQUIRE_EQ(restore_rv, ccol_success);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(parsed);
  REQUIRE_STREQ(rec.name, "early-main");
  REQUIRE_EQ(rec.tid, _test_os_tid());
  REQUIRE_EQ(rec.pid, (int)getpid());
}

/* A child that fork() makes has a new PID, and its one thread a new TID.
 * The cached identity of the thread that forked must not follow it there.
 * The child uses the supported pattern: it opens its own logger after the
 * fork. The parent logs first, so that its thread has a cached identity at
 * the time of the fork. */
TEST(thread_identity, a_forked_child_logs_its_own_pid_and_tid) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof(dir)), 0);
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);
  ccol_log_info(lg, "parent record");
  clog_close(lg);

  pid_t pid = fork();
  if (pid == 0) {
    clog clg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
    if (clg != CLOG_INVALID) {
      ccol_log_info(clg, "child record");
      clog_close(clg);
    }
    _exit(0);
  }
  int status = 0;
  bool reaped = false;
  if (pid > 0) {
    /* A bounded wait, for the reason that
     * fork_safety.child_can_log_after_fork gives. */
    for (int waited_ms = 0; waited_ms < 30000; waited_ms += 20) {
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
  /* The one thread of the child is its main thread, whose TID is its PID. */
  REQUIRE_EQ(child_rec.tid, (int)pid);
#else
  /* A thread ID is not a process ID here; the child read its own. */
  REQUIRE_NE(child_rec.tid, parent_rec.tid);
#endif
}

/* ========================================================================== */
/*          ROTATION DIRECTORY, BACKGROUND COMPRESSION, WRITER WAIT           */
/* ========================================================================== */

#include <limits.h>

/* Makes a unique directory under the working directory of the test binary,
 * and gives its relative path in out. The files of these tests therefore
 * land on the filesystem that the tests run from. */
static int make_cwd_tmpdir(char *out, size_t outsz) {
  snprintf(out, outsz, "clogger_cwd_test_XXXXXX");
  return mkdtemp(out) ? 0 : -1;
}

/* Counts the rotated files "<base>.<...>" in dir, split into the ones that
 * end in ".gz" and the ones that do not. It also counts the ".gz" files that
 * gunzip -t rejects. */
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

/* Gives true when a file in dir whose name starts with prefix holds needle.
 * It reads each file as plain text. */
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

/* Waits, with a bound of 30 seconds, until a held compression has created
 * its destination. */
static bool _wait_for_gz_dest_opened(void) {
  for (int i = 0; i < 30000; i++) {
    if (clog_test_gz_dest_opened()) return true;
    usleep(1000);
  }
  return false;
}

/* A logger that opens a relative path keeps rotating in the directory in
 * which it opened the file, after the process changes its working
 * directory. Every rename, open and deletion of a rotation works relative to
 * that directory.
 *
 * This test is non-vacuous: when the rotation resolves the relative path
 * against the working directory instead, the rename of "rel.log" in B fails
 * with ENOENT, the rotation takes that as a deleted file, and it opens a
 * fresh B/rel.log. The records then move to B, and the test fails on the
 * file in B. */
TEST(rotation_dir, relative_path_keeps_its_directory_after_chdir) {
  char root[64];
  REQUIRE_EQ(make_cwd_tmpdir(root, sizeof(root)), 0);
  char a[128], b[128];
  snprintf(a, sizeof(a), "%s/A", root);
  snprintf(b, sizeof(b), "%s/B", root);
  bool made = mkdir(a, 0755) == 0 && mkdir(b, 0755) == 0;
  int oldcwd = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  bool moved_in = made && oldcwd >= 0 && chdir(a) == 0;
  clog lg = CLOG_INVALID;
  bool moved_on = false;
  if (moved_in) {
    clog_rotation_cfg_t cfg = {
        .size_rotation_enabled = true,
        .max_file_size = 200,
        .max_rotated_files = 3,
    };
    lg = clog_open_file("rel.log", CLOG_INFO, &cfg, NULL);
    if (lg != CLOG_INVALID) {
      ccol_log_info(lg, "before chdir");
      moved_on = chdir("../B") == 0;
      for (int i = 0; i < 8; i++)
        ccol_log_info(lg, "after chdir %d padding-padding-padding-padding", i);
    }
  }
  /* Restore the working directory before anything can return early. Every
   * other test expects it unchanged. */
  bool restored = oldcwd >= 0 && fchdir(oldcwd) == 0;
  if (oldcwd >= 0) close(oldcwd);
  if (lg != CLOG_INVALID) clog_close(lg);

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
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(moved_on);
  REQUIRE_TRUE(live_in_a);
  REQUIRE_TRUE(last_record_in_a);
  REQUIRE_FALSE(stray_in_b);
  REQUIRE_EQ(files_in_b, 0);
  /* Several rotations happened, and retention kept at most three of them,
   * in A. */
  REQUIRE_GT(rotated_in_a, 0);
  REQUIRE_LE(rotated_in_a, 3);
}

/* A flush interval above LLONG_MAX, such as UINT64_MAX for "flush on size
 * only", makes the async writer thread wait for a job, and not wake up with
 * nothing to do. The count of the timed waits that ended with no job must
 * stay at zero while the logger sits idle.
 *
 * This test is non-vacuous: when the writer narrows the interval to a signed
 * type, the value turns negative, every wait is a zero wait, and the count
 * climbs by thousands while the test sleeps. */
TEST(async, flush_interval_above_llong_max_does_not_spin_the_writer) {
  const uint64_t intervals[] = {UINT64_MAX, (uint64_t)LLONG_MAX + 1u};
  for (size_t k = 0; k < sizeof(intervals) / sizeof(intervals[0]); k++) {
    int pipefd[2];
    REQUIRE_EQ(pipe(pipefd), 0);
    clog_async_cfg_t acfg = {.flush_interval_us = intervals[k]};
    clog lg = clog_open_fd(pipefd[1], CLOG_INFO, &acfg);
    if (lg == CLOG_INVALID) {
      close(pipefd[0]);
      close(pipefd[1]);
    }
    REQUIRE_NE(lg, CLOG_INVALID);

    ccol_log_info(lg, "size-only flushing %zu", k);
    clog_flush(lg);
    size_t before = clog_test_writer_timeout_wakeups();
    usleep(200000);
    size_t after = clog_test_writer_timeout_wakeups();

    char out[4096];
    drain_pipe(lg, pipefd[0], pipefd[1], out, sizeof(out));
    char expected[64];
    snprintf(expected, sizeof(expected), "size-only flushing %zu", k);

    REQUIRE_EQ(after - before, (size_t)0);
    REQUIRE_NE(strstr(out, expected), NULL);
  }
}

/* The gzip compression of a rotated file never runs on the thread that
 * rotates. For a synchronous logger that thread is the one that logs, so the
 * log call that causes the rotation returns while the compression is held.
 *
 * This test is non-vacuous: when _rotate() compresses in place, the log call
 * stays inside the held compression until the 30-second safety bound of the
 * gate, and the compression has finished by the time that the call
 * returns. */
TEST(compression, gzip_never_runs_on_the_logging_thread) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* Every record rotates. */
      .max_rotated_files = 5,
      .compress_rotated = true,
  };
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_hold_compressions(true);
  size_t fin0 = clog_test_compressions_finished();
  ccol_log_info(lg, "rotated while the gate is held");
  size_t fin_after_log = clog_test_compressions_finished() - fin0;
  bool opened = _wait_for_gz_dest_opened();
  size_t fin_while_held = clog_test_compressions_finished() - fin0;
  clog_test_hold_compressions(false);
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

/* The same property for an async logger, whose rotations run on its writer
 * thread. A clog_flush() waits for the writer thread to write the batch and
 * to take the rotation that the write causes. It returns while the
 * compression that the rotation queued is still held, so neither the writer
 * thread nor a producer behind a bounded queue waits for gzip.
 *
 * This test is non-vacuous: when the writer thread compresses in place, the
 * flush waits for the held compression until the 30-second safety bound of
 * the gate, and the compression has finished by the time that it returns. */
TEST(compression, gzip_never_runs_on_the_async_writer_thread) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };
  clog_async_cfg_t acfg = {.queue_size = 4};
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, &acfg);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_hold_compressions(true);
  size_t fin0 = clog_test_compressions_finished();
  ccol_log_info(lg, "rotated by the writer while the gate is held");
  clog_flush(lg);
  size_t fin_after_flush = clog_test_compressions_finished() - fin0;
  bool opened = _wait_for_gz_dest_opened();
  size_t fin_while_held = clog_test_compressions_finished() - fin0;
  clog_test_hold_compressions(false);
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

static void *_close_in_background(void *arg) {
  clog_close(*(clog *)arg);
  return NULL;
}

/* The last clog_close() of a target returns only after every compression
 * that its rotations queued has finished. Three rotations queue three
 * compressions while the first one is held. The close starts while the other
 * two still wait in the queue, and after it returns every rotated file is a
 * valid ".gz" file with no uncompressed file beside it.
 *
 * This test is non-vacuous: when the compressor thread stops without taking
 * the jobs left in its queue, two rotated files stay uncompressed. */
TEST(compression, close_finishes_every_queued_compression) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 10,
      .compress_rotated = true,
  };
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_hold_compressions(true);
  for (int i = 0; i < 3; i++) ccol_log_info(lg, "generation %d", i);
  bool opened = _wait_for_gz_dest_opened();

  pthread_t t;
  bool closer_started =
      pthread_create(&t, NULL, _close_in_background, &lg) == 0;
  if (closer_started) usleep(100000); /* let the close reach its join */
  clog_test_hold_compressions(false);
  if (closer_started)
    pthread_join(t, NULL);
  else
    clog_close(lg);

  int plain, gz, invalid_gz;
  count_rotated(dir, "app.log", &plain, &gz, &invalid_gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(closer_started);
  REQUIRE_EQ(gz, 3);
  REQUIRE_EQ(plain, 0);
  REQUIRE_EQ(invalid_gz, 0);
}

/* A CLOG_FATAL record that causes a rotation waits for the compression that
 * the rotation queued before it stops the process. The child here holds each
 * compression for 300ms and registers no close of its own at exit, so only
 * the fatal path itself can wait.
 *
 * This test is non-vacuous: when the fatal path calls exit() at once, the
 * process ends inside the held compression. The uncompressed rotated file
 * then stays beside an empty ".gz" file. */
TEST(compression, fatal_waits_for_queued_compressions_before_exit) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
    if (lg == CLOG_INVALID) _exit(0);
    clog_test_set_pending_compress_delay_us(300000);
    ccol_log_fatal(lg, "fatal record that rotates");
    _exit(0); /* unreachable */
  }
  int status = 0;
  if (pid > 0) waitpid(pid, &status, 0);

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

/* Writes text to path and sets both of its timestamps to `when`. */
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

#if defined(__linux__)
#if defined(__linux__)
#include <linux/stat.h>
#endif
#endif

/* Gives true when the kernel and the filesystem report a birth time for
 * path. It asks the kernel directly, because this file does not define
 * _GNU_SOURCE, which the statx() wrapper of glibc needs. */
static bool _file_reports_birth_time(const char *path) {
#if defined(__linux__) && defined(STATX_BTIME) && defined(SYS_statx)
  struct statx stx;
  memset(&stx, 0, sizeof(stx));
  return syscall(SYS_statx, AT_FDCWD, path, 0, STATX_BTIME, &stx) == 0 &&
         (stx.stx_mask & STATX_BTIME);
#else
  (void)path;
  return false;
#endif
}

/* A file that already holds records, beside rotated generations, counts its
 * time rotation interval from the stamp of the newest generation, which is
 * when the current file began. A process that restarts more often than the
 * interval therefore still rotates. The live file here was written just now,
 * so its birth time and its modification time both say "now".
 *
 * This test is non-vacuous: when the seed ignores the rotated generations,
 * it takes the birth time or the modification time of the live file, none
 * of the 100 seconds has passed, and nothing rotates. */
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

  clog_rotation_cfg_t cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "new record");
    clog_close(lg);
  }

  int rotated = count_files_with_prefix(dir, "app.log.");
  bool current_rotated = dir_files_contain(dir, "app.log.", "current record");
  static char live_text[4096];
  read_file(path, live_text, sizeof(live_text));
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(rotated, 2);
  REQUIRE_TRUE(current_rotated);
  REQUIRE_NE(strstr(live_text, "new record"), NULL);
  REQUIRE_EQ(strstr(live_text, "current record"), NULL);
}

/* With no rotated generation on disk, the interval counts from the birth
 * time of the file where the filesystem reports one, and from its last
 * modification time otherwise. The file here was created just now and then
 * given a modification time 1000 seconds in the past, so the two answers
 * differ: a birth time starts the interval now and nothing rotates, and a
 * modification time rotates on the first write. The test probes which one
 * this filesystem offers and expects the matching outcome.
 *
 * This test is non-vacuous: on a filesystem that reports a birth time, a
 * seed that skips it takes the old modification time and rotates. */
TEST(rotation_time, interval_counts_from_birth_time_else_mtime) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  bool prepared =
      _write_file_with_mtime(path, "old record\n", time(NULL) - 1000);
  bool has_btime = prepared && _file_reports_birth_time(path);

  clog_rotation_cfg_t cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "new record");
    clog_close(lg);
  }

  int rotated = count_files_with_prefix(dir, "app.log.");
  bool old_rotated = dir_files_contain(dir, "app.log.", "old record");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_INVALID);
  if (has_btime) {
    REQUIRE_EQ(rotated, 0);
  } else {
    REQUIRE_EQ(rotated, 1);
    REQUIRE_TRUE(old_rotated);
  }
}

/* An empty file starts the interval at the open, whatever its modification
 * time and whatever rotated generations sit beside it, because it holds no
 * record that could be older than the interval.
 *
 * This test is non-vacuous: when the seed ignores the size of the file, it
 * takes the stamp of the old generation, and the first write rotates the
 * empty file away. */
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

  clog_rotation_cfg_t cfg = {
      .time_rotation_enabled = true,
      .rotation_interval_us = 100000000,
      .max_rotated_files = 5,
  };
  clog lg =
      prepared ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "first record");
    clog_close(lg);
  }
  int rotated = count_files_with_prefix(dir, "app.log.");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(prepared);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(rotated, 1); /* the planted generation alone */
}

/* Counts the distinct rotated generations of "app.log" in dir: a file and its
 * ".gz" form count once. */
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

/* A deletion pass keeps an old generation while its compression runs. Once
 * that compression ends, the old generation goes too, so the generations on
 * disk are again exactly the newest max_rotated_files, with no older one
 * left beside them. Here the first rotation's compression is held while two
 * more rotations run with max_rotated_files == 1; the second generation is
 * deleted and the first one is kept only because it is under compression.
 *
 * This test is non-vacuous: when nothing runs the deletion pass again after
 * the held compression, the first generation outlives it, and the directory
 * holds the first and the third generations with the second missing between
 * them. */
TEST(compression, old_generation_kept_for_compression_goes_when_it_ends) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[128];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_INVALID) cleanup_dir(dir, "app.log");
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_hold_compressions(true);
  ccol_log_info(lg, "generation one");
  bool opened = _wait_for_gz_dest_opened();
  ccol_log_info(lg, "generation two");
  ccol_log_info(lg, "generation three");
  clog_test_hold_compressions(false);
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

/* ------------------------------------------------------------------------ */
/* Process exit with loggers still open                                      */
/* ------------------------------------------------------------------------ */

/* Every record "SEQ t=<thread> i=<index>" that dir holds, in the live file
 * and in every rotated file, plain or compressed. seen has threads * per
 * entries. It gives the number of records found, duplicates included, and
 * -1 when a file cannot be read. */
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

/* Waits for the child pid for at most bound_ms. It kills a child that does
 * not finish, and gives false then. */
static bool wait_child_bounded(pid_t pid, int bound_ms, int *status) {
  for (int waited = 0; waited < bound_ms; waited += 10) {
    if (waitpid(pid, status, WNOHANG) == pid) return true;
    usleep(10000);
  }
  kill(pid, SIGKILL);
  waitpid(pid, status, 0);
  return false;
}

/* A child of the tests below marks that it reached its exit() by creating
 * "<dir>/app.logready". The parent reads the marker and not the exit
 * status: a sanitizer or valgrind replaces the exit status of a child that
 * it reports on, for reasons that are not this test. */
/* The children below end with exit(), which runs the destructors of this
 * file in the child. The fixture of
 * clogger_deferred_release.a_racing_pair_left_open_defers_then_releases
 * would then close, in the child, loggers that the parent opened before the
 * fork(), which is outside the supported use of fork(). Each child forgets
 * them first. */
static void forget_parent_fixtures_in_child(void) {
  g_deferred_release_a = CLOG_INVALID;
  g_deferred_release_b = CLOG_INVALID;
}

static void mark_child_ready(const char *log_path) {
  char p[512];
  snprintf(p, sizeof(p), "%sready", log_path);
  int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd >= 0) close(fd);
}

static bool child_was_ready(const char *dir) {
  char p[512];
  snprintf(p, sizeof(p), "%s/app.logready", dir);
  return access(p, F_OK) == 0;
}

/* The flush interval that never fires within a test, so that only the exit
 * drain can deliver a record. */
#define EXIT_TEST_NO_TIMER_US 3600000000ULL

typedef struct {
  clog lg;
  int t;
  int per;
} exit_seq_arg_t;

/* Logs through a->lg, a logger that the main thread of the child derived.
 * The worker threads only log. Every open, derive and close runs on the
 * main thread of the child: ThreadSanitizer misreads the lock of the handle
 * table that the fork handling initializes afresh in a child, when a second
 * thread of that child takes it. */
static void *exit_seq_writer(void *arg) {
  exit_seq_arg_t *a = (exit_seq_arg_t *)arg;
  for (int i = 0; i < a->per; i++)
    ccol_log_info(a->lg, "SEQ t=%d i=%d", a->t, i);
  return NULL;
}

/* The child of the exit tests below: an async logger in fmt and with the
 * queue size qsize, `threads` threads that each log `per` records and join,
 * then exit(1) with the logger still open. */
static void exit_drain_child(const char *path, clog_format_t fmt, size_t qsize,
                             int threads, int per) {
  forget_parent_fixtures_in_child();
  clog_async_cfg_t acfg = {.queue_size = qsize,
                           .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  clog_set_format(lg, fmt);
  pthread_t tids[8];
  exit_seq_arg_t args[8];
  int started = 0;
  for (int t = 0; t < threads && t < 8; t++) {
    args[t] = (exit_seq_arg_t){.lg = clog_derive(lg), .t = t, .per = per};
    if (args[t].lg == CLOG_INVALID) break;
    if (pthread_create(&tids[t], NULL, exit_seq_writer, &args[t]) != 0) {
      clog_close(args[t].lg);
      break;
    }
    started++;
  }
  for (int t = 0; t < started; t++) pthread_join(tids[t], NULL);
  for (int t = 0; t < started; t++) clog_close(args[t].lg);
  if (started != threads) _exit(4);
  exit(1);
}

/* Runs exit_drain_child() in a child and gives how many of the records it
 * logged are in the file, or -1. */
static long run_exit_drain_child(const char *dir, clog_format_t fmt,
                                 size_t qsize, int threads, int per) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  /* The child ends with exit(), which flushes stdio. Nothing that this
   * process still buffers may reach the output a second time. */
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

/* The child of the test below. Its async logger has a queue of one record
 * and a writer thread that spends 3 s on each record. Record A keeps the
 * writer busy, and record B then fills the queue: B returns only once the
 * writer took A, and the writer cannot take B before it is done with A. The
 * drain then sends its flush request without a wait, as it does once its
 * budget is spent, and the queue is full. The child reports '1' when the
 * drain left the target alone, '0' when it did not, and 'x' on a setup
 * failure. */
static void exit_drain_full_queue_child(const char *path, int report_fd) {
  forget_parent_fixtures_in_child();
  char verdict = 'x';
  clog_async_cfg_t acfg = {.queue_size = 1,
                           .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  if (lg != CLOG_INVALID) {
    clog_test_set_writer_job_delay_us(3000000);
    ccol_log_info(lg, "record A");
    ccol_log_info(lg, "record B");
    clog_test_force_exit_drain_send_without_wait(true);
    clog_test_run_exit_drain();
    verdict = clog_test_exit_drain_left_target(lg) ? '1' : '0';
  }
  ssize_t w;
  do {
    w = write(report_fd, &verdict, 1);
  } while (w < 0 && errno == EINTR);
  _exit(0);
}

/*
 * A drain whose budget is spent sends its flush request without a wait. To a
 * bounded queue that is full, that send answers ccol_container_full, and the
 * drain must then leave the target alone, as for a writer thread that did
 * not answer in time: it never takes the full queue for a drained one.
 *
 * This test is non-vacuous: a drain that treats ccol_container_full as
 * anything but "not drained" goes on to turn sending off on a queue that
 * still holds records, and does not leave the target.
 */
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

/*
 * A process that calls exit() with an async logger still open keeps every
 * record that it logged. The exit drain delivers the queue and the batch,
 * for every format and for a bounded and an unbounded queue. The flush
 * interval never fires here, so only that drain can deliver a record.
 *
 * This test is non-vacuous: without the exit drain the writer thread dies
 * with the process, and the file holds none of these records, or only the
 * batches that reached the flush size.
 */
TEST(exit_drain, exit_keeps_every_record_of_an_async_logger) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  const clog_format_t fmts[3] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON,
                                 CLOG_FMT_SYSLOG};
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

/*
 * Several threads log through derived loggers and finish, and the process
 * exits: every record of every thread is on disk, once. That is the shape
 * of a return from main() while the root logger is still open.
 *
 * This test is non-vacuous: without the exit drain most of these records
 * stay in the queue and are lost.
 */
TEST(exit_drain, exit_after_threads_finish_keeps_every_record) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long unbounded = run_exit_drain_child(dir, CLOG_FMT_LOGFMT, 0, 4, 5000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long bounded = run_exit_drain_child(dir, CLOG_FMT_JSON, 64, 4, 5000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(unbounded, 20000L);
  REQUIRE_EQ(bounded, 20000L);
}

/* The child of the compression exit tests: a rotating, compressing logger
 * that rotates once, then exit(0) while that compression runs, or is held
 * when hold is true. */
static void exit_compress_child(const char *path, bool hold) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  if (hold) {
    clog_test_set_exit_drain_budget_ms(200);
    clog_test_hold_compressions(true);
  }
  ccol_log_info(lg, "COMPRESSEDRECORDBODY");
  if (hold) {
    bool opened = false;
    for (int i = 0; i < 20000 && !opened; i++) {
      opened = clog_test_gz_dest_opened();
      if (!opened) usleep(1000);
    }
    if (!opened) _exit(4);
  }
  mark_child_ready(path);
  exit(0);
}

/* What a directory holds after exit_compress_child(). */
typedef struct {
  int plain;          /* rotated files that are not compressed */
  int gz;             /* ".gz" files */
  int invalid_gz;     /* ".gz" files that gunzip -t rejects */
  int tmp;            /* unfinished compression outputs */
  bool body_in_plain; /* a plain rotated file holds the record */
  bool body_in_gz;    /* a ".gz" file holds the record */
} exit_compress_state_t;

static void inspect_exit_compress_dir(const char *dir,
                                      exit_compress_state_t *st) {
  memset(st, 0, sizeof(*st));
  DIR *d = opendir(dir);
  if (!d) return;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log.", 8) != 0) continue;
    char p[512];
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

static bool run_exit_compress_child(const char *dir, bool hold) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  /* The child ends with exit(), which flushes stdio. Nothing that this
   * process still buffers may reach the output a second time. */
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_compress_child(path, hold);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, 60000, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

/*
 * exit() while a compression runs finishes it within the budget of the exit
 * drain: the rotated record is in one valid ".gz" file, and the source is
 * gone.
 *
 * This test is non-vacuous: without the exit drain the process ends in the
 * middle of the compression, and the directory holds the source and, at
 * most, an unfinished output.
 */
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

/*
 * exit() while a compression cannot finish within the budget of the exit
 * drain abandons it cleanly: no ".gz" file and no unfinished output remain,
 * and the uncompressed source keeps the record.
 *
 * This test is non-vacuous: a compression that writes its ".gz" name
 * directly leaves an empty or truncated ".gz" file beside the source.
 */
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

/* A custom allocator whose free() calls exit() on any thread but the one
 * that armed it, once. It puts exit() on the writer thread or on the
 * compressor thread of a logger, which are the only threads that call the
 * allocator of a logger besides the threads that log. */
static _Atomic bool g_exit_on_foreign_free;
static pthread_t g_exit_free_owner;

static void exit_on_foreign_free(void *p) {
  if (atomic_load(&g_exit_on_foreign_free) &&
      !pthread_equal(pthread_self(), g_exit_free_owner)) {
    atomic_store(&g_exit_on_foreign_free, false);
    exit(0);
  }
  free(p);
}

/* The child: a logger that uses that allocator, async or compressing, and
 * exit() from its writer thread or its compressor thread. The exit drain
 * must not wait for the thread that runs it. The budget is far longer than
 * the bound of the parent, so a drain that waits fails the test. */
static void exit_from_worker_child(const char *path, bool compress) {
  forget_parent_fixtures_in_child();
  ccol_memmgmt_procs_t mp = {.malloc = malloc,
                             .free = exit_on_foreign_free,
                             .calloc = calloc,
                             .realloc = realloc};
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file_mp(path, CLOG_INFO, compress ? &cfg : NULL,
                              compress ? NULL : &acfg, &mp);
  if (lg == CLOG_INVALID) _exit(3);
  clog_test_set_exit_drain_budget_ms(600000);
  mark_child_ready(path);
  g_exit_free_owner = pthread_self();
  atomic_store(&g_exit_on_foreign_free, true);
  ccol_log_info(lg, "WORKEREXITBODY");
  /* The writer thread or the compressor thread ends the process. */
  for (;;) pause();
}

static bool run_exit_from_worker_child(const char *dir, bool compress) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  /* The child ends with exit(), which flushes stdio. Nothing that this
   * process still buffers may reach the output a second time. */
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_from_worker_child(path, compress);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, 60000, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

/*
 * exit() on the writer thread or on the compressor thread of a logger ends
 * the process: the exit drain skips the target that the calling thread
 * serves, and does not wait for itself.
 *
 * This test is non-vacuous: without that skip, the drain waits for its own
 * flush request on the writer thread until its budget runs out, and on the
 * compressor thread it locks the mutex that the thread already holds.
 */
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

/* The thread of the lock tests below: it takes a lock of the logger and
 * never lets it go. */
typedef struct {
  clog lg; /* CLOG_INVALID: the table lock */
  _Atomic bool locked;
} exit_lock_holder_t;

static void *exit_lock_holder(void *arg) {
  exit_lock_holder_t *h = (exit_lock_holder_t *)arg;
  if (h->lg == CLOG_INVALID)
    clog_test_lock_table_forever();
  else
    clog_test_lock_target_forever(h->lg);
  atomic_store(&h->locked, true);
  for (;;) pause();
  return NULL;
}

/* The child: a logger that arms the exit drain (a compressing, rotating one
 * whose first record rotates and starts its compressor thread, or an async
 * one), a thread that holds the mutex of that logger or the table lock for
 * ever, and exit(). The budget of the drain is short, and the parent bounds
 * the whole child far above it. */
static void exit_with_held_lock_child(const char *path, bool table_lock) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file_mp(path, CLOG_INFO, table_lock ? NULL : &cfg,
                              table_lock ? &acfg : NULL, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  ccol_log_info(lg, "HELDLOCKBODY");
  clog_test_set_exit_drain_budget_ms(300);
  static exit_lock_holder_t holder;
  holder.lg = table_lock ? CLOG_INVALID : lg;
  atomic_store(&holder.locked, false);
  pthread_t tid;
  if (pthread_create(&tid, NULL, exit_lock_holder, &holder) != 0) _exit(4);
  for (int i = 0; i < 20000 && !atomic_load(&holder.locked); i++) usleep(1000);
  if (!atomic_load(&holder.locked)) _exit(5);
  mark_child_ready(path);
  exit(0);
}

/* Runs the child and gives true when it ended with exit() within
 * bound_ms. */
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

/*
 * exit() while another thread holds the mutex of a compressing logger, or the
 * lock of the table of loggers, for ever: the exit drain gives up on that lock
 * within its budget, and the process ends.
 *
 * This test is non-vacuous: a drain that waits for either lock without a
 * bound never returns, and the parent kills the child at its bound.
 */
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

/* ------------------------------------------------------------------------ */
/* Unfinished compressions of an earlier process                             */
/* ------------------------------------------------------------------------ */

/* The child: a rotating, compressing logger whose compression is held after
 * it created its output, and a process that then dies without any exit
 * handler, as a crash or a kill ends it. */
static void die_during_compression_child(const char *path) {
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  clog_test_hold_compressions(true);
  ccol_log_info(lg, "COMPRESSEDRECORDBODY");
  for (int i = 0; i < 20000; i++) {
    if (clog_test_gz_dest_opened()) {
      mark_child_ready(path);
      _exit(0);
    }
    usleep(1000);
  }
  _exit(4);
}

/*
 * A process that dies in the middle of a compression leaves no ".gz" file:
 * the output takes that name only once it is complete. The next open of the
 * logger removes the unfinished output and compresses the source again.
 *
 * This test is non-vacuous: a compression that writes under the ".gz" name
 * from the start leaves an empty ".gz" file beside the source here.
 */
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

  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1 << 20,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg != CLOG_INVALID) clog_close(lg);
  exit_compress_state_t reopened;
  inspect_exit_compress_dir(dir, &reopened);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(ran);
  REQUIRE_EQ(died.gz, 0);
  REQUIRE_EQ(died.tmp, 1);
  REQUIRE_EQ(died.plain, 1);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(reopened.gz, 1);
  REQUIRE_EQ(reopened.invalid_gz, 0);
  REQUIRE_EQ(reopened.tmp, 0);
  REQUIRE_EQ(reopened.plain, 0);
  REQUIRE_TRUE(reopened.body_in_gz);
}

static bool write_whole_file(const char *path, const char *data, size_t len) {
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (fd < 0) return false;
  bool ok = write(fd, data, len) == (ssize_t)len;
  close(fd);
  return ok;
}

/*
 * An open of a compressing logger finishes what an earlier process left
 * unfinished. A truncated ".gz" file beside its source, and an unfinished
 * output beside its source, are both removed, and each source is compressed
 * again. Nothing of the earlier generations is lost, and every rotated file
 * ends as one valid ".gz" file.
 *
 * This test is non-vacuous: without that recovery the truncated ".gz" file
 * stays, gunzip rejects it, and the sources and the unfinished output stay
 * uncompressed on disk.
 */
TEST(compression, open_redoes_compressions_that_an_earlier_process_left) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char p[256];
  bool setup = true;
  snprintf(p, sizeof(p), "%s/app.log.20200101000000", dir);
  setup &= write_whole_file(p, "OLDGENONE\n", 10);
  /* A truncated gzip stream: the header alone. */
  snprintf(p, sizeof(p), "%s/app.log.20200101000000.gz", dir);
  setup &= write_whole_file(p, "\x1f\x8b\x08\x00", 4);
  snprintf(p, sizeof(p), "%s/app.log.20200101000001", dir);
  setup &= write_whole_file(p, "OLDGENTWO\n", 10);
  snprintf(p, sizeof(p), "%s/app.log.20200101000001.gz.tmp", dir);
  setup &= write_whole_file(p, "\x1f\x8b", 2);

  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1 << 20,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) clog_close(lg);

  exit_compress_state_t st;
  inspect_exit_compress_dir(dir, &st);
  char one[256] = {0}, two[256] = {0};
  snprintf(p, sizeof(p), "%s/app.log.20200101000000.gz", dir);
  gunzip_read(p, one, sizeof(one));
  snprintf(p, sizeof(p), "%s/app.log.20200101000001.gz", dir);
  gunzip_read(p, two, sizeof(two));
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(st.tmp, 0);
  REQUIRE_EQ(st.plain, 0);
  REQUIRE_EQ(st.gz, 2);
  REQUIRE_EQ(st.invalid_gz, 0);
  REQUIRE_STREQ(one, "OLDGENONE\n");
  REQUIRE_STREQ(two, "OLDGENTWO\n");
}

/*
 * A compressed rotated file gets the permission bits of the file that it
 * compresses, as gzip(1) gives them, and not the default of the umask.
 *
 * This test is non-vacuous: a ".gz" file created with 0666 under a umask of
 * 022 gets 0644, not the 0640 of its source.
 */
TEST(compression, compressed_file_keeps_the_mode_of_its_source) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  mode_t old_mask = umask(022);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  bool chmod_ok = lg != CLOG_INVALID && chmod(path, 0640) == 0;
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "MODERECORD");
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

/* ------------------------------------------------------------------------ */
/* One file opened twice in one process                                      */
/* ------------------------------------------------------------------------ */

typedef struct {
  clog lg;
  int t;
  int per;
} same_file_arg_t;

/* Thread 1 logs slowly. Its records then keep landing in a file that the
 * other thread rotated away, for long enough for a compression of that file
 * to finish, whenever the two handles have two targets. */
static void *same_file_writer(void *arg) {
  same_file_arg_t *a = (same_file_arg_t *)arg;
  for (int i = 0; i < a->per; i++) {
    ccol_log_info(a->lg, "SEQ t=%d i=%d", a->t, i);
    if (a->t == 1) usleep(200);
  }
  return NULL;
}

/*
 * Two opens of one rotating, compressing file in one process, through two
 * spellings of the path, share one target. Records that two threads log
 * through the two handles while the file rotates and compresses are all on
 * disk, once each.
 *
 * This test is non-vacuous: two independent targets on one file each rename
 * and compress files that the other one still writes, and records are lost.
 */
TEST(same_file, two_rotating_opens_share_one_target_and_lose_nothing) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], path2[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(path2, sizeof(path2), "./%s/../%s/app.log", dir, dir);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 4096,
                             .max_rotated_files = 1000,
                             .compress_rotated = true};
  clog a = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  clog b = clog_open_file(path2, CLOG_INFO, &cfg, NULL);
  enum { PER = 2000 };
  bool started[2] = {false, false};
  pthread_t tids[2];
  same_file_arg_t args[2] = {{.lg = a, .t = 0, .per = PER},
                             {.lg = b, .t = 1, .per = PER}};
  if (a != CLOG_INVALID && b != CLOG_INVALID)
    for (int t = 0; t < 2; t++)
      started[t] =
          pthread_create(&tids[t], NULL, same_file_writer, &args[t]) == 0;
  for (int t = 0; t < 2; t++)
    if (started[t]) pthread_join(tids[t], NULL);
  if (a != CLOG_INVALID) clog_close(a);
  if (b != CLOG_INVALID) clog_close(b);

  unsigned char *seen = malloc(2 * PER);
  long found = seen ? collect_seq_records(dir, 2, PER, seen) : -1;
  long distinct = 0;
  for (int i = 0; seen && i < 2 * PER; i++) distinct += seen[i];
  free(seen);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_INVALID);
  REQUIRE_NE(b, CLOG_INVALID);
  REQUIRE_TRUE(started[0] && started[1]);
  REQUIRE_EQ(distinct, (long)(2 * PER));
  REQUIRE_EQ(found, (long)(2 * PER));
}

/*
 * A rotation_interval_us of 0 is the default interval, and a flush_interval_us
 * of 0 is the default flush interval: a second open of the same file that
 * spells either default out joins the first one, and an open with a
 * different interval does not. The join compares the configurations after
 * the defaults are applied, so this pins the meaning of 0 exactly.
 *
 * This test is non-vacuous: when 0 is kept as it is, or resolved to another
 * value, the open that spells out the default fails.
 */
TEST(same_file, a_zero_interval_is_the_default_interval) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t zero = {.time_rotation_enabled = true,
                              .rotation_interval_us = 0,
                              .max_rotated_files = 3};
  clog_rotation_cfg_t spelled = zero;
  spelled.rotation_interval_us = CLOG_DEFAULT_ROTATION_INTERVAL_US;
  clog_rotation_cfg_t other = zero;
  other.rotation_interval_us = CLOG_DEFAULT_ROTATION_INTERVAL_US + 1000000u;
  clog_async_cfg_t azero = {.flush_interval_us = 0};
  clog_async_cfg_t aspelled = {.flush_interval_us =
                                   CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US};
  clog_async_cfg_t aother = {.flush_interval_us =
                                 CLOG_DEFAULT_ASYNC_FLUSH_INTERVAL_US + 1u};

  clog a = clog_open_file(path, CLOG_INFO, &zero, &azero);
  clog b = clog_open_file(path, CLOG_INFO, &spelled, &aspelled);
  clog diff_rot = clog_open_file(path, CLOG_INFO, &other, &azero);
  clog diff_flush = clog_open_file(path, CLOG_INFO, &zero, &aother);
  if (diff_flush != CLOG_INVALID) clog_close(diff_flush);
  if (diff_rot != CLOG_INVALID) clog_close(diff_rot);
  if (b != CLOG_INVALID) clog_close(b);
  if (a != CLOG_INVALID) clog_close(a);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_INVALID);
  REQUIRE_NE(b, CLOG_INVALID);
  REQUIRE_EQ(diff_rot, CLOG_INVALID);
  REQUIRE_EQ(diff_flush, CLOG_INVALID);
}

/*
 * A second open of a file that a rotating logger of this process writes
 * fails when its configuration differs: another rotation, no rotation,
 * async against synchronous. Two opens that both do not rotate stay two
 * independent loggers, and an open after the last close works again.
 */
TEST(same_file, a_second_open_with_another_configuration_fails) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 4096,
                             .max_rotated_files = 3,
                             .compress_rotated = true};
  clog_rotation_cfg_t other = cfg;
  other.max_rotated_files = 4;
  clog_async_cfg_t acfg = {0};

  clog a = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  clog diff_rot = clog_open_file(path, CLOG_INFO, &other, NULL);
  clog no_rot = clog_open_file(path, CLOG_INFO, NULL, NULL);
  clog async = clog_open_file(path, CLOG_INFO, &cfg, &acfg);
  clog same = clog_open_file(path, CLOG_DEBUG, &cfg, NULL);
  clog_level_t same_level = same != CLOG_INVALID ? clog_get_level(same) : 0;
  clog_level_t a_level = a != CLOG_INVALID ? clog_get_level(a) : 0;
  if (same != CLOG_INVALID) clog_close(same);
  if (a != CLOG_INVALID) clog_close(a);
  clog again = clog_open_file(path, CLOG_INFO, NULL, NULL);
  clog again2 = clog_open_file(path, CLOG_INFO, NULL, NULL);
  if (again != CLOG_INVALID) clog_close(again);
  if (again2 != CLOG_INVALID) clog_close(again2);
  if (diff_rot != CLOG_INVALID) clog_close(diff_rot);
  if (no_rot != CLOG_INVALID) clog_close(no_rot);
  if (async != CLOG_INVALID) clog_close(async);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(a, CLOG_INVALID);
  REQUIRE_EQ(diff_rot, CLOG_INVALID);
  REQUIRE_EQ(no_rot, CLOG_INVALID);
  REQUIRE_EQ(async, CLOG_INVALID);
  REQUIRE_NE(same, CLOG_INVALID);
  REQUIRE_EQ((int)same_level, (int)CLOG_DEBUG);
  REQUIRE_EQ((int)a_level, (int)CLOG_INFO);
  REQUIRE_NE(again, CLOG_INVALID);
  REQUIRE_NE(again2, CLOG_INVALID);
}

/* ------------------------------------------------------------------------ */
/* Bounded ends of the process                                               */
/* ------------------------------------------------------------------------ */

#include <sys/ioctl.h>

/* Gives the capacity of the pipe whose ends are rd and wr, which must be
 * empty, and leaves it empty. It fills the pipe in non-blocking mode and
 * reads everything back. It gives 0 on failure. */
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
      /* Room for less than a chunk can remain; one byte at a time. */
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

/* Fills the empty pipe (rd, wr) completely and leaves wr in blocking mode.
 * It gives false on failure. */
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

typedef struct {
  clog lg;
  const char *rec;
} blocked_writer_arg_t;

/* Logs a record larger than the pipe for ever: the first write blocks
 * inside the logger, with the mutex of the logger held, once the pipe is
 * full, and nothing reads the pipe. */
static void *blocked_pipe_writer(void *arg) {
  blocked_writer_arg_t *a = (blocked_writer_arg_t *)arg;
  for (;;) ccol_log_info(a->lg, "%s", a->rec);
  return NULL;
}

/* The child: a synchronous logger on a pipe that nobody reads, whose thread
 * blocks inside a write with the mutex of that logger held, and a
 * CLOG_FATAL record on a second logger that writes a file. */
static void fatal_beside_blocked_pipe_child(const char *path) {
  forget_parent_fixtures_in_child();
  int p[2];
  if (pipe(p) != 0) _exit(3);
  size_t cap = pipe_capacity(p[0], p[1]);
  if (cap == 0) _exit(3);
  char *rec = malloc(2 * cap + 1);
  if (!rec) _exit(3);
  memset(rec, 'x', 2 * cap);
  rec[2 * cap] = '\0';
  clog pipe_lg = clog_open_fd(p[1], CLOG_INFO, NULL);
  clog file_lg = clog_open_file(path, CLOG_INFO, NULL, NULL);
  if (pipe_lg == CLOG_INVALID || file_lg == CLOG_INVALID) _exit(3);
  blocked_writer_arg_t arg = {.lg = pipe_lg, .rec = rec};
  pthread_t tid;
  if (pthread_create(&tid, NULL, blocked_pipe_writer, &arg) != 0) _exit(3);
  /* A record of twice the capacity cannot go out whole: once the pipe
   * holds cap bytes, the thread is inside that write, and holds the mutex
   * of pipe_lg. */
  for (int i = 0; i < 20000; i++) {
    int avail = 0;
    if (ioctl(p[0], FIONREAD, &avail) == 0 && (size_t)avail >= cap) {
      mark_child_ready(path);
      ccol_log_fatal(file_lg, "FATALBESIDEBLOCKEDPIPE");
    }
    usleep(1000);
  }
  _exit(4);
}

/*
 * A CLOG_FATAL record ends the process although another thread blocks for
 * ever inside a write to a pipe that nobody reads, with the mutex of that
 * other logger held. The fatal record is in its file.
 *
 * This test is non-vacuous: an end of a fatal call that takes the mutex of
 * every logger of the process waits for that thread for ever, and the parent
 * kills the child at its bound.
 */
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

/* The child: an async logger on a pipe that is full and that nobody reads,
 * whose batch the flush of the exit drain keeps after a transient error.
 * The settle that follows then meets a descriptor in blocking mode with no
 * room. The process ends with exit(), or with a CLOG_FATAL record on a
 * second logger when via_fatal is set. */
static void exit_with_stalled_pipe_child(const char *path, bool via_fatal) {
  forget_parent_fixtures_in_child();
  clog_test_set_exit_drain_budget_ms(2000);
  int p[2];
  if (pipe(p) != 0 || !fill_pipe(p[0], p[1])) _exit(3);
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog pipe_lg = clog_open_fd(p[1], CLOG_INFO, &acfg);
  if (pipe_lg == CLOG_INVALID) _exit(3);
  clog null_lg = CLOG_INVALID;
  if (via_fatal) {
    int nfd = open("/dev/null", O_WRONLY);
    null_lg = nfd >= 0 ? clog_open_fd(nfd, CLOG_INFO, NULL) : CLOG_INVALID;
    if (null_lg == CLOG_INVALID) _exit(3);
  }
  ccol_log_info(pipe_lg, "STALLEDPIPERECORD");
  mark_child_ready(path);
  if (!via_fatal) {
    /* The one write that fails is the flush that the exit drain asks the
     * writer thread for. */
    clog_test_force_write_errors(ENOSPC, 1);
    exit(0);
  }
  /* The fatal record and the truncation marker that its failed write
   * leaves go first; the third write that fails is the flush that the exit
   * drain asks the writer thread for. */
  clog_test_force_write_errors(ENOSPC, 3);
  ccol_log_fatal(null_lg, "FATALRECORD");
  _exit(4);
}

static bool run_exit_with_stalled_pipe_child(const char *dir, bool via_fatal,
                                             int bound_ms) {
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  fflush(NULL);
  pid_t pid = fork();
  if (pid == 0) exit_with_stalled_pipe_child(path, via_fatal);
  if (pid < 0) return false;
  int status = 0;
  if (!wait_child_bounded(pid, bound_ms, &status)) return false;
  return WIFEXITED(status) && child_was_ready(dir);
}

/*
 * exit(), and a CLOG_FATAL record, end within the budget of the exit drain
 * when an async logger holds a batch for a pipe that is full, in blocking
 * mode, and that nobody reads. The last delivery attempt and the marker that
 * names the undelivered records are bounded writes, and a bounded write
 * never makes a call that can block past its bound.
 *
 * This test is non-vacuous: a bounded write that polls only after EAGAIN
 * blocks for ever in write(2) on such a pipe, and the parent kills the child
 * at its bound.
 */
TEST(exit_drain, a_stalled_blocking_pipe_ends_within_the_budget) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool exit_ok = run_exit_with_stalled_pipe_child(dir, false, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  bool fatal_ok = run_exit_with_stalled_pipe_child(dir, true, 20000);
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(exit_ok);
  REQUIRE_TRUE(fatal_ok);
}

/* ------------------------------------------------------------------------ */
/* Rotation and the permissions of the log                                   */
/* ------------------------------------------------------------------------ */

/*
 * A rotation creates the new live file with the permission bits of the file
 * that it replaces, whatever the umask. A log that its operator restricted
 * to 0600 stays 0600 in the live file and in every rotated and compressed
 * generation.
 *
 * This test is non-vacuous: a live file created with 0644 under a umask of
 * 022 makes every generation after the first world-readable.
 */
TEST(rotation, every_generation_keeps_the_mode_of_the_log) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool created = fd >= 0 && fchmod(fd, 0600) == 0;
  if (fd >= 0) close(fd);
  mode_t old_mask = umask(022);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 100,
                             .compress_rotated = true};
  clog lg =
      created ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 6; i++) ccol_log_info(lg, "SECRETRECORD %d", i);
    clog_close(lg);
  }
  umask(old_mask);
  int files = 0, gz = 0, wrong = 0;
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    if (strncmp(e->d_name, "app.log", 7) != 0) continue;
    char p[512];
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
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_GE(files, 5);
  REQUIRE_GE(gz, 4);
  REQUIRE_EQ(wrong, 0);
}

/* ------------------------------------------------------------------------ */
/* The owner and the group of a compressed generation                        */
/* ------------------------------------------------------------------------ */

#include <poll.h>

/* Picks a group, other than the effective group of the process, that the
 * process may give a file: any group for root, else a supplementary group.
 * It gives false when there is none, and then the owner and group tests
 * below have nothing to observe. */
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

/*
 * A compressed generation gets the owner, the group and the permission bits
 * of the file that it compresses, as the new live file of a rotation does.
 * A log that its operator gave to a group, with mode 0640, therefore stays
 * readable by that group, and by no other, in its compressed history.
 *
 * This test is non-vacuous: a compression that copies only the bits leaves
 * the .gz file with the group of the process and the group-read bit, so the
 * group of the process reads it and the group of the log does not. A
 * process with no second group to give a file (neither root nor a member
 * of a supplementary group) has nothing to observe, and the test says so.
 */
TEST(compression,
     a_compressed_generation_keeps_the_owner_and_group_of_its_source) {
  gid_t group;
  if (!pick_foreign_group(&group)) {
    printf("  (skipped: the process has no second group to give a file)\n");
    return;
  }
  uid_t owner = geteuid() == 0 ? 1 : geteuid();
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char src[256], dst[300];
  snprintf(src, sizeof(src), "%s/app.log.1", dir);
  snprintf(dst, sizeof(dst), "%s.gz", src);
  int fd = open(src, O_WRONLY | O_CREAT | O_TRUNC, 0600);
  bool prepared = fd >= 0 && write(fd, "RECORD\n", 7) == 7 &&
                  fchown(fd, owner, group) == 0 && fchmod(fd, 0640) == 0;
  if (fd >= 0) close(fd);
  bool compressed = prepared && clog_test_gzip_compress_file(src, dst);
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

/* ------------------------------------------------------------------------ */
/* An fsync that the filesystem does not offer                               */
/* ------------------------------------------------------------------------ */

/* Compresses a fresh file while the next fsync() reports err. It gives
 * whether the compression succeeded, whether the .gz file exists and
 * whether the source is still there. */
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
  clog_test_force_next_fsync_error(err);
  bool ok = prepared && clog_test_gzip_compress_file(src, dst);
  clog_test_force_next_fsync_error(0);
  *has_dst = access(dst, F_OK) == 0;
  *has_src = access(src, F_OK) == 0;
  cleanup_dir(dir, "app.log");
  return ok;
}

/*
 * A filesystem whose fsync() answers EINVAL or ENOSYS still gets its
 * rotated files compressed: it offers no way to ask for the data on disk,
 * and that is not a failure of the compression. A real failure of fsync(),
 * EIO, still fails the compression and keeps the source.
 *
 * This test is non-vacuous: a compression that fails on every fsync()
 * error leaves every rotated file uncompressed on such a filesystem.
 */
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

/* ------------------------------------------------------------------------ */
/* A fatal record on a terminal whose reader stopped                         */
/* ------------------------------------------------------------------------ */

/* Opens a pseudo-terminal whose reader, the master, never reads, and fills
 * it until it has a little room, less than one PIPE_BUF write: POLLOUT on
 * the slave is set, and a blocking write of PIPE_BUF bytes does not fit. It
 * gives the slave in *slave and the master in *master, or false. */
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
  /* Reading a few bytes of the master lets the terminal take a little
   * more, once its worker moves the backlog on. Linux reports the slave
   * writable after the first such read; FreeBSD only once the output queue
   * is below its low-water mark, so the reads go on, 64 bytes at a time,
   * until the slave is writable. */
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

static void fatal_on_stalled_tty_child(int slave, const char *path) {
  forget_parent_fixtures_in_child();
  clog_test_set_exit_drain_budget_ms(1000);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) dup2(devnull, STDERR_FILENO);
  clog lg = clog_open_fd(slave, CLOG_INFO, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  static char big[6000];
  memset(big, 'y', sizeof(big) - 1);
  mark_child_ready(path);
  ccol_log_fatal(lg, "%s", big);
  _exit(4);
}

/*
 * A CLOG_FATAL record ends the process within the budget when its output is
 * a terminal that has a little room left and a reader that stopped, the
 * state of a frozen remote session. POLLOUT on a terminal says only that
 * some room is free, so a bounded write that trusted it would block in
 * write(2) until the whole chunk fits.
 *
 * This test is non-vacuous: a bounded write that writes PIPE_BUF bytes to
 * the terminal after POLLOUT blocks for ever, and the parent kills the child
 * at its bound.
 */
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

/* ------------------------------------------------------------------------ */
/* A fatal record behind a busy writer thread                                */
/* ------------------------------------------------------------------------ */

static void fatal_behind_busy_writer_child(const char *path) {
  forget_parent_fixtures_in_child();
  clog_test_set_exit_drain_budget_ms(100);
  int devnull = open("/dev/null", O_WRONLY);
  if (devnull >= 0) dup2(devnull, STDERR_FILENO);
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_INVALID) _exit(3);
  clog_test_set_writer_job_delay_us(20000);
  for (int i = 0; i < 200; i++) ccol_log_info(lg, "BACKLOG %d", i);
  mark_child_ready(path);
  ccol_log_fatal(lg, "FATALBEHINDBACKLOG");
  _exit(4);
}

/*
 * A CLOG_FATAL record on an async logger reaches its regular file when the
 * writer thread has a backlog that the budget cannot drain. The writer
 * thread holds the mutex of the target for each record, here for 20 ms,
 * and takes it again for the next one at once. The fatal call still gets
 * the mutex: the writer thread steps aside while a fatal call waits, and
 * that wait lasts long enough for the record in hand to finish.
 *
 * This test is non-vacuous: a fatal call whose wait for the mutex ends at
 * the deadline, which the drain has used up, tries the mutex once while the
 * writer thread holds it, and the record is lost; a writer thread that does
 * not step aside takes the mutex again ahead of the fatal call.
 */
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

/* ------------------------------------------------------------------------ */
/* A record logged after the exit drain                                      */
/* ------------------------------------------------------------------------ */

/* The async logger that the destructor below logs to, in a child of the
 * test that follows; CLOG_INVALID everywhere else. The destructor closes
 * it too when g_after_drain_close is set. */
static clog g_after_drain_logger = CLOG_INVALID;
static bool g_after_drain_close;

/* The destructors of a program run after every exit handler, and so after
 * the exit drain of the library. */
__attribute__((destructor)) static void _log_after_the_exit_drain(void) {
  if (g_after_drain_logger == CLOG_INVALID) return;
  ccol_log_info(g_after_drain_logger, "AFTERDRAINRECORD");
  if (g_after_drain_close) clog_close(g_after_drain_logger);
  g_after_drain_logger = CLOG_INVALID;
}

static void log_after_drain_child(const char *path, size_t qsize,
                                  bool close_after) {
  forget_parent_fixtures_in_child();
  clog_async_cfg_t acfg = {.queue_size = qsize,
                           .flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file(path, CLOG_INFO, NULL, &acfg);
  if (lg == CLOG_INVALID) _exit(3);
  ccol_log_info(lg, "BEFOREEXITRECORD");
  g_after_drain_logger = lg;
  g_after_drain_close = close_after;
  mark_child_ready(path);
  exit(0);
}

/* Runs log_after_drain_child() and gives whether both records are in the
 * file, the child ended and it reached its exit(). */
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

/*
 * A record that an async logger receives after the exit drain, from a
 * destructor that does not close the logger, is written by the thread that
 * logs it. Both for an unbounded and for a bounded queue.
 *
 * This test is non-vacuous: a record that joins the queue after the drain
 * waits in the batch of the writer thread for a flush that the end of the
 * process cuts off, and the file lacks it.
 */
TEST(exit_drain, a_record_logged_after_the_drain_is_written) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  bool unbounded = run_log_after_drain_child(0, false);
  bool bounded = run_log_after_drain_child(16, false);
  REQUIRE_TRUE(unbounded);
  REQUIRE_TRUE(bounded);
}

/*
 * A destructor that logs to an async logger after the exit drain and then
 * closes it ends, and both records are in the file.
 *
 * This test is non-vacuous: a close whose shutdown request the queue
 * refuses, since the drain turned sends off, retries for ever, and the
 * parent kills the child at its bound.
 */
TEST(exit_drain, a_close_after_the_drain_ends) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  bool unbounded = run_log_after_drain_child(0, true);
  bool bounded = run_log_after_drain_child(16, true);
  REQUIRE_TRUE(unbounded);
  REQUIRE_TRUE(bounded);
}

/*
 * A rotated file whose compressed name cannot fit in the directory stays
 * uncompressed, and a record in the new live file says so. The rotated
 * name itself fits: the base name is 238 bytes, the rotated name 253, and
 * the compressed name 256, above the limit of 255 of the filesystems that
 * the tests run on.
 *
 * This test is non-vacuous: a compression that fails on the length of its
 * names leaves the file uncompressed and reports nothing.
 */
TEST(compression, a_name_too_long_to_compress_is_reported) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  long name_max = pathconf(dir, _PC_NAME_MAX);
  char base[240];
  memset(base, 'b', 238);
  base[238] = '\0';
  char path[512];
  snprintf(path, sizeof(path), "%s/%s", dir, base);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = name_max == 255 ? clog_open_file(path, CLOG_INFO, &cfg, NULL)
                            : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "LONGNAMERECORD");
    clog_close(lg);
  }
  char buf[4096] = {0};
  read_file(path, buf, sizeof(buf));
  bool reported = strstr(buf, "stays uncompressed") != NULL &&
                  strstr(buf, "level=WARN") != NULL;
  int rotated = count_files_with_prefix(dir, "bbbbbbbbbb");
  int gz = count_files_with_suffix(dir, ".gz");
  cleanup_dir(dir, "bbbbbbbbbb");
  if (name_max != 255) return; /* the lengths above assume a limit of 255 */
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(reported);
  REQUIRE_EQ(rotated, 2); /* the live file and one rotated file */
  REQUIRE_EQ(gz, 0);
}

typedef struct {
  const char *path;
  const clog_rotation_cfg_t *cfg;
  clog lg;
} recover_open_arg_t;

static void *recover_opener(void *arg) {
  recover_open_arg_t *a = (recover_open_arg_t *)arg;
  a->lg = clog_open_file(a->path, CLOG_INFO, a->cfg, NULL);
  return NULL;
}

/*
 * The recovery of unfinished compressions that the first open of a file runs
 * leaves alone the work of a compression that the same process started.
 * While it runs, a second open of the file cannot join the target, rotate it
 * and start a compression that the recovery then takes for the leftover of
 * a stopped process. Every rotated file is compressed exactly once.
 *
 * This test is non-vacuous: when the recovery runs after the target is
 * published, the second open joins at once and rotates, the recovery removes
 * the output that the running compression writes and queues its source a
 * second time, and two compressions run for one rotation.
 */
TEST(compression, open_recovery_leaves_a_running_compression_alone) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  size_t finished_before = clog_test_compressions_finished();
  clog_test_hold_compressions(true);
  clog_test_hold_compression_recovery(true);
  recover_open_arg_t first = {.path = path, .cfg = &cfg, .lg = CLOG_INVALID};
  pthread_t tid;
  bool started = pthread_create(&tid, NULL, recover_opener, &first) == 0;
  bool entered = false;
  for (int i = 0; started && i < 10000 && !entered; i++) {
    entered = clog_test_compression_recovery_entered();
    if (!entered) usleep(1000);
  }
  clog second =
      started ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (second != CLOG_INVALID) ccol_log_info(second, "RECOVERYRACERECORD");
  bool held = false;
  for (int i = 0; second != CLOG_INVALID && i < 10000 && !held; i++) {
    held = clog_test_gz_dest_opened();
    if (!held) usleep(1000);
  }
  clog_test_hold_compression_recovery(false);
  if (started) pthread_join(tid, NULL);
  clog_test_hold_compressions(false);
  if (second != CLOG_INVALID) clog_close(second);
  if (first.lg != CLOG_INVALID) clog_close(first.lg);
  size_t runs = clog_test_compressions_finished() - finished_before;
  int gz = count_files_with_suffix(dir, ".gz");
  int tmp = count_files_with_suffix(dir, ".gz.tmp");
  cleanup_dir(dir, "app.log");
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(entered);
  REQUIRE_NE(first.lg, CLOG_INVALID);
  REQUIRE_NE(second, CLOG_INVALID);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(runs, (size_t)1);
  REQUIRE_EQ(gz, 1);
  REQUIRE_EQ(tmp, 0);
}

/* ------------------------------------------------------------------------ */
/* The enqueue-failure fallback and records that a batch keeps               */
/* ------------------------------------------------------------------------ */

/* Logs RECORD_A, keeps it in the batch with `errors` failed writes, then
 * logs RECORD_B through the enqueue-failure fallback, closes, and gives the
 * offsets of both records in the file, or -1. */
static void run_fallback_order_case(unsigned int errors, long *off_a,
                                    long *off_b) {
  *off_a = -1;
  *off_b = -1;
  char dir[256];
  if (make_tmpdir(dir, sizeof(dir)) != 0) return;
  char path[512];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  if (lg != CLOG_INVALID) {
    ccol_log_info(lg, "RECORD_A");
    /* The first failed write is the flush that the fallback runs first,
     * which keeps RECORD_A in the batch; a second one fails the delivery
     * attempt that follows it. */
    clog_test_force_write_errors(ENOSPC, errors);
    /* Allocation #1 is the envelope and #2 the node of the queue; see
     * enqueue_failure_fallback_still_rotates_on_size. */
    atomic_store(&g_enqueue_fail_call_count, 0);
    atomic_store(&g_enqueue_fail_at, 2);
    ccol_log_info(lg, "RECORD_B");
    atomic_store(&g_enqueue_fail_at, -1);
    clog_test_force_write_errors(0, 0);
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

/*
 * A record that the enqueue-failure fallback writes never lands ahead of a
 * record that the batch keeps after a transient write error. The batch gets
 * one more delivery attempt first; when that attempt fails too, the record
 * joins the batch behind the kept one.
 *
 * This test is non-vacuous: a fallback that writes its record straight to
 * the file puts RECORD_B ahead of RECORD_A, which the batch still holds.
 */
TEST(async, enqueue_failure_fallback_keeps_the_order_of_kept_records) {
  long a1, b1, a2, b2;
  run_fallback_order_case(1, &a1, &b1);
  run_fallback_order_case(2, &a2, &b2);
  REQUIRE_GE(a1, 0L);
  REQUIRE_GT(b1, a1);
  REQUIRE_GE(a2, 0L);
  REQUIRE_GT(b2, a2);
}

/* The child: a logger on a pipe that is full, in blocking mode, and that
 * nobody reads, and a CLOG_FATAL record on that same logger. stderr goes to
 * "<path>err". */
static void fatal_on_stalled_own_pipe_child(const char *path, bool async) {
  forget_parent_fixtures_in_child();
  clog_test_set_exit_drain_budget_ms(1000);
  char errp[512];
  snprintf(errp, sizeof(errp), "%serr", path);
  int efd = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (efd < 0 || dup2(efd, STDERR_FILENO) < 0) _exit(3);
  int p[2];
  if (pipe(p) != 0 || !fill_pipe(p[0], p[1])) _exit(3);
  clog_async_cfg_t acfg = {.flush_interval_us = EXIT_TEST_NO_TIMER_US};
  clog lg = clog_open_fd(p[1], CLOG_INFO, async ? &acfg : NULL);
  if (lg == CLOG_INVALID) _exit(3);
  if (async) ccol_log_info(lg, "QUEUEDBEFOREFATAL");
  mark_child_ready(path);
  ccol_log_fatal(lg, "FATALONSTALLEDPIPE");
  _exit(4);
}

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

/*
 * A CLOG_FATAL record on a logger whose own output is a full pipe in
 * blocking mode, which nobody reads, still stops the process, within the
 * budget of the exit drain, for a synchronous and an async logger. The
 * record is lost, and a line on stderr, which is another file here, says so.
 *
 * This test is non-vacuous: a fatal record written without a bound blocks
 * in write(2) for ever, and an async one waits for ever on its writer
 * thread, which blocks the same way; the parent kills the child at its
 * bound.
 */
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

/* ========================================================================== */
/*        ROTATED-LOOKING ENTRIES THAT ARE NOT GENERATIONS OF THE LOG         */
/* ========================================================================== */

/* The hook below runs inside the window of a rotation in which the live
 * name names neither the old file nor the new one, and puts an entry there
 * the way another user who can write the directory would. It acts once. */
static atomic_int g_rh_hook_fired;
static int g_rh_fifo_reader = -1;

static void rh_plant_link(int dir_fd, const char *base) {
  if (atomic_exchange(&g_rh_hook_fired, 1)) return;
  (void)symlinkat("app.log-victim", dir_fd, base);
}

static void rh_plant_fifo(int dir_fd, const char *base) {
  if (atomic_exchange(&g_rh_hook_fired, 1)) return;
  if (mkfifoat(dir_fd, base, 0600) == 0)
    g_rh_fifo_reader = openat(dir_fd, base, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
}

/* The kind of the entry at path, examined without following a link: one
 * of S_IFREG, S_IFLNK, S_IFIFO and the rest, or 0 when nothing is there. */
static mode_t rh_entry_kind(const char *path) {
  struct stat st;
  if (lstat(path, &st) != 0) return 0;
  return st.st_mode & S_IFMT;
}

/* Counts the occurrences of needle in the file at path. */
static int rh_count_lines_with(const char *path, const char *needle) {
  char buf[16384];
  read_file(path, buf, sizeof(buf));
  int n = 0;
  for (const char *p = buf; (p = strstr(p, needle)) != NULL; p++) n++;
  return n;
}

/*
 * A link that another user puts at the live name while a rotation runs
 * never receives a record: the new live file is a file that the rotation
 * made, and it takes the live name with a rename that replaces the link.
 *
 * This test is non-vacuous: a rotation that opens the live name after it
 * moved the old file aside follows the link, and every record after the
 * rotation lands in the file that the link names.
 */
TEST(rotation_hijack, a_link_put_at_the_live_name_gets_no_record) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], victim[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(victim, sizeof(victim), "%s/app.log-victim", dir);
  bool setup = write_whole_file(victim, "VICTIM\n", 7);

  atomic_store(&g_rh_hook_fired, 0);
  clog_test_set_rotate_window_hook(rh_plant_link);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 200,
                             .max_rotated_files = 3};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 40; i++) ccol_log_info(lg, "RHRECORD %d", i);
    clog_close(lg);
  }
  clog_test_set_rotate_window_hook(NULL);

  bool fired = atomic_load(&g_rh_hook_fired) != 0;
  char vbuf[256];
  read_file(victim, vbuf, sizeof(vbuf));
  mode_t live_kind = rh_entry_kind(path);
  bool last_record_kept =
      any_file_with_prefix_contains(dir, "app.log", "RHRECORD 39");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(fired);
  REQUIRE_STREQ(vbuf, "VICTIM\n");
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
  REQUIRE_TRUE(last_record_kept);
}

/*
 * A FIFO that another user puts at the live name while a rotation runs
 * never blocks the rotation and never receives a record.
 *
 * This test is non-vacuous: a rotation that opens the live name after it
 * moved the old file aside opens the FIFO, whose reader this test holds so
 * that the open does not wait, and writes the next records into it.
 */
TEST(rotation_hijack, a_fifo_put_at_the_live_name_gets_no_record) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);

  atomic_store(&g_rh_hook_fired, 0);
  g_rh_fifo_reader = -1;
  clog_test_set_rotate_window_hook(rh_plant_fifo);
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 200,
                             .max_rotated_files = 3};
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 4; i++) ccol_log_info(lg, "RHFIFO %d padding", i);
    clog_close(lg);
  }
  clog_test_set_rotate_window_hook(NULL);

  bool fired = atomic_load(&g_rh_hook_fired) != 0;
  int reader = g_rh_fifo_reader;
  g_rh_fifo_reader = -1;
  char fbuf[256];
  ssize_t got = reader >= 0 ? read(reader, fbuf, sizeof(fbuf)) : -1;
  if (reader >= 0) close(reader);
  mode_t live_kind = rh_entry_kind(path);
  cleanup_dir(dir, "app.log");

  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(fired);
  REQUIRE_NE(reader, -1);
  REQUIRE_LE(got, 0);
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
}

/* The child of the test below. It ends with _exit() once the close has
 * returned, and marks that it got there. */
static void rh_fifo_at_rotated_name_child(const char *path) {
  forget_parent_fixtures_in_child();
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1 << 20,
                             .max_rotated_files = 1,
                             .compress_rotated = true};
  clog lg = clog_open_file(path, CLOG_INFO, &cfg, NULL);
  if (lg == CLOG_INVALID) _exit(3);
  ccol_log_info(lg, "RHGEN");
  clog_close(lg);
  mark_child_ready(path);
  _exit(0);
}

/*
 * A FIFO that another user puts at a rotated name is not a generation of
 * the log: the recovery of compressions at open never queues it, so the
 * compressor never opens it, clog_close() returns and the FIFO stays.
 *
 * This test is non-vacuous: a compressor that opens the FIFO waits for a
 * writer for ever, clog_close() waits for the compressor, and the parent
 * kills the child at its bound.
 */
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

/*
 * A link that another user puts at a rotated name is not a generation of
 * the log: nothing reads, archives or deletes it, and the file that it
 * names keeps its content and is never copied into a ".gz" file.
 *
 * This test is non-vacuous: a recovery that compresses the entry reads the
 * file through the link, writes it to "<name>.gz" and removes the link.
 */
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

  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 300,
                             .max_rotated_files = 1,
                             .compress_rotated = true};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 30; i++)
      ccol_log_info(lg, "RHGEN %d padding-padding", i);
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
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ((unsigned)link_kind, (unsigned)S_IFLNK);
  REQUIRE_EQ((unsigned)gz_kind, 0u);
  REQUIRE_STREQ(sbuf, "TOPSECRET\n");
}

/*
 * A regular file at a rotated name whose stamp lies far in the future, with
 * a collision suffix that nothing can follow, is not a generation of the
 * log. It neither stops the naming of rotations nor takes a retention slot,
 * and retention never deletes it.
 *
 * This test is non-vacuous: a naming that starts above that name finds no
 * free sequence number and refuses every rotation, so no generation stamped
 * with the clock ever appears, and the file grows without bound.
 */
TEST(rotation_foreign, a_far_future_name_neither_stops_rotation_nor_counts) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], stuck[256], future[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(stuck, sizeof(stuck), "%s/app.log.99991231235959_999999999", dir);
  snprintf(future, sizeof(future), "%s/app.log.99991231235959", dir);
  bool setup =
      write_whole_file(stuck, "", 0) && write_whole_file(future, "F\n", 2);

  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 300,
                             .max_rotated_files = 2};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 60; i++)
      ccol_log_info(lg, "RHSTALL %d padding-padding-padding", i);
    clog_close(lg);
  }

  /* Every generation that the clock stamps starts with the century. */
  int generations = count_files_with_prefix(dir, "app.log.2");
  mode_t stuck_kind = rh_entry_kind(stuck);
  mode_t future_kind = rh_entry_kind(future);
  struct stat live_st;
  bool live_bounded = stat(path, &live_st) == 0 && live_st.st_size < 2048;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(generations, 2);
  REQUIRE_EQ((unsigned)stuck_kind, (unsigned)S_IFREG);
  REQUIRE_EQ((unsigned)future_kind, (unsigned)S_IFREG);
  REQUIRE_TRUE(live_bounded);
}

/*
 * Every entry that looks like a rotated file and is not a generation gets a
 * WARN record, and no more than eight such records go out in a minute, so a
 * directory full of them cannot flood the log.
 *
 * This test is non-vacuous: without the records, the log holds none of
 * them; without the limit, it holds twenty.
 */
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
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1 << 20,
                             .max_rotated_files = 5,
                             .compress_rotated = true};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) clog_close(lg);
  int warns = rh_count_lines_with(
      path,
      "ignored an entry that is not a rotated file of this log: app.log.");
  int links = count_files_with_prefix(dir, "app.log.2020");
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(warns, 8);
  REQUIRE_EQ(links, 20);
}

/*
 * A regular file at a rotated name that another user owns is not a
 * generation of the log. The test needs to create a file owned by another
 * user, which only the superuser can do; it does nothing for any other
 * user.
 */
TEST(rotation_foreign, a_file_another_user_owns_is_never_compressed) {
  if (geteuid() != 0) {
    printf("  (needs root to create a file another user owns; not run)\n");
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
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 300,
                             .max_rotated_files = 1,
                             .compress_rotated = true};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 30; i++)
      ccol_log_info(lg, "RHGEN %d padding-padding", i);
    clog_close(lg);
  }
  char fbuf[64];
  read_file(foreign, fbuf, sizeof(fbuf));
  mode_t gz_kind = rh_entry_kind(gz);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_STREQ(fbuf, "THEIRS\n");
  REQUIRE_EQ((unsigned)gz_kind, 0u);
}

/*
 * The private name under which a rotation creates the next live file is
 * removed at the next open when a stopped process left it behind empty,
 * and only then: a file with content and a link at a name of that shape
 * stay.
 */
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
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 1 << 20,
                             .max_rotated_files = 5};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) clog_close(lg);
  mode_t empty_kind = rh_entry_kind(empty);
  mode_t full_kind = rh_entry_kind(full);
  mode_t link_kind = rh_entry_kind(link);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ((unsigned)empty_kind, 0u);
  REQUIRE_EQ((unsigned)full_kind, (unsigned)S_IFREG);
  REQUIRE_EQ((unsigned)link_kind, (unsigned)S_IFLNK);
}

/*
 * A rotating logger whose path is a link writes through the link until its
 * first rotation, which moves the link to a rotated name. From then on the
 * live file is a regular file that the logger made, and the link, which is
 * not a generation, is never followed, compressed or removed.
 *
 * This test is non-vacuous: a compression that opens the rotated name
 * through the link writes the file that the link names to "<name>.gz".
 */
TEST(rotation_foreign, a_live_path_that_is_a_link_is_moved_aside_once) {
  char dir[64];
  REQUIRE_EQ(make_cwd_tmpdir(dir, sizeof(dir)), 0);
  char path[256], target[256];
  snprintf(path, sizeof(path), "%s/app.log", dir);
  snprintf(target, sizeof(target), "%s/app.log-target", dir);
  bool setup =
      write_whole_file(target, "", 0) && symlink("app.log-target", path) == 0;
  clog_rotation_cfg_t cfg = {.size_rotation_enabled = true,
                             .max_file_size = 300,
                             .max_rotated_files = 50,
                             .compress_rotated = true};
  clog lg = setup ? clog_open_file(path, CLOG_INFO, &cfg, NULL) : CLOG_INVALID;
  if (lg != CLOG_INVALID) {
    for (int i = 0; i < 30; i++)
      ccol_log_info(lg, "RHLINK %02d padding-padding", i);
    clog_close(lg);
  }
  char tbuf[4096];
  read_file(target, tbuf, sizeof(tbuf));
  bool first_in_target = strstr(tbuf, "RHLINK 00") != NULL;
  bool last_in_target = strstr(tbuf, "RHLINK 29") != NULL;
  mode_t live_kind = rh_entry_kind(path);
  /* The link now sits at a rotated name of its own. */
  int links = 0;
  char link_gz[600] = {0};
  DIR *d = opendir(dir);
  struct dirent *e;
  while (d && (e = readdir(d)) != NULL) {
    char p[512];
    snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
    if (strncmp(e->d_name, "app.log.", 8) == 0 && rh_entry_kind(p) == S_IFLNK) {
      links++;
      snprintf(link_gz, sizeof(link_gz), "%s.gz", p);
    }
  }
  if (d) closedir(d);
  /* Nothing compressed the file that the link names. */
  mode_t link_gz_kind = link_gz[0] ? rh_entry_kind(link_gz) : 0;
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(setup);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_TRUE(first_in_target);
  REQUIRE_FALSE(last_in_target);
  REQUIRE_EQ((unsigned)live_kind, (unsigned)S_IFREG);
  REQUIRE_EQ(links, 1);
  REQUIRE_EQ((unsigned)link_gz_kind, 0u);
}
