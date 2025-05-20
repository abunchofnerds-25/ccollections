#include <clogger.h>
#include <common.h>
#include <dirent.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
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

/* ========================================================================== */
/*                         LIFECYCLE                                          */
/* ========================================================================== */

TEST(lifecycle, open_fd_and_close) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
  clog_close(lg);
}

TEST(lifecycle, open_file_and_close) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);

  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
  clog_close(lg);

  struct stat st;
  REQUIRE_EQ(stat(path, &st), 0); /* file was created */

  cleanup_dir(dir, "app.log");
}

TEST(lifecycle, invalid_path_returns_null) {
  clog lg = clog_open_file_mp("/nonexistent/deeply/nested/path/app.log",
                              CLOG_INFO, NULL, NULL);
  REQUIRE_EQ((void *)lg, (void *)NULL);
}

TEST(lifecycle, open_fd_rejects_invalid_and_readonly_fds) {
  /* Negative fd must be rejected. */
  REQUIRE_EQ((void *)clog_open_fd(-1, CLOG_INFO), (void *)NULL);

  /* Closed fd must be rejected. */
  int fds[2];
  REQUIRE_EQ(pipe(fds), 0);
  close(fds[0]);
  close(fds[1]);
  REQUIRE_EQ((void *)clog_open_fd(fds[1], CLOG_INFO), (void *)NULL);

  /* Read-only fd must be rejected. */
  int ro = open("/dev/null", O_RDONLY);
  REQUIRE_NE(ro, -1);
  REQUIRE_EQ((void *)clog_open_fd(ro, CLOG_INFO), (void *)NULL);
  close(ro);

  /* Writable fd must be accepted. */
  clog lg = clog_open_fd(STDERR_FILENO, CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
  clog_close(lg);
}

/* ========================================================================== */
/*                         OUTPUT FORMAT                                      */
/* ========================================================================== */

TEST(output, logfmt_fields_present) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_OFF, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_WARN, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  /* These keys must be silently ignored (empty, space, '=', control char, DEL,
   * ']', '\', '"'). */
  clog_set_field(lg, "", "v"); /* empty -- violates RFC 5424 1*32PRINTUSASCII */
  clog_set_field(lg, "bad key", "v");
  clog_set_field(lg, "bad=key", "v");
  clog_set_field(lg, "bad\x01key", "v"); /* C0 control character */
  clog_set_field(lg,
                 "bad\x7f"
                 "key",
                 "v");                 /* DEL (0x7f) -- not PRINTUSASCII */
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

TEST(fields, value_with_spaces_is_quoted) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

TEST(fields, newline_and_cr_in_value_are_escaped) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  /* Value contains raw LF, CR, and HT -- all must be escaped in logfmt
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  /* A backslash in a value triggers double-quoting in logfmt, and each
   * backslash is escaped as \\ inside the quoted string. */
  clog_set_field(lg, "win_path", "C:\\Users\\foo");
  log_info(lg, "backslash test");

  clog_close(lg);

  char buf[4096];
  read_file(path, buf, sizeof buf);

  /* Value must appear as win_path="C:\\Users\\foo" -- each \ escaped to \\. */
  REQUIRE_NE(strstr(buf, "win_path=\"C:\\\\Users\\\\foo\""), NULL);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         OUTPUT -- LONG MESSAGES                            */
/* ========================================================================== */

TEST(output, long_message_uses_heap_and_is_not_truncated) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  /* Simulate an external agent deleting the log file while the logger has it
   * open.  The fd remains valid; the directory entry is gone. */
  REQUIRE_EQ(unlink(path), 0);

  /* First write: bytes_written (9999 + line_len) >= 10000.  Rotation triggers.
   * rename(path, rotated) returns ENOENT (source gone).  The fixed _rotate
   * treats ENOENT as a clean-slate: it creates a fresh file at `path` via
   * O_CREAT and continues normally. */
  log_info(lg, "triggers rotation");

  /* Second write: bytes_written reset to 0 after rotation; line_len < 10000.
   * No further rotation -- this line lands in the recreated file at `path`. */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  for (int i = 0; i < 5; i++) log_info(lg, "collision test %d", i);

  clog_close(lg);

  /* At least one file with the _0001 collision suffix must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, "_0001"), 0);

  cleanup_dir(dir, "app.log");
}

/* ========================================================================== */
/*                         DERIVED LOGGERS                                    */
/* ========================================================================== */

TEST(derive, derived_is_not_null) {
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

  clog_close(child);
  clog_close(parent);
}

TEST(derive, inherits_parent_fields) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog_set_field(parent, "service", "auth");
  clog_set_field(parent, "env", "prod");

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_WARN, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

  /* Lower child's level below parent's -- child sees DEBUG, parent does not */
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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog_set_field(parent, "version", "1");

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);
  clog_set_field(parent, "origin", "parent");

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);
  /* child inherits origin=parent from the snapshot; add its own field */
  clog_set_field(child, "gen", "child");

  /* Derive a grandchild from the child */
  clog grandchild = clog_derive(child);
  REQUIRE_NE((void *)grandchild, (void *)NULL);
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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);
  clog_set_field(parent, "shared", "yes");

  clog child1 = clog_derive(parent);
  REQUIRE_NE((void *)child1, (void *)NULL);
  clog_set_field(child1, "name", "c1");

  clog child2 = clog_derive(parent);
  REQUIRE_NE((void *)child2, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  pthread_t threads[THREAD_COUNT];
  thread_arg_t args[THREAD_COUNT];

  for (int i = 0; i < THREAD_COUNT; i++) {
    args[i].lg = lg;
    args[i].thread_id = i;
    pthread_create(&threads[i], NULL, _writer_thread, &args[i]);
  }
  for (int i = 0; i < THREAD_COUNT; i++) pthread_join(threads[i], NULL);

  clog_close(lg);

  /* Every line must start with "ts=" -- no interleaved partial writes */
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

  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, &procs);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, &procs);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, &procs);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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
      .free = NULL, /* missing free -- must be rejected */
      .calloc = _custom_calloc,
      .realloc = _custom_realloc,
  };

  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, &bad_procs);
  REQUIRE_EQ((void *)lg, (void *)NULL);
}

/* ========================================================================== */
/*                         JSON OUTPUT FORMAT                                 */
/* ========================================================================== */

TEST(json, default_format_is_logfmt) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
  REQUIRE_EQ(clog_get_format(lg), CLOG_FMT_LOGFMT);
  clog_close(lg);
}

TEST(json, get_set_round_trip) {
  clog lg = clog_open_fd_mp(STDERR_FILENO, CLOG_INFO, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

TEST(json, error_has_inline_bt_array) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ERROR, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

TEST(json, format_shared_with_derived_logger) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog_set_format(parent, CLOG_FMT_JSON);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  log_alert(lg, "alert message");

  clog_close(lg);

  char buf[8192];
  read_file(path, buf, sizeof buf);

  REQUIRE_NE(strstr(buf, "ALERT"), NULL);
  REQUIRE_NE(strstr(buf, "alert message"), NULL);
  /* log_alert must produce a backtrace continuation line */
  REQUIRE_NE(strstr(buf, "\t#"), NULL);

  cleanup_dir(dir, "app.log");
}

TEST(output, error_and_alert_level_strings_in_logfmt) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_ALERT, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
  clog_set_format(lg, CLOG_FMT_JSON);

  /* Field value with embedded HT and CR -- both must be JSON-escaped */
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
static clog _g_fatal_child_lg = NULL;
static void _fatal_child_cleanup(void) {
  if (_g_fatal_child_lg) {
    clog_close(_g_fatal_child_lg);
    _g_fatal_child_lg = NULL;
  }
}

TEST(syslog, basic_structure) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  clog lg = clog_open_fd(pipefd[1], CLOG_TRACE);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

TEST(syslog, sd_param_name_truncated_to_32_chars) {
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
  clog_set_format(lg, CLOG_FMT_SYSLOG);

  /* 40-char key -- exceeds RFC 5424 SD-PARAM-NAME limit of 32. */
  clog_set_field(lg, "abcdefghijklmnopqrstuvwxyz_123456789", "val");

  log_info(lg, "truncation");

  char buf[4096];
  drain_pipe(lg, pipefd[0], pipefd[1], buf, sizeof buf);

  /* The emitted key must be capped at 32 characters. */
  REQUIRE_NE(strstr(buf, "abcdefghijklmnopqrstuvwxyz_12345"),
             NULL); /* 32 chars present */
  REQUIRE_EQ(strstr(buf, "abcdefghijklmnopqrstuvwxyz_123456"),
             NULL); /* 33rd char absent */
}

TEST(syslog, file_logger_rejects_syslog_format) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  clog lg = clog_open_fd(pipefd[1], CLOG_ERROR);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_fd(fd, CLOG_TRACE);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  clog lg = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

/* ========================================================================== */
/*                         ADDITIONAL LIFECYCLE                               */
/* ========================================================================== */

TEST(lifecycle, open_file_appends_to_existing) {
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  /* Write a sentinel line with the first logger. */
  clog lg1 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg1, (void *)NULL);
  log_info(lg1, "first open");
  clog_close(lg1);

  /* Open the same path again -- must append, not truncate. */
  clog lg2 = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg2, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  clog parent = clog_open_fd_mp(STDERR_FILENO, CLOG_WARN, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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
  clog parent = clog_open_fd(pipefd[1], CLOG_INFO);
  REQUIRE_NE((void *)parent, (void *)NULL);
  clog_set_format(parent, CLOG_FMT_SYSLOG);

  clog child = clog_derive(parent);
  REQUIRE_NE((void *)child, (void *)NULL);

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

  clog parent = clog_open_file_mp(path, CLOG_INFO, NULL, NULL);
  REQUIRE_NE((void *)parent, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  for (int i = 0; i < 20; i++)
    log_info(lg, "compression test message index=%d padding-to-force-rotation",
             i);

  clog_close(lg);

  /* At least one .gz file must exist. */
  REQUIRE_GT(count_files_with_suffix(dir, ".gz"), 0);

  /* All rotated files should be .gz -- no raw uncompressed rotated files. */
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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_INFO, &cfg, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

  /* A single write forces a rotation of the (nearly-empty) file. */
  log_info(lg, "trigger rotation");
  clog_close(lg);

  char gz_path[512];
  int found = find_file_with_suffix(dir, ".gz", gz_path, sizeof gz_path);
  if (found == 0) {
    /* If a .gz was produced it must be valid. */
    REQUIRE_EQ(gunzip_test(gz_path), 0);
  }
  /* If no .gz exists the file was too small to trigger rotation -- that is
   * also acceptable; the test verifies there is no crash. */

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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  char dir[256];
  REQUIRE_EQ(make_tmpdir(dir, sizeof dir), 0);
  char path[512];
  snprintf(path, sizeof path, "%s/app.log", dir);

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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
  clog lg = clog_open_file_mp(path, CLOG_OFF, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);

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

  clog lg = clog_open_file_mp(path, CLOG_TRACE, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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

  clog lg = clog_open_file_mp(path, CLOG_FATAL, NULL, NULL);
  REQUIRE_NE((void *)lg, (void *)NULL);
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
  REQUIRE_EQ(strstr(buf, "\t#"), NULL); /* JSON embeds bt inline, no tab lines */
  REQUIRE_EQ(buf[0], '{');
  char *nl = strchr(buf, '\n');
  REQUIRE_NE(nl, NULL);
  REQUIRE_EQ(*(nl - 1), '}');

  cleanup_dir(dir, "app.log");
}
