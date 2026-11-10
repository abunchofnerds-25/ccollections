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

#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/test_helpers/chttpserver.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <unistd.h>

TAU_MAIN()

// Ignores SIGPIPE and resets the rest of the inherited signal state before any
// thread starts. The test clients write with plain write(2) and the server can
// close a connection during one; tests_sigpipe.c covers the default
// disposition.
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

// chttpsvr_start creates the thread-specific key that marks the worker threads
// of a server. This binary exists so that the first creation in the process
// happens while a test holds every key (in tests.c the shared _setup already
// created it). Its listeners are unix:// sockets, so it binds no fixed port.

// Answers "hello"; the route that proves a server serves.
static void _hello(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

// Sends one GET of /hello over the Unix socket at path and gives the status, or
// -1; the tests use it to prove that a server serves.
static int _get_over_unix(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req =
      "GET /hello HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n";
  char buf[512] = {0};
  size_t total = 0;
  if (test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
      write(fd, req, strlen(req)) == (ssize_t)strlen(req)) {
    ssize_t r;
    while (total < sizeof(buf) - 1 &&
           (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
      total += (size_t)r;
  }
  close(fd);
  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

// The most keys the test takes, more than any system we run on allows a
// process, so taking them all exhausts the process's keys.
#define WORKER_KEY_TEST_MAX_KEYS 4096

// With every key taken, chttpsvr_start must refuse instead of running workers
// that write through a key some other component owns. Once keys are free, a
// later start creates the key and serves. An unchecked key creation would make
// the first start succeed.
TEST(chttpsvr_worker_key, start_fails_cleanly_when_no_thread_key_is_left) {
  chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(srv, CHTTPSVR_NULL);
  ccol_retval reg_rv =
      chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello, NULL);
  if (reg_rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_key_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;

  pthread_key_t *keys = malloc(WORKER_KEY_TEST_MAX_KEYS * sizeof(*keys));
  if (!keys) chttpsvr_destroy(srv);
  REQUIRE_TRUE(keys != NULL);
  size_t nkeys = 0;
  int create_rv = 0;
  while (nkeys < WORKER_KEY_TEST_MAX_KEYS &&
         (create_rv = pthread_key_create(&keys[nkeys], NULL)) == 0)
    nkeys++;
  bool exhausted = create_rv == EAGAIN;

  ccol_retval first_rv = chttpsvr_start(srv, &cfg);
  if (first_rv == ccol_success) chttpsvr_stop(srv);

  for (size_t i = 0; i < nkeys; i++) pthread_key_delete(keys[i]);
  free(keys);

  ccol_retval second_rv = chttpsvr_start(srv, &cfg);
  int status = second_rv == ccol_success ? _get_over_unix(sock_path) : -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
  chttpsvr_engine_wait();

  REQUIRE_TRUE(exhausted);
  REQUIRE_EQ((int)first_rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ((int)second_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// An allocator that counts live blocks, so the late-destroy test can check that
// nothing the server allocated is left.
static _Atomic long g_late_live_blocks = 0;
static void *_late_malloc(size_t n) {
  void *p = malloc(n);
  if (p) atomic_fetch_add(&g_late_live_blocks, 1);
  return p;
}
static void *_late_calloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (p) atomic_fetch_add(&g_late_live_blocks, 1);
  return p;
}
static void *_late_realloc(void *old, size_t n) {
  void *p = realloc(old, n);
  if (p && !old) atomic_fetch_add(&g_late_live_blocks, 1);
  return p;
}
static void _late_free(void *p) {
  if (p) atomic_fetch_sub(&g_late_live_blocks, 1);
  free(p);
}

// What the child of the test below found, one byte per check.
enum {
  LATE_DEFERRED,
  LATE_SERVED,
  LATE_SLOTS,
  LATE_SHELLS,
  LATE_LOGGER,
  LATE_BLOCKS,
  LATE_CHECKS
};

// An application destructor can destroy its server after the library's
// exit-time destructors (as a statically linked program can). The library then
// leaves its handle table, the router shells and the engine logger to that
// destroy and the engine stop, so nothing is left behind. The child runs the
// library's destructors with a started server and a sub-router alive, then
// destroys the server. Without the deferral the table and shells stay allocated
// and the logger stays open.
TEST(chttpsvr_worker_key, a_server_destroyed_after_the_library_releases_all) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  pid_t pid = fork();
  if (pid < 0) {
    close(pipefd[0]);
    close(pipefd[1]);
    REQUIRE_TRUE(pid >= 0);
  }
  if (pid == 0) {
    close(pipefd[0]);
    alarm(20);
    char out[LATE_CHECKS] = {0};
    static ccol_memmgmt_procs procs = {.malloc = _late_malloc,
                                       .calloc = _late_calloc,
                                       .realloc = _late_realloc,
                                       .free = _late_free};
    int null_fd = open("/dev/null", O_WRONLY);
    clog cl = null_fd >= 0 ? clog_create_from_fd(null_fd, CLOG_INFO, NULL)
                           : CLOG_NULL;
    if (cl) chttpsvr_set_engine_logger(cl);
    chttpsvr srv = chttpsvr_create_mp(&procs, CLOG_NULL, NULL);
    char sock_path[64];
    snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_late_%d.sock",
             (int)getpid());
    unlink(sock_path);
    char host_buf[96];
    snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
    chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = host_buf;
    chttpsvr_router *api =
        srv != CHTTPSVR_NULL ? chttpsvr_subrouter(srv, "/api") : NULL;
    if (api &&
        chttpsvr_router_on(api, CHTTP_GET, "/x", _hello, NULL) ==
            ccol_success &&
        chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello, NULL) ==
            ccol_success &&
        chttpsvr_start(srv, &cfg) == ccol_success) {
      out[LATE_SERVED] = _get_over_unix(sock_path) == 200;
      chttpsvr__run_exit_cleanup_for_tests();
      size_t shells = 0;
      bool array = false;
      chttpsvr__router_shell_registry_state_for_tests(&shells, &array);
      out[LATE_DEFERRED] = !chttpsvr__slot_table_released_for_tests() &&
                           shells == 2 && array &&
                           chttpsvr__engine_user_log_set_for_tests();
      chttpsvr_destroy(srv);
      chttpsvr_engine_wait();
      chttpsvr__router_shell_registry_state_for_tests(&shells, &array);
      out[LATE_SLOTS] = chttpsvr__slot_table_released_for_tests();
      out[LATE_SHELLS] = shells == 0 && !array;
      out[LATE_LOGGER] = !chttpsvr__engine_user_log_set_for_tests();
      out[LATE_BLOCKS] = atomic_load(&g_late_live_blocks) == 0;
    }
    unlink(sock_path);
    ssize_t written = write(pipefd[1], out, sizeof(out));
    (void)written;
    test_child_exit(0);
  }
  close(pipefd[1]);
  char got[LATE_CHECKS] = {0};
  ssize_t n = read(pipefd[0], got, sizeof(got));
  close(pipefd[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  REQUIRE_EQ(waited, pid);
  REQUIRE_EQ(n, (ssize_t)LATE_CHECKS);
  REQUIRE_EQ(got[LATE_SERVED], 1);
  REQUIRE_EQ(got[LATE_DEFERRED], 1);
  REQUIRE_EQ(got[LATE_SLOTS], 1);
  REQUIRE_EQ(got[LATE_SHELLS], 1);
  REQUIRE_EQ(got[LATE_LOGGER], 1);
  REQUIRE_EQ(got[LATE_BLOCKS], 1);
}

// A handle whose server is destroyed, for the check that runs after the
// library's exit-time destructor. This binary destroys every server it creates,
// so the library releases its handle table at exit.
static chttpsvr g_stale_handle_for_exit_check = CHTTPSVR_NULL;

TEST(chttpsvr_worker_key, a_stale_handle_is_kept_for_the_check_at_exit) {
  chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(srv, CHTTPSVR_NULL);
  chttpsvr kept = srv;
  chttpsvr_destroy(srv);
  g_stale_handle_for_exit_check = kept;
}

// Checks, after the library's exit-time destructors (which have the default
// priority), that a stale handle resolves to nothing and that a creation fails
// with an error string once the handle table is released. It reads
// g_stale_handle_for_exit_check and ends the process with status 1 on a
// failure. If the release left freed arrays in place, these calls would read
// them (ASan and valgrind report it) or the creation would succeed.
__attribute__((destructor(101))) static void
_check_handles_after_library_destructor(void) {
  if (g_stale_handle_for_exit_check == CHTTPSVR_NULL) return;
  chttpsvr_stop(g_stale_handle_for_exit_check);
  ccol_retval reg_rv = chttpsvr_register_handler(
      g_stale_handle_for_exit_check, CHTTP_GET, "/late", _hello, NULL);
  char *err = NULL;
  chttpsvr late = chttpsvr_create(CLOG_NULL, &err);
  if (late != CHTTPSVR_NULL || err == NULL || reg_rv == ccol_success) {
    fprintf(stderr,
            "FAILED: a call after the exit-time destructor of the library "
            "used its released handle table (create=%llu err=%s reg=%d)\n",
            (unsigned long long)late, err ? err : "(null)", (int)reg_rv);
    test_child_exit(1);
  }
}
