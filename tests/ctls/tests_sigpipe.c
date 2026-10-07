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
 * process. It must stay a binary of its own: tests.c ignores SIGPIPE for the
 * whole process, and an ignored SIGPIPE is discarded when it is raised, so a
 * write that raises it is invisible there.
 *
 * Each test blocks SIGPIPE on its own thread around the operation under test
 * and then asks whether one is pending. A write(2) to a socket whose peer is
 * gone raises SIGPIPE for the writing thread. With the signal blocked it stays
 * pending instead of ending the process, so a failure is reported as one
 * failed test with the pending signal consumed, and the binary goes on. */

#include <common.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/ctls.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_signals.h>
#pragma GCC diagnostic pop

TAU_MAIN()

extern size_t _ctls_live_sock_bio_count_for_tests(void);

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

/* Reports whether a SIGPIPE is pending, consumes it if so, and restores the
 * signal mask. */
static bool _take_pending_sigpipe_and_unblock(const sigset_t *old) {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  bool raised = sigismember(&pending, SIGPIPE) == 1;
  if (raised) test_consume_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, old, NULL);
  return raised;
}

static bool _make_nonblocking_pair(int fds[2]) {
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) return false;
  for (int i = 0; i < 2; ++i) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    if (flags < 0 || fcntl(fds[i], F_SETFL, flags | O_NONBLOCK) != 0) {
      close(fds[0]);
      close(fds[1]);
      return false;
    }
  }
  return true;
}

static bool _drive_both(ctls_conn_t *a, ctls_conn_t *b, int max_iters) {
  bool a_done = false, b_done = false, ok = true;
  for (int i = 0; i < max_iters && !(a_done && b_done); ++i) {
    if (!a_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) a_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) a_done = true, ok = false;
    }
    if (!b_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(b);
      if (r == CTLS_HANDSHAKE_DONE) b_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) b_done = true, ok = false;
    }
  }
  return ok && a_done && b_done;
}

/* One connected pair of TLS connections over an AF_UNIX socketpair, with
 * the handshake complete. fds[0] carries the server and fds[1] the client. */
typedef struct {
  ctls_ctx_t *server_ctx;
  ctls_ctx_t *client_ctx;
  ctls_conn_t *server;
  ctls_conn_t *client;
  int fds[2];
} tls_pair;

static bool _tls_pair_open(tls_pair *p) {
  memset(p, 0, sizeof(*p));
  p->fds[0] = p->fds[1] = -1;
  p->server_ctx = ctls_ctx_new(NULL);
  p->client_ctx = ctls_ctx_new(NULL);
  if (!p->server_ctx || !p->client_ctx) return false;
  if (ctls_ctx_cert_add(p->server_ctx, "srv.test", NULL, NULL, NULL, NULL) !=
      ccol_success)
    return false;
  if (!_make_nonblocking_pair(p->fds)) return false;
  p->server = ctls_conn_create_server(p->server_ctx, p->fds[0], NULL, NULL);
  p->client = ctls_conn_create_client(p->client_ctx, p->fds[1], "srv.test",
                                      false, NULL);
  if (!p->server || !p->client) return false;
  return _drive_both(p->client, p->server, 200);
}

/* Destroys whatever _tls_pair_open built. A connection or a descriptor that a
 * test already released is NULL or -1 here. */
static void _tls_pair_close(tls_pair *p) {
  ctls_conn_destroy(p->client);
  ctls_conn_destroy(p->server);
  if (p->fds[0] >= 0) close(p->fds[0]);
  if (p->fds[1] >= 0) close(p->fds[1]);
  ctls_ctx_release(p->client_ctx);
  ctls_ctx_release(p->server_ctx);
}

/* A peer that is gone when the connection is destroyed: the close_notify that
 * ctls_conn_destroy() writes must fail with EPIPE and must not raise SIGPIPE.
 * This is the teardown of a keep-alive connection that the peer reset. */
static bool _destroy_after_peer_close_raises_sigpipe(bool destroy_client) {
  tls_pair p;
  bool opened = _tls_pair_open(&p);
  bool raised = true;
  if (opened) {
    int peer = destroy_client ? 0 : 1;
    close(p.fds[peer]);
    p.fds[peer] = -1;
    sigset_t old;
    _block_sigpipe(&old);
    if (destroy_client) {
      ctls_conn_destroy(p.client);
      p.client = NULL;
    } else {
      ctls_conn_destroy(p.server);
      p.server = NULL;
    }
    raised = _take_pending_sigpipe_and_unblock(&old);
  }
  _tls_pair_close(&p);
  return !opened || raised;
}

TEST(ctls_sigpipe, the_binary_runs_with_the_default_disposition) {
  /* Every other test here is vacuous under an ignored SIGPIPE. */
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
}

TEST(ctls_sigpipe, a_client_destroy_after_the_peer_left_raises_no_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  REQUIRE_FALSE(_destroy_after_peer_close_raises_sigpipe(true));
}

TEST(ctls_sigpipe, a_server_destroy_after_the_peer_left_raises_no_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  REQUIRE_FALSE(_destroy_after_peer_close_raises_sigpipe(false));
}

TEST(ctls_sigpipe, a_write_after_the_peer_left_fails_without_sigpipe) {
  REQUIRE_TRUE(_sigpipe_has_default_disposition());
  tls_pair p;
  bool opened = _tls_pair_open(&p);
  ssize_t n = 0;
  int err = 0;
  bool raised = false;
  if (opened) {
    close(p.fds[0]);
    p.fds[0] = -1;
    sigset_t old;
    _block_sigpipe(&old);
    errno = 0;
    n = ctls_conn_write(p.client, "hello", 5);
    err = errno;
    raised = _take_pending_sigpipe_and_unblock(&old);
  }
  _tls_pair_close(&p);
  REQUIRE_TRUE(opened);
  REQUIRE_FALSE(raised);
  REQUIRE_EQ(n, (ssize_t)-1);
  REQUIRE_NE(err, EWOULDBLOCK);
}

TEST(ctls_sigpipe, every_connection_gives_its_socket_bio_back) {
  size_t before = _ctls_live_sock_bio_count_for_tests();
  tls_pair p;
  bool opened = _tls_pair_open(&p);
  size_t during = _ctls_live_sock_bio_count_for_tests();
  _tls_pair_close(&p);
  size_t after = _ctls_live_sock_bio_count_for_tests();
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(during, before + 2);
  REQUIRE_EQ(after, before);
}
