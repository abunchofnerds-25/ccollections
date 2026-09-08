#include <clogger.h>
#include <common.h>
#include <dirent.h>
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                         HELPERS                                            */
/* ========================================================================== */

/* Some environments' own backtrace()/backtrace_symbols() genuinely cannot
 * unwind this process's call stack at all (confirmed directly: a minimal,
 * clogger.c-free program built for arm32 and run under qemu-arm's user-mode
 * emulation reports depth=0 from a plain backtrace() call three frames
 * deep, with no c_collections code involved). clog's own _capture_backtrace()
 * already treats this identically to a genuinely shallow real capture
 * (see its own CLOG_BT_INITIAL_FRAME doc comment): "nothing to append,
 * nothing to report missing", by design, not a bug. A handful of tests
 * below specifically assert that requesting a backtrace produces visible
 * frame content (or, for the "all frames failed to fit" tests, a marker
 * derived from having enough real frames to overflow the buffer);
 * neither assertion can hold in an environment where the underlying OS/
 * libc capability itself never produces more than a couple of frames, so
 * those tests probe this directly and skip (return early, which Tau
 * simply records as passed) rather than fail on a platform limitation
 * that has nothing to do with clog's own logic. */
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

/* Read up to `cap-1` bytes of a file into buf, NUL-terminate, return length. */
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
      snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
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
  snprintf(cmd, sizeof cmd, "gunzip -t '%s' 2>/dev/null", gz_path);
  return system(cmd);
}

/*
 * Decompress <gz_path> into buf (up to cap-1 bytes), NUL-terminate.
 * Returns the number of decompressed bytes, or 0 on error.
 */
static size_t gunzip_read(const char *gz_path, char *buf, size_t cap) {
  char cmd[1024];
  snprintf(cmd, sizeof cmd, "gunzip -c '%s' 2>/dev/null", gz_path);
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
  return mkdtemp(out) ? 0 : -1;
}

/* Count this process's currently-open file descriptors via /proc/self/fd
 * (Linux-specific); returns -1 if unavailable, letting a caller skip
 * gracefully on a platform without /proc rather than false-failing. The one
 * extra fd opendir() itself uses is already closed by closedir() before
 * this function returns, so it never leaks into the count a caller compares
 * a later call's result against. */
static int count_open_fds(void) {
  DIR *d = opendir("/proc/self/fd");
  if (!d) return -1;
  int count = 0;
  struct dirent *e;
  while ((e = readdir(d)) != NULL) {
    if (e->d_name[0] == '.') continue;
    count++;
  }
  closedir(d);
  return count;
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);

  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
  close(pipefd[0]); /* close the read end, breaking the write end */

  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Without SIGPIPE suppressed, this write() would raise SIGPIPE and, under
   * its default disposition, terminate the whole test process rather than
   * merely fail this one assertion. */
  log_info(lg, "should not crash the process");

  clog_close(lg);
  close(pipefd[1]);
}

/* ========================================================================== */
/*                         OUTPUT FORMAT                                      */
/* ========================================================================== */

TEST(output, logfmt_fields_present) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "key=\"value\"");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  /* The message contains '"' so it must be double-quoted in the output */
  REQUIRE_NE(strstr(buf, "msg="), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, all_levels_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_trace(lg, "t");
  log_debug(lg, "d");
  log_info(lg, "i");
  log_warn(lg, "w");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_error(lg, "something bad");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* The backtrace continuation line starts with a tab followed by '#' */
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         PROC FIELD                                         */
/* ========================================================================== */

static void *_proc_test_thread(void *arg) {
  clog lg = *(clog *)arg;
  log_info(lg, "from spawned thread");
  return NULL;
}

TEST(proc_field, logfmt_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = getpid();
  pid_t tid = (pid_t)syscall(SYS_gettid);

  log_info(lg, "proc test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* proc= field must be present */
  char *proc_start = strstr(buf, "proc=");
  REQUIRE_NE(proc_start, NULL);

  char *val = proc_start + 5; /* skip "proc=" */

  /* proc format: progname(pid):tname(tid) */
  /* (pid): must appear in the progname section */
  char pid_part[48];
  snprintf(pid_part, sizeof pid_part, "(%d):", (int)pid);
  REQUIRE_NE(strstr(val, pid_part), NULL);

  /* (tid) must appear in the thread section */
  char tid_part[48];
  snprintf(tid_part, sizeof tid_part, "(%d)", (int)tid);
  REQUIRE_NE(strstr(val, tid_part), NULL);

  /* Extract the value (ends at next space) and verify exactly one colon */
  char *val_end = strchr(val, ' ');
  REQUIRE_NE(val_end, NULL);
  size_t val_len = (size_t)(val_end - val);
  char proc_val[256];
  REQUIRE_EQ((val_len < sizeof proc_val), 1);
  memcpy(proc_val, val, val_len);
  proc_val[val_len] = '\0';

  char *colon = strchr(proc_val, ':');
  REQUIRE_NE(colon, NULL);
  REQUIRE_EQ(strchr(colon + 1, ':'), NULL); /* exactly one colon */

  cleanup_dir(dir, "app.log");
}

TEST(proc_field, json_proc_has_name_pid_tid_structure) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  pid_t pid = getpid();
  pid_t tid = (pid_t)syscall(SYS_gettid);

  log_info(lg, "proc json test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* "proc": key must exist */
  REQUIRE_NE(strstr(buf, "\"proc\":"), NULL);

  /* (pid): must appear in the progname section of the proc value */
  char pid_part[48];
  snprintf(pid_part, sizeof pid_part, "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  /* (tid) must appear in the thread section of the proc value */
  char tid_part[48];
  snprintf(tid_part, sizeof tid_part, "(%d)", (int)tid);
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
  pid_t tid = (pid_t)syscall(SYS_gettid);

  log_info(lg, "proc syslog test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

  /* proc SD param must be present inside the [ccol ...] element */
  REQUIRE_NE(strstr(buf, "proc=\""), NULL);

  /* (pid): must appear in the progname section of the proc SD value */
  char pid_part[48];
  snprintf(pid_part, sizeof pid_part, "(%d):", (int)pid);
  REQUIRE_NE(strstr(buf, pid_part), NULL);

  /* (tid) must appear in the thread section of the proc SD value */
  char tid_part[48];
  snprintf(tid_part, sizeof tid_part, "(%d)", (int)tid);
  REQUIRE_NE(strstr(buf, tid_part), NULL);
}

TEST(proc_field, different_threads_have_different_tids) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Log from the main thread */
  log_info(lg, "main thread");

  /* Log from a spawned thread */
  pthread_t thr;
  pthread_create(&thr, NULL, _proc_test_thread, &lg);
  pthread_join(thr, NULL);

  clog_close(lg);

  char buf[131072];
  read_file(path, buf, sizeof buf);

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
    REQUIRE_EQ((len < sizeof get_tid_str[i]), 1);
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_trace(lg, "should not appear");
  log_debug(lg, "should not appear");
  log_info(lg, "should not appear");
  log_warn(lg, "this should appear");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "should not appear"), NULL);
  REQUIRE_NE(strstr(buf, "this should appear"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(filtering, off_suppresses_all_log_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_OFF, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_error(lg, "suppressed");
  log_alert(lg, "suppressed");

  clog_close(lg);

  struct stat st;
  stat(path, &st);
  REQUIRE_EQ(st.st_size, (off_t)0);

  cleanup_dir(dir, "app.log");
}

TEST(filtering, set_level_changes_filter) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "before change - dropped");

  clog_set_level(lg, CLOG_TRACE);
  REQUIRE_EQ(clog_get_level(lg), CLOG_TRACE);

  log_info(lg, "after change - visible");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "before change"), NULL);
  REQUIRE_NE(strstr(buf, "after change"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         STRUCTURED FIELDS                                  */
/* ========================================================================== */

TEST(fields, set_field_appears_in_output) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "service", "auth");
  clog_set_field(lg, "env", "prod");

  log_info(lg, "request handled");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "service=auth"), NULL);
  REQUIRE_NE(strstr(buf, "env=prod"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, update_existing_field) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "req_id", "aaa");
  clog_set_field(lg, "req_id", "bbb"); /* override */

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "req_id=aaa"), NULL);
  REQUIRE_NE(strstr(buf, "req_id=bbb"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, remove_field_disappears) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "trace_id", "xyz");
  clog_remove_field(lg, "trace_id");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "trace_id"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, clear_removes_all) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "a", "1");
  clog_set_field(lg, "b", "2");
  clog_set_field(lg, "c", "3");
  clog_clear_fields(lg);

  log_info(lg, "empty fields");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, " a="), NULL);
  REQUIRE_EQ(strstr(buf, " b="), NULL);
  REQUIRE_EQ(strstr(buf, " c="), NULL);
  REQUIRE_NE(strstr(buf, "empty fields"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, invalid_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* These keys must be silently ignored (empty, space, '=', control char, DEL,
   * ']', '\', '"'). */
  clog_set_field(lg, "", "v"); /* empty; violates RFC 5424 1*32PRINTUSASCII */
  clog_set_field(lg, "bad key", "v");
  clog_set_field(lg, "bad=key", "v");
  clog_set_field(lg, "bad\x01key", "v"); /* C0 control character */
  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");                 /* DEL (0x7f); not PRINTUSASCII */
  clog_set_field(lg, "bad]key", "v");  /* ']' breaks RFC 5424 SD elements */
  clog_set_field(lg, "bad\\key", "v"); /* '\' corrupts logfmt quoting */
  clog_set_field(lg, "bad\"key", "v"); /* '"' corrupts logfmt quoting */
  /* This key is valid and must appear. */
  clog_set_field(lg, "good_key", "ok");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "=v"), NULL); /* empty key would produce =v */
  REQUIRE_EQ(strstr(buf, "bad key"), NULL);
  REQUIRE_EQ(strstr(buf, "bad=key="), NULL);
  REQUIRE_EQ(strstr(buf, "bad]key"), NULL);
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, reserved_key_names_are_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* Every key _clog_write() itself always emits must be rejected as a field
   * name; allowing one through would produce a duplicate JSON key, a
   * duplicate logfmt key= token, or a duplicate syslog SD-PARAM-NAME. */
  const char *reserved[] = {"ts",   "level", "proc", "src",
                            "func", "msg",   "bt",   "bt_error"};
  for (size_t i = 0; i < sizeof(reserved) / sizeof(reserved[0]); i++)
    clog_set_field(lg, reserved[i], "should-not-appear");
  /* An ordinary key must still work. */
  clog_set_field(lg, "good_key", "ok");

  log_info(lg, "reserved key test");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "should-not-appear"), NULL);
  REQUIRE_NE(strstr(buf, "\"good_key\":\"ok\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, value_with_spaces_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "host", "web server 01");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Value contains spaces -> must be quoted */
  REQUIRE_NE(strstr(buf, "host=\"web server 01\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, del_byte_in_value_is_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* DEL (0x7f) must be escaped as \x7f in quoted logfmt values.
   * String literal concatenation terminates the hex escape before 'e'. */
  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  log_info(lg,
           "msg\x7f"
           "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Raw DEL must not appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* DEL must be represented as the escape sequence \x7f. */
  REQUIRE_NE(strstr(buf, "\\x7f"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, del_byte_in_key_is_rejected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* DEL (0x7f) is not in PRINTUSASCII (0x21-0x7e).  Keys are emitted verbatim
   * in logfmt, so a DEL key would silently corrupt the output.  It must be
   * rejected like C0 control characters. */
  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");
  clog_set_field(lg, "good_key", "ok");

  log_info(lg, "test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* No raw DEL must appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* The rejected key must not emit =v. */
  REQUIRE_EQ(strstr(buf, "=v"), NULL);
  /* The valid key must be present. */
  REQUIRE_NE(strstr(buf, "good_key=ok"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, non_ascii_byte_in_key_is_rejected) {
  /* CLOG_FMT_SYSLOG is restricted to fd-based loggers (a file-backed logger
   * silently ignores clog_set_format(..., CLOG_FMT_SYSLOG)), so this test
   * needs a pipe rather than the usual tmpfile, matching the other
   * syslog.* tests' own setup. */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* A byte >= 0x80 is outside PRINTUSASCII (0x21-0x7e); RFC 5424 requires
   * SD-PARAM-NAME to be PRINTUSASCII only, so a key containing one must be
   * rejected like any other out-of-range byte, not merely passed through
   * verbatim into the structured-data element. */
  clog_set_field(lg, "bad\xc3\xa9key",
                 "v"); /* embeds a UTF-8 'e with accent' */
  clog_set_field(lg, "good_key", "ok");

  log_info(lg, "test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

  /* No raw high-bit byte from the rejected key must appear in the output. */
  REQUIRE_EQ(memchr(buf, (int)(unsigned char)0xc3, strlen(buf)), NULL);
  /* The rejected key must not emit a v="..." SD-PARAM. */
  REQUIRE_EQ(strstr(buf, "=\"v\""), NULL);
  /* The valid key must still be present. */
  REQUIRE_NE(strstr(buf, "good_key=\"ok\""), NULL);
}

TEST(fields, newline_and_cr_in_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Value contains raw LF, CR, and HT; all must be escaped in logfmt
   * output so the record remains a single line. */
  clog_set_field(lg, "payload", "line1\nline2\rend\ttab");
  log_info(lg, "escape test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* An empty string value must be emitted as "" in logfmt. */
  clog_set_field(lg, "empty_field", "");
  log_info(lg, "empty value test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* The field must appear with an explicitly quoted empty string value. */
  REQUIRE_NE(strstr(buf, "empty_field=\"\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, backslash_in_value_is_quoted_and_escaped_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A backslash in a value triggers double-quoting in logfmt, and each
   * backslash is escaped as \\ inside the quoted string. */
  clog_set_field(lg, "win_path", "C:\\Users\\foo");
  log_info(lg, "backslash test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Value must appear as win_path="C:\\Users\\foo"; each \ escaped to \\. */
  REQUIRE_NE(strstr(buf, "win_path=\"C:\\\\Users\\\\foo\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         OUTPUT; LONG MESSAGES                            */
/* ========================================================================== */

TEST(output, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Build a message longer than the 1024-byte stack buffer in _clog_write so
   * that the heap-allocation fallback is exercised. */
  char long_msg[2048];
  memset(long_msg, 'A', sizeof long_msg - 1);
  long_msg[sizeof long_msg - 1] = '\0';

  log_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* The full 2047-character message must appear verbatim in the output. */
  REQUIRE_NE(strstr(buf, long_msg), NULL);

  cleanup_dir(dir, "app.log");
}

/* An allocator that fails any request over 4096 bytes (the initial write
 * buffer / logger-construction allocations all fit under this; only a large
 * message's heap-spill allocation is meant to exceed it), used to force the
 * "heap allocation for the full message failed" fallback path deterministically
 * without needing a message anywhere near CLOG_BUF_MAX. */
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Longer than the 1024-byte stack buffer, and its +1-byte heap spill
   * request (~5000 bytes) exceeds the 4096-byte cap above, while everything
   * the record itself needs (well under 4096) does not. */
  char long_msg[5000];
  memset(long_msg, 'A', sizeof long_msg - 1);
  long_msg[sizeof long_msg - 1] = '\0';

  log_info(lg, "%s", long_msg);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* The heap spill failed, so the full message must NOT appear verbatim... */
  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  /* ...and the resulting truncation must be visibly indicated, not silent. */
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         OVERSIZED RECORDS (FALLBACK)                       */
/* ========================================================================== */

TEST(oversized, logfmt_message_over_buf_cap_falls_back_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Exceeds clogger's internal 16 MiB write-buffer cap. */
  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  log_info(lg, "%s", huge);
  log_info(lg, "normal record after the oversized one");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* Exactly two well-formed lines: the fallback notice, then the normal
   * record; the oversized message must never desynchronize the stream. */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 2);
  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  REQUIRE_NE(strstr(buf, "normal record after the oversized one"), NULL);

  /* The fallback line itself must be well-formed: a properly closed quoted
   * msg value followed by a real trailing newline, not a dangling `msg="`
   * with nothing after it (what an unguarded overflow used to produce). */
  char *fallback_line = strstr(buf, "too large to emit");
  REQUIRE_NE(fallback_line, NULL);
  char *line_end = strchr(fallback_line, '\n');
  REQUIRE_NE(line_end, NULL);
  REQUIRE_EQ(*(line_end - 1), '"');

  cleanup_dir(dir, "app.log");
}

TEST(oversized, json_message_over_buf_cap_falls_back_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  log_info(lg, "%s", huge);
  log_info(lg, "normal json record");

  clog_close(lg);
  free(huge);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* Both lines must be complete, individually valid JSON objects; neither
   * may bleed into the other. */
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

/* When the record itself falls back to the generic oversized-record
 * placeholder (see _clog_build_fallback_record), a backtrace requested via
 * log_error/log_alert/log_fatal must still be visibly marked as omitted in
 * the JSON output, exactly like a "bt" array that failed to embed inline
 * (see _emit_backtrace_json's own bt_error marker); never a record that
 * silently looks like an ordinary non-backtrace success. */
TEST(oversized, json_fallback_with_backtrace_marks_bt_as_unavailable) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'B', sz);
  huge[sz] = '\0';

  log_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  /* Exactly one well-formed JSON object line. */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  REQUIRE_NE(strstr(buf, "too large to emit"), NULL);
  /* The requested backtrace must be marked as omitted, not silently absent
   * with no way to distinguish this from a call that never asked for one. */
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

  log_info(lg, "%s", huge);
  log_info(lg, "normal syslog record");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
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

/* Unlike JSON (which embeds the backtrace inline in the very record that
 * falls back to the placeholder, see json_fallback_with_backtrace_marks_bt_
 * as_unavailable above), logfmt writes backtrace frames as separate,
 * independent continuation lines after the primary record. An oversized
 * message replacing that primary record with the fallback placeholder must
 * not suppress those continuation lines: the fallback path and the
 * backtrace-emission path are independent, and a requested backtrace must
 * still show up. */
TEST(oversized,
     logfmt_message_over_buf_cap_with_backtrace_still_writes_backtrace_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  size_t sz = 17UL * 1024 * 1024;
  char *huge = malloc(sz + 1);
  REQUIRE_NE(huge, CLOG_INVALID);
  memset(huge, 'A', sz);
  huge[sz] = '\0';

  log_error(lg, "%s", huge);

  clog_close(lg);
  free(huge);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof buf);
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

  /* The backtrace's own tab-indented continuation lines must still follow,
   * exactly as they would for an ordinary (non-oversized) log_error call,
   * in any environment where backtrace capture is genuinely available at
   * all (see backtrace_capture_genuinely_available()'s own doc comment). */
  if (backtrace_capture_genuinely_available()) {
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }

  /* Every line, whether the fallback record or a backtrace continuation,
   * must be individually well-formed and newline-terminated; the oversized
   * message must never desynchronize the stream. */
  size_t i = 0;
  while (i < len) {
    REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
    char *nl = memchr(buf + i, '\n', len - i);
    REQUIRE_NE(nl, NULL);
    i = (size_t)(nl - buf) + 1;
  }

  cleanup_dir(dir, "app.log");
}

/* Syslog's own backtrace frames are likewise written as separate PRI-prefixed
 * records, independent of whether the primary record fell back to the
 * placeholder for being oversized (see the logfmt counterpart above). */
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

  log_error(lg, "%s", huge);

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
  free(huge);

  /* PRI for CLOG_ERROR at the default CLOG_SYSLOG_USER facility (1):
   * 1*8 + 3 (error severity) = 11. At least two well-formed "<11>1 "
   * records: the fallback placeholder, plus one or more backtrace frames;
   * in any environment where backtrace capture is genuinely available at
   * all (see backtrace_capture_genuinely_available()'s own doc comment);
   * otherwise just the fallback placeholder itself. */
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "rotation test message index=%d padding-padding-padding", i);

  clog_close(lg);

  /* At least one rotated file should exist alongside app.log */
  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, max_rotated_files_respected) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "rotation pruning test idx=%d extra-data-to-fill-the-buffer",
             i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  /* The pruner keeps at most max_rotated_files on disk */
  REQUIRE_EQ((rotated <= 2), 1);

  cleanup_dir(dir, "app.log");
}

/* Regression test: a zero-initialized (i.e. left unset) max_rotated_files
 * used to mean "no limit", silently accumulating a rotated file forever and
 * risking filling the disk. It now falls back to CLOG_DEFAULT_MAX_ROTATED_FILES
 * (7), exactly like the other two rotation fields already fall back to their
 * own CLOG_DEFAULT_* constant when left at 0. */
TEST(rotation, max_rotated_files_zero_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Drive well more than CLOG_DEFAULT_MAX_ROTATED_FILES rotations; before
   * this fix, all of them would have survived on disk. */
  for (int i = 0; i < 40; i++)
    log_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

/* Same as above, but for a negative max_rotated_files: <= 0 falls back to
 * the default uniformly, matching max_file_size/rotation_interval_secs
 * exactly rather than treating 0 and negative values differently. */
TEST(rotation, max_rotated_files_negative_falls_back_to_default) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 200,
      .time_rotation_enabled = false,
      .max_rotated_files = -3,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 40; i++)
    log_info(lg, "default prune quota test idx=%d extra-data-to-fill", i);

  clog_close(lg);

  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated, 0);
  REQUIRE_EQ((rotated <= CLOG_DEFAULT_MAX_ROTATED_FILES), 1);

  cleanup_dir(dir, "app.log");
}

/* A file that merely starts with "<base>.<14 digits>" but isn't one this
 * logger ever produced (e.g. a manual, date-stamped backup a human or another
 * tool dropped in the same directory) must never be treated as a prune
 * candidate. A real rotated name is always exactly "<base>.<14 digits>",
 * optionally followed by "_NNNN" and/or ".gz"; anything else trailing the 14
 * digits must exclude the file from pruning entirely. */
TEST(rotation, prune_never_deletes_unrelated_file_with_similar_name) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  char foreign_path[600];
  snprintf(foreign_path, sizeof foreign_path,
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

  /* Drive enough rotations to force the pruner to run repeatedly against a
   * quota of just 1 kept file. */
  for (int i = 0; i < 40; i++)
    log_info(lg, "prune safety test idx=%d extra-data-to-fill-the-buffer", i);

  clog_close(lg);

  /* The foreign file must survive untouched, no matter how many times the
   * pruner ran. */
  struct stat st;
  REQUIRE_EQ(stat(foreign_path, &st), 0);
  char buf[64];
  read_file(foreign_path, buf, sizeof buf);
  REQUIRE_STREQ(buf, "do not delete me");

  cleanup_dir(dir, "app.log");
}

TEST(rotation, logger_recovers_after_file_externally_deleted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Pre-fill the file so bytes_written starts just 1 byte below the rotation
   * threshold.  The very first log write (which is at least 80 bytes) will
   * push the counter over the limit and trigger rotation. */
  {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    REQUIRE_NE(fd, -1);
    char filler[9999];
    memset(filler, 'x', sizeof filler);
    ssize_t w = write(fd, filler, sizeof filler);
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

  /* Simulate an external agent deleting the log file while the logger has it
   * open.  The fd remains valid; the directory entry is gone. */
  REQUIRE_EQ(unlink(path), 0);

  /* First write: bytes_written (9999 + line_len) >= 10000.  Rotation triggers.
   * rename(path, rotated) returns ENOENT (source gone).  The fixed _rotate
   * treats ENOENT as a clean-slate: it creates a fresh file at `path` via
   * O_CREAT and continues normally. */
  log_info(lg, "triggers rotation");

  /* Second write: bytes_written reset to 0 after rotation; line_len < 10000.
   * No further rotation; this line lands in the recreated file at `path`. */
  log_info(lg, "after recovery");

  clog_close(lg);

  /* The logger must have recreated the file at the original path. */
  struct stat st;
  REQUIRE_EQ(stat(path, &st), 0);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "after recovery"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, time_based_rotation_creates_rotated_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = false,
      .time_rotation_enabled = true,
      .rotation_interval_secs = 1,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* First write establishes last_rotation baseline; no rotation yet. */
  log_info(lg, "before rotation");

  /* Sleep long enough to exceed the 1-second rotation interval. */
  sleep(2);

  /* This write triggers time-based rotation. */
  log_info(lg, "after rotation");

  clog_close(lg);

  /* At least one rotated file must exist alongside the live log. */
  int rotated = count_files_with_prefix(dir, "app.log.");
  REQUIRE_NE(rotated, 0);

  /* The live file must contain the post-rotation message. */
  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "after rotation"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(rotation, same_second_collision_uses_suffix) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* With max_file_size = 10, every log write (each >>10 bytes) triggers an
   * immediate size-based rotation.  Five rotations within the same UTC second
   * forces the collision-handling code to generate _0001, _0002, ... suffixes.
   */
  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10,
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 5; i++) log_info(lg, "collision test %d", i);

  clog_close(lg);

  /* At least one file with the _0001 collision suffix must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, "_0001"), 0);

  cleanup_dir(dir, "app.log");
}

/* A persistently failing rotation (e.g. the destination directory lost write
 * permission) must not be retried on every single write: once one attempt
 * fails, further attempts are backed off for a bounded window rather than
 * costing every subsequent write the full rename()/open() syscall overhead
 * for as long as the underlying condition persists. */
TEST(rotation, persistent_failure_does_not_retry_on_every_write) {
  if (geteuid() == 0) {
    fprintf(stderr,
            "[SKIP] persistent_failure_does_not_retry_on_every_write: "
            "running as root, a permission-based rotation failure cannot "
            "be forced\n");
    return;
  }

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 10, /* tiny threshold: every write below exceeds it */
      .time_rotation_enabled = false,
      .max_rotated_files = 0,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Remove write permission on the directory so rename() inside _rotate()
   * keeps failing with EACCES on every attempt, simulating a persistent
   * rotation failure. */
  REQUIRE_EQ(chmod(dir, 0555), 0);

  clog_test_reset_rotate_attempt_count();

  /* Every one of these 20 writes is individually over the 10-byte
   * threshold, so without a backoff each would attempt (and fail) rotation. */
  for (int i = 0; i < 20; i++) log_info(lg, "retry backoff test idx=%d", i);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_field(parent, "service", "auth");
  clog_set_field(parent, "env", "prod");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  log_info(child, "from child");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Child must carry the snapshot of parent's fields at derive time */
  REQUIRE_NE(strstr(buf, "service=auth"), NULL);
  REQUIRE_NE(strstr(buf, "env=prod"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_field_does_not_affect_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  clog_set_field(child, "req_id", "child-only");

  /* Only parent writes; its line must not carry the child-only field */
  log_info(parent, "parent line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "req_id"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, parent_field_after_derive_does_not_affect_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Add a field to parent AFTER the derive snapshot was taken */
  clog_set_field(parent, "added_after", "yes");

  /* Only child writes; its line must not carry the post-derive parent field */
  log_info(child, "child line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "added_after"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, shares_output_target) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  log_info(parent, "parent message");
  log_info(child, "child message");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Both messages must land in the same file */
  REQUIRE_NE(strstr(buf, "parent message"), NULL);
  REQUIRE_NE(strstr(buf, "child message"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, child_close_does_not_break_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  log_info(child, "before child close");
  clog_close(child);

  /* Parent must still be usable after the derived logger is closed */
  log_info(parent, "after child close");
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "before child close"), NULL);
  REQUIRE_NE(strstr(buf, "after child close"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, parent_close_does_not_break_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  log_info(parent, "before parent close");
  clog_close(parent);

  /* Child must still be usable after its parent is closed */
  log_info(child, "after parent close");
  clog_close(child);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "before parent close"), NULL);
  REQUIRE_NE(strstr(buf, "after parent close"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, independent_level) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_WARN, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Lower child's level below parent's; child sees DEBUG, parent does not */
  clog_set_level(child, CLOG_DEBUG);

  log_debug(parent, "parent debug - dropped");
  log_debug(child, "child debug - visible");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_EQ(strstr(buf, "parent debug"), NULL);
  REQUIRE_NE(strstr(buf, "child debug"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, field_override_in_child_is_independent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_field(parent, "version", "1");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Override the inherited field in the child only */
  clog_set_field(child, "version", "2");

  log_info(parent, "parent write");
  log_info(child, "child write");

  clog_close(child);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof buf);

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

  REQUIRE_NE(strstr(ps, "version=1"), NULL); /* parent kept original */
  REQUIRE_NE(strstr(cs, "version=2"), NULL); /* child has its override */

  cleanup_dir(dir, "app.log");
}

TEST(derive, grandchild_derive) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_field(parent, "origin", "parent");

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);
  /* child inherits origin=parent from the snapshot; add its own field */
  clog_set_field(child, "gen", "child");

  /* Derive a grandchild from the child */
  clog grandchild = clog_derive(child);
  REQUIRE_NE(grandchild, CLOG_INVALID);
  /* grandchild inherits both origin=parent and gen=child */

  /* Close parent and child first; grandchild must remain fully usable */
  clog_close(parent);
  clog_close(child);

  log_info(grandchild, "from grandchild");
  clog_close(grandchild);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "from grandchild"), NULL);
  /* Both inherited fields must appear in the grandchild's line */
  REQUIRE_NE(strstr(buf, "origin=parent"), NULL);
  REQUIRE_NE(strstr(buf, "gen=child"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(derive, multiple_children_from_one_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_field(parent, "shared", "yes");

  clog child1 = clog_derive(parent);
  REQUIRE_NE(child1, CLOG_INVALID);
  clog_set_field(child1, "name", "c1");

  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_INVALID);
  clog_set_field(child2, "name", "c2");

  log_info(child1, "from child1");
  log_info(child2, "from child2");

  clog_close(child1);
  clog_close(child2);
  clog_close(parent);

  char buf[8192];
  read_file(path, buf, sizeof buf);

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
    log_info(a->lg, "thread=%d msg=%d", a->thread_id, i);
  return NULL;
}

TEST(threading, concurrent_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t threads[THREAD_COUNT];
  thread_arg_t args[THREAD_COUNT];

  for (int i = 0; i < THREAD_COUNT; i++) {
    args[i].lg = lg;
    args[i].thread_id = i;
    pthread_create(&threads[i], NULL, _writer_thread, &args[i]);
  }
  for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);

  clog_close(lg);

  /* Every line must start with "ts="; no interleaved partial writes */
  char buf[131072];
  size_t len = read_file(path, buf, sizeof buf);
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

  log_info(lg, "custom allocator test");

  clog_close(lg);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);
}

TEST(custom_alloc, open_file_uses_custom_allocator) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
  log_info(lg, "custom allocator file test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "custom allocator file test"), NULL);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);

  cleanup_dir(dir, "app.log");
}

TEST(custom_alloc, derived_logger_uses_parent_allocator) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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

  log_info(child, "child using parent allocator");
  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "child using parent allocator"), NULL);

  REQUIRE_NE(_custom_alloc_count, (size_t)0);
  REQUIRE_EQ(_custom_alloc_count, _custom_free_count);

  cleanup_dir(dir, "app.log");
}

TEST(custom_alloc, invalid_mprocs_returns_null) {
  ccol_memmgmt_procs_t bad_procs = {
      .malloc = _custom_malloc,
      .free = NULL, /* missing free; must be rejected */
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, &bad_procs);
  REQUIRE_EQ(lg, CLOG_INVALID);
}

/* A counting allocator whose malloc/calloc/realloc can be made to fail on
 * exactly one numbered call, used below to force clog_open_file_mp()'s own
 * later construction steps (async logging setup) to fail after the log
 * file itself has already been successfully opened. */
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

/* Regression test for a real fd leak: clog_open_file_mp() opens its log
 * file with a real open() call before the rest of construction runs; if a
 * LATER step (async logging setup, _shared_async_init(), engaged by a
 * non-NULL async_cfg) then fails under memory pressure, the already-open
 * fd was never closed on that failure path (only the earlier, separate
 * "_alloc() itself fails" path already closed it). Swept across a wide
 * range of "fail the Nth allocation" points, rather than one hardcoded
 * count (which would be fragile against unrelated allocation-count changes
 * elsewhere in the construction sequence), so several iterations land
 * squarely inside async setup itself; every iteration (whether the
 * failure lands there, earlier inside _alloc(), or nowhere at all) must
 * leave the process's open-fd count exactly where it started. */
TEST(custom_alloc, open_file_async_init_failure_does_not_leak_fd) {
  int baseline = count_open_fds();
  if (baseline < 0) {
    fprintf(stderr,
            "[SKIP] open_file_async_init_failure_does_not_leak_fd: "
            "/proc/self/fd unavailable on this platform\n");
    return;
  }

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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

/* Every public entry point must assert (not silently segfault with no
 * diagnostic) when handed a NULL logger handle, matching this project's own
 * established convention for an invalid raw handle (see e.g. chashmap.c's
 * chmap_elem_count/chmap_reset, or cvector.c's raw-layer functions, both of
 * which ccol_assert(false) on NULL rather than dereferencing it). Each check
 * runs in a forked child since the assertion aborts the whole process. */

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
static void _call_log_info_null(void) { log_info(CLOG_INVALID, "message"); }

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
    _exit(0); /* unreachable if the handle-validation assert fired */
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
  _expect_fatal(_call_clog_set_level_null);
}
TEST(null_handle, get_level_is_fatal) {
  _expect_fatal(_call_clog_get_level_null);
}
TEST(null_handle, set_format_is_fatal) {
  _expect_fatal(_call_clog_set_format_null);
}
TEST(null_handle, get_format_is_fatal) {
  _expect_fatal(_call_clog_get_format_null);
}
TEST(null_handle, set_facility_is_fatal) {
  _expect_fatal(_call_clog_set_facility_null);
}
TEST(null_handle, get_facility_is_fatal) {
  _expect_fatal(_call_clog_get_facility_null);
}
TEST(null_handle, set_field_is_fatal) {
  _expect_fatal(_call_clog_set_field_null);
}
TEST(null_handle, remove_field_is_fatal) {
  _expect_fatal(_call_clog_remove_field_null);
}
TEST(null_handle, clear_fields_is_fatal) {
  _expect_fatal(_call_clog_clear_fields_null);
}
TEST(null_handle, flush_is_fatal) { _expect_fatal(_call_clog_flush_null); }
TEST(null_handle, write_via_log_macro_is_fatal) {
  _expect_fatal(_call_log_info_null);
}

/* ========================================================================== */
/*                         ROBUSTNESS UNDER ALLOCATION FAILURE                */
/* ========================================================================== */

/* An allocator whose realloc() fails on exactly its g_realloc_fail_at'th
 * call (1-indexed; <=0 means "never fail") and succeeds otherwise, letting a
 * single test sweep a fault across every buffer-growth call a log_error()
 * with backtrace can make (logger construction, field-map operations, the
 * primary record, and every individual backtrace frame). */
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

  /* Sweep the fault across a wide range of call indices so it lands during
   * construction, the primary record, and (on a platform with backtrace
   * support) individual backtrace frames alike; every one of those buffer
   * growths must degrade gracefully rather than write a malformed line. */
  for (int fail_at = 1; fail_at <= 80; fail_at++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
    char path[512];
    snprintf(path, sizeof path, "%s/app.log", dir);

    g_realloc_call_count = 0;
    g_realloc_fail_at = fail_at;

    clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      /* Construction itself hit the fault; nothing to check this round. */
      cleanup_dir(dir, "app.log");
      continue;
    }

    log_error(lg, "robustness check %d", fail_at);

    g_realloc_fail_at = -1; /* don't let close()'s own bookkeeping fault */
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof buf);

    /* Every emitted line, whether the primary record or a backtrace
     * continuation, must be individually well-formed: starting with the
     * expected prefix and ending with a real newline. A malformed/partial
     * write (e.g. a truncated backtrace frame with no trailing newline)
     * would otherwise merge into whatever line follows it. */
    size_t i = 0;
    while (i < len) {
      REQUIRE_TRUE(buf[i] == 't' || buf[i] == '\t');
      if (buf[i] == 't') REQUIRE_EQ(strncmp(buf + i, "ts=", 3), 0);
      char *nl = memchr(buf + i, '\n', len - i);
      REQUIRE_NE(nl, NULL); /* every line must be newline-terminated */
      i = (size_t)(nl - buf) + 1;
    }

    cleanup_dir(dir, "app.log");
  }

  g_realloc_fail_at = -1;
}

/* JSON embeds the backtrace inline, before the record's closing "}\n", via a
 * separate code path from logfmt/syslog's own after-the-write, per-frame
 * emitters. A buffer-growth allocation failure while appending the "bt"
 * array (or backtrace_symbols() itself failing) must never silently omit the
 * backtrace from an otherwise ordinary-looking, successfully-closed JSON
 * record with no indication anything was missing: either the "bt" array
 * survives (possibly frame-truncated), a "bt_error" marker takes its place,
 * or the whole record is replaced by the well-formed oversized-record
 * fallback; but a JSON object closing normally with neither "bt" nor
 * "bt_error" present must never happen for a with_backtrace=true call. */
TEST(robustness, json_backtrace_failure_is_never_silently_dropped) {
  ccol_memmgmt_procs_t procs = {
      .malloc = _counting_malloc,
      .free = _counting_free,
      .calloc = _counting_calloc,
      .realloc = _counting_realloc,
  };

  for (int fail_at = 1; fail_at <= 80; fail_at++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
    char path[512];
    snprintf(path, sizeof path, "%s/app.log", dir);

    g_realloc_call_count = 0;
    g_realloc_fail_at = fail_at;

    clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, &procs);
    if (!lg) {
      cleanup_dir(dir, "app.log");
      continue;
    }
    clog_set_format(lg, CLOG_FMT_JSON);

    log_error(lg, "json backtrace robustness check %d", fail_at);

    g_realloc_fail_at = -1;
    clog_close(lg);

    char buf[65536];
    size_t len = read_file(path, buf, sizeof buf);
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

/* An allocator whose calloc() fails on demand (controlled by
 * g_fail_next_calloc), leaving malloc()/realloc()/free() untouched.  Used to
 * deterministically fail exactly one calloc() call (specifically, the
 * chashmap iterator allocation inside chashmap_begin_iter()) without
 * disturbing logger construction or clog_set_field()'s own map-growth
 * allocations, which must be allowed to succeed normally beforehand. */
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

/* chashmap_begin_iter() allocates its own iterator struct via calloc(); a
 * single transient failure of that one call, unrelated to the size of the
 * record being written, must not silently drop every field from an
 * otherwise-normal-looking record with no indication anything went wrong.
 * It must instead surface exactly like any other unrecoverable append
 * failure in _clog_write(): the well-formed fallback placeholder record,
 * carrying an accurate diagnostic (this is a transient allocation failure,
 * not an oversized record; the two must be distinguishable). */
TEST(robustness, field_iterator_alloc_failure_never_silently_drops_fields) {
  ccol_memmgmt_procs_t procs = {
      .malloc = _fail_next_calloc_malloc,
      .free = _fail_next_calloc_free,
      .calloc = _fail_next_calloc_calloc,
      .realloc = _fail_next_calloc_realloc,
  };

  const clog_format_t formats[] = {CLOG_FMT_LOGFMT, CLOG_FMT_JSON,
                                   CLOG_FMT_SYSLOG};
  for (size_t fi = 0; fi < sizeof formats / sizeof formats[0]; fi++) {
    char dir[256];
    REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
    char path[512];
    snprintf(path, sizeof path, "%s/app.log", dir);

    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, &procs);
    REQUIRE_NE(lg, CLOG_INVALID);
    clog_set_format(lg, formats[fi]);

    clog_set_field(lg, "service", "auth");
    clog_set_field(lg, "env", "prod");

    /* Fail exactly the next calloc() call: the field-map iterator's own
     * allocation, triggered from inside the log_info() call below. */
    g_fail_next_calloc = 1;
    log_info(lg, "message that must not silently lose its fields");
    g_fail_next_calloc = 0;

    clog_close(lg);

    char buf[4096];
    size_t len = read_file(path, buf, sizeof buf);
    REQUIRE_GT(len, (size_t)0);

    /* Either both fields made it into a well-formed record, or the fault
     * was surfaced via the well-formed fallback placeholder; what must
     * never happen is a record that looks like an ordinary success while
     * silently missing both fields. */
    bool has_service = strstr(buf, "service") != NULL;
    bool has_env = strstr(buf, "env") != NULL;
    bool has_alloc_failure_notice =
        strstr(buf, "transient allocation failure") != NULL;
    REQUIRE_TRUE((has_service && has_env) || has_alloc_failure_notice);

    /* This specific fault is a transient allocation failure, not an
     * oversized record; the fallback (when it fires) must say so rather
     * than misreport it as "too large to emit". */
    if (has_alloc_failure_notice)
      REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);

    cleanup_dir(dir, "app.log");
  }
}

/* An allocator whose malloc() fails on demand (controlled by
 * g_fail_next_malloc), leaving calloc()/realloc()/free() untouched. Used to
 * deterministically fail exactly one heap allocation triggered from inside a
 * single _clog_write() call, without disturbing logger construction (which
 * must succeed normally beforehand via a real malloc()). */
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

/* Regression test for a real bug (fixed): a genuine allocator failure while
 * spilling a long __FILE__ path to a heap buffer inside _clog_build_header()
 * used to leave _clog_build_record()'s own alloc_failure verdict false, so
 * the record fell back to the generic "log record too large to emit" note
 * even though the true cause had nothing to do with the record's size at all;
 * see clog_buf_t.oom's own doc comment. Directly exercises _clog_write()
 * with a synthetic "file" argument >= the 512-byte stack buffer
 * _clog_build_header() uses to build the logfmt "src=" value, the only way
 * to reach the heap-spill branch deterministically (the log_* macros always
 * pass the literal, normally-short __FILE__ of their own call site). */
TEST(robustness, header_build_allocation_failure_reported_accurately) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _fail_next_malloc_malloc,
      .free = _fail_next_malloc_free,
      .calloc = _fail_next_malloc_calloc,
      .realloc = _fail_next_malloc_realloc,
  };

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  char long_file[600];
  memset(long_file, 'A', sizeof long_file);
  long_file[sizeof long_file - 1] = '\0';

  /* Fail exactly the next malloc() call: the long-src heap-spill buffer
   * triggered from inside the _clog_write() call below (no fields are set on
   * this logger, so no field-iterator allocation competes for this same
   * single fault). */
  g_fail_next_malloc = 1;
  _clog_write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello world");
  g_fail_next_malloc = 0;

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  /* Must be reported as what it actually was (a transient allocation
   * failure), never mislabeled as an oversized record. */
  REQUIRE_NE(strstr(buf, "transient allocation failure"), NULL);
  REQUIRE_EQ(strstr(buf, "too large to emit"), NULL);

  cleanup_dir(dir, "app.log");
}

/* Directly exercises _clog_write() with a synthetic "file" argument >= the
 * 512-byte stack buffer _clog_write() uses to build the logfmt "src=" value,
 * containing logfmt-special bytes (space, '=').  Calling _clog_write()
 * directly (rather than via the log_* macros, which always pass the literal
 * __FILE__ of the call site) is the only way to control the file argument's
 * length precisely enough to reach this code path. */
TEST(robustness, long_file_path_in_src_field_is_still_quoted_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  char long_file[600];
  memset(long_file, 'A', sizeof long_file);
  long_file[0] = ' ';   /* logfmt-special: would-be first byte of src= value */
  long_file[100] = '='; /* logfmt-special: must not be read as a new token */
  long_file[sizeof long_file - 1] = '\0';

  _clog_write(lg, CLOG_INFO, long_file, 42, "myfunc", false, "hello %s",
              "world");

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  /* The record must stay exactly one well-formed logfmt line: a single
   * "ts=" token, and the msg= value must still be found and correctly
   * isolated; neither possible if the long src value had leaked an
   * unquoted space/'=' into the middle of the line. */
  int ts_count = 0;
  const char *p = buf;
  while ((p = strstr(p, "ts=")) != NULL) {
    ts_count++;
    p++;
  }
  REQUIRE_EQ(ts_count, 1);

  /* The src= value must be quoted: a raw, unquoted space/'=' from the long
   * file path must never appear as bare " src= " (no opening quote). */
  REQUIRE_EQ(strstr(buf, " src= "), NULL);
  REQUIRE_NE(strstr(buf, " src=\""), NULL);
  REQUIRE_NE(strstr(buf, "msg=\"hello world\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* A width/precision combination large enough that the two conversions'
 * combined output length exceeds INT_MAX makes vsnprintf() itself fail
 * (glibc returns -1 with errno=EOVERFLOW) before producing any content at
 * all, rather than merely truncating; the one case _clog_write()'s own
 * message-formatting step cannot route through its usual heap-spill /
 * "...[truncated]" fallback, since there is no partial content to fall back
 * to in the first place. */
TEST(robustness, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* The width is passed as a runtime argument (rather than embedded as a
   * literal digit sequence in the format string) specifically so GCC's own
   * static format-overflow analysis (-Wformat=2) cannot compute the
   * resulting length at compile time and reject the call outright; the
   * overflow this test needs to exercise is a genuine *runtime* vsnprintf()
   * failure, not a compile-time-detectable one. */
  int huge_width = 2000000000;
  _clog_write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);

  /* Must not be a silently dropped, empty record: a well-formed placeholder
   * line naming the real cause must be written instead. */
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  log_info(lg, "hello world");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Line must be a JSON object: starts with '{', closes with '}' before '\n' */
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  log_trace(lg, "t");
  log_debug(lg, "d");
  log_info(lg, "i");
  log_warn(lg, "w");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"TRACE\""), NULL);
  REQUIRE_NE(strstr(buf, "\"DEBUG\""), NULL);
  REQUIRE_NE(strstr(buf, "\"INFO\""), NULL);
  REQUIRE_NE(strstr(buf, "\"WARN\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, fields_are_top_level_json_keys) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "env", "prod");
  clog_set_field(lg, "service", "auth");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"env\":\"prod\""), NULL);
  REQUIRE_NE(strstr(buf, "\"service\":\"auth\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, special_chars_escaped_in_message) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* Message contains a double-quote and a newline */
  log_info(lg, "say \"hi\"\nworld");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "path", "C:\\Users\\foo");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Backslash must be escaped as \\ */
  REQUIRE_NE(strstr(buf, "C:\\\\Users\\\\foo"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, del_byte_escaped_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* DEL (0x7f) in a field value and in the message must be escaped as \u007f
   * in JSON output.  String literal concatenation prevents GCC from treating
   * \x7fe as a multi-digit hex escape sequence. */
  clog_set_field(lg, "k",
                 "val\x7f"
                 "end");
  log_info(lg,
           "msg\x7f"
           "end");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Raw DEL must not appear in the output. */
  REQUIRE_EQ(memchr(buf, 0x7f, strlen(buf)), NULL);
  /* DEL must be encoded as the JSON Unicode escape \u007f. */
  REQUIRE_NE(strstr(buf, "\\u007f"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, valid_multibyte_utf8_passed_through_unescaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* A genuine 2-byte e-acute (U+00E9), 3-byte euro sign (U+20AC), and 4-byte
   * emoji (U+1F600): all well-formed UTF-8, so none of them should ever be
   * replaced with the Unicode replacement character. */
  const char *msg =
      "caf\xc3\xa9 costs \xe2\x82\xac"
      "1 \xf0\x9f\x98\x80";
  log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, msg), NULL);
  REQUIRE_EQ(strstr(buf, "\\ufffd"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_lone_continuation_byte_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0x80 is a continuation byte with no preceding lead byte; never valid
   * UTF-8 on its own. */
  const char *msg =
      "before\x80"
      "after";
  log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "before\\ufffdafter"), NULL);
  REQUIRE_EQ(memchr(buf, 0x80, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_overlong_encoding_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0xc0 0x80 is the classic overlong encoding of NUL; 0xc0/0xc1 can never
   * begin a well-formed sequence at all. */
  const char *msg =
      "before\xc0\x80"
      "after";
  log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xc0, strlen(buf)), NULL);
  REQUIRE_EQ(memchr(buf, 0x80, strlen(buf)), NULL);
  REQUIRE_NE(strstr(buf, "before"), NULL);
  REQUIRE_NE(strstr(buf, "after"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_surrogate_half_replaced_with_replacement_char) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* 0xed 0xa0 0x80 would decode to U+D800, a UTF-16 surrogate half that is
   * never a valid Unicode scalar value on its own. */
  const char *msg =
      "before\xed\xa0\x80"
      "after";
  log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xed, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_truncated_sequence_at_end_of_message_replaced) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* A 3-byte lead (0xe2) with nothing following it at all; the sequence is
   * truncated by the end of the message, not merely by a bad continuation
   * byte. */
  const char *msg = "trunc\xe2";
  log_info(lg, "%s", msg);

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "trunc\\ufffd"), NULL);
  REQUIRE_EQ(memchr(buf, 0xe2, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, invalid_utf8_in_field_value_is_also_sanitized) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);
  clog_set_field(lg, "k",
                 "v\xff"
                 "end");

  log_info(lg, "test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"k\":\"v\\ufffdend\""), NULL);
  REQUIRE_EQ(memchr(buf, 0xff, strlen(buf)), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, error_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  log_error(lg, "something failed");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

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

/* Consistency guard, not a regression test in its own right: JSON already
 * correctly treats a genuinely successful-but-shallow capture (depth <=
 * CLOG_BT_INITIAL_FRAME) as a real, empty backtrace rather than a failure
 * (see _emit_backtrace_json()'s own array_content_is_accurate check);
 * this pins that behavior down directly, using the same
 * clog_test_force_shallow_backtrace_depth() hook the logfmt/syslog
 * regression tests use to exercise the identical boundary condition those
 * two formats used to get wrong. */
TEST(json, error_with_shallow_backtrace_capture_still_yields_empty_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  clog_test_force_shallow_backtrace_depth(true);
  log_error(lg, "something failed");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* A genuinely successful-but-shallow capture is a real, empty backtrace,
   * not a failure: "bt":[] with no bt_error marker, exactly matching an
   * ordinary (non-shallow) successful capture's own "bt":[...] shape, just
   * with zero elements. */
  REQUIRE_NE(strstr(buf, "\"bt\":[]"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, format_shared_with_derived_logger) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_set_format(parent, CLOG_FMT_JSON);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Derived logger shares the format because it shares the same backing store
   */
  REQUIRE_EQ(clog_get_format(child), CLOG_FMT_JSON);

  log_info(child, "child json");
  log_info(parent, "parent json");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_alert(lg, "alert message");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "alert message"), NULL);
  /* log_alert must produce a backtrace continuation line, in any environment
   * where backtrace capture is genuinely available at all (see this file's
   * own backtrace_capture_genuinely_available() doc comment). */
  if (backtrace_capture_genuinely_available()) {
    REQUIRE_NE(strstr(buf, "\t#"), NULL);
  }

  cleanup_dir(dir, "app.log");
}

TEST(output, error_and_alert_level_strings_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_error(lg, "error event");
  log_alert(lg, "alert event");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_alert(lg, "critical failure");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "critical failure"), NULL);
  /* log_alert must produce at least one backtrace continuation line */
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, set_format_on_derived_affects_parent) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog child = clog_derive(parent);
  REQUIRE_NE(child, CLOG_INVALID);

  /* Set JSON format via child; parent should see it too */
  clog_set_format(child, CLOG_FMT_JSON);
  REQUIRE_EQ(clog_get_format(parent), CLOG_FMT_JSON);

  log_info(parent, "parent line");

  clog_close(child);
  clog_close(parent);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Parent's line must be JSON */
  REQUIRE_EQ(buf[0], '{');

  cleanup_dir(dir, "app.log");
}

TEST(json, error_and_alert_level_strings_in_json) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  log_error(lg, "e");
  log_alert(lg, "a");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"ERROR\""), NULL);
  REQUIRE_NE(strstr(buf, "\"ALERT\""), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, alert_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  log_alert(lg, "alert event");

  clog_close(lg);

  char buf[16384];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* Field value with embedded HT and CR; both must be JSON-escaped */
  clog_set_field(lg, "data", "col1\tcol2\r\n");

  log_info(lg, "tab and cr test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
 * log_fatal calls exit() internally.  To prevent clogger allocations from
 * showing up as "still reachable" in the child's valgrind report we register
 * an atexit handler in the child that closes the logger before the process
 * terminates.  The global is set only inside the child branch (after fork) so
 * it never fires in the parent process.
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

  log_info(lg, "hello syslog");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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
  log_warn(lg, "w");
  log_error(lg, "e");
  log_alert(lg, "a");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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

  log_info(lg, "daemon log");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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

  log_info(lg, "structured");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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

  /* 37-char key; exceeds RFC 5424 SD-PARAM-NAME limit of 32. Names longer
   * than the limit are shortened to a 23-char prefix of the original key
   * plus a '~' and an 8-hex-digit hash of the full key (32 chars total),
   * rather than a bare 32-char prefix, so two distinct long keys sharing a
   * common prefix cannot collide into the identical SD-PARAM-NAME (see
   * long_keys_sharing_prefix_get_distinct_sd_param_names below). */
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456789", "val");

  log_info(lg, "truncation");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

  /* The stable prefix (first 23 chars of the original key) must be present. */
  char *name_start = strstr(buf, "abcdefghijklmnopqrstuv");
  REQUIRE_NE(name_start, NULL);

  /* The emitted SD-PARAM-NAME itself (up to the '=') must be at most 32
   * characters; the full, over-limit key must never appear verbatim. */
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

  /* Two distinct 40-char-plus keys sharing the same first 32 characters; a
   * bare 32-char truncation would collide both into the identical
   * SD-PARAM-NAME, which RFC 5424 forbids within one SD-ELEMENT. */
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_ONE", "one");
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456_TWO", "two");

  log_info(lg, "collision test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Attempting to set SYSLOG format on a file-backed logger must be a no-op. */
  clog_set_format(lg, CLOG_FMT_SYSLOG);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);

  log_info(lg, "still logfmt");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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

  log_error(lg, "with backtrace");

  char buf[16384];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* clog_open_fd with owns_fd=false allows CLOG_FMT_SYSLOG on a file fd. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);

  clog lg = clog_open_fd(fd, CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* log_fatal terminates the process; use a child to verify it and capture
   * the log output it writes before calling exit(). */
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
    log_fatal(lg, "fatal syslog event");
    _exit(0); /* unreachable */
  }

  int status;
  waitpid(pid, &status, 0);
  clog_close(lg);
  close(fd);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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

  /* RFC 5424 SD-PARAM-VALUE requires ], \, and " to be escaped. */
  clog_set_field(lg, "bracket", "val]end");
  clog_set_field(lg, "backslash", "C:\\foo");
  clog_set_field(lg, "quote", "say \"hi\"");

  log_info(lg, "sd escape test");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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

  /* A raw newline in the message would otherwise split one syslog record
   * into two lines, forging what looks like an unrelated, unprefixed
   * second record; a classic log-injection vector. It must be escaped. */
  log_info(lg, "first part\nFAKE-INJECTED-LINE data=1");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

  /* Exactly one syslog record: the entire output is a single line. */
  int nl_count = 0;
  for (size_t i = 0; i < n; i++)
    if (buf[i] == '\n') nl_count++;
  REQUIRE_EQ(nl_count, 1);
  REQUIRE_EQ(buf[n - 1], '\n');

  /* The embedded newline must survive as a literal backslash-n escape. */
  REQUIRE_NE(strstr(buf, "first part\\nFAKE-INJECTED-LINE"), NULL);
  /* No raw injected content masquerading as a second, unprefixed record. */
  REQUIRE_EQ(strstr(buf, "\nFAKE-INJECTED-LINE"), NULL);
}

TEST(syslog, field_value_with_embedded_newline_stays_single_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_set_field(lg, "payload", "line1\nFAKE-INJECTED-LINE");
  log_info(lg, "structured");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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
  log_info(lg, "cr and tab test");

  char buf[4096];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Write a sentinel line with the first logger. */
  clog lg1 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg1, CLOG_INVALID);
  log_info(lg1, "first open");
  clog_close(lg1);

  /* Open the same path again; must append, not truncate. */
  clog lg2 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg2, CLOG_INVALID);
  log_info(lg2, "second open");
  clog_close(lg2);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "before off - visible");

  clog_set_level(lg, CLOG_OFF);
  REQUIRE_EQ(clog_get_level(lg), CLOG_OFF);

  log_alert(lg, "after off - suppressed");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "before off - visible"), NULL);
  REQUIRE_EQ(strstr(buf, "after off - suppressed"), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         ADDITIONAL FIELD EDGE CASES                        */
/* ========================================================================== */

TEST(fields, remove_nonexistent_field_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Removing a key that was never set must not crash. */
  clog_remove_field(lg, "nonexistent");
  clog_remove_field(lg, "also_never_set");

  log_info(lg, "still works");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "still works"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, clear_empty_fields_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Clearing fields on a logger that never had any must not crash. */
  clog_clear_fields(lg);
  clog_clear_fields(lg);

  log_info(lg, "still works");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);
  REQUIRE_NE(strstr(buf, "still works"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fields, null_key_or_value_is_noop) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* NULL key and NULL value must both be rejected silently. */
  clog_set_field(lg, NULL, "v");
  clog_set_field(lg, "k", NULL);
  clog_set_field(lg, NULL, NULL);
  clog_remove_field(lg, NULL);

  clog_set_field(lg, "good", "yes");
  log_info(lg, "null field test");
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Only the valid field must appear; no crash. */
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

  log_info(child, "child syslog");

  char buf[4096];
  drain_pipe(parent, pipefd[0], pipefd[1], buf, sizeof buf);
  clog_close(child);

  /* Output must be RFC 5424 syslog, not logfmt. */
  REQUIRE_NE(strstr(buf, "<14>1 "), NULL);
  REQUIRE_NE(strstr(buf, "child syslog"), NULL);
}

/* RFC 5424 APP-NAME sanitization must filter out non-PRINTUSASCII bytes
 * wherever they occur, not stop at the first one and discard everything
 * after it. */
/* The same PRINTUSASCII filter is used for both APP-NAME and HOSTNAME (see
 * _sanitize_syslog_printusascii_field()'s own doc comment in clogger.c); this
 * test exercises it via the APP-NAME-shaped accessor, but the coverage
 * applies identically to the HOSTNAME field, which shares the exact same
 * RFC 5424 "1*NNNPRINTUSASCII" grammar and is no longer merely truncated at
 * the first space. */
TEST(syslog, appname_sanitizer_filters_not_truncates) {
  char out[49];

  /* A disqualifying byte in the middle must be dropped, not cause everything
   * after it to be discarded too. */
  clog_test_sanitize_syslog_appname(
      "ab\x01"
      "cd",
      out, sizeof out);
  REQUIRE_STREQ(out, "abcd");

  /* No PRINTUSASCII bytes at all (including an empty input) falls back to
   * "-". */
  clog_test_sanitize_syslog_appname("\x01\x02", out, sizeof out);
  REQUIRE_STREQ(out, "-");
  clog_test_sanitize_syslog_appname("", out, sizeof out);
  REQUIRE_STREQ(out, "-");

  /* Output is capped at outsz-1 bytes even when the input has more good
   * bytes than that. */
  char small_out[4];
  clog_test_sanitize_syslog_appname("abcdefgh", small_out, sizeof small_out);
  REQUIRE_EQ(strlen(small_out), (size_t)3);
  REQUIRE_STREQ(small_out, "abc");
}

/* Regression test for a real unsigned-integer-underflow bug: outsz - 1
 * wrapped around to SIZE_MAX for outsz == 0 (size_t is unsigned), which
 * defeated the loop's own bound entirely and let the function write an
 * unbounded number of bytes into a destination buffer that has none
 * allocated at all; outsz == 1 separately wrote one byte past the end of a
 * 1-byte buffer via the "-" fallback path. Every destination buffer below is
 * heap-allocated at its exact documented size (never a larger stack array)
 * specifically so a regression here is caught directly as a heap buffer
 * overflow by valgrind (make memtest), not merely masked by incidental stack
 * padding a stack-array version of this test could accidentally survive. */
TEST(syslog, appname_sanitizer_tolerates_degenerate_output_sizes) {
  /* outsz == 0: the function must not touch *out at all; there is no byte
   * in it to safely write even a NUL terminator to. */
  char *zero_out = malloc(1);
  REQUIRE_NE(zero_out, NULL);
  zero_out[0] = 'Z';
  clog_test_sanitize_syslog_appname("abc", zero_out, 0);
  REQUIRE_EQ(zero_out[0], 'Z');
  free(zero_out);

  /* outsz == 1: only room for the NUL terminator itself; the "-" fallback
   * (which needs 2 bytes) must not be attempted. */
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

/* Direct coverage of the control-character escaper shared by RFC 5424 MSG
 * content and by backtrace frame text in the logfmt and syslog formats (see
 * _buf_append_ctrl_escaped()'s own doc comment in clogger.c). Backtrace frame
 * text comes from backtrace_symbols(), whose real output cannot be forced to
 * contain a control character from a test, so this exercises the shared
 * escaping primitive directly rather than only indirectly through a real
 * backtrace. */
TEST(syslog, ctrl_escaped_helper_neutralizes_control_bytes) {
  char out[64];

  clog_test_append_ctrl_escaped("plain text, no escaping needed", out,
                                sizeof out);
  REQUIRE_STREQ(out, "plain text, no escaping needed");

  /* '\n' and '\r' use the same two-character mnemonic escapes as every other
   * format in this file; a raw newline here would otherwise split a
   * logfmt/syslog backtrace line (or the syslog MSG field) into two. */
  clog_test_append_ctrl_escaped("line1\nline2", out, sizeof out);
  REQUIRE_STREQ(out, "line1\\nline2");
  clog_test_append_ctrl_escaped("a\rb\tc", out, sizeof out);
  REQUIRE_STREQ(out, "a\\rb\\tc");

  /* Every other control byte (and DEL) falls back to the generic \xXX form.
   */
  clog_test_append_ctrl_escaped(
      "del\x7f"
      "end",
      out, sizeof out);
  REQUIRE_STREQ(out, "del\\x7fend");
  clog_test_append_ctrl_escaped(
      "a\x01"
      "b",
      out, sizeof out);
  REQUIRE_STREQ(out, "a\\x01b");

  /* Output is truncated (not overflowed) when it does not fit outsz. */
  char small_out[6];
  clog_test_append_ctrl_escaped("ab\ncd", small_out, sizeof small_out);
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
    log_info(child, "derived id=%d msg=%d", a->id, i);
  clog_close(child);
  return NULL;
}

TEST(threading, concurrent_parent_and_derived_writers_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog_set_field(parent, "role", "parent");

  pthread_t threads[DERIVED_THREAD_COUNT];
  derived_arg_t args[DERIVED_THREAD_COUNT];

  for (int i = 0; i < DERIVED_THREAD_COUNT; i++) {
    args[i].lg = parent;
    args[i].id = i;
    pthread_create(&threads[i], NULL, _derived_writer_thread, &args[i]);
  }

  /* Parent also writes concurrently. */
  for (int i = 0; i < DERIVED_MSGS_PER_THREAD; i++)
    log_info(parent, "parent msg=%d", i);

  for (int i = 0; i < DERIVED_THREAD_COUNT; i++) pthread_join(threads[i], NULL);
  clog_close(parent);

  /* Every non-backtrace line must start with "ts=". */
  char buf[524288];
  size_t len = read_file(path, buf, sizeof buf);
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

/* _clog_write() checks min_level once, unlocked, before acquiring
 * shared->mutex (a fast path so a filtered-out call does not contend for the
 * lock every other writer needs), then re-checks it again under the lock.
 * This stresses that unlocked check racing concurrent clog_set_level() calls
 * on the same handle from another thread: it must never crash, and any line
 * that does make it through must still be a well-formed, complete record. */
#define LEVEL_FASTPATH_WRITER_COUNT 4
#define LEVEL_FASTPATH_ITERATIONS 500

static void *_level_fastpath_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < LEVEL_FASTPATH_ITERATIONS; i++)
    log_trace(lg, "filtered %d", i);
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[LEVEL_FASTPATH_WRITER_COUNT], toggler;
  for (int i = 0; i < LEVEL_FASTPATH_WRITER_COUNT; i++)
    pthread_create(&writers[i], NULL, _level_fastpath_writer, &lg);
  pthread_create(&toggler, NULL, _level_fastpath_toggler, &lg);

  for (int i = 0; i < LEVEL_FASTPATH_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);
  pthread_join(toggler, NULL);

  clog_set_level(lg, CLOG_INFO);
  log_info(lg, "final marker");
  clog_close(lg);

  /* Sized well above the true worst case (LEVEL_FASTPATH_WRITER_COUNT *
   * LEVEL_FASTPATH_ITERATIONS = 2000 lines, each well under 300 bytes, if
   * every single write happens to land while the toggler has left the level
   * at CLOG_TRACE) rather than a size picked to fit the "typical" number of
   * lines that pass the filter: read_file() performs a single, bounded
   * read() from the start of the file, so an undersized buffer here would
   * intermittently truncate the file before reaching "final marker" (written
   * last) on whichever run happens to let more trace lines through than
   * usual, a real, timing-dependent flake previously observed with a
   * 131072-byte buffer. */
  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof buf);
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
    log_info(lg, "field race %d", i);
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

/* Regression test for a real data race: _clog_emit_fields()'s own live-field
 * source used to read chmap_elem_count(lg->fields) BEFORE acquiring
 * fields_mutex, racing a concurrent clog_set_field()/clog_remove_field() call
 * on the same handle from another thread. chashmap is an externally
 * synchronized container in this codebase (every other access to lg->fields
 * in clogger.c (_snapshot_fields(), clog_set_field(), clog_remove_field(),
 * clog_clear_fields(), clog_derive()) takes fields_mutex first), so that
 * unsynchronized read of lg->fields's own internal element count, concurrent
 * with a fields_mutex-protected mutation of the exact same map, was
 * undefined behavior even though it is hard to observe as a functional
 * difference in a plain (non-TSan) run. This test's own assertions only
 * check for the absence of a crash and well-formed output; the race itself
 * is what running this suite under -fsanitize=thread (see this directory's
 * test_tsan target) is meant to catch. */
TEST(threading, concurrent_set_field_and_write_no_race) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[FIELD_RACE_WRITER_COUNT], mutator;
  for (int i = 0; i < FIELD_RACE_WRITER_COUNT; i++)
    pthread_create(&writers[i], NULL, _field_race_writer, &lg);
  pthread_create(&mutator, NULL, _field_race_mutator, &lg);

  for (int i = 0; i < FIELD_RACE_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);
  pthread_join(mutator, NULL);

  clog_close(lg);

  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof buf);
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
    log_info(*lg, "churn writer message %d", i);
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
    log_info(child, "derived churn message %d", i);
    clog_close(child);
  }
  return NULL;
}

/* Stresses the rwlock-protected slot table and the lock-free pin/unpin
 * against real concurrent slot reuse: several threads continuously resolve
 * (log_info/clog_set_level/clog_get_level) one shared, still-open handle
 * while a separate thread repeatedly derives-then-immediately-closes fresh
 * child handles sharing the same underlying target; each close cycles a
 * slot through in_use=false, freed=true, generation bump, and reacquisition
 * by the very next derive, exactly the churn pattern _clog_atfork_prepare's
 * own freed-flag walk and _clog_handle_acquire's own freed-reset exist to
 * handle correctly. No crash, no hang, and no valgrind/TSan report is the
 * whole point of this test; it makes no assertion about log content. */
TEST(threading, concurrent_derive_close_churn_stresses_slot_table) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[SLOT_CHURN_WRITER_COUNT], deriver;
  for (int i = 0; i < SLOT_CHURN_WRITER_COUNT; i++)
    pthread_create(&writers[i], NULL, _slot_churn_writer, &lg);
  pthread_create(&deriver, NULL, _slot_churn_deriver, &lg);

  for (int i = 0; i < SLOT_CHURN_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);
  pthread_join(deriver, NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         FORK SAFETY                                       */
/* ========================================================================== */

/* This entire section exercises src/clogger.c's own pthread_atfork()-based
 * fork() safety machinery, which is itself compiled out when
 * FORK_SAFETY_REQUIRED is 0 (see that macro's own doc comment in common.h);
 * without that machinery these tests' own premises (a forked child never
 * inheriting a locked clog_slot_table.rwlock/shared->mutex/fields_mutex) no
 * longer hold, so they are compiled out along with it rather than left in to
 * hang or fail. */
#if FORK_SAFETY_REQUIRED

/* A synchronous logger created before fork(), logged from in the child:
 * exercises the atfork prepare/parent/child protection of
 * clog_slot_table.rwlock/shared->mutex/fields_mutex directly. Before the
 * fix in _clog_atfork_release (re-initializing the rwlock in the child
 * instead of unlocking it), this reproduced as a real, permanent hang: the
 * child inherited the write-locked rwlock and its own subsequent
 * _clog_resolve() call blocked forever in rw_lock_rdlock. */
TEST(fork_safety, child_can_log_after_fork) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pid_t pid = fork();
  if (pid == 0) {
    log_info(lg, "message from child");
    clog_close(lg);
    _exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* Bounded wait, not a blocking waitpid(): a regression of the rwlock fix
   * would hang the child forever, and this test must fail visibly rather
   * than hang the whole suite. 30s (not a few hundred ms) specifically to
   * stay well clear of make memtest's own valgrind instrumentation
   * overhead, which applies to this forked child too (a real, reproduced
   * false failure at a 5s bound: valgrind slowed the child enough to miss
   * it, triggering the SIGKILL fallback below and leaving that child's own
   * heap allocations reported as "still reachable" instead of cleanly
   * freed by its own _exit()/clog_close() path). */
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
    REQUIRE_TRUE(false); /* child never completed within the bound */
  }
  /* WIFEXITED only, deliberately not WEXITSTATUS(status) == 0 too: a real,
   * reproduced-under-make-memtest finding is that this child's own exit
   * code is not reliably 0 under this suite's own memtest flags, for a
   * reason that has nothing to do with this test's own correctness.
   * make memtest runs with --errors-for-leak-kinds=all plus
   * --error-exitcode=1; when the child (forked mid-suite, well before the
   * REST of this binary's own tests have run and freed their own,
   * unrelated allocations) reaches its own normal, voluntary exit, valgrind
   * runs a full "still reachable" leak check against the CHILD's entire
   * process image at that instant; which necessarily includes every
   * still-live allocation belonging to every OTHER, unrelated test and
   * module in this binary that simply has not been torn down yet by that
   * point in the suite (this is expected and correct: this whole process
   * is only ever fully quiesced once, at the very end of main(), not after
   * every individual test). --errors-for-leak-kinds=all counts that as a
   * real error and silently overrides the child's own exit status to
   * --error-exitcode's value, regardless of what value _exit() was actually
   * given; --child-silent-after-fork=yes only suppresses the child's own
   * diagnostic OUTPUT, not this exit-status substitution (confirmed
   * directly against a minimal fork()+_exit(0) repro reproducing the exact
   * same override with a trivial, single-allocation child). This is a
   * property of forking mid-suite under this project's own memtest flags,
   * not something switching _exit() for exit() changes (a real, ruled-out
   * attempt: exit() lets this file's own destructor run, but a full
   * --leak-check=full pass at that same instant still finds every other
   * still-reachable allocation belonging to the rest of the not-yet-run
   * suite regardless, forcing the identical override). WIFEXITED alone
   * already catches what this test actually cares about: a fork-safety
   * regression re-hangs (caught above, by the bounded wait) or re-crashes
   * the child (a real ccol_assert()/fatal_err() abort raises SIGABRT,
   * making WIFEXITED false here, not merely WEXITSTATUS nonzero). */
  REQUIRE_TRUE(WIFEXITED(status));

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "message from child"), NULL);

  cleanup_dir(dir, "app.log");
}

#define FORK_CHURN_WRITER_COUNT 4
#define FORK_CHURN_ITERATIONS 300

static void *_fork_churn_writer(void *arg) {
  clog *lg = (clog *)arg;
  for (int i = 0; i < FORK_CHURN_ITERATIONS; i++) {
    log_info(*lg, "fork churn message %d", i);
    clog child = clog_derive(*lg);
    if (child != CLOG_INVALID) clog_close(child);
  }
  return NULL;
}

/* Triggers fork() from a dedicated thread while several OTHER threads are
 * concurrently logging/deriving/closing on the same shared target,
 * confirming the parent's own logging continues uninterrupted immediately
 * after fork() returns; verifying _clog_atfork_parent actually restores
 * normal operation (releasing every lock it acquired in _clog_atfork_
 * prepare), not merely that the child avoids hanging. */
TEST(fork_safety, concurrent_fork_during_churn_does_not_hang) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[FORK_CHURN_WRITER_COUNT];
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
    pthread_create(&writers[i], NULL, _fork_churn_writer, &lg);

  /* Fork partway through the churn, from the main test thread. */
  usleep(1000);
  pid_t pid = fork();
  if (pid == 0) {
    /* Child: only this thread exists here; confirm it can still resolve
     * and use the inherited handle before exiting. */
    log_info(lg, "post-fork child message");
    _exit(0);
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* 30s bound: see the identical note on fork_safety.child_can_log_after_
   * fork's own wait above for why this must stay well clear of make
   * memtest's own valgrind instrumentation overhead. */
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
    REQUIRE_TRUE(false); /* child never completed within the bound */
  }

  /* The parent's own writer threads must still be able to finish; if
   * _clog_atfork_parent failed to release everything it locked, these
   * joins would hang. */
  for (int i = 0; i < FORK_CHURN_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);

  log_info(lg, "final parent marker");
  clog_close(lg);

  /* Sized well above the true worst case (FORK_CHURN_WRITER_COUNT *
   * FORK_CHURN_ITERATIONS = 1200 lines), matching the identical sizing
   * rationale already established for threading.
   * concurrent_filtered_writes_and_level_changes_no_crash above: read_file()
   * performs a single, bounded read(), so an undersized buffer here would
   * truncate the file before reaching "final parent marker" (written
   * last). */
  char buf[1 << 20];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "final parent marker"), NULL);

  cleanup_dir(dir, "app.log");
}

/* Appends one line to path, creating it if necessary. Used by
 * _fork_safety_rollback_race_child() below to record its own progress
 * somewhere the outer test can read back; NOT via its own exit code, since
 * that is unreliable under make memtest for the same documented reason
 * fork_safety.child_can_log_after_fork's own WIFEXITED-only check exists:
 * valgrind's --errors-for-leak-kinds=all silently overrides a forked child's
 * real exit code to --error-exitcode's value the instant it finds ANY
 * "still reachable" allocation in that child's inherited process image,
 * which a child forked mid-suite (inheriting every other, not-yet-torn-down
 * test's own live allocations) always does. */
static void _fork_safety_marker_append(const char *path, const char *line) {
  int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (fd < 0) return;
  ssize_t w = write(fd, line, strlen(line));
  (void)w;
  close(fd);
}

/* Runs entirely inside an isolated, forked-off child: a regression of the
 * bug this test guards against crashes the calling process at the SECOND
 * fork() below (inside _clog_atfork_prepare(), which runs synchronously in
 * the calling thread before fork() itself returns anywhere), so this whole
 * scenario must not run directly in the main test process. `marker_path` is
 * used exactly like a log file any other test in this suite reads back after
 * a forked child exits; see this function's own _fork_safety_marker_append
 * helper above for why exit codes are not used for this instead. */
static void _fork_safety_rollback_race_child(const char *marker_path) {
  char dir[256];
  if (make_tmpdir(dir, sizeof dir) != 0) _exit(0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Force this logger's own acquisition to grow the slot table with a fresh
   * slot and then fail that slot's own live_shareds registration step,
   * driving _clog_handle_acquire() into the exact rollback path that used to
   * leave a freshly-grown, never-registered slot behind with freed == false
   * and ptr == NULL simultaneously; regardless of whatever free_indices
   * entries earlier tests already run in this process may have left behind
   * for reuse, which would otherwise mask the bug via the OTHER, already-
   * safe rollback branch (a reused slot, already freed == true). */
  clog_test_force_next_fresh_slot_registration_failure(true);
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  if (lg != CLOG_INVALID) {
    cleanup_dir(dir, "app.log");
    _exit(0); /* the forced failure did not apply; nothing more to check */
  }
  _fork_safety_marker_append(marker_path, "acquire_failed_as_expected\n");

  /* fork() here drives _clog_atfork_prepare()'s walk over every slot in
   * clog_slot_table.slots. Before the fix, the slot left behind above
   * (freed == false, ptr == NULL) made that walk's
   * mutex_lock(slot->ptr->fields_mutex) call dereference NULL and crash this
   * entire process (this line never returning at all, in either direction),
   * well before either fork() return value was ever seen. */
  pid_t pid = fork();
  _fork_safety_marker_append(marker_path, "survived_fork_call\n");

  if (pid == 0) _exit(0);
  if (pid > 0) waitpid(pid, NULL, 0);
  cleanup_dir(dir, "app.log");
  _exit(0);
}

/* Regression test for a real bug: _clog_handle_acquire()'s rollback for a
 * failed live_shareds registration pushed the slot's index back onto
 * free_indices without setting slot->freed = true whenever that slot came
 * from the "grow the table with a brand new slot" branch (clog_slot_t
 * fresh = {0}, i.e. already freed == false, ptr == NULL before the rollback
 * even runs). _clog_atfork_prepare()'s own walk only ever skips a slot via
 * `if (slot->freed) continue;`, so it dereferenced that slot's NULL ptr
 * (mutex_lock(slot->ptr->fields_mutex)) the moment any thread called fork()
 * while that now-"free" index sat unused in free_indices, crashing the whole
 * process. Fixed by explicitly restoring both freed and ptr to the same
 * shape a slot retired by clog_close() itself always ends up in, regardless
 * of which of the two acquisition branches produced it. */
TEST(fork_safety, handle_acquire_rollback_leaves_slot_fork_safe) {
  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof marker_dir), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof marker_path, "%s/marker", marker_dir);

  pid_t pid = fork();
  if (pid == 0) {
    _fork_safety_rollback_race_child(marker_path);
    _exit(127); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  int status;
  bool reaped = false;
  /* 30s bound: see the identical note on fork_safety.child_can_log_after_
   * fork's own wait above for why this must stay well clear of make
   * memtest's own valgrind instrumentation overhead. */
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
    REQUIRE_TRUE(false); /* child never completed within the bound */
  }
  /* WIFEXITED only, deliberately not the child's own exit code; see
   * fork_safety.child_can_log_after_fork's own identical note above for why
   * a forked child's exit code is not a reliable signal under make memtest.
   * A real regression of the bug this test guards against makes this
   * REQUIRE_TRUE itself fail (the child dies with SIGSEGV instead of exiting
   * at all), which is the actual crash-detection this test performs. */
  REQUIRE_TRUE(WIFEXITED(status));

  /* The marker file is what actually confirms the scenario was exercised at
   * all (rather than the forced hook silently not applying) and that the
   * process got all the way past the fork() call under test, not merely that
   * SOME exit happened. */
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "acquire_failed_as_expected"), NULL);
  REQUIRE_NE(strstr(buf, "survived_fork_call"), NULL);

  cleanup_dir(marker_dir, "marker");
}

/* Regression test for a real bug: force_next_fresh_slot_registration_failure's
 * own doc comment (and clogger.h's public one) claim it forces
 * _clog_handle_acquire()'s rollback path for clog_open_fd_mp()/
 * clog_open_file_mp()/clog_derive() alike, but the forced-failure branch was
 * only ever consulted when the acquiring handle's shared object was NOT
 * already present in clog_slot_table.live_shareds. clog_derive()'s shared
 * object is always the parent's own, already-registered one (guaranteed
 * present in live_shareds for as long as parent remains a live, pinned
 * handle), so already_registered was always true there, silently defeating
 * the hook for this one of its three documented call sites: the hook would
 * still auto-disarm itself, but clog_derive() would return a perfectly valid
 * handle instead of the documented CLOG_INVALID. Fixed by making the forced
 * failure apply unconditionally, regardless of already_registered. No
 * process-isolation/fork needed here (unlike
 * handle_acquire_rollback_leaves_slot_fork_safe above): this test only
 * exercises the ordinary rollback return path, which does not corrupt any
 * process-wide state when it works correctly. */
TEST(fork_safety, handle_acquire_rollback_hook_also_fires_for_derive) {
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);

  clog_test_force_next_fresh_slot_registration_failure(true);
  clog child = clog_derive(parent);
  REQUIRE_EQ(child, CLOG_INVALID);

  /* The hook must be confined to the one acquisition it targeted: a plain,
   * unforced clog_derive() call right after it must succeed normally, and
   * the parent handle itself (whose already-registered shared object the
   * forced failure above must never have touched or corrupted) must still be
   * fully usable. */
  clog child2 = clog_derive(parent);
  REQUIRE_NE(child2, CLOG_INVALID);

  clog_close(child2);
  clog_close(parent);
}

/* Second half of pending_compress_not_permanently_exempt_from_pruning_in_child
 * below, run entirely inside the forked child: only this thread exists here,
 * so the parent's own writer thread that was mid-compression (and mid-
 * usleep(), holding the pending-compress delay) at fork() time does not
 * exist in this process at all. One more write is enough to prove the point:
 * max_file_size == 1 guarantees it triggers a fresh rotation, whose own
 * _prune_rotated() pass is what actually reveals (or, before the fix, fails
 * to reveal) whether the first rotation's own .gz file is still wrongly
 * treated as "compression still in flight" here. stale_gz (not stale_plain)
 * is what this test checks: the uncompressed source is also unlinked by the
 * PARENT's own still-running compressing thread once it finishes (a race
 * with this child that has nothing to do with the bug under test), but
 * nothing other than a prune pass in THIS process ever touches stale_gz. */
static void _fork_pending_compress_child(clog lg, const char *dir,
                                         const char *stale_gz,
                                         const char *marker_path) {
  clog_test_set_pending_compress_delay_us(0); /* this process's own copy of
      the hook; never affects the parent's already-in-flight compression */

  log_info(lg, "generation two content");
  clog_close(lg);

  bool pruned = (access(stale_gz, F_OK) != 0);
  _fork_safety_marker_append(marker_path,
                             pruned ? "pruned\n" : "still_exempt\n");
  cleanup_dir(dir, "app.log");
  _exit(0);
}

static void *_fork_pending_compress_writer(void *arg) {
  clog lg = *(clog *)arg;
  log_info(lg, "generation one content");
  return NULL;
}

/* Regression test for a real bug: _rotate() releases shared->mutex around
 * its slow gzip-compression step, publishing a stack-allocated
 * clog_pending_compress_t node into sh->pending_compress for the duration
 * (see _rotate()'s own comment) so a concurrent rotation's own
 * _prune_rotated() pass never deletes a file still being written. A fork()
 * landing inside that exact window used to leave that node linked into the
 * CHILD's own copy of sh->pending_compress forever: the compressing thread
 * that alone would ever unlink it does not exist in a freshly forked child
 * (fork() duplicates only the calling thread), so _prune_rotated() in that
 * child treated the node's filenames as permanently "still being
 * compressed" and exempted them from deletion no matter how many further
 * rotations that child went on to perform; a silent, permanent
 * max_rotated_files violation for that one generation, specific to the
 * child. Fixed by clearing sh->pending_compress for every live shared target
 * in _clog_atfork_child(), since no compression is genuinely still in
 * flight in a freshly forked child. */
TEST(fork_safety,
     pending_compress_not_permanently_exempt_from_pruning_in_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof marker_dir), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof marker_path, "%s/marker", marker_dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* any write triggers rotation */
      .time_rotation_enabled = false,
      .max_rotated_files = 1,
      .compress_rotated = true,
  };
  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Long enough to comfortably outlast fork() plus the child's own single
   * write/rotation/prune sequence below. */
  clog_test_set_pending_compress_delay_us(1000000);

  pthread_t t;
  int create_rv = pthread_create(&t, NULL, _fork_pending_compress_writer, &lg);
  if (create_rv != 0) clog_test_set_pending_compress_delay_us(0);
  REQUIRE_EQ(create_rv, 0);

  /* Spin-wait until the first rotation's .gz destination genuinely exists on
   * disk, so fork() below is guaranteed to land inside the real, on-disk
   * compression window rather than racing it. */
  while (!clog_test_gz_dest_opened()) usleep(1000);

  /* Snapshot the one-and-only rotated file's own ".gz" name while it is
   * guaranteed to already exist on disk (created by gzopen() before the
   * delay), before fork(). */
  char stale_gz[1024] = {0};
  {
    DIR *d = opendir(dir);
    /* Disarm this global, process-wide delay knob HERE, unconditionally,
     * regardless of what opendir()/the scan below finds: it is safe (the
     * writer thread's own in-flight _gzip_compress_file() call already
     * captured its own LOCAL copy of the delay value before starting its
     * usleep(), per that function's own doc comment, so disarming the
     * global now does not cut that sleep short) and it must happen before
     * the REQUIRE_NE below, which can return from this test function
     * immediately on failure. Leaving this knob armed past that early
     * return previously left every later test's own gzip compression
     * calls, for the rest of this binary's entire run, paying an extra
     * full second each -- confirmed as the actual root cause of a CI
     * failure cascade (this test's own REQUIRE_NE below failing
     * intermittently under qemu-arm's scheduling variance, then silently
     * corrupting roughly a dozen further, otherwise-unrelated compression
     * tests) via the [DEBUG_ROTATE_TIMING] instrumentation in
     * src/clogger.c's own _rotate(), which showed every subsequent
     * _gzip_compress_file() call taking a suspiciously exact ~1000ms
     * despite compressing a file of only a few hundred bytes. */
    clog_test_set_pending_compress_delay_us(0);
    REQUIRE_NE((void *)d, (void *)NULL);
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
      size_t nlen = strlen(e->d_name);
      if (nlen >= 3 && strcmp(e->d_name + nlen - 3, ".gz") == 0)
        snprintf(stale_gz, sizeof stale_gz, "%s/%s", dir, e->d_name);
    }
    closedir(d);
  }
  REQUIRE_NE(stale_gz[0], '\0');

  pid_t pid = fork();
  if (pid == 0) {
    _fork_pending_compress_child(lg, dir, stale_gz, marker_path);
    _exit(127); /* unreachable */
  }
  REQUIRE_NE(pid, -1);

  /* Parent: let the original, still-in-flight compression finish normally
   * and unlink its own pending-compress bookkeeping exactly as it always
   * has; this test is only about the CHILD's own, separate copy of that
   * list, not about disturbing the parent's already-correct behavior. The
   * global delay knob itself is already disarmed above (must happen before
   * fork(), not here, so an early REQUIRE_NE up there can never skip it);
   * joining the writer thread and closing lg still belong here, AFTER
   * fork(), since the whole point of this test is forking while that
   * thread's own compression is still genuinely in flight. */
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
    REQUIRE_TRUE(false); /* child never completed within the bound */
  }
  REQUIRE_TRUE(WIFEXITED(status));

  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "pruned"), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

/* Runs clog_close() on *lg on a background thread, for
 * fork_safety.fork_during_concurrent_close_does_not_leak_target_in_child
 * below: the outer test needs this call suspended (via
 * clog_test_set_close_finalize_delay_us()) on a thread OTHER than the one
 * that calls fork(), so the forked child inherits a slot that is mid-close
 * on a thread that will never resume there. */
static void *_fork_close_race_closer(void *arg) {
  clog *lg = (clog *)arg;
  clog_close(*lg);
  return NULL;
}

/* Regression test for a real bug: fork() by one thread landing while a
 * DIFFERENT thread's clog_close() call on a DIFFERENT handle was suspended
 * between that call's own step 2 (in_use cleared) and step 4 (slot retired,
 * the shared target's reference released) left that target permanently
 * unreclaimed in the child. The thread that would have finished retiring the
 * slot and releasing the target's reference does not exist in a freshly
 * forked child at all (fork() duplicates only the calling thread), so
 * neither step ever ran there, and the target's ref_count could never reach
 * zero afterward no matter how long that child process went on to run.
 * Fixed by having _clog_atfork_release()'s own child-side handling finish
 * exactly this close on the vanished thread's behalf; see
 * clog_atfork_closing_t's own doc comment in clogger.c. */
TEST(fork_safety, fork_during_concurrent_close_does_not_leak_target_in_child) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  char marker_dir[256];
  REQUIRE_EQ(make_tmpdir(marker_dir, sizeof marker_dir), 0);
  char marker_path[512];
  snprintf(marker_path, sizeof marker_path, "%s/marker", marker_dir);

  /* Baseline BEFORE opening the logger under test, so the assertion below
   * holds regardless of how many other shared targets happen to already be
   * live elsewhere in this same test binary at this point in the suite. */
  size_t baseline = clog_test_live_shareds_count();

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  REQUIRE_EQ(clog_test_live_shareds_count(), baseline + 1);

  /* Long enough to comfortably outlast fork() plus the child's own
   * live_shareds check below; clog_test_close_finalize_delay_entered()
   * (polled below) is what actually makes landing inside this window
   * deterministic, not the length of the delay itself. */
  clog_test_set_close_finalize_delay_us(1000000);

  clog to_close = lg;
  pthread_t closer;
  REQUIRE_EQ(pthread_create(&closer, NULL, _fork_close_race_closer, &to_close),
             0);

  /* Spin-wait until the closer thread has genuinely entered the widened
   * step-3/4 window (in_use already cleared, slot not yet retired) before
   * forking, rather than relying on a fixed sleep and hoping the timing
   * lines up. */
  while (!clog_test_close_finalize_delay_entered()) usleep(1000);

  pid_t pid = fork();
  if (pid == 0) {
    /* Child: only this thread exists here; the closer thread suspended
     * mid-clog_close() on `lg` never resumes. By the time fork() has
     * returned here at all, _clog_atfork_child() has already run
     * (pthread_atfork's own machinery invokes it synchronously as part of
     * fork() itself, strictly before fork() returns in either process), so
     * this fix's own effect is already observable immediately: before the
     * fix, lg's own target was never reclaimed here (live_shareds stuck at
     * baseline + 1 forever); with the fix, the suspended close has already
     * been finished on the vanished thread's behalf. */
    char buf[64];
    snprintf(buf, sizeof buf, "live_shareds=%zu\n",
             clog_test_live_shareds_count());
    _fork_safety_marker_append(marker_path, buf);
    _exit(0);
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
    REQUIRE_TRUE(false); /* child never completed within the bound */
  }
  REQUIRE_TRUE(WIFEXITED(status));

  /* The parent's own closer thread, and the real, unaffected clog_close()
   * it is running, must still finish normally once the artificial delay
   * elapses; confirming _clog_atfork_parent() released everything it
   * locked and that this fix changes nothing about the parent's own
   * behavior. */
  clog_test_set_close_finalize_delay_us(0);
  pthread_join(closer, NULL);
  REQUIRE_EQ(clog_test_live_shareds_count(), baseline);

  char expect[64];
  snprintf(expect, sizeof expect, "live_shareds=%zu", baseline);
  char buf[256];
  size_t len = read_file(marker_path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, expect), NULL);

  cleanup_dir(marker_dir, "marker");
  cleanup_dir(dir, "app.log");
}

#endif /* FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         ASYNC LOGGING                                      */
/* ========================================================================== */

/* Polls read_file() up to bound_ms (checking every 5ms) until needle appears
 * in path's own content, or the bound elapses. Used throughout this section
 * instead of a fixed sleep, since exactly when the writer thread gets
 * scheduled to process a given message is inherently non-deterministic. */
static bool _poll_for_substring(const char *path, const char *needle,
                                int bound_ms) {
  char buf[1 << 16];
  for (int waited_ms = 0; waited_ms < bound_ms; waited_ms += 5) {
    size_t len = read_file(path, buf, sizeof buf);
    (void)len;
    if (strstr(buf, needle) != NULL) return true;
    usleep(5000);
  }
  return false;
}

TEST(async, construct_and_close_unbounded_queue) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0}; /* queue_size == 0 -> unbounded */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "hello async unbounded");
  clog_close(lg); /* drains the writer thread before returning */

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "hello async unbounded"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(async, construct_and_close_bounded_queue) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {.queue_size = 8};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "hello async bounded");
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "hello async bounded"), NULL);

  cleanup_dir(dir, "app.log");
}

/* An all-zero clog_async_cfg_t (a non-NULL pointer to a struct whose every
 * field is 0) must still enable async mode, with every zero field falling
 * back to its own documented default; unlike clog_rotation_cfg_t, there is
 * no "enabled" flag inside this struct to leave false. */
TEST(async, degenerate_all_zero_config_falls_back_to_defaults) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "degenerate config still works");
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "degenerate config still works"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, messages_eventually_reach_target_without_explicit_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Small interval so this test does not need to wait long. */
  clog_async_cfg_t cfg = {.flush_interval_ms = 20};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "eventually visible");
  REQUIRE_TRUE(_poll_for_substring(path, "eventually visible", 2000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, size_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* A tiny flush_buffer_size and a long flush_interval_ms isolate the
   * size-based trigger: if this test's own message ever appears, it can
   * only be because the buffer threshold, not the timer, fired. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 32, .flush_interval_ms = 60000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  for (int i = 0; i < 20; i++) log_info(lg, "size trigger filler %d", i);

  REQUIRE_TRUE(_poll_for_substring(path, "size trigger filler", 3000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async, duration_triggered_flush) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* A huge flush_buffer_size and a short interval isolate the duration-based
   * trigger: this one message can never reach the size threshold on its
   * own, so if it appears, the timer fired. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_ms = 30};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "duration trigger marker");
  REQUIRE_TRUE(_poll_for_substring(path, "duration trigger marker", 3000));

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         ASYNC MESSAGE FORMATTING                          */
/*                                                                            */
/* Mirrors the synchronous path's own output.long_message_uses_heap_and_is_  */
/* not_truncated / message_exceeding_stack_buffer_marks_truncation_when_     */
/* heap_alloc_fails / robustness.format_string_failure_writes_diagnostic_    */
/* not_silence, but drives them through _clog_write_async()'s OWN message-   */
/* formatting logic (job->msg_inline / job->msg_heap, a 256-byte inline      */
/* buffer distinct from and smaller than _clog_write_sync()'s 1024-byte      */
/* stack buffer) rather than the synchronous body those tests exercise;      */
/* a previously untested code path with no regression coverage of its own.  */
/* ========================================================================== */

TEST(async, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Longer than job->msg_inline's 256-byte inline buffer, forcing
   * _clog_write_async()'s own job->msg_heap spill. */
  char long_msg[2048];
  memset(long_msg, 'A', sizeof long_msg - 1);
  long_msg[sizeof long_msg - 1] = '\0';

  log_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* The full 2047-character message must appear verbatim in the output. */
  REQUIRE_NE(strstr(buf, long_msg), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

TEST(async,
     message_exceeding_inline_buffer_marks_truncation_when_heap_alloc_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Reuses output.message_exceeding_stack_buffer_marks_truncation_when_
   * heap_alloc_fails' own _size_capped_* allocator (fails any request over
   * 4096 bytes): every allocation this job needs besides the message's own
   * heap spill (the envelope, sh->async_buf's CLOG_BUF_INITIAL == 4096
   * byte buffer, the unbounded queue's own per-message node) fits at or
   * under that cap, isolating the failure to job->msg_heap alone. */
  ccol_memmgmt_procs_t procs = {
      .malloc = _size_capped_malloc,
      .free = _size_capped_free,
      .calloc = _size_capped_calloc,
      .realloc = _size_capped_realloc,
  };

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Longer than the 256-byte inline buffer, and its +1-byte heap spill
   * request (~5000 bytes) exceeds the 4096-byte cap above. */
  char long_msg[5000];
  memset(long_msg, 'A', sizeof long_msg - 1);
  long_msg[sizeof long_msg - 1] = '\0';

  log_info(lg, "%s", long_msg);
  clog_flush(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  /* The heap spill failed, so the full message must NOT appear verbatim... */
  REQUIRE_EQ(strstr(buf, long_msg), NULL);
  /* ...and the resulting truncation must be visibly indicated, not silent. */
  REQUIRE_NE(strstr(buf, "...[truncated]"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A width/precision combination large enough that the two conversions'
 * combined output length exceeds INT_MAX makes vsnprintf() itself fail
 * before producing any content at all (see robustness.format_string_
 * failure_writes_diagnostic_not_silence's own identical rationale for why
 * the width is a runtime argument); here it is _clog_write_async()'s own
 * vsnprintf() call into job->msg_inline that must fail this way, not
 * _clog_write_sync()'s. */
TEST(async, format_string_failure_writes_diagnostic_not_silence) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  int huge_width = 2000000000;
  _clog_write(lg, CLOG_INFO, __FILE__, __LINE__, __func__, false, "%*d%*d",
              huge_width, 1, huge_width, 1);
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);

  /* Must not be a silently dropped, empty record: a well-formed placeholder
   * line naming the real cause must be written instead. */
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "ts="), NULL);
  REQUIRE_NE(strstr(buf, "log message formatting failed"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A field mutated/removed between submission and flush must still show its
 * value AS OF SUBMISSION TIME in the emitted record; proving fields are
 * genuinely snapshotted at submission, not read live by the writer thread. */
TEST(async, fields_snapshot_reflects_submission_time_not_flush_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Long interval and large buffer: nothing auto-flushes before this test's
   * own explicit clog_flush() call below, giving a wide window to mutate
   * the field before the writer thread ever builds the record. */
  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_ms = 60000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_set_field(lg, "stage", "before");
  log_info(lg, "field snapshot check");
  clog_set_field(lg, "stage", "after");
  clog_remove_field(lg, "stage");

  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "stage=before"), NULL);
  REQUIRE_EQ(strstr(buf, "stage=after"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* A backtrace requested from a spawned thread with a uniquely-named function
 * must show that thread's own call stack, captured on the calling thread at
 * submission time; never the writer thread's own, unrelated stack. */
static clog g_bt_test_lg;
static void *_clog_bt_uniquely_named_worker_fn(void *arg) {
  (void)arg;
  log_error(g_bt_test_lg, "backtrace from worker thread");
  return NULL;
}

TEST(async, backtrace_captured_on_calling_thread_not_writer_thread) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  g_bt_test_lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(g_bt_test_lg, CLOG_INVALID);

  pthread_t th;
  pthread_create(&th, NULL, _clog_bt_uniquely_named_worker_fn, NULL);
  pthread_join(th, NULL);

  clog_flush(g_bt_test_lg);

  char buf[8192];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "backtrace from worker thread"), NULL);
  REQUIRE_NE(strstr(buf, "_clog_bt_uniquely_named_worker_fn"), NULL);
  REQUIRE_EQ(strstr(buf, "_clog_writer_thread_main"), NULL);

  clog_close(g_bt_test_lg);
  cleanup_dir(dir, "app.log");
}

/* A real bug (fixed): _emit_backtrace_syslog_lines() reset its scratch
 * buffer before building each frame's line but never reset it again after
 * writing the LAST one. For the async writer thread, that scratch buffer is
 * sh->async_buf itself; the same buffer _writer_flush_now() consults via
 * "sh->async_buf.len > 0" to decide whether there is unflushed data
 * pending. Left non-empty after a with_backtrace job, the next flush
 * trigger that isn't itself another CLOG_FMT_SYSLOG job silently re-wrote
 * the last frame's already-transmitted bytes to the fd a second time. A
 * long flush_interval_ms makes clog_close()'s own shutdown-sentinel-driven
 * flush the ONLY flush this job can ever see before drain_pipe() reads the
 * pipe, so this exercises that trigger deterministically rather than racing
 * a real-time idle timeout. */
TEST(async, syslog_backtrace_not_duplicated_on_shutdown_drain) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_ms = 60000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  log_error(lg, "with backtrace via async");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  /* Split into individual syslog messages (each starts with "<11>1 ") and
   * confirm none of them repeats verbatim; the exact signature of the
   * fixed bug, where the last backtrace frame's own line was silently
   * re-sent immediately afterward by the shutdown drain. */
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

/* Same fixed bug, exercised via _emit_backtrace_syslog_lines()'s OTHER
 * leftover-leaving return path: the single "#error backtrace unavailable"
 * marker record (backtrace capture forced to fail) must appear exactly
 * once, not be silently re-sent by clog_close()'s own shutdown drain. */
TEST(async,
     syslog_backtrace_capture_failure_marker_not_duplicated_on_shutdown_drain) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_ms = 60000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_ERROR, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_backtrace_capture_failure(true);
  log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

  char buf[8192];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  int occurrences = 0;
  const char *p = buf;
  while ((p = strstr(p, "#error backtrace unavailable")) != NULL) {
    occurrences++;
    p++;
  }
  REQUIRE_EQ(occurrences, 1);
}

/* Regression test for a real bug (fixed): _emit_backtrace_syslog_lines() used
 * to build each backtrace continuation line's own TIMESTAMP field from a
 * fresh gettimeofday() call made at WRITE time, instead of the SAME
 * submission-time timestamp (job->ts) already used for the primary record it
 * continues; so under async logging, where the writer thread's own
 * processing of a job happens on a different thread (and potentially well
 * after) the original submission, a record's TIMESTAMP field and its own
 * backtrace frames' TIMESTAMP fields could silently disagree. Every syslog
 * message produced for one log_error() call (the primary record and every
 * one of its backtrace frames) must carry the exact same, byte-identical
 * TIMESTAMP field. */
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

  log_error(lg, "timestamp correlation check");

  char buf[16384];
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
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

/* A real bug (fixed): the writer thread's own CLOG_FMT_SYSLOG branch used to
 * unconditionally _buf_reset() the shared async batch buffer before building
 * its own record, rather than flushing it first. clog_set_format() never
 * touches that buffer, and a job's own render format is read live from the
 * shared target (never captured at submission time), so a message logged
 * under a batching format (CLOG_FMT_LOGFMT/CLOG_FMT_JSON) that was already
 * built into the still-unflushed batch buffer by the time the format was
 * switched to CLOG_FMT_SYSLOG was silently discarded (never written to the
 * fd at all) the instant the next (syslog) job's record was built. A
 * generous flush_interval_ms keeps the first message from being flushed on
 * its own before the switch; the short sleep after it gives the writer
 * thread time to have already dequeued and built it into the batch buffer,
 * reproducing the exact window the bug depended on. Both messages must
 * survive regardless of exactly how that race resolves. */
TEST(async, format_switch_to_syslog_does_not_drop_buffered_batch) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog_async_cfg_t cfg = {.flush_interval_ms = 60000};
  clog lg = clog_open_fd_mp(pipefd[1], CLOG_INFO, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "pre-switch logfmt message");
  usleep(50000); /* let the writer thread build this into the batch buffer */

  clog_set_format(lg, CLOG_FMT_SYSLOG);
  log_info(lg, "post-switch syslog message");

  char buf[8192];
  size_t n = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
  REQUIRE_GT(n, (size_t)0);

  REQUIRE_NE(strstr(buf, "pre-switch logfmt message"), NULL);
  REQUIRE_NE(strstr(buf, "post-switch syslog message"), NULL);
}

/* CLOG_FATAL must drain everything already queued before writing (and
 * exiting after) the fatal record itself; otherwise messages logged
 * moments before a crash, still sitting unflushed, would be silently lost. */
TEST(async, fatal_drains_queue_before_writing_and_terminating) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  int dn = open("/dev/null", O_WRONLY);
  pid_t pid = fork();
  if (pid == 0) {
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
    }
    /* Long interval and large buffer: none of the non-fatal messages below
     * can have been auto-flushed by the timer or the size threshold before
     * log_fatal() runs a few lines later. */
    clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                            .flush_interval_ms = 60000};
    clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
    if (lg == CLOG_INVALID) _exit(2);
    log_info(lg, "queued message one");
    log_info(lg, "queued message two");
    log_fatal(lg, "the fatal record itself");
    _exit(0); /* unreachable */
  }
  REQUIRE_NE(pid, -1);
  if (dn >= 0) close(dn);

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
    REQUIRE_TRUE(false);
  }
  REQUIRE_FALSE(WIFSIGNALED(status));

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  char *p1 = strstr(buf, "queued message one");
  char *p2 = strstr(buf, "queued message two");
  char *pf = strstr(buf, "the fatal record itself");
  REQUIRE_NE(p1, NULL);
  REQUIRE_NE(p2, NULL);
  REQUIRE_NE(pf, NULL);
  /* Chronological order on disk: both queued messages, then the fatal one.
   * Compared as byte offsets (not raw pointers) since tau's own _Generic
   * printer treats char* as a C string, which would print confusingly (the
   * rest of buf from that point on) in a failure message otherwise. */
  REQUIRE_LT((long)(p1 - buf), (long)(p2 - buf));
  REQUIRE_LT((long)(p2 - buf), (long)(pf - buf));

  cleanup_dir(dir, "app.log");
}

/* An idle async logger (nothing logged after its first message) must NEVER
 * rotate on its own, no matter how many multiples of the interval elapse
 * with nothing further written; exactly mirroring the synchronous write
 * path's own documented "time rotation triggers on the first WRITE after
 * the interval elapses" contract, not "rotates automatically once the
 * interval elapses regardless of whether anything new is logged". The
 * writer thread's own periodic flush-interval wakeups (distinct from the
 * rotation interval) must never fire a rotation on their own account either,
 * since _writer_flush_now()'s whole rotation-and-write block is gated on
 * async_buf actually holding something to write. Once a NEW message finally
 * is logged after the interval has elapsed, exactly one rotation must
 * occur (not one per elapsed interval, and not zero). */
TEST(async, idle_logger_does_not_rotate_until_next_write_after_interval) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* rotation_interval_secs must stay comfortably above the tolerance this
   * test itself grants the "first message" flush below (2000ms): a
   * rotation_interval_secs of 1 (as this test originally used) is TIGHTER
   * than that self-declared 2000ms worst-case landing time, so a flush that
   * merely lands anywhere between 1000ms and 2000ms after construction
   * (within the test's own stated tolerance, and genuinely reachable under
   * make memtest's own valgrind instrumentation overhead, reproduced this
   * way directly) makes _clog_time_rotate_if_due() correctly (per this
   * library's own documented "next write after the interval elapses"
   * contract) rotate as part of THAT flush, before the idle-wait phase
   * below even starts, spuriously failing the "must not have rotated yet"
   * check a few lines down. 3 seconds leaves a wide margin above that same
   * 2000ms tolerance. */
  clog_rotation_cfg_t rcfg = {
      .time_rotation_enabled = true,
      .rotation_interval_secs = 3,
  };
  clog_async_cfg_t acfg = {.flush_interval_ms = 50};
  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "first message");
  REQUIRE_TRUE(_poll_for_substring(path, "first message", 2000));

  /* Idle well past the rotation interval (which the writer thread's own
   * 50ms flush-interval wakeups keep waking up through), with nothing
   * further logged: must not rotate on its own. */
  usleep(4000000);
  REQUIRE_EQ(count_files_with_prefix(dir, "app.log."), 0);

  /* The next write, now that the interval has elapsed, must trigger exactly
   * one rotation. */
  log_info(lg, "second message");
  clog_close(lg);

  REQUIRE_EQ(count_files_with_prefix(dir, "app.log."), 1);

  cleanup_dir(dir, "app.log");
}

TEST(async, flush_drains_a_slow_to_self_flush_setup) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {.flush_buffer_size = 1 << 20,
                          .flush_interval_ms = 60000};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "needs an explicit flush");

  /* Without clog_flush(), this message would not appear for a full minute
   * (flush_interval_ms=60000) or a very large accumulated batch
   * (flush_buffer_size=1MiB); neither of which this test waits for. */
  clog_flush(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "needs an explicit flush"), NULL);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* An allocator whose malloc() can be made to fail on demand, used to force
 * the unbounded queue's own internal node allocation to fail inside
 * dynmq_send_zc(), pinning clog_flush()'s own "return promptly instead of
 * hanging when the enqueue itself fails" path (see _clog_flush_pinned()). */
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
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg_t cfg = {0}; /* unbounded queue: its own send-side node
      allocation is the thing this test forces to fail */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  atomic_store(&g_flush_oom_fail_malloc, true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return promptly, not hang */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  atomic_store(&g_flush_oom_fail_malloc, false);
  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* Regression test for a real bug: _clog_flush_pinned() called mutex_init()/
 * cond_var_init() on its own stack-local synchronization object without
 * checking either return value, then unconditionally proceeded to
 * mutex_lock()/cond_var_wait() on it. A failed init (impossible to trigger
 * for real against glibc's own default-attribute implementation, but not
 * impossible per POSIX, e.g. under ENOMEM) would have meant operating on a
 * not-fully-initialized mutex (undefined behavior, not a graceful
 * degradation), reachable from clog_flush() and, worse, from _clog_write()'s
 * own FATAL path. Fixed by checking both return values and failing this one
 * call open (nothing enqueued, no wait attempted) exactly like the existing,
 * already-handled "enqueue itself failed" case just above. */
TEST(async, flush_returns_promptly_when_mutex_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "before forced mutex_init failure");

  clog_test_force_flush_mutex_init_failure(true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return promptly, not hang or crash */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  /* The forced failure must have auto-disarmed; this logger must still be
   * fully usable afterward, proving the failed init left no corrupted state
   * or leaked resource behind. */
  log_info(lg, "after forced mutex_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "before forced mutex_init failure"), NULL);
  REQUIRE_NE(strstr(buf, "after forced mutex_init failure"), NULL);

  cleanup_dir(dir, "app.log");
}

/* Same bug as flush_returns_promptly_when_mutex_init_fails above, exercised
 * via the OTHER init call: cond_var_init() failing after mutex_init() already
 * succeeded must release that already-initialized mutex rather than leaking
 * it, and must not attempt to wait on the never-initialized condition
 * variable. */
TEST(async, flush_returns_promptly_when_condvar_init_fails) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {0};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "before forced condvar_init failure");

  clog_test_force_flush_condvar_init_failure(true);

  struct timeval t0, t1;
  gettimeofday(&t0, NULL);
  clog_flush(lg); /* must return promptly, not hang or crash */
  gettimeofday(&t1, NULL);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_usec - t0.tv_usec) / 1000L;
  REQUIRE_LT(elapsed_ms, (long)2000);

  log_info(lg, "after forced condvar_init failure");
  clog_flush(lg);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
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

/* Regression test for a real bug: _shared_async_teardown() sent the writer
 * thread's shutdown sentinel without checking whether the send itself
 * succeeded, then unconditionally joined the writer thread. For the default
 * unbounded queue, that send (dynmq_send_zc()) can genuinely fail under
 * memory pressure (its own internal node allocation failing, exactly the
 * condition a caller may be closing loggers in response to), silently
 * leaving the writer thread waiting forever for a sentinel that would now
 * never arrive, hanging clog_close() permanently. Reproduced directly
 * against the pre-fix code with a standalone repro plus a gdb backtrace
 * (the closing thread parked in pthread_join inside _shared_async_teardown,
 * the writer thread parked in dynmq_timed_recv_zc waiting for a sentinel
 * that would never come) before being fixed by retrying the sentinel send
 * with a bounded backoff instead of giving up after one attempt. */
TEST(async, close_recovers_from_transient_oom_instead_of_hanging) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _flush_oom_malloc,
      .free = _flush_oom_free,
      .calloc = _flush_oom_calloc,
      .realloc = _flush_oom_realloc,
  };

  clog_async_cfg_t cfg = {0}; /* unbounded queue: its own shutdown-sentinel
      node allocation is what this test forces to fail, then recover */
  g_teardown_retry_lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, &procs);
  REQUIRE_NE(g_teardown_retry_lg, CLOG_INVALID);

  log_info(g_teardown_retry_lg, "before oom");

  atomic_store(&g_flush_oom_fail_malloc, true);
  atomic_store(&g_teardown_retry_close_done, false);

  pthread_t th;
  REQUIRE_EQ(pthread_create(&th, NULL, _teardown_retry_closer, NULL), 0);

  /* Simulate a transient OOM window: every allocation fails for a while,
   * then the allocator recovers, exactly like a real transient allocation
   * failure clearing up. If the underlying bug were still present, close()
   * would already be unrecoverably hung by the time the very first sentinel
   * send attempt failed, so clearing the flag afterward would not help. */
  usleep(300000);
  atomic_store(&g_flush_oom_fail_malloc, false);

  /* Bounded wait, not a blocking pthread_join(): a regression of the retry
   * fix would hang this thread forever, and this test must fail visibly
   * rather than hang the whole suite. */
  bool done = false;
  for (int waited_ms = 0; waited_ms < 10000; waited_ms += 20) {
    if (atomic_load(&g_teardown_retry_close_done)) {
      done = true;
      break;
    }
    usleep(20000);
  }
  REQUIRE_TRUE(done); /* close() must eventually return, not hang forever */
  pthread_join(th, NULL);

  cleanup_dir(dir, "app.log");
}

/* An allocator that fails exactly its Nth malloc/calloc/realloc call
 * (counted together, matching real allocation order), used to land a
 * forced failure precisely on dynmq_send_zc()'s own internal node
 * allocation inside _clog_write_async(); distinct from
 * flush_returns_promptly_when_enqueue_fails' own allocator above, which
 * fails every allocation unconditionally rather than one numbered call, and
 * is therefore unsuitable for reaching this specific call site without also
 * failing the envelope/field-snapshot allocations that must be allowed to
 * succeed first. */
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

/* Regression test for a real bug: _clog_write_async()'s own enqueue-failure
 * fallback (used when dynmq_send_zc()/circq_send_zc() itself fails, e.g. the
 * unbounded queue's own node allocation failing under memory pressure)
 * wrote the record directly to sh->fd but never ran either of the two
 * rotation checks every other write path in this file applies, silently
 * defeating max_file_size for exactly the record that hit this path; and,
 * since that record never touched sh->async_buf, not even a later
 * clog_flush() would notice, since _writer_flush_now() only runs its own
 * rotation checks when async_buf has content. */
TEST(async, enqueue_failure_fallback_still_rotates_on_size) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg_t rcfg = {0};
  rcfg.size_rotation_enabled = true;
  rcfg.max_file_size = 10; /* tiny: any real record crosses it */

  clog_async_cfg_t acfg = {0}; /* unbounded queue */

  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* One ordinary write to confirm rotation genuinely works end to end for
   * this setup, and to leave bytes_written freshly reset to 0 by _rotate(). */
  log_info(lg, "seed");
  clog_flush(lg);

  int rotated_before = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ(rotated_before, 1);

  /* Fail exactly the 2nd allocation of the next log_info() call: with no
   * fields set and a short message, allocation #1 is the envelope calloc,
   * and #2 is dynmq_send_zc()'s own internal node allocation;
   * landing this failure squarely on the enqueue call itself, not on
   * anything upstream of it that would otherwise route through the
   * ordinary, already-correct _clog_write_sync() fallback instead.
   * (_snapshot_fields()'s own ccol_growbuf_init() is skipped entirely for a
   * zero-field logger like this one, so it never appears in this count.) */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  log_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  /* The record must have been written synchronously (this fallback path
   * never touches the queue), and it must have triggered rotation right
   * then and there; no clog_flush() needed. */
  int rotated_after = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated_after, rotated_before);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* Sibling of the test above, covering time-based rather than size-based
 * rotation: the same enqueue-failure fallback path must also honor
 * rotation_interval_secs, not just max_file_size. */
TEST(async, enqueue_failure_fallback_still_rotates_on_time) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_rotation_cfg_t rcfg = {0};
  rcfg.time_rotation_enabled = true;
  rcfg.rotation_interval_secs = 1;

  clog_async_cfg_t acfg = {0};

  clog lg = clog_open_file_mp(path, CLOG_INFO, &rcfg, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "seed");
  clog_flush(lg);
  int rotated_before = count_files_with_prefix(dir, "app.log.");
  REQUIRE_EQ(rotated_before, 0);

  usleep(1100 * 1000); /* let rotation_interval_secs elapse */

  /* See enqueue_failure_fallback_still_rotates_on_size's own comment: with no
   * fields set, allocation #1 is the envelope calloc and #2 is
   * dynmq_send_zc()'s own internal node allocation (the enqueue call
   * itself); _snapshot_fields()'s ccol_growbuf_init() is skipped entirely
   * for a zero-field logger, so it is never part of this count. */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  log_info(lg, "x");
  atomic_store(&g_enqueue_fail_at, -1);

  int rotated_after = count_files_with_prefix(dir, "app.log.");
  REQUIRE_GT(rotated_after, rotated_before);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* Regression test for a real bug: _clog_write_async()'s own enqueue-failure
 * fallback wrote its own record directly to sh->fd with no regard for
 * whatever was already sitting, successfully enqueued but not yet drained by
 * the writer thread, ahead of it in the SAME queue; letting a message that
 * failed to enqueue win the race for lg->shared->mutex and land on disk
 * BEFORE an earlier, already-submitted message the writer thread simply
 * hadn't gotten around to processing yet, silently reordering output
 * relative to submission order. flush_buffer_size/flush_interval_ms are both
 * set far out of reach so nothing OTHER than this fix's own drain-before-
 * direct-write (_clog_flush_pinned(), called before the fallback write) could
 * ever put "message A" on disk this early; before the fix, "message A" would
 * still be sitting unflushed in sh->async_buf at this point (only
 * clog_close()'s own final drain, never reached in this test, would have
 * written it), so "message B" would appear in the file FIRST, or "message A"
 * would be entirely absent yet. */
TEST(async,
     enqueue_failure_direct_write_does_not_reorder_already_queued_message) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg_t acfg = {.flush_buffer_size = 1 << 20,
                           .flush_interval_ms = 60000}; /* unbounded queue */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  log_info(lg, "message A (already queued)");

  /* Fail exactly the enqueue call for the very next log_info(): allocation
   * #1 is the envelope calloc and #2 is dynmq_send_zc()'s own internal node
   * allocation; the same targeting already established and explained by
   * enqueue_failure_fallback_still_rotates_on_size above. */
  atomic_store(&g_enqueue_fail_call_count, 0);
  atomic_store(&g_enqueue_fail_at, 2);
  log_info(lg, "message B (enqueue fails, written directly)");
  atomic_store(&g_enqueue_fail_at, -1);

  /* No explicit clog_flush() here at all: by the time log_info() for
   * "message B" has returned, both messages must already be on disk, in
   * submission order, purely as a consequence of the fallback path's own
   * drain-before-direct-write. */
  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  char *pos_a = strstr(buf, "message A");
  char *pos_b = strstr(buf, "message B");
  REQUIRE_NE(pos_a, NULL);
  REQUIRE_NE(pos_b, NULL);
  REQUIRE_TRUE(pos_a < pos_b);

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* Regression test for a real double-free: _snapshot_fields()'s own
 * out_pool->oom branch used to destroy job->field_pool itself
 * (ccol_growbuf_destroy() frees out_pool->buf without ever nulling the
 * pointer afterward), even though _clog_async_job_release() unconditionally
 * destroys job->field_pool again once it is done with the job; freeing the
 * same heap block twice the moment a field's key/value data was large enough
 * to force the field pool's internal growbuf to grow past its initial
 * capacity and that growth's own realloc() failed. A field long enough to
 * guarantee at least one growbuf_grow() call is set once up front; an
 * allocator that fails exactly one numbered malloc/calloc/realloc call is
 * then swept across enough calls to land on that growth's realloc() at least
 * once. A double free here either aborts the process outright (glibc's own
 * heap consistency checks) or is caught by make memtest (valgrind); this
 * test's own job is simply to survive the sweep without crashing. */
TEST(async, field_snapshot_growbuf_oom_does_not_double_free) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  ccol_memmgmt_procs_t procs = {
      .malloc = _enqueue_fail_malloc,
      .free = _enqueue_fail_free,
      .calloc = _enqueue_fail_calloc,
      .realloc = _enqueue_fail_realloc,
  };

  clog_async_cfg_t acfg = {0}; /* unbounded queue */
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, &procs);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A value long enough that appending "key\0value\0" to the field pool's
   * 256-byte initial growbuf capacity always requires at least one
   * growbuf_grow() call (and therefore one _mem_realloc() call), regardless
   * of exactly where else in this call an allocation happens to land. */
  char big_value[400];
  memset(big_value, 'v', sizeof big_value - 1);
  big_value[sizeof big_value - 1] = '\0';
  clog_set_field(lg, "big", big_value);

  for (int fail_at = 1; fail_at <= 20; fail_at++) {
    atomic_store(&g_enqueue_fail_call_count, 0);
    atomic_store(&g_enqueue_fail_at, fail_at);
    log_info(lg, "msg %d", fail_at);
    atomic_store(&g_enqueue_fail_at, -1);
    clog_flush(lg); /* drain this iteration's job before arming the next
        fault, so the writer thread's own, unrelated allocations never fall
        inside the next iteration's armed window */
  }

  clog_close(lg);
  cleanup_dir(dir, "app.log");
}

/* Regression test for a real bug: the async writer thread's fallback
 * placeholder (built whenever a queued job's own record is too large, or
 * hits a transient allocation failure) used to append itself into
 * sh->async_buf (the shared aggregation buffer batching jobs from every
 * handle sharing this target) via a sequence of UNCHECKED appends. If that
 * buffer was already sitting close enough to its own growth ceiling that
 * even the small, fixed-size fallback could not fit (reachable whenever
 * flush_buffer_size is configured close to that ceiling, since nothing then
 * proactively flushes the buffer before a job's own record building runs
 * into it directly), the fallback itself silently truncated mid-record
 * instead of ever being written; contradicting this library's own
 * documented "the record is not truncated or emitted malformed" guarantee.
 * The fix flushes whatever is already safely buffered and retries against a
 * freshly emptied buffer, which always has room. async_buf's own growth
 * ceiling follows the configured flush_buffer_size whenever that is larger
 * than the library's internal 16 MiB single-record default (see
 * _buf_raise_cap_limit()), so this test sizes its messages against the
 * actually-configured flush_buffer_size below, not a hardcoded constant. */
TEST(async,
     fallback_record_still_well_formed_when_batch_buffer_is_nearly_full) {
  /* Calibrate this format's fixed per-record overhead (everything on a
   * logfmt line except the message content itself) with a plain synchronous
   * logger and a short, otherwise unremarkable message that needs no
   * quoting; letting the real, multi-megabyte messages below be sized
   * precisely without this test hardcoding any internal layout beyond the
   * documented 16 MiB cap itself. */
  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof calib_dir), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof calib_path, "%s/calib.log", calib_dir);

  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "CALIBRATION_MESSAGE_CONTENT";
  log_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);

  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof calib_buf);
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* flush_buffer_size configured well above the library's own internal 16
   * MiB single-record default, so async_buf's own growth ceiling is raised
   * to match it (see _buf_raise_cap_limit()) and is never proactively
   * flushed by size before a job's own record building can run up against
   * that (now larger) ceiling directly; exactly the scenario that makes
   * the underlying recovery bug reachable. */
  const size_t write_buf_cap = 20UL * 1024 * 1024; /* == acfg.flush_buffer_size
      below, and therefore genuinely async_buf's own cap_limit */
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

  /* Both queued before any flush, so they land in async_buf back to back on
   * the writer thread: the first brings async_buf to within `room` bytes of
   * the cap (a legitimate, well-formed record of its own), then the second
   * one's own record (and, before the fix, even ITS fallback) cannot
   * fit as-is. */
  log_info(lg, "%s", first_msg);
  log_info(lg, "%s", huge_msg);
  clog_flush(lg);
  clog_close(lg);

  free(first_msg);
  free(huge_msg);

  size_t read_cap = write_buf_cap + (2UL * 1024 * 1024);
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  size_t len = read_file(path, buf, read_cap);
  REQUIRE_GT(len, (size_t)0);

  /* Exactly two well-formed lines: the first (huge but individually
   * fitting) message, then the second message's fallback placeholder;
   * the two must never desynchronize into a single, merged/truncated line. */
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
  /* Well-formed: a properly closed quoted msg value followed by a real
   * trailing newline, not a dangling, mid-field fragment silently glued to
   * whatever a corrupted implementation would have written next. */
  REQUIRE_EQ(*(line_end - 1), '"');

  free(buf);
  cleanup_dir(dir, "app.log");
}

/*
 * A real, distinct defect from the one the test above guards: a genuinely
 * ordinary, small message must never be silently replaced by the "too large
 * to emit" fallback placeholder just because unrelated content already
 * sitting in the shared async batch buffer left too little room for its own
 * real record while still leaving enough room for the much smaller fallback
 * note. The test above only exercises the case where NEITHER the real
 * record NOR the fallback fits (which already correctly flushes and
 * retries); this one exercises the narrower, previously-unhandled case where
 * the fallback alone fits, so the writer thread never even attempted a
 * retry, and a message that would have fit fine on its own was lost and
 * misreported as oversized.
 */
TEST(async,
     ordinary_small_message_not_misreported_as_too_large_by_batch_neighbor) {
  const char *small_msg = "reachable small message";

  /* Calibrate this test's own fixed per-record overhead (everything on a
   * logfmt line besides the message content itself; ts/level/proc/src/
   * func=), via a plain synchronous logger sharing this test's own
   * __FILE__/__func__ (tau generates one function per TEST(), so this test's
   * own generated function name, whatever length it happens to be, is
   * exactly what every log_info() call below also pays for). Without this,
   * a filler record sized purely off write_buf_cap could itself exceed
   * CLOG_BUF_MAX in an empty buffer once this per-record overhead is added
   * back in, polluting the sweep below with the (correctly-handled) genuine
   * oversized-record case instead of the narrow one this test targets. */
  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof calib_dir), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof calib_path, "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  log_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof calib_buf);
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* flush_buffer_size deliberately configured ABOVE clogger's internal 16
   * MiB write-buffer cap, so async_buf is never proactively flushed by size
   * before a job's own record building runs into whatever room a prior job
   * already sharing the batch left behind. */
  clog_async_cfg_t acfg = {.flush_buffer_size = 20UL * 1024 * 1024};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &acfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  const size_t write_buf_cap = 16UL * 1024 * 1024; /* CLOG_BUF_MAX */
  char *filler = malloc(write_buf_cap);
  REQUIRE_NE(filler, NULL);
  memset(filler, 'B', write_buf_cap);

  /* Sweep a wide range of leftover headroom values (bytes remaining in the
   * batch buffer after the filler record, i.e. `room - fixed_overhead`)
   * rather than trying to precisely calibrate the fallback placeholder's own
   * size: at least one value in this range is guaranteed to land strictly
   * between the fallback placeholder's own size and the small message's real
   * record size, which is exactly the previously-mishandled window (the
   * fallback fits, the real record doesn't, and nothing before the fix ever
   * retried against a freshly emptied buffer). Starting the sweep at
   * fixed_overhead keeps every filler record, on its own, safely within
   * CLOG_BUF_MAX (so it never itself needs the fallback, genuinely or
   * otherwise). clog_flush() between iterations drains each pair to disk and
   * resets the batch buffer to empty, so every iteration starts from the
   * same, deterministic "buffer just grew to CLOG_BUF_MAX and is otherwise
   * empty" state regardless of what an earlier iteration did.
   *
   * The underlying file is truncated back to empty after each iteration's
   * own pair is checked (via a second fd on the same path; clog itself
   * always opens for append, with no truncate of its own): the destination
   * fd stays open and O_APPEND throughout, so a write immediately following
   * an external truncate simply lands at the new, now-zero end of file,
   * exactly as if this were a fresh logger for that one iteration. Without
   * this, the file would accumulate roughly (number of iterations) times
   * write_buf_cap bytes on disk, and this test's own read-back buffer would
   * need to grow to match just to see the last iteration's own pair at all. */
  size_t read_cap = write_buf_cap * 2;
  char *buf = malloc(read_cap);
  REQUIRE_NE(buf, NULL);
  int pairs_logged = 0;
  for (size_t room = fixed_overhead + 50; room <= fixed_overhead + 1200;
       room += 25) {
    filler[write_buf_cap - room] = '\0';
    log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B'; /* restore for the next iteration */
    log_info(lg, "%s", small_msg);
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);
    /* This one iteration's ordinary small message must never be replaced by
     * the misleading "too large to emit" placeholder, or be silently
     * dropped altogether. */
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
 * Regression test for a real bug: _emit_backtrace_lines() (the logfmt
 * backtrace emitter) used to be void-returning, with every failure
 * (including the case where not one single frame's own line fit in whatever
 * room was left, not just an individual oversized frame) silently
 * discarded; _clog_build_record() then unconditionally reported the record
 * as fully, successfully built regardless. Reachable whenever the batch
 * buffer a with_backtrace=true job's own record lands in (sh->async_buf,
 * here left with only a few hundred to a couple thousand bytes of headroom
 * by an earlier, unrelated filler record already sharing the same batch) has
 * enough room for the record's own primary content but not enough for any
 * individual backtrace frame line: a real log_error()/log_alert()/
 * log_fatal() call could reach disk with its message intact but its entire
 * requested backtrace silently missing; indistinguishable from a call that
 * never requested one at all, contradicting this file's own "a reader must
 * always be able to tell 'backtrace omitted' apart from 'never requested'"
 * guarantee (already correctly honored, and tested, for capture failure,
 * per logfmt_backtrace_capture_failure_emits_marker_line above, and for
 * JSON/syslog's own equivalent scenarios). The fix makes
 * _emit_backtrace_lines() fall back to the same small, fixed-size
 * "unavailable" marker already used when capture itself fails, whenever not
 * one single frame fit.
 *
 * Mirrors ordinary_small_message_not_misreported_as_too_large_by_batch_
 * neighbor's own room-sweep exactly (same calibration, same range): the
 * precise byte offset this failure mode needs depends on backtrace frame
 * text (full binary/library paths from backtrace_symbols()) this test has no
 * way to predict in advance, so at least one value in this range is expected
 * to land inside the previously-mishandled window regardless of environment.
 */
TEST(async, backtrace_never_silently_lost_when_batch_buffer_is_nearly_full) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  const char *small_msg = "reachable small message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof calib_dir), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof calib_path, "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  log_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof calib_buf);
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B'; /* restore for the next iteration */
    log_error(lg, "%s", small_msg);     /* with_backtrace = true */
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    /* Never neither: this record's own trailing content must always be
     * either at least one real backtrace frame line or the fixed-size
     * "unavailable" marker; never a silent absence of both. */
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
 * Regression test for a real bug: _emit_backtrace_json() used to close an
 * empty "bt":[] array whenever a record's own header/fields/msg left just
 * enough room in the batch buffer to open and close the "bt" array but not
 * enough for even one real frame's own text to fit; byte-for-byte
 * indistinguishable from a genuinely shallow backtrace with no frames beyond
 * the initial two, silently defeating the whole reason "bt_error" exists
 * (see this file's own "In the rare case that the backtrace itself cannot be
 * embedded ... the record is still closed normally with a bt_error key"
 * contract in README.md). Because closing an empty array counted as a fully
 * successful build, the writer thread's own used_fallback-driven retry (see
 * _clog_writer_thread_main()'s logfmt/JSON branch) never even noticed
 * anything had been lost, unlike the LOGFMT sibling test directly above this
 * one, which never had this gap since _emit_backtrace_lines() already
 * detected "not one frame fit" correctly.
 *
 * Mirrors backtrace_never_silently_lost_when_batch_buffer_is_nearly_full's
 * own approach (a calibrated filler message that pushes async_buf to within
 * a swept range of `room` bytes of CLOG_BUF_MAX before an error-with-
 * backtrace record is logged) but for CLOG_FMT_JSON specifically, and adds an
 * explicit check that "bt":[] never stands in for a genuinely non-empty,
 * merely space-starved backtrace.
 */
TEST(async, json_backtrace_never_silently_becomes_an_empty_array) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  const char *small_msg = "reachable small json message with backtrace";

  char calib_dir[256];
  REQUIRE_EQ(make_tmpdir(calib_dir, sizeof calib_dir), 0);
  char calib_path[512];
  snprintf(calib_path, sizeof calib_path, "%s/calib.log", calib_dir);
  clog calib_lg = clog_open_file_mp(calib_path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(calib_lg, CLOG_INVALID);
  clog_set_format(calib_lg, CLOG_FMT_JSON);
  const char *calib_msg = "FILLER_OVERHEAD_CALIBRATION_MESSAGE";
  log_info(calib_lg, "%s", calib_msg);
  clog_close(calib_lg);
  char calib_buf[1024];
  size_t calib_len = read_file(calib_path, calib_buf, sizeof calib_buf);
  REQUIRE_GT(calib_len, strlen(calib_msg));
  size_t fixed_overhead = calib_len - strlen(calib_msg);
  cleanup_dir(calib_dir, "calib.log");

  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "%s", filler);
    filler[write_buf_cap - room] = 'B'; /* restore for the next iteration */
    log_error(lg, "%s", small_msg);     /* with_backtrace = true */
    clog_flush(lg);
    pairs_logged++;

    size_t len = read_file(path, buf, read_cap);
    REQUIRE_GT(len, (size_t)0);

    char *rec = strstr(buf, small_msg);
    REQUIRE_NE(rec, NULL);
    /* Never a "bt":[] standing in for a genuinely dropped, non-empty
     * backtrace: either real frame content, or the explicit bt_error
     * marker, but never an empty array silently masquerading as a shallow
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
 * Regression test for a real bug: _clog_write_unrepresentable_record()
 * (the very last resort _clog_build_record() itself falls back to when not
 * even its own small fallback placeholder fits into the target buffer)
 * used to have no notion of with_backtrace at all, silently dropping a
 * requested LOGFMT backtrace with no trace of it whatsoever. CLOG_FMT_SYSLOG
 * never had this gap (its own backtrace lines are always emitted
 * independently of this function, by its own callers, regardless of what
 * happened to the primary record); CLOG_FMT_JSON already embedded a
 * "bt_error" marker into its own fallback text via
 * _clog_build_fallback_record() whenever THAT (ordinarily-reachable)
 * fallback fit; but neither format's caller ever passed with_backtrace
 * through to THIS function, so the one case where not even the JSON/logfmt
 * fallback fits was still a fully silent backtrace loss for both.
 *
 * Reaching this function through the ordinary log_* call path requires not
 * even _clog_build_record()'s own small fallback placeholder to fit in the
 * target buffer (unreachable in practice given this library's actual
 * CLOG_BUF_INITIAL value; see that function's own doc comment), so this
 * exercises it directly via clog_test_write_unrepresentable_record(),
 * mirroring clog_test_gzip_compress_file()'s own precedent for testing a
 * similarly hard-to-reach internal helper.
 */
TEST(robustness,
     unrepresentable_record_logfmt_still_signals_missing_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR, true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  char *primary = strstr(buf, "log record dropped");
  REQUIRE_NE(primary, NULL);

  /* The fix: a "backtrace unavailable" continuation line must follow the
   * primary placeholder line, exactly like an ordinary (non-unrepresentable)
   * logfmt record whose backtrace capture failed already gets one. */
  REQUIRE_NE(strstr(primary, "#error backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

/* Regression guard for the fix above: with_backtrace == false must never
 * grow a marker line that was never requested in the first place. */
TEST(robustness, unrepresentable_record_logfmt_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_LOGFMT, CLOG_ERROR,
                                         false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);

  cleanup_dir(dir, "app.log");
}

/* The CLOG_FMT_JSON analogue of the fix above: _clog_write_unrepresentable_
 * record() must embed a "bt_error" marker into its own single JSON line
 * whenever with_backtrace is true, exactly like _clog_build_fallback_
 * record() already does for JSON's own ordinarily-reachable fallback. */
TEST(robustness, unrepresentable_record_json_embeds_bt_error_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, true);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  /* The fix: JSON's own bt_error marker must be embedded even in this
   * innermost fallback, not just in the ordinarily-reachable one
   * _clog_build_fallback_record() already handled. */
  REQUIRE_NE(strstr(buf, "\"bt_error\":\"unavailable\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* Regression guard: a JSON unrepresentable record without with_backtrace
 * must stay exactly as before (no spuriously-added bt_error marker). */
TEST(robustness, unrepresentable_record_json_no_marker_without_backtrace) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_write_unrepresentable_record(lg, CLOG_FMT_JSON, CLOG_ERROR, false);
  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  REQUIRE_NE(strstr(buf, "log record dropped"), NULL);
  REQUIRE_EQ(strstr(buf, "bt_error"), NULL);

  cleanup_dir(dir, "app.log");
}

/*
 * Regression test for a real bug: _emit_backtrace_syslog_lines()'s own
 * syms == NULL ("backtrace capture failed") branch built its small
 * "backtrace unavailable" marker record into the caller's buffer and simply
 * returned, silently writing nothing at all, if that one small append
 * itself also failed; a double failure (capture already failed, then the
 * marker's own append failed too) that, unlike almost every other
 * build-into-a-buffer call site in this file, had no allocation-free last
 * resort to fall back on. Forces both failures deterministically: real
 * backtrace capture never even runs here (this test calls the marker branch
 * directly), and clog_test_force_next_buf_ensure_failure() forces the one
 * append inside it to fail despite this library's own buffer-sizing
 * constants otherwise making that unreachable in practice.
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
  size_t len = drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);

  /* The fix: some record signalling the omitted backtrace must still reach
   * the wire, even though the ordinary (buffer-based) marker build failed. */
  REQUIRE_NE(strstr(buf, "backtrace unavailable"), NULL);
  /* Well-formed: a real syslog PRI header, and a single, newline-terminated
   * line; not a truncated fragment that could desynchronize the stream. */
  REQUIRE_EQ(buf[0], '<');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

/* The logfmt/syslog analogue of json.fatal_has_inline_bt_array's own
 * "backtrace omission must never be silent" guarantee: unlike JSON (which
 * has always carried a dedicated bt_error marker), a NULL syms used to make
 * _emit_backtrace_lines()/_emit_backtrace_syslog_lines() bare no-ops, so a
 * reader of a with_backtrace=true logfmt/syslog record had no way to tell
 * "the backtrace was omitted" apart from "no backtrace was ever requested". */
TEST(robustness, logfmt_backtrace_capture_failure_emits_marker_line) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_force_backtrace_capture_failure(true);
  log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
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
  log_error(lg, "boom");
  clog_test_force_backtrace_capture_failure(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof buf - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  REQUIRE_NE(strstr(buf, "#error backtrace unavailable"), NULL);
}

/*
 * Regression test for a real bug: _emit_backtrace_syslog_lines()'s
 * real-syms loop had no notion of "did any frame actually get written";
 * if every single frame's own append failed (each frame gets its own
 * freshly reset scratch buffer, so this is reachable only under a genuine,
 * sustained allocation failure persisting across all of them), the function
 * returned having written nothing at all: no frame, and no "unavailable"
 * marker either, unlike the syms == NULL (capture failed outright) case
 * right above, which already emits one. A real backtrace that is
 * successfully captured but entirely unwritten was indistinguishable from
 * one that was never requested. Uses
 * clog_test_force_all_syslog_backtrace_frames_failure() to force every
 * frame to fail deterministically, since genuinely reproducing a sustained
 * allocation failure across every one of this function's own small,
 * comfortably-sized per-frame appends is impractical given this library's
 * own buffer-sizing constants.
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

  /* A genuine, successfully captured backtrace (unlike the capture-failure
   * test above): real backtrace support must be present on this platform
   * and functioning normally, only every frame's own append is forced to
   * fail. */
  clog_test_force_all_syslog_backtrace_frames_failure(true);
  log_error(lg, "boom");
  clog_test_force_all_syslog_backtrace_frames_failure(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof buf - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* Never neither: some record signalling the omitted backtrace must still
   * reach the wire, exactly as when capture fails outright, and no real
   * "\t#N " frame line (which the forced failure never let through) must be
   * present instead. */
  REQUIRE_NE(strstr(buf, "#error backtrace unavailable"), NULL);
  REQUIRE_EQ(strstr(buf, "\t#0 "), NULL);
}

/*
 * Regression test for a real bug: _emit_backtrace_lines() treated a
 * genuinely successful capture that simply found no frame beyond the two
 * internal bookkeeping ones (depth <= CLOG_BT_INITIAL_FRAME) identically to
 * "every frame failed to fit", emitting the misleading "#error backtrace
 * unavailable" marker even though nothing had actually gone wrong.
 * _emit_backtrace_json() already made this exact distinction (see its own
 * array_content_is_accurate check); this exercises the fixed logfmt
 * counterpart via clog_test_force_shallow_backtrace_depth(), which reports a
 * real, non-NULL capture clamped to a shallow depth, deterministically
 * reproducing the boundary condition without depending on a genuinely
 * shallow call stack (which never happens in practice on this platform).
 */
TEST(robustness, logfmt_shallow_backtrace_capture_emits_no_marker) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  clog_test_force_shallow_backtrace_depth(true);
  log_error(lg, "boom");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);

  char buf[4096];
  size_t len = read_file(path, buf, sizeof buf);
  REQUIRE_GT(len, (size_t)0);
  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* The fix: a genuinely successful-but-shallow capture must never be
   * misreported as an unavailable one. */
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  /* Nor should it fabricate a frame line for a frame that was never
   * actually captured. */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

/* The CLOG_FMT_SYSLOG analogue of the fix above: a genuinely successful but
 * shallow capture must not produce a spurious extra "unavailable" marker
 * record on the wire either. */
TEST(robustness, syslog_shallow_backtrace_capture_emits_no_marker_record) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);

  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  clog_test_force_shallow_backtrace_depth(true);
  log_error(lg, "boom");
  clog_test_force_shallow_backtrace_depth(false);

  clog_close(lg);
  close(pipefd[1]);

  char buf[4096];
  ssize_t n = read(pipefd[0], buf, sizeof buf - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(pipefd[0]);

  REQUIRE_NE(strstr(buf, "boom"), NULL);
  /* The fix: no "unavailable" marker for a genuinely successful-but-shallow
   * capture. */
  REQUIRE_EQ(strstr(buf, "backtrace unavailable"), NULL);
  /* And no fabricated frame line either. */
  REQUIRE_EQ(strstr(buf, "\t#"), NULL);
  /* Exactly one record reached the wire (the primary message); not a
   * second, spurious marker record appended after it. */
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl + 1), '\0');
}

#define ASYNC_STRESS_WRITER_COUNT 4
#define ASYNC_STRESS_ITERATIONS 500

static void *_async_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ASYNC_STRESS_ITERATIONS; i++)
    log_info(lg, "async stress line %d", i);
  return NULL;
}

/* Async analogue of threading.concurrent_parent_and_derived_writers_no_
 * garbled_lines: several threads hammering an async-enabled logger
 * concurrently must never produce a garbled (partial/merged) line. */
TEST(threading, concurrent_async_writes_produce_no_garbled_lines) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_async_cfg_t cfg = {.flush_buffer_size = 4096, .flush_interval_ms = 20};
  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &cfg, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  pthread_t writers[ASYNC_STRESS_WRITER_COUNT];
  for (int i = 0; i < ASYNC_STRESS_WRITER_COUNT; i++)
    pthread_create(&writers[i], NULL, _async_stress_writer, &lg);
  for (int i = 0; i < ASYNC_STRESS_WRITER_COUNT; i++)
    pthread_join(writers[i], NULL);

  clog_close(lg);

  int fd = open(path, O_RDONLY);
  REQUIRE_NE(fd, -1);
  FILE *f = fdopen(fd, "r");
  REQUIRE_NE((void *)f, (void *)NULL);
  char line[512];
  int total_lines = 0;
  int bad_lines = 0;
  while (fgets(line, sizeof line, f)) {
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

/* Rotation produces a valid gzip file decompressible with gunzip. */
TEST(compression, rotated_file_is_valid_gzip) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "compression test message index=%d padding-to-force-rotation",
             i);

  clog_close(lg);

  /* At least one .gz file must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, ".gz"), 0);

  /* All rotated files should be .gz; no raw uncompressed rotated files. */
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"),
             count_files_with_prefix(dir, "app.log."));

  /* gunzip -t must pass on the first .gz file found. */
  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof gz_path), 0);
  REQUIRE_EQ(gunzip_test(gz_path), 0);

  cleanup_dir(dir, "app.log");
}

/* Decompressed content must match the log lines that were written. */
TEST(compression, decompressed_content_matches_written_log) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 400,
      .time_rotation_enabled = false,
      .max_rotated_files = 10,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Write a distinctive sentinel into the first batch so it ends up rotated. */
  log_info(
      lg,
      "sentinel_marker_abc123 index=0 pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx");
  for (int i = 1; i < 15; i++)
    log_info(
        lg,
        "sentinel_marker_abc123 index=%d pad=xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx",
        i);

  clog_close(lg);

  char gz_path[512];
  REQUIRE_EQ(find_file_with_suffix(dir, ".gz", gz_path, sizeof gz_path), 0);

  char decomp[65536];
  size_t dlen = gunzip_read(gz_path, decomp, sizeof decomp);
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

/* With compress_rotated=false the old behaviour is preserved (no .gz files). */
TEST(compression, no_compression_when_disabled) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "no-compress test message index=%d padding-padding-padding",
             i);

  clog_close(lg);

  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 0);

  cleanup_dir(dir, "app.log");
}

/* max_rotated_files cap is respected even when compression is enabled. */
TEST(compression, max_rotated_files_respected_with_compression) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_info(lg, "pruning+compress test idx=%d extra-data-to-fill-the-buffer",
             i);

  clog_close(lg);

  /*
   * _prune_rotated runs before compression, keying on the timestamp-only name.
   * After pruning at most max_rotated_files raw rotated files remain; they are
   * then compressed to .gz.  The live file ("app.log") is not counted.
   * Allow one extra in case the final rotation raced with the check.
   */
  int gz_count = count_files_with_suffix(dir, ".gz");
  REQUIRE_LE(gz_count, cfg.max_rotated_files + 1);
  REQUIRE_GT(gz_count, 0);

  cleanup_dir(dir, "app.log");
}

/* An empty log file (0 bytes at rotation) compresses to a valid gzip. */
TEST(compression, empty_file_compresses_cleanly) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Pre-create the log file with 0 bytes so we can manually trigger rotation
   * via the internal clog_rotate_now helper available in unit-test builds. */
  int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);
  close(fd);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* any write triggers rotation */
      .time_rotation_enabled = false,
      .max_rotated_files = 5,
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* A single write forces a rotation of the (nearly-empty) file. */
  log_info(lg, "trigger rotation");
  clog_close(lg);

  char gz_path[512];
  int found = find_file_with_suffix(dir, ".gz", gz_path, sizeof gz_path);
  if (found == 0) {
    /* If a .gz was produced it must be valid. */
    REQUIRE_EQ(gunzip_test(gz_path), 0);
  }
  /* If no .gz exists the file was too small to trigger rotation; that is
   * also acceptable; the test verifies there is no crash. */

  cleanup_dir(dir, "app.log");
}

/* Regression test for a real bug: _gzip_compress_file() unconditionally
 * unlink()'d its destination path on any failure, even when its own
 * gzopen() call had never run (a source file that can't even be opened
 * means the destination was never touched by this call at all). A
 * coincidentally pre-existing, unrelated file at that exact destination
 * path would therefore be silently deleted by a compression attempt that
 * never wrote a single byte to it. Exercised directly via the internal
 * helper (clog_test_gzip_compress_file), since reproducing the real
 * scenario end to end would require an external actor deleting a just-
 * rotated file in the narrow window between rename() and this call's own
 * fopen(). */
TEST(compression,
     compress_failure_before_dst_created_leaves_existing_dst_untouched) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);

  char src[512];
  snprintf(src, sizeof src, "%s/does-not-exist.log", dir);

  char dst[512];
  snprintf(dst, sizeof dst, "%s/preexisting.log.gz", dir);
  int fd = open(dst, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
  REQUIRE_NE(fd, -1);
  const char *marker = "unrelated pre-existing content";
  REQUIRE_EQ(write(fd, marker, strlen(marker)), (ssize_t)strlen(marker));
  close(fd);

  /* src does not exist, so fopen() fails before gzopen(dst, ...) is ever
   * reached; compression must report failure without touching dst at all. */
  REQUIRE_FALSE(clog_test_gzip_compress_file(src, dst));

  char buf[256];
  size_t len = read_file(dst, buf, sizeof buf);
  REQUIRE_EQ(len, strlen(marker));
  REQUIRE_EQ(memcmp(buf, marker, strlen(marker)), 0);

  unlink(dst);
  rmdir(dir);
}

/* Concurrent rotation stress test targeting the race between one rotation's
 * gzip compression (run outside shared->mutex, on purpose, so slow I/O
 * doesn't stall other writers) and a second, concurrent rotation's own
 * pruning pass: without the fix, that second rotation's prune could delete
 * the first rotation's rotated file before its compression had finished (or
 * even started) reading it, silently losing that generation's log data
 * entirely rather than merely leaving it uncompressed. Many writer threads
 * sharing one logger, a tiny max_file_size, and compress_rotated=true drive
 * many overlapping rotate+compress cycles; every .gz file that does get
 * produced must be a complete, valid gzip archive; a file destroyed
 * mid-read by a racing prune would instead fail gunzip -t or produce
 * truncated content. */
#define ROTATE_STRESS_THREAD_COUNT 6
#define ROTATE_STRESS_MSGS_PER_THREAD 150

static void *_rotate_stress_writer(void *arg) {
  clog lg = *(clog *)arg;
  for (int i = 0; i < ROTATE_STRESS_MSGS_PER_THREAD; i++)
    log_info(lg, "rotate stress padding-padding-padding-padding idx=%d", i);
  return NULL;
}

TEST(compression, concurrent_rotations_never_corrupt_or_lose_a_gz_file) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
  for (int i = 0; i < ROTATE_STRESS_THREAD_COUNT; i++)
    pthread_create(&threads[i], NULL, _rotate_stress_writer, &lg);
  for (int i = 0; i < ROTATE_STRESS_THREAD_COUNT; i++)
    pthread_join(threads[i], NULL);

  clog_close(lg);

  /* Every .gz file produced under this concurrent hammering must be a
   * complete, valid gzip archive; never truncated or missing entirely due
   * to a racing prune. */
  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  int gz_checked = 0;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof gz_path, "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    gz_checked++;
  }
  closedir(d);
  REQUIRE_GT(gz_checked, 0);

  cleanup_dir(dir, "app.log");
}

/* Deterministic regression test: _prune_rotated()'s pending-compress
 * protection must cover the compressed DESTINATION (".gz") file a
 * concurrent rotation is still writing, not merely the uncompressed SOURCE
 * it is reading from. Uses the RUNNING_UNIT_TESTS-only synchronization hooks
 * (clog_test_set_pending_compress_delay_us / clog_test_gz_dest_opened) to
 * force this exact narrow window deterministically, rather than relying on
 * real thread-scheduling luck the way the broader stress test above does. */
static void *_gzrace_first_rotation_writer(void *arg) {
  clog lg = *(clog *)arg;
  log_info(lg, "first rotation content");
  return NULL;
}

TEST(compression,
     prune_never_deletes_a_gz_file_still_being_written_by_another_rotation) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* any write triggers rotation */
      .time_rotation_enabled = false,
      .max_rotated_files = 1, /* tight quota: forces prune to try to delete
                                  something on the second rotation */
      .compress_rotated = true,
  };

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL, NULL);
  REQUIRE_NE(lg, CLOG_INVALID);

  /* Arm a long, deterministic delay: the first rotation's compress step will
   * pause right after creating its .gz destination on disk, before writing
   * any content to it. */
  clog_test_set_pending_compress_delay_us(500000); /* 500ms */

  pthread_t t;
  REQUIRE_EQ(pthread_create(&t, NULL, _gzrace_first_rotation_writer, &lg), 0);

  /* Spin-wait until the first rotation's .gz destination genuinely exists on
   * disk (rather than sleeping a fixed guess), so the second rotation
   * triggered right below is guaranteed to race a real, on-disk file. */
  while (!clog_test_gz_dest_opened()) usleep(1000);

  /* Disarm the delay so the second rotation's own compress step (triggered
   * below) does not also pause; the first rotation's own in-flight compress
   * already captured its own 500ms delay locally and is unaffected. */
  clog_test_set_pending_compress_delay_us(0);

  /* This rotation's own pruning pass runs while the first rotation's
   * compress is still mid-flight; its candidate set on disk is the first
   * rotation's uncompressed source, that same rotation's in-progress .gz
   * destination, and this rotation's own freshly rotated (not yet
   * compressed) file. */
  log_info(lg, "second rotation content");

  pthread_join(t, NULL);
  clog_close(lg);

  /* Both rotation generations' compressed output must have survived: a
   * racing prune deleting the first rotation's still-in-flight .gz file
   * would make that generation's content vanish entirely rather than merely
   * corrupt it (unlinking a still-open file only removes its directory
   * entry; the writer keeps appending to the now-nameless inode until its
   * own fd closes, at which point the data is gone for good, and the
   * now-"successfully" closed compression then also removes its own
   * uncompressed source); leaving only one .gz file instead of two. */
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 2);

  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  bool found_first = false, found_second = false;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof gz_path, "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    char content[512];
    gunzip_read(gz_path, content, sizeof content);
    if (strstr(content, "first rotation content")) found_first = true;
    if (strstr(content, "second rotation content")) found_second = true;
  }
  closedir(d);
  REQUIRE_TRUE(found_first);
  REQUIRE_TRUE(found_second);

  cleanup_dir(dir, "app.log");
}

/* Regression test for a real bug: _prune_rotated()'s own reconstruction of
 * each candidate's full path (via _path_dir()/_path_base() + readdir()'s
 * bare filename) is NOT guaranteed to reproduce the literal file_path prefix
 * _rotate() itself used to build the pending-compress list's own path/gz_path
 * strings, whenever file_path has no '/' at all: _path_dir() then returns
 * "." (a directory component that never actually appeared in file_path),
 * so the reconstructed candidate ("./app.log.<ts>...") never string-compares
 * equal to the pending entry ("app.log.<ts>..."), silently defeating the
 * in-flight-compression protection. This is otherwise identical to
 * prune_never_deletes_a_gz_file_still_being_written_by_another_rotation
 * above, just opened via a bare relative filename (with the process cwd
 * temporarily pointed at the test's own tmpdir) instead of an absolute path;
 * every existing test in this suite uses make_tmpdir()'s absolute paths,
 * which happen to round-trip correctly through _path_dir()/_path_base() and
 * so could never have caught this. */
TEST(compression,
     prune_survives_gz_race_with_a_bare_relative_filename_no_slash) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);

  char oldcwd[1024];
  REQUIRE_NE((void *)getcwd(oldcwd, sizeof oldcwd), (void *)NULL);
  REQUIRE_EQ(chdir(dir), 0);

  clog_rotation_cfg_t cfg = {
      .size_rotation_enabled = true,
      .max_file_size = 1, /* any write triggers rotation */
      .time_rotation_enabled = false,
      .max_rotated_files = 1, /* tight quota: forces prune to try to delete
                                  something on the second rotation */
      .compress_rotated = true,
  };

  /* "app.log", not "<dir>/app.log": no '/' anywhere in this path at all. */
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

  log_info(lg, "second rotation content");

  pthread_join(t, NULL);
  clog_close(lg);

  /* Every file operation this test needs is done; restore the process-wide
   * cwd before any further REQUIRE (below) could return early and leave it
   * pointed at a directory this test's own cleanup_dir() is about to
   * remove; every other test in this suite assumes an unchanged cwd. */
  REQUIRE_EQ(chdir(oldcwd), 0);

  /* Both rotation generations' compressed output must have survived, exactly
   * as in the absolute-path version of this race above. */
  REQUIRE_EQ(count_files_with_suffix(dir, ".gz"), 2);

  DIR *d = opendir(dir);
  REQUIRE_NE((void *)d, (void *)NULL);
  struct dirent *e;
  bool found_first = false, found_second = false;
  while ((e = readdir(d)) != NULL) {
    size_t nlen = strlen(e->d_name);
    if (nlen < 3 || strcmp(e->d_name + nlen - 3, ".gz") != 0) continue;
    char gz_path[1024];
    snprintf(gz_path, sizeof gz_path, "%s/%s", dir, e->d_name);
    REQUIRE_EQ(gunzip_test(gz_path), 0);
    char content[512];
    gunzip_read(gz_path, content, sizeof content);
    if (strstr(content, "first rotation content")) found_first = true;
    if (strstr(content, "second rotation content")) found_second = true;
  }
  closedir(d);
  REQUIRE_TRUE(found_first);
  REQUIRE_TRUE(found_second);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         FATAL LEVEL (PROCESS TERMINATION)                  */
/* ========================================================================== */

TEST(fatal, terminates_process) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_fatal(lg, "process must die");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);
  cleanup_dir(dir, "app.log");

  REQUIRE_TRUE(WIFEXITED(wstatus));
  REQUIRE_NE(WEXITSTATUS(wstatus), 0);
}

TEST(fatal, writes_log_before_terminating) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_fatal(lg, "fatal condition encountered");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_TRUE(WIFEXITED(wstatus));
  REQUIRE_NE(WEXITSTATUS(wstatus), 0);
  REQUIRE_NE(strstr(buf, "FATAL"), NULL);
  REQUIRE_NE(strstr(buf, "fatal condition encountered"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fatal, writes_backtrace_before_terminating) {
  if (!backtrace_capture_genuinely_available())
    return; /* see this helper's
                 own doc comment */
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_fatal(lg, "fatal with trace");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(fatal, writes_and_terminates_with_clog_off) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* log_fatal bypasses the level filter: even with CLOG_OFF the message
   * must be written and the process must terminate. */
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
    log_fatal(lg, "fatal bypasses filter");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

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
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_fatal(lg, "fatal json message");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"FATAL\""), NULL);
  REQUIRE_NE(strstr(buf, "fatal json message"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(json, fatal_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

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
    log_fatal(lg, "fatal event");
    _exit(0); /* unreachable */
  }

  int wstatus;
  waitpid(pid, &wstatus, 0);
  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "\"bt\":["), NULL);
  REQUIRE_EQ(strstr(buf, "\t#"),
             NULL); /* JSON embeds bt inline, no tab lines */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}
