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

/* This binary runs with the default disposition of SIGPIPE, which ends the
 * process. It must stay a binary of its own: the other binaries of this
 * directory ignore SIGPIPE for the whole process, and an ignored SIGPIPE is
 * discarded when it is raised, so a write that raises it is invisible there.
 *
 * It checks two things: that the server leaves the disposition of SIGPIPE
 * as the application set it, and that no write of the server raises SIGPIPE
 * when the peer is gone. Each write test blocks SIGPIPE around the operation
 * and then asks whether one is pending, so a failure is one failed test
 * instead of a dead binary. A write(2) or writev(2) to a socket whose peer has
 * closed raises SIGPIPE for the writing thread; send(2) and sendmsg(2) with
 * MSG_NOSIGNAL do not.
 */

#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/chttp1_parser.h>
#include <internal/csock.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_signals.h>
#pragma GCC diagnostic pop

TAU_MAIN()

__attribute__((constructor)) static void _setup(void) {
  signal(SIGPIPE, SIG_DFL);
}

static bool _sigpipe_has_default_disposition(void) {
  struct sigaction sa;
  if (sigaction(SIGPIPE, NULL, &sa) != 0) return false;
  return sa.sa_handler == SIG_DFL;
}

static void _block_sigpipe(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, old);
}

/* Reports whether a SIGPIPE is pending for the calling thread or for the
 * process, and consumes it if so. The signal must be blocked. */
static bool _take_pending_sigpipe(void) {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  bool raised = sigismember(&pending, SIGPIPE) == 1;
  if (raised) test_consume_pending_sigpipe();
  return raised;
}

static void _sleep_ms(long ms) {
  struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&ts, NULL);
}

/* A connected pair where fds[1] is already closed, so every write to fds[0]
 * meets a peer that is gone. The pair is made the way the server makes the
 * socket of a connection (ccol_accept_nb): non-blocking, closed on exec, and
 * on a system without MSG_NOSIGNAL carrying SO_NOSIGPIPE. */
static bool _pair_with_closed_peer(int *fd) {
  int fds[2];
  if (ccol_socketpair_nb(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  close(fds[1]);
  *fd = fds[0];
  return true;
}

TEST(chttpsvr_sigpipe, the_binary_runs_with_the_default_disposition) {
  /* Every other test here is vacuous under an ignored SIGPIPE. */
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}

/* chttp1_stream_write() is the single-buffer write of every plaintext
   connection, the 100-continue interim line included. */
TEST(chttpsvr_sigpipe, a_stream_write_to_a_closed_peer_raises_no_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  int fd = -1;
  REQUIRE_TRUE(_pair_with_closed_peer(&fd));
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fd, NULL, 0, NULL);
  sigset_t old;
  _block_sigpipe(&old);
  ssize_t n = prepared ? chttp1_stream_write(&s, "hello", 5, 1000) : 0;
  int err = prepared ? chttp1_stream_last_error(&s) : 0;
  bool raised = _take_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  chttp1_stream_release(&s);
  close(fd);
  REQUIRE_TRUE(prepared);
  REQUIRE_FALSE(raised);
  REQUIRE_EQ(n, (ssize_t)-1);
  REQUIRE_EQ(err, EPIPE);
}

/* chttp1_stream_writev2() is the gather write of every plaintext response,
   head and body together. */
TEST(chttpsvr_sigpipe, a_gather_write_to_a_closed_peer_raises_no_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  int fd = -1;
  REQUIRE_TRUE(_pair_with_closed_peer(&fd));
  chttp1_stream_t s;
  bool prepared = chttp1_stream_prepare(&s, fd, NULL, 0, NULL);
  sigset_t old;
  _block_sigpipe(&old);
  ssize_t n =
      prepared ? chttp1_stream_writev2(&s, "head", 4, "body", 4, 1000) : 0;
  int err = prepared ? chttp1_stream_last_error(&s) : 0;
  bool raised = _take_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, &old, NULL);
  chttp1_stream_release(&s);
  close(fd);
  REQUIRE_TRUE(prepared);
  REQUIRE_FALSE(raised);
  REQUIRE_EQ(n, (ssize_t)-1);
  REQUIRE_EQ(err, EPIPE);
}

/* ========================================================================== */
/*                  A RESPONSE TO A CLIENT THAT HAS CLOSED                    */
/* ========================================================================== */

static atomic_int g_slow_entered;
static atomic_int g_client_gone;
static atomic_int g_probe_ran;
static atomic_int g_probe_saw_sigpipe;

/* Holds the response back until the client has closed, so that the write of
   the response meets a peer that is gone. */
static void _slow_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  atomic_store(&g_slow_entered, 1);
  for (int i = 0; i < 10000 && !atomic_load(&g_client_gone); ++i) _sleep_ms(1);
  static char body[64 * 1024];
  memset(body, 'x', sizeof(body));
  chttpsvr_resp_write(resp, body, sizeof(body));
}

/* The server runs one worker thread, so this handler runs on the thread that
   wrote the response of _slow_handler, after that write. Because the worker
   inherits the blocked SIGPIPE of the thread that started the server, a
   SIGPIPE that the write raised is still pending here. */
static void _probe_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  atomic_store(&g_probe_saw_sigpipe, _take_pending_sigpipe() ? 1 : 0);
  atomic_store(&g_probe_ran, 1);
  chttpsvr_resp_write_str(resp, "ok");
}

static int _connect_unix(const char *path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
  struct timeval rcvtimeo = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool _send_all(int fd, const char *s) {
  size_t len = strlen(s), off = 0;
  while (off < len) {
    ssize_t n = send(fd, s + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) return false;
    off += (size_t)n;
  }
  return true;
}

TEST(chttpsvr_sigpipe,
     starting_keeps_the_disposition_and_no_response_write_raises_it) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  atomic_store(&g_slow_entered, 0);
  atomic_store(&g_client_gone, 0);
  atomic_store(&g_probe_ran, 0);
  atomic_store(&g_probe_saw_sigpipe, 0);

  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_sigpipe_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);

  /* Every thread that the server starts inherits this mask. */
  sigset_t old;
  _block_sigpipe(&old);

  chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
  bool created = srv != CHTTPSVR_INVALID;
  bool default_after_create = _sigpipe_has_default_disposition();
  bool registered =
      created &&
      chttpsvr_register_handler(srv, CHTTP_GET, "/slow", _slow_handler, NULL) ==
          ccol_success &&
      chttpsvr_register_handler(srv, CHTTP_GET, "/probe", _probe_handler,
                                NULL) == ccol_success;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  cfg.worker_thread_count = 1;
  bool started = registered && chttpsvr_start(srv, &cfg) == ccol_success;
  bool default_after_start = _sigpipe_has_default_disposition();

  bool slow_sent = false, slow_entered = false;
  int probe_status = -1;
  if (started) {
    int fd = _connect_unix(sock_path);
    if (fd >= 0) {
      slow_sent = _send_all(
          fd, "GET /slow HTTP/1.1\r\nHost: x\r\nConnection: close\r\n\r\n");
      for (int i = 0; slow_sent && i < 10000 && !atomic_load(&g_slow_entered);
           ++i)
        _sleep_ms(1);
      slow_entered = atomic_load(&g_slow_entered) != 0;
      close(fd);
    }
    atomic_store(&g_client_gone, 1);

    fd = _connect_unix(sock_path);
    if (fd >= 0) {
      char buf[512] = {0};
      size_t total = 0;
      if (_send_all(fd,
                    "GET /probe HTTP/1.1\r\nHost: x\r\nConnection: "
                    "close\r\n\r\n")) {
        ssize_t r;
        while (total < sizeof(buf) - 1 &&
               (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
          total += (size_t)r;
      }
      close(fd);
      sscanf(buf, "HTTP/1.1 %d", &probe_status);
    }
  }
  atomic_store(&g_client_gone, 1);
  if (created) chttpsvr_destroy(srv);
  chttpsvr_engine_wait();
  unlink(sock_path);
  bool main_raised = _take_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, &old, NULL);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(default_after_create);
  REQUIRE_TRUE(registered);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(default_after_start);
  REQUIRE_TRUE(slow_sent);
  REQUIRE_TRUE(slow_entered);
  REQUIRE_EQ(probe_status, 200);
  REQUIRE_EQ(atomic_load(&g_probe_ran), 1);
  REQUIRE_EQ(atomic_load(&g_probe_saw_sigpipe), 0);
  REQUIRE_FALSE(main_raised);
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}
