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

// This binary runs with SIGPIPE at its default disposition, which ends the
// process. It's its own binary because tests.c ignores SIGPIPE, and an ignored
// SIGPIPE is thrown away the moment it's raised, so a write that raises it
// can't be seen there. Each test blocks SIGPIPE on its own thread around the
// operation and then asks whether one is pending, so a failure shows up as
// one failed test and the binary carries on.

#include <common.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/ctls.h>
#include <internal/test_helpers/ctls.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <tau/tau.h>
#include <test_signals.h>
#include <time.h>
#include <unistd.h>

TAU_MAIN()

// Puts SIGPIPE back to its default disposition before any test runs, whatever
// the parent process set, so a raised SIGPIPE would be visible.
__attribute__((constructor)) static void _setup(void) {
  signal(SIGPIPE, SIG_DFL);
}

// Tells whether SIGPIPE is at SIG_DFL. Every test checks it first, since none
// of them proves anything with SIGPIPE ignored.
static bool _sigpipe_has_default_disposition(void) {
  struct sigaction sa;
  if (sigaction(SIGPIPE, NULL, &sa) != 0) return false;
  return sa.sa_handler == SIG_DFL;
}

// Blocks SIGPIPE on the calling thread and stores the old mask in *old, so a
// raised SIGPIPE stays pending instead of ending the process.
static void _block_sigpipe(sigset_t *old) {
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGPIPE);
  pthread_sigmask(SIG_BLOCK, &set, old);
}

// Tells whether a SIGPIPE is pending, consumes it if there is one, and puts
// the signal mask *old back. It's the check half of _block_sigpipe().
static bool _take_pending_sigpipe_and_unblock(const sigset_t *old) {
  sigset_t pending;
  sigemptyset(&pending);
  sigpending(&pending);
  bool raised = sigismember(&pending, SIGPIPE) == 1;
  if (raised) test_consume_pending_sigpipe();
  pthread_sigmask(SIG_SETMASK, old, NULL);
  return raised;
}

// Creates a non-blocking AF_UNIX stream socketpair in fds, the transport of
// every TLS pair here. Gives false, with nothing left open, on failure.
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

// Steps the handshakes of a and b in turn for at most max_iters rounds, so two
// connections in one thread can finish their handshake. True when both
// finished without an error.
static bool _drive_both(ctls_conn *a, ctls_conn *b, int max_iters) {
  bool a_done = false, b_done = false, ok = true;
  for (int i = 0; i < max_iters && !(a_done && b_done); ++i) {
    if (!a_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) a_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) a_done = true, ok = false;
    }
    if (!b_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(b);
      if (r == CTLS_HANDSHAKE_DONE) b_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) b_done = true, ok = false;
    }
  }
  return ok && a_done && b_done;
}

// A connected pair of TLS connections over an AF_UNIX socketpair, with the
// handshake already done. fds[0] is the server side and fds[1] the client.
typedef struct {
  ctls_ctx *server_ctx;
  ctls_ctx *client_ctx;
  ctls_conn *server;
  ctls_conn *client;
  int fds[2];
} tls_pair;

// Builds a tls_pair in *p: a server context with a self-signed certificate,
// a client context, the socketpair and both connections, then the handshake.
// Gives false on any failure; the caller closes the pair either way.
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

// Destroys whatever _tls_pair_open() built. A connection or descriptor the
// test already released is NULL or -1 by then and is skipped.
static void _tls_pair_close(tls_pair *p) {
  ctls_conn_destroy(p->client);
  ctls_conn_destroy(p->server);
  if (p->fds[0] >= 0) close(p->fds[0]);
  if (p->fds[1] >= 0) close(p->fds[1]);
  ctls_ctx_release(p->client_ctx);
  ctls_ctx_release(p->server_ctx);
}

// Closes the peer's socket and destroys the client (destroy_client) or the
// server connection with SIGPIPE blocked, as when a peer resets a keep-alive
// connection. The close_notify written by ctls_conn_destroy() then fails with
// EPIPE. Gives true when SIGPIPE was raised or the pair couldn't be opened.
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
  size_t before = ctls__live_sock_bio_count_for_tests();
  tls_pair p;
  bool opened = _tls_pair_open(&p);
  size_t during = ctls__live_sock_bio_count_for_tests();
  _tls_pair_close(&p);
  size_t after = ctls__live_sock_bio_count_for_tests();
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(during, before + 2);
  REQUIRE_EQ(after, before);
}
