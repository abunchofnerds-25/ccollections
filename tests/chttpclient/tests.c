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

/* pthread_tryjoin_np() is a glibc extension, and <pthread.h> does not
 * declare it without this define. The code below uses it as a join that
 * does not block, to reap mock-server connection threads that have already
 * stopped. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chttpclient.h>
#include <common.h>
#include <cthreadpool.h>
#include <ctype.h>
#include <dirent.h>
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/csock.h>
#if defined(__APPLE__)
#include <malloc/malloc.h>
/* The usable size of a block of malloc(3); macOS names it malloc_size(). */
#define malloc_usable_size(p) malloc_size(p)
#else
#include <malloc.h>
#endif
#include <netdb.h>
#include <netinet/in.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_fds.h>
#pragma GCC diagnostic pop

TAU_MAIN()

#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

/* Joins t and gives 0 when it has already ended, or gives non-zero at once
 * when it still runs. glibc has pthread_tryjoin_np(); FreeBSD gets the same
 * from pthread_timedjoin_np() with a deadline that has already passed.
 * macOS has neither, so there this never reaps early, as under
 * ThreadSanitizer below.
 *
 * ThreadSanitizer on FreeBSD intercepts pthread_join() and pthread_detach()
 * but not pthread_timedjoin_np(). A thread joined through the latter stays
 * live in its registry, and the next thread that libthr starts in the same
 * thread structure stops the process with a CHECK failure of the runtime.
 * Such a build therefore never reaps early: every thread stays registered
 * until the join of the teardown, or is detached when the registry is
 * full. */
#if defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define TEST_UNDER_TSAN 1
#endif
#endif
#if defined(__SANITIZE_THREAD__)
#define TEST_UNDER_TSAN 1
#endif
static int test_tryjoin(pthread_t t) {
#if defined(__GLIBC__)
  return pthread_tryjoin_np(t, NULL);
#elif defined(TEST_UNDER_TSAN) || defined(__APPLE__)
  (void)t;
  return EBUSY;
#else
  const struct timespec passed = {0, 0};
  return pthread_timedjoin_np(t, NULL, &passed);
#endif
}

/* This binary must ignore SIGPIPE for the sake of its own mock HTTP server.
 * The send() calls of the mock server below use plain flags, with no
 * MSG_NOSIGNAL, so a client that closes or reuses a connection while a
 * server thread is inside send() raises SIGPIPE, and the default action for
 * SIGPIPE kills the whole process, not only that one thread.
 *
 * Every write of chttpclient.c passes MSG_NOSIGNAL, and the library never
 * changes the disposition of SIGPIPE, so nothing but this constructor
 * protects the writes of the mock server. The race window is wide enough
 * only under the slowdown of valgrind; a process that it kills has the wait
 * status 128+13 (SIGPIPE) and no core file, because the default action for
 * SIGPIPE writes none. One process-wide SIGPIPE-ignore, installed before the
 * mock server or any test runs, closes the window for every test order and
 * every tier that a test uses. tests_sigpipe.c covers the client under the
 * default disposition. */
__attribute__((constructor)) static void _ignore_sigpipe_for_mock_server(void) {
  signal(SIGPIPE, SIG_IGN);
}

/* ========================================================================== */
/*                     MINIMAL HTTP/1.1 TEST SERVER                           */
/* ========================================================================== */

#define TEST_SERVER_BUF 65536
#define TEST_SERVER_PORT 0 /* OS assigns a free port */

typedef struct {
  /* This field is an atomic_int rather than a plain int: the teardown in
   * stop_test_server writes -1 here after it has called shutdown() and
   * close() on the fd, while the accept-loop thread reads the same field to
   * make its accept() syscall. With a plain int, that write and that read
   * are a real data race, which ThreadSanitizer reports. The result is
   * harmless either way, because the fd is already closed when the write
   * happens; this _Atomic field follows the convention that this codebase
   * uses for this shape of hazard. */
  atomic_int server_fd;
  int port;
  pthread_t accept_tid;
  atomic_int running;

  /* IPv6 loopback listener, with fields that match the fields above. The
   * bind is best effort, because not every sandbox or CI environment has an
   * IPv6 stack: when the bind fails, server_fd6 stays -1 and port6 stays 0,
   * and the test server setup does not treat this as a hard failure. */
  atomic_int server_fd6;
  int port6;
  pthread_t accept_tid6;

  /* Unix domain socket listener for http+unix:// coverage, with fields that
   * match the fields above. The OS assigns the ports of the TCP listeners,
   * but unix_path is fixed: the bind step builds it from getpid(), so it is
   * unique for each test process. */
  atomic_int server_fd_unix;
  char unix_path[128];
  pthread_t accept_tid_unix;
} test_server_t;

static test_server_t g_srv;
static pthread_once_t g_srv_once = PTHREAD_ONCE_INIT;
static atomic_int g_slow_started = 0;
static atomic_int g_accept_count = 0;

/* Connection-thread registry so stop_test_server can join them all. */
#define MAX_CONN_THREADS 256
static pthread_t g_conn_threads[MAX_CONN_THREADS];
static int g_conn_thread_count = 0;
static pthread_mutex_t g_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* Registers a new connection thread in g_conn_threads, after it first reaps,
 * with a join that does not block, every earlier entry that has already
 * stopped. Each connection thread here has a short life, but a thread that
 * stopped and that nobody joined keeps its stack mapping until the join
 * happens.
 *
 * Without this reap step the registry only grows, because the mock server
 * starts once for the whole process and accepts connections across hundreds
 * of tests, one after the other. The thread-stack memory that the binary
 * never frees then climbs until the join at process exit in
 * stop_test_server catches up. Since the usable address space of a 32-bit
 * build is much smaller than that of a 64-bit build, this growth alone can
 * exhaust it there before any other resource limit is reached. */
static void register_conn_thread(pthread_t tid) {
  pthread_mutex_lock(&g_conn_mutex);
  int kept = 0;
  for (int i = 0; i < g_conn_thread_count; i++) {
    if (test_tryjoin(g_conn_threads[i]) != 0) {
      g_conn_threads[kept++] = g_conn_threads[i];
    }
  }
  g_conn_thread_count = kept;

  if (g_conn_thread_count < MAX_CONN_THREADS) {
    g_conn_threads[g_conn_thread_count++] = tid;
  } else {
    pthread_detach(tid); /* registry full: fall back to detach */
  }
  pthread_mutex_unlock(&g_conn_mutex);
}

/*
 * Sends a complete HTTP response. keep_alive selects whether the server
 * sends "Connection: close"; when keep_alive is true, the response relies on
 * the implicit keep-alive default of HTTP/1.1 instead. Only the keep-alive
 * routes, which exist for real connection-reuse coverage, pass true; every
 * other route passes false and so closes after one response.
 */
static void srv_respond(int fd, int status, const char *status_text,
                        const char *content_type, const char *extra_hdrs,
                        const char *body, size_t body_len, bool keep_alive) {
  char header[2048];
  int hlen = snprintf(
      header, sizeof(header),
      "HTTP/1.1 %d %s\r\n"
      "Content-Type: %s\r\n"
      "Content-Length: %zu\r\n"
      "%s"
      "%s"
      "\r\n",
      status, status_text, content_type ? content_type : "text/plain", body_len,
      keep_alive ? "" : "Connection: close\r\n", extra_hdrs ? extra_hdrs : "");
  if (hlen > 0) {
    size_t to_send =
        ((size_t)hlen < sizeof(header)) ? (size_t)hlen : sizeof(header) - 1;
    send(fd, header, to_send, 0);
  }
  if (body && body_len > 0) send(fd, body, body_len, 0);
}

/* Find a header value in the raw request text (case-insensitive name). */
static int srv_find_header(const char *raw, const char *name, char *out,
                           size_t out_size) {
  const char *p = raw;
  size_t nlen = strlen(name);
  while (*p) {
    /* Move to start of next header line. */
    const char *eol = strstr(p, "\r\n");
    if (!eol) break;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') {
      const char *val = p + nlen + 1;
      while (*val == ' ' || *val == '\t') val++;
      size_t vlen = (size_t)(eol - val);
      if (vlen >= out_size) vlen = out_size - 1;
      memcpy(out, val, vlen);
      out[vlen] = '\0';
      return 1;
    }
    p = eol + 2;
  }
  return 0;
}

/* Count how many header lines match name (case-insensitive) in the raw request.
 */
static int srv_count_header(const char *raw, const char *name) {
  int count = 0;
  const char *p = raw;
  size_t nlen = strlen(name);
  while (*p) {
    const char *eol = strstr(p, "\r\n");
    if (!eol) break;
    if (strncasecmp(p, name, nlen) == 0 && p[nlen] == ':') count++;
    p = eol + 2;
  }
  return count;
}

/* Parse the first line of the request into method and path. */
static void srv_parse_request_line(const char *buf, char *method, size_t mlen,
                                   char *path, size_t plen) {
  const char *p = buf;
  size_t i = 0;
  while (*p && *p != ' ' && i < mlen - 1) method[i++] = *p++;
  method[i] = '\0';
  if (*p == ' ') p++;
  i = 0;
  while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < plen - 1)
    path[i++] = *p++;
  path[i] = '\0';
}

/*
 * Reads the request headers only: it stops at the first "\r\n\r\n" and
 * never blocks to wait for more body bytes, keeping only the body bytes
 * that arrive in the same recv() calls as the headers. It writes the length
 * of the header block, including the CRLFCRLF at the end, to *hdr_len_out.
 *
 * Reading the headers alone, instead of the headers plus the whole declared
 * body in one step, is what lets srv_conn_thread answer an
 * "Expect: 100-continue" request header and choose the route before it
 * reads any body byte; for the reject route it never reads one. Every other
 * route gets its body from the content-length loop of srv_conn_thread,
 * which runs directly after this call returns.
 */
static ssize_t srv_read_headers(int fd, char *buf, size_t max,
                                size_t *hdr_len_out) {
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

  ssize_t total = 0;
  while (total < (ssize_t)(max - 1)) {
    ssize_t n = recv(fd, buf + total, max - 1 - (size_t)total, 0);
    if (n <= 0) {
      /* The connection closed or failed after some bytes arrived but before the
       * header terminator did. This code sets *hdr_len_out for the same
       * reason as the buffer-exhausted case below, so that the output stays
       * self-consistent: some callers check only `if (n <= 0) break;`
       * rather than `total` and `*hdr_len_out` together, and without this
       * line such a caller reads these leftover bytes as a header block of
       * zero length. */
      *hdr_len_out = 0;
      return total > 0 ? total : n;
    }
    total += n;
    buf[total] = '\0';

    char *hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) {
      *hdr_len_out = (size_t)(hdr_end - buf) + 4;
      return total;
    }
  }
  /* The buffer is full and the header terminator never arrived, which means
   * a request whose headers alone are larger than `max` bytes. This code
   * sets *hdr_len_out here on purpose, instead of leaving it to the caller,
   * so that the output of this function is self-consistent whatever value
   * the caller wrote into hdr_len_out before the call. */
  *hdr_len_out = 0;
  return total;
}

/*
 * Handles the two test routes that know about Expect: 100-continue. `total`
 * is the number of bytes already in buf: the headers, and possibly part or
 * all of the body if it arrived in the same reads. hdr_len is the length of
 * the header block. This function returns true when the connection must
 * close after this response, which is the same return convention that
 * srv_handle_route uses.
 *
 * accept_body selects the behavior of the route. When it is true, the route
 * sends "100 Continue" first, then reads the body and echoes it. This
 * exercises one path of chttp_do_internal, which sees the interim 100,
 * resets pctx, sends the body and reads the real final response; only this
 * route can exercise the correct reset of pctx.
 *
 * When accept_body is false, the route answers 417 directly, without ever
 * sending "100 Continue" or reading a body byte. This is the case of RFC
 * 7231 SS5.1.1 where the server can reject without a wait, and it exercises
 * the other path of chttp_do_internal: the server answered directly, that
 * answer IS the final response, and the client never sends the body.
 */
static bool srv_handle_expect_continue_route(int conn_fd, char *buf, size_t max,
                                             size_t total, size_t hdr_len,
                                             bool accept_body) {
  if (!accept_body) {
    const char *b = "expectation failed";
    srv_respond(conn_fd, 417, "Expectation Failed", "text/plain", NULL, b,
                strlen(b), false);
    return true;
  }

  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  const char *body = buf + hdr_len;
  size_t body_len = (cl > 0) ? (size_t)cl : 0;
  srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
              body_len, false);
  return true;
}

/*
 * A third route that knows about Expect: 100-continue. Only
 * expect_continue.reused_connection_dies_after_partial_interim_line_is_not_retried
 * uses it. It writes an INCOMPLETE fragment of a "100 Continue" status line on
 * purpose, with no CRLF at the end, then sleeps for longer than
 * CHTTP_100_CONTINUE_WAIT_US while it holds the connection open, so that the
 * window of the client ends while the message is half read. At the end the
 * route closes without ever reading the body or sending a real response. It
 * never returns false, because it always closes.
 */
static bool srv_handle_expect_continue_timeout_then_die_route(int conn_fd) {
  const char *partial = "HTTP/1.1 100 Con";
  send(conn_fd, partial, strlen(partial), 0);
  usleep(1300000); /* > CHTTP_100_CONTINUE_WAIT_US (1000ms) */
  return true;
}

/*
 * A fourth route that knows about Expect: 100-continue, which sends "103
 * Early Hints" (RFC 8297) BEFORE "100 Continue". Since the "100 Continue"
 * wait watches for one status only (stop_at_status = 100), this route tests
 * whether _chttp_read_message_loop can discard a different interim 1xx
 * status and go on waiting inside the same CHTTP_100_CONTINUE_WAIT_US
 * budget, without reading the 103 as the "100 Continue" or as the final
 * response.
 */
static bool srv_handle_expect_continue_with_hints_route(int conn_fd, char *buf,
                                                        size_t max,
                                                        size_t total,
                                                        size_t hdr_len) {
  const char *hints =
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
  send(conn_fd, hints, strlen(hints), 0);
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  const char *body = buf + hdr_len;
  size_t body_len = (cl > 0) ? (size_t)cl : 0;
  srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
              body_len, false);
  return true;
}

/*
 * A fifth route that knows about Expect: 100-continue, used only by
 * expect_continue.direct_rejection_without_100_never_pools_connection. It
 * answers directly, with no "100 Continue", and never reads the body, just
 * like the accept_body=false branch of srv_handle_expect_continue_route.
 * Unlike that branch, which always closes, this route leaves the connection
 * open: it does not send Connection: close, and it tells the handler loop
 * to continue.
 *
 * This route models a server that obeys RFC 7231 SS5.1.1, which only SHOULD
 * closes here and never MUST closes, but that is naive: it does not track
 * that this connection still owes it a body before the connection is back
 * at a request boundary. The route pins one contract: the client must
 * NOT put such a connection into the pool for reuse. *keep_alive_out must
 * account for whether the client ever sent the declared request body,
 * instead of only copying the Connection header of this response.
 */
static bool srv_handle_expect_continue_reject_keepalive_route(int conn_fd) {
  const char *b = "expectation failed";
  srv_respond(conn_fd, 417, "Expectation Failed", "text/plain", NULL, b,
              strlen(b), true);
  return false;
}

/*
 * A sixth route that knows about Expect: 100-continue, used only by
 * expect_continue.dead_connection_after_100_with_fake_leftover_final_not_retried_with_stale_state.
 * It writes "100 Continue" and more bytes in the SAME send() call, and those
 * bytes LOOK like the header block of a complete final response plus a
 * truncated body. One send() call puts them in the same TCP segment, so the
 * client gets them in the same read. Then the route closes without ever
 * finishing that body or reading what the client sends. This models a
 * server, fast or buggy, that runs ahead of its own "100 Continue" with
 * response bytes that it cannot deliver in full.
 *
 * This route pins two contracts that belong together. The parser reads the
 * leftover bytes after the "100 Continue" boundary into the pctx and the
 * body buffer of chttp_do_internal, as part of a carry-in read that cannot
 * complete, and that carry-in parse must set any_bytes_read_out. Without
 * that, the retry-once safety net for a reused connection sends the request
 * again on a fresh connection while it reuses the same polluted pctx and
 * body buffer, so the headers and the body prefix of the fake response mix
 * into the response that the caller gets.
 */
static bool srv_handle_expect_continue_fake_final_then_die_route(int conn_fd) {
  const char *wire =
      "HTTP/1.1 100 Continue\r\n\r\n"
      "HTTP/1.1 200 OK\r\n"
      "x-stale: yes\r\n"
      "content-length: 100\r\n"
      "\r\n"
      "STALE-BODY-PREFIX-THAT-MUST-NEVER-REACH-THE-CALLER"; /* < 100 bytes */
  send(conn_fd, wire, strlen(wire), 0);
  return true; /* close without completing the declared 100-byte body */
}

/* srv_handle_expect_continue_die_after_100_clean_route below adds one to
 * this counter for each full body that it reads, which lets
 * expect_continue.dead_connection_after_100_clean_eof_not_retried assert
 * that the client sent the body to the server exactly once. Without the
 * counter, the test could only assert that nobody opened a second TCP
 * connection. */
static _Atomic int g_die_after_100_clean_body_recv_count = 0;

/*
 * A seventh route that knows about Expect: 100-continue, used only by
 * expect_continue.dead_connection_after_100_clean_eof_not_retried. It sends
 * "100 Continue" in its own separate send() call, unlike the route
 * srv_handle_expect_continue_fake_final_then_die_route above, which runs
 * ahead with fake trailing bytes in the SAME read. This route then reads
 * the full declared body and closes the connection at once with NO more
 * bytes: a clean EOF on a message boundary for the final response, with
 * nothing left in the same read to start the carry-in path.
 *
 * The route pins this contract: a clean EOF (n == 0, no carry-in) must
 * still mean that the peer was alive here. The client has already sent the
 * body to a server that confirmed it was alive with "100 Continue", so a
 * false any_bytes_read_out lets the retry-once safety net for a reused
 * connection inside chttp_do_internal send the whole body again on a second
 * connection.
 */
static bool srv_handle_expect_continue_die_after_100_clean_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len) {
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }
  if (cl > 0 && total - hdr_len >= (size_t)cl)
    atomic_fetch_add(&g_die_after_100_clean_body_recv_count, 1);

  return true; /* close with no further bytes at all: a clean EOF */
}

/*
 * An eighth route that knows about Expect: 100-continue, used only by the
 * test async_expect_continue.interim_100_bundled_with_final_response_in_same_
 * read of the async engine. It writes "100 Continue" and, in the SAME send()
 * call, a COMPLETE and valid final response, so that both arrive in the same
 * read on the client side. This differs from the route
 * srv_handle_expect_continue_fake_final_then_die_route above, whose
 * trailing bytes are truncated garbage on purpose and whose connection then
 * dies in the middle of them.
 *
 * Here the trailing bytes are a real, complete response that the client can
 * consume. The route then drains and discards the body that the client
 * sends instead of closing at once, so the body write of the client
 * completes in the normal way and does not race the close of this route.
 *
 * This route exercises the carry-forward path of the async engine for this
 * exact case (see the field comment of chttp_async_ctx_t.continue_carry).
 * The bytes of the final response arrive before the body write starts, and
 * the engine must store them and replay them after that write finishes,
 * without losing them or reading them as part of a later message.
 */
static bool srv_handle_expect_continue_bundled_final_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len) {
  const char *resp_body = "bundled-response";
  char wire[512];
  int wn = snprintf(wire, sizeof(wire),
                    "HTTP/1.1 100 Continue\r\n\r\n"
                    "HTTP/1.1 200 OK\r\n"
                    "content-type: text/plain\r\n"
                    "content-length: %zu\r\n"
                    "connection: close\r\n"
                    "\r\n%s",
                    strlen(resp_body), resp_body);
  send(conn_fd, wire, (size_t)wn, 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  return true; /* Connection: close was already declared above */
}

/*
 * A ninth route that knows about Expect: 100-continue, used only by
 * expect_continue.hints_both_sides_of_100_continue_each_phase_gets_its_own_cap
 * and its async counterpart. It sends n_before interim "103 Early Hints"
 * responses, THEN sends "100 Continue" and reads the declared body, THEN
 * sends n_after more interim "103 Early Hints" responses, and only after
 * that answers for real. The route takes n_before and n_after from the path
 * ("/expect-continue-hints-both-sides/<n_before>/<n_after>").
 *
 * The route tests how the client obeys its own CHTTP_MAX_INTERIM_RESPONSES
 * cap, which must be a count of CONSECUTIVE discards. The client discards
 * interim responses both while it waits for "100 Continue" and while it
 * reads the real final response, and the cap must bound each of the two
 * phases on its own: a confirmed "100 Continue" is a real message that
 * nobody discards, so it breaks the run. The cap must not be one budget that
 * both phases share.
 *
 * See the field comment of chttp_async_ctx_t.interim_responses_seen for the
 * failure that this guards against. With one counter for both phases, a hop
 * that discards interim responses before "100 Continue" keeps less than the
 * documented budget of 64 responses that belongs to its own final-response
 * read.
 */
static bool srv_handle_expect_continue_hints_both_sides_route(
    int conn_fd, char *buf, size_t max, size_t total, size_t hdr_len,
    const char *path) {
  int n_before = 0, n_after = 0;
  sscanf(path, "/expect-continue-hints-both-sides/%d/%d", &n_before, &n_after);
  const char *hints =
      "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
  for (int i = 0; i < n_before; i++) {
    if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
  }
  const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
  send(conn_fd, cont, strlen(cont), 0);

  char cl_str[32] = {0};
  long cl = 0;
  if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
    cl = atol(cl_str);

  while (cl > 0 && total - hdr_len < (size_t)cl && total < max - 1) {
    ssize_t m = recv(conn_fd, buf + total, max - 1 - total, 0);
    if (m <= 0) break;
    total += (size_t)m;
    buf[total] = '\0';
  }

  for (int i = 0; i < n_after; i++) {
    if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
  }

  const char *b = "{\"status\":\"ok\"}";
  srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
              false);
  return true;
}

/* The definition is further below, next to the other helpers for a
 * Unix-socket URL; the declaration is here because the redirect routes that
 * give out a "http+unix://" Location need it at this point. */
static void percent_encode_unix_path(char *buf, size_t buf_size,
                                     const char *raw);

/*
 * Routes the request and sends a response. It returns true when the
 * connection must close after this response, and false when the caller
 * (srv_conn_thread) must loop and read another request from the same fd,
 * which is the keep-alive case.
 */
/* The /sweep-race route answers only once this is true. */
static atomic_bool g_sweep_race_respond = false;

static bool srv_handle_route(int conn_fd, const char *method, const char *path,
                             const char *raw, const char *body,
                             size_t body_len) {
  if (strcmp(method, "GET") == 0 && strcmp(path, "/get") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if ((strcmp(method, "POST") == 0 && strcmp(path, "/post") == 0) ||
      (strcmp(method, "PATCH") == 0 && strcmp(path, "/patch") == 0)) {
    /* Echo the request body. */
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, body, body_len,
                false);
    return true;
  }

  if (strcmp(method, "PUT") == 0 && strcmp(path, "/put") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "PUT") == 0 && strcmp(path, "/put-echo") == 0) {
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete") == 0) {
    srv_respond(conn_fd, 204, "No Content", "text/plain", NULL, NULL, 0, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/headers") == 0) {
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json",
                "X-Chttp-Test: hello\r\n", b, strlen(b), false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/echo-header") == 0) {
    char echo_val[256] = {0};
    srv_find_header(raw, "x-echo", echo_val, sizeof(echo_val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, echo_val,
                strlen(echo_val), false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/echo-auth") == 0) {
    /* Echoes the Authorization header, or "" when there is none. Tests use
     * it to check three things: the client must build Basic auth from the
     * userinfo of the URL, must never overwrite a header that the caller
     * gives, and must strip the credentials on a cross-origin redirect. */
    char auth_val[256] = {0};
    srv_find_header(raw, "authorization", auth_val, sizeof(auth_val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, auth_val,
                strlen(auth_val), false);
    return true;
  }

  /* /status/NNN */
  if (strncmp(path, "/status/", 8) == 0) {
    int code = atoi(path + 8);
    if (code >= 100 && code <= 599) {
      const char *b = "status";
      srv_respond(conn_fd, code, "Status", "text/plain", NULL, b, strlen(b),
                  false);
    } else {
      const char *b = "bad code";
      srv_respond(conn_fd, 400, "Bad Request", "text/plain", NULL, b, strlen(b),
                  false);
    }
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/redirect") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/redirect-with-body") == 0) {
    /* This route is like /redirect, except that the 301 response here
     * carries a non-empty body with a name that nothing else uses. The
     * intermediate body of /redirect is always empty, so a streaming
     * redirect test that uses /redirect cannot tell whether the client
     * discarded the body of the intermediate hop or whether there was never
     * anything to leak; such a test would not see a client that
     * gives the intermediate body to the sink of the caller. This route
     * gives such a test a real intermediate body, so that the test can
     * prove that the client did NOT capture it. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    const char *b = "this-intermediate-body-must-never-reach-the-caller";
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, b,
                strlen(b), false);
    return true;
  }

  if (strcmp(method, "HEAD") == 0 && strcmp(path, "/head") == 0) {
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, NULL, 0, false);
    return true;
  }

  if (strcmp(method, "OPTIONS") == 0 && strcmp(path, "/options") == 0) {
    srv_respond(conn_fd, 200, "OK", "text/plain",
                "Allow: GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS\r\n", NULL,
                0, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/slow") == 0) {
    /* This route signals that it got the request before it sleeps, so that
     * the test can act on a full pool while chttpclient_do is still
     * blocked inside the request for this connection.
     */
    atomic_fetch_add(&g_slow_started, 1);
    usleep(100000); /* 100 ms */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/very-slow") == 0) {
    /* A much slower sibling of /slow. The test async_deadline.
     * request_timeout_fires_against_slow_endpoint needs a wide margin
     * between its short request_timeout_ms and the first moment a real
     * response can arrive. The deadline sweep ticks every
     * CHTTP_DEADLINE_SWEEP_INTERVAL_MS (100ms), and the sleep of /slow is
     * exactly 100ms, so a race between the two puts the worst-case latency
     * of the sweep and the arrival of the real response too close together
     * for the test to tell them apart. It signals its start as /slow does. */
    atomic_fetch_add(&g_slow_started, 1);
    usleep(500000); /* 500 ms */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/large") == 0) {
    /* The body is larger than the first 4096-byte cap of _write_cb, which
     * makes the buffer grow. */
    static char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, large_body,
                sizeof(large_body), false);
    return true;
  }

  if (strcmp(path, "/echo-content-type") == 0) {
    /* Answers with the content-type that the client sent, for any method. */
    char ct[256] = {0};
    srv_find_header(raw, "content-type", ct, sizeof(ct));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, ct, strlen(ct), false);
    return true;
  }

  if (strcmp(path, "/count-content-type") == 0) {
    /* Answers with the number of Content-Type header lines in the request,
     * for any method.
     */
    int count = srv_count_header(raw, "content-type");
    char body_buf[8];
    int blen = snprintf(body_buf, sizeof(body_buf), "%d", count);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(path, "/count-header") == 0) {
    /* A general sibling of /count-content-type: the "x-count-name" header of
     * the request names the header line to count, and the route answers with
     * that count, for any method. The client never runs a case-sensitive
     * presence check on that header, because the test sets it and it is always
     * lowercase. It is not one of the headers that _serialize_request adds by
     * itself. This route covers Accept, User-Agent, Content-Length,
     * Authorization and Expect, in the same way as /count-content-type covers
     * Content-Type. */
    char name[64] = {0};
    srv_find_header(raw, "x-count-name", name, sizeof(name));
    int count = name[0] ? srv_count_header(raw, name) : -1;
    char body_buf[8];
    int blen = snprintf(body_buf, sizeof(body_buf), "%d", count);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, body_buf, (size_t)blen,
                false);
    return true;
  }

  if (strcmp(path, "/echo-header-raw") == 0) {
    /* A sibling of /echo-header that echoes the header line which the
     * "x-echo-name" header of the request names, instead of a fixed
     * "x-echo". A test uses it to check the *value* that the server got for
     * a header it cares about, for example Host, where a count alone is not
     * enough. When the named header line is absent, the route answers with
     * an empty body, which is not the same answer as "not present". */
    char name[64] = {0};
    srv_find_header(raw, "x-echo-name", name, sizeof(name));
    char val[256] = {0};
    if (name[0]) srv_find_header(raw, name, val, sizeof(val));
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, val, strlen(val),
                false);
    return true;
  }

  if (strcmp(method, "DELETE") == 0 && strcmp(path, "/delete-echo") == 0) {
    /* Echoes the request body for DELETE; body_len is 0 when the client sends
     * no body. */
    srv_respond(conn_fd, 200, "OK", "application/octet-stream", NULL, body,
                body_len, false);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/keepalive") == 0) {
    /* This route sends no "Connection: close" and relies on the implicit
     * keep-alive default of HTTP/1.1, so that a test can exercise how the
     * client reuses a connection from its idle pool. */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/keepalive-then-close") == 0) {
    /* The response looks eligible for keep-alive, because it carries no
     * Connection: close header, but the server closes its own end at once
     * after it. This exercises how the client finds a dead idle connection
     * with its liveness probe on reuse. */
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return true;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/early-hints") == 0) {
    /* Sends "103 Early Hints" (RFC 8297) before the real final response, on
     * the SAME connection, in two SEPARATE send() calls. This exercises the
     * general response path of the client (the one that does not handle
     * Expect: 100-continue), which must discard an interim informational
     * response that it does not watch for and then go on reading for the
     * real response, instead of reading the 103 as the final answer. The
     * response is eligible for keep-alive, because it has no "Connection:
     * close", so a later request that reuses this same connection can
     * confirm that the connection is clean. */
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    send(conn_fd, hints, strlen(hints), 0);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                true);
    return false;
  }

  if (strcmp(method, "GET") == 0 && strcmp(path, "/endless-early-hints") == 0) {
    /* Sends many more interim "103 Early Hints" responses than the
     * CHTTP_MAX_INTERIM_RESPONSES cap of either tier (64), and only then
     * answers for real, which exercises that cap directly. Without the cap,
     * this route makes a caller thread (Tier 1) or a chain and its future
     * (Tier 2 and Tier 3) wait forever. A client that behaves correctly
     * gives up and tears down its own end of the connection long before
     * this loop stops, so the "close after" return value of this route is
     * never reached in practice. */
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    for (int i = 0; i < 100; i++) {
      if (send(conn_fd, hints, strlen(hints), 0) < 0) break;
    }
    return true;
  }

  if (strncmp(path, "/early-hints-count/", 19) == 0) {
    /* This route sends exactly N interim "103 Early Hints" responses and
     * THEN a real, final 200 response, taking N from the path, for example
     * "/early-hints-count/64". Unlike /endless-early-hints, which sends a
     * fixed 100 (more than the cap of each tier, whatever the exact cap
     * is), this route lets a test pin the EXACT boundary of
     * CHTTP_MAX_INTERIM_RESPONSES: N == 64 must succeed, because it is the
     * last legal discard, and N == 65 must fail with
     * ccol_http_transfer_aborted, because it is one discard more than the
     * documented cap. See the contract "after 64 consecutive discarded
     * interim responses" in chttpclient_do(3).
     *
     * This route finds an off-by-one difference between the tiers: Tier 2
     * and Tier 3 must not accept and discard 65 interim responses before
     * they stop, which is one more than the 64 of Tier 1 and silently
     * breaks the documented contract. */
    int n = atoi(path + 19);
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    for (int i = 0; i < n; i++) {
      if (send(conn_fd, hints, strlen(hints), 0) < 0) return true;
    }
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/early-hints-same-write") == 0) {
    /* The same as /early-hints, except that this route builds the interim
     * "103 Early Hints" response and the real final response into ONE
     * buffer and sends them with a SINGLE send() call, so they very
     * probably arrive in one chunk in the read() of the client. This
     * exercises the path that re-feeds the leftover bytes after the
     * boundary of a discarded interim message into the next read: Tier 1
     * does this with the carry_in argument of _chttp_read_message_loop, and
     * Tier 2 with the re-parse loop over data and data_len in
     * _async_on_readable_impl. The route /early-hints only covers the more
     * common case, where each message arrives in its own read. */
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nLink: </style.css>; rel=preload\r\n\r\n";
    const char *b = "{\"status\":\"ok\"}";
    char combined[512];
    int clen = snprintf(combined, sizeof(combined),
                        "%sHTTP/1.1 200 OK\r\n"
                        "Content-Type: application/json\r\n"
                        "Content-Length: %zu\r\n\r\n%s",
                        hints, strlen(b), b);
    if (clen > 0) send(conn_fd, combined, (size_t)clen, 0);
    return false;
  }

  if (strcmp(method, "GET") == 0 &&
      strcmp(path, "/early-hints-oversized-content-length") == 0) {
    /* A buggy or hostile server whose interim "103 Early Hints" response
     * carries a "Content-Length" that declares much more than the cap of
     * chttpclient_set_max_response_body_size, while the body of the REAL
     * final response is tiny and stays inside that cap. Such a
     * Content-Length breaks RFC 7230 SS3.3.2, but this client cannot trust
     * a peer to obey that rule.
     *
     * This gives regression coverage for one contract: the up-front
     * too-large check in _on_headers_complete must not fire for the
     * declared Content-Length of a discarded 1xx message. A check that
     * looks at the declared length of ANY message fails the whole request
     * with ccol_msg_too_large, even though the response that arrives stays
     * inside the cap. */
    const char *hints =
        "HTTP/1.1 103 Early Hints\r\nContent-Length: 999999\r\n\r\n";
    send(conn_fd, hints, strlen(hints), 0);
    const char *b = "{\"status\":\"ok\"}";
    srv_respond(conn_fd, 200, "OK", "application/json", NULL, b, strlen(b),
                false);
    return true;
  }

  if (strcmp(method, "HEAD") == 0 &&
      strcmp(path, "/head-oversized-content-length") == 0) {
    /* A HEAD response whose Content-Length describes what a GET gives back
     * (RFC 7231 SS4.3.2), with no body byte following it on the wire. This
     * gives regression coverage for one contract: the up-front too-large
     * check in _on_headers_complete must skip a HEAD response. Without
     * that, a large declared Content-Length here fails the whole request
     * with ccol_msg_too_large, although the client buffers no body for
     * HEAD. */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/304-oversized-content-length") == 0) {
    /* A 304 Not Modified that carries the Content-Length of the original
     * resource (RFC 7232 SS4.1), which is a real and common pattern for a
     * conditional GET against a CDN or a server of static assets. By RFC
     * 7230 SS3.3, no body byte follows it on the wire; 1xx and 204 obey the
     * same framing rule.
     *
     * This gives regression coverage for one contract: the up-front
     * too-large check in _on_headers_complete must skip 204 and 304 as well
     * as 1xx and HEAD. A check that skips only 1xx and HEAD fails the whole
     * request with ccol_msg_too_large for the large declared
     * Content-Length here. chttp1_parser itself already forces no_body for
     * a 304, whatever length the response declares. */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 304 Not Modified\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/204-oversized-content-length") == 0) {
    /* The same reason as for /304-oversized-content-length above, but for
     * 204 No Content, which is the pattern of an old or buggy server. RFC
     * 7230 SS3.3 forbids a body here just as strictly. */
    char header[256];
    int hlen = snprintf(header, sizeof(header),
                        "HTTP/1.1 204 No Content\r\n"
                        "Content-Length: 999999\r\nConnection: close\r\n\r\n");
    if (hlen > 0) send(conn_fd, header, (size_t)hlen, 0);
    return true;
  }

  if (strcmp(path, "/echo-method-body") == 0) {
    /* Echoes "<METHOD>:<body_len>", so that a test can check the policy for
     * the method and the body when the client follows a redirect: for 301,
     * 302 and 303 the next request is a GET with no body (except after a
     * HEAD), while for 307 and 308 the client keeps the method and the
     * body. */
    char b[64];
    int blen = snprintf(b, sizeof(b), "%s:%zu", method, body_len);
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, (size_t)blen, false);
    return true;
  }

  if (strcmp(path, "/redirect-301-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n", g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-307-to-echo") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-method-body\r\n", g_srv.port);
    srv_respond(conn_fd, 307, "Temporary Redirect", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-301-then-307-to-echo") == 0) {
    /* The first hop of a chain of two hops. The first hop is a redirect that
     * keeps nothing (it drops the body and turns the method into GET), and
     * a 307 or 308 follows it, which keeps the current method and the
     * current body. A test uses this route to check that the drop stays in
     * place for the LATER 307 hop, so that the 307 does not bring the body
     * of the ORIGINAL request back. See the comment on
     * chttp_async_chain_t.body_dropped in chttpclient.c for the defect that
     * this guards against on the Tier 2 and Tier 3 side. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-307-to-echo\r\n",
             g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-301-to-count-header") == 0) {
    /* Like /redirect-301-to-echo, but with /count-header as the target
     * instead of /echo-method-body. A test uses it to check that none of
     * three headers of the caller (Content-Length, Content-Type and Expect)
     * stays on the next request, because those headers describe the
     * ORIGINAL POST body and this redirect turns the request into a GET
     * with no body. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/count-header\r\n", g_srv.port);
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain", loc_hdr, NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-relative") == 0) {
    /* A Location that is relative to the root, with no scheme and no host,
     * which exercises the root-relative branch of _resolve_redirect_url. */
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: /get\r\n", NULL,
                0, false);
    return true;
  }

  if (strcmp(path, "/redirect-empty-location") == 0) {
    /* A redirect status with a Location header that is present but EMPTY. A
     * server can legally send this, because no rule asks for a non-empty
     * Location value, and the client delivers it as the final response. */
    srv_respond(conn_fd, 301, "Moved Permanently", "text/plain",
                "Location: \r\n", NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/sweep-race") == 0) {
    /* A keep-alive answer that waits until the test lets it go, for at most
     * ten seconds. */
    for (int i = 0; i < 10000 && !atomic_load(&g_sweep_race_respond); i++)
      usleep(1000);
    const char *b = "ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), true);
    return false;
  }

  if (strcmp(path, "/repeated-fields") == 0) {
    /* Three names that repeat, interleaved with each other and with a name
     * that occurs once. A Set-Cookie value holds a comma of its own, which
     * is why no reader may split a combined Set-Cookie on commas. */
    srv_respond(conn_fd, 200, "OK", "text/plain",
                "Set-Cookie: a=1; Path=/\r\n"
                "X-Multi: one\r\n"
                "Set-Cookie: b=2; Expires=Wed, 21 Oct 2037 07:28:00 GMT\r\n"
                "X-Single: s\r\n"
                "x-multi: two\r\n"
                "X-Empty-Twice: \r\n"
                "X-Multi: three\r\n"
                "X-Empty-Twice: \r\n",
                "ok", 2, false);
    return true;
  }

  if (strncmp(path, "/long-header-line/", 18) == 0 ||
      strncmp(path, "/long-header-hint/", 18) == 0 ||
      strncmp(path, "/long-header-redir/", 19) == 0) {
    /* One header line whose value is the decimal count of bytes that the
     * path names, followed by the same field again with a short value. The
     * hint variant sends a 103 with such a line first, and the redirect
     * variant answers 302 with such a line and names the plain variant of
     * the same size. */
    bool hint = strncmp(path, "/long-header-hint/", 18) == 0;
    bool redir = strncmp(path, "/long-header-redir/", 19) == 0;
    size_t n = (size_t)strtoul(path + (redir ? 19 : 18), NULL, 10);
    if (n > 200000) n = 200000;
    char *head = malloc(2 * n + 512);
    if (!head) return false;
    int off = 0;
    if (hint) {
      off = snprintf(head, 256, "HTTP/1.1 103 Early Hints\r\nX-Hint: ");
      memset(head + off, 'h', n);
      off += (int)n;
      off += snprintf(head + off, 16, "\r\n\r\n");
    }
    if (redir)
      off += snprintf(head + off, 256,
                      "HTTP/1.1 302 Found\r\nLocation: /long-header-line/"
                      "%zu\r\nContent-Length: 2\r\n"
                      "Connection: close\r\nX-Long: ",
                      n);
    else
      off += snprintf(head + off, 256,
                      "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                      "Connection: close\r\nX-Long: ");
    memset(head + off, 'v', n);
    size_t len = (size_t)off + n;
    static const char tail[] = "\r\nX-Long: tail\r\n\r\nok";
    memcpy(head + len, tail, sizeof(tail) - 1);
    len += sizeof(tail) - 1;
    size_t sent = 0;
    while (sent < len) {
      ssize_t w = send(conn_fd, head + sent, len - sent, MSG_NOSIGNAL);
      if (w <= 0) break;
      sent += (size_t)w;
    }
    free(head);
    return true;
  }

  if (strcmp(path, "/many-repeated-fields") == 0) {
    /* 44 names that each occur twice, with the second pass in reverse order,
     * and one name whose first occurrence is empty. Together with the three
     * fields that srv_respond adds, the block stays under the header count
     * cap of the parser. */
    char extra[1536];
    size_t off = 0;
    for (int pass = 0; pass < 2; pass++)
      for (int i = 0; i < 44; i++) {
        int n = pass == 0 ? i : 43 - i;
        off += (size_t)snprintf(extra + off, sizeof(extra) - off,
                                "X-R%02d: %c%d\r\n", n, pass ? 'b' : 'a', n);
      }
    snprintf(extra + off, sizeof(extra) - off, "X-E: \r\nX-E: z\r\n");
    srv_respond(conn_fd, 200, "OK", "text/plain", extra, "ok", 2, false);
    return true;
  }

  if (strcmp(path, "/redirect-two-locations") == 0) {
    /* A redirect with two Location fields: the first names /get, and the
     * second names a route that answers 404. */
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: /get\r\nLocation: /no-such-route\r\n", NULL, 0,
                false);
    return true;
  }

  if (strcmp(path, "/redirect-unsupported-scheme-location") == 0) {
    /* A Location value that is an absolute-URI reference whose scheme this
     * client does not know (the client knows only http, https and
     * http+unix). By RFC 3986 SS5.2.2, T = R whenever R has ANY scheme, so
     * _resolve_redirect_url must give the value back unchanged instead of
     * merging it onto the origin of this hop as though it were a relative
     * path. Without that, "mailto:test@example.com" resolves to
     * "http://<this origin>/mailto:test@example.com". The correct result is
     * an absolute URL that _parse_chttp_url of the next hop rejects with
     * ccol_http_invalid_url. */
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: mailto:test@example.com\r\n", NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-unix-socket") == 0 ||
      strcmp(path, "/redirect-to-unix-socket-mixed-case") == 0) {
    /* An ordinary TCP origin answers with a Location that names the AF_UNIX
     * transport and points at the Unix listener of this suite, which serves
     * "/get" without a problem. A client that obeys such a Location
     * connects to a filesystem path that the server chose, with a request
     * target that also comes from the server: server-side request forgery
     * against a local socket. The mixed-case spelling is a separate route
     * because every place that recognises the scheme compares it without
     * case, so a refusal that compares with case is no refusal. */
    char encoded[256];
    percent_encode_unix_path(encoded, sizeof(encoded), g_srv.unix_path);
    char loc_hdr[512];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: %s%s/get\r\n",
             strcmp(path, "/redirect-to-unix-socket") == 0 ? "http+unix://"
                                                           : "HtTp+UnIx://",
             encoded);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-then-redirect-to-unix-socket") == 0) {
    /* Hop 0 of a chain whose SECOND hop names a Unix socket. The client
     * decides for each hop which transport that hop can redirect to, judged
     * against the hop that the Location arrived on, so one more hop gives a
     * hostile server nothing. */
    char loc_hdr[256];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-to-unix-socket\r\n",
             g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-other-unix-socket") == 0) {
    /* A test reaches this route over the Unix listener. Since a Unix hop can
     * redirect only to its OWN socket, the client refuses a Location that
     * names a different socket path, just as it refuses one on a TCP hop. */
    srv_respond(
        conn_fd, 302, "Found", "text/plain",
        "Location: http+unix://%2Ftmp%2Fccol-not-this-socket.sock/get\r\n",
        NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/redirect-relative-dotted") == 0) {
    /* A relative Location with more than one level ("../../get"), which
     * exercises the merge of RFC 3986 SS5.3 and remove_dot_segments from end
     * to end, not only at the unit level. The base path
     * "/nested/dir/redirect-relative-dotted" merges with "../../get" to
     * "/nested/dir/../../get", which must normalise to "/get". */
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: ../../get\r\n",
                NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/redirect-relative-plain") == 0) {
    /* A plain relative Location ("sibling") with no leading "/" and no ".."
     * or "." segment: the simplest reference that still needs the merge of
     * RFC 3986 SS5.3. The base path "/nested/dir/redirect-relative-plain"
     * merges with "sibling" to "/nested/dir/sibling".
     *
     * This route pins how the async engine tracks path_and_query for each
     * hop: the chttp_url_t base that _async_handle_redirect builds must
     * carry a real path_and_query, because a NULL there crashes the whole
     * process on a strchr() call with a NULL pointer. Every Location value
     * that reaches _merge_ref_path hits this, which means every value that
     * is not an absolute URL, not a protocol-relative "//host/..." reference
     * and not an absolute-path "/..." reference. */
    srv_respond(conn_fd, 302, "Found", "text/plain", "Location: sibling\r\n",
                NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/nested/dir/sibling") == 0) {
    const char *b = "sibling-ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), false);
    return true;
  }

  if (strcmp(path, "/redirect-abs-path-query-with-slashes") == 0) {
    /* An absolute-path Location whose QUERY string, not its path, holds
     * "/../". The removal of dot segments in RFC 3986 SS5.2.4 must never
     * touch a query byte, so the absolute-path branch of
     * _resolve_redirect_url must split the query off before it calls
     * remove_dot_segments. With the whole "path?query" string, the "/../"
     * inside the query corrupts the resolved path: "/foo/bar?x=1/../2" then
     * resolves to "/foo/2", and the query is gone. The client must keep the
     * query byte for byte, so the request line of the next hop must land on
     * the exact route below, not on a path that dot-segment removal
     * changed. */
    srv_respond(conn_fd, 302, "Found", "text/plain",
                "Location: /query-preserved-target?x=1/../2\r\n", NULL, 0,
                false);
    return true;
  }

  if (strcmp(path, "/query-preserved-target?x=1/../2") == 0) {
    const char *b = "ok";
    srv_respond(conn_fd, 200, "OK", "text/plain", NULL, b, strlen(b), false);
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-same-origin") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/echo-auth\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-upper-localhost") == 0) {
    /* Redirects to the same server under the host spelled in upper case, so a
     * request that starts on http://localhost:<port> stays on its own
     * origin, because a host compares without regard to case. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://LOCALHOST:%d/echo-auth\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/partial-then-reset") == 0) {
    /* Sends the head of a response and 3 of its 100 body bytes, waits until
     * the client is blocked on the rest, and then resets the connection
     * (SO_LINGER with a zero timeout turns the close of the caller into an
     * RST). The client sees a socket error in the middle of reading an
     * established response. */
    const char *part = "HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\nabc";
    ssize_t w = send(conn_fd, part, strlen(part), MSG_NOSIGNAL);
    (void)w;
    usleep(200 * 1000);
    struct linger lg = {.l_onoff = 1, .l_linger = 0};
    setsockopt(conn_fd, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
    return true;
  }

  if (strcmp(path, "/redirect-to-echo-auth-cross-origin") == 0) {
    /* Redirects to the IPv6 loopback listener. This is only an easy way to
     * get a different origin, with a different host, for the same server
     * process; it does not test IPv6 itself. A test that needs this route
     * skips itself when the IPv6 listener never bound (see
     * get_test_port6). */
    char loc_hdr[160];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://[::1]:%d/echo-auth\r\n", g_srv.port6);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-chain-1") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-chain-2\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-chain-2") == 0) {
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr), "Location: http://127.0.0.1:%d/get\r\n",
             g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/redirect-infinite") == 0) {
    /* This route always redirects to itself, to exercise the
     * CHTTP_MAX_REDIRECTS cap: Tier 1 and the async engine must both stop
     * after the cap and report ccol_http_too_many_redirects, instead of
     * looping forever or giving the last 302 to the caller as an ordinary
     * response. */
    char loc_hdr[128];
    snprintf(loc_hdr, sizeof(loc_hdr),
             "Location: http://127.0.0.1:%d/redirect-infinite\r\n", g_srv.port);
    srv_respond(conn_fd, 302, "Found", "text/plain", loc_hdr, NULL, 0, false);
    return true;
  }

  if (strcmp(path, "/chunked-body") == 0) {
    /* This route writes raw bytes on purpose, instead of using srv_respond,
     * which always sets Content-Length. It sends a real response with
     * chunked transfer encoding, in two data chunks plus the final chunk of
     * zero length, and it sends no "Connection: close", because chunked
     * framing carries its own end marker; the connection therefore stays
     * eligible for keep-alive and the client can reuse it.
     *
     * Because this is the only route that sends the client a real chunked
     * RESPONSE, it is the only end-to-end coverage, on either tier, of
     * three things on the client side: the assembly of more than one
     * chunk, keep-alive after a chunked body, and the reactive
     * max_response_body_size check on a chunked body. Other tests cover
     * chunked REQUEST bodies, and tests_parser.c covers the chunked decoder
     * of chttp1_parser on its own. */
    const char *raw =
        "HTTP/1.1 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "Transfer-Encoding: chunked\r\n"
        "\r\n"
        "7\r\n"
        "Hello, \r\n"
        "e\r\n"
        "chunked world!\r\n"
        "0\r\n"
        "\r\n";
    send(conn_fd, raw, strlen(raw), 0);
    return false;
  }

  if (strcmp(path, "/eof-delimited-body") == 0) {
    /* This route writes raw bytes on purpose, instead of using srv_respond,
     * which always sets Content-Length. The body ends at the EOF: there is
     * no Content-Length and no Transfer-Encoding, and only the close of the
     * connection marks the end, which is how a real HTTP/1.0 server
     * answers, and also a server that sends Connection: close with no
     * explicit length. This route gives regression coverage for one
     * contract: a valid completion at an EOF must be reported as
     * ccol_success with the body intact, not as ccol_http_transfer_aborted.
     */
    const char *raw =
        "HTTP/1.0 200 OK\r\n"
        "Content-Type: text/plain\r\n"
        "\r\n"
        "eof-delimited-body-ok";
    send(conn_fd, raw, strlen(raw), 0);
    return true;
  }

  /* 404 for everything else. */
  const char *b = "not found";
  srv_respond(conn_fd, 404, "Not Found", "text/plain", NULL, b, strlen(b),
              false);
  return true;
}

/*
 * The handler thread for one connection. It loops and reads more requests
 * from the same fd for as long as srv_handle_route says that the
 * connection must stay open (the keep-alive routes). The loop has a bound,
 * so a client that behaves badly can never hold this thread open forever.
 */
/*
 * A route that knows about Expect: 100-continue. It answers 200 at once
 * with no "100 Continue" and holds the end of the body back for longer than
 * CHTTP_100_CONTINUE_WAIT_US, so the window of the client ends while the
 * final response is half read. The route then counts every byte that the
 * client sends after the headers of its request, until the client closes
 * or two seconds pass, and publishes the count before it marks itself
 * done. The body of the request must never arrive: the answer came without
 * a "100 Continue", so it is a direct final response.
 */
static _Atomic int g_slow_final_body_bytes = 0;
static _Atomic int g_slow_final_done = 0;

static bool srv_handle_expect_continue_slow_final_route(int conn_fd,
                                                        bool chunked) {
  const char *head =
      chunked ? "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
                "5\r\nhello\r\n"
              : "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  const char *tail = chunked ? "5\r\nworld\r\n0\r\n\r\n" : "world";
  send(conn_fd, head, strlen(head), MSG_NOSIGNAL);
  usleep(1300000); /* > CHTTP_100_CONTINUE_WAIT_US (1000ms) */
  send(conn_fd, tail, strlen(tail), MSG_NOSIGNAL);
  int seen = 0;
  for (;;) {
    struct pollfd p = {.fd = conn_fd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 2000) <= 0) break;
    char b[256];
    ssize_t m = recv(conn_fd, b, sizeof(b), 0);
    if (m <= 0) break;
    seen += (int)m;
  }
  atomic_fetch_add(&g_slow_final_body_bytes, seen);
  atomic_fetch_add(&g_slow_final_done, 1);
  return true;
}

#define MAX_KEEPALIVE_REQUESTS_PER_CONN 100

static void *srv_conn_thread(void *arg) {
  int conn_fd = (int)(intptr_t)arg;
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (!buf) {
    close(conn_fd);
    return NULL;
  }

  for (int iter = 0; iter < MAX_KEEPALIVE_REQUESTS_PER_CONN; iter++) {
    size_t hdr_len = 0;
    ssize_t n = srv_read_headers(conn_fd, buf, TEST_SERVER_BUF, &hdr_len);
    if (n <= 0) break;

    char method[16], path[512];
    srv_parse_request_line(buf, method, sizeof(method), path, sizeof(path));

    bool close_after;
    if (strcmp(path, "/expect-continue-echo") == 0) {
      close_after = srv_handle_expect_continue_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, true);
    } else if (strcmp(path, "/expect-continue-reject") == 0) {
      close_after = srv_handle_expect_continue_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, false);
    } else if (strcmp(path, "/expect-continue-slow-final") == 0) {
      close_after = srv_handle_expect_continue_slow_final_route(conn_fd, false);
    } else if (strcmp(path, "/expect-continue-slow-final-chunked") == 0) {
      close_after = srv_handle_expect_continue_slow_final_route(conn_fd, true);
    } else if (strcmp(path, "/expect-continue-timeout-then-die") == 0) {
      close_after = srv_handle_expect_continue_timeout_then_die_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-with-hints") == 0) {
      close_after = srv_handle_expect_continue_with_hints_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strcmp(path, "/expect-continue-reject-keepalive") == 0) {
      close_after = srv_handle_expect_continue_reject_keepalive_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-fake-final-then-die") == 0) {
      close_after =
          srv_handle_expect_continue_fake_final_then_die_route(conn_fd);
    } else if (strcmp(path, "/expect-continue-die-after-100-clean") == 0) {
      close_after = srv_handle_expect_continue_die_after_100_clean_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strcmp(path, "/expect-continue-bundled-final") == 0) {
      close_after = srv_handle_expect_continue_bundled_final_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len);
    } else if (strncmp(path, "/expect-continue-hints-both-sides/",
                       strlen("/expect-continue-hints-both-sides/")) == 0) {
      close_after = srv_handle_expect_continue_hints_both_sides_route(
          conn_fd, buf, TEST_SERVER_BUF, (size_t)n, hdr_len, path);
    } else {
      /* An ordinary route. This code reads the rest of the body, if there is
       * one, up to the content-length, so that the route sees the complete
       * request as though the headers and the body arrived in one read. */
      size_t total = (size_t)n;
      char cl_str[32] = {0};
      long cl = 0;
      if (srv_find_header(buf, "content-length", cl_str, sizeof(cl_str)))
        cl = atol(cl_str);
      while (cl > 0 && total - hdr_len < (size_t)cl &&
             total < TEST_SERVER_BUF - 1) {
        ssize_t m = recv(conn_fd, buf + total, TEST_SERVER_BUF - 1 - total, 0);
        if (m <= 0) break;
        total += (size_t)m;
        buf[total] = '\0';
      }

      char *body = (cl > 0) ? buf + hdr_len : NULL;
      size_t body_len = (cl > 0) ? (size_t)cl : 0;
      close_after =
          srv_handle_route(conn_fd, method, path, buf, body, body_len);
    }
    if (close_after) break;
  }
  free(buf);
  close(conn_fd);
  return NULL;
}

/* The accept loop of the server, which runs in a background thread. */
static void *srv_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = accept(g_srv.server_fd, NULL, NULL);
    if (conn_fd < 0) break;
    atomic_fetch_add(&g_accept_count, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

/* The IPv6-loopback counterpart of srv_accept_loop. It shares
 * g_srv.running, so both loops stop together, and it shares the registry of
 * connection threads and the handler too; only the fd that it listens on
 * is different. */
static void *srv_accept_loop6(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = accept(g_srv.server_fd6, NULL, NULL);
    if (conn_fd < 0) break;
    atomic_fetch_add(&g_accept_count, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

/* The Unix-domain-socket counterpart of srv_accept_loop. It shares
 * g_srv.running, the registry of connection threads and the handler; only
 * the fd that it listens on and the address family are different. */
static void *srv_accept_loop_unix(void *arg) {
  (void)arg;
  while (atomic_load(&g_srv.running)) {
    int conn_fd = accept(g_srv.server_fd_unix, NULL, NULL);
    if (conn_fd < 0) break;
    atomic_fetch_add(&g_accept_count, 1);

    pthread_t tid;
    if (pthread_create(&tid, NULL, srv_conn_thread,
                       (void *)(intptr_t)conn_fd) != 0) {
      close(conn_fd);
    } else {
      register_conn_thread(tid);
    }
  }
  return NULL;
}

static void start_test_server(void) {
  g_srv.server_fd6 = -1;
  g_srv.server_fd_unix = -1;

  g_srv.server_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_srv.server_fd < 0) return;

  int opt = 1;
  setsockopt(g_srv.server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons(TEST_SERVER_PORT);
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

  if (bind(g_srv.server_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(g_srv.server_fd, 64) != 0) {
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    return;
  }

  /* Retrieve the assigned port. */
  socklen_t len = sizeof(addr);
  getsockname(g_srv.server_fd, (struct sockaddr *)&addr, &len);
  g_srv.port = ntohs(addr.sin_port);
  atomic_store(&g_srv.running, 1);

  if (pthread_create(&g_srv.accept_tid, NULL, srv_accept_loop, NULL) != 0) {
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    atomic_store(&g_srv.running, 0);
    return;
  }

  /* A best-effort IPv6 loopback listener. Some sandboxes and CI environments
   * have no IPv6 stack, and then this whole block leaves server_fd6 at -1
   * and port6 at 0. A test that needs the listener checks for that and
   * skips itself instead of failing. */
  int fd6 = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd6 >= 0) {
    int opt6 = 1;
    setsockopt(fd6, SOL_SOCKET, SO_REUSEADDR, &opt6, sizeof(opt6));

    struct sockaddr_in6 addr6;
    memset(&addr6, 0, sizeof(addr6));
    addr6.sin6_family = AF_INET6;
    addr6.sin6_port = htons(TEST_SERVER_PORT);
    addr6.sin6_addr = in6addr_loopback;

    if (bind(fd6, (struct sockaddr *)&addr6, sizeof(addr6)) == 0 &&
        listen(fd6, 64) == 0) {
      socklen_t len6 = sizeof(addr6);
      getsockname(fd6, (struct sockaddr *)&addr6, &len6);
      g_srv.server_fd6 = fd6;
      g_srv.port6 = ntohs(addr6.sin6_port);
      if (pthread_create(&g_srv.accept_tid6, NULL, srv_accept_loop6, NULL) !=
          0) {
        close(fd6);
        g_srv.server_fd6 = -1;
        g_srv.port6 = 0;
      }
    } else {
      close(fd6);
    }
  }

  /* The Unix domain socket listener. The OS assigns the ports of the TCP
   * listeners, but this path is fixed; it comes from getpid(), so two test
   * processes on one machine cannot collide. This code first unlinks any
   * stale file at the same path. */
  snprintf(g_srv.unix_path, sizeof(g_srv.unix_path),
           "/tmp/chttpclient_test_%d.sock", (int)getpid());
  unlink(g_srv.unix_path);

  int fd_unix = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd_unix >= 0) {
    struct sockaddr_un addr_un;
    memset(&addr_un, 0, sizeof(addr_un));
    addr_un.sun_family = AF_UNIX;
    size_t path_len = strlen(g_srv.unix_path);
    bool bound = false;
    /* This code uses memcpy under an explicit length check instead of
     * snprintf, because the check proves that the copy fits, and the
     * -Wformat-truncation warning of gcc cannot see that proof through a
     * "%s" format: it only knows the fixed 108-byte size of sun_path and
     * the much larger size of the unix_path buffer. A path from the fixed
     * "/tmp/chttpclient_test_<pid>.sock" format is short and never fails
     * this check in practice. */
    if (path_len < sizeof(addr_un.sun_path)) {
      memcpy(addr_un.sun_path, g_srv.unix_path, path_len + 1);
      bound =
          (bind(fd_unix, (struct sockaddr *)&addr_un, sizeof(addr_un)) == 0 &&
           listen(fd_unix, 64) == 0);
    }
    if (bound) {
      g_srv.server_fd_unix = fd_unix;
      if (pthread_create(&g_srv.accept_tid_unix, NULL, srv_accept_loop_unix,
                         NULL) != 0) {
        close(fd_unix);
        g_srv.server_fd_unix = -1;
        unlink(g_srv.unix_path);
      }
    } else {
      close(fd_unix);
    }
  }
}

__attribute__((destructor)) static void stop_test_server(void) {
  if (g_srv.server_fd > 0) {
    atomic_store(&g_srv.running, 0);
    /* On Linux, shutdown() makes a blocked accept() return with EINVAL, while
     * close() alone does not reliably unblock it. */
    shutdown(g_srv.server_fd, SHUT_RDWR);
    close(g_srv.server_fd);
    g_srv.server_fd = -1;
    pthread_join(g_srv.accept_tid, NULL);
  }
  if (g_srv.server_fd6 > 0) {
    shutdown(g_srv.server_fd6, SHUT_RDWR);
    close(g_srv.server_fd6);
    g_srv.server_fd6 = -1;
    pthread_join(g_srv.accept_tid6, NULL);
  }
  if (g_srv.server_fd_unix > 0) {
    shutdown(g_srv.server_fd_unix, SHUT_RDWR);
    close(g_srv.server_fd_unix);
    g_srv.server_fd_unix = -1;
    pthread_join(g_srv.accept_tid_unix, NULL);
    unlink(g_srv.unix_path);
  }
  /* Joins every connection-handler thread, so that a sanitizer can account
   * for each allocation on their stacks and no thread stays alive. */
  pthread_mutex_lock(&g_conn_mutex);
  int n = g_conn_thread_count;
  pthread_mutex_unlock(&g_conn_mutex);
  for (int i = 0; i < n; i++) pthread_join(g_conn_threads[i], NULL);
}

static int get_test_port(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port;
}

/* Gives 0 when no IPv6 loopback listener could bind in this environment. A
 * test that needs the listener must check for that and skip itself. */
static int get_test_port6(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.port6;
}

/* The number of TCP connections that the test server has accepted so far.
 * A test uses it to assert that keep-alive reuse skipped the handshake and
 * the TCP setup for a request instead of opening a fresh connection. */
static int test_server_accept_count(void) {
  return atomic_load(&g_accept_count);
}

/* Builds a URL for the test server: http://127.0.0.1:<port><path>. */
static void make_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://127.0.0.1:%d%s", get_test_port(), path);
}

/* Builds an IPv6-loopback URL for the test server:
 * http://[::1]:<port><path>. Call it only after checking that
 * get_test_port6() is not 0. */
static void make_url6(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "http://[::1]:%d%s", get_test_port6(), path);
}

/* The path (not a URL) of the Unix domain socket that the test server
 * listens on, or "" when no listener could bind in this environment.
 * AF_UNIX is always present on any POSIX target that this library
 * supports, so this is not a best-effort case that a test skips when it is
 * absent, as IPv6 is. */
static const char *get_test_unix_socket_path(void) {
  pthread_once(&g_srv_once, start_test_server);
  return g_srv.unix_path;
}

/* Percent-encodes a raw filesystem path into the authority part of a
 * "http+unix://<encoded-path>" URL, with the encoding that
 * _parse_chttp_unix_url at the other end expects: an unreserved character
 * of RFC 3986 passes through unchanged, and every other byte, '/'
 * included, becomes %XX. This is the test-side counterpart of the internal
 * _percent_encode_unix_path of chttpclient.c, which is static, so the two
 * cannot share one definition. */
static void percent_encode_unix_path(char *buf, size_t buf_size,
                                     const char *raw) {
  size_t w = 0;
  for (const char *p = raw; *p && w + 4 < buf_size; p++) {
    unsigned char c = (unsigned char)*p;
    if (isalnum(c) || c == '-' || c == '.' || c == '_' || c == '~') {
      buf[w++] = (char)c;
    } else {
      snprintf(buf + w, 4, "%%%02X", c);
      w += 3;
    }
  }
  buf[w] = '\0';
}

/* Builds a "http+unix://<encoded-path><path>" URL for the Unix domain
 * socket test server. */
static void make_unix_url(char *buf, size_t buf_size, const char *path) {
  char encoded[256];
  percent_encode_unix_path(encoded, sizeof(encoded),
                           get_test_unix_socket_path());
  snprintf(buf, buf_size, "http+unix://%s%s", encoded, path);
}

/* ========================================================================== */
/*                     HELPER UTILITIES                                       */
/* ========================================================================== */

/* ========================================================================== */
/*                     REQUEST CONSTRUCTION TESTS                             */
/* ========================================================================== */

TEST(request, new_copies_url_and_body) {
  const char *json = "{\"x\":1}";
  chttp_request_body_t body = CHTTP_JSON_BODY(json, strlen(json));

  chttp_request_t *req =
      chttp_request_new(CHTTP_POST, "http://example.com/api", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_STREQ(req->url, "http://example.com/api");
  REQUIRE_EQ(req->method, CHTTP_POST);
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, strlen(json));
  REQUIRE_STREQ(req->body.content_type, "application/json");
  /* Checks that this is a separate copy. */
  REQUIRE_NE((void *)req->body.data, (void *)json);

  chttp_request_free(req);
}

TEST(request, new_null_body) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  chttp_request_free(req);
}

TEST(request, new_no_body_macro) {
  chttp_request_body_t nb = CHTTP_NO_BODY;
  chttp_request_t *req =
      chttp_request_new(CHTTP_DELETE, "http://example.com/r", &nb, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  chttp_request_free(req);
}

TEST(request, new_null_url_fails) {
  char *err = NULL;
  chttp_request_t *req =
      chttp_request_new_mp(CHTTP_GET, NULL, NULL, NULL, &err);
  REQUIRE_EQ((void *)req, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(request, set_and_get_header) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval_t rv =
      chttp_request_set_header(req, "Authorization", "Bearer token123");
  REQUIRE_EQ(rv, ccol_success);

  const char *val = chttp_request_get_header(req, "Authorization");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "Bearer token123");

  chttp_request_free(req);
}

TEST(request, header_lookup_case_insensitive) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "Content-Type", "application/json");

  /* Looks the header up with different letter cases. */
  REQUIRE_NE((void *)chttp_request_get_header(req, "content-type"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "CONTENT-TYPE"), NULL);
  REQUIRE_NE((void *)chttp_request_get_header(req, "Content-Type"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "content-type"),
                "application/json");

  chttp_request_free(req);
}

TEST(request, set_header_overwrites) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "X-Custom", "first");
  chttp_request_set_header(req, "X-Custom", "second");

  const char *val = chttp_request_get_header(req, "x-custom");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "second");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_crlf_in_name) {
  /* A regression test: chttp_request_set_header must check the name and the
   * value instead of storing them unchecked. _serialize_request writes
   * "name: value\r\n" onto the wire and escapes nothing, so a name that
   * holds a CR or an LF adds an extra header line before the real value,
   * or splits the request into two. The function must reject such a name
   * and leave the headers of the request intact. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "x-evil\r\nx-injected", "1"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-evil\r\nx-injected"),
             NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-injected"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_crlf_in_value) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "x-evil", "1\r\nx-injected: evil"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "x-evil"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_empty_name) {
  /* An empty name has no valid form on the wire, and the rejection must
   * leave the headers of the request intact. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "", "v"), ccol_invalid_args);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_non_tchar_name) {
  /* chttp_request_set_header must reject a header NAME that holds a byte
   * outside the tchar set of RFC 7230 SS3.2.6; a check for CR and LF alone
   * is not enough. A space or a colon cannot inject a CRLF, but it still
   * makes a malformed "name: value\r\n" line on the wire, which a strict
   * server or proxy downstream can read wrongly. The check on the server
   * side, in chttpsvr_resp_set_header in chttpserver.c, is the same. A real
   * header that uses every non-alphanumeric tchar byte of RFC 7230 SS3.2.6
   * must still work after both rejections. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ(chttp_request_set_header(req, "X Foo", "bar"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "X:Foo", "bar"), ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X Foo"), NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X:Foo"), NULL);

  REQUIRE_EQ(chttp_request_set_header(req, "x-legit!#$%&'*+-.^_`|~", "fine"),
             ccol_success);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit!#$%&'*+-.^_`|~"),
                "fine");

  chttp_request_free(req);
}

TEST(request, set_header_rejects_transfer_encoding) {
  /* A regression test. chttpclient never applies a transfer coding to a
   * request body: it always sends a request with a body whole, framed by
   * Content-Length, so it can never obey a "Transfer-Encoding" header that
   * the caller sets. _serialize_request adds a Content-Length header
   * whenever the request has no explicit content-length header, so a
   * Transfer-Encoding header that the library accepts ends up beside a
   * Content-Length header, over a body with no transfer coding, which is
   * ambiguous framing. The function must reject such a header, comparing
   * the name without case, and leave the headers of the request intact. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "x-legit", "fine");
  REQUIRE_EQ(chttp_request_set_header(req, "Transfer-Encoding", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "transfer-encoding", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "TRANSFER-ENCODING", "chunked"),
             ccol_invalid_args);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "transfer-encoding"), NULL);
  REQUIRE_STREQ(chttp_request_get_header(req, "x-legit"), "fine");

  chttp_request_free(req);
}

TEST(request, new_null_body_data_with_nonzero_len_fails) {
  /* A regression test: chttp_request_new_mp must reject a NULL body->data
   * next to a body->len that is not zero, instead of reading that pair as
   * "no body". The block that copies the body runs only when body->data is
   * not NULL, and chttp_base64_encode_mp rejects the same pair of a NULL
   * data pointer and a non-zero length. A caller with a real bug (a wrongly
   * computed length next to a null buffer) needs a failure that it can
   * diagnose, not a request that silently goes out with no body. */
  chttp_request_body_t bad_body = {
      .data = NULL, .len = 5, .content_type = NULL};
  char *err = NULL;
  chttp_request_t *req = chttp_request_new_mp(CHTTP_POST, "http://example.com/",
                                              &bad_body, NULL, &err);
  REQUIRE_EQ((void *)req, NULL);
  REQUIRE_NE((void *)err, NULL);
}

TEST(request, get_missing_header_returns_null) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)chttp_request_get_header(req, "X-Missing"), NULL);
  chttp_request_free(req);
}

TEST(request, headers_begin_iterator) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttp_request_set_header(req, "X-A", "1");
  chttp_request_set_header(req, "X-B", "2");
  chttp_request_set_header(req, "X-C", "3");

  int count = 0;

  ccol_for_each(req->headers, it, {
    REQUIRE_NE(*ccol_iter_key_ptr(it), NULL);
    REQUIRE_NE(*ccol_iter_val_ptr(it), NULL);
    count++;
  });

  REQUIRE_EQ(count, 3);

  chttp_request_free(req);
}

TEST(request, headers_begin_empty_returns_null) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)ccol_begin(req->headers), NULL);
  chttp_request_free(req);
}

TEST(request, free_null_safe) { chttp_request_free(NULL); }

/* ========================================================================== */
/*                     CLIENT CONSTRUCTION TESTS                              */
/* ========================================================================== */

TEST(client_construction, create_and_destroy) {
  char *err = NULL;
  chttpcli cli = ccol_create_chttpclient(&err);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttpclient_destroy(cli);
  REQUIRE_EQ(cli, CHTTPCLI_INVALID);
}

TEST(client_construction, construct_macro) {
  chttpcli_construct(cli);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttpclient_destroy(cli);
}

TEST(client_construction, construct_scoped_macro) {
  {
    chttpcli_construct_scoped(cli);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
  }
  /* The scope exit destroys cli by itself. There is nothing to assert here,
   * but valgrind checks the result. */
}

TEST(client_construction, declare_and_init) {
  chttpcli_declare(cli);
  char *err = NULL;
  cli = ccol_create_chttpclient(&err);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttpclient_destroy(cli);
}

TEST(client_construction, set_configuration) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 5000000), ccol_success);
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 30000000), ccol_success);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);
  chttpclient_destroy(cli);
}

TEST(client_construction, null_args) {
  REQUIRE_EQ(chttpclient_set_pool_size(CHTTPCLI_INVALID, 4), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_connect_timeout(CHTTPCLI_INVALID, 0),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_request_timeout(CHTTPCLI_INVALID, 0),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_set_tls(CHTTPCLI_INVALID, NULL), ccol_invalid_args);
}

TEST(client_construction, half_configured_client_cert_pair_rejected) {
  /* A regression test. cert_path and key_path belong together: the
   * have_cert_pair check in _rebuild_tls_ctx_locked needs BOTH of them
   * before it loads a client certificate. Without a rejection, a
   * configuration with exactly one of the two means "no client
   * certificate", so chttpclient_set_tls reports ccol_success and every
   * later "mTLS" request connects without a client certificate and reports
   * no error. The function must reject both directions: a certificate with
   * no key, and a key with no certificate. */
  chttpcli_construct(cli);

  chttp_tls_config_t cert_only = CHTTP_TLS_DEFAULT;
  cert_only.cert_path = "/nonexistent/cert.pem";
  REQUIRE_EQ(chttpclient_set_tls(cli, &cert_only), ccol_invalid_args);

  chttp_tls_config_t key_only = CHTTP_TLS_DEFAULT;
  key_only.key_path = "/nonexistent/key.pem";
  REQUIRE_EQ(chttpclient_set_tls(cli, &key_only), ccol_invalid_args);

  /* A complete pair must still be accepted: the files do not exist, but the
   * library checks that they are readable only later. This confirms that
   * the rejection above catches exactly one half of the pair, instead of
   * broadly rejecting every certificate configuration. */
  chttp_tls_config_t both = CHTTP_TLS_DEFAULT;
  both.cert_path = "/nonexistent/cert.pem";
  both.key_path = "/nonexistent/key.pem";
  REQUIRE_EQ(chttpclient_set_tls(cli, &both), ccol_success);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     HTTP TRAFFIC TESTS                                     */
/* ========================================================================== */

TEST(http, get_200) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  chttpclient_resp_free(resp);
}

TEST(http, eof_delimited_body_without_content_length) {
  char url[128];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "eof-delimited-body-ok");
  chttpclient_resp_free(resp);
}

TEST(http, post_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"hello\":\"world\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(http, post_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

TEST(http, put_200) {
  char url[128];
  make_url(url, sizeof(url), "/put");

  const char *data = "updated";
  chttp_request_body_t body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_put(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, delete_204) {
  char url[128];
  make_url(url, sizeof(url), "/delete");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_delete(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 204);
  chttpclient_resp_free(resp);
}

TEST(http, patch_echos_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  const char *data = "patch-data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(data, strlen(data));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_patch(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, data);
  chttpclient_resp_free(resp);
}

TEST(http, patch_null_body) {
  char url[128];
  make_url(url, sizeof(url), "/patch");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_patch(url, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

TEST(http, response_headers) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *val = chttpclient_resp_header(resp, "X-Chttp-Test");
  REQUIRE_NE((void *)val, NULL);
  REQUIRE_STREQ(val, "hello");

  chttpclient_resp_free(resp);
}

TEST(http, response_header_lookup_case_insensitive) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);

  /* The server sends X-Chttp-Test. The library stores the name in lowercase,
   * so a lookup with any letter case finds it. */
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "x-chttp-test"), NULL);
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "X-CHTTP-TEST"), NULL);
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "X-Chttp-Test"), NULL);

  chttpclient_resp_free(resp);
}

TEST(http, request_headers_forwarded) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Echo", "test-value-42");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "test-value-42");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, not_found_404) {
  char url[128];
  make_url(url, sizeof(url), "/no-such-route");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(http, status_route_custom_code) {
  char url[128];
  make_url(url, sizeof(url), "/status/418");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 418);
  chttpclient_resp_free(resp);
}

TEST(http, follows_redirect) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  /* chttpclient_do follows the 301 to /get, which answers with 200. */
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, redirect_does_not_leak_intermediate_headers) {
  char url[128];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  /* The 301 response carries a Location header, while the final response
   * after the redirect is a 200 from /get with no Location header. The
   * client must reset the map of headers between two responses; without
   * that, "location" passes from the intermediate 301 into the final
   * response. */
  REQUIRE_EQ((void *)chttpclient_resp_header(resp, "location"), NULL);

  /* The final response must still expose its own headers. */
  REQUIRE_NE((void *)chttpclient_resp_header(resp, "content-type"), NULL);

  chttpclient_resp_free(resp);
}

TEST(http, redirect_with_empty_location_header_reported_cleanly) {
  /* The Location header of a redirect can be present but empty. An empty
   * value names no target, so the client does not follow it and delivers
   * the 3xx itself as the final response, as it does for a 3xx with no
   * Location at all. Any server that this client talks to can send this,
   * and the run under valgrind checks that the path frees each URL once. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-empty-location");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 301);
  chttpclient_resp_free(resp);
}

TEST(http, redirect_to_unsupported_scheme_location_reports_invalid_url) {
  /* An end-to-end regression test, beyond the unit level of
   * _resolve_redirect_url, which
   * relative_redirects.location_with_unrecognized_scheme_resolves_absolute
   * below covers. A redirect can carry a Location that is an absolute-URI
   * reference with a scheme that this client does not support; by RFC 3986
   * SS5.2.2, T = R whenever R has ANY scheme. The client must report
   * ccol_http_invalid_url for it, which is the same code that an
   * unsupported scheme in the ORIGINAL request URL already gets (see
   * error_codes.unsupported_scheme_returns_invalid_url), instead of reading
   * the Location as a same-origin relative path and following it to a
   * nonsense URL on this server. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-unsupported-scheme-location");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(http, default_client_convenience) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli def = chttp_default_client();
  REQUIRE_NE(def, CHTTPCLI_INVALID);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(def, NULL, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  rv = chttpclient_do(def, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, custom_client_with_pool_size) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     STREAMING RESPONSE TEST                                */
/* ========================================================================== */

typedef struct {
  char buf[4096];
  size_t len;
} stream_sink_t;

static size_t stream_sink_write(const void *data, size_t len, void *ctx) {
  stream_sink_t *s = (stream_sink_t *)ctx;
  size_t copy = len;
  if (s->len + copy >= sizeof(s->buf) - 1) copy = sizeof(s->buf) - 1 - s->len;
  memcpy(s->buf + s->len, data, copy);
  s->len += copy;
  s->buf[s->len] = '\0';
  return len;
}

TEST(http, streaming_response) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  int status = 0;
  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

TEST(http, streaming_through_redirect_delivers_final_body_not_intermediate) {
  /* The Tier 1 counterpart of
   * async_streaming.redirect_final_body_delivered_not_intermediate.
   * chttpclient_do_streaming and chttpclient_do_async_streaming share the
   * code in _on_headers_complete that chooses the sink, but their hop loops
   * are separate (chttp_do_internal against _async_submit_hop), so this
   * exact combination of Tier 1, streaming and a redirect needs its own
   * coverage. Every other chttpclient_do_streaming test in this file uses a
   * route that does not redirect. The intermediate 301 of
   * /redirect-with-body carries a real body with a name that nothing else
   * uses, so this test catches a client that gives that body to the sink
   * instead of to _sink_discard. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  int status = 0;
  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_STREQ(sink.buf, "{\"status\":\"ok\"}");

  chttp_request_free(req);
}

TEST(http, streaming_null_args) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  REQUIRE_EQ(chttpclient_do_streaming(CHTTPCLI_INVALID, req, stream_sink_write,
                                      &sink, &status),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_streaming(chttp_default_client(), NULL,
                                      stream_sink_write, &sink, &status),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_streaming(chttp_default_client(), req, NULL, &sink,
                                      &status),
             ccol_invalid_args);
  chttp_request_free(req);
}

/* ========================================================================== */
/*                     RESPONSE HEADER ITERATION TEST                         */
/* ========================================================================== */

TEST(http, response_header_iteration) {
  char url[128];
  make_url(url, sizeof(url), "/headers");

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);

  int count = 0;
  bool found_test_header = false;

  ccol_for_each(resp->headers, it, {
    REQUIRE_NE(*ccol_iter_key_ptr(it), NULL);
    REQUIRE_NE(*ccol_iter_val_ptr(it), NULL);
    if (strcmp(*ccol_iter_key_ptr(it), "x-chttp-test") == 0 &&
        strcmp(*ccol_iter_val_ptr(it), "hello") == 0)
      found_test_header = true;
    count++;
  });

  REQUIRE_TRUE(found_test_header);
  REQUIRE_GT(count, 0);

  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     CONCURRENT REQUESTS TEST                               */
/* ========================================================================== */

typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval_t result_rv;
} concurrent_req_arg_t;

static void *concurrent_req_thread(void *arg) {
  concurrent_req_arg_t *a = (concurrent_req_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }

  chttpcli_response *resp = NULL;
  a->result_rv = chttpclient_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

/*
 * probe_arg_t and probe_thread, which the test
 * "not_permitted_when_destroying" uses to synchronise a caller that races
 * __chttpclient_destroy. The probe sets ready=1 directly before the call to
 * chttpclient_do, not after it enters that call, so the window between the
 * signal and the call is small but not zero. After ready=1 the probe takes
 * one of two paths. (a) It blocks in the ccol_cond_var_wait or the
 * ccol_cond_var_timedwait of _slot_acquire, because another thread uses
 * both pool slots, and the broadcast of the destroy wakes it. (b) It sees
 * destroying=true at the fast-path guard, because the destroy thread won
 * the race. Both paths return ccol_not_permitted, so the test is correct
 * for either one.
 */
typedef struct {
  chttpcli cli;
  char url[128];
  int result_status;
  ccol_retval_t result_rv;
  atomic_int ready;
} probe_arg_t;

static void *probe_thread(void *arg) {
  probe_arg_t *a = (probe_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) {
    a->result_rv = ccol_not_enough_memory;
    return NULL;
  }
  atomic_store(&a->ready, 1);
  chttpcli_response *resp = NULL;
  a->result_rv = chttpclient_do(a->cli, req, &resp);
  if (resp) {
    a->result_status = resp->status_code;
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  return NULL;
}

static void *do_destroy_thread(void *arg) {
  chttpcli h = *(chttpcli *)arg;
  __chttpclient_destroy(h);
  return NULL;
}

TEST(http, concurrent_requests) {
  enum { NTHREADS = 8 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, NTHREADS), ccol_success);

  concurrent_req_arg_t args[NTHREADS];
  pthread_t threads[NTHREADS];

  int started = 0;
  for (int i = 0; i < NTHREADS; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    /* This loop counts the started threads instead of asserting here. A
     * REQUIRE_* here is the stack-use-after-return that the join-first
     * rule below prevents: it returns from this function while the threads
     * of the earlier iterations still run and still write into args[] and
     * threads[], which live on this frame. The code below joins only the
     * threads that started, and checks the count after every one of them
     * is back. */
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }

  /* Joins every thread FIRST, in its own loop, before any REQUIRE_* runs.
   * The REQUIRE_* macros of tau return from this function at once on a
   * failure, and args and threads live on the stack of this function. With
   * a check in the same loop as the join, a failed assertion of an earlier
   * iteration returns from this function while a thread that nobody has
   * joined yet still runs and still writes into args[] after the stack
   * frame is gone. That is a real stack-use-after-return, not only a lost
   * test result, and it can corrupt whatever later test reuses that same
   * stack memory. */
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, NTHREADS);
  for (int i = 0; i < NTHREADS; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     POOL RESIZE TESTS                                      */
/* ========================================================================== */

TEST(pool, grow_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 1), ccol_success);

  /* The first request initializes the pool at size 1. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Grows the pool to 4. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* The second request must work too. */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(http, head_200) {
  char url[128];
  make_url(url, sizeof(url), "/head");

  chttp_request_t *req = chttp_request_new(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* A HEAD response carries no body, so the client never calls the write
   * callback for it. */
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, options_200) {
  char url[128];
  make_url(url, sizeof(url), "/options");

  chttp_request_t *req = chttp_request_new(CHTTP_OPTIONS, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(chttp_default_client(), req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  const char *allow = chttpclient_resp_header(resp, "allow");
  REQUIRE_NE((void *)allow, NULL);
  REQUIRE_STREQ(allow, "GET, POST, PUT, DELETE, PATCH, HEAD, OPTIONS");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, large_body_response) {
  char url[128];
  make_url(url, sizeof(url), "/large");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server sends 8192 bytes, which makes the buffer of _write_cb grow
   * past its first capacity of 4096 bytes. */
  REQUIRE_EQ(resp->body_len, (size_t)8192);
  REQUIRE_NE((void *)resp->body, NULL);
  /* A sample check: every byte must be 'x'. */
  REQUIRE_EQ((int)resp->body[0], (int)'x');
  REQUIRE_EQ((int)resp->body[4095], (int)'x');
  REQUIRE_EQ((int)resp->body[8191], (int)'x');

  chttpclient_resp_free(resp);
}

TEST(http, content_type_auto_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  /* Sets body.content_type, but no explicit Content-Type header. */
  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server echoes the content-type that it got. */
  REQUIRE_STREQ(resp->body, "application/json");

  chttpclient_resp_free(resp);
}

TEST(http, content_type_explicit_header_not_duplicated) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "text";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  /* An explicit header wins over the header that the library adds, so the
   * server must see exactly this value.
   */
  chttp_request_set_header(req, "Content-Type", "text/csv");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/csv");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, streaming_post) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"stream\":true}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  /* The server echoes the body, which the streaming sink must hold. */
  REQUIRE_EQ(sink.len, strlen(payload));
  REQUIRE_STREQ(sink.buf, payload);

  chttp_request_free(req);
}

TEST(http, streaming_post_content_type_injected) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "{\"k\":\"v\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;

  ccol_retval_t rv = chttpclient_do_streaming(
      chttp_default_client(), req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  /* The server echoes the content-type that the test sent, which checks
   * that the streaming path adds the header in the same way as the path
   * that buffers the response. */
  REQUIRE_STREQ(sink.buf, "application/json");

  chttp_request_free(req);
}

TEST(http, streaming_null_status_code_out) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ccol_retval_t rv = chttpclient_do_streaming(chttp_default_client(), req,
                                              stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
}

/* ========================================================================== */
/*                     POOL RESIZE TESTS (continued)                          */
/* ========================================================================== */

TEST(pool, shrink_after_initialization) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* The first request initializes the pool at size 4. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Shrinks the pool to 2. A slot that is already in flight completes in the
   * old range. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  /* A later request, which uses slot 0 or slot 1, must still succeed. */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(pool, idle_handles_freed_on_shrink) {
  /* Grows the pool so that POOL_LARGE requests can be in flight at once,
   * then shrinks it after all of them complete. The concurrency limiter
   * (_slot_acquire and _slot_release) is a plain counting semaphore over
   * cli->in_flight_count and pool_cap, and owns no per-slot object that a
   * shrink could leak. The job of this test is to confirm that a shrink
   * after a burst of concurrency neither corrupts that bookkeeping nor
   * breaks a later request; valgrind checks that nothing leaks. */
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_LARGE), ccol_success);

  /* Sends POOL_LARGE requests at once, so that every slot is used. */
  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  int started = 0;
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    /* This loop counts the started threads instead of asserting here. See the
     * same comment in http.concurrent_requests for the reason: a
     * REQUIRE_* inside this loop is the stack-use-after-return that the
     * join below prevents. */
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }
  /* Joins every thread FIRST, in its own loop, before any REQUIRE_* runs.
   * See the same comment in http.concurrent_requests for the reason: a
   * thread that nobody has joined yet gives a stack-use-after-return, not
   * only a lost test result. */
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, POOL_LARGE);
  for (int i = 0; i < POOL_LARGE; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  /* After every request completes, cli->in_flight_count is 0 again, and a
   * shrink of pool_cap here must neither disturb that nor break a later
   * request. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_SMALL), ccol_success);

  /* The pool must still work at the new size. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(pool, shrink_while_in_flight_exiles_slots) {
  /* Starts POOL_LARGE requests at once and shrinks the pool to POOL_SMALL
   * while all of them are in flight, so cli->in_flight_count is briefly
   * larger than the new, smaller cli->pool_cap; then it waits for the
   * requests to complete. _slot_acquire and _slot_release track the
   * occupancy only through cli->in_flight_count and pool_cap and own no
   * per-slot object that a shrink must clean up. The job of this test is
   * to confirm that the shrink does not corrupt that bookkeeping while real
   * requests run against it; valgrind checks that nothing leaks. */
  enum { POOL_LARGE = 4, POOL_SMALL = 2 };
  char url[128];
  make_url(url, sizeof(url), "/slow");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, POOL_LARGE), ccol_success);

  concurrent_req_arg_t args[POOL_LARGE];
  pthread_t threads[POOL_LARGE];
  int started = 0;
  for (int i = 0; i < POOL_LARGE; i++) {
    args[i].cli = cli;
    memcpy(args[i].url, url, sizeof(url));
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    /* This loop counts the started threads instead of asserting here. See the
     * same comment in http.concurrent_requests for the reason: a
     * REQUIRE_* inside this loop is the stack-use-after-return that the
     * join below prevents. */
    if (pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]) != 0)
      break;
    started++;
  }

  /* Spins until the test server has a request from every thread that
   * started. At that point each of those client threads is blocked inside
   * chttp_do_internal, waiting for its own response, and is past
   * _slot_acquire, which has already added one to cli->in_flight_count. The
   * shrink below therefore drops pool_cap below the number of slots that
   * threads occupy at that moment.
   *
   * The bound is `started`, and never POOL_LARGE: a count that no live
   * thread can reach turns a failure of pthread_create into a binary that
   * hangs here with no diagnostic, while with `started` it fails the
   * assertion below instead. */
  while (atomic_load(&g_slow_started) < started) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */
    nanosleep(&ts, NULL);
  }

  /* This code keeps the return value and does not assert it yet, because a
   * REQUIRE_* here returns early, before the joins below, while POOL_LARGE
   * threads still run against the args[] and threads[] arrays on the stack
   * after the frame of this function is gone. */
  ccol_retval_t set_pool_size_rv = chttpclient_set_pool_size(cli, POOL_SMALL);

  /* Joins every thread FIRST, in its own loop, before any REQUIRE_* runs.
   * See the same comment in http.concurrent_requests for the reason: a
   * thread that nobody has joined yet gives a stack-use-after-return, not
   * only a lost test result. */
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);

  REQUIRE_EQ(started, POOL_LARGE);
  REQUIRE_EQ(set_pool_size_rv, ccol_success);
  for (int i = 0; i < POOL_LARGE; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  /* The pool must still work at the smaller size, after the cleanup of the
   * slots that the shrink put outside the new range. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     INVALID ARGUMENT TESTS                                 */
/* ========================================================================== */

TEST(invalid_args, chttpclient_do_null_checks) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(CHTTPCLI_INVALID, req, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(invalid_args, chttp_convenience_null_checks) {
  chttpcli_response *resp = NULL;
  /* A NULL url. */
  REQUIRE_EQ(chttp_get(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_post(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_put(NULL, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch(NULL, NULL, &resp), ccol_invalid_args);
  /* A NULL resp_out. */
  REQUIRE_EQ(chttp_get("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_post("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_put("http://x.com/", NULL, NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_delete("http://x.com/", NULL), ccol_invalid_args);
  REQUIRE_EQ(chttp_patch("http://x.com/", NULL, NULL), ccol_invalid_args);
}

TEST(invalid_args, resp_header_null_checks) {
  REQUIRE_EQ((void *)chttpclient_resp_header(NULL, "X-Foo"), NULL);
  chttpclient_resp_free(NULL);
}

TEST(invalid_args, request_set_header_null_checks) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://x.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(NULL, "X", "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, NULL, "v"), ccol_invalid_args);
  REQUIRE_EQ(chttp_request_set_header(req, "X", NULL), ccol_invalid_args);
  chttp_request_free(req);
}

/* ========================================================================== */
/*                     CUSTOM ALLOCATOR TEST                                  */
/* ========================================================================== */

static atomic_int g_alloc_count = 0;
static atomic_int g_free_count = 0;

static void *tracked_malloc(size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return malloc(sz);
}
static void tracked_free(void *p) {
  if (p) atomic_fetch_add(&g_free_count, 1);
  free(p);
}
static void *tracked_calloc(size_t n, size_t sz) {
  atomic_fetch_add(&g_alloc_count, 1);
  return calloc(n, sz);
}
static void *tracked_realloc(void *p, size_t sz) {
  /* Counts realloc as a free of the old block plus a malloc of the new one,
   * so that the two counters stay balanced. */
  if (p) atomic_fetch_add(&g_free_count, 1);
  atomic_fetch_add(&g_alloc_count, 1);
  return realloc(p, sz);
}

TEST(custom_allocator, allocations_go_through_custom_procs) {
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};

  /* Tests a request with a custom allocator. */
  char *err = NULL;
  const char *url = "http://example.com/";
  chttp_request_t *req = chttp_request_new_mp(CHTTP_GET, url, NULL, &mp, &err);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "X-Custom", "value");
  REQUIRE_NE((void *)chttp_request_get_header(req, "x-custom"), NULL);
  chttp_request_free(req);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));

  /* Tests a client with a custom allocator. */
  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  err = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttpclient_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* ========================================================================== */
/*                     BODY-BUFFER / SERIALIZATION OOM                        */
/* ========================================================================== */

/* Fails every realloc() call that asks for 4096 bytes or more, while every
 * smaller realloc, and every malloc, calloc and free, works normally. The
 * growth of chttp_bodybuf_t in _sink_buffered starts at exactly 4096, and
 * the buffer that serializes a small plain GET request never grows near
 * that size, so this hits the growth call of _sink_buffered alone, without
 * the test having to guess a global index of an allocation call. */
static void *bodybuf_oom_malloc(size_t sz) { return malloc(sz); }
static void *bodybuf_oom_calloc(size_t n, size_t sz) { return calloc(n, sz); }
static void bodybuf_oom_free(void *p) { free(p); }
static void *bodybuf_oom_realloc(void *p, size_t sz) {
  if (sz >= 4096) return NULL;
  return realloc(p, sz);
}
static ccol_memmgmt_procs_t g_bodybuf_oom_mp = {.malloc = bodybuf_oom_malloc,
                                                .free = bodybuf_oom_free,
                                                .calloc = bodybuf_oom_calloc,
                                                .realloc = bodybuf_oom_realloc};

TEST(body_buffer_oom, realloc_failure_reports_not_enough_memory) {
  /* A regression test. _sink_buffered sets chttp_bodybuf_t.oom when a
   * realloc fails, and something must read that flag, so that _on_body does
   * not treat the short return that follows as a streaming abort by the
   * caller. Without the flag, a buffered response body large enough to grow
   * the sink buffer reports ccol_http_transfer_aborted for a local
   * out-of-memory condition, a code that means the connection failed in
   * the middle of a transfer or that the server sent a malformed response.
   * The documented code here is ccol_not_enough_memory. The difference
   * matters, because a caller can retry a transfer error but cannot retry
   * an out-of-memory condition. The response body of /large is 8192 bytes,
   * which is larger than the first capacity of 4096 bytes of the sink, so
   * it forces the growth call that this allocator fails. */
  char *cerr = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&g_bodybuf_oom_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req =
      chttp_request_new_mp(CHTTP_GET, url, NULL, &g_bodybuf_oom_mp, &cerr);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_not_enough_memory);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     PUT / PATCH CONTENT-TYPE TESTS                         */
/* ========================================================================== */

TEST(http, put_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "text/plain");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  /* Builds a PUT body with content_type = NULL, to check that
   * _serialize_request adds no default header when the caller gives none:
   * neither "content-type: application/x-www-form-urlencoded" nor any other
   * default. */
  const char *payload = "raw";
  chttp_request_body_t body = {.data = payload, .len = 3, .content_type = NULL};

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server echoes the content-type, and since the client sends none, the
   * body is empty. */
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_body_round_trip) {
  char url[128];
  make_url(url, sizeof(url), "/put-echo");

  const char *payload = "hello-put";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, put_without_body) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  chttp_request_t *req = chttp_request_new(CHTTP_PUT, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, patch_with_content_type) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "delta";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "application/json");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, patch_without_content_type_sends_none) {
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  const char *payload = "patch";
  chttp_request_body_t body = {.data = payload, .len = 5, .content_type = NULL};

  chttp_request_t *req = chttp_request_new(CHTTP_PATCH, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     TLS CONFIGURATION TEST                                 */
/* ========================================================================== */

TEST(tls, set_tls_deep_copies_strings) {
  /* Checks that chttpclient_set_tls owns copies of the path strings; without
   * those copies, valgrind reports a use-after-free in memtest. */
  chttpcli_construct(cli);

  {
    char cert[64], key[64], ca[64];
    memcpy(cert, "/tmp/test.crt", sizeof("/tmp/test.crt"));
    memcpy(key, "/tmp/test.key", sizeof("/tmp/test.key"));
    memcpy(ca, "/tmp/ca.pem", sizeof("/tmp/ca.pem"));

    chttp_tls_config_t tls = {
        .cert_path = cert,
        .key_path = key,
        .ca_bundle_path = ca,
    };
    REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

    /* Overwrites the stack buffers of the caller; the copies that the client
     * owns must not change. */
    memset(cert, 0xff, sizeof(cert));
    memset(key, 0xff, sizeof(key));
    memset(ca, 0xff, sizeof(ca));
  }

  /* Sets the configuration again, to exercise the path that frees the old
   * copy, and then puts the defaults back. */
  chttp_tls_config_t tls2 = {
      .cert_path = "/tmp/other.crt",
      .key_path = "/tmp/other.key",
      .ca_bundle_path = NULL,
      .insecure_skip_verify = true,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls2), ccol_success);
  REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);

  chttpclient_destroy(cli);
}

TEST(tls, unreadable_cert_path_reports_cert_load_failed) {
  /* _rebuild_tls_ctx_locked reports a failure later, not at once, with the
   * code ccol_http_tls_cert_load_failed, which reaches the caller once an
   * HTTPS request needs a certificate path, a key path or a CA path that
   * nobody can read. All three of its call sites must report that code
   * instead of turning it into the general ccol_http_tls_handshake_failed,
   * which a caller cannot tell apart from a real failure after the
   * handshake. The client finds the failure before it tries to connect,
   * because chttp_do_internal checks tls_ctx_usable directly after it
   * parses the URL, so the target does not have to resolve or accept a
   * real connection. */
  chttpcli_construct(cli);

  chttp_tls_config_t tls = {
      .cert_path = "/nonexistent/does-not-exist.crt",
      .key_path = "/nonexistent/does-not-exist.key",
      .ca_bundle_path = NULL,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(tls, unreadable_ca_bundle_path_reports_cert_load_failed) {
  /* The same contract as above, through the ca_bundle_path branch of
   * _rebuild_tls_ctx_locked, while the test above uses the branch for the
   * certificate and key pair. */
  chttpcli_construct(cli);

  chttp_tls_config_t tls = {
      .cert_path = NULL,
      .key_path = NULL,
      .ca_bundle_path = "/nonexistent/does-not-exist-ca.pem",
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     POOL SIZE = 0 (CPU COUNT) TEST                         */
/* ========================================================================== */

TEST(pool, set_pool_size_zero_uses_cpu_count) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  /* A size of 0 must resolve to the CPU count, which is more than 0, instead
   * of setting the pool to 0. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 0), ccol_success);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(pool, set_pool_size_zero_after_initialization) {
  /* Checks a call to chttpclient_set_pool_size with 0 (the CPU count) after
   * the pool is already initialized. The call must resolve the size and
   * resize the pool, which grows or shrinks depending on the current CPU
   * count against pool_cap. */
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 4), ccol_success);

  /* The first request initializes the pool at size 4. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  /* Resizes to the CPU count, with 0, now that pool_initialized is true. */
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 0), ccol_success);

  /* The pool must still serve requests after the resize. */
  req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttp_request_free(req);

  chttpclient_destroy(cli);
}

TEST(pool, request_timeout_counts_time_spent_waiting_for_a_pool_slot) {
  /* The documented contract of chttpclient_set_request_timeout is "the
   * maximum time from when chttpclient_do is called...", and that must
   * hold when the pool that limits the concurrency
   * (chttpclient_set_pool_size) is full and this call blocks inside
   * _slot_acquire to wait for a slot. The deadline therefore starts BEFORE
   * _slot_acquire. A deadline that starts after _slot_acquire returns hides
   * the whole wait for the slot from the budget, so a caller behind a full
   * pool gets a complete, fresh request_timeout_ms that starts only once a
   * slot frees up, however long that takes.
   *
   * This test uses a pool of size 1. One thread holds the only slot with a
   * request against /very-slow, where the server sleeps for 500ms, and the
   * main thread then calls chttpclient_do against the fast /get route with
   * a request timeout of 20ms.
   *
   * This test is not vacuous. With a deadline that starts after
   * _slot_acquire, the second call blocks on the full pool for about
   * 500ms, THEN gets a fresh budget of 20ms, which is enough for a /get
   * over the loopback, and succeeds. With a deadline that starts at entry,
   * the 20ms run out while the call waits for the slot, so it times out
   * near the 20ms mark, long before the slot of the occupant frees up. The
   * signal is a clear success against a timeout, and it does not depend on
   * fine timing. */
  char slow_url[128], get_url[128];
  make_url(slow_url, sizeof(slow_url), "/very-slow"); /* sleeps 500ms */
  make_url(get_url, sizeof(get_url), "/get");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 1), ccol_success);

  concurrent_req_arg_t occupant = {
      .cli = cli, .result_status = 0, .result_rv = ccol_unexpected_failure};
  memcpy(occupant.url, slow_url, sizeof(slow_url));
  pthread_t occupant_thread;
  REQUIRE_EQ(
      pthread_create(&occupant_thread, NULL, concurrent_req_thread, &occupant),
      0);

  /* Spins until the occupant is certainly in flight, which means it is past
   * _slot_acquire, which has already raised cli->in_flight_count to the
   * cap of 1 of the pool. Only then does the test set the short timeout and
   * send the second call. This is the synchronisation pattern that this
   * file uses everywhere instead of a fixed sleep (see
   * do_returns_not_permitted_when_destroying). */
  while (atomic_load(&g_slow_started) < 1) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */
    nanosleep(&ts, NULL);
  }

  /* Every result below goes into a local variable instead of being asserted
   * at once. The spin above confirms that occupant_thread runs, and it does
   * not finish until the /very-slow sleep of 500ms on the server ends, so a
   * REQUIRE_* between this point and the join further down returns early
   * while the thread still runs and still writes into the local `occupant`
   * on the stack after the frame of this function is gone. That is a real
   * stack-use-after-return, not only a lost test result. This covers
   * chttpclient_set_request_timeout and chttp_request_new as well as the
   * result of chttpclient_do, because an assertion on those two calls
   * before the join opens this same window. */
  ccol_retval_t set_timeout_rv = chttpclient_set_request_timeout(cli, 20000);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, get_url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = ccol_unexpected_failure;
  long elapsed_ms = -1;
  if (req) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    rv = chttpclient_do(cli, req, &resp);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    elapsed_ms =
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
    chttp_request_free(req);
  }

  /* The join runs before any REQUIRE_* below (see the comment above). It
   * does not change the measurement above, because elapsed_ms already holds
   * it at this point. */
  pthread_join(occupant_thread, NULL);

  REQUIRE_EQ(set_timeout_rv, ccol_success);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_EQ((void *)resp, NULL);
  /* The return code alone is not enough of a signal, because _slot_acquire
   * must know the deadline too. With a deadline that starts before
   * _slot_acquire, but a _slot_acquire that ignores it, this call still
   * blocks for the full 500ms of the occupant, since a plain
   * ccol_cond_var_wait cannot see that the deadline has passed. A later
   * connect step or read step then finds the expired deadline, so the call
   * still ends in ccol_timed_out, but only after blocking for much longer
   * than the 20ms that the test set; that is what this test catches. The
   * upper bound of 300ms clearly separates a call that returns soon after
   * its own 20ms (a timer of a busy runner can wake 150ms late, and the
   * bound leaves room for that) from a call that first blocks for the
   * 500ms of the occupant. */
  REQUIRE_LT(elapsed_ms, 300L);

  REQUIRE_EQ(occupant.result_rv, ccol_success);
  REQUIRE_EQ(occupant.result_status, 200);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     BODY MACRO TESTS                                       */
/* ========================================================================== */

TEST(body_macros, chttp_body) {
  const char *data = "raw";
  chttp_request_body_t b = CHTTP_BODY(data, 3, "text/html");
  REQUIRE_EQ((void *)b.data, (void *)data);
  REQUIRE_EQ(b.len, (size_t)3);
  REQUIRE_STREQ(b.content_type, "text/html");
}

TEST(body_macros, chttp_json_body) {
  const char *data = "{}";
  chttp_request_body_t b = CHTTP_JSON_BODY(data, 2);
  REQUIRE_STREQ(b.content_type, "application/json");
  REQUIRE_EQ(b.len, (size_t)2);
}

TEST(body_macros, chttp_text_body) {
  const char *data = "hello";
  chttp_request_body_t b = CHTTP_TEXT_BODY(data, 5);
  REQUIRE_STREQ(b.content_type, "text/plain");
}

TEST(body_macros, chttp_form_body) {
  const char *data = "a=1&b=2";
  chttp_request_body_t b = CHTTP_FORM_BODY(data, strlen(data));
  REQUIRE_STREQ(b.content_type, "application/x-www-form-urlencoded");
}

TEST(body_macros, chttp_no_body) {
  chttp_request_body_t b = CHTTP_NO_BODY;
  REQUIRE_EQ((void *)b.data, NULL);
  REQUIRE_EQ(b.len, (size_t)0);
  REQUIRE_EQ((void *)b.content_type, NULL);
}

/* ========================================================================== */
/*                     METHOD STRING TEST                                     */
/* ========================================================================== */

TEST(method, chttp_method_str) {
  REQUIRE_STREQ(chttp_method_str(CHTTP_GET), "GET");
  REQUIRE_STREQ(chttp_method_str(CHTTP_POST), "POST");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PUT), "PUT");
  REQUIRE_STREQ(chttp_method_str(CHTTP_DELETE), "DELETE");
  REQUIRE_STREQ(chttp_method_str(CHTTP_PATCH), "PATCH");
  REQUIRE_STREQ(chttp_method_str(CHTTP_HEAD), "HEAD");
  REQUIRE_STREQ(chttp_method_str(CHTTP_OPTIONS), "OPTIONS");
}

/* ========================================================================== */
/*                     RUN QUERY TESTS                                        */
/* ========================================================================== */

TEST(http, run_query_null_headers) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_post_with_body) {
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"run\":\"query\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, NULL, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_headers_forwarded_and_not_consumed) {
  char url[128];
  make_url(url, sizeof(url), "/echo-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-echo", "hello-from-run-query");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello-from-run-query");
  chttpclient_resp_free(resp);

  /* hdrs must still be alive after the call, because chttp_run_query must
   * not take ownership of it. */
  const char *stored = chmap_get(hdrs, "x-echo");
  REQUIRE_NE((void *)stored, NULL);
  REQUIRE_STREQ(stored, "hello-from-run-query");
  chmap_destroy(hdrs);
}

TEST(http, run_query_invalid_args) {
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, NULL, NULL, NULL, &resp),
             ccol_invalid_args);
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, "http://x/", NULL, NULL, NULL),
             ccol_invalid_args);
}

/* ========================================================================== */
/*                     CLIENT DESTRUCTION TESTS                               */
/* ========================================================================== */

TEST(pool, do_returns_not_permitted_when_destroying) {
  /* The plan of this test: fill ALL pool slots with requests of 100 ms, so
   * that a later chttpclient_do MUST block in the ccol_cond_var_wait of
   * _slot_acquire. Once both slots are certainly in flight, the test starts
   * a probe thread, which blocks on the full pool, and only then starts the
   * destroy thread. The destroy sets destroying=true and broadcasts, which
   * wakes the probe, which sees the flag and returns ccol_not_permitted.
   * The test needs no sleep of a fixed length. */
  char slow_url[128], url[128];
  make_url(slow_url, sizeof(slow_url), "/slow");
  make_url(url, sizeof(url), "/get");
  atomic_store(&g_slow_started, 0);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 2), ccol_success);

  /* Fills both slots with slow requests of 100 ms. */
  concurrent_req_arg_t slow_args[2];
  pthread_t slow_tids[2];
  int slow_started = 0;
  for (int i = 0; i < 2; i++) {
    slow_args[i].cli = cli;
    memcpy(slow_args[i].url, slow_url, sizeof(slow_url));
    slow_args[i].result_status = 0;
    slow_args[i].result_rv = ccol_unexpected_failure;
    /* This loop counts the started threads instead of asserting here (see the
     * same comment in http.concurrent_requests). One block further down
     * joins every thread that this test starts, and every result, this
     * count included, is asserted only after that block. */
    if (pthread_create(&slow_tids[i], NULL, concurrent_req_thread,
                       &slow_args[i]) != 0)
      break;
    slow_started++;
  }

  /* Spins until the server side confirms that every slow request that
   * started is in flight. The bound is slow_started, and never 2: with a
   * count that no live thread can reach, the test hangs here and the test
   * below never fails. */
  while (atomic_load(&g_slow_started) < slow_started) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000};
    nanosleep(&ts, NULL);
  }

  /* Starts the probe thread. Since both pool slots are in use,
   * chttpclient_do either blocks in _slot_acquire or sees destroying=true
   * at the fast-path guard. The test spins on probe.ready, which the probe
   * sets directly before its chttpclient_do call, to keep the window
   * before the destroy as small as it can be. */
  probe_arg_t probe;
  probe.cli = cli;
  memcpy(probe.url, url, sizeof(url));
  probe.result_status = 0;
  probe.result_rv = ccol_unexpected_failure;
  atomic_store(&probe.ready, 0);
  pthread_t probe_tid;
  bool probe_started =
      (pthread_create(&probe_tid, NULL, probe_thread, &probe) == 0);

  /* The test waits here only when the probe thread runs, because only that
   * thread sets probe.ready. */
  while (probe_started && !atomic_load(&probe.ready)) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 100000}; /* 0.1 ms */
    nanosleep(&ts, NULL);
  }

  /* Now the destroy starts. __chttpclient_destroy sets destroying=true and
   * broadcasts, which wakes the probe, and then waits for in_flight_count
   * to reach 0. The probe sees destroying==true and returns
   * ccol_not_permitted. */
  pthread_t destroy_tid;
  bool destroy_started =
      (pthread_create(&destroy_tid, NULL, do_destroy_thread, &cli) == 0);

  /* This one block joins every thread FIRST, before any REQUIRE_* below
   * runs. The REQUIRE_* macros of tau return from this function at once on
   * a failure, while probe, slow_args and cli all live on the stack of this
   * function, and the test even gave &cli to destroy_tid. With assertions
   * mixed between the joins, a thread that nobody has joined yet still
   * runs and still writes into the stack frame of this function from the
   * moment an earlier assertion fails; destroy_tid in particular still
   * calls chttpclient_destroy on a `cli` variable that is gone. That is a
   * real stack-use-after-return, not only a lost test result. */
  if (probe_started) pthread_join(probe_tid, NULL);
  for (int i = 0; i < slow_started; i++) pthread_join(slow_tids[i], NULL);
  if (destroy_started) pthread_join(destroy_tid, NULL);
  /* do_destroy_thread frees cli when that thread ran. When it never started,
   * this function still owns the handle and must free it; without that, a
   * leak report hides the failure that the code below reports. */
  if (!destroy_started) chttpclient_destroy(cli);

  REQUIRE_EQ(slow_started, 2);
  REQUIRE_TRUE(probe_started);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_EQ(probe.result_rv, ccol_not_permitted);
  REQUIRE_EQ(probe.result_status, 0);
  for (int i = 0; i < 2; i++) {
    REQUIRE_EQ(slow_args[i].result_rv, ccol_success);
    REQUIRE_EQ(slow_args[i].result_status, 200);
  }
}

/* ========================================================================== */
/*                     NETWORK ERROR CODE TESTS                               */
/* ========================================================================== */

TEST(error_codes, unsupported_scheme_returns_invalid_url) {
  /* _parse_chttp_url knows only "http://", "https://" and "http+unix://".
   * Every other scheme reaches its own return of
   * ccol_http_invalid_url. */
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("ccol-not-a-scheme://example.com/", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

/* A sentinel value that is wrong on purpose, never NULL, and never
 * dereferenced. Each test below writes it into *resp_out before a call
 * that must fail, so the test can pass only when the library writes NULL
 * into *resp_out itself; a library that leaves an already-NULL value alone
 * fails. Every other test in this file starts resp at NULL, so the same
 * assertion there passes without any work by the library. See the
 * contract that these tests pin, below. */
#define SENTINEL_RESP ((chttpcli_response *)(uintptr_t)0xdeadbeefUL)

TEST(error_codes, resp_out_actively_reset_to_null_chttpclient_do) {
  /* chttp_do_internal is the shared code behind chttpclient_do and every
   * convenience wrapper. It must write *resp_out on every failure path,
   * instead of leaving the value that the local variable of the caller held
   * before the call. The documentation says that a call to
   * chttpclient_resp_free() with NULL is safe, so that a caller can free
   * without a check, and some callers do not set their own pointer to NULL
   * first: such a caller frees or dereferences garbage after any ordinary
   * failure, such as a host that is down, a bad URL or a timeout. */
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, "ccol-not-a-scheme://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = SENTINEL_RESP;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(error_codes, resp_out_actively_reset_to_null_convenience_wrappers) {
  /* The same contract, through the early-return path that each convenience
   * wrapper has of its own. Those paths run before chttp_do_internal, so the
   * contract has to hold there by itself. */
  chttpcli_response *resp;

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_get("ccol-not-a-scheme://example.com/", &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_post("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_put("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_delete("ccol-not-a-scheme://example.com/", &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_patch("ccol-not-a-scheme://example.com/", NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_do(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, "ccol-not-a-scheme://example.com/",
                             NULL, NULL, &resp),
             ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  /* A NULL url. The library checks resp_out and writes NULL into it BEFORE
   * it checks the url, so the contract must hold for this failure too, not
   * only for the failures further inside. */
  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_get(NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  resp = SENTINEL_RESP;
  REQUIRE_EQ(chttp_run_query(CHTTP_GET, NULL, NULL, NULL, &resp),
             ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(error_codes, resp_out_actively_reset_to_null_chttpclient_do_pooled) {
  /* chttpclient_do_pooled of Tier 3 owes the caller the same *resp_out
   * contract as Tier 1, and this test pins it so that the two tiers cannot
   * drift apart. */
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, "ccol-not-a-scheme://example.com/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = SENTINEL_RESP;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(error_codes, resp_out_actively_reset_to_null_for_a_stale_handle) {
  /* The handle check comes after *resp_out is written, in both Tier 1 and
   * Tier 3, so a stale handle, the invalid handle and a handle that was
   * never issued all fail with ccol_invalid_args and leave *resp_out NULL.
   * This test is non-vacuous: resolving the handle before the reset of
   * *resp_out in chttpclient_do leaves the sentinel in place. */
  chttpcli dead = ccol_create_chttpclient(NULL);
  REQUIRE_NE(dead, CHTTPCLI_INVALID);
  chttpcli dead_handle = dead;
  __chttpclient_destroy(dead);

  char url[160];
  make_url(url, sizeof(url), "/get");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli handles[] = {dead_handle, CHTTPCLI_INVALID,
                        (chttpcli)0x0000000700000009ULL};
  bool all_null = true;
  bool all_invalid = true;
  for (size_t i = 0; i < sizeof(handles) / sizeof(handles[0]); i++) {
    chttpcli_response *resp = SENTINEL_RESP;
    if (chttpclient_do(handles[i], req, &resp) != ccol_invalid_args)
      all_invalid = false;
    if (resp != NULL) all_null = false;
    resp = SENTINEL_RESP;
    if (chttpclient_do_pooled(handles[i], req, &resp) != ccol_invalid_args)
      all_invalid = false;
    if (resp != NULL) all_null = false;
  }
  chttp_request_free(req);
  REQUIRE_TRUE(all_invalid);
  REQUIRE_TRUE(all_null);
}

#undef SENTINEL_RESP

TEST(error_codes, embedded_crlf_in_path_rejected_end_to_end) {
  /* End-to-end coverage of request-line injection by an attacker, which the
   * group url_parsing.path_with_embedded_crlf_is_invalid pins at the parser
   * level. chttpclient_do of Tier 1 must itself refuse to send a request
   * whose URL hides a raw CR or LF byte in its path, instead of writing
   * those bytes onto the wire, where they become an extra header line or a
   * second, hidden request. The client must not even try to connect: the
   * URL names a port that nothing listens on, so without a rejection by the
   * URL parser at the start, the test fails with a connection error rather
   * than with ccol_http_invalid_url. */
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(
      "http://127.0.0.1:1/search?q=x\r\nX-Injected: 1\r\n\r\nGET /evil", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(error_codes, host_resolution_failure) {
  /* RFC 6761 reserves the .invalid top-level domain, which never resolves.
   * The short timeout keeps the test from a long wait on a slow
   * resolver. */
  chttpcli_construct(cli);
  chttpclient_set_request_timeout(cli, 5000000);

  chttp_request_t *req = chttp_request_new(
      CHTTP_GET, "http://this-host-will-not-resolve.invalid.ccol.test/", NULL,
      NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_host_resolution_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(error_codes, connection_refused) {
  /* Binds a port, keeps its number, and closes the socket at once, so that
   * nothing listens on that port afterwards and the kernel refuses a
   * connection to it with an RST. */
  int s = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_GT(s, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  a.sin_port = htons(0);
  bind(s, (struct sockaddr *)&a, sizeof(a));
  socklen_t alen = sizeof(a);
  getsockname(s, (struct sockaddr *)&a, &alen);
  int closed_port = ntohs(a.sin_port);
  close(s);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/", closed_port);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_connection_failed);
  REQUIRE_EQ((void *)resp, NULL);
}

/* ========================================================================== */
/*                MAX RESPONSE BODY SIZE (TIER 1)                            */
/* ========================================================================== */

TEST(max_response_body_size, unset_default_is_unlimited) {
  /* The baseline, with no cap set (the default): the 8192-byte response of
   * /large still succeeds, while every other test in this group sets a cap
   * far below that size. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, declared_content_length_rejected_up_front) {
  /* /large declares "Content-Length: 8192" in its headers, and the cap below
   * is smaller than that, so the up-front check in _on_headers_complete
   * must reject the request before it reads one body byte, reporting
   * ccol_msg_too_large instead of buffering the whole large body first. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpclient_destroy(cli);
}

TEST(max_response_body_size, eof_delimited_body_rejected_reactively) {
  /* /eof-delimited-body sends no Content-Length; only the close of the
   * connection marks the end of its body, so the up-front check in
   * _on_headers_complete has no declared length to compare, and the
   * reactive check that _sink_buffered runs for each append must still
   * catch the body. The body is "eof-delimited-body-ok", 22 bytes, which is
   * larger than the cap. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpclient_destroy(cli);
}

TEST(max_response_body_size, chunked_body_rejected_reactively) {
  /* /chunked-body declares no Content-Length, because chunked framing
   * carries no length in the headers, so, as in the EOF case above, only
   * the reactive check that _sink_buffered runs for each append can catch
   * it. The body is "Hello, chunked world!", 21 bytes carried by two
   * separate chunks, which is larger than the cap. The doc comment of
   * max_response_body_size says that the cap holds for every framing mode
   * of a body, and this test pins it for a chunked body. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/chunked-body");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  chttpclient_destroy(cli);
}

TEST(max_response_body_size, redirect_hop_body_exempt_from_the_cap) {
  /* chttpclient.h says that max_response_body_size does not hold for the
   * body of an intermediate redirect hop, which the !ctx->will_redirect
   * guard in _on_headers_complete implements. The intermediate 301 body of
   * /redirect-with-body is
   * "this-intermediate-body-must-never-reach-the-caller", 51 bytes, which
   * is larger than the cap below on purpose, while the final /get body is
   * 15 bytes and fits well inside it. This request therefore succeeds only
   * when the client obeys the exemption. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 20), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size,
     interim_1xx_oversized_content_length_exempt_from_the_cap) {
  /* The up-front too-large check in _on_headers_complete must not fire for
   * the declared Content-Length of a discarded interim 1xx response. Such a
   * Content-Length breaks RFC 7230 SS3.3.2, and a check that looks at the
   * declared length of ANY message fails the WHOLE request with
   * ccol_msg_too_large, although the real final response that arrives,
   * "{\"status\":\"ok\"}" (16 bytes), stays well inside the cap below. The
   * "103 Early Hints" of
   * /early-hints-oversized-content-length declares
   * "Content-Length: 999999". */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/early-hints-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, head_response_oversized_content_length_exempt) {
  /* The up-front too-large check in _on_headers_complete must skip a HEAD
   * response, whose Content-Length describes what a GET gives back (RFC
   * 7231 SS4.3.2) and which no body byte follows.
   * /head-oversized-content-length declares "Content-Length: 999999" and
   * sends no body on the wire. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/head-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, response_304_oversized_content_length_exempt) {
  /* The up-front too-large check in _on_headers_complete must skip 204 and
   * 304 in the same way as it skips 1xx and HEAD, because RFC 7230 SS3.3
   * treats all of them the same for body framing; chttp1_parser.c already
   * forces no_body for 204 and 304, whatever Content-Length the response
   * declares. A 304 often carries the Content-Length of the original
   * resource (RFC 7232 SS4.1), which can be large: this is a real and
   * common pattern for a conditional GET against a CDN. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/304-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 304);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, response_204_oversized_content_length_exempt) {
  /* The same reason as for response_304_oversized_content_length_exempt
   * above, but for 204 No Content. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/204-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 204);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, response_exactly_at_cap_succeeds) {
  /* The cap includes its own value, so a response whose body is exactly
   * max_bytes must still succeed. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 8192), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(max_response_body_size, streaming_path_is_unaffected_by_the_cap) {
  /* chttpclient_do_streaming has no chttp_bodybuf_t, because the write_fn of
   * the caller controls the memory instead, so the cap must never hold for
   * it, not even when the cap is far smaller than the response. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;
  ccol_retval_t rv =
      chttpclient_do_streaming(cli, req, stream_sink_write, &sink, &status);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     CHUNKED RESPONSE TESTS                                 */
/*                                                                            */
/* /chunked-body sends a real response with chunked transfer encoding. It   */
/* has two data chunks plus the final chunk of zero length, and no          */
/* Content-Length. This group is the only integration coverage, on either   */
/* tier, of the client side of a chunked RESPONSE. It covers the assembly   */
/* of more than one chunk through _chttp_read_message and through the       */
/* on-readable loop of the async engine. It also covers keep-alive after a  */
/* chunked body. tests_parser.c covers the chunked decoder of               */
/* chttp1_parser.c as a unit, and other tests cover chunked REQUEST bodies. */
/* ========================================================================== */

TEST(chunked_response, decoded_correctly) {
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen("Hello, chunked world!"));
  REQUIRE_STREQ(resp->body, "Hello, chunked world!");

  chttpclient_resp_free(resp);
}

TEST(chunked_response, connection_reused_after_chunked_body) {
  /* Chunked framing carries its own end marker, the final chunk of zero
   * length, so the connection stays eligible for keep-alive, just as after
   * a body framed by Content-Length. A wrong keep_alive after a chunked
   * body shows up here, and so does a client that does not drain past the
   * final chunk: either one gives a second accept that nothing needs,
   * instead of a reused connection. */
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 3; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_STREQ(resp->body, "Hello, chunked world!");
    chttpclient_resp_free(resp);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     KEEP-ALIVE / IDLE POOL TESTS                           */
/*                                                                            */
/* These tests exercise how the client reuses a connection. They cover the  */
/* idle pool, its liveness probe, and the key that it builds from the       */
/* origin.                                                                  */
/* ========================================================================== */

TEST(keepalive, sequential_requests_reuse_connection) {
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
  }

  /* Waits a short time for the server to add to its accept count, because
   * the thread that calls accept() does that, not the thread of the client
   * that reads the response. */
  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli);
}

TEST(keepalive, dead_connection_detected_and_reconnects) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  chttp_request_t *req1 = chttp_request_new(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval_t rv1 = chttpclient_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);

  /* The server closed its end after that response. A naive pool tries to
   * reuse the dead connection here and fails, while the liveness probe of
   * the client must find this and connect again without reporting anything
   * to the caller. */
  chttp_request_t *req2 = chttp_request_new(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  chttpcli_response *resp2 = NULL;
  ccol_retval_t rv2 = chttpclient_do(cli, req2, &resp2);
  chttp_request_free(req2);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpclient_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
}

TEST(keepalive, concurrent_requests_exceeding_idle_cap_no_crash) {
  /* Sends more keep-alive requests at once than the idle pool can hold for
   * one origin. The pool does not keep the extra connections, which is the
   * documented behavior, but it must not crash, leak or fail. */
  char url[128];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  chttpclient_set_pool_size(cli, 16);

  const int n = 12;
  pthread_t threads[12];
  concurrent_req_arg_t args[12];
  /* created and create_rv guard against a stack-use-after-return when
   * pthread_create fails in the middle of this loop, because threads[] and
   * args[] live on the stack (see the same comment in
   * async_idle_pool.concurrent_stale_eviction_races_dispatch_no_uaf).
   * Without these two variables, a failed pthread_create leaves threads[i]
   * uninitialized, and the join loop below then calls pthread_join on a
   * garbage pthread_t, which is undefined behavior and can hang this whole
   * test binary instead of failing cleanly. */
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < n; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].result_status = 0;
    args[i].result_rv = ccol_unexpected_failure;
    create_rv =
        pthread_create(&threads[i], NULL, concurrent_req_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < n; i++) {
    REQUIRE_EQ(args[i].result_rv, ccol_success);
    REQUIRE_EQ(args[i].result_status, 200);
  }

  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     MAX IDLE ORIGINS (Tier 1 and Tier 2/3)                 */
/* ========================================================================== */

/* Forward declarations. The full doc comments sit next to the main
 * declarations further below (search for these same names), which come too
 * late in this file for the tests in this section. */
extern struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h);
extern size_t _chttpclient_idle_pools_key_count_for_tests(
    struct chttpclient *cli);
extern size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli);
extern void _chttpclient_set_max_idle_origins_for_tests(size_t n);

TEST(max_idle_origins, tier1_distinct_origins_bounded_and_reclaimed) {
  /* A regression test for two related properties of _idle_pool_take and
   * _idle_pool_offer.
   *
   * (1) An idle-pool list for one origin can become empty after a pop, and
   *     it must not leave its own empty chmap entry behind forever:
   *     _idle_pool_take deletes the entry at the moment a pop empties its
   *     list.
   *
   * (2) That prune alone does not bound the growth for an origin that a
   *     caller visits once and never again, which is the pattern of a web
   *     crawler: nothing ever pops its connection back out, so nothing
   *     triggers the prune above. CHTTP_MAX_IDLE_ORIGINS closes this as a
   *     hard cap on how many distinct origin keys _idle_pool_offer creates
   *     a fresh entry for. This test sets it to 2 through the test-only
   *     hook, because the real cap of 128 needs 128 distinct origins.
   *
   * This test uses the three real listeners of this suite (IPv4, IPv6 and
   * a Unix domain socket, all serving the same routes) as three distinct
   * origin_keys, with nothing synthetic. */
  _chttpclient_set_max_idle_origins_for_tests(2);

  char url4[160], url6[160], url_unix[256];
  make_url(url4, sizeof(url4), "/keepalive");
  make_url6(url6, sizeof(url6), "/keepalive");
  make_unix_url(url_unix, sizeof(url_unix), "/keepalive");

  chttpcli_construct(cli);
  struct chttpclient *raw = _chttpcli_resolve_for_tests(cli);
  REQUIRE_NE((void *)raw, NULL);

  /* The first two distinct origins fill the cap of 2 that this test set. */
  chttp_request_t *req4 = chttp_request_new(CHTTP_GET, url4, NULL, NULL);
  REQUIRE_NE((void *)req4, NULL);
  chttpcli_response *resp4 = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req4, &resp4), ccol_success);
  REQUIRE_NE((void *)resp4, NULL);
  chttpclient_resp_free(resp4);
  chttp_request_free(req4);

  chttp_request_t *req6 = chttp_request_new(CHTTP_GET, url6, NULL, NULL);
  REQUIRE_NE((void *)req6, NULL);
  chttpcli_response *resp6 = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req6, &resp6), ccol_success);
  REQUIRE_NE((void *)resp6, NULL);
  chttpclient_resp_free(resp6);
  chttp_request_free(req6);

  usleep(20000); /* lets both connections reach the idle pool */
  REQUIRE_EQ(_chttpclient_idle_pools_key_count_for_tests(raw), (size_t)2);

  /* A third distinct origin, the Unix one, still succeeds, because the cap
   * only decides whether the pool keeps ITS connection afterward and never
   * whether the client serves the request. But the request must NOT raise
   * the key count above 2. */
  int accepts_before_unix = test_server_accept_count();
  chttp_request_t *req_u1 = chttp_request_new(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u1, NULL);
  chttpcli_response *resp_u1 = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req_u1, &resp_u1), ccol_success);
  REQUIRE_NE((void *)resp_u1, NULL);
  chttpclient_resp_free(resp_u1);
  chttp_request_free(req_u1);

  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_key_count_for_tests(raw), (size_t)2);

  /* The pool never kept the Unix connection, because the cap was already
   * full, so a SECOND request against it can reuse nothing. This proves the
   * "not pooled" result directly, not only through the key count. */
  chttp_request_t *req_u2 = chttp_request_new(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u2, NULL);
  chttpcli_response *resp_u2 = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req_u2, &resp_u2), ccol_success);
  REQUIRE_NE((void *)resp_u2, NULL);
  chttpclient_resp_free(resp_u2);
  chttp_request_free(req_u2);

  usleep(20000);
  int accepts_after_unix = test_server_accept_count();
  REQUIRE_EQ(accepts_after_unix - accepts_before_unix, 2);

  /* This block proves property (1) directly, not only property (2) above.
   * It pops the one pooled connection of IPv4 with an ordinary, successful
   * reuse. A pop of the LAST entry in the list of an origin prunes the map
   * entry of that origin at once, inside the same locked section as the
   * pop, whether the popped connection later turns out to be alive or
   * dead.
   *
   * /get sends a real "Connection: close" and /keepalive does not, so the
   * client tears the reused connection down afterward instead of offering
   * it back. Nothing recreates the pruned entry of IPv4, so the prune stays
   * visible instead of being hidden at once by a fresh offer.
   *
   * This check is not vacuous: with a broken prune, the empty entry of
   * IPv4 stays in the map and the key count below reads 2 instead of 1. */
  char url4_close[160];
  make_url(url4_close, sizeof(url4_close), "/get");
  chttp_request_t *req4b = chttp_request_new(CHTTP_GET, url4_close, NULL, NULL);
  REQUIRE_NE((void *)req4b, NULL);
  chttpcli_response *resp4b = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req4b, &resp4b), ccol_success);
  REQUIRE_NE((void *)resp4b, NULL);
  chttpclient_resp_free(resp4b);
  chttp_request_free(req4b);
  usleep(20000);
  /* The code above popped the connection of IPv4 for reuse, and since /get
   * sends Connection: close, nothing offers that connection back, so the
   * count shows only the live entry of IPv6. This proves that the code
   * really pruned the entry of IPv4, rather than merely showing that the
   * cap had room left because nothing ever reached it. */
  REQUIRE_EQ(_chttpclient_idle_pools_key_count_for_tests(raw), (size_t)1);

  /* The slot of IPv4 is now free, and the count of 1 is below the cap of 2,
   * so a fresh Unix request must be able to take it. This proves that the
   * cap sees room that became free; a correct refusal while the cap is full
   * is not enough. */
  chttp_request_t *req_u3 = chttp_request_new(CHTTP_GET, url_unix, NULL, NULL);
  REQUIRE_NE((void *)req_u3, NULL);
  chttpcli_response *resp_u3 = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req_u3, &resp_u3), ccol_success);
  REQUIRE_NE((void *)resp_u3, NULL);
  chttpclient_resp_free(resp_u3);
  chttp_request_free(req_u3);
  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_key_count_for_tests(raw), (size_t)2);

  _chttpclient_set_max_idle_origins_for_tests(0);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     URL PARSER EDGE CASES                                  */
/* ========================================================================== */

TEST(url_parsing, missing_host_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http:///get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, non_numeric_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:abc/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, port_out_of_range_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:99999/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, zero_port_returns_invalid_url) {
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:0/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, trailing_garbage_after_valid_port_returns_invalid_url) {
  /* A regression test. The loop over the port digits stops at the first
   * byte that is not a digit, whatever that byte is, so an explicit check
   * must then confirm that this byte is '\0', '/', '?' or '#'. Without that
   * check, garbage directly after a valid port falls into the code that
   * computes the path and the query: here "abc" follows "80", and
   * "abc/get" becomes the request path, so the client sends the request to
   * a target that the URL string does not name and reports nothing. It
   * must reject the URL instead. This test differs from
   * non_numeric_port_returns_invalid_url above, which covers a port with no
   * digit at all. */
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get("http://127.0.0.1:80abc/get", &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(url_parsing, explicit_non_default_port_succeeds) {
  /* The whole test suite already runs against a port that the OS assigns,
   * which is not the default one. This test gives that property a name and
   * an assertion of its own: the client parses an explicit port and
   * connects to it correctly. */
  char url[128];
  make_url(url, sizeof(url), "/get");
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

/* White-box helpers for _parse_chttp_url and _resolve_redirect_url (see
 * the RUNNING_UNIT_TESTS block in src/chttpclient.c for the exact contract
 * about ownership). With a NULL mp, plain malloc allocates every out
 * string, and the caller must free() each one. */
extern ccol_retval_t _chttp_parse_url_for_tests(
    const char *url, bool *is_https_out, bool *is_ipv6_out, char **host_out,
    uint16_t *port_out, char **path_and_query_out, char **origin_key_out,
    char **userinfo_authorization_out, bool *is_unix_out,
    char **unix_socket_path_out);
extern char *_chttp_resolve_redirect_url_for_tests(const char *base_url,
                                                   const char *location);

TEST(url_parsing, ipv6_literal_no_port) {
  bool is_https = false, is_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests("https://[::1]/path", &is_https,
                                                &is_ipv6, &host, &port, &pq,
                                                &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_https);
  REQUIRE_TRUE(is_ipv6);
  REQUIRE_STREQ(host, "::1");
  REQUIRE_EQ(port, 443);
  REQUIRE_STREQ(pq, "/path");
  REQUIRE_STREQ(origin_key, "https://[::1]:443");
  REQUIRE_EQ((void *)auth, NULL);
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, ipv6_literal_with_port) {
  bool is_https = false, is_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests("http://[::1]:8443/", &is_https,
                                                &is_ipv6, &host, &port, &pq,
                                                &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_ipv6);
  REQUIRE_STREQ(host, "::1");
  REQUIRE_EQ(port, 8443);
  REQUIRE_STREQ(origin_key, "http://[::1]:8443");
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, ipv6_missing_closing_bracket_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://[::1/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_garbage_after_bracket_is_invalid) {
  ccol_retval_t rv =
      _chttp_parse_url_for_tests("http://[::1]x/path", NULL, NULL, NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_empty_brackets_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://[]/path", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, host_with_embedded_crlf_is_invalid) {
  /* The loop that scans the host stops at ':', '/', '?' and '#', but not at
   * a raw CR or LF byte. Without an explicit rejection, a URL string with a
   * control character in its authority part therefore reaches url->host
   * unchanged, however the caller built the string, and from there it
   * reaches the "host: " header line that the client builds and adds extra
   * header lines onto the wire. The parser must reject such a URL. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://evil.com\r\nx-injected:1/path", NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_host_with_embedded_crlf_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://[::1\r\nx-injected:1]/path", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, path_with_embedded_crlf_is_invalid) {
  /* The parser must check path_and_query for CR and LF in the same way as it
   * checks the host component, which the two tests above cover. Without
   * that check, a raw control byte (one that is not percent-encoded) in the
   * path of a URL reaches path_and_query unchanged, and _serialize_request
   * then copies path_and_query into the request-target of the request line
   * ("METHOD <path_and_query> HTTP/1.1\r\n"). A URL string that the caller
   * built can then end the request line early and add one more header line,
   * or a whole second request. The parser must reject such a URL, just as
   * it rejects the same bytes in the host component. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://example.com/a\r\nX-Injected: 1", NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, query_with_embedded_crlf_is_invalid) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://example.com/search?q=x\r\nX-Injected:1", NULL, NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, bare_lf_in_path_is_invalid) {
  /* A bare LF, with no CR in front of it, ends the request line on the wire
   * just as well as a full CRLF pair, so the parser must reject it on its
   * own, not only as part of the CRLF pair. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://example.com/a\nX-Injected:1", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, ipv6_live_connect) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] ipv6_live_connect: no IPv6 loopback listener available "
            "in this environment\n");
    return;
  }
  char url[128];
  make_url6(url, sizeof(url), "/get");
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(url_parsing, userinfo_user_and_pass) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://alice:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port,
      &pq, &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)auth, NULL);
  /* base64("alice:s3cr3t") == "YWxpY2U6czNjcjN0" */
  REQUIRE_STREQ(auth, "Basic YWxpY2U6czNjcjN0");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_user_only) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://alice@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  /* base64("alice:") == "YWxpY2U6" */
  REQUIRE_STREQ(auth, "Basic YWxpY2U6");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_pass_only) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://:s3cr3t@host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  /* base64(":s3cr3t") == "OnMzY3IzdA==" */
  REQUIRE_STREQ(auth, "Basic OnMzY3IzdA==");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_percent_encoded_components) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  /* Inside the raw parts, "%40" means '@' and "%3A" means ':', so the parser
   * must find the delimiters before it decodes, not after. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://user%40x:pa%3Ass@host/path", &dummy_https, &dummy_ipv6, &host,
      &port, &pq, &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  /* base64("user@x:pa:ss") == "dXNlckB4OnBhOnNz" */
  REQUIRE_STREQ(auth, "Basic dXNlckB4OnBhOnNz");
  free(host);
  free(pq);
  free(origin_key);
  free(auth);
}

TEST(url_parsing, userinfo_malformed_percent_escape_is_invalid) {
  ccol_retval_t rv =
      _chttp_parse_url_for_tests("http://user%zzpass@host/path", NULL, NULL,
                                 NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, no_userinfo_means_no_auto_authorization) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://host/path", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ((void *)auth, NULL);
  free(host);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, fragment_is_stripped_from_wire) {
  /* This test is not vacuous: with the fragment inside the request line, the
   * server sees the path "/get#section", which reaches its 404 handler
   * instead of matching "/get". */
  char base[128];
  make_url(base, sizeof(base), "/get");
  char url[160];
  snprintf(url, sizeof(url), "%s#section", base);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(url_parsing, fragment_only_defaults_to_root_path) {
  bool dummy_https = false, dummy_ipv6 = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://host#section", &dummy_https, &dummy_ipv6, &host, &port, &pq,
      &origin_key, &auth, NULL, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_STREQ(pq, "/");
  free(host);
  free(pq);
  free(origin_key);
}

/* ========================================================================== */
/*                     RELATIVE REDIRECT RESOLUTION (RFC 3986)                */
/* ========================================================================== */

TEST(relative_redirects, rfc3986_5_4_reference_table) {
  /* A part of the well-known example table of RFC 3986 SS5.4, with its
   * normal and abnormal cases, against the base "http://a/b/c/d;p?q". The
   * cases cover a plain relative reference, "./", a root-relative
   * reference, a protocol-relative reference, a query alone, and a query
   * with a path; ".", "./", "..", "../", more than one level of "..", a walk
   * that reaches the root, "/./" and "/../"; dot segments in the middle of
   * a path ("g/./h" and "g/../h"); and dots at the start and at the end
   * that are ordinary characters ("g." and ".g"), which the parser must NOT
   * treat as dot segments.
   *
   * The cases also cover a query string that holds bytes in the shape of
   * "/../", on both the absolute-path branch and the relative-path branch.
   * The removal of dot segments in RFC 3986 SS5.2.4 works on the path part
   * alone, so the parser must never read a query value as navigation
   * through a path, even when the value holds slashes and dots that look
   * like dot segments. Passing the whole "path?query" string to
   * remove_dot_segments corrupts both the path and the query whenever the
   * query holds a sequence in the shape of "/../", and these cases catch
   * that.
   *
   * The cases cover the "#s" and "g#s" rows of the table too. By RFC 3986
   * SS3.5 a fragment is never part of what a redirect sends to the next
   * hop, and _parse_chttp_url drops one from any URL in the same way, so
   * the output of this function never carries a fragment, whatever the
   * worked example table of RFC 3986 SS5.4 shows for T.fragment. "g#/../h"
   * is the sharpest of those rows: a fragment that holds its own "/../"
   * bytes must be dropped BEFORE the removal of dot segments runs, not
   * after; otherwise the code walks those bytes as real path navigation and
   * silently resolves to the wrong target.
   *
   * The cases also cover the "g:h" row. A reference with its own scheme is
   * always absolute (T = R, RFC 3986 SS5.2.2), whether or not the client
   * knows that scheme, so such a reference must resolve to itself unchanged
   * instead of merging onto the origin of the base like an ordinary
   * relative reference. See the comment on
   * _location_has_scheme. */
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"g:h", "g:h"},
      {"g", "http://a/b/c/g"},
      {"g#s", "http://a/b/c/g"},
      {"g#/../h", "http://a/b/c/g"},
      {"#s", "http://a/b/c/d;p?q"},
      {"./g", "http://a/b/c/g"},
      {"g/", "http://a/b/c/g/"},
      {"/g", "http://a/g"},
      {"//g", "http://g"},
      {"?y", "http://a/b/c/d;p?y"},
      {"g?y", "http://a/b/c/g?y"},
      {"g;x", "http://a/b/c/g;x"},
      {".", "http://a/b/c/"},
      {"./", "http://a/b/c/"},
      {"..", "http://a/b/"},
      {"../", "http://a/b/"},
      {"../g", "http://a/b/g"},
      {"../..", "http://a/"},
      {"../../", "http://a/"},
      {"../../g", "http://a/g"},
      {"../../../g", "http://a/g"},
      {"../../../../g", "http://a/g"},
      {"/./g", "http://a/g"},
      {"/../g", "http://a/g"},
      {"g.", "http://a/b/c/g."},
      {".g", "http://a/b/c/.g"},
      {"g..", "http://a/b/c/g.."},
      {"..g", "http://a/b/c/..g"},
      {"./../g", "http://a/b/g"},
      {"./g/.", "http://a/b/c/g/"},
      {"g/./h", "http://a/b/c/g/h"},
      {"g/../h", "http://a/b/c/h"},
      {"/foo/bar?x=1/../2", "http://a/foo/bar?x=1/../2"},
      {"/a/../b?x=1/../2", "http://a/b?x=1/../2"},
      {"g?x=1/../2", "http://a/b/c/g?x=1/../2"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = _chttp_resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
                                                         cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

TEST(relative_redirects,
     query_only_reference_against_dotted_base_left_verbatim) {
  /* RFC 3986 SS5.3 says that when R.path is "" (a reference with a query
   * alone, such as "?y"), T.path is Base.path WITHOUT A CHANGE, with no
   * merge and no removal of dot segments. The table above only uses "?y"
   * against the base "/b/c/d;p", which holds no dot segment at all, so it
   * cannot tell "left as it is" apart from "silently normalised". The base
   * here holds unresolved ".." segments on purpose.
   *
   * This client never removes the dot segments of the original request URL
   * of the caller (see _parse_chttp_url), so the exact same bytes must
   * still be there after a redirect with a query alone. This matches the
   * RFC and a reference implementation: for example, Python gives
   * urllib.parse.urljoin('http://example.com/a/../b?x', '?y') ==
   * 'http://example.com/a/../b?y'. The "#frag" case (a reference with a
   * fragment alone) is here as a control against the same base, to confirm
   * that the two stay consistent with each other. */
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"?y", "http://example.com/a/../b?y"},
      {"#frag", "http://example.com/a/../b?x"},
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = _chttp_resolve_redirect_url_for_tests(
        "http://example.com/a/../b?x", cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

/* ========================================================================== */
/*                  REQUEST SERIALIZATION OVERFLOW GUARD                      */
/* ========================================================================== */

extern bool _chttp_ob_append_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                      size_t fake_len,
                                                      size_t n);

/* This allocator counts each call, allocates nothing and always returns
 * NULL. A test uses it to prove that the overflow guard of _ob_append
 * rejects an out-of-range append BEFORE the append reaches the real
 * allocator. This project uses the same "the allocator was never called"
 * pattern for this class of overflow guard in cvector, csort and cmempool.
 *
 * The allocator is safe whether the guard is there or not: a regression
 * that lets a call through still gets NULL at once, and no real allocation
 * of a wrapped size happens. The count of calls, not a crash, is what
 * tells the two outcomes apart. */
static atomic_int g_ob_overflow_alloc_calls;
static void *ob_overflow_never_malloc(size_t sz) {
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void *ob_overflow_never_calloc(size_t n, size_t sz) {
  (void)n;
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void *ob_overflow_never_realloc(void *p, size_t sz) {
  (void)p;
  (void)sz;
  atomic_fetch_add(&g_ob_overflow_alloc_calls, 1);
  return NULL;
}
static void ob_overflow_never_free(void *p) { (void)p; }
static ccol_memmgmt_procs_t g_ob_overflow_never_mp = {
    .malloc = ob_overflow_never_malloc,
    .free = ob_overflow_never_free,
    .calloc = ob_overflow_never_calloc,
    .realloc = ob_overflow_never_realloc};

TEST(request_serialization,
     ob_append_overflow_guard_rejects_without_allocating) {
  /* fake_len + n is SIZE_MAX - 3 + 8, which wraps past SIZE_MAX. The guard
   * must reject this and report an out-of-memory condition without ever
   * calling the malloc, calloc or realloc of mp. A broken guard lets the
   * loop that doubles the size settle on a small, wrapped `nc`, and then
   * allocates and copies; the count of calls of this allocator is what
   * catches that. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool oom = _chttp_ob_append_overflow_guard_for_tests(&g_ob_overflow_never_mp,
                                                       SIZE_MAX - 3, 8);
  REQUIRE_TRUE(oom);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, ob_append_ordinary_small_append_still_works) {
  /* The same helper, with an ordinary start length that is far from an
   * overflow, through the default allocator, to confirm that the guard
   * above does not reject a real buffer of a realistic size. */
  bool oom = _chttp_ob_append_overflow_guard_for_tests(NULL, 0, 8);
  REQUIRE_FALSE(oom);
}

/* ========================================================================== */
/*             UNIX-SOCKET-PATH PERCENT-ENCODING OVERFLOW GUARD               */
/* ========================================================================== */

extern bool _chttp_percent_encode_unix_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *path, size_t fake_len);

TEST(request_serialization,
     percent_encode_unix_path_overflow_guard_rejects_without_allocating) {
  /* fake_len * 3 + 1 wraps past SIZE_MAX for any fake_len above
   * (SIZE_MAX - 1) / 3. The guard must reject this without ever calling the
   * malloc, calloc or realloc of mp; a broken guard allocates `out` too
   * small and then writes past its end in the loop that encodes. This test
   * follows the pattern of
   * ob_append_overflow_guard_rejects_without_allocating, which covers the
   * sibling guard in _ob_append. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = _chttp_percent_encode_unix_path_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/x", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization,
     percent_encode_unix_path_ordinary_small_path_still_works) {
  /* The same helper, with a real path that is far from an overflow and with
   * its own real length, through the default allocator, to confirm that
   * the guard above does not reject a real socket path. */
  const char *path = "/var/run/app.sock";
  bool rejected = _chttp_percent_encode_unix_path_overflow_guard_for_tests(
      NULL, path, strlen(path));
  REQUIRE_FALSE(rejected);
}

/* ========================================================================== */
/*                  RESPONSE-BODY-BUFFER OVERFLOW GUARD                       */
/* ========================================================================== */

extern bool _chttp_sink_buffered_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, size_t fake_len);

TEST(request_serialization,
     sink_buffered_overflow_guard_rejects_without_allocating) {
  /* fake_len + sizeof(probe) is SIZE_MAX - 3 + 8, which wraps past
   * SIZE_MAX. The guard must reject this and report an out-of-memory
   * condition, not a too-large condition, without ever calling the malloc,
   * calloc or realloc of mp. This test follows the pattern of
   * ob_append_overflow_guard_rejects_without_allocating, which covers the
   * sibling guard in _ob_append. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = _chttp_sink_buffered_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, SIZE_MAX - 3);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, sink_buffered_ordinary_small_body_still_works) {
  /* The same helper, with an ordinary start length that is far from an
   * overflow, through the default allocator, to confirm that the guard
   * above does not reject a real response body of a realistic size. */
  bool rejected = _chttp_sink_buffered_overflow_guard_for_tests(NULL, 0);
  REQUIRE_FALSE(rejected);
}

/* ========================================================================== */
/*             HEADER-NAME-DEDUP-TRACKER OVERFLOW GUARD                       */
/* ========================================================================== */

extern bool _chttp_seen_names_add_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, size_t fake_cap);

TEST(request_serialization,
     seen_names_add_overflow_guard_rejects_without_allocating) {
  /* fake_cap * 2 is SIZE_MAX + 1, which wraps past SIZE_MAX to 0. The guard
   * must reject this without ever calling the malloc, calloc or realloc of mp.
   * This test follows the pattern of
   * ob_append_overflow_guard_rejects_without_allocating, which covers the
   * sibling guard in _ob_append. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = _chttp_seen_names_add_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, SIZE_MAX / 2 + 1);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(request_serialization, seen_names_add_ordinary_small_cap_still_works) {
  /* The same helper, with an ordinary start cap that is far from an
   * overflow (0, the real start value of every chttp_seen_names_t), through
   * the default allocator, to confirm that the guard above does not reject
   * a real header count. */
  bool rejected = _chttp_seen_names_add_overflow_guard_for_tests(NULL, 0);
  REQUIRE_FALSE(rejected);
}

/* ========================================================================== */
/*             REDIRECT-PATH-QUERY-CONCAT OVERFLOW GUARD                      */
/* ========================================================================== */

extern bool _chttp_merge_ref_path_overflow_guard_for_tests(
    ccol_memmgmt_procs_t *mp, const char *base_path_and_query,
    const char *ref_path, size_t fake_ref_path_len);
extern bool _chttp_concat_len_overflow_guard_for_tests(ccol_memmgmt_procs_t *mp,
                                                       const char *a,
                                                       size_t fake_a_len,
                                                       const char *b,
                                                       size_t fake_b_len);

TEST(relative_redirects,
     merge_ref_path_overflow_guard_rejects_without_allocating) {
  /* dir_len is a few bytes here and fake_ref_path_len is SIZE_MAX, so their
   * sum wraps past SIZE_MAX. The guard must reject this without ever
   * calling the malloc, calloc or realloc of mp. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = _chttp_merge_ref_path_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/a/b/c", "x", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(relative_redirects, merge_ref_path_ordinary_small_path_still_works) {
  /* The same helper, with a real reference path that is far from an
   * overflow and with its own real length, through the default allocator. */
  bool rejected =
      _chttp_merge_ref_path_overflow_guard_for_tests(NULL, "/a/b/c", "d", 1);
  REQUIRE_FALSE(rejected);
}

TEST(relative_redirects, concat_len_overflow_guard_rejects_without_allocating) {
  /* fake_b_len is SIZE_MAX and fake_a_len is a few bytes here, so their sum
   * wraps past SIZE_MAX. The guard must reject this without ever calling
   * the malloc, calloc or realloc of mp. */
  atomic_store(&g_ob_overflow_alloc_calls, 0);
  bool rejected = _chttp_concat_len_overflow_guard_for_tests(
      &g_ob_overflow_never_mp, "/a/b", 4, "?q=1", SIZE_MAX);
  REQUIRE_TRUE(rejected);
  REQUIRE_EQ(atomic_load(&g_ob_overflow_alloc_calls), 0);
}

TEST(relative_redirects, concat_len_ordinary_small_strings_still_works) {
  /* The same helper, with real strings that are far from an overflow and
   * with their own real lengths, through the default allocator. */
  bool rejected =
      _chttp_concat_len_overflow_guard_for_tests(NULL, "/a/b", 4, "?q=1", 4);
  REQUIRE_FALSE(rejected);
}

TEST(relative_redirects, location_with_unrecognized_scheme_resolves_absolute) {
  /* Wider coverage for the "g:h" row of the RFC table above: any Location
   * value that carries its own scheme must resolve to itself unchanged. The
   * grammar of a scheme is
   * ALPHA *( ALPHA / DIGIT / "+" / "-" / "." ) ":" in RFC 3986 SS3.1, and
   * this holds for every scheme, so the parser must never merge such a
   * value onto the origin of the base as a relative path.
   *
   * The cases also cover schemes with digits, "+", "-" and "." in them,
   * which are real, legal scheme shapes such as "s3", "coap+tcp" and
   * "git-http". Two cases are negative, and the parser must NOT read them
   * as a scheme: a bare colon inside a relative path segment with no query
   * and no fragment, in a string that does not start with a letter; and a
   * percent-encoded colon, which is never a real delimiter of a
   * scheme. */
  static const struct {
    const char *location;
    const char *expected;
  } cases[] = {
      {"mailto:test@example.com", "mailto:test@example.com"},
      {"ftp://other.example/x", "ftp://other.example/x"},
      {"s3://bucket/key", "s3://bucket/key"},
      {"coap+tcp:5683/x", "coap+tcp:5683/x"},
      {"g%3Ah", "http://a/b/c/g%3Ah"}, /* no real ':' -> ordinary relative */
      {"./this:that", "http://a/b/c/this:that"}, /* starts with a dot
                                                  * segment -> not a
                                                  * scheme */
  };

  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char *result = _chttp_resolve_redirect_url_for_tests("http://a/b/c/d;p?q",
                                                         cases[i].location);
    REQUIRE_NE((void *)result, NULL);
    REQUIRE_STREQ(result, cases[i].expected);
    free(result);
  }
}

TEST(relative_redirects, live_multi_level_dot_segments) {
  char url[160];
  make_url(url, sizeof(url), "/nested/dir/redirect-relative-dotted");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(relative_redirects, live_absolute_path_query_with_slashes_not_corrupted) {
  /* An end-to-end regression test, beyond the unit level of
   * _resolve_redirect_url: an absolute-path Location whose query string
   * holds "/../" must reach the server byte for byte. Without that, the
   * client turns the request target into a different path (see the comment
   * on /redirect-abs-path-query-with-slashes in the mock server), so the
   * request gets a 404 instead of reaching
   * /query-preserved-target. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-abs-path-query-with-slashes");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     USERINFO-DERIVED BASIC AUTH (LIVE)                     */
/* ========================================================================== */

TEST(credentials, embedded_userinfo_sets_authorization_header) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  /* Puts "alice:s3cr3t@" directly after "http://". */
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpclient_resp_free(resp);
}

TEST(credentials, explicit_authorization_header_wins) {
  char base[128];
  make_url(base, sizeof(base), "/echo-auth");
  char url[160];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");
  chttpclient_resp_free(resp);
}

TEST(credentials, same_origin_redirect_carries_authorization) {
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");
  chttpclient_resp_free(resp);
}

TEST(credentials, cross_origin_redirect_drops_authorization) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] cross_origin_redirect_drops_authorization: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-cross-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The redirect target, which is a different origin, never saw the
   * userinfo, so the Authorization header that the client built from the
   * original origin must not follow. A body of zero length is
   * resp->body == NULL, not "" (see every other assertion about an empty
   * body in this file). */
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     REDIRECT METHOD/BODY POLICY                            */
/* ========================================================================== */

TEST(redirect_policy, post_301_becomes_bodyless_get) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");
  chttpclient_resp_free(resp);
}

TEST(redirect_policy, post_307_preserves_method_and_body) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

/* Regression tests. A Content-Length, a Content-Type or an Expect header
 * that the caller set describes the ORIGINAL POST body, so it must not stay
 * on a GET with no body that a 301, a 302 or a 303 produced. A client that
 * rewrites only cur_method and cur_body on that change, and never
 * req->headers, leaves those headers in place. A stale Content-Length is
 * the worst of the three, because it can make the server that gets the
 * request block while it waits for a body that never arrives, which the
 * server chttpserver.c in this codebase does too.
 * /redirect-301-to-count-header turns the request into a GET against
 * /count-header, which reports how many times the header that
 * "x-count-name" names appears on the wire. */
TEST(redirect_policy, stale_content_length_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "9"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-length"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(redirect_policy, stale_content_type_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Type", "application/json"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-type"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(redirect_policy, stale_expect_stripped_after_downgrade) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Expect", "100-continue"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

/* When an earlier redirect that keeps nothing drops the body, that body must
 * stay dropped across a LATER 307 or 308 hop on the same chain, because such a
 * hop keeps the current body, not the original body of the chain. The chain
 * here is /redirect-301-then-307-to-echo, then /redirect-307-to-echo, then
 * /echo-method-body. The 301 of hop 0 drops the POST body and turns the method
 * into GET, and the 307 of hop 1 must keep that GET with no body instead of
 * bringing the original POST body back. See the comment on
 * chttp_async_chain_t.body_dropped in chttpclient.c for how Tier 2 and Tier 3
 * carry that state across their own chain of hops. On Tier 1 this follows from
 * the structure of the code: the cur_body of chttp_do_internal is a local of
 * the loop, and it keeps the drop across the hops. */
TEST(redirect_policy, body_stays_dropped_across_a_later_preserving_hop) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-then-307-to-echo");

  const char *body = "some=data";
  chttp_request_t *req = chttp_request_new(
      CHTTP_POST, url, &CHTTP_FORM_BODY(body, strlen(body)), NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");
  chttpclient_resp_free(resp);
}

/* An Authorization header that the caller sets obeys the same cross-origin
 * rule as one that the client builds from the userinfo of the URL (see
 * credentials.cross_origin_redirect_drops_authorization). This matches the
 * default of curl, which its fix for CVE-2018-1000007 made strict: the
 * client drops the header for good at the first hop whose origin differs
 * from the origin of the ORIGINAL request. A client that forwards the
 * header across every redirect leaks the credential in it to the redirect
 * target, whatever the origin of that target is. */
TEST(credentials, explicit_authorization_header_carried_same_origin_redirect) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-same-origin");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");
  chttpclient_resp_free(resp);
}

TEST(credentials, explicit_authorization_header_dropped_cross_origin_redirect) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] explicit_authorization_header_dropped_cross_origin_"
            "redirect: no IPv6 loopback listener available in this "
            "environment\n");
    return;
  }
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-cross-origin");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  chttp_request_free(req);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* A body of zero length is resp->body == NULL, not "" (see every other
   * assertion about an empty body in this file). */
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

TEST(redirect_policy, exceeding_max_redirects_reports_error) {
  /* /redirect-infinite always redirects to itself. This is the Tier 1
   * (synchronous) counterpart of
   * async_redirects.exceeding_max_redirects_reports_error: after
   * CHTTP_MAX_REDIRECTS hops, chttp_do stops and reports
   * ccol_http_too_many_redirects, instead of looping forever or giving the
   * last 302 to the caller as an ordinary response. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-infinite");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_http_too_many_redirects);
  chttp_request_free(req);
  REQUIRE_EQ((void *)resp, NULL);
}

/* ========================================================================== */
/*                REDIRECT TRANSPORT (http+unix) REFUSAL TESTS                */
/* ========================================================================== */

/* Resolves `location` against `base_url` in the same way as a hop does, and
 * reports whether the client accepts the transport of the resolved target.
 * See the RUNNING_UNIT_TESTS block in src/chttpclient.c for the contract
 * about ownership of *resolved_out: plain malloc allocates it, and the
 * caller must free() it. */
extern bool _chttp_redirect_transport_allowed_for_tests(const char *base_url,
                                                        const char *location,
                                                        bool prevent_downgrade,
                                                        char **resolved_out);

/* The definition sits with the other helpers of the async tier further
 * below. */
static void wait_for_async_engine_idle(void);

/*
 * Every test in this group pins one rule: a redirect that the client
 * follows must never bring in the AF_UNIX transport, and it must never
 * point that transport somewhere else. The mock server serves the same
 * routes on its TCP listener and on its Unix listener, so a client that
 * obeys such a Location comes back with ccol_success and with the 200
 * response body of the Unix listener. The return code AND the absence of
 * that response tell a refusal apart from a pass, and neither depends on
 * an accident of timing or of connectivity.
 *
 * These tests are not vacuous: without the transport check on each hop,
 * every "cannot" test below reports ccol_success with status 200 instead
 * of ccol_http_invalid_url.
 */
TEST(redirect_transport, tcp_hop_cannot_redirect_to_a_unix_socket) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_mixed_case_unix_scheme_is_refused_too) {
  /* Every place that parses a URL compares the scheme without case, so the
   * refusal must compare without case too. Without that, "HtTp+UnIx://"
   * reaches the Unix listener while "http+unix://" does not. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket-mixed-case");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_second_hop_cannot_reach_a_unix_socket_either) {
  /* The check runs on every hop, against the hop that the Location arrived
   * on, so an ordinary TCP hop in front of the hostile one changes
   * nothing. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-then-redirect-to-unix-socket");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, async_tier_refuses_a_unix_socket_redirect) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  REQUIRE_NE((void *)f, NULL);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  chttpcli_response *resp = raw->resp;
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, pooled_sync_tier_refuses_a_unix_socket_redirect) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-unix-socket");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, a_unix_hop_still_follows_its_own_relative_redirect) {
  /* The positive half of the same rule: a hop that is already bound to a
   * socket can keep redirecting inside that socket, so the refusal cannot
   * be a flat rule of "no Unix target after hop 0". */
  char url[256];
  make_unix_url(url, sizeof(url), "/redirect-relative");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(redirect_transport, a_unix_hop_cannot_redirect_to_a_different_socket) {
  char url[256];
  make_unix_url(url, sizeof(url), "/redirect-to-other-unix-socket");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(redirect_transport, resolver_level_decisions) {
  /* Unit-level coverage of the same rule, for the shapes that the mock
   * server cannot produce easily: a decoded socket path that another
   * percent-encoding writes in a different but equal way, and a relative
   * reference, which can only resolve back onto the transport of the
   * base. */
  char *resolved = NULL;

  /* With a TCP base, the client refuses every Unix target, however the
   * Location writes it. */
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "http://example.com/a", "http+unix://%2Ftmp%2Fx.sock/secret",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "http://example.com/a", "HTTP+UNIX://%2ftmp%2fx.sock/secret",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;

  /* A relative reference against a TCP base stays on the transport of that
   * base, so the client always allows it. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "http://example.com/a/b", "../c", /*prevent_downgrade=*/false,
      &resolved));
  REQUIRE_STREQ(resolved, "http://example.com/c");
  free(resolved);
  resolved = NULL;

  /* With a Unix base, the client allows the same socket, even when the
   * Location writes the percent-encoding in another letter case, but not a
   * different socket. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2ftmp%2fx.sock/b",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a", "http+unix://%2Ftmp%2Fy.sock/b",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;

  /* A relative reference against a Unix base builds the socket of that base
   * again, so the client allows it. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "http+unix://%2Ftmp%2Fx.sock/a/b", "../c", /*prevent_downgrade=*/false,
      &resolved));
  REQUIRE_STREQ(resolved, "http+unix://%2Ftmp%2Fx.sock/c");
  free(resolved);
}

/* ========================================================================== */
/*                     STREAMING ABORT TEST                                   */
/* ========================================================================== */

static size_t abort_write_fn(const void *data, size_t len, void *ctx) {
  (void)data;
  (void)len;
  (void)ctx;
  /* A streaming write_fn that returns a count below len asks for an abort;
   * here it returns 0. _on_body sees that the return value of
   * requested_sink_fn is not len and sets ctx->aborted, which
   * chttp_do_internal and the async engine both report to the caller as
   * ccol_http_transfer_aborted. */
  return 0;
}

TEST(http, streaming_write_fn_abort_returns_transfer_aborted) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  int status = 0;
  ccol_retval_t rv = chttpclient_do_streaming(chttp_default_client(), req,
                                              abort_write_fn, NULL, &status);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
}

/* ========================================================================== */
/*                     CUSTOM ALLOCATOR RESPONSE LIFETIME TEST                */
/* ========================================================================== */

TEST(custom_allocator, response_freed_before_client_with_custom_alloc) {
  /* Checks two things: (a) the custom allocator of the client allocates the
   * response, and (b) freeing the response before destroying the client
   * leaves the counts of allocations and frees balanced. */
  char url[128];
  make_url(url, sizeof(url), "/get");

  atomic_store(&g_alloc_count, 0);
  atomic_store(&g_free_count, 0);

  ccol_memmgmt_procs_t mp = {.malloc = tracked_malloc,
                             .free = tracked_free,
                             .calloc = tracked_calloc,
                             .realloc = tracked_realloc};
  char *err = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttp_request_free(req);

  /* Frees the response BEFORE the client. The contract asks for this order
   * with a custom allocator, because resp->_m_procs borrows the allocator
   * of the client. */
  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);

  REQUIRE_GT(atomic_load(&g_alloc_count), 0);
  REQUIRE_GT(atomic_load(&g_free_count), 0);
  REQUIRE_EQ(atomic_load(&g_alloc_count), atomic_load(&g_free_count));
}

/* ========================================================================== */
/*                     BORROWED HEADER MAP CONTENT-TYPE TESTS                 */
/* ========================================================================== */

TEST(http, run_query_borrowed_map_mixed_case_ct_no_duplication) {
  /* A regression test. _serialize_request must not look "content-type" up
   * with an exact-key chmap lookup: such a lookup does not see a borrowed
   * chmap whose key is "Content-Type" in mixed case, and the client then
   * adds a second Content-Type header from body.content_type.
   *
   * _scan_header_presence runs a linear scan that ignores the case, so it
   * finds the header whatever the case of the key is. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "text/csv"); /* a key in mixed case */

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Exactly one Content-Type header must reach the server: the borrowed map
   * already holds one, so the client must add none. */
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_lowercase_ct_no_duplication) {
  /* The same case as above, but with a key in lowercase, which checks that
   * the common path (the one without a borrowed map) still works. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "content-type", "text/csv");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, patch_borrowed_map_mixed_case_ct_no_duplication) {
  /* _serialize_request adds a "content-type:" header for a request with a
   * body (a POST, a PUT or a PATCH) whenever the caller set none.
   * _scan_header_presence must see a "Content-Type" in mixed case in the
   * borrowed map, so that the client adds none and the header of the
   * caller stays the only Content-Type. */
  char url[128];
  make_url(url, sizeof(url), "/count-content-type");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Type", "application/json");

  const char *payload = "patch";
  chttp_request_body_t body = {.data = payload, .len = 5, .content_type = NULL};

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_PATCH, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*         BORROWED HEADER MAP CASE-SENSITIVITY (NON-CONTENT-TYPE) TESTS      */
/* ========================================================================== */

/*
 * The same contract as the Content-Type tests above, for the other five
 * headers that _serialize_request adds a default for (Accept, User-Agent,
 * Content-Length, Authorization and Expect): the check for the presence of
 * each one must ignore the letter case.
 *
 * chttp_request_get_header looks a key up exactly, against a string that it
 * turned into lowercase, so it misses the natural case of a key in a
 * borrowed chmap, for example "Accept". The default that the client adds
 * then lands ON TOP OF the header of the caller instead of adding nothing,
 * and two lines that disagree reach the wire.
 *
 * The scan of _map_has_content_type ignores the case for Content-Type
 * alone, while _scan_header_presence does all six checks in one pass.
 *
 * Each case below sets exactly one header in mixed case through a borrowed
 * map with chttp_run_query, so nothing goes through
 * chttp_request_set_header, which turns a name into lowercase. Each case
 * then asserts that /count-header sees that header line exactly once on
 * the wire.
 */

TEST(http, run_query_borrowed_map_mixed_case_accept_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Accept", "application/xml");
  chmap_insert(hdrs, "x-count-name", "accept");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_user_agent_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "User-Agent", "my-agent/1.0");
  chmap_insert(hdrs, "x-count-name", "user-agent");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_content_length_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Length", "4");
  chmap_insert(hdrs, "x-count-name", "content-length");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_authorization_no_duplication) {
  /* This path runs only when the URL ALSO carries credentials in its
   * userinfo, because only then does auto_authorization exist for has_auth
   * to stop. An "Authorization" in mixed case with no userinfo in the URL
   * can never appear twice, since nothing else writes an authorization
   * line, so this case must use a URL with userinfo. */
  char base[128];
  make_url(base, sizeof(base), "/count-header");
  char url[192];
  /* Puts "user:pass@" directly after "http://". */
  const char *scheme_end = strstr(base, "://");
  snprintf(url, sizeof(url), "%.*s://user:pass@%s", (int)(scheme_end - base),
           base, scheme_end + 3);

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Authorization", "Bearer token");
  chmap_insert(hdrs, "x-count-name", "authorization");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mixed_case_expect_no_duplication) {
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Expect", "100-continue");
  chmap_insert(hdrs, "x-count-name", "expect");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->headers = hdrs;
  req->expect_continue = true; /* this ALSO writes an "expect:" line when
                                * has_expect is wrongly false */

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  req->headers = NULL; /* borrowed: chttp_request_free must not destroy it */
  chttp_request_free(req);
  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_case_variant_duplicate_keys_deduplicated) {
  /* This test differs from the ones above, each of which sets exactly ONE
   * header in mixed case and checks that no default from the client lands
   * beside it. A chmap keys an entry by its exact bytes, so a borrowed map
   * can hold "Host" and "host" as two different entries at the same time,
   * and without the case-insensitive dedup of _serialize_request, BOTH
   * reach the wire as separate header lines. Two Host headers are not a
   * cosmetic problem: RFC 7230 SS5.4 asks a server to reject such a
   * request, naming "more than one Host header field" as a condition for a
   * 400. */
  char url[128];
  make_url(url, sizeof(url), "/count-header");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Host", "should-not-reach-the-wire-twice");
  chmap_insert(hdrs, "host", "also-should-not-reach-the-wire-twice");
  chmap_insert(hdrs, "x-count-name", "host");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "1");

  chmap_destroy(hdrs);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_crlf_in_value_rejected) {
  /* The CRLF check of _serialize_request, which backs up the same check in
   * chttp_request_set_header. req->headers is an internal chmap handle that
   * a caller can fill directly and hand to chttp_run_query, skipping
   * chttp_request_set_header, which is exactly what chttp_run_query itself
   * does here. The loop that writes the headers must therefore also reject
   * a CR or an LF inside a value. The call must fail before it opens a
   * connection, so nothing reaches the wire and no resp leaks. */
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-evil", "1\r\nx-injected: evil");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_crlf_in_name_rejected) {
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "x-evil\r\nx-injected", "1");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_non_tchar_name_rejected) {
  /* The tchar check of _serialize_request, which backs up the same check in
   * chttp_request_set_header. req->headers is an internal chmap handle that
   * a caller can fill directly and hand to chttp_run_query, skipping
   * chttp_request_set_header, which is exactly what chttp_run_query itself
   * does here. The loop that writes the headers must therefore also reject
   * a name byte outside the tchar set; the byte here is a space, and the
   * name holds no CR and no LF. The call must fail before it opens a
   * connection, so nothing reaches the wire and no resp leaks. */
  char url[128];
  make_url(url, sizeof(url), "/get");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "X Evil", "1");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_GET, url, NULL, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, run_query_borrowed_map_transfer_encoding_rejected) {
  /* The Transfer-Encoding check of _serialize_request, which backs up the
   * same check in chttp_request_set_header. req->headers is an internal
   * chmap handle that a caller can fill directly and hand to
   * chttp_run_query, skipping chttp_request_set_header, which is exactly
   * what chttp_run_query itself does here. The guard against ambiguous
   * framing must therefore also catch a "Transfer-Encoding" key in a
   * borrowed map. The call must fail before it opens a connection, so
   * nothing reaches the wire and no resp leaks. This test uses POST, a
   * method that carries a body, because that is the case where an accepted
   * Transfer-Encoding header collides with the Content-Length header that
   * the client adds. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Transfer-Encoding", "chunked");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, post_body_content_type_crlf_rejected) {
  /* content_type has no setter of its own where a check could run: it
   * arrives inside a chttp_request_body_t, and chttp_request_new_mp copies
   * it unchanged. _serialize_request is therefore the only place that
   * checks it before it reaches the wire as
   * "content-type: <value>\r\n". */
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "data";
  chttp_request_body_t body = {
      .data = payload,
      .len = strlen(payload),
      .content_type = "text/plain\r\nx-injected: evil"};

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(request, set_header_mismatched_content_length_rejected) {
  /* _serialize_request adds no Content-Length of its own whenever ANY
   * content-length header is already there, and then writes the value of
   * the caller unchanged, so it must compare that value against the body
   * bytes that it appends a few lines later. A Content-Length from the
   * caller can differ from the real body length, and without that check
   * the declared framing of the request disagrees with what goes onto the
   * wire. That is the same hazard that the rejection of Transfer-Encoding
   * guards against in set_header_rejects_transfer_encoding above, caused
   * here by a wrong length instead of a wrong transfer coding. The call
   * must fail before it opens a connection. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "0"),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_leading_sign_rejected) {
  /* The check above cannot rest on the numeric VALUE of a Content-Length
   * from the caller alone, because strtoull accepts a leading '+' or '-'
   * sign before the digits, while the request parser of this codebase
   * (chttp1_parser.c, which chttpserver.c uses) does not. A value like
   * "+22" that equals the real body length therefore passes that check,
   * and the client writes it onto the wire as "content-length: +22\r\n",
   * which the server-side parser of this library rejects as an invalid
   * Content-Length, although the client accepted it. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), "+%zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_embedded_whitespace_rejected) {
  /* The same hazard as in set_header_content_length_leading_sign_rejected
   * above, but through a leading space instead of a sign: strtoull skips
   * whitespace at the start before it parses the digits, so " 22", which
   * equals the real body length, passes the check and reaches the wire as
   * "content-length:  22\r\n", with two spaces after the colon. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), " %zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
}

TEST(request, set_header_content_length_leading_zeros_still_accepted) {
  /* A value with leading zeros, such as "00022", is a plain string of digits,
   * and parse_uint64_decimal in chttp1_parser.c places no limit on leading
   * zeros either, so the client must accept it. The digits-only check that
   * closes the '+' case and the whitespace case above must not reject this
   * legal, if unusual, form. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  const char *payload = "some-nonempty-payload";
  size_t payload_len = strlen(payload);
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, payload_len);
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  char cl_val[32];
  snprintf(cl_val, sizeof(cl_val), "000%zu", payload_len);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", cl_val),
             ccol_success);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, run_query_borrowed_map_mismatched_content_length_rejected) {
  /* Borrowed-map counterpart of set_header_mismatched_content_length_rejected
   * above: req->headers is an internal chmap handle a caller can populate
   * directly, bypassing chttp_request_set_header entirely, exactly like
   * chttp_run_query itself does here. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  chmap_construct(hdrs, char *, char *);
  chmap_insert(hdrs, "Content-Length", "999");

  const char *payload = "data";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_run_query(CHTTP_POST, url, &body, hdrs, &resp);
  REQUIRE_EQ(rv, ccol_invalid_args);
  REQUIRE_EQ((void *)resp, NULL);

  chmap_destroy(hdrs);
}

TEST(http, set_header_host_reaches_the_wire) {
  /* The loop that writes the headers in _serialize_request must not always
   * skip a "host" entry from req->headers. Such a skip assumes that the
   * code above has already written that line, but the block above writes a
   * Host line only when the request has no Host header, so a skip that
   * always runs drops a Host header from the caller off the wire: there is
   * then neither a line from the client nor a line from the caller. It
   * does this even for a header that the caller set through
   * chttp_request_set_header, which always turns a name into lowercase, so
   * this is more than a question of letter case in a borrowed map. The
   * test uses /echo-header-raw to check the ACTUAL value that the server
   * got, not only a count. */
  char url[128];
  make_url(url, sizeof(url), "/echo-header-raw");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "Host", "custom-host.example");
  chttp_request_set_header(req, "x-echo-name", "host");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "custom-host.example");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     ZERO-LENGTH BODY TESTS                                 */
/* ========================================================================== */

TEST(request, zero_len_body_content_type_still_reaches_the_wire) {
  /* The end-to-end companion of
   * request.body_data_non_null_but_zero_len_treated_as_no_body. A caller
   * can set an explicit content_type on an empty body, which is a real and
   * legal shape, for example CHTTP_JSON_BODY("", 0), and the client must
   * still write a real Content-Type header onto the wire for it instead of
   * dropping that header because no body goes with it. The test uses
   * /echo-header-raw to check the value that the server got, not only that
   * the call succeeded. */
  char url[128];
  make_url(url, sizeof(url), "/echo-header-raw");

  chttp_request_body_t body = {
      .data = "ignored", .len = 0, .content_type = "text/plain"};
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttp_request_set_header(req, "x-echo-name", "content-type");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "text/plain");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(request, body_data_non_null_but_zero_len_treated_as_no_body) {
  /* The client must not copy a body into the request when body.data is not
   * NULL and body.len is 0; the condition (body->data && body->len > 0)
   * guards that copy. But the client copies content_type whenever the
   * caller gives one, whatever the body length is: an empty body with an
   * explicit content type, such as CHTTP_JSON_BODY("", 0), is a legal
   * request shape, and the documented contract of chttp_request_new copies
   * body->content_type whenever it is not NULL. */
  const char *data = "ignored";
  chttp_request_body_t body = {
      .data = data, .len = 0, .content_type = "text/plain"};

  chttp_request_t *req =
      chttp_request_new(CHTTP_POST, "http://example.com/", &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)0);
  REQUIRE_NE((void *)req->body.content_type, NULL);
  REQUIRE_STREQ(req->body.content_type, "text/plain");
  chttp_request_free(req);
}

TEST(http, post_zero_len_body_no_data_transmitted) {
  /* A POST whose body struct has non-NULL data and a len of 0 has no body:
   * the client copies no body data into the request and sends none. The
   * server echoes the body that it got, which must be empty. */
  char url[128];
  make_url(url, sizeof(url), "/post");

  chttp_request_body_t body = {
      .data = "ignored", .len = 0, .content_type = "text/plain"};

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server echoes the body bytes that it got, and the count must be
   * zero, because the client sent none. */
  REQUIRE_EQ(resp->body_len, (size_t)0);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     DELETE WITH BODY TEST                                  */
/* ========================================================================== */

TEST(http, delete_body_not_transmitted) {
  /* _serialize_request writes the bytes of req->body onto the wire only for
   * a body_carrying_method (a POST, a PUT or a PATCH), and DELETE is not
   * one of them, so body data inside the request struct never reaches the
   * server. This test states that behavior and guards against a change
   * that starts to send it. */
  char url[128];
  make_url(url, sizeof(url), "/delete-echo");

  chttp_request_body_t body = CHTTP_TEXT_BODY("payload", 7);
  chttp_request_t *req = chttp_request_new(CHTTP_DELETE, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  /* The request struct holds the body. */
  REQUIRE_NE((void *)req->body.data, NULL);
  REQUIRE_EQ(req->body.len, (size_t)7);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The server echoes the body that it got, which must be empty. */
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(http, delete_with_body_content_type_not_sent) {
  /* A regression test. _serialize_request must not write a "content-type:"
   * header for every request whose req->body.content_type is set; it must
   * first ask whether the method carries a body onto the wire at all. A
   * DELETE never sends its body (see delete_body_not_transmitted above),
   * so without that question a request reaches the server declaring a
   * content-type for a body that the client never sent, and with no
   * Content-Length either. The same body_carrying_method check must guard
   * the content-type line, the body bytes and the Content-Length line. */
  char url[128];
  make_url(url, sizeof(url), "/echo-content-type");

  chttp_request_body_t body = CHTTP_JSON_BODY("{\"k\":\"v\"}", 9);
  chttp_request_t *req = chttp_request_new(CHTTP_DELETE, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_NE((void *)req->body.content_type, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_do(req, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* /echo-content-type echoes the content-type value that it got, or an
   * empty value when the header is absent. The value must be empty here. */
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     EXPECT: 100-CONTINUE (TIER 1)                          */
/* ========================================================================== */

TEST(expect_continue, interim_100_then_body_sent) {
  /* /expect-continue-echo sends "100 Continue" first, then reads the body
   * and echoes it. This exercises the branch of _chttp_send_and_read that
   * sees an interim 100, and covers _parse_ctx_reset_for_continue too. The
   * headers and the body of the final response must be exactly the real
   * response, with nothing from the interim message left in them. */
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-echo");

  const char *payload = "hold-until-continue";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(expect_continue, server_rejects_without_100) {
  /* /expect-continue-reject answers 417 directly, without ever sending "100
   * Continue" or reading a body. This exercises the branch of
   * _chttp_send_and_read where the server answered directly: the client
   * must give that response to the caller unchanged, by RFC 7231 SS5.1.1,
   * and must never send the body. */
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-reject");

  const char *payload = "should-never-be-sent";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 417);
  REQUIRE_STREQ(resp->body, "expectation failed");

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(expect_continue, direct_rejection_without_100_never_pools_connection) {
  /* The branch of _chttp_send_and_read where the server answered directly
   * must always set *keep_alive_out to false, not only when trailing bytes
   * follow the response. chttp1_should_keep_alive() reads the Connection
   * header of the response alone, so its answer ignores the fact that the
   * client never sent the declared body on this connection; RFC 7231
   * SS5.1.1 only SHOULD closes the connection here, and never MUST closes
   * it. If the pool kept that connection and reused it later, the server,
   * which from its own view is still in the middle of reading the rejected
   * request, would take the bytes of a later, unrelated request on that
   * connection as more of the body of the first request.
   *
   * /expect-continue-reject-keepalive answers 417 directly, with no "100
   * Continue" and no read of the body, just as /expect-continue-reject
   * does, but it leaves the connection open on purpose instead of closing
   * it, to model a naive server that still obeys the rules. The accept
   * count is the clear signal: a pooled connection makes that count rise by
   * only 1 across both requests, while the correct behavior always opens a
   * fresh connection for the second, unrelated request, so the count rises
   * by 2. */
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/expect-continue-reject-keepalive");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  const char *payload = "should-never-be-sent";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req1 = chttp_request_new(CHTTP_POST, url1, &body, NULL);
  REQUIRE_NE((void *)req1, NULL);
  req1->expect_continue = true;
  chttpcli_response *resp1 = NULL;
  ccol_retval_t rv1 = chttpclient_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 417);
  chttpclient_resp_free(resp1);

  chttp_request_t *req2 = chttp_request_new(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  chttpcli_response *resp2 = NULL;
  ccol_retval_t rv2 = chttpclient_do(cli, req2, &resp2);
  chttp_request_free(req2);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpclient_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
}

TEST(
    expect_continue,
    dead_connection_after_100_with_fake_leftover_final_not_retried_with_stale_state) {
  /* The *any_bytes_read_out of _chttp_read_message must count carry_in
   * bytes too: bytes that an earlier call read off the wire and passed
   * forward. A non-empty carry_in goes straight into the parser here, where
   * it can call on_header, on_headers_complete and on_body against the pctx
   * and the body sink of the caller, exactly as a live read does.
   *
   * The retry-once safety net for a reused connection in chttp_do_internal
   * reads *any_bytes_read_out to decide whether the parser has parsed
   * nothing yet, so the carry_in bytes must be in that count. Without them,
   * a carry-in parse that cannot complete fills the pctx and the body
   * buffer with the headers of a stale response and a body prefix, and the
   * retry then sends the request again on a fresh connection while it
   * reuses that SAME polluted pctx and buffer, so the data of the stale
   * attempt mixes into the response that the caller gets.
   *
   * /expect-continue-fake-final-then-die writes "100 Continue" and, in the
   * same send() and the same TCP segment, more bytes that look like a
   * complete header block of a final response (with status 200, a clear
   * "x-stale" header and a declared Content-Length of 100), followed by a
   * body prefix below 100 bytes. Then the route closes without ever
   * finishing that body. The correct outcome is a hard failure, because the
   * carry-in parse that cannot complete rules the safe retry out; the wrong
   * outcome is a response built from the stale leftover and, after a
   * retry, from the data of a fresh connection.
   *
   * The retry-once safety net in chttp_do_internal runs only for a REUSED
   * connection from the pool, never for a fresh one, so the request to
   * /expect-continue-fake-final-then-die must be the SECOND request on this
   * client, reusing a connection that an earlier, successful /keepalive
   * request put into the idle pool. Without that, the path that this test
   * targets never runs.
   *
   * Both outcomes end in an error here, so the return code alone cannot
   * tell them apart: the first outcome is no retry at all, and the second
   * is a retry onto a fresh connection, which reaches this same route and
   * dies too, because the route always behaves this way. The accept count
   * is the deciding signal. The correct behavior opens exactly ONE
   * connection in total, the one of the first /keepalive request, because
   * nothing retries the failed reuse of the second request, while a wrong
   * retry opens a SECOND connection. The test uses its own client instead
   * of the process-wide default one, so that a pooled connection of an
   * unrelated test to this origin cannot change the count. */
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-fake-final-then-die");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  chttp_request_t *req1 = chttp_request_new(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval_t rv1 = chttpclient_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);

  const char *payload = "hold-until-continue";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req2 = chttp_request_new(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;

  chttpcli_response *resp2 = NULL;
  ccol_retval_t rv2 = chttpclient_do(cli, req2, &resp2);
  REQUIRE_NE((int)rv2, (int)ccol_success);
  REQUIRE_EQ((void *)resp2, NULL);

  chttp_request_free(req2);
  chttpclient_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli);
}

TEST(expect_continue, dead_connection_after_100_clean_eof_not_retried) {
  /* Inside _chttp_send_and_read, the client can get an explicit "100
   * Continue" and send the body, and the read of the final response can
   * then end in a clean EOF on a message boundary, with no leftover bytes
   * at all. That case must still set *any_bytes_read_out. This differs from
   * the sibling test
   * dead_connection_after_100_with_fake_leftover_final_not_retried_with_stale_state
   * above, which needs fake trailing bytes in the same read for the
   * behavior that it targets.
   *
   * With a false *any_bytes_read_out, the retry-once safety net for a reused
   * connection in chttp_do_internal reads the hop as "the client sent nothing,
   * a retry is safe" and sends the WHOLE request again, body included, to a
   * brand-new connection, although the server confirmed with "100 Continue"
   * moments earlier that it was alive, and had taken the body on the first
   * connection. For a request that is not idempotent, the server therefore
   * processes the body twice.
   *
   * /expect-continue-die-after-100-clean sends "100 Continue" in its own
   * separate send() call, reads the full declared body, adds one to
   * g_die_after_100_clean_body_recv_count, and then closes with NO more
   * bytes: a clean EOF for the final response that never came, with
   * nothing left over to reach the carry-in path of the sibling test
   * above.
   *
   * As in the sibling test, the request to this route must be the SECOND
   * request on this client, reusing a connection that an earlier,
   * successful /keepalive request put into the idle pool; without that,
   * the retry path under test never runs.
   *
   * Both outcomes return an error here, so the return code alone cannot
   * tell them apart: the first outcome is no retry, and the second is a
   * retry onto a fresh connection, which reaches this same route and dies
   * too, because the route always behaves this way. Two signals decide.
   * The first is the accept count: the correct behavior opens exactly ONE
   * connection in total, and a wrong retry opens a SECOND. The second is
   * the body-receive count: the correct behavior gives the body to the
   * server exactly once, and a wrong retry gives it twice. The test uses
   * its own client instead of the process-wide default one, so that a
   * pooled connection of an unrelated test to this origin cannot change
   * either count. */
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-die-after-100-clean");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();
  atomic_store(&g_die_after_100_clean_body_recv_count, 0);

  chttp_request_t *req1 = chttp_request_new(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  chttpcli_response *resp1 = NULL;
  ccol_retval_t rv1 = chttpclient_do(cli, req1, &resp1);
  chttp_request_free(req1);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);

  const char *payload = "must-not-be-sent-twice";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req2 = chttp_request_new(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;

  chttpcli_response *resp2 = NULL;
  ccol_retval_t rv2 = chttpclient_do(cli, req2, &resp2);
  REQUIRE_NE((int)rv2, (int)ccol_success);
  REQUIRE_EQ((void *)resp2, NULL);

  chttp_request_free(req2);
  chttpclient_resp_free(resp2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);
  REQUIRE_EQ(atomic_load(&g_die_after_100_clean_body_recv_count), 1);

  chttpclient_destroy(cli);
}

TEST(expect_continue, wait_times_out_body_sent_anyway) {
  /* /post is an ordinary route that knows nothing about Expect:
   * 100-continue: it waits and reads the full body before it answers. This
   * exercises the timeout branch of _chttp_send_and_read: after
   * CHTTP_100_CONTINUE_WAIT_US with no interim response, the client sends
   * the body anyway and reads the real response in the normal way, which
   * here is the only response. This test is slow, about 1 second, and that
   * is its whole point. */
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"sent\":\"after-timeout\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen(payload));
  REQUIRE_STREQ(resp->body, payload);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/*
 * A direct final response that the server starts inside the window of
 * CHTTP_100_CONTINUE_WAIT_US and finishes after it. Because the window
 * bounds only the wait for the first byte of a response, the client must
 * read that response to its end with the parser that holds its start, and
 * must never send the body that the answer made unnecessary. Each case runs
 * on its own client, which it destroys before it waits for the route to
 * report how many body bytes reached the server. The chunked route serves
 * the streaming cases, so that both framings end after the window.
 */
typedef struct {
  char data[64];
  size_t len;
} slow_final_sink_t;

static size_t slow_final_write(const void *data, size_t len, void *ctx) {
  slow_final_sink_t *sink = (slow_final_sink_t *)ctx;
  size_t room = sizeof(sink->data) - 1 - sink->len;
  size_t n = len < room ? len : room;
  memcpy(sink->data + sink->len, data, n);
  sink->len += n;
  sink->data[sink->len] = '\0';
  return len;
}

typedef struct {
  ccol_retval_t rv;
  int status;
  char body[64];
  int body_bytes_at_server;
  bool route_done;
} slow_final_outcome_t;

static slow_final_outcome_t run_slow_final_case(int tier, bool streaming,
                                                long request_timeout_ms) {
  slow_final_outcome_t out = {.rv = ccol_unexpected_failure, .status = -1};
  int done_before = atomic_load(&g_slow_final_done);
  int bytes_before = atomic_load(&g_slow_final_body_bytes);
  char url[160];
  make_url(url, sizeof(url),
           streaming ? "/expect-continue-slow-final-chunked"
                     : "/expect-continue-slow-final");
  const char *payload = "must-never-be-sent";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (req && cli != CHTTPCLI_INVALID) {
    req->expect_continue = true;
    if (request_timeout_ms > 0)
      (void)chttpclient_set_request_timeout(
          cli, (uint64_t)request_timeout_ms * 1000u);
    slow_final_sink_t sink = {.len = 0};
    chttpcli_response *resp = NULL;
    if (tier == 2) {
      ctpool_future *f = streaming ? chttpclient_do_async_streaming(
                                         cli, req, slow_final_write, &sink)
                                   : chttpclient_do_async(cli, req);
      chttpcli_async_result_t *r = f ? chttpclient_async_result_get(f) : NULL;
      if (r) {
        out.rv = r->rv;
        resp = r->resp;
        r->resp = NULL;
        chttpclient_async_result_free(r);
      }
      if (f) ctpool_future_free(f);
    } else if (streaming) {
      out.rv = tier == 1 ? chttpclient_do_streaming(cli, req, slow_final_write,
                                                    &sink, &out.status)
                         : chttpclient_do_pooled_streaming(
                               cli, req, slow_final_write, &sink, &out.status);
    } else {
      out.rv = tier == 1 ? chttpclient_do(cli, req, &resp)
                         : chttpclient_do_pooled(cli, req, &resp);
    }
    if (resp) {
      out.status = resp->status_code;
      if (!streaming && resp->body)
        snprintf(out.body, sizeof(out.body), "%.*s", (int)resp->body_len,
                 (const char *)resp->body);
      chttpclient_resp_free(resp);
    }
    if (streaming) snprintf(out.body, sizeof(out.body), "%s", sink.data);
  }
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  if (req) chttp_request_free(req);
  /* The route publishes its count once the client has closed the
   * connection. The bound is a safety net against a hang, not a timing
   * assumption. */
  for (int i = 0; i < 3000 && atomic_load(&g_slow_final_done) == done_before;
       i++)
    usleep(10000);
  out.route_done = atomic_load(&g_slow_final_done) != done_before;
  out.body_bytes_at_server =
      atomic_load(&g_slow_final_body_bytes) - bytes_before;
  return out;
}

/* This asserts one outcome. It is a macro so that a failure names the line
 * of the case that failed. */
#define REQUIRE_SLOW_FINAL_READ_WHOLE(o)     \
  do {                                       \
    REQUIRE_EQ((o).rv, ccol_success);        \
    REQUIRE_EQ((o).status, 200);             \
    REQUIRE_STREQ((o).body, "helloworld");   \
    REQUIRE_TRUE((o).route_done);            \
    REQUIRE_EQ((o).body_bytes_at_server, 0); \
  } while (0)

TEST(expect_continue, direct_final_that_outlasts_the_window_tier1) {
  /* This test is non-vacuous: a window that bounds the whole read makes the
   * client drop the parser of the half-read response when the window ends,
   * send the body, and parse the tail of the response as a new message. The
   * request then fails with ccol_http_transfer_aborted, or waits for good
   * when no request timeout is set. */
  slow_final_outcome_t a = run_slow_final_case(1, false, 0);
  slow_final_outcome_t b = run_slow_final_case(1, true, 10000);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, direct_final_that_outlasts_the_window_tier3) {
  slow_final_outcome_t a = run_slow_final_case(3, false, 10000);
  slow_final_outcome_t b = run_slow_final_case(3, true, 0);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, direct_final_that_outlasts_the_window_tier2) {
  /* This test is non-vacuous: a sweep that ends the wait while the response
   * is half read sends the body that the direct answer made unnecessary,
   * and the route counts it. */
  slow_final_outcome_t a = run_slow_final_case(2, false, 0);
  slow_final_outcome_t b = run_slow_final_case(2, true, 10000);
  REQUIRE_SLOW_FINAL_READ_WHOLE(a);
  REQUIRE_SLOW_FINAL_READ_WHOLE(b);
}

TEST(expect_continue, explicit_expect_header_suppresses_the_wait) {
  /* chttpclient.h says that expect_continue "has no effect if... the caller
   * already set an explicit Expect header", in which case
   * _serialize_request writes no "expect: 100-continue" onto the wire, so
   * the use_100_continue value that chttp_do_internal computes must also
   * check for an Expect header from the caller. Without that check, the hop
   * still goes through _chttp_send_and_read and waits the full
   * CHTTP_100_CONTINUE_WAIT_US of 1000ms for an interim response that can
   * never arrive, because the client sent no "expect:" line. /post is an
   * ordinary route that
   * knows nothing about Expect: 100-continue: it reads the body and answers
   * at once, so a correct request finishes almost at once, while a broken
   * client takes about 1 second here, which is the point of the assertion
   * on the elapsed time below. */
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"x\":1}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  chttp_request_set_header(req, "Expect", "204-content");

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* This bound is far below CHTTP_100_CONTINUE_WAIT_US, which is 1000ms, so
   * a request that wrongly waits for an interim response fails here, even
   * with the scheduling jitter of a busy CI machine. */
  REQUIRE_LT(elapsed_ms, 500);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

TEST(expect_continue,
     reused_connection_dies_after_partial_interim_line_is_not_retried) {
  /* The window of CHTTP_100_CONTINUE_WAIT_US bounds only the wait for the
   * first byte of a response. /expect-continue-timeout-then-die writes an
   * incomplete "100 Con..." fragment inside the window, so the client reads
   * that message on past the window and never sends the body. The route
   * then closes without an answer, and the read fails in the middle of a
   * message on a reused connection.
   *
   * Since bytes of a response arrived, the server provably received this
   * request, so the retry-once safety net for a reused connection must not
   * send it again: the request fails with ccol_http_transfer_aborted, and
   * the client opens no new connection. This test is slow, about 1.3
   * seconds, because the route holds the connection that long. */
  char keepalive_url[160], timeout_url[160];
  make_url(keepalive_url, sizeof(keepalive_url), "/keepalive");
  make_url(timeout_url, sizeof(timeout_url),
           "/expect-continue-timeout-then-die");

  chttpcli_construct(cli);

  /* Puts a connection to this origin into the idle pool, so that a later
   * request can reuse it. */
  chttp_request_t *warm =
      chttp_request_new(CHTTP_GET, keepalive_url, NULL, NULL);
  REQUIRE_NE((void *)warm, NULL);
  chttpcli_response *warm_resp = NULL;
  ccol_retval_t warm_rv = chttpclient_do(cli, warm, &warm_resp);
  chttp_request_free(warm);
  REQUIRE_EQ(warm_rv, ccol_success);
  REQUIRE_EQ(warm_resp->status_code, 200);
  chttpclient_resp_free(warm_resp);

  int accepts_before = test_server_accept_count();

  const char *payload = "{\"n\":1}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req =
      chttp_request_new(CHTTP_POST, timeout_url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  if (resp) chttpclient_resp_free(resp);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  chttpclient_destroy(cli);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);
  REQUIRE_EQ(accepts_after - accepts_before, 0); /* no retry */
}

TEST(expect_continue, early_hints_before_100_continue_still_waits) {
  /* /expect-continue-with-hints sends "103 Early Hints" BEFORE "100
   * Continue". The stop_at_status of 100 in _chttp_read_message_loop must
   * discard the 103 and go on waiting inside the same
   * CHTTP_100_CONTINUE_WAIT_US budget, instead of reading the 103 as the
   * "100 Continue" itself or as the final response; such a client sends
   * the body at once, without a real "100 Continue", and falls out of step
   * with the read-then-answer order of the route. */
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-with-hints");

  const char *payload = "hello";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(expect_continue,
     hints_both_sides_of_100_continue_each_phase_gets_its_own_cap) {
  /* With /expect-continue-hints-both-sides/40/40, the client discards 40
   * interim "103 Early Hints" responses while it waits for "100 Continue";
   * a real "100 Continue" then arrives and the client sends the body; THEN
   * it discards 40 MORE while it reads the final response. That is 80 in
   * total, well past CHTTP_MAX_INTERIM_RESPONSES of 64, but each of the two
   * phases stays inside its own budget of 64.
   *
   * The _chttp_send_and_read of Tier 1 gives the wait for the continue and
   * the read of the final response to two SEPARATE calls of
   * _chttp_read_message_loop, each with its own discard counter, so the
   * budget for each phase follows from the structure of the code here.
   * This test pins that down directly, and also gives the async
   * counterpart (the test of the same name in async_expect_continue) a
   * reference that is known to be correct. */
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-hints-both-sides/40/40");

  const char *payload = "hello";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(expect_continue, ignored_for_bodyless_request) {
  /* A GET has no body, so expect_continue must have no effect at all, as its
   * own doc comment says, and the client writes no "expect:" header: the
   * condition in _serialize_request asks for a method that carries a body
   * and for a non-empty body. The ordinary /get
   * route knows nothing about Expect: 100-continue, and it answers in the
   * normal way. */
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttp_request_free(req);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     INTERIM 1xx RESPONSE TESTS (TIER 1)                    */
/* ========================================================================== */

TEST(early_hints, discarded_and_real_response_delivered) {
  /* /early-hints sends "103 Early Hints" as a SEPARATE message before the
   * real "200 OK" response, on an ordinary GET with no Expect:
   * 100-continue anywhere. The general response path
   * (_chttp_read_message_loop, which _chttp_read_response_carry uses) must
   * discard the 103 and go on reading. A client that takes the FIRST
   * message off the wire as the final response gives the caller a
   * status_code of 103 and an empty body, while the real "200 OK" waits
   * unread on the wire. */
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
  chttpclient_resp_free(resp);
}

TEST(early_hints,
     discarded_when_arriving_in_the_same_read_as_the_final_response) {
  /* /early-hints-same-write puts the interim "103 Early Hints" response and
   * the real final response into ONE send() call, so they very probably
   * arrive together in one read(). This test exercises how
   * _chttp_read_message_loop passes the leftover bytes after the boundary
   * of a discarded interim message into the next read, while /early-hints
   * only covers the case where the leftover arrives in its own read. */
  char url[160];
  make_url(url, sizeof(url), "/early-hints-same-write");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_do(req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
  chttpclient_resp_free(resp);
}

TEST(early_hints, connection_stays_reusable_after_discarding_interim_response) {
  /* Pins the connection against a desync of the responses. A client that
   * gives the "103" message to the caller as the final response leaves the
   * connection looking idle and eligible for keep-alive, so the pool keeps
   * it while the real "200 OK" still waits unread on the wire. The NEXT
   * unrelated request that reuses that connection then reads THAT leftover
   * response instead of its own: a real mix-up of responses between two
   * requests.
   *
   * The test runs two /early-hints requests one after the other on the
   * same client, so the second very probably reuses the pooled connection
   * of the first, and then checks that the SECOND request gets ITS OWN
   * correct response, with nothing left over from the
   * first. */
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttpcli_construct(cli);

  for (int i = 0; i < 2; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");
    chttpclient_resp_free(resp);
  }

  chttpclient_destroy(cli);
}

TEST(early_hints, tier1_gives_up_after_too_many_interim_responses) {
  /* The loop in _chttp_read_message_loop that discards interim 1xx
   * responses needs a cap on its iterations. The default
   * request_timeout_ms of this client is 0, which means no timeout, and a
   * server can send interim responses without ever stopping (an endless
   * stream of "103 Early Hints", for example), so such a server holds this
   * call for ever, together with the slot of the concurrency limiter.
   * /endless-early-hints sends 100, which is well above
   * CHTTP_MAX_INTERIM_RESPONSES of 64, so a
   * client with a correct cap gives up with an error long before the
   * server finishes. */
  char url[160];
  make_url(url, sizeof(url), "/endless-early-hints");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_NE(rv, ccol_success);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(early_hints, tier1_discards_exactly_64_before_giving_up) {
  /* This test pins the EXACT boundary of CHTTP_MAX_INTERIM_RESPONSES, which
   * chttpclient_do(3) documents as "after 64 consecutive discarded interim
   * responses". The test tier1_gives_up_after_too_many_interim_responses
   * above only makes sure that SOME cap exists, with a fixed stream of 100
   * hints. Here, 64 discarded interim responses followed by a real answer
   * must succeed, because the 64th is the last legal discard.
   *
   * The cap check in _chttp_read_message_loop must run AFTER the client
   * reads a message and identifies it, never before each read. A check
   * that applies to each read, whatever the message is, makes the loop
   * refuse to read more after 64 discards, so a real final response that
   * comes as the next message (the 65th) is refused although only 64
   * interim responses were discarded, not 65. The loop then stops one
   * message too early and breaks the documented contract. A cap check must
   * run only after the client identifies a message as interim, so that it
   * controls the continuation of the discard loop rather than the read of
   * the next message. The matching loop of the async engine,
   * _async_on_readable_impl, applies the check in the same way. */
  char url64[160];
  make_url(url64, sizeof(url64), "/early-hints-count/64");

  chttpcli_response *resp64 = NULL;
  ccol_retval_t rv64 = chttp_get(url64, &resp64);
  REQUIRE_EQ(rv64, ccol_success);
  REQUIRE_NE((void *)resp64, NULL);
  REQUIRE_EQ(resp64->status_code, 200);
  chttpclient_resp_free(resp64);

  /* One more, 65, must fail: the other side of the same boundary. The async
   * counterpart of this test, in the ASYNC ENGINE section below, pins both
   * tiers to exactly the same cutoff. */
  char url65[160];
  make_url(url65, sizeof(url65), "/early-hints-count/65");

  chttpcli_response *resp65 = NULL;
  ccol_retval_t rv65 = chttp_get(url65, &resp65);
  REQUIRE_NE(rv65, ccol_success);
  REQUIRE_EQ((void *)resp65, NULL);
}

/* ========================================================================== */
/*                     ASYNC ENGINE LIFECYCLE (WHITE-BOX)                     */
/* ========================================================================== */

/*
 * White-box tests for the async engine of chttpclient: the static
 * g_client_reactor of chttpclient.c, a process-wide ccol_event_loop
 * instance that starts when a caller first needs it, whose users a
 * refcount tracks, and which is fully independent of the reactor of
 * chttpserver. The DNS pool, the connect pool and the deadline sweep of
 * this module sit on top of it and back chttpclient_do_async and the
 * pooled-sync tier. The build compiles these helpers only under
 * RUNNING_UNIT_TESTS, the same white-box pattern that tests/cvector uses
 * for cvector_get_capacity.
 *
 * These tests run the engine through full start-and-stop cycles on
 * purpose, rather than through one acquire-and-release pair, to prove that
 * the lazy restart path works; nothing else in this suite exercises it.
 */
extern int _chttpclient_engine_ref_count_for_tests(void);
extern bool _chttpclient_engine_running_for_tests(void);
extern ccol_retval_t _chttpclient_engine_acquire_for_tests(void);
extern void _chttpclient_engine_release_for_tests(void);
extern size_t _chttp_async_chain_struct_size_for_tests(void);
/* _client_engine_release() hands the real teardown (which calls
 * ccol_event_loop_destroy on g_client_reactor, stops the deadline sweep and
 * destroys the DNS pool) to a detached reaper thread instead of blocking
 * the caller. The engine needs this because a release often happens inside
 * one of its own dispatch callbacks (see the several
 * _client_engine_release call sites in chttpclient.c), where a block is not
 * safe.
 *
 * g_client_reactor_refs therefore reaches zero at once, while the real
 * teardown runs later. Every test below that stops the engine calls this
 * function afterward, so that the engine is fully quiet before the test
 * returns. Without that, a reaper thread can still run when the process
 * exits and race the teardown of the process, which valgrind reports as a
 * crash. */
extern void _chttpclient_engine_wait_for_quiescence_for_tests(void);
extern size_t _chttpclient_engine_num_reactor_threads_for_tests(void);
/* An opaque forward declaration: the real definition of struct chttpclient
 * is private to chttpclient.c. This declaration is at file scope on
 * purpose, because a struct tag that first appears inside a parameter list
 * has the scope of that prototype alone, not file scope. Without it, each
 * function prototype below introduces its own separate tag, so every
 * "struct chttpclient *" below would be a different type, and none of
 * them would match another. */
struct chttpclient;

/* Makes every idle connection in the pool of Tier 2 and Tier 3 look older
 * than CHTTP_IDLE_MAX_AGE_MS, so that the next pop from that pool always
 * reaches the eviction branch for a stale entry in _async_idle_pool_take
 * and a test does not wait out the real window of 60 seconds. Only the
 * test
 * async_idle_pool.stale_connection_eviction_releases_engine_reference below
 * uses this. */
extern void _chttpclient_force_async_idle_stale_for_tests(
    struct chttpclient *cli);
/* Shuts down every fd that sits in the async idle pool of Tier 2 and Tier
 * 3, leaving each connection where it is, so that the IDLE dispatch of the
 * reactor reaps each one and frees the engine reference that it held. Only
 * the test
 * async_engine.reaper_spawn_failure_on_a_reactor_thread_does_not_abort
 * below uses this; its own comment says why a release on that thread is
 * the point. */
extern void _chttpclient_shutdown_async_idle_connections_for_tests(
    struct chttpclient *cli);
/* Reads how many connections sit in the async idle pool of Tier 2 and Tier
 * 3, across every origin. Only the test
 * async_idle_pool.reused_hop_setup_failure_releases_engine_reference below
 * uses this. */
extern size_t _chttpclient_async_idle_total_count_for_tests(
    struct chttpclient *cli);
/* Reads how many distinct origin keys cli->idle_pools of Tier 1 holds an
 * entry for. See
 * max_idle_origins.tier1_distinct_origins_bounded_and_reclaimed below. */
extern size_t _chttpclient_idle_pools_key_count_for_tests(
    struct chttpclient *cli);
/* The async counterpart of the accessor above, for Tier 2 and Tier 3, which
 * reads cli->idle_pools_async. */
extern size_t _chttpclient_idle_pools_async_key_count_for_tests(
    struct chttpclient *cli);
/* Replaces the working value of CHTTP_MAX_IDLE_ORIGINS, which the idle pool
 * of Tier 1 and the idle pool of Tier 2 and Tier 3 share, for the whole
 * process. A value of 0 puts the real compile-time constant back. A test
 * needs this because the real cap of 128 asks for 128 distinct
 * origins. */
extern void _chttpclient_set_max_idle_origins_for_tests(size_t n);
/* Resolves a chttpcli handle to the struct chttpclient* behind it, but does
 * NOT pin that pointer against a concurrent destroy. This is safe here,
 * because every call site below runs synchronously and no destroy races it.
 * The two white-box accessors above need it because they take the raw
 * pointer, and a chttpcli handle means nothing outside the slot table of
 * the library. */
extern struct chttpclient *_chttpcli_resolve_for_tests(chttpcli h);
/* Reads how many slots the chttpcli handle table holds. Only the test
 * chttpcli_handle_reuse.bounded_slot_reuse_under_churn below uses this. */
extern size_t _chttpcli_slot_table_capacity_for_tests(void);
extern size_t _chttpcli_free_index_count_for_tests(void);
/* Makes the next cvector_push_back call of _async_idle_pool_offer look like
 * a failure, because a real out-of-memory condition cannot reach that call
 * site (see async_idle_pool.offer_push_failure_no_double_free below for
 * why). */
extern void _chttpclient_force_offer_push_fail_once_for_tests(void);
/* Makes the next ccol_event_loop_modify call of _async_submit_hop for a
 * reused connection look like a failure, because a real failure cannot
 * reach that call site under ordinary conditions (see
 * async_idle_pool.reactivate_failure_retries_without_uaf below for why). */
extern void _chttpclient_force_reactivate_fail_once_for_tests(void);
/* Makes the next read dispatch of _async_on_readable_impl look like a hard
 * transport or TLS error, but only for a ctx that has already had at least
 * one real, successful read. The result is the same as a recv() or a
 * ctls_conn_read() that returned -1 with ECONNRESET, while the real socket
 * stays untouched. Only the test
 * async_step_a.hard_read_error_during_eof_delimited_body below uses this;
 * its own comment says why a real TCP RST does not land reliably over a
 * real socket. */
extern void _chttpclient_force_async_hard_read_error_once_for_tests(void);
/* The first two functions arm the next plain-HTTP hop that finishes the
 * write of its request, so that this hop at once turns its own
 * registration back to the write direction. The third function reports how
 * many times the write-direction dispatch path had to restore the read
 * direction of a registration. Only the test
 * async_stray_write_arm.a_write_arm_with_nothing_to_write_self_heals below
 * uses these; its own comment says why the race that they stand for does
 * not land reliably over a real socket. */
extern void _chttpclient_force_stray_write_arm_once_for_tests(void);
extern void _chttpclient_force_stray_idle_write_arm_once_for_tests(void);
extern unsigned long long _chttpclient_stray_write_arm_restores_for_tests(void);

TEST(async_engine, starts_on_first_acquire_and_stops_at_zero_refcount) {
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);

  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests());
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 1);

  _chttpclient_engine_release_for_tests();
  /* The release lowers the ref count at once, inside the call, but the
   * "running" state of the reactor changes only after its real teardown
   * finishes on a separate reaper thread, so the test waits for that
   * instead of assuming that the state changes at once. */
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  _chttpclient_engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
}

TEST(async_engine, refcount_tracks_multiple_acquirers) {
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 3);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests());

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 2);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests()); /* 2 users left */

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 1);
  REQUIRE_TRUE(_chttpclient_engine_running_for_tests()); /* 1 user left */

  _chttpclient_engine_release_for_tests();
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  _chttpclient_engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
}

TEST(async_engine, restart_after_full_stop_works) {
  /* Proves that a caller can stop the engine and start it again more than
   * once, rather than it starting only once for the life of the process. */
  for (int i = 0; i < 3; i++) {
    REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
    REQUIRE_TRUE(_chttpclient_engine_running_for_tests());
    _chttpclient_engine_release_for_tests();
    _chttpclient_engine_wait_for_quiescence_for_tests();
    REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
  }
}

typedef struct {
  atomic_int *acquired_ok;
} engine_thread_arg_t;

static void *engine_acquire_release_thread(void *arg) {
  engine_thread_arg_t *a = (engine_thread_arg_t *)arg;
  if (_chttpclient_engine_acquire_for_tests() == ccol_success) {
    atomic_fetch_add(a->acquired_ok, 1);
  }
  /* Holds the reference for a short time, so that acquires from other
   * threads overlap it, and then releases it. */
  usleep(1000);
  _chttpclient_engine_release_for_tests();
  return NULL;
}

TEST(async_engine, concurrent_acquire_release_no_corruption) {
  enum { N = 16 };
  pthread_t threads[N];
  atomic_int acquired_ok = 0;
  engine_thread_arg_t arg = {.acquired_ok = &acquired_ok};

  int started = 0;
  for (int i = 0; i < N; i++) {
    /* This loop counts the started threads instead of asserting here (see the
     * same comment in http.concurrent_requests): threads[] and arg live on
     * the stack, so a REQUIRE_* inside this loop returns while earlier
     * threads still run against them. */
    if (pthread_create(&threads[i], NULL, engine_acquire_release_thread,
                       &arg) != 0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) {
    pthread_join(threads[i], NULL);
  }

  REQUIRE_EQ(started, N);
  REQUIRE_EQ(atomic_load(&acquired_ok), N);
  /* Exactly one release matches each acquire. */
  REQUIRE_EQ(_chttpclient_engine_ref_count_for_tests(), 0);
  _chttpclient_engine_wait_for_quiescence_for_tests();
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());
}

TEST(async_engine, num_reactor_threads_defaults_to_cpu_count) {
  /* When nothing configures the thread count here, or it passes 0 (the
   * sentinel that puts the default back), the reactor must size itself from
   * sysconf(_SC_NPROCESSORS_ONLN), falling back to 1 when that query fails.
   * The default of the sibling function
   * chttpsvr_set_engine_num_reactor_threads works the same way. */
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);

  long cpus = sysconf(_SC_NPROCESSORS_ONLN);
  size_t expected = (cpus > 0) ? (size_t)cpus : 1;
  REQUIRE_EQ(_chttpclient_engine_num_reactor_threads_for_tests(), expected);

  _chttpclient_engine_release_for_tests();
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

TEST(async_engine, num_reactor_threads_explicit_value_is_wired_in) {
  /* A value above zero must reach ccol_event_loop_create_with_mprocs
   * exactly, instead of being accepted and then ignored. */
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(3), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(_chttpclient_engine_num_reactor_threads_for_tests(), (size_t)3);
  _chttpclient_engine_release_for_tests();
  _chttpclient_engine_wait_for_quiescence_for_tests();

  /* Puts the default back for every test after this one. */
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_success);
}

TEST(async_engine, num_reactor_threads_rejected_while_running) {
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(2), ccol_not_permitted);
  /* A value of 0 puts the default back, and the "not while it runs" rule
   * covers it too, but it still changes the thread count for the next
   * reactor that the library creates. */
  REQUIRE_EQ(chttpcli_set_engine_num_reactor_threads(0), ccol_not_permitted);
  _chttpclient_engine_release_for_tests();
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; STEP A (WHITE-BOX, HTTP ONLY)          */
/* ========================================================================== */

/*
 * Functional tests for the async engine of Tier 2, chttpclient_do_async,
 * over plain HTTP with no TLS. They follow no redirect and reuse nothing
 * from the idle pool, so every request opens a fresh connection and then
 * closes it. They exercise the real, shared ccol_event_loop reactor from
 * end to end, against the same mock test server that the synchronous tests
 * of Tier 1 use.
 */

/*
 * Waits for the async engine to become fully idle after a request
 * completes. _async_fulfill fulfills the future of the request BEFORE the
 * dispatch callback tears the connection down, and the real teardown (in
 * _async_ctx_teardown and _async_ctx_free), which is what calls
 * _client_engine_release(), runs later. A return from ctpool_future_get()
 * is therefore no proof that the release has even started.
 *
 * This function first polls until the ref count reaches 0, then waits for
 * the reaper that this drop starts, so that each test leaves the engine
 * fully torn down before it returns. See the extern declarations above for
 * why that matters.
 */
static void wait_for_async_engine_idle(void) {
  for (int i = 0; i < 2000 && _chttpclient_engine_ref_count_for_tests() > 0;
       i++) {
    usleep(1000);
  }
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

/*
 * Polls _chttpclient_engine_ref_count_for_tests() until it reaches
 * `expected` or a bounded number of iterations passes, and gives back the
 * last value that it saw, so that a real mismatch or leak still fails the
 * REQUIRE_EQ of the caller, against a settled value rather than one that
 * is still moving.
 *
 * The reason is the one that the comment on wait_for_async_engine_idle
 * above gives: _async_fulfill_chain fulfills the future of a request
 * BEFORE the dispatch callback frees the engine reference of the chain
 * (see the success path of _async_on_readable_impl). A caller that wakes
 * up through ctpool_future_get() or chttpclient_async_result_get() and
 * then reads the ref count at once, or after a sleep of a fixed length,
 * can therefore briefly see a count that is too high, which has nothing to
 * do with a real leak.
 *
 * Every assertion that compares this count against an expected value
 * directly after a future resolves must poll for it, rather than reading it
 * once or waiting a fixed time. A single read fails from time to time
 * under the heavier scheduling of valgrind, and almost never in a plain
 * run, and each such failure skips the trailing chttpclient_destroy and
 * wait_for_async_engine_idle of that test, because REQUIRE_EQ returns at
 * once on a failure, so the client under test then really leaks.
 */
static int poll_engine_ref_count(int expected) {
  int refs = -1;
  for (int i = 0; i < 2000; i++) {
    refs = _chttpclient_engine_ref_count_for_tests();
    if (refs == expected) break;
    usleep(1000);
  }
  return refs;
}

TEST(async_engine, destroy_waits_for_in_flight_async_request) {
  /* A regression test. __chttpclient_destroy must wait for an ACTIVE
   * request of Tier 2 or Tier 3, one that is in flight and that the pool
   * does not hold yet; a wait on the in_flight_count of Tier 1 and on the
   * count of pooled idle connections of Tier 2 and Tier 3 is not enough.
   * Take the pair
   * `f = chttpclient_do_async(cli, req); chttpclient_destroy(cli);`, with
   * no wait on f between the two calls: without this wait, it frees cli
   * under a request that still connects, writes or reads on a reactor
   * thread, while the code dereferences
   * chain->cli and ctx->cli throughout that life cycle.
   *
   * /slow sleeps 100ms on the server before it answers, so the request is
   * still in flight when the code below calls chttpclient_destroy: the
   * client has sent the headers and waits for the response. A real
   * use-after-free here also shows up under valgrind, whatever the
   * assertion on the elapsed time says. */
  char url[160];
  make_url(url, sizeof(url), "/slow");

  chttpcli_construct(cli);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  REQUIRE_NE((void *)f, NULL);

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpclient_destroy(cli); /* blocks until the /slow request finishes */
  clock_gettime(CLOCK_MONOTONIC, &t1);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  /* This bound is far below the 100ms sleep of /slow, so a destroy that
   * returns almost at once, because it did not wait, fails here, even with
   * the scheduling jitter of a busy CI machine. */
  REQUIRE_GT(elapsed_ms, 50);

  /* This is a stronger check than "the caller can get it in the end": when
   * the destroy returns, the teardown of the in-flight request is already
   * complete, which is what the wait under test promises, so the future is
   * already done, not about to become done. */
  REQUIRE_TRUE(ctpool_future_done(f));

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw) {
    if (raw->resp) chttpclient_resp_free(raw->resp);
    chttpclient_async_result_free(raw);
  }
  ctpool_future_free(f);
  wait_for_async_engine_idle();
}

TEST(async_step_a, get_200) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, eof_delimited_body_without_content_length) {
  /* The async counterpart of
   * http.eof_delimited_body_without_content_length, which checks that a
   * valid completion at an EOF gives ccol_success on this tier too, where
   * the async dispatch path handles that completion with its own separate
   * code. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "eof-delimited-body-ok");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, hard_read_error_during_eof_delimited_body_reports_error) {
  /* _async_on_readable_impl must tell a read failure that is not
   * EWOULDBLOCK or EAGAIN apart from a clean EOF with n == 0. Without that,
   * a real transport or TLS error in the middle of a transfer (a TCP RST, a
   * fatal TLS alert or something similar) goes straight into
   * chttp1_parser_finish(), and because the connection here frames its body
   * by the EOF, with no Content-Length and no chunked Transfer-Encoding
   * (see eof-delimited-body-ok above), the client reports a failed,
   * truncated transfer as a complete response with ccol_success. Tier 1
   * makes this distinction in _chttp_read_message, and this test exercises
   * the same distinction on the async engine.
   *
   * The test uses the /eof-delimited-body route unchanged. The graceful
   * close of that route normally drives the SECOND read dispatch for this
   * connection, the one that completes the response (see the sibling test
   * eof_delimited_body_without_content_length above), and a test-only hook
   * makes that second dispatch look like a hard transport error without
   * ever touching the real socket. The exact timing of a real TCP RST
   * against data that has already arrived is a race at the OS level that
   * no test can pin down over a real socket, which is why the mock server
   * does not reset the connection here. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/eof-delimited-body");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  _chttpclient_force_async_hard_read_error_once_for_tests();

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, post_echoes_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"n\":42}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, expect_continue_field_reaches_the_wire) {
  /* /count-header knows nothing about Expect: 100-continue, so this hop
   * takes the timeout-then-send-anyway path; the async_expect_continue
   * group below covers the other outcomes of this tier. This test has one
   * narrow purpose: it checks, through the x-count-name mechanism of
   * /count-header, that the header itself reaches the wire, not how an
   * interim response is handled. The test is slow, at about 1s, and that
   * timeout wait is the reason it uses /count-header instead of a route
   * that knows about Expect.
   */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/count-header");

  const char *payload = "async-body";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_STREQ(resp->body, "1");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, large_body_response) {
  /* /large gives back 8192 bytes, which exercises more than one call to
   * on_data against one response, instead of a single read. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_chunked_response, decoded_correctly) {
  /* The async-tier counterpart of chunked_response.decoded_correctly. The
   * async engine assembles more than one chunk through its own
   * _async_on_readable loop, which is code of its own, separate from
   * _chttp_read_message of Tier 1. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/chunked-body");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, strlen("Hello, chunked world!"));
  REQUIRE_STREQ(resp->body, "Hello, chunked world!");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                MAX RESPONSE BODY SIZE (TIER 2/3)                          */
/*                                                                            */
/* The group of Tier 1 above covers the up-front check and the reactive      */
/* check in full. Both live in the shared _on_headers_complete and           */
/* _sink_buffered code that both tiers call through. The tests here          */
/* confirm the separate wiring of Tier 2 and Tier 3. That wiring takes a     */
/* snapshot of the cap into chttp_async_chain_t. _async_chain_create,        */
/* _async_submit_hop and _async_retry_hop then copy it into the ctx->bb of   */
/* each hop. These tests confirm that the copy reaches the check.            */
/* ========================================================================== */

TEST(async_max_response_body_size, declared_content_length_rejected_up_front) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_max_response_body_size, response_exactly_at_cap_succeeds) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 8192), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_max_response_body_size, redirect_hop_body_exempt_from_the_cap) {
  /* The counterpart for Tier 2 and Tier 3 of
   * max_response_body_size.redirect_hop_body_exempt_from_the_cap. The
   * shared _on_headers_complete, which both tiers call through, holds the
   * exemption, but Tier 2 and Tier 3 have their own separate wiring:
   * _async_submit_hop and _async_retry_hop copy
   * chain->max_response_body_size into the ctx->bb of each hop, and that
   * wiring can differ from the wiring of Tier 1. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 20), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_max_response_body_size,
     interim_1xx_oversized_content_length_exempt_from_the_cap) {
  /* The counterpart for Tier 2 and Tier 3 of the max_response_body_size
   * suite's interim_1xx_oversized_content_length_exempt_from_the_cap.
   * Both tiers share the same _on_headers_complete callback, so the same
   * failure mode reaches here in the same way: a discarded 1xx interim
   * response declares an oversized Content-Length of its own, and the
   * whole request fails with ccol_msg_too_large. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/early-hints-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_max_response_body_size,
     head_response_oversized_content_length_exempt) {
  /* The counterpart for Tier 2 and Tier 3 of
   * max_response_body_size.head_response_oversized_content_length_exempt.
   * Both tiers share the same _on_headers_complete callback, so the same
   * failure mode reaches here in the same way: the Content-Length of a
   * HEAD response describes what a GET gives back (RFC 7231 SS4.3.2), and
   * no body byte ever follows it, yet the whole request fails with
   * ccol_msg_too_large. */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/head-oversized-content-length");
  chttp_request_t *req = chttp_request_new(CHTTP_HEAD, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_max_response_body_size, async_streaming_is_unaffected_by_the_cap) {
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 5), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ctpool_future *f =
      chttpclient_do_async_streaming(cli, req, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);

  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(pooled, response_exceeding_cap_reports_msg_too_large) {
  /* Tier 3 (chttpclient_do_pooled) is a thin wrapper over Tier 2, and this
   * test confirms that the specific error code of the cap survives that
   * unwrap. chttpclient_do_pooled documents its own contract: it gives the
   * "same specific codes as chttpclient_do". */
  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_max_response_body_size(cli, 100), ccol_success);

  char url[160];
  make_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  chttp_request_free(req);
  REQUIRE_EQ(rv, ccol_msg_too_large);
  REQUIRE_EQ((void *)resp, NULL);

  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, status_code_forwarded) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/status/404");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 404);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, connection_refused_reports_error) {
  chttpcli_construct(cli);
  /* Nothing listens on this port: it is on the 127.0.0.1 loopback address,
   * and port 1 is a reserved privileged port that a test environment
   * almost never binds. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, https_connection_refused_reports_error) {
  /* The client really tries an https:// request and gives back a future,
   * not NULL, which exercises the is_https flag on its way through the
   * creation of the ctx and into the connect stage. Since nothing listens
   * on this port, the failure appears before any TLS handshake starts, so
   * this test stays fast. The end-to-end coverage of a handshake that
   * succeeds is in tests/chttpserver/tests_tls.c, which builds a real
   * throwaway certificate and key pair and drives a real HTTPS request
   * through chttpclient (see the comment at the top of that file). */
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, https_handshake_fails_against_plain_http_server) {
  /* This test connects with https:// to the plain-HTTP mock server of this
   * suite, which never speaks TLS. This is a real exercise of the
   * handshake-failure path of ctls_conn_handshake_step that needs no live
   * fixture that can speak TLS.
   *
   * The test is slow, at about 5s: the srv_read_headers of the mock server
   * has a fixed 5-second SO_RCVTIMEO, and a raw TLS ClientHello never holds
   * the "\r\n\r\n" that it waits for, so the server stays silent until its
   * own timeout closes the connection. This client sets no request timeout
   * (the default is none; see chttpclient_set_request_timeout), so nothing
   * on the client side cuts the wait short. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  /* make_url builds an http:// URL, and this code adds an "s" to it:
   * "url + 4" steps past the literal "http", which leaves
   * "://127.0.0.1:<port>/get" to append. */
  char https_url[168];
  snprintf(https_url, sizeof(https_url), "https%s", url + 4);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, https_url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_TRUE(rv == ccol_http_tls_handshake_failed ||
               rv == ccol_http_tls_cert_verification_failed ||
               rv == ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_step_a, null_args_returns_null) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpclient_do_async(CHTTPCLI_INVALID, req), NULL);
  REQUIRE_EQ((void *)chttpclient_do_async(cli, NULL), NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

typedef struct {
  chttpcli cli;
  char url[160];
  int expected_status;
  bool ok;
} async_concurrent_arg_t;

static void *async_concurrent_thread(void *arg) {
  async_concurrent_arg_t *a = (async_concurrent_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;

  ctpool_future *f = chttpclient_do_async(a->cli, req);
  chttp_request_free(req);
  if (!f) return NULL;

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == a->expected_status;
    chttpclient_resp_free(resp);
  }
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  return NULL;
}

TEST(async_step_a, concurrent_requests_all_succeed) {
  /* This test fires several async requests at the same time against one
   * shared engine, to prove that the reactor really multiplexes more than
   * one live connection instead of handling only one at a time. */
  enum { N = 12 };
  chttpcli_construct(cli);
  pthread_t threads[N];
  async_concurrent_arg_t args[N];

  int started = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    make_url(args[i].url, sizeof(args[i].url), "/get");
    args[i].expected_status = 200;
    args[i].ok = false;
    /* This loop counts the threads instead of asserting here; see the identical
     * comment in http.concurrent_requests for why a REQUIRE_* inside this
     * loop is itself the stack-use-after-return that the join below exists
     * to prevent. */
    if (pthread_create(&threads[i], NULL, async_concurrent_thread, &args[i]) !=
        0)
      break;
    started++;
  }
  for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(started, N);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     INTERIM 1xx RESPONSE TESTS (TIER 2)                    */
/* ========================================================================== */

TEST(async_early_hints, discarded_and_real_response_delivered) {
  /* The async-tier counterpart of
   * early_hints.discarded_and_real_response_delivered. /early-hints sends
   * "103 Early Hints" as a SEPARATE message before the real "200 OK"
   * response, so the FIRST message that _async_on_readable_impl parses off
   * the wire is the 103, which it must not always read as the final
   * response. Without that rule, the future resolves with a status_code of
   * 103 and an empty body, while the real "200 OK" stays unread on the
   * wire. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  /* /early-hints is eligible for keep-alive, so the connection of this
   * request still sits in the async idle pool of cli at this point, holding
   * an engine reference of its own (see _async_idle_pool_offer).
   * chttpclient_destroy must run FIRST to drain it, waiting on
   * idle_async_drained inside itself; a call to
   * wait_for_async_engine_idle() before that only polls for its full
   * 2-second budget, because the ref count of the engine cannot reach 0
   * while the client of this test still holds a pooled connection open. */
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints,
     discarded_when_arriving_in_the_same_read_as_the_final_response) {
  /* The async-tier counterpart of
   * early_hints.discarded_when_arriving_in_the_same_read_as_the_final_response.
   * /early-hints-same-write puts both messages into ONE send() call, which
   * exercises the data and data_len re-parse loop of
   * _async_on_readable_impl: a fresh ctx->parser gets the trailing bytes
   * that are left over after the discard of the 103, inside the SAME
   * on_readable dispatch. The test above only covers the case of two
   * separate reads. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints-same-write");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  /* /early-hints is eligible for keep-alive, so the connection of this
   * request still sits in the async idle pool of cli at this point, holding
   * an engine reference of its own (see _async_idle_pool_offer).
   * chttpclient_destroy must run FIRST to drain it, waiting on
   * idle_async_drained inside itself; a call to
   * wait_for_async_engine_idle() before that only polls for its full
   * 2-second budget, because the ref count of the engine cannot reach 0
   * while the client of this test still holds a pooled connection open. */
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints,
     connection_stays_reusable_after_discarding_interim_response) {
  /* The async-tier counterpart of
   * early_hints.connection_stays_reusable_after_discarding_interim_response.
   * Without this, the client wrongly offers the connection to the idle
   * pool of Tier 2 as done, while the real response is still unread on the
   * wire and corrupts whatever later request reuses that connection. This
   * test runs two /early-hints requests one after the other on the same
   * client, so the second very probably reuses the pooled connection of
   * the first, and each request must get its OWN correct response. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/early-hints");

  for (int i = 0; i < 2; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);

    ctpool_future *f = chttpclient_do_async(cli, req);
    REQUIRE_NE((void *)f, NULL);
    chttp_request_free(req);

    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);

    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

    chttpclient_resp_free(resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(f);
  }

  /* See the same comment in the two async_early_hints tests above: the
   * connection of the second request is still pooled at this point, so
   * chttpclient_destroy must run before wait_for_async_engine_idle, not
   * after it. */
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/* ========================================================================== */
/*             EXPECT: 100-CONTINUE TESTS (TIER 2/3), ASYNC ENGINE            */
/* ========================================================================== */

/*
 * The async-tier counterparts of the expect_continue suite of Tier 1
 * above. They exercise CHTTP_ASYNC_AWAITING_CONTINUE,
 * _async_awaiting_continue_on_data and the continue_deadline branch of the
 * deadline sweep, against the same mock server routes. Those routes are
 * reused directly instead of being duplicated, because each one speaks raw
 * HTTP over the socket, so which client tier connects makes no difference
 * to them.
 */

TEST(async_expect_continue, interim_100_then_body_sent) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-echo");

  const char *payload = "hold-until-continue-async";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue, server_rejects_without_100) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-reject");

  const char *payload = "should-never-be-sent-async";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 417);
  REQUIRE_STREQ(resp->body, "expectation failed");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue,
     direct_rejection_without_100_never_pools_connection) {
  /* The async-tier counterpart of the test of Tier 1 with the same purpose.
   * The handling of ctx->retry_unsafe and keep_alive does more than avoid a
   * crash: it forces a fresh connection for the next, unrelated request,
   * because the server can still read the pooled connection as one in the
   * middle of a request. */
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/expect-continue-reject-keepalive");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  const char *payload = "should-never-be-sent-async";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req1 = chttp_request_new(CHTTP_POST, url1, &body, NULL);
  REQUIRE_NE((void *)req1, NULL);
  req1->expect_continue = true;
  ctpool_future *f1 = chttpclient_do_async(cli, req1);
  REQUIRE_NE((void *)f1, NULL);
  chttp_request_free(req1);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  REQUIRE_NE((void *)raw1->resp, NULL);
  REQUIRE_EQ(raw1->resp->status_code, 417);
  chttpclient_resp_free(raw1->resp);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);

  chttp_request_t *req2 = chttp_request_new(CHTTP_GET, url2, NULL, NULL);
  REQUIRE_NE((void *)req2, NULL);
  ctpool_future *f2 = chttpclient_do_async(cli, req2);
  REQUIRE_NE((void *)f2, NULL);
  chttp_request_free(req2);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  REQUIRE_NE((void *)raw2->resp, NULL);
  REQUIRE_EQ(raw2->resp->status_code, 200);
  chttpclient_resp_free(raw2->resp);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  wait_for_async_engine_idle();
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
}

TEST(async_expect_continue, dead_connection_after_100_clean_eof_not_retried) {
  /* The async-tier counterpart of the test of Tier 1 with the same purpose.
   * Once the client gets a real "100 Continue" and sends the body,
   * ctx->retry_unsafe must stop the usual retry-once safety net of
   * _async_ctx_finish for a reused connection. THIS failure is a clean EOF
   * on the final read, which looks the same as an ordinary dead pooled
   * connection. */
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive");
  make_url(url2, sizeof(url2), "/expect-continue-die-after-100-clean");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();
  atomic_store(&g_die_after_100_clean_body_recv_count, 0);

  chttp_request_t *req1 = chttp_request_new(CHTTP_GET, url1, NULL, NULL);
  REQUIRE_NE((void *)req1, NULL);
  ctpool_future *f1 = chttpclient_do_async(cli, req1);
  REQUIRE_NE((void *)f1, NULL);
  chttp_request_free(req1);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  REQUIRE_NE((void *)raw1->resp, NULL);
  REQUIRE_EQ(raw1->resp->status_code, 200);
  chttpclient_resp_free(raw1->resp);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);

  /* This gives the connection of the first hop time to land in the idle pool
   * before the code submits the second request, so that the second request
   * really reuses it. Tier 1 gets the same guarantee from its own blocking
   * calls, one after the other, while this async engine has no such
   * synchronous ordering. */
  usleep(20000);

  const char *payload = "must-not-be-sent-twice-async";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req2 = chttp_request_new(CHTTP_POST, url2, &body, NULL);
  REQUIRE_NE((void *)req2, NULL);
  req2->expect_continue = true;
  ctpool_future *f2 = chttpclient_do_async(cli, req2);
  REQUIRE_NE((void *)f2, NULL);
  chttp_request_free(req2);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_NE((int)raw2->rv, (int)ccol_success);
  REQUIRE_EQ((void *)raw2->resp, NULL);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  wait_for_async_engine_idle();
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);
  REQUIRE_EQ(atomic_load(&g_die_after_100_clean_body_recv_count), 1);

  chttpclient_destroy(cli);
}

TEST(async_expect_continue, wait_times_out_body_sent_anyway) {
  /* /post knows nothing about Expect: 100-continue, so this test exercises
   * the continue_deadline branch of the deadline sweep, which calls
   * ccol_event_loop_modify to the write direction and dispatches to the
   * CHTTP_ASYNC_AWAITING_CONTINUE branch of _async_on_writable_impl. No
   * real "100 Continue" ever arrives. The test is slow, at about 1s, and
   * that is the point of it. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"sent\":\"after-timeout-async\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue, explicit_expect_header_suppresses_the_wait) {
  /* /post answers at once after it reads the body, so a request that behaves
   * correctly finishes almost at once, while the test
   * wait_times_out_body_sent_anyway above takes about 1s. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"x\":1}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;
  chttp_request_set_header(req, "Expect", "204-content");

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  /* This bound is wide, and does not match the tighter 500ms of Tier 1 (see
   * the comment of that sibling test). The async engine adds thread hops of
   * its own, from the submitter to the DNS pool and the connect pool, then
   * to the reactor, then to a dispatch worker, and under valgrind each hop
   * pays its own share of the instrumentation cost for each instruction.
   * That can push this well past 500ms, to 838ms to 887ms, and steady load
   * from other test suites that run at the same time (a full `make memtest`
   * sweep from the root of the repository, for example) pushes it further
   * still. The 3000ms bound holds both, and is still an explicit ceiling on
   * a path that must never sit out a real CHTTP_100_CONTINUE_WAIT_US wait of
   * about 1000ms. */
  REQUIRE_LT(elapsed_ms, 3000L);

  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue, early_hints_before_100_continue_still_waits) {
  /* /expect-continue-with-hints sends "103 Early Hints" BEFORE "100
   * Continue". This exercises the branch of
   * _async_awaiting_continue_on_data that discards an interim response
   * other than a 100; the branch of CHTTP_ASYNC_READING in
   * _async_process_reading_data looks the same, but it is a separate
   * branch. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-with-hints");

  const char *payload = "hello-async";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, payload);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue,
     hints_both_sides_of_100_continue_each_phase_gets_its_own_cap) {
  /* The async counterpart of the test of expect_continue with the same name.
   * This failure is really reachable; it is not a defensive check.
   *
   * Two loops discard interim responses: the first, inside
   * _async_awaiting_continue_on_data, runs while the client waits for "100
   * Continue", and the second, inside _async_process_reading_data, looks
   * the same and runs while the client reads the final response.
   * chttp_async_ctx_t.interim_responses_seen must NOT be one counter that
   * both loops share; the code must reset it at the hand-off between the
   * two, which is either a real "100 Continue" or a continue-wait that
   * times out.
   *
   * One shared counter with no reset leaves too small a budget: a hop that
   * discarded 40 interim responses before "100 Continue" then keeps only 24
   * for its own final-response read, instead of a fresh 64. That silently
   * breaks the documented cap of "64 CONSECUTIVE discarded interim
   * responses", because a confirmed "100 Continue" is itself a message that
   * nobody discards, so it breaks the run. THIS scenario then fails with
   * ccol_http_transfer_aborted, well before the real and separate cap of 65
   * in a row applies. See
   * async_early_hints.discards_exactly_64_before_giving_up.
   *
   * This test is not vacuous. ctx->interim_responses_seen has two reset
   * points, one in the confirmed-100 branch of
   * _async_awaiting_continue_on_data and the other in the continue-timeout
   * branch of _async_on_writable_impl; remove either one and this test
   * fails with raw->rv != ccol_success. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-hints-both-sides/40/40");

  const char *payload = "hello-async";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_expect_continue,
     interim_100_bundled_with_final_response_in_same_read) {
  /* /expect-continue-bundled-final writes "100 Continue" and a complete,
   * valid final response in the SAME send() call, then drains and discards
   * whatever body the client sends. This exercises ctx->continue_carry:
   * the trailing bytes of the final response arrive before the body write
   * starts, and the engine must store them and replay them through
   * _async_process_reading_data the moment that write finishes. See the
   * 100-branch of _async_awaiting_continue_on_data, and also the shared
   * completion-path check of _async_plain_try_write and
   * _tls_try_write.
   */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/expect-continue-bundled-final");

  const char *payload = "hold-until-continue-bundled";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "bundled-response");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* Sets the delay on each side of the carry-over replay of Expect:
 * 100-continue, so that a chttpclient_destroy that runs at the same time
 * has room to run to completion inside that window. The test
 * async_expect_continue.carry_replay_survives_a_racing_destroy below is the
 * one place that uses this. */
extern void _chttpclient_set_continue_carry_replay_delay_ms_for_tests(long ms);

/* A counting allocator whose blocks are the ones that the client under
 * test allocates and frees.
 *
 * Every object that the caller builds on its own memory management
 * procedures keeps a copy of them, and such a copy is handed back through
 * itself, so an allocation through a copy after that hand-back uses memory
 * that its owner gave up. No assertion can see a read of a function
 * pointer out of a plain freed block, because the bytes usually survive
 * long enough for the call to land on the real function and behave
 * correctly. This allocator therefore poisons every copy that comes back
 * and holds it instead of freeing it, which turns such a use into a call
 * into carry_mp_poisoned_free below. The test frees the held blocks
 * itself. */
#define CARRY_MP_MAX_HELD_COPIES 4096
static atomic_int g_carry_mp_live_blocks = 0;
static void *g_carry_mp_held[CARRY_MP_MAX_HELD_COPIES];
static size_t g_carry_mp_held_count = 0;
static atomic_int g_carry_mp_held_overflow = 0;
static ccol_memmgmt_procs_t g_carry_mp;
static pthread_mutex_t g_carry_mp_lock = PTHREAD_MUTEX_INITIALIZER;

static void carry_mp_poisoned_free(void *p) {
  (void)p;
  fprintf(stderr,
          "chttpclient: a copy of the caller's memory management procedures "
          "was used after its owner had already handed it back; that copy is "
          "gone by then and this is a use-after-free\n");
  fflush(stderr);
  abort();
}
static void *carry_mp_poisoned_malloc(size_t sz) {
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}
static void *carry_mp_poisoned_calloc(size_t n, size_t sz) {
  (void)n;
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}
static void *carry_mp_poisoned_realloc(void *p, size_t sz) {
  (void)p;
  (void)sz;
  carry_mp_poisoned_free(NULL);
  return NULL;
}

static void *carry_mp_malloc(size_t sz) {
  void *p = malloc(sz);
  if (p) atomic_fetch_add(&g_carry_mp_live_blocks, 1);
  return p;
}
static void *carry_mp_calloc(size_t n, size_t sz) {
  void *p = calloc(n, sz);
  if (p) atomic_fetch_add(&g_carry_mp_live_blocks, 1);
  return p;
}
static void *carry_mp_realloc(void *p, size_t sz) {
  void *q = realloc(p, sz);
  if (q && !p) atomic_fetch_add(&g_carry_mp_live_blocks, 1);
  return q;
}
static void carry_mp_free(void *p) {
  if (!p) return;
  atomic_fetch_sub(&g_carry_mp_live_blocks, 1);
  /* A copy of these procedures is a byte-for-byte duplicate of g_carry_mp,
   * and no other block that this allocator hands out carries that content.
   * This code checks the size first, because most blocks here are smaller
   * than that, and a read past one is a heap-buffer-overflow, which
   * AddressSanitizer reports. */
  if (malloc_usable_size(p) >= sizeof(g_carry_mp) &&
      memcmp(p, &g_carry_mp, sizeof(g_carry_mp)) == 0) {
    ccol_memmgmt_procs_t *copy = (ccol_memmgmt_procs_t *)p;
    copy->malloc = carry_mp_poisoned_malloc;
    copy->free = carry_mp_poisoned_free;
    copy->calloc = carry_mp_poisoned_calloc;
    copy->realloc = carry_mp_poisoned_realloc;
    pthread_mutex_lock(&g_carry_mp_lock);
    if (g_carry_mp_held_count < CARRY_MP_MAX_HELD_COPIES) {
      g_carry_mp_held[g_carry_mp_held_count++] = p;
      pthread_mutex_unlock(&g_carry_mp_lock);
      return;
    }
    pthread_mutex_unlock(&g_carry_mp_lock);
    atomic_store(&g_carry_mp_held_overflow, 1);
  }
  free(p);
}
static ccol_memmgmt_procs_t g_carry_mp = {.malloc = carry_mp_malloc,
                                          .free = carry_mp_free,
                                          .calloc = carry_mp_calloc,
                                          .realloc = carry_mp_realloc};

/* Frees every poisoned copy that the code above holds. The test calls this
 * after it has finished with the client, so that nothing can still reach
 * one. */
static size_t carry_mp_release_held(void) {
  pthread_mutex_lock(&g_carry_mp_lock);
  size_t n = g_carry_mp_held_count;
  for (size_t i = 0; i < n; i++) free(g_carry_mp_held[i]);
  g_carry_mp_held_count = 0;
  pthread_mutex_unlock(&g_carry_mp_lock);
  return n;
}

static void *carry_destroy_thread_fn(void *arg) {
  chttpclient_destroy(*(chttpcli *)arg);
  return NULL;
}

TEST(async_expect_continue, carry_replay_survives_a_racing_destroy) {
  /*
   * The carry-over replay can fulfil its chain and drop the last reference
   * of that chain, which is the signal that a blocked chttpclient_destroy
   * waits for before it frees the client. When a caller builds a client on
   * its own memory management procedures, the destroy also frees the copy
   * of those procedures that the client holds, and the replay frees the
   * bytes that it replays through exactly that copy, so the replay must
   * hold the chain until after that free.
   *
   * Scheduling alone decides how much of a destroy fits between the two,
   * so the delay below makes the whole of it fit every time: the destroy
   * thread reaches its wait during the leading half, and then runs to
   * completion during the trailing half if nothing holds the chain.
   *
   * This test is not vacuous. Without the hold on the chain, the free that
   * follows the replay reads its function pointer out of freed memory and
   * calls through it, which crashes the binary here; AddressSanitizer and
   * valgrind both report a use-after-free for it.
   */
  atomic_store(&g_carry_mp_live_blocks, 0);
  char *err = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&g_carry_mp, &err);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  char url[160];
  make_url(url, sizeof(url), "/expect-continue-bundled-final");
  const char *payload = "hold-until-continue-bundled";
  chttp_request_body_t body = CHTTP_TEXT_BODY(payload, strlen(payload));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &body, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  _chttpclient_set_continue_carry_replay_delay_ms_for_tests(300);
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);

  pthread_t destroyer;
  bool destroyer_started = false;
  ccol_retval_t rv = ccol_unexpected_failure;
  int status = 0;
  if (f) {
    destroyer_started =
        (pthread_create(&destroyer, NULL, carry_destroy_thread_fn, &cli) == 0);
    /* A response holds a reference to the allocator of its client without
     * owning it, so the code frees the response here, while the destroy is
     * still blocked, rather than after the join. */
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    if (raw) {
      rv = raw->rv;
      if (raw->resp) {
        status = raw->resp->status_code;
        chttpclient_resp_free(raw->resp);
      }
      chttpclient_async_result_free(raw);
    }
    ctpool_future_free(f);
    if (destroyer_started) pthread_join(destroyer, NULL);
  }
  _chttpclient_set_continue_carry_replay_delay_ms_for_tests(0);
  wait_for_async_engine_idle();

  REQUIRE_NE((void *)f, NULL);
  REQUIRE_TRUE(destroyer_started);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  int live_blocks = atomic_load(&g_carry_mp_live_blocks);
  int held_overflow = atomic_load(&g_carry_mp_held_overflow);
  size_t held = carry_mp_release_held();

  /* The client gives back every block that it took from this allocator,
   * including its own copy of these procedures. */
  REQUIRE_EQ(live_blocks, 0);
  REQUIRE_EQ(held_overflow, 0);
  REQUIRE_GT(held, (size_t)0);
}

TEST(async_expect_continue, bodyless_request_not_affected) {
  /* There is no body at all here, so
   * "body_carrying_method && body.data && body.len > 0" is false and
   * want_100_continue must stay false: this hop must behave like an
   * ordinary send that happens at once, just like the Tier 1 case for a
   * request with no body. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/post");

  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  req->expect_continue = true;

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  /* This bound is wide, and does not match the tighter 500ms of Tier 1 (see
   * the comment of that sibling test). The async engine adds thread hops of
   * its own, from the submitter to the DNS pool and the connect pool, then
   * to the reactor, then to a dispatch worker, and under valgrind each hop
   * pays its own share of the instrumentation cost for each instruction.
   * That can push this well past 500ms, to 838ms to 887ms, and steady load
   * from other test suites that run at the same time (a full `make memtest`
   * sweep from the root of the repository, for example) pushes it further
   * still. The 3000ms bound holds both, and is still an explicit ceiling on
   * a path that must never sit out a real CHTTP_100_CONTINUE_WAIT_US wait of
   * about 1000ms. */
  REQUIRE_LT(elapsed_ms, 3000L);

  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; STEP B (REDIRECT FOLLOWING)            */
/* ========================================================================== */

/*
 * Functional tests for how the async engine follows a redirect, in
 * _chttp_do_async_internal, _async_handle_redirect and _async_submit_hop.
 * These tests match the assertions about the policy for the method and the
 * body in the synchronous redirect_policy suite of Tier 1, and add
 * coverage of the chain life cycle that only the async engine has: a chain
 * of more than one hop, the CHTTP_MAX_REDIRECTS cap and a relative
 * Location. Tier 1 has no counterpart above for those, because it shares
 * those code paths through _resolve_redirect_url and the shared
 * on_headers_complete callback of chttp1_parser.
 */

TEST(async_redirects, get_301_follows_to_final_resource) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, post_301_becomes_bodyless_get) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-echo");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, post_307_preserves_method_and_body) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-307-to-echo");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  /* The code frees the original request directly after the call returns,
   * before the redirect hop that needs the body again runs. This proves
   * that what the second hop really sends again is the deep copy of the
   * chain (chttp_async_chain_t.body_data), not a reference into req. */
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[32];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body));
  REQUIRE_STREQ(resp->body, expected);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* The counterparts for Tier 2 and Tier 3 of
 * redirect_policy.stale_*_stripped_after_downgrade above.
 * _async_submit_hop must strip the same stale headers whenever a hop
 * downgrades, because its own default is to send chain->req_headers again,
 * unchanged, on every hop. */
TEST(async_redirects, stale_content_length_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Length", "9"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-length"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, stale_content_type_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Content-Type", "application/json"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "content-type"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, stale_expect_stripped_after_downgrade) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-to-count-header");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Expect", "100-continue"),
             ccol_success);
  REQUIRE_EQ(chttp_request_set_header(req, "x-count-name", "expect"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* The counterpart for Tier 2 and Tier 3 of
 * redirect_policy.body_stays_dropped_across_a_later_preserving_hop above.
 * _async_handle_redirect writes chain->body_dropped, and a 307 or 308 hop
 * that follows a downgrade that keeps nothing must obey the drop that the
 * earlier hop on the same chain made, instead of reading
 * chain->body_data, chain->body_len and chain->body_content_type again,
 * which hold the ORIGINAL values of hop 0. See the comment on
 * chttp_async_chain_t.body_dropped in chttpclient.c. */
TEST(async_redirects, body_stays_dropped_across_a_later_preserving_hop) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-301-then-307-to-echo");

  const char *body = "some=data";
  chttp_request_body_t rbody = CHTTP_FORM_BODY(body, strlen(body));
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rbody, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "GET:0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* The counterparts for Tier 2 and Tier 3 of the
 * explicit_authorization_header_* tests above:
 * chain->explicit_auth_suppressed, which _async_submit_hop mutates. */
TEST(async_redirects, explicit_authorization_header_carried_same_origin) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-same-origin");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Bearer mytoken");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, explicit_authorization_header_dropped_cross_origin) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] explicit_authorization_header_dropped_cross_origin: no "
            "IPv6 loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-to-echo-auth-cross-origin");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(chttp_request_set_header(req, "Authorization", "Bearer mytoken"),
             ccol_success);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, relative_location_resolved_against_current_host) {
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-relative");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, plain_relative_location_merged_against_current_path) {
  /* chttp_async_ctx_t must carry a field that tracks the path_and_query of
   * the current hop, and the chttp_url_t base that _async_handle_redirect
   * builds by hand must never leave it NULL. A Location header that is a
   * real relative reference, such as "sibling" below (not a full URL, not a
   * "//host/..." protocol-relative reference, and not an absolute-path
   * "/..." reference), reaches the merge branch of _resolve_redirect_url,
   * and with the field NULL such a header stops the whole process with a
   * strchr() call on a NULL pointer inside _merge_ref_path. The sibling
   * test above uses /redirect-relative, which cannot catch this, because
   * "Location: /get" is an absolute-path reference that never reaches the
   * merge branch. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/nested/dir/redirect-relative-plain");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "sibling-ok");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, relative_location_resolved_against_ipv6_host) {
  /* The same chttp_url_t base, which the code builds by hand, depends on a
   * second field: chttp_async_ctx_t must also track whether its host is an
   * IPv6 literal, so that base.is_ipv6 describes the real connection
   * instead of defaulting to false. _resolve_redirect_url puts the brackets
   * back around the host from base->is_ipv6, on both the absolute-path
   * branch and the relative-path branch, so with that flag wrong ANY
   * redirect on a connection to an IPv6 literal gives a malformed target.
   * This test reuses the absolute-path "/get" of /redirect-relative, and
   * even that one fails: the target then has no brackets, as in
   * "http://::1:<port>/get", which the next hop cannot parse, so the call
   * fails with ccol_http_invalid_url. */
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] relative_location_resolved_against_ipv6_host: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char url[160];
  make_url6(url, sizeof(url), "/redirect-relative");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, userinfo_credentials_carried_same_origin) {
  /* The carried_auth and carried_auth_origin fields that Tier 2 keeps on
   * chttp_async_chain_t: a code path separate from that of Tier 1, which
   * _async_submit_hop implements as a mirror of the Tier 1 logic. */
  chttpcli_construct(cli);
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-same-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* base64("alice:s3cr3t") == "YWxpY2U6czNjcjN0" */
  REQUIRE_STREQ(resp->body, "Basic YWxpY2U6czNjcjN0");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, userinfo_credentials_dropped_cross_origin) {
  if (get_test_port6() == 0) {
    fprintf(stderr,
            "[SKIP] userinfo_credentials_dropped_cross_origin: no IPv6 "
            "loopback listener available in this environment\n");
    return;
  }
  chttpcli_construct(cli);
  char base[128];
  make_url(base, sizeof(base), "/redirect-to-echo-auth-cross-origin");
  char url[192];
  snprintf(url, sizeof(url), "http://alice:s3cr3t@%s", base + 7);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, multi_hop_chain_reaches_final_resource) {
  /* The chain is /redirect-chain-1 -> /redirect-chain-2 -> /get. Each hop
   * gets a fresh chttp_async_ctx_t of its own, which is a new connection,
   * while the whole chain still fulfils exactly one future exactly one
   * time. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-chain-1");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_redirects, exceeding_max_redirects_reports_error) {
  /* /redirect-infinite always redirects to itself. Tier 1 and the async
   * engine both stop after CHTTP_MAX_REDIRECTS hops and report
   * ccol_http_too_many_redirects, instead of looping for ever or giving the
   * last 302 to the caller as an ordinary response. This is the one test in
   * this suite that walks the real cap, so it is also a stress test of the
   * refcount of the chain, and of the hop chaining, across 51 real
   * connections. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/redirect-infinite");

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_too_many_redirects);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*              ASYNC STATE MACHINE; IDLE CONNECTION POOL (TIER 2)          */
/* ========================================================================== */

/*
 * Functional tests for the idle pool of Tier 2: _async_idle_pool_take,
 * _async_idle_pool_offer, the CHTTP_ASYNC_IDLE state, and the retry for a
 * dead connection that reads reused and any_bytes_read. These tests check
 * in the same way as the synchronous `keepalive` suite of Tier 1, by
 * reading test_server_accept_count() before and after, against the same
 * /keepalive and /keepalive-then-close mock routes.
 *
 * Unlike the async_step_a and async_redirects tests above, which pool
 * nothing, a connection that goes into the pool here holds an engine
 * reference of its OWN until something reuses it or the caller destroys
 * the client, which drains its pool at once. These tests therefore call
 * chttpclient_destroy(cli) BEFORE wait_for_async_engine_idle(), the
 * reverse of the order that the rest of this file uses; without that, the
 * reference that the pooled connection holds keeps the ref count of the
 * engine above zero for ever.
 */

static ctpool_future *async_get(chttpcli cli, const char *url) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  return f;
}

TEST(async_early_hints, gives_up_after_too_many_interim_responses) {
  /* Tier 2 and Tier 3 have their own separate loop that discards an interim
   * 1xx response, in _async_on_readable_impl, and it needs the same cap
   * that the test
   * early_hints.tier1_gives_up_after_too_many_interim_responses of Tier 1
   * covers. chttp_async_ctx_t.interim_responses_seen tracks that cap,
   * because this loop can stop and start again across more than one
   * on_readable dispatch callback, while Tier 1 makes one blocking call.
   * /endless-early-hints sends 100, which is well above
   * CHTTP_MAX_INTERIM_RESPONSES of 64. */
  char url[160];
  make_url(url, sizeof(url), "/endless-early-hints");

  chttpcli_construct(cli);
  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_NE(raw->rv, ccol_success);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_early_hints, discards_exactly_64_before_giving_up) {
  /* The async counterpart of
   * early_hints.tier1_discards_exactly_64_before_giving_up, which pins the
   * exact boundary; the test gives_up_after_too_many_interim_responses
   * above only confirms that SOME cap exists, with a fixed stream of 100
   * hints.
   *
   * The interim-discard cap of _async_on_readable_impl,
   * ctx->interim_responses_seen, guards only the continuation of the
   * discard loop and never the read of the final message, which is not a
   * 1xx, so 64 discarded interim responses followed by a real answer
   * succeed here. The sibling loop of Tier 1, _chttp_read_message_loop,
   * guards in the same way (see the comment of that test for what the
   * ordering buys), and this test pins both tiers to exactly the same
   * boundary. */
  char url64[160];
  make_url(url64, sizeof(url64), "/early-hints-count/64");

  chttpcli_construct(cli64);
  ctpool_future *f64 = async_get(cli64, url64);
  REQUIRE_NE((void *)f64, NULL);
  chttpcli_async_result_t *raw64 = chttpclient_async_result_get(f64);
  REQUIRE_NE((void *)raw64, NULL);
  REQUIRE_EQ(raw64->rv, ccol_success);
  REQUIRE_NE((void *)raw64->resp, NULL);
  REQUIRE_EQ(raw64->resp->status_code, 200);
  chttpclient_resp_free(raw64->resp);
  chttpclient_async_result_free(raw64);
  ctpool_future_free(f64);
  chttpclient_destroy(cli64);
  wait_for_async_engine_idle();

  char url65[160];
  make_url(url65, sizeof(url65), "/early-hints-count/65");

  chttpcli_construct(cli65);
  ctpool_future *f65 = async_get(cli65, url65);
  REQUIRE_NE((void *)f65, NULL);
  chttpcli_async_result_t *raw65 = chttpclient_async_result_get(f65);
  REQUIRE_NE((void *)raw65, NULL);
  REQUIRE_NE(raw65->rv, ccol_success);
  REQUIRE_EQ((void *)raw65->resp, NULL);
  chttpclient_async_result_free(raw65);
  ctpool_future_free(f65);
  chttpclient_destroy(cli65);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, sequential_requests_reuse_connection) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    ctpool_future *f = async_get(cli, url);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(f);
  }

  /* This gives the reactor a short moment to finish offering the connection
   * of the last response to the idle pool, which happens directly before
   * the future resolves (see the HPE_PAUSED handling of _async_on_data).
   * The server adds to its own accept count on its own accept() thread, so
   * that count moves independently. */
  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli); /* drains the one still-pooled connection */
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, dead_connection_detected_and_retried) {
  char url1[160], url2[160];
  make_url(url1, sizeof(url1), "/keepalive-then-close");
  make_url(url2, sizeof(url2), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  ctpool_future *f1 = async_get(cli, url1);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpcli_response *resp1 = raw1->resp;
  REQUIRE_NE((void *)resp1, NULL);
  REQUIRE_EQ(resp1->status_code, 200);
  chttpclient_resp_free(resp1);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);

  /* The server closed its end after that response. The on_data and on_close
   * callbacks of the pooled connection, which run in the IDLE state, find
   * this on their own and evict the connection, but that eviction can still
   * be pending when the next request pops the connection. The retry-once
   * mechanism that reads reused and any_bytes_read must then recover by
   * opening a fresh connection, without the caller seeing anything. */
  ctpool_future *f2 = async_get(cli, url2);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  chttpcli_response *resp2 = raw2->resp;
  REQUIRE_NE((void *)resp2, NULL);
  REQUIRE_EQ(resp2->status_code, 200);
  chttpclient_resp_free(resp2);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 2);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, stale_connection_eviction_releases_engine_reference) {
  /* _async_idle_pool_take has a branch that evicts a stale connection: it
   * pops a pooled connection and finds it older than
   * CHTTP_IDLE_MAX_AGE_MS. _async_idle_pool_offer took a separate engine
   * reference for that connection while it sat in the pool, and this branch
   * must release that reference; without that, every connection that ages
   * out leaks one engine reference for ever.
   *
   * This test forces that branch every time with
   * _chttpclient_force_async_idle_stale_for_tests, instead of waiting out
   * the real window of 60 seconds, and then reads the ref count of the
   * engine directly, because the leak has no symptom that a test can see
   * from outside, short of a process that runs for a very long time.
   *
   * The staleness scan of _async_idle_pool_take never tears a stale
   * candidate down itself: it leaves it exactly where it is, still
   * registered for reads with the reactor, and only calls shutdown(fd,
   * SHUT_RDWR) on it. That forces a real EPOLLIN or EPOLLERR dispatch, and
   * the dispatch context then reaps the connection for real and releases
   * this engine reference, independently, not on this thread (see the doc
   * comment of _async_idle_pool_take).
   *
   * The drop of the ref count below can therefore still be pending when
   * the future of the second request resolves, so the check must be a
   * bounded poll, in the same way as wait_for_async_engine_idle; an
   * immediate assertion fails from time to time purely on timing, not on
   * any real leak. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  ctpool_future *f1 = async_get(cli, url);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpclient_resp_free(raw1->resp);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);

  /* Exactly one connection now sits in the idle pool, holding exactly one
   * engine reference of its own, while the reference of the chain for this
   * call went away when the only hop of that chain detached to join the
   * pool. This code polls instead of reading the count once (see the
   * comment of poll_engine_ref_count), because an immediate read here races
   * the future that just resolved. */
  REQUIRE_EQ(poll_engine_ref_count(1), 1);
  _chttpclient_force_async_idle_stale_for_tests(
      _chttpcli_resolve_for_tests(cli));

  ctpool_future *f2 = async_get(cli, url);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  chttpclient_resp_free(raw2->resp);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  /* The reactor reaps the stale connection that the code above forced (the
   * shutdown() drives that dispatch), and that connection must then release
   * its own engine reference. Only one connection may still hold a
   * reference: the fresh connection of this second request, which is back
   * in the pool. The count must therefore settle back to 1 instead of
   * staying at 2. This check is not vacuous: a leak of the reference of the
   * evicted connection keeps the count at 2 for ever, whatever the poll
   * does. */
  REQUIRE_EQ(poll_engine_ref_count(1), 1);

  chttpclient_destroy(cli); /* drains the one still-pooled connection */
  wait_for_async_engine_idle();
}

typedef struct {
  chttpcli cli;
  const char *url;
  int iterations;
} stale_race_arg_t;

static void *stale_race_thread(void *arg) {
  stale_race_arg_t *a = (stale_race_arg_t *)arg;
  for (int i = 0; i < a->iterations; i++) {
    ctpool_future *f = async_get(a->cli, a->url);
    if (!f) continue;
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    if (raw) {
      if (raw->resp) chttpclient_resp_free(raw->resp);
      chttpclient_async_result_free(raw);
    }
    ctpool_future_free(f);
    /* The server closes its end directly after it answers (see
     * /keepalive-then-close), so a real on_readable dispatch for the EOF of
     * the connection that this call just pooled is already in flight on a
     * reactor thread, running independently of whatever this thread does
     * next. This code forces staleness and at once races a fresh request
     * against the SAME origin, to give the best chance that the
     * staleness-eviction branch of _async_idle_pool_take on another thread
     * pops this exact ctx while that separate dispatch touches it. */
    _chttpclient_force_async_idle_stale_for_tests(
        _chttpcli_resolve_for_tests(a->cli));
  }
  return NULL;
}

TEST(async_idle_pool, concurrent_stale_eviction_races_dispatch_no_uaf) {
  /* Stress coverage for a use-after-free that no ordinary test shows: an
   * ordinary application thread calls _async_ctx_free directly on a ctx
   * whose ccol_event_loop registration is still fully live. A connection in
   * the idle pool stays fully attached to the reactor for as long as it
   * sits there, so a reactor dispatch thread can be in the middle of a
   * callback for that exact ctx, having already read ctx->hop_completed as
   * false, at the same moment that an evicting thread frees the ctx; the
   * dispatch thread then touches ctx->idle_lock, ctx->fd and ctx->tls after
   * they are gone.
   *
   * An atomic pin for each ctx (ctx->refs, which every dispatch callback
   * pins through _async_ctx_pin) only NARROWS this; it does not close it.
   * It closes one case that is easy to reproduce, where a dispatch reads
   * ctx->refs == 0 and "resurrects" an object in the middle of its
   * destruction. But a thread can already be past the reg->removed liveness
   * check of ccol_event_loop, in a real dispatch call that is under way, and
   * the OS can then preempt that thread for an unbounded time before it
   * touches ctx->refs at all. _async_ctx_destroy_now can run to completion
   * on another thread inside that window, including the final free of the
   * memory of the ctx, and the first atomic_load of the pin is then itself
   * a use-after-free, whatever the value of ctx->refs is. AddressSanitizer
   * reports both shapes against this exact test; the unpinned one appears
   * in about 1 run in 3 to 12 of this stress loop, even under heavy load.
   *
   * The design closes the race by construction instead of narrowing it: no
   * application thread ever calls the real, destructive teardown
   * (_async_ctx_teardown and _async_ctx_free) on a ctx that is still
   * registered with the reactor. The staleness scan of
   * _async_idle_pool_take tears no stale candidate down; it leaves the
   * candidate exactly where it is and only calls shutdown() on its fd,
   * which forces a real EPOLLIN or EPOLLERR dispatch that reaps the
   * candidate for real, entirely from dispatch context (see the doc
   * comment of that function). Dispatch callbacks for one registration are
   * serialised against each other by the entry->dispatch_lock of
   * ccol_event_loop, together with its "one job in flight for each entry"
   * invariant, which entry->refcount guards (see cthreadcomm.c). Since no
   * application thread frees a ctx directly, no other thread is left to
   * race the touch of the ctx inside a dispatch callback. See the field
   * comment of pending_app_teardown in chttpclient.c for the whole design;
   * it names the two call sites on an application thread, and the
   * staleness path of _async_idle_pool_take is neither of them.
   *
   * A black-box test cannot reproduce the exact interleaving that this
   * test drives. The test
   * stale_connection_eviction_releases_engine_reference above forces
   * staleness with no dispatch activity in flight at the same time, so it
   * cannot reach this interleaving at all. This test stresses the scenario
   * instead: several threads share one client and pool a connection again
   * and again, to a route where the server closes directly after it
   * answers, so a real, separate on_readable dispatch for the EOF is always
   * in flight for the connection that a thread just pooled; each thread
   * then forces staleness and sends a fresh request to the same origin
   * directly after every response. The signal is a clean plain run, plus a
   * clean pass under valgrind,
   * AddressSanitizer and ThreadSanitizer, across many repeated runs, rather
   * than one assertion inside the loop; every request must still succeed,
   * and the test checks that too. Keep this test and this comment, because
   * nothing else exercises this interleaving. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive-then-close");

  chttpcli_construct(cli);

  enum { NUM_THREADS = 6, ITERATIONS_PER_THREAD = 40 };
  pthread_t tids[NUM_THREADS];
  stale_race_arg_t args[NUM_THREADS];
  /* This code captures create_rv instead of asserting on it at once.
   * pthread_create itself can fail part way through this loop (very
   * unlikely in practice, but real exhaustion of threads or resources makes
   * it possible), and an immediate REQUIRE_EQ then returns from this
   * function while the threads of the earlier iterations still run and
   * still use the args[] and tids[] arrays on this stack. That is the same
   * class of stack-use-after-return that the join-before-assert order
   * elsewhere in this file guards against, reached here by a failed
   * creation. created holds exactly how many threads there are to
   * join. */
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < NUM_THREADS; i++) {
    args[i].cli = cli;
    args[i].url = url;
    args[i].iterations = ITERATIONS_PER_THREAD;
    create_rv = pthread_create(&tids[i], NULL, stale_race_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(tids[i], NULL);
  REQUIRE_EQ(create_rv, 0);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/* This allocator fails exactly one allocation call through this
 * ccol_memmgmt_procs_t, the one at index g_hop_fail_at_call, which can be a
 * malloc, a calloc or a realloc, and passes every other call to the real
 * allocator; a value of -1 means "never fail". The test
 * async_idle_pool.reused_hop_setup_failure_releases_engine_reference below
 * uses it to sweep every allocation site inside one hop attempt on a
 * reused connection, following the same pattern as
 * oom.basic_auth_fails_at_every_allocation_site in tests/chttp/tests.c. */
static atomic_int g_hop_fail_call_index;
static int g_hop_fail_at_call = -1;

static void *hop_fail_malloc(size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return malloc(sz);
}
static void *hop_fail_calloc(size_t n, size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return calloc(n, sz);
}
static void *hop_fail_realloc(void *p, size_t sz) {
  int idx = atomic_fetch_add(&g_hop_fail_call_index, 1);
  if (g_hop_fail_at_call >= 0 && idx == g_hop_fail_at_call) return NULL;
  return realloc(p, sz);
}
static void hop_fail_free(void *p) { free(p); }

static ccol_memmgmt_procs_t g_hop_fail_mp = {.malloc = hop_fail_malloc,
                                             .free = hop_fail_free,
                                             .calloc = hop_fail_calloc,
                                             .realloc = hop_fail_realloc};

/* This allocator fails exactly one calloc(n, sz) call, the one whose n*sz
 * equals g_size_fail_target_size (a value of 0 means "never fail"), and
 * passes every other call to the real allocator, every malloc and every
 * realloc included. While g_hop_fail_mp counts call indexes, this allocator
 * picks one allocation by its BYTE SIZE instead, so it stays correct
 * whatever number of other allocations of other sizes run before it. The
 * test
 * async_idle_pool.chain_creation_oom_reports_not_enough_memory_not_generic
 * uses it to fail only the calloc of the chain struct inside
 * _async_chain_create, every time. */
static size_t g_size_fail_target_size = 0;
static void *size_fail_malloc(size_t sz) { return malloc(sz); }
static void *size_fail_calloc(size_t n, size_t sz) {
  if (g_size_fail_target_size != 0 && n * sz == g_size_fail_target_size)
    return NULL;
  return calloc(n, sz);
}
static void *size_fail_realloc(void *p, size_t sz) { return realloc(p, sz); }
static void size_fail_free(void *p) { free(p); }
static ccol_memmgmt_procs_t g_size_fail_mp = {.malloc = size_fail_malloc,
                                              .free = size_fail_free,
                                              .calloc = size_fail_calloc,
                                              .realloc = size_fail_realloc};

TEST(async_idle_pool, reused_hop_setup_failure_releases_engine_reference) {
  /* The reused branch of _async_submit_hop_fail allocates a headers map, a
   * serialisation buffer and an origin_key, and one of those allocations
   * can fail AFTER the code has popped a pooled connection off the idle
   * pool. _async_idle_pool_offer took a separate engine reference for that
   * pooled connection while it sat there, and such a failure must not leak
   * that reference. This is the same rule of ownership as in
   * async_idle_pool.stale_connection_eviction_releases_engine_reference
   * above, which covers the exit from the idle pool through a staleness
   * eviction, while this test covers the exit through a failed setup of a
   * reused hop.
   *
   * This test hardcodes no allocation ordinal inside _async_submit_hop.
   * Instead it sweeps every allocation index across a second request, which
   * uses a reused connection, and checks an invariant that holds wherever
   * the injected failure lands. The async idle pool of this client can end
   * up with zero connections after the swept request, either because the
   * failure landed after the code popped the pool, or because the request
   * succeeded and pooled a connection again that a later reuse took away.
   * In either case the ref count of the engine must be zero too, matching
   * the live membership of the pool exactly, so a failure that lands after
   * the pop must not leave the pool empty while still holding 1 leaked
   * reference. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  char *cerr = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  enum { SWEEP_UPPER = 40 };
  for (int idx = 0; idx < SWEEP_UPPER; idx++) {
    /* This warms the pool again, because a connection that a reuse can take
     * must be in the pool before each swept attempt, and a failure in an
     * earlier iteration can land after the code popped the pool, which
     * leaves the pool empty. */
    g_hop_fail_at_call = -1;
    ctpool_future *fw = async_get(cli, url);
    REQUIRE_NE((void *)fw, NULL);
    chttpcli_async_result_t *rw = chttpclient_async_result_get(fw);
    REQUIRE_NE((void *)rw, NULL);
    REQUIRE_EQ(rw->rv, ccol_success);
    chttpclient_resp_free(rw->resp);
    chttpclient_async_result_free(rw);
    ctpool_future_free(fw);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;

    /* An idx can land on one of the earliest allocations (the preflight check,
     * the creation of the future, and the creation of the chain itself), all of
     * which run before anything touches the idle pool. chttpclient_do_async can
     * then either give back NULL directly, or give back a future whose result
     * carries an error and a NULL resp: the failure path of _async_chain_create
     * fulfils the future with ccol_not_enough_memory and a NULL response. Both
     * are legal, documented outcomes, and this sweep must hold both (it also
     * tolerates a NULL result) instead of assuming them away. */
    ctpool_future *f2 = async_get(cli, url);
    if (f2) {
      chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
      if (raw2) {
        if (raw2->resp) chttpclient_resp_free(raw2->resp);
        chttpclient_async_result_free(raw2);
      }
      ctpool_future_free(f2);
    }
    g_hop_fail_at_call = -1;

    usleep(20000);

    /* This code polls both sides together instead of reading them once: the
     * chain of a future that just resolved may still hold its own engine
     * reference (see the comment of poll_engine_ref_count), and the
     * membership of the idle pool can also still be settling, because a
     * staleness-eviction dispatch can still be reaping the connection of
     * an earlier iteration. This loop samples both again until they agree,
     * with the same bound that poll_engine_ref_count uses, because one
     * sleep of a fixed length is not always enough. */
    size_t pooled = 0;
    int refs = -1;
    for (int i = 0; i < 2000; i++) {
      pooled = _chttpclient_async_idle_total_count_for_tests(
          _chttpcli_resolve_for_tests(cli));
      refs = _chttpclient_engine_ref_count_for_tests();
      if (refs == (int)pooled) break;
      usleep(1000);
    }
    REQUIRE_EQ(refs, (int)pooled);
  }

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool,
     chain_creation_oom_reports_not_enough_memory_not_generic) {
  /* An allocation inside _async_chain_create can fail, and the code must
   * report that by fulfilling the future with a real
   * chttpcli_async_result_t that carries ccol_not_enough_memory, never with
   * a bare NULL data pointer, as in ctpool_future_fulfill(future, NULL). A
   * NULL there makes chttpclient_async_result_get(f) give back NULL, which
   * is exactly what it documents for a CANCELLED future, so a real and
   * common allocation failure gives the same signal as a cancellation and
   * the specific code is lost. Tier 3 (chttpclient_do_pooled) makes that
   * worse: it maps ANY NULL result from chttpclient_async_result_get to the
   * generic ccol_unexpected_failure, so the same allocation failure appears
   * there as a code with no diagnostic value at all.
   *
   * This test uses g_size_fail_mp to reach the behavior exactly: it fails
   * only the one calloc call whose byte size matches the internal chain
   * struct, which _chttp_async_chain_struct_size_for_tests gives, because
   * chttp_async_chain_t itself is not a public type. That guarantees a hit
   * on the calloc of _async_chain_create alone, however many string
   * allocations of other sizes (such as the parse of the URL and
   * initial_origin_key) run before it; that count makes no difference and
   * can change.
   *
   * Counting call indexes with g_hop_fail_mp makes this test vacuous: such
   * a version passes even with the guard fully removed, because an index
   * past the target can land on a LATER ccol_not_enough_memory report
   * elsewhere in _async_submit_hop, which is already correct, so the
   * assertion holds without exercising this behavior at all. Only a run
   * with the guard removed tells the two apart, which is why the test picks
   * the allocation by its size. */
  char url[160];
  make_url(url, sizeof(url), "/get");

  size_t chain_size = _chttp_async_chain_struct_size_for_tests();

  /* Tier 2. */
  {
    char *cerr = NULL;
    chttpcli cli = ccol_create_chttpclient_mp(&g_size_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    g_size_fail_target_size = chain_size;

    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    ctpool_future *f = chttpclient_do_async(cli, req);
    chttp_request_free(req);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_not_enough_memory);
    REQUIRE_EQ((void *)raw->resp, NULL);
    chttpclient_async_result_free(raw);
    ctpool_future_free(f);

    g_size_fail_target_size = 0;
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
  }

  /* Tier 3. The same fault must give the same specific code through
   * chttpclient_do_pooled too, instead of collapsing to the generic
   * ccol_unexpected_failure. */
  {
    char *cerr = NULL;
    chttpcli cli = ccol_create_chttpclient_mp(&g_size_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    g_size_fail_target_size = chain_size;

    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_not_enough_memory);
    REQUIRE_EQ((void *)resp, NULL);

    g_size_fail_target_size = 0;
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
  }
}

TEST(client_construction, tier1_hop_allocation_failure_sweep_no_leak) {
  /* The chttp_do_internal of Tier 1 must free `wire` on EVERY exit from the
   * hop loop: _serialize_request built `wire` earlier in the same hop, and
   * it holds the whole request body for a POST, a PUT or a PATCH. The
   * !pctx.headers branch, which runs when chmap_create_full fails for the
   * response header map of the hop, is one such exit; it leaves through
   * _conn_teardown and _url_free, where a free is easy to miss, while every
   * other break and return past that point does free it.
   *
   * This is the Tier 1 counterpart of the g_hop_fail_mp sweep of
   * reused_hop_setup_failure_releases_engine_reference above, and both
   * inject an out-of-memory condition. This sweep cannot assert on a leak
   * directly, because the tests of this codebase have no allocation
   * tracker beyond the counting mp itself, which counts calls, not the
   * bytes still out. The sweep exists so that `make memtest`, which
   * runs valgrind, really exercises every allocation-failure branch in one
   * plain, synchronous hop of chttp_do_internal, and that run is what
   * catches such a leak. */
  char url[160];
  make_url(url, sizeof(url), "/post");

  const char *payload = "{\"leak\":\"check\"}";
  chttp_request_body_t body = CHTTP_JSON_BODY(payload, strlen(payload));

  enum { SWEEP_UPPER = 60 };
  for (int idx = -1; idx < SWEEP_UPPER; idx++) {
    char *cerr = NULL;
    chttpcli cli = ccol_create_chttpclient_mp(&g_hop_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);

    char *rerr = NULL;
    chttp_request_t *req =
        chttp_request_new_mp(CHTTP_POST, url, &body, &g_hop_fail_mp, &rerr);
    REQUIRE_NE((void *)req, NULL);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx; /* -1 comes first. It confirms that the happy
                               * path passes under this allocator. */

    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    if (rv == ccol_success) chttpclient_resp_free(resp);

    g_hop_fail_at_call = -1;
    chttp_request_free(req);
    chttpclient_destroy(cli);
  }
}

typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} async_idle_concurrent_arg_t;

static void *async_idle_concurrent_thread(void *arg) {
  async_idle_concurrent_arg_t *a = (async_idle_concurrent_arg_t *)arg;
  ctpool_future *f = async_get(a->cli, a->url);
  if (!f) return NULL;
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw && raw->rv == ccol_success) {
    chttpcli_response *resp = raw->resp;
    a->ok = resp && resp->status_code == 200;
    chttpclient_resp_free(resp);
  }
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  return NULL;
}

TEST(async_idle_pool, concurrent_requests_exceeding_idle_cap_no_crash) {
  /* This test fires more keep-alive requests at the same time than the idle
   * pool can hold for each origin. The pool takes none of the connections
   * above that cap, which is the documented, deliberate design that the
   * matching keepalive test of Tier 1 also exercises, and nothing crashes,
   * leaks or fails. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  enum { N = 12 };
  pthread_t threads[N];
  async_idle_concurrent_arg_t args[N];
  /* create_rv and created: see the identical comment of
   * async_idle_pool.concurrent_stale_eviction_races_
   * dispatch_no_uaf. They guard against a stack-use-after-return if
   * pthread_create itself fails part way through this loop, since
   * threads[] and args[] live on the stack. */
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    create_rv = pthread_create(&threads[i], NULL, async_idle_concurrent_thread,
                               &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_idle_pool, offer_push_failure_no_double_free) {
  /* A regression test against a double free in the out-of-memory branch of
   * _async_idle_pool_offer. The has_room check confirms the capacity first,
   * but cvector_push_back can still fail after that, and the function then
   * tears ctx down itself, with _async_chain_release, _async_ctx_free and
   * _client_engine_release, so it must not return false. The documented
   * contract of false is "ctx left completely untouched, caller falls back
   * to a normal teardown", and a false makes the caller,
   * _async_finish_connection, call _async_ctx_finish() on that same ctx a
   * second time, after it is already freed: a real double free.
   *
   * cvector_push_back can never really fail at this call site under real
   * pressure on the allocator: CHTTP_MAX_IDLE_PER_ORIGIN equals the
   * _ccol_cvector_minimum_capacity of cvector itself, so the list of each
   * origin never needs to grow for any push that the has_room check of
   * this function lets through. Ordinary injection of an allocator failure
   * (the g_hop_fail_mp pattern that the rest of this file uses) therefore
   * cannot reach this branch at all, and
   * _chttpclient_force_offer_push_fail_once_for_tests exists to make the
   * branch reachable every time. The one real job of this test is to not
   * crash: valgrind and AddressSanitizer catch a regression here (see
   * `make memtest`), while none of the assertions below catches it. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  _chttpclient_force_offer_push_fail_once_for_tests();

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  /* The connection never went into the pool, because the simulated push
   * failure discarded it instead, so the idle pool must be empty and must
   * hold no engine reference for that connection. The count of the idle
   * pool races nothing here, because the out-of-memory branch that this
   * test forces never touches idle_total_count_async either way. The ref
   * count does race (see the comment of poll_engine_ref_count), which is
   * why this code polls it instead of reading it once directly after the
   * future resolves. */
  REQUIRE_EQ(_chttpclient_async_idle_total_count_for_tests(
                 _chttpcli_resolve_for_tests(cli)),
             (size_t)0);
  REQUIRE_EQ(poll_engine_ref_count(0), 0);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * The code can flip a registration to the write direction while its
 * connection has nothing at all to write. The deadline sweep samples the
 * state of a connection and then issues the flip that delivers a "100
 * Continue" timeout, and between those two steps the dispatch of another
 * thread can move that same connection on, to the read of its response or
 * all the way into the idle pool. The write-direction dispatch path must
 * restore the read direction for every such state.
 *
 * The scheduling of the OS decides which thread wins that race, so both
 * tests below reproduce the losing order directly instead of racing for
 * it: each one starts at the exact transition that the race can land
 * directly after.
 *
 * A connected socket is almost always writable, so the shared,
 * process-wide client reactor dispatches a write-armed registration with
 * nothing to write again and again for as long as it stays that way, and
 * nothing else ever issues another modify for it. Without the restore,
 * that connection is out of reach for the rest of the process: a hop that
 * is in flight never completes, and its own request deadline does not
 * help, because the shutdown of that deadline goes to the very dispatch
 * that does nothing; a pooled connection can never retire, which blocks
 * chttpclient_destroy for ever. Both tests therefore confirm the self-heal
 * FIRST, while they can still report, and then stop the process with a
 * diagnostic instead of leaving every later call in the binary blocked
 * for ever.
 */
static unsigned long stray_write_arm_restores(void) {
  return (unsigned long)_chttpclient_stray_write_arm_restores_for_tests();
}

static void die_if_wedged(bool ok, const char *what) {
  if (ok) return;
  fprintf(stderr,
          "chttpclient: a write-armed registration with nothing to write "
          "never restored its read direction (%s); the shared reactor is "
          "spinning on it and every remaining call would block forever\n",
          what);
  fflush(stderr);
  abort();
}

/* Both waits below are bounded only as a safety net against a hang; the
 * restore lands one or two dispatches after the arming. */
static bool wait_for_stray_write_arm_restore(unsigned long before) {
  for (int i = 0; i < 500; i++) {
    if (stray_write_arm_restores() > before) return true;
    usleep(10000);
  }
  return false;
}

static bool wait_for_future_done(ctpool_future *f) {
  for (int i = 0; i < 500; i++) {
    if (ctpool_future_done(f)) return true;
    usleep(10000);
  }
  return false;
}

TEST(async_stray_write_arm, a_pooled_connection_write_arm_self_heals) {
  /* This test is not vacuous: without the restore, the count below never
   * moves. The request itself succeeds either way, which is why this test
   * waits on the count. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  wait_for_async_engine_idle();
  unsigned long restores_before = stray_write_arm_restores();
  _chttpclient_force_stray_idle_write_arm_once_for_tests();

  ctpool_future *f = async_get(cli, url);
  ccol_retval_t rv = ccol_unexpected_failure;
  int status = 0;
  if (f) {
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    if (raw) {
      rv = raw->rv;
      if (raw->resp) {
        status = raw->resp->status_code;
        chttpclient_resp_free(raw->resp);
      }
      chttpclient_async_result_free(raw);
    }
    ctpool_future_free(f);
  }
  die_if_wedged(wait_for_stray_write_arm_restore(restores_before),
                "idle-pooled connection");

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(async_stray_write_arm, an_in_flight_connection_write_arm_self_heals) {
  /* The same property, one transition earlier, where the connection still
   * waits for its response. The check for a wedge here reads behavior
   * rather than a counter: the hop has to finish. This test is not
   * vacuous: without the restore, the future never completes. */
  chttpcli_construct(cli);
  char url[160];
  make_url(url, sizeof(url), "/get");

  wait_for_async_engine_idle();
  unsigned long restores_before = stray_write_arm_restores();
  _chttpclient_force_stray_write_arm_once_for_tests();

  ctpool_future *f = async_get(cli, url);
  die_if_wedged(f != NULL && wait_for_future_done(f), "in-flight connection");

  ccol_retval_t rv = ccol_unexpected_failure;
  int status = 0;
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw) {
    rv = raw->rv;
    if (raw->resp) {
      status = raw->resp->status_code;
      chttpclient_resp_free(raw->resp);
    }
    chttpclient_async_result_free(raw);
  }
  ctpool_future_free(f);
  unsigned long restores_after = stray_write_arm_restores();

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(restores_after, restores_before);
}

TEST(async_idle_pool, reactivate_failure_retries_without_uaf) {
  /* The reused-connection path of _async_submit_hop pops an idle connection
   * and then re-activates its registration for write interest, and
   * ccol_event_loop_modify() can fail there. The code must not then call
   * _async_ctx_teardown(ctx) directly on the application thread that made
   * the call, which is not a reactor thread, while ctx is still fully
   * registered with the shared reactor: that is the same class of
   * use-after-free that
   * async_idle_pool.concurrent_stale_eviction_races_dispatch_no_uaf above
   * covers for the staleness-eviction path, sitting at this exit
   * instead.
   *
   * This path must also release the separate engine reference that
   * _async_idle_pool_offer took for this ctx while it sat in the idle pool,
   * which is the same class of leak that
   * async_idle_pool.reused_hop_setup_failure_releases_engine_reference
   * covers for a setup failure a little earlier in the same function.
   *
   * There is no natural way to make ccol_event_loop_modify fail here: a
   * connection in the idle pool is always registered for reads alone, so
   * nothing holds the write slot that it needs, and ccol_event_loop_modify
   * allocates nothing of its own, so an injected allocator failure has no
   * target. _chttpclient_force_reactivate_fail_once_for_tests exists to
   * make this branch reachable every time.
   *
   * valgrind and AddressSanitizer catch a breach of the first rule (see
   * `make memtest`), while no assertion below catches it. But this test
   * also checks the two consequences that it can see from outside: the
   * retried request must still succeed from end to end, on a brand-new
   * connection, and the reap of the abandoned connection, which its own
   * dispatch drives, must release its engine reference instead of leaking
   * it. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);

  /* Seed a reusable pooled connection. */
  ctpool_future *f1 = async_get(cli, url);
  REQUIRE_NE((void *)f1, NULL);
  chttpcli_async_result_t *raw1 = chttpclient_async_result_get(f1);
  REQUIRE_NE((void *)raw1, NULL);
  REQUIRE_EQ(raw1->rv, ccol_success);
  chttpclient_resp_free(raw1->resp);
  chttpclient_async_result_free(raw1);
  ctpool_future_free(f1);
  REQUIRE_EQ(_chttpclient_async_idle_total_count_for_tests(
                 _chttpcli_resolve_for_tests(cli)),
             (size_t)1);

  _chttpclient_force_reactivate_fail_once_for_tests();

  /* This pops the pooled connection, meets the forced failure of
   * ccol_event_loop_modify, and then retries against a brand-new
   * connection, which must still succeed. */
  ctpool_future *f2 = async_get(cli, url);
  REQUIRE_NE((void *)f2, NULL);
  chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
  REQUIRE_NE((void *)raw2, NULL);
  REQUIRE_EQ(raw2->rv, ccol_success);
  REQUIRE_NE((void *)raw2->resp, NULL);
  REQUIRE_EQ(raw2->resp->status_code, 200);
  chttpclient_resp_free(raw2->resp);
  chttpclient_async_result_free(raw2);
  ctpool_future_free(f2);

  /* The abandoned ctx has its own reap, which the shutdown() triggers and
   * which runs from dispatch context, independently and not on this
   * thread. That reap must settle the ref count of the engine back down so
   * that only the retry connection, which is back in the pool, accounts for
   * it; it must not leak the idle-pool reference that the abandoned
   * connection still held. */
  int refs = -1;
  for (int i = 0; i < 2000; i++) {
    refs = _chttpclient_engine_ref_count_for_tests();
    if (refs <= 1) break;
    usleep(1000);
  }
  REQUIRE_TRUE(refs <= 1);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(max_idle_origins, tier2_distinct_origins_bounded_and_reclaimed) {
  /* The async counterpart, for Tier 2 and Tier 3, of
   * max_idle_origins.tier1_distinct_origins_bounded_and_reclaimed (see the
   * comment of that test for the whole reason). It pins the same two
   * properties, here in _async_idle_pool_take and _async_idle_pool_offer,
   * and uses chttpclient_do_async and async_get instead of the blocking
   * chttpclient_do, with the same structure and mechanism otherwise. An
   * ordinary reuse pop of the last pooled ctx of an origin prunes the map
   * entry of that origin, inside the same locked section as the pop, and
   * since /get sends a real "Connection: close", nothing offers the reused
   * ctx again afterward, so the prune stays visible. */
  _chttpclient_set_max_idle_origins_for_tests(2);

  char url4[160], url6[160], url_unix[256];
  make_url(url4, sizeof(url4), "/keepalive");
  make_url6(url6, sizeof(url6), "/keepalive");
  make_unix_url(url_unix, sizeof(url_unix), "/keepalive");

  chttpcli_construct(cli);
  struct chttpclient *raw = _chttpcli_resolve_for_tests(cli);
  REQUIRE_NE((void *)raw, NULL);

  ctpool_future *f4 = async_get(cli, url4);
  REQUIRE_NE((void *)f4, NULL);
  chttpcli_async_result_t *raw4 = chttpclient_async_result_get(f4);
  REQUIRE_NE((void *)raw4, NULL);
  REQUIRE_EQ(raw4->rv, ccol_success);
  chttpclient_resp_free(raw4->resp);
  chttpclient_async_result_free(raw4);
  ctpool_future_free(f4);

  ctpool_future *f6 = async_get(cli, url6);
  REQUIRE_NE((void *)f6, NULL);
  chttpcli_async_result_t *raw6 = chttpclient_async_result_get(f6);
  REQUIRE_NE((void *)raw6, NULL);
  REQUIRE_EQ(raw6->rv, ccol_success);
  chttpclient_resp_free(raw6->resp);
  chttpclient_async_result_free(raw6);
  ctpool_future_free(f6);

  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_async_key_count_for_tests(raw), (size_t)2);

  /* A third, genuinely distinct origin (Unix) still succeeds, but it must not
   * grow the key count past the (overridden) cap of 2. */
  ctpool_future *fu1 = async_get(cli, url_unix);
  REQUIRE_NE((void *)fu1, NULL);
  chttpcli_async_result_t *rawu1 = chttpclient_async_result_get(fu1);
  REQUIRE_NE((void *)rawu1, NULL);
  REQUIRE_EQ(rawu1->rv, ccol_success);
  chttpclient_resp_free(rawu1->resp);
  chttpclient_async_result_free(rawu1);
  ctpool_future_free(fu1);

  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_async_key_count_for_tests(raw), (size_t)2);

  /* This block reclaims a slot by popping the one pooled ctx of IPv4 with an
   * ordinary reuse. Since /get sends Connection: close, nothing offers that
   * ctx again afterward, so the entry of IPv4 is pruned for good. With a
   * broken prune, the key count below reads 2 instead of 1, the extra one
   * being an empty entry for IPv4 that nothing removed. */
  char url4_close[160];
  make_url(url4_close, sizeof(url4_close), "/get");
  ctpool_future *f4b = async_get(cli, url4_close);
  REQUIRE_NE((void *)f4b, NULL);
  chttpcli_async_result_t *raw4b = chttpclient_async_result_get(f4b);
  REQUIRE_NE((void *)raw4b, NULL);
  REQUIRE_EQ(raw4b->rv, ccol_success);
  chttpclient_resp_free(raw4b->resp);
  chttpclient_async_result_free(raw4b);
  ctpool_future_free(f4b);

  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_async_key_count_for_tests(raw), (size_t)1);

  /* The slot of IPv4 is now really free, so a fresh Unix request must be
   * able to claim it. */
  ctpool_future *fu2 = async_get(cli, url_unix);
  REQUIRE_NE((void *)fu2, NULL);
  chttpcli_async_result_t *rawu2 = chttpclient_async_result_get(fu2);
  REQUIRE_NE((void *)rawu2, NULL);
  REQUIRE_EQ(rawu2->rv, ccol_success);
  chttpclient_resp_free(rawu2->resp);
  chttpclient_async_result_free(rawu2);
  ctpool_future_free(fu2);

  usleep(20000);
  REQUIRE_EQ(_chttpclient_idle_pools_async_key_count_for_tests(raw), (size_t)2);

  _chttpclient_set_max_idle_origins_for_tests(0);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * THE ASYNC DEADLINE SWEEP, which applies connect_timeout_ms and
 * request_timeout_ms on Tier 2. These tests exercise the periodic sweep
 * that the reactor owns, which forcibly closes every connection whose
 * absolute wall-clock deadline has passed, with the same timeout meaning
 * as Tier 1 (ccol_timed_out). They also check that a wide timeout never
 * disturbs a request that would otherwise succeed.
 */

TEST(async_deadline, request_timeout_fires_against_slow_endpoint) {
  char url[160];
  make_url(url, sizeof(url),
           "/very-slow"); /* server sleeps 500ms before responding */

  chttpcli_construct(cli);
  /* This timeout is well below the 500ms sleep of /very-slow, even with the
   * worst-case detection latency of the deadline sweep added (the sweep
   * ticks every CHTTP_DEADLINE_SWEEP_INTERVAL_MS, which is 100ms, rather
   * than at once), and well above a connect over the loopback, so it
   * isolates the request deadline from the connect deadline. This test
   * deliberately avoids /slow, which sleeps only 100ms and so puts the
   * worst-case detection latency and the arrival of the real response too
   * close together to judge. See the comment of /very-slow in the mock
   * server. */
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 20000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_timed_out);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_request_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/slow");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 5000000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, connect_timeout_fires_against_unroutable_address) {
  chttpcli_construct(cli);
  /* This timeout is short, so it fires quickly. TEST-NET-1, from RFC 5737,
   * is never routable on a normal network, so the connect deadline trips
   * here, never the request deadline. But some sandboxed or virtualized
   * network environments answer it with an immediate rejection, such as
   * ENETUNREACH, instead of letting the packet silently vanish.
   *
   * This is reproducible outside the library: a raw connect attempt with
   * `bash -c 'exec 3<>/dev/tcp/192.0.2.1/9'`, in this same CI environment
   * or sandbox, sometimes hangs for the whole probe and sometimes fails at
   * once with "Network is unreachable". That two-way behavior belongs to
   * the underlying network, and the connect-timeout logic of chttpclient
   * neither controls it nor hides it.
   *
   * A connection failure that happens at once still proves that the
   * connect did not silently succeed, so this test accepts
   * ccol_http_connection_failed and ccol_http_transfer_aborted beside the
   * "real" ccol_timed_out outcome; the second of those is a fio_socket
   * failure that comes through the async engine. */
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 300000), ccol_success);

  ctpool_future *f = async_get(cli, "http://192.0.2.1:9/");
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  bool connect_did_not_silently_succeed =
      raw->rv == ccol_timed_out || raw->rv == ccol_http_connection_failed ||
      raw->rv == ccol_http_transfer_aborted;
  REQUIRE_TRUE(connect_did_not_silently_succeed);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_deadline, generous_connect_timeout_does_not_interfere) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 5000000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * A raw TCP listener that accepts a connection and then speaks no byte of
 * TLS, or of anything else, to it. Only the async_deadline test
 * tls_handshake_stuck_peer_reports_timed_out_not_handshake_failed
 * below uses it, to stall a client in the middle of a handshake every
 * time, unlike the test connect_timeout_fires_against_unroutable_address
 * above, which depends on how the network around it behaves toward an
 * unroutable address. The accept thread here blocks in recv() until the
 * client shuts the connection down: the deadline sweep calls shutdown(fd,
 * SHUT_RDWR), and the ordinary close() of the ctx follows after the
 * teardown, so the thread always exits cleanly on its own, without
 * anything having to signal it, and nothing leaks.
 */
typedef struct {
  int listen_fd;
  int port;
  pthread_t tid;
} tls_black_hole_srv_t;

static void *tls_black_hole_accept_thread(void *arg) {
  tls_black_hole_srv_t *s = (tls_black_hole_srv_t *)arg;
  int conn_fd = accept(s->listen_fd, NULL, NULL);
  if (conn_fd >= 0) {
    char buf[16];
    while (recv(conn_fd, buf, sizeof(buf), 0) > 0) { /* it sends nothing */
    }
    close(conn_fd);
  }
  return NULL;
}

static bool tls_black_hole_srv_start(tls_black_hole_srv_t *s) {
  s->listen_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->listen_fd < 0) return false;
  int yes = 1;
  setsockopt(s->listen_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
  struct sockaddr_in addr = {0};
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;
  if (bind(s->listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(s->listen_fd);
    return false;
  }
  socklen_t alen = sizeof(addr);
  if (getsockname(s->listen_fd, (struct sockaddr *)&addr, &alen) != 0) {
    close(s->listen_fd);
    return false;
  }
  s->port = ntohs(addr.sin_port);
  if (listen(s->listen_fd, 1) != 0) {
    close(s->listen_fd);
    return false;
  }
  if (pthread_create(&s->tid, NULL, tls_black_hole_accept_thread, s) != 0) {
    close(s->listen_fd);
    return false;
  }
  return true;
}

static void tls_black_hole_srv_stop(tls_black_hole_srv_t *s) {
  pthread_join(s->tid, NULL);
  close(s->listen_fd);
}

TEST(async_deadline,
     tls_handshake_stuck_peer_reports_timed_out_not_handshake_failed) {
  /* When the deadline sweep calls shutdown(fd, SHUT_RDWR) against a
   * connection that is stuck in CHTTP_ASYNC_TLS_HANDSHAKING, that must
   * appear as ccol_timed_out, which chttpclient.h documents for exactly
   * this case, where connect_timeout_ms expires, and not as
   * ccol_http_tls_handshake_failed or
   * ccol_http_tls_cert_verification_failed, which two branches can give:
   * the CTLS_HANDSHAKE_ERROR branch of _async_tls_advance and the
   * TLS_HANDSHAKING branch of _async_on_error_impl.
   *
   * The test connect_timeout_fires_against_unroutable_address above
   * depends on how the network around it behaves toward an unroutable
   * address, so it accepts three different outcomes. Here a peer completes
   * the TCP handshake and then sends no TLS byte at all, which stalls in
   * TLS_HANDSHAKING every time until the deadline sweep itself steps in, so
   * this test can assert one exact outcome. */
  tls_black_hole_srv_t srv;
  REQUIRE_TRUE(tls_black_hole_srv_start(&srv));

  char url[64];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/", srv.port);

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_connect_timeout(cli, 200000), ccol_success);

  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_timed_out);
  REQUIRE_EQ((void *)raw->resp, NULL);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  tls_black_hole_srv_stop(&srv);
}

/*
 * ASYNC STREAMING, through chttpclient_do_async_streaming on Tier 2. These
 * tests reuse stream_sink_t, stream_sink_write and abort_write_fn, which
 * this file defines earlier for the streaming tests of Tier 1, and they run
 * against the same async engine that the async_step_a, async_redirects,
 * async_idle_pool and async_deadline suites above exercise.
 */

static ctpool_future *async_get_streaming(chttpcli cli, const char *url,
                                          chttpcli_write_fn write_fn,
                                          void *write_ctx) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  ctpool_future *f =
      chttpclient_do_async_streaming(cli, req, write_fn, write_ctx);
  chttp_request_free(req);
  return f;
}

TEST(async_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The callback got the body and nothing buffered it, so
   * chttpcli_response.body stays NULL, just as the chttpclient_do_streaming
   * of Tier 1 leaves it. */
  REQUIRE_EQ((void *)resp->body, NULL);
  REQUIRE_GT(sink.len, (size_t)0);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, large_body_streamed_in_chunks) {
  char url[160];
  make_url(url, sizeof(url), "/large");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ((void *)resp->body, NULL);
  /* /large serves 8192 'x' bytes, while the buffer of stream_sink_t captures
   * at most 4095 bytes. The callback must still get every chunk; a partial
   * capture is the choice of the sink, not a failed transfer, as the
   * "rv == ccol_success" above already proves. */
  REQUIRE_EQ(sink.len, sizeof(sink.buf) - 1);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);

  ctpool_future *f = async_get_streaming(cli, url, abort_write_fn, NULL);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_http_transfer_aborted);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, null_write_fn_returns_null) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ((void *)chttpclient_do_async_streaming(cli, req, NULL, NULL),
             NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(async_streaming, redirect_final_body_delivered_not_intermediate) {
  /* This test uses /redirect-with-body on purpose, because the intermediate
   * 301 body of /redirect is always empty: against an empty intermediate
   * body, nothing shows a client that feeds the body of the intermediate
   * hop straight to the sink of the caller instead of to _sink_discard,
   * since there is no text there to leak. This test therefore uses
   * REQUIRE_STREQ against the exact final body, not only
   * REQUIRE_GT(sink.len, 0), which is what proves that the distinctive text
   * of the intermediate body never reached the sink. */
  char url[160];
  make_url(url, sizeof(url), "/redirect-with-body");

  chttpcli_construct(cli);
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));

  ctpool_future *f = async_get_streaming(cli, url, stream_sink_write, &sink);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  /* This is the status of the final resource, not the 301. The sink must
   * hold the body of the FINAL hop alone, because inside the library a
   * redirect hop goes through _sink_discard, just as in Tier 1. */
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(sink.buf, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

/*
 * THE POOLED SYNCHRONOUS API of Tier 3: chttpclient_do_pooled and
 * chttpclient_do_pooled_streaming, both thin blocking wrappers over Tier 2.
 * These tests reuse stream_sink_t, stream_sink_write and abort_write_fn,
 * which this file defines earlier for the streaming tests of Tier 1.
 */

TEST(pooled, get_200) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "{\"status\":\"ok\"}");

  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled, null_checks) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do_pooled(CHTTPCLI_INVALID, req, &resp),
             ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_pooled(cli, NULL, &resp), ccol_invalid_args);
  REQUIRE_EQ(chttpclient_do_pooled(cli, req, NULL), ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled, bad_url_returns_specific_error_not_generic) {
  /* This is the main promise of this tier. Tier 2 collapses a failure before
   * the queue into a plain NULL: chttpclient_do_async gives back NULL here,
   * which nothing tells apart from an out-of-memory condition or a failed
   * start of the engine. Tier 3 must still give the exact code that
   * chttpclient_do gives for the same URL (see
   * _chttp_async_preflight_check). */
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled, unreadable_cert_path_reports_cert_load_failed) {
  /* The counterpart for Tier 2 and Tier 3 of
   * tls.unreadable_cert_path_reports_cert_load_failed.
   * chttpclient_do_pooled and chttpclient_do_pooled_streaming share
   * _chttp_async_preflight_check, which must report the same specific
   * ccol_http_tls_cert_load_failed that Tier 1 reports here, not the
   * generic ccol_unexpected_failure that chttpclient_do_pooled otherwise
   * gives for a failure before the queue. */
  chttpcli_construct(cli);

  chttp_tls_config_t tls = {
      .cert_path = "/nonexistent/does-not-exist.crt",
      .key_path = "/nonexistent/does-not-exist.key",
      .ca_bundle_path = NULL,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled, allocation_failure_reports_not_enough_memory) {
  /* chttpclient_do_pooled reports the real cause of an allocation that
   * fails, before the request is queued as well as on the reactor, and that
   * cause is ccol_not_enough_memory, exactly as in Tier 1. It is never the
   * generic ccol_unexpected_failure. The sweep fails each allocation index
   * of one call in turn, so that it needs no knowledge of which allocation
   * comes first. This test is non-vacuous: a Tier 3 that maps a NULL future
   * to ccol_unexpected_failure fails it at the index of the copy of the
   * origin key, and a fulfilment that allocates its result on the reactor
   * fails it at the index of that allocation. */
  char url[160];
  make_url(url, sizeof(url), "/get");

  char *cerr = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  enum { SWEEP_UPPER = 40 };
  int unexpected_at = -1;
  int oom_seen = 0;
  bool success_seen = false;
  for (int idx = 0; idx < SWEEP_UPPER && !success_seen; idx++) {
    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
    g_hop_fail_at_call = -1;
    if (rv == ccol_unexpected_failure && unexpected_at < 0) unexpected_at = idx;
    if (rv == ccol_not_enough_memory) oom_seen++;
    if (rv == ccol_success) success_seen = true;
    chttpclient_resp_free(resp);
  }
  g_hop_fail_at_call = -1;
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(unexpected_at, -1);
  REQUIRE_GT(oom_seen, 0);
  REQUIRE_TRUE(success_seen);
}

TEST(pooled, redirect_followed_transparently) {
  char url[160];
  make_url(url, sizeof(url), "/redirect");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);

  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

typedef struct {
  chttpcli cli;
  char url[160];
  bool ok;
} pooled_concurrent_arg_t;

static void *pooled_concurrent_thread(void *arg) {
  pooled_concurrent_arg_t *a = (pooled_concurrent_arg_t *)arg;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, a->url, NULL, NULL);
  if (!req) return NULL;
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(a->cli, req, &resp);
  a->ok = (rv == ccol_success) && resp && resp->status_code == 200;
  if (resp) chttpclient_resp_free(resp);
  chttp_request_free(req);
  return NULL;
}

TEST(pooled, concurrent_callers_all_succeed) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  enum { N = 12 };
  pthread_t threads[N];
  pooled_concurrent_arg_t args[N];
  /* create_rv and created: see the identical comment of
   * async_idle_pool.concurrent_stale_eviction_races_
   * dispatch_no_uaf. They guard against a stack-use-after-return if
   * pthread_create itself fails part way through this loop, since
   * threads[] and args[] live on the stack. */
  int created = 0;
  int create_rv = 0;
  for (int i = 0; i < N; i++) {
    args[i].cli = cli;
    snprintf(args[i].url, sizeof(args[i].url), "%s", url);
    args[i].ok = false;
    create_rv =
        pthread_create(&threads[i], NULL, pooled_concurrent_thread, &args[i]);
    if (create_rv != 0) break;
    created++;
  }
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(create_rv, 0);
  for (int i = 0; i < N; i++) REQUIRE_TRUE(args[i].ok);

  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, basic_get_delivers_body_via_callback) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  int status = 0;
  ccol_retval_t rv = chttpclient_do_pooled_streaming(
      cli, req, stream_sink_write, &sink, &status);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_GT(sink.len, (size_t)0);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, null_write_fn_returns_invalid_args) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  REQUIRE_EQ(chttpclient_do_pooled_streaming(cli, req, NULL, NULL, NULL),
             ccol_invalid_args);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

TEST(pooled_streaming, write_fn_returning_less_aborts_transfer) {
  char url[160];
  make_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ccol_retval_t rv =
      chttpclient_do_pooled_streaming(cli, req, abort_write_fn, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_transfer_aborted);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(pooled_streaming, bad_url_returns_specific_error_not_generic) {
  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http:///get", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval_t rv =
      chttpclient_do_pooled_streaming(cli, req, stream_sink_write, &sink, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);

  chttp_request_free(req);
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*                     UNIX DOMAIN SOCKET TESTS (http+unix://)                */
/*                                                                            */
/* chttpclient supports real "http+unix://" URLs. See                        */
/* _parse_chttp_unix_url, _unix_connect and the is_unix branch of            */
/* _async_connect_task, in src/chttpclient.c. These tests exercise that      */
/* support from end to end. They run against the Unix-domain listener of the */
/* mock server in this file, which is srv_accept_loop_unix. They cover all   */
/* three tiers. They cover connection pooling, which the "unix://<path>"     */
/* origin_key names. They also cover the errors that only this scheme has,   */
/* in the parse of a URL and at connect time.                                */
/* ========================================================================== */

TEST(unix_socket, get_request_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

TEST(unix_socket, post_with_body_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/echo-method-body");

  const char *body_str = "hello-over-unix-socket";
  chttp_request_body_t body = CHTTP_JSON_BODY(body_str, strlen(body_str));
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_post(url, &body, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  /* /echo-method-body responds with "<METHOD>:<body_len>". */
  char expected[64];
  snprintf(expected, sizeof(expected), "POST:%zu", strlen(body_str));
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

TEST(unix_socket, keepalive_reuses_connection) {
  char url[256];
  make_unix_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv = chttpclient_do(cli, req, &resp);
    chttp_request_free(req);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
  }

  /* The same reason as in keepalive.sequential_requests_reuse_connection:
   * give the accept-count increment of the server a brief moment to land. */
  usleep(20000);
  int accepts_after = test_server_accept_count();
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  chttpclient_destroy(cli);
}

TEST(unix_socket, nonexistent_socket_path_fails) {
  char url[512];
  char encoded[256];
  percent_encode_unix_path(encoded, sizeof(encoded),
                           "/tmp/chttpclient_test_nonexistent_xyz.sock");
  snprintf(url, sizeof(url), "http+unix://%s/get", encoded);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_connection_failed);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(unix_socket, path_too_long_returns_invalid_url) {
  /* sizeof(struct sockaddr_un.sun_path) is 108 on Linux, so a raw path of
   * 108 bytes or more, before the NUL, does not fit. The client must reject
   * such a URL as a bad URL instead of trying to connect. */
  char raw_path[200];
  memset(raw_path, 'a', sizeof(raw_path) - 1);
  raw_path[0] = '/';
  raw_path[sizeof(raw_path) - 1] = '\0';

  char encoded[512];
  percent_encode_unix_path(encoded, sizeof(encoded), raw_path);
  char url[600];
  snprintf(url, sizeof(url), "http+unix://%s/get", encoded);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttp_get(url, &resp);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
  REQUIRE_EQ((void *)resp, NULL);
}

TEST(unix_socket_async, basic_get_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  ctpool_future *f = async_get(cli, url);
  REQUIRE_NE((void *)f, NULL);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(unix_socket_async, sequential_requests_reuse_connection_via_idle_pool) {
  char url[256];
  make_unix_url(url, sizeof(url), "/keepalive");

  chttpcli_construct(cli);
  int accepts_before = test_server_accept_count();

  for (int i = 0; i < 5; i++) {
    ctpool_future *f = async_get(cli, url);
    REQUIRE_NE((void *)f, NULL);
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    REQUIRE_NE((void *)raw->resp, NULL);
    REQUIRE_EQ(raw->resp->status_code, 200);
    chttpclient_resp_free(raw->resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(f);
  }

  usleep(20000);
  int accepts_after = test_server_accept_count();
  /* This confirms that the pool really keys on the "unix://<path>"
   * origin_key (see how _parse_chttp_unix_url builds it). If the pool held
   * no Unix-socket connection at all, opened one fresh connection for each
   * request, or keyed such a connection the same as some other origin and
   * then evicted it or sent it to the wrong place, this check would see 5
   * accepts. */
  REQUIRE_EQ(accepts_after - accepts_before, 1);

  /* This follows the destroy-before-wait order of async_idle_pool: a
   * connection that goes into the pool holds an engine reference of its own
   * until something reuses it or chttpclient_destroy drains it. */
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(unix_socket_pooled, basic_get_succeeds) {
  char url[256];
  make_unix_url(url, sizeof(url), "/get");

  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do_pooled(cli, req, &resp);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);

  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
}

TEST(url_parsing, http_unix_scheme_basic) {
  bool is_https = false, is_ipv6 = false, is_unix = false;
  char *host = NULL, *pq = NULL, *origin_key = NULL, *auth = NULL;
  char *unix_path = NULL;
  uint16_t port = 0;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock/api/users", &is_https, &is_ipv6, &host,
      &port, &pq, &origin_key, &auth, &is_unix, &unix_path);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_unix);
  REQUIRE_FALSE(is_https);
  REQUIRE_STREQ(unix_path, "/tmp/app.sock");
  REQUIRE_STREQ(pq, "/api/users");
  REQUIRE_STREQ(origin_key, "unix:///tmp/app.sock");
  REQUIRE_EQ((void *)host, NULL);
  REQUIRE_EQ((void *)auth, NULL);
  free(unix_path);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, http_unix_scheme_no_path_defaults_to_root) {
  bool is_unix = false;
  char *pq = NULL, *origin_key = NULL, *unix_path = NULL;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock", NULL, NULL, NULL, NULL, &pq, &origin_key,
      NULL, &is_unix, &unix_path);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(is_unix);
  REQUIRE_STREQ(unix_path, "/tmp/app.sock");
  REQUIRE_STREQ(pq, "/");
  free(unix_path);
  free(pq);
  free(origin_key);
}

TEST(url_parsing, https_unix_scheme_rejected) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "https+unix://%2Ftmp%2Fapp.sock/api", NULL, NULL, NULL, NULL, NULL, NULL,
      NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, http_unix_scheme_empty_path_rejected) {
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http+unix:///api", NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

TEST(url_parsing, http_unix_scheme_path_with_embedded_crlf_is_invalid) {
  /* The same concern about injection into the request line as in
   * path_with_embedded_crlf_is_invalid above, reached here through the
   * path_and_query that the "http+unix://" scheme builds, in a separate code
   * path (_parse_chttp_unix_url) that _parse_chttp_url does not share. */
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http+unix://%2Ftmp%2Fapp.sock/api\r\nX-Injected:1", NULL, NULL, NULL,
      NULL, NULL, NULL, NULL, NULL, NULL);
  REQUIRE_EQ(rv, ccol_http_invalid_url);
}

/* ========================================================================== */
/*         CHTTPCLI HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)          */
/* ========================================================================== */

/*
 * chttpcli is a value handle with a generation tag: it holds a slot index
 * and a generation, and a slot table that the library owns resolves it
 * before anything touches the struct chttpclient pointer under it. See the
 * "CHTTPCLI HANDLE SLOT TABLE" section of src/chttpclient.c.
 *
 * This section tests that design directly by pinning three properties. A
 * second destroy must be a ccol_fatal_err (an abort() and a SIGABRT), both
 * for two destroys at the same time and for two destroys one after the
 * other, and never a use-after-free or a double free. A destroy that runs
 * at the same time must not free a handle that a caller has resolved and
 * not yet pinned for its tier. And a real reuse of a slot must never look
 * like a stale handle to whatever held that slot before.
 */

/* This runs chttpclient_destroy(h) on a detached background thread and
 * polls for completion, instead of calling destroy directly from the test
 * thread. A regression in the pin and unpin discipline can leave a resolve
 * pinned for ever on some exit path, so that __chttpclient_destroy blocks
 * for ever in its own ccol_cond_var_wait, and a direct call here would then
 * hang this whole test binary instead of cleanly failing one test.
 *
 * It gives back true when the destroy finished inside the bound, and false
 * otherwise, which means it detected a real hang. On the false path it
 * never frees the watchdog argument on the heap, because the background
 * thread can still touch that argument at any later time; that is a small,
 * deliberate leak that only the artificial-regression path reaches. */
typedef struct {
  chttpcli h;
  atomic_bool done;
} destroy_watchdog_arg_t;

static void *destroy_watchdog_thread(void *arg) {
  destroy_watchdog_arg_t *a = (destroy_watchdog_arg_t *)arg;
  chttpclient_destroy(a->h);
  atomic_store(&a->done, true);
  return NULL;
}

static bool destroy_completes_promptly(chttpcli h) {
  destroy_watchdog_arg_t *warg =
      (destroy_watchdog_arg_t *)malloc(sizeof(*warg));
  if (!warg) return false;
  warg->h = h;
  atomic_store(&warg->done, false);
  pthread_t tid;
  if (pthread_create(&tid, NULL, destroy_watchdog_thread, warg) != 0) {
    free(warg);
    return false;
  }
  pthread_detach(tid);
  for (int i = 0; i < 300; i++) { /* up to about 3s */
    if (atomic_load(&warg->done)) {
      free(warg);
      return true;
    }
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 10000000}; /* 10 ms */
    nanosleep(&ts, NULL);
  }
  return false; /* warg leaks on purpose; see the comment above */
}

/* After a destroy has completed in full, a second destroy call that comes
 * later, on a separate copy of the same original handle value, must be a
 * fatal error. This is what the generation-tagged slot table buys over a
 * claim registry, which can catch only two destroys that overlap in time,
 * not two that run one after the other, as here. This test runs in a
 * forked child, because ccol_fatal_err stops the whole process;
 * tests/clogger/tests.c sets the same precedent for a misuse that stops the
 * process. */
/* The last step of the creation of a client publishes the handle into the
 * pin index, and that step can fail: the index allocates a chunk and a
 * stripe block on the first use of an index, with a plain calloc that no
 * allocator of the caller reaches. The rollback that runs on that failure
 * has two jobs: it must clear the record that the client keeps of the
 * handle, and it must put the slot back on the free list.
 * Without that, the next client
 * that takes that slot inherits a handle that names somebody else. Without
 * the hook below, that path needs a real out-of-memory condition, so an
 * ordinary test run cannot reach it. */
extern void _ccol_pintable_force_next_publish_failure_for_tests(void);
extern size_t _chttpcli_pin_count_for_tests(chttpcli h);

TEST(chttpcli_handle_lifecycle, handle_publish_failure_rolls_the_slot_back) {
  /* This test repeats and asserts on how much the table grows over the whole
   * run, because one cycle cannot tell a rollback apart from no rollback:
   * one lost slot makes the next client grow the table by one, which looks
   * the same as a table that had no free slot to start with. Over CYCLES
   * rounds, a rollback that works grows the table by nothing at all, while
   * a missing push onto the free list grows it by one for each round. */
  enum { CYCLES = 8 };
  size_t before = _chttpcli_slot_table_capacity_for_tests();
  size_t free_before = _chttpcli_free_index_count_for_tests();

  bool all_failed = true, all_created = true;
  for (int i = 0; i < CYCLES; i++) {
    _ccol_pintable_force_next_publish_failure_for_tests();
    chttpcli bad = ccol_create_chttpclient(NULL);
    if (bad != CHTTPCLI_INVALID) {
      all_failed = false;
      __chttpclient_destroy(bad);
      break;
    }
    chttpcli good = ccol_create_chttpclient(NULL);
    if (good == CHTTPCLI_INVALID) {
      all_created = false;
      break;
    }
    __chttpclient_destroy(good);
  }

  _ccol_pintable_force_next_publish_failure_for_tests();
  char *err = NULL;
  chttpcli failed = ccol_create_chttpclient(&err);
  /* This code captures the result instead of asserting here, because the
     forced failure may not have applied: `failed` is then a live client,
     and a return now would leak it, and everything below it. */
  bool forced_failure_applied = (failed == CHTTPCLI_INVALID);
  if (!forced_failure_applied) __chttpclient_destroy(failed);

  /* The slot went back on the free list, so the next client takes it again,
   * and the table does not grow for the failed attempt. That client must
   * also be fully usable, which the pin count below checks. The rollback
   * path also clears the record that the client keeps of the handle, which
   * is defence in depth that this test cannot detect, because the failed
   * create frees that struct, so the next client is a fresh allocation
   * either way. */
  char url[128];
  make_url(url, sizeof(url), "/get");
  chttpcli_construct(cli);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  bool req_built = (req != NULL);
  ccol_retval_t rv = ccol_unexpected_failure;
  bool pins_measured = false;
  size_t pins = 0;
  if (req_built) {
    chttpcli_response *resp = NULL;
    rv = chttpclient_do(cli, req, &resp);
    if (resp) chttpclient_resp_free(resp);
    chttp_request_free(req);
    pins = _chttpcli_pin_count_for_tests(cli);
    pins_measured = true;
  }
  size_t after = _chttpcli_slot_table_capacity_for_tests();
  size_t free_after = _chttpcli_free_index_count_for_tests();

  /* This is the only assertion before the destroy, because a leaked pin would
   * make that destroy never return; the code checks everything else after
   * it. The check runs only when something really took the count, so a
   * request that the code could not build fails on its own check below,
   * with the client still destroyed, instead of being reported as a leaked
   * pin and skipping the destroy. */
  if (pins_measured && pins != 0) {
    REQUIRE_EQ(pins, (size_t)0);
    return;
  }
  chttpclient_destroy(cli);

  REQUIRE_TRUE(forced_failure_applied);
  REQUIRE_TRUE(req_built);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(all_failed);
  REQUIRE_TRUE(all_created);
  /* Growth alone is not enough, because a lost slot grows the table only
     while the free list is empty, and earlier tests can leave several free
     indexes behind, so such a run silently absorbs every loss. The free
     list must therefore also come back to where it started, however deep
     the list was. */
  REQUIRE_LE(after, before + 2);
  REQUIRE_GE(free_after + 2, free_before);
}

TEST(chttpcli_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpcli cli = ccol_create_chttpclient(NULL);
    if (cli == CHTTPCLI_INVALID) _exit(2);
    chttpcli stale = cli;         /* a separate copy of the handle value. The
                macro below sets the local to CHTTPCLI_INVALID, and this copy is
                not that local. A real program that holds a second copy
                elsewhere has exactly this. */
    chttpclient_destroy(cli);     /* this completes in the normal way. The local
            `cli` is now CHTTPCLI_INVALID, and `stale` still holds the original
            value. */
    __chttpclient_destroy(stale); /* the misuse under test. This is a second
        destroy of a handle that is already fully torn down, and it comes
        after the first one. */
    _exit(0); /* nothing reaches this when ccol_fatal_err() aborts. */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  chttpcli h;
} concurrent_destroy_arg_t;

static void *concurrent_destroy_thread(void *arg) {
  concurrent_destroy_arg_t *a = (concurrent_destroy_arg_t *)arg;
  __chttpclient_destroy(a->h);
  return NULL;
}

/* When two threads call destroy on two separate copies of the SAME handle,
 * which is still valid, as close to the same moment as possible, that must also
 * be fatal. Without the generation check of the slot table, this is a real
 * double free on the heap, which valgrind reports as one. */
TEST(chttpcli_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpcli cli = ccol_create_chttpclient(NULL);
    if (cli == CHTTPCLI_INVALID) _exit(2);
    concurrent_destroy_arg_t a1 = {.h = cli};
    concurrent_destroy_arg_t a2 = {.h = cli};
    pthread_t t1, t2;
    /* This code checks the result itself instead of using a REQUIRE_* macro,
     * because it runs inside the forked child, where an early return skips
     * _exit() and falls back into the test-running loop of the harness a
     * second time. A failed pthread_create also leaves t1 and t2 as
     * garbage, and feeding such a value into the pthread_join below is
     * undefined behavior that can hang this child, and with it the waitpid
     * of the parent below. _exit(3) gives a clean failure instead, with an
     * exit code that is easy to tell apart from a SIGABRT; this child
     * already uses one like it for the cli == CHTTPCLI_INVALID
     * precondition above. The WIFSIGNALED and WTERMSIG checks of the
     * parent below report it as a clean test failure rather than as a
     * hang. */
    int t1_rv = pthread_create(&t1, NULL, concurrent_destroy_thread, &a1);
    int t2_rv = (t1_rv == 0)
                    ? pthread_create(&t2, NULL, concurrent_destroy_thread, &a2)
                    : -1;
    if (t1_rv != 0 || t2_rv != 0) _exit(3);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* nothing reaches this. One of the two destroy calls loses
                  the race, and it must reach ccol_fatal_err(). */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* This test races a slow, blocking chttpclient_do call against a
 * chttpclient_destroy on the same handle, at the same time. That call keeps
 * a struct chttpclient pointer resolved and pinned for the whole request,
 * through pending_resolve_count and in_flight_count, so the destroy must
 * block until the call in flight completes, and that call must itself
 * complete and succeed without touching freed memory. A resolve step that
 * only checks the handle and gives the pointer back, with no pin, fails
 * exactly here. */
TEST(chttpcli_handle_lifecycle, resolve_then_use_race_destroy_waits) {
  char slow_url[128];
  make_url(slow_url, sizeof(slow_url), "/slow");

  chttpcli_construct(cli);
  REQUIRE_EQ(chttpclient_set_pool_size(cli, 1), ccol_success);

  atomic_store(&g_slow_started, 0);
  concurrent_req_arg_t occupant = {
      .cli = cli, .result_status = 0, .result_rv = ccol_unexpected_failure};
  memcpy(occupant.url, slow_url, sizeof(slow_url));
  pthread_t occupant_thread;
  REQUIRE_EQ(
      pthread_create(&occupant_thread, NULL, concurrent_req_thread, &occupant),
      0);

  /* This waits until the occupant is confirmed to be in flight (the server
   * has started to handle /slow by then), and only then fires the destroy,
   * so that the request in flight and the destroy call below overlap as
   * much as possible. */
  while (atomic_load(&g_slow_started) < 1) {
    struct timespec ts = {.tv_sec = 0, .tv_nsec = 1000000}; /* 1 ms */
    nanosleep(&ts, NULL);
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpclient_destroy(cli); /* this must block until the chttpclient_do call
                                of the occupant above completes in full */
  clock_gettime(CLOCK_MONOTONIC, &t1);
  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  pthread_join(occupant_thread, NULL);
  REQUIRE_EQ(occupant.result_rv, ccol_success);
  REQUIRE_EQ(occupant.result_status, 200);
  /* /slow sleeps about 100ms on the server, so a destroy that returns well
   * below that did NOT wait for the call in flight, which means the
   * resolve-then-use protection failed to pin it. */
  REQUIRE_GT(elapsed_ms, 50);
}

/* Resolve and unpin must stay exactly balanced. An unpin that is skipped,
 * or that releases against the wrong slot, leaves a pin out for ever, and
 * nothing fails at that moment: the damage appears later, as a
 * chttpclient_destroy that never returns. An assertion on the count turns
 * that silent, late hang into an immediate, local failure.
 *
 * This test is not vacuous: remove the ccol_pintable_unpin call from
 * _chttpcli_resolve_unpin and it fails here, instead of hanging in a later
 * destroy. */
TEST(chttpcli_handle_lifecycle, resolve_and_unpin_leave_no_outstanding_pin) {
  char url[128];
  make_url(url, sizeof(url), "/hello");

  chttpcli_construct(cli);

  /* This test captures every outcome into a local, and every assertion waits
   * until after the destroy of the client, because a REQUIRE_* that fires
   * returns from this function at once, and a client that stays alive here
   * keeps any_slot_in_use true at exit, which stops the teardown of the slot
   * table and of the pin index and buries the real failure under leak
   * reports. */
  size_t pins_fresh = _chttpcli_pin_count_for_tests(cli);

  /* A setter for a configuration value, which resolves, pins, does its work
   * and unpins. */
  ccol_retval_t set_rv = chttpclient_set_pool_size(cli, 2);
  size_t pins_after_set = _chttpcli_pin_count_for_tests(cli);

  /* A full request, which hands off to in_flight_count and back again. */
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  bool req_built = (req != NULL);
  ccol_retval_t rv = ccol_unexpected_failure;
  size_t pins_after_req = 0;
  if (req_built) {
    chttpcli_response *resp = NULL;
    rv = chttpclient_do(cli, req, &resp);
    if (resp) chttpclient_resp_free(resp);
    chttp_request_free(req);
    pins_after_req = _chttpcli_pin_count_for_tests(cli);
  }

  /* The assertions about the pins must fire BEFORE the destroy, and the
   * other assertions after it. chttpclient_destroy waits for the pin count
   * to reach zero, so a call to it with one pin leaked never returns, and
   * the failure then appears as a job that hangs with no diagnostic instead
   * of as an assertion. A leak of this one client costs less, and only a
   * run that has already failed pays it; every other outcome is still
   * asserted after the cleanup. */
  if (pins_fresh != 0 || pins_after_set != 0 || pins_after_req != 0) {
    REQUIRE_EQ(pins_fresh, (size_t)0);
    REQUIRE_EQ(pins_after_set, (size_t)0);
    REQUIRE_EQ(pins_after_req, (size_t)0);
    return;
  }

  chttpclient_destroy(cli);

  REQUIRE_TRUE(req_built);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
}

typedef struct {
  chttpcli h;
} pool_size_setter_arg_t;

static void *pool_size_setter_thread(void *arg) {
  pool_size_setter_arg_t *a = (pool_size_setter_arg_t *)arg;
  /* This code ignores the return value on purpose: a legal race with a
   * destroy on another thread can make this resolve fail with
   * ccol_invalid_args instead of succeeding, and both outcomes are correct.
   * This thread exists only to make resolve, pin and unpin traffic at the
   * same time as the destroy thread below. */
  chttpclient_set_pool_size(a->h, 4);
  return NULL;
}

/* This test differs from resolve_then_use_race_destroy_waits above instead
 * of repeating it. The slow, blocking call of that test keeps
 * in_flight_count above 0 for the whole race window, so the combined wait
 * predicate of the destroy is always true on its first check there, and
 * that test cannot, by its shape, exercise the path on which the
 * predicate is already false and the code never enters ccol_cond_var_wait
 * at all. That path is a real use-after-free hazard on the heap, inside
 * _chttpcli_resolve_unpin; see the comment of that function in
 * src/chttpclient.c for the whole account.
 *
 * This test needs the opposite shape: a fast entry point that never
 * blocks. chttpclient_set_pool_size resolves, pins, runs a short critical
 * section, unpins and returns, with no blocking I/O at all, and the test
 * races that against a destroy on another thread, repeated under stress,
 * because the failure window is only a handful of instructions wide and
 * does not reproduce reliably in one run with no stress. Each iteration
 * uses a fresh client, so every repetition gets its own separate race, and
 * none of them reuses a handle that something has already destroyed. */
TEST(chttpcli_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 25 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpcli cli = ccol_create_chttpclient(NULL);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);

    pool_size_setter_arg_t setter_arg = {.h = cli};
    concurrent_destroy_arg_t destroy_arg = {.h = cli};
    pthread_t setter_tid, destroy_tid;
    /* This code captures both results of the creation instead of asserting on
     * them at once. The SECOND pthread_create can fail here, and an
     * immediate REQUIRE_EQ then returns from this function while the
     * setter_tid that already exists still runs and uses setter_arg, which
     * lives on this stack; setter_arg holds `cli` by value, and the thread
     * function needs setter_arg to stay alive for its own duration, so a
     * return there is a stack-use-after-return. This code therefore joins
     * only what it really created, before it asserts. */
    int setter_rv =
        pthread_create(&setter_tid, NULL, pool_size_setter_thread, &setter_arg);
    int destroy_rv = 0;
    if (setter_rv == 0) {
      destroy_rv = pthread_create(&destroy_tid, NULL, concurrent_destroy_thread,
                                  &destroy_arg);
    }
    if (setter_rv == 0) pthread_join(setter_tid, NULL);
    if (setter_rv == 0 && destroy_rv == 0) pthread_join(destroy_tid, NULL);
    REQUIRE_EQ(setter_rv, 0);
    REQUIRE_EQ(destroy_rv, 0);
  }
}

/* A real reuse of a slot must never look like a stale handle to whatever
 * held that slot before, and the address alone cannot tell the two apart:
 * the tcache of glibc often (though not always) gives the exact address of
 * a struct chttpclient that it just freed to the very next one that the
 * code allocates. The generation counter exists to tell the two apart. */
TEST(chttpcli_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpcli a = ccol_create_chttpclient(NULL);
  REQUIRE_NE(a, CHTTPCLI_INVALID);
  chttpcli stale_a = a;
  chttpclient_destroy(a);

  chttpcli b = ccol_create_chttpclient(NULL);
  REQUIRE_NE(b, CHTTPCLI_INVALID);

  /* The operations of B must succeed in the normal way, whether or not the
   * allocator gave the exact address of A to B. */
  REQUIRE_EQ(chttpclient_set_pool_size(b, 4), ccol_success);

  /* The stale handle of A must never resolve to B, even when B reused the
   * same address under it; that is the whole point of the generation
   * counter. */
  REQUIRE_EQ((void *)_chttpcli_resolve_for_tests(stale_a), NULL);

  chttpclient_destroy(b);
}

/* The slot table is bounded instead of growing for ever: a churn loop of
 * creates and destroys that holds only one slot at a time must reuse that
 * one freed slot on every iteration, without growing the table.
 *
 * This test reads the capacity directly after the first create and destroy
 * pair, instead of asserting a fixed value such as 1, because earlier tests
 * in this same process can already have grown the table to some N above 1.
 * This test only has to prove that ITS OWN churn adds no more growth,
 * whatever the absolute size of the table is when it runs. */
TEST(chttpcli_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 25 };

  chttpcli cli0 = ccol_create_chttpclient(NULL);
  REQUIRE_NE(cli0, CHTTPCLI_INVALID);
  chttpclient_destroy(cli0);
  size_t capacity_after_first = _chttpcli_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpcli cli = ccol_create_chttpclient(NULL);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    chttpclient_destroy(cli);
  }

  REQUIRE_EQ(_chttpcli_slot_table_capacity_for_tests(), capacity_after_first);
}

/* Every exit path of chttpclient_set_tls must release its pin, and a
 * generic template of "resolve and pin, do the body, unpin" covers only one
 * of them. This function has four separate exit points: the !tls branch,
 * the normal success path, and the oom: label, which three different
 * checks for a failed strdup reach with a goto; the third and fourth exit
 * points are the same return statement.
 *
 * A missed unpin on any one of them is silent: pending_resolve_count never
 * returns to zero for that client, so a later chttpclient_destroy call
 * against it hangs for ever instead of crashing.
 * destroy_completes_promptly detects that without hanging this whole test
 * binary. */
TEST(tls, set_tls_all_exit_paths_release_pin) {
  /* Exit 1 of 4: the !tls branch, which restores the defaults. */
  {
    chttpcli cli = ccol_create_chttpclient(NULL);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }

  /* Exit 2 of 4: the normal success path, with a real, non-empty
   * configuration. */
  {
    chttpcli cli = ccol_create_chttpclient(NULL);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
    tls.ca_bundle_path = "/nonexistent/ca-bundle.pem"; /* set_tls itself
        never reads this. The client checks it at request time. */
    REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }

  /* Exits 3 and 4 of 4, which are the same oom: label, reached by an
   * injected strdup failure on the first copy of an owned path, which is
   * cert_path. */
  {
    char *cerr = NULL;
    chttpcli cli = ccol_create_chttpclient_mp(&g_hop_fail_mp, &cerr);
    REQUIRE_NE(cli, CHTTPCLI_INVALID);
    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = 0;
    chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
    tls.cert_path = "/nonexistent/cert.pem";
    tls.key_path = "/nonexistent/key.pem";
    REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_not_enough_memory);
    g_hop_fail_at_call = -1;
    REQUIRE_TRUE(destroy_completes_promptly(cli));
  }
}

/* ========================================================================== */
/*                REAL TLS HANDSHAKE COVERAGE FOR THE ASYNC ENGINE            */
/*                                                                            */
/* The TLS tests of async_step_a above cover only the paths that fail. Those */
/* are a refused connection, and a handshake against a server that speaks no */
/* TLS. A handshake that succeeds needs a valid certificate and key pair, and*/
/* a peer that really speaks TLS. This section builds a real, throwaway,     */
/* self-signed certificate and key pair with the `openssl` CLI at startup. It*/
/* then runs a small mock TLS server, written by hand on raw OpenSSL calls:  */
/* SSL_accept, SSL_read and SSL_write, and NOT chttpserver.c. That server    */
/* drives chttpclient through a real HTTPS request from end to end, both     */
/* synchronously on Tier 1 and through the async engine on Tier 2.           */
/*                                                                            */
/* The mock server is written by hand, in place of a reuse of chttpserver.c, */
/* only for simplicity. That keeps this section, which is about the TLS      */
/* handshake, free of the routing and dispatch machinery of chttpserver. No  */
/* restriction for each process forces this. chttpserver and the async engine*/
/* of chttpclient each own a fully independent reactor, and each one starts  */
/* and stops on its own. A process can therefore run both at once.           */
/* ========================================================================== */

#define TLS_TEST_BODY "{\"status\":\"ok\"}"
/* The size of the single TLS record that /one-record-held-open sends: more
 * than one read of either tier takes at a time, and less than the 16 KB
 * that one record can carry. */
#define ONE_RECORD_TOTAL 12000
/* How long /one-record-held-open keeps its end open for the client to
 * close. */
#define ONE_RECORD_HOLD_MS 8000
/* Set to end the hold of /early-reject-hold. */
static atomic_int g_tls_early_release = 0;
static atomic_int g_one_record_closed_by_client = 0;
static atomic_int g_one_record_hold_expired = 0;

static SSL_CTX *g_tls_ssl_ctx = NULL;
/* This field is an atomic_int rather than a plain int, because the
 * teardown in _stop_tls_server writes it while the accept() of
 * _tls_accept_loop reads the same field on its way out, and those two race.
 * g_srv.server_fd handles exactly the same shape for the plain-HTTP mock
 * server of this file; the result is harmless, but ThreadSanitizer reports
 * it. See the comment on that field. */
static atomic_int g_tls_srv_fd = -1;
static int g_tls_srv_port = 0;
static pthread_t g_tls_accept_tid;
static atomic_int g_tls_srv_running = 0;
static char g_tls_cert_dir[256];
static char g_tls_cert_path[320];
static char g_tls_key_path[320];
static bool g_tls_cert_ready = false;
/* The count of connections that this mock TLS server has accepted, which a
 * test uses to tell a reused keep-alive connection from a fresh one that
 * ran its own handshake. */
static atomic_int g_tls_accept_count = 0;

static int tls_server_accept_count(void) {
  return atomic_load(&g_tls_accept_count);
}

/* A registry of the connection threads, all of which _tls_teardown joins
 * before it frees g_tls_ssl_ctx; without that, it would free the context
 * under an SSL_accept that still runs. This registry is separate from
 * g_conn_threads, g_conn_thread_count and g_conn_mutex in this file, which
 * belong to the plain-HTTP mock server, because the two mock servers are
 * separate listeners whose connection threads have separate lives too. */
#define MAX_TLS_CONN_THREADS 64
static pthread_t g_tls_conn_threads[MAX_TLS_CONN_THREADS];
static int g_tls_conn_thread_count = 0;
static pthread_mutex_t g_tls_conn_mutex = PTHREAD_MUTEX_INITIALIZER;

/* The TLS counterpart of register_conn_thread: when it can, it reaps a
 * connection thread that has already finished before it registers a new
 * one, so that this registry does not only grow, since the TLS mock server
 * also starts once for the whole process and keeps accepting
 * connections. */
static void register_tls_conn_thread(pthread_t tid) {
  pthread_mutex_lock(&g_tls_conn_mutex);
  int kept = 0;
  for (int i = 0; i < g_tls_conn_thread_count; i++) {
    if (test_tryjoin(g_tls_conn_threads[i]) != 0) {
      g_tls_conn_threads[kept++] = g_tls_conn_threads[i];
    }
  }
  g_tls_conn_thread_count = kept;

  if (g_tls_conn_thread_count < MAX_TLS_CONN_THREADS) {
    g_tls_conn_threads[g_tls_conn_thread_count++] = tid;
  } else {
    pthread_detach(tid);
  }
  pthread_mutex_unlock(&g_tls_conn_mutex);
}

/* This builds a throwaway self-signed certificate and key pair with the
 * openssl CLI and puts them into a fresh mkdtemp() directory, as
 * tests/chttpserver/tests_tls.c does. It gives back 0 on success and -1 on
 * any failure, which a caller must read as "this environment could not
 * check the TLS integration" instead of crashing on it. */
static int _openssl_selfsigned(const char *key_path, const char *cert_path,
                               const char *cn, const char *san) {
  char cmd[1024];
  int cn_len;
  if (san) {
    cn_len = snprintf(cmd, sizeof(cmd),
                      "openssl req -x509 -newkey rsa:2048 -nodes "
                      "-keyout '%s' -out '%s' -days 1 -subj '/CN=%s' "
                      "-addext 'subjectAltName=%s' >/dev/null 2>&1",
                      key_path, cert_path, cn, san);
  } else {
    cn_len = snprintf(cmd, sizeof(cmd),
                      "openssl req -x509 -newkey rsa:2048 -nodes "
                      "-keyout '%s' -out '%s' -days 1 -subj '/CN=%s' "
                      ">/dev/null 2>&1",
                      key_path, cert_path, cn);
  }
  if (cn_len < 0 || (size_t)cn_len >= sizeof(cmd)) return -1;
  if (system(cmd) != 0) return -1;
  if (access(cert_path, R_OK) != 0 || access(key_path, R_OK) != 0) return -1;
  return 0;
}

static int _generate_self_signed_cert(void) {
  snprintf(g_tls_cert_dir, sizeof(g_tls_cert_dir),
           "/tmp/chttpclient_tls_test_XXXXXX");
  if (!mkdtemp(g_tls_cert_dir)) return -1;

  int dn = snprintf(g_tls_cert_path, sizeof(g_tls_cert_path), "%s/cert.pem",
                    g_tls_cert_dir);
  int kn = snprintf(g_tls_key_path, sizeof(g_tls_key_path), "%s/key.pem",
                    g_tls_cert_dir);
  if (dn < 0 || (size_t)dn >= sizeof(g_tls_cert_path) || kn < 0 ||
      (size_t)kn >= sizeof(g_tls_key_path))
    return -1;

  /* subjectAltName=IP:127.0.0.1 makes a real certificate for an IP address,
   * which exercises the X509_check_ip path on the connect side, reached by
   * X509_VERIFY_PARAM_set1_ip_asc for a target that is an IP literal,
   * rather than the older fallback that matches the CN. */
  return _openssl_selfsigned(g_tls_key_path, g_tls_cert_path, "127.0.0.1",
                             "IP:127.0.0.1");
}

static void _remove_generated_cert(void) {
  if (g_tls_cert_path[0]) unlink(g_tls_cert_path);
  if (g_tls_key_path[0]) unlink(g_tls_key_path);
  if (g_tls_cert_dir[0]) rmdir(g_tls_cert_dir);
}

/* This reads one HTTP/1.1 request off ssl, up to the blank line at the
 * end. It reads the headers alone, because a request in this section never
 * sends a body, and it ignores the content: every route below answers in
 * the same way, whatever the request asked for, except /large, whose route
 * comes from the path. */
static void _tls_read_request(SSL *ssl, char *buf, size_t max, char *path,
                              size_t path_max) {
  size_t total = 0;
  buf[0] = '\0';
  while (total < max - 1) {
    int n = SSL_read(ssl, buf + total, (int)(max - 1 - total));
    if (n <= 0) break;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n")) break;
  }
  path[0] = '\0';
  const char *sp1 = strchr(buf, ' ');
  if (sp1) {
    const char *sp2 = strchr(sp1 + 1, ' ');
    if (sp2 && (size_t)(sp2 - sp1 - 1) < path_max) {
      memcpy(path, sp1 + 1, (size_t)(sp2 - sp1 - 1));
      path[sp2 - sp1 - 1] = '\0';
    }
  }
}

static void _tls_send_response(SSL *ssl, int status, const char *status_text,
                               const char *body, size_t body_len) {
  char header[256];
  int hlen = snprintf(header, sizeof(header),
                      "HTTP/1.1 %d %s\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      status, status_text, body_len);
  if (hlen > 0) SSL_write(ssl, header, hlen);
  if (body && body_len > 0) SSL_write(ssl, body, (int)body_len);
}

/*
 * TLS 1.3 lets either peer send a KeyUpdate at any point after the
 * handshake completes. It carries no application bytes, and the OpenSSL
 * that receives it consumes it inside its own SSL_read and then reports
 * "nothing available", so the message is invisible above the TLS layer.
 * But it is real bytes on the socket, so it is real read readiness for
 * anything that polls that fd. SSL_KEY_UPDATE_NOT_REQUESTED asks the peer
 * for no reply of its own, so a peer that only watches for readability
 * never has to write anything back to stay correct.
 *
 * That combination lets _tls_wait_readable_poking below give a waiting
 * client real read readiness while saying nothing in HTTP. This function
 * does nothing on a connection that negotiated TLS 1.2, where the message
 * does not exist.
 */
static void _tls_poke_key_update(SSL *ssl) {
  if (SSL_version(ssl) != TLS1_3_VERSION) return;
  if (SSL_key_update(ssl, SSL_KEY_UPDATE_NOT_REQUESTED) != 1) return;
  /* The call above queues the message, and SSL_do_handshake is what flushes
   * it now; without it, the flush waits for the next application write,
   * which may never come. */
  SSL_do_handshake(ssl);
}

/*
 * This waits for the next request of the peer on a keep-alive connection,
 * sending one KeyUpdate poke for each POKE_INTERVAL_MS that passes with
 * nothing arriving. It gives back true once there is something to read,
 * and false once the poke budget runs out, which tells the caller to close
 * this end.
 *
 * The poke repeats, because one poke is not enough, and that is what makes
 * it useful for synchronisation: the peer can be doing anything at all
 * when any one poke lands, and the poke that matters is the one that
 * arrives after the peer starts to watch for readability.
 */
static bool _tls_wait_readable_poking(SSL *ssl, int fd) {
  enum { POKE_INTERVAL_MS = 20, MAX_POKES = 250 };
  for (int i = 0; i < MAX_POKES; i++) {
    if (SSL_pending(ssl) > 0) return true;
    struct pollfd pfd = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&pfd, 1, POKE_INTERVAL_MS);
    if (rc < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    if (rc > 0) return true;
    _tls_poke_key_update(ssl);
  }
  return false;
}

/*
 * This serves requests on one connection that has already run its
 * handshake, and stops when the peer stops sending or when the bounded wait
 * below gives up, so that a test can see whether a second request reused
 * the connection (the accept count does not move) or ran a new handshake.
 *
 * poke_while_idle picks between the two keep-alive routes: /ka blocks on
 * the next request under a receive timeout, while /ka-poke also pokes (see
 * _tls_wait_readable_poking), so that a peer that watches this connection
 * for readability, instead of sending, still gets events.
 */
static void _tls_keepalive_loop(SSL *ssl, int fd, bool poke_while_idle) {
  struct timeval ka_rcvto = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &ka_rcvto, sizeof(ka_rcvto));
  for (;;) {
    char hdr[256];
    int hl = snprintf(hdr, sizeof(hdr),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: application/json\r\n"
                      "Content-Length: %zu\r\n"
                      "Connection: keep-alive\r\n"
                      "\r\n",
                      strlen(TLS_TEST_BODY));
    if (hl <= 0 || SSL_write(ssl, hdr, hl) <= 0) break;
    if (SSL_write(ssl, TLS_TEST_BODY, (int)strlen(TLS_TEST_BODY)) <= 0) break;
    if (poke_while_idle && !_tls_wait_readable_poking(ssl, fd)) break;
    char nbuf[8192];
    char npath[256];
    _tls_read_request(ssl, nbuf, sizeof(nbuf), npath, sizeof(npath));
    if (nbuf[0] == '\0') break;
  }
}

/* This reads and drops what arrives on fd until the peer closes, for at
 * most two seconds and 1 MiB, so that a reset that discards an answer which
 * the peer has not read yet can only follow a peer that kept sending that
 * much after the answer arrived. */
static void ers_discard(int fd, char *buf, size_t buf_size) {
  struct timeval tv = {.tv_sec = 0, .tv_usec = 100000};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct timespec t0, now;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  size_t got = 0;
  while (got < 1024u * 1024u) {
    ssize_t r = recv(fd, buf, buf_size, 0);
    if (r == 0 ||
        (r < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR))
      break;
    if (r > 0) got += (size_t)r;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - t0.tv_sec >= 2) break;
  }
}

static void *_tls_conn_thread(void *arg) {
  int fd = (int)(intptr_t)arg;
  atomic_fetch_add(&g_tls_accept_count, 1);
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
  if (!ssl) {
    close(fd);
    return NULL;
  }
  SSL_set_fd(ssl, fd);

  if (SSL_accept(ssl) <= 0) {
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  char buf[8192];
  char path[256];
  _tls_read_request(ssl, buf, sizeof(buf), path, sizeof(path));

  if (strcmp(path, "/ka") == 0 || strcmp(path, "/ka-poke") == 0) {
    /* These are the only two keep-alive routes of this mock TLS server; every
     * other route answers with "Connection: close", so a pooled HTTPS
     * connection has nowhere else to come from. */
    _tls_keepalive_loop(ssl, fd, strcmp(path, "/ka-poke") == 0);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/one-record-held-open") == 0) {
    /* The whole response goes out in ONE SSL_write, which is one TLS record of
     * ONE_RECORD_TOTAL bytes, more than the first read of the client takes.
     * The response says "Connection: close", so the client never reuses the
     * connection, but this end keeps it open until the client closes it,
     * with nothing more on the socket, so the rest of the record is only
     * inside the TLS layer of the client. A client that waits for the
     * socket before it reads again stalls until the peer closes. */
    char rec[ONE_RECORD_TOTAL];
    int hl = snprintf(rec, sizeof(rec),
                      "HTTP/1.1 200 OK\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %06d\r\n"
                      "Connection: close\r\n"
                      "\r\n",
                      0);
    size_t body_len = sizeof(rec) - (size_t)hl;
    snprintf(rec, sizeof(rec),
             "HTTP/1.1 200 OK\r\n"
             "Content-Type: text/plain\r\n"
             "Content-Length: %06zu\r\n"
             "Connection: close\r\n"
             "\r\n",
             body_len);
    memset(rec + hl, 'y', body_len);
    SSL_write(ssl, rec, (int)sizeof(rec));
    /* The first thing that the socket reports is the close of the client,
     * once it has the whole response; a client that never reads the rest of
     * the record never closes, and the hold runs out instead. */
    struct pollfd hp = {.fd = fd, .events = POLLIN, .revents = 0};
    int prc;
    do {
      prc = poll(&hp, 1, ONE_RECORD_HOLD_MS);
    } while (prc < 0 && errno == EINTR);
    if (prc > 0)
      atomic_fetch_add(&g_one_record_closed_by_client, 1);
    else
      atomic_fetch_add(&g_one_record_hold_expired, 1);
    SSL_shutdown(ssl);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/early-reject-close") == 0) {
    /* 413 once the header block arrived, then a lingering close: shut the
     * write side down, discard what arrives for a while, and close with the
     * rest unread, which resets the connection under the write of a client
     * that is still sending. */
    static const char r[] =
        "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n"
        "Connection: close\r\n\r\ntoo large";
    SSL_write(ssl, r, (int)sizeof(r) - 1);
    shutdown(fd, SHUT_WR);
    ers_discard(fd, buf, sizeof(buf));
    SSL_free(ssl);
    close(fd);
    return NULL;
  }
  if (strcmp(path, "/early-reject-hold") == 0) {
    /* 401 once the header block arrived, then the connection stays open
     * with nothing read until the test releases it. */
    static const char r[] =
        "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\n"
        "unauthorized";
    SSL_write(ssl, r, (int)sizeof(r) - 1);
    for (int i = 0; i < 1500 && !atomic_load(&g_tls_early_release); i++)
      usleep(10000);
    SSL_free(ssl);
    close(fd);
    return NULL;
  }

  if (strcmp(path, "/echo-headers") == 0) {
    /* The body is the request header block exactly as this end read it. */
    _tls_send_response(ssl, 200, "OK", buf, strlen(buf));
  } else if (strcmp(path, "/large") == 0) {
    static char large_body[8192];
    memset(large_body, 'x', sizeof(large_body));
    _tls_send_response(ssl, 200, "OK", large_body, sizeof(large_body));
  } else {
    _tls_send_response(ssl, 200, "OK", TLS_TEST_BODY, strlen(TLS_TEST_BODY));
  }

  /* This gives the peer a bounded chance to finish its read, then close and
   * send its own close_notify, before this end tears down. A full byte
   * count from SSL_write() means only that the bytes went to the send
   * buffer of the kernel, not that the peer received them, so a close
   * directly after that can race the read of the peer: the transfer then
   * aborts, and the response body never arrives in full. That is rare at
   * native speed, but it reproduces under the much heavier scheduling of
   * valgrind.
   *
   * A receive timeout bounds the wait, so a peer that fails to close,
   * either from a bug or because a client keeps the connection open, can
   * never hang this thread for ever, which would also hang the join of
   * every connection thread inside _tls_teardown(). */
  struct timeval rcvto = {.tv_sec = 2, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvto, sizeof(rcvto));
  char drain[64];
  while (SSL_read(ssl, drain, sizeof(drain)) > 0) {
    /* This drains until the peer closes, an error happens, or the timeout
     * above fires. */
  }

  SSL_shutdown(ssl);
  SSL_free(ssl);
  close(fd);
  return NULL;
}

static void *_tls_accept_loop(void *arg) {
  (void)arg;
  while (atomic_load(&g_tls_srv_running)) {
    int fd = accept(g_tls_srv_fd, NULL, NULL);
    if (fd < 0) {
      if (!atomic_load(&g_tls_srv_running)) break;
      continue;
    }
    pthread_t tid;
    if (pthread_create(&tid, NULL, _tls_conn_thread, (void *)(intptr_t)fd) !=
        0) {
      close(fd);
      continue;
    }
    register_tls_conn_thread(tid);
  }
  return NULL;
}

static int _start_tls_server(void) {
  g_tls_srv_fd = socket(AF_INET, SOCK_STREAM, 0);
  if (g_tls_srv_fd < 0) return -1;
  int opt = 1;
  setsockopt(g_tls_srv_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  addr.sin_port = 0;

  if (bind(g_tls_srv_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(g_tls_srv_fd, 64) != 0) {
    close(g_tls_srv_fd);
    g_tls_srv_fd = -1;
    return -1;
  }

  socklen_t len = sizeof(addr);
  getsockname(g_tls_srv_fd, (struct sockaddr *)&addr, &len);
  g_tls_srv_port = ntohs(addr.sin_port);

  atomic_store(&g_tls_srv_running, 1);
  if (pthread_create(&g_tls_accept_tid, NULL, _tls_accept_loop, NULL) != 0) {
    close(g_tls_srv_fd);
    g_tls_srv_fd = -1;
    atomic_store(&g_tls_srv_running, 0);
    return -1;
  }
  return 0;
}

static void _stop_tls_server(void) {
  if (g_tls_srv_fd < 0) return;
  atomic_store(&g_tls_srv_running, 0);
  shutdown(g_tls_srv_fd, SHUT_RDWR);
  close(g_tls_srv_fd);
  g_tls_srv_fd = -1;
  pthread_join(g_tls_accept_tid, NULL);

  pthread_mutex_lock(&g_tls_conn_mutex);
  for (int i = 0; i < g_tls_conn_thread_count; i++) {
    pthread_join(g_tls_conn_threads[i], NULL);
  }
  g_tls_conn_thread_count = 0;
  pthread_mutex_unlock(&g_tls_conn_mutex);
}

static void _tls_teardown(void) {
  _stop_tls_server();
  if (g_tls_ssl_ctx) {
    SSL_CTX_free(g_tls_ssl_ctx);
    g_tls_ssl_ctx = NULL;
  }
  _remove_generated_cert();
}

__attribute__((constructor)) static void _tls_setup(void) {
  if (_generate_self_signed_cert() != 0) {
    fprintf(stderr,
            "WARNING: could not generate a self-signed cert with the openssl "
            "CLI. This environment skips the real TLS handshake tests.\n");
    g_tls_cert_ready = false;
    atexit(_tls_teardown);
    return;
  }

  g_tls_ssl_ctx = SSL_CTX_new(TLS_server_method());
  if (!g_tls_ssl_ctx) {
    fprintf(stderr, "FATAL: SSL_CTX_new failed\n");
    exit(1);
  }
  if (SSL_CTX_use_certificate_file(g_tls_ssl_ctx, g_tls_cert_path,
                                   SSL_FILETYPE_PEM) <= 0 ||
      SSL_CTX_use_PrivateKey_file(g_tls_ssl_ctx, g_tls_key_path,
                                  SSL_FILETYPE_PEM) <= 0) {
    fprintf(stderr, "FATAL: could not load generated cert/key into SSL_CTX\n");
    exit(1);
  }

  if (_start_tls_server() != 0) {
    fprintf(stderr, "FATAL: could not start mock TLS server\n");
    exit(1);
  }
  g_tls_cert_ready = true;
  atexit(_tls_teardown);
}

static void make_tls_url(char *buf, size_t buf_size, const char *path) {
  snprintf(buf, buf_size, "https://127.0.0.1:%d%s", g_tls_srv_port, path);
}

/* The Tier 1 counterparts (the synchronous chttpclient_do) of the async_tls
 * tests below. The connect and handshake code of Tier 1, _conn_open and
 * _tls_handshake, is separate text from the code of Tier 2
 * (_async_tls_advance and others). Without these tests, every "https://"
 * request in this file targets either an unreachable port, which is a test
 * for a refused connection, or a plain-HTTP server, which fails at once on
 * garbage in the handshake, so the synchronous path would have no coverage
 * at all of a real handshake that succeeds, or of a real failure to verify
 * a certificate over a live TLS connection. */
TEST(sync_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);

  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, TLS_TEST_BODY);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(sync_tls, large_body_response_over_tls) {
  /* This exercises more than one call to _tls_handshake and to _conn_read
   * against one TLS response, instead of a single read. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttpclient_do(cli, req, &resp), ccol_success);
  chttp_request_free(req);

  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_destroy(cli);
}

TEST(sync_tls, untrusted_cert_fails_verification) {
  /* Nothing sets ca_bundle_path here, so the client uses the default trust
   * store of the system, which does not, and cannot, trust a throwaway
   * self-signed certificate that somebody just built. This is a real
   * negative proof that the synchronous path really verifies a certificate
   * too, instead of silently skipping that step. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);

  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_EQ((void *)resp, NULL);

  chttpclient_destroy(cli);
}

TEST(async_tls, handshake_succeeds_when_ca_is_trusted) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);

  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_NE((void *)resp->body, NULL);
  REQUIRE_STREQ(resp->body, TLS_TEST_BODY);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, large_body_response_over_tls) {
  /* This exercises more than one call to on_data (coming from repeated
   * ctls_conn_read calls) against one TLS response, instead of a single
   * read. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/large");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  chttpcli_response *resp = raw->resp;
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, (size_t)8192);

  chttpclient_resp_free(resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, untrusted_cert_fails_verification) {
  /* Nothing sets ca_bundle_path here, so the client uses the default trust
   * store of the system, which does not, and cannot, trust a throwaway
   * self-signed certificate that somebody just built. This is a real
   * negative proof that the client really verifies a certificate instead of
   * silently skipping that step. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  ccol_retval_t rv = raw->rv;
  REQUIRE_EQ(rv, ccol_http_tls_cert_verification_failed);
  REQUIRE_EQ((void *)raw->resp, NULL);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(async_tls, concurrent_https_requests_all_succeed) {
  /* More than one HTTPS request at the same time, through the shared async
   * engine, to prove that the reactor multiplexes several TLS handshakes and
   * several encrypted data streams correctly, rather than one at a time. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  enum { N = 6 };
  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/hello");

  ctpool_future *futures[N];
  for (int i = 0; i < N; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    futures[i] = chttpclient_do_async(cli, req);
    REQUIRE_NE((void *)futures[i], NULL);
    chttp_request_free(req);
  }

  for (int i = 0; i < N; i++) {
    chttpcli_async_result_t *raw = chttpclient_async_result_get(futures[i]);
    REQUIRE_NE((void *)raw, NULL);
    REQUIRE_EQ(raw->rv, ccol_success);
    chttpcli_response *resp = raw->resp;
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    chttpclient_resp_free(resp);
    chttpclient_async_result_free(raw);
    ctpool_future_free(futures[i]);
  }

  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

/* ========================================================================== */
/*        TLS I/O DIRECTION, POOLED-CONNECTION TLS POLICY, ENGINE REAPER      */
/* ========================================================================== */

extern void _chttpclient_tls_dir_inject_for_tests(int mode);
extern bool _chttpclient_tls_dir_released_for_tests(void);
extern unsigned _chttpclient_tls_dir_read_interest_while_writing_for_tests(
    void);
extern unsigned _chttpclient_tls_dir_write_interest_while_reading_for_tests(
    void);
extern unsigned _chttpclient_tls_dir_block_attempts_for_tests(void);
extern int _chttpclient_abandon_shutdown_under_lock_for_tests(void);
extern void _chttpclient_reset_abandon_shutdown_probe_for_tests(void);
extern void _chttpclient_force_reaper_spawn_fail_for_tests(bool on);
extern void _chttpclient_hold_next_reaper_spawn_for_tests(void);
extern bool _chttpclient_reaper_spawn_is_held_for_tests(void);
extern void _chttpclient_release_reaper_spawn_for_tests(void);
extern unsigned _chttpclient_reaper_created_count_for_tests(void);
extern unsigned _chttpclient_reaper_joined_count_for_tests(void);
extern unsigned _chttpclient_engine_reaper_abandoned_count_for_tests(void);

#define TLS_DIR_INJECT_OFF 0
#define TLS_DIR_INJECT_WRITE_WANTS_READ 1
#define TLS_DIR_INJECT_READ_WANTS_WRITE 2

/*
 * The ceiling for the warm-up hop: a safety net against a hang, not a
 * measurement. The warm-up runs a full TLS handshake, whose cost belongs to
 * the machine and to whatever instrumentation the suite runs under (under
 * valgrind the handshake takes about a second), so this ceiling must sit
 * far above every one of those costs. Each test sets the deadline that it
 * really needs after the warm-up finishes, and that deadline bounds the
 * measured hop, so an unrelated slowdown in the handshake cannot consume
 * it.
 */
#define TLS_DIR_WARMUP_TIMEOUT_MS 30000

/*
 * SSL_write can need the connection to become READABLE before it can make
 * progress, and SSL_read can need it to become WRITABLE; a TLS 1.2
 * renegotiation and a TLS 1.3 KeyUpdate are two such cases. ctls reports
 * both as -1 with EWOULDBLOCK and publishes the real direction through
 * ctls_conn_wants_write(), so an I/O loop that always waits in its own home
 * direction either spins or waits for a readiness event that never comes.
 *
 * _chttpclient_tls_dir_inject_for_tests reproduces exactly that condition on
 * a live TLS connection, and only one thing releases it: on Tier 1 the
 * client must wait in the direction that the injection names, and on Tier
 * 2 it must register in that direction. That is a transition of state,
 * not a clock, so an implementation that keeps waiting in its home
 * direction never releases it and runs out of deadline instead.
 *
 * Each test arms the injection only AFTER a connection is already in the
 * pool, so that the legal direction changes of the handshake itself cannot
 * release it, and disarms it before any assertion, because the reactor
 * threads of this module keep running between tests.
 */

/* This opens one HTTPS keep-alive connection and puts it into the pool. It
 * gives back false when the environment has no usable certificate or when
 * the first request failed. */
static bool tls_warm_pooled_sync_connection(chttpcli cli, const char *url) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return false;
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  if (rv != ccol_success || !resp) return false;
  bool ok = (resp->status_code == 200);
  chttpclient_resp_free(resp);
  return ok;
}

TEST(tls_io_direction, sync_write_wanting_read_waits_on_the_read_direction) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  /* Warming is bounded only against a hang; see
   * TLS_DIR_WARMUP_TIMEOUT_MS. */
  REQUIRE_EQ(chttpclient_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");
  bool warmed = tls_warm_pooled_sync_connection(cli, url);

  ccol_retval_t rv = ccol_unexpected_failure;
  ccol_retval_t bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  if (warmed) {
    /* From here the deadline bounds the measured hop, which is what it exists
     * to bound: a loop under test that does not release the injection runs
     * out of that deadline. */
    bound_rv = chttpclient_set_request_timeout(cli, 700000);
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_WRITE_WANTS_READ);
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      rv = chttpclient_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) {
        status = resp->status_code;
        chttpclient_resp_free(resp);
      }
    }
    released = _chttpclient_tls_dir_released_for_tests();
    /* This disarms the injection before any assertion below; without that, a
     * REQUIRE_* that returns early leaves the injection armed for every
     * later test. */
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  chttpclient_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  /* Non-vacuous: with the send loop pinned to POLLOUT the injected block is
   * never released, so rv is ccol_timed_out and released is false. */
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, sync_read_wanting_write_waits_on_the_write_direction) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  /* Warming is bounded only against a hang; see
   * TLS_DIR_WARMUP_TIMEOUT_MS. */
  REQUIRE_EQ(chttpclient_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");
  bool warmed = tls_warm_pooled_sync_connection(cli, url);

  ccol_retval_t rv = ccol_unexpected_failure;
  ccol_retval_t bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  if (warmed) {
    /* From here the deadline bounds the measured hop, which is what it exists
     * to bound: a loop under test that does not release the injection runs
     * out of that deadline. */
    bound_rv = chttpclient_set_request_timeout(cli, 700000);
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_READ_WANTS_WRITE);
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    if (req) {
      chttpcli_response *resp = NULL;
      rv = chttpclient_do(cli, req, &resp);
      chttp_request_free(req);
      if (resp) {
        status = resp->status_code;
        chttpclient_resp_free(resp);
      }
    }
    released = _chttpclient_tls_dir_released_for_tests();
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  chttpclient_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  /* Non-vacuous: with the read loop pinned to POLLIN the injected block is
   * never released, so rv is ccol_timed_out and released is false. */
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, async_read_wanting_write_registers_write_interest) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  /* Warming is bounded only against a hang; see
   * TLS_DIR_WARMUP_TIMEOUT_MS. */
  REQUIRE_EQ(chttpclient_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  /* The warm-up hop leaves a connection that has already run its handshake
   * in the async idle pool, so the measured hop below never runs a
   * handshake that the injection could latch onto. */
  bool warmed = false;
  ctpool_future *fw = async_get(cli, url);
  if (fw) {
    chttpcli_async_result_t *rw = chttpclient_async_result_get(fw);
    if (rw) {
      warmed =
          (rw->rv == ccol_success && rw->resp && rw->resp->status_code == 200);
      if (rw->resp) chttpclient_resp_free(rw->resp);
      chttpclient_async_result_free(rw);
    }
    ctpool_future_free(fw);
  }

  ccol_retval_t rv = ccol_unexpected_failure;
  ccol_retval_t bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  unsigned write_interest = 0;
  if (warmed) {
    /* From here the deadline bounds the measured hop, which is what it exists
     * to bound: a loop under test that does not release the injection runs
     * out of that deadline. */
    bound_rv = chttpclient_set_request_timeout(cli, 1500000);
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_READ_WANTS_WRITE);
    ctpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
      if (raw) {
        rv = raw->rv;
        if (raw->resp) {
          status = raw->resp->status_code;
          chttpclient_resp_free(raw->resp);
        }
        chttpclient_async_result_free(raw);
      }
      ctpool_future_free(f);
    }
    released = _chttpclient_tls_dir_released_for_tests();
    write_interest =
        _chttpclient_tls_dir_write_interest_while_reading_for_tests();
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  /* This is not vacuous: with the registration left on the read direction,
   * the socket never reports a readiness that this hop can act on, so
   * write_interest stays 0, released stays false, and rv is
   * ccol_timed_out. */
  REQUIRE_GT(write_interest, 0u);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST(tls_io_direction, async_write_wanting_read_registers_read_interest) {
  /* The counterpart of the test above on the write side, with the same
   * end-to-end shape: the hop must really complete after the registration
   * moves.
   *
   * It runs against /ka-poke instead of /ka, because the two directions are
   * not symmetric here. A read that wants the write direction gets its
   * readiness for free, because an idle socket is writable at once, while a
   * write that wants the read direction gets nothing, because a server with
   * a request still open has nothing to say. /ka-poke supplies exactly what
   * the real condition arrives with, and nothing more: a TLS 1.3 KeyUpdate
   * every few milliseconds while it waits, which is real read readiness
   * that carries no application bytes (see _tls_poke_key_update). A pooled
   * connection that sits idle between the two hops is unaffected by it,
   * because the probe for an idle connection reads through TLS, so it sees
   * the record consumed and nothing available, exactly as on a silent
   * connection.
   *
   * Without the direction flip, the registration stays on the write
   * direction, on a socket that is already writable, so the reactor
   * dispatches it again as fast as it can and refuses the same write again
   * and again: a spin at 100% CPU on a reactor thread that the whole
   * process shares. The assertion on the block count below pins that harm,
   * which the direction flip exists to avoid, while the counters for the
   * direction and for the release pin the decision
   * itself. */
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);
  /* Warming is bounded only against a hang; see
   * TLS_DIR_WARMUP_TIMEOUT_MS. */
  REQUIRE_EQ(chttpclient_set_request_timeout(
                 cli, (uint64_t)TLS_DIR_WARMUP_TIMEOUT_MS * 1000u),
             ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka-poke");

  /* The warm-up hop leaves a connection that has already run its handshake
   * in the async idle pool, so the measured hop below never runs a
   * handshake that the injection could catch. */
  bool warmed = false;
  ctpool_future *fw = async_get(cli, url);
  if (fw) {
    chttpcli_async_result_t *rw = chttpclient_async_result_get(fw);
    if (rw) {
      warmed =
          (rw->rv == ccol_success && rw->resp && rw->resp->status_code == 200);
      if (rw->resp) chttpclient_resp_free(rw->resp);
      chttpclient_async_result_free(rw);
    }
    ctpool_future_free(fw);
  }

  ccol_retval_t rv = ccol_unexpected_failure;
  ccol_retval_t bound_rv = ccol_unexpected_failure;
  int status = 0;
  bool released = false;
  unsigned read_interest = 0;
  unsigned block_attempts = 0;
  /* These two counts bracket the measured hop, which must finish on the
   * connection that it started on. A blocked write that nothing drives
   * again eventually loses the connection, and the hop then retries against
   * a fresh connection that runs its own handshake; that retry also ends in
   * a 200, so the two assertions on rv and status below would then say
   * nothing about the dispatch in the read direction. This bracket
   * separates a recovery from a fresh start. */
  int accepts_before = tls_server_accept_count();
  int accepts_after = accepts_before;
  if (warmed) {
    /* From here the deadline bounds the measured hop, which is what it exists
     * to bound: a loop under test that does not release the injection runs
     * out of that deadline. */
    bound_rv = chttpclient_set_request_timeout(cli, 3000000);
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_WRITE_WANTS_READ);
    ctpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
      if (raw) {
        rv = raw->rv;
        if (raw->resp) {
          status = raw->resp->status_code;
          chttpclient_resp_free(raw->resp);
        }
        chttpclient_async_result_free(raw);
      }
      ctpool_future_free(f);
    }
    released = _chttpclient_tls_dir_released_for_tests();
    read_interest =
        _chttpclient_tls_dir_read_interest_while_writing_for_tests();
    block_attempts = _chttpclient_tls_dir_block_attempts_for_tests();
    accepts_after = tls_server_accept_count();
    /* This disarms the injection before any assertion below; without that, a
     * REQUIRE_* that returns early leaves the injection armed for every
     * later test. */
    _chttpclient_tls_dir_inject_for_tests(TLS_DIR_INJECT_OFF);
  }
  /* The destroy comes before the wait, not after it. This route is
   * keep-alive, so a connection is still in the pool, holding an engine
   * reference, and the destroy is what drains it. A wait first would only
   * run out its own bound with that reference held, and would leave the
   * engine in the middle of its teardown at the exit of the process. */
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_TRUE(warmed);
  REQUIRE_EQ(bound_rv, ccol_success);
  /* This is not vacuous: with the registration left on the write direction,
   * read_interest stays 0 and released stays false, and the code refuses
   * the write once for each new dispatch until the runaway guard of the
   * injection stops refusing, which takes block_attempts far past this
   * bound. A correct flip costs one refusal. */
  REQUIRE_GT(read_interest, 0u);
  REQUIRE_TRUE(released);
  REQUIRE_LE(block_attempts, 8u);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(accepts_after, accepts_before);
}

/*
 * Both idle pools key on the origin alone ("scheme://host:port"), which
 * says nothing about the TLS policy that a pooled connection ran its
 * handshake under. A chttpclient_set_tls call can pin a private CA, turn
 * verification back on, or change the client certificate, so such a
 * call must retire every pooled HTTPS connection; without that, requests
 * keep getting answers over the old configuration for as long as an idle
 * connection lives.
 */
TEST(tls_config_generation, set_tls_retires_pooled_sync_connections) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  int accepts_before = tls_server_accept_count();
  bool first = tls_warm_pooled_sync_connection(cli, url);
  bool second = tls_warm_pooled_sync_connection(cli, url);
  int accepts_after_reuse = tls_server_accept_count();

  /* The same trust material with a different policy for verification: a
   * real, different configuration that a caller can switch to at run
   * time. */
  chttp_tls_config_t tighter = CHTTP_TLS_DEFAULT;
  tighter.ca_bundle_path = g_tls_cert_path;
  tighter.insecure_skip_hostname_check = true;
  ccol_retval_t set_rv = chttpclient_set_tls(cli, &tighter);

  bool third = tls_warm_pooled_sync_connection(cli, url);
  int accepts_after_reconfig = tls_server_accept_count();
  chttpclient_destroy(cli);

  REQUIRE_TRUE(first);
  REQUIRE_TRUE(second);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(third);
  /* The second request proves that the pool really does reuse an HTTPS
   * connection, so the extra accept of the third request comes from the new
   * configuration, not from a pool that never works. */
  REQUIRE_EQ(accepts_after_reuse - accepts_before, 1);
  /* This is not vacuous: without the retirement, the client with the new
   * configuration reuses the connection that ran its handshake under the
   * old policy, and this count stays 1. */
  REQUIRE_EQ(accepts_after_reconfig - accepts_before, 2);
}

TEST(tls_config_generation, set_tls_retires_pooled_async_connections) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }

  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  REQUIRE_EQ(chttpclient_set_tls(cli, &tls), ccol_success);

  char url[160];
  make_tls_url(url, sizeof(url), "/ka");

  int accepts_before = tls_server_accept_count();
  bool ok[3] = {false, false, false};
  for (int i = 0; i < 2; i++) {
    ctpool_future *f = async_get(cli, url);
    if (f) {
      chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
      if (raw) {
        ok[i] = (raw->rv == ccol_success && raw->resp &&
                 raw->resp->status_code == 200);
        if (raw->resp) chttpclient_resp_free(raw->resp);
        chttpclient_async_result_free(raw);
      }
      ctpool_future_free(f);
    }
  }
  int accepts_after_reuse = tls_server_accept_count();

  chttp_tls_config_t tighter = CHTTP_TLS_DEFAULT;
  tighter.ca_bundle_path = g_tls_cert_path;
  tighter.insecure_skip_hostname_check = true;
  ccol_retval_t set_rv = chttpclient_set_tls(cli, &tighter);

  ctpool_future *f3 = async_get(cli, url);
  if (f3) {
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f3);
    if (raw) {
      ok[2] = (raw->rv == ccol_success && raw->resp &&
               raw->resp->status_code == 200);
      if (raw->resp) chttpclient_resp_free(raw->resp);
      chttpclient_async_result_free(raw);
    }
    ctpool_future_free(f3);
  }
  int accepts_after_reconfig = tls_server_accept_count();
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  REQUIRE_TRUE(ok[0]);
  REQUIRE_TRUE(ok[1]);
  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_TRUE(ok[2]);
  REQUIRE_EQ(accepts_after_reuse - accepts_before, 1);
  /* This is not vacuous: without the retirement, Tier 2 skips CONNECTING and
   * TLS_HANDSHAKING for a reused connection, and this count stays 1. */
  REQUIRE_EQ(accepts_after_reconfig - accepts_before, 2);
}

TEST(async_idle_pool, abandoned_reused_ctx_is_shut_down_under_its_idle_lock) {
  /* Two branches abandon a ctx that is still registered and call shutdown(2)
   * on its fd to force the dispatch that does the real teardown: the reused
   * branch of _async_submit_hop_fail and the reactivation-failure branch of
   * _async_submit_hop. That shutdown must happen while the code still holds
   * the idle_lock of the ctx, because the moment it releases that lock, a
   * reactor dispatch that waits in _async_dispatch_kind sees hop_completed,
   * runs the teardown and calls close() on the fd, so a shutdown after that
   * names an fd number that the kernel is free to have given to an
   * unrelated socket anywhere in this process.
   *
   * The probe reports the state of a marker that the hold and the release of
   * the lock maintain, and that the shutdown site does not touch, so it really
   * tells the two orders apart. To reach the branch, an allocation must fail
   * after the code has already popped a pooled connection; the same sweep over
   * allocation indexes that
   * async_idle_pool.reused_hop_setup_failure_releases_engine_reference uses
   * finds one without hardcoding an ordinal. */
  char url[160];
  make_url(url, sizeof(url), "/keepalive");

  char *cerr = NULL;
  chttpcli cli = ccol_create_chttpclient_mp(&g_hop_fail_mp, &cerr);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);

  _chttpclient_reset_abandon_shutdown_probe_for_tests();

  int observed = -1;
  enum { SWEEP_UPPER = 40 };
  for (int idx = 0; idx < SWEEP_UPPER && observed < 0; idx++) {
    g_hop_fail_at_call = -1;
    ctpool_future *fw = async_get(cli, url);
    if (!fw) break;
    chttpcli_async_result_t *rw = chttpclient_async_result_get(fw);
    if (rw) {
      if (rw->resp) chttpclient_resp_free(rw->resp);
      chttpclient_async_result_free(rw);
    }
    ctpool_future_free(fw);

    atomic_store(&g_hop_fail_call_index, 0);
    g_hop_fail_at_call = idx;
    ctpool_future *f2 = async_get(cli, url);
    if (f2) {
      chttpcli_async_result_t *raw2 = chttpclient_async_result_get(f2);
      if (raw2) {
        if (raw2->resp) chttpclient_resp_free(raw2->resp);
        chttpclient_async_result_free(raw2);
      }
      ctpool_future_free(f2);
    }
    g_hop_fail_at_call = -1;

    /* The branch runs on this thread, because chttpclient_do_async submits hop
     * 0 synchronously, so the probe is already set when the future resolves.
     * The short bounded poll only covers a hop that a reactor thread
     * submitted instead. */
    for (int i = 0; i < 100; i++) {
      observed = _chttpclient_abandon_shutdown_under_lock_for_tests();
      if (observed >= 0) break;
      usleep(1000);
    }
  }

  g_hop_fail_at_call = -1;
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  /* A -1 means that the sweep never reached the branch at all, which makes
   * the assertion below vacuous, so this test fails on it instead of
   * skipping. */
  REQUIRE_NE(observed, -1);
  /* This is not vacuous: a shutdown of the fd after the release of idle_lock
   * records 0. */
  REQUIRE_EQ(observed, 1);
}

static void *_reaper_race_release_thread(void *arg) {
  (void)arg;
  _chttpclient_engine_release_for_tests();
  return NULL;
}

static void *_reaper_race_second_cycle_thread(void *arg) {
  atomic_bool *done = (atomic_bool *)arg;
  if (_chttpclient_engine_acquire_for_tests() == ccol_success)
    _chttpclient_engine_release_for_tests();
  atomic_store(done, true);
  return NULL;
}

TEST(async_engine, a_reaper_handle_is_never_lost_to_a_second_spawn) {
  /* Every reaper thread that the engine creates must be joined. A spawn
   * holds its new reaper between the creation of the thread and the
   * publication of its handle. A second acquire and release then runs on
   * another thread, which spawns a second reaper. Without a publication
   * that the reaper cannot overtake, the first reaper finishes, the second
   * cycle completes and publishes its own handle, and the first spawn then
   * overwrites it: one reaper is never joined. With it, the second acquire
   * waits until the first handle is published, and both reapers are
   * joined.
   *
   * The spawn is released after the second cycle finishes, or after one
   * second when the second cycle is blocked. Every thread is joined before
   * any assertion. */
  _chttpclient_engine_wait_for_quiescence_for_tests();
  unsigned created_before = _chttpclient_reaper_created_count_for_tests();
  unsigned joined_before = _chttpclient_reaper_joined_count_for_tests();
  REQUIRE_EQ(created_before, joined_before);
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);

  _chttpclient_hold_next_reaper_spawn_for_tests();
  pthread_t releaser;
  bool releaser_started =
      pthread_create(&releaser, NULL, _reaper_race_release_thread, NULL) == 0;
  bool held = false;
  for (int i = 0; releaser_started && i < 10000 && !held; ++i) {
    held = _chttpclient_reaper_spawn_is_held_for_tests();
    if (!held) usleep(1000);
  }

  atomic_bool second_done;
  atomic_init(&second_done, false);
  pthread_t second;
  bool second_started = false;
  if (held) {
    second_started =
        pthread_create(&second, NULL, _reaper_race_second_cycle_thread,
                       &second_done) == 0;
    for (int i = 0; second_started && i < 1000 && !atomic_load(&second_done);
         ++i)
      usleep(1000);
  }
  _chttpclient_release_reaper_spawn_for_tests();
  if (second_started) pthread_join(second, NULL);
  if (releaser_started) {
    pthread_join(releaser, NULL);
  } else {
    _chttpclient_engine_release_for_tests();
  }
  wait_for_async_engine_idle();

  unsigned created = _chttpclient_reaper_created_count_for_tests();
  unsigned joined = _chttpclient_reaper_joined_count_for_tests();
  REQUIRE_TRUE(releaser_started);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(second_started);
  REQUIRE_EQ(created - created_before, 2u);
  REQUIRE_EQ(joined - joined_before, 2u);
}

TEST(async_engine, reaper_spawn_failure_is_retried_by_the_sweep) {
  /* The teardown of the engine joins the deadline sweep, destroys the
   * reactor and destroys the DNS pool, and a reactor dispatch thread often
   * calls _client_engine_release. Running that teardown inline when the code
   * can spawn no thread is therefore two errors at once: a join of a thread
   * with itself, and a worker that calls ctpool_destroy on its own pool,
   * which cthreadpool detects and turns into a ccol_fatal_err() and a
   * SIGABRT. With no thread available, the engine must stay up, and the
   * deadline sweep retries the teardown until a thread is available.
   *
   * This test takes and releases the engine reference directly, not through
   * a request, so it owns the whole reference count, and nothing else in the
   * process is in the middle of a teardown while the spawn is forced to
   * fail.
   *
   * This test is non-vacuous: without the retry of the sweep, nothing
   * releases an engine that has no reference, and it stays up until the
   * next acquire and release. */
  unsigned abandoned_before =
      _chttpclient_engine_reaper_abandoned_count_for_tests();
  /* Nothing is armed yet and nothing holds a reference yet, so an early
   * return here leaves no state behind. */
  REQUIRE_EQ(_chttpclient_engine_acquire_for_tests(), ccol_success);

  _chttpclient_force_reaper_spawn_fail_for_tests(true);
  _chttpclient_engine_release_for_tests();
  /* While the injection stays armed, every retry of the sweep fails too, so
   * the state below is stable. */
  unsigned abandoned_after =
      _chttpclient_engine_reaper_abandoned_count_for_tests();
  bool still_running = _chttpclient_engine_running_for_tests();
  int refs = _chttpclient_engine_ref_count_for_tests();
  /* The wait for quiescence must return instead of blocking: the give-up
   * path clears `stopping`, so no teardown is in flight to wait for. */
  _chttpclient_engine_wait_for_quiescence_for_tests();
  /* This disarms it before any assertion, because an injection that stays
   * armed breaks every later engine teardown in this process. */
  _chttpclient_force_reaper_spawn_fail_for_tests(false);

  /* Thread creation works again now and nothing takes a reference, so the
   * sweep alone must bring the engine down. The bound is a safety net
   * against a hang; the sweep ticks every 100 ms. */
  bool reaped_by_sweep = false;
  for (int i = 0; i < 1000 && !reaped_by_sweep; i++) {
    reaped_by_sweep = !_chttpclient_engine_running_for_tests();
    if (!reaped_by_sweep) usleep(10000);
  }
  /* A failure above must not leave the engine up for the rest of the
   * binary, so one more acquire and release reaps it in the normal way. */
  ccol_retval_t reacquire_rv = ccol_success;
  if (!reaped_by_sweep) {
    reacquire_rv = _chttpclient_engine_acquire_for_tests();
    if (reacquire_rv == ccol_success) _chttpclient_engine_release_for_tests();
  }
  wait_for_async_engine_idle();
  bool running_at_end = _chttpclient_engine_running_for_tests();

  REQUIRE_EQ(reacquire_rv, ccol_success);
  REQUIRE_GE(abandoned_after - abandoned_before, 1u);
  REQUIRE_TRUE(still_running);
  REQUIRE_EQ(refs, 0);
  REQUIRE_TRUE(reaped_by_sweep);
  REQUIRE_FALSE(running_at_end);
}

/*
 * The test above drives the give-up path from its own thread, where the
 * teardown would in fact have been safe, so that test pins the decision
 * but never reaches the consequence. The thread that matters is a reactor
 * thread, where the teardown destroys the very event loop inside whose
 * callback it runs; ccol_event_loop_destroy detects that as a self-destroy
 * and stops the process through ccol_fatal_err(). Leaving the engine up is
 * what avoids that.
 *
 * No test can check this inside its own process, because the abort stops
 * the whole binary and takes the result of every other test with it. The
 * dangerous path therefore runs in a forked child, and the parent reads the
 * outcome off a pipe, as a marker byte rather than as the exit status of
 * the child: valgrind runs with --errors-for-leak-kinds=all, so it
 * overrides the status of a forked child the moment it finds a
 * still-reachable allocation in the inherited process image, which every
 * child forked in the middle of a suite has. A child that dies writes
 * nothing, and the read of the parent then reports
 * end-of-file.
 *
 * Which thread runs the last release is not left to chance. The child
 * brings the engine down to one reference, held by one connection in the
 * idle pool, arms the spawn failure, and only then shuts the fd of that
 * connection down, so the IDLE dispatch of the reactor is what releases
 * the reference.
 */
TEST(async_engine, reaper_spawn_failure_on_a_reactor_thread_does_not_abort) {
  /* The child builds its engine from nothing, so the engine of the parent
   * must be fully down before the fork. This is the documented way to
   * combine fork() with this module: a caller creates a handle after the
   * fork, and a handle is never inherited across one. */
  wait_for_async_engine_idle();
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);

  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    char marker = 'F';
    /* More than one reactor thread is what gives the reactor a dispatch pool,
     * so readiness arrives on a worker of that pool instead of on the
     * poller thread. This code pins the count instead of leaving it at the
     * default of the module (the CPU count), so that the thread that the
     * release lands on has the same shape on a machine with one core as on
     * any other. */
    (void)chttpcli_set_engine_num_reactor_threads(4);
    chttpcli cli = ccol_create_chttpclient(NULL);
    if (cli != CHTTPCLI_INVALID) {
      char child_url[160];
      make_url(child_url, sizeof(child_url), "/keepalive");
      ctpool_future *f = async_get(cli, child_url);
      if (f) {
        chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
        if (raw) {
          if (raw->resp) chttpclient_resp_free(raw->resp);
          chttpclient_async_result_free(raw);
        }
        ctpool_future_free(f);
      }
      /* One connection now sits in the idle pool, holding the only reference
       * that the engine has left; the reference of the chain went away when
       * the hop detached to join the pool. This code polls instead of
       * reading the count once, for the reason that poll_engine_ref_count
       * documents.
       *
       * Everything up to this point uses a plain if instead of a REQUIRE_*,
       * because this runs in the forked child, where an early return skips
       * _exit() and drops back into the test loop of the harness, while
       * this process exists to run one branch and then exit. */
      if (poll_engine_ref_count(1) == 1) {
        _chttpclient_force_reaper_spawn_fail_for_tests(true);
        /* A pooled connection holds that one reference, and no
         * chttpclient_destroy on this thread drains it, so the IDLE dispatch
         * of the reactor is the only thing that can release it. The
         * shutdown of the fd is what makes that dispatch happen now; without
         * it, the dispatch waits until the server hangs up. The last
         * release, and with it the give-up path, therefore runs on a
         * reactor thread by construction, not by luck. */
        _chttpclient_shutdown_async_idle_connections_for_tests(
            _chttpcli_resolve_for_tests(cli));
        /* A give-up path that runs is a change of state, so this code polls the
         * counter that records it instead of waiting out a duration. The
         * bound is a safety net against a hang: a child that never reaches
         * the branch reports 'F', and the parent then fails on that rather
         * than on a timeout. */
        unsigned abandoned = 0;
        for (int i = 0; i < 10000 && abandoned == 0; i++) {
          abandoned = _chttpclient_engine_reaper_abandoned_count_for_tests();
          if (abandoned == 0) usleep(1000);
        }
        if (abandoned > 0 && _chttpclient_engine_running_for_tests())
          marker = 'K';
      }
    }
    ssize_t written = write(pfd[1], &marker, 1);
    (void)written;
    _exit(0);
  }

  close(pfd[1]);
  char marker = 0;
  ssize_t got = 0;
  bool timed_out = false;
  if (pid != -1) {
    /* This wait is bounded, so that a child that hangs, instead of aborting
     * or answering, fails this test instead of blocking the whole binary
     * on a read that never returns. */
    struct pollfd pr = {.fd = pfd[0], .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pr, 1, 120000);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0) {
      do {
        got = read(pfd[0], &marker, 1);
      } while (got < 0 && errno == EINTR);
    } else {
      timed_out = true;
      kill(pid, SIGKILL);
    }
  }
  close(pfd[0]);
  int status = 0;
  if (pid != -1) waitpid(pid, &status, 0);

  REQUIRE_NE(pid, -1);
  REQUIRE_FALSE(timed_out);
  /* This is not vacuous: a teardown that runs inline, instead of leaving the
   * engine up, aborts the child, because the self-destroy check of
   * ccol_event_loop_destroy stops it before it can write anything. got is
   * then 0, and the child reports as signalled rather than as exited. */
  REQUIRE_EQ((int)got, 1);
  REQUIRE_EQ((int)marker, (int)'K');
  REQUIRE_TRUE(WIFEXITED(status));
}

extern clog _chttpclient_engine_logger_for_tests(void);

/* The grandchild side of engine_logger_of_an_inherited_engine_is_closed. It
 * reports on report_fd, and it never returns. */
static void _inherited_logger_grandchild(clog inherited, int report_fd) {
  char marker = 'A'; /* The acquire failed. */
  if (_chttpclient_engine_acquire_for_tests() == ccol_success) {
    clog now = _chttpclient_engine_logger_for_tests();
    marker = (now == CLOG_INVALID) ? 'Z' : (now == inherited ? 'S' : 'N');
  }
  ssize_t w = write(report_fd, &marker, 1);
  (void)w;
  /* A closed handle is fatal to use, so this aborts when the grandchild has
   * closed the logger of the engine run that it inherited. */
  (void)clog_get_level(inherited);
  marker = 'L';
  w = write(report_fd, &marker, 1);
  (void)w;
  _exit(0);
}

/* A child of fork() inherits an engine of the parent that has no reference
 * left, with the fallback logger of that engine run. The engine that the
 * child builds opens a logger of its own, and the child closes the one that
 * it inherited, which nothing else would ever close there. The engine
 * logger of the application stays across the fork; this test runs before
 * any test of this file installs one. This test is non-vacuous: without the
 * close in the forget of an inherited engine, the new engine of the child
 * keeps logging through the inherited handle, and the grandchild reports
 * 'S' and then 'L'.
 *
 * The inherited engine with no reference comes from the give-up path of a
 * reaper that could not start, which runs in a child of its own for the
 * reason that reaper_spawn_failure_on_a_reactor_thread_does_not_abort
 * gives. That child then forks the grandchild that checks the logger. */
TEST(async_engine, engine_logger_of_an_inherited_engine_is_closed) {
  wait_for_async_engine_idle();
  REQUIRE_FALSE(_chttpclient_engine_running_for_tests());

  int pfd[2];
  REQUIRE_EQ(pipe(pfd), 0);
  pid_t pid = fork();
  if (pid == 0) {
    close(pfd[0]);
    char verdict[2] = {'F', 'F'};
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    (void)chttpcli_set_engine_num_reactor_threads(4);
    chttpcli cli = ccol_create_chttpclient(NULL);
    clog inherited = CLOG_INVALID;
    bool abandoned = false;
    if (cli != CHTTPCLI_INVALID) {
      char child_url[160];
      make_url(child_url, sizeof(child_url), "/keepalive");
      ctpool_future *f = async_get(cli, child_url);
      if (f) {
        chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
        if (raw) {
          if (raw->resp) chttpclient_resp_free(raw->resp);
          chttpclient_async_result_free(raw);
        }
        ctpool_future_free(f);
      }
      if (poll_engine_ref_count(1) == 1) {
        /* The count covers the whole process, and this child inherits the
         * count of the parent, so the give-up path shows as a rise. */
        unsigned before =
            _chttpclient_engine_reaper_abandoned_count_for_tests();
        _chttpclient_force_reaper_spawn_fail_for_tests(true);
        _chttpclient_shutdown_async_idle_connections_for_tests(
            _chttpcli_resolve_for_tests(cli));
        for (int i = 0; i < 10000 && !abandoned; i++) {
          abandoned =
              _chttpclient_engine_reaper_abandoned_count_for_tests() > before;
          if (!abandoned) usleep(1000);
        }
        inherited = _chttpclient_engine_logger_for_tests();
      }
    }
    /* Each precondition that fails leaves a marker of its own. */
    verdict[0] = !abandoned                                 ? 'a'
                 : inherited == CLOG_INVALID                ? 'b'
                 : !_chttpclient_engine_running_for_tests() ? 'c'
                                                            : 'F';
    if (verdict[0] == 'F') {
      int gfd[2];
      if (pipe(gfd) == 0) {
        pid_t g = fork();
        if (g == 0) {
          close(gfd[0]);
          _inherited_logger_grandchild(inherited, gfd[1]);
        }
        close(gfd[1]);
        if (g > 0) {
          char got[2] = {0, 0};
          ssize_t n = 0;
          for (int i = 0; i < 2; i++) {
            struct pollfd pr = {.fd = gfd[0], .events = POLLIN, .revents = 0};
            if (poll(&pr, 1, 60000) <= 0) break;
            ssize_t r = read(gfd[0], &got[i], 1);
            if (r != 1) break;
            n++;
          }
          int st = 0;
          if (n == 0) kill(g, SIGKILL);
          waitpid(g, &st, 0);
          verdict[0] = got[0] ? got[0] : 'F';
          /* One byte and a death by signal: the inherited handle was
           * closed. A second byte: it was still open. */
          verdict[1] = (n == 1 && WIFSIGNALED(st)) ? 'C' : (n == 2 ? 'L' : 'F');
        }
        close(gfd[0]);
      }
    }
    ssize_t w = write(pfd[1], verdict, 2);
    (void)w;
    _exit(0);
  }

  close(pfd[1]);
  char verdict[2] = {0, 0};
  ssize_t got = 0;
  bool timed_out = false;
  if (pid != -1) {
    struct pollfd pr = {.fd = pfd[0], .events = POLLIN, .revents = 0};
    int rc;
    do {
      rc = poll(&pr, 1, 120000);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0) {
      do {
        got = read(pfd[0], verdict, 2);
      } while (got < 0 && errno == EINTR);
    } else {
      timed_out = true;
      kill(pid, SIGKILL);
    }
  }
  close(pfd[0]);
  int status = 0;
  if (pid != -1) waitpid(pid, &status, 0);

  REQUIRE_NE(pid, -1);
  REQUIRE_FALSE(timed_out);
  REQUIRE_EQ((int)got, 2);
  REQUIRE_EQ((int)verdict[0], (int)'N');
  REQUIRE_EQ((int)verdict[1], (int)'C');
}

/* ========================================================================== */
/*          chttp_tls_config_t IS SAFE WHEN ZERO-INITIALISED                  */
/* ========================================================================== */

/* The one thing that must stay true of chttp_tls_config_t is that a caller
 * cannot end up with less verification than they asked for by writing LESS
 * than they meant to. Every idiomatic way of building the struct leaves the
 * fields that the caller did not name as zero, so zero has to be the strict
 * setting. These tests pin that property on the struct itself, so a future
 * field that is added with the opposite polarity fails here. */
TEST(tls_config_defaults, zero_initialised_config_verifies_everything) {
  chttp_tls_config_t zeroed;
  memset(&zeroed, 0, sizeof(zeroed));
  REQUIRE_FALSE(zeroed.insecure_skip_verify);
  REQUIRE_FALSE(zeroed.insecure_skip_hostname_check);

  chttp_tls_config_t braced = {0};
  REQUIRE_FALSE(braced.insecure_skip_verify);
  REQUIRE_FALSE(braced.insecure_skip_hostname_check);

  /* A designated initialiser that names only the CA bundle: the shape that
   * would silently disable every check if zero were not the strict
   * setting. */
  chttp_tls_config_t just_a_bundle = {.ca_bundle_path = "/etc/ssl/ca.pem"};
  REQUIRE_FALSE(just_a_bundle.insecure_skip_verify);
  REQUIRE_FALSE(just_a_bundle.insecure_skip_hostname_check);

  /* And one that names only a client certificate pair. */
  chttp_tls_config_t just_a_cert = {.cert_path = "c.pem", .key_path = "k.pem"};
  REQUIRE_FALSE(just_a_cert.insecure_skip_verify);
  REQUIRE_FALSE(just_a_cert.insecure_skip_hostname_check);
}

/* CHTTP_TLS_DEFAULT and a zero-initialised struct are the two documented ways
 * to spell "the defaults". They must keep naming the same settings. If the two
 * drift, one of them silently stops meaning what the header says it means.
 *
 * The comparison is field by field and NOT a memcmp of the two structs. A
 * compound literal initialises the members that it names and leaves the
 * PADDING between them unspecified, so a memcmp compares stack residue and
 * fails for a reason that has nothing to do with the settings. It really does
 * fail that way on this target.
 *
 * A field added to chttp_tls_config_t later must be added here too. It must
 * also default to the strict setting, which is what the test above pins. */
TEST(tls_config_defaults, the_default_macro_matches_a_zeroed_struct) {
  chttp_tls_config_t from_macro = CHTTP_TLS_DEFAULT;
  chttp_tls_config_t zeroed;
  memset(&zeroed, 0, sizeof(zeroed));

  REQUIRE_EQ((void *)from_macro.cert_path, (void *)zeroed.cert_path);
  REQUIRE_EQ((void *)from_macro.key_path, (void *)zeroed.key_path);
  REQUIRE_EQ((void *)from_macro.ca_bundle_path, (void *)zeroed.ca_bundle_path);
  /* The casts to int are what the _Generic printer of tau accepts. It knows
     the standard numeric types and void *, and bool is not among them. */
  REQUIRE_EQ((int)from_macro.insecure_skip_verify,
             (int)zeroed.insecure_skip_verify);
  REQUIRE_EQ((int)from_macro.insecure_skip_hostname_check,
             (int)zeroed.insecure_skip_hostname_check);
  REQUIRE_EQ((int)from_macro.client_cert_optional,
             (int)zeroed.client_cert_optional);
}

/* A CA bundle exists to be verified against, so naming one while switching
 * verification off states two incompatible policies. The library must refuse
 * the pair at the call that declares it rather than pick one silently. */
TEST(tls_config_defaults, skip_verify_together_with_a_ca_bundle_is_refused) {
  chttpcli cli _ccol_destructor(___chttpclient_destroy) =
      ccol_create_chttpclient(NULL);
  REQUIRE_TRUE(cli != CHTTPCLI_INVALID);

  chttp_tls_config_t contradictory = {
      .ca_bundle_path = "/etc/ssl/ca.pem",
      .insecure_skip_verify = true,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &contradictory), ccol_invalid_args);

  /* Each half on its own stays perfectly legal. */
  chttp_tls_config_t bundle_only = {.ca_bundle_path = "/etc/ssl/ca.pem"};
  REQUIRE_EQ(chttpclient_set_tls(cli, &bundle_only), ccol_success);

  chttp_tls_config_t skip_only = {.insecure_skip_verify = true};
  REQUIRE_EQ(chttpclient_set_tls(cli, &skip_only), ccol_success);

  /* So does the narrower relaxation beside a pinned bundle, which is the
   * "reach this host by an address it has no certificate for, but still
   * require my CA" configuration. */
  chttp_tls_config_t pinned_no_hostname = {
      .ca_bundle_path = "/etc/ssl/ca.pem",
      .insecure_skip_hostname_check = true,
  };
  REQUIRE_EQ(chttpclient_set_tls(cli, &pinned_no_hostname), ccol_success);

  REQUIRE_EQ(chttpclient_set_tls(cli, NULL), ccol_success);
}

/* ========================================================================== */
/*                    REDIRECT TRANSPORT RULES                                */
/* ========================================================================== */

/* The client has two transport rules for a redirect, and they differ on
 * purpose.
 *
 * The AF_UNIX rule is unconditional. A redirect can never add that transport
 * and can never re-point it at another socket, because that would hand any
 * http or https server a request-forgery primitive against every local socket
 * the process can reach.
 *
 * The TLS rule is an opt-in, through the prevent_tls_downgrade_on_redirect
 * field of chttp_request_t.
 * By DEFAULT a chain follows a redirect from https to http, which is what
 * curl and the Go net/http client both do. A caller who cannot accept a
 * downgrade sets the field and gets a refusal instead. */

/* The default must follow a downgrade. This is the half that keeps the
 * library interchangeable with curl and Go for a caller who is porting.
 *
 * This test is non-vacuous in the direction that matters: against a build
 * that refuses the downgrade unconditionally, every case here reports
 * allowed = false. */
TEST(redirect_transport, https_to_http_is_followed_by_default) {
  char *resolved = NULL;

  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example/y", /*prevent_downgrade=*/false,
      &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://evil.example/y",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;

  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example:8080/y",
      /*prevent_downgrade=*/false, &resolved));
  free(resolved);
  resolved = NULL;
}

/* A request that opts in gets the refusal, on every spelling of the
 * downgrade.
 *
 * This test is non-vacuous: against a build that ignores the field, every
 * case here reports allowed = true. */
TEST(redirect_transport, prevent_downgrade_refuses_the_downgrade) {
  char *resolved = NULL;

  /* Same host. */
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example/y", /*prevent_downgrade=*/true,
      &resolved));
  free(resolved);
  resolved = NULL;

  /* Different host, which is the shape that matters most. */
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://evil.example/y",
      /*prevent_downgrade=*/true, &resolved));
  free(resolved);
  resolved = NULL;

  /* A different port must not change the answer. */
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "http://a.example:8080/y",
      /*prevent_downgrade=*/true, &resolved));
  free(resolved);
  resolved = NULL;

  /* The scheme comparison ignores case, exactly as the parser does. */
  REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "HTTP://a.example/y", /*prevent_downgrade=*/true,
      &resolved));
  free(resolved);
  resolved = NULL;
}

/* The opt-in must cost nothing that is legitimate: a reference that only
 * inherits the scheme of the base resolves back to https and follows, an
 * upgrade adds protection and follows, and a chain that was never on TLS
 * is unaffected. All of these hold with the field SET, which is what makes
 * the opt-in usable rather than a blunt "refuse every redirect". */
TEST(redirect_transport, prevent_downgrade_still_follows_everything_else) {
  char *resolved = NULL;

  /* Protocol-relative: takes the scheme of the base, so it stays https. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "//other.example/y", /*prevent_downgrade=*/true,
      &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  /* Absolute path on the same origin. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "/y", /*prevent_downgrade=*/true, &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  /* Relative reference. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/dir/x", "../y",
      /*prevent_downgrade=*/true, &resolved));
  REQUIRE_NE((void *)resolved, NULL);
  REQUIRE_EQ(strncmp(resolved, "https://", 8), 0);
  free(resolved);
  resolved = NULL;

  /* https to https on another host stays allowed. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "https://a.example/x", "https://b.example/y", /*prevent_downgrade=*/true,
      &resolved));
  free(resolved);
  resolved = NULL;

  /* An UPGRADE can only add protection, so it follows either way. */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "http://a.example/x", "https://a.example/y", /*prevent_downgrade=*/true,
      &resolved));
  free(resolved);
  resolved = NULL;

  /* A chain that never used TLS keeps following plain http, because there is
     no TLS to lose. The field must not turn into "https only". */
  REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
      "http://a.example/x", "http://b.example/y", /*prevent_downgrade=*/true,
      &resolved));
  free(resolved);
  resolved = NULL;
}

/* The AF_UNIX rule is unconditional, so it must give the same answer with the
 * field set and with it clear. */
TEST(redirect_transport, the_unix_rule_ignores_the_opt_in) {
  for (int i = 0; i < 2; i++) {
    bool prevent_downgrade = (i == 1);
    char *resolved = NULL;

    /* An https hop may not reach a socket. */
    REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
        "https://a.example/x", "http+unix://%2Ftmp%2Fs/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;

    /* Neither may a plain http hop. */
    REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
        "http://a.example/x", "http+unix://%2Ftmp%2Fs/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;

    /* A socket hop stays on its own socket. */
    REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http+unix://%2Ftmp%2Fs/y",
        prevent_downgrade, &resolved));
    free(resolved);
    resolved = NULL;

    /* And may not move to a different one. */
    REQUIRE_FALSE(_chttp_redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http+unix://%2Ftmp%2Fother/y",
        prevent_downgrade, &resolved));
    free(resolved);
    resolved = NULL;

    /* A socket hop leaving for http adds no transport and re-points no
       socket, so it follows. It was never on TLS, so prevent_downgrade does not
       apply to it either. */
    REQUIRE_TRUE(_chttp_redirect_transport_allowed_for_tests(
        "http+unix://%2Ftmp%2Fs/x", "http://a.example/y", prevent_downgrade,
        &resolved));
    free(resolved);
    resolved = NULL;
  }
}

/* The field defaults to false, so a caller who never mentions it gets the
   curl and Go behaviour. chttp_request_new_mp zero-fills the struct. */
TEST(redirect_transport, the_request_field_defaults_to_off) {
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "https://a.example/x", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);
  bool defaulted_off = !req->prevent_tls_downgrade_on_redirect;
  chttp_request_free(req);
  REQUIRE_TRUE(defaulted_off);
}

/* ========================================================================== */
/*          CONNECT FALLBACK, REDIRECT HEADER SCOPE, REQUEST-TARGET           */
/* ========================================================================== */

/* Runs one request through the tier that `tier` names: 1 is chttpclient_do,
 * 2 is chttpclient_do_async and 3 is chttpclient_do_pooled. */
static ccol_retval_t run_in_tier(int tier, chttpcli cli,
                                 const chttp_request_t *req,
                                 chttpcli_response **resp_out) {
  *resp_out = NULL;
  if (tier == 1) return chttpclient_do(cli, req, resp_out);
  if (tier == 3) return chttpclient_do_pooled(cli, req, resp_out);
  ctpool_future *f = chttpclient_do_async(cli, req);
  if (!f) return ccol_unexpected_failure;
  chttpcli_async_result_t *r = chttpclient_async_result_get(f);
  ccol_retval_t rv = ccol_unexpected_failure;
  if (r) {
    rv = r->rv;
    *resp_out = r->resp;
    chttpclient_async_result_free(r);
  }
  ctpool_future_free(f);
  return rv;
}

/* A small scripted HTTP/1.1 listener for the tests of this section. It
 * serves one connection at a time and closes each one after one response.
 * The routes are:
 *   /redir?u=<url>   302 with "Location: <url>"
 *   /raw-location    302 with the Location of g_hs_raw_location, byte for
 *                    byte
 *   /http10-te       an HTTP/1.0 response that asks for keep-alive and is
 *                    framed by Transfer-Encoding: chunked; the connection
 *                    stays open for another request
 *   /http11-ka       an HTTP/1.1 keep-alive response; the connection stays
 *                    open for another request
 *   /bad-chunk-ext   a chunked response whose chunk extension holds a
 *                    control byte
 *   anything else    200 whose body is the request header block, request
 *                    line included, exactly as this end read it
 * Every route that does not keep the connection closes it after one
 * response. accepts counts the connections. redirs counts the /redir
 * requests, and redirs_with_credentials those of them that carried an
 * Authorization or a Cookie header. */
/* Wakes a thread that is blocked in accept(2) on the listening socket lfd.
 * Linux and FreeBSD wake it on shutdown(2). macOS refuses shutdown(2) on a
 * listening socket with ENOTCONN and leaves the thread blocked, so a
 * connection to the listener wakes it there. Each accept loop that a stop
 * wakes in this way checks its running flag right after accept(2) returns,
 * so the wake connection is never served. */
static void test_wake_listener(int lfd) {
  (void)shutdown(lfd, SHUT_RDWR);
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  if (getsockname(lfd, (struct sockaddr *)&ss, &len) != 0) return;
  if (ss.ss_family == AF_INET) {
    struct sockaddr_in *a = (struct sockaddr_in *)&ss;
    if (a->sin_addr.s_addr == htonl(INADDR_ANY))
      a->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  } else if (ss.ss_family == AF_INET6) {
    struct sockaddr_in6 *a = (struct sockaddr_in6 *)&ss;
    if (IN6_IS_ADDR_UNSPECIFIED(&a->sin6_addr)) a->sin6_addr = in6addr_loopback;
  }
  int c = socket(ss.ss_family, SOCK_STREAM, 0);
  if (c < 0) return;
  (void)connect(c, (struct sockaddr *)&ss, len);
  close(c);
}

typedef struct {
  int fd;
  int port;
  pthread_t tid;
  atomic_int running;
  atomic_int accepts;
  atomic_int redirs;
  atomic_int redirs_with_credentials;
} hs_srv_t;

static char g_hs_raw_location[512];

static void *hs_loop(void *arg) {
  hs_srv_t *s = (hs_srv_t *)arg;
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (!buf) return NULL;
  while (atomic_load(&s->running)) {
    int c = accept(s->fd, NULL, NULL);
    if (c < 0) {
      if (!atomic_load(&s->running)) break;
      continue;
    }
    if (!atomic_load(&s->running)) { /* the wake of hs_stop() */
      close(c);
      break;
    }
    atomic_fetch_add(&s->accepts, 1);
    for (;;) {
      size_t hl = 0;
      ssize_t n = srv_read_headers(c, buf, TEST_SERVER_BUF, &hl);
      if (n <= 0 || hl == 0) break;
      char method[16], path[1024];
      srv_parse_request_line(buf, method, sizeof(method), path, sizeof(path));
      if (strcmp(path, "/http10-te") == 0) {
        static const char r[] =
            "HTTP/1.0 200 OK\r\n"
            "Connection: keep-alive\r\n"
            "Transfer-Encoding: chunked\r\n"
            "\r\n"
            "5\r\nhello\r\n0\r\n\r\n";
        send(c, r, sizeof(r) - 1, 0);
        continue;
      }
      if (strcmp(path, "/http11-ka") == 0) {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "\r\n"
            "hello";
        send(c, r, sizeof(r) - 1, 0);
        continue;
      }
      if (strcmp(path, "/bad-chunk-ext") == 0) {
        static const char r[] =
            "HTTP/1.1 200 OK\r\n"
            "Transfer-Encoding: chunked\r\n"
            "Connection: close\r\n"
            "\r\n"
            "5;a=\x01\r\nhello\r\n0\r\n\r\n";
        send(c, r, sizeof(r) - 1, 0);
        break;
      }
      if (strncmp(path, "/redir?u=", 9) == 0) {
        char v[128];
        atomic_fetch_add(&s->redirs, 1);
        if (srv_find_header(buf, "authorization", v, sizeof(v)) ||
            srv_find_header(buf, "cookie", v, sizeof(v)))
          atomic_fetch_add(&s->redirs_with_credentials, 1);
        char loc[1100];
        snprintf(loc, sizeof(loc), "Location: %s\r\n", path + 9);
        srv_respond(c, 302, "Found", "text/plain", loc, NULL, 0, false);
      } else if (strcmp(path, "/raw-location") == 0) {
        char loc[600];
        snprintf(loc, sizeof(loc), "Location: %s\r\n", g_hs_raw_location);
        srv_respond(c, 302, "Found", "text/plain", loc, NULL, 0, false);
      } else {
        srv_respond(c, 200, "OK", "text/plain", NULL, buf, hl, false);
      }
      break;
    }
    close(c);
  }
  free(buf);
  return NULL;
}

/* Starts a listener on the IPv4 address `ip`. It returns false when the
 * environment cannot bind that address. */
static bool hs_start(hs_srv_t *s, const char *ip) {
  memset(s, 0, sizeof(*s));
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->fd < 0) return false;
  int opt = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  if (inet_pton(AF_INET, ip, &a.sin_addr) != 1 ||
      bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(s->fd, 16) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  socklen_t len = sizeof(a);
  getsockname(s->fd, (struct sockaddr *)&a, &len);
  s->port = ntohs(a.sin_port);
  atomic_store(&s->running, 1);
  if (pthread_create(&s->tid, NULL, hs_loop, s) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  return true;
}

/* The listener fd is closed only after the join. The accept loop reads
 * s->fd, and a write to it before the join is a data race. */
static void hs_stop(hs_srv_t *s) {
  if (s->fd < 0) return;
  atomic_store(&s->running, 0);
  test_wake_listener(s->fd);
  pthread_join(s->tid, NULL);
  close(s->fd);
  s->fd = -1;
}

/* --------------------------- connect fallback ---------------------------- */

extern void _chttp_set_addr_override_for_tests(
    const struct sockaddr *const *addrs, const socklen_t *lens, size_t count);
extern void _chttp_set_silent_addr_for_tests(const struct sockaddr *addr,
                                             socklen_t len);

/* A socket bound to 127.0.0.1 that never listens. A connect to its port is
 * refused, and the port stays taken for as long as the socket lives. */
static int refused_port_socket(struct sockaddr_in *out) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(*out);
  if (bind(fd, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(fd, (struct sockaddr *)out, &len) != 0) {
    close(fd);
    return -1;
  }
  return fd;
}

/* The first candidate address refuses and the second one serves. Every tier
 * must go on to the second one. This test is non-vacuous: without the
 * fallback of _async_connect_failed, Tier 2 and Tier 3 report
 * ccol_http_connection_failed. */
TEST(connect_fallback, next_address_is_tried_after_a_refusal_in_every_tier) {
  struct sockaddr_in refused;
  int rfd = refused_port_socket(&refused);
  REQUIRE_GE(rfd, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());

  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&refused,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(refused), sizeof(good)};
  _chttp_set_addr_override_for_tests(addrs, lens, 2);

  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://fallback.invalid/get", NULL, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  close(rfd);

  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
}

TEST(connect_fallback, every_candidate_refusing_reports_connection_failed) {
  struct sockaddr_in r1, r2;
  int f1 = refused_port_socket(&r1);
  int f2 = refused_port_socket(&r2);
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&r1,
                                     (const struct sockaddr *)&r2};
  socklen_t lens[2] = {sizeof(r1), sizeof(r2)};
  if (f1 >= 0 && f2 >= 0) _chttp_set_addr_override_for_tests(addrs, lens, 2);

  chttpcli_construct(cli);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://fallback.invalid/get", NULL, NULL);
  ccol_retval_t rv[4] = {0};
  for (int tier = 1; tier <= 3 && req && f1 >= 0 && f2 >= 0; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  if (f1 >= 0) close(f1);
  if (f2 >= 0) close(f2);

  REQUIRE_GE(f1, 0);
  REQUIRE_GE(f2, 0);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_connection_failed);
}

/* The everyday form of the case above. The mock server listens on
 * 127.0.0.1 alone, and a standard /etc/hosts resolves "localhost" to ::1
 * first. Where the resolver of this machine lists 127.0.0.1 first, the test
 * still runs and passes, but it proves nothing about the fallback; the test
 * above is the one that holds on every machine. */
TEST(connect_fallback, localhost_reaches_an_ipv4_only_listener_in_every_tier) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  bool v6_first = getaddrinfo("localhost", "80", &hints, &res) == 0 && res &&
                  res->ai_family == AF_INET6;
  if (res) freeaddrinfo(res);
  if (!v6_first)
    fprintf(stderr,
            "NOTE: localhost does not resolve to ::1 first here, so "
            "localhost_reaches_an_ipv4_only_listener_in_every_tier does not "
            "exercise the fallback\n");

  char url[160];
  snprintf(url, sizeof(url), "http://localhost:%d/get", get_test_port());
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);

  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
}

/* ----------------------- redirect header scope ---------------------------- */

/* What the final hop of a redirect chain received from the headers that
 * this section sets on the request. */
typedef struct {
  ccol_retval_t rv;
  int status;
  bool auth, cookie, cookie2, www_auth, keep;
  char host[128];
} hop_seen_t;

static chttp_request_t *new_request_with_sensitive_headers(const char *url) {
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  if (chttp_request_set_header(req, "Authorization", "Bearer t0k3n") !=
          ccol_success ||
      chttp_request_set_header(req, "Cookie", "session=abc") != ccol_success ||
      chttp_request_set_header(req, "Cookie2", "$Version=1") != ccol_success ||
      chttp_request_set_header(req, "WWW-Authenticate", "Basic realm=x") !=
          ccol_success ||
      chttp_request_set_header(req, "Host", "custom.example") != ccol_success ||
      chttp_request_set_header(req, "X-Keep", "kept") != ccol_success) {
    chttp_request_free(req);
    return NULL;
  }
  return req;
}

/* Runs `url` in every tier and records what the final hop saw. */
static void run_sensitive_redirect(chttpcli cli, const char *url,
                                   hop_seen_t seen[4]) {
  memset(seen, 0, 4 * sizeof(hop_seen_t));
  chttp_request_t *req = new_request_with_sensitive_headers(url);
  for (int tier = 1; tier <= 3; tier++) {
    seen[tier].rv = ccol_unexpected_failure;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    seen[tier].rv = run_in_tier(tier, cli, req, &resp);
    if (resp && resp->body) {
      char v[128];
      seen[tier].status = resp->status_code;
      seen[tier].auth = srv_find_header(resp->body, "authorization", v, 128);
      seen[tier].cookie = srv_find_header(resp->body, "cookie", v, 128);
      seen[tier].cookie2 = srv_find_header(resp->body, "cookie2", v, 128);
      seen[tier].www_auth =
          srv_find_header(resp->body, "www-authenticate", v, 128);
      seen[tier].keep = srv_find_header(resp->body, "x-keep", v, 128);
      srv_find_header(resp->body, "host", seen[tier].host,
                      sizeof(seen[tier].host));
    }
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
}

/* A redirect to another host drops the credential headers and the Host
 * header of the caller, and the client writes its own Host line. This test
 * is non-vacuous: without CHTTP_REDIRECT_DROP_CREDENTIALS, the Cookie
 * reaches 127.0.0.2, and without CHTTP_REDIRECT_DROP_HOST, the Host line
 * still names custom.example. */
TEST(redirect_header_scope, cross_host_drops_credentials_and_host) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[256], expect_host[64];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/final", a.port,
           b.port);
  snprintf(expect_host, sizeof(expect_host), "127.0.0.2:%d", b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  hs_stop(&a);
  hs_stop(&b);

  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, expect_host);
  }
}

/* Another port on the same host is another origin, so the credentials go,
 * and the same host keeps the Host header of the caller. */
TEST(redirect_header_scope, cross_port_drops_credentials_keeps_host) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.1:%d/final", a.port,
           b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  if (ok_a && ok_b) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  if (ok_a) hs_stop(&a);
  if (ok_b) hs_stop(&b);

  REQUIRE_TRUE(ok_a && ok_b);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

/* A redirect within one origin keeps every header of the caller. */
TEST(redirect_header_scope, same_origin_keeps_every_header) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.1:%d/final", a.port,
           a.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  if (ok_a) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_TRUE(seen[tier].auth);
    REQUIRE_TRUE(seen[tier].cookie);
    REQUIRE_TRUE(seen[tier].cookie2);
    REQUIRE_TRUE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

/* Every hop is judged against the origin of the FIRST request, which is
 * the rule of curl. A chain A -> B -> A leaves the headers off the hop to B
 * and sends them again on the hop back to A. This test is non-vacuous: a
 * drop that latched for the rest of the chain leaves them off the last
 * hop too. */
TEST(redirect_header_scope, return_to_first_origin_sends_headers_again) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[512];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/redir?u="
           "http://127.0.0.1:%d/final",
           a.port, b.port, a.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  int b_redirs = atomic_load(&b.redirs);
  int b_with_credentials = atomic_load(&b.redirs_with_credentials);
  hs_stop(&a);
  hs_stop(&b);

  REQUIRE_EQ(b_redirs, 3);
  REQUIRE_EQ(b_with_credentials, 0);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_TRUE(seen[tier].auth);
    REQUIRE_TRUE(seen[tier].cookie);
    REQUIRE_TRUE(seen[tier].cookie2);
    REQUIRE_TRUE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

/* A chain A -> B -> B stays on a foreign origin for its last hop. That hop
 * matches the hop before it, but not the first request, so the headers stay
 * off. This test is non-vacuous: a comparison against the previous hop
 * sends them on the last hop. */
TEST(redirect_header_scope, second_hop_on_foreign_origin_still_drops) {
  hs_srv_t a, b;
  bool ok_a = hs_start(&a, "127.0.0.1");
  bool ok_b = hs_start(&b, "127.0.0.2");
  if (!ok_a || !ok_b) {
    if (ok_a) hs_stop(&a);
    if (ok_b) hs_stop(&b);
    fprintf(stderr, "SKIP: cannot bind 127.0.0.2 in this environment\n");
    return;
  }
  char url[512], expect_host[64];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=http://127.0.0.2:%d/redir?u="
           "http://127.0.0.2:%d/final",
           a.port, b.port, b.port);
  snprintf(expect_host, sizeof(expect_host), "127.0.0.2:%d", b.port);
  chttpcli_construct(cli);
  hop_seen_t seen[4];
  run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  hs_stop(&a);
  hs_stop(&b);

  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, expect_host);
  }
}

/* http to https on the same host is another origin: the credentials go and
 * the Host header of the caller stays. */
TEST(redirect_header_scope, cross_scheme_drops_credentials) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url),
           "http://127.0.0.1:%d/redir?u=https://127.0.0.1:%d/echo-headers",
           a.port, g_tls_srv_port);
  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval_t trv = chttpclient_set_tls(cli, &tls);
  hop_seen_t seen[4];
  if (ok_a && trv == ccol_success) run_sensitive_redirect(cli, url, seen);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_EQ(trv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(seen[tier].rv, ccol_success);
    REQUIRE_EQ(seen[tier].status, 200);
    REQUIRE_FALSE(seen[tier].auth);
    REQUIRE_FALSE(seen[tier].cookie);
    REQUIRE_FALSE(seen[tier].cookie2);
    REQUIRE_FALSE(seen[tier].www_auth);
    REQUIRE_TRUE(seen[tier].keep);
    REQUIRE_STREQ(seen[tier].host, "custom.example");
  }
}

/* --------------------------- request-target ------------------------------ */

static ccol_retval_t parsed_target(const char *url, char **pq) {
  *pq = NULL;
  return _chttp_parse_url_for_tests(url, NULL, NULL, NULL, NULL, pq, NULL, NULL,
                                    NULL, NULL);
}

/* Every byte that RFC 3986 does not allow literally in a path or a query is
 * percent-encoded. This test is non-vacuous: without _build_request_target,
 * the space and every other byte below reach the request line raw. */
TEST(request_target, bytes_outside_rfc3986_are_percent_encoded) {
  static const struct {
    const char *url;
    const char *target;
  } cases[] = {
      {"http://h/my file", "/my%20file"},
      {"http://h/a\"b<c>d\\e^f`g{h|i}j",
       "/a%22b%3Cc%3Ed%5Ce%5Ef%60g%7Bh%7Ci%7Dj"},
      {"http://h/[x]", "/%5Bx%5D"},
      {"http://h/t\x01u\x7fv", "/t%01u%7Fv"},
      {"http://h/caf\xc3\xa9", "/caf%C3%A9"},
      {"http://h/s?q=a b&x=<1>", "/s?q=a%20b&x=%3C1%3E"},
      {"http://h?x y", "/?x%20y"},
      {"http://h/ok-._~!$&'()*+,;=:@/?/?", "/ok-._~!$&'()*+,;=:@/?/?"},
      {"http+unix://%2Ftmp%2Fs/p q", "/p%20q"},
  };
  size_t n = sizeof(cases) / sizeof(cases[0]);
  size_t matched = 0;
  for (size_t i = 0; i < n; i++) {
    char *pq = NULL;
    ccol_retval_t rv = parsed_target(cases[i].url, &pq);
    if (rv == ccol_success && pq && strcmp(pq, cases[i].target) == 0)
      matched++;
    else
      fprintf(stderr, "request_target mismatch for case %zu: got '%s'\n", i,
              pq ? pq : "(null)");
    free(pq);
  }
  REQUIRE_EQ(matched, n);
}

/* An existing escape is kept as it is, and never encoded a second time. */
TEST(request_target, existing_escapes_are_not_encoded_twice) {
  char *pq = NULL;
  REQUIRE_EQ(parsed_target("http://h/a%20b%2Fc?d=%7e%41", &pq), ccol_success);
  bool same = pq && strcmp(pq, "/a%20b%2Fc?d=%7e%41") == 0;
  free(pq);
  REQUIRE_TRUE(same);
}

/* A '%' that two hex digits do not follow is not a valid URL. */
TEST(request_target, stray_percent_is_invalid) {
  static const char *bad[] = {"http://h/100%", "http://h/a%zz", "http://h/a%2",
                              "http://h/?q=%"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *pq = NULL;
    ccol_retval_t rv = parsed_target(bad[i], &pq);
    free(pq);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
  }
}

/* The request line that reaches the server, for a caller URL and for a
 * Location, in every tier. */
static void run_target_check(const char *url, const char *expect_line,
                             ccol_retval_t rv_out[4], bool line_ok[4]) {
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  for (int tier = 1; tier <= 3; tier++) {
    rv_out[tier] = ccol_unexpected_failure;
    line_ok[tier] = false;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    rv_out[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp && resp->body)
      line_ok[tier] =
          strncmp(resp->body, expect_line, strlen(expect_line)) == 0;
    if (!line_ok[tier] && resp && resp->body)
      fprintf(stderr, "request line seen: %.80s\n", resp->body);
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
}

TEST(request_target, caller_url_is_encoded_on_the_wire_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/my file?x=a b", a.port);
  ccol_retval_t rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /my%20file?x=a%20b HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

/* A Location with a raw space, which a server that writes a filename into
 * the header produces, must not split the request line. This test is
 * non-vacuous: without the encoding, the request line reads
 * "GET /my file HTTP/1.0 HTTP/1.1". */
TEST(request_target, location_is_encoded_on_the_wire_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  snprintf(g_hs_raw_location, sizeof(g_hs_raw_location), "/my file HTTP/1.0");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/raw-location", a.port);
  ccol_retval_t rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /my%20file%20HTTP/1.0 HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

TEST(request_target, non_ascii_location_is_encoded_in_every_tier) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  snprintf(g_hs_raw_location, sizeof(g_hs_raw_location),
           "/caf\xc3\xa9?q=\xe2\x82\xac%%41");
  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/raw-location", a.port);
  ccol_retval_t rv[4];
  bool ok[4];
  if (ok_a)
    run_target_check(url, "GET /caf%C3%A9?q=%E2%82%AC%41 HTTP/1.1\r\n", rv, ok);
  if (ok_a) hs_stop(&a);
  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_TRUE(ok[tier]);
  }
}

/* --------------------- TLS input held above the socket -------------------- */

extern void _chttp_set_async_tls_first_read_cap_for_tests(size_t cap);

/* The server sends its whole response as one TLS record and then keeps the
 * connection open. Part of that record stays inside the TLS layer of the
 * client after the first read, and the socket has nothing more to report.
 * Tier 1 reads with a buffer smaller than the record. Tier 2 and Tier 3
 * take a whole record in one read, so the first read of each dispatch is
 * capped here to leave the same remainder.
 *
 * The response says "Connection: close", so a client that has it all closes
 * at once, and the server counts that. A client that waits for the socket
 * instead gets the rest only when the server gives up and closes, and the
 * server counts an expired hold. This test is non-vacuous: without the
 * check of ctls_conn_has_pending_input, every tier ends by an expired
 * hold. */
TEST(tls_buffered_input, response_left_inside_the_tls_layer_is_read) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  int closed_before = atomic_load(&g_one_record_closed_by_client);
  int expired_before = atomic_load(&g_one_record_hold_expired);
  chttpcli_construct(cli);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval_t trv = chttpclient_set_tls(cli, &tls);
  _chttp_set_async_tls_first_read_cap_for_tests(1024);

  char url[160];
  make_tls_url(url, sizeof(url), "/one-record-held-open");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv[4] = {0};
  size_t body_len[4] = {0};
  for (int tier = 1; tier <= 3 && req && trv == ccol_success; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    body_len[tier] = resp ? resp->body_len : 0;
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  _chttp_set_async_tls_first_read_cap_for_tests(0);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  /* The server thread counts the close a moment after the client makes it.
   * This bounded wait only covers that lag. */
  for (int i = 0; i < 2 * ONE_RECORD_HOLD_MS / 10; i++) {
    if (atomic_load(&g_one_record_closed_by_client) +
            atomic_load(&g_one_record_hold_expired) - closed_before -
            expired_before >=
        3)
      break;
    usleep(10000);
  }
  int closed = atomic_load(&g_one_record_closed_by_client) - closed_before;
  int expired = atomic_load(&g_one_record_hold_expired) - expired_before;

  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_GT(body_len[tier], (size_t)8192);
  }
  REQUIRE_EQ(expired, 0);
  REQUIRE_EQ(closed, 3);
}

/* ----------------------- TLS client creation codes ------------------------ */

extern ccol_retval_t _chttp_tls_client_create_code_for_tests(const char *host,
                                                             bool verify_host);

/* ctls refuses to verify a name when it has none. That refusal is not a
 * shortage of memory, and the request reports ccol_http_invalid_url for it.
 * This test is non-vacuous: without _tls_client_create_failure_code, every
 * refusal reads as ccol_not_enough_memory. */
TEST(tls_client_create, missing_name_with_verification_is_invalid_url) {
  ccol_retval_t empty = _chttp_tls_client_create_code_for_tests("", true);
  ccol_retval_t null_host = _chttp_tls_client_create_code_for_tests(NULL, true);
  ccol_retval_t skipped = _chttp_tls_client_create_code_for_tests("", false);
  ccol_retval_t named =
      _chttp_tls_client_create_code_for_tests("example.com", true);
  ccol_retval_t ip = _chttp_tls_client_create_code_for_tests("127.0.0.1", true);
  REQUIRE_EQ(empty, ccol_http_invalid_url);
  REQUIRE_EQ(null_host, ccol_http_invalid_url);
  REQUIRE_EQ(skipped, ccol_success);
  REQUIRE_EQ(named, ccol_success);
  REQUIRE_EQ(ip, ccol_success);
}

/* ------------------- response framing and keep-alive ---------------------- */

/* Runs the same request twice in `tier` and gives back how many connections
 * the server accepted for the pair. */
static int accepts_for_two_requests(int tier, hs_srv_t *srv, const char *path,
                                    ccol_retval_t rv[2], size_t body_len[2]) {
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d%s", srv->port, path);
  int before = atomic_load(&srv->accepts);
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  for (int i = 0; i < 2; i++) {
    rv[i] = ccol_unexpected_failure;
    body_len[i] = 0;
    if (!req) continue;
    chttpcli_response *resp = NULL;
    rv[i] = run_in_tier(tier, cli, req, &resp);
    body_len[i] = resp ? resp->body_len : 0;
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  return atomic_load(&srv->accepts) - before;
}

/* RFC 9112 section 6.1: the framing of an HTTP/1.0 message with
 * Transfer-Encoding is faulty, and the recipient closes the connection after
 * it. The client reads the body and never reuses the connection, in every
 * tier. The HTTP/1.1 control proves that the same measurement sees a reuse
 * when there is one. */
TEST(response_framing, http10_transfer_encoding_is_never_kept_alive) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  int te_accepts[4] = {0}, ka_accepts[4] = {0};
  ccol_retval_t te_rv[4][2], ka_rv[4][2];
  size_t te_len[4][2], ka_len[4][2];
  for (int tier = 1; tier <= 3 && ok_a; tier++) {
    te_accepts[tier] = accepts_for_two_requests(tier, &a, "/http10-te",
                                                te_rv[tier], te_len[tier]);
    ka_accepts[tier] = accepts_for_two_requests(tier, &a, "/http11-ka",
                                                ka_rv[tier], ka_len[tier]);
  }
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  for (int tier = 1; tier <= 3; tier++) {
    for (int i = 0; i < 2; i++) {
      REQUIRE_EQ(te_rv[tier][i], ccol_success);
      REQUIRE_EQ(te_len[tier][i], (size_t)5);
      REQUIRE_EQ(ka_rv[tier][i], ccol_success);
      REQUIRE_EQ(ka_len[tier][i], (size_t)5);
    }
    REQUIRE_EQ(te_accepts[tier], 2);
    REQUIRE_EQ(ka_accepts[tier], 1);
  }
}

/* A chunk extension must follow the grammar of RFC 9112 section 7.1.1. A
 * control byte inside one makes the response malformed. */
TEST(response_framing, malformed_chunk_extension_is_refused) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bad-chunk-ext", a.port);
  ccol_retval_t rv[4] = {0};
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  for (int tier = 1; tier <= 3 && ok_a && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_transfer_aborted);
}

/* ------------------ a deadline that passes at a read point ---------------- */

extern void _chttp_set_expire_at_read_for_tests(bool on);

/* The same timeline in every tier: the whole response is already in the
 * socket, and the overall deadline has passed by the time the client reads
 * it. Every tier checks the deadline before it reads, so every tier reports
 * ccol_timed_out, and none of them completes the response. This test is
 * non-vacuous: without the check at the start of the read dispatch of Tier
 * 2, Tier 2 and Tier 3 read the buffered response and report success. */
TEST(read_deadline, every_tier_times_out_on_an_already_buffered_response) {
  hs_srv_t a;
  bool ok_a = hs_start(&a, "127.0.0.1");
  char url[160];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/final", a.port);
  chttpcli_construct(cli);
  ccol_retval_t torv = chttpclient_set_request_timeout(cli, 60000000);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv[4] = {0};
  bool got_resp[4] = {0};
  _chttp_set_expire_at_read_for_tests(true);
  for (int tier = 1; tier <= 3 && ok_a && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    got_resp[tier] = (resp != NULL);
    chttpclient_resp_free(resp);
  }
  _chttp_set_expire_at_read_for_tests(false);
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  if (ok_a) hs_stop(&a);

  REQUIRE_TRUE(ok_a);
  REQUIRE_TRUE(have_req);
  REQUIRE_EQ(torv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], rv[1]);
    REQUIRE_EQ(rv[tier], ccol_timed_out);
    REQUIRE_FALSE(got_resp[tier]);
  }
}

/* ========================================================================== */
/*          RESPONSES AND RESULTS OUTLIVE THEIR CLIENT                        */
/* ========================================================================== */

/* A custom allocator that never gives a freed block back while a test runs.
 * It fills each freed block with pointers to quarantine_trap_free and keeps
 * it. Any later call through an allocator that lived in a freed block then
 * lands in quarantine_trap_free, which counts the call and frees the pointer
 * in the normal way. A read of the procs of a destroyed client is therefore
 * a count, and not a crash. The blocks stay allocated until
 * quarantine_drain, so valgrind sees no access to freed memory either way. */
/* The header is padded to max_align_t, so the block after it keeps the
 * alignment that malloc() gives. The library relies on that, as it relies on
 * it for any allocator. Two bare fields make an 8-byte header on a 32-bit
 * target, where malloc() aligns to 16. */
typedef union quarantine_hdr {
  struct {
    size_t size;
    union quarantine_hdr *next;
  } h;
  max_align_t align;
} quarantine_hdr;

static pthread_mutex_t g_quarantine_lock = PTHREAD_MUTEX_INITIALIZER;
static quarantine_hdr *g_quarantine_list = NULL;
static atomic_uint g_quarantine_trap_calls;

static void quarantine_free(void *p);

static void quarantine_trap_free(void *p) {
  atomic_fetch_add(&g_quarantine_trap_calls, 1u);
  quarantine_free(p);
}

static void *quarantine_malloc(size_t n) {
  quarantine_hdr *h = (quarantine_hdr *)malloc(sizeof(*h) + n);
  if (!h) return NULL;
  h->h.size = n;
  h->h.next = NULL;
  return h + 1;
}

static void *quarantine_calloc(size_t c, size_t n) {
  if (n && c > SIZE_MAX / n) return NULL;
  void *p = quarantine_malloc(c * n);
  if (p) memset(p, 0, c * n);
  return p;
}

static void quarantine_free(void *p) {
  if (!p) return;
  quarantine_hdr *h = (quarantine_hdr *)p - 1;
  void (*trap)(void *) = quarantine_trap_free;
  for (size_t off = 0; off + sizeof(trap) <= h->h.size; off += sizeof(trap))
    memcpy((char *)p + off, &trap, sizeof(trap));
  pthread_mutex_lock(&g_quarantine_lock);
  h->h.next = g_quarantine_list;
  g_quarantine_list = h;
  pthread_mutex_unlock(&g_quarantine_lock);
}

static void *quarantine_realloc(void *p, size_t n) {
  if (!p) return quarantine_malloc(n);
  quarantine_hdr *h = (quarantine_hdr *)p - 1;
  void *q = quarantine_malloc(n);
  if (!q) return NULL;
  memcpy(q, p, h->h.size < n ? h->h.size : n);
  quarantine_free(p);
  return q;
}

static void quarantine_drain(void) {
  pthread_mutex_lock(&g_quarantine_lock);
  quarantine_hdr *h = g_quarantine_list;
  g_quarantine_list = NULL;
  pthread_mutex_unlock(&g_quarantine_lock);
  while (h) {
    quarantine_hdr *next = h->h.next;
    free(h);
    h = next;
  }
}

static ccol_memmgmt_procs_t g_quarantine_mp = {.malloc = quarantine_malloc,
                                               .free = quarantine_free,
                                               .calloc = quarantine_calloc,
                                               .realloc = quarantine_realloc};

TEST(lifetime, a_response_freed_after_its_client_uses_no_freed_allocator) {
  atomic_store(&g_quarantine_trap_calls, 0u);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttpcli cli = ccol_create_chttpclient_mp(&g_quarantine_mp, NULL);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = ccol_unexpected_failure;
  if (cli != CHTTPCLI_INVALID && req) rv = chttpclient_do(cli, req, &resp);
  chttp_request_free(req);
  int status = resp ? resp->status_code : -1;
  chttpclient_destroy(cli);
  /* The client and its copy of the procs are gone here. */
  chttpclient_resp_free(resp);
  unsigned traps = atomic_load(&g_quarantine_trap_calls);
  quarantine_drain();
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(traps, 0u);
}

TEST(lifetime, an_async_result_freed_after_its_client_uses_no_freed_allocator) {
  atomic_store(&g_quarantine_trap_calls, 0u);
  char url[160];
  make_url(url, sizeof(url), "/get");
  chttpcli cli = ccol_create_chttpclient_mp(&g_quarantine_mp, NULL);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ctpool_future *f = NULL;
  if (cli != CHTTPCLI_INVALID && req) f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  chttpcli_async_result_t *res = f ? chttpclient_async_result_get(f) : NULL;
  ccol_retval_t rv = res ? res->rv : ccol_unexpected_failure;
  int status = (res && res->resp) ? res->resp->status_code : -1;
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  /* The client and its copy of the procs are gone here. */
  if (res) chttpclient_resp_free(res->resp);
  chttpclient_async_result_free(res);
  ctpool_future_free(f);
  unsigned traps = atomic_load(&g_quarantine_trap_calls);
  quarantine_drain();
  REQUIRE_NE((void *)f, NULL);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(traps, 0u);
}

/* ========================================================================== */
/*          A SOCKET ERROR IN THE MIDDLE OF A RESPONSE                        */
/* ========================================================================== */

TEST(mid_transfer_error, every_tier_reports_a_reset_as_an_aborted_transfer) {
  /* The server sends the head and part of the body, then resets the
   * connection while the client waits for the rest. The connection was
   * established, so this is an aborted transfer and not a failed connect,
   * and every tier must say so with the same code. */
  char url[160];
  make_url(url, sizeof(url), "/partial-then-reset");
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv[4] = {ccol_success, ccol_success, ccol_success,
                         ccol_success};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++)
    REQUIRE_EQ(rv[tier], ccol_http_transfer_aborted);
}

/* ========================================================================== */
/*          A 3XX WITH AN EMPTY LOCATION IS THE FINAL RESPONSE                */
/* ========================================================================== */

TEST(redirect_policy, an_empty_location_is_delivered_in_every_tier) {
  char url[160];
  make_url(url, sizeof(url), "/redirect-empty-location");
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  for (int tier = 1; tier <= 3 && req; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : -1;
    chttpclient_resp_free(resp);
  }
  bool have_req = (req != NULL);
  chttp_request_free(req);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  REQUIRE_TRUE(have_req);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 301);
  }
}

/* ========================================================================== */
/*          AN ORIGIN COMPARES ITS HOST WITHOUT REGARD TO CASE                */
/* ========================================================================== */

static bool upper_localhost_resolves(void) {
  struct addrinfo hints = {.ai_socktype = SOCK_STREAM};
  struct addrinfo *res = NULL;
  bool ok = getaddrinfo("LOCALHOST", "80", &hints, &res) == 0 && res;
  if (res) freeaddrinfo(res);
  return ok;
}

TEST(credentials,
     a_redirect_that_changes_only_the_case_of_the_host_keeps_them) {
  /* http://localhost:<port> and http://LOCALHOST:<port> are one origin. The
   * userinfo credentials, and an Authorization header that the caller set,
   * therefore follow the redirect in every tier. */
  if (!upper_localhost_resolves()) {
    fprintf(stderr,
            "[SKIP] a_redirect_that_changes_only_the_case_of_the_host_keeps_"
            "them: LOCALHOST does not resolve here\n");
    return;
  }
  char userinfo_url[192];
  snprintf(userinfo_url, sizeof(userinfo_url),
           "http://alice:s3cr3t@localhost:%d"
           "/redirect-to-echo-auth-upper-localhost",
           get_test_port());
  char plain_url[192];
  snprintf(plain_url, sizeof(plain_url),
           "http://localhost:%d/redirect-to-echo-auth-upper-localhost",
           get_test_port());
  chttpcli_construct(cli);
  chttp_request_t *ureq =
      chttp_request_new(CHTTP_GET, userinfo_url, NULL, NULL);
  chttp_request_t *hreq = chttp_request_new(CHTTP_GET, plain_url, NULL, NULL);
  ccol_retval_t hrv =
      hreq ? chttp_request_set_header(hreq, "Authorization", "Bearer mytoken")
           : ccol_unexpected_failure;
  char ubody[4][64] = {{0}};
  char hbody[4][64] = {{0}};
  ccol_retval_t urv[4] = {0}, hrvs[4] = {0};
  for (int tier = 1; tier <= 3 && ureq && hreq; tier++) {
    chttpcli_response *resp = NULL;
    urv[tier] = run_in_tier(tier, cli, ureq, &resp);
    if (resp && resp->body)
      snprintf(ubody[tier], sizeof(ubody[tier]), "%s", resp->body);
    chttpclient_resp_free(resp);
    resp = NULL;
    hrvs[tier] = run_in_tier(tier, cli, hreq, &resp);
    if (resp && resp->body)
      snprintf(hbody[tier], sizeof(hbody[tier]), "%s", resp->body);
    chttpclient_resp_free(resp);
  }
  bool have_reqs = ureq && hreq;
  chttp_request_free(ureq);
  chttp_request_free(hreq);
  wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  REQUIRE_TRUE(have_reqs);
  REQUIRE_EQ(hrv, ccol_success);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(urv[tier], ccol_success);
    REQUIRE_STREQ(ubody[tier], "Basic YWxpY2U6czNjcjN0");
    REQUIRE_EQ(hrvs[tier], ccol_success);
    REQUIRE_STREQ(hbody[tier], "Bearer mytoken");
  }
}

/* ========================================================================== */
/*          A DEADLINE FAR AWAY                                               */
/* ========================================================================== */

extern bool _chttp_deadline_remaining_ms_for_tests(long long add_sec,
                                                   int *out_ms);

TEST(deadline, a_deadline_whose_milliseconds_overflow_a_long_saturates) {
  /* LONG_MAX / 1000 + 1 seconds is more milliseconds than a long holds, at
   * every width of long. The remaining time must saturate at INT_MAX and
   * must not read as a deadline that already passed. */
  int ms = 0;
  bool live = _chttp_deadline_remaining_ms_for_tests(
      (long long)(LONG_MAX / 1000) + 1, &ms);
  REQUIRE_TRUE(live);
  REQUIRE_EQ(ms, INT_MAX);

  ms = 0;
  live = _chttp_deadline_remaining_ms_for_tests(-5, &ms);
  REQUIRE_FALSE(live);

  ms = 0;
  live = _chttp_deadline_remaining_ms_for_tests(10, &ms);
  REQUIRE_TRUE(live);
  REQUIRE_GT(ms, 9000);
  REQUIRE_LE(ms, 10000);
}

/* ========================================================================== */
/*          REPEATED RESPONSE FIELDS                                          */
/* ========================================================================== */

/* This runs one GET for url through Tier 1 (chttpclient_do, tier value 0),
 * Tier 2 (chttpclient_do_async, 1) or Tier 3 (chttpclient_do_pooled, 2), on its
 * own client, and hands back the result code and the response. */
static ccol_retval_t repeated_fields_get(int tier, const char *url,
                                         chttpcli_response **resp_out) {
  *resp_out = NULL;
  char *err = NULL;
  chttpcli cli = ccol_create_chttpclient(&err);
  if (cli == CHTTPCLI_INVALID) return ccol_unexpected_failure;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpclient_destroy(cli);
    return ccol_not_enough_memory;
  }
  ccol_retval_t rv = ccol_unexpected_failure;
  if (tier == 0) {
    rv = chttpclient_do(cli, req, resp_out);
  } else if (tier == 1) {
    ctpool_future *f = chttpclient_do_async(cli, req);
    if (f) {
      chttpcli_async_result_t *res = chttpclient_async_result_get(f);
      if (res) {
        rv = res->rv;
        *resp_out = res->resp;
        res->resp = NULL;
        chttpclient_async_result_free(res);
      }
      ctpool_future_free(f);
    }
  } else {
    rv = chttpclient_do_pooled(cli, req, resp_out);
  }
  chttp_request_free(req);
  if (tier != 0) wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  return rv;
}

/* The checks run after chttpclient_destroy, so they also pin that every
 * value stays readable once the client is gone. */
static void repeated_fields_check(int tier, int *failures) {
  char url[160];
  make_url(url, sizeof(url), "/repeated-fields");
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = repeated_fields_get(tier, url, &resp);
#define RF_EXPECT(cond)                                            \
  do {                                                             \
    if (!(cond)) {                                                 \
      fprintf(stderr, "tier %d: check failed: %s\n", tier, #cond); \
      (*failures)++;                                               \
    }                                                              \
  } while (0)
#define RF_STREQ(a, b) RF_EXPECT((a) != NULL && strcmp((a), (b)) == 0)
  RF_EXPECT(rv == ccol_success);
  RF_EXPECT(resp != NULL);
  if (rv == ccol_success && resp) {
    RF_STREQ(chttpclient_resp_header(resp, "X-Multi"), "one, two, three");
    RF_STREQ(chttpclient_resp_header(resp, "x-single"), "s");
    RF_STREQ(chttpclient_resp_header(resp, "X-Empty-Twice"), ", ");
    RF_STREQ(chttpclient_resp_header(resp, "Set-Cookie"), "a=1; Path=/");

    RF_EXPECT(chttpclient_resp_header_count(resp, "x-multi") == 3);
    RF_STREQ(chttpclient_resp_header_at(resp, "X-MULTI", 0), "one");
    RF_STREQ(chttpclient_resp_header_at(resp, "X-MULTI", 1), "two");
    RF_STREQ(chttpclient_resp_header_at(resp, "X-MULTI", 2), "three");
    RF_EXPECT(chttpclient_resp_header_at(resp, "X-MULTI", 3) == NULL);

    RF_EXPECT(chttpclient_resp_header_count(resp, "set-cookie") == 2);
    RF_STREQ(chttpclient_resp_header_at(resp, "Set-Cookie", 0), "a=1; Path=/");
    RF_STREQ(chttpclient_resp_header_at(resp, "Set-Cookie", 1),
             "b=2; Expires=Wed, 21 Oct 2037 07:28:00 GMT");
    RF_EXPECT(chttpclient_resp_header_at(resp, "Set-Cookie", 2) == NULL);

    RF_EXPECT(chttpclient_resp_header_count(resp, "X-Single") == 1);
    RF_STREQ(chttpclient_resp_header_at(resp, "X-Single", 0), "s");
    RF_EXPECT(chttpclient_resp_header_at(resp, "X-Single", 1) == NULL);

    RF_EXPECT(chttpclient_resp_header_count(resp, "x-empty-twice") == 2);
    RF_STREQ(chttpclient_resp_header_at(resp, "x-empty-twice", 1), "");

    RF_EXPECT(chttpclient_resp_header_count(resp, "X-Absent") == 0);
    RF_EXPECT(chttpclient_resp_header_at(resp, "X-Absent", 0) == NULL);

    /* The public map holds the same value that the lookup gives. */
    const char *direct = chmap_get(resp->headers, "x-multi");
    RF_STREQ(direct, "one, two, three");
  }
#undef RF_STREQ
#undef RF_EXPECT
  chttpclient_resp_free(resp);
}

TEST(repeated_fields, combined_in_order_except_set_cookie_on_every_tier) {
  int failures = 0;
  for (int tier = 0; tier < 3; tier++) repeated_fields_check(tier, &failures);
  REQUIRE_EQ(failures, 0);
}

TEST(repeated_fields, null_arguments_and_streaming_results) {
  REQUIRE_EQ(chttpclient_resp_header_count(NULL, "x"), (size_t)0);
  REQUIRE_EQ((void *)chttpclient_resp_header_at(NULL, "x", 0), NULL);

  char url[160];
  make_url(url, sizeof(url), "/repeated-fields");
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  size_t n = chttpclient_resp_header_count(resp, NULL);
  const char *v = chttpclient_resp_header_at(resp, NULL, 0);
  /* A name longer than any stack buffer takes the heap path of the lookup. */
  char long_name[300];
  memset(long_name, 'x', sizeof(long_name) - 1);
  long_name[sizeof(long_name) - 1] = '\0';
  size_t n_long = chttpclient_resp_header_count(resp, long_name);
  const char *v_long = chttpclient_resp_header_at(resp, long_name, 0);
  chttpclient_resp_free(resp);
  REQUIRE_EQ(n, (size_t)0);
  REQUIRE_EQ((void *)v, NULL);
  REQUIRE_EQ(n_long, (size_t)0);
  REQUIRE_EQ((void *)v_long, NULL);
}

/* Tier 1, 2 and 3 of the streaming path, on its own client: the status and
 * the count of body bytes. */
static ccol_retval_t long_header_stream(int tier, const char *url,
                                        int *status_out, size_t *len_out) {
  *status_out = 0;
  *len_out = 0;
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli == CHTTPCLI_INVALID) return ccol_unexpected_failure;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpclient_destroy(cli);
    return ccol_not_enough_memory;
  }
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval_t rv = ccol_unexpected_failure;
  if (tier == 0) {
    rv = chttpclient_do_streaming(cli, req, stream_sink_write, &sink,
                                  status_out);
  } else if (tier == 1) {
    ctpool_future *f =
        chttpclient_do_async_streaming(cli, req, stream_sink_write, &sink);
    if (f) {
      chttpcli_async_result_t *res = chttpclient_async_result_get(f);
      if (res) {
        rv = res->rv;
        if (res->resp) *status_out = res->resp->status_code;
        chttpclient_resp_free(res->resp);
        chttpclient_async_result_free(res);
      }
      ctpool_future_free(f);
    }
  } else {
    rv = chttpclient_do_pooled_streaming(cli, req, stream_sink_write, &sink,
                                         status_out);
  }
  chttp_request_free(req);
  if (tier != 0) wait_for_async_engine_idle();
  chttpclient_destroy(cli);
  *len_out = sink.len;
  return rv;
}

TEST(repeated_fields, a_header_line_longer_than_the_fixed_parser_buffer) {
  /* A response header line may be longer than the fixed line buffer of the
   * parser, up to CHTTP1_MAX_SPILL_LINE_LEN (64 KiB), on every tier,
   * buffered and streaming, behind a 103 whose own line is that long, and
   * on a redirect hop whose line is that long. A line past the limit is
   * refused cleanly. Under make memtest this also shows that no heap line
   * buffer outlives its message on any of those paths. This test is
   * non-vacuous: without the opt-in to the heap line buffer of the parser,
   * the 40000-byte line fails with ccol_http_transfer_aborted. */
  static const char *const ok_paths[] = {"/long-header-line/40000",
                                         "/long-header-hint/40000",
                                         "/long-header-redir/40000"};
  enum { NPATHS = 3 };
  ccol_retval_t ok_rv[NPATHS][3], long_rv[3], stream_rv[3];
  size_t ok_len[NPATHS][3];
  bool tail_ok[NPATHS][3];
  int stream_status[3];
  size_t stream_len[3];
  char url[160];
  for (int p = 0; p < NPATHS; p++)
    for (int tier = 0; tier < 3; tier++) {
      make_url(url, sizeof(url), ok_paths[p]);
      chttpcli_response *resp = NULL;
      ok_len[p][tier] = 0;
      tail_ok[p][tier] = false;
      ok_rv[p][tier] = repeated_fields_get(tier, url, &resp);
      if (ok_rv[p][tier] == ccol_success && resp) {
        const char *v0 = chttpclient_resp_header_at(resp, "x-long", 0);
        const char *v1 = chttpclient_resp_header_at(resp, "x-long", 1);
        const char *all = chttpclient_resp_header(resp, "X-Long");
        ok_len[p][tier] = v0 ? strlen(v0) : 0;
        tail_ok[p][tier] =
            v1 && strcmp(v1, "tail") == 0 && all && strlen(all) == 40000 + 6;
      }
      chttpclient_resp_free(resp);
    }
  for (int tier = 0; tier < 3; tier++) {
    make_url(url, sizeof(url), "/long-header-line/70000");
    chttpcli_response *resp = NULL;
    long_rv[tier] = repeated_fields_get(tier, url, &resp);
    chttpclient_resp_free(resp);
    make_url(url, sizeof(url), "/long-header-line/40000");
    stream_rv[tier] =
        long_header_stream(tier, url, &stream_status[tier], &stream_len[tier]);
  }
  for (int tier = 0; tier < 3; tier++) {
    for (int p = 0; p < NPATHS; p++) {
      REQUIRE_EQ(ok_rv[p][tier], ccol_success);
      REQUIRE_EQ(ok_len[p][tier], (size_t)40000);
      REQUIRE_TRUE(tail_ok[p][tier]);
    }
    REQUIRE_NE(long_rv[tier], ccol_success);
    REQUIRE_EQ(stream_rv[tier], ccol_success);
    REQUIRE_EQ(stream_status[tier], 200);
    REQUIRE_EQ(stream_len[tier], (size_t)2);
  }
}

extern unsigned long _chttpclient_field_rec_reads_for_tests(void);

TEST(repeated_fields, work_is_linear_in_the_number_of_occurrences) {
  /* 44 repeated names and one more whose first value is empty. Recording
   * every occurrence, combining every name and answering a count and an
   * index query for every name must each touch each record a bounded
   * number of times. The bound, 12 reads for each record, sits far below
   * what a rescan of the block for each occurrence costs, which is several
   * thousand reads here. This test is non-vacuous: a lookup that walks all
   * the records for each new occurrence fails it. */
  char url[160];
  make_url(url, sizeof(url), "/many-repeated-fields");
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  unsigned long before = _chttpclient_field_rec_reads_for_tests();
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  int wrong = 0;
  if (rv == ccol_success && resp) {
    for (int i = 0; i < 44; i++) {
      char name[16], want[32], a[8], b[8];
      snprintf(name, sizeof(name), "x-r%02d", i);
      snprintf(a, sizeof(a), "a%d", i);
      snprintf(b, sizeof(b), "b%d", i);
      snprintf(want, sizeof(want), "%s, %s", a, b);
      const char *v = chttpclient_resp_header(resp, name);
      const char *v0 = chttpclient_resp_header_at(resp, name, 0);
      const char *v1 = chttpclient_resp_header_at(resp, name, 1);
      if (!v || strcmp(v, want) != 0) wrong++;
      if (!v0 || strcmp(v0, a) != 0) wrong++;
      if (!v1 || strcmp(v1, b) != 0) wrong++;
      if (chttpclient_resp_header_count(resp, name) != 2) wrong++;
    }
    const char *e = chttpclient_resp_header(resp, "X-E");
    if (!e || strcmp(e, ", z") != 0) wrong++;
    const char *e0 = chttpclient_resp_header_at(resp, "x-e", 0);
    if (!e0 || strcmp(e0, "") != 0) wrong++;
  }
  unsigned long reads = _chttpclient_field_rec_reads_for_tests() - before;
  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);

  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(wrong, 0);
  /* 90 records. */
  REQUIRE_LT(reads, 90ul * 12ul);
}

TEST(repeated_fields, a_redirect_with_two_locations_follows_the_first) {
  int status[3] = {0, 0, 0};
  ccol_retval_t rvs[3];
  char url[160];
  make_url(url, sizeof(url), "/redirect-two-locations");
  for (int tier = 0; tier < 3; tier++) {
    chttpcli_response *resp = NULL;
    rvs[tier] = repeated_fields_get(tier, url, &resp);
    status[tier] = resp ? resp->status_code : -1;
    chttpclient_resp_free(resp);
  }
  /* The streaming path of Tier 1 reads Location from the same parse. */
  chttpcli cli = ccol_create_chttpclient(NULL);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  int streamed_status = 0;
  stream_sink_t sink;
  memset(&sink, 0, sizeof(sink));
  ccol_retval_t srv =
      req ? chttpclient_do_streaming(cli, req, stream_sink_write, &sink,
                                     &streamed_status)
          : ccol_not_enough_memory;
  chttp_request_free(req);
  chttpclient_destroy(cli);
  for (int tier = 0; tier < 3; tier++) {
    REQUIRE_EQ(rvs[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
  }
  REQUIRE_EQ(srv, ccol_success);
  REQUIRE_EQ(streamed_status, 200);
}

/* ========================================================================== */
/*          ENGINE LOGGER ACROSS ENGINE RESTARTS                              */
/* ========================================================================== */

static int count_lines_containing(const char *path, const char *needle) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char line[4096];
  int n = 0;
  while (fgets(line, sizeof(line), f))
    if (strstr(line, needle)) n++;
  fclose(f);
  return n;
}

TEST(engine_logger, the_installed_logger_survives_every_engine_restart) {
  /* The file lives in the working directory, on the filesystem that a log
   * normally goes to. */
  const char *path = "chttpclient_engine_logger_restart.log";
  int fd = open(path, O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, 0644);
  REQUIRE_GE(fd, 0);
  wait_for_async_engine_idle();

  clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
  ccol_retval_t set_rv =
      lg ? chttpcli_set_engine_logger(lg) : ccol_invalid_args;
  /* The engine holds a derived logger, which keeps the destination of lg
   * open after the caller closes lg itself. */
  if (lg) clog_close(lg);

  char url[128];
  make_url(url, sizeof(url), "/get");
  enum { CYCLES = 3 };
  int ok = 0;
  for (int i = 0; i < CYCLES && set_rv == ccol_success; i++) {
    chttpcli cli = ccol_create_chttpclient(NULL);
    chttp_request_t *req =
        cli ? chttp_request_new(CHTTP_GET, url, NULL, NULL) : NULL;
    ctpool_future *f = req ? chttpclient_do_async(cli, req) : NULL;
    chttpcli_async_result_t *res = f ? chttpclient_async_result_get(f) : NULL;
    if (res && res->rv == ccol_success && res->resp &&
        res->resp->status_code == 200)
      ok++;
    if (res) {
      chttpclient_resp_free(res->resp);
      chttpclient_async_result_free(res);
    }
    if (f) ctpool_future_free(f);
    chttp_request_free(req);
    if (cli) chttpclient_destroy(cli);
    /* The engine stops completely between two cycles. */
    wait_for_async_engine_idle();
  }

  /* A quiet logger replaces this one, so that nothing later in this binary
   * writes into the file. */
  clog quiet = clog_open_fd(2, CLOG_FATAL, NULL);
  ccol_retval_t quiet_rv =
      quiet ? chttpcli_set_engine_logger(quiet) : ccol_invalid_args;
  if (quiet) clog_close(quiet);
  close(fd);

  int created = count_lines_containing(
      path, "New http client reactor engine has been created");
  int destroyed = count_lines_containing(
      path, "The http client reactor engine has been destroyed");
  unlink(path);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_EQ(quiet_rv, ccol_success);
  REQUIRE_EQ(ok, CYCLES);
  REQUIRE_EQ(created, CYCLES);
  REQUIRE_EQ(destroyed, CYCLES);
}

/* ========================================================================== */
/*          A SWEEP VERDICT AGAINST A HOP THAT FINISHED                       */
/* ========================================================================== */

extern void _chttp_sweep_race_arm_for_tests(void);
extern int _chttp_sweep_race_parked_phase_for_tests(void);
extern void _chttp_sweep_race_release_for_tests(int phase);
extern void _chttp_sweep_race_disarm_for_tests(void);
extern unsigned _chttp_offer_entered_count_for_tests(void);
extern unsigned _chttp_offer_done_count_for_tests(void);
extern size_t _chttpclient_async_idle_timed_out_count_for_tests(
    struct chttpclient *cli);

static bool sweep_race_wait_for_phase(int phase, int max_ms) {
  for (int i = 0; i < max_ms; i++) {
    if (_chttp_sweep_race_parked_phase_for_tests() == phase) return true;
    usleep(1000);
  }
  return false;
}

static bool sweep_race_wait_for_count(unsigned (*read)(void), unsigned above,
                                      int max_ms) {
  for (int i = 0; i < max_ms; i++) {
    if (read() > above) return true;
    usleep(1000);
  }
  return false;
}

TEST(deadline_sweep, a_verdict_on_a_finished_hop_never_marks_its_pooled_ctx) {
  /* The sweep judges a hop in CHTTP_ASYNC_READING as expired and parks
   * before it writes timed_out. The response then arrives, the dispatch
   * finishes the hop and offers the connection to the idle pool, and the
   * sweep writes its verdict afterwards. The pooled connection must not
   * carry that verdict: a request that reuses it would report
   * ccol_timed_out in place of the retry of a dead connection.
   *
   * This test is non-vacuous: without the offer and the verdict sharing
   * deadline_lock, the offer completes while the sweep is parked, the sweep
   * then marks the idle ctx, and the count below is 1. */
  wait_for_async_engine_idle();
  atomic_store(&g_sweep_race_respond, false);
  char url[128];
  make_url(url, sizeof(url), "/sweep-race");
  chttpcli cli = ccol_create_chttpclient(NULL);
  REQUIRE_NE(cli, CHTTPCLI_INVALID);
  REQUIRE_EQ(chttpclient_set_request_timeout(cli, 30000000), ccol_success);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  unsigned entered0 = _chttp_offer_entered_count_for_tests();
  unsigned done0 = _chttp_offer_done_count_for_tests();
  _chttp_sweep_race_arm_for_tests();
  ctpool_future *f = chttpclient_do_async(cli, req);
  bool parked1 = f && sweep_race_wait_for_phase(1, 5000);
  atomic_store(&g_sweep_race_respond, true);
  bool entered =
      parked1 && sweep_race_wait_for_count(_chttp_offer_entered_count_for_tests,
                                           entered0, 5000);
  /* An offer that does not wait for the verdict finishes within a few
   * instructions of its entry. An offer that does wait never finishes
   * here. The bound only gives the first kind the time to finish. */
  (void)sweep_race_wait_for_count(_chttp_offer_done_count_for_tests, done0,
                                  300);
  _chttp_sweep_race_release_for_tests(1);
  bool parked2 = entered && sweep_race_wait_for_phase(2, 5000);

  chttpcli_async_result_t *res = f ? chttpclient_async_result_get(f) : NULL;
  ccol_retval_t rv = res ? res->rv : ccol_unexpected_failure;
  struct chttpclient *raw = _chttpcli_resolve_for_tests(cli);
  size_t idle = raw ? _chttpclient_async_idle_total_count_for_tests(raw) : 0;
  size_t marked =
      raw ? _chttpclient_async_idle_timed_out_count_for_tests(raw) : 99;
  _chttp_sweep_race_disarm_for_tests();

  if (res) {
    chttpclient_resp_free(res->resp);
    chttpclient_async_result_free(res);
  }
  if (f) ctpool_future_free(f);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  atomic_store(&g_sweep_race_respond, false);

  REQUIRE_TRUE(parked1);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(parked2);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(idle, (size_t)1);
  REQUIRE_EQ(marked, (size_t)0);
}

/* ========================================================================== */
/*          EMPTY PORT, HOST CASE AND TRAILING DOT                            */
/* ========================================================================== */

/* This parses url and hands back the port, the path and query, the host and
 * the origin key. It frees whatever the caller does not take. */
static ccol_retval_t parse_url_parts(const char *url, uint16_t *port,
                                     bool *ipv6, char **host, char **pq,
                                     char **origin) {
  bool https = false, is_unix = false;
  char *auth = NULL, *sock = NULL;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      url, &https, ipv6, host, port, pq, origin, &auth, &is_unix, &sock);
  free(auth);
  free(sock);
  return rv;
}

TEST(url_parsing, an_empty_port_names_the_default_port_of_the_scheme) {
  struct {
    const char *url;
    uint16_t port;
    bool ipv6;
    const char *host;
    const char *pq;
  } cases[] = {
      {"http://example.com:/x", 80, false, "example.com", "/x"},
      {"https://example.com:/", 443, false, "example.com", "/"},
      {"http://[::1]:/y", 80, true, "::1", "/y"},
      {"https://[::1]:", 443, true, "::1", "/"},
      {"http://example.com:?q=1", 80, false, "example.com", "/?q=1"},
      {"http://example.com:#frag", 80, false, "example.com", "/"},
  };
  int failures = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    uint16_t port = 0;
    bool ipv6 = false;
    char *host = NULL, *pq = NULL, *origin = NULL;
    ccol_retval_t rv =
        parse_url_parts(cases[i].url, &port, &ipv6, &host, &pq, &origin);
    bool ok = rv == ccol_success && port == cases[i].port &&
              ipv6 == cases[i].ipv6 && host &&
              strcmp(host, cases[i].host) == 0 && pq &&
              strcmp(pq, cases[i].pq) == 0;
    if (!ok) {
      fprintf(stderr, "empty port case %zu (%s) failed: rv=%d port=%u\n", i,
              cases[i].url, (int)rv, (unsigned)port);
      failures++;
    }
    free(host);
    free(pq);
    free(origin);
  }
  /* An empty port and the default port name one origin. */
  char *o1 = NULL, *o2 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  (void)parse_url_parts("http://example.com:/", &p, &v6, NULL, NULL, &o1);
  (void)parse_url_parts("http://example.com/", &p, &v6, NULL, NULL, &o2);
  bool same_origin = o1 && o2 && strcmp(o1, o2) == 0;
  free(o1);
  free(o2);

  /* A Location that carries an empty port resolves and parses the same
   * way. */
  char *resolved = _chttp_resolve_redirect_url_for_tests(
      "http://example.com:8080/a", "https://other.example:/b");
  uint16_t rport = 0;
  bool rv6 = false;
  ccol_retval_t rrv =
      resolved ? parse_url_parts(resolved, &rport, &rv6, NULL, NULL, NULL)
               : ccol_unexpected_failure;
  free(resolved);

  /* Port 0 and a port with garbage stay refused. */
  ccol_retval_t zero =
      parse_url_parts("http://example.com:0/", &p, &v6, NULL, NULL, NULL);
  ccol_retval_t garbage =
      parse_url_parts("http://example.com:x/", &p, &v6, NULL, NULL, NULL);

  REQUIRE_EQ(failures, 0);
  REQUIRE_TRUE(same_origin);
  REQUIRE_EQ(rrv, ccol_success);
  REQUIRE_EQ(rport, 443);
  REQUIRE_EQ(zero, ccol_http_invalid_url);
  REQUIRE_EQ(garbage, ccol_http_invalid_url);
}

TEST(url_parsing, the_idle_pool_key_ignores_the_case_of_the_host) {
  char *o1 = NULL, *o2 = NULL, *o3 = NULL, *h1 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  ccol_retval_t r1 =
      parse_url_parts("HTTP://Example.COM:8080/a", &p, &v6, &h1, NULL, &o1);
  ccol_retval_t r2 =
      parse_url_parts("http://example.com:8080/b", &p, &v6, NULL, NULL, &o2);
  ccol_retval_t r3 =
      parse_url_parts("http://[::ABCD]:8080/", &p, &v6, NULL, NULL, &o3);
  bool same = o1 && o2 && strcmp(o1, o2) == 0;
  bool key_lower = o1 && strcmp(o1, "http://example.com:8080") == 0;
  bool v6_lower = o3 && strcmp(o3, "http://[::abcd]:8080") == 0;
  /* The host keeps the spelling of the URL for the Host header. */
  bool host_kept = h1 && strcmp(h1, "Example.COM") == 0;
  free(o1);
  free(o2);
  free(o3);
  free(h1);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_EQ(r3, ccol_success);
  REQUIRE_TRUE(same);
  REQUIRE_TRUE(key_lower);
  REQUIRE_TRUE(v6_lower);
  REQUIRE_TRUE(host_kept);
}

TEST(url_parsing, a_trailing_dot_stays_in_the_host_and_the_origin) {
  /* The connect and the Host header use the name as the URL gives it, and
   * "a." and "a" are not one origin, because "a" can resolve through a
   * search domain. Only TLS drops the dot; see the tls_trailing_dot tests. */
  char *host = NULL, *o1 = NULL, *o2 = NULL;
  uint16_t p = 0;
  bool v6 = false;
  ccol_retval_t r1 =
      parse_url_parts("https://www.example.com./", &p, &v6, &host, NULL, &o1);
  ccol_retval_t r2 =
      parse_url_parts("https://www.example.com/", &p, &v6, NULL, NULL, &o2);
  bool host_ok = host && strcmp(host, "www.example.com.") == 0;
  bool distinct = o1 && o2 && strcmp(o1, o2) != 0;
  free(host);
  free(o1);
  free(o2);
  REQUIRE_EQ(r1, ccol_success);
  REQUIRE_EQ(r2, ccol_success);
  REQUIRE_TRUE(host_ok);
  REQUIRE_TRUE(distinct);
}

extern bool _chttp_tls_client_handshake_for_tests(
    const char *host, const char *ca_path, const char *default_cert,
    const char *default_key, const char *named_as, const char *named_cert,
    const char *named_key);

TEST(tls_trailing_dot, a_fully_qualified_name_verifies_and_sends_sni) {
  /* The server presents the certificate of tls-dot.test only to a client
   * whose SNI names tls-dot.test exactly, and an unrelated default
   * certificate to any other client. The client trusts the first one
   * alone. A handshake therefore succeeds only when the client both sends
   * the name without its dot and verifies the certificate against the name
   * without its dot. */
  char dir[] = "chttpclient_tls_dot_XXXXXX";
  REQUIRE_NE((void *)mkdtemp(dir), NULL);
  char named_cert[128], named_key[128], def_cert[128], def_key[128];
  snprintf(named_cert, sizeof(named_cert), "%s/named.pem", dir);
  snprintf(named_key, sizeof(named_key), "%s/named.key", dir);
  snprintf(def_cert, sizeof(def_cert), "%s/default.pem", dir);
  snprintf(def_key, sizeof(def_key), "%s/default.key", dir);
  int g1 = _openssl_selfsigned(named_key, named_cert, "tls-dot.test",
                               "DNS:tls-dot.test");
  int g2 = _openssl_selfsigned(def_key, def_cert, "unrelated.test",
                               "DNS:unrelated.test");
  bool dotted = false, plain = false, wrong = true;
  if (g1 == 0 && g2 == 0) {
    dotted = _chttp_tls_client_handshake_for_tests(
        "tls-dot.test.", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
    plain = _chttp_tls_client_handshake_for_tests(
        "tls-dot.test", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
    /* A name that differs in more than its dot still fails. */
    wrong = _chttp_tls_client_handshake_for_tests(
        "tls-dot.test..", named_cert, def_cert, def_key, "tls-dot.test",
        named_cert, named_key);
  }
  unlink(named_cert);
  unlink(named_key);
  unlink(def_cert);
  unlink(def_key);
  rmdir(dir);
  REQUIRE_EQ(g1, 0);
  REQUIRE_EQ(g2, 0);
  REQUIRE_TRUE(plain);
  REQUIRE_TRUE(dotted);
  REQUIRE_FALSE(wrong);
}

/* ========================================================================== */
/*          A RESPONSE THAT ARRIVES WHILE THE REQUEST IS STILL SENT           */
/* ========================================================================== */

/* A listener whose answer depends on its mode. Each connection runs on a
 * thread of its own, which ers_stop joins.
 *   ERS_REJECT_CLOSE  413 once the header block arrived, then a lingering
 *                     close: shut the write side down, discard what
 *                     arrives (see ers_discard), and close with the rest
 *                     unread, so the kernel resets the connection under
 *                     the write of a client that is still sending.
 *   ERS_REJECT_HOLD   401 once the header block arrived, keep-alive, then
 *                     keep the connection open without reading until the
 *                     test releases it. A GET gets 200 "ok" and a close.
 *   ERS_HINTS_READ    103 once the header block arrived, then read the
 *                     whole body and answer 200 "got <n>".
 *   ERS_STREAM_ECHO   200 with the Content-Length of the body once the
 *                     header block arrived, then echo the body while it
 *                     arrives.
 *   ERS_STREAM_ECHO_KA as ERS_STREAM_ECHO with keep-alive, and then 200
 *                     "ok" for every later request of the connection.
 *   ERS_EARLY_200     the header block of a chunked 200 once the header
 *                     block of the request arrived, then read the whole
 *                     body with nothing more sent, then one chunk
 *                     "got <n>" and the end of the body.
 *   ERS_LATE_REJECT   read 64 KiB of the body, stop reading for 300 ms, so
 *                     that the send of the client blocks first, then 413
 *                     and keep the connection open without reading until
 *                     the test releases it.
 *   ERS_SLOW_CONTINUE for each request of a keep-alive connection, wait
 *                     250 ms, send 100 Continue, read the body and answer
 *                     200 "ok". */
enum {
  ERS_REJECT_CLOSE,
  ERS_REJECT_HOLD,
  ERS_HINTS_READ,
  ERS_STREAM_ECHO,
  ERS_EARLY_200,
  ERS_LATE_REJECT,
  ERS_SLOW_CONTINUE,
  ERS_STREAM_ECHO_KA
};

#define ERS_MAX_CONNS 16

typedef struct {
  int fd;
  int port;
  int mode;
  pthread_t tid;
  atomic_int running;
  atomic_int accepts;
  atomic_int release;
  pthread_mutex_t mu;
  pthread_t conn_tids[ERS_MAX_CONNS];
  int conn_count;
} ers_srv_t;

typedef struct {
  ers_srv_t *s;
  int fd;
} ers_conn_t;

static size_t ers_content_length(const char *hdrs) {
  const char *p = strcasestr(hdrs, "\r\ncontent-length:");
  if (!p) return 0;
  return (size_t)strtoull(p + strlen("\r\ncontent-length:"), NULL, 10);
}

static void ers_send_all(int fd, const char *p, size_t n) {
  while (n > 0) {
    ssize_t w = send(fd, p, n, MSG_NOSIGNAL);
    if (w <= 0) return;
    p += w;
    n -= (size_t)w;
  }
}

static void *ers_conn_thread(void *arg) {
  ers_conn_t *c = (ers_conn_t *)arg;
  ers_srv_t *s = c->s;
  int fd = c->fd;
  free(c);
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  size_t hl = 0;
  ssize_t n = buf ? srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl) : -1;
  if (n <= 0 || hl == 0) goto out;
  if (strncmp(buf, "GET ", 4) == 0) {
    static const char ok[] =
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
    goto out;
  }
  size_t cl = ers_content_length(buf);
  size_t have = (size_t)n - hl;
  while (s->mode == ERS_SLOW_CONTINUE) {
    usleep(250000);
    static const char c100[] = "HTTP/1.1 100 Continue\r\n\r\n";
    ers_send_all(fd, c100, sizeof(c100) - 1);
    while (have < cl) {
      ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
      if (r <= 0) goto out;
      have += (size_t)r;
    }
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
    n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
    if (n <= 0 || hl == 0) goto out;
    cl = ers_content_length(buf);
    have = (size_t)n - hl;
  }
  switch (s->mode) {
    case ERS_REJECT_CLOSE: {
      static const char r[] =
          "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n"
          "Connection: close\r\n\r\ntoo large";
      ers_send_all(fd, r, sizeof(r) - 1);
      shutdown(fd, SHUT_WR);
      ers_discard(fd, buf, TEST_SERVER_BUF);
      break;
    }
    case ERS_REJECT_HOLD: {
      static const char r[] =
          "HTTP/1.1 401 Unauthorized\r\nContent-Length: 12\r\n\r\n"
          "unauthorized";
      ers_send_all(fd, r, sizeof(r) - 1);
      for (int i = 0; i < 1500 && !atomic_load(&s->release); i++) usleep(10000);
      break;
    }
    case ERS_HINTS_READ: {
      static const char h[] =
          "HTTP/1.1 103 Early Hints\r\nLink: </style.css>\r\n\r\n";
      ers_send_all(fd, h, sizeof(h) - 1);
      while (have < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      char resp[128];
      char body[32];
      int bl = snprintf(body, sizeof(body), "got %zu", have);
      int rl = snprintf(resp, sizeof(resp),
                        "HTTP/1.1 200 OK\r\nContent-Length: %d\r\n"
                        "Connection: close\r\n\r\n%s",
                        bl, body);
      ers_send_all(fd, resp, (size_t)rl);
      break;
    }
    case ERS_STREAM_ECHO:
    case ERS_STREAM_ECHO_KA: {
      char head[128];
      int hl2 =
          snprintf(head, sizeof(head),
                   "HTTP/1.1 200 OK\r\nContent-Length: %zu\r\n%s\r\n", cl,
                   s->mode == ERS_STREAM_ECHO ? "Connection: close\r\n" : "");
      ers_send_all(fd, head, (size_t)hl2);
      size_t echoed = 0;
      if (have > 0) {
        ers_send_all(fd, buf + hl, have);
        echoed = have;
      }
      while (echoed < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        ers_send_all(fd, buf, (size_t)r);
        echoed += (size_t)r;
      }
      /* Keep-alive: answer every later GET on this connection. */
      while (s->mode == ERS_STREAM_ECHO_KA) {
        n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
        if (n <= 0 || hl == 0) break;
        static const char ok[] =
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
        ers_send_all(fd, ok, sizeof(ok) - 1);
      }
      break;
    }
    case ERS_EARLY_200: {
      static const char h[] =
          "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
          "Connection: close\r\n\r\n";
      ers_send_all(fd, h, sizeof(h) - 1);
      while (have < cl) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      char body[32];
      int bl = snprintf(body, sizeof(body), "got %zu", have);
      char tail[96];
      int tl = snprintf(tail, sizeof(tail), "%x\r\n%s\r\n0\r\n\r\n", bl, body);
      ers_send_all(fd, tail, (size_t)tl);
      break;
    }
    case ERS_LATE_REJECT: {
      while (have < 65536) {
        ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
        if (r <= 0) break;
        have += (size_t)r;
      }
      usleep(300000);
      static const char r[] =
          "HTTP/1.1 413 Payload Too Large\r\nContent-Length: 9\r\n\r\n"
          "too large";
      ers_send_all(fd, r, sizeof(r) - 1);
      for (int i = 0; i < 1500 && !atomic_load(&s->release); i++) usleep(10000);
      break;
    }
    default:
      break;
  }
out:
  free(buf);
  close(fd);
  return NULL;
}

static void *ers_accept_loop(void *arg) {
  ers_srv_t *s = (ers_srv_t *)arg;
  while (atomic_load(&s->running)) {
    int fd = accept(s->fd, NULL, NULL);
    if (fd < 0) {
      if (!atomic_load(&s->running)) break;
      continue;
    }
    if (!atomic_load(&s->running)) { /* the wake of ers_stop() */
      close(fd);
      break;
    }
    atomic_fetch_add(&s->accepts, 1);
    ers_conn_t *c = (ers_conn_t *)malloc(sizeof(*c));
    pthread_mutex_lock(&s->mu);
    bool room = s->conn_count < ERS_MAX_CONNS;
    pthread_mutex_unlock(&s->mu);
    if (!c || !room) {
      free(c);
      close(fd);
      continue;
    }
    c->s = s;
    c->fd = fd;
    pthread_t tid;
    if (pthread_create(&tid, NULL, ers_conn_thread, c) != 0) {
      free(c);
      close(fd);
      continue;
    }
    pthread_mutex_lock(&s->mu);
    s->conn_tids[s->conn_count++] = tid;
    pthread_mutex_unlock(&s->mu);
  }
  return NULL;
}

static bool ers_start(ers_srv_t *s, int mode) {
  memset(s, 0, sizeof(*s));
  s->mode = mode;
  pthread_mutex_init(&s->mu, NULL);
  s->fd = socket(AF_INET, SOCK_STREAM, 0);
  if (s->fd < 0) return false;
  int opt = 1;
  setsockopt(s->fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));
  /* Small buffers on the side of the server, which every accepted socket
   * inherits, keep the bytes that the kernel can hold for either direction
   * well below the size of ERS_BIG_BODY, whatever the tuning of the host. */
  int small = 16384;
  setsockopt(s->fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  setsockopt(s->fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(a);
  if (bind(s->fd, (struct sockaddr *)&a, sizeof(a)) != 0 ||
      listen(s->fd, 16) != 0 ||
      getsockname(s->fd, (struct sockaddr *)&a, &len) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  s->port = ntohs(a.sin_port);
  atomic_store(&s->running, 1);
  if (pthread_create(&s->tid, NULL, ers_accept_loop, s) != 0) {
    close(s->fd);
    s->fd = -1;
    return false;
  }
  return true;
}

static void ers_stop(ers_srv_t *s) {
  if (s->fd < 0) return;
  atomic_store(&s->release, 1);
  atomic_store(&s->running, 0);
  test_wake_listener(s->fd);
  pthread_join(s->tid, NULL);
  close(s->fd);
  s->fd = -1;
  for (int i = 0; i < s->conn_count; i++) pthread_join(s->conn_tids[i], NULL);
  s->conn_count = 0;
  pthread_mutex_destroy(&s->mu);
}

static long ers_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long)t.tv_sec * 1000L + t.tv_nsec / 1000000L;
}

/* A body larger than any socket buffer of the loopback, so the send of the
 * request cannot finish before the server answers. */
#define ERS_BIG_BODY (16u * 1024u * 1024u)

/* The request timeout of a test that echoes ERS_BIG_BODY back, so that 16 MiB
 * go each way. Under valgrind on a host with 2 CPUs, one such exchange takes
 * about 10 s alone and more than 20 s beside the rest of the suite. A client
 * that deadlocks still reports ccol_timed_out with this limit. */
#define ERS_ECHO_TIMEOUT_US 90000000u

static chttp_request_t *ers_post(int port, size_t body_len, char **body_out) {
  char *body = (char *)malloc(body_len);
  if (!body) return NULL;
  for (size_t i = 0; i < body_len; i++) body[i] = (char)('a' + (i % 23));
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/upload", port);
  chttp_request_body_t rb = {.data = body,
                             .len = body_len,
                             .content_type = "application/octet-stream"};
  chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rb, NULL);
  if (body_out)
    *body_out = body;
  else
    free(body);
  return req;
}

/* The server answers 413, discards what arrives for a while and closes with
 * the rest unread, which resets the connection under the write of a client
 * that is still sending. Every tier returns that answer. The reset discards
 * whatever the client has not read by then, so the answer must be read
 * while the body goes out. This test is non-vacuous: a client that reads
 * only once the whole body went out reports ccol_http_transfer_aborted with
 * no response. */
TEST(early_response, reject_then_close_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_REJECT_CLOSE));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 20000000);
  chttp_request_t *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  char body[4][16];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 413);
    REQUIRE_STREQ(body[tier], "too large");
  }
}

/* The server answers 401 and keeps the connection open without reading the
 * body, so the write of the client blocks for good. Every tier returns the
 * answer at once, and never pools the connection: the GET that follows
 * reaches the server on a connection of its own. This test is non-vacuous:
 * a client that only writes waits for its request timeout and reports
 * ccol_timed_out. */
TEST(early_response, reject_while_not_reading_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_REJECT_HOLD));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 15000000);
  chttp_request_t *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  char get_url[128];
  snprintf(get_url, sizeof(get_url), "http://127.0.0.1:%d/after", s.port);
  chttp_request_t *get = chttp_request_new(CHTTP_GET, get_url, NULL, NULL);
  ccol_retval_t rv[4] = {0}, grv[4] = {0};
  int status[4] = {0}, gstatus[4] = {0};
  long elapsed[4] = {0};
  int accepts[4] = {0};
  for (int tier = 1; tier <= 3 && req && get && cli != CHTTPCLI_INVALID;
       tier++) {
    int before = atomic_load(&s.accepts);
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
    resp = NULL;
    grv[tier] = run_in_tier(tier, cli, get, &resp);
    gstatus[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
    accepts[tier] = atomic_load(&s.accepts) - before;
  }
  bool have = req && get && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  chttp_request_free(get);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 401);
    REQUIRE_LT(elapsed[tier], 10000L);
    REQUIRE_EQ(grv[tier], ccol_success);
    REQUIRE_EQ(gstatus[tier], 200);
    REQUIRE_EQ(accepts[tier], 2);
  }
}

/* An interim response that arrives while the body goes out does not answer
 * the request: the whole body still reaches the server in every tier. */
TEST(early_response, interim_response_lets_the_upload_finish_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_HINTS_READ));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 20000000);
  size_t len = 2u * 1024u * 1024u;
  chttp_request_t *req = ers_post(s.port, len, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  char body[4][32];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  char expect[32];
  snprintf(expect, sizeof(expect), "got %zu", len);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_STREQ(body[tier], expect);
  }
}

/* A server that answers 200 at once and streams the body back while it
 * reads it. The client must keep sending while it reads, in every tier.
 * This test is non-vacuous: a client that only writes until the request is
 * complete deadlocks with this server once both socket buffers fill, and
 * reports ccol_timed_out. */
TEST(early_response, streamed_2xx_answer_keeps_the_upload_going_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_STREAM_ECHO));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID)
    chttpclient_set_request_timeout(cli, ERS_ECHO_TIMEOUT_US);
  size_t len = ERS_BIG_BODY;
  char *sent = NULL;
  chttp_request_t *req = ers_post(s.port, len, &sent);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  bool same[4] = {false};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      same[tier] = resp->body_len == len && resp->body &&
                   memcmp(resp->body, sent, len) == 0;
    }
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  free(sent);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_TRUE(same[tier]);
  }
}

/* A server that sends the header block of a 200 at once and then reads the
 * whole body without writing anything more. The client must go on with the
 * upload although the answer has started, in every tier. This test is
 * non-vacuous: a client that stops the upload at the first byte of an
 * answer, or that waits for more of the answer while the server waits for
 * the body, reports ccol_timed_out. */
TEST(early_response, early_2xx_head_lets_the_upload_finish_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_EARLY_200));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 20000000);
  size_t len = ERS_BIG_BODY;
  chttp_request_t *req = ers_post(s.port, len, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  char body[4][32];
  memset(body, 0, sizeof(body));
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    if (resp) {
      status[tier] = resp->status_code;
      if (resp->body && resp->body_len < sizeof(body[0]))
        memcpy(body[tier], resp->body, resp->body_len);
    }
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  char expect[32];
  snprintf(expect, sizeof(expect), "got %zu", len);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_STREQ(body[tier], expect);
  }
}

/* The server stops reading first, so that the send of the client already
 * waits for room to write, and answers 413 only afterwards, still without
 * reading. Every tier returns that answer, which arrives on the direction
 * that the blocked send does not wait on. This test is non-vacuous: a
 * client that waits only for room to write reports ccol_timed_out. */
TEST(early_response, reject_after_the_send_blocked_is_returned_in_every_tier) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_LATE_REJECT));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 15000000);
  chttp_request_t *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 413);
    REQUIRE_LT(elapsed[tier], 10000L);
  }
}

/* ========================================================================== */
/*                      HAPPY EYEBALLS (RFC 8305)                             */
/* ========================================================================== */

extern void _chttp_addr_list_interleave_for_tests(const int *families, size_t n,
                                                  size_t *order_out);

/* The families alternate, starting with the family of the first address,
 * and each family keeps the order of the resolver. */
TEST(happy_eyeballs, addresses_alternate_their_families) {
  const int f1[] = {AF_INET6, AF_INET6, AF_INET6, AF_INET, AF_INET};
  size_t o1[5];
  _chttp_addr_list_interleave_for_tests(f1, 5, o1);
  const size_t e1[] = {0, 3, 1, 4, 2};
  for (size_t i = 0; i < 5; i++) REQUIRE_EQ(o1[i], e1[i]);

  const int f2[] = {AF_INET, AF_INET, AF_INET6};
  size_t o2[3];
  _chttp_addr_list_interleave_for_tests(f2, 3, o2);
  const size_t e2[] = {0, 2, 1};
  for (size_t i = 0; i < 3; i++) REQUIRE_EQ(o2[i], e2[i]);

  const int f3[] = {AF_INET, AF_INET, AF_INET};
  size_t o3[3];
  _chttp_addr_list_interleave_for_tests(f3, 3, o3);
  for (size_t i = 0; i < 3; i++) REQUIRE_EQ(o3[i], i);
}

/* An address whose connects neither complete nor fail: the shape of an
 * address whose packets are dropped on the way. On Linux it is a listener
 * whose accept queue is full, so the kernel drops the SYN of every further
 * connect, and *filler_out holds the one connection that fills the queue.
 * The BSDs and macOS answer such a SYN (with a reset or from the SYN
 * cache), so there the address is a port that a socket holds without
 * listening, and the client's own test hook leaves every attempt on it
 * unconnected; clearing the address override forgets it. */
static int blackhole_listener(struct sockaddr_in *out, int *filler_out) {
  *filler_out = -1;
  int l = socket(AF_INET, SOCK_STREAM, 0);
  if (l < 0) return -1;
#if !defined(__linux__)
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t blen = sizeof(*out);
  if (bind(l, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(l, (struct sockaddr *)out, &blen) != 0) {
    close(l);
    return -1;
  }
  _chttp_set_silent_addr_for_tests((const struct sockaddr *)out, sizeof(*out));
  return l;
#endif
  memset(out, 0, sizeof(*out));
  out->sin_family = AF_INET;
  out->sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(*out);
  if (bind(l, (struct sockaddr *)out, sizeof(*out)) != 0 ||
      getsockname(l, (struct sockaddr *)out, &len) != 0 || listen(l, 0) != 0) {
    close(l);
    return -1;
  }
  int f = ccol_socket_nb(AF_INET, SOCK_STREAM, 0);
  if (f < 0) {
    close(l);
    return -1;
  }
  (void)connect(f, (struct sockaddr *)out, sizeof(*out));
  struct pollfd p = {.fd = f, .events = POLLOUT, .revents = 0};
  int soerr = 1;
  socklen_t sl = sizeof(soerr);
  if (poll(&p, 1, 2000) != 1 ||
      getsockopt(f, SOL_SOCKET, SO_ERROR, &soerr, &sl) != 0 || soerr != 0) {
    close(f);
    close(l);
    return -1;
  }
  *filler_out = f;
  return l;
}

static int count_open_fds(void) { return test_count_open_fds(); }

/* The count of open descriptors settles back to `baseline` once every
 * socket of the client is closed. A server thread of this file can still
 * hold its end of a connection for a moment, so this polls. */
static int settle_open_fds(int baseline) {
  int n = count_open_fds();
  for (int i = 0; i < 300 && n != baseline; i++) {
    usleep(10000);
    n = count_open_fds();
  }
  return n;
}

/* The first address drops every packet and the second one serves. Every
 * tier connects to the second one after about one attempt delay, and not
 * after the connect timeout, and closes the attempt on the first one. This
 * test is non-vacuous: a client that tries the addresses one after the other
 * waits the whole connect timeout on the first one and reports
 * ccol_timed_out. */
TEST(happy_eyeballs, a_silent_first_address_costs_one_attempt_delay) {
  struct sockaddr_in hole;
  int filler = -1;
  int hl = blackhole_listener(&hole, &filler);
  REQUIRE_GE(hl, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&hole,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(hole), sizeof(good)};

  int fds_before = count_open_fds();
  _chttp_set_addr_override_for_tests(addrs, lens, 2);
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_connect_timeout(cli, 10000000);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  int fds_after = settle_open_fds(fds_before);
  close(filler);
  close(hl);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_GE(elapsed[tier], 200L);
    REQUIRE_LT(elapsed[tier], 3000L);
  }
  REQUIRE_EQ(fds_after, fds_before);
}

/* No address answers. The connect timeout bounds the whole race in every
 * tier, which ends with ccol_timed_out, and every attempt is closed. */
TEST(happy_eyeballs, the_connect_timeout_bounds_the_whole_race) {
  struct sockaddr_in h1, h2;
  int f1 = -1, f2 = -1;
  int l1 = blackhole_listener(&h1, &f1);
  int l2 = blackhole_listener(&h2, &f2);
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&h1,
                                     (const struct sockaddr *)&h2};
  socklen_t lens[2] = {sizeof(h1), sizeof(h2)};
  bool ready = l1 >= 0 && l2 >= 0;

  int fds_before = count_open_fds();
  if (ready) _chttp_set_addr_override_for_tests(addrs, lens, 2);
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_connect_timeout(cli, 700000);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval_t rv[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && ready && req && cli != CHTTPCLI_INVALID;
       tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  int fds_after = settle_open_fds(fds_before);
  if (f1 >= 0) close(f1);
  if (f2 >= 0) close(f2);
  if (l1 >= 0) close(l1);
  if (l2 >= 0) close(l2);

  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_timed_out);
    REQUIRE_GE(elapsed[tier], 600L);
    REQUIRE_LT(elapsed[tier], 5000L);
  }
  REQUIRE_EQ(fds_after, fds_before);
}

extern void _chttp_set_connect_attempt_delay_ms_for_tests(long ms);

/* A refused first address hands over to the second one at once, without
 * the attempt delay, in every tier. The delay is raised to one minute for
 * this test, so that a hand-over which waits for it shows as a request of a
 * minute and not of a moment, whatever the instrumentation of the run. This
 * test is non-vacuous: a hand-over that waits for the delay takes at least
 * 60 s and fails the bound of 30 s. */
TEST(happy_eyeballs, a_refused_address_hands_over_at_once) {
  struct sockaddr_in refused;
  int rfd = refused_port_socket(&refused);
  REQUIRE_GE(rfd, 0);
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[2] = {(const struct sockaddr *)&refused,
                                     (const struct sockaddr *)&good};
  socklen_t lens[2] = {sizeof(refused), sizeof(good)};
  _chttp_set_addr_override_for_tests(addrs, lens, 2);
  _chttp_set_connect_attempt_delay_ms_for_tests(60000);
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_connect_timeout(cli, 90000000);
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
  ccol_retval_t rv[4] = {0};
  long elapsed[4] = {0};
  for (int tier = 1; tier <= 3 && req && cli != CHTTPCLI_INVALID; tier++) {
    chttpcli_response *resp = NULL;
    long t0 = ers_now_ms();
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    elapsed[tier] = ers_now_ms() - t0;
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  _chttp_set_connect_attempt_delay_ms_for_tests(0);
  _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  close(rfd);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_LT(elapsed[tier], 30000L);
  }
}

extern void _chttp_set_connect_short_delays_for_tests(int n);
extern void _chttp_set_connect_async_failures_for_tests(int n);

/* Tier 1: an attempt that fails after its connect started hands over to the
 * next address at once, while an earlier attempt is still in flight. The
 * first address drops every packet, so its attempt stays in flight for the
 * whole test. The second address starts 1 ms later; its connect completes,
 * and the hook reports that completion as ECONNREFUSED, which is what a
 * refusal by a remote host looks like. The attempt delay is one minute for
 * every other attempt. This test is non-vacuous: a hand-over that waits for
 * the delay of the failed attempt leaves the third address unstarted until
 * the connect timeout of 30 s ends the request with ccol_timed_out. */
TEST(happy_eyeballs, tier1_an_asynchronous_failure_hands_over_at_once) {
  struct sockaddr_in hole;
  int filler = -1;
  int hl = blackhole_listener(&hole, &filler);
  struct sockaddr_in lst;
  memset(&lst, 0, sizeof(lst));
  lst.sin_family = AF_INET;
  lst.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t llen = sizeof(lst);
  int lfd = ccol_socket_nb(AF_INET, SOCK_STREAM, 0);
  bool ready = hl >= 0 && lfd >= 0 &&
               bind(lfd, (struct sockaddr *)&lst, sizeof(lst)) == 0 &&
               listen(lfd, 8) == 0 &&
               getsockname(lfd, (struct sockaddr *)&lst, &llen) == 0;
  struct sockaddr_in good;
  memset(&good, 0, sizeof(good));
  good.sin_family = AF_INET;
  good.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  good.sin_port = htons((uint16_t)get_test_port());
  const struct sockaddr *addrs[3] = {(const struct sockaddr *)&hole,
                                     (const struct sockaddr *)&lst,
                                     (const struct sockaddr *)&good};
  socklen_t lens[3] = {sizeof(hole), sizeof(lst), sizeof(good)};

  ccol_retval_t rv = ccol_unexpected_failure;
  int status = 0;
  long elapsed = -1;
  bool have = false;
  if (ready) {
    _chttp_set_addr_override_for_tests(addrs, lens, 3);
    _chttp_set_connect_attempt_delay_ms_for_tests(60000);
    _chttp_set_connect_short_delays_for_tests(1);
    _chttp_set_connect_async_failures_for_tests(1);
    chttpcli cli = ccol_create_chttpclient(NULL);
    /* The request timeout bounds a run where the wrong address wins: its
     * listener never answers. */
    if (cli != CHTTPCLI_INVALID) {
      chttpclient_set_connect_timeout(cli, 30000000);
      chttpclient_set_request_timeout(cli, 60000000);
    }
    chttp_request_t *req =
        chttp_request_new(CHTTP_GET, "http://eyeballs.invalid/get", NULL, NULL);
    have = req && cli != CHTTPCLI_INVALID;
    if (have) {
      chttpcli_response *resp = NULL;
      long t0 = ers_now_ms();
      rv = chttpclient_do(cli, req, &resp);
      elapsed = ers_now_ms() - t0;
      status = resp ? resp->status_code : 0;
      chttpclient_resp_free(resp);
    }
    chttp_request_free(req);
    if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
    _chttp_set_connect_async_failures_for_tests(0);
    _chttp_set_connect_short_delays_for_tests(0);
    _chttp_set_connect_attempt_delay_ms_for_tests(0);
    _chttp_set_addr_override_for_tests(NULL, NULL, 0);
  }
  /* The attempt on the second address really connected, so its failure is
   * the one the hook reported after the connect had started. */
  int accepted = lfd >= 0 ? accept(lfd, NULL, NULL) : -1;
  if (accepted >= 0) close(accepted);
  if (lfd >= 0) close(lfd);
  if (filler >= 0) close(filler);
  if (hl >= 0) close(hl);

  REQUIRE_TRUE(ready);
  REQUIRE_TRUE(have);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_LT(elapsed, 20000L);
  REQUIRE_GE(accepted, 0);
}

/* ========================================================================== */
/*                         CLOSE-ON-EXEC                                      */
/* ========================================================================== */

/* This counts the sockets of this process whose peer is the TCP port `port`
 * of the loopback, or the unix socket at `unix_path`, and how many of them
 * lack FD_CLOEXEC. */
typedef struct {
  int port;
  const char *unix_path;
  int found;
  int inheritable;
} client_socket_census_t;

static void census_one_fd(int fd, void *arg) {
  client_socket_census_t *c = (client_socket_census_t *)arg;
  int port = c->port;
  const char *unix_path = c->unix_path;
  {
    struct sockaddr_storage ss;
    memset(&ss, 0, sizeof(ss));
    socklen_t sl = sizeof(ss);
    if (getpeername(fd, (struct sockaddr *)&ss, &sl) != 0) return;
    bool match = false;
    if (ss.ss_family == AF_INET && port > 0)
      match = ntohs(((struct sockaddr_in *)&ss)->sin_port) == port;
    else if (ss.ss_family == AF_UNIX && unix_path)
      match = strcmp(((struct sockaddr_un *)&ss)->sun_path, unix_path) == 0;
    if (!match) return;
    c->found++;
    int flags = fcntl(fd, F_GETFD);
    if (flags < 0 || !(flags & FD_CLOEXEC)) c->inheritable++;
  }
}

static void count_client_sockets(int port, const char *unix_path, int *found,
                                 int *inheritable) {
  client_socket_census_t c = {
      .port = port, .unix_path = unix_path, .found = 0, .inheritable = 0};
  test_for_each_open_fd(census_one_fd, &c);
  *found = c.found;
  *inheritable = c.inheritable;
}

/* Every socket of the client is close-on-exec, so that a process that the
 * application spawns inherits none of its connections. The pooled
 * connections of Tier 1 and of Tier 2, over TCP and over a unix socket, are
 * what this test finds still open after the requests. This test is
 * non-vacuous: without SOCK_CLOEXEC, every one of them is inheritable. */
TEST(cloexec, pooled_client_sockets_are_close_on_exec) {
  char url[160];
  make_url(url, sizeof(url), "/keepalive");
  char uurl[512];
  make_unix_url(uurl, sizeof(uurl), "/keepalive");
  chttpcli cli = ccol_create_chttpclient(NULL);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  chttp_request_t *ureq = chttp_request_new(CHTTP_GET, uurl, NULL, NULL);
  ccol_retval_t rv[4] = {0}, urv[4] = {0};
  for (int tier = 1; tier <= 2 && req && ureq && cli != CHTTPCLI_INVALID;
       tier++) {
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    chttpclient_resp_free(resp);
    resp = NULL;
    urv[tier] = run_in_tier(tier, cli, ureq, &resp);
    chttpclient_resp_free(resp);
  }
  int found = 0, inheritable = 0, ufound = 0, uinheritable = 0;
  count_client_sockets(get_test_port(), NULL, &found, &inheritable);
  count_client_sockets(0, get_test_unix_socket_path(), &ufound, &uinheritable);
  bool have = req && ureq && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  chttp_request_free(ureq);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 2; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(urv[tier], ccol_success);
  }
  REQUIRE_GE(found, 2);
  REQUIRE_GE(ufound, 2);
  REQUIRE_EQ(inheritable, 0);
  REQUIRE_EQ(uinheritable, 0);
}

/* An fopen() and an open() of this binary in front of the ones of the C
 * library. Each records whether an open of the path that g_open_watch names
 * asked for close-on-exec, and hands every call to the C library. On glibc a
 * translation unit that selects 64-bit file offsets calls fopen64 and
 * open64 under the names fopen and open, and so do these definitions, which
 * is why glibc is reached through fopen64 and open64 on every target. The
 * other C libraries have 64-bit offsets under the plain names. */
#if defined(__GLIBC__)
#define TEST_REAL_FOPEN "fopen64"
#define TEST_REAL_OPEN "open64"
#else
#define TEST_REAL_FOPEN "fopen"
#define TEST_REAL_OPEN "open"
#endif
/* AddressSanitizer and ThreadSanitizer define their own fopen() and open()
 * to watch them. With glibc they define them weakly, so the definitions here
 * take their place. Elsewhere (FreeBSD) they define them strongly, and a
 * second definition does not link; such a build leaves the watch out and
 * skips its test. */
#if defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define TEST_UNDER_SANITIZER 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define TEST_UNDER_SANITIZER 1
#endif
#if defined(TEST_UNDER_SANITIZER) && !defined(__GLIBC__)
#define TEST_OPEN_WATCH 0
#else
#define TEST_OPEN_WATCH 1
#endif
#if TEST_OPEN_WATCH
static char g_open_watch[512];
static int g_open_watch_seen;
static bool g_open_watch_cloexec;
static pthread_mutex_t g_open_mu = PTHREAD_MUTEX_INITIALIZER;

static void open_watch_note(const char *path, bool cloexec) {
  pthread_mutex_lock(&g_open_mu);
  if (path && g_open_watch[0] && strcmp(path, g_open_watch) == 0) {
    g_open_watch_seen++;
    g_open_watch_cloexec = cloexec;
  }
  pthread_mutex_unlock(&g_open_mu);
}

FILE *fopen(const char *path, const char *mode) {
  static void *_Atomic real = NULL;
  void *fn = atomic_load(&real);
  if (!fn) {
    fn = dlsym(RTLD_NEXT, TEST_REAL_FOPEN);
    atomic_store(&real, fn);
  }
  open_watch_note(path, mode && strchr(mode, 'e') != NULL);
  if (!fn) {
    errno = ENOSYS;
    return NULL;
  }
  return ((FILE * (*)(const char *, const char *)) fn)(path, mode);
}

int open(const char *path, int flags, ...) {
  static void *_Atomic real = NULL;
  void *fn = atomic_load(&real);
  if (!fn) {
    fn = dlsym(RTLD_NEXT, TEST_REAL_OPEN);
    atomic_store(&real, fn);
  }
  unsigned mode = 0;
  bool takes_mode = (flags & O_CREAT) != 0;
#ifdef O_TMPFILE
  takes_mode = takes_mode || (flags & O_TMPFILE) == O_TMPFILE;
#endif
  if (takes_mode) {
    va_list ap;
    va_start(ap, flags);
    mode = va_arg(ap, unsigned);
    va_end(ap);
  }
  open_watch_note(path, (flags & O_CLOEXEC) != 0);
  if (!fn) {
    errno = ENOSYS;
    return -1;
  }
  return ((int (*)(const char *, int, ...))fn)(path, flags, mode);
}

/* With _FORTIFY_SOURCE, the open() of glibc calls the checking entry point
 * __open64_2 (or __open_2 without 64-bit offsets) for a call with no mode
 * argument, so the watch interposes both of those too. Each one carries its
 * symbol name in an asm label. With 64-bit offsets the fortify header of
 * glibc gives the C name __open_2 the symbol __open64_2, so a definition
 * under that C name defines __open64_2 a second time. */
static int open_watch_checked(const char *name, const char *path, int flags) {
  open_watch_note(path, (flags & O_CLOEXEC) != 0);
  int (*fn)(const char *, int) =
      (int (*)(const char *, int))dlsym(RTLD_NEXT, name);
  if (!fn) {
    errno = ENOSYS;
    return -1;
  }
  return fn(path, flags);
}

int test_open_watch_open_2(const char *path, int flags) __asm__("__open_2");
int test_open_watch_open_2(const char *path, int flags) {
  return open_watch_checked("__open_2", path, flags);
}

int test_open_watch_open64_2(const char *path, int flags) __asm__("__open64_2");
int test_open_watch_open64_2(const char *path, int flags) {
  return open_watch_checked("__open64_2", path, flags);
}
#endif /* TEST_OPEN_WATCH */

/* The certificate and key files that the TLS layer reads are opened
 * close-on-exec, so that a process that another thread spawns meanwhile
 * does not inherit them. This test is non-vacuous: without O_CLOEXEC on the
 * open of the CA bundle, or the "e" of an fopen mode, it fails. */
TEST(cloexec, tls_files_are_opened_close_on_exec) {
#if !TEST_OPEN_WATCH
  fprintf(stderr, "SKIP: the sanitizer here owns fopen() and open()\n");
#else
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  pthread_mutex_lock(&g_open_mu);
  snprintf(g_open_watch, sizeof(g_open_watch), "%s", g_tls_cert_path);
  g_open_watch_seen = 0;
  g_open_watch_cloexec = false;
  pthread_mutex_unlock(&g_open_mu);

  chttpcli cli = ccol_create_chttpclient(NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval_t trv = cli != CHTTPCLI_INVALID ? chttpclient_set_tls(cli, &tls)
                                              : ccol_invalid_args;
  char url[160];
  make_tls_url(url, sizeof(url), "/hello");
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ccol_retval_t rv = ccol_invalid_args;
  if (req && trv == ccol_success) {
    chttpcli_response *resp = NULL;
    rv = chttpclient_do(cli, req, &resp);
    chttpclient_resp_free(resp);
  }
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);

  pthread_mutex_lock(&g_open_mu);
  int seen = g_open_watch_seen;
  bool cloexec = g_open_watch_cloexec;
  g_open_watch[0] = '\0';
  pthread_mutex_unlock(&g_open_mu);

  REQUIRE_EQ(trv, ccol_success);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_GT(seen, 0);
  REQUIRE_TRUE(cloexec);
#endif
}

/* The TLS form of the early_response tests: the answer that a server sends
 * while the body is still going out reaches the caller over TLS too, in
 * every tier. This test is non-vacuous: without the read after a failed or
 * blocked write, every tier reports ccol_http_transfer_aborted or
 * ccol_timed_out. */
TEST(early_response, tls_answers_during_the_upload_are_returned_in_every_tier) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  atomic_store(&g_tls_early_release, 0);
  chttpcli cli = ccol_create_chttpclient(NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval_t trv = cli != CHTTPCLI_INVALID ? chttpclient_set_tls(cli, &tls)
                                              : ccol_invalid_args;
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 15000000);
  size_t len = ERS_BIG_BODY;
  char *body = (char *)malloc(len);
  if (body) memset(body, 'b', len);
  chttp_request_body_t rb = {
      .data = body, .len = len, .content_type = "application/octet-stream"};
  const char *paths[2] = {"/early-reject-close", "/early-reject-hold"};
  const int want[2] = {413, 401};
  ccol_retval_t rv[2][4];
  int status[2][4];
  memset(rv, 0, sizeof(rv));
  memset(status, 0, sizeof(status));
  for (int k = 0; k < 2 && body && trv == ccol_success; k++) {
    char url[160];
    make_tls_url(url, sizeof(url), paths[k]);
    chttp_request_t *req = chttp_request_new(CHTTP_POST, url, &rb, NULL);
    for (int tier = 1; tier <= 3 && req; tier++) {
      chttpcli_response *resp = NULL;
      rv[k][tier] = run_in_tier(tier, cli, req, &resp);
      status[k][tier] = resp ? resp->status_code : 0;
      chttpclient_resp_free(resp);
    }
    chttp_request_free(req);
  }
  atomic_store(&g_tls_early_release, 1);
  free(body);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();

  REQUIRE_EQ(trv, ccol_success);
  for (int k = 0; k < 2; k++)
    for (int tier = 1; tier <= 3; tier++) {
      REQUIRE_EQ(rv[k][tier], ccol_success);
      REQUIRE_EQ(status[k][tier], want[k]);
    }
}

/* A response that started before the request was sent in full never leaves
 * its connection in the pool, whatever it says about keep-alive: the GET
 * that follows reaches the server on a connection of its own, in every
 * tier. This test is non-vacuous: without that rule, the GET reuses the
 * connection of the echo and the server counts one connection. */
TEST(early_response, a_response_that_started_during_the_send_is_never_pooled) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_STREAM_ECHO_KA));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID)
    chttpclient_set_request_timeout(cli, ERS_ECHO_TIMEOUT_US);
  chttp_request_t *req = ers_post(s.port, ERS_BIG_BODY, NULL);
  char get_url[128];
  snprintf(get_url, sizeof(get_url), "http://127.0.0.1:%d/after", s.port);
  chttp_request_t *get = chttp_request_new(CHTTP_GET, get_url, NULL, NULL);
  ccol_retval_t rv[4] = {0}, grv[4] = {0};
  int status[4] = {0};
  int accepts[4] = {0};
  for (int tier = 1; tier <= 3 && req && get && cli != CHTTPCLI_INVALID;
       tier++) {
    int before = atomic_load(&s.accepts);
    chttpcli_response *resp = NULL;
    rv[tier] = run_in_tier(tier, cli, req, &resp);
    status[tier] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
    resp = NULL;
    grv[tier] = run_in_tier(tier, cli, get, &resp);
    chttpclient_resp_free(resp);
    accepts[tier] = atomic_load(&s.accepts) - before;
  }
  bool have = req && get && cli != CHTTPCLI_INVALID;
  chttp_request_free(req);
  chttp_request_free(get);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int tier = 1; tier <= 3; tier++) {
    REQUIRE_EQ(rv[tier], ccol_success);
    REQUIRE_EQ(status[tier], 200);
    REQUIRE_EQ(grv[tier], ccol_success);
    REQUIRE_EQ(accepts[tier], 2);
  }
}

/* Several requests with "Expect: 100-continue" on one pooled connection,
 * each of which waits in the continue window across ticks of the deadline
 * sweep, which reads the continue deadline of the connection that each
 * request rewrites. Every request gets its answer on the one connection. */
TEST(expect_continue, pooled_continue_windows_over_sweep_ticks) {
  ers_srv_t s;
  REQUIRE_TRUE(ers_start(&s, ERS_SLOW_CONTINUE));
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 10000000);
  chttp_request_t *req = ers_post(s.port, 64, NULL);
  if (req) req->expect_continue = true;
  ccol_retval_t rv[4] = {0};
  int status[4] = {0};
  for (int i = 1; i <= 3 && req && cli != CHTTPCLI_INVALID; i++) {
    chttpcli_response *resp = NULL;
    rv[i] = run_in_tier(2, cli, req, &resp);
    status[i] = resp ? resp->status_code : 0;
    chttpclient_resp_free(resp);
  }
  bool have = req && cli != CHTTPCLI_INVALID;
  int accepts = atomic_load(&s.accepts);
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  ers_stop(&s);

  REQUIRE_TRUE(have);
  for (int i = 1; i <= 3; i++) {
    REQUIRE_EQ(rv[i], ccol_success);
    REQUIRE_EQ(status[i], 200);
  }
  REQUIRE_EQ(accepts, 1);
}

extern void _chttp_continue_sweep_arm_for_tests(void);
extern bool _chttp_continue_sweep_parked_for_tests(void);
extern void _chttp_continue_sweep_release_for_tests(void);
extern unsigned _chttp_continue_deadline_stores_for_tests(void);

/* A one-connection server for the test below. Request 1 waits for go1, then
 * gets "100 Continue", its body is read, and it gets 200 with keep-alive.
 * Request 2 never gets "100 Continue": the server waits up to 5 s for its
 * body and records how long after the header block the first body byte
 * arrived, then reads the rest and answers 200. */
typedef struct {
  int lfd;
  int port;
  pthread_t tid;
  atomic_int go1;
  atomic_long body2_after_ms; /* -1 until request 2 sent body bytes */
  atomic_int done;
} cwin_srv_t;

static void *cwin_srv_thread(void *arg) {
  cwin_srv_t *s = (cwin_srv_t *)arg;
  int fd = accept(s->lfd, NULL, NULL);
  char *buf = (char *)malloc(TEST_SERVER_BUF);
  if (fd < 0 || !buf) goto out;
  for (int req = 1; req <= 2; req++) {
    size_t hl = 0;
    ssize_t n = srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
    if (n <= 0 || hl == 0) goto out;
    long t_h = ers_now_ms();
    size_t cl = ers_content_length(buf);
    size_t have = (size_t)n - hl;
    if (req == 1) {
      for (int i = 0; i < 1500 && !atomic_load(&s->go1); i++) usleep(10000);
      static const char c100[] = "HTTP/1.1 100 Continue\r\n\r\n";
      ers_send_all(fd, c100, sizeof(c100) - 1);
    }
    while (have < cl) {
      ssize_t r = recv(fd, buf, TEST_SERVER_BUF, 0);
      if (r <= 0) goto out;
      if (req == 2 && have == 0)
        atomic_store(&s->body2_after_ms, ers_now_ms() - t_h);
      have += (size_t)r;
    }
    static const char ok[] = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    ers_send_all(fd, ok, sizeof(ok) - 1);
  }
  /* Wait for the client to close its pooled connection. */
  {
    size_t hl = 0;
    (void)srv_read_headers(fd, buf, TEST_SERVER_BUF, &hl);
  }
out:
  free(buf);
  if (fd >= 0) close(fd);
  atomic_store(&s->done, 1);
  return NULL;
}

/* Waits until the count of continue_deadline stores moves past `from`,
 * then releases the parked sweep. It runs on a thread of its own, created
 * before the second request starts, so that nothing orders the store of the
 * request with the release: the second request may store its deadline on
 * the thread that submits it. */
typedef struct {
  unsigned from;
  atomic_int stored;
} cwin_release_t;

static void *cwin_release_thread(void *arg) {
  cwin_release_t *r = (cwin_release_t *)arg;
  bool stored = false;
  for (int i = 0; i < 5000 && !stored; i++) {
    stored = _chttp_continue_deadline_stores_for_tests() != r->from;
    if (!stored) usleep(1000);
  }
  atomic_store(&r->stored, stored ? 1 : 0);
  _chttp_continue_sweep_release_for_tests();
  return NULL;
}

/* The continue window of a request is judged by its own deadline only. The
 * deadline sweep snapshots a pooled connection while its first request
 * waits for "100 Continue" and is held right there, past the end of that
 * window; meanwhile the first request completes and the second request on
 * the same connection starts its own window. The second request must still
 * wait its whole window, about 1 s, before it sends its body. This test is
 * non-vacuous: when the write dispatch of the wait sends the body on the
 * word of the sweep alone, the deadline of the first request ends the
 * window of the second at once and its body arrives within milliseconds.
 * Under ThreadSanitizer it also checks that the sweep never reads the
 * deadline of the connection outside the lock under which the request
 * stores it: the second request stores its deadline while the sweep is
 * held, and the thread that releases the sweep learns of that store
 * through a relaxed counter that orders nothing. */
TEST(expect_continue, a_stale_sweep_snapshot_never_ends_a_later_window) {
  cwin_srv_t s;
  memset(&s, 0, sizeof(s));
  atomic_store(&s.body2_after_ms, -1L);
  s.lfd = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t alen = sizeof(a);
  bool listening = s.lfd >= 0 &&
                   bind(s.lfd, (struct sockaddr *)&a, sizeof(a)) == 0 &&
                   listen(s.lfd, 4) == 0 &&
                   getsockname(s.lfd, (struct sockaddr *)&a, &alen) == 0;
  s.port = ntohs(a.sin_port);
  bool srv_started =
      listening && pthread_create(&s.tid, NULL, cwin_srv_thread, &s) == 0;

  chttpcli cli = srv_started ? ccol_create_chttpclient(NULL) : CHTTPCLI_INVALID;
  if (cli != CHTTPCLI_INVALID) chttpclient_set_request_timeout(cli, 10000000);
  chttp_request_t *req = srv_started ? ers_post(s.port, 64, NULL) : NULL;
  if (req) req->expect_continue = true;
  bool have = req && cli != CHTTPCLI_INVALID;

  bool parked = false, stored = false;
  ccol_retval_t rv1 = ccol_unexpected_failure, rv2 = ccol_unexpected_failure;
  int st1 = 0, st2 = 0;
  if (have) {
    _chttp_continue_sweep_arm_for_tests();
    ctpool_future *f1 = chttpclient_do_async(cli, req);
    for (int i = 0; f1 && i < 5000 && !parked; i++) {
      parked = _chttp_continue_sweep_parked_for_tests();
      if (!parked) usleep(1000);
    }
    /* The window of the first request ends while the sweep is held. */
    if (parked) usleep(1200000);
    atomic_store(&s.go1, 1);
    if (f1) {
      chttpcli_async_result_t *r = chttpclient_async_result_get(f1);
      if (r) {
        rv1 = r->rv;
        st1 = r->resp ? r->resp->status_code : 0;
        chttpclient_resp_free(r->resp);
        chttpclient_async_result_free(r);
      }
      ctpool_future_free(f1);
    }
    cwin_release_t rel = {.from = _chttp_continue_deadline_stores_for_tests()};
    atomic_store(&rel.stored, 0);
    pthread_t rel_tid;
    bool rel_started =
        parked && rv1 == ccol_success &&
        pthread_create(&rel_tid, NULL, cwin_release_thread, &rel) == 0;
    ctpool_future *f2 = rel_started ? chttpclient_do_async(cli, req) : NULL;
    if (rel_started) {
      pthread_join(rel_tid, NULL);
      stored = atomic_load(&rel.stored) != 0;
    }
    _chttp_continue_sweep_release_for_tests();
    if (f2) {
      chttpcli_async_result_t *r = chttpclient_async_result_get(f2);
      if (r) {
        rv2 = r->rv;
        st2 = r->resp ? r->resp->status_code : 0;
        chttpclient_resp_free(r->resp);
        chttpclient_async_result_free(r);
      }
      ctpool_future_free(f2);
    }
  }
  _chttp_continue_sweep_release_for_tests();
  chttp_request_free(req);
  if (cli != CHTTPCLI_INVALID) chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  if (srv_started) {
    test_wake_listener(s.lfd);
    pthread_join(s.tid, NULL);
  }
  if (s.lfd >= 0) close(s.lfd);
  long body2_after = atomic_load(&s.body2_after_ms);

  REQUIRE_TRUE(srv_started);
  REQUIRE_TRUE(have);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(stored);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_EQ(st1, 200);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_EQ(st2, 200);
  REQUIRE_GE(body2_after, 900L);
}

extern bool _chttp_deadline_remaining_ms_at_for_tests(long long ahead_ns,
                                                      int *out_ms);

/* A remaining time is rounded up to whole milliseconds, so that a wait of
 * that many milliseconds never ends before the deadline. This test is
 * non-vacuous: a truncating conversion gives 0 or "passed" for a deadline
 * that lies less than a millisecond ahead, and 1 for one 1.5 ms ahead. */
TEST(deadline, remaining_milliseconds_round_up) {
  int ms = -7;
  REQUIRE_TRUE(_chttp_deadline_remaining_ms_at_for_tests(1, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(_chttp_deadline_remaining_ms_at_for_tests(500000, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(_chttp_deadline_remaining_ms_at_for_tests(1000000, &ms));
  REQUIRE_EQ(ms, 1);
  REQUIRE_TRUE(_chttp_deadline_remaining_ms_at_for_tests(1500000, &ms));
  REQUIRE_EQ(ms, 2);
  REQUIRE_TRUE(_chttp_deadline_remaining_ms_at_for_tests(2999999999LL, &ms));
  REQUIRE_EQ(ms, 3000);
  REQUIRE_FALSE(_chttp_deadline_remaining_ms_at_for_tests(0, &ms));
  REQUIRE_FALSE(_chttp_deadline_remaining_ms_at_for_tests(-1, &ms));
}

/* Runs one GET of /slow, which the server answers after about 100 ms, with
 * the given timeouts, through Tier 1 or Tier 2. It returns the result and
 * the HTTP status, and stores the elapsed time in *elapsed_ms. */
static ccol_retval_t huge_timeout_get(uint64_t connect_us, uint64_t request_us,
                                      bool async, int *status,
                                      long *elapsed_ms) {
  char url[160];
  make_url(url, sizeof(url), "/slow");
  *status = -1;
  *elapsed_ms = -1;
  chttpcli cli = ccol_create_chttpclient(NULL);
  if (cli == CHTTPCLI_INVALID) return ccol_unexpected_failure;
  ccol_retval_t rv = chttpclient_set_connect_timeout(cli, connect_us);
  if (rv == ccol_success) rv = chttpclient_set_request_timeout(cli, request_us);
  if (rv == ccol_success) {
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    if (async) {
      ctpool_future *f = async_get(cli, url);
      chttpcli_async_result_t *raw = f ? chttpclient_async_result_get(f) : NULL;
      rv = raw ? raw->rv : ccol_unexpected_failure;
      if (raw && raw->resp) {
        *status = raw->resp->status_code;
        chttpclient_resp_free(raw->resp);
      }
      if (raw) chttpclient_async_result_free(raw);
      if (f) ctpool_future_free(f);
    } else {
      chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
      chttpcli_response *resp = NULL;
      rv = req ? chttpclient_do(cli, req, &resp) : ccol_unexpected_failure;
      if (resp) {
        *status = resp->status_code;
        chttpclient_resp_free(resp);
      }
      if (req) chttp_request_free(req);
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    *elapsed_ms =
        (t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L;
  }
  chttpclient_destroy(cli);
  if (async) wait_for_async_engine_idle();
  return rv;
}

/* A timeout of 0 is no limit, and the largest timeouts behave as no limit:
 * each request waits for the slow answer and succeeds, in Tier 1 and in Tier
 * 2. This test is non-vacuous: a conversion that narrows the timeout to a
 * signed type, or a deadline that wraps into the past, makes every request
 * with UINT64_MAX fail with ccol_timed_out at once. */
TEST(client_construction, zero_and_huge_timeouts_wait_for_the_answer) {
  const uint64_t timeouts[] = {0, UINT64_MAX, UINT64_MAX - 1};
  enum { N = sizeof(timeouts) / sizeof(timeouts[0]) };
  ccol_retval_t rv[2][N];
  int status[2][N];
  long elapsed[2][N];
  for (int tier = 0; tier < 2; tier++)
    for (int k = 0; k < N; k++)
      rv[tier][k] = huge_timeout_get(timeouts[k], timeouts[k], tier == 1,
                                     &status[tier][k], &elapsed[tier][k]);
  for (int tier = 0; tier < 2; tier++)
    for (int k = 0; k < N; k++) {
      REQUIRE_EQ(rv[tier][k], ccol_success);
      REQUIRE_EQ(status[tier][k], 200);
      /* /slow answers after about 100 ms, so the request waited for it. */
      REQUIRE_GT(elapsed[tier][k], 50L);
    }
}

/* A small request timeout is measured in microseconds: 20000 us against
 * /slow, which answers after about 100 ms, times out. This test is
 * non-vacuous: a client that reads the value as milliseconds waits 20 s,
 * and the request succeeds. */
TEST(client_construction, a_small_request_timeout_times_out_in_microseconds) {
  int status = 0;
  long elapsed = 0;
  ccol_retval_t rv = huge_timeout_get(0, 20000, false, &status, &elapsed);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(elapsed, 5000L);
}

/* ========================================================================== */
/*            SCRIPTED PEERS: FRAMING, UPGRADE AND REUSE CHECKS               */
/* ========================================================================== */

/* A listener on an ephemeral loopback port that runs fn for each accepted
 * connection on a thread of its own. Every wait inside fn is bounded and
 * also ends on `stop`, so scripted_peer_stop joins every thread it started.
 * A rcvbuf that is not 0 is set on the listener before listen(), and every
 * accepted socket inherits it: a peer that does not read then fills after a
 * few kilobytes, so that a client that writes to it blocks at once. */
#define SCRIPTED_PEER_MAX_CONNS 8

typedef struct scripted_peer scripted_peer;
typedef void (*scripted_peer_fn)(scripted_peer *sp, int fd, int index);

struct scripted_peer {
  int lfd;
  int port;
  scripted_peer_fn fn;
  pthread_t accept_tid;
  bool accept_started;
  pthread_t conn_tids[SCRIPTED_PEER_MAX_CONNS];
  atomic_int nconn;
  atomic_bool stop;
  /* What the test tells a connection thread, and what that thread saw. */
  atomic_int phase;
  atomic_llong body_bytes;
  atomic_int saw_eof;
  atomic_int requests;
  /* The bytes that the stray-input scenarios send; set before the start. */
  const char *stray;
};

typedef struct {
  scripted_peer *sp;
  int fd;
  int index;
} scripted_peer_conn_arg;

static void *scripted_peer_conn_main(void *arg) {
  scripted_peer_conn_arg a = *(scripted_peer_conn_arg *)arg;
  free(arg);
  a.sp->fn(a.sp, a.fd, a.index);
  close(a.fd);
  return NULL;
}

static void *scripted_peer_accept_main(void *arg) {
  scripted_peer *sp = (scripted_peer *)arg;
  while (!atomic_load(&sp->stop)) {
    struct pollfd p = {.fd = sp->lfd, .events = POLLIN, .revents = 0};
    if (poll(&p, 1, 50) <= 0) continue;
    int fd = accept(sp->lfd, NULL, NULL);
    if (fd < 0) continue;
    int idx = atomic_load(&sp->nconn);
    scripted_peer_conn_arg *a = NULL;
    if (idx < SCRIPTED_PEER_MAX_CONNS)
      a = (scripted_peer_conn_arg *)malloc(sizeof(*a));
    if (!a) {
      close(fd);
      continue;
    }
    a->sp = sp;
    a->fd = fd;
    a->index = idx;
    if (pthread_create(&sp->conn_tids[idx], NULL, scripted_peer_conn_main, a) !=
        0) {
      free(a);
      close(fd);
      continue;
    }
    atomic_store(&sp->nconn, idx + 1);
  }
  return NULL;
}

static int scripted_peer_start(scripted_peer *sp, scripted_peer_fn fn,
                               int rcvbuf) {
  sp->fn = fn;
  sp->accept_started = false;
  atomic_store(&sp->nconn, 0);
  atomic_store(&sp->stop, false);
  atomic_store(&sp->phase, 0);
  atomic_store(&sp->body_bytes, 0);
  atomic_store(&sp->saw_eof, 0);
  atomic_store(&sp->requests, 0);
  sp->lfd = socket(AF_INET, SOCK_STREAM, 0);
  if (sp->lfd < 0) return -1;
  (void)fcntl(sp->lfd, F_SETFD, FD_CLOEXEC);
  if (rcvbuf > 0)
    setsockopt(sp->lfd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
  struct sockaddr_in addr;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
  socklen_t len = sizeof(addr);
  if (bind(sp->lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 ||
      listen(sp->lfd, 16) != 0 ||
      getsockname(sp->lfd, (struct sockaddr *)&addr, &len) != 0) {
    close(sp->lfd);
    sp->lfd = -1;
    return -1;
  }
  sp->port = ntohs(addr.sin_port);
  if (pthread_create(&sp->accept_tid, NULL, scripted_peer_accept_main, sp) !=
      0) {
    close(sp->lfd);
    sp->lfd = -1;
    return -1;
  }
  sp->accept_started = true;
  return 0;
}

static void scripted_peer_stop(scripted_peer *sp) {
  atomic_store(&sp->stop, true);
  if (sp->accept_started) pthread_join(sp->accept_tid, NULL);
  sp->accept_started = false;
  int n = atomic_load(&sp->nconn);
  for (int i = 0; i < n; i++) pthread_join(sp->conn_tids[i], NULL);
  if (sp->lfd >= 0) close(sp->lfd);
  sp->lfd = -1;
}

/* Reads up to the end of a request head, for at most ms milliseconds.
 * Returns the number of bytes read, or -1. */
static ssize_t scripted_peer_read_head(scripted_peer *sp, int fd, char *buf,
                                       size_t max, int ms) {
  size_t total = 0;
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop);) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&p, 1, 50);
    if (rc == 0) {
      waited += 50;
      continue;
    }
    if (rc < 0) return -1;
    ssize_t n = recv(fd, buf + total, max - 1 - total, 0);
    if (n <= 0) return -1;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "\r\n\r\n")) return (ssize_t)total;
    if (total >= max - 1) return -1;
  }
  return -1;
}

/* Reads and counts whatever arrives into sp->body_bytes, until the peer
 * closes (sp->saw_eof), an error, stop, or ms milliseconds. */
static void scripted_peer_drain(scripted_peer *sp, int fd, int ms) {
  char b[16384];
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop);) {
    struct pollfd p = {.fd = fd, .events = POLLIN, .revents = 0};
    int rc = poll(&p, 1, 50);
    if (rc == 0) {
      waited += 50;
      continue;
    }
    if (rc < 0) return;
    ssize_t n = recv(fd, b, sizeof(b), 0);
    if (n == 0) {
      atomic_store(&sp->saw_eof, 1);
      return;
    }
    if (n < 0) return;
    atomic_fetch_add(&sp->body_bytes, (long long)n);
  }
}

/* Waits until stop, or ms milliseconds, without reading. */
static void scripted_peer_idle(scripted_peer *sp, int ms) {
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop); waited += 20)
    usleep(20000);
}

/* Waits for the test to raise sp->phase to at least `phase`. */
static bool scripted_peer_wait_phase(scripted_peer *sp, int phase, int ms) {
  for (int waited = 0; waited < ms && !atomic_load(&sp->stop); waited += 5) {
    if (atomic_load(&sp->phase) >= phase) return true;
    usleep(5000);
  }
  return atomic_load(&sp->phase) >= phase;
}

static void scripted_peer_send(int fd, const char *s) {
  size_t len = strlen(s), off = 0;
  while (off < len) {
    ssize_t n = send(fd, s + off, len - off, MSG_NOSIGNAL);
    if (n <= 0) return;
    off += (size_t)n;
  }
}

static bool scripted_peer_wait_eof(scripted_peer *sp, int ms) {
  for (int waited = 0; waited < ms; waited += 5) {
    if (atomic_load(&sp->saw_eof)) return true;
    usleep(5000);
  }
  return atomic_load(&sp->saw_eof) != 0;
}

/* Runs req through Tier 1 (tier 1), Tier 2 (tier 2, through the future) or
 * Tier 3 (tier 3). *resp_out gets the response, if there is one. */
static ccol_retval_t scripted_do(chttpcli cli, const chttp_request_t *req,
                                 int tier, chttpcli_response **resp_out) {
  *resp_out = NULL;
  if (tier == 1) return chttpclient_do(cli, req, resp_out);
  if (tier == 3) return chttpclient_do_pooled(cli, req, resp_out);
  ctpool_future *f = chttpclient_do_async(cli, req);
  if (!f) return ccol_unexpected_failure;
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  ccol_retval_t rv = ccol_unexpected_failure;
  if (raw) {
    rv = raw->rv;
    *resp_out = raw->resp;
    chttpclient_async_result_free(raw);
  }
  ctpool_future_free(f);
  return rv;
}

/* ---- A TLS peer that sends part of a record and stops reading ---------- */

/* The peer completes the handshake, reads the start of the request, and
 * then sends the first 8 bytes of one TLS record and nothing more. It never
 * reads again. The client then holds the start of a record that no read can
 * finish, while the send of its body has no room. */
static void scripted_tls_partial_record(scripted_peer *sp, int fd, int index) {
  (void)index;
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
  if (!ssl) return;
  SSL_set_fd(ssl, fd);
  if (SSL_accept(ssl) == 1) {
    char b[4096];
    (void)SSL_read(ssl, b, sizeof(b));
    BIO *mem = BIO_new(BIO_s_mem());
    if (mem) {
      /* The SSL takes one reference with set0; this function keeps the
       * other, so that the record can be read after the write. */
      BIO_up_ref(mem);
      SSL_set0_wbio(ssl, mem);
      const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
      (void)SSL_write(ssl, resp, (int)strlen(resp));
      char *rec = NULL;
      long rec_len = BIO_get_mem_data(mem, &rec);
      if (rec_len > 8) (void)send(fd, rec, 8, MSG_NOSIGNAL);
      atomic_store(&sp->phase, 1);
      scripted_peer_idle(sp, 10000);
      BIO_free(mem);
    }
  }
  SSL_free(ssl);
}

static double thread_cpu_seconds(void) {
  struct timespec ts;
  clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
  return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* Tier 1 sends a body while it watches for an early response. When the
 * TLS layer holds the start of a record that is not complete, and the
 * socket has no room, the send must wait in poll(2) until the deadline. The
 * request times out after 2 s; the thread that ran it spends a small part
 * of that on the CPU. This test is non-vacuous: a send loop that trusts the
 * pending-input report of the TLS layer after a read that blocked spins for
 * the whole 2 s and spends about 2 s of CPU time. */
TEST(tls_partial_record, tier1_upload_waits_without_spinning) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  REQUIRE_EQ(scripted_peer_start(&sp, scripted_tls_partial_record, 4096), 0);

  chttpcli cli = ccol_create_chttpclient(NULL);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.ca_bundle_path = g_tls_cert_path;
  ccol_retval_t set_rv = chttpclient_set_tls(cli, &tls);
  (void)chttpclient_set_request_timeout(cli, 2000000);

  size_t body_len = 8u << 20;
  char *body = (char *)calloc(1, body_len);
  char url[96];
  snprintf(url, sizeof(url), "https://127.0.0.1:%d/upload", sp.port);
  chttp_request_t *req =
      body ? chttp_request_new(CHTTP_POST, url,
                               &CHTTP_BODY(body, body_len, "x/y"), NULL)
           : NULL;
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = ccol_unexpected_failure;
  double cpu = 0;
  if (req) {
    double c0 = thread_cpu_seconds();
    rv = chttpclient_do(cli, req, &resp);
    cpu = thread_cpu_seconds() - c0;
  }
  int phase = atomic_load(&sp.phase);
  chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  scripted_peer_stop(&sp);
  free(body);

  REQUIRE_EQ(set_rv, ccol_success);
  REQUIRE_NE((void *)req, NULL);
  REQUIRE_EQ(phase, 1);
  REQUIRE_EQ(rv, ccol_timed_out);
  REQUIRE_LT(cpu, 1.0);
}

#define TLS_PARTIAL_RECORD_BODY_LEN (1u << 20)

/* The peer completes the handshake and sends the first 8 bytes of its
 * response record while it goes on reading. It sends the rest of the record
 * once the whole body arrived, as a server that answers when it has read
 * the request does. */
static void scripted_tls_partial_record_reading(scripted_peer *sp, int fd,
                                                int index) {
  (void)index;
  struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
  SSL *ssl = SSL_new(g_tls_ssl_ctx);
  if (!ssl) return;
  SSL_set_fd(ssl, fd);
  BIO *mem = NULL;
  if (SSL_accept(ssl) == 1 && (mem = BIO_new(BIO_s_mem())) != NULL) {
    BIO_up_ref(mem);
    SSL_set0_wbio(ssl, mem);
    const char *resp = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    (void)SSL_write(ssl, resp, (int)strlen(resp));
    char *rec = NULL;
    long rec_len = BIO_get_mem_data(mem, &rec);
    if (rec_len > 8 && send(fd, rec, 8, MSG_NOSIGNAL) == 8) {
      /* Reads the head and the body; the body follows the blank line. */
      char b[16384];
      long long body = -1;
      while (!atomic_load(&sp->stop)) {
        int n = SSL_read(ssl, b, sizeof(b));
        if (n <= 0) break;
        if (body < 0) {
          size_t take = (size_t)n < sizeof(b) - 1 ? (size_t)n : sizeof(b) - 1;
          b[take] = '\0';
          char *end = strstr(b, "\r\n\r\n");
          if (end) body = (long long)((size_t)n - (size_t)(end + 4 - b));
        } else {
          body += n;
        }
        if (body >= (long long)TLS_PARTIAL_RECORD_BODY_LEN) break;
      }
      atomic_store(&sp->body_bytes, body);
      if (body >= (long long)TLS_PARTIAL_RECORD_BODY_LEN)
        (void)send(fd, rec + 8, (size_t)(rec_len - 8), MSG_NOSIGNAL);
      scripted_peer_idle(sp, 10000);
    }
  }
  SSL_free(ssl);
  if (mem) BIO_free(mem);
}

/* The start of a TLS record that is not complete must not hold the send of
 * the body back while the socket has room: the peer reads the whole body
 * and only then sends the rest of its answer. Every tier gets that answer.
 * This test is non-vacuous: an engine that treats the start of a record as
 * input to read pauses the body for a read that cannot finish, and the
 * request times out. */
TEST(tls_partial_record, upload_goes_on_while_the_peer_reads) {
  if (!g_tls_cert_ready) {
    fprintf(stderr,
            "SKIP: no self-signed cert available in this environment\n");
    return;
  }
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(
        scripted_peer_start(&sp, scripted_tls_partial_record_reading, 4096), 0);
    chttpcli cli = ccol_create_chttpclient(NULL);
    chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
    tls.ca_bundle_path = g_tls_cert_path;
    ccol_retval_t set_rv = chttpclient_set_tls(cli, &tls);
    (void)chttpclient_set_request_timeout(cli, 10000000);
    char *body = (char *)calloc(1, TLS_PARTIAL_RECORD_BODY_LEN);
    char url[96];
    snprintf(url, sizeof(url), "https://127.0.0.1:%d/upload", sp.port);
    chttp_request_t *req =
        body ? chttp_request_new(
                   CHTTP_POST, url,
                   &CHTTP_BODY(body, TLS_PARTIAL_RECORD_BODY_LEN, "x/y"), NULL)
             : NULL;
    chttpcli_response *resp = NULL;
    ccol_retval_t rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    int status = resp ? resp->status_code : -1;
    chttpclient_resp_free(resp);
    chttp_request_free(req);
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
    scripted_peer_stop(&sp);
    long long got = atomic_load(&sp.body_bytes);
    free(body);

    REQUIRE_EQ(set_rv, ccol_success);
    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_EQ(got, (long long)TLS_PARTIAL_RECORD_BODY_LEN);
  }
}

/* ---- "100 Continue" with the final response in the same read ----------- */

/* The peer answers the head of the request with "100 Continue" and a
 * complete final response in one send(), and then counts every byte that
 * arrives until the client closes. */
static void scripted_continue_with_final(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  /* Anything past the head is the start of the body. */
  const char *end = strstr(head, "\r\n\r\n");
  if (end) {
    size_t extra = strlen(end + 4);
    atomic_fetch_add(&sp->body_bytes, (long long)extra);
  }
  scripted_peer_send(fd,
                     "HTTP/1.1 100 Continue\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  scripted_peer_drain(sp, fd, 10000);
}

/* A "100 Continue" whose read also holds a complete final response: that
 * response answers the request, the body is never sent, and the connection
 * is closed and not pooled, in every tier. This test is non-vacuous: an
 * engine that sends the body first and parses the final response after it
 * sends all 256 KiB of the body to the peer, and pools the connection. */
TEST(expect_continue_hardening, final_response_with_the_100_stops_the_body) {
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_continue_with_final, 0), 0);

    chttpcli cli = ccol_create_chttpclient(NULL);
    (void)chttpclient_set_request_timeout(cli, 10000000);
    size_t body_len = 256u * 1024u;
    char *body = (char *)calloc(1, body_len);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/upload", sp.port);
    chttp_request_t *req =
        body ? chttp_request_new(CHTTP_POST, url,
                                 &CHTTP_BODY(body, body_len, "x/y"), NULL)
             : NULL;
    if (req) req->expect_continue = true;
    chttpcli_response *resp = NULL;
    ccol_retval_t rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    int status = resp ? resp->status_code : -1;
    bool body_ok = resp && resp->body && strcmp(resp->body, "ok") == 0;
    /* The client closes a connection that it does not pool, so the peer
     * sees the end of the stream while the client still exists. */
    bool closed = scripted_peer_wait_eof(&sp, 5000);
    long long sent = atomic_load(&sp.body_bytes);
    chttpclient_resp_free(resp);
    chttp_request_free(req);
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
    scripted_peer_stop(&sp);
    free(body);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_success);
    REQUIRE_EQ(status, 200);
    REQUIRE_TRUE(body_ok);
    REQUIRE_TRUE(closed);
    REQUIRE_EQ(sent, 0LL);
  }
}

/* ---- An unsolicited "101 Switching Protocols" --------------------------- */

/* The peer answers with a 101 and, after it, bytes that read as an HTTP
 * response. After a 101 those bytes belong to another protocol. */
static void scripted_switching_protocols(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  scripted_peer_send(fd,
                     "HTTP/1.1 101 Switching Protocols\r\n"
                     "Connection: upgrade\r\nUpgrade: websocket\r\n\r\n"
                     "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
  scripted_peer_drain(sp, fd, 10000);
}

/* The client never asks for an upgrade, so a 101 fails the request with
 * ccol_http_transfer_aborted in every tier, with and without a wait for
 * "100 Continue". This test is non-vacuous: a client that discards a 101
 * like any other interim response reads the bytes after it as the final
 * response and reports a 200. */
TEST(interim_responses, unsolicited_101_fails_every_tier) {
  for (int variant = 0; variant < 6; variant++) {
    int tier = 1 + variant % 3;
    bool with_continue = variant >= 3;
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_switching_protocols, 0), 0);

    chttpcli cli = ccol_create_chttpclient(NULL);
    (void)chttpclient_set_request_timeout(cli, 10000000);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ws", sp.port);
    const char *payload = "payload";
    chttp_request_t *req =
        with_continue ? chttp_request_new(
                            CHTTP_POST, url,
                            &CHTTP_TEXT_BODY(payload, strlen(payload)), NULL)
                      : chttp_request_new(CHTTP_GET, url, NULL, NULL);
    if (req) req->expect_continue = with_continue;
    chttpcli_response *resp = NULL;
    ccol_retval_t rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    bool got_resp = resp != NULL;
    chttpclient_resp_free(resp);
    chttp_request_free(req);
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
    int requests = atomic_load(&sp.requests);
    scripted_peer_stop(&sp);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(requests, 1);
    REQUIRE_EQ(rv, ccol_http_transfer_aborted);
    REQUIRE_FALSE(got_resp);
  }
}

/* ---- Hosts outside the uri-host grammar --------------------------------- */

TEST(url_parsing, host_outside_the_uri_host_grammar_is_invalid) {
  static const char *const bad[] = {
      "http://a b/",
      "http://a\tb:80/",
      "http://h\x01/",
      "http://h\x7f/",
      "http://ex\xc3\xa4mple.com/",
      "http://a\\b/",
      "http://a\"b/",
      "http://a<b>/",
      "http://a{b}/",
      "http://a^b/",
      "http://a|b/",
      "http://a`b/",
      "http://a%/",
      "http://a%4/",
      "http://a%zz/",
      "http://[evil/x]/y",
      "http://[evil?x]/",
      "http://[evil#x]/",
      "http://[::1%25lo]/",
      "http://[::1%lo]/",
      "http://[127.0.0.1]/",
      "http://[::1 ]/",
      "http://[::g]/",
      "http://[1:2:3:4:5:6:7:8:9]/",
      "http://[v]/",
      "http://[v1]/",
      "http://[v1.]/",
      "http://[vx.a]/",
      "http://[v1.a/b]/",
      "https://a\r\nb/",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char *host = NULL;
    ccol_retval_t rv = _chttp_parse_url_for_tests(
        bad[i], NULL, NULL, &host, NULL, NULL, NULL, NULL, NULL, NULL);
    bool refused = (rv == ccol_http_invalid_url) && host == NULL;
    free(host);
    if (!refused) fprintf(stderr, "accepted: \"%s\"\n", bad[i]);
    REQUIRE_TRUE(refused);
  }
}

TEST(url_parsing, every_uri_host_form_is_accepted_as_written) {
  static const struct {
    const char *url;
    const char *host;
    bool ipv6;
  } good[] = {
      {"http://example.com/", "example.com", false},
      {"http://Ex-am_ple.~co/", "Ex-am_ple.~co", false},
      {"http://a!$&'()*+,;=b/", "a!$&'()*+,;=b", false},
      {"http://xn--exmple-cua.com/", "xn--exmple-cua.com", false},
      {"http://a%41b/", "a%41b", false},
      {"http://10.1.2.3:8080/", "10.1.2.3", false},
      {"http://localhost./", "localhost.", false},
      {"http://[::1]/", "::1", true},
      {"http://[::ffff:192.0.2.1]/", "::ffff:192.0.2.1", true},
      {"http://[2001:DB8::a:1]:81/", "2001:DB8::a:1", true},
      {"http://[v1.fe80::a+en1]/", "v1.fe80::a+en1", true},
      {"http://[VF.x!$:]/", "VF.x!$:", true},
  };
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    char *host = NULL, *pq = NULL, *key = NULL;
    bool ipv6 = !good[i].ipv6;
    ccol_retval_t rv = _chttp_parse_url_for_tests(
        good[i].url, NULL, &ipv6, &host, NULL, &pq, &key, NULL, NULL, NULL);
    bool ok = rv == ccol_success && host && strcmp(host, good[i].host) == 0 &&
              ipv6 == good[i].ipv6;
    free(host);
    free(pq);
    free(key);
    if (!ok) fprintf(stderr, "refused or changed: \"%s\"\n", good[i].url);
    REQUIRE_TRUE(ok);
  }
}

TEST(url_parsing, userinfo_outside_the_grammar_is_invalid) {
  /* "a\@b": this parser names the host after the '@', and a reader that
   * treats a backslash as '/' names the one before it. */
  static const char *const bad[] = {
      "http://good.example\\@evil.example/",
      "http://u ser:p@h/",
      "http://u:p\tw@h/",
      "http://u:p\x01@h/",
      "http://u:p\xc3\xa4@h/",
      "http://u:p[w]@h/",
      "http://u:p\"w@h/",
  };
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    ccol_retval_t rv = _chttp_parse_url_for_tests(
        bad[i], NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
    if (rv != ccol_http_invalid_url)
      fprintf(stderr, "accepted: \"%s\"\n", bad[i]);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
  }
  /* Every character of the userinfo rule, an unescaped '@' in the password
   * and a percent-encoded byte outside ASCII are accepted. */
  char *auth = NULL, *host = NULL;
  ccol_retval_t rv = _chttp_parse_url_for_tests(
      "http://a-._~!$&'()*+,;=:p@ss%C3%A4@h/", NULL, NULL, &host, NULL, NULL,
      NULL, &auth, NULL, NULL);
  bool host_ok = host && strcmp(host, "h") == 0;
  free(host);
  free(auth);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(host_ok);
}

/* The peer redirects /start to a Location whose host is a bracketed IPv4
 * address, which is no IP-literal, and answers every other path with 200.
 * The Location names the peer itself. */
static void scripted_bad_location(scripted_peer *sp, int fd, int index) {
  (void)index;
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  if (strncmp(head, "GET /start ", 11) == 0) {
    char resp[256];
    snprintf(resp, sizeof(resp),
             "HTTP/1.1 302 Found\r\nLocation: http://[127.0.0.1]:%d/next\r\n"
             "Content-Length: 0\r\nConnection: close\r\n\r\n",
             sp->port);
    scripted_peer_send(fd, resp);
  } else {
    scripted_peer_send(fd,
                       "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
                       "Connection: close\r\n\r\nok");
  }
  scripted_peer_drain(sp, fd, 2000);
}

/* A Location goes through the same grammar as a request URL. This test is
 * non-vacuous: a parser that takes everything up to the ']' as the host
 * connects to 127.0.0.1 and follows the redirect to a 200. */
TEST(redirect_hardening, location_host_outside_the_grammar_is_invalid) {
  for (int tier = 1; tier <= 3; tier++) {
    scripted_peer sp;
    memset(&sp, 0, sizeof(sp));
    REQUIRE_EQ(scripted_peer_start(&sp, scripted_bad_location, 0), 0);
    chttpcli cli = ccol_create_chttpclient(NULL);
    (void)chttpclient_set_request_timeout(cli, 10000000);
    char url[96];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/start", sp.port);
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
    chttpcli_response *resp = NULL;
    ccol_retval_t rv =
        req ? scripted_do(cli, req, tier, &resp) : ccol_unexpected_failure;
    chttpclient_resp_free(resp);
    chttp_request_free(req);
    chttpclient_destroy(cli);
    wait_for_async_engine_idle();
    int requests = atomic_load(&sp.requests);
    scripted_peer_stop(&sp);

    REQUIRE_NE((void *)req, NULL);
    REQUIRE_EQ(rv, ccol_http_invalid_url);
    REQUIRE_EQ(requests, 1);
  }
}

/* ---- Input on a pooled connection that a new hop takes ------------------ */

extern void _chttpclient_arm_idle_probe_hold_for_tests(void);
extern bool _chttpclient_wait_idle_probe_held_for_tests(long timeout_ms);
extern void _chttpclient_release_idle_probe_hold_for_tests(void);

/* The first connection answers one request with "first" and keeps the
 * connection open. Once the test raises the phase to 1, it sends sp->stray
 * on that idle connection, and then answers any further request on it with
 * "reuse". Every later connection answers with "fresh". */
static void scripted_stray_after_response(scripted_peer *sp, int fd,
                                          int index) {
  char head[8192];
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  if (index != 0) {
    scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfresh");
    scripted_peer_drain(sp, fd, 10000);
    return;
  }
  scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst");
  if (!scripted_peer_wait_phase(sp, 1, 10000)) return;
  scripted_peer_send(fd, sp->stray);
  if (scripted_peer_read_head(sp, fd, head, sizeof(head), 5000) < 0) return;
  atomic_fetch_add(&sp->requests, 1);
  scripted_peer_send(fd, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nreuse");
  scripted_peer_drain(sp, fd, 10000);
}

/* Runs the take-wins-the-race order with `stray` as the input that arrives
 * on the pooled connection, and reports what the second request got. */
static void run_stray_input_case(const char *stray, ccol_retval_t *rv1_out,
                                 ccol_retval_t *rv2_out, bool *held_out,
                                 char *body2, size_t body2_len,
                                 int *conns_out) {
  scripted_peer sp;
  memset(&sp, 0, sizeof(sp));
  sp.stray = stray;
  *rv1_out = *rv2_out = ccol_unexpected_failure;
  *held_out = false;
  body2[0] = '\0';
  *conns_out = -1;
  if (scripted_peer_start(&sp, scripted_stray_after_response, 0) != 0) return;

  chttpcli cli = ccol_create_chttpclient(NULL);
  (void)chttpclient_set_request_timeout(cli, 10000000);
  char url[96];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/get", sp.port);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  if (req) *rv1_out = scripted_do(cli, req, 3, &resp);
  chttpclient_resp_free(resp);
  resp = NULL;

  /* The first connection is in the idle pool now. The next dispatch that
   * finds it idle stops before its read, the peer sends the stray input,
   * and the second request takes the connection inside that window. */
  _chttpclient_arm_idle_probe_hold_for_tests();
  atomic_store(&sp.phase, 1);
  *held_out = _chttpclient_wait_idle_probe_held_for_tests(5000);
  ctpool_future *f = req ? chttpclient_do_async(cli, req) : NULL;
  _chttpclient_release_idle_probe_hold_for_tests();
  if (f) {
    chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
    if (raw) {
      *rv2_out = raw->rv;
      if (raw->resp && raw->resp->body)
        snprintf(body2, body2_len, "%s", raw->resp->body);
      chttpclient_resp_free(raw->resp);
      chttpclient_async_result_free(raw);
    }
    ctpool_future_free(f);
  }
  chttp_request_free(req);
  chttpclient_destroy(cli);
  wait_for_async_engine_idle();
  scripted_peer_stop(&sp);
  *conns_out = atomic_load(&sp.nconn);
}

/* Input that arrives on a pooled connection after its response answers no
 * request. A hop that takes the connection before the dispatch that would
 * evict it runs never reads that input as its own response: it goes to a
 * fresh connection. This test is non-vacuous: a hop that writes its request
 * on that connection reads the rest of the stray message as the start of
 * its response and fails with ccol_http_transfer_aborted. */
TEST(async_idle_pool, stray_input_on_a_taken_connection_is_never_read) {
  ccol_retval_t rv1, rv2;
  bool held;
  char body2[32];
  int conns;
  run_stray_input_case("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nstray",
                       &rv1, &rv2, &held, body2, sizeof(body2), &conns);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_STREQ(body2, "fresh");
  REQUIRE_EQ(conns, 2);
}

/* The same order with one stray byte, which the dispatch that found the
 * connection idle reads itself. The connection is still never reused. This
 * test is non-vacuous: without the mark that dispatch leaves, the hop finds
 * an empty socket, reuses the connection and gets "reuse". */
TEST(async_idle_pool, a_stray_byte_that_the_idle_dispatch_took_still_evicts) {
  ccol_retval_t rv1, rv2;
  bool held;
  char body2[32];
  int conns;
  run_stray_input_case("x", &rv1, &rv2, &held, body2, sizeof(body2), &conns);
  REQUIRE_EQ(rv1, ccol_success);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(rv2, ccol_success);
  REQUIRE_STREQ(body2, "fresh");
  REQUIRE_EQ(conns, 2);
}
