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
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* The test clients of this binary write to the server with plain write(2),
 * and the server can close a connection while one of them writes. The
 * library leaves the disposition of SIGPIPE to the application, so this
 * binary ignores it itself. tests_sigpipe.c covers the library under the
 * default disposition. */
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
}

/* ========================================================================== */
/*          THREAD-SPECIFIC KEY EXHAUSTION (dedicated binary)                 */
/*                                                                            */
/* chttpsvr_start creates the thread-specific key that marks the worker      */
/* threads of a server. The first creation in the process must happen while */
/* this test holds every key that the process can have, so this file is a   */
/* binary of its own: in tests.c, the shared _setup already started a       */
/* server and created the key. The test uses a unix:// listener, so it      */
/* binds no fixed port.                                                      */
/* ========================================================================== */

static void _hello(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

/* Sends one GET over the Unix domain socket at path and gives the status. */
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
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0 &&
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

#define WORKER_KEY_TEST_MAX_KEYS 4096

/* With every key of the process taken, chttpsvr_start has no key to mark its
   worker threads with. It must refuse to start, and not run workers that
   write the server pointer through a key that some other component owns.
   Once keys are free again, a later chttpsvr_start creates the key and the
   server serves normally. This test is non-vacuous: when the creation of the
   key is not checked, the first chttpsvr_start succeeds. */
TEST(chttpsvr_worker_key, start_fails_cleanly_when_no_thread_key_is_left) {
  chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(srv, CHTTPSVR_INVALID);
  ccol_retval_t reg_rv =
      chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello, NULL);
  if (reg_rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_key_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
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

  ccol_retval_t first_rv = chttpsvr_start(srv, &cfg);
  if (first_rv == ccol_success) chttpsvr_stop(srv);

  for (size_t i = 0; i < nkeys; i++) pthread_key_delete(keys[i]);
  free(keys);

  ccol_retval_t second_rv = chttpsvr_start(srv, &cfg);
  int status = second_rv == ccol_success ? _get_over_unix(sock_path) : -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
  chttpsvr_engine_wait();

  REQUIRE_TRUE(exhausted);
  REQUIRE_EQ((int)first_rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ((int)second_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}
