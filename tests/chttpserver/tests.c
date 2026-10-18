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

/* pthread_timedjoin_np (a glibc extension) is what makes _bounded_join below a
 * bounded join instead of a blind pthread_join. Without it a background thread
 * can hang the join forever when a real regression in the mechanism that a
 * race-hook test checks leaves that thread stuck for good, and then the whole
 * binary goes down instead of just that one test failing cleanly. The macro has
 * to be defined before the first #include that can pull in <pthread.h>
 * indirectly. tests_engine_stop.c puts it in the same place for the same
 * reason, and so do src/chttpserver.c and src/clogger.c. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/test_helpers/chttpserver.h>
#include <limits.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <pthread_np.h> /* pthread_timedjoin_np */
#endif

#include <tau/tau.h>
#include <test_connect.h>
#include <test_sanitizer.h>

TAU_MAIN()

/* The test clients of this binary write to the server with plain write(2), and
 * the server can close a connection while one of them is writing. The library
 * leaves the SIGPIPE disposition to the application, so this binary ignores it
 * itself; tests_sigpipe.c covers the library under the default disposition. */
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
}

/* ========================================================================== */
/*                         GLOBAL TEST SERVER STATE                           */
/* ========================================================================== */

#define TEST_PORT 18765
#define BASE_URL "http://127.0.0.1:18765"

static clog g_test_logger = CLOG_INVALID;
static chttpsvr g_srv = CHTTPSVR_INVALID;

/* Scope-exit cleanup for a plain client-side test socket fd, used as `int fd
   _ccol_destructor(_close_scoped_fd) = socket(...);`. A later REQUIRE_* check
   in the test can return early, before the test's final explicit close(fd), and
   the fd would then leak for the rest of this binary's run; this cleanup
   prevents that. It accepts a negative fd without complaining, because socket()
   or test_connect() can fail, which follows the close(2) convention, used
   elsewhere in this file too, that an EBADF is harmless to ignore here. */
static void _close_scoped_fd(int *fd) {
  if (fd && *fd >= 0) close(*fd);
}

/* Bounded join for a background thread that a test just released from a
   white-box race-test hook (_chttpsvr_release_*_race_hook_for_tests()). It's
   the same as _fx_timed_join in tests_engine_stop.c: same mechanism, same 30s
   bound and same reason. A regression can leave the released thread stuck in
   the mechanism under test, and that fails the REQUIRE_TRUE of this one
   test below instead of hanging the whole binary in an unbounded pthread_join.
   30s is far more than any real join in this file's race-hook tests, since each
   hook release unblocks its thread within microseconds, so the bound never
   fires against a thread that behaves correctly. Returns true only when tid
   stopped and this call joined it. On a false return the caller needs to call
   pthread_detach(tid), or the still running thread leaks as permanently
   joinable. */
static bool _bounded_join(pthread_t tid, void **retval) {
#if TEST_TIMEDJOIN_VISIBLE
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 30;
  return pthread_timedjoin_np(tid, retval, &deadline) == 0;
#else
  return pthread_join(tid, retval) == 0;
#endif
}

/* ========================================================================== */
/*                         ROUTE HANDLER FUNCTIONS                            */
/* ========================================================================== */

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, world!");
}

static void _echo_body_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  size_t len = 0;
  const void *body = chttpsvr_req_body(req, &len);
  if (body && len > 0) {
    chttpsvr_resp_write(resp, body, len);
  } else {
    chttpsvr_resp_write_str(resp, "(empty)");
  }
}

static void _echo_param_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  const char *id = chttpsvr_req_param(req, "id");
  const char *sub = chttpsvr_req_param(req, "sub");
  char buf[256];
  int n = snprintf(buf, sizeof(buf), "id=%s sub=%s", id ? id : "(nil)",
                   sub ? sub : "(nil)");
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

/* Writes ctx, a literal string the caller owns, as it is. The per-router
   segment-decode cache tests below use it to tell which route matched, out of
   several routes that share one decoded prefix. */
static void _cache_literal_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

/* Writes "<ctx>:<id param>". The per-router segment-decode cache tests below
   use it: several OTHER candidate routes try this param's position first and
   each fails at a LATER segment, and the tests show that the captured param
   reads back correctly for the route that matches. */
static void _cache_param_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  const char *id = chttpsvr_req_param(req, "id");
  char buf[256];
  int n =
      snprintf(buf, sizeof(buf), "%s:%s", (const char *)ctx, id ? id : "(nil)");
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

static void _echo_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  size_t n = 0;
  const char **vals = chttpsvr_req_query(req, "q", &n);
  if (!vals || n == 0) {
    chttpsvr_resp_write_str(resp, "no_q");
    return;
  }
  /* Join the values with a comma. */
  char buf[512];
  size_t pos = 0;
  for (size_t i = 0; i < n && pos < sizeof(buf) - 1; i++) {
    if (i > 0 && pos < sizeof(buf) - 1) buf[pos++] = ',';
    size_t vl = strlen(vals[i]);
    if (pos + vl >= sizeof(buf)) break;
    memcpy(buf + pos, vals[i], vl);
    pos += vl;
  }
  buf[pos] = '\0';
  chttpsvr_resp_write_str(resp, buf);
}

static void _echo_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "x-test-header");
  chttpsvr_resp_write_str(resp, val ? val : "(none)");
}

static void _set_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-custom", "my-value");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _status_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_CREATED);
  chttpsvr_resp_write_str(resp, "created");
}

static void _json_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  const char *body = "{\"ok\":true}";
  chttpsvr_resp_write_json(resp, body, strlen(body));
}

/* Sub-router handlers. */
static void _api_items_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *id = chttpsvr_req_param(req, "id");
  char buf[64];
  snprintf(buf, sizeof(buf), "item:%s", id ? id : "nil");
  chttpsvr_resp_write_str(resp, buf);
}

static void _api_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "api_root");
}

/* Counts middleware calls. _Atomic so that there is no data race between the
   reactor threads, which increment, and the test thread, which reads. */
static _Atomic int g_global_mw_count = 0;
static _Atomic int g_router_mw_count = 0;

static void _global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
  (void)ctx;
  g_global_mw_count++;
  /* Add a response header to show that the middleware ran. */
  chttpsvr_resp_set_header(resp, "x-global-mw", "1");
  next(req, resp);
}

static void _router_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
  (void)ctx;
  g_router_mw_count++;
  chttpsvr_resp_set_header(resp, "x-router-mw", "1");
  next(req, resp);
}

/* Middleware that short-circuits, it doesn't call next. */
static void _blocking_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                         chttpsvr_next_fn next) {
  (void)req;
  (void)ctx;
  (void)next;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_FORBIDDEN);
  chttpsvr_resp_write_str(resp, "blocked");
}

/* Two middlewares registered at the same router level, to check that both run
   in registration order and both call next correctly. */
static _Atomic int g_mw_a_count = 0;
static _Atomic int g_mw_b_count = 0;

static void _mw_a(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                  chttpsvr_next_fn next) {
  (void)ctx;
  g_mw_a_count++;
  chttpsvr_resp_set_header(resp, "x-mw-a", "1");
  next(req, resp);
}

static void _mw_b(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                  chttpsvr_next_fn next) {
  (void)ctx;
  g_mw_b_count++;
  chttpsvr_resp_set_header(resp, "x-mw-b", "1");
  next(req, resp);
}

/* Streaming handler. */
static void _stream_echo_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  char buf[256];
  ssize_t n;
  while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) {
    chttpsvr_resp_write(resp, buf, (size_t)n);
  }
}

/* Streaming handler that counts the separate chttpsvr_req_read() calls that
   returned data before EOF and reports the count in a response header. It shows
   that the server delivers the body in separate batches as the bytes arrive on
   the wire, and doesn't buffer the whole body before the handler to hand it
   over as one blob. */
static void _stream_batch_count_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  char buf[8];
  ssize_t n;
  int batches = 0;
  while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) batches++;
  char cbuf[16];
  snprintf(cbuf, sizeof(cbuf), "%d", batches);
  chttpsvr_resp_set_header(resp, "x-batch-count", cbuf);
  chttpsvr_resp_write_str(resp, "ok");
}

/* Streaming handler that drains the body and reports in a response header why
   the final chttpsvr_req_read() call returned -1, if it did. This tests
   stream_read_timeout_us and the report of an abort in the middle of the
   stream. */
static void _stream_error_report_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                         void *ctx) {
  (void)ctx;
  char buf[64];
  ssize_t n;
  while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) {
  }
  const char *err_str =
      (n < 0) ? ccol_retval_to_str(chttpsvr_req_stream_error(req)) : "none";
  chttpsvr_resp_set_header(resp, "x-stream-err", err_str);
  chttpsvr_resp_write_str(resp, "done");
}

/* Buffered handler that reports what chttpsvr_req_header says for one name that
   the client sends only as a trailer field of a chunked body (RFC 7230 SS4.1.2)
   and never as a regular header. It pins the documented contract of
   chttpsvr_req_header: a trailer field isn't a request header and reads as
   absent. A buffered handler runs only after the server has read the whole body
   with its trailers, so this is the strongest form of the check: the parser has
   read the trailer before the first statement here runs, and the trailer is
   still not readable. */
static void _buffered_trailer_echo_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-value", val ? val : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

/* Streaming handler that reports what chttpsvr_req_header says for "X-Trailer",
   again a trailer field of a chunked body and not a regular header. It asks
   BEFORE it drains the body to EOF and again AFTER, and both answers have to be
   "absent". A streaming route drives the parse of the trailer itself through
   its own chttpsvr_req_read calls, so this check covers both sides of the point
   where the parser reads the trailer. */
static void _stream_trailer_visibility_handler(chttpsvr_req *req,
                                               chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *before = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-before",
                           before ? before : "(absent)");
  char buf[64];
  while (chttpsvr_req_read(req, buf, sizeof(buf)) > 0) {
  }
  const char *after = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-after", after ? after : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

/* Buffered handler that echoes what chttpsvr_req_header says for two names that
   a reverse proxy or an authentication gateway sets on behalf of the client. It
   pins that the trailer fields of a chunked body can't push them aside: the
   client sends both names twice, once as a real header and once as a trailer
   with a different value, and the handler reports the header's value. */
static void _identity_header_echo_handler(chttpsvr_req *req,
                                          chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *xff = chttpsvr_req_header(req, "X-Forwarded-For");
  const char *who = chttpsvr_req_header(req, "X-Authenticated-User");
  chttpsvr_resp_set_header(resp, "x-seen-forwarded-for",
                           xff ? xff : "(absent)");
  chttpsvr_resp_set_header(resp, "x-seen-user", who ? who : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

/* Writes a response body of several megabytes, much bigger than any socket send
   buffer, so sending it to a client that never reads should block
   chttp1_stream_write halfway. This exercises max_response_write_duration_us,
   the write-side counterpart of max_body_read_duration_us. */
/* 16 MiB, much more than the server's send buffer, which the kernel autotunes
   for a TCP connection (the explicit 128 KiB SO_SNDBUF floor of
   _apply_accepted_socket_options applies only to a unix socket), plus the test
   client's default receive buffer and TCP window, which the OS tunes
   automatically. The client in
   max_response_write_duration_exceeded_closes_connection below never reads at
   all, so the kernel's receive-buffer autotuning has no read pattern to grow
   the window from and the real ceiling stays close to the small default.
   Sending this body to such a client therefore must block chttp1_stream_write
   on backpressure long before the whole body is in flight, instead of looking
   instant because the body happened to fit in some generous end-to-end pipeline
   of buffers. */
#define _CHTTPSVR_TEST_LARGE_BODY_SIZE (16 * 1024 * 1024)

static void _large_response_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  /* A plain static array in BSS instead of a heap allocation. A single malloc()
     that nothing frees for the rest of the process's life shows up as a real
     "still reachable" block, which make memtest reports with
     --errors-for-leak-kinds=all, while a BSS array never shows up there. */
  static char big_buf[_CHTTPSVR_TEST_LARGE_BODY_SIZE];
  static bool filled = false;
  if (!filled) {
    memset(big_buf, 'x', sizeof(big_buf));
    filled = true;
  }
  chttpsvr_resp_write(resp, big_buf, sizeof(big_buf));
}

/* Streaming handler that never calls chttpsvr_req_read() and always rejects the
   request as soon as it runs. That's the case Expect: 100-continue (RFC 7231
   SS5.1.1) exists for, a server that rejects a request and never wants its
   body. */
static void _stream_reject_without_reading_handler(chttpsvr_req *req,
                                                   chttpsvr_resp *resp,
                                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "no thanks");
}

/* Streaming handler that reads exactly one small batch of the body and then
   returns without draining the rest. It shows that a connection is safe for the
   next request when the handler leaves part of the body unread, because
   chttp1_stream_release throws the unread remainder away instead of
   keeping it. */
static void _stream_read_once_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[8];
  chttpsvr_req_read(req, buf, sizeof(buf));
  chttpsvr_resp_write_str(resp, "early-stop");
}

/* Echoes the raw query. */
static void _raw_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *rq = chttpsvr_req_raw_query(req);
  chttpsvr_resp_write_str(resp, rq ? rq : "(none)");
}

/* Echoes path + method, for quick checks. */
static void _path_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, chttpsvr_req_path(req));
}

/* Handler that calls chttpsvr_req_query_one directly. */
static void _query_one_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *val = NULL;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "q", &val);
  if (rv == ccol_success) {
    chttpsvr_resp_write_str(resp, val ? val : "(null)");
  } else if (rv == ccol_key_not_found) {
    chttpsvr_resp_write_str(resp, "not_found");
  } else if (rv == ccol_key_already_present) {
    chttpsvr_resp_write_str(resp, "multi_value");
  } else {
    chttpsvr_resp_write_str(resp, "error");
  }
}

/* Streaming handler that echoes a request header. It tests the pre-extracted
   header array path that chttpsvr_req_header uses on streaming routes. */
static void _stream_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "x-stream-test");
  chttpsvr_resp_write_str(resp, val ? val : "(none)");
}

/* Handler that calls chttpsvr_resp_write_str three times, to check that several
   writes add up into one response body. */
static void _multi_write_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "foo");
  chttpsvr_resp_write_str(resp, "bar");
  chttpsvr_resp_write_str(resp, "baz");
}

/* Handler that exercises chttpsvr_resp_printf. First a short call with a mix of
   scalar types that fits the internal stack buffer, then a long call whose
   formatted output is bigger than that stack buffer, so the function has to
   take the heap fallback path. Both calls have to add to the same body, the
   same way chttpsvr_resp_write_str does. */
static void _printf_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_printf(resp, "n=%d s=%s f=%.2f ", 42, "hi", 3.5);
  for (int i = 0; i < 40; i++) {
    chttpsvr_resp_printf(resp, "%08d-", i);
  }
}

/* Handler that queries two different keys in one request, to check that the
   _qresult scratch array is reused correctly across calls. */
static void _multi_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  size_t na = 0, nb = 0;
  const char **va = chttpsvr_req_query(req, "a", &na);
  /* Take the string value before the next query call recycles _qresult. */
  const char *a_val = (va && na > 0) ? va[0] : "nil";
  const char **vb = chttpsvr_req_query(req, "b", &nb);
  const char *b_val = (vb && nb > 0) ? vb[0] : "nil";
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "a=%s b=%s", a_val, b_val);
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

/* Handler that calls chttpsvr_req_read() on a buffered (non-streaming) route,
   where chttpsvr_req_read() must return -1 because is_streaming is false. */
static void _read_on_buffered_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that reads the body with chttpsvr_req_body() instead of
   chttpsvr_req_read(). A streaming route reads its body live off the socket
   with chttpsvr_req_read(), and the documentation of chttpsvr_req_body() says
   it works only for a buffered route, so here it needs to return NULL and 0 and
   not some body the handler never asked the server to read. */
static void _stream_body_check_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx) {
  (void)ctx;
  size_t len = 0;
  const void *body = chttpsvr_req_body(req, &len);
  if (body && len > 0) {
    chttpsvr_resp_write(resp, body, len);
  } else {
    chttpsvr_resp_write_str(resp, "(empty)");
  }
}

/* Streaming handler that calls chttpsvr_req_read() FIRST, which drains part of
   the body off the socket and fills conn->body's internal cursor and growbuf
   state, and only then calls chttpsvr_req_body(). That covers the after-a-read
   case, which _stream_body_check_handler above can't reach because it never
   calls chttpsvr_req_read() and so conn->body is still empty when it checks.
   chttpsvr_req_body() must report NULL and 0 here too, and not the internal
   accounting state left in conn->body. Without the is_streaming guard that
   state holds bytes the server has already delivered and a length that reflects
   internal bookkeeping, which is neither the real total nor the real number of
   bytes still unread. */
static void _stream_body_after_read_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[8];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  size_t len = (size_t)-1; /* poisoned, chttpsvr_req_body must overwrite it */
  const void *body = chttpsvr_req_body(req, &len);
  char s[64];
  snprintf(s, sizeof(s), "read=%ld body=%s len=%zu", (long)n,
           body ? "non-null" : "null", len);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that calls chttpsvr_req_read with buflen == 0. A read of
   zero length must return 0 (a no-op) and not -1 (an error). */
static void _stream_zero_buflen_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  char buf[4];
  ssize_t n = chttpsvr_req_read(req, buf, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler on a route with no request body. The first call to
   chttpsvr_req_read has to return 0 (EOF) immediately. */
static void _stream_read_empty_body_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler that sets the same response header twice, and the second value has to
   win.
 */
static void _dup_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-dup", "first");
  chttpsvr_resp_set_header(resp, "x-dup", "second");
  chttpsvr_resp_write_str(resp, "ok");
}

/* Handler that sets a status code outside the valid range. The server has to
   turn it into 500 (see the [100,999] range check in _send_response) instead of
   writing the raw out-of-range value onto the wire. */
static void _bad_status_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 0);
  chttpsvr_resp_write_str(resp, "bad");
}

static void _query_one_null_key_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  const char *val = NULL;
  /* A NULL key should return ccol_invalid_args and not crash. */
  ccol_retval_t rv = chttpsvr_req_query_one(req, NULL, &val);
  char s[16];
  snprintf(s, sizeof(s), "%d", (int)rv);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler that calls chttpsvr_req_query_one with a NULL val_out for a key that
   is present. The function must still return ccol_success (it has a value to
   report, the caller just chose not to take it) and must not crash. */
static void _query_one_null_val_out_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "q", NULL);
  chttpsvr_resp_write_str(resp, rv == ccol_success ? "ok" : "fail");
}

/* Streaming handler that calls chttpsvr_req_read with a NULL buffer. The NULL
   buffer guard should fire before the is_streaming check and return -1.
 */
static void _stream_null_buf_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 4);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that calls chttpsvr_req_read(req, NULL, 0). When buf is
   NULL and buflen is 0 at the same time, the buflen==0 guard must win and
   return 0, not the -1 the NULL buf guard would give. */
static void _stream_null_buf_zero_len_handler(chttpsvr_req *req,
                                              chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler for the headers-only response test. It sets a status code and a
   response header but writes NO body bytes, so we can check that _send_response
   writes the head alone, with no body, when the body buffer is empty. */
static void _headers_only_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_NO_CONTENT);
  chttpsvr_resp_set_header(resp, "x-no-body", "1");
  /* On purpose there's no chttpsvr_resp_write* call. */
}

/* Always writes a non-empty body, then overwrites the status code with the
   number named by the "x-force-status" request header (200 when the header is
   missing or can't be parsed). We check that _send_response drops the body for
   a status code that must never carry one, and for a 204 also the automatic
   Content-Length, whatever the handler itself wrote. When the request has
   "x-force-cl", the handler also sets that value as its own Content-Length. */
static void _forced_status_with_body_handler(chttpsvr_req *req,
                                             chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, "this-body-must-never-reach-the-wire");
  const char *forced = chttpsvr_req_header(req, "x-force-status");
  chttpsvr_resp_set_status(resp, forced ? atoi(forced) : 200);
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
}

/* A route registered for HEAD itself, which writes no body. When the request
   has "x-force-cl" it sets that value as the representation's Content-Length.
 */
static void _bodyless_head_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
  chttpsvr_resp_set_header(resp, "x-explicit-head", "1");
}

/* Tries a list of Content-Length values through both header calls and answers
   with one character for each, 'y' for ccol_success and 'n' for
   ccol_invalid_args. Whatever value the last accepted call left behind gets
   dropped again, because the handler writes a body. */
static void _content_length_validation_handler(chttpsvr_req *req,
                                               chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  static const char *const values[] = {"0",
                                       "1234",
                                       "007",
                                       "9223372036854775807",
                                       "",
                                       "-1",
                                       "12a",
                                       " 12",
                                       "12 ",
                                       "+1",
                                       "0x1",
                                       "9223372036854775808",
                                       "00000000000000000000001"};
  char out[2 * (sizeof(values) / sizeof(values[0])) + 1];
  size_t n = 0;
  for (size_t i = 0; i < sizeof(values) / sizeof(values[0]); i++) {
    out[n++] = chttpsvr_resp_set_header(resp, "Content-Length", values[i]) ==
                       ccol_success
                   ? 'y'
                   : 'n';
    out[n++] = chttpsvr_resp_add_header(resp, "content-length", values[i]) ==
                       ccol_success
                   ? 'y'
                   : 'n';
  }
  out[n] = '\0';
  chttpsvr_resp_write_str(resp, out);
}

/* Builds a head with repeated names through chttpsvr_resp_add_header and
   chttpsvr_resp_set_header (see resp_add_header_keeps_every_field_in_order).
   The handler reports every return value that isn't ccol_success in the body.
 */
static void _add_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  int bad = 0;
  bad += chttpsvr_resp_add_header(resp, "Set-Cookie", "a=1") != ccol_success;
  bad += chttpsvr_resp_set_header(resp, "x-single", "one") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "set-cookie", "b=2") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "x-single", "two") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "X-SINGLE", "three") != ccol_success;
  /* set_header collapses every x-single into the first one. */
  bad += chttpsvr_resp_set_header(resp, "x-single", "final") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "Set-Cookie", "c=3") != ccol_success;
  /* Date is a single field whichever call sets it. */
  bad += chttpsvr_resp_add_header(resp, "Date", "d1") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "date", "d2") != ccol_success;
  /* Rejected exactly like set_header rejects them, with nothing changed. */
  bad += chttpsvr_resp_add_header(resp, "Transfer-Encoding", "chunked") !=
         ccol_invalid_args;
  bad += chttpsvr_resp_add_header(resp, "x-bad", "a\r\nx-injected: 1") !=
         ccol_invalid_args;
  bad += chttpsvr_resp_add_header(resp, "x bad", "v") != ccol_invalid_args;
  bad += chttpsvr_resp_add_header(resp, "", "v") != ccol_invalid_args;
  bad += chttpsvr_resp_add_header(NULL, "x", "v") != ccol_invalid_args;
  bad += chttpsvr_resp_add_header(resp, NULL, "v") != ccol_invalid_args;
  bad += chttpsvr_resp_add_header(resp, "x", NULL) != ccol_invalid_args;
  chttpsvr_resp_printf(resp, "bad=%d", bad);
}

/* Sets an explicit "Connection" response header to the value the caller passes
   in the "x-force-connection" request header, a value that may have nothing to
   do with what the server's framing logic decides later. We check that a
   handler can't make the Connection header on the wire disagree with what the
   server really does after the response (keep-alive or close); see the doc
   comment of _send_response on this. */
static void _explicit_connection_header_handler(chttpsvr_req *req,
                                                chttpsvr_resp *resp,
                                                void *ctx) {
  (void)ctx;
  const char *forced = chttpsvr_req_header(req, "x-force-connection");
  if (forced) chttpsvr_resp_set_header(resp, "Connection", forced);
  chttpsvr_resp_write_str(resp, "explicit-connection-header-ok");
}

/* Sets enough response headers, each with a large value, to make the combined
   header block bigger than a few KiB. _send_response has to build the header
   block in a heap buffer that doubles when it needs to grow. A fixed 4096-byte
   stack buffer with no fallback would lose the ENTIRE response as soon as the
   headers alone go past that size, without even a graceful 500: the server
   closes the connection, writes zero bytes and logs nothing. This call has to
   succeed with every header intact, whatever the size. */
static void _large_response_headers_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  char value[300];
  memset(value, 'x', sizeof(value) - 1);
  value[sizeof(value) - 1] = '\0';
  for (int i = 0; i < 20; i++) {
    char name[32];
    snprintf(name, sizeof(name), "x-custom-%02d", i);
    chttpsvr_resp_set_header(resp, name, value);
  }
  chttpsvr_resp_write_str(resp, "large-headers-ok");
}

/* Handler for the SECOND registration of the same path and method, which checks
   that the server accepts a duplicate registration without complaining. Only
   the FIRST handler ever runs, because the first one wins. */
static void _dup_second_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "second");
}

/* Handler that calls chttpsvr_req_header(req, NULL) and writes "null" if the
   result is NULL and "non-null" otherwise. It exercises the !name guard through
   an HTTP round trip, since chttpsvr_req is opaque in the public API. */
static void _null_name_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_header(req, NULL);
  chttpsvr_resp_write_str(resp, v == NULL ? "null" : "non-null");
}

/* Handler that calls chttpsvr_req_query(req, NULL, &n) and writes "null" if the
   result is NULL and "non-null" otherwise. */
static void _null_key_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)ctx;
  size_t n = 99;
  const char **v = chttpsvr_req_query(req, NULL, &n);
  chttpsvr_resp_write_str(resp, (v == NULL && n == 0) ? "null" : "non-null");
}

/* A second server for the "already running" test (it never really serves). */
static chttpsvr g_srv2 = CHTTPSVR_INVALID;

/* ========================================================================== */
/*       SHARED RESULT ARRAYS FOR HANDLERS REGISTERED IN _setup              */
/* ========================================================================== */

/* Owned by routes registered in _setup, and the tests reset them before each
 * request. They're _Atomic so there's no data race between the handler thread
 * (the writer) and the test thread (the reader). The HTTP round trip gives the
 * happens-before ordering we need, but _Atomic int removes the formal UB that a
 * plain int would carry under the C memory model. */
static _Atomic int g_null_guard_results[2]; /* resp_write_str_null_guard */
static _Atomic int g_null_data_results[2]; /* resp_write_null_data_edge_cases */
static _Atomic int
    g_null_hdr_results[2]; /* resp_set_header_null_name_value_live */
static _Atomic int
    g_crlf_hdr_results[3]; /* resp_set_header_crlf_injection_rejected */
static _Atomic int
    g_bare_crlf_hdr_results[3]; /* resp_set_header_bare_cr_and_lf_rejected */
static _Atomic int
    g_disallowed_hdr_results[4]; /* resp_set_header_disallowed_names_rejected
                                  */
static _Atomic int
    g_non_tchar_hdr_results[4]; /* resp_set_header_non_tchar_name_rejected */
static _Atomic int
    g_json_zero_len_result; /* resp_write_json_zero_len_rejected */

/* ========================================================================== */
/*       HANDLER FUNCTIONS FOR ROUTES REGISTERED IN _setup                   */
/* ========================================================================== */

static void _null_guard_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_write_str(NULL, "hi");
  results[1] = (int)chttpsvr_resp_write_str(resp, NULL);
  chttpsvr_resp_write_str(resp, "ok");
}

static void _resp_write_null_guard_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_write(resp, NULL, 5);
  results[1] = (int)chttpsvr_resp_write(resp, NULL, 0);
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_null_guards_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, NULL, "v");
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-null-v", NULL);
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_crlf_guards_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* A CRLF inside the NAME, trying to inject a second header line ("x-injected:
   * evil") ahead of the real value. */
  results[0] =
      (int)chttpsvr_resp_set_header(resp, "x-evil\r\nx-injected: evil", "v");
  /* A CRLF inside the VALUE, trying to inject a fake status line for a second
   * response that the attacker controls. */
  results[1] = (int)chttpsvr_resp_set_header(
      resp, "x-evil", "v\r\n\r\nHTTP/1.1 200 OK\r\nx-injected: evil");
  /* A legitimate header must still work after the two rejections above, since
   * a rejection must not corrupt resp's header list. */
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_bare_cr_lf_guards_handler(chttpsvr_req *req,
                                                  chttpsvr_resp *resp,
                                                  void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* A lone CR without an LF after it. The value check has to catch a single
   * forbidden byte and not only the combined "\r\n" pair, which the
   * resp_set_header_crlf_injection_rejected test above already covers. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "x-bare-cr", "v\rinjected");
  /* The same guard for the other single forbidden byte, a lone LF without a CR.
   */
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-bare-lf", "v\ninjected");
  /* A legitimate header must still work after both rejections above, since a
   * rejection must not corrupt resp's header list. */
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_disallowed_name_guards_handler(chttpsvr_req *req,
                                                       chttpsvr_resp *resp,
                                                       void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* An empty name has no valid representation on the wire. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "", "v");
  /* "Transfer-Encoding" has to be rejected outright, in lower case ... */
  results[1] =
      (int)chttpsvr_resp_set_header(resp, "transfer-encoding", "chunked");
  /* ... and in mixed case, since the check ignores case. */
  results[2] =
      (int)chttpsvr_resp_set_header(resp, "Transfer-Encoding", "chunked");
  /* A legitimate header still works after all three rejections above,
   * since a rejection must not corrupt resp's header list. */
  results[3] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_non_tchar_name_guards_handler(chttpsvr_req *req,
                                                      chttpsvr_resp *resp,
                                                      void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* A space inside the name. The name has no CR and no LF, so a check for CR
   * and LF alone would let it through, but it isn't a real RFC 7230 token of
   * tchar bytes. The name "X Foo: bar" would put "X Foo:bar:baz\r\n" on the
   * wire, which isn't a real split into two fields. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "X Foo", "bar");
  /* A literal colon inside the name, for the same reason. */
  results[1] = (int)chttpsvr_resp_set_header(resp, "X:Foo", "bar");
  /* A byte that is not a CR or an LF but is still outside the printable tchar
   * set. A bare NUL can't happen because the name is a NUL-terminated C string,
   * so this case uses a DEL byte (0x7F), which is outside the tchar set as
   * well. */
  results[2] = (int)chttpsvr_resp_set_header(resp,
                                             "X\x7F"
                                             "Foo",
                                             "bar");
  /* A legitimate header (including one with every non-alnum tchar byte that RFC
   * 7230 SS3.2.6 allows) should still work after all three rejections above. */
  results[3] =
      (int)chttpsvr_resp_set_header(resp, "x-legit!#$%&'*+-.^_`|~", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _json_zero_len_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  _Atomic int *result = (_Atomic int *)ctx;
  *result = (int)chttpsvr_resp_write_json(resp, "{}", 0);
  chttpsvr_resp_write_str(resp, "done");
}

static void _root_subrouter_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root_subrouter");
}

static void _empty_query_key_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  size_t n = 0;
  const char **vals = chttpsvr_req_query(req, "", &n);
  if (vals && n > 0)
    chttpsvr_resp_write_str(resp, vals[0]);
  else
    chttpsvr_resp_write_str(resp, "not_found");
}

/* Reports the value counts for the keys "a", "b" and "", in that order and
   separated by commas. It checks that the parser skips a stray '&' in the raw
   query string, and two '&' characters in a row, instead of reading them as a
   bogus entry with an empty key and an empty value. */
static void _query_stray_amp_counts_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t na = 0, nb = 0, nempty = 0;
  chttpsvr_req_query(req, "a", &na);
  chttpsvr_req_query(req, "b", &nb);
  chttpsvr_req_query(req, "", &nempty);
  chttpsvr_resp_printf(resp, "%zu,%zu,%zu", na, nb, nempty);
}

static void _stream_read_eof_twice_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n1 = chttpsvr_req_read(req, buf, sizeof(buf));
  ssize_t n2 = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[64];
  snprintf(s, sizeof(s), "%ld %ld", (long)n1, (long)n2);
  chttpsvr_resp_write_str(resp, s);
}

static void _param_null_name_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_param(req, NULL);
  chttpsvr_resp_write_str(resp, v == NULL ? "null" : "non-null");
}

/* Streaming GET handler that reads the body with chttpsvr_req_body(). For a GET
   without a body the function must return NULL and len=0. */
static void _stream_get_body_check_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t len = 0;
  const void *body = chttpsvr_req_body(req, &len);
  if (body && len > 0)
    chttpsvr_resp_write(resp, body, len);
  else
    chttpsvr_resp_write_str(resp, "(empty)");
}

/* A pair of handlers for the root-router shadowing test, both registered on
   /shadow-test/ping. The root handler is registered directly on g_srv and the
   server always checks it first, while the sub-router handler sits on a
   /shadow-test sub-router that the server checks second and never gets to. */
/* Marks its response so that a test can tell which of the two routes registered
   for /head-pref (a CHTTP_GET one and a CHTTP_HEAD one) really ran. */
static void _explicit_head_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-head-route", "1");
  chttpsvr_resp_write_str(resp, "head-route");
}

static void _shadow_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root-wins");
}
static void _shadow_sub_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "sub-wins");
}

/* CHTTP_ANY handlers. */

/* Echoes the request's method name, so tests can check which method was
   dispatched when a CHTTP_ANY route matches. */
static void _any_method_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, chttp_method_str(chttpsvr_req_method(req)));
}

/* Echoes "<METHOD>:<id-param>" for CHTTP_ANY routes with a path parameter. */
static void _any_method_param_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *id = chttpsvr_req_param(req, "id");
  char buf[64];
  snprintf(buf, sizeof(buf), "%s:%s",
           chttp_method_str(chttpsvr_req_method(req)), id ? id : "nil");
  chttpsvr_resp_write_str(resp, buf);
}

/* ========================================================================== */
/*                         BOUNDED SERVER 503 TEST STATE                      */
/* ========================================================================== */

/* A dedicated server started by _setup, with a bounded pool of 1 thread and
   queue_capacity=1. It checks that a full ctpool makes ctpool_try_submit return
   ccol_container_full and that the server then answers with a 503. It listens
   on TEST_PORT+2. */
static chttpsvr g_bounded_srv = CHTTPSVR_INVALID;

/* A dedicated server with a small max_body_size (64 bytes) for the boundary
   tests of the 413/PAYLOAD_TOO_LARGE enforcement, buffered and streaming. It
   listens on TEST_PORT+3. */
static chttpsvr g_small_body_srv = CHTTPSVR_INVALID;
#define SMALL_BODY_MAX 64

/* Mutex and condition variable that synchronize the blocking handler of the 503
   test. The test sends two HTTP requests at the same time, both block inside
   _bounded_blk_handler and fill the pool of 1 thread and the queue of 1 slot,
   and then it sends a third request, which gets a 503. */
static pthread_mutex_t g_blk_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_blk_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_blk_count = 0; /* handlers that are blocking right now */
static bool g_blk_go = false;

static void _bounded_blk_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  g_blk_count++;
  pthread_mutex_lock(&g_blk_mtx);
  while (!g_blk_go) pthread_cond_wait(&g_blk_cv, &g_blk_mtx);
  pthread_mutex_unlock(&g_blk_mtx);
  g_blk_count--;
  chttpsvr_resp_write_str(resp, "ok");
}

/* The client of bounded_pool_full_returns_503. Its own pool has room for every
 * request of the test. The default client allows one connection per CPU, so on
 * a machine with two CPUs the two requests blocked in the handler would hold
 * every slot and the third one would wait forever. */
static chttpcli g_bounded_cli = CHTTPCLI_INVALID;

/* Sends one GET /bounded-503 to g_bounded_srv through g_bounded_cli. */
static chttpcli_response *_bounded_get(void) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bounded-503", TEST_PORT + 2);
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  if (req) chttpclient_do(g_bounded_cli, req, &resp);
  chttp_request_free(req);
  return resp;
}

/* Thread entry: sends one GET /bounded-503 to g_bounded_srv and throws the
   response away. We use it to fill pool slots from background threads. */
static void *_send_bounded_req(void *arg) {
  (void)arg;
  chttpcli_response *resp = _bounded_get();
  if (resp) chttpclient_resp_free(resp);
  return NULL;
}

/* Thread entry: sends one GET to a route that doesn't match on g_srv (a sure
   404) and throws the response away. We use it to fire a concurrent burst of
   route rejections; see
   concurrent_route_rejections_do_not_starve_other_requests. */
static void *_send_unmatched_route_req(void *arg) {
  (void)arg;
  chttpcli_response *resp = NULL;
  chttp_get(BASE_URL "/definitely-not-a-registered-route-xyz", &resp);
  if (resp) chttpclient_resp_free(resp);
  return NULL;
}

/* ========================================================================== */
/*                         SERVER SETUP / TEARDOWN                            */
/* ========================================================================== */

static void _teardown(void) {
  /* Unblock any handler still blocked inside _bounded_blk_handler, so that
   * g_bounded_srv can drain its in-flight requests cleanly. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  /* Stop every listener first, so that no new requests are accepted. */
  if (g_bounded_srv) chttpsvr_stop(g_bounded_srv);
  if (g_small_body_srv) chttpsvr_stop(g_small_body_srv);
  if (g_srv) chttpsvr_stop(g_srv);
  if (g_srv2) chttpsvr_stop(g_srv2);

  /* Destroy each server. Each destroy drains the in-flight requests and frees
   * that server's single shared-engine reference. chttpserver's shared
   * ccol_event_loop reactor stops asynchronously, on a joinable reaper thread
   * that has nothing to do with chttpclient's engine, and the last destroy
   * doesn't join that thread inline. This function is an atexit handler, so
   * chttpsvr_engine_wait() below has to block until the reactor has really
   * stopped; without that wait a reactor thread could still be using the logger
   * that _setup installed in the engine with chttpsvr_set_engine_logger
   * (g_test_logger) when this function closes it just below. */
  if (g_bounded_srv) {
    __chttpsvr_destroy(g_bounded_srv);
    g_bounded_srv = CHTTPSVR_INVALID;
  }
  if (g_small_body_srv) {
    __chttpsvr_destroy(g_small_body_srv);
    g_small_body_srv = CHTTPSVR_INVALID;
  }
  if (g_srv) {
    __chttpsvr_destroy(g_srv);
    g_srv = CHTTPSVR_INVALID;
  }
  if (g_srv2) {
    __chttpsvr_destroy(g_srv2);
    g_srv2 = CHTTPSVR_INVALID;
  }
  /* On OpenBSD an exit handler registered by a constructor runs after the
   * dynamic loader has taken its lock for the destructors of every object, and
   * the loader keeps that lock until the process ends. pthread_join() on
   * another thread needs that lock there, so waiting for the engine's reaper,
   * which joins threads, would never return. There the reactor may still be
   * running when the process ends, so the logger stays open too. The wait and
   * the close are only there so that nothing is left running or allocated for
   * valgrind, which doesn't run on OpenBSD anyway. */
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_INVALID;
  }
#endif
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  /* Several tests hold both ends of hundreds of connections in this one process
   * (one_tick_closes_every_expired_idle_and_parked_connection holds about 620
   * descriptors), which is more than the soft limit of some systems (128 for
   * root and 512 for a user on OpenBSD). So we raise the soft limit as far as
   * the hard limit allows, up to 4096. */
  struct rlimit nofile;
  if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 && nofile.rlim_cur < 4096) {
    nofile.rlim_cur = nofile.rlim_max == RLIM_INFINITY || nofile.rlim_max > 4096
                          ? 4096
                          : nofile.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &nofile);
  }

  /* The logger for the engine and both server instances. */
  g_test_logger = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  /* Create and configure the test server. */
  g_srv = chttpsvr_create(g_test_logger, &err);
  if (!g_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n", err ? err : "?");
    exit(1);
  }

  /* Global middleware, active for ALL routes. */
  chttpsvr_use(g_srv, _global_mw, NULL);

  /* Root-level routes. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/echo-body", _echo_body_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/params/{id}/{sub}",
                            _echo_param_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query", _echo_query_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/header", _echo_header_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/set-header",
                            _set_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/status", _status_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/status-with-body",
                            _forced_status_with_body_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_HEAD, "/head-explicit",
                            _bodyless_head_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/content-length-validation",
                            _content_length_validation_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/add-header",
                            _add_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/explicit-connection-header",
                            _explicit_connection_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/large-response-headers",
                            _large_response_headers_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/json", _json_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/raw-query", _raw_query_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/headers-only",
                            _headers_only_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/path", _path_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/blocked", _hello_handler,
                            NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one", _query_one_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/multi-write",
                            _multi_write_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/printf", _printf_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/multi-query",
                            _multi_query_handler, NULL);
  /* /echo-path/{v} reuses _path_handler to test the URL decoding of req->path.
   */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/echo-path/{v}", _path_handler,
                            NULL);

  /* A buffered route that calls chttpsvr_req_read(), which must return -1. */
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/req-read-buffered",
                            _read_on_buffered_handler, NULL);

  /* Routes with more than one method: the same path is registered for both GET
     and POST, to check that the server routes each of them on its own.
     _find_route keeps searching past a path match whose method doesn't
     match, because stopping at the first path match would give a POST a 405
     just because the GET route matched first. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dual", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/dual", _echo_body_handler,
                            NULL);

  /* Routes for extra coverage tests. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-header",
                            _dup_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/bad-status",
                            _bad_status_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-key",
                            _query_one_null_key_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-val-out",
                            _query_one_null_val_out_handler, NULL);

  /* A duplicate route, the same method+path registered twice. The first handler
     wins. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
                            _dup_second_handler, NULL);

  /* Routes for the null-guard HTTP round-trip tests. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-name-header",
                            _null_name_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-key-query",
                            _null_key_query_handler, NULL);

  /* Streaming routes. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-echo",
                                      _stream_echo_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-header",
                                      _stream_header_handler, NULL);
  /* A streaming route that reads the body with chttpsvr_req_body() instead of
   * req_read. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-body-api",
                                      _stream_body_check_handler, NULL);
  /* A streaming route that calls chttpsvr_req_body() AFTER chttpsvr_req_read()
   * has already been used on it; see the doc comment of
   * _stream_body_after_read_handler. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST,
                                      "/stream-body-after-read",
                                      _stream_body_after_read_handler, NULL);
  /* A streaming GET route that calls chttpsvr_req_body() on a request without a
   * body, which takes the else branch of (body && len > 0). It must return
   * "(empty)" and not crash or give undefined output. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET,
                                      "/stream-body-get-no-body",
                                      _stream_get_body_check_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-batch-count",
                                      _stream_batch_count_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-error-report",
                                      _stream_error_report_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/buffered-trailer-echo",
                            _buffered_trailer_echo_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST,
                                      "/stream-trailer-visibility",
                                      _stream_trailer_visibility_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/identity-header-echo",
                            _identity_header_echo_handler, NULL);
  /* Streaming routes for the chttpsvr_req_read edge cases. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-zero-buflen",
                                      _stream_zero_buflen_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-empty-read",
                                      _stream_read_empty_body_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-null-buf",
                                      _stream_null_buf_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET,
                                      "/stream-null-buf-zero-len",
                                      _stream_null_buf_zero_len_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-read-once",
                                      _stream_read_once_handler, NULL);

  /* A sub-router for /api/v1 with its own middleware. */
  chttpsvr_router *api = chttpsvr_subrouter(g_srv, "/api/v1");
  if (!api) {
    fprintf(stderr, "FATAL: could not create subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(api, _router_mw, NULL);
  chttpsvr_router_on(api, CHTTP_GET, "/", _api_root_handler, NULL);
  chttpsvr_router_on(api, CHTTP_GET, "/items/{id}", _api_items_handler, NULL);
  chttpsvr_router_on_stream(api, CHTTP_POST, "/stream-echo",
                            _stream_echo_handler, NULL);

  /* A sub-router with blocking middleware. */
  chttpsvr_router *blocked_r = chttpsvr_subrouter(g_srv, "/blocked-area");
  if (!blocked_r) {
    fprintf(stderr, "FATAL: could not create blocked subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(blocked_r, _blocking_mw, NULL);
  chttpsvr_router_on(blocked_r, CHTTP_GET, "/secret", _hello_handler, NULL);

  /* A sub-router with two chained middlewares, to check that both run in order.
   */
  chttpsvr_router *mw_chain = chttpsvr_subrouter(g_srv, "/mw-chain");
  if (!mw_chain) {
    fprintf(stderr, "FATAL: could not create mw-chain subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(mw_chain, _mw_a, NULL);
  chttpsvr_router_use(mw_chain, _mw_b, NULL);
  chttpsvr_router_on(mw_chain, CHTTP_GET, "/ping", _hello_handler, NULL);

  /* A sub-router registered with a trailing-slash prefix. The server normalises
     it to "/api/v3", so routing behaves exactly like with a clean prefix. */
  chttpsvr_router *api_v3 = chttpsvr_subrouter(g_srv, "/api/v3/");
  if (!api_v3) {
    fprintf(stderr, "FATAL: could not create /api/v3/ subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(api_v3, CHTTP_GET, "/ping", _hello_handler, NULL);

  /* Routes for tests that would otherwise register routes in the middle of the
   * test. Registering them here makes sure each route exists exactly once for
   * the life of the process, and avoids one test polluting another through
   * duplicate registrations. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-guard",
                            _null_guard_handler, g_null_guard_results);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/resp-write-null-guard",
                            _resp_write_null_guard_handler,
                            g_null_data_results);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/set-header-null-guards",
                            _set_header_null_guards_handler,
                            g_null_hdr_results);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/set-header-crlf-guards",
                            _set_header_crlf_guards_handler,
                            g_crlf_hdr_results);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/set-header-bare-crlf-guards",
                            _set_header_bare_cr_lf_guards_handler,
                            g_bare_crlf_hdr_results);
  chttpsvr_register_handler(
      g_srv, CHTTP_GET, "/set-header-disallowed-name-guards",
      _set_header_disallowed_name_guards_handler, g_disallowed_hdr_results);
  chttpsvr_register_handler(
      g_srv, CHTTP_GET, "/set-header-non-tchar-name-guards",
      _set_header_non_tchar_name_guards_handler, g_non_tchar_hdr_results);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/json-zero-len",
                            _json_zero_len_handler, &g_json_zero_len_result);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-empty-key",
                            _empty_query_key_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-stray-amp-counts",
                            _query_stray_amp_counts_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET,
                                      "/stream-read-eof-twice",
                                      _stream_read_eof_twice_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/param-null-name",
                            _param_null_name_handler, NULL);

  /* A "/" sub-router, which matches only the exact root path "/". The IMPORTANT
   * edge-case note in chttpserver.h explains the routing mechanics. */
  chttpsvr_router *root_r = chttpsvr_subrouter(g_srv, "/");
  if (!root_r) {
    fprintf(stderr, "FATAL: could not create / subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(root_r, CHTTP_GET, "/", _root_subrouter_handler, NULL);

  /* Routes for explicit_head_route_wins_over_get_route. The CHTTP_GET route is
   * registered FIRST on purpose, because the fallback where a GET serves a HEAD
   * finds it first, so a result that depended on registration order would pick
   * it instead of the explicit CHTTP_HEAD route registered next. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/head-pref", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_HEAD, "/head-pref",
                            _explicit_head_handler, NULL);

  /* Routes for the subrouter_wins_over_root_route_at_same_path test. They're
   * registered here and not in the test body, so they exist exactly once for
   * the life of the process and the test doesn't change g_srv while it runs. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/shadow-test/ping",
                            _shadow_root_handler, NULL);
  {
    chttpsvr_router *shadow_r = chttpsvr_subrouter(g_srv, "/shadow-test");
    if (!shadow_r) {
      fprintf(stderr, "FATAL: could not create /shadow-test subrouter\n");
      exit(1);
    }
    chttpsvr_router_on(shadow_r, CHTTP_GET, "/ping", _shadow_sub_handler, NULL);
  }

  /* A sub-router for the middleware-overflow-produces-500 test. g_srv has 1
   * global middleware (_global_mw), and registering 32 middlewares on this
   * router brings the combined count at dispatch time to 33 > _CHTTPSVR_MAX_MW
   * (32), so every request to /mw-overflow-live/<any> must get a 500. */
  {
    chttpsvr_router *ov_live = chttpsvr_subrouter(g_srv, "/mw-overflow-live");
    if (!ov_live) {
      fprintf(stderr, "FATAL: could not create /mw-overflow-live subrouter\n");
      exit(1);
    }
    for (int i = 0; i < 32; i++) {
      chttpsvr_router_use(ov_live, _global_mw, NULL);
    }
    chttpsvr_router_on(ov_live, CHTTP_GET, "/ping", _hello_handler, NULL);
    chttpsvr_router_on_stream(ov_live, CHTTP_POST, "/stream-ping",
                              _stream_echo_handler, NULL);
  }

  /* CHTTP_ANY routes. */

  /* Catch-all: every method is dispatched to _any_method_handler. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method",
                            _any_method_handler, NULL);

  /* Catch-all with a path parameter. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method/{id}",
                            _any_method_param_handler, NULL);

  /* A specific GET registered BEFORE CHTTP_ANY on the same pattern. GET uses
     _hello_handler, and every other method falls through to CHTTP_ANY. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-with-specific",
                            _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-with-specific",
                            _any_method_handler, NULL);

  /* CHTTP_ANY registered BEFORE a specific GET on the same pattern. The first
     one wins, so CHTTP_ANY wins for every method, GET included. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-first", _any_method_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-first", _hello_handler,
                            NULL);

  /* A second server instance (not started) for the route-registration edge-case
   * tests and the double-start rejection tests. */
  g_srv2 = chttpsvr_create(g_test_logger, NULL);

  /* Send the engine's log to g_test_logger before the first chttpsvr_start. */
  chttpsvr_set_engine_logger(g_test_logger);

  /* Start the main test server, with 4 worker threads and the default
   * (unbounded) queue, so that both streaming and buffered handlers run on the
   * server's ctpool.
   */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  cfg.worker_thread_count = 4;

  ccol_retval_t rv = chttpsvr_start(g_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }
  /* No readiness poll is needed: chttpsvr_start binds the listening socket
   * synchronously, through _make_listen_socket, before it returns. */

  /* Create and start the bounded server for the bounded_pool_full_returns_503
   * test, with 1 worker thread and queue_capacity=1, so the total capacity is 2
   * (1 running and 1 queued) and a third concurrent request gets a 503.
   */
  g_bounded_srv = chttpsvr_create(g_test_logger, NULL);
  if (!g_bounded_srv) {
    fprintf(stderr, "FATAL: could not create bounded chttpsvr\n");
    exit(1);
  }
  chttpsvr_register_streaming_handler(g_bounded_srv, CHTTP_GET, "/bounded-503",
                                      _bounded_blk_handler, NULL);
  chttpsvr_register_streaming_handler(g_bounded_srv, CHTTP_POST,
                                      "/stream-error-report-slow",
                                      _stream_error_report_handler, NULL);
  {
    chttpsvr_config_t bcfg = CHTTPSVR_CONFIG_DEFAULT;
    bcfg.host = "127.0.0.1";
    bcfg.port = TEST_PORT + 2;
    bcfg.worker_thread_count = 1;
    bcfg.worker_queue_capacity = 1;
    /* This server's routes are streaming routes and the tests that use it fill
     * the WORKER pool, so here the streaming handlers share that pool instead
     * of running on a streaming pool of their own. */
    bcfg.streaming_thread_count = CHTTPSVR_STREAMING_POOL_OFF;
    /* Short enough that a client which stops sending halfway through the body
     * hits a timeout quickly in the tests, without affecting /bounded-503,
     * which never calls chttpsvr_req_read. */
    bcfg.stream_read_timeout_us = 300000;
    ccol_retval_t brv = chttpsvr_start(g_bounded_srv, &bcfg);
    if (brv != ccol_success) {
      fprintf(stderr, "FATAL: bounded chttpsvr_start failed: %d\n", brv);
      exit(1);
    }
  }
  /* No readiness poll needed here either: the engine is already running, so a
   * later chttpsvr_start binds the socket synchronously, through
   * _make_listen_socket. */

  /* Create and start the server with the small max_body_size for the
   * max_body_size/413 boundary tests, buffered and streaming. */
  g_small_body_srv = chttpsvr_create(g_test_logger, NULL);
  if (!g_small_body_srv) {
    fprintf(stderr, "FATAL: could not create small-body chttpsvr\n");
    exit(1);
  }
  chttpsvr_register_handler(g_small_body_srv, CHTTP_POST, "/small-body-echo",
                            _echo_body_handler, NULL);
  chttpsvr_register_streaming_handler(g_small_body_srv, CHTTP_POST,
                                      "/small-body-stream",
                                      _stream_error_report_handler, NULL);
  {
    chttpsvr_config_t scfg = CHTTPSVR_CONFIG_DEFAULT;
    scfg.host = "127.0.0.1";
    scfg.port = TEST_PORT + 3;
    scfg.max_body_size = SMALL_BODY_MAX;
    ccol_retval_t srv_rv = chttpsvr_start(g_small_body_srv, &scfg);
    if (srv_rv != ccol_success) {
      fprintf(stderr, "FATAL: small-body chttpsvr_start failed: %d\n", srv_rv);
      exit(1);
    }
  }

  atexit(_teardown);
}

/* Forward declaration, defined in the RAW SOCKET HELPER section below. */
static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz);
static void _raw_request_to_test_server(const char *req, char *buf,
                                        size_t buflen);

/* ========================================================================== */
/*                         HELPER: MAKE GET/POST REQUESTS                    */
/* ========================================================================== */

static chttpcli_response *_get(const char *path) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  return resp;
}

static chttpcli_response *_post(const char *path, const char *body,
                                const char *ct) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttp_request_body_t b = {body, body ? strlen(body) : 0, ct};
  chttpcli_response *resp = NULL;
  chttp_post(url, &b, &resp);
  return resp;
}

/* GET with a custom header. */
static chttpcli_response *_get_with_header(const char *path, const char *name,
                                           const char *value) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  chttp_request_set_header(req, name, value);
  chttpcli_response *resp = NULL;
  chttpclient_do(chttp_default_client(), req, &resp);
  chttp_request_free(req);
  return resp;
}

/* ========================================================================== */
/*                         TESTS                                              */
/* ========================================================================== */

TEST(chttpserver, get_hello) {
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, post_echo_body) {
  chttpcli_response *resp = _post("/echo-body", "ping", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ping");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, post_echo_body_empty) {
  chttpcli_response *resp = _post("/echo-body", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_params) {
  chttpcli_response *resp = _get("/params/42/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=42 sub=hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_params_url_encoded) {
  chttpcli_response *resp = _get("/params/hello%20world/foo%2Fbar");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=hello world sub=foo/bar");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_param_plus_literal) {
  /* A '+' in a URL path segment has to stay a literal '+' in the captured
   * parameter value. The encoding rules of a path segment (RFC 3986) give '+'
   * no special meaning, and only application/x-www-form-urlencoded, which query
   * strings use, maps '+' to a space. So both the segment match and the
   * parameter capture decode with _decode_path_unsafe, never with
   * _decode_url_unsafe, which follows the query-string rules, turns '+' into '
   * ' and would give values that disagree with chttpsvr_req_path. */
  chttpcli_response *resp = _get("/params/hello+world/foo");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=hello+world sub=foo");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_single_value) {
  chttpcli_response *resp = _get("/query?q=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_multi_value) {
  chttpcli_response *resp = _get("/query?q=one&q=two&q=three");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "one,two,three");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_no_value) {
  chttpcli_response *resp = _get("/query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "no_q");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, request_header_read) {
  chttpcli_response *resp =
      _get_with_header("/header", "x-test-header", "myvalue");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "myvalue");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, missing_request_header) {
  chttpcli_response *resp = _get("/header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, custom_response_header) {
  chttpcli_response *resp = _get("/set-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *v = chttpclient_resp_header(resp, "x-custom");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "my-value");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, custom_status_code) {
  chttpcli_response *resp = _get("/status");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 201);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "created");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, json_response) {
  chttpcli_response *resp = _get("/json");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *ct = chttpclient_resp_header(resp, "content-type");
  REQUIRE_TRUE(ct != NULL);
  REQUIRE_TRUE(strstr(ct, "application/json") != NULL);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "{\"ok\":true}");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, not_found) {
  chttpcli_response *resp = _get("/does/not/exist");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, method_not_allowed) {
  /* /hello is registered only for GET, so a POST should get 405. */
  chttpcli_response *resp = _post("/hello", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 405);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, global_middleware_runs) {
  int before = g_global_mw_count;
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* _global_mw adds the x-global-mw header. */
  const char *v = chttpclient_resp_header(resp, "x-global-mw");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "1");
  REQUIRE_GT(g_global_mw_count, before);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_get_root) {
  chttpcli_response *resp = _get("/api/v1/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_with_param) {
  chttpcli_response *resp = _get("/api/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "item:99");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_segment_matches) {
  /* "%61" decodes to 'a'. The prefix part of the request path is
     percent-encoded, and it should still match the /api/v1 sub-router exactly
     like the request without the encoding does, just as registering the same
     effective pattern at the root level matches it. The server decodes the
     segments of a route pattern before comparing them, and a sub-router's
     prefix needs to be encoding-aware in the same way instead of being a raw
     byte-for-byte comparison. */
  chttpcli_response *resp = _get("/%61pi/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "item:99");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_root_matches) {
  /* The same as above for the path that is only the prefix (no route segments
     after it): /api/v1/ against /%61pi/v1/. */
  chttpcli_response *resp = _get("/%61pi/v1/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_middleware_runs) {
  int before = g_router_mw_count;
  chttpcli_response *resp = _get("/api/v1/items/1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* Both the global and the router middleware should have run. */
  const char *gv = chttpclient_resp_header(resp, "x-global-mw");
  const char *rv = chttpclient_resp_header(resp, "x-router-mw");
  REQUIRE_TRUE(gv != NULL);
  REQUIRE_TRUE(rv != NULL);
  REQUIRE_STREQ(gv, "1");
  REQUIRE_STREQ(rv, "1");
  REQUIRE_GT(g_router_mw_count, before);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, global_mw_not_for_unrelated) {
  /* Requests outside the sub-router should NOT have x-router-mw. */
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *rv = chttpclient_resp_header(resp, "x-router-mw");
  REQUIRE_TRUE(rv == NULL);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_short_circuit) {
  /* /blocked-area/secret uses _blocking_mw, which doesn't call next. */
  chttpcli_response *resp = _get("/blocked-area/secret");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 403);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "blocked");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_echo) {
  const char *payload = "streaming payload data";
  chttpcli_response *resp = _post("/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, payload);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, raw_query_string) {
  chttpcli_response *resp = _get("/raw-query?foo=bar&baz=1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  /* The raw query is the query string before any decoding. */
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_TRUE(strstr(resp->body, "foo=bar") != NULL);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_accessor) {
  chttpcli_response *resp = _get("/path");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "/path");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, serve_double_start) {
  /* Starting a server instance that is already started fails with
   * not_permitted. _setup starts g_srv, so a second start of it must be
   * rejected whatever port is chosen. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval_t rv = chttpsvr_start(g_srv, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

TEST(chttpserver, multi_server_start_stop) {
  /* A second server on a different port has to start correctly and serve its
   * own requests, which shows that the library supports more than one server.
   *
   * g_srv2 is a long-lived shared fixture handle that several other tests of
   * this file reuse, and this test doesn't own it, so it can't give it an RAII
   * scope here. Instead every outcome below goes into a local and the test
   * calls chttpsvr_stop(g_srv2) unconditionally, before any REQUIRE_* that
   * could return early. A real regression in what this test checks (starting
   * and serving a second concurrent server) must not leave g_srv2's listener
   * alive for every later test of this binary to trip over. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  chttpsvr_register_handler(g_srv2, CHTTP_GET, "/ping-srv2", _hello_handler,
                            NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  /* No readiness poll: chttpsvr_start binds the socket synchronously, through
   * _make_listen_socket. */

  bool resp1_ok = false, body1_ok = false;
  int status1 = -1;
  char body1_copy[128] = {0};
  bool resp2_ok = false;
  int status2 = -1;

  if (rv == ccol_success) {
    /* The request to srv2 needs to succeed. */
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ping-srv2", TEST_PORT + 1);
    chttpcli_response *resp = NULL;
    chttp_get(url, &resp);
    resp1_ok = resp != NULL;
    if (resp1_ok) {
      status1 = resp->status_code;
      body1_ok = resp->body != NULL;
      if (body1_ok) snprintf(body1_copy, sizeof(body1_copy), "%s", resp->body);
      chttpclient_resp_free(resp);
    }

    /* Make sure g_srv (on TEST_PORT) isn't affected. */
    resp = _get("/hello");
    resp2_ok = resp != NULL;
    if (resp2_ok) {
      status2 = resp->status_code;
      chttpclient_resp_free(resp);
    }
  }

  chttpsvr_stop(g_srv2);

  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_TRUE(resp1_ok);
  REQUIRE_EQ(status1, 200);
  REQUIRE_TRUE(body1_ok);
  REQUIRE_STREQ(body1_copy, "Hello, world!");
  REQUIRE_TRUE(resp2_ok);
  REQUIRE_EQ(status2, 200);
}

TEST(chttpserver, query_one_single) {
  /* chttpsvr_req_query_one returns the value when there is exactly one pair. */
  chttpcli_response *resp = _get("/query-one?q=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "only");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_404_outside_prefix) {
  /* /api/v2/ isn't registered, so it should be 404. */
  chttpcli_response *resp = _get("/api/v2/items/1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, large_body_post) {
  /* POST a 64 KiB body and check the echo. */
  size_t sz = 65536;
  char *big = (char *)malloc(sz + 1);
  REQUIRE_TRUE(big != NULL);
  for (size_t i = 0; i < sz; i++) big[i] = (char)('A' + (int)(i % 26));
  big[sz] = '\0';

  chttpcli_response *resp =
      _post("/echo-body", big, "application/octet-stream");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_EQ(resp->body_len, sz);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_EQ(memcmp(resp->body, big, sz), 0);
  chttpclient_resp_free(resp);
  free(big);
}

/* ========================================================================== */
/*                         ADDITIONAL COVERAGE TESTS                          */
/* ========================================================================== */

TEST(chttpserver, query_one_multi_rejected) {
  /* chttpsvr_req_query_one reports ccol_key_already_present for a key
   * with several values. */
  chttpcli_response *resp = _get("/query-one?q=a&q=b");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "multi_value");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_missing) {
  /* chttpsvr_req_query_one needs to report ccol_key_not_found when the key is
     missing.
   */
  chttpcli_response *resp = _get("/query-one");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_plus_decoded_to_space) {
  /* A '+' in a query value has to decode to a space. */
  chttpcli_response *resp = _get("/query?q=hello+world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_percent_decoded) {
  /* Percent-encoded characters in a query value are decoded. */
  chttpcli_response *resp = _get("/query?q=hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_present) {
  /* chttpsvr_req_header reads conn->hdr_names/hdr_values, the same flat header
     array filled for every request whatever the kind of route, and this checks
     that it works the same way for a streaming route. */
  chttpcli_response *resp =
      _get_with_header("/stream-header", "x-stream-test", "stream-val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "stream-val");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_absent) {
  /* A missing header on a streaming route gives NULL from chttpsvr_req_header.
   */
  chttpcli_response *resp = _get("/stream-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, stream_handler_registration_accepted) {
  /* chttpsvr_register_streaming_handler should accept a valid registration on a
     server that isn't started. The server owns its ctpool, so the caller passes
     no pool. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, subrouter_stream_route) {
  /* A streaming route registered on a sub-router must get the body, go
     through the global and the router middleware, and echo the response. */
  const char *payload = "api-stream-payload";
  chttpcli_response *resp = _post("/api/v1/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, payload);
  /* The global middleware still runs on streaming sub-router routes. */
  const char *gv = chttpclient_resp_header(resp, "x-global-mw");
  REQUIRE_TRUE(gv != NULL);
  REQUIRE_STREQ(gv, "1");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_method_not_allowed) {
  /* A method that matches no registered route on a sub-router whose path does
     match has to return 405, not 404. */
  chttpcli_response *resp = _post("/api/v1/items/1", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 405);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_mw_in_chain_runs_both) {
  /* Two middlewares registered one after the other on the same sub-router both
     need to run, in registration order (x-mw-a first, x-mw-b second). */
  int ba = g_mw_a_count, bb = g_mw_b_count;
  chttpcli_response *resp = _get("/mw-chain/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *va = chttpclient_resp_header(resp, "x-mw-a");
  const char *vb = chttpclient_resp_header(resp, "x-mw-b");
  REQUIRE_TRUE(va != NULL);
  REQUIRE_TRUE(vb != NULL);
  REQUIRE_STREQ(va, "1");
  REQUIRE_STREQ(vb, "1");
  REQUIRE_GT(g_mw_a_count, ba);
  REQUIRE_GT(g_mw_b_count, bb);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, invalid_param_pattern_rejected) {
  /* A route pattern with an unclosed '{' is rejected at registration
     time with ccol_invalid_args instead of silently extracting a wrong param
     name. We use g_srv2 because it's never started, so adding invalid routes to
     it doesn't affect the running test server. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{unclosed",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* Well-formed patterns are still expected to be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{valid}/end",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, invalid_param_name_chars_rejected) {
  /* A route pattern whose {name} segment holds a character outside [A-Za-z0-9_]
   * must be rejected at registration time with ccol_invalid_args, because
   * chttpsvr_req_param could never read such a name back and the pattern would
   * create parameters nothing can reach. The test uses g_srv2, which is never
   * started, so it doesn't pollute the running test server. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  /* A space inside the param name. */
  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/{hello world}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* A hyphen inside the param name. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{user-id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* A plus inside the param name. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{a+b}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Names made only of [A-Za-z0-9_] are still accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{userId}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{order_id}/items",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, duplicate_param_name_in_pattern_rejected) {
  /* A pattern that uses the same {name} more than once, for example
   * /a/{id}/b/{id}, has to be rejected at registration time with
   * ccol_invalid_args. Without this check the route compiles fine, but the
   * value captured by the SECOND use can't be reached, because
   * chttpsvr_req_param always returns on the first match of the name. The test
   * uses g_srv2, which is never started, so it doesn't pollute the running test
   * server. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/dup/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/dup-stream/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/dup-router");
  REQUIRE_TRUE(r != NULL);
  rv = chttpsvr_router_on(r, CHTTP_GET, "/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* A pattern with distinct param names should still be accepted, even when it
   * reuses the same shape of literal segments. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/dup/{id}/b/{other_id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                    MULTI-METHOD SAME-PATH TESTS                            */
/* ========================================================================== */

TEST(chttpserver, multi_method_get) {
  /* GET /dual must reach the GET handler even though POST /dual is registered
     too. _find_route matches on the path AND the method together, so this holds
     whatever order the two registrations come in, it doesn't depend on that
     order by luck. */
  chttpcli_response *resp = _get("/dual");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_method_post) {
  /* POST /dual reaches the POST handler even though GET /dual is
     registered first. A _find_route that stopped at the first path match would
     break this: the GET route matches the path, the method check fails and the
     search ends there, so the client would get a 405 instead of the POST
     handler's response. */
  chttpcli_response *resp = _post("/dual", "dual-body", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "dual-body");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_method_unregistered_405) {
  /* A method that isn't registered for /dual must still return 405, not 404.
   */
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

/* RFC 9110 SS15.5.6: a 405 carries an Allow header listing the methods the
   target resource supports. /dual is registered for GET and POST, and the GET
   route serves HEAD as well. */
TEST(chttpserver, method_not_allowed_lists_the_allowed_methods) {
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:GET, HEAD, POST\r\n") != NULL);
}

/* /echo-body is registered only for POST, so HEAD isn't allowed and the header
   names only POST. */
TEST(chttpserver, method_not_allowed_without_a_get_route_omits_head) {
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/echo-body", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:POST\r\n") != NULL);
}

/* A 404 names no methods. */
TEST(chttpserver, not_found_carries_no_allow_header) {
  char buf[2048] = {0};
  int status =
      _raw_request("PUT", "/no-such-path-for-allow", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:") == NULL);
}

/* ========================================================================== */
/*                    DOUBLE-SLASH PATTERN REJECTION TEST                     */
/* ========================================================================== */

TEST(chttpserver, double_slash_pattern_rejected) {
  /* A route pattern with two slashes in a row, for example /foo//bar, has to be
     rejected at registration time with ccol_invalid_args, because quietly
     turning such a pattern into /foo/bar surprises the caller and leads to
     mistakes. The test uses g_srv2, which is never started. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo//bar",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* A leading double slash (after one is stripped) must be rejected as well.
   */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "//leading", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* A well-formed pattern still succeeds. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                           MORE COVERAGE TESTS                              */
/* ========================================================================== */

TEST(chttpserver, multi_write_accumulates) {
  /* Several chttpsvr_resp_write_str calls on the same response need to add up
     into a single body. */
  chttpcli_response *resp = _get("/multi-write");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "foobarbaz");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, printf_accumulates) {
  /* chttpsvr_resp_printf has to format like printf and add to the body across
     several calls. One of the calls produces output long enough to force the
     heap fallback path inside chttpsvr_resp_printf. */
  chttpcli_response *resp = _get("/printf");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[512];
  size_t off =
      (size_t)snprintf(expected, sizeof(expected), "n=42 s=hi f=3.50 ");
  for (int i = 0; i < 40; i++) {
    off += (size_t)snprintf(expected + off, sizeof(expected) - off, "%08d-", i);
  }
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_url_decoded) {
  /* chttpsvr_req_path() needs to return the URL-decoded path, not the raw
     percent-encoded form the client sent. */
  chttpcli_response *resp = _get("/echo-path/hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "/echo-path/hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_literal_segment_is_route_mismatch) {
  /* A path segment can hold a NUL byte coming from a %00 escape, and the match
   * of a literal segment, which uses strcmp, must not take "hello" plus the
   * garbage after it as an exact match for the registered literal segment
   * "hello": /hello%00xyz must NOT match the literal route /hello. Without the
   * guard against an embedded NUL, the literal-segment comparison of
   * _match_route_cached decodes "hello%00xyz" to "hello\0xyz" and compares it
   * with "hello" through strcmp, which stops at the first NUL of either operand
   * and reports a match. A suffix of the attacker's choice could then hide
   * behind a route match that the caller expects to cover the whole segment. */
  chttpcli_response *resp = _get("/hello%00xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_param_segment_is_route_mismatch) {
  /* The same kind of hazard through a {param} segment instead of a literal one.
   * _seg_cache_get should treat an embedded NUL in the decoded value as a
   * decode failure, the same as a malformed %XX escape, instead of handing a
   * truncated value to a match. */
  chttpcli_response *resp = _get("/echo-path/hello%00world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_query_key_does_not_alias) {
  /* The same kind of hazard in the query-string decoder: a key "q%00x" must not
   * decode and then strcmp its way into aliasing the unrelated key "q". Without
   * the guard, _parse_qparams stores the fully decoded "q\0x", and the
   * strcmp(qp->keys[i], "q") of chttpsvr_req_query_one stops at the embedded
   * NUL and reports a match that isn't one. */
  chttpcli_response *resp = _get("/query-one?q%00x=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query) {
  /* Calling chttpsvr_req_query for two different keys in one request has to
     give the right values for both, even though the two calls share and reuse
     the _qresult scratch array. */
  chttpcli_response *resp = _get("/multi-query?a=hello&b=world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=hello b=world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query_missing) {
  /* Missing keys must give the nil sentinel whatever the order of the calls.
   */
  chttpcli_response *resp = _get("/multi-query?a=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=only b=nil");
  chttpclient_resp_free(resp);
}

/* A pass-through allocator that counts its calls. It exercises
 * chttpsvr_create_mp and lets the test show that the caller's procs handle
 * every allocation and every free and the library's default allocator handles
 * none of them, which a create and a destroy that merely succeed don't show. */
static size_t g_wrap_malloc_count;
static size_t g_wrap_free_count;
static size_t g_wrap_calloc_count;
static size_t g_wrap_realloc_count;

static void *_wrap_malloc(size_t n) {
  g_wrap_malloc_count++;
  return malloc(n);
}
static void _wrap_free(void *p) {
  if (p) g_wrap_free_count++;
  free(p);
}
static void *_wrap_calloc(size_t n, size_t s) {
  g_wrap_calloc_count++;
  return calloc(n, s);
}
static void *_wrap_realloc(void *p, size_t s) {
  g_wrap_realloc_count++;
  return realloc(p, s);
}

TEST(chttpserver, custom_allocator_lifecycle) {
  /* chttpsvr_create_mp must succeed with a custom allocator, accept route
     registrations, and on destroy free all the memory through the same
     allocator. This success path uses the same copy of m_procs that the failure
     path of _ccol_mutex_init depends on. The test counts the wrapper's calls
     instead of using bare pass-through functions, and the counts show that
     chttpsvr_create_mp hands the supplied procs to every place that allocates
     instead of falling back to the library's default. */
  g_wrap_malloc_count = 0;
  g_wrap_free_count = 0;
  g_wrap_calloc_count = 0;
  g_wrap_realloc_count = 0;

  ccol_memmgmt_procs_t mp = {_wrap_malloc, _wrap_free, _wrap_calloc,
                             _wrap_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_GT(g_wrap_malloc_count + g_wrap_calloc_count, (size_t)0);
  REQUIRE_EQ(g_wrap_free_count, (size_t)0);

  ccol_retval_t rv =
      chttpsvr_register_handler(srv, CHTTP_GET, "/ping", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  rv = chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/sping",
                                           _stream_echo_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_router *r = chttpsvr_subrouter(srv, "/sub");
  REQUIRE_TRUE(r != NULL);
  rv = chttpsvr_router_on(r, CHTTP_GET, "/x", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  size_t allocs_before_destroy = g_wrap_malloc_count + g_wrap_calloc_count;
  chttpsvr_destroy(srv);
  /* The test drives a create and three route registrations, and the same
     wrapper has to free every one of those allocations before destroy returns.
     A custom allocator that the library ignored would leave g_wrap_free_count
     at 0 here. */
  REQUIRE_GT(g_wrap_free_count, (size_t)0);
  REQUIRE_GE(g_wrap_free_count, allocs_before_destroy);
}

/* ========================================================================== */
/*                    COVERAGE OF PATHS, ALLOCATION AND REJECTION             */
/* ========================================================================== */

TEST(chttpserver, subrouter_root_no_trailing_slash) {
  /* /api/v1 without a trailing slash must match the "/" route of the /api/v1
     sub-router. _find_route turns the empty sub-path into "/" before matching,
     so both /api/v1 and /api/v1/ reach _api_root_handler. */
  chttpcli_response *resp = _get("/api/v1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, long_path_heap_alloc) {
  /* A URL path longer than 511 bytes takes the heap allocation branch in
     _on_request (instead of the 512-byte stack buffer). The /echo-path/{v}
     route echoes the decoded path back, and a 510-char param gives a 521-char
     path, which is over the threshold. */
  char param[511];
  memset(param, 'x', sizeof(param) - 1);
  param[sizeof(param) - 1] = '\0';

  size_t url_sz =
      strlen(BASE_URL) + strlen("/echo-path/") + (sizeof(param) - 1) + 1;
  char *url = (char *)malloc(url_sz);
  REQUIRE_TRUE(url != NULL);
  snprintf(url, url_sz, BASE_URL "/echo-path/%s", param);

  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  free(url);

  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);

  char expected[530];
  snprintf(expected, sizeof(expected), "/echo-path/%s", param);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

/* A reject_pool thread increments g_reject_task_run_count_for_tests AFTER the
   server has written the rejection response to the client and closed the
   connection (the comment of _reject_task explains why releasing
   in_flight_requests comes last, and this counter goes up with it). So a client
   that has just read its response can get control back in the test before that
   increment runs, which is a harmless scheduling race and not an order the
   library promises. That's why this function polls for the expected delta
   instead of asserting right away, the same pattern this codebase uses for
   timing after an asynchronous completion (see the wait loop for
   g_mm_free_count in tests_mem_mgmt.c). It returns the count it saw, so the
   caller can still assert on it. */
static size_t _wait_for_reject_pool_task_count(size_t expected) {
  for (int attempt = 0; attempt < 50; attempt++) {
    if (_chttpsvr_reject_pool_task_count_for_tests() >= expected) break;
    usleep(20000);
  }
  return _chttpsvr_reject_pool_task_count_for_tests();
}

/* ========================================================================== */
/*                  HELPERS: THE DATE RESPONSE HEADER                         */
/* ========================================================================== */

/* Parses an IMF-fixdate of RFC 9110 SS5.6.7, such as "Sun, 06 Nov 1994 08:49:37
   GMT", strictly and independently of the locale. The weekday must agree with
   the date. */
static bool _parse_imf_fixdate(const char *v, size_t len, time_t *out) {
  static const char *days[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
  static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                 "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
  if (len != 29) return false;
  if (v[3] != ',' || v[4] != ' ' || v[7] != ' ' || v[11] != ' ' ||
      v[16] != ' ' || v[19] != ':' || v[22] != ':' ||
      memcmp(v + 25, " GMT", 4) != 0)
    return false;
  const int digit_at[] = {5, 6, 12, 13, 14, 15, 17, 18, 20, 21, 23, 24};
  for (size_t i = 0; i < sizeof(digit_at) / sizeof(digit_at[0]); i++)
    if (v[digit_at[i]] < '0' || v[digit_at[i]] > '9') return false;
  int wday = -1, mon = -1;
  for (int i = 0; i < 7; i++)
    if (memcmp(v, days[i], 3) == 0) wday = i;
  for (int i = 0; i < 12; i++)
    if (memcmp(v + 8, months[i], 3) == 0) mon = i;
  if (wday < 0 || mon < 0) return false;
#define _D2(o) ((v[o] - '0') * 10 + (v[(o) + 1] - '0'))
  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_mday = _D2(5);
  tm.tm_mon = mon;
  tm.tm_year = _D2(12) * 100 + _D2(14) - 1900;
  tm.tm_hour = _D2(17);
  tm.tm_min = _D2(20);
  tm.tm_sec = _D2(23);
#undef _D2
  time_t t = timegm(&tm);
  struct tm back;
  if (t == (time_t)-1 || !gmtime_r(&t, &back) || back.tm_wday != wday ||
      back.tm_mday != tm.tm_mday)
    return false;
  *out = t;
  return true;
}

/* Walks every response in buf, counts the Date header lines of each header
   block and checks that each one parses and, when now isn't 0, is within
   max_skew seconds of now. Returns the number of responses whose header block
   has exactly one such line, and puts the total number of responses in *total.
 */
static int _responses_with_one_date(const char *buf, time_t now, long max_skew,
                                    int *total) {
  int good = 0;
  *total = 0;
  const char *p = buf;
  while ((p = strstr(p, "HTTP/1.1 ")) != NULL) {
    const char *end = strstr(p, "\r\n\r\n");
    if (!end) break;
    (*total)++;
    int dates = 0;
    bool fresh = true;
    size_t body_len = 0;
    for (const char *line = strstr(p, "\r\n") + 2; line < end;) {
      const char *eol = strstr(line, "\r\n");
      if (strncasecmp(line, "date:", 5) == 0) {
        dates++;
        time_t t;
        const char *val = line + 5;
        while (*val == ' ') val++;
        if (!_parse_imf_fixdate(val, (size_t)(eol - val), &t) ||
            (now && labs((long)(t - now)) > max_skew))
          fresh = false;
      }
      if (strncasecmp(line, "content-length:", 15) == 0)
        body_len = strtoul(line + 15, NULL, 10);
      line = eol + 2;
    }
    if (dates == 1 && fresh) good++;
    /* A HEAD response reports a length and sends no body. */
    size_t rest = strlen(end + 4);
    p = end + 4 + (body_len < rest ? body_len : rest);
  }
  return good;
}

/* Puts the value of the single Date header of the first response in buf into
   out, or returns false when there isn't exactly one. */
static bool _first_date_value(const char *buf, char *out, size_t out_sz) {
  const char *end = strstr(buf, "\r\n\r\n");
  if (!end) return false;
  int found = 0;
  for (const char *line = strstr(buf, "\r\n") + 2; line < end;) {
    const char *eol = strstr(line, "\r\n");
    if (strncasecmp(line, "date:", 5) == 0) {
      const char *val = line + 5;
      while (*val == ' ') val++;
      size_t n = (size_t)(eol - val);
      if (n >= out_sz) return false;
      memcpy(out, val, n);
      out[n] = '\0';
      found++;
    }
    line = eol + 2;
  }
  return found == 1;
}

TEST(chttpserver, bounded_pool_full_returns_503) {
  /* ctpool_try_submit returns ccol_container_full when the server's ctpool is
   * full, and the server then must answer with a 503.
   *
   * g_bounded_srv has 1 worker thread and queue_capacity=1, so its total
   * capacity is 2, one running task and one queued task. The test sends two
   * background HTTP requests that block inside _bounded_blk_handler and fill
   * both slots, and then a third request, which gets a 503.
   *
   * The white-box counter g_reject_task_run_count_for_tests also confirms that
   * a reject_pool thread wrote the 503, not the synchronous last-resort
   * fallback; a black-box client can't tell the two apart because both put the
   * same response on the wire. The counter is process-wide and not per server,
   * but tau runs one test at a time on this thread and nothing else in this
   * process (neither the idle sweep nor other servers' handlers) rejects a
   * connection on its own, so a plain before-and-after delta across this test's
   * window is unambiguous.
   *
   * Every outcome below goes into a local instead of being asserted at once,
   * and the test releases and joins every background thread UNCONDITIONALLY
   * before any REQUIRE_* runs at all. g_bounded_srv has only ONE worker thread,
   * so a REQUIRE_* that returned early from this function before that release
   * would leave that worker blocked inside _bounded_blk_handler for the rest of
   * the process's life, since nothing else in this test binary unblocks it
   * before the process exits, and every later test that reaches g_bounded_srv
   * would wait behind a worker that can never make progress. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();
  /* The request timeout turns a wrong outcome into a failure instead of a hang.
   * If request 3 got a queue slot instead of a 503, it would wait for g_blk_go,
   * which this test sets only after request 3 returns. */
  g_bounded_cli = chttpclient_create(NULL);
  bool cli_ok =
      g_bounded_cli != CHTTPCLI_INVALID &&
      chttpclient_set_pool_size(g_bounded_cli, 8) == ccol_success &&
      chttpclient_set_request_timeout(g_bounded_cli, 30000000) == ccol_success;

  /* Reset whatever an earlier run of this test left behind. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  g_blk_count = 0;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Request 1 fills the running worker's slot. */
  pthread_t t1;
  bool t1_created =
      cli_ok && pthread_create(&t1, NULL, _send_bounded_req, NULL) == 0;

  /* Spin until the worker is really running (and blocked in) the handler. Give
   * up after 5 s, so that a stalled environment fails instead of hanging. */
  bool worker_blocked = false;
  if (t1_created) {
    struct timespec nap = {0, 1000000L}; /* 1 ms */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 5;
    for (;;) {
      if (g_blk_count >= 1) {
        worker_blocked = true;
        break;
      }
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!(now.tv_sec < deadline.tv_sec ||
            (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)))
        break; /* timed out, worker_blocked stays false */
      nanosleep(&nap, NULL);
    }
  }

  /* Request 2 fills the single queue slot. The worker is blocked, so this
   * request stays queued until g_blk_go is set. */
  pthread_t t2;
  bool t2_created = false;
  bool request2_queued = false;
  if (worker_blocked) {
    t2_created = pthread_create(&t2, NULL, _send_bounded_req, NULL) == 0;
    if (t2_created) {
      /* Wait until the server counts request 2 as in flight. The single reactor
       * thread increments that count and submits the task in the same dispatch,
       * so once the count is 2, request 3 (dispatched later by the same reactor
       * thread) can't take the queue slot ahead of request 2. The 5 s bound is
       * only a guard against a hang. */
      struct timespec nap = {0, 1000000L}; /* 1 ms */
      struct timespec deadline;
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_sec += 5;
      for (;;) {
        if (_chttpsvr_in_flight_requests_for_tests(g_bounded_srv) >= 2) {
          request2_queued = true;
          break;
        }
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (!(now.tv_sec < deadline.tv_sec || (now.tv_sec == deadline.tv_sec &&
                                               now.tv_nsec < deadline.tv_nsec)))
          break;
        nanosleep(&nap, NULL);
      }
    }
  }

  /* Now the pool is full (1 running + 1 queued), and the third request has to
     get 503. */
  int status = -1;
  bool got_resp = false;
  bool date_ok = false;
  size_t reject_count_after = reject_count_before;
  if (worker_blocked && request2_queued) {
    chttpcli_response *resp = _bounded_get();
    if (resp) {
      got_resp = true;
      status = resp->status_code;
      const char *d = chttpclient_resp_header(resp, "date");
      time_t t;
      date_ok = d && _parse_imf_fixdate(d, strlen(d), &t) &&
                labs((long)(t - time(NULL))) <= 5;
      chttpclient_resp_free(resp);
    }
    reject_count_after =
        _wait_for_reject_pool_task_count(reject_count_before + 1);
  }

  /* Release the blocked handlers and join every background thread that started,
   * before any REQUIRE_* below runs. The opening comment of this test explains
   * why this order is load-bearing and not just tidy. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  if (t1_created) pthread_join(t1, NULL);
  if (t2_created) pthread_join(t2, NULL);
  if (g_bounded_cli != CHTTPCLI_INVALID) chttpclient_destroy(g_bounded_cli);
  g_bounded_cli = CHTTPCLI_INVALID;

  /* Reset g_blk_go in case of a rerun, for example the safety broadcast of
   * _teardown. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Every background thread is joined by now and the shared worker is
   * unblocked, so from here on it's safe to fail. */
  REQUIRE_TRUE(t1_created);
  REQUIRE_TRUE(worker_blocked);
  REQUIRE_TRUE(t2_created);
  REQUIRE_TRUE(request2_queued);
  REQUIRE_TRUE(got_resp);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(date_ok);
  REQUIRE_EQ(reject_count_after, reject_count_before + 1);
}

TEST(chttpserver, stop_on_unstarted_server_is_safe) {
  /* chttpsvr_stop on a server with started==false has to be a safe no-op that
   * the caller can call as many times as it likes without a crash. A server has
   * started==false when it was never started, and also after an earlier
   * chttpsvr_stop() call put it there. _setup doesn't start g_srv2, but at this
   * point of the suite's order multi_server_start_stop has already started and
   * stopped it once. Either way started==false here, and that's the only
   * property this test relies on. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2); /* must not block or crash */
  chttpsvr_stop(g_srv2); /* second call, should be a no-op too */
}

/* ========================================================================== */
/*                         RAW SOCKET HELPER                                  */
/* ========================================================================== */

/* Sends a hand-made HTTP/1.1 request over a raw TCP socket and returns the HTTP
   status code, or -1 on a socket error. extra_headers is either NULL or holds
   the \r\n at the end of each header line. The full raw response (status line,
   headers and body) is written into buf[0..buf_sz-1]. Use it for cases the
   chttpclient abstraction can't express, such as sending the same header name
   twice, which exercises the documented "last occurrence wins" behaviour of
   chttpsvr_req_header. */
static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* Bounds the read loop below. A regression can make the server never respond
     and never close, for example a server that reads a declared but never sent
     body before it rejects an unmatched route (the kind of bug
     unmatched_route_rejected_without_reading_body catches). Without the bound
     the read(2) call blocks forever and the caller of this helper can't fail
     its own assertion cleanly. Every caller of this shared helper gets the
     protection, not only the one named here. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  char req[2048];
  int n = snprintf(req, sizeof(req),
                   "%s %s HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "%s"
                   "Connection: close\r\n"
                   "\r\n",
                   method, path, extra_headers ? extra_headers : "");
  /* snprintf returns the number of bytes it WOULD have written, which can be
   * more than sizeof(req) when it truncates. Clamp to the real buffer length so
   * the write loop doesn't read past the array. */
  if (n >= (int)sizeof(req)) n = (int)sizeof(req) - 1;
  {
    size_t sent = 0;
    while (sent < (size_t)n) {
      ssize_t w = write(fd, req + sent, (size_t)n - sent);
      if (w < 0) {
        close(fd);
        fd = -1;
        return -1;
      }
      sent += (size_t)w;
    }
  }

  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = read(fd, buf + total, buf_sz - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);
  fd = -1;

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

/* Connects and sends a request whose body goes out in several chunks with a
   delay between them, so the server needs to see separate reads off the socket,
   one after the other, instead of one blob from a single recv. Then it reads
   the full response and returns the HTTP status code, or -1 on a socket
   failure. It always closes after one response. */
static int _raw_request_drip_body(int port, const char *method,
                                  const char *path, const char *body,
                                  size_t chunk_len, unsigned delay_us,
                                  char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* The same comment in _raw_request applies. A regression can make the server
     never respond, and without this bound the final read loop below blocks
     forever: the caller can't fail its own assertion cleanly and the whole
     binary hangs instead of one test failing. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  size_t body_len = strlen(body);
  char hdr[512];
  int hn = snprintf(hdr, sizeof(hdr),
                    "%s %s HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    method, path, body_len);
  if (hn < 0 || (size_t)hn >= sizeof(hdr)) {
    close(fd);
    fd = -1;
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return -1;
  }

  size_t sent = 0;
  while (sent < body_len) {
    size_t n = body_len - sent < chunk_len ? body_len - sent : chunk_len;
    ssize_t w = write(fd, body + sent, n);
    if (w < 0) {
      close(fd);
      fd = -1;
      return -1;
    }
    sent += (size_t)w;
    if (sent < body_len && delay_us) usleep(delay_us);
  }

  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = read(fd, buf + total, buf_sz - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);
  fd = -1;

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

/* Connects and sends headers that declare a Content-Length much bigger than
   what it writes: only `sent_len` bytes of the body go out, and then the socket
   is closed without waiting for or reading a response. That
   simulates a client aborting halfway through an upload. Returns 0 when the
   connect and the write succeed and -1 on a socket failure; the caller has no
   response to look at, because the connection is torn down on purpose. */
static int _raw_request_abort_mid_body(uint16_t port, const char *path,
                                       const char *partial_body,
                                       size_t declared_len) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }

  char hdr[512];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST %s HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    path, declared_len);
  if (hn < 0 || (size_t)hn >= sizeof(hdr)) {
    close(fd);
    fd = -1;
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return -1;
  }
  size_t partial_len = strlen(partial_body);
  if (partial_len && write(fd, partial_body, partial_len) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  close(fd); /* abort, never send the rest of the declared body */
  fd = -1;
  return 0;
}

/* The same shape as _raw_request_abort_mid_body above, connect and then send
   part of the body. But instead of closing the whole socket it half-closes the
   write side with shutdown(fd, SHUT_WR) and then reads back the response the
   server sends before it closes. A full close gives the server a connection
   reset, while this half-close gives it a clean EOF from read() or
   ctls_conn_read() in the middle of the body. That's the truncation path of
   chttpsvr_req_read(), where n == 0 arrives before the message has finished its
   framing, and the caller can look at the response instead of racing a
   connection that's already gone. Returns 0 when the connect, the write and the
   read succeed, with buf holding the NUL-terminated raw response, and -1 on a
   socket failure. */
static int _raw_request_truncate_body_half_close(const char *path,
                                                 const char *partial_body,
                                                 size_t declared_len, char *buf,
                                                 size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* The same comment in _raw_request applies. A regression can make the server
     never respond, and without this bound the final read loop below blocks
     forever: the caller can't fail its own assertion cleanly and the whole
     binary hangs instead of one test failing. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  char hdr[512];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST %s HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    path, declared_len);
  if (hn < 0 || (size_t)hn >= sizeof(hdr)) {
    close(fd);
    fd = -1;
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return -1;
  }
  size_t partial_len = strlen(partial_body);
  if (partial_len && write(fd, partial_body, partial_len) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  shutdown(fd, SHUT_WR);

  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = read(fd, buf + total, buf_sz - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);
  fd = -1;
  return 0;
}

/* Reads from fd until EOF or until buf is full, whichever comes first, and
   NUL-terminates the result. Tests use it to drain one whole raw HTTP response,
   or several pipelined ones, off a socket that the server closes when
   it's done. Every read(2) is bounded with SO_RCVTIMEO, the same guard as in
   _raw_request and _read_one_http_response. Without it a regression that makes
   the server never respond and never close would block this call forever, the
   caller couldn't fail its own assertion cleanly and the whole binary would
   hang instead of one test failing. Returns the total number of bytes read,
   never counting the trailing NUL, and doesn't close fd. */
static size_t _drain_socket_until_eof(int fd, char *buf, size_t buf_sz) {
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = read(fd, buf + total, buf_sz - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  return total;
}

/* Reads exactly one HTTP/1.1 response (headers plus a body with a
   Content-Length) off an already connected socket and leaves the connection
   open for the next request, which the tests use for keep-alive across two
   requests on one connection. It assumes a short, non-chunked response, which
   holds for every fixture handler that uses this helper. Returns the status
   code, or -1 on a failure.

   Every read(2) below is bounded. Without that a regression that makes the
   server never respond and never close would block this call forever, the
   caller couldn't fail its own assertion cleanly and the whole binary would
   hang instead of one test failing. It's the same SO_RCVTIMEO guard as in
   _raw_request above, and every caller of this shared helper gets the
   protection, not only the one named above: the keep-alive tests, the
   Unix-socket tests, the max_connections tests and the self-restart tests of
   this file all use it. For a caller that already set its own SO_RCVTIMEO on fd
   the call changes nothing, since setsockopt writes the same option with the
   same value. */
static int _read_one_http_response(int fd, char *buf, size_t buf_sz) {
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  size_t total = 0;
  char *hdr_end = NULL;
  while (total < buf_sz - 1) {
    ssize_t r = read(fd, buf + total, buf_sz - 1 - total);
    if (r <= 0) return -1;
    total += (size_t)r;
    buf[total] = '\0';
    hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) break;
  }
  if (!hdr_end) return -1;

  size_t need = (size_t)(hdr_end - buf) + 4;
  char *cl = strstr(buf, "content-length:");
  if (cl && cl < hdr_end) need += (size_t)strtoul(cl + 15, NULL, 10);

  while (total < need && total < buf_sz - 1) {
    ssize_t r = read(fd, buf + total, buf_sz - 1 - total);
    if (r <= 0) break;
    total += (size_t)r;
  }
  buf[total] = '\0';

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

/* Takes the response body out of the raw response that _raw_request wrote and
   decodes it in place inside buf, both for a response with a Content-Length and
   for one with Transfer-Encoding: chunked. Returns a pointer to the
   NUL-terminated decoded body, or NULL when the separator between the headers
   and the body is missing.

   With a Content-Length the bytes after \r\n\r\n are already the literal body.
   chttpserver always sets Content-Length explicitly and never chunk-encodes its
   own responses (see the doc comment of _send_response in chttpserver.c), so
   the chunked branch below is a defence and not a path this server's responses
   take. It stays for two reasons: it keeps the helper correct if that behaviour
   ever changes, and it also decodes the chunked *request* bodies that the tests
   of this file send, so the file doesn't need a second, almost identical helper
   (chunked_body_with_trailer_headers_handled_once is one such test). */
static char *_decode_raw_body(char *buf) {
  char *sep = strstr(buf, "\r\n\r\n");
  if (!sep) return NULL;
  char *body = sep + 4;

  /* Look for chunked encoding in the header section only. */
  char saved = sep[0];
  sep[0] = '\0'; /* end the header block here for a moment */
  bool chunked = (strcasestr(buf, "transfer-encoding: chunked") != NULL);
  sep[0] = saved;

  if (!chunked) return body;

  /* _raw_request always NUL-terminates its buffer, so buf+strlen(buf) is a safe
   * upper bound for the reads. */
  char *end_buf = buf + strlen(buf);

  /* In-place chunked decode: chunk-size CRLF chunk-data CRLF ... 0 CRLF CRLF
   */
  char *out = body;
  char *p = body;
  while (*p) {
    char *end = NULL;
    unsigned long chunk_size = strtoul(p, &end, 16);
    if (end == p) break; /* malformed, stop */
    if (*end == '\r') end++;
    if (*end == '\n') end++;
    if (chunk_size == 0) break;
    /* Safety: don't read or write past the valid data. */
    if (end + (ptrdiff_t)chunk_size > end_buf) break;
    memmove(out, end, chunk_size);
    out += chunk_size;
    p = end + chunk_size;
    if (p < end_buf && *p == '\r') p++;
    if (p < end_buf && *p == '\n') p++;
  }
  *out = '\0';
  return body;
}

/* ========================================================================== */
/*                         BUG-COVERAGE TESTS                                 */
/* ========================================================================== */

TEST(chttpserver, req_read_on_buffered_returns_minus_one) {
  /* chttpsvr_req_read() returns -1 on buffered (non-streaming) routes,
     because req->is_streaming is false and a body that was buffered up front
     has no stream position. */
  chttpcli_response *resp =
      _post("/req-read-buffered", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "-1");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_on_streaming_route) {
  /* chttpsvr_req_body() works only for a buffered route. The server never pulls
     out the body of a streaming route ahead of time, it reads that body live
     off the socket with chttpsvr_req_read(), one batch at a time. So calling
     chttpsvr_req_body() on such a route must return NULL and 0 instead of
     handing back a buffered copy. */
  const char *payload = "body-via-api";
  chttpcli_response *resp = _post("/stream-body-api", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_after_req_read_on_streaming_route) {
  /* This is the gap that the is_streaming check of chttpsvr_req_body() closes.
     Without that check the function returns conn->body.buf and conn->body.len
     as they are, and for a streaming route conn->body is a live cursor that
     chttpsvr_req_read() drains, which the server compacts back to empty only
     when the NEXT batch arrives, not as soon as the cursor is empty. The test
     above calls chttpsvr_req_body() only BEFORE any chttpsvr_req_read() call,
     so it passes by luck, because conn->body is still all zero at that point,
     and not because of a real rule for streaming routes. This test calls
     chttpsvr_req_read() first, so a regression that drops the guard shows up
     here as a non-NULL body and a non-zero len (whatever chttpsvr_req_read
     already delivered and left in the buffer) instead of the documented NULL
     and 0. */
  const char *payload = "12345678-more-than-eight-bytes";
  chttpcli_response *resp =
      _post("/stream-body-after-read", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  /* chttpsvr_req_read() runs with an 8-byte buffer against a 31-byte payload,
     so it has to read something real and give n > 0. That confirms the test
     covers the case after a real read and not a request without a body. */
  REQUIRE_TRUE(strncmp(resp->body, "read=0 ", 7) != 0);
  REQUIRE_TRUE(strstr(resp->body, "body=null len=0") != NULL);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_repeated_header) {
  /* A client can send the same header name twice, and _on_header then adds both
     to conn->hdr_names and conn->hdr_values in the order they arrive (see the
     doc comment of that struct). chttpsvr_req_header must scan backwards and
     return the LAST one. The chttpclient abstraction overwrites a duplicate
     header name, so this test uses a raw socket to send the two literal lines.
 */
  char buf[4096] = {0};
  int status = _raw_request("GET", "/stream-header",
                            "x-stream-test: first\r\nx-stream-test: second\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "second");
}

/* ========================================================================== */
/*                    WORKER-DRIVEN INGESTION TESTS                           */
/* ========================================================================== */

TEST(chttpserver, streaming_body_delivered_in_separate_batches) {
  /* A client writes its body in several chunks with a delay between them, so
     the streaming handler's chttpsvr_req_read() sees more than one batch.
     That shows the worker thread reads the body live off the socket as it
     arrives, and that the server doesn't buffer the whole body before the
     handler starts. */
  char buf[4096] = {0};
  int status = _raw_request_drip_body(TEST_PORT, "POST", "/stream-batch-count",
                                      "aaaabbbbccccddddeeee", 4, 20000, buf,
                                      sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *hdr_end = strstr(buf, "\r\n\r\n");
  REQUIRE_TRUE(hdr_end != NULL);
  char saved = hdr_end[0];
  hdr_end[0] = '\0';
  char *count_hdr = strstr(buf, "x-batch-count:");
  REQUIRE_TRUE(count_hdr != NULL);
  int batches = atoi(count_hdr + 14);
  hdr_end[0] = saved;
  /* 20 bytes written 4 at a time with a delay between writes need to arrive as
     several separate reads on the server side, not one. */
  REQUIRE_GT(batches, 1);
}

TEST(chttpserver, unmatched_route_rejected_without_reading_body) {
  /* The server routes a request as soon as the headers are complete, before it
     reads any body byte, so it has to reject an unmatched route up front even
     though the client declares a huge body and never sends it. A server that
     read the body before responding would make this test hang instead of
     returning quickly. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();

  char buf[4096] = {0};
  int status = _raw_request("POST", "/no-such-route-at-all",
                            "Content-Length: 100000000\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 404);

  /* reject_pool handles this rejection too, and its counter goes up strictly
     after the response is on the wire (see the comment of
     _wait_for_reject_pool_task_count). Waiting for that counter here keeps the
     late increment out of the test tau runs next; without the wait that next
     test would see one more than it expects, because this test returned before
     the asynchronous increment landed. */
  _wait_for_reject_pool_task_count(reject_count_before + 1);
}

TEST(chttpserver, unmatched_route_rejection_routed_through_reject_pool) {
  /* reject_pool handles every rejection, not only the 503 for a full pool (see
     the assertion on that case in bounded_pool_full_returns_503), so a slowly
     reading client that gets a bad-route message can't stall the single reactor
     thread. A black-box client can't tell a reject_pool answer from one written
     by the synchronous last-resort fallback, because both put the same 404 on
     the wire, so this test checks the mechanism directly with the white-box
     counter g_reject_task_run_count_for_tests. */
  size_t before = _chttpsvr_reject_pool_task_count_for_tests();

  chttpcli_response *resp = _get("/definitely-not-a-registered-route-xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);

  REQUIRE_EQ(_wait_for_reject_pool_task_count(before + 1), before + 1);
}

TEST(chttpserver, concurrent_route_rejections_do_not_starve_other_requests) {
  /* A rejection for an unmatched route is a 404, a 405 or a 500. Running it
     synchronously on chttpserver's single reactor thread would be a hazard,
     because a burst of them would delay the dispatch of every other connection
     waiting behind them on that thread, and since g_srv and g_bounded_srv share
     one process-wide reactor the concern is real. So every rejection goes
     through reject_pool, a small dedicated pool sized from worker_thread_count
     (see the comment of that pool in chttpserver.c), and a burst of concurrent
     rejections must not delay an ordinary request the server handles at the
     same time. A courtesy rejection response is small, so unlike the large body
     of response_write_timeout_closes_slow_reader_connection it can't reliably
     fill the socket buffer and force a write to block, and this test can't
     prove the non-blocking property as tightly as that one does. It's a smoke
     check against gross starvation of the reactor thread, and it also exercises
     reject_pool's multi-threaded submit and dispatch path under real
     concurrency, which the single 503 of bounded_pool_full_returns_503 never
     does because it never contends with anything. */
  /* Every outcome below goes into a local instead of being asserted inline,
   * and the test joins every background thread that started UNCONDITIONALLY
   * before any REQUIRE_* runs. Each of those threads does a full HTTP round
   * trip against g_srv, which lives as long as the process, so a REQUIRE_* that
   * returned early while one of them is still in flight would leave it unjoined
   * for the rest of the binary's life. That thread would keep incrementing the
   * process's reject-pool task counter, which several OTHER tests of this file
   * need for exact-delta assertions. */
  enum { N_REJECTIONS = 40 };
  pthread_t threads[N_REJECTIONS];
  int created = 0;
  ccol_retval_t create_rv = ccol_success;
  for (; created < N_REJECTIONS; created++) {
    int rc = pthread_create(&threads[created], NULL, _send_unmatched_route_req,
                            NULL);
    if (rc != 0) {
      create_rv = ccol_unexpected_failure;
      break;
    }
  }

  /* Time one ordinary request on the same server while the burst above is in
     flight. The bound is generous, since this is a starvation smoke test and
     not a tight latency proof. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpcli_response *resp = _get("/hello");
  clock_gettime(CLOCK_MONOTONIC, &t1);
  int status_code = resp ? resp->status_code : -1;
  if (resp) chttpclient_resp_free(resp);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);

  REQUIRE_EQ((int)create_rv, (int)ccol_success);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_LT(elapsed_ms, 2000L);
}

TEST(chttpserver, pipelined_bytes_after_rejected_route_not_misparsed) {
  /* _on_headers_complete can reject a route and set conn->req_rejected, as the
     404 case here does. The CHTTP1_USER branch of _conn_pump then always calls
     _conn_close, unconditionally, instead of keeping the connection alive to
     read a next request (that branch's comment explains why a rejected route
     gets an error response and still closes). This test locks the guarantee in
     end to end. The bytes of a second, pipelined request may already sit in the
     same read buffer right behind the rejected one, and the server must never
     read them as the body of that first, finished request, or as a second
     request on the same connection. Exactly one response comes back and the
     connection closes cleanly, with no second, bogus response and no hang. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* One write() call sends both requests together, so they arrive together in
     the same read buffer on the server side. The first request hits an
     unmatched route, which the server rejects and never diverts, and the second
     hits a matched route; a wrong parse would swallow the second request as the
     "body" of the first. */
  const char *req =
      "GET /no-such-route-at-all HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "\r\n"
      "GET /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* Exactly one response arrives, the 404 for the rejected first request, and
     then the server closes the connection. The server must never use the
     second request's bytes as the body of this connection, or as the next
     message. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_across_two_requests_on_one_connection) {
  /* The test sends two matched requests one after the other on the same
     connection, without Connection: close, and both should succeed: the server
     pauses when the headers are complete, not after the full body, and that
     pause must not break ordinary keep-alive or pipelining. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  char buf[1024];

  for (int i = 0; i < 2; i++) {
    REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
    memset(buf, 0, sizeof(buf));
    int status = _read_one_http_response(fd, buf, sizeof(buf));
    REQUIRE_EQ(status, 200);
    REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  }
  close(fd);
  fd = -1;
}

TEST(chttpserver, pipelined_bodyless_requests_both_answered) {
  /* A GET here is a request without a body: it finishes its own framing right
     after the headers, with no Content-Length and no chunked Transfer-Encoding.
     Such a request never runs the chttp1_stream_read loop of _drain_body at
     all, because chttp1_parser_message_complete() is already true when the
     worker starts, so the leftover bytes that chttp1_stream_prepare() gets have
     to be reclaimed explicitly. Here those bytes are the ENTIRE second,
     pipelined request of the connection, already off the wire and gone for good
     from the kernel's socket buffer. Left in the stream's carry-over, they'd be
     thrown away by chttp1_stream_release() the moment it runs: the connection
     correctly stays alive, but the second request's bytes are lost forever and
     the client hangs waiting for a response that never comes. One write() call
     sends both requests, so they land together in the reactor's read buffer on
     the server side, which is exactly the shape that exercises this path. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "\r\n"
      "GET /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Connection: close\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* Exactly two full responses arrive, both a 200 with the handler's body, and
     the server closes the connection right after the second one because of
     Connection: close. The client must not have to time out waiting for a
     second response that never comes. */
  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

TEST(chttpserver, pipelined_bytes_after_buffered_body_request_not_lost) {
  /* The same contract as pipelined_bodyless_requests_both_answered, for its
     OTHER half. This request DOES have a body, so the chttp1_stream_read and
     chttp1_parser_execute loop of _drain_body really runs, and that loop can
     lose a pipelined next request when its own read pulls in bytes past this
     message's body boundary. chttp1_parser_execute reports through
     chttp1_parser_consumed() how many of the bytes it got it consumed, and the
     rest at the end is captured. Here that rest is the raw bytes of the
     second request, which the same chttp1_stream_read call that returned the
     first request's 5-byte body swept up, and the server must not throw them
     away together with the rest of that read buffer. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *body = "howdy";
  char req[512];
  int n = snprintf(req, sizeof(req),
                   "POST /echo-body HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "Content-Length: %zu\r\n"
                   "\r\n"
                   "%s"
                   "GET /hello HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   strlen(body), body);
  REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(req));
  REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(buf, body) != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

TEST(chttpserver, pipelined_bytes_after_streaming_body_request_not_lost) {
  /* The same test for a streaming route. chttpsvr_req_read() drives the same
     shape of chttp1_stream_read and chttp1_parser_execute loop that _drain_body
     uses for a buffered route, so it needs, and gets, the same treatment: push
     the bytes back, then reclaim them. Without it a pipelined next request that
     arrives in the same read as the last bytes of a streamed body would be lost
     in exactly the same way. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *body = "streamed-and-pipelined";
  char req[512];
  int n = snprintf(req, sizeof(req),
                   "POST /stream-echo HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "Content-Length: %zu\r\n"
                   "\r\n"
                   "%s"
                   "GET /hello HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "Connection: close\r\n"
                   "\r\n",
                   strlen(body), body);
  REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(req));
  REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(buf, body) != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

TEST(chttpserver, five_pipelined_bodyless_requests_all_answered) {
  /* Each pipelining test above (a rejected route, two requests without a body,
     a buffered body and a streamed body) puts exactly TWO requests into one
     write(), so it drives _conn_start_diverted and _conn_feed_bytes ONCE per
     connection, from the reactor thread only. This test puts the bytes of a
     THIRD request behind the second one, all delivered by one read, so the
     keep-alive tail of _task_worker has to feed those bytes into
     _conn_feed_bytes a SECOND time (see the comment of that tail on
     chttp1_stream_take_leftover), on the worker thread instead of the reactor
     thread. When the headers of that second request complete right there, as
     they do here, _conn_start_diverted runs a second time too, recursively from
     inside _task_worker itself, and submits to the same worker pool again for
     the bytes of a fourth request, and so on. A two-request test never gets to
     that recursive hand-off, which is why this test exists: a chain of
     pipelined requests of any length, all delivered in one socket read, gets a
     full answer, and the server drops, corrupts or duplicates none of them,
     however many times the hand-off needs to happen. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *keepalive_req = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  const char *final_req =
      "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";

#define _FIVE_PIPELINED_COUNT 5
  char req[4096] = {0};
  size_t off = 0;
  for (int i = 0; i < _FIVE_PIPELINED_COUNT - 1; i++) {
    size_t l = strlen(keepalive_req);
    memcpy(req + off, keepalive_req, l);
    off += l;
  }
  size_t fl = strlen(final_req);
  memcpy(req + off, final_req, fl);
  off += fl;
  REQUIRE_TRUE(off < sizeof(req));
  REQUIRE_EQ(write(fd, req, off), (ssize_t)off);

  char buf[16384] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* Exactly _FIVE_PIPELINED_COUNT complete responses arrive, each with the
     right body. Fewer means a dropped request and more means a duplicate or a
     corrupt parse. The server closes the connection after the last one. */
  int count = 0;
  const char *p = buf;
  while ((p = strstr(p, "HTTP/1.1 200")) != NULL) {
    count++;
    p += 12;
  }
  REQUIRE_EQ(count, _FIVE_PIPELINED_COUNT);

  size_t hello_count = 0;
  const char *hp = buf;
  while ((hp = strstr(hp, "Hello, world!")) != NULL) {
    hello_count++;
    hp += 13;
  }
  REQUIRE_EQ(hello_count, (size_t)_FIVE_PIPELINED_COUNT);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
#undef _FIVE_PIPELINED_COUNT
}

TEST(chttpserver, pipelined_malformed_request_after_worker_processed_request) {
  /* Guards against a use-after-free in the keep-alive tail of _task_worker. A
     worker thread can feed the bytes of a further pipelined request into
     _conn_feed_bytes, so the reactor thread isn't its only caller (see the doc
     comment of _conn_feed_bytes and the comment of this code path in
     _task_worker). _conn_feed_bytes can decide conn's fate before it returns
     and can free conn outright, and its own contract says the caller must not
     touch conn again. A follow-up request with bad syntax, such as the negative
     Content-Length case of negative_content_length_rejected above, makes
     chttp1_parser_execute return CHTTP1_ERROR and takes the synchronous
     _conn_close branch of _conn_feed_bytes on the same thread, which frees conn
     every single time before _conn_feed_bytes returns. The cross-thread divert
     case frees it only sometimes, but this one is deterministic, so reading
     conn->m_procs right after the call (to free a local scratch buffer, for
     example) would always be a use-after-free on this path. One write() sends
     both requests. The reactor diverts the first, matched, kept-alive request
     to a worker thread from its own first read, and the bytes of the second,
     malformed request come along as leftover that reaches _conn_feed_bytes only
     when the worker thread's tail runs. The reactor thread never touches the
     second request's bytes at all. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "\r\n"
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: -1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* Two responses arrive in order, the 200 for the well-formed request and then
     the 400 for the malformed one, matching the documented behaviour of
     negative_content_length_rejected. Then the connection closes, and
     the server process must not crash. */
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strncmp(first, "HTTP/1.1 200", 12) == 0);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  char *second = strstr(first + 8, "HTTP/1.1");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strncmp(second, "HTTP/1.1 400", 12) == 0);
  /* No third response, the connection closed after the rejection. */
  REQUIRE_TRUE(strstr(second + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_two_consecutive_streaming_requests) {
  /* _conn_reset_for_request() resets each message's ingestion state
     (conn->body, body_too_large, transfer_aborted, deadline_exceeded,
     _carry_over and conn->parser itself) between all the requests of a
     keep-alive connection, which it does because one chttpsvr_conn_t
     serves the whole life of the connection. Every other keep-alive test in
     this file pairs at most one streaming request with a buffered GET on the
     same connection, while this test sends two full streaming POSTs back to
     back on one connection, the case most likely to show stale streaming state
     from an earlier message, like a leftover body buffer or an error flag of
     request 1 that corrupts the ingestion of request 2. Each request has a
     different body, so both responses can only match if the ingestion state was
     really reset in between. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *bodies[2] = {"first-streamed-body",
                           "second-streamed-body-is-longer-than-the-first"};
  for (int i = 0; i < 2; i++) {
    size_t body_len = strlen(bodies[i]);
    char hdr[256];
    int hn = snprintf(hdr, sizeof(hdr),
                      "POST /stream-echo HTTP/1.1\r\n"
                      "Host: 127.0.0.1\r\n"
                      "Content-Type: text/plain\r\n"
                      "Content-Length: %zu\r\n"
                      "\r\n",
                      body_len);
    REQUIRE_TRUE(hn > 0 && (size_t)hn < sizeof(hdr));
    REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);
    REQUIRE_EQ(write(fd, bodies[i], body_len), (ssize_t)body_len);

    char buf[1024];
    memset(buf, 0, sizeof(buf));
    int status = _read_one_http_response(fd, buf, sizeof(buf));
    REQUIRE_EQ(status, 200);
    char *body_out = _decode_raw_body(buf);
    REQUIRE_TRUE(body_out != NULL);
    REQUIRE_STREQ(body_out, bodies[i]);
  }

  close(fd);
  fd = -1;
}

TEST(chttpserver,
     streaming_handler_early_stop_of_fully_arrived_body_stays_keepalive) {
  /* /stream-read-once reads only the first 8 bytes of the body and returns
     without calling chttpsvr_req_read() again. Here one write sends the whole
     64-byte body, which is on the wire before the handler's single read call
     runs, so chttpsvr_req_read's internal pass (chttp1_stream_read plus
     chttp1_parser_execute) parses the ENTIRE declared body into conn->body in
     that one internal call (see the loop of that function in chttpserver.c),
     however few bytes the handler's buffer got. The server consumes the whole
     Content-Length, so chttp1_parser_message_complete(&conn->parser) is already
     true before the handler returns. _task_worker forces Connection: close only
     when that check is still false at the end of the request, which the paired
     ..._with_undrained_body_forces_close test below covers. Here it's true, so
     the connection must stay usable for a second, separate request. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Declare and send a 64-byte body, of which the handler reads only the first
     8. */
  char body[64];
  memset(body, 'x', sizeof(body));
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-read-once HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    sizeof(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);
  REQUIRE_EQ(write(fd, body, sizeof(body)), (ssize_t)sizeof(body));

  char buf[1024];
  memset(buf, 0, sizeof(buf));
  int status1 = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status1, 200);
  REQUIRE_TRUE(strstr(buf, "early-stop") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") == NULL);

  /* A second, unrelated request on the same connection. */
  const char *req2 = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req2, strlen(req2)), (ssize_t)strlen(req2));
  memset(buf, 0, sizeof(buf));
  int status2 = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status2, 200);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);

  close(fd);
  fd = -1;
}

TEST(chttpserver,
     streaming_handler_early_stop_with_undrained_body_forces_close) {
  /* In the case above the body arrives in full. Here the client sends only the
     first 8 bytes of a declared 64-byte body, exactly what the single
     chttpsvr_req_read(req, buf, 8) call of /stream-read-once consumes. The
     content_length of 64 is more than the 8 bytes read when the handler
     returns, so the body really is undrained, and _task_finish's safety net has
     to force Connection: close, since it keeps the connection alive only when
     chttp1_parser_message_complete reports the whole message. Without that,
     reusing the connection would let the server read the 56 bytes that never
     came as the start of a new pipelined request. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-read-once HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: 64\r\n"
                    "\r\n");
  REQUIRE_TRUE(hn > 0 && (size_t)hn < sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  char partial_body[8];
  memset(partial_body, 'x', sizeof(partial_body));
  REQUIRE_EQ(write(fd, partial_body, sizeof(partial_body)),
             (ssize_t)sizeof(partial_body));

  char buf[1024] = {0};
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "early-stop") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);

  close(fd);
  fd = -1;
}

TEST(chttpserver, stream_read_timeout_reports_ccol_timed_out) {
  /* A client sends headers that declare more body than it ever delivers, and
     then stalls. chttpsvr_req_read() then must return -1 and
     chttpsvr_req_stream_error() must give ccol_timed_out after a wait bounded
     by stream_read_timeout_us (300ms for this server, set in this test's
     setup), instead of blocking the worker thread forever. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 2);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *hdr =
      "POST /stream-error-report-slow HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n"
      "only-ten-"; /* 9 of the declared 100 bytes, and never any more */
  REQUIRE_EQ(write(fd, hdr, strlen(hdr)), (ssize_t)strlen(hdr));

  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);
}

TEST(chttpserver, client_disconnect_mid_body_does_not_hang_server) {
  /* A client sends part of its body and then disconnects completely, which must
     neither hang the worker thread nor leak the connection's resources. There's
     no response to check since the client tore the connection down, so the test
     shows that the one worker that handled the aborted request got back to the
     pool.

     The test uses a dedicated local server with one worker, and not g_srv,
     which has 4 worker threads, or a share of g_bounded_srv, which has one
     worker and serves the bounded-503 test elsewhere in this file. With g_srv's
     4 workers a follow-up request would succeed quickly through one of the
     other 3 even if the bug this test catches came back and the aborted
     connection's worker wedged forever, so such a check would pass for nothing.
     g_bounded_srv has a queue_capacity of 1, and that OTHER test fills it
     exactly with two concurrent blocking requests, which leaves no room for
     this test's follow-up to race the server's abort cleanup, so an occasional
     bogus 503 shows up. That effect is real under valgrind, where concurrency
     is heavily serialized and a worker may wake up long after a queue-capacity
     check has run. A generous queue_capacity here removes that race completely,
     since this test only ever uses two connections, and it keeps the one
     property that makes the test meaningful: with a single worker thread, the
     follow-up below can only succeed if that same worker recovered from the
     abrupt disconnect and went back to the pool. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t reg_rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/disconnect-mid-body", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 30;
  cfg.worker_thread_count = 1;
  cfg.worker_queue_capacity = 4;
  /* A short value, so the test only waits a short time for this fallback path
     to notice the abrupt disconnect, which matters if the fast, EOF-driven
     detection is ever late. */
  cfg.stream_read_timeout_us = 300000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int rc = _raw_request_abort_mid_body(TEST_PORT + 30, "/disconnect-mid-body",
                                       "partial", 1000);
  REQUIRE_EQ(rc, 0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 30);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* This read has a bound. If the worker really fails to recover, the read
     times out and the REQUIRE below fails cleanly instead of hanging the whole
     test binary. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "ok";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /disconnect-mid-body HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n"
                    "%s",
                    strlen(body), body);
  REQUIRE_TRUE(hn > 0 && (size_t)hn < sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  char buf[1024] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, truncated_body_reports_ccol_http_transfer_aborted) {
  /* The documentation of chttpsvr_req_stream_error() says it returns
     ccol_http_transfer_aborted for a closed connection or bad framing in the
     middle of a body, so chttpsvr_req_read()'s exit paths for a truncated body
     and for a hard I/O error have to record that on the connection. Quietly
     falling through to ccol_success would tell a correct streaming handler that
     a truncated upload was a clean read: for example a POST declares
     Content-Length: 100, the client sends 20 bytes and half-closes,
     chttpsvr_req_read() returns -1, chttpsvr_req_stream_error() reports
     ccol_success and the server replies 200 OK. The test
     client_disconnect_mid_body_does_not_hang_server above does a full close, so
     there's no response left to read back, while this test only half-closes the
     write side and can look at the response directly. */
  char buf[4096] = {0};
  int rc = _raw_request_truncate_body_half_close(
      "/stream-error-report", "only-part-of-the-declared-body", 1000, buf,
      sizeof(buf));
  REQUIRE_EQ(rc, 0);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_http_transfer_aborted") != NULL);
}

TEST(chttpserver, expect_100_continue_interim_response_sent_before_body) {
  /* The interim "100 Continue" write for conn->expects_continue happens on the
     worker thread, in _task_worker, right before the body drain starts, and not
     on the reactor thread in _conn_pump when the headers finish, so a slowly
     reading client can't stall the server's single reactor thread during this
     write. This test pins the wire behaviour that this placement must keep:
     the interim response arrives before the client sends its body, and the real
     final response follows once the client has sent the body. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "expect-continue-body";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-error-report HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  /* Read the interim "100 Continue" response. It must arrive before the test
     sends any of the body, which shows that the body isn't needed to unblock
     it. */
  char interim[128] = {0};
  size_t got = 0;
  while (got < sizeof(interim) - 1 && strstr(interim, "\r\n\r\n") == NULL) {
    ssize_t r = read(fd, interim + got, sizeof(interim) - 1 - got);
    REQUIRE_GT(r, 0);
    got += (size_t)r;
  }
  REQUIRE_TRUE(strstr(interim, "HTTP/1.1 100 Continue") != NULL);

  /* Now send the body and check that the real final response still follows
     correctly on the same connection. */
  REQUIRE_EQ(write(fd, body, strlen(body)), (ssize_t)strlen(body));
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  close(fd);
  fd = -1;
}

/* A streaming route's handler can reject a request without ever calling
   chttpsvr_req_read(), and it then gives the real final response
   directly, with NO interim "100 Continue" reaching the client first. A
   _task_worker that sent "100 Continue" unconditionally before the handler
   runs, for buffered and streaming routes alike, would break this: it would
   tell the client to upload a body the server was never going to read, which
   defeats the whole point of Expect: 100-continue. RFC 7231 SS5.1.1 lets the
   server answer with a final status instead of "100 Continue", and the client
   then skips the upload. For a streaming route the interim send waits for
   chttpsvr_req_read() itself (see that function's comment), which is what keeps
   it from going out until the handler asks to read. */
TEST(
    chttpserver,
    expect_100_continue_not_sent_when_streaming_handler_rejects_without_reading) {
  REQUIRE_EQ((int)chttpsvr_register_streaming_handler(
                 g_srv, CHTTP_POST, "/stream-reject-no-read",
                 _stream_reject_without_reading_handler, NULL),
             (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *body = "this-body-must-never-be-read-by-the-server";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-reject-no-read HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  /* On purpose this test never sends `body`. A correct Expect: 100-continue
     client waits for a "100 Continue" or a final response before uploading, and
     this test shows that the server never asks it to upload. */
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 401);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "no thanks") != NULL);
  close(fd);
  fd = -1;
}

/* A request can carry "Expect: 100-continue" and have no body at all, with no
   Content-Length and no chunked Transfer-Encoding, and such a request must
   never get the interim "100 Continue" response, not even on a STREAMING route
   whose handler does call chttpsvr_req_read(). The lazy send inside
   chttpsvr_req_read() must not fire unconditionally on the first call: it has
   to check whether chttp1_parser_message_complete() is already true, that is,
   whether there was ever a body to invite. Sending it here would tell the
   client to upload a body that was never coming. */
TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_streaming_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* No Content-Length and no Transfer-Encoding: under the request-specific
     framing rule of RFC 7230 SS3.3 this is an ordinary, valid POST without a
     body, not a truncated or malformed one. */
  const char *req =
      "POST /stream-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Expect: 100-continue\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  close(fd);
  fd = -1;
}

/* The same contract for a BUFFERED route. _task_worker sends the interim
   response early, before _drain_body, for a route registered with
   chttpsvr_register_handler (not the streaming variant), and that send has to
   carry exactly the same condition. */
TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_buffered_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /echo-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Expect: 100-continue\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "(empty)") != NULL);
  close(fd);
  fd = -1;
}

/* The interim line "HTTP/1.1 100 Continue\r\n\r\n" is 25 bytes, and a real
   short write of it leaves a truncated status line on the wire that no parser
   can read. Such a short write happens when a slowly reading peer stalls the
   write halfway through, which is exactly what max_response_write_duration_us
   defends against. So _write_interim_continue reports whether it wrote the
   complete line, and both call sites (_task_worker, which this test uses for
   the buffered route, and chttpsvr_req_read, which the streaming-route test
   right below uses) treat a false return as fatal for the connection: they skip
   sending the real response altogether and close, instead of putting more bytes
   on top of a possibly corrupt stream. A handler that runs and sends a
   complete, valid final response right after the truncated prefix would give a
   byte stream like "HTTP/1.1 100 ConHTTP/1.1 200 OK\r\n...", which no HTTP/1.1
   client that follows the standard can parse.

   _chttpsvr_force_short_interim_write_for_tests() reproduces the short write
   every time by making a real partial write to the real socket instead of
   faking one, so the test doesn't depend on the OS's socket buffering to
   produce a real partial write of such a small message. */
TEST(chttpserver, interim_continue_short_write_buffered_route_forces_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "will-never-be-drained";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /echo-body HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));

  _chttpsvr_force_short_interim_write_for_tests(10); /* < 25: a real short
                                                          write */
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  /* Exactly the truncated 10-byte prefix must arrive, matching what the
     forced short write really put on the wire, maybe split over several read(2)
     calls. */
  char got[64] = {0};
  size_t got_n = 0;
  while (got_n < 10) {
    ssize_t r = read(fd, got + got_n, sizeof(got) - 1 - got_n);
    if (r <= 0) break;
    got_n += (size_t)r;
  }
  REQUIRE_EQ(got_n, (size_t)10);
  REQUIRE_EQ(memcmp(got, "HTTP/1.1 1", 10), 0);

  /* No more bytes are ever sent (in particular no real final response glued
     onto the truncated line), the connection is simply closed. */
  char extra[64];
  ssize_t r2 = read(fd, extra, sizeof(extra));
  REQUIRE_EQ(r2, (ssize_t)0);

  close(fd);
  fd = -1;
}

/* The streaming-route version of the test above. It produces the same
   short-write corruption, but gets there through chttpsvr_req_read's lazy
   interim send instead of _task_worker's eager one. It also checks that
   chttpsvr_req_read itself returns -1 instead of trying to read a
   body over a connection whose response channel is corrupt. The test uses the
   existing _stream_error_report_handler, whose "done" and x-stream-err response
   must never reach the wire either. */
TEST(chttpserver, interim_continue_short_write_streaming_route_forces_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "will-never-be-drained";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-error-report HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));

  _chttpsvr_force_short_interim_write_for_tests(10);
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  char got[64] = {0};
  size_t got_n = 0;
  while (got_n < 10) {
    ssize_t r = read(fd, got + got_n, sizeof(got) - 1 - got_n);
    if (r <= 0) break;
    got_n += (size_t)r;
  }
  REQUIRE_EQ(got_n, (size_t)10);
  REQUIRE_EQ(memcmp(got, "HTTP/1.1 1", 10), 0);

  char extra[64];
  ssize_t r2 = read(fd, extra, sizeof(extra));
  REQUIRE_EQ(r2, (ssize_t)0);

  close(fd);
  fd = -1;
}

TEST(chttpserver,
     max_response_write_duration_bounds_interim_continue_response) {
  /* _write_interim_continue writes the RFC 7231 SS5.1.1 "100 Continue" interim
     line that the server sends for an ordinary "Expect: 100-continue" request,
     which is standard client behaviour (curl sends one by default for a large
     upload). That write must NOT go through conn->write_deadline with a bare
     _shrink_timeout_to_deadline call. It needs the same
     _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS ceiling that the rejection-response
     path of _send_response carries, applied unconditionally, because a small
     internal write of a fixed shape must never stay completely unbounded, which
     is what happens when the operator leaves max_response_write_duration_us at
     its default of 0, which turns it off. Without the ceiling a peer could send
     an ordinary Expect: 100-continue request and then read the interim line one
     byte at a time, holding a worker pool thread forever with the configuration
     this project ships by default, and a handful of such connections would use
     up the whole pool. It's the same Slowloris-type denial of service that the
     same ceiling closes for the rejection response, which is an internal write
     as well. The test uses g_srv directly, which never sets
     max_response_write_duration_us and so keeps the real shipped default of 0,
     so it shows that the ceiling really applies unconditionally instead of just
     repeating a cap the test set up itself. */

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Bounds the read below. A regression could stop the server from asking the
     hook at all on this path, but then the server sends an ordinary "100
     Continue" interim line and the read below still returns quickly with those
     bytes instead of hanging. So the timeout is only an extra defence, no
     likely regression here hangs the read. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  _chttpsvr_force_next_response_write_deadline_expired_for_tests();

  const char *body = "will-never-be-sent";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stream-error-report HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Expect: 100-continue\r\n"
                    "Content-Length: %zu\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  char buf[64];
  ssize_t r = read(fd, buf, sizeof(buf));

  /* The interim continue write loop checks the write deadline once before any
     real write(2) call, and the forced hook makes that check say "already
     expired", so the interim write returns false and sends zero bytes. The
     documented contract of _write_interim_continue says that any false return,
     not only a short write, makes the caller skip sending the real response and
     close the connection, so the client sees an EOF (read returns 0) and gets
     nothing at all. A build that never asks the hook on this path sends a
     complete, ordinary "100 Continue" line, which shows up here as a byte count
     above zero. */
  REQUIRE_EQ((int)r, 0);
}

TEST(chttpserver, stream_read_timeout_ms_zero_means_wait_indefinitely) {
  /* The documentation of chttpsvr_config_t.stream_read_timeout_us says 0 means
     "wait indefinitely", so every call site in chttpserver.c has to translate
     that value into the int timeout_ms parameter of chttp1_stream_read and
     chttp1_stream_write, which follows the poll(2) convention: a negative value
     blocks forever and 0 makes one non-blocking try. A plain cast skips the
     translation, and a configured 0 then means the opposite, giving up at once
     on every call without even trying. That has two symptoms. A streaming
     handler's chttpsvr_req_read() returns -1 at about t=0 however long the
     client takes to send the body, and a GET without a body gets no response at
     all, because response_write_timeout_us has its own default where 0 means
     "use the value of stream_read_timeout_us", so it inherits the same 0 and
     every response write fails immediately too. This test covers both cases: a
     client delays sending its body well past any accidental "instant" failure,
     the request must still succeed once the bytes arrive, and the server also
     delivers the response. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/zero-timeout-wait",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 17;
  cfg.stream_read_timeout_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 17));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  /* Bounds the final loop that reads the response below. A regression could
     make the server hang forever without responding, and since the
     configuration under test is "wait indefinitely", a real server bug has no
     other bound to catch it. Without this bound that read(2) would block
     forever and hang the whole test binary instead of this one assertion
     failing cleanly. 10s is generous next to the 300ms delay this test adds
     below. */
  struct timeval rcvtimeo = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *body = "delayed-body";
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /zero-timeout-wait HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    strlen(body));
  REQUIRE_TRUE(hn > 0 && (size_t)hn < (int)sizeof(hdr));
  REQUIRE_EQ(write(fd, hdr, (size_t)hn), (ssize_t)hn);

  /* This delay before sending the body is long, well past any accidental
     "instant" failure, and a correct "wait indefinitely" implementation still
     needs to take these bytes once they arrive. */
  struct timespec delay = {0, 300000000L}; /* 300ms */
  nanosleep(&delay, NULL);
  REQUIRE_EQ(write(fd, body, strlen(body)), (ssize_t)strlen(body));

  char buf[4096] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                         CONCURRENT STREAMING TEST                          */
/* ========================================================================== */

#define N_CONCURRENT_STREAMS 8

typedef struct {
  int id;
  int ok;
} conc_stream_arg_t;

static void *_conc_stream_thread(void *arg) {
  conc_stream_arg_t *a = (conc_stream_arg_t *)arg;
  char payload[64];
  snprintf(payload, sizeof(payload), "concurrent-%d", a->id);
  chttpcli_response *resp = _post("/stream-echo", payload, "text/plain");
  a->ok = (resp != NULL && resp->status_code == 200 && resp->body != NULL &&
           strcmp(resp->body, payload) == 0);
  /* The global middleware has to add x-global-mw to every streaming response.
   */
  if (a->ok && resp) {
    const char *gv = chttpclient_resp_header(resp, "x-global-mw");
    a->ok = (gv != NULL && strcmp(gv, "1") == 0);
  }
  if (resp) chttpclient_resp_free(resp);
  return NULL;
}

TEST(chttpserver, concurrent_streaming) {
  /* Fire N_CONCURRENT_STREAMS streaming POSTs at the same time and check that
     each response echoes its own distinct payload, with nothing mixed up
     between them.
   */
  pthread_t threads[N_CONCURRENT_STREAMS];
  conc_stream_arg_t args[N_CONCURRENT_STREAMS];
  int created = 0;
  for (int i = 0; i < N_CONCURRENT_STREAMS; i++) {
    args[i].id = i;
    args[i].ok = 0;
    if (pthread_create(&threads[i], NULL, _conc_stream_thread, &args[i]) != 0)
      break;
    created++;
  }
  /* Every thread that started is joined before any REQUIRE_* below can
   * return early. A Tau REQUIRE_* returns from the test function at once on a
   * failure, and args[] and threads[] live on the stack, so an unjoined thread
   * still running _conc_stream_thread would keep writing into its own a->ok
   * after this function's stack frame is gone and the next test is reusing it.
   * That's a real stack-use-after-return that can silently corrupt a later,
   * unrelated test, not just a leaked thread. So we join first and assert in a
   * separate loop afterwards, and every thread has finished before any
   * REQUIRE_* in this function can return. */
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(created, N_CONCURRENT_STREAMS);
  for (int i = 0; i < created; i++) REQUIRE_TRUE(args[i].ok);
}

/* ========================================================================== */
/*                       SUBROUTER TRAILING-SLASH TEST                        */
/* ========================================================================== */

TEST(chttpserver, subrouter_trailing_slash_prefix) {
  /* A sub-router can have a slash at the end of its prefix, for example
     "/api/v3/", and it should behave exactly like one without it, "/api/v3":
     the library strips that slash from the stored prefix, so a request to
     /api/v3/ping must route correctly. */
  chttpcli_response *resp = _get("/api/v3/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     BUFFERED REPEATED-HEADER TEST                          */
/* ========================================================================== */

TEST(chttpserver, buffered_repeated_header) {
  /* chttpsvr_req_header reads the same conn->hdr_names and conn->hdr_values
     arrays for every kind of route. The streaming_repeated_header test above
     covers the same "last occurrence wins" behaviour through a streaming route,
     and this one goes through a buffered route (the /header route). chttpclient
     overwrites a duplicate name, so this test uses a raw socket too. */
  char buf[4096] = {0};
  int status = _raw_request("GET", "/header",
                            "x-test-header: first\r\nx-test-header: second\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "second");
}

/* ========================================================================== */
/*                    chttpsvr_resp_write_str NULL-GUARD TEST                 */
/* ========================================================================== */

TEST(chttpserver, resp_write_str_null_guard) {
  /* chttpsvr_resp_write_str should return ccol_invalid_args, not crash, when it
     gets a NULL resp or a NULL str. The /null-guard route is registered in
     _setup, and g_null_guard_results is reset before each call so the test can
     be repeated. */
  g_null_guard_results[0] = g_null_guard_results[1] = -1;
  chttpcli_response *resp = _get("/null-guard");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(g_null_guard_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_null_guard_results[1], (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    TRAILING SLASH REJECTION TEST                           */
/* ========================================================================== */

TEST(chttpserver, trailing_slash_not_matched) {
  /* A path with a trailing slash must NOT match a route whose pattern has none,
   * so /api/v1/items/99/ has to give 404 even though /api/v1/items/{id} is
   * registered. */
  chttpcli_response *resp = _get("/api/v1/items/99/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/* ROUTE-MATCHING PER-ROUTER SEGMENT-DECODE CACHE TESTS                       */
/*                                                                            */
/* The route matcher of chttpserver.c percent-decodes each raw path segment   */
/* at most once per router and per request and keeps the decoded value in a   */
/* small cache shared by every candidate route the matcher tries against that */
/* router, instead of decoding the segment again for each candidate that      */
/* needs it. The decoded value of a raw segment, or its decode failure, is    */
/* the same answer whatever route asks. The tests below exercise that sharing */
/* directly by registering several routes on the same root router, where      */
/* trying an earlier candidate can fail, and that failure must never corrupt  */
/* what a later candidate reads back for a shared position, whether it's a    */
/* literal segment, a captured param or one with malformed percent-encoding.  */
/* Every route pattern below belongs to this test group alone, so a dynamic   */
/* registration here can neither shadow anything _setup() registered nor be   */
/* shadowed by anything else.                                                 */
/* ========================================================================== */

TEST(chttpserver, cache_shared_literal_prefix_segment_multiple_candidates) {
  /* Three routes share their first two segments. The second segment reaches the
     server percent-encoded, "%61lpha" for "alpha", so a real decode needs to be
     shared correctly across every candidate, not a raw byte-for-byte
     comparison. The test asks for the THIRD route's path, so the server tries
     the first two candidates and rejects each on its own, different third
     segment before it gets to the third, and those tries and rejections must
     not corrupt the shared "alpha" cache entry. A corrupt entry (freed, stale
     or never computed again) would make this test crash or return the wrong
     body. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(
                 g_srv, CHTTP_GET, "/cache-share/alpha/one",
                 _cache_literal_handler, (void *)"alpha-one"),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(
                 g_srv, CHTTP_GET, "/cache-share/alpha/two",
                 _cache_literal_handler, (void *)"alpha-two"),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(
                 g_srv, CHTTP_GET, "/cache-share/alpha/three",
                 _cache_literal_handler, (void *)"alpha-three"),
             (int)ccol_success);

  chttpcli_response *resp = _get("/cache-share/%61lpha/three");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "alpha-three");
  chttpclient_resp_free(resp);
}

TEST(chttpserver,
     cache_shared_param_segment_survives_earlier_failed_candidate) {
  /* Two routes both capture {id} at segment 0 and differ only at segment 1,
     which is a literal. The test asks for the SECOND route's path, so the
     server tries the first route first. That route captures id="xyz" (sent
     percent-encoded on the wire as "x%79z") into its own param array, then
     fails on segment 1 because "first" isn't "second", and frees that array.
     The second route still reads back the right value "xyz" for the same
     segment 0, which shows that the cache keeps its own decoded copy, whatever
     an earlier candidate did with a copy of it, and not a reference into memory
     that an earlier failed match has already freed. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(
                 g_srv, CHTTP_GET, "/cache-param/{id}/first",
                 _cache_param_handler, (void *)"first"),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(
                 g_srv, CHTTP_GET, "/cache-param/{id}/second",
                 _cache_param_handler, (void *)"second"),
             (int)ccol_success);

  chttpcli_response *resp = _get("/cache-param/x%79z/second");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "second:xyz");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, cache_malformed_segment_rejects_every_sharing_candidate) {
  /* Two routes share a param at segment 0 and differ only at segment 1. The
     request's segment 0 has a malformed percent-encoding, "%ZZ", which isn't
     valid hex, and the server must reject both routes with a 404, not only
     the one it tries first. A decode failure of a raw segment is a permanent
     fact about that segment (it can never match), and it doesn't depend on
     which candidate route asks. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/x",
                                     _cache_param_handler, (void *)"x"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/y",
                                     _cache_param_handler, (void *)"y"),
      (int)ccol_success);

  /* The request is sent raw, because an HTTP client refuses to send a malformed
     escape in the first place, and this test is about what the server does when
     one shows up anyway. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /cache-bad/%ZZ/y HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
}

/* ========================================================================== */
/* SUB-ROUTER PREFIX-MATCHING SEGMENT-DECODE CACHE TESTS                      */
/*                                                                            */
/* _find_route also percent-decodes each raw request-path segment at most     */
/* once per request and keeps the decoded value in a cache shared by every    */
/* _prefix_matches call of every non-root sub-router. That's a different      */
/* cache from the one of the group above, which is scoped to the candidate    */
/* routes of one router. The decoded value of a raw segment is the same       */
/* answer whatever sub-router prefix the matcher compares it with. The tests  */
/* below register several sibling sub-routers that share one leading prefix   */
/* segment, which the wire carries percent-encoded. Trying an earlier         */
/* sub-router can fail on a later, different prefix segment, and that failure */
/* must never corrupt what a later sub-router reads back for the same shared  */
/* leading position. Every prefix below belongs to this test group alone, so  */
/* a dynamic registration here can neither shadow anything _setup()           */
/* registered nor be shadowed by anything else.                               */
/* ========================================================================== */

TEST(chttpserver, subrouter_prefix_cache_shared_across_candidates) {
  /* Three sub-routers share their first prefix segment, which reaches the
     server percent-encoded, "pfx-sh%61re" for "pfx-share", so a real decode has
     to be shared correctly across every sub-router's _prefix_matches call, and
     a raw byte-for-byte comparison isn't enough. The test asks for the THIRD
     sub-router's route, so the server tries the first two and rejects each on
     its own, different second prefix segment before it gets to the third, and
     those tries and rejections must not corrupt the shared "pfx-share" cache
     entry. A corrupt entry (freed, stale or never computed again) would make
     this test crash or route to the wrong handler. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpsvr_router *r1 = chttpsvr_subrouter(g_srv, "/pfx-share/alpha");
  chttpsvr_router *r2 = chttpsvr_subrouter(g_srv, "/pfx-share/beta");
  chttpsvr_router *r3 = chttpsvr_subrouter(g_srv, "/pfx-share/gamma");
  REQUIRE_TRUE(r1 != NULL);
  REQUIRE_TRUE(r2 != NULL);
  REQUIRE_TRUE(r3 != NULL);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(r1, CHTTP_GET, "/leaf", _cache_literal_handler,
                              (void *)"alpha-leaf"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(r2, CHTTP_GET, "/leaf", _cache_literal_handler,
                              (void *)"beta-leaf"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(r3, CHTTP_GET, "/leaf", _cache_literal_handler,
                              (void *)"gamma-leaf"),
      (int)ccol_success);

  chttpcli_response *resp = _get("/pfx-sh%61re/gamma/leaf");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "gamma-leaf");
  chttpclient_resp_free(resp);
}

TEST(chttpserver,
     subrouter_prefix_cache_malformed_segment_rejects_every_sharing_router) {
  /* Two sub-routers share a leading prefix segment that has a malformed
     percent-encoding in the request, "%ZZ", which isn't valid hex. The server
     must reject both sub-routers with a 404, not only the one it tries first.
     A decode failure of a raw path segment is a permanent fact about that
     segment (it can never match), and it doesn't depend on which sub-router
     prefix asks. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpsvr_router *r1 = chttpsvr_subrouter(g_srv, "/pfx-bad/one");
  chttpsvr_router *r2 = chttpsvr_subrouter(g_srv, "/pfx-bad/two");
  REQUIRE_TRUE(r1 != NULL);
  REQUIRE_TRUE(r2 != NULL);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(r1, CHTTP_GET, "/leaf", _cache_literal_handler,
                              (void *)"one-leaf"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(r2, CHTTP_GET, "/leaf", _cache_literal_handler,
                              (void *)"two-leaf"),
      (int)ccol_success);

  /* The request is sent raw, for the reason
     cache_malformed_segment_rejects_every_sharing_candidate gives. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /pfx-b%ZZd/two/leaf HTTP/1.1\r\nHost: h\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
}

/* ========================================================================== */
/*                    NULL-HANDLER GUARD TESTS                                */
/* ========================================================================== */

TEST(chttpserver, on_null_fn_rejected) {
  /* chttpsvr_register_handler must return ccol_invalid_args when fn is NULL,
   * so a NULL handler can never be installed and crash later at dispatch time.
   * Uses g_srv2, which is created but never started. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, "/test-null-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_use_null_fn_rejected) {
  /* chttpsvr_router_use has to return ccol_invalid_args when fn is NULL, so
   * that no NULL middleware can be appended to the chain. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-mw-test");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_use(r, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, use_null_fn_rejected) {
  /* chttpsvr_use should return ccol_invalid_args when fn is NULL, just like
   * chttpsvr_register_handler and chttpsvr_router_use. Uses g_srv2, which is
   * never started, so the running server isn't affected. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_use(g_srv2, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    NULL PATTERN GUARD TESTS */
/* ========================================================================== */

TEST(chttpserver, on_null_pattern_rejected) {
  /* chttpsvr_register_handler needs to enforce the NULL-pattern precondition at
   * the public API boundary itself and not quietly leave it to
   * _router_add_route. Uses g_srv2. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, on_stream_null_pattern_rejected) {
  /* chttpsvr_register_streaming_handler rejects a NULL pattern at the
   * public API boundary. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_pattern_rejected) {
  /* chttpsvr_router_on must guard against a NULL pattern too, the same way as
   * chttpsvr_register_handler. Uses a sub-router on g_srv2. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-pat-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv =
      chttpsvr_router_on(r, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_pattern_rejected) {
  /* chttpsvr_router_on_stream has to enforce the NULL-pattern precondition at
   * the public API boundary, the same way as chttpsvr_router_on and
   * chttpsvr_register_streaming_handler. Uses a sub-router on g_srv2. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-pat-stream");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv =
      chttpsvr_router_on_stream(r, CHTTP_POST, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    DOUBLE-STOP SAFETY TEST */
/* ========================================================================== */

TEST(chttpserver, stop_twice_safe) {
  /* Two chttpsvr_stop calls on a server that was never started need to be safe.
   * Both are no-ops because chttpsvr_stop's was_started check is false, and
   * neither may crash or touch the shared engine reactor. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2);
  chttpsvr_stop(g_srv2);
}

/* ========================================================================== */
/*                    DUPLICATE RESPONSE HEADER TEST                          */
/* ========================================================================== */

TEST(chttpserver, resp_header_duplicate_last_wins) {
  /* When chttpsvr_resp_set_header is called twice with the same header name,
   * the second call must overwrite the first value and not add a second
   * entry. The /dup-header route sets x-dup twice. */
  chttpcli_response *resp = _get("/dup-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *v = chttpclient_resp_header(resp, "x-dup");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "second");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    INVALID STATUS CODE CLAMPING TEST                       */
/* ========================================================================== */

TEST(chttpserver, invalid_status_code_clamped_to_500) {
  /* chttpsvr_resp_set_status(resp, 0) stores an out-of-range value, and
   * _send_response needs to turn it into 500 (its own [100,999] range check)
   * instead of writing the raw out-of-range value onto the wire. The
   * /bad-status route exercises this. */
  chttpcli_response *resp = _get("/bad-status");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_null_key_returns_invalid_args) {
  /* chttpsvr_req_query_one has to return ccol_invalid_args when the key
   * argument is NULL. The handler calls it with key=NULL and writes the numeric
   * return value as the response body, so we can assert on it here. */
  chttpcli_response *resp = _get("/query-one-null-key?q=val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  char expected[16];
  snprintf(expected, sizeof(expected), "%d", (int)ccol_invalid_args);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    chttpsvr_req_read EDGE-CASE TESTS                       */
/* ========================================================================== */

TEST(chttpserver, req_read_zero_buflen_returns_zero) {
  /* On a streaming route chttpsvr_req_read(req, buf, 0) should return 0 (a
   * no-op) and not -1 (an error). Lumping the buflen == 0 check together with
   * the hard-error guards would wrongly return -1. */
  chttpcli_response *resp = _get("/stream-zero-buflen");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_read_empty_body_streaming_returns_zero) {
  /* The server can call a streaming handler for a request without a body, and
   * the first chttpsvr_req_read call should then return 0 for EOF at once,
   * not a negative value and not hang. */
  chttpcli_response *resp = _get("/stream-empty-read");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_null_req_returns_invalid_args) {
  /* chttpsvr_req_query_one must return ccol_invalid_args when req is NULL.
   * It's a pure C-level unit test with no HTTP round trip, since the function's
   * NULL guard fires before any HTTP state is touched. */
  const char *val = NULL;
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "key", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    TRAILING-SLASH PATTERN REJECTION TESTS                  */
/* ========================================================================== */

TEST(chttpserver, trailing_slash_pattern_rejected) {
  /* A pattern that ends with '/' is rejected at registration time with
   * ccol_invalid_args, because nothing could ever reach it: _compile_pattern's
   * split into segments can't tell an empty trailing segment from the variant
   * without the trailing slash, so such a pattern would match the same requests
   * as that variant and the API contract would mislead the caller. Rejecting it
   * is the only honest behaviour.
   *
   * "/" (the root) is a special case that is always accepted.
   *
   * The test uses g_srv2, which is never started, so it doesn't pollute g_srv's
   * routing table. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  /* Rejected: a trailing slash on a single-segment pattern. */
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Rejected: a trailing slash on a multi-segment pattern. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar/", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Accepted: the root pattern "/" has no trailing segment. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  /* Accepted: an ordinary multi-segment pattern without a trailing slash. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                    chttpsvr_req_read NULL-GUARD TESTS                      */
/* ========================================================================== */

TEST(chttpserver, req_read_null_req_returns_minus_one) {
  /* chttpsvr_req_read has to return -1 when req is NULL instead of
   * dereferencing it. A pure C-level unit test. */
  char buf[16];
  ssize_t n = chttpsvr_req_read(NULL, buf, sizeof(buf));
  REQUIRE_EQ(n, (ssize_t)-1);
}

TEST(chttpserver, req_read_null_buf_returns_minus_one) {
  /* chttpsvr_req_read needs to return -1 when buf is NULL (and len > 0). The
   * /stream-null-buf route calls req_read(req, NULL, 4) and echoes the return
   * value as a decimal string, so we can assert on it here. */
  chttpcli_response *resp = _get("/stream-null-buf");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "-1");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    chttpsvr_subrouter NULL-GUARD TESTS                     */
/* ========================================================================== */

TEST(chttpserver, subrouter_null_prefix_rejected) {
  /* chttpsvr_subrouter returns NULL when prefix is NULL, so the server
   * never dereferences the prefix later when it matches paths. Uses g_srv2
   * (never started) so that g_srv's routing tables stay clean. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, NULL);
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_no_leading_slash_rejected) {
  /* chttpsvr_subrouter must return NULL when the prefix doesn't start with
   * '/', since every valid HTTP path does. Uses g_srv2 (never started) so that
   * g_srv's routing tables stay clean. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "api/v1");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_at_start_rejected) {
  /* A prefix of "//api" starts with '/' but has two slashes in a row at index
   * 0-1. Such a prefix can never match a valid HTTP path and would make a
   * router that can never be reached, so chttpsvr_subrouter must reject it by
   * returning NULL. Uses g_srv2 (never started) so that g_srv's routing tables
   * stay clean. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "//api");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_in_middle_rejected) {
  /* A prefix with two slashes in a row in the middle, for example "/api//v1",
   * can't be reached either, so the server rejects it too. The test uses
   * g_srv2, which is never started. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/api//v1");
  REQUIRE_TRUE(r == NULL);
}

/* ========================================================================== */
/*                    on_stream / router_on_stream NULL-FN TESTS              */
/* ========================================================================== */

TEST(chttpserver, on_stream_null_fn_rejected) {
  /* chttpsvr_register_streaming_handler has to return ccol_invalid_args when fn
   * is NULL, just like chttpsvr_register_handler rejects a NULL handler. A NULL
   * streaming handler would crash when the ctpool tries to call it. Uses g_srv2
   * (never started). */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_fn_rejected) {
  /* chttpsvr_router_on_stream should return ccol_invalid_args when fn is NULL,
   * just like chttpsvr_register_streaming_handler and chttpsvr_router_on. Uses
   * a sub-router on g_srv2 (never started). */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-stream-fn");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_on_stream(r, CHTTP_POST, "/x", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    chttpsvr_resp_write NULL-DATA GUARD TESTS               */
/* ========================================================================== */

TEST(chttpserver, resp_write_null_data_edge_cases) {
  /* chttpsvr_resp_write has to:
   *   - return ccol_invalid_args when data is NULL and len > 0, and
   *   - return ccol_success when data is NULL and len == 0 (a no-op).
   * The /resp-write-null-guard route is registered in _setup. */
  g_null_data_results[0] = g_null_data_results[1] = -1;
  chttpcli_response *resp = _get("/resp-write-null-guard");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(g_null_data_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_null_data_results[1], (int)ccol_success);
}

/* ========================================================================== */
/*                    chttpsvr_req_read NULL-BUF ZERO-LEN TEST                */
/* ========================================================================== */

TEST(chttpserver, req_read_null_buf_zero_buflen_returns_zero) {
  /* chttpsvr_req_read(req, NULL, 0) needs to return 0, not -1. The buflen==0
   * guard has to run before the NULL buf guard, so a NULL buffer with a zero
   * length is a harmless no-op and not a hard error. */
  chttpcli_response *resp = _get("/stream-null-buf-zero-len");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    chttpsvr_resp_write_json ZERO-LEN TEST                  */
/* ========================================================================== */

TEST(chttpserver, resp_write_json_zero_len_rejected) {
  /* chttpsvr_resp_write_json must return ccol_invalid_args when len == 0. The
   * /json-zero-len route is registered in _setup. */
  g_json_zero_len_result = -1;
  chttpcli_response *resp = _get("/json-zero-len");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "done");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(g_json_zero_len_result, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*             PURE C-LEVEL NULL-GUARD UNIT TESTS (no HTTP round-trip)        */
/* ========================================================================== */

TEST(chttpserver, resp_write_json_null_resp_returns_invalid_args) {
  /* chttpsvr_resp_write_json(NULL, ...) has to return ccol_invalid_args without
   * dereferencing the NULL response pointer. Next to the zero-len rejection
   * test and the null-resp test of resp_write_str, this covers the specific
   * code path in resp_write_json that guards the resp pointer. */
  ccol_retval_t rv = chttpsvr_resp_write_json(NULL, "{}", 2);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_method_null_returns_chttp_get) {
  /* chttpsvr_req_method(NULL) must not crash and needs to return CHTTP_GET (the
   * enum's zero value, which can't be told apart from a real method when
   * there's no request). */
  REQUIRE_EQ((int)chttpsvr_req_method(NULL), (int)CHTTP_GET);
}

TEST(chttpserver, req_path_null_returns_null) {
  /* chttpsvr_req_path(NULL) returns NULL instead of dereferencing a NULL
   * request pointer. */
  REQUIRE_TRUE(chttpsvr_req_path(NULL) == NULL);
}

TEST(chttpserver, req_body_null_returns_null_and_zeroes_len) {
  /* chttpsvr_req_body(NULL, &len) needs to return NULL and write 0 into len
   * instead of dereferencing a NULL request pointer. */
  size_t len = 99;
  const void *p = chttpsvr_req_body(NULL, &len);
  REQUIRE_TRUE(p == NULL);
  REQUIRE_EQ(len, (size_t)0);
}

TEST(chttpserver, req_body_null_req_null_len_out_is_safe) {
  /* chttpsvr_req_body(NULL, NULL) has to return NULL without crashing even when
   * the request pointer and the len_out pointer are both NULL, so the function
   * guards the write to *len_out before it dereferences it. */
  REQUIRE_TRUE(chttpsvr_req_body(NULL, NULL) == NULL);
}

TEST(chttpserver, req_raw_query_null_returns_null) {
  /* chttpsvr_req_raw_query(NULL) should return NULL instead of crashing. */
  REQUIRE_TRUE(chttpsvr_req_raw_query(NULL) == NULL);
}

TEST(chttpserver, req_param_null_req_returns_null) {
  /* chttpsvr_req_param(NULL, key) must return NULL instead of crashing. */
  REQUIRE_TRUE(chttpsvr_req_param(NULL, "id") == NULL);
}

TEST(chttpserver, req_header_null_req_returns_null) {
  /* chttpsvr_req_header(NULL, name) returns NULL instead of dereferencing
   * a NULL request pointer. */
  REQUIRE_TRUE(chttpsvr_req_header(NULL, "x-foo") == NULL);
}

TEST(chttpserver, req_header_null_name_returns_null) {
  /* chttpsvr_req_header(req, NULL) has to return NULL without crashing.
   * chttpsvr_req is opaque, so the test reaches the guard through a full HTTP
   * round trip: _null_name_header_handler calls chttpsvr_req_header(req, NULL)
   * on a real live request and writes "null" only when the result is NULL. */
  chttpcli_response *resp = _get("/null-name-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_query_null_req_returns_null_and_zero_count) {
  /* chttpsvr_req_query(NULL, key, &n) needs to return NULL and write 0 into the
   * count instead of dereferencing a NULL request pointer. */
  size_t n = 99;
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", &n) == NULL);
  REQUIRE_EQ(n, (size_t)0);
}

TEST(chttpserver, req_query_null_req_null_count_out_is_safe) {
  /* chttpsvr_req_query(NULL, key, NULL) returns NULL without crashing
   * even when count_out is NULL, so the guard must check count_out before
   * writing 0 into it. */
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", NULL) == NULL);
}

TEST(chttpserver, req_query_one_null_req_null_val_out_is_safe) {
  /* chttpsvr_req_query_one(NULL, key, NULL) must return ccol_invalid_args (a
   * NULL req hits the early-exit guard) without trying to write through a NULL
   * val_out pointer. */
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "q", NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_query_one_invalid_args_resets_poisoned_val_out) {
  /* The header's doc comment of chttpsvr_req_query_one says *val_out is reset
   * to NULL on every failure return, ccol_invalid_args included, so the test
   * first poisons it with a non-NULL sentinel. That makes a regression that
   * leaves it untouched visible, while starting at NULL would let the test pass
   * whether or not the function touches it. */
  const char *val = (const char *)0xdeadbeefUL;
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "q", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ((void *)val, (void *)NULL);
}

TEST(chttpserver, req_query_one_null_val_out_with_hit_returns_success) {
  /* chttpsvr_req_query_one(req, key, NULL) returns ccol_success when the
   * key is present even though the caller passed NULL for val_out (a pure
   * existence check), and it must not crash. */
  chttpcli_response *resp = _get("/query-one-null-val-out?q=present");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_query_oom_null_req_returns_false) {
  /* chttpsvr_req_query_oom(NULL) has to return false instead of crashing on a
   * NULL request pointer. */
  REQUIRE_FALSE(chttpsvr_req_query_oom(NULL));
}

TEST(chttpserver, req_query_null_key_returns_null_and_zero_count) {
  /* chttpsvr_req_query(req, NULL, &n) should return NULL and write 0 into the
   * count without crashing. chttpsvr_req is opaque, so the test reaches the
   * guard through a full HTTP round trip: _null_key_query_handler calls
   * chttpsvr_req_query(req, NULL, &n) on a real live request and writes "null"
   * only when the result is NULL and the count is 0. */
  chttpcli_response *resp = _get("/null-key-query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, resp_set_status_null_is_noop) {
  /* chttpsvr_resp_set_status(NULL, ...) must not crash. There's no return value
   * to check, so not crashing is the only assertion. */
  chttpsvr_resp_set_status(NULL, 200);
}

TEST(chttpserver, resp_set_header_null_resp_returns_invalid_args) {
  /* chttpsvr_resp_set_header(NULL, ...) needs to return ccol_invalid_args, so a
   * caller can notice the error instead of the call silently doing nothing. */
  ccol_retval_t rv = chttpsvr_resp_set_header(NULL, "x-foo", "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, serve_null_srv_returns_invalid_args) {
  /* chttpsvr_start(NULL, ...) returns ccol_invalid_args instead of
   * crashing on a NULL server pointer. */
  ccol_retval_t rv = chttpsvr_start(CHTTPSVR_INVALID, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, stop_null_is_noop) {
  /* chttpsvr_stop(CHTTPSVR_INVALID) must not crash. */
  chttpsvr_stop(CHTTPSVR_INVALID);
}

/* (The engine lifecycle is implicit: the engine starts with the first
   chttpsvr_start call and stops when the last server is destroyed.) */

/* ========================================================================== */
/*                    RAW-QUERY ABSENT STRING TEST */
/* ========================================================================== */

TEST(chttpserver, raw_query_absent) {
  /* When the request URL has no query string, chttpsvr_req_raw_query returns
   * NULL and _raw_query_handler writes the literal "(none)". */
  chttpcli_response *resp = _get("/raw-query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    HEADERS-ONLY RESPONSE TEST */
/* ========================================================================== */

TEST(chttpserver, headers_only_response) {
  /* A handler can set a 204 status and a response header without ever calling a
   * chttpsvr_resp_write* function, and the server must still deliver the
   * right status code and header to the client: _send_response has to write the
   * head alone, with no body. */
  chttpcli_response *resp = _get("/headers-only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 204);
  const char *v = chttpclient_resp_header(resp, "x-no-body");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "1");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    DUPLICATE ROUTE REGISTRATION TEST */
/* ========================================================================== */

TEST(chttpserver, duplicate_route_first_wins) {
  /* One router can hold two registrations of the same method and path. The
   * routing table keeps BOTH entries in registration order and the first one
   * wins at match time. The server accepts the second registration and returns
   * ccol_success, but its handler never runs, because _find_route returns on
   * the first full match.
   *
   * _setup registers the /dup-first-wins route in advance:
   *   chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
   *                             _hello_handler, NULL);
   *   chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
   *                             _dup_second_handler, NULL);
   * _hello_handler answers "Hello, world!" and _dup_second_handler answers
   * "second". The expected body is "Hello, world!", since the first
   * registration wins. */
  chttpcli_response *resp = _get("/dup-first-wins");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    NULL-GUARD COVERAGE COMPLETION TESTS                    */
/* ========================================================================== */

TEST(chttpserver, router_on_null_fn_rejected) {
  /* chttpsvr_router_on needs to return ccol_invalid_args when fn is NULL, just
   * like chttpsvr_register_handler, chttpsvr_register_streaming_handler and
   * chttpsvr_router_on_stream, which have their own tests for it. Uses a
   * sub-router on g_srv2 (never started). */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-fn-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_on(r, CHTTP_GET, "/x", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_null_resp_set_status_no_crash_and_dual_null_header) {
  /* chttpsvr_resp_set_status(NULL, ...) must not crash, and since it returns
   * void there's no guard to check. chttpsvr_resp_set_header(NULL, NULL, ...)
   * returns ccol_invalid_args from the NULL resp guard, which fires
   * before the NULL name guard, so the result is the same as with only a NULL
   * resp. The companion test resp_set_header_null_name_value_live covers a NULL
   * name and a NULL value with a live resp inside a handler. */
  chttpsvr_resp_set_status(NULL, 200); /* must not crash */
  ccol_retval_t rv = chttpsvr_resp_set_header(NULL, NULL, "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args); /* NULL resp guard fires first */
}

TEST(chttpserver, resp_set_header_null_name_value_live) {
  /* Exercise the NULL name and NULL value guards of chttpsvr_resp_set_header
   * with a real, live chttpsvr_resp, which only exists inside a handler. The
   * /set-header-null-guards route is registered in _setup. */
  g_null_hdr_results[0] = g_null_hdr_results[1] = -1;
  chttpcli_response *resp = _get("/set-header-null-guards");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(g_null_hdr_results[0], (int)ccol_invalid_args); /* NULL name */
  REQUIRE_EQ(g_null_hdr_results[1], (int)ccol_invalid_args); /* NULL value */
}

TEST(chttpserver, resp_set_header_crlf_injection_rejected) {
  /* chttpsvr_resp_set_header needs to reject a name or a value with an embedded
   * CR or LF byte and return ccol_invalid_args without writing the bytes onto
   * the wire. _send_response emits "name:value\r\n" without escaping anything,
   * so without this check a handler that puts request data into a response
   * header would let an attacker add extra header lines or even split the
   * response in two, the classic HTTP response splitting. The test also checks
   * that a valid header set afterwards still works, which shows that the two
   * rejections don't corrupt resp's header list. _setup registers the
   * /set-header-crlf-guards route. */
  g_crlf_hdr_results[0] = g_crlf_hdr_results[1] = g_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status =
      _raw_request("GET", "/set-header-crlf-guards", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Neither injection attempt got onto the wire at all: no second status line
   * and no injected header name anywhere in the response. */
  REQUIRE_TRUE(strstr(buf, "x-injected") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-evil") == NULL);

  REQUIRE_EQ(g_crlf_hdr_results[0], (int)ccol_invalid_args); /* CRLF in name */
  REQUIRE_EQ(g_crlf_hdr_results[1], (int)ccol_invalid_args); /* CRLF in value */
  REQUIRE_EQ(g_crlf_hdr_results[2],
             (int)ccol_success); /* the legitimate header still works */
}

TEST(chttpserver, resp_set_header_bare_cr_and_lf_rejected) {
  /* chttpsvr_resp_set_header's guard against CRLF injection uses strpbrk(x,
   * "\r\n"), and that's what makes it catch a BARE CR or a BARE LF on its own,
   * and not only the combined "\r\n" pair of the test above. This test checks
   * that directly instead of assuming the paired case covers it: a regression
   * that only looked for the literal two-byte "\r\n" substring (with strstr
   * instead of strpbrk, for example) would pass that test and fail this one.
   * _setup registers the /set-header-bare-crlf-guards route. */
  g_bare_crlf_hdr_results[0] = g_bare_crlf_hdr_results[1] =
      g_bare_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-bare-crlf-guards", NULL, buf,
                            sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Neither injection attempt got onto the wire at all. */
  REQUIRE_TRUE(strstr(buf, "injected") == NULL);

  REQUIRE_EQ(g_bare_crlf_hdr_results[0], (int)ccol_invalid_args); /* bare CR */
  REQUIRE_EQ(g_bare_crlf_hdr_results[1], (int)ccol_invalid_args); /* bare LF */
  REQUIRE_EQ(g_bare_crlf_hdr_results[2],
             (int)ccol_success); /* the legitimate header still works */
}

TEST(chttpserver, resp_set_header_disallowed_names_rejected) {
  /* chttpsvr_resp_set_header has to reject an empty name with
   * ccol_invalid_args, because an empty name has no valid form on the wire, and
   * it also rejects a "Transfer-Encoding" name in any letter case,
   * storing neither of them. _send_response always computes and emits its own
   * Content-Length header from the real response body and never chunk-encodes
   * that body, so a handler's Transfer-Encoding header reaching the wire would
   * pair a false "chunked" claim with a real Content-Length over a body that
   * has no transfer coding at all. That framing is ambiguous, and an
   * intermediary that prefers Transfer-Encoding over Content-Length (RFC 7230
   * SS3.3.3) could then parse it wrongly. chttp_request_set_header makes the
   * same rejection on the client side. The test also checks that a valid header
   * set afterwards still works, which shows that the three rejections don't
   * corrupt resp's header list. _setup registers the
   * /set-header-disallowed-name-guards route. */
  g_disallowed_hdr_results[0] = g_disallowed_hdr_results[1] =
      g_disallowed_hdr_results[2] = g_disallowed_hdr_results[3] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-disallowed-name-guards", NULL,
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Transfer-Encoding never reached the wire in either spelling. */
  REQUIRE_TRUE(strcasestr(buf, "transfer-encoding") == NULL);

  REQUIRE_EQ(g_disallowed_hdr_results[0], (int)ccol_invalid_args); /* "" */
  REQUIRE_EQ(g_disallowed_hdr_results[1],
             (int)ccol_invalid_args); /* transfer-encoding */
  REQUIRE_EQ(g_disallowed_hdr_results[2],
             (int)ccol_invalid_args); /* Transfer-Encoding */
  REQUIRE_EQ(g_disallowed_hdr_results[3],
             (int)ccol_success); /* the legitimate header still works */
}

TEST(chttpserver, resp_set_header_non_tchar_name_rejected) {
  /* chttpsvr_resp_set_header should reject a header NAME with a byte outside
   * the tchar set of RFC 7230 SS3.2.6, and checking only for a CR or an LF
   * isn't enough. A space, a literal colon or a control byte such as DEL (0x7F)
   * is no vector for CRLF injection, but each still produces a malformed line
   * on the wire that a strict downstream parser can read wrongly: the name "X
   * Foo: bar", for example, puts "X Foo:bar:baz\r\n" on the wire, which isn't a
   * real split into two fields. The test also checks that a valid header with
   * every tchar byte that isn't a letter or a digit still works after the three
   * rejections, which shows that they don't corrupt resp's header list. _setup
   * registers the /set-header-non-tchar-name-guards route. */
  g_non_tchar_hdr_results[0] = g_non_tchar_hdr_results[1] =
      g_non_tchar_hdr_results[2] = g_non_tchar_hdr_results[3] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-non-tchar-name-guards", NULL,
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit!#$%&'*+-.^_`|~:fine") != NULL);
  REQUIRE_TRUE(strstr(buf, "X Foo") == NULL);
  REQUIRE_TRUE(strstr(buf, "X:Foo") == NULL);

  REQUIRE_EQ(g_non_tchar_hdr_results[0], (int)ccol_invalid_args); /* space */
  REQUIRE_EQ(g_non_tchar_hdr_results[1], (int)ccol_invalid_args); /* colon */
  REQUIRE_EQ(g_non_tchar_hdr_results[2], (int)ccol_invalid_args); /* DEL */
  REQUIRE_EQ(g_non_tchar_hdr_results[3],
             (int)ccol_success); /* the legitimate header still works */
}

TEST(chttpserver, subrouter_null_srv_rejected) {
  /* chttpsvr_subrouter must return NULL when srv is NULL, so that a dangling
   * back-pointer in the new router can't cause crashes at routing time. The
   * NULL prefix and the no-leading-slash cases have their own tests, and this
   * one completes the guard coverage. */
  chttpsvr_router *r = chttpsvr_subrouter(CHTTPSVR_INVALID, "/any");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, req_param_null_name_returns_null) {
  /* chttpsvr_req_param(req, NULL) returns NULL instead of crashing. The
   * /param-null-name route is registered in _setup. */
  chttpcli_response *resp = _get("/param-null-name");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                          COVERAGE GAP TESTS                                */
/* ========================================================================== */

/* 1. on_stream with a NULL server returns ccol_invalid_args */

TEST(chttpserver, on_stream_null_srv_returns_invalid_args) {
  /* chttpsvr_register_streaming_handler has to check srv != CHTTPSVR_INVALID
   * before doing anything else, or dereferencing the server pointer crashes. A
   * pure API validation test that needs no live server. */
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      CHTTPSVR_INVALID, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* 2. chttpsvr_req_query can read an empty query key (?=value) */

TEST(chttpserver, query_empty_key) {
  /* A query string like "?=hello" has an empty key, which the server has to
   * parse correctly so that chttpsvr_req_query(req, "", &n) reads it back.
   * _setup registers the /query-empty-key route. */
  chttpcli_response *resp = _get("/query-empty-key?=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_stray_ampersand_does_not_produce_phantom_empty_key) {
  /* "a=1&&b=2" has a really empty pair between the two '&' characters, with
     nothing at all in it, not even an '='. The parser must skip it instead of
     reading it as a bogus entry with an empty key and an empty value next to
     the real "a" and "b" pairs. That's a different case from "?=hello" above,
     which is a non-empty pair whose key half is empty, and that case has to
     stay as it is. */
  chttpcli_response *resp = _get("/query-stray-amp-counts?a=1&&b=2");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver,
     query_leading_and_trailing_ampersand_does_not_produce_phantom_empty_key) {
  /* "&a=1" has an empty pair before the first real pair, and "b=2&" one after
     the last pair. The parser skips both the same way instead of counting
     either as a bogus "" key. */
  chttpcli_response *resp = _get("/query-stray-amp-counts?&a=1&b=2&");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpclient_resp_free(resp);
}

/* 3. chttpsvr_req_read after EOF keeps returning 0 */

TEST(chttpserver, req_read_eof_is_idempotent) {
  /* Once chttpsvr_req_read has returned 0 (EOF), calling it again must keep
   * returning 0. The /stream-read-eof-twice route is registered in _setup. */
  chttpcli_response *resp = _get("/stream-read-eof-twice");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0 0");
  chttpclient_resp_free(resp);
}

/* 4. The chttpsvr_subrouter("/") edge case */

TEST(chttpserver, subrouter_slash_prefix_matches_only_root) {
  /* A sub-router created with the prefix "/" keeps prefix_len = 1 (the loop
   * that strips a trailing slash only runs when plen > 1). It matches only the
   * exact request path "/", and for any other path it doesn't match and the
   * router is skipped altogether. The "/" sub-router and its routes are
   * registered in _setup. */

  /* The sub-router must handle the exact path "/". */
  chttpcli_response *resp_root = _get("/");
  REQUIRE_TRUE(resp_root != NULL);
  REQUIRE_EQ(resp_root->status_code, 200);
  REQUIRE_TRUE(resp_root->body != NULL);
  REQUIRE_STREQ(resp_root->body, "root_subrouter");
  chttpclient_resp_free(resp_root);

  /* A path that starts with "/" but isn't exactly "/" must NOT match. */
  chttpcli_response *resp_foo = _get("/nonexistent-path-xyz");
  REQUIRE_TRUE(resp_foo != NULL);
  REQUIRE_EQ(resp_foo->status_code, 404);
  chttpclient_resp_free(resp_foo);
}

TEST(chttpserver, subrouter_slash_prefix_rejects_doubled_leading_slash) {
  /* A path can have a second slash right after the first, like "//foo", and
   * such a path must NOT match a "/" sub-router either. The server strips
   * exactly one leading slash and what's left, "/foo", isn't empty, so the path
   * isn't the exact root path "/" and the server skips the router, as it does
   * for any other non-root path. The test uses a raw socket instead of _get,
   * which goes through chttpclient's URL parsing and normalisation, so that it
   * exercises the server's literal path match on the wire directly. */
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "//nonexistent-path-xyz", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                    INPUT VALIDATION TESTS */
/* ========================================================================== */

/* 1. A pattern without a leading slash is rejected */

TEST(chttpserver, pattern_no_leading_slash_rejected) {
  /* _compile_pattern rejects every pattern that doesn't start with '/'.
   * Such a pattern can't represent a valid HTTP path, and it would also match
   * the same requests as the form with the slash, because the server strips the
   * leading '/' from both before comparing segments, which would break the
   * documented API contract. The test uses g_srv2 because it's never started,
   * so registering an invalid route on it doesn't affect the running test
   * server. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "health",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_POST, "echo", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* An empty string has no leading slash either. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* A well-formed pattern still has to be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/health", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* 2. Releasing the engine logger is refused while the engine runs */

TEST(chttpserver, set_engine_logger_release_refused_while_running) {
  /* chttpsvr_set_engine_logger(CLOG_INVALID) releases the engine logger, and
   * this binary's engine runs for its whole life, so the release is refused and
   * the logger _setup installed stays in place. */
  ccol_retval_t rv = chttpsvr_set_engine_logger(CLOG_INVALID);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

/* 3. The server handle is valid after a stop */

TEST(chttpserver, serve_stopped_server_state_valid) {
  /* chttpsvr_stop on a server that wasn't started is a no-op, and the handle
   * should stay consistent afterwards: route registration still needs to work
   * without crashing or returning an error. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2); /* no-op, g_srv2 isn't started */
  chttpsvr_stop(g_srv2); /* second call, is a no-op too */

  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/state-valid-check", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* 4. Argument guard coverage of the TLS configuration */

TEST(chttpserver, serve_tls_with_missing_certificate_files_fails_cleanly) {
  /* A TLS configuration whose certificate files don't exist fails the start
   * with ccol_http_tls_cert_load_failed, and the server stays stopped.
   *
   * A full TLS integration test, which starts a TLS server and completes HTTPS
   * handshakes, needs valid certificate files, so that coverage lives in the
   * dedicated tests_tls binary of this directory and not here; see this
   * directory's Makefile. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/cert.pem";
  tls.key_path = "/nonexistent/key.pem";
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.port = 0;
  cfg.host = "127.0.0.1";
  cfg.tls = &tls;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  bool running = true;
  ccol_retval_t run_rv = chttpsvr_is_running(g_srv2, &running);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((int)run_rv, (int)ccol_success);
  REQUIRE_FALSE(running);
}

/* On purpose this file has no test that blocks starting a TLS server while
 * another server runs: several server instances can run at the same time, so
 * starting a TLS server while g_srv is active is allowed. TLS integration needs
 * valid certificate files, so that coverage lives in the dedicated tests_tls
 * binary of this directory. */

/* The test tls_context_cleaned_before_second_serve_attempt needs real TLS
 * certificate files so that it exercises a real handshake. A certificate that
 * fails to load isn't enough, because ctls_ctx_cert_add reports a missing or
 * invalid certificate file in its own ccol_retval_t return, and a load failure
 * alone never gets to the code path this test must cover. That coverage lives
 * in the dedicated tests_tls binary of this directory, and this suite leaves it
 * out. */

/* ============================================================ */
/*            COVERAGE-GAP FILL TESTS                           */
/* ============================================================ */

TEST(chttpserver, streaming_get_no_body_via_req_body) {
  /* A streaming GET request has no body. Calling chttpsvr_req_body() inside the
   * handler has to return NULL/0, and the handler needs to produce a valid
   * "(empty)" response instead of crashing or returning garbage. This covers
   * the else branch of (body && len > 0) in a streaming route. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/stream-body-get-no-body");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, dynamic_route_registration_while_running) {
  /* chttpsvr_register_handler succeeds on a server that is already
   * serving requests, which exercises the write path of the rwlock while
   * reactor threads hold read locks, and a later request in the same test
   * process needs to reach the new route right away. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv, CHTTP_GET, "/late-dynamic-route", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  chttpcli_response *resp = _get("/late-dynamic-route");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_wins_over_root_route_at_same_path) {
  /* The server tries the routers in order of how specific their mount prefix
   * is, most specific first, so a sub-router mounted at /shadow-test answers a
   * request inside its own prefix even when the root router has a route for the
   * same effective path. The root's prefix matches every path, so it's the
   * least specific mount and the server tries it last; the registration order
   * plays no part in the outcome.
   *
   * _setup registers both routes in advance so the test doesn't change g_srv
   * while it runs, and it registers the ROOT one FIRST:
   *   1. Root route  /shadow-test/ping -> _shadow_root_handler
   *   2. Sub-router  /shadow-test  with /ping -> _shadow_sub_handler
   * Request: GET /shadow-test/ping -> has to return "sub-wins". */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/shadow-test/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "sub-wins");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_registration_rejected) {
  /* The middleware chain is capped at _CHTTPSVR_MAX_MW = 32, and the server
   * applies the cap at registration time: the first 32 chttpsvr_router_use
   * calls return ccol_success and the 33rd should return
   * ccol_not_permitted. Rejecting the registration itself keeps any single
   * router from ever building a chain that overflows, so the public API can't
   * reach the quiet 500 path at dispatch time this way. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *ov_r = chttpsvr_subrouter(g_srv2, "/overflow-mw-test");
  REQUIRE_TRUE(ov_r != NULL);

  for (int i = 0; i < 32; i++) {
    ccol_retval_t rv = chttpsvr_router_use(ov_r, _global_mw, NULL);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }
  /* The 33rd registration must be rejected. */
  ccol_retval_t rv33 = chttpsvr_router_use(ov_r, _global_mw, NULL);
  REQUIRE_EQ((int)rv33, (int)ccol_not_permitted);

  ccol_retval_t rv =
      chttpsvr_router_on(ov_r, CHTTP_GET, "/ping", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, middleware_overflow_produces_500) {
  /* A request whose combined global+router middleware count goes over
   * _CHTTPSVR_MAX_MW (32) gets a 500 Internal Server Error.
   *
   * The /mw-overflow-live sub-router is registered in _setup with 32
   * router-level middlewares, and g_srv also has 1 global middleware
   * (_global_mw), so the combined count at dispatch time is 33 > 32, which
   * trips the overflow guard in _on_request before the route handler is called.
   */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/mw-overflow-live/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_streaming_produces_500) {
  /* The middleware-overflow check in _on_request fires before the is_streaming
   * branch, so buffered and streaming routes share the same overflow detection
   * code. This test covers the streaming variant, so that the suite catches it
   * if the two code paths ever diverge.
   *
   * The /mw-overflow-live sub-router is registered in _setup with 32
   * router-level middlewares, and with the 1 global middleware the total is 33
   * > _CHTTPSVR_MAX_MW (32). The streaming route /mw-overflow-live/stream-ping
   * has to get the same 500 response as the buffered /mw-overflow-live/ping
   * route. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp =
      _post("/mw-overflow-live/stream-ping", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, malformed_encoding_in_param_correct_method_returns_404) {
  /* A request with a bad percent-encoding in a {param} segment needs to return
   * 404, even when the method matches the registered route. This covers the
   * capture-mode path of _match_route_cached, where _seg_cache_get returns NULL
   * for a bad encoding and the server treats the route as no match.
   *
   * _setup registers /api/v1/items/{id} for GET only. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_param_wrong_method_returns_404_not_405) {
  /* The server uses a dry-run match when the route's registered method doesn't
   * match the request's method, and the dry run and the capture match have to
   * check the percent-encoding the same way. A dry run that accepted any
   * non-empty token for a {param} segment without checking its percent-encoding
   * would break this: it would set method_mismatch_seen=true and answer a POST
   * to the GET-only /api/v1/items/{id} with a 405, while a GET to the same
   * malformed URL correctly returns a 404.
   *
   * The design of _match_route_cached and _seg_cache_get rules that difference
   * out: _seg_cache_get decodes every segment unconditionally, whether or not
   * the caller passed a non-NULL pv_out to capture the value, so a malformed
   * encoding is rejected the same way in both cases.
   *
   * This request needs to return 404 and not 405. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status =
      _raw_request("POST", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, valid_percent_encoded_literal_segment_matches_route) {
  /* A request path can have a percent-encoded character in a LITERAL route
   * segment, not in a {param}, and it is still expected to match the route.
   *
   * _setup registers /hello for GET. /hel%6Co percent-encodes 'l' (%6C is 0x6C,
   * which is 'l'), so after decoding it's the same as /hello and must match.
   *
   * This covers the literal-segment comparison of _match_route_cached, where
   * the text written by _decode_path_unsafe ends in a NUL. Without that
   * NUL the following strcmp reads past the valid content into uninitialised
   * stack memory, and on a stack where that byte isn't zero the route fails to
   * match and the server returns 404. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[4096] = {0};
  int status = _raw_request("GET", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "Hello, world!");
}

TEST(chttpserver,
     valid_percent_encoded_literal_segment_wrong_method_returns_405) {
  /* The same path as valid_percent_encoded_literal_segment_matches_route with
   * the wrong method. /hel%6Co decodes to /hello, which is GET-only. The path
   * has to match (the literal-segment decode succeeds), so the server sets the
   * method-mismatch flag and replies 405, not 404. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

TEST(chttpserver, malformed_encoding_in_literal_segment_returns_404) {
  /* A bad percent-encoding in a LITERAL route segment should give a 404.
   * /hel%ZZo has a bad encoding where the "hello" literal sits, so no route can
   * match it, and the server returns 404 and not a 500 or a crash. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_literal_segment_wrong_method_returns_404_not_405) {
  /* When a literal segment has an invalid percent-encoding, the path can't
   * match any route even in dry-run mode (the method-check only), so the
   * method-mismatch flag is never set and the server must return 404 and not
   * 405, whatever methods are registered for the pattern. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                    NULL-GUARD COMPLETENESS TESTS                           */
/* ========================================================================== */

TEST(chttpserver, on_null_srv_returns_invalid_args) {
  /* chttpsvr_register_handler must check srv != CHTTPSVR_INVALID and return
   * ccol_invalid_args before it touches the root router, mirroring
   * on_stream_null_srv_returns_invalid_args, which covers the streaming
   * variant. */
  ccol_retval_t rv = chttpsvr_register_handler(CHTTPSVR_INVALID, CHTTP_GET,
                                               "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_router_returns_invalid_args) {
  /* chttpsvr_router_on must check router != NULL. Dereferencing a NULL router
   * inside _router_add_route would crash when writing to router->routes, and
   * this guard catches it at the API boundary. */
  ccol_retval_t rv =
      chttpsvr_router_on(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_router_returns_invalid_args) {
  /* chttpsvr_router_on_stream has to check router != NULL, just like
   * chttpsvr_router_on. */
  ccol_retval_t rv =
      chttpsvr_router_on_stream(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    chttpsvr_start NULL-CONFIG TEST                         */
/* ========================================================================== */

TEST(chttpserver, serve_null_cfg_uses_default) {
  /* chttpsvr_start(srv, NULL) needs to fall back to CHTTPSVR_CONFIG_DEFAULT
   * instead of dereferencing the NULL cfg and crashing. The test uses g_srv,
   * which is already started, so the second-start guard returns
   * ccol_not_permitted. It must NOT return ccol_invalid_args, a code that would
   * wrongly claim NULL is an invalid argument when it's a default the function
   * handles. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_start(g_srv, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

/* ========================================================================== */
/*                         CHTTP_ANY WILDCARD TESTS                           */
/* ========================================================================== */

TEST(chttpserver, any_method_get) {
  /* A CHTTP_ANY route dispatches a GET request, and the handler can read
     CHTTP_GET back from chttpsvr_req_method(). */
  chttpcli_response *resp = _get("/any-method");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_post) {
  /* A CHTTP_ANY route dispatches a POST request, and the handler sees
     CHTTP_POST. */
  chttpcli_response *resp = _post("/any-method", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "POST");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_put) {
  /* A CHTTP_ANY route dispatches a PUT request. */
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_delete) {
  /* A CHTTP_ANY route dispatches a DELETE request. */
  char buf[2048] = {0};
  int status = _raw_request("DELETE", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "DELETE");
}

TEST(chttpserver, any_method_with_path_param) {
  /* A CHTTP_ANY route with a path parameter captures the param correctly, and
     the handler sees the real method. */
  chttpcli_response *resp = _get("/any-method/42");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET:42");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_specific_wins_for_registered_method) {
  /* A method-specific route registered BEFORE CHTTP_ANY on the same pattern
     wins for its own method (the first one wins). */
  chttpcli_response *resp = _get("/any-with-specific");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_catches_unregistered_method) {
  /* A CHTTP_ANY registered after a specific GET catches the methods that don't
     have their own route entry on the same pattern. */
  char buf[2048] = {0};
  int status =
      _raw_request("PUT", "/any-with-specific", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_first_wins_over_specific) {
  /* A CHTTP_ANY registered BEFORE a specific GET on the same pattern wins for
     every method, GET included (the usual first-wins rule). */
  chttpcli_response *resp = _get("/any-first");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, unrecognized_method_rejected_with_501_not_dispatched) {
  /* CHTTP_ANY is only a placeholder at registration time, meaning "match any of
     the seven real methods this server knows", and it is never the method of a
     real incoming request. A method token can be valid syntax and still be
     unknown here (a WebDAV verb such as PROPFIND, or TRACE, CONNECT or some
     custom verb), and such a token must never get to a CHTTP_ANY handler at
     all. _find_route's method_ok test is route->method == CHTTP_ANY, and it
     must not answer true whatever the real method is. A check that did would
     let such a request reach _any_method_handler, which calls
     chttp_method_str(chttpsvr_req_method(req)) and gets "UNKNOWN" back from the
     switch's default case instead of a real method name, and the client would
     get a 200 response that misreports the request. Instead the server rejects
     the request up front with a 501 (RFC 7231 SS6.6.2), before it parses the
     path or the headers and before it matches any route, so the handler never
     runs. An empty body proves this, because _any_method_handler always writes
     a non-empty method name. */
  char buf[2048] = {0};
  int status = _raw_request("PROPFIND", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
  REQUIRE_TRUE(strstr(buf, "content-length:0") != NULL);
}

TEST(chttpserver, unrecognized_method_rejected_even_on_unmatched_path) {
  /* The rejection happens before the route match and even before the path is
     parsed, so an unknown method on a path with no route at all still reports
     501 and not 404. */
  char buf[2048] = {0};
  int status =
      _raw_request("PROPFIND", "/no-such-route-at-all", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
}

/* ========================================================================== */
/*                    max_body_size / 413 BOUNDARY TESTS                      */
/* ========================================================================== */

/* Connects to `port` and sends a POST to `path` with Connection: close and a
   body of exactly `body_len` 'a' bytes, then reads the response, or reads until
   the connection closes. Returns the parsed status code, or -1 when no valid
   status line came back, for example after a connection reset before any
   response bytes arrived. */
static int _raw_post_fixed_body(int port, const char *path, size_t body_len,
                                char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* Bounds the read loop below, the same SO_RCVTIMEO guard as in _raw_request
     and for the same reason. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST %s HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    path, body_len);
  if (hn < 0 || (size_t)hn >= sizeof(hdr)) {
    close(fd);
    fd = -1;
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return -1;
  }

  char *body = (char *)malloc(body_len ? body_len : 1);
  if (!body) {
    close(fd);
    fd = -1;
    return -1;
  }
  memset(body, 'a', body_len);
  size_t sent = 0;
  while (sent < body_len) {
    ssize_t w = write(fd, body + sent, body_len - sent);
    if (w < 0) {
      free(body);
      close(fd);
      fd = -1;
      return -1;
    }
    sent += (size_t)w;
  }
  free(body);

  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = read(fd, buf + total, buf_sz - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);
  fd = -1;

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

TEST(chttpserver, buffered_max_body_size_at_limit_succeeds) {
  /* The server accepts a body of exactly max_body_size, 64 bytes, and
     echo it back in full. The check inside _on_body (chttpserver.c) uses a
     strict "greater than", so the boundary value itself needs to succeed. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_EQ(strlen(body), (size_t)SMALL_BODY_MAX);
}

TEST(chttpserver, buffered_max_body_size_exceeded_rejected) {
  /* The server has to reject a body one byte over max_body_size with a real 413
     Payload Too Large response, not a bare connection reset, and then close the
     connection, with Connection: close, instead of keeping it alive for a
     corrupt next request, because the body bytes over the limit stay unread on
     the wire. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 413);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, streaming_max_body_size_at_limit_succeeds) {
  /* A streaming route reads a body of exactly max_body_size bytes with
     chttpsvr_req_read and sees the full body with no stream error. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, streaming_max_body_size_exceeded_reported) {
  /* On the buffered path the library itself turns this into a 413 and never
     calls the handler, while a streaming route's handler always runs and
     decides its own response: chttpsvr_req_read() returns -1 and
     chttpsvr_req_stream_error() reports ccol_msg_too_large, the same shape as
     the ccol_timed_out case of the stream_read_timeout_reports_ccol_timed_out
     test. The connection should still carry a real, complete HTTP response, not
     a bare reset, and then close, with Connection: close, instead of staying
     alive for a corrupt next request, because the body bytes over the limit
     stay unread. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_msg_too_large") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

/* The field comment of max_body_size in chttpserver.h documents 0 as
   "unlimited", the same convention max_connections and max_header_bytes use in
   this config struct, so both the Content-Length pre-check of
   _on_headers_complete and the cumulative check of _on_body must read it as
   no cap at all. A bare `> limit` comparison would read limit == 0 as a cap at
   zero and reject every request with a body, so a caller choosing "no limit"
   this way (a real choice, next to the sibling fields of the struct) would get
   a server that answers every POST, PUT and PATCH with a body with a 413. This
   test sends a 200000-byte body, well above any ordinary default or small test
   limit, to a buffered and to a streaming route, and neither path may cap at
   zero. It uses a dedicated server of its own and not the shared
   g_small_body_srv fixture, whose max_body_size is fixed at SMALL_BODY_MAX, so
   the max_body_size of 0 here can't be mixed up with that fixture's boundary
   tests, and those tests can't disturb it. */
TEST(chttpserver, buffered_max_body_size_zero_means_unlimited) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_POST, "/unlimited-body-echo", _echo_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 80;
  cfg.max_body_size = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* _echo_body_handler writes the whole body back, so the response is as big as
     the request, which is why buf is on the heap and not a large stack array.
     Its size is body_len plus extra room for the headers. */
  const size_t body_len = 200000;
  char *buf = (char *)calloc(1, body_len + 4096);
  REQUIRE_TRUE(buf != NULL);
  int status = _raw_post_fixed_body(TEST_PORT + 80, "/unlimited-body-echo",
                                    body_len, buf, body_len + 4096);
  char *body = (status == 200) ? _decode_raw_body(buf) : NULL;
  bool body_found = body != NULL;
  size_t body_len_got = body_found ? strlen(body) : 0;
  free(buf);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(body_found);
  REQUIRE_EQ(body_len_got, body_len);
}

TEST(chttpserver, streaming_max_body_size_zero_means_unlimited) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/unlimited-body-stream", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 81;
  cfg.max_body_size = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  const size_t body_len = 200000;
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 81, "/unlimited-body-stream",
                                    body_len, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, malformed_chunked_encoding_forces_connection_close) {
  /* One mechanism says that a body error found while the server drains a
     diverted request must not force a synchronous close of the socket, and the
     413 and ccol_msg_too_large tests above are the only other place in this
     file that exercises it. _drain_body reports the failure back to
     _task_worker as a ccol_retval_t, and _task_worker turns that into a
     graceful synchronous status response. It must not tear the connection down
     early, because that would destroy the connection before the worker can
     write its own error response. The connection closes only after the server
     has flushed that response, through the ordinary keep-alive or close
     decision further on. This test exercises a different kind of parse error on
     that same path: malformed chunk-size framing in a Transfer-Encoding:
     chunked body, which chttp1_parser's chunk-size decoder catches, and not a
     max_body_size or declared-length check. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* "ZZZZ" isn't a valid hex chunk-size token, and the parser rejects it
     while consuming the body on the worker thread, well after the routing,
     which runs when the headers are complete and has already matched
     /stream-error-report. */
  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "ZZZZ\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below. The rule that the connection has to close
     afterwards can regress, and without this bound such a regression would hang
     the test, waiting for more bytes or an EOF that never come, instead of
     letting it fail its own assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The malformed body still needs to give a real, complete HTTP response that
     reports the ingestion failure, not a bare reset, and the connection then
     closes instead of staying alive for a corrupt next request. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") == NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, oversized_chunk_size_hex_rejected_gracefully) {
  /* "8000000000000000" is a 16-hex-digit chunk-size token with valid syntax,
     about 9.2 exabytes as a uint64_t. This parser's chunk-size decoder is pure
     unsigned arithmetic with correct overflow checks, so there's no bug class
     here about a signed negative value. chttp1_parser_t's
     max_chunk_size_override, which _conn_reset_for_request wires from
     chttpsvr_config_t.max_body_size, makes a value this large a rejection at
     decode time. Without it the parser would accept the value as the declared
     size of the current chunk, and the connection would wait for that many
     bytes of chunk data that never arrive, until the server's
     stream_read_timeout_us (30s by default on g_srv) settles it about 30 real
     seconds later. Asserting on the error value alone, that is on any
     x-stream-err value other than "none", passes in both cases, which is why
     the test also checks the elapsed time. The server is expected to reject the
     oversized chunk as soon as it parses the chunk-size line, never wait for
     that chunk's data, which doesn't exist, and report the same
     ccol_msg_too_large that a merely oversized *cumulative* body gives (see
     streaming_max_body_size_exceeded_reported). The round trip must finish
     well within a second, not in 30. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "8000000000000000\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below. The fast rejection can regress, and a
     regression that waits on stream_read_timeout_us instead would hang this
     test inside read() itself, where the elapsed_s check below never fails
     because it only runs after the loop returns. */
  struct timeval rcvtimeo = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed_s =
      (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  REQUIRE_TRUE(elapsed_s < 5.0);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_msg_too_large") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, oversized_content_length_buffered_route_rejected_upfront) {
  /* The counterpart of the chunked case above for plain Content-Length framing.
     A buffered route can get a declared Content-Length that is already above
     max_body_size, and the server rejects it with a 413 as soon as
     the headers are complete (the upfront check of _on_headers_complete),
     before it diverts to a worker thread and before it waits for any body byte.
     It must not wait until that many bytes have arrived, because the peer here
     never sends them at all. g_small_body_srv, on TEST_PORT+3, has
     max_body_size == 64. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 3);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* This request declares far more than SMALL_BODY_MAX (64) and never sends a
     single body byte. _setup registers /small-body-echo as a plain route with
     chttpsvr_register_handler, not as a streaming one. */
  const char *req =
      "POST /small-body-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 999999999\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below. The upfront rejection can regress, and a
     regression that waits for body bytes that never come would hang this test
     inside read() itself, where the elapsed_s check below never fails. */
  struct timeval rcvtimeo = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  clock_gettime(CLOCK_MONOTONIC, &t1);
  double elapsed_s =
      (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) / 1e9;
  REQUIRE_TRUE(elapsed_s < 5.0);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 413") != NULL);
}

TEST(chttpserver, chunked_body_with_trailer_headers_handled_once) {
  /* The "0\r\n" that ends a chunked body can carry a trailer part after it,
     before the final CRLF: zero or more header-field lines, per RFC 7230
     SS4.1.2. The server dispatches a route with CHTTP1_HEADERS_DIVERT_BODY (see
     _on_headers_complete), and it has to call on_headers_complete exactly once
     per request, even when a real trailer field follows the body. The test
     checks two things, that the server accepts and parses a trailer field at
     all and that it dispatches the request to the handler exactly once. The
     second one is checked indirectly: exactly one response comes back, and the
     connection behaves like an ordinary single request and response on a
     Connection: close connection. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "5\r\n"
      "hello\r\n"
      "0\r\n"
      "X-Trailer: some-value\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[2048] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* Exactly one response should arrive. A second on_headers_complete dispatch
     for the end of the trailer would add extra bytes to that one response and
     corrupt it, and it could also crash the worker or hang the connection. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, buffered_route_trailer_not_visible_via_req_header) {
  /* The documented contract of chttpsvr_req_header says that a trailer field of
     a chunked body (RFC 7230 SS4.1.2) isn't a request header and reads as
     absent (see the doc comment of that function in chttpserver.h). A
     regression shows best on a BUFFERED route, where the server always reads
     the whole body with the trailers before the handler runs, so the parser has
     read the trailer before the handler asks; see
     _buffered_trailer_echo_handler. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /buffered-trailer-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "5\r\n"
      "hello\r\n"
      "0\r\n"
      "X-Trailer: some-value\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[2048] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-trailer-value:(absent)") != NULL);
}

TEST(chttpserver, trailer_cannot_override_a_real_request_header) {
  /* This is the privilege escalation that splitting headers from trailers
     closes. A reverse proxy sets X-Forwarded-For and an authentication gateway
     sets X-Authenticated-User, and then the client adds chunked trailer fields
     with those same two names and values of its own choice. chttpsvr_req_header
     resolves a repeated header name to its LAST occurrence in wire order, so an
     index that held a trailer next to the headers would let the trailer outrank
     both, and the handler would get the client's values on a request the server
     has already routed and run its middleware over. Both header values have to
     come back unchanged. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /identity-header-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "X-Forwarded-For: 203.0.113.9\r\n"
      "X-Authenticated-User: guest\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "5\r\n"
      "hello\r\n"
      "0\r\n"
      "X-Forwarded-For: 127.0.0.1\r\n"
      "X-Authenticated-User: admin\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[2048] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-seen-forwarded-for:203.0.113.9") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-seen-user:guest") != NULL);
  REQUIRE_TRUE(strstr(buf, "127.0.0.1\r\n") == NULL ||
               strstr(buf, "x-seen-forwarded-for:127.0.0.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-seen-user:admin") == NULL);
}

TEST(chttpserver, trailer_cannot_turn_a_keep_alive_connection_into_closing) {
  /* The other half of the same isolation: the server must not interpret a
     trailer field named Connection, Content-Length or Transfer-Encoding. The
     framing is settled before the body is read, and the connection's keep-alive
     decision follows from the header block, so a "Connection: close" trailer
     needs to leave this connection usable again. The check is that the server
     answers a second request on the same socket. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *first =
      "POST /identity-header-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "5\r\n"
      "hello\r\n"
      "0\r\n"
      "Connection: close\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, first, strlen(first)), (ssize_t)strlen(first));

  char buf1[2048] = {0};
  REQUIRE_EQ(_read_one_http_response(fd, buf1, sizeof(buf1)), 200);
  REQUIRE_TRUE(strstr(buf1, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strcasestr(buf1, "connection:close") == NULL);

  const char *second =
      "GET /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Connection: close\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, second, strlen(second)), (ssize_t)strlen(second));

  char buf2[2048] = {0};
  _drain_socket_until_eof(fd, buf2, sizeof(buf2));
  close(fd);
  fd = -1;
  REQUIRE_TRUE(strstr(buf2, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf2, "Hello, world!") != NULL);
}

TEST(chttpserver, htab_in_request_target_rejected) {
  /* RFC 7230 SS3.1.1 and SS5.3 allow no whitespace inside a request-target, and
     a HTAB that gets through is a request smuggling primitive: a front end that
     splits the request line on whitespace reads "GET /hello", while this server
     reads "GET /hello<HTAB>x". The parser rejects the request line outright, so
     the server answers 400 Bad Request and closes. An accepted target would
     give a ROUTED answer instead, a 404 here because no route matches the path
     with the tab in it, so the two are told apart by the status and not by
     whether there's a response. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /hello\tx HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Connection: close\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[2048] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  /* A 404 here would mean the tab reached the route search as ordinary target
     content, which is the smuggling primitive this test is here to refuse. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

/* A small helper for the rejection tests below. It opens a connection to the
   shared test server, writes one raw request, reads until the server closes and
   returns the bytes. The read is bounded, so a regression that keeps the
   connection open fails the assertion instead of hanging the binary. */
static void _raw_request_to_test_server(const char *req, char *buf,
                                        size_t buflen) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  memset(buf, 0, buflen);
  _drain_socket_until_eof(fd, buf, buflen);
  close(fd);
  fd = -1;
}

TEST(chttpserver, http_1_1_request_without_host_gets_400) {
  /* RFC 7230 SS5.4 makes this a MUST for a server. A request without Host names
     no authority, and a front end and a back end may resolve that differently.
     The server refuses it before any routing, so the 200 of the /hello
     route must not show up. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, request_with_two_host_headers_gets_400) {
  /* The same MUST, and the half of it that matters for smuggling: two Host
     lines let a front end forward one authority while this server reads
     another. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /hello HTTP/1.1\r\nHost: a.example\r\nHost: b.example\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, http_1_0_request_without_host_is_served) {
  /* The missing-Host half of the rule only names HTTP/1.1. An HTTP/1.0 request
     is older than the field and must still be served. */
  char buf[1024];
  _raw_request_to_test_server("GET /hello HTTP/1.0\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(chttpserver, absolute_form_request_target_is_served) {
  /* RFC 7230 SS5.3.2: "a server MUST accept the absolute-form in requests". A
     client configured with an explicit proxy sends one, and so does a proxy
     that forwards the target it got as it is. Without the strip the route
     search sees the four segments "http:", "", "host" and "hello", matches
     nothing and answers 404 for a path this very server serves. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com/hello HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(chttpserver, absolute_form_with_port_and_query_is_served) {
  char buf[1024];
  _raw_request_to_test_server(
      "GET https://example.com:8443/hello?a=1 HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(chttpserver, absolute_form_with_empty_path_is_the_root_path) {
  /* RFC 7230 SS5.3.1: an empty path component stands for "/". This suite's "/"
     sub-router answers the exact root path with the body "root_subrouter", so
     that body proves that the empty component really became "/". A 404 would
     mean the server routed on an empty path, and a 400 that it couldn't read
     the target at all. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "root_subrouter") != NULL);
}

TEST(chttpserver, absolute_form_with_query_only_is_the_root_path) {
  /* The same rule with a query attached: "http://host?a=1" has an empty path
     component, and the query belongs to it. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com?a=1 HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "root_subrouter") != NULL);
}

TEST(chttpserver, unknown_scheme_in_target_is_not_stripped) {
  /* Only http and https can name an HTTP request. A target with any other
     scheme isn't an absolute-form target this server strips, and it doesn't
     start with "/", so it isn't an origin-form target either. The server
     refuses it with a 400 and must not make up a path from it. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET ftp://example.com/hello HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, malformed_chunked_body_gets_400_and_not_500) {
  /* A non-hexadecimal chunk size is a client error, and RFC 7231 SS6.5.1 gives
     it 400. A 500 here would blame the server for what the client sent and
     count a client fault into a 5xx rate. */
  char buf[1024];
  _raw_request_to_test_server(
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "ZZ\r\nhello\r\n0\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") == NULL);
}

TEST(chttpserver, malformed_chunk_terminator_gets_400_and_not_500) {
  char buf[1024];
  _raw_request_to_test_server(
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "5\r\nhelloXX0\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") == NULL);
}

TEST(chttpserver, oversized_chunk_still_gets_413_and_not_400) {
  /* max_body_size keeps its own status, and the 400 of a malformed body must
     not swallow it. */
  char buf[1024];
  _raw_request_to_test_server(
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "F0000000\r\nx\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 413") != NULL);
}

TEST(chttpserver, config_default_ships_finite_timeouts_and_limits) {
  /* An application that tunes nothing runs with CHTTPSVR_CONFIG_DEFAULT, so the
     shipped values are a security property in their own right. With the idle
     timeout off, a connection that opens and then says nothing would hold its
     slot until the peer closes it, and with the caps on total duration off, a
     peer that trickles a body, or reads a response one byte at a time, would
     hold a worker thread for as long as it likes. This test pins only what the
     library ships; other tests pin what an explicit 0 in each field means,
     starting a real server with that value. See
     stream_read_timeout_ms_zero_means_wait_indefinitely,
     max_body_read_duration_disabled_allows_slow_drip and
     max_response_write_duration_disabled_allows_slow_reader. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_GT(cfg.read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.idle_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.stream_read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_body_read_duration_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_response_write_duration_us, (uint64_t)0);
  REQUIRE_GT((long)cfg.max_connections, 0L);
}

TEST(chttpserver, streaming_route_trailer_never_visible_via_req_header) {
  /* The documented contract of chttpsvr_req_header for a STREAMING route: a
     trailer field reads as absent both before and after the handler's
     chttpsvr_req_read() calls drain the body to EOF. The "after" answer is the
     one that matters, because on a streaming route the parse of the body and
     the trailer only moves forward when the handler calls chttpsvr_req_read():
     once that loop returns 0 the parser has read the trailer, and the trailer
     must still not be readable. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /stream-trailer-visibility HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "Connection: close\r\n"
      "\r\n"
      "5\r\n"
      "hello\r\n"
      "0\r\n"
      "X-Trailer: some-value\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[2048] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-trailer-before:(absent)") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-trailer-after:(absent)") != NULL);
}

TEST(chttpserver, negative_content_length_rejected) {
  /* A Content-Length value may hold only digits, so "Content-Length: -1" has to
     fail. A parser that accepted a leading '-' would read a content_length of 0
     or less as "no body, already complete" as soon as the headers finish, and
     the body bytes the client did send would then be parsed again as the start
     of the next pipelined request, a desync of the framing. process_header_line
     catches this while it parses the headers, because parse_uint64_decimal
     accepts only digits, before any routing or diversion, so this is just the
     parser's ordinary answer to malformed input: the server answers HTTP 400
     Bad Request and closes the connection, and no handler runs. Every other
     parse error before routing, a request line with invalid syntax for example,
     behaves the same way in this parser. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: -1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below. The reject-and-close behaviour can regress, and
     a regression that kept the connection open would hang this test inside
     read() instead of letting it fail its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The server answered 400 Bad Request and closed. The request never reached a
     handler, so no routed answer shows up. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, chunked_not_last_in_transfer_encoding_list_rejected) {
  /* RFC 7230 SS3.3.1 says that `chunked`, when present, has to be the last
     transfer-coding. transfer_encoding_scan reports whether a `chunked` isn't
     the final coding of the list, and the parser must reject every such list
     outright, for example "chunked, gzip" or a `chunked` in the middle. A
     parser that stored it as an ordinary opaque header would set neither
     F_CHUNKED nor a usable Content-Length and so frame the body as an implicit
     zero-length message, while any upstream proxy that DOES honour a `chunked`
     anywhere in the list would disagree, which is the shape of request
     smuggling. The parser catches this while it reads the headers, before
     routing, so the server answers HTTP 400 Bad Request and closes the
     connection, as in the negative Content-Length case. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, gzip\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below. The reject-and-close behaviour can regress, and
     a regression that kept the connection open would hang this test inside
     read() instead of letting it fail its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

/* RFC 7230 SS7 tells a recipient to parse and ignore an empty element of a
   comma-separated list, and SS4.1 gives the "#rule" list syntax that
   Transfer-Encoding uses the same treatment. So "chunked," names the
   one-element list "chunked", and the message is chunked-framed.

   A scan that counted the empty element as a real token would see the list as
   ending with that empty element, so the message wouldn't be chunked-framed,
   and a recipient that follows SS7 would disagree with it about where the body
   ends, which is exactly the disagreement request smuggling needs.

   Against a scan that doesn't ignore an empty element, the trailing comma
   makes "chunked" a non-final coding, and the server answers 400 for a request
   that every conforming recipient accepts. */
TEST(chttpserver, trailing_comma_in_transfer_encoding_is_still_chunked) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /echo-body HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked,\r\n"
      "\r\n"
      "5\r\nhello\r\n"
      "0\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The body really was de-chunked, so the route ran and echoed it back. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "hello") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") == NULL);
}

/* A leading empty element is the same rule from the other side, and an interior
   one must not turn a final "chunked" into a non-final one. */
TEST(chttpserver, other_empty_transfer_encoding_elements_are_ignored) {
  const char *const reqs[] = {
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: ,chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, ,\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

    int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_TRUE(fd >= 0);
    REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, reqs[i], strlen(reqs[i])), (ssize_t)strlen(reqs[i]));

    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    _drain_socket_until_eof(fd, buf, sizeof(buf));
    close(fd);
    fd = -1;

    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
    REQUIRE_TRUE(strstr(buf, "hello") != NULL);
  }
}

/* The empty-element rule must not weaken the two refusals next to it. A
   "chunked" that really isn't last is still rejected, and so is a second
   application of "chunked", whatever empty elements are around them. */
TEST(chttpserver, empty_elements_do_not_weaken_the_chunked_refusals) {
  const char *const reqs[] = {
      /* chunked really not last, with an empty element in the list. */
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: ,chunked, gzip\r\n\r\n",
      /* chunked applied twice, with empty elements in between. */
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, ,chunked\r\n\r\n",
      /* chunked applied twice across two merged header lines. */
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked,\r\nTransfer-Encoding: chunked\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

    int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_TRUE(fd >= 0);
    REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, reqs[i], strlen(reqs[i])), (ssize_t)strlen(reqs[i]));

    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    _drain_socket_until_eof(fd, buf, sizeof(buf));
    close(fd);
    fd = -1;

    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  }
}

/* ========================================================================== */
/*                    SERVER DESTROY-WHILE-IN-FLIGHT TESTS                    */
/* ========================================================================== */

/* Connects to `port` and sends a POST whose body goes out one byte at a time,
   50ms apart, so a ctpool worker stays blocked inside chttpsvr_req_read and
   chttp1_stream_read the whole time. It then drains and throws away whatever
   response it gets, or the abrupt close. It's background traffic while the main
   thread destroys the server under it. */
static void *_drip_body_bg_thread(void *arg) {
  int port = *(int *)arg;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return NULL;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
  /* Bounds the drain loop below, because a regression could stop destroy() from
     closing this connection. This test's measured time is well under a second,
     so there's a wide margin here. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "abcdefghijklmnopqrst";
  const size_t body_len = 20;
  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /destroy-slow-body HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Type: text/plain\r\n"
                    "Content-Length: %zu\r\n"
                    "Connection: close\r\n"
                    "\r\n",
                    body_len);
  if (hn < 0 || (size_t)hn >= sizeof(hdr) || write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return NULL;
  }

  for (size_t i = 0; i < body_len; i++) {
    if (write(fd, body + i, 1) != 1) break;
    usleep(50000);
  }

  char buf[256];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
  }
  close(fd);
  fd = -1;
  return NULL;
}

/* Private synchronization state for one run of each of the two tests below,
 * destroy_while_worker_reading_slow_body_is_safe and
 * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts, which
 * destroy a server from a background thread and wait for it with a bound.
 *
 * One file-scope mutex, condition variable and done flag shared by both tests
 * isn't safe here. The two tests run back to back with nothing in between, and
 * each one deliberately detaches its thread on a timeout instead of joining it
 * (see the comment of either test), so a detached thread of an EARLIER test,
 * which may just be slow and not a regression at all, can still be running when
 * a LATER test starts. The later test then resets the shared `done` flag and
 * waits on the shared condition variable, and when the earlier thread finishes
 * and broadcasts during that wait, the later test sees a stale "done" that its
 * own destroy_th never produced. It then skips its own safety net on the
 * timeout path, which is there to keep it from blocking without a bound on a
 * thread that may hang, and falls through to a plain, unbounded pthread_join of
 * its OWN destroy_th. Since that test's chttpsvr_destroy() call can be the one
 * that really hangs (the regression these tests catch), a clean, bounded,
 * reported failure would turn into a silent, permanent hang of the whole
 * binary, which is exactly what the comments of both tests say they prevent. So
 * each run of each test gets its own private mutex, condition variable and done
 * flag, together with the srv handle in one struct on the heap, so that the
 * same timeout-and-detach path can leak them safely just as it already leaks
 * srv. That makes the problem impossible: no thread of one test can ever see or
 * signal another test's ctx. */
typedef struct {
  chttpsvr srv;
  pthread_mutex_t mtx;
  pthread_cond_t cv;
  bool done;
} _destroy_hang_ctx_t;

static void *_destroy_hang_thread_fn(void *arg) {
  _destroy_hang_ctx_t *ctx = (_destroy_hang_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  pthread_mutex_lock(&ctx->mtx);
  ctx->done = true;
  pthread_cond_broadcast(&ctx->cv);
  pthread_mutex_unlock(&ctx->mtx);
  return NULL;
}

TEST(chttpserver, destroy_while_worker_reading_slow_body_is_safe) {
  /* A ctpool worker can still hold state owned by the server, for example
   * srv->max_body_size, which chttpsvr_req_read reads, and __chttpsvr_destroy
   * must not free that state while the worker is blocked reading a slow body on
   * another connection. chttpsvr_stop's _drain_and_close_all_connections waits
   * for in_flight_requests to reach zero before anything frees the server's
   * memory, and that's what this test exercises. It asserts on no return value:
   * a use-after-free here shows up under `make memtest` (valgrind), or as an
   * abort or a crash in a debug build, and that is the real check. The test
   * uses its own short-lived server, so it can't disturb the shared test
   * fixture.
   *
   * A background thread drives chttpsvr_destroy() and this thread waits for it
   * with a bounded pthread_cond_timedwait instead of a bare, unbounded call on
   * this thread, the same pattern as
   * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts below. So
   * a regression in destroy's bounded wait and forced unblock (that test's
   * comment explains what the mechanism guards against) fails this one test
   * cleanly instead of hanging the whole binary with no failure to point at.
   * ctx owns srv and lives on the heap and not on the stack with RAII, for the
   * reason that test's comment gives: this function must be able to leave the
   * destroy thread detached and never join it when the bounded wait itself
   * times out, and a stack local would turn that bounded worst case into a
   * use-after-free as soon as a later test reuses this function's stack frame.
   * ctx is this test's private synchronization state; the comment of
   * _destroy_hang_ctx_t explains why the sibling test below must not share it.
   */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  REQUIRE_NE((void *)ctx, NULL);
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = chttpsvr_create(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_INVALID) {
    free(ctx);
    REQUIRE_TRUE(false);
  }
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      ctx->srv, CHTTP_POST, "/destroy-slow-body", _stream_error_report_handler,
      NULL);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 4;
  cfg.stream_read_timeout_us = 2000000;
  rv = chttpsvr_start(ctx->srv, &cfg);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  int port = TEST_PORT + 4;
  pthread_t bg;
  if (pthread_create(&bg, NULL, _drip_body_bg_thread, &port) != 0) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    REQUIRE_TRUE(false);
  }

  /* Polls until the server has accepted the background connection, parsed its
     headers and dispatched it to a ctpool worker, so in_flight_requests goes
     above 0, which means the worker is blocked inside chttpsvr_req_read in the
     middle of the drip, and only then destroys the server under it, instead of
     guessing with a fixed sleep. The 5s bound is well clear of this test's drip
     of about 1s, so a slow run under CI or valgrind can't turn this into a
     destroy of a server that's already idle; the race this test is named after
     is a destroy while a worker reads. On a timeout bg is joined before the
     REQUIRE_TRUE below (bg's read loop is bounded by its own 5s SO_RCVTIMEO,
     see its comment), which is this file's convention of cleaning up
     unconditionally before a REQUIRE. */
  bool dispatched = false;
  struct timespec dispatch_deadline;
  clock_gettime(CLOCK_MONOTONIC, &dispatch_deadline);
  dispatch_deadline.tv_sec += 5;
  for (;;) {
    if (_chttpsvr_in_flight_requests_for_tests(ctx->srv) > 0) {
      dispatched = true;
      break;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < dispatch_deadline.tv_sec ||
          (now.tv_sec == dispatch_deadline.tv_sec &&
           now.tv_nsec < dispatch_deadline.tv_nsec)))
      break;
    struct timespec nap = {0, 1000000L}; /* 1 ms */
    nanosleep(&nap, NULL);
  }
  if (!dispatched) {
    pthread_join(bg, NULL);
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    REQUIRE_TRUE(dispatched);
  }

  pthread_t destroy_th;
  if (pthread_create(&destroy_th, NULL, _destroy_hang_thread_fn, ctx) != 0) {
    /* destroy_th never started, so nothing else can be touching ctx yet. */
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    pthread_join(bg, NULL);
    REQUIRE_TRUE(false);
  }

  /* A generous bound. This test's drip takes about 1s (20 bytes at 50ms each),
     and the bounded-wait defaults of 30s and 5s keep their production size,
     since unlike the disabled-timeouts test below this one doesn't shrink them.
     Here stream_read_timeout_us=2000000 is already enough for destroy's
     graceful wait to succeed quickly. The bound is wide enough to fail cleanly
     on a loaded CI machine without masking a real hang. */
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 40;
  pthread_mutex_lock(&ctx->mtx);
  bool destroyed_in_time = true;
  while (!ctx->done) {
    if (pthread_cond_timedwait(&ctx->cv, &ctx->mtx, &deadline) == ETIMEDOUT) {
      destroyed_in_time = false;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->mtx);

  /* bg is joined unconditionally before the destroyed_in_time REQUIRE_* below,
     which can return early. bg's read loop is already bounded by a 5s
     SO_RCVTIMEO (see the comment of _drip_body_bg_thread), so joining here is
     always safe whatever destroy_th did, while skipping it on a timeout would
     leave bg unreclaimed for the rest of this binary's run for no reason. */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach the thread instead of joining it: on a real regression the destroy
       call may never return at all, and blocking here would turn one clean,
       bounded, reported test failure into a second, silent hang. This path is
       already a failure and leaks ctx and the srv it owns on purpose, with
       nothing freeing them, so the still running detached thread can never turn
       its own read of ctx into a use-after-free. */
    pthread_detach(destroy_th);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);
}

/* Connects to `port` and sends a POST that declares a Content-Length far bigger
   than what it sends: only a few body bytes go out, then it never sends the
   rest and never closes the connection. It's a peer that stalls forever by
   design, for the test
   destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts below. That
   test sets both stream_read_timeout_us and max_body_read_duration_us to 0,
   which means "wait indefinitely", so only the server's forced-unblock
   mechanism can make the worker thread reading this connection's body return;
   see _force_unblock_diverted_connections in chttpserver.c. */
static void *_stall_forever_bg_thread(void *arg) {
  int port = *(int *)arg;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return NULL;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
  /* Bounds the read loop below, because a regression could stop the server's
     forced-unblock mechanism from firing at all. The test driving this thread
     shrinks that mechanism's bounds to about 2.3s in total with
     _chttpsvr_set_wait_in_flight_bounds_for_tests, well clear of this margin,
     so this backstop can't mask a real regression there. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  char hdr[256];
  int hn = snprintf(hdr, sizeof(hdr),
                    "POST /stall-forever HTTP/1.1\r\n"
                    "Host: 127.0.0.1\r\n"
                    "Content-Length: 1000000\r\n"
                    "\r\n");
  if (hn < 0 || (size_t)hn >= sizeof(hdr) || write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    fd = -1;
    return NULL;
  }
  const char *partial = "only-a-few-bytes-of-a-much-larger-declared-body";
  /* far short of the declared length */
  if (write(fd, partial, strlen(partial)) != (ssize_t)strlen(partial)) {
    close(fd);
    fd = -1;
    return NULL;
  }

  /* This thread never sends the rest and never closes from its side, it just
     blocks and waits for whatever the server does. The unblock is part of what
     this test proves: without it this thread hangs forever, and the
     pthread_join below with it. */
  char buf[16];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
  }
  close(fd);
  fd = -1;
  return NULL;
}

/* chttpsvr_config_t.stream_read_timeout_us and max_body_read_duration_us both
   document 0 as "wait indefinitely", a real, supported configuration, for a
   slow but legitimate upload for example. chttpsvr_destroy(), through
   _quiesce_server_once, always calls ctpool_shutdown_drain() on the worker pool
   shortly after its own careful 30s wait for the in-flight requests, and
   ctpool_shutdown_drain() has no timeout of its own at all: it blocks until
   every queued and active task is done. With this configuration a single peer
   can stall in the middle of a body: it sends part of the body, declares much
   more in the Content-Length, then never sends the rest and never closes.
   Against a streaming route, with the rate floor off as well, that peer would
   hold the handler's thread blocked forever inside chttpsvr_req_read with
   nothing to unblock it, and chttpsvr_destroy() would then hang forever in
   spite of that careful bounded wait. What prevents this is a forced
   shutdown(2) of any connection that a worker thread still holds once the
   bounded wait runs out; see _wait_in_flight_bounded and
   _force_unblock_diverted_connections in chttpserver.c. The real assertion of
   this test is that chttpsvr_destroy() returns at all, checked with a bounded
   wait on a condition variable instead of a bare pthread_join, so a regression
   fails this one test cleanly instead of hanging the whole binary. The test
   uses its own short-lived server rather than the shared g_srv and turns both
   timeouts off, which is exactly the documented configuration it needs. It also
   uses the white-box hook _chttpsvr_set_wait_in_flight_bounds_for_tests(),
   which shrinks the graceful-wait and grace-period bounds of
   _wait_in_flight_bounded from tens of real seconds to a few hundred
   milliseconds, so the test runs the same escalation logic every time without
   waiting out the real, production-sized timers. */

TEST(chttpserver,
     destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts) {
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  /* ctx is on the heap and not on the stack, because _destroy_hang_thread_fn
     runs on a background thread, and this function must be able to leave that
     thread running and never join it when the bounded wait below times out.
     (The comment of that branch explains why an unconditional join there could
     turn a real regression into a second hang that's harder to diagnose, when
     what it has to give is a clean, reported test failure.) A stack local would
     turn that bounded worst case into a use-after-free as soon as a later test
     reuses this function's stack frame. Every early-exit path below destroys
     and frees ctx itself instead of relying on a REQUIRE_*'s early return plus
     a cleanup at scope exit, which can't run here, so a failure halfway through
     this test never leaks the shared engine's reference to this server; a
     leaked server still referenced by the engine would hang this whole binary
     in the chttpsvr_engine_wait() call of _teardown. ctx is this test's private
     synchronization state; the comment of _destroy_hang_ctx_t explains why the
     sibling test above must not share it. */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  if (ctx == NULL) {
    /* Every early-exit path below also resets this process-wide override to (0,
       0) before its own REQUIRE_*, just like the unconditional final reset at
       the very end of this test's success path. A value left shrunk here would
       corrupt the graceful-wait and grace-period bounds for the rest of this
       process's run, and every OTHER test of this binary that meets a really
       slow worker depends on those bounds. */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_NE((void *)ctx, NULL);
  }
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = chttpsvr_create(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_INVALID) {
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  /* A streaming handler is the one place where a thread still waits on a client
     for as long as the configuration allows, because it reads its body at the
     client's pace. A buffered route parks instead and holds no thread at all.
 */
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      ctx->srv, CHTTP_POST, "/stall-forever", _stream_echo_handler, NULL);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 29;
  cfg.stream_read_timeout_us = 0;    /* "wait indefinitely" */
  cfg.max_body_read_duration_us = 0; /* "wait indefinitely" */
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  rv = chttpsvr_start(ctx->srv, &cfg);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  int port = TEST_PORT + 29;
  pthread_t bg;
  if (pthread_create(&bg, NULL, _stall_forever_bg_thread, &port) != 0) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }

  /* Polls until the server has accepted the background connection, parsed its
     headers and diverted it to a ctpool worker, so in_flight_requests goes
     above 0: the worker is then blocked inside chttp1_stream_read, and without
     this guard it would stay blocked forever. Only then does the test destroy
     the server, instead of guessing with a fixed sleep. The 5s bound is well
     clear of the shrunk wait-in-flight overrides of 300ms and 2000ms above, so
     a slow run under CI or valgrind can't turn this into a destroy of a server
     that's already idle; the race this test is named after is a destroy while a
     worker is blocked forever. On a timeout bg is joined and the wait-in-flight
     override reset before the REQUIRE_TRUE below (bg's read loop is bounded by
     its own 5s SO_RCVTIMEO, see its comment), which is this test's convention
     of cleaning up unconditionally before a REQUIRE. */
  bool dispatched = false;
  struct timespec dispatch_deadline;
  clock_gettime(CLOCK_MONOTONIC, &dispatch_deadline);
  dispatch_deadline.tv_sec += 5;
  for (;;) {
    if (_chttpsvr_in_flight_requests_for_tests(ctx->srv) > 0) {
      dispatched = true;
      break;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < dispatch_deadline.tv_sec ||
          (now.tv_sec == dispatch_deadline.tv_sec &&
           now.tv_nsec < dispatch_deadline.tv_nsec)))
      break;
    struct timespec nap = {0, 1000000L}; /* 1 ms */
    nanosleep(&nap, NULL);
  }
  if (!dispatched) {
    pthread_join(bg, NULL);
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(dispatched);
  }

  pthread_t destroy_th;
  if (pthread_create(&destroy_th, NULL, _destroy_hang_thread_fn, ctx) != 0) {
    /* destroy_th never started, so nothing else can be touching ctx yet. */
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    pthread_join(bg, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }

  /* A generous bound next to the shrunk overrides of 300ms and 2000ms above, so
     there's real margin for a loaded CI machine, but nowhere near the real
     defaults of 30s and 5s that this test would otherwise need. */
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 15;
  pthread_mutex_lock(&ctx->mtx);
  bool destroyed_in_time = true;
  while (!ctx->done) {
    if (pthread_cond_timedwait(&ctx->cv, &ctx->mtx, &deadline) == ETIMEDOUT) {
      destroyed_in_time = false;
      break;
    }
  }
  pthread_mutex_unlock(&ctx->mtx);

  /* bg is joined unconditionally before the destroyed_in_time REQUIRE_* below,
     which can return early. bg's read loop is already bounded by a 5s
     SO_RCVTIMEO (see the comment of _stall_forever_bg_thread), so joining here
     is always safe whatever destroy_th did, while skipping it on a timeout
     would leave bg unreclaimed for the rest of this binary's run for no reason.
 */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach the thread instead of joining it: on a real regression the destroy
       call may never return at all, and blocking here would turn one clean,
       bounded, reported test failure into a second, silent hang. This path is
       already a failure and leaks ctx and the srv it owns on purpose, with
       nothing freeing them, so the still running detached thread can never turn
       its own read of ctx into a use-after-free. */
    pthread_detach(destroy_th);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);

  /* Put the real, production-sized defaults back for every later test of this
     process, since the override is process-wide and not per server. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
}

static _Atomic bool g_restart_race_stop;

/* Pipelines GET requests over `fd`, an already open keep-alive connection,
   until g_restart_race_stop is set. Any write or read failure is accepted and
   ends the loop, because the main thread tears down and replaces the listener
   side of the connection many times while this runs. The test
   restart_races_live_keep_alive_connection_is_safe below uses it to keep real
   pressure on the read of srv->worker_pool in _conn_start_diverted through a
   storm of restarts, and the same pressure also reaches the reads of
   srv->max_header_bytes and srv->max_body_size in _conn_reset_for_request and
   _on_body, on the worker thread the call diverts to. */
static void *_restart_race_pipeline_thread(void *arg) {
  int fd = *(int *)arg;
  const char *req = "GET /restart-race HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  char buf[512];
  while (!atomic_load(&g_restart_race_stop)) {
    if (write(fd, req, strlen(req)) <= 0) break;
    ssize_t r = read(fd, buf, sizeof(buf));
    if (r <= 0) break;
  }
  return NULL;
}

TEST(chttpserver, restart_races_live_keep_alive_connection_is_safe) {
  /* Every restart and teardown path of chttpsvr_start writes
   * srv->worker_pool (a write that drains, destroys or creates the pool) under
   * srv->mutex, because _conn_start_diverted reads that field under it. An
   * unsynchronized write is a real data race and also risks a use-after-free: a
   * leftover pool could be destroyed while the next pipelined request of a
   * keep-alive connection that's still open reads it or submits to it.
   * chttpsvr_stop closes only the listener, and connections the server already
   * accepted keep running, and their _on_headers_complete doesn't check
   * srv->lifecycle, so _conn_start_diverted can fire at any moment during a
   * restart. _retire_pools prevents this: the restart swaps fresh pools in
   * under srv->mutex and hands the old ones to a drain thread of their own,
   * which lets every task already on them finish before destroying them, so no
   * _conn_start_diverted call can be in flight with a stale copy of the pointer
   * that goes to ctpool_destroy.
   *
   * The test also covers a close relative of that data race on the same restart
   * path: srv->max_header_bytes and srv->max_body_size must be _Atomic, like
   * stream_read_timeout_us and its neighbours on the same struct. As plain
   * fields, chttpsvr_start would rewrite them without synchronization in the
   * small gap after it publishes srv->worker_pool and before it registers the
   * listener again, and the worker thread of an existing keep-alive connection
   * could read either field inside that same gap, in _conn_reset_for_request or
   * _on_body. The test changes both fields on every restart to exercise that
   * path. The window is narrow, so a plain CI run doesn't reliably hit it (the
   * comment on the struct field explains why); the real check for both races is
   * a clean run of this test under `make memtest` (valgrind) and under
   * `-fsanitize=thread`, and a debug build aborts or crashes outright if either
   * race comes back.
   *
   * For the same reason most of this test's body asserts on no return value,
   * since the hazards are data races and a use-after-free, not a wrong result.
   * A background thread keeps one keep-alive connection pipelining requests the
   * whole time while the main thread restarts the server many times in a tight
   * loop. Each cycle uses a fresh port, so the timing of a bind on a port that
   * just closed can never make the test flaky; the race under test is entirely
   * on the srv side and not on the listener's port. The test uses its own
   * short-lived server, so it can't disturb the shared test fixture. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/restart-race",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 50;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 50));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Bounds each read(2) inside the loop of _restart_race_pipeline_thread. That
     loop already accepts any read or write failure and exits on it (see that
     function's doc comment), so the bound only matters for a real regression
     that hangs the connection with neither an error nor a response, which would
     otherwise hang the pthread_join below, and this whole binary, forever. It
     never fires during correct operation, where every real response arrives
     well within a second. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  atomic_store(&g_restart_race_stop, false);
  pthread_t pipeline_thread;
  bool pipeline_thread_created =
      pthread_create(&pipeline_thread, NULL, _restart_race_pipeline_thread,
                     &fd) == 0;

  /* Every restart's result goes into a local instead of being asserted at
     once. A REQUIRE_* that returned early in the middle of the loop on a real
     regression would leave pipeline_thread unjoined forever (that thread only
     exits once g_restart_race_stop is set, which happens below) and also leave
     fd open, so the test would leak a thread and a socket for the rest of this
     binary's run when all it should do is fail this one test. */
  ccol_retval_t restart_rv[5];
  if (pipeline_thread_created) {
    for (int i = 0; i < 5; i++) {
      chttpsvr_stop(srv);
      cfg.port = (uint16_t)(TEST_PORT + 51 + i);
      cfg.max_header_bytes = 2048 + (size_t)(i * 512);
      cfg.max_body_size = (4 * 1024 * 1024) + (size_t)(i * 65536);
      restart_rv[i] = chttpsvr_start(srv, &cfg);
    }
  }

  atomic_store(&g_restart_race_stop, true);
  if (pipeline_thread_created) pthread_join(pipeline_thread, NULL);
  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(pipeline_thread_created);
  if (!pipeline_thread_created) return;
  for (int i = 0; i < 5; i++) REQUIRE_EQ((int)restart_rv[i], (int)ccol_success);
}

/* ========================================================================== */
/*                   MAX_BODY_READ_DURATION_MS TESTS                          */
/* ========================================================================== */

TEST(chttpserver, max_body_read_duration_exceeded_reports_ccol_timed_out) {
  /* stream_read_timeout_us only bounds each single gap between batches of body
   * bytes, so a client that sends a bit of data and then stalls inside that gap
   * never trips it. max_body_read_duration_us closes that loophole by bounding
   * the *total* time the server spends reading one request's body, whatever the
   * progress in each gap. This test sets a generous per-gap timeout so that one
   * can't fire first, and a short cap on the overall duration next to it. The
   * client sends part of the declared body once and never the rest, the proven
   * send-once-then-just-read pattern of
   * stream_read_timeout_reports_ccol_timed_out: a client that dripped its
   * writes would risk a server that responds and closes the connection halfway
   * through the upload, and that client's later writes would then fail with
   * EPIPE before it ever reads the response. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/deadline-test", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 5;
  cfg.stream_read_timeout_us = 5000000;   /* generous, must not fire first */
  cfg.max_body_read_duration_us = 300000; /* the cap really under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 5));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Well clear of the 300ms cap under test. It bounds the read loop below in
     case a regression makes the server never respond and never close. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *hdr =
      "POST /deadline-test HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 10\r\n"
      "Connection: close\r\n"
      "\r\n"
      "abc"; /* 3 of the declared 10 bytes, and never the rest */
  struct timespec t_start, t_end;
  clock_gettime(CLOCK_MONOTONIC, &t_start);
  REQUIRE_EQ(write(fd, hdr, strlen(hdr)), (ssize_t)strlen(hdr));

  char buf[1024] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  clock_gettime(CLOCK_MONOTONIC, &t_end);
  buf[total] = '\0';
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);

  /* The response body alone can't tell which of the two caps fired, since
   * stream_read_timeout_us and max_body_read_duration_us give the same
   * ccol_timed_out value (see chttpsvr_req_stream_error and
   * conn->deadline_exceeded in src/chttpserver.c). Without a wall-clock check a
   * max_body_read_duration_us that does nothing would still pass this test: the
   * connection would sit until the 5000ms cap of stream_read_timeout_us fires
   * instead, about 5s instead of about 300ms. So the test bounds the elapsed
   * time well below the 5000ms per-gap cap while leaving plenty of room above
   * the 300ms overall cap for scheduling jitter under load or valgrind, which
   * shows that the *short* cap is the one that fired. */
  long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                    (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
  REQUIRE_LT(elapsed_ms, 2500L);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_body_read_duration_disabled_allows_slow_drip) {
  /* A max_body_read_duration_us of 0 means there's no overall cap, so a slow
   * but steady drip that would trip a short overall cap must still succeed
   * here, and this server's stream_read_timeout_us is generous enough that the
   * per-gap timeout doesn't fire either. It guards against a deadline check
   * that fires when it should do nothing. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/deadline-disabled",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 6;
  cfg.stream_read_timeout_us = 5000000;
  cfg.max_body_read_duration_us = 0; /* the disabled state under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char buf[4096] = {0};
  int status =
      _raw_request_drip_body(TEST_PORT + 6, "POST", "/deadline-disabled",
                             "0123456789", 1, 80000, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_exceeded_closes_connection) {
  /* response_write_timeout_us only bounds each single write(2)-like call, so a
   * client that reads a byte or two just before each call's timeout expires
   * never trips it. max_response_write_duration_us, the write-side counterpart
   * of max_body_read_duration_us, closes that loophole by bounding the *total*
   * time the server spends sending one response, whatever the progress in each
   * call. This test sets a generous per-call timeout so that one can't fire
   * first, and a short cap on the overall duration next to it. The client sends
   * its request and then reads nothing at all, not even a slow trickle, for
   * much longer than the cap, so the kernel's socket buffers fill up and
   * chttp1_stream_write blocks waiting for space that never frees during that
   * silence. The client's silence is load-bearing: a client that reads the
   * response back in a tight, unthrottled loop right after sending the request
   * drains the socket fast enough that the write side never blocks at all, and
   * the test then passes while exercising nothing. The symptom of that mistake
   * is a measured elapsed time of a few milliseconds instead of the 300ms that
   * a deadline which really fires produces.
   *
   * The assertion compares byte counts and not times, because measuring how
   * long the client's read loop takes once it reads again can't separate two
   * cases on a fast loopback connection: "the deadline already closed this
   * connection" and "the deadline never fired, but the now unblocked write
   * finishes quickly once the reads resume". The rest of the transfer completes
   * quickly either way, but whether the FULL response arrives does separate
   * them: a deadline that really fires closes the connection halfway through
   * the write, during the silent window, and no later read can get back the
   * rest of the response however long it tries. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 60;
  cfg.response_write_timeout_us = 5000000; /* generous, must not fire first */
  cfg.max_response_write_duration_us = 300000; /* the cap really under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 60));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  size_t write_parks_before = _chttpsvr_park_kind_count_for_tests(2);
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Real silence: no read() call of any kind until the server's response write
     has parked on backpressure (a bounded poll of the process-wide count of
     write parks, and nothing else in this binary writes a response now), and
     then 700ms more, which is longer than the 300ms cap. The cap counts from
     the start of the response, which is before the park, so the deadline has
     passed when the reads below start, however fast the machine is. */
  bool write_parked = false;
  for (int i = 0; i < 10000 && !write_parked; i++) { /* at most 10 s */
    write_parked = _chttpsvr_park_kind_count_for_tests(2) > write_parks_before;
    if (!write_parked) usleep(1000);
  }
  struct timespec nap = {0, 700000000L}; /* 700ms */
  nanosleep(&nap, NULL);

  /* Bounds every read call below in case of a regression: the connection then
     stays open and the blocked write resumes only once the test starts to
     drain, so it eventually delivers the full response. Without this bound a
     regression would hang this test instead of just failing its assertion. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  size_t total = 0;
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) total += (size_t)r;
  close(fd);
  fd = -1;

  /* A working cap closes the connection halfway through the write, during the
     silent window, so whatever arrives afterwards is a truncated prefix of the
     status line, the headers and the full body, well under the 16 MiB of the
     body alone. A cap that does nothing delivers the complete response once the
     reads resume here, because nothing else in this setup closes the connection
     early (compare max_response_write_duration_disabled_allows_slow_reader,
     which shows that this exact handler and setup deliver the full response
     when the cap is off). */
  REQUIRE_TRUE(write_parked);
  REQUIRE_LT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_disabled_allows_slow_reader) {
  /* A max_response_write_duration_us of 0 means "no overall cap": a response
     the client reads slowly but steadily still arrives in full there, on
     a server whose response_write_timeout_us is generous enough that the
     per-call timeout doesn't fire either. It guards against a deadline check
     that fires when it should do nothing. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response-slow-ok", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 61;
  cfg.response_write_timeout_us = 5000000;
  cfg.max_response_write_duration_us = 0; /* the disabled state under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 61));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  /* Bounds the read loop below in case a regression makes the server hang
     instead of delivering the response (max_response_write_duration_us is off
     here on purpose, so no server-side deadline is left to catch that kind of
     bug). Without it read(2) would block forever and hang the whole test binary
     instead of failing this one assertion cleanly. The bound is generous next
     to the pacing this test adds below (up to about 2.5s of deliberate 5ms naps
     for a 16 MiB body). */
  struct timeval rcvtimeo = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response-slow-ok HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Read in small chunks at a deliberate pace, each gap well within the 5000ms
     per-call bound of response_write_timeout_ms, until EOF, and add up the
     total number of bytes that arrive. */
  char buf[4096];
  size_t total = 0;
  ssize_t r;
  int reads = 0;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    total += (size_t)r;
    reads++;
    if (reads % 8 == 0) {
      struct timespec nap = {0, 5000000L}; /* 5ms */
      nanosleep(&nap, NULL);
    }
  }
  close(fd);
  fd = -1;

  /* The test has to get the whole response (status line, headers and the full
     16 MiB body), not a truncated one cut short by a deadline that fired when
     it shouldn't. */
  REQUIRE_GT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_bounds_rejection_response) {
  /* max_response_write_duration_us needs to bound a courtesy rejection response
     (404/405/413/500/...) exactly as it bounds the response of a real matched
     route, because both go out through the same _send_response and a slowly
     reading peer could otherwise stretch either one out without limit. Unlike
     in max_response_write_duration_exceeded_closes_connection, a rejection's
     header block (the status line and a few fixed headers, no body at all) is
     always well under a kilobyte and in practice goes out in a single write(2)
     call, so real socket buffering can't produce a real short write or an
     expired deadline for it the way that other test does with a real, large
     handler body. So a dedicated hook that only exists under RUNNING_UNIT_TESTS
     forces the very next write-deadline check inside _send_response to report
     "already expired". */

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 65;
  cfg.response_write_timeout_us = 5000000;     /* generous, must not matter */
  cfg.max_response_write_duration_us = 300000; /* value itself is irrelevant:
                                                the hook below forces the
                                                check to report expiry
                                                regardless. */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 65));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Bounds the read below in case a regression makes this path stop consulting
     the guarded write-deadline check (exactly the regressed behaviour). If the
     hook is never consumed, the server sends an ordinary, complete 404 response
     and the read below returns promptly with its bytes instead of hanging, so
     the timeout is just an extra defensive measure and no plausible regression
     here hangs. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  _chttpsvr_force_next_response_write_deadline_expired_for_tests();

  const char *req =
      "GET /no-such-route-xyz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  /* fd has its own cleanup at scope exit (_close_scoped_fd above), so a
     write(2) failure, which we don't expect on this freshly connected local
     socket, doesn't leak it for the rest of this binary's run even if the
     REQUIRE_* below returns early. */
  ssize_t wn = write(fd, req, strlen(req));

  char buf[256];
  ssize_t r = (wn == (ssize_t)strlen(req)) ? read(fd, buf, sizeof(buf)) : -1;

  REQUIRE_EQ(wn, (ssize_t)strlen(req));

  /* The very first write-deadline check of the header-write loop (before any
     real write(2) call) reports "already expired" through the forced hook, so
     _send_response returns false after sending zero bytes. The connection then
     closes at once, so the client sees EOF (read returns 0) and gets nothing at
     all. A build that passed NULL for conn here would never consult the hook
     (conn && ... short-circuits) and would send a complete, ordinary 404
     response instead, which shows up here as a positive byte count. */
  REQUIRE_EQ((int)r, 0);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                         SERVER-OWNED LOGGER TESTS                          */
/* ========================================================================== */

TEST(chttpserver, create_with_null_logger_uses_internal_fatal_only_logger) {
  /* The server accepts cl == NULL. It then creates its own internal
   * logger (stderr, FATAL-only) and doesn't need one from the caller, and it
   * is still expected to work fully. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/null-logger-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 7;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/null-logger-hello",
           TEST_PORT + 7);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, create_with_logger_derives_and_leaves_parent_open) {
  /* The server must not store a cl != CLOG_INVALID directly. It derives its own
   * logger from it (tagged component=http-server), and on destroy it closes
   * only that derived logger, so the caller's handle stays open and usable. */
  clog parent = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_TRUE(parent != CLOG_INVALID);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(parent, NULL);
  if (srv == CHTTPSVR_INVALID) {
    clog_close(parent);
    REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  }

  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/derived-logger-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 8;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char url[256];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/derived-logger-hello",
           TEST_PORT + 8);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);

  chttpsvr_destroy(srv);

  /* parent must still be alive, so write through it and derive another
   * (unrelated) server from it. Both of these misbehave under valgrind or ASan
   * if the server closes the caller's handle by mistake. */
  clog_info(parent, "parent logger still usable after server destroy");

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(parent, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_destroy(srv2);

  clog_close(parent);
}

/* ========================================================================== */
/*                    UNIX DOMAIN SOCKET TESTS                                */
/* ========================================================================== */

TEST(chttpserver, unix_socket_listen_and_round_trip) {
  /* A "unix://path" in chttpsvr_config_t.host must bind a Unix domain socket
     instead of a TCP listener, and the server has to route and answer a request
     over that socket exactly like over a TCP connection. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/unix-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_%d.sock",
           (int)getpid());
  unlink(sock_path);

  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

  const char *req =
      "GET /unix-hello HTTP/1.1\r\nHost: localhost\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024] = {0};
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "Hello, world!");

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
}

/* A TCP connection keeps the kernel's socket buffer sizes, because an explicit
   SO_SNDBUF or SO_RCVBUF locks that size and turns Linux's buffer autotuning
   off for the socket (tcp(7)). A unix:// connection has no autotuning, so the
   server raises its buffers, and a white-box counter in the library counts the
   connections whose buffers it raised. Raising the buffers of a TCP connection
   makes the first half fail. */

TEST(chttpserver, accepted_tcp_socket_keeps_kernel_buffer_autotuning) {
  size_t before_tcp = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hello", NULL, buf, sizeof(buf));
  size_t after_tcp = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(after_tcp, before_tcp);

  /* The counter is live, a unix:// connection does raise its buffers. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/unix-bufs",
                                            _hello_handler, NULL),
             (int)ccol_success);
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_bufs_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  size_t before_unix = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
  const char *req =
      "GET /unix-bufs HTTP/1.1\r\nHost: localhost\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
  char rbuf[1024] = {0};
  int ustatus = _read_one_http_response(fd, rbuf, sizeof(rbuf));
  size_t after_unix = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
  REQUIRE_EQ(ustatus, 200);
  REQUIRE_EQ(after_unix, before_unix + 1);
}

/* Leaves a stale socket file at path, one that was bound there and closed, with
   no process listening on it afterwards. Returns whether it managed to. */
static bool _make_stale_unix_socket(const char *path) {
  unlink(path);
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  bool ok =
      bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0 && listen(fd, 1) == 0;
  close(fd);
  struct stat st;
  return ok && lstat(path, &st) == 0 && S_ISSOCK(st.st_mode);
}

TEST(chttpserver, unix_socket_stale_file_replaced_on_start) {
  /* A stale socket file at the configured path, which no process listens on,
     gets replaced by the start instead of making bind() fail. */
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_stale_%d.sock",
           (int)getpid());
  REQUIRE_TRUE(_make_stale_unix_socket(sock_path));

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/stale-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, sock_path, sizeof(addr.sun_path) - 1);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

  const char *req =
      "GET /stale-hello HTTP/1.1\r\nHost: localhost\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
  char buf[1024] = {0};
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
}

TEST(chttpserver, unix_socket_unwritable_path_start_fails) {
  /* A directory component that doesn't exist should make chttpsvr_start fail
     gracefully (bind() fails), and not crash or quietly succeed. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "unix:///chttpsvr_test_nonexistent_dir_xyz/socket.sock";
  ccol_retval_t rv = chttpsvr_start(srv, &cfg);
  REQUIRE_NE((int)rv, (int)ccol_success);
  chttpsvr_destroy(srv);
}

/* A unix:// path that doesn't fit in sun_path fails the start with ENAMETOOLONG
   as the errno the engine log reports, and an empty path with EINVAL. A
   refusal that left errno alone would report whatever an earlier call left
   there. */
TEST(chttpserver, unix_socket_path_too_long_reports_enametoolong) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  char host[512];
  memcpy(host, "unix:///tmp/", 12);
  memset(host + 12, 'p', sizeof(host) - 13);
  host[sizeof(host) - 1] = '\0';
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  errno = EBADF;
  ccol_retval_t too_long = chttpsvr_start(srv, &cfg);
  int too_long_errno = _chttpsvr_last_listen_errno_for_tests();
  cfg.host = "unix://";
  errno = EBADF;
  ccol_retval_t empty = chttpsvr_start(srv, &cfg);
  int empty_errno = _chttpsvr_last_listen_errno_for_tests();
  REQUIRE_NE((int)too_long, (int)ccol_success);
  REQUIRE_EQ(too_long_errno, ENAMETOOLONG);
  REQUIRE_NE((int)empty, (int)ccol_success);
  REQUIRE_EQ(empty_errno, EINVAL);
}

/* ========================================================================== */
/*                    MAX_CONNECTIONS TESTS                                   */
/* ========================================================================== */

TEST(chttpserver, max_connections_enforced) {
  /* With max_connections == 1, a second concurrent connection needs to stay
     pending in the kernel's listen backlog (the server never calls accept() for
     it and never serves it) until the first connection closes and frees the one
     slot. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/maxconn-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 9;
  cfg.max_connections = 1;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 9));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Give the server a moment to accept() fd_a and take the one slot before fd_b
     tries to connect. */
  struct timespec nap = {0, 150000000L}; /* 150ms */
  nanosleep(&nap, NULL);

  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  /* Bounds the final _read_one_http_response call below in case a regression
     breaks the listener's resume path when capacity frees up. Without it the
     underlying read(2) of fd_b would block forever waiting for bytes that never
     come and hang the whole test binary instead of failing this one assertion
     cleanly; that's exactly the kind of bug this test is here to catch. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req =
      "GET /maxconn-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_b, req, strlen(req)), (ssize_t)strlen(req));

  /* The server must NOT serve fd_b's request yet, because it's at capacity. */
  struct pollfd pfd = {.fd = fd_b, .events = POLLIN};
  int pr = poll(&pfd, 1, 300);
  REQUIRE_EQ(pr, 0);

  close(fd_a); /* frees the one connection slot */
  fd_a = -1;

  /* Now fd_b is accepted and served. */
  char buf[1024] = {0};
  int status = _read_one_http_response(fd_b, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  close(fd_b);
  fd_b = -1;
  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_connections_at_capacity_does_not_busy_loop) {
  /* While a connection waits in the backlog at capacity, the server's listener
     registration must be paused, rather than dispatched again with nothing
     to do. Level-triggered epoll reports a listen socket with a non-empty
     accept backlog as ready on every epoll_wait call, so a listener that keeps
     returning without pausing gets dispatched over and over and pins the
     reactor thread at about 100% CPU for as long as the server stays at
     capacity. That's directly measurable as CPU ticks in /proc/<pid>/stat, but
     the test doesn't assert on it, because a raw CPU time measurement is noisy
     on a shared or loaded machine. Instead it uses a white-box counter to count
     how many times the server dispatches the listener during a window held at
     capacity: a listener that pauses correctly produces at most a small, fixed
     handful of dispatches (the one that decides to pause), while a busy-looping
     one produces many thousands within a fraction of a second. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/busyloop-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 62;
  cfg.max_connections = 2;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 62));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Give the server a moment to accept() both and take the two slots before
     fd_c tries to connect. */
  struct timespec nap = {0, 150000000L}; /* 150ms */
  nanosleep(&nap, NULL);

  int fd_c _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_c >= 0);
  /* Bounds the final _read_one_http_response call below in case a regression
     breaks the listener's resume path when capacity frees up. Without it the
     underlying read(2) of fd_c would block forever waiting for bytes that never
     come and hang the whole test binary instead of failing this one assertion
     cleanly. The fd_b protection of max_connections_enforced is the same. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_c, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd_c, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req_c =
      "GET /busyloop-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_c, req_c, strlen(req_c)), (ssize_t)strlen(req_c));

  /* Give the pause dispatch that capacity triggers (at most one) time to happen
     before the measurement window starts, then hold at capacity for another
     300ms while nothing else touches the server, and count how many more times
     the server dispatches the listener during that hold. Everything below, up
     to and including __chttpsvr_destroy, runs unconditionally (the results go
     into locals and aren't asserted until the end), so that even with a real
     regression here (a listener that stays live and busy-loops) this server is
     torn down before any REQUIRE_* can end the test function early. A listener
     left registered and at capacity would otherwise busy-loop forever on the
     single process-wide shared reactor thread that this whole binary uses, and
     hang every later test that needs it too. */
  nanosleep(&nap, NULL); /* another 150ms */
  size_t before = _chttpsvr_listener_dispatch_count_for_tests();
  struct timespec hold = {0, 300000000L}; /* 300ms */
  nanosleep(&hold, NULL);
  size_t after = _chttpsvr_listener_dispatch_count_for_tests();
  size_t dispatch_delta = after - before;

  /* The server must not serve fd_c during the hold either, since it stays at
     capacity the whole time. */
  struct pollfd pfd_c = {.fd = fd_c, .events = POLLIN};
  int pr_c = poll(&pfd_c, 1, 0);

  close(fd_a); /* frees one of the two slots */
  fd_a = -1;
  char buf[1024] = {0};
  int status_c = _read_one_http_response(fd_c, buf, sizeof(buf));

  close(fd_b);
  fd_b = -1;
  close(fd_c);
  fd_c = -1;
  chttpsvr_destroy(srv);

  /* A generous bound that still separates the cases sharply. A listener that
     pauses correctly produces 0 further dispatches during the hold (it's
     already paused before the window starts), while a busy-looping one produces
     many thousands in 300ms (well under a microsecond per dispatch, at about
     100% CPU). 200 is far above any legitimate jitter and still orders of
     magnitude below what a real regression here produces. */
  REQUIRE_LT(dispatch_delta, (size_t)200);
  REQUIRE_EQ(pr_c, 0);
  REQUIRE_EQ(status_c, 200);
}

/* An allocator whose calloc() fails every time (returns NULL) once it's armed
   and passes calls through otherwise; malloc, free and realloc always pass
   through. The allocation of the conn struct in _conn_create is the only
   calloc() call this module sends through a server's mp while accepting an
   ordinary connection (see _listener_on_readable). Arming it after
   chttpsvr_start() has returned (all the server's setup allocations are done by
   then) therefore targets exactly that one allocation for every connection
   accepted afterwards, without having to guess or match an exact byte size like
   the fail-at-size allocator elsewhere in this file does. */
static _Atomic bool g_fail_every_calloc_for_backoff_test = false;
static void *_backoff_test_malloc(size_t n) { return malloc(n); }
static void _backoff_test_free(void *p) { free(p); }
static void *_backoff_test_calloc(size_t n, size_t s) {
  if (atomic_load(&g_fail_every_calloc_for_backoff_test)) return NULL;
  return calloc(n, s);
}
static void *_backoff_test_realloc(void *p, size_t s) { return realloc(p, s); }

/* Waits, for at most 5 s, until the process-wide count of listener pauses for
   resource pressure goes above `before`, and reports whether it did. The count
   only goes up after ccol_event_loop_pause() succeeded and the pause flag was
   stored, and the idle-timeout sweep never takes it back, so a late poll can't
   miss a pause. */
static bool _wait_for_listener_resource_pause(size_t before) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += 5;
  for (;;) {
    if (_chttpsvr_listener_resource_pause_count_for_tests() > before)
      return true;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)))
      return false;
    struct timespec nap = {0, 1000000L}; /* 1 ms */
    nanosleep(&nap, NULL);
  }
}

/* After a successful accept4(), a failure of _conn_create() or
   ctls_conn_create_server() (an allocation failure) must not loop straight back
   to accept4() with zero backoff. It needs the same backoff that the accept4()
   handling of EMFILE/ENFILE/ENOBUFS/ENOMEM a few lines up in the same loop
   applies, for the same reason: a persistent resource-exhaustion condition must
   not turn the loop into a busy loop. When allocations keep failing (a custom,
   bounded ccol_memmgmt_procs_t is the realistic trigger) while connections keep
   coming in, a loop without backoff spins the reactor thread and gets nowhere.

   The first allocation failure pauses srv's listener registration (see
   _listener_pause_for_resource_pressure); the loop doesn't just sleep before
   trying again inline. A sleep would block the single process-wide shared
   reactor thread for its whole duration on every dispatch, starving every OTHER
   connection on every OTHER server sharing that reactor thread for as long as
   the condition lasts. A pause takes the reactor out of the picture until the
   idle-timeout sweep thread resumes the listener (see
   _listener_resume_if_resource_pressure_cleared), at most
   _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS after the condition clears.

   The test checks both halves with white-box counters instead of inferring them
   from network activity over some time window. The sweep thread's real 1s timer
   runs unsynchronized with this test's clock, so it can resume this exact
   listener, and clear its pause flag, at any moment during the test, and
   neither a window of network activity nor a poll of the flag is a reliable
   signal of the pause. (1) The pause really happens and really takes effect:
   the count of real listener pauses goes up, and the count of pauses after an
   allocation failure goes up by exactly one. (2) The pause isn't permanent:
   once the failure condition clears, the server accepts and serves a fresh
   connection after the idle-timeout sweep thread has resumed the listener. */
TEST(chttpserver,
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping) {
  ccol_memmgmt_procs_t mp = {_backoff_test_malloc, _backoff_test_free,
                             _backoff_test_calloc, _backoff_test_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/backoff-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 63;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 63));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  size_t pause_before =
      _chttpsvr_listener_alloc_failure_pause_count_for_tests();
  size_t resource_pauses_before =
      _chttpsvr_listener_resource_pause_count_for_tests();

  /* The failure is armed only now, after chttpsvr_start() has returned, so only
     the connections this test makes from here on can reach _conn_create's
     calloc. */
  atomic_store(&g_fail_every_calloc_for_backoff_test, true);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* The server has to accept fd_a, fail the allocation of its connection and
     close it without sending a single byte, which a read of EOF (0) confirms.
     The SO_RCVTIMEO set above bounds that read, so a regression (a connection
     the server never processes) fails this REQUIRE_* cleanly instead of hanging
     the whole binary. */
  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  /* On the server side, close(cfd), which delivers this EOF to fd_a, runs
     BEFORE the pause, so the EOF doesn't prove the pause happened. So the test
     waits, with a bound, for the count of real listener pauses instead. That
     count only goes up once the pause has taken effect and nothing takes it
     back, so the idle-timeout sweep thread, which clears the pause flag on its
     own 1s timer, can't hide the pause from this wait. */
  bool paused = _wait_for_listener_resource_pause(resource_pauses_before);
  size_t pause_after = _chttpsvr_listener_alloc_failure_pause_count_for_tests();

  /* Clear the failure condition and check that the pause isn't permanent: the
     idle-timeout sweep thread needs to resume the listener (within at most
     _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, about 1s) and let the server accept and
     serve a fresh connection normally. A generous SO_RCVTIMEO bounds the wait,
     so a regression in the resume path fails this one assertion cleanly instead
     of hanging the whole binary, like the fd_c protection of
     max_connections_at_capacity_does_not_busy_loop does. */
  atomic_store(&g_fail_every_calloc_for_backoff_test, false);
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  struct timeval rcvtimeo_b = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_b, sizeof(rcvtimeo_b));
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req_b =
      "GET /backoff-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_b, req_b, strlen(req_b)), (ssize_t)strlen(req_b));
  char buf[1024] = {0};
  int status_b = _read_one_http_response(fd_b, buf, sizeof(buf));
  close(fd_b);
  fd_b = -1;

  chttpsvr_destroy(srv);

  REQUIRE_EQ(eof_r, (ssize_t)0);
  /* Exactly one pause, the one fd_a's allocation failure triggers. */
  REQUIRE_EQ(pause_after - pause_before, (size_t)1);
  REQUIRE_TRUE(paused);
  REQUIRE_EQ(status_b, 200);
}

/* The accept loop pauses unconditionally for any accept4() errno that isn't
   EWOULDBLOCK, EAGAIN, EINTR or one of the per-connection cases of
   _accept_errno_is_transient, and not only for the specific
   EMFILE/ENFILE/ENOBUFS/ENOMEM allow-list of resource exhaustion that
   _accept_errno_is_resource_exhaustion names (see that helper's comment and the
   comment at its one call site in _listener_on_readable_impl). This is the only
   test that drives a real accept4() failure with an UNCLASSIFIED errno
   (EBADF/EINVAL/ENOTSOCK/EFAULT and the like) through the loop, to check that
   the loop pauses for one of those and doesn't busy-loop. A design gated on
   the allow-list (pausing only for the four named resource-exhaustion errnos)
   passes every other test of this file and fails only this one.

   The test uses _chttpsvr_force_next_accept_errno_for_tests, a white-box hook
   scoped to one specific server, so this file's long-lived shared fixture
   server, or any other server active in the process at the same time, can never
   steal the errno forced for srv; a hook without that scope hits exactly that
   race now and then under valgrind. The hook makes the very next accept4()
   dispatch for srv report EINVAL, which on purpose is NOT one of the four
   resource-exhaustion errnos, so the test goes down the "unclassified" branch.
   It needs no corruption of a real fd and no exhaustion of a real system
   resource, which would depend on the environment and be out of proportion for
   what is really a simple control-flow decision. The test checks the LISTENER's
   reaction directly through the count of real listener pauses, which the real
   idle-sweep timer's independent schedule can't take back, unlike the pause
   flag or a paused state inferred from timing seen on the network (see the same
   reasoning in
   post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping). Then
   it checks that the pause isn't permanent, because a fresh connection succeeds
   once the real sweep has resumed the listener. */
TEST(chttpserver,
     accept_unclassified_errno_pauses_and_resumes_instead_of_busy_looping) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/accept-errno-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 83;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 83));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  /* The hook is armed only now, after chttpsvr_start() has returned, and scoped
     to srv, so it affects only the very next accept4() dispatch for THIS server
     and this test's long-lived shared fixture server (or any other server
     active in the process at the same time) never steals it. */
  size_t resource_pauses_before =
      _chttpsvr_listener_resource_pause_count_for_tests();
  _chttpsvr_force_next_accept_errno_for_tests(srv, EINVAL);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Wait, with a bound, for the count of real listener pauses. It only goes up
     once the pause has taken effect, and the idle-timeout sweep thread, which
     clears the pause flag on its own 1s timer, never takes it back, so the wait
     can't miss the pause however late it looks. */
  bool paused = _wait_for_listener_resource_pause(resource_pauses_before);

  /* The real accept4() of fd_a succeeds inside the server, but the hook throws
     that real connection away and simulates EINVAL instead. The server closes
     the real fd first, without reading or writing on it, before it pauses
     the listener, so fd_a sees a clean EOF and not an error, and this read
     returns at once however long the poll above took. */
  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  /* Clear the failure condition and check that the pause isn't permanent: the
     idle-timeout sweep thread should resume the listener (within at most
     _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, about 1s) and let the server accept and
     serve a fresh connection normally. A generous SO_RCVTIMEO bounds the wait,
     so a regression in the resume path fails this one assertion cleanly instead
     of hanging the whole binary. */
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  struct timeval rcvtimeo_b = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_b, sizeof(rcvtimeo_b));
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req_b =
      "GET /accept-errno-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_b, req_b, strlen(req_b)), (ssize_t)strlen(req_b));
  char buf[1024] = {0};
  int status_b = _read_one_http_response(fd_b, buf, sizeof(buf));
  close(fd_b);
  fd_b = -1;

  chttpsvr_destroy(srv);

  REQUIRE_EQ(eof_r, (ssize_t)0);
  REQUIRE_TRUE(paused);
  REQUIRE_EQ(status_b, 200);
}

/* ========================================================================== */
/*                    MAX_HEADER_BYTES TESTS                                  */
/* ========================================================================== */

TEST(chttpserver, max_header_bytes_within_limit_succeeds) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdrcap-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 10;
  cfg.max_header_bytes = 512;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/hdrcap-hello",
           TEST_PORT + 10);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_header_bytes_exceeded_closes_connection) {
  /* A header block over the configured cap needs to be rejected before routing:
     the server answers 400 Bad Request and then closes, like every other
     pre-routing parse error of this parser (see
     negative_content_length_rejected above), and the rejection must never reach
     the handler, so no routed 200 may show up.

     On purpose the 400 is best effort and not the whole assertion. A peer that
     is still streaming its oversized header block when the server closes may
     get a reset before it reads anything, so the read below may legitimately
     come back empty. What must NEVER show up is a routed answer. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdrcap-hello2",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 11;
  cfg.max_header_bytes = 128;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 11));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  char padding[300];
  memset(padding, 'a', sizeof(padding) - 1);
  padding[sizeof(padding) - 1] = '\0';
  char req[512];
  int n = snprintf(req, sizeof(req),
                   "GET /hdrcap-hello2 HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                   "X-Padding: %s\r\n\r\n",
                   padding);
  REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(req));
  REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);

  /* Bounds the read loop below. The reject-and-close behaviour can regress, and
     a regression that kept the connection open would hang this test inside
     read() instead of letting it fail its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_header_bytes_exact_boundary) {
  /* Neither max_header_bytes_within_limit_succeeds nor
     max_header_bytes_exceeded_closes_connection above sends a header block of
     EXACTLY max_header_bytes bytes, both stay well away from the boundary (a
     small ordinary request against a 512-byte cap, and about 330 bytes against
     a 128-byte cap). An off-by-one regression in chttp1_parser.c's strict
     greater-than comparison (the "total_header_bytes + contribution >
     max_bytes" check of process_header_line, and the same check for the request
     line's contribution) would slip through both of those tests. This test
     builds a request whose request-line plus header contribution lands EXACTLY
     on the configured cap, which has to succeed, and separately one a byte over
     it, which fails, the same exact-boundary pattern that
     buffered_max_body_size_at_limit_succeeds and
     buffered_max_body_size_exceeded_rejected use for max_body_size. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/hdrcap-boundary", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  enum { MAX_HDR_BYTES = 200 };
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 79;
  cfg.max_header_bytes = MAX_HDR_BYTES;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Every line's contribution, the request line included, is strlen(line) + 2
     (the CRLF the accumulator strips); see the same "+ 2" comment of
     process_header_line and its request-line sibling in chttp1_parser.c. */
  const char *request_line = "GET /hdrcap-boundary HTTP/1.1";
  const char *host_line = "Host: 127.0.0.1";
  size_t used = (strlen(request_line) + 2) + (strlen(host_line) + 2);
  REQUIRE_LT(used + 9, (size_t)MAX_HDR_BYTES); /* sanity: room for a filler */
  size_t budget_left = (size_t)MAX_HDR_BYTES - used;
  /* "X-Pad: " (7 bytes) + N filler bytes + 2 (CRLF) should equal budget_left
     exactly in the at-the-limit case. */
  size_t pad_len = budget_left - 9;

  char padding[256];
  REQUIRE_LT(pad_len, sizeof(padding));
  memset(padding, 'a', pad_len);
  padding[pad_len] = '\0';

  /* Exactly at the limit, which must succeed. */
  {
    char req[512];
    int n = snprintf(req, sizeof(req), "%s\r\n%s\r\nX-Pad: %s\r\n\r\n",
                     request_line, host_line, padding);
    REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(req));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(TEST_PORT + 79));
    REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
    int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_TRUE(fd >= 0);
    REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);
    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    ssize_t got = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    fd = -1;
    REQUIRE_GT(got, (ssize_t)0);
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  }

  /* One byte over the limit fails (a 400, and the connection closes),
     with exactly the same shape and one more filler byte. */
  {
    char padding_over[257];
    memcpy(padding_over, padding, pad_len);
    padding_over[pad_len] = 'a';
    padding_over[pad_len + 1] = '\0';

    char req[512];
    int n = snprintf(req, sizeof(req), "%s\r\n%s\r\nX-Pad: %s\r\n\r\n",
                     request_line, host_line, padding_over);
    REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(req));

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(TEST_PORT + 79));
    REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
    int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_TRUE(fd >= 0);
    REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);
    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    _drain_socket_until_eof(fd, buf, sizeof(buf));
    close(fd);
    fd = -1;
    /* One byte over the cap is a rejection, a 400 and never a routed answer,
       while exactly at the cap gave the 200 above. */
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  }

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    RESPONSE_WRITE_TIMEOUT_MS TEST                          */
/* ========================================================================== */

static void _large_body_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  char chunk[65536];
  memset(chunk, 'x', sizeof(chunk));
  for (int i = 0; i < 400; i++) /* About 25 MiB in total: well above any
                                    default OS socket buffer, so a client
                                    that never reads stalls the write of the
                                    server in the end. */
    chttpsvr_resp_write(resp, chunk, sizeof(chunk));
}

TEST(chttpserver, response_write_timeout_closes_slow_reader_connection) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/big-body",
                                               _large_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 12;
  cfg.response_write_timeout_us = 200000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 12));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req = "GET /big-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* On purpose we never read while the server writes, so its send has to stall
     eventually against this test's receive buffer, which nothing drains, hit
     response_write_timeout_us and force the connection closed instead of
     pinning a thread forever.

     First wait, with a bound, until the response write parks. Then move the
     slow-client limits' clock forward by 1 s and run one sweep tick on this
     thread, again and again, until the parked write is gone and no worker owns
     the connection. A send queue that has shrunk since the last look counts as
     progress, so the first ticks may keep the connection while the kernel is
     still filling this test's receive buffer, and a later tick sees a queue
     that no reader drains and closes the connection. The 20 ticks move the
     clock by at most 20 s, in which neither the rate floor nor
     max_response_write_duration_us can fire, so only response_write_timeout_us
     can close the connection here. */
  bool parked = false;
  for (int i = 0; i < 10000 && !parked; i++) { /* at most 10 s */
    parked = _chttpsvr_parked_count_for_tests(srv, 2) >= 1;
    if (!parked) usleep(1000);
  }
  bool closed = false;
  for (int i = 0; parked && i < 20 && !closed; i++) {
    _chttpsvr_advance_slow_clock_for_tests(1000);
    _chttpsvr_sweep_now_for_tests(srv);
    closed = _chttpsvr_parked_count_for_tests(srv, 2) == 0 &&
             _chttpsvr_in_flight_requests_for_tests(srv) == 0;
    if (!closed) usleep(10000);
  }

  /* Bounds every read call below in case of a regression: the connection would
     then stay open, and without this the test would block in read() forever
     instead of failing its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  size_t total = 0;
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) total += (size_t)r;

  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(closed);
  REQUIRE_EQ(r, 0); /* EOF, the server closed the connection */
  /* The byte count makes this test non-vacuous. The server doesn't keep the
     connection alive, so a timeout that does nothing would end in EOF as well,
     but only after the drain above got the whole body of 400 * 65536 bytes. A
     connection closed by the timeout delivers a cut-off response. */
  REQUIRE_LT(total, (size_t)400 * 65536);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP TEST                                 */
/* ========================================================================== */

TEST(chttpserver, idle_timeout_closes_unused_connection) {
  /* The module-local idle-timeout sweep thread needs to close a connection that
     never sends a request at all once idle_timeout_us has passed, and must not
     keep it open forever. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/idle-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 13;
  cfg.idle_timeout_us = 300000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 13));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  /* Never send anything. Only the idle-timeout sweep can make this fd readable
     (with an EOF), because the server has no data of its own to push. */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 3000); /* generous against idle_timeout_us=300000
                                   and the 1s interval of the sweep */
  REQUIRE_TRUE(pr > 0);
  char buf[16];
  ssize_t r = read(fd, buf, sizeof(buf));
  REQUIRE_EQ(r, 0);

  chttpsvr_destroy(srv);
}

/* The idle sweep takes its own `now` snapshot once, before it takes idle_mutex
   or looks at any particular connection, while _idle_list_add refreshes a
   connection's last_activity from a DIFFERENT thread every time the connection
   lands back in the idle list, which includes the ordinary case of a keep-alive
   request that finishes at the same moment as a sweep tick. A connection that
   gets fresh activity between the sweep's `now` snapshot and the moment the
   sweep reaches it therefore legitimately has last_activity > now. So the
   elapsed time must not be computed with plain `long` arithmetic compared
   through `(unsigned long)elapsed_ms >= idle_ms`, which silently wraps a small
   negative elapsed_ms to a huge unsigned value (always >= idle_ms for any
   realistic timeout) and evicts a connection that has only just become idle,
   instead of seeing zero (or negative) elapsed idle time. The test exercises
   this directly through the pure decision function rather than trying to win a
   real scheduling race against a live sweep thread. */
TEST(chttpserver, idle_sweep_negative_elapsed_never_flagged_as_timed_out) {
  /* The racing case itself: last_activity is "in the future" compared with the
     sweep's now snapshot (it got fresh activity after the sweep took now but
     before it looked at this connection). The function must never report it as
     timed out, however large idle_ms is (a small idle_ms is exactly what an
     unsigned wraparound makes always true). */
  struct timespec now = {.tv_sec = 1000, .tv_nsec = 0};
  struct timespec last_activity_future_by_1ms = {.tv_sec = 1000,
                                                 .tv_nsec = 1000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 1));
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 0 /* must still not underflow */));

  struct timespec last_activity_future_by_5s = {.tv_sec = 1005, .tv_nsec = 0};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_5s, 300));

  /* A future last_activity that crosses a tv_nsec borrow (now.tv_nsec <
     last_activity.tv_nsec but now.tv_sec == last_activity.tv_sec) also has to
     give a real negative elapsed time, and not a bogus wrap from subtracting
     the nanoseconds alone. */
  struct timespec now_zero_nsec = {.tv_sec = 2000, .tv_nsec = 0};
  struct timespec last_activity_same_sec_later_nsec = {.tv_sec = 2000,
                                                       .tv_nsec = 500000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now_zero_nsec, last_activity_same_sec_later_nsec, 300));

  /* Ordinary cases without a race behave exactly as documented. */
  struct timespec last_activity_past_by_500ms = {.tv_sec = 999,
                                                 .tv_nsec = 500000000L};
  /* elapsed == 500ms, which is >= a 300ms timeout, so it really timed out. */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 300));
  /* elapsed == 500ms, which is < a 1000ms timeout, so not timed out yet. */
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 1000));

  /* The exact boundary: elapsed_ms == idle_ms satisfies >=, so it has timed
     out. */
  struct timespec last_activity_past_by_exactly_idle_ms = {
      .tv_sec = 999, .tv_nsec = 700000000L}; /* exactly 300ms before now */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_exactly_idle_ms, 300));

  /* One millisecond short of the boundary, not timed out yet. */
  struct timespec last_activity_past_by_299ms = {.tv_sec = 999,
                                                 .tv_nsec = 701000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_299ms, 300));

  /* Identical now and last_activity (elapsed == 0) with idle_ms == 0: 0 >= 0 is
     a real, if degenerate, timeout. In practice the caller guarantees idle_ms
     is never zero, because _idle_sweep_fn skips a server altogether when it's
     0, but the function itself must still behave sanely without relying on
     that. */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(now, now, 0));
}

/* A duration cap expires only once its whole span has passed, never up to a
   millisecond early: the time left before a deadline rounds up, an elapsed time
   rounds down, and a deadline has passed only when no nanosecond is left. Each
   case here is less than a millisecond away from the limit, including across a
   tv_sec boundary. Arithmetic that truncates the tv_nsec difference toward zero
   would report the 0.5 ms that are left as 0 (so the cap expires early), and an
   elapsed 0.5 ms crossing a second boundary as 1 ms (so the 1 ms idle timeout
   fires early). */
TEST(chttpserver, duration_caps_never_expire_early) {
  struct timespec deadline = {.tv_sec = 10, .tv_nsec = 900000L};
  struct timespec half_ms_before = {.tv_sec = 10, .tv_nsec = 400000L};
  unsigned t1 = 0;
  bool live1 =
      _chttpsvr_shrink_timeout_at_for_tests(deadline, half_ms_before, &t1);
  struct timespec one_ns_before = {.tv_sec = 10, .tv_nsec = 899999L};
  unsigned t2 = 0;
  bool live2 =
      _chttpsvr_shrink_timeout_at_for_tests(deadline, one_ns_before, &t2);
  unsigned t3 = 0;
  bool live3 = _chttpsvr_shrink_timeout_at_for_tests(deadline, deadline, &t3);
  struct timespec far_deadline = {.tv_sec = 11, .tv_nsec = 500000001L};
  struct timespec start = {.tv_sec = 10, .tv_nsec = 0};
  unsigned t4 = 0;
  bool live4 = _chttpsvr_shrink_timeout_at_for_tests(far_deadline, start, &t4);
  unsigned t5 = 100;
  bool live5 = _chttpsvr_shrink_timeout_at_for_tests(far_deadline, start, &t5);

  struct timespec after_boundary = {.tv_sec = 1, .tv_nsec = 100000L};
  struct timespec before_boundary = {.tv_sec = 0, .tv_nsec = 999600000L};
  long long floor_ms =
      _chttpsvr_ms_since_for_tests(after_boundary, before_boundary, false);
  long long ceil_ms =
      _chttpsvr_ms_since_for_tests(after_boundary, before_boundary, true);
  long long ceil_same_sec =
      _chttpsvr_ms_since_for_tests(deadline, half_ms_before, true);
  long long exact = _chttpsvr_ms_since_for_tests(
      (struct timespec){.tv_sec = 5, .tv_nsec = 0},
      (struct timespec){.tv_sec = 3, .tv_nsec = 0}, true);
  bool idle_early = _chttpsvr_conn_idle_timed_out_for_tests(after_boundary,
                                                            before_boundary, 1);
  bool idle_due = _chttpsvr_conn_idle_timed_out_for_tests(
      (struct timespec){.tv_sec = 1, .tv_nsec = 999600000L}, before_boundary,
      1000);

  REQUIRE_TRUE(live1);
  REQUIRE_EQ(t1, 1u);
  REQUIRE_TRUE(live2);
  REQUIRE_EQ(t2, 1u);
  REQUIRE_FALSE(live3);
  REQUIRE_TRUE(live4);
  REQUIRE_EQ(t4, 1501u);
  REQUIRE_TRUE(live5);
  REQUIRE_EQ(t5, 100u);
  REQUIRE_EQ(floor_ms, 0LL);
  REQUIRE_EQ(ceil_ms, 1LL);
  REQUIRE_EQ(ceil_same_sec, 1LL);
  REQUIRE_EQ(exact, 2000LL);
  REQUIRE_FALSE(idle_early);
  REQUIRE_TRUE(idle_due);
}

TEST(chttpserver, idle_timeout_us_overflow_saturates_not_wrapped) {
  /* chttpsvr_config_t.idle_timeout_us and read_timeout_us are both uint64_t
     microsecond counts, while the internal field chttpsvr_start() feeds them
     into is an `unsigned` count of milliseconds. A value of UINT_MAX
     milliseconds or more must saturate to UINT_MAX - 1 instead of silently
     wrapping: a multiple of 2^32 milliseconds wraps to exactly 0, this field's
     "idle timeout off" sentinel, so such a wrap would quietly turn off the
     idle-timeout sweep the caller configured instead of applying the very long
     timeout the caller asked for. The result is checked through the white-box
     accessor instead of waiting out a real idle timeout, since the whole point
     is that the configured value is enormous. The arithmetic is the same on
     ILP32 and LP64, so the test runs on every platform. */

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 24;
  /* exactly 2^32 milliseconds, which would wrap to 0 without the correction */
  cfg.idle_timeout_us = UINT64_C(4294967296000);
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv), UINT_MAX - 1u);
  chttpsvr_destroy(srv);

  /* The same saturation through the read_timeout_us fallback path, with the
     largest value, and idle_timeout_us left at 0. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg2 = CHTTPSVR_CONFIG_DEFAULT;
  cfg2.host = "127.0.0.1";
  cfg2.port = TEST_PORT + 25;
  cfg2.idle_timeout_us = 0;
  cfg2.read_timeout_us = UINT64_MAX;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg2), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv2), UINT_MAX - 1u);
  chttpsvr_destroy(srv2);

  /* A value below one millisecond rounds up to one, and never to the 0 that
     turns the sweep off. */
  chttpsvr srv4 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv4 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg4 = CHTTPSVR_CONFIG_DEFAULT;
  cfg4.host = "127.0.0.1";
  cfg4.port = TEST_PORT + 25;
  cfg4.idle_timeout_us = 1;
  REQUIRE_EQ((int)chttpsvr_start(srv4, &cfg4), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv4), 1u);
  chttpsvr_destroy(srv4);

  /* An ordinary, small value still passes through untouched, on every
   * platform, whatever the above does. */
  chttpsvr srv3 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv3 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg3 = CHTTPSVR_CONFIG_DEFAULT;
  cfg3.host = "127.0.0.1";
  cfg3.port = TEST_PORT + 26;
  cfg3.idle_timeout_us = 5000000;
  REQUIRE_EQ((int)chttpsvr_start(srv3, &cfg3), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv3), (unsigned)5000);
  chttpsvr_destroy(srv3);
}

/* Starts a server on port with cfg and reads back every duration the start
 * resolved, in milliseconds. */
static bool _resolved_ms(chttpsvr_config_t cfg, int port, unsigned out[9]) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return false;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  bool ok = chttpsvr_start(srv, &cfg) == ccol_success &&
            _chttpsvr_resolved_ms_for_tests(srv, out);
  chttpsvr_destroy(srv);
  return ok;
}

/* Every duration of chttpsvr_config_t is in microseconds and the server keeps
 * milliseconds. The defaults convert exactly, a 0 meaning "the default" selects
 * it, a 0 in response_write_timeout_us takes stream_read_timeout_us, a value
 * below a millisecond rounds up and never gets to 0, CHTTPSVR_NO_DEADLINE stays
 * the internal "no deadline", and the largest finite value saturates one below
 * it. The expected values are literals. */
TEST(chttpserver, every_duration_converts_from_microseconds) {
  unsigned def[9], zero[9], edge[9];
  chttpsvr_config_t c_def = CHTTPSVR_CONFIG_DEFAULT;

  chttpsvr_config_t c_zero = CHTTPSVR_CONFIG_DEFAULT;
  c_zero.response_write_timeout_us = 0;
  c_zero.min_transfer_rate_grace_us = 0;
  c_zero.body_memory_wait_timeout_us = 0;
  c_zero.streaming_queue_timeout_us = 0;
  c_zero.stream_read_timeout_us = 2500;
  c_zero.idle_timeout_us = 0;
  c_zero.read_timeout_us = 7000;

  chttpsvr_config_t c_edge = CHTTPSVR_CONFIG_DEFAULT;
  c_edge.stream_read_timeout_us = 1;
  c_edge.max_body_read_duration_us = 1500;
  c_edge.response_write_timeout_us = UINT64_MAX;
  c_edge.max_response_write_duration_us = 999;
  c_edge.max_header_read_duration_us = 1000;
  c_edge.min_transfer_rate_grace_us = 1001;
  c_edge.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  c_edge.streaming_queue_timeout_us = CHTTPSVR_NO_DEADLINE - 1;

  bool ok_def = _resolved_ms(c_def, TEST_PORT + 231, def);
  bool ok_zero = _resolved_ms(c_zero, TEST_PORT + 232, zero);
  bool ok_edge = _resolved_ms(c_edge, TEST_PORT + 233, edge);
  REQUIRE_TRUE(ok_def && ok_zero && ok_edge);

  const unsigned want_def[9] = {30000u, 300000u, 30000u, 300000u, 30000u,
                                60000u, 5000u,   30000u, 5000u};
  const unsigned want_zero[9] = {3u, 300000u, 3u,     300000u, 30000u,
                                 7u, 5000u,   30000u, 5000u};
  const unsigned want_edge[9] = {1u,     2u, UINT_MAX - 1u, 1u,           1u,
                                 60000u, 2u, UINT_MAX,      UINT_MAX - 1u};
  for (int i = 0; i < 9; i++) {
    REQUIRE_EQ(def[i], want_def[i]);
    REQUIRE_EQ(zero[i], want_zero[i]);
    REQUIRE_EQ(edge[i], want_edge[i]);
  }
}

TEST(chttpserver, enable_keepalive_does_not_break_normal_requests) {
  /* SO_KEEPALIVE is set on an accepted connection's fd, which a client can't
     observe portably from outside (getsockopt only reports the socket state of
     the calling process). So this is a black-box smoke test that the
     setsockopt(2) call itself neither fails nor disturbs the normal request and
     response path, the same pattern this file uses for configuration knobs
     whose effect a client can't observe otherwise
     (max_header_bytes_within_limit_succeeds above does it for the byte cap
     itself, for example). */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/keepalive-opt-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 14;
  cfg.enable_keepalive = true;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/keepalive-opt-hello",
           TEST_PORT + 14);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, enable_reuseport_allows_second_listener_on_same_port) {
  /* Without SO_REUSEPORT a second bind to the same host:port fails with
     EADDRINUSE (chttpsvr_start returns ccol_unexpected_failure). That's a real
     effect of the option that can be seen from outside, unlike enable_keepalive
     and ipv6_only above. */
  chttpsvr srv1 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv1 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 15;
  cfg.enable_reuseport = true;
  REQUIRE_EQ((int)chttpsvr_start(srv1, &cfg), (int)ccol_success);

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv2, CHTTP_GET, "/reuseport-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  /* Make sure the second listener really serves traffic, not only that its
     bind() succeeded. The kernel spreads new connections over every
     SO_REUSEPORT listener on this port, so which of the two servers answers
     isn't deterministic. Only srv2 has the route registered, so the test treats
     a 404 (srv1 answered) as inconclusive but acceptable rather than as a hard
     failure, while any 200 shows that the mechanism works end to end.

     Each attempt uses chttp_do() with an explicit "Connection: close" header
     and NOT chttp_get(), because chttp_get() goes through the shared default
     client's keep-alive idle pool, keyed by "scheme://host:port". srv1 and srv2
     bind the very same 127.0.0.1:port pair, so the pool can't tell them apart,
     and once one connection exists every later chttp_get() to this URL silently
     reuses it instead of letting the kernel pick a listener again. Logging the
     status code of every attempt shows this directly: with chttp_get() the
     outcomes aren't independent trials at all but a short run of one result
     followed by an unbroken streak of the other for the rest of the loop (the
     kept-alive connection), while forcing Connection: close on every attempt
     gives really interleaved 200/404 results that look like a fair coin. The 64
     retries are only insurance against ordinary coin-flip variance; with each
     attempt a real, independent trial the chance of a false failure is already
     well below 1 in 10^18. */
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/reuseport-hello",
           TEST_PORT + 15);
  bool got_200 = false;
  for (int i = 0; i < 64 && !got_200; i++) {
    chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttp_request_set_header(req, "Connection", "close");
    chttpcli_response *resp = NULL;
    chttp_do(req, &resp);
    chttp_request_free(req);
    if (resp && resp->status_code == 200) got_200 = true;
    if (resp) chttpclient_resp_free(resp);
  }
  REQUIRE_TRUE(got_200);

  chttpsvr_destroy(srv1);
  chttpsvr_destroy(srv2);
}

TEST(chttpserver, ipv6_only_listener_still_serves_ipv6_traffic) {
  /* Best effort, following this codebase's IPv6 convention elsewhere (not every
     sandbox or CI environment has an IPv6 stack): skip instead of failing hard
     if binding "::1" doesn't work at all, because that's a limit of the
     environment and has nothing to do with ipv6_only. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/v6only-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "::1";
  cfg.port = TEST_PORT + 16;
  cfg.ipv6_only = true;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    fprintf(stderr,
            "[SKIP] ipv6_only_listener_still_serves_ipv6_traffic: no IPv6 "
            "stack available in this environment\n");
    return;
  }

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET6, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_port = htons((uint16_t)(TEST_PORT + 16));
  REQUIRE_EQ(inet_pton(AF_INET6, "::1", &sa.sin6_addr), 1);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Bounds the read(2) call below in case a regression makes the server never
     respond and never close, the same SO_RCVTIMEO guard that _raw_request and
     _read_one_http_response use elsewhere in this file. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req =
      "GET /v6only-hello HTTP/1.1\r\nHost: [::1]\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  REQUIRE_TRUE(strstr(buf, "200") != NULL);
  close(fd);
  fd = -1;

  chttpsvr_destroy(srv);
}

TEST(chttpserver, ipv6_only_blocks_ipv4_mapped_connections) {
  /* The sibling test above only exercises real IPv6 traffic, which passes the
     same way whether or not IPV6_V6ONLY is applied at all, so a regression that
     silently drops the setsockopt() call (or applies it to the wrong socket, or
     at the wrong moment relative to bind()) would still pass it. The whole
     documented purpose of ipv6_only (chttpserver.h) is that a dual-stack
     listener bound to the IPv6 wildcard must NOT also quietly accept IPv4
     traffic once the flag is set, and this test goes after that guarantee
     directly. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/v6only-blocks-v4", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "::"; /* the IPv6 wildcard, which accepts both stacks when
                       ipv6_only is false; that is what makes this a
                       meaningful test of the flag, unlike "::1" (IPv6
                       loopback), which never accepts IPv4 traffic,
                       whatever ipv6_only is. */
  cfg.port = TEST_PORT + 77;
  cfg.ipv6_only = true;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    fprintf(stderr,
            "[SKIP] ipv6_only_blocks_ipv4_mapped_connections: no IPv6 "
            "wildcard bind available in this environment\n");
    return;
  }

  /* Control: a real IPv6 connection still has to work, which confirms that this
     listener is otherwise healthy and that the port really is bound (the same
     request as in the sibling test). */
  int fd6 _ccol_destructor(_close_scoped_fd) = socket(AF_INET6, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd6 >= 0);
  struct sockaddr_in6 sa6;
  memset(&sa6, 0, sizeof(sa6));
  sa6.sin6_family = AF_INET6;
  sa6.sin6_port = htons((uint16_t)(TEST_PORT + 77));
  REQUIRE_EQ(inet_pton(AF_INET6, "::1", &sa6.sin6_addr), 1);
  REQUIRE_EQ(test_connect(fd6, (struct sockaddr *)&sa6, sizeof(sa6)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd6, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req6 =
      "GET /v6only-blocks-v4 HTTP/1.1\r\nHost: [::1]\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd6, req6, strlen(req6)), (ssize_t)strlen(req6));
  char buf6[512] = {0};
  ssize_t n6 = read(fd6, buf6, sizeof(buf6) - 1);
  close(fd6);
  fd6 = -1;
  REQUIRE_GT(n6, (ssize_t)0);
  REQUIRE_TRUE(strstr(buf6, "200") != NULL);

  /* The real assertion: a PLAIN IPv4 connection to the very same port should be
     refused outright and not quietly accepted by the same listener. With
     IPV6_V6ONLY applied no socket reachable over IPv4 is bound to this port at
     all, so test_connect() itself must fail right away with ECONNREFUSED, and
     must never just time out waiting for a response the way a regression's
     accepted but never routed connection would. */
  int fd4 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd4 >= 0);
  struct sockaddr_in sa4;
  memset(&sa4, 0, sizeof(sa4));
  sa4.sin_family = AF_INET;
  sa4.sin_port = htons((uint16_t)(TEST_PORT + 77));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa4.sin_addr), 1);
  int rc = test_connect(fd4, (struct sockaddr *)&sa4, sizeof(sa4));
  int connect_errno = errno;
  close(fd4);
  fd4 = -1;
  REQUIRE_EQ(rc, -1);
  REQUIRE_EQ(connect_errno, ECONNREFUSED);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*         HEAD METHOD + CARRY-OVER ALLOCATION-FAILURE TESTS                  */
/* ========================================================================== */

TEST(chttpserver, head_request_suppresses_response_body) {
  /* RFC 7231 SS4.3.2: a HEAD response reports the same header fields
     (Content-Length included) as a GET but must never send the message body.
     _send_response must not write conn->resp.body to the wire unconditionally,
     whatever conn->method is, or it would stream a body back for a HEAD request
     that reached a handler which writes one. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_ANY, "/head-body",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 18;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 18));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req = "HEAD /head-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Read only up to the separator between headers and body. */
  char buf[1024] = {0};
  size_t total = 0;
  char *hdr_end = NULL;
  while (total < sizeof(buf) - 1) {
    ssize_t r = read(fd, buf + total, sizeof(buf) - 1 - total);
    REQUIRE_GT(r, (ssize_t)0);
    total += (size_t)r;
    buf[total] = '\0';
    hdr_end = strstr(buf, "\r\n\r\n");
    if (hdr_end) break;
  }
  REQUIRE_TRUE(hdr_end != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);

  /* _hello_handler writes a non-empty "Hello, world!" body, and Content-Length
     still reports its real length even though it's never sent. */
  char *cl = strstr(buf, "content-length:");
  REQUIRE_TRUE(cl != NULL);
  unsigned long declared_len = strtoul(cl + 15, NULL, 10);
  REQUIRE_EQ(declared_len, (unsigned long)strlen("Hello, world!"));

  /* Nothing past the header terminator may already be in the buffer. A faulty
     server writes the header block and the body back to back (often in the same
     TCP segment or the very next one), so waiting for the next read() to time
     out isn't enough, because the body bytes may already be sitting in buf past
     hdr_end, from the very read() call(s) that found the header terminator. */
  REQUIRE_EQ(total, (size_t)(hdr_end - buf) + 4);

  /* No more body bytes may ever follow either, so a short poll() must see
     nothing else arrive. */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 200);
  REQUIRE_EQ(pr, 0);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
}

TEST(chttpserver, head_request_on_rejected_route_still_has_no_body) {
  /* _conn_reject_and_close builds resp with a bare memset to zero and never
     writes any body content onto it, for ANY reject_status, so resp.body_len is
     always 0 here whatever the request method. This call site passes
     conn->method == CHTTP_HEAD as _send_response's suppress_body argument
     rather than a hardcoded false. That argument can't change what reaches the
     wire: the early return of _send_response gated by no_body only skips a
     body-writing loop that runs zero times here anyway. Passing the real method
     through is still right, as defensive consistency with every other
     _send_response call site of this file, for the day a reject response grows
     a body, such as an error-detail message. But this test can neither drive
     nor check that specific wiring, since asserting that "the bytes on the wire
     differ between suppress_body=true and false" would be vacuous when they
     provably don't differ either way. So the test checks the real behaviour you
     can observe: a HEAD request to a rejected route gets back a well-formed 404
     with really no body. That response carries an explicit "content-length:0"
     header (404 is neither 204 nor 304, and a rejection the server writes
     itself reports the length of its own body, which is always 0 here, for HEAD
     as for GET), and no body byte of any kind after the CRLF that ends the
     header block, so it's not just "some text that contains 404". */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 19;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 19));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req = "HEAD /no-such-route HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024] = {0};
  size_t n = _drain_socket_until_eof(fd, buf, sizeof(buf));
  REQUIRE_GT(n, (size_t)0);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
  REQUIRE_TRUE(strcasestr(buf, "content-length:0") != NULL);

  const char *header_end = strstr(buf, "\r\n\r\n");
  REQUIRE_TRUE(header_end != NULL);
  REQUIRE_EQ((size_t)(buf + n - (header_end + 4)), (size_t)0);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
}

/* An allocator whose malloc, calloc and realloc fail exactly when the requested
   size equals g_fail_alloc_size (0 = never fail), and pass through otherwise.
   That lets a test target one specific allocation (here the copy that
   _conn_start_diverted makes of a pipelined request's leftover body bytes, the
   carry-over) without disturbing any other allocation the server makes while
   serving the same request. */
static _Atomic size_t g_fail_alloc_size = 0;

static void *_fail_at_size_malloc(size_t n) {
  if (n == g_fail_alloc_size) return NULL;
  return malloc(n);
}
static void _fail_at_size_free(void *p) { free(p); }
static void *_fail_at_size_calloc(size_t n, size_t s) {
  if (n * s == g_fail_alloc_size) return NULL;
  return calloc(n, s);
}
static void *_fail_at_size_realloc(void *p, size_t s) {
  if (s == g_fail_alloc_size) return NULL;
  return realloc(p, s);
}

TEST(chttpserver,
     carry_over_alloc_failure_rejects_gracefully_instead_of_crashing) {
  /* When a request's headers and the start of its body arrive in the same
     read() (the leftover, or carry-over, bytes past the header block),
     _conn_start_diverted must not set conn->_carry_over_len to the leftover
     length if the matching _ccol_mem_alloc for conn->_carry_over failed,
     because the worker thread would then call chttp1_stream_prepare with a NULL
     pointer and a non-zero length, which memcpy()s from that NULL pointer
     unconditionally and crashes. This test makes that exact allocation fail
     with a custom allocator and asserts that the server rejects the connection
     gracefully instead (500), and that the server (and the rest of this test
     process) is still alive and working afterwards. */
  size_t body_len = 6151; /* distinctive; unlikely to collide with any other
                           * allocation size this request triggers */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_POST, "/carry-oom",
                                               _echo_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 20;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 20));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  char *body = (char *)malloc(body_len);
  REQUIRE_TRUE(body != NULL);
  memset(body, 'x', body_len);

  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /carry-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n",
                    body_len);
  REQUIRE_GT(hn, 0);

  /* The headers and the whole body go into one buffer and one write() call, so
     the reactor's single read() sees the body bytes as "leftover" past the
     header block in the very call that triggers the diversion path under test.
     Only now is g_fail_alloc_size armed, so the server startup, the route
     registration and the connect above aren't affected by it. */
  char *wire = (char *)malloc((size_t)hn + body_len);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, head, (size_t)hn);
  memcpy(wire + hn, body, body_len);
  g_fail_alloc_size = body_len;
  ssize_t written = write(fd, wire, (size_t)hn + body_len);
  /* Left armed on success. The server side injects the fault asynchronously,
     once the worker thread processes these bytes, and the test can't observe it
     until the read() below returns the resulting response, which is where the
     real disarm happens. It's disarmed here only on the (rare) write() failure
     path, where nothing triggers that allocation afterwards; an allocator left
     armed there would affect every later test of this process once the
     REQUIRE_EQ below returns early. */
  if (written != (ssize_t)((size_t)hn + body_len)) g_fail_alloc_size = 0;
  free(wire);
  free(body);
  REQUIRE_EQ(written, (ssize_t)((size_t)hn + body_len));

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0; /* disarm before any further allocation anywhere */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  /* The server (and this process) must be fully usable afterwards: a fresh,
     ordinary request on a brand new connection and port has to succeed, which
     shows that the earlier allocation failure stayed inside that one request
     and didn't corrupt shared state. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-carry-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 21;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  sa.sin_port = htons((uint16_t)(TEST_PORT + 21));
  REQUIRE_EQ(test_connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
  setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req2 =
      "GET /after-carry-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd2, req2, strlen(req2)), (ssize_t)strlen(req2));
  char buf2[512] = {0};
  ssize_t n2 = read(fd2, buf2, sizeof(buf2) - 1);
  REQUIRE_GT(n2, (ssize_t)0);
  buf2[n2] = '\0';
  REQUIRE_TRUE(strstr(buf2, "HTTP/1.1 200") != NULL);

  close(fd2);
  fd2 = -1;
  chttpsvr_destroy(srv2);
}

/* A real allocation failure in _on_body while it grows a streaming route's
   body-accumulation buffer must NOT reach the handler as
   ccol_http_transfer_aborted ("connection closed or malformed framing") through
   chttpsvr_req_stream_error(). That outcome can't be told apart from a real
   dropped connection or malformed chunk framing, although nothing is wrong with
   the peer or its framing at all, so it would misclassify a server-side OOM as
   a transfer error caused by the client. So this particular failure has its own
   flag (conn->body_alloc_failed) and its own, separate
   chttpsvr_req_stream_error() outcome (ccol_not_enough_memory), and a handler
   that reacts differently to "the peer misbehaved" and "the server is out of
   memory" (logging or alerting differently, or retrying a downstream call only
   in the first case, for example) can tell the two apart.

   The test targets the very first growth of the body buffer, which always asks
   for exactly 8192 bytes however much body data arrived (conn->body starts with
   cap == 0, and on that first call _body_grow_target takes "cap ? cap :
   start", where start is 8192 for a streaming route), so a tiny, distinctive
   body is enough to trigger it and the test doesn't have to send anywhere
   near 8192 bytes. */
TEST(chttpserver,
     streaming_body_buffer_alloc_failure_reports_not_enough_memory) {
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/body-buf-oom", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 82;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 82));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "hi";
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /body-buf-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n",
                    strlen(body));
  REQUIRE_GT(hn, 0);

  /* Armed only now, once chttpsvr_start(), the route registration and the
     connect are done, so none of their allocations are affected. 8192 matches
     _on_body's first growth target exactly (see this test's comment above). A
     write() failure disarms it on its own path, just like the other
     g_fail_alloc_size tests of this file. */
  g_fail_alloc_size = 8192;
  ssize_t written_hdr = write(fd, head, (size_t)hn);
  ssize_t written_body =
      (written_hdr == hn) ? write(fd, body, strlen(body)) : -1;
  if (written_hdr != hn || written_body != (ssize_t)strlen(body))
    g_fail_alloc_size = 0;
  REQUIRE_EQ(written_hdr, hn);
  REQUIRE_EQ(written_body, (ssize_t)strlen(body));

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0; /* disarm before any further allocation anywhere */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_not_enough_memory") != NULL);
}

/* _on_header: an allocation failure while it copies a header's name or value
   (before routing runs) must not return 1 (which aborts the parse with
   CHTTP1_USER) without also setting conn->req_rejected and reject_status.
   Without them _conn_feed_bytes drops into its "else" branch and silently
   closes the connection with zero response bytes, unlike the identical OOM
   failure that _on_headers_complete handles gracefully (a 500 through
   reject_pool) a few callbacks later. The test makes that exact allocation fail
   with the same fail-at-size custom allocator as the carry-over OOM test above
   and asserts that the server delivers a graceful 500 and not a bare closed
   connection. */
TEST(
    chttpserver,
    on_header_alloc_failure_rejects_gracefully_instead_of_dropping_connection) {
  size_t value_strlen = 6201; /* distinctive; value_strlen+1 targets the
                               * header value allocation specifically */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdr-oom",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 27;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 27));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req_head = "GET /hdr-oom HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Big: ";
  const char *req_tail = "\r\n\r\n";
  size_t total = strlen(req_head) + value_strlen + strlen(req_tail);
  char *wire = (char *)malloc(total + 1);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, req_head, strlen(req_head));
  memset(wire + strlen(req_head), 'y', value_strlen);
  memcpy(wire + strlen(req_head) + value_strlen, req_tail, strlen(req_tail));
  wire[total] = '\0';

  g_fail_alloc_size = value_strlen + 1;
  ssize_t written = write(fd, wire, total);
  /* Left armed on success; the same write() guard in
     carry_over_alloc_failure_rejects_gracefully_instead_of_crashing has the
     full reasoning. It's disarmed here only on the (rare) write() failure path.
 */
  if (written != (ssize_t)total) g_fail_alloc_size = 0;
  REQUIRE_EQ(written, (ssize_t)total);
  free(wire);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0; /* disarm before any further allocation anywhere */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;

  /* The server (and this process) is fully usable afterwards: an
     ordinary request on a fresh connection must succeed, which shows that the
     earlier allocation failure stayed inside that one request and didn't
     corrupt shared state. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  REQUIRE_EQ(test_connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
  setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req2 =
      "GET /hdr-oom HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  REQUIRE_EQ(write(fd2, req2, strlen(req2)), (ssize_t)strlen(req2));
  char buf2[512] = {0};
  ssize_t n2 = read(fd2, buf2, sizeof(buf2) - 1);
  REQUIRE_GT(n2, (ssize_t)0);
  buf2[n2] = '\0';
  REQUIRE_TRUE(strstr(buf2, "HTTP/1.1 200") != NULL);

  close(fd2);
  fd2 = -1;
  chttpsvr_destroy(srv);
}

/* The same contract as the test just above, here in _on_request_line: an
   allocation failure while it copies the raw (still percent-encoded) request
   path, which happens before any route matches at all, must not abort the parse
   with CHTTP1_USER without also setting conn->req_rejected and reject_status,
   or the connection drops silently instead of getting a graceful 500. */
TEST(
    chttpserver,
    on_request_line_path_alloc_failure_rejects_gracefully_instead_of_dropping_connection) {
  size_t path_len = 6221; /* distinctive; path_len+1 targets the raw-path
                           * allocation _on_request_line makes before any
                           * routing has run */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/after-path-oom", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 28;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 28));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req_head = "GET /";
  const char *req_tail = " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  size_t path_body_len = path_len - 1; /* excludes the leading '/' already
                                        * in req_head */
  size_t total = strlen(req_head) + path_body_len + strlen(req_tail);
  char *wire = (char *)malloc(total + 1);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, req_head, strlen(req_head));
  memset(wire + strlen(req_head), 'p', path_body_len);
  memcpy(wire + strlen(req_head) + path_body_len, req_tail, strlen(req_tail));
  wire[total] = '\0';

  g_fail_alloc_size = path_len + 1;
  ssize_t written = write(fd, wire, total);
  /* Left armed on success; the same write() guard in
     carry_over_alloc_failure_rejects_gracefully_instead_of_crashing has the
     full reasoning. It's disarmed here only on the (rare) write() failure path.
 */
  if (written != (ssize_t)total) g_fail_alloc_size = 0;
  REQUIRE_EQ(written, (ssize_t)total);
  free(wire);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0; /* disarm before any further allocation anywhere */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;

  /* Usable afterwards too, just like in the sibling _on_header test above. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  REQUIRE_EQ(test_connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
  setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req2 =
      "GET /after-path-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd2, req2, strlen(req2)), (ssize_t)strlen(req2));
  char buf2[512] = {0};
  ssize_t n2 = read(fd2, buf2, sizeof(buf2) - 1);
  REQUIRE_GT(n2, (ssize_t)0);
  buf2[n2] = '\0';
  REQUIRE_TRUE(strstr(buf2, "HTTP/1.1 200") != NULL);

  close(fd2);
  fd2 = -1;
  chttpsvr_destroy(srv);
}

/* chttpsvr_req_query_oom() is documented (chttpserver.h) to latch true once
   allocating chttpsvr_req_query()'s result array fails under memory pressure,
   and to stay true "for the lifetime of the request regardless of later query
   calls". This is the only test that triggers a real OOM on that path and
   checks the flag (the rest of its coverage only exercises the trivial NULL-req
   guard), so without it a regression that broke the latch (never setting it, or
   resetting it on a later successful call) would pass the whole suite
   unnoticed. */
static _Atomic bool g_query_oom_flag_right_after_failure = false;
static _Atomic bool g_query_oom_flag_after_later_success = false;
static _Atomic size_t g_query_oom_failed_call_count = (size_t)-1;
static _Atomic size_t g_query_oom_later_call_count = (size_t)-1;
static _Atomic bool g_query_oom_failed_call_result_is_null = false;

#define QUERY_OOM_REPEATED_KEY_COUNT 91 /* distinctive result-array size */

static void _query_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  /* Warm up: make _ensure_qparams parse the whole query string BEFORE the
     allocator below is armed, so the fault injection hits ONLY the _qresult
     realloc of chttpsvr_req_query and none of _ensure_qparams's internal
     parsing allocations (which could happen to need the very same byte count).
 */
  size_t warmup_n = 0;
  chttpsvr_req_query(req, "does-not-exist", &warmup_n);

  /* +1 for the NULL-terminator slot that chttpsvr_req_query always reserves. */
  g_fail_alloc_size = (QUERY_OOM_REPEATED_KEY_COUNT + 1) * sizeof(char *);
  size_t n = (size_t)-1;
  const char **vals = chttpsvr_req_query(req, "a", &n);
  g_fail_alloc_size = 0; /* disarm right after the one targeted call */

  atomic_store(&g_query_oom_failed_call_result_is_null, vals == NULL);
  atomic_store(&g_query_oom_failed_call_count, n);
  atomic_store(&g_query_oom_flag_right_after_failure,
               chttpsvr_req_query_oom(req));

  /* A second, ordinary query call afterwards that doesn't fail, for a different
     key: the flag has to stay latched true whatever happens, as its documented
     "for the lifetime of the request" contract says. */
  size_t n2 = (size_t)-1;
  const char **vals2 = chttpsvr_req_query(req, "b", &n2);
  atomic_store(&g_query_oom_later_call_count,
               (vals2 && n2 == 1) ? n2 : (size_t)-1);
  atomic_store(&g_query_oom_flag_after_later_success,
               chttpsvr_req_query_oom(req));

  chttpsvr_resp_write_str(resp, "ok");
}

TEST(chttpserver, req_query_oom_latches_true_across_later_successful_calls) {
  g_query_oom_flag_right_after_failure = false;
  g_query_oom_flag_after_later_success = false;
  g_query_oom_failed_call_count = (size_t)-1;
  g_query_oom_later_call_count = (size_t)-1;
  g_query_oom_failed_call_result_is_null = false;

  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/query-oom",
                                               _query_oom_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 76;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Build "?a=1&a=2&...&a=91&b=only", so that key "a" occurs exactly
     QUERY_OOM_REPEATED_KEY_COUNT times (which forces the targeted realloc size
     above) and key "b" exactly once, for the call after the failure. */
  char query[2048] = {0};
  size_t qoff = 0;
  for (int i = 0; i < QUERY_OOM_REPEATED_KEY_COUNT; i++) {
    int w = snprintf(query + qoff, sizeof(query) - qoff, "a=%d&", i);
    REQUIRE_GT(w, 0);
    qoff += (size_t)w;
  }
  int w = snprintf(query + qoff, sizeof(query) - qoff, "b=only");
  REQUIRE_GT(w, 0);

  char req_line[2200];
  int rn = snprintf(req_line, sizeof(req_line),
                    "GET /query-oom?%s HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Connection: close\r\n\r\n",
                    query);
  REQUIRE_GT(rn, 0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 76));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(write(fd, req_line, (size_t)rn), (ssize_t)rn);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(fd);
  fd = -1;
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);

  REQUIRE_TRUE(g_query_oom_failed_call_result_is_null);
  REQUIRE_EQ(g_query_oom_failed_call_count, (size_t)0);
  REQUIRE_TRUE(g_query_oom_flag_right_after_failure);
  /* The real assertion: the flag is still latched true after a LATER query call
     for an unrelated key that really succeeds. */
  REQUIRE_EQ(g_query_oom_later_call_count, (size_t)1);
  REQUIRE_TRUE(g_query_oom_flag_after_later_success);

  chttpsvr_destroy(srv);
}

/* The documented ccol_not_enough_memory return of chttpsvr_req_query_one
   (chttpserver.h) is covered only here. The OOM coverage above exercises the
   flag-based contract of chttpsvr_req_query and chttpsvr_req_query_oom and
   never chttpsvr_req_query_one's direct return-value contract, which is a
   structurally different code path (its own `if (!qp) return
   ccol_not_enough_memory; if (req->_qparams_parse_oom) return
   ccol_not_enough_memory;` checks, both ahead of any "was the key even found"
   logic). Nothing else in this suite would catch a regression that merged those
   two checks into the ordinary ccol_key_not_found path. */
#define QUERY_ONE_OOM_KEY_LEN                    \
  700 /* forces _parse_qparams's own per-pair    \
         key_decoded allocation onto the heap    \
         with a distinctive, unlikely-to-collide \
         byte count */

static _Atomic int g_query_one_oom_rv = -999;

static void _query_one_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  /* chttpsvr_req_query_one is the very first query accessor this request calls,
     so _ensure_qparams's lazy first-call parse of the whole query string runs
     synchronously inside this armed window. Nothing else on this thread has any
     reason to need the same distinctive byte count, so the fault hits only
     _parse_qparams's key_decoded allocation for this request's single,
     deliberately long query key. */
  g_fail_alloc_size = QUERY_ONE_OOM_KEY_LEN + 1;
  /* Poisoned with a non-NULL sentinel first, not left at NULL. A bare NULL
     starting value can't tell "the OOM path resets *val_out" from "it never
     touched a local that was already NULL", so a regression that never touches
     it would look the same (see the header doc comment of
     chttpsvr_req_query_one: every failure return, OOM included, leaves
     *val_out reset to NULL). */
  const char *val = (const char *)0xdeadbeefUL;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "irrelevant-key", &val);
  g_fail_alloc_size = 0;
  atomic_store(&g_query_one_oom_rv, (int)rv);
  chttpsvr_resp_write_str(resp, val ? "unexpected-value" : "ok");
}

TEST(chttpserver, req_query_one_reports_not_enough_memory_not_key_not_found) {
  g_query_one_oom_rv = -999;

  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/query-one-oom",
                                               _query_one_oom_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 78;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char key[QUERY_ONE_OOM_KEY_LEN + 1];
  memset(key, 'k', QUERY_ONE_OOM_KEY_LEN);
  key[QUERY_ONE_OOM_KEY_LEN] = '\0';

  char req_line[QUERY_ONE_OOM_KEY_LEN + 256];
  int rn = snprintf(req_line, sizeof(req_line),
                    "GET /query-one-oom?%s=v HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Connection: close\r\n\r\n",
                    key);
  REQUIRE_GT(rn, 0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 78));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(write(fd, req_line, (size_t)rn), (ssize_t)rn);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  close(fd);
  fd = -1;
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "unexpected-value") == NULL);

  REQUIRE_EQ(g_query_one_oom_rv, (int)ccol_not_enough_memory);

  chttpsvr_destroy(srv);
}

/* Unlike the carry-over copy above (which follows this module's
   custom-allocator convention, so _conn_start_diverted rejects it gracefully
   before the diversion), _task_worker's OWN chttp1_stream_prepare()/_tls() call
   (a second, separate copy of the same carry-over bytes, made on the worker
   thread) asks the server's allocator (not plain malloc()) for the same byte
   count as that first copy, so a fail-at-size allocator always fails the first
   copy instead. _chttpsvr_force_stream_prepare_fail_for_tests() drives that
   same failure path deterministically. If every response path in _task_worker
   were gated on `prepared`, a buffered route would close the connection with
   zero response bytes (the intended 500 is set but never sent), and a streaming
   route would run the handler against a NULL req->stream, where
   chttpsvr_req_stream_error() falsely reports ccol_success instead of the real
   failure. This test drives that exact case against a streaming route and
   asserts that the connection gets a graceful 500 instead and that the handler
   never runs at all (no x-stream-err header, which only the handler itself
   sets). */
TEST(chttpserver, stream_prepare_failure_in_worker_sends_500_not_bare_close) {
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/stream-prepare-oom", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 22;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 22));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  /* The headers and the body go into one write() call, so the reactor's single
     read() sees the body bytes as carry-over past the header block, exactly as
     in the allocation-failure test above. Only now is the worker thread's
     prepare forced to fail, so the server startup, the route registration and
     the connect above aren't affected by it. */
  const char *body = "hello";
  char wire[256];
  int wn = snprintf(wire, sizeof(wire),
                    "POST /stream-prepare-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n%s",
                    strlen(body), body);
  REQUIRE_GT(wn, 0);

  _chttpsvr_force_stream_prepare_fail_for_tests(true);
  ssize_t written = write(fd, wire, (size_t)wn);
  /* Left armed on success. The server side injects the fault asynchronously,
     once the worker thread processes these bytes, and the test can't observe it
     until the read() below returns the resulting response, which is where the
     real disarm happens. It's disarmed here only on the (rare) write() failure
     path, where nothing triggers that path afterwards; a hook left armed there
     would affect every later test of this process once the REQUIRE_EQ below
     returns early. */
  if (written != (ssize_t)wn)
    _chttpsvr_force_stream_prepare_fail_for_tests(false);
  REQUIRE_EQ(written, (ssize_t)wn);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  _chttpsvr_force_stream_prepare_fail_for_tests(false); /* disarm first */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);
  /* The handler never ran, since it's the only thing that sets this header. */
  REQUIRE_TRUE(strstr(buf, "x-stream-err") == NULL);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  /* The server (and this process) should be fully usable afterwards. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-stream-prepare-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 23;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  sa.sin_port = htons((uint16_t)(TEST_PORT + 23));
  REQUIRE_EQ(test_connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
  setsockopt(fd2, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  const char *req2 =
      "GET /after-stream-prepare-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd2, req2, strlen(req2)), (ssize_t)strlen(req2));
  char buf2[512] = {0};
  ssize_t n2 = read(fd2, buf2, sizeof(buf2) - 1);
  REQUIRE_GT(n2, (ssize_t)0);
  buf2[n2] = '\0';
  REQUIRE_TRUE(strstr(buf2, "HTTP/1.1 200") != NULL);

  close(fd2);
  fd2 = -1;
  chttpsvr_destroy(srv2);
}

/* ========================================================================== */
/*   204/304 BODY SUPPRESSION AND 1xx FINAL STATUSES (RFC 9110 SS6.4.1/15) */
/* ========================================================================== */

TEST(chttpserver, response_204_never_sends_body_or_content_length) {
  /* _send_response's body suppression must cover a 204 as well as HEAD, since
     a 204 can never carry a body either, whatever the method (RFC 9110
     SS15.3.5). Suppressing only for suppress_body (HEAD) would stream the
     36-byte body of /status-with-body back as it is on a plain GET whose
     handler sets 204 after writing the body. Any client that correctly treats
     204 as bodyless (including this library's chttpclient parser; see the
     CHTTP1_ST_HEADERS handling in chttp1_parser.c) would then misparse that
     leaked body as the start of the next pipelined response on a keep-alive
     connection. A 204 also MUST NOT carry a Content-Length at all (RFC 9110
     SS6.4.1), unlike HEAD and 304, where one is expected or allowed. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 204\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 204);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

/* Counts how many times needle occurs in the header block of the response in
   buf, which ends at the first empty line. */
static int _count_in_head(const char *buf, const char *needle) {
  const char *end = strstr(buf, "\r\n\r\n");
  size_t len = end ? (size_t)(end - buf) : strlen(buf);
  int count = 0;
  size_t nlen = strlen(needle);
  for (const char *p = buf; (p = strstr(p, needle)) != NULL && p < buf + len;
       p += nlen)
    count++;
  return count;
}

TEST(chttpserver, response_304_carries_no_content_length_of_its_own) {
  /* The same body-suppression contract as
     response_204_never_sends_body_or_content_length, for 304 (RFC 9110
     SS15.4.5). A 304 describes the stored representation, not this message, and
     a Content-Length there equals that representation's length (RFC 9110
     SS8.6). The server doesn't know that length, so it writes none, because a
     "content-length:0" would tell a cache that the representation is empty.
     A server that computed the field from the body the handler wrote would
     send "content-length:35" here. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 304\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 304);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, response_304_sends_the_content_length_that_the_handler_set) {
  /* A handler that knows the representation's length sets it, and the 304
     carries exactly that value, once, and still no body. If the server
     filtered out every Content-Length a handler sets, it would send either no
     field or the length of the body the handler wrote. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 304\r\nx-force-cl: 4321\r\n", buf,
                            sizeof(buf));
  REQUIRE_EQ(status, 304);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:4321\r\n") != NULL);
  REQUIRE_EQ(_count_in_head(buf, "ontent-"), 1);
  const char *end = strstr(buf, "\r\n\r\n");
  REQUIRE_TRUE(end != NULL);
  REQUIRE_EQ(strlen(end + 4), (size_t)0);
}

TEST(chttpserver,
     response_with_a_body_ignores_the_content_length_of_the_handler) {
  /* Where a body goes out its length is the framing, whatever the handler set,
     so the 200 carries the 35 bytes the handler wrote and a content-length of
     35. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body", "x-force-cl: 5\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "content-length:35\r\n") != NULL);
  REQUIRE_EQ(_count_in_head(buf, "ontent-"), 1);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\nthis-body-must-never-reach-the-wire") !=
               NULL);
}

TEST(chttpserver, explicit_head_route_without_a_body_invents_no_length) {
  /* A HEAD route that writes no body answers for a representation that isn't in
     the message. Without a length from the handler the response carries no
     Content-Length at all, like Go's net/http server, and not a
     "content-length:0" that would describe an empty representation.
     A server that always computed the field from the body the handler wrote
     would send "content-length:0" here. */
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/head-explicit", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-explicit-head:1") != NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, explicit_head_route_sends_the_content_length_of_the_handler) {
  /* The same HEAD route with a length from the handler carries that length,
     once, and no body, where a server that filtered out every Content-Length
     a handler sets would send "content-length:0". */
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/head-explicit", "x-force-cl: 98765\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "content-length:98765\r\n") != NULL);
  REQUIRE_EQ(_count_in_head(buf, "ontent-"), 1);
  const char *end = strstr(buf, "\r\n\r\n");
  REQUIRE_TRUE(end != NULL);
  REQUIRE_EQ(strlen(end + 4), (size_t)0);
}

TEST(chttpserver, resp_add_header_fields_reach_the_client_one_by_one) {
  /* This library's client sees every Set-Cookie field that
     chttpsvr_resp_add_header added, in call order, and a single x-single field
     after chttpsvr_resp_set_header collapsed the handler's three. An
     add_header that replaced would leave one Set-Cookie, and a set_header that
     replaced only the first match would leave x-single with three fields.
 */
  chttpcli_response *resp = _get("/add-header");
  REQUIRE_TRUE(resp != NULL);
  int status = resp->status_code;
  size_t cookies = chttpclient_resp_header_count(resp, "set-cookie");
  const char *c0 = chttpclient_resp_header_at(resp, "set-cookie", 0);
  const char *c1 = chttpclient_resp_header_at(resp, "set-cookie", 1);
  const char *c2 = chttpclient_resp_header_at(resp, "set-cookie", 2);
  bool order = c0 && c1 && c2 && strcmp(c0, "a=1") == 0 &&
               strcmp(c1, "b=2") == 0 && strcmp(c2, "c=3") == 0;
  size_t singles = chttpclient_resp_header_count(resp, "x-single");
  const char *single = chttpclient_resp_header(resp, "x-single");
  bool single_final = single && strcmp(single, "final") == 0;
  size_t dates = chttpclient_resp_header_count(resp, "date");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(cookies, (size_t)3);
  REQUIRE_TRUE(order);
  REQUIRE_EQ(singles, (size_t)1);
  REQUIRE_TRUE(single_final);
  REQUIRE_EQ(dates, (size_t)1);
}

TEST(chttpserver, resp_header_content_length_accepts_only_a_number) {
  /* Both header calls accept one or more ASCII digits whose number fits in a
     signed 64-bit integer, leading zeros included, and refuse every other value
     with ccol_invalid_args. Without the check, both calls would accept every
     value of the list. */
  chttpcli_response *resp = _get("/content-length-validation");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "yyyyyyyynnnnnnnnnnnnnnnnyy");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, resp_add_header_keeps_every_field_in_order) {
  /* chttpsvr_resp_add_header puts one more field on the wire per call, in call
     order, so a response can carry several Set-Cookie lines.
     chttpsvr_resp_set_header replaces every field of its name with a single
     one, at the place of the first. Connection, Content-Length and Date stay
     single whichever call sets them. Without add_header this doesn't link, with
     an add_header that replaced only one Set-Cookie would arrive, and with a
     set_header that replaced only the first match three x-single lines would
     arrive. */
  char buf[4096] = {0};
  int status = _raw_request("GET", "/add-header", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "bad=0") != NULL);
  const char *a = strstr(buf, "Set-Cookie:a=1\r\n");
  const char *x = strstr(buf, "x-single:final\r\n");
  const char *b = strstr(buf, "set-cookie:b=2\r\n");
  const char *c = strstr(buf, "Set-Cookie:c=3\r\n");
  const char *d = strstr(buf, "Date:d2\r\n");
  REQUIRE_TRUE(a != NULL);
  REQUIRE_TRUE(x != NULL);
  REQUIRE_TRUE(b != NULL);
  REQUIRE_TRUE(c != NULL);
  REQUIRE_TRUE(d != NULL);
  REQUIRE_TRUE(a < x);
  REQUIRE_TRUE(x < b);
  REQUIRE_TRUE(b < c);
  REQUIRE_TRUE(c < d);
  REQUIRE_EQ(_count_in_head(buf, "ookie:"), 3);
  REQUIRE_EQ(_count_in_head(buf, "x-single:"), 1);
  REQUIRE_EQ(_count_in_head(buf, "ate:"), 1);
  REQUIRE_EQ(_count_in_head(buf, "x-injected"), 0);
  REQUIRE_EQ(_count_in_head(buf, "ransfer-"), 0);
}

TEST(chttpserver, a_1xx_final_status_goes_out_as_500) {
  /* A 1xx is interim (RFC 9110 SS15.2), and a client that gets one as the only
     response keeps waiting for the final one. So a handler that sets a 1xx as
     its final status gets 500, just like for a status outside the range, with
     its body framed by a Content-Length, so the client gets a complete response
     without waiting.

     If the server sent the 1xx itself, the answer would be 199 with no body
     and no Content-Length. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 199\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 500);
  REQUIRE_TRUE(strstr(buf, "content-length:") != NULL);
}

TEST(chttpserver, large_response_headers_still_delivered_in_full) {
  /* _send_response must not build the response's header block in a fixed
     4096-byte stack buffer with no fallback, because a response whose headers
     alone go past that size would then silently lose the ENTIRE response, with
     no graceful 500 and no truncated write, just a bare close of the connection
     with zero bytes written and nothing logged. /large-response-headers sets 20
     headers of about 300 bytes each (about 6.2 KiB of header block, well past a
     4096-byte cap), and every one of them, the 200 status and the body all have
     to reach the client intact. */
  char buf[8192] = {0};
  int status =
      _raw_request("GET", "/large-response-headers", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "large-headers-ok") != NULL);
  char expect_val[300];
  memset(expect_val, 'x', sizeof(expect_val) - 1);
  expect_val[sizeof(expect_val) - 1] = '\0';
  for (int i = 0; i < 20; i++) {
    char expect_line[340];
    snprintf(expect_line, sizeof(expect_line), "x-custom-%02d:%s", i,
             expect_val);
    REQUIRE_TRUE(strstr(buf, expect_line) != NULL);
  }
}

TEST(chttpserver,
     explicit_connection_close_header_ignored_when_actually_kept_alive) {
  /* _send_response must not emit a handler's "Connection" response header as it
     is, independently of the keep_alive value _task_worker uses afterwards to
     decide whether the connection stays open. Here a handler explicitly sets
     "Connection: close" although nothing about this ordinary HTTP/1.1 request
     gives the server a real reason to close (fully parsed, no body_too_large,
     no abort). So the real decision is keep-alive, the wire has to say so (and
     not whatever the handler set), and the connection needs to be usable for a
     second request right afterwards. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /explicit-connection-header HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "x-force-connection: close\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024] = {0};
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "explicit-connection-header-ok") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:keep-alive") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") == NULL);

  /* The connection is still alive: a second request on the same socket
     is expected to succeed without hanging or getting reset. */
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
  memset(buf, 0, sizeof(buf));
  int status2 = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status2, 200);
  REQUIRE_TRUE(strstr(buf, "explicit-connection-header-ok") != NULL);
  close(fd);
  fd = -1;
}

TEST(chttpserver,
     explicit_connection_keepalive_header_ignored_when_actually_closing) {
  /* The other direction of the same contract. A handler explicitly sets
     "Connection: keep-alive" on an HTTP/1.0 request that has no "Connection:
     keep-alive" token of its own, so chttp1_should_keep_alive() correctly
     reports that this connection can't be reused, whatever the handler wants.
     The wire must say "close" (matching what the server does), never the
     value the handler set. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /explicit-connection-header HTTP/1.0\r\n"
      "Host: 127.0.0.1\r\n"
      "x-force-connection: keep-alive\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "explicit-connection-header-ok") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:keep-alive") == NULL);

  /* The read loop above stops only on EOF (r <= 0) and would block forever on a
     keep-alive connection that is still open, so getting here proves that the
     server closed its end. */
  close(fd);
  fd = -1;
}

/* ========================================================================== */
/*         CHTTPSVR HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)          */
/* ========================================================================== */

/*
 * chttpsvr is a value handle with a generation tag. It holds a slot index and a
 * generation, and the library resolves it through a slot table it owns before
 * anything touches the struct chttpserver* underneath (see the "CHTTPSVR HANDLE
 * SLOT TABLE" section of src/chttpserver.c). chttpclient.c uses exactly the
 * same mechanism for its own chttpcli handle, for the same reason. Without it
 * __chttpsvr_destroy would have no protection at all against a second run on
 * the same handle, whether that run comes afterwards or at the same time, and
 * either one is a real double free. This section tests that design directly.
 */

static void _noop_middleware_for_lifecycle_tests(chttpsvr_req *req,
                                                 chttpsvr_resp *resp, void *ctx,
                                                 chttpsvr_next_fn next) {
  (void)ctx;
  if (next) next(req, resp);
}

/* After a destroy that has finished completely, a second destroy call on a
 * separate copy of the same original handle value is a fatal error. The
 * test runs in a forked child, because _ccol_fatal_err aborts the whole
 * process; tests/clogger/tests.c is the precedent for such a fork test in this
 * codebase. Every server in this section takes a NULL logger instead of
 * g_test_logger, so chttpsvr_create uses its own internal logger, which writes
 * only FATAL messages to stderr, and these tests depend on none of the
 * process-wide shared test server and logger state that _setup and _teardown
 * manage. */
/* chttpsvr_req_stream_error() with no request reports ccol_invalid_args, like
   every other call of this API does for a NULL argument. */
TEST(chttpserver, stream_error_of_no_request_is_invalid_args) {
  REQUIRE_EQ((int)chttpsvr_req_stream_error(NULL), (int)ccol_invalid_args);
}

/* The library never hands a server's root router to a caller, so its shell goes
   away with the server, and a create and destroy cycle leaves no router shell
   behind. Every sub-router keeps its small shell for the life of the process,
   so that a stale chttpsvr_router pointer stays safe to pass. A root shell that
   stayed allocated would add one entry to the shell registry for every server
   ever created. */
/* A parent logger that's already closed is a caller error, and the create call
 * reports it through err_str. clog_derive() would end the process for the same
 * handle, so without the checked derive this test would die with clog_derive's
 * fatal error. A live parent still gives a working server. */
TEST(chttpsvr_handle_lifecycle, create_with_a_closed_logger_fails_cleanly) {
  clog parent = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_NE(parent, CLOG_INVALID);
  clog stale = parent;
  clog_close(parent);

  char *err = NULL;
  chttpsvr srv = chttpsvr_create(stale, &err);
  REQUIRE_EQ(srv, CHTTPSVR_INVALID);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "the parent logger is not open"), NULL);

  clog live = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_NE(live, CLOG_INVALID);
  err = NULL;
  chttpsvr ok = chttpsvr_create(live, &err);
  bool created = ok != CHTTPSVR_INVALID;
  if (created) chttpsvr_destroy(ok);
  clog_close(live);
  REQUIRE_TRUE(created);
}

TEST(chttpsvr_handle_lifecycle, a_destroyed_server_keeps_no_root_router) {
  size_t before = 0, after_plain = 0, after_sub = 0;
  bool array = false;
  _chttpsvr_router_shell_registry_state_for_tests(&before, &array);
  bool created = true;
  for (int i = 0; i < 20; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) {
      created = false;
      continue;
    }
    chttpsvr_destroy(srv);
  }
  _chttpsvr_router_shell_registry_state_for_tests(&after_plain, &array);
  bool subrouted = true;
  for (int i = 0; i < 5; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) {
      created = false;
      continue;
    }
    if (!chttpsvr_subrouter(srv, "/api")) subrouted = false;
    chttpsvr_destroy(srv);
  }
  _chttpsvr_router_shell_registry_state_for_tests(&after_sub, &array);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(subrouted);
  REQUIRE_EQ(after_plain, before);
  REQUIRE_EQ(after_sub, before + 5);
}

TEST(chttpsvr_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    chttpsvr stale = srv;  /* an independently-held copy of the handle value,
         distinct from the local the macro below NULLs out */
    chttpsvr_destroy(srv); /* completes normally; the local `srv` is now
        CHTTPSVR_INVALID, but `stale` still holds the original value */
    __chttpsvr_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check waitpid's return value before trusting status. An EINTR failure, or
   * any other one, leaves status at its initial 0, and without this check we
   * would quietly read that as "the child exited normally with status 0" when
   * waitpid never actually reported this child's real outcome, which gives a
   * confusing WIFSIGNALED failure instead of a clear one. Every later, more
   * elaborate fork test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

typedef struct {
  chttpsvr h;
} concurrent_svr_destroy_arg_t;

static void *concurrent_svr_destroy_thread(void *arg) {
  concurrent_svr_destroy_arg_t *a = (concurrent_svr_destroy_arg_t *)arg;
  __chttpsvr_destroy(a->h);
  return NULL;
}

/* Two threads that each call destroy on their own copy of the SAME, still valid
 * handle, as close to the same moment as the test can manage, has to be fatal
 * as well: this is the heap double free that the generation-tagged slot table
 * is there to turn into a failure the library detects and reports loudly. */
TEST(chttpsvr_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    concurrent_svr_destroy_arg_t a1 = {.h = srv};
    concurrent_svr_destroy_arg_t a2 = {.h = srv};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, concurrent_svr_destroy_thread, &a1);
    pthread_create(&t2, NULL, concurrent_svr_destroy_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit _ccol_fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check waitpid's return value before trusting status. An EINTR failure, or
   * any other one, leaves status at its initial 0, and without this check we
   * would quietly read that as "the child exited normally with status 0" when
   * waitpid never actually reported this child's real outcome, which gives a
   * confusing WIFSIGNALED failure instead of a clear one. Every later, more
   * elaborate fork test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A request handler, or a middleware, can destroy the very server whose worker
 * pool is running it, and that must be a fatal error, just like a second
 * destroy on a stale handle; the comment of chttpsvr_worker_key_bundle in
 * chttpserver.c describes the real hazard this guards against. Without the
 * guard this call hangs for about a minute waiting for its own in-flight
 * request to finish and then aborts from deep inside cthreadpool.c's unrelated
 * self-destroy guard, and if it races a concurrent chttpsvr_engine_stop() for
 * the same server it deadlocks for good instead.
 *
 * The test drives the case directly with
 * _chttpsvr_mark_self_as_worker_for_tests instead of a real end-to-end request
 * dispatched by a real worker thread. It needs fork() either way, because
 * _ccol_fatal_err() aborts the whole process, and a real chttpsvr_start() on
 * top of that fork would need the forked child's shared reactor to service a
 * new listener registration. fork(2) duplicates no thread other than the
 * calling one, and by this point this suite's shared reactor already runs real
 * OS threads, because hundreds of earlier tests started servers, so a child
 * this suite forks inherits a hollow reactor handle that nothing ever services.
 * A chttpsvr_start() call after the fork would hang there forever instead of
 * aborting, with every thread idle and the listener never dispatched once. The
 * white-box hook avoids that fork and reactor interaction completely while
 * driving exactly the same comparison that __chttpsvr_destroy itself makes. */
TEST(chttpsvr_handle_lifecycle, destroy_from_within_own_handler_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    __chttpsvr_destroy(srv); /* the actual misuse under test */
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check waitpid's return value before trusting status. An EINTR failure, or
   * any other one, leaves status at its initial 0, and without this check we
   * would quietly read that as "the child exited normally with status 0" when
   * waitpid never actually reported this child's real outcome, which gives a
   * confusing WIFSIGNALED failure instead of a clear one. Every later, more
   * elaborate fork test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A request handler, or a middleware, can block in chttpsvr_engine_wait(), and
 * that must be a fatal error too, for the same underlying reason as a handler
 * that destroys its own server from inside itself (see the previous test). The
 * ctpool_shutdown_drain of _engine_force_stop_quiesce_all has no timeout and
 * could never finish draining this very worker's pool, because this call stack
 * is what would finally let the task return, so the shared engine could never
 * finish its teardown and this call could never wake up: a permanent deadlock
 * of the whole engine, not just one stuck server. A caller gets there through
 * exactly the pattern chttpserver.h itself documents for an administration or
 * shutdown endpoint, which calls chttpsvr_engine_stop() and then
 * chttpsvr_engine_wait() to hold the response until the drain finishes. While
 * chttpsvr_destroy()'s self-call guard covers one specific server,
 * chttpsvr_engine_wait() covers the whole engine, so it's enough to mark the
 * calling thread as a worker of ANY server, even one that was never started,
 * and this test needs no real dispatch and no running reactor. */
TEST(chttpsvr_handle_lifecycle, engine_wait_from_within_own_handler_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    chttpsvr_engine_wait(); /* the actual misuse under test */
    _exit(0); /* unreachable if _ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check waitpid's return value before trusting status. An EINTR failure, or
   * any other one, leaves status at its initial 0, and without this check we
   * would quietly read that as "the child exited normally with status 0" when
   * waitpid never actually reported this child's real outcome, which gives a
   * confusing WIFSIGNALED failure instead of a clear one. Every later, more
   * elaborate fork test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

static chttpsvr g_self_restart_srv_for_lifecycle_test = CHTTPSVR_INVALID;
static _Atomic int g_self_restart_result_for_lifecycle_test = -1;

static void _self_restarting_handler_for_lifecycle_test(chttpsvr_req *req,
                                                        chttpsvr_resp *resp,
                                                        void *ctx) {
  (void)req;
  (void)ctx;
  /* This call is safe on its own: chttpsvr_stop() only closes the listener and
   * never touches worker_pool or reject_pool, so it never runs into the
   * self-call hazard that chttpsvr_start()'s restart path below runs into. */
  chttpsvr_stop(g_self_restart_srv_for_lifecycle_test);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval_t rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  atomic_store(&g_self_restart_result_for_lifecycle_test, (int)rv);
  chttpsvr_resp_write_str(resp, "done");
}

/* A handler can stop the very server whose worker pool runs it and then restart
 * that server immediately, which runs into the same self-call hazard as
 * chttpsvr_destroy() (see the comment of chttpsvr_worker_key_bundle in
 * chttpserver.c). But chttpsvr_start() has a real ccol_retval_t to report
 * through, so it refuses gracefully with ccol_not_permitted instead of
 * hanging or aborting, and it must leave the server in a state that a later,
 * legitimate restart from another thread can still recover cleanly. */
/* g_self_restart_srv_for_lifecycle_test is a process-wide global and not a
 * local, because the self-restarting handler above has to reach it from any
 * worker thread, so unlike every other server handle in this file it can't
 * carry a scope-exit _ccol_destructor as a safety net (GCC's cleanup attribute
 * only applies to a local variable with automatic storage). So every REQUIRE_*
 * below that can return early once the server has started is guarded: on a
 * failure it destroys the server, releasing that server's shared-engine
 * reference, before the REQUIRE_* itself runs and returns, which is the
 * "capture, clean up, then assert" idiom this file uses for exactly this kind
 * of leak. Without it a REQUIRE_* failure here would leak a global server still
 * holding an engine reference (the REQUIRE_EQ(..., ccol_not_permitted) below is
 * the critical one, since it is the real regression this test is here to
 * catch), and such a leak hangs the chttpsvr_engine_wait() call in this file's
 * _teardown() when the process exits, so a real regression in exactly the
 * feature under test would become a silent hang of the whole binary instead of
 * a clean, reported failure. */
static void _destroy_self_restart_test_server_if_live(void) {
  if (g_self_restart_srv_for_lifecycle_test != CHTTPSVR_INVALID)
    chttpsvr_destroy(g_self_restart_srv_for_lifecycle_test);
}

TEST(chttpsvr_handle_lifecycle,
     restart_from_within_own_handler_returns_not_permitted) {
  atomic_store(&g_self_restart_result_for_lifecycle_test, -1);
  g_self_restart_srv_for_lifecycle_test = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(g_self_restart_srv_for_lifecycle_test, CHTTPSVR_INVALID);
  ccol_retval_t reg_rv = chttpsvr_register_handler(
      g_self_restart_srv_for_lifecycle_test, CHTTP_GET, "/self-restart",
      _self_restarting_handler_for_lifecycle_test, NULL);
  if (reg_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval_t start_rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  if (start_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 71));
  int pton_rv = inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (pton_rv != 1) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(pton_rv, 1);
  const char *req_line =
      "GET /self-restart HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) _destroy_self_restart_test_server_if_live();
  REQUIRE_TRUE(fd >= 0);
  int connect_rv = test_connect(fd, (struct sockaddr *)&sa, sizeof(sa));
  if (connect_rv != 0) {
    close(fd);
    fd = -1;
    _destroy_self_restart_test_server_if_live();
  }
  REQUIRE_EQ(connect_rv, 0);
  ssize_t write_rv = write(fd, req_line, strlen(req_line));
  if (write_rv != (ssize_t)strlen(req_line)) {
    close(fd);
    fd = -1;
    _destroy_self_restart_test_server_if_live();
  }
  REQUIRE_EQ(write_rv, (ssize_t)strlen(req_line));
  char buf[512] = {0};
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The in-flight request that triggered the doomed self-restart still has to
   * complete normally, because chttpsvr_stop() alone never affects a connection
   * the server has already accepted. */
  if (status != 200) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(status, 200);
  int self_restart_result =
      atomic_load(&g_self_restart_result_for_lifecycle_test);
  if (self_restart_result != (int)ccol_not_permitted)
    _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(self_restart_result, (int)ccol_not_permitted);

  /* The server itself must be left in a perfectly safe, recoverable state, so
   * an ordinary, legitimate restart from THIS (non-worker) thread has to
   * succeed now. */
  ccol_retval_t restart_rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  if (restart_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);

  /* This second request runs the very same self-restarting handler again, so
   * the handler tries once more to restart the server from its own worker
   * thread and the library correctly refuses again. The request still has to
   * get a normal 200 response, which shows that the server keeps working
   * correctly however many times a caller retries this doomed pattern. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd2 < 0) _destroy_self_restart_test_server_if_live();
  REQUIRE_TRUE(fd2 >= 0);
  int connect_rv2 = test_connect(fd2, (struct sockaddr *)&sa, sizeof(sa));
  if (connect_rv2 != 0) {
    close(fd2);
    fd2 = -1;
    _destroy_self_restart_test_server_if_live();
  }
  REQUIRE_EQ(connect_rv2, 0);
  ssize_t write_rv2 = write(fd2, req_line, strlen(req_line));
  if (write_rv2 != (ssize_t)strlen(req_line)) {
    close(fd2);
    fd2 = -1;
    _destroy_self_restart_test_server_if_live();
  }
  REQUIRE_EQ(write_rv2, (ssize_t)strlen(req_line));
  char buf2[512] = {0};
  int status2 = _read_one_http_response(fd2, buf2, sizeof(buf2));
  close(fd2);
  fd2 = -1;
  if (status2 != 200) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(status2, 200);

  chttpsvr_destroy(g_self_restart_srv_for_lifecycle_test);
}

typedef struct {
  chttpsvr h;
} use_setter_arg_t;

static void *use_setter_thread(void *arg) {
  use_setter_arg_t *a = (use_setter_arg_t *)arg;
  /* The return value is ignored on purpose. A legitimate race with a concurrent
   * destroy can make this resolve fail with ccol_invalid_args instead of
   * succeeding, and both outcomes are correct. This thread is only here to
   * produce resolve, pin and unpin traffic at the same time as the destroy
   * thread below. chttpsvr_use stands for the module's whole public API apart
   * from destroy: chttpsvr_start, chttpsvr_stop, chttpsvr_register_handler,
   * chttpsvr_register_streaming_handler, chttpsvr_use and chttpsvr_subrouter
   * all resolve, pin for their own short synchronous duration and unpin before
   * they return, so driving any one of them against a concurrent destroy covers
   * the same race window as the others. */
  chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
  return NULL;
}

/* Races a fast, non-blocking entry point, chttpsvr_use, against a concurrent
 * destroy, and repeats that under stress. chttpsvr_use resolves, pins, runs a
 * short critical section, unpins and returns, with no blocking I/O at all. A
 * real regression here would be a resolve-then-use race or a use-after-free in
 * the unpin path itself (the comment of _chttpcli_resolve_unpin in
 * chttpclient.c gives a real example of the second), reachable only in a window
 * a few instructions wide that a single run without stress doesn't reproduce
 * reliably. Each iteration uses a fresh server, so every repetition is an
 * independent race and none of them reuses a handle that was already destroyed.
 */
TEST(chttpsvr_handle_lifecycle, resolve_unpin_race_stress) {
  /* Each intermediate outcome below goes into a local instead of being asserted
   * with a REQUIRE_*, and srv is always either handed to destroy_tid
   * or destroyed directly before any REQUIRE_* can return early. The test
   * creates a fresh chttpsvr handle in each of its 30 iterations, each holding
   * a live shared-engine reference, so a REQUIRE_* that returned early with srv
   * not destroyed would hang this whole binary's _teardown() forever when the
   * process exits, because that _teardown() blocks in chttpsvr_engine_wait()
   * until every server has been destroyed. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) REQUIRE_NE(srv, CHTTPSVR_INVALID);

    use_setter_arg_t setter_arg = {.h = srv};
    concurrent_svr_destroy_arg_t destroy_arg = {.h = srv};
    pthread_t setter_tid, destroy_tid;
    int setter_rc =
        pthread_create(&setter_tid, NULL, use_setter_thread, &setter_arg);
    int destroy_rc = 0;
    if (setter_rc != 0) {
      /* Neither thread ever started, so destroy srv directly. */
      chttpsvr_destroy(srv);
    } else {
      destroy_rc = pthread_create(&destroy_tid, NULL,
                                  concurrent_svr_destroy_thread, &destroy_arg);
      /* Join setter_tid either way, since it has already started and doesn't
       * block. If destroy_tid never started, destroy srv directly here. */
      pthread_join(setter_tid, NULL);
      if (destroy_rc == 0) {
        pthread_join(destroy_tid, NULL);
      } else {
        chttpsvr_destroy(srv);
      }
    }

    REQUIRE_EQ(setter_rc, 0);
    REQUIRE_EQ(destroy_rc, 0);
  }
}

typedef struct {
  chttpsvr_router *router;
} router_use_setter_arg_t;

static void *router_use_setter_thread(void *arg) {
  router_use_setter_arg_t *a = (router_use_setter_arg_t *)arg;
  /* The return value is ignored on purpose. A legitimate race with a concurrent
   * destroy of the server owning the router can make this resolve fail with
   * ccol_invalid_args instead of succeeding, and both outcomes are correct.
   * chttpsvr_router_on, chttpsvr_router_on_stream and chttpsvr_router_use must
   * not dereference router->srv directly, or change router->routes or
   * router->mw_head directly; they first resolve and pin against the
   * handle of the server that owns the router, like every other state-changing
   * entry point of this API. Without that pin they'd race a concurrent
   * chttpsvr_destroy(), which frees srv and every router under it through
   * _destroy_router once it sees pending_resolve_count == 0, a count they'd add
   * nothing to. */
  chttpsvr_router_on(a->router, CHTTP_GET, "/race", _hello_handler, NULL);
  return NULL;
}

/* The sub-router version of resolve_unpin_race_stress above: it races
 * chttpsvr_router_on against a concurrent chttpsvr_destroy() of the same
 * server. chttpsvr_router_on stands for chttpsvr_router_on,
 * chttpsvr_router_on_stream and chttpsvr_router_use, since all three resolve
 * and pin the server owning the router the same way. The real check here is
 * `make memtest` (valgrind) and a build with -fsanitize=thread, either of which
 * reports a real use-after-free if the resolve and pin protection regresses;
 * this test's job is only to produce the race window reliably. */
TEST(chttpsvr_handle_lifecycle, router_resolve_unpin_race_stress) {
  /* Each intermediate outcome below goes into a local instead of being asserted
   * at once with a REQUIRE_*, and srv is always either handed to destroy_tid
   * or destroyed directly before any REQUIRE_* can return early. The same
   * comment in resolve_unpin_race_stress just above explains why a leaked
   * chttpsvr handle here would hang this whole binary's teardown when the
   * process exits. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) REQUIRE_NE(srv, CHTTPSVR_INVALID);
    chttpsvr_router *router = chttpsvr_subrouter(srv, "/race-router");
    if (!router) {
      chttpsvr_destroy(srv);
      REQUIRE_TRUE(router != NULL);
    }

    router_use_setter_arg_t setter_arg = {.router = router};
    concurrent_svr_destroy_arg_t destroy_arg = {.h = srv};
    pthread_t setter_tid, destroy_tid;
    int setter_rc = pthread_create(&setter_tid, NULL, router_use_setter_thread,
                                   &setter_arg);
    int destroy_rc = 0;
    if (setter_rc != 0) {
      chttpsvr_destroy(srv);
    } else {
      destroy_rc = pthread_create(&destroy_tid, NULL,
                                  concurrent_svr_destroy_thread, &destroy_arg);
      pthread_join(setter_tid, NULL);
      if (destroy_rc == 0) {
        pthread_join(destroy_tid, NULL);
      } else {
        chttpsvr_destroy(srv);
      }
    }

    REQUIRE_EQ(setter_rc, 0);
    REQUIRE_EQ(destroy_rc, 0);
  }
}

/* This whole group of fork-safety regression tests drives chttpserver.c's
 * pthread_atfork() protection. The build compiles that protection out
 * completely when CCOL_FORK_SAFETY_REQUIRED is 0 (see that macro's doc comment
 * in common.h), and without it the premise of these tests, that the protection
 * prevents a hang, doesn't hold: a fork while the feeder thread below is in the
 * middle of a critical section would then become a real source of flakiness, if
 * a rare one, and not a meaningful regression check. tests/cthreadpool and
 * tests/cthreadcomm wrap their own fork-safety groups in the same `#if
 * CCOL_FORK_SAFETY_REQUIRED` in exactly this way. */
#if CCOL_FORK_SAFETY_REQUIRED
typedef struct {
  chttpsvr h;
  _Atomic int stop;
} fork_feeder_arg_t;

/* This thread drives chttpsvr_use against one long-lived server that it shares
 * with the fork trials below. chttpsvr_use resolves through
 * chttpsvr_slot_table.mutex and then runs a critical section under a write lock
 * on routes_lock. The sched_yield() after every call is load-bearing and not a
 * nicety, and tests/cthreadpool's ctp_fork_feeder_thread does the same. A bare
 * `while (!stop) chttpsvr_use(...);` loop reproduces exactly the same
 * contention natively, but it iterates so fast that valgrind can't keep up:
 * memcheck gives threads no real parallelism across cores, because it schedules
 * every thread through a single instrumented execution engine, so an
 * unthrottled spin becomes an astronomically larger total of instrumented
 * instructions for the same wall-clock duration of the test. Unthrottled, this
 * test doesn't finish a single one of its 60 trials in ten minutes under
 * valgrind, although natively it passes in well under a second. A yield after
 * every call caps this thread's call rate at what the scheduler's time-slice
 * granularity allows, so the race stays reproducible without the test paying
 * that multiplier. */
static void *fork_feeder_thread(void *arg) {
  fork_feeder_arg_t *a = (fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
    sched_yield();
  }
  return NULL;
}

/* Covers chttpserver.c's pthread_atfork() protection. fork() duplicates only
 * the calling thread, while some OTHER thread may hold one of this module's
 * locks (chttpsvr_slot_table.mutex, srv_engine_bundler.mutex,
 * servers_bundler.mutex, chttpsvr_router_shell_registry.mutex, and the mutex,
 * idle_mutex, diverted_mutex and routes_lock of any live server) at the exact
 * moment of the fork. Without the protection the child inherits such a lock
 * locked, for good, since no thread survives in the child that could ever
 * unlock it, and any later chttpsvr_* call in the child hangs forever. The
 * fork_does_not_inherit_a_locked_ctpool_mutex test of tests/cthreadpool has the
 * same shape. A feeder thread calling chttpsvr_use, throttled with a yield,
 * runs against one shared, long-lived server, keeping chttpsvr_slot_table.mutex
 * and that server's routes_lock under realistic contention that doesn't spin,
 * while this test forks over and over. A small, fixed-size, synchronous burst
 * of real create and destroy cycles runs right before each fork (the ctpool
 * sibling test uses the same shape for its burst of submits before a fork), and
 * that burst also drives chttpsvr_router_shell_registry.mutex and the mutex,
 * idle_mutex, diverted_mutex and routes_lock of a fresh instance, without a
 * second background thread spinning all the time. An alarm(1) bounds each
 * child, so a real regression here fails this test instead of hanging the whole
 * suite. Each child makes exactly the call a real application would make right
 * after inheriting a live server handle across a fork, chttpsvr_use on the
 * shared feeder server, and that call needs to return promptly either way: its
 * own resolve may legitimately race the parent's concurrent use of the same
 * handle, but it must never just hang.
 *
 * The burst before the fork below is synchronous and bounded, instead of a
 * second background thread that creates and destroys throwaway servers
 * continuously without any throttle, the way the ctpool sibling test's churn
 * thread does, where it's confirmed cheap. Unthrottled churn here doesn't
 * finish a single one of its 60 trials in ten minutes under valgrind, although
 * natively it passes in well under a second. A standalone reproduction outside
 * this suite measures that directly, so nothing is assumed: isolating each
 * background thread in turn, a yield-throttled chttpsvr_use feeder costs about
 * 0.03s per fork under valgrind, while the unthrottled churn thread doesn't
 * finish even one fork within 60 seconds and burns CPU the whole time. It isn't
 * blocked or deadlocked, it really makes progress, only catastrophically
 * slowly. The reason is the cost of each chttpsvr_create_mp iteration, which
 * covers a full root router, several mutexes and condition variables, and a
 * derived logger that itself registers into, and unregisters from, clogger's
 * separate global slot table, which is far heavier than ctpool's minimal pool
 * creation. memcheck also gives threads no real parallelism across cores at
 * all, since it time-slices every thread through a single instrumented
 * execution engine, so an unthrottled loop over this much heavier work doesn't
 * split across cores the way it does natively, it just hands valgrind an
 * astronomically larger total of instrumented instructions to simulate for the
 * same wall-clock duration of the test. The burst's cost scales with TRIALS and
 * not with how much work a background thread crams into an unbounded window of
 * wall-clock time. TRIALS is deliberately smaller than the 60 of the ctpool
 * sibling test, because fork() itself already has a large, roughly fixed cost
 * per call under valgrind whatever the number of trials. The test also covers
 * the hazard where routes_lock tracks a TID (see the doc comment of
 * _chttpsvr_atfork_release_impl), which is deterministic rather than
 * probabilistic, so it needs no repetition at all to be caught; only the other
 * half of this test's coverage, a lock really still held at the moment of the
 * fork, benefits from repeated trials. */
TEST(chttpsvr_handle_lifecycle, fork_does_not_inherit_a_locked_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* Each intermediate outcome below goes into a local instead of being asserted
   * with a REQUIRE_*, and feeder_tid is stopped and joined and
   * feeder_srv destroyed, unconditionally, before any REQUIRE_* can return
   * early. feeder_tid runs a bare `while (!stop) ...` loop (see the comment of
   * fork_feeder_thread) that only ends once feeder.stop is set, so an early
   * return here would leave it spinning forever, and feeder_srv still holds a
   * live shared-engine reference, so if nothing destroyed it, this whole
   * binary's teardown would hang when the process exits. */
  chttpsvr feeder_srv = chttpsvr_create(CLOG_INVALID, NULL);
  if (feeder_srv == CHTTPSVR_INVALID) REQUIRE_NE(feeder_srv, CHTTPSVR_INVALID);
  fork_feeder_arg_t feeder = {.h = feeder_srv, .stop = 0};
  pthread_t feeder_tid;
  int feeder_create_rc =
      pthread_create(&feeder_tid, NULL, fork_feeder_thread, &feeder);
  if (feeder_create_rc != 0) {
    chttpsvr_destroy(feeder_srv);
    REQUIRE_EQ(feeder_create_rc, 0);
  }

  enum { TRIALS = 15 };
  enum { BURST = 3 };
  int hangs = 0;
  pid_t bad_fork_pid = 0; /* 0 = every fork() call so far returned != -1 */
  pid_t bad_waitpid_pid = 0;
  for (int i = 0; i < TRIALS && bad_fork_pid == 0 && bad_waitpid_pid == 0;
       i++) {
    for (int b = 0; b < BURST; b++) {
      chttpsvr burst_srv = chttpsvr_create(CLOG_INVALID, NULL);
      if (burst_srv != CHTTPSVR_INVALID) chttpsvr_destroy(burst_srv);
    }

    pid_t pid = fork();
    if (pid == -1) {
      bad_fork_pid = -1;
      break;
    }
    if (pid == 0) {
      int dn = open("/dev/null", O_WRONLY);
      if (dn >= 0) {
        dup2(dn, STDOUT_FILENO);
        dup2(dn, STDERR_FILENO);
        close(dn);
      }
      /* Bounds this child's lifetime, so that if the hazard this test guards
       * against still fires somehow it doesn't hang the whole suite; the parent
       * below tells that case apart from a clean exit with WIFEXITED. */
      alarm(1);
      chttpsvr_use(feeder_srv, _noop_middleware_for_lifecycle_tests, NULL);
      _exit(0); /* we only get here if the call above returned */
    }

    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
      bad_waitpid_pid = pid;
      break;
    }
    if (!WIFEXITED(status)) hangs++;
  }

  atomic_store(&feeder.stop, 1);
  pthread_join(feeder_tid, NULL);
  chttpsvr_destroy(feeder_srv);

  REQUIRE_EQ(bad_fork_pid, 0);
  REQUIRE_EQ(bad_waitpid_pid, 0);
  REQUIRE_EQ(hangs, 0);
}

static chttpsvr g_fork_stop_race_srv = CHTTPSVR_INVALID;

static void *_fork_stop_race_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_stop(g_fork_stop_race_srv);
  return NULL;
}

/* Covers the unconditional child-side reset that _chttpsvr_atfork_release_impl
   makes of a server's lifecycle from CHTTPSVR_LC_STOPPING back to
   CHTTPSVR_LC_IDLE (that function's doc comment explains it, including
   why an unconditional reset is safe here in particular, unlike quiesce_state
   above). fork() can land while a parent-side thread is in the middle of the
   real teardown work of _chttpsvr_stop_internal for some server (lifecycle
   already CHTTPSVR_LC_STOPPING), and the child gets no copy of that thread.
   Without that reset the retry loop of chttpsvr_start() (a plain poll with a
   1ms sleep on exactly this state, not a wait on a condition variable) spins
   forever for this handle in the child. The test reuses g_stop_race_hook (which
   the analogous single-process scenario of tests_engine_stop.c already
   exercises) to pause a real chttpsvr_stop() call exactly there, in the parent,
   before the fork. */
TEST(chttpsvr_handle_lifecycle,
     fork_mid_stop_internal_does_not_hang_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  g_fork_stop_race_srv = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(g_fork_stop_race_srv, CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 73;
  ccol_retval_t start_rv = chttpsvr_start(g_fork_stop_race_srv, &cfg);
  if (start_rv != ccol_success) chttpsvr_destroy(g_fork_stop_race_srv);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  _chttpsvr_arm_stop_race_hook_for_tests();

  pthread_t stop_th;
  int create_rv =
      pthread_create(&stop_th, NULL, _fork_stop_race_stop_thread, NULL);
  if (create_rv != 0) {
    _chttpsvr_release_stop_race_hook_for_tests();
    chttpsvr_destroy(g_fork_stop_race_srv);
  }
  REQUIRE_EQ(create_rv, 0);

  /* Deterministic: this returns only once stop_th's _chttpsvr_stop_internal()
     call has entered CHTTPSVR_LC_STOPPING and is paused right there, strictly
     before its ccol_event_loop_remove()/close() call for the listener. */
  _chttpsvr_wait_stop_race_hook_entered_for_tests();

  pid_t pid = fork();
  bool fork_failed = pid == -1;
  bool waitpid_failed = false;
  bool child_hung = false;
  if (!fork_failed && pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's lifetime in case the hazard this test guards against
       still fires (chttpsvr_start() would otherwise block forever polling for
       lifecycle to leave CHTTPSVR_LC_STOPPING); the parent below tells that
       case apart from a clean exit with WIFEXITED. */
    alarm(3);
    chttpsvr_config_t restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 73;
    /* Not asserted. A fresh listener on this exact host:port may or may not
       bind cleanly here (the OLD listener fd, orphaned by the parent-side
       thread the child doesn't have, may still be open in this child; see this
       test's doc comment above), which is an accepted, documented outcome that
       the code handles gracefully and not what this test checks. The only thing
       under test is whether the call returns at all. */
    chttpsvr_start(g_fork_stop_race_srv, &restart_cfg);
    _exit(0); /* reached only if the call above really returned */
  } else if (!fork_failed) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
      waitpid_failed = true;
    } else {
      child_hung = !WIFEXITED(status);
    }
  }

  /* Parent side: let the real, paused stop call finish normally, then clean
     everything up here too, whatever happened in the child (or whether
     fork()/waitpid() itself failed above). Every REQUIRE_* below runs only
     after this unconditional cleanup, because g_fork_stop_race_srv holds a live
     shared-engine reference and stop_th stays paused inside a real
     chttpsvr_stop() call until the hook is released. On purpose there's no
     chttpsvr_engine_wait() here, just like in
     fork_does_not_inherit_a_locked_mutex just above: this file's shared g_srv
     holds an engine reference for this binary's whole run, so waiting here for
     the shared reactor to stop completely would hang forever. */
  _chttpsvr_release_stop_race_hook_for_tests();
  bool stop_th_joined = _bounded_join(stop_th, NULL);
  if (!stop_th_joined) pthread_detach(stop_th);
  chttpsvr_destroy(g_fork_stop_race_srv);

  REQUIRE_FALSE(fork_failed);
  REQUIRE_FALSE(waitpid_failed);
  REQUIRE_FALSE(child_hung);
  REQUIRE_TRUE(stop_th_joined);
}

static chttpsvr g_fork_starting_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_fork_starting_race_start_rv = ccol_success;

static void *_fork_starting_race_start_thread(void *arg) {
  (void)arg;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 74;
  g_fork_starting_race_start_rv =
      chttpsvr_start(g_fork_starting_race_srv, &cfg);
  return NULL;
}

/* Covers the unconditional child-side reset that _chttpsvr_atfork_release_impl
   makes of a server's lifecycle from CHTTPSVR_LC_STARTING back to
   CHTTPSVR_LC_IDLE (that function's doc comment has the full story, including
   why an unconditional reset is safe here in particular, as for the
   CHTTPSVR_LC_STOPPING case right above). fork() can land while a parent-side
   thread is in the middle of the real work of chttpsvr_start() for some server
   (lifecycle already CHTTPSVR_LC_STARTING: worker and reject pools created and
   an engine reference confirmed, but strictly before the listener socket
   exists), and the child gets no copy of that thread. Without that reset this
   handle stays unusable for good in the child: the retry loop of
   chttpsvr_start() treats CHTTPSVR_LC_STARTING exactly like a second
   chttpsvr_start() call really running at the same time and refuses outright
   with ccol_not_permitted (with no retry at all, unlike the
   CHTTPSVR_LC_STOPPING case above), and chttpsvr_stop() can't release it either
   (CHTTPSVR_LC_STARTING resolves to a silent was_started == false no-op that
   never touches lifecycle). The test reuses g_start_race_hook (which the
   analogous single-process scenario of tests_engine_stop.c already exercises)
   to pause a real chttpsvr_start() call exactly there, in the parent, before
   the fork. */
TEST(chttpsvr_handle_lifecycle,
     fork_mid_start_internal_does_not_disable_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  g_fork_starting_race_srv = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(g_fork_starting_race_srv, CHTTPSVR_INVALID);
  g_fork_starting_race_start_rv = ccol_success;

  _chttpsvr_arm_start_race_hook_for_tests();

  pthread_t start_th;
  int create_rv =
      pthread_create(&start_th, NULL, _fork_starting_race_start_thread, NULL);
  if (create_rv != 0) {
    _chttpsvr_release_start_race_hook_for_tests();
    chttpsvr_destroy(g_fork_starting_race_srv);
  }
  REQUIRE_EQ(create_rv, 0);

  /* Deterministic: this returns only once start_th's chttpsvr_start() call has
     entered CHTTPSVR_LC_STARTING, confirmed its engine reference and registered
     with servers_bundler, and is paused right there, strictly before it creates
     the listener socket. */
  _chttpsvr_wait_start_race_hook_entered_for_tests();

  int pipefd[2];
  int pipe_rv = pipe(pipefd);
  if (pipe_rv != 0) {
    /* pipefd was never opened, so there's nothing of our own to close, but
     * start_th is still paused and the hook still armed, so clean both up
     * before the REQUIRE_* below can return early, as every other early-failure
     * path of this test does. */
    _chttpsvr_release_start_race_hook_for_tests();
    bool start_th_joined = _bounded_join(start_th, NULL);
    if (!start_th_joined) pthread_detach(start_th);
    chttpsvr_destroy(g_fork_starting_race_srv);
    REQUIRE_EQ(pipe_rv, 0);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    _chttpsvr_release_start_race_hook_for_tests();
    bool start_th_joined = _bounded_join(start_th, NULL);
    if (!start_th_joined) pthread_detach(start_th);
    chttpsvr_destroy(g_fork_starting_race_srv);
    REQUIRE_NE(pid, -1);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    /* Bounds this child's lifetime only as a safety net (unlike the
       CHTTPSVR_LC_STOPPING scenario above this hazard doesn't hang: an unfixed
       CHTTPSVR_LC_STARTING refuses at once with ccol_not_permitted and
       doesn't block); the parent below tells a real hang apart from a clean
       exit with WIFEXITED. The child reports its outcome through the pipe and
       not through its process exit code, because under make memtest valgrind
       overrides a forked child's real exit code with its own --error-exitcode
       the instant it finds ANY "still reachable" allocation in the process
       image the child inherited at exit (which every child forked in the middle
       of the suite always has, since the rest of this suite hasn't gone quiet
       yet), so the exit code can't carry this result reliably.
       fork_mid_quiesce_teardown_does_not_hang_child in tests_engine_stop.c (and
       further away wait_from_within_own_task_does_not_hang in tests/cthreadpool
       and the fork_safety group in tests/clogger) give the full, independently
       confirmed story of this valgrind behaviour. */
    alarm(3);
    chttpsvr_config_t restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 75;
    /* The property under test: without that reset this call would return
       ccol_not_permitted (lifecycle stuck at CHTTPSVR_LC_STARTING by the
       parent-side thread the child doesn't have) instead of being able to go
       on. The test doesn't assert that it succeeds outright here, only that it
       isn't the one specific, permanent failure the reset prevents, because a
       fresh listener can legitimately fail to bind for unrelated environmental
       reasons; see this test's doc comment above. */
    ccol_retval_t child_rv =
        chttpsvr_start(g_fork_starting_race_srv, &restart_cfg);
    char ok = (child_rv != ccol_not_permitted) ? 1 : 0;
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  bool waitpid_failed = waitpid(pid, &status, 0) != pid;
  bool child_hung = !waitpid_failed && !WIFEXITED(status);

  /* Parent side: let the real, paused start call finish normally, then clean
     everything up here too, whatever happened in the child (or whether
     waitpid() itself failed above). On purpose there's no
     chttpsvr_engine_wait() here, just like in
     fork_does_not_inherit_a_locked_mutex above: this file's shared g_srv holds
     an engine reference for this binary's whole run, so waiting here for the
     shared reactor to stop completely would hang forever. */
  _chttpsvr_release_start_race_hook_for_tests();
  bool start_th_joined = _bounded_join(start_th, NULL);
  if (!start_th_joined) pthread_detach(start_th);
  chttpsvr_destroy(g_fork_starting_race_srv);

  REQUIRE_FALSE(waitpid_failed);
  REQUIRE_EQ((int)g_fork_starting_race_start_rv, (int)ccol_success);
  REQUIRE_FALSE(child_hung);
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
  REQUIRE_TRUE(start_th_joined);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* _listener_on_readable() gets srv as the bare void* arg of a ccol_event_loop
   callback, completely outside the ordinary resolve and pin mechanism of a
   chttpsvr handle that every other entry point into this server goes through.
   The documented promise of ccol_event_loop_remove(), that a caller may free
   the registration's arg as soon as the call returns, doesn't cover a callback
   already in progress at the moment of the call (ccol_event_loop never waits on
   the registration's dispatch_lock during a removal), so without a pin of its
   own chttpsvr_destroy() could free srv while a listener dispatch that's still
   running keeps reading its fields. The window is normally a handful of
   instructions, real but not reliably reproducible, and it widens to one
   valgrind can catch whenever a dispatch legitimately takes longer (the backoff
   after a persistent allocation failure following an accept, for example). The
   test reproduces it deterministically with a dedicated white-box race hook
   instead of trying to win that real timing race. */

typedef struct {
  chttpsvr h;
  _Atomic bool *returned;
} listener_race_destroy_arg_t;

static void *listener_race_destroy_thread(void *arg) {
  listener_race_destroy_arg_t *a = (listener_race_destroy_arg_t *)arg;
  __chttpsvr_destroy(a->h);
  atomic_store(a->returned, true);
  return NULL;
}

TEST(chttpsvr_handle_lifecycle,
     destroy_waits_for_in_progress_listener_dispatch) {
  /* Every intermediate step (releasing the hook, joining the destroy thread,
     closing fd_a, destroying srv) runs unconditionally before any REQUIRE_*
     that can return early. So a failing assertion here can never leave the race
     hook armed for good (which would hang every later test of this binary that
     dispatches a listener), a destroy thread parked for good waiting for a
     release that never comes, or srv leaked with its engine reference still
     held (which would hang chttpsvr_engine_wait() in this file's _teardown()
     when the process exits, turning a rare setup failure into a silent hang of
     the whole binary instead of a clean, reported failure). */
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_INVALID);

  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/listener-race-hello", _hello_handler, NULL);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 64;
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  _chttpsvr_arm_listener_dispatch_race_hook_for_tests();

  /* One real connection attempt is enough to trigger a real dispatch of
     _listener_on_readable on the shared reactor thread, and the armed hook
     parks that dispatch right after it pinned srv (listener_dispatch_pins) but
     strictly before it does anything else with it. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 64));
  int pton_rv = inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (pton_rv != 1) {
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_EQ(pton_rv, 1);
  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_a < 0) {
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_TRUE(fd_a >= 0);
  int connect_rv = test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa));
  /* Checked, unlike a bare call that ignores its result:
     _wait_..._entered_for_tests just below blocks on an unbounded
     _ccol_cond_var_wait (with no deadline, on purpose, like every other caller
     of this hook, all of which rely on a real test_connect() that triggered a
     real dispatch). A failed test_connect() here means the listener never
     dispatches at all, which would hang this test (and, since the hook stays
     armed, every later test of this binary that dispatches a listener) forever
     instead of failing cleanly. */
  if (connect_rv != 0) {
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_EQ(connect_rv, 0);

  _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests();

  _Atomic bool destroy_returned = false;
  listener_race_destroy_arg_t darg = {.h = srv, .returned = &destroy_returned};
  pthread_t destroy_tid;
  bool destroy_th_created =
      pthread_create(&destroy_tid, NULL, listener_race_destroy_thread, &darg) ==
      0;
  if (!destroy_th_created) {
    /* There's no thread to own the destroy, so nothing else releases the
       listener dispatch (already parked in the hook, as the
       _wait_..._entered_for_tests() call just above shows) or destroys srv.
       Release the hook and destroy srv here instead, before the
       REQUIRE_TRUE(destroy_th_created) below can return early, as the opening
       comment of this test and every other pthread_create failure branch of
       this file do. */
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }

  bool still_blocked = false;
  if (destroy_th_created) {
    /* Give chttpsvr_destroy() time to run and get to (and block inside) its
       listener_dispatch_pins wait, the same 150ms this file uses wherever it
       needs to "let the other side get to its own blocking point". */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* __chttpsvr_destroy() still has to be blocked here waiting for
       listener_dispatch_pins to reach 0. A destroy that has already returned
       means srv was freed while the listener dispatch is still paused and holds
       a bare pointer to it. */
    still_blocked = !atomic_load(&destroy_returned);
  }

  /* Release the hook so the parked dispatch can finish (it may or may not see
     srv->listen_fd already cleared by then, either is fine since this
     dispatch's outcome isn't what's under test), which in turn lets
     chttpsvr_destroy()'s wait go on. It's harmless even if the dispatch never
     reached the hook (which would mean fd_a's connection was never dispatched
     at all, a real bug in itself that the "entered" wait above already catches
     by hanging). */
  _chttpsvr_release_listener_dispatch_race_hook_for_tests();
  bool destroy_tid_joined = true;
  if (destroy_th_created) {
    destroy_tid_joined = _bounded_join(destroy_tid, NULL);
    if (!destroy_tid_joined) pthread_detach(destroy_tid);
  }
  close(fd_a);
  fd_a = -1;

  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(destroy_tid_joined);
}

/* A listener dispatch can start before chttpsvr_stop removes the listener and
   only get to its first statement afterwards. At that point it holds no pin, so
   neither the stop nor the destroy after it waits for it. The server's memory
   still stays valid for that dispatch: the listener registration holds a
   reference on the server until its on_removed fires, which only happens after
   the dispatch returns. The dispatch should also see that its listener is gone
   and return without running the accept loop. The entry race hook parks the
   dispatch at its first statement, which makes the window deterministic. Take
   the reference away and the server's final free runs before the parked
   dispatch returns; drop the listener fd check and the parked dispatch runs
   the accept loop. */

/* Polls for up to ms milliseconds until the watched server reaches its final
   free. Returns whether it did. */
static bool _wait_watched_finish_destroy(unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    if (_chttpsvr_watched_finish_destroy_ran_for_tests()) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return _chttpsvr_watched_finish_destroy_ran_for_tests();
}

typedef struct {
  chttpsvr h;
  _Atomic bool returned;
} entry_race_destroy_arg_t;

static void *_entry_race_destroy_thread(void *arg) {
  entry_race_destroy_arg_t *a = (entry_race_destroy_arg_t *)arg;
  __chttpsvr_destroy(a->h);
  atomic_store(&a->returned, true);
  return NULL;
}

/* Polls for up to ms milliseconds until the destroy thread returns. */
static bool _wait_entry_race_destroy_returned(entry_race_destroy_arg_t *a,
                                              unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    if (atomic_load(&a->returned)) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return atomic_load(&a->returned);
}

TEST(chttpsvr_handle_lifecycle,
     destroy_keeps_server_alive_for_a_listener_dispatch_that_has_not_pinned) {
  /* Every step that could leave the hook armed, the reactor thread parked or
     the server alive runs before the first REQUIRE_* below. */
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/entry-race-hello", _hello_handler, NULL);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 110;
  rv = chttpsvr_start(srv, &cfg);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  _chttpsvr_watch_finish_destroy_for_tests(srv);
  _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests();

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 110));
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  bool connected =
      fd >= 0 && test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  if (!connected) {
    /* No dispatch is ever going to reach the hook. */
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    if (fd >= 0) close(fd);
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(connected);
  }

  /* The dispatch now sits at its first statement without having taken a pin. */
  _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests();
  size_t dispatches_before = _chttpsvr_listener_dispatch_count_for_tests();

  entry_race_destroy_arg_t darg = {.h = srv, .returned = false};
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, _entry_race_destroy_thread, &darg) == 0;
  if (!created) chttpsvr_destroy(srv); /* returns, the dispatch isn't pinned */

  /* Nothing in the destroy waits for an unpinned dispatch, so it returns while
     the dispatch is still parked, and the server's final free must not have run
     by then. */
  bool destroy_returned =
      created ? _wait_entry_race_destroy_returned(&darg, 20000) : true;
  bool freed_under_dispatch = _chttpsvr_watched_finish_destroy_ran_for_tests();

  _chttpsvr_release_listener_dispatch_race_hook_for_tests();
  bool joined = created ? _bounded_join(tid, NULL) : true;
  if (created && !joined) pthread_detach(tid);
  /* The listener's on_removed fires once the dispatch returns, and it drops the
     last reference. */
  bool finished = _wait_watched_finish_destroy(10000);
  size_t dispatches_after = _chttpsvr_listener_dispatch_count_for_tests();
  close(fd);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(destroy_returned);
  REQUIRE_FALSE(freed_under_dispatch);
  REQUIRE_TRUE(joined);
  REQUIRE_TRUE(finished);
  /* The parked dispatch found its listener gone and never ran the accept loop.
   */
  REQUIRE_EQ(dispatches_after, dispatches_before);
}

/* A connection whose handler outlives a destroy's bounded drain is freed later,
   from its registration's on_removed. Until then it can still read its matched
   route: its handler, its middleware chain and its parameter names and count.
   So the server's route data must live until the server's final free, which
   waits for every such connection. A white-box counter in the library records a
   teardown of the route data that finds a connection of the server still
   allocated, and it stays at 0.

   The test makes the order deterministic. The slow request is the second one on
   its keep-alive connection, so the connection has a registration of its own.
   The drain bounds are shrunk, so the destroy gives up on the slow handler
   early and shuts its socket down. The single reactor thread is parked in
   the listener's entry race hook, so the connection's on_removed can't fire
   until the test releases it, after the destroy has returned. If the route
   data were freed inside chttpsvr_destroy, the counter would go up by one. */

static _Atomic bool g_route_lifetime_handler_entered = false;
static _Atomic bool g_route_lifetime_handler_release = false;

static void _route_lifetime_slow_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                         void *ctx) {
  (void)ctx;
  (void)chttpsvr_req_param(req, "id");
  atomic_store(&g_route_lifetime_handler_entered, true);
  /* Bounded, so that a failing test never leaves a worker blocked. */
  for (int i = 0; i < 30000 && !atomic_load(&g_route_lifetime_handler_release);
       i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  chttpsvr_resp_write_str(resp, "late");
}

TEST(chttpsvr_handle_lifecycle,
     route_data_outlives_a_connection_that_outlives_the_destroy_drain) {
  atomic_store(&g_route_lifetime_handler_entered, false);
  atomic_store(&g_route_lifetime_handler_release, false);

  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_INVALID);
  ccol_retval_t rv1 =
      chttpsvr_register_handler(srv, CHTTP_GET, "/route-lifetime/{id}",
                                _route_lifetime_slow_handler, NULL);
  ccol_retval_t rv2 = chttpsvr_register_handler(
      srv, CHTTP_GET, "/route-lifetime-fast", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 111;
  ccol_retval_t rv3 = (rv1 == ccol_success && rv2 == ccol_success)
                          ? chttpsvr_start(srv, &cfg)
                          : ccol_invalid_args;
  if (rv3 != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv1, (int)ccol_success);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
  REQUIRE_EQ((int)rv3, (int)ccol_success);
  _chttpsvr_watch_finish_destroy_for_tests(srv);
  size_t violations_before =
      _chttpsvr_route_teardown_with_live_connections_for_tests();

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 111));
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

  /* The first request completes and leaves the connection registered with the
     reactor, and the second one gets to the slow handler. */
  int fd_a = socket(AF_INET, SOCK_STREAM, 0);
  bool ok =
      fd_a >= 0 && test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  const char *fast = "GET /route-lifetime-fast HTTP/1.1\r\nHost: x\r\n\r\n";
  const char *slow = "GET /route-lifetime/7 HTTP/1.1\r\nHost: x\r\n\r\n";
  char buf[1024];
  ok = ok && write(fd_a, fast, strlen(fast)) == (ssize_t)strlen(fast) &&
       _read_one_http_response(fd_a, buf, sizeof(buf)) == 200 &&
       write(fd_a, slow, strlen(slow)) == (ssize_t)strlen(slow);
  for (int i = 0;
       ok && i < 10000 && !atomic_load(&g_route_lifetime_handler_entered);
       i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  ok = ok && atomic_load(&g_route_lifetime_handler_entered);

  /* Park the reactor thread in a listener dispatch of this same server. */
  int fd_b = -1;
  if (ok) {
    _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests();
    fd_b = socket(AF_INET, SOCK_STREAM, 0);
    ok = fd_b >= 0 &&
         test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    if (ok)
      _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests();
    else
      _chttpsvr_release_listener_dispatch_race_hook_for_tests();
  }
  if (!ok) {
    atomic_store(&g_route_lifetime_handler_release, true);
    if (fd_a >= 0) close(fd_a);
    if (fd_b >= 0) close(fd_b);
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(ok);
  }

  _chttpsvr_set_wait_in_flight_bounds_for_tests(50, 50);
  _chttpsvr_set_drain_connections_wait_ms_for_tests(50);
  entry_race_destroy_arg_t darg = {.h = srv, .returned = false};
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, _entry_race_destroy_thread, &darg) == 0;

  /* The destroy gives up on the slow handler and shuts its connection down,
     which the client sees as the end of the stream. */
  bool saw_shutdown = false;
  if (created) {
    struct timeval rcvtimeo = {20, 0};
    setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char tmp[256];
    ssize_t r;
    while ((r = read(fd_a, tmp, sizeof(tmp))) > 0) {
    }
    saw_shutdown = r == 0 || errno == ECONNRESET;
  }
  atomic_store(&g_route_lifetime_handler_release, true);
  if (!created) chttpsvr_destroy(srv);
  bool destroy_returned =
      created ? _wait_entry_race_destroy_returned(&darg, 20000) : true;
  size_t violations_after =
      _chttpsvr_route_teardown_with_live_connections_for_tests();

  _chttpsvr_release_listener_dispatch_race_hook_for_tests();
  bool joined = created ? _bounded_join(tid, NULL) : true;
  if (created && !joined) pthread_detach(tid);
  bool finished = _wait_watched_finish_destroy(10000);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  _chttpsvr_set_drain_connections_wait_ms_for_tests(0);
  close(fd_a);
  close(fd_b);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(saw_shutdown);
  REQUIRE_TRUE(destroy_returned);
  REQUIRE_TRUE(joined);
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(violations_after, violations_before);
}

/* The library must never mistake legitimate reuse of a slot for a stale handle
 * to whatever held the slot before, which a naive design keyed on an address
 * and remembering every pointer it ever destroyed can't handle safely: glibc's
 * tcache routinely hands the exact address of a just freed struct chttpserver
 * to the very next one it allocates (not guaranteed, but often). */
TEST(chttpsvr_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpsvr a = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(a, CHTTPSVR_INVALID);
  chttpsvr stale_a = a;
  chttpsvr_destroy(a);

  chttpsvr b = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(b, CHTTPSVR_INVALID);

  /* Every operation on b has to succeed normally, whether or not the allocator
   * reused a's exact address for b. */
  REQUIRE_EQ(chttpsvr_use(b, _noop_middleware_for_lifecycle_tests, NULL),
             ccol_success);

  /* a's stale handle must never resolve to b, even when b reuses the same
   * underlying address, and that's the whole point of the generation counter.
   */
  REQUIRE_EQ((void *)_chttpsvr_resolve_for_tests(stale_a), NULL);

  chttpsvr_destroy(b);
}

/* The slot table is bounded and doesn't grow forever: a create and destroy
 * churn loop with only one slot in flight at a time must reuse that one freed
 * slot in every iteration, and the table must not grow any further. The test
 * takes the capacity right after the first create and destroy pair instead of
 * asserting a fixed absolute value, because earlier tests in the same process
 * may have grown the table to some N > 1 already; what this test shows is
 * that ITS OWN churn adds no further growth. */
TEST(chttpsvr_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 100 };

  chttpsvr s0 = chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_NE(s0, CHTTPSVR_INVALID);
  chttpsvr_destroy(s0);
  size_t capacity_after_first = _chttpsvr_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_INVALID, NULL);
    REQUIRE_NE(srv, CHTTPSVR_INVALID);
    chttpsvr_destroy(srv);
  }

  REQUIRE_EQ(_chttpsvr_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*             DOUBLING-GROWTH REGISTRY OVERFLOW GUARD                        */
/* ========================================================================== */

TEST(doubling_growth_cap, first_growth_returns_initial) {
  /* cap == 0 (nothing allocated yet) must always return `initial` as it is,
   * whatever elem_size is, matching every caller's own "cap ? cap*2 : 8" shape.
   */
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(0, sizeof(void *), 8),
             (size_t)8);
}

TEST(doubling_growth_cap, ordinary_growth_doubles) {
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(8, sizeof(void *), 8),
             (size_t)16);
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(1024, sizeof(void *), 8),
             (size_t)2048);
}

TEST(doubling_growth_cap, cap_doubling_overflow_rejected) {
  /* cap * 2 itself wraps past SIZE_MAX, and the function must return 0, the
   * documented "can't grow" sentinel, which matches this project's guard idiom
   * of comparing against SIZE_MAX that this file uses throughout:
   * _router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_header and
   * _parse_qparams all make the same check. */
  REQUIRE_EQ(
      _chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX, sizeof(void *), 8),
      (size_t)0);
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX / 2 + 1,
                                                     sizeof(void *), 8),
             (size_t)0);
}

TEST(doubling_growth_cap, byte_size_multiplication_overflow_rejected) {
  /* cap * 2 alone doesn't overflow, but the new_cap * elem_size multiply the
   * caller does after it does. The function still returns 0 instead of a
   * new_cap that looks valid on its own and silently makes the real allocation
   * too small once the caller multiplies it by elem_size. */
  size_t cap = SIZE_MAX / 2; /* cap*2 == SIZE_MAX-1, no overflow yet */
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(cap, 4096, 8), (size_t)0);
}

/* ========================================================================== */
/* ACCEPT()-FAILURE ERRNO CLASSIFICATION                                      */
/*                                                                            */
/* The accept4() error handling of _listener_on_readable must not treat every */
/* unexpected failure the same way, logging nothing and returning at once     */
/* with no retry. Take a persistent resource-exhaustion condition such as     */
/* EMFILE, ENFILE, ENOBUFS or ENOMEM, where the process or the system really  */
/* has no file descriptor left, a realistic state under a sustained flood of  */
/* connections with a modest ulimit -n. The listen backlog then stays         */
/* non-empty, since nothing ever accepted anything, so the reactor's          */
/* level-triggered epoll dispatches this same handler again at once, a        */
/* silent, unbounded busy loop that burns 100% of a CPU for as long as the    */
/* condition lasts and gives an operator nothing to see. Two                  */
/* errno-classifying helpers handle this. For the resource-exhaustion class   */
/* the code sleeps briefly to back off and writes a rate-limited log. For an  */
/* error that's really transient for one connection (ECONNABORTED is one) it  */
/* retries at once, without another round trip through epoll. Driving the     */
/* real EMFILE and ECONNABORTED conditions end to end would need changing the */
/* fd limits of this whole test process, or kernel-side connection state,     */
/* which depends on the environment and is out of proportion for testing a    */
/* simple, deterministic classification of an integer, so these tests drive   */
/* that classification directly through a white-box hook.                     */
/* ========================================================================== */

TEST(accept_errno_classification,
     econnaborted_and_pending_network_errors_are_transient) {
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ECONNABORTED));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPROTO));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENETDOWN));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENETUNREACH));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENOPROTOOPT));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EHOSTDOWN));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EHOSTUNREACH));
  /* accept(2)'s own ERRORS section names exactly this set: "In the case of
     TCP/IP, these are ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET,
     EHOSTUNREACH, EOPNOTSUPP, and ENETUNREACH." */
#ifdef ENONET /* Linux only */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENONET));
#endif
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EOPNOTSUPP));
  /* EPERM ("Firewall rules forbid connection", per accept(2)'s own ERRORS
     section) has the same shape as ECONNABORTED, a rejection of one pending
     connection, only coming from a firewall rule instead of the peer. That's a
     realistic condition for a server behind connection-rate-limiting or
     fail2ban-style REJECT rules, not just a theoretical one. */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPERM));
  /* ETIMEDOUT/ENOSR/ESOCKTNOSUPPORT/EPROTONOSUPPORT come from accept(2)'s own
     "in addition, network errors for the new socket... may be returned; various
     Linux kernels can return other errors such as..." sentence, the same
     per-pending-connection category as the ones above. */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ETIMEDOUT));
#ifdef ENOSR /* Linux only */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENOSR));
#endif
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ESOCKTNOSUPPORT));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPROTONOSUPPORT));

  /* None of these are also (wrongly) classified as resource exhaustion. */
  REQUIRE_FALSE(
      _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(ECONNABORTED));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_resource_exhaustion_for_tests(EPERM));
  REQUIRE_FALSE(
      _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(ETIMEDOUT));
}

TEST(accept_errno_classification,
     resource_exhaustion_errors_classified_correctly) {
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_resource_exhaustion_for_tests(EMFILE));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_resource_exhaustion_for_tests(ENFILE));
  REQUIRE_TRUE(
      _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(ENOBUFS));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_resource_exhaustion_for_tests(ENOMEM));

  /* None of these are also (wrongly) classified as transient. */
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(EMFILE));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENFILE));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENOBUFS));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENOMEM));
}

TEST(accept_errno_classification, unrelated_errnos_are_neither) {
  /* EWOULDBLOCK/EAGAIN/EINTR are handled completely separately, before either
     classification helper is consulted (see _listener_on_readable), so a
     handful of unrelated errno values, and those two themselves, must not end
     up in either bucket. */
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(EWOULDBLOCK));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(EINTR));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(EINVAL));
  REQUIRE_FALSE(
      _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(EWOULDBLOCK));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_resource_exhaustion_for_tests(EINTR));
  REQUIRE_FALSE(
      _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(EINVAL));
}

/* ========================================================================== */
/*                  DISABLED-BY-ZERO CONFIGURATION FIELDS                     */
/* ========================================================================== */

/* Opens a connection to port and returns the fd, or -1. It sends nothing. */
static int _connect_silent(uint16_t port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }
  /* Bounded, so that a regression that stops serving this connection fails the
     test instead of wedging the whole binary in recv(). */
  struct timeval rcv = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  return fd;
}

/* Waits, with a bound, for the peer to close fd, and returns true once it has.
 * The bound is only a safety net against a hang and not the mechanism, since
 * the loop returns as soon as the sweep really fires. */
static bool _wait_for_peer_close(int fd, int bound_ms) {
  for (int waited = 0; waited < bound_ms; waited += 50) {
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, 50) > 0) {
      char probe[1];
      ssize_t r = recv(fd, probe, sizeof(probe), MSG_PEEK);
      if (r == 0) return true;
    }
  }
  return false;
}

/* Sends a GET on an already-open fd and returns the parsed status, or -1. */
static int _get_on(int fd, const char *path) {
  char req[256];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
  if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) return -1;
  char buf[1024] = {0};
  if (recv(fd, buf, sizeof(buf) - 1, 0) <= 0) return -1;
  int status = 0;
  if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
  return status;
}

/*
 * read_timeout_us == 0 turns the idle sweep off, so a connection that opens and
 * then says nothing is left alone instead of being closed under the peer's
 * feet; include/chttpserver.h promises exactly that for a connection that
 * "sends a partial header block (or nothing at all) and then goes quiet".
 *
 * The test calibrates itself instead of racing a clock. It idles a second
 * server next to the first one, with a short positive read_timeout_us, and
 * waits for THAT connection to close, which is a real state change and not an
 * elapsed duration. The sweep runs on its own period, so no fixed sleep would
 * be right here. Only once the sweep has visibly fired does the test require
 * the disabled server's connection to still work, so the test can't pass just
 * because nothing ever closes anything.
 */
TEST(chttpserver, read_timeout_ms_zero_leaves_an_idle_connection_alone) {
  chttpsvr disabled _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  chttpsvr calibrated _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(disabled != CHTTPSVR_INVALID);
  REQUIRE_TRUE(calibrated != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(disabled, CHTTP_GET, "/idle",
                                            _hello_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(calibrated, CHTTP_GET, "/idle",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t off = CHTTPSVR_CONFIG_DEFAULT;
  off.host = "127.0.0.1";
  off.port = TEST_PORT + 90;
  off.read_timeout_us = 0; /* the disabled state under test */
  off.idle_timeout_us = 0; /* 0 here means "the same as read_timeout_us" */
  REQUIRE_EQ((int)chttpsvr_start(disabled, &off), (int)ccol_success);

  chttpsvr_config_t on = CHTTPSVR_CONFIG_DEFAULT;
  on.host = "127.0.0.1";
  on.port = TEST_PORT + 91;
  on.read_timeout_us = 200000;
  on.idle_timeout_us = 200000;
  REQUIRE_EQ((int)chttpsvr_start(calibrated, &on), (int)ccol_success);

  int fd_off = _connect_silent(TEST_PORT + 90);
  int fd_on = _connect_silent(TEST_PORT + 91);
  bool both_open = (fd_off >= 0 && fd_on >= 0);

  bool sweep_fired = false;
  int served_when_disabled = -1;
  if (both_open) {
    sweep_fired = _wait_for_peer_close(fd_on, 30000);
    served_when_disabled = _get_on(fd_off, "/idle");
  }
  if (fd_off >= 0) close(fd_off);
  if (fd_on >= 0) close(fd_on);

  chttpsvr_stop(disabled);
  chttpsvr_stop(calibrated);

  REQUIRE_TRUE(both_open);
  /* The calibration: the sweep really closes an idle connection when a positive
     timeout is configured. */
  REQUIRE_TRUE(sweep_fired);
  REQUIRE_EQ(served_when_disabled, 200);
}

TEST(chttpserver, max_connections_zero_means_unlimited) {
  enum { CONNS = 12 };
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/many",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 92;
  cfg.max_connections = 0; /* the unlimited state under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Held open together, so that any cap would be in force for the later ones,
     instead of each connection being reused one after the other. */
  int fds[CONNS];
  int opened = 0;
  for (int i = 0; i < CONNS; i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)(TEST_PORT + 92));
    if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) break;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) break;
    if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
      close(fd);
      break;
    }
    /* Bounded, so a cap wrongly applied at 0 fails this test instead of
       blocking it forever on a connection the server will never serve. */
    struct timeval rcv = {.tv_sec = 10, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
    fds[opened++] = fd;
  }

  int served = 0;
  for (int i = 0; i < opened; i++) {
    const char *req = "GET /many HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
    if (send(fds[i], req, strlen(req), MSG_NOSIGNAL) < 0) break;
    char buf[512] = {0};
    if (recv(fds[i], buf, sizeof(buf) - 1, 0) <= 0 ||
        strstr(buf, "HTTP/1.1 200") == NULL)
      break; /* served < CONNS already fails below; stopping here keeps a
                regression's cost to one receive timeout instead of CONNS */
    served++;
  }
  for (int i = 0; i < opened; i++) close(fds[i]);

  chttpsvr_stop(srv);

  REQUIRE_EQ(opened, (int)CONNS);
  REQUIRE_EQ(served, (int)CONNS);
}

/* ========================================================================== */
/*          ROUTER PRECEDENCE, HEAD FALLBACK, REJECTION MIDDLEWARE            */
/* ========================================================================== */

/* Sends one request on its own connection to any port and returns the status
   code, or -1 on a socket-level failure. _raw_request above is the same thing
   pinned to TEST_PORT, and since each of these tests sets up its own server
   they need the port as an argument. */
static int _raw_request_on_port(uint16_t port, const char *method,
                                const char *path, const char *extra_headers,
                                char *buf, size_t buf_sz) {
  int fd _ccol_destructor(_close_scoped_fd) = _connect_silent(port);
  if (fd < 0) return -1;
  char req[1024];
  int n = snprintf(req, sizeof(req),
                   "%s %s HTTP/1.1\r\n"
                   "Host: 127.0.0.1\r\n"
                   "%s"
                   "Connection: close\r\n"
                   "\r\n",
                   method, path, extra_headers ? extra_headers : "");
  if (n >= (int)sizeof(req)) n = (int)sizeof(req) - 1;
  if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) return -1;
  size_t total = 0;
  ssize_t r;
  while (total < buf_sz - 1 &&
         (r = recv(fd, buf + total, buf_sz - 1 - total, 0)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

/* A sub-router middleware that answers 401 and doesn't call next, which is the
   real shape of an authenticator mounted on a prefix. */
static void _prec_auth_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                          chttpsvr_next_fn next) {
  (void)ctx;
  (void)next;
  (void)req;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "auth-required");
}

static void _prec_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root-catch-all");
}

static void _prec_secret_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "secret");
}

TEST(chttpserver, subrouter_middleware_not_bypassed_by_broad_root_pattern) {
  /* A root-level pattern with named parameters overlaps every path of the same
     arity in the process, a sub-router's own mount included. Routers are tried
     by how specific their mount prefix is, so the sub-router answers inside its
     own prefix and its middleware chain runs, and the root pattern serves
     everything outside that prefix.

     The sub-router and its route are registered FIRST and the broad root
     pattern afterwards. A router choice that followed registration order would
     also pick the sub-router here, so this order alone can't tell the two rules
     apart; subrouter_wins_over_root_route_at_same_path pins the opposite order
     on g_srv. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_router *admin = chttpsvr_subrouter(srv, "/admin");
  REQUIRE_TRUE(admin != NULL);
  REQUIRE_EQ((int)chttpsvr_router_use(admin, _prec_auth_mw, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_router_on(admin, CHTTP_GET, "/secret",
                                     _prec_secret_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/{a}/{b}",
                                            _prec_root_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 100;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char buf[2048] = {0};
  int mounted = _raw_request_on_port(TEST_PORT + 100, "GET", "/admin/secret",
                                     NULL, buf, sizeof(buf));
  bool mounted_body_is_auth = (strstr(buf, "auth-required") != NULL);

  char buf2[2048] = {0};
  int outside = _raw_request_on_port(TEST_PORT + 100, "GET", "/other/thing",
                                     NULL, buf2, sizeof(buf2));
  bool outside_body_is_root = (strstr(buf2, "root-catch-all") != NULL);

  chttpsvr_stop(srv);

  /* The sub-router's own chain answered, so nothing reached the root handler.
     Without the specificity ordering this would be 200 "root-catch-all" and the
     mounted authenticator would never run at all. */
  REQUIRE_EQ(mounted, 401);
  REQUIRE_TRUE(mounted_body_is_auth);
  /* A path outside every mount still gets to the root pattern. */
  REQUIRE_EQ(outside, 200);
  REQUIRE_TRUE(outside_body_is_root);
}

TEST(chttpserver, more_specific_mount_wins_over_less_specific_one) {
  /* Two mounts both match /api/v1/thing; the one with more prefix segments
     answers, whichever was registered first. The shorter prefix is
     registered first here, so registration order alone would pick it. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_router *shallow = chttpsvr_subrouter(srv, "/api");
  chttpsvr_router *deep = chttpsvr_subrouter(srv, "/api/v1");
  REQUIRE_TRUE(shallow != NULL);
  REQUIRE_TRUE(deep != NULL);
  REQUIRE_EQ((int)chttpsvr_router_on(shallow, CHTTP_GET, "/v1/thing",
                                     _prec_root_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_router_on(deep, CHTTP_GET, "/thing",
                                     _prec_secret_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 101;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char buf[2048] = {0};
  int status = _raw_request_on_port(TEST_PORT + 101, "GET", "/api/v1/thing",
                                    NULL, buf, sizeof(buf));
  bool deep_answered = (strstr(buf, "secret") != NULL);

  chttpsvr_stop(srv);

  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(deep_answered);
}

TEST(chttpserver, head_request_is_served_by_a_get_route) {
  /* RFC 9110 section 9.3.2: HEAD is identical to GET except that no body is
     sent, so a path registered for GET alone must answer HEAD. /hello is
     registered for CHTTP_GET only. The response still carries the
     content-length the GET body would have had, and carries no body. */
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/hello", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "content-length:13") != NULL);
  /* "Hello, world!" is what the GET body would be, and a HEAD response must not
     contain it. */
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, explicit_head_route_wins_over_get_route) {
  /* /head-pref has both a CHTTP_GET and a CHTTP_HEAD route, the GET one
     registered first (see _setup). The explicit HEAD route has to serve a HEAD
     request and must not serve a GET. */
  char head_buf[2048] = {0};
  int head_status =
      _raw_request("HEAD", "/head-pref", NULL, head_buf, sizeof(head_buf));
  char get_buf[2048] = {0};
  int get_status =
      _raw_request("GET", "/head-pref", NULL, get_buf, sizeof(get_buf));

  REQUIRE_EQ(head_status, 200);
  REQUIRE_TRUE(strstr(head_buf, "x-head-route") != NULL);
  REQUIRE_EQ(get_status, 200);
  REQUIRE_TRUE(strstr(get_buf, "x-head-route") == NULL);
  REQUIRE_TRUE(strstr(get_buf, "Hello, world!") != NULL);
}

TEST(chttpserver, head_on_a_path_with_no_get_route_is_still_405) {
  /* The fallback only goes from GET to HEAD. /echo-body is registered for
     CHTTP_POST alone, so a HEAD request for it matches the path but no method
     and gets rejected with 405, which the fallback leaves alone. */
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/echo-body", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

TEST(chttpserver, head_on_an_unregistered_path_is_still_404) {
  char buf[2048] = {0};
  int status =
      _raw_request("HEAD", "/no-such-path-at-all", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                 MIDDLEWARE FOR REJECTED REQUESTS                           */
/* ========================================================================== */

/* Written by the middleware below (on one of the server's rejection-pool
   threads) and read by the test body once the response has arrived.
   g_rej_mw_runs is the synchronisation edge: the middleware fills the two
   observation buffers and only then increments it, and every test reads those
   buffers only after it has seen the increment. */
static _Atomic int g_rej_mw_runs = 0;
static _Atomic int g_mount_mw_runs = 0;
static char g_rej_mw_path[256];
static chttp_method_t g_rej_mw_method;

static void _rej_global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                           chttpsvr_next_fn next) {
  (void)ctx;
  const char *path = chttpsvr_req_path(req);
  snprintf(g_rej_mw_path, sizeof(g_rej_mw_path), "%s", path ? path : "(nil)");
  g_rej_mw_method = chttpsvr_req_method(req);
  chttpsvr_resp_set_header(resp, "x-rej-mw", "1");
  atomic_fetch_add(&g_rej_mw_runs, 1);
  /* What a rate limiter looks like: it answers the request itself and never
     calls next, so the chain short-circuits and this response replaces the
     rejection the request was heading for. */
  if (path && strcmp(path, "/rate-limited") == 0) {
    chttpsvr_resp_set_status(resp, 429);
    chttpsvr_resp_write_str(resp, "slow down");
    return;
  }
  next(req, resp);
}

static void _rej_mount_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                          chttpsvr_next_fn next) {
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-mount-mw", "1");
  atomic_fetch_add(&g_mount_mw_runs, 1);
  next(req, resp);
}

static chttpsvr g_rej_srv = CHTTPSVR_INVALID;
#define REJ_PORT ((uint16_t)(TEST_PORT + 102))

/* One server shared by all the rejection-middleware tests below. It starts on
   first use and is stopped by _teardown's atexit hook through the destroy
   below, and it's kept out of _setup so that the tests that don't need it pay
   nothing for it. */
static void _rej_srv_destroy_at_exit(void) {
  if (g_rej_srv != CHTTPSVR_INVALID) {
    chttpsvr_destroy(g_rej_srv);
  }
}

static bool _rej_srv_ready(void) {
  if (g_rej_srv != CHTTPSVR_INVALID) return true;
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (!srv) return false;
  bool ok = false;
  chttpsvr_router *mounted = NULL;
  if (chttpsvr_use(srv, _rej_global_mw, NULL) != ccol_success) goto done;
  if (chttpsvr_register_handler(srv, CHTTP_GET, "/ok", _hello_handler, NULL) !=
      ccol_success)
    goto done;
  mounted = chttpsvr_subrouter(srv, "/mounted");
  if (!mounted) goto done;
  if (chttpsvr_router_use(mounted, _rej_mount_mw, NULL) != ccol_success)
    goto done;
  if (chttpsvr_router_on(mounted, CHTTP_POST, "/up", _echo_body_handler,
                         NULL) != ccol_success)
    goto done;
  {
    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = REJ_PORT;
    /* Small enough that an ordinary Content-Length is already over the limit,
       so the 413 pre-check fires at headers-complete without any body being
       sent. */
    cfg.max_body_size = 64;
    if (chttpsvr_start(srv, &cfg) != ccol_success) goto done;
  }
  ok = true;
done:
  /* Destroyed right here on any failure: the caller's REQUIRE_TRUE returns from
     the test on a false, so nothing else would ever get it back and memtest
     would report the whole server as definitely lost. */
  if (!ok) {
    chttpsvr_destroy(srv);
    return false;
  }
  g_rej_srv = srv;
  atexit(_rej_srv_destroy_at_exit);
  return true;
}

TEST(chttpserver, global_middleware_runs_for_an_unmatched_route) {
  /* A 404 is exactly the traffic a rate limiter or an access log built as
     middleware is there to see. If the chain didn't run here, a flood of probes
     for paths that don't exist would be invisible to the application. */
  REQUIRE_TRUE(_rej_srv_ready());
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "GET", "/nope", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(after, before + 1);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") != NULL);
  /* The middleware can see the request it's being asked about. */
  REQUIRE_STREQ(g_rej_mw_path, "/nope");
  REQUIRE_EQ((int)g_rej_mw_method, (int)CHTTP_GET);
}

TEST(chttpserver, global_middleware_runs_for_a_method_rejection) {
  REQUIRE_TRUE(_rej_srv_ready());
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "PUT", "/ok", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 405);
  REQUIRE_EQ(after, before + 1);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") != NULL);
}

TEST(chttpserver, global_middleware_can_short_circuit_a_rejected_request) {
  /* The middleware answers /rate-limited itself and never calls next, so its
     own 429 replaces the 404 this request was heading for. */
  REQUIRE_TRUE(_rej_srv_ready());
  char buf[2048] = {0};
  int status = _raw_request_on_port(REJ_PORT, "GET", "/rate-limited", NULL, buf,
                                    sizeof(buf));
  REQUIRE_EQ(status, 429);
  REQUIRE_TRUE(strstr(buf, "slow down") != NULL);
}

TEST(chttpserver, payload_too_large_rejection_runs_the_matched_routers_chain) {
  /* A 413 is decided after a route has already matched, so the chain that match
     produced runs in full (the global middleware and the mounted sub-router's
     own), while no handler runs. */
  REQUIRE_TRUE(_rej_srv_ready());
  int before_global = atomic_load(&g_rej_mw_runs);
  int before_mount = atomic_load(&g_mount_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "POST", "/mounted/up",
                           "Content-Length: 4096\r\n", buf, sizeof(buf));
  int after_global = atomic_load(&g_rej_mw_runs);
  int after_mount = atomic_load(&g_mount_mw_runs);

  REQUIRE_EQ(status, 413);
  REQUIRE_EQ(after_global, before_global + 1);
  REQUIRE_EQ(after_mount, before_mount + 1);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-mount-mw") != NULL);
}

TEST(chttpserver, unknown_method_rejection_runs_no_middleware) {
  /* 501 is decided from the request line, for a method token outside the seven
     chttp_method_t names and before the request target is parsed, so there's no
     request object a middleware could look at, and on purpose the chain doesn't
     run. */
  REQUIRE_TRUE(_rej_srv_ready());
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "PROPFIND", "/ok", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 501);
  REQUIRE_EQ(after, before);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") == NULL);
}

/* ========================================================================== */
/*        REJECTED-REQUEST PATH DECODE UNDER A REFUSING ALLOCATOR             */
/* ========================================================================== */

/* For a rejection, the decoded path a middleware reads is decided lazily, with
   one allocation that can fail under memory pressure. chttpsvr_req_path is
   documented to give a middleware a readable path, and a middleware written the
   documented way (an access log, a rate limiter) goes straight for
   strlen/strcmp/printf("%s") on it, so a rejection whose path couldn't be
   decoded must not run the chain at all. Otherwise a scanner generating 404s
   against a process short of memory would take the server down.

   Targeting that one allocation: the request target below is REJ_OOM_PATH_LEN
   bytes long, and both the raw copy stored by the request-line parse and the
   decoded copy made for the middleware are exactly REJ_OOM_PATH_LEN + 1 bytes.
   The raw copy always comes first, and nothing else this server allocates while
   serving this one request has that size, so refusing the SECOND allocation of
   exactly that size after arming refuses the decode and nothing else. The test
   asserts that both sites were reached and exactly one allocation was refused,
   so a change that moves either allocation fails this test instead of letting
   it pass vacuously. */

#define REJ_OOM_PORT ((uint16_t)(TEST_PORT + 107))
#define REJ_OOM_PATH_LEN 251

static _Atomic bool g_rej_oom_armed = false;
static _Atomic int g_rej_oom_target_seen = 0;
static _Atomic int g_rej_oom_refused = 0;

static void *_rej_oom_malloc(size_t size) {
  if (atomic_load(&g_rej_oom_armed) && size == (size_t)REJ_OOM_PATH_LEN + 1 &&
      atomic_fetch_add(&g_rej_oom_target_seen, 1) == 1) {
    atomic_fetch_add(&g_rej_oom_refused, 1);
    return NULL;
  }
  return malloc(size);
}

static void _rej_oom_free(void *ptr) { free(ptr); }

static void *_rej_oom_calloc(size_t count, size_t size) {
  return calloc(count, size);
}

static void *_rej_oom_realloc(void *ptr, size_t size) {
  return realloc(ptr, size);
}

static _Atomic int g_rej_oom_mw_runs = 0;
static _Atomic bool g_rej_oom_mw_saw_null_path = false;

/* Records what it got instead of dereferencing it, so a regression shows up as
   a failed assertion here and not as a SIGSEGV that would take every other test
   result of this binary down with it. */
static void _rej_oom_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                        chttpsvr_next_fn next) {
  (void)ctx;
  if (chttpsvr_req_path(req) == NULL)
    atomic_store(&g_rej_oom_mw_saw_null_path, true);
  atomic_fetch_add(&g_rej_oom_mw_runs, 1);
  next(req, resp);
}

TEST(chttpserver, rejection_middleware_never_sees_an_undecodable_path) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = CHTTPSVR_INVALID;
  {
    ccol_memmgmt_procs_t procs = {
        .malloc = _rej_oom_malloc,
        .free = _rej_oom_free,
        .calloc = _rej_oom_calloc,
        .realloc = _rej_oom_realloc,
    };
    srv = chttpsvr_create_mp(&procs, g_test_logger, NULL);
  }
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _rej_oom_mw, NULL), (int)ccol_success);
  /* A single registered route, on a path the request below deliberately misses,
     so the request gets a 404, which is the status the chain does run for. */
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = REJ_OOM_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char path[REJ_OOM_PATH_LEN + 1];
  path[0] = '/';
  memset(path + 1, 'a', REJ_OOM_PATH_LEN - 1);
  path[REJ_OOM_PATH_LEN] = '\0';

  char buf[2048] = {0};
  atomic_store(&g_rej_oom_armed, true);
  int status =
      _raw_request_on_port(REJ_OOM_PORT, "GET", path, NULL, buf, sizeof(buf));
  /* Disarmed before anything else runs, so the server's own teardown below gets
     an honest allocator. */
  atomic_store(&g_rej_oom_armed, false);

  bool saw_null_path = atomic_load(&g_rej_oom_mw_saw_null_path);
  int mw_runs = atomic_load(&g_rej_oom_mw_runs);
  int target_seen = atomic_load(&g_rej_oom_target_seen);
  int refused = atomic_load(&g_rej_oom_refused);

  chttpsvr_stop(srv);

  /* The contract: a middleware never gets a NULL path. */
  REQUIRE_FALSE(saw_null_path);
  /* And this is how it's kept: the chain doesn't run at all, just as it doesn't
     when the rejection pool is saturated. */
  REQUIRE_EQ(mw_runs, 0);
  /* 404 and not 500: the raw copy of the target succeeded, so the refusal hit
     the decode and not the request-line parse. */
  REQUIRE_EQ(status, 404);
  /* Both allocation sites were reached and exactly one was refused. */
  REQUIRE_EQ(target_seen, 2);
  REQUIRE_EQ(refused, 1);
}

/* ========================================================================== */
/*                 HEADER-PHASE TOTAL DURATION CEILING                        */
/* ========================================================================== */

/* Sends one byte of a deliberately never-ending header block every interval_ms
   and reports whether the server closed the connection within bound_ms.

   Every byte is a legitimate part of a well-formed header block, so only a
   total-duration bound can end this: the parser keeps waiting for more input
   instead of rejecting anything, read_timeout_us/idle_timeout_us only measure
   the gap between bytes and every byte resets them, and the handful of bytes
   sent stays orders of magnitude below max_header_bytes. The poll is both the
   delay between bytes and the close detector, so a server that closes the
   connection is noticed in the same iteration and not one interval later. */
static bool _trickle_headers_until_closed(int fd, int interval_ms,
                                          int bound_ms) {
  static const char prefix[] = "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
  const int plen = (int)(sizeof(prefix) - 1);
  for (int waited = 0, i = 0; waited < bound_ms; waited += interval_ms, i++) {
    /* After the prefix the same header line just keeps growing, so the block
       never completes however long this runs. */
    char c = (i < plen) ? prefix[i] : 'x';
    if (send(fd, &c, 1, MSG_NOSIGNAL) != 1) return true; /* the peer is gone */
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, interval_ms) > 0) {
      char probe[1];
      if (recv(fd, probe, sizeof(probe), MSG_PEEK) <= 0) return true;
    }
  }
  return false;
}

/* Sends one request and consumes its WHOLE response (status line, headers and
   exactly content-length body bytes), returning the status code or -1. _get_on
   above stops at the first recv, which is enough when the caller closes right
   afterwards, but a caller that sends a SECOND request on the same keep-alive
   connection must drain the first response completely, or a response that
   arrived split over two segments leaves its tail in the socket buffer and the
   next read parses that tail as the second response's status line. */
static int _get_on_complete(int fd, const char *path) {
  char req[256];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
  if (n >= (int)sizeof(req)) return -1;
  if (send(fd, req, (size_t)n, MSG_NOSIGNAL) != n) return -1;

  char buf[4096];
  size_t total = 0;
  for (;;) {
    ssize_t r = recv(fd, buf + total, sizeof(buf) - 1 - total, 0);
    if (r <= 0) return -1;
    total += (size_t)r;
    buf[total] = '\0';
    char *head_end = strstr(buf, "\r\n\r\n");
    if (!head_end) continue;
    size_t head_len = (size_t)(head_end - buf) + 4;
    const char *cl = strcasestr(buf, "\ncontent-length:");
    size_t body_len = 0;
    if (cl)
      body_len = (size_t)strtoul(cl + strlen("\ncontent-length:"), NULL, 10);
    if (total >= head_len + body_len) break;
  }
  int status = 0;
  if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
  return status;
}

/* Sends a complete, well-formed request one byte at a time, interval_ms apart,
   then reads the status line. Returns the status code, or -1. */
static int _slow_get_on(int fd, const char *path, int interval_ms) {
  char req[256];
  int n = snprintf(req, sizeof(req),
                   "GET %s HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n", path);
  if (n >= (int)sizeof(req)) return -1;
  struct timespec ts = {.tv_sec = interval_ms / 1000,
                        .tv_nsec = (long)(interval_ms % 1000) * 1000000L};
  for (int i = 0; i < n; i++) {
    if (send(fd, req + i, 1, MSG_NOSIGNAL) != 1) return -1;
    nanosleep(&ts, NULL);
  }
  char buf[1024] = {0};
  if (recv(fd, buf, sizeof(buf) - 1, 0) <= 0) return -1;
  int status = 0;
  if (sscanf(buf, "HTTP/1.%*d %d", &status) != 1) return -1;
  return status;
}

#define HDR_DURATION_MS 1200

TEST(chttpserver, config_default_bounds_the_header_read_duration) {
  /* The shipped defaults need to be safe on their own, so a deployment that
     never touches this knob is still bounded against a trickled header block.
     It's read off the macro and not the server, so a change to the default is a
     change to this assertion. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_EQ(cfg.max_header_read_duration_us, (uint64_t)30000000);
}

TEST(chttpserver, trickled_header_block_is_closed_at_the_duration_ceiling) {
  /* The connection keeps sending real header bytes, so the idle timeout
     (deliberately left at a minute here) never fires and the byte count never
     gets near max_header_bytes. Only the total-duration ceiling can end it. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 103;
  cfg.read_timeout_us = 60000000;
  cfg.idle_timeout_us = 60000000;
  cfg.max_header_read_duration_us = (uint64_t)HDR_DURATION_MS * 1000u;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd = _connect_silent(TEST_PORT + 103);
  bool connected = (fd >= 0);
  bool closed = false;
  if (connected) {
    /* The bound is many times the ceiling plus the sweep's own interval, so it
       only runs out when the ceiling really isn't enforced. */
    closed = _trickle_headers_until_closed(fd, 100, 15000);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_TRUE(closed);
}

TEST(chttpserver, header_read_duration_zero_leaves_a_trickling_peer_alone) {
  /* 0 keeps this knob's documented "no limit" meaning, like every other limit
     in chttpsvr_config_t. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 104;
  cfg.read_timeout_us = 60000000;
  cfg.idle_timeout_us = 60000000;
  cfg.max_header_read_duration_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd = _connect_silent(TEST_PORT + 104);
  bool connected = (fd >= 0);
  bool closed = true;
  if (connected) {
    /* Twice the ceiling of the sibling test, plus the sweep's interval, which
       is long enough that an enforced ceiling of any comparable size would
       already have closed this. */
    closed = _trickle_headers_until_closed(fd, 100, 2 * HDR_DURATION_MS + 1200);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_FALSE(closed);
}

TEST(chttpserver, slow_but_completing_header_block_is_served) {
  /* The ceiling must not break a client that's merely slow. This one sends its
     whole header block a byte at a time, which takes a few hundred
     milliseconds, and gets served normally. The ceiling here is 10 s, far above
     that time even on a loaded machine or under valgrind, so the test only
     checks that a header block that ends under the ceiling is served; the
     sibling trickled_header_block_is_closed_at_the_duration_ceiling checks that
     the ceiling closes a block that doesn't end. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 105;
  cfg.max_header_read_duration_us = 10000000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd = _connect_silent(TEST_PORT + 105);
  bool connected = (fd >= 0);
  int status = -1;
  if (connected) {
    status = _slow_get_on(fd, "/ok", 5);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_EQ(status, 200);
}

TEST(chttpserver, header_read_duration_budget_is_fresh_per_keepalive_request) {
  /* The budget covers the reactor-owned phase of one request, so the second
     request of a keep-alive connection gets its own. The gap between the two is
     deliberately longer than the ceiling and far shorter than the idle timeout:
     a budget anchored at the connection instead of the request would have
     closed this connection during that gap, and the second request would never
     be answered. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 106;
  cfg.read_timeout_us = 60000000;
  cfg.idle_timeout_us = 60000000;
  cfg.max_header_read_duration_us = (uint64_t)HDR_DURATION_MS * 1000u;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd = _connect_silent(TEST_PORT + 106);
  bool connected = (fd >= 0);
  int first = -1, second = -1;
  if (connected) {
    first = _get_on_complete(fd, "/ok");
    /* Split across both fields because tv_nsec must stay below one second. A
       bigger value makes nanosleep fail with EINVAL without sleeping, which
       would leave no gap at all and let this test pass vacuously. */
    struct timespec gap = {
        .tv_sec = (HDR_DURATION_MS + 1000) / 1000,
        .tv_nsec = (long)((HDR_DURATION_MS + 1000) % 1000) * 1000000L};
    nanosleep(&gap, NULL);
    second = _get_on_complete(fd, "/ok");
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_EQ(first, 200);
  REQUIRE_EQ(second, 200);
}

/* ========================================================================== */
/*                 SLOT-TABLE LIVENESS AT PROCESS EXIT                        */
/* ========================================================================== */

TEST(chttpsvr_handle_lifecycle, slot_table_counts_a_mid_teardown_server_live) {
  /* The exit-time destructor releases the slot table's own vectors only when no
     slot is still live. __chttpsvr_destroy clears slot->in_use as its very
     FIRST step, so a second destroy or a new resolve is rejected immediately,
     and then runs the whole rest of its teardown (quiescing the server,
     draining pins, destroying routers) before its last step indexes that same
     table to clear slot->ptr and push the index back onto the free list. During
     that whole window the slot isn't in use but still names a server, and it
     must count as live, or a process exit landing in it would free both
     vectors while the teardown is still going to write through them.

     The window is set up directly instead of racing a real teardown, which a
     test can't reach deterministically. */

  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  bool live_while_resolvable = _chttpsvr_slot_live_for_tests(srv);
  _chttpsvr_slot_set_in_use_for_tests(srv, false);
  bool live_mid_teardown = _chttpsvr_slot_live_for_tests(srv);
  /* Restored before the destroy, because every public entry point,
     chttpsvr_destroy included, refuses a handle whose slot isn't in use. */
  _chttpsvr_slot_set_in_use_for_tests(srv, true);
  /* Kept because chttpsvr_destroy NULLs its argument, and a NULL handle names
     slot 0, which belongs to a completely different server. */
  chttpsvr destroyed = srv;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(live_while_resolvable);
  REQUIRE_TRUE(live_mid_teardown);
  /* The slot is released as the destroy's last step above, so after it the slot
     isn't live and doesn't hold the table open. */
  REQUIRE_FALSE(_chttpsvr_slot_live_for_tests(destroyed));
}

/* ========================================================================== */
/*               REQUEST-TARGET FORM (RFC 9112 SS3.2)                         */
/* ========================================================================== */

/* A middleware that guards a path prefix the way an application would write
   one: it reads chttpsvr_req_path() and refuses everything under "/admin". */
static void _admin_guard_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                            chttpsvr_next_fn next) {
  (void)ctx;
  const char *path = chttpsvr_req_path(req);
  if (path && strncmp(path, "/admin", 6) == 0) {
    chttpsvr_resp_set_status(resp, 403);
    chttpsvr_resp_write_str(resp, "forbidden");
    return;
  }
  next(req, resp);
}

static void _target_path_echo_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *path = chttpsvr_req_path(req);
  chttpsvr_resp_write_str(resp, path ? path : "(null)");
}

#define TARGET_FORM_PORT ((uint16_t)(TEST_PORT + 108))

TEST(chttpserver, request_target_must_be_origin_or_absolute_form) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _admin_guard_mw, NULL), (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/admin/secret",
                                            _prec_secret_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/",
                                            _target_path_echo_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TARGET_FORM_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct {
    const char *method;
    const char *target;
    int status;
    const char *body; /* NULL means the body isn't checked */
  } cases[] = {
      /* The guard sees the real path and refuses it. */
      {"GET", "/admin/secret", 403, "forbidden"},
      /* A target without a leading "/" would route to /admin/secret while the
         guard reads "admin/secret" and lets it through. */
      {"GET", "admin/secret", 400, NULL},
      {"GET", "admin", 400, NULL},
      {"GET", "?x=1", 400, NULL},
      /* The asterisk-form, see options_star_is_answered_by_the_server. */
      {"OPTIONS", "*", 200, ""},
      {"GET", "*", 400, NULL},
      {"POST", "*", 400, NULL},
      /* An absolute-form target with an empty path stands for "/". */
      {"GET", "http://127.0.0.1", 200, "/"},
      {"GET", "http://127.0.0.1?x=1", 200, "/"},
      {"GET", "http://127.0.0.1/", 200, "/"},
      /* The absolute-form path goes through the same guard. */
      {"GET", "http://127.0.0.1/admin/secret", 403, "forbidden"},
      {"GET", "http://127.0.0.1#f", 400, NULL},
      {"GET", "/", 200, "/"},
  };
  size_t n_cases = sizeof(cases) / sizeof(cases[0]);
  int got[16];
  bool body_ok[16];
  bool secret_leaked = false;
  for (size_t i = 0; i < n_cases; i++) {
    char buf[2048] = {0};
    got[i] = _raw_request_on_port(TARGET_FORM_PORT, cases[i].method,
                                  cases[i].target, NULL, buf, sizeof(buf));
    const char *body = strstr(buf, "\r\n\r\n");
    body_ok[i] =
        !cases[i].body || (body && strcmp(body + 4, cases[i].body) == 0);
    if (strstr(buf, "secret") && !strstr(buf, "forbidden"))
      secret_leaked = true;
  }
  chttpsvr_stop(srv);

  for (size_t i = 0; i < n_cases; i++) {
    if (got[i] != cases[i].status || !body_ok[i])
      fprintf(stderr, "target form case %zu (%s %s): status %d\n", i,
              cases[i].method, cases[i].target, got[i]);
  }
  for (size_t i = 0; i < n_cases; i++) {
    REQUIRE_EQ(got[i], cases[i].status);
    REQUIRE_TRUE(body_ok[i]);
  }
  REQUIRE_FALSE(secret_leaked);
}

/* ========================================================================== */
/*           HTTP/1.0 WITH TRANSFER-ENCODING (RFC 9112 SS6.1)                 */
/* ========================================================================== */

TEST(chttpserver, http_1_0_request_with_transfer_encoding_gets_400_and_close) {
  /* The chunked body ends before the second request. A server that de-chunks
     and honours the keep-alive would answer that second request as a request of
     its own, one that a front end framing the first request differently never
     sent. */
  char buf[4096];
  _raw_request_to_test_server(
      "POST /echo-body HTTP/1.0\r\nHost: h\r\n"
      "Transfer-Encoding: chunked\r\nConnection: keep-alive\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n"
      "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "hello") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

/* ========================================================================== */
/*                     "OPTIONS *" (RFC 9112 SS3.2.4)                         */
/* ========================================================================== */

static _Atomic int g_star_mw_calls = 0;
static _Atomic int g_star_handler_calls = 0;

static void _star_counting_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                              chttpsvr_next_fn next) {
  (void)ctx;
  atomic_fetch_add(&g_star_mw_calls, 1);
  next(req, resp);
}

static void _star_counting_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  atomic_fetch_add(&g_star_handler_calls, 1);
  chttpsvr_resp_write_str(resp, "routed");
}

#define STAR_PORT ((uint16_t)(TEST_PORT + 109))

static const char star_ok[] =
    "HTTP/1.1 200 OK\r\ncontent-length:0\r\nconnection:keep-alive\r\n\r\n";

/* Removes the first "date:" header line of the first response in buf, in place,
   and returns buf. The Date tests check that line, and the tests that compare a
   whole response byte for byte compare the rest. */
static char *_strip_date_line(char *buf) {
  char *end = strstr(buf, "\r\n\r\n");
  char *d = strstr(buf, "\r\ndate:");
  if (d && end && d < end) {
    char *eol = strstr(d + 2, "\r\n");
    memmove(d, eol, strlen(eol) + 1);
  }
  return buf;
}

/* Writes wire on a fresh connection and reads until the server closes it or the
   5 s receive timeout runs out. */
static void _star_exchange(const char *wire, size_t wire_len, char *buf,
                           size_t buf_sz) {
  buf[0] = '\0';
  int fd _ccol_destructor(_close_scoped_fd) = _connect_silent(STAR_PORT);
  if (fd < 0) return;
  if (send(fd, wire, wire_len, MSG_NOSIGNAL) != (ssize_t)wire_len) return;
  _drain_socket_until_eof(fd, buf, buf_sz);
}

TEST(chttpserver, options_star_is_answered_by_the_server) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _star_counting_mw, NULL),
             (int)ccol_success);
  /* A catch-all for every method. If the server routed "*" at all, the request
     would either reach this handler or be refused with a 404 that runs the
     middleware, and either way a counter would move. */
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/",
                                            _star_counting_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = STAR_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Every response also has a Date line, which the Date tests pin, so the
     helper below drops it and the rest compares exactly. */
  static const char follow[] =
      "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  /* 1. On its own: 200, Content-Length 0, and nothing else runs. */
  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b1[2048];
  static const char w1[] =
      "OPTIONS * HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
  _star_exchange(w1, sizeof(w1) - 1, b1, sizeof(b1));
  int mw1 = atomic_load(&g_star_mw_calls);
  int h1 = atomic_load(&g_star_handler_calls);

  /* 2. Keep-alive: a request that follows on the same connection is served as
     usual. */
  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b2[4096];
  char w2[512];
  int n2 = snprintf(w2, sizeof(w2), "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n%s",
                    follow);
  _star_exchange(w2, (size_t)n2, b2, sizeof(b2));
  int mw2 = atomic_load(&g_star_mw_calls);
  int h2 = atomic_load(&g_star_handler_calls);

  /* 3. A small body, framed by Content-Length and by chunks, is read and thrown
     away, and the connection stays usable. */
  char b3[4096];
  char w3[512];
  int n3 = snprintf(w3, sizeof(w3),
                    "OPTIONS * HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n"
                    "hello%s",
                    follow);
  _star_exchange(w3, (size_t)n3, b3, sizeof(b3));
  char b4[4096];
  char w4[512];
  int n4 =
      snprintf(w4, sizeof(w4),
               "OPTIONS * HTTP/1.1\r\nHost: h\r\n"
               "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n%s",
               follow);
  _star_exchange(w4, (size_t)n4, b4, sizeof(b4));

  /* 4. A body above 4 KiB still gets 200 and the connection closes, so the
     request after it is never read. */
  char b5[4096];
  char w5[512];
  int n5 = snprintf(w5, sizeof(w5),
                    "OPTIONS * HTTP/1.1\r\nHost: h\r\nContent-Length: 5000\r\n"
                    "\r\n%s",
                    follow);
  _star_exchange(w5, (size_t)n5, b5, sizeof(b5));
  static char w6[8192];
  size_t n6 = (size_t)snprintf(w6, sizeof(w6),
                               "OPTIONS * HTTP/1.1\r\nHost: h\r\n"
                               "Transfer-Encoding: chunked\r\n\r\n1388\r\n");
  memset(w6 + n6, 'x', 5000);
  n6 += 5000;
  n6 += (size_t)snprintf(w6 + n6, sizeof(w6) - n6, "\r\n0\r\n\r\n%s", follow);
  char b6[4096];
  _star_exchange(w6, n6, b6, sizeof(b6));

  /* 5. Any other method with "*" is a 400 and a close. */
  char b7[4096];
  char w7[512];
  int n7 =
      snprintf(w7, sizeof(w7), "GET * HTTP/1.1\r\nHost: h\r\n\r\n%s", follow);
  _star_exchange(w7, (size_t)n7, b7, sizeof(b7));

  chttpsvr_stop(srv);

  REQUIRE_STREQ(
      _strip_date_line(b1),
      "HTTP/1.1 200 OK\r\ncontent-length:0\r\nconnection:close\r\n\r\n");
  REQUIRE_EQ(mw1, 0);
  REQUIRE_EQ(h1, 0);

  _strip_date_line(b2);
  REQUIRE_EQ(strncmp(b2, star_ok, sizeof(star_ok) - 1), 0);
  REQUIRE_TRUE(strstr(b2 + sizeof(star_ok) - 1, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(b2, "routed") != NULL);
  REQUIRE_EQ(mw2, 1);
  REQUIRE_EQ(h2, 1);

  REQUIRE_EQ(strncmp(_strip_date_line(b3), star_ok, sizeof(star_ok) - 1), 0);
  REQUIRE_TRUE(strstr(b3, "routed") != NULL);
  REQUIRE_EQ(strncmp(_strip_date_line(b4), star_ok, sizeof(star_ok) - 1), 0);
  REQUIRE_TRUE(strstr(b4, "routed") != NULL);

  REQUIRE_STREQ(
      _strip_date_line(b5),
      "HTTP/1.1 200 OK\r\ncontent-length:0\r\nconnection:close\r\n\r\n");
  REQUIRE_STREQ(
      _strip_date_line(b6),
      "HTTP/1.1 200 OK\r\ncontent-length:0\r\nconnection:close\r\n\r\n");

  REQUIRE_EQ(strncmp(b7, "HTTP/1.1 400", 12), 0);
  REQUIRE_TRUE(strstr(b7, "connection:close") != NULL);
  REQUIRE_TRUE(strstr(b7, "routed") == NULL);
}

/* ========================================================================== */
/*                     THE DATE RESPONSE HEADER                               */
/* ========================================================================== */

static void _date_own_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "date", "Mon, 01 Jan 2001 00:00:00 GMT");
  chttpsvr_resp_set_header(resp, "DATE", "Tue, 02 Jan 2001 00:00:00 GMT");
  chttpsvr_resp_write_str(resp, "own-date");
}

TEST(chttpserver, every_response_class_carries_one_fresh_date) {
  struct {
    const char *wire;
    int responses;
  } cases[] = {
      {"GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"HEAD /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"POST /stream-echo HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n"
       "Connection: close\r\n\r\nabc",
       1},
      {"GET /no-such-route HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
       1},
      {"PUT /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"GET hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"GET /hello HTTP/1.1\r\n\r\n", 1},
      {"PROPFIND /x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"OPTIONS * HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", 1},
      {"GET * HTTP/1.1\r\nHost: h\r\n\r\n", 1},
      /* Two pipelined requests, the keep-alive response and the last one. */
      {"GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"
       "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
       2},
  };
  size_t n_cases = sizeof(cases) / sizeof(cases[0]);
  int good[16], total[16];
  for (size_t i = 0; i < n_cases; i++) {
    char buf[4096];
    _raw_request_to_test_server(cases[i].wire, buf, sizeof(buf));
    good[i] = _responses_with_one_date(buf, time(NULL), 5, &total[i]);
  }

  /* 413 is a rejection of the small-body server. */
  char buf413[2048] = {0};
  int st413 =
      _raw_request_on_port(TEST_PORT + 3, "POST", "/small-body-echo",
                           "Content-Length: 1000\r\n", buf413, sizeof(buf413));
  int total413 = 0;
  int good413 = _responses_with_one_date(buf413, time(NULL), 5, &total413);

  for (size_t i = 0; i < n_cases; i++) {
    if (good[i] != cases[i].responses || total[i] != cases[i].responses)
      fprintf(stderr, "date case %zu: %d of %d good, expected %d\n", i, good[i],
              total[i], cases[i].responses);
  }
  for (size_t i = 0; i < n_cases; i++) {
    REQUIRE_EQ(total[i], cases[i].responses);
    REQUIRE_EQ(good[i], cases[i].responses);
  }
  REQUIRE_EQ(st413, 413);
  REQUIRE_EQ(total413, 1);
  REQUIRE_EQ(good413, 1);
}

TEST(chttpserver, interim_100_continue_carries_no_date) {
  /* Only a final response carries a Date, like in Go's net/http server. */
  int fd _ccol_destructor(_close_scoped_fd) = _connect_silent(TEST_PORT);
  REQUIRE_TRUE(fd >= 0);
  static const char head[] =
      "POST /echo-body HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n"
      "Expect: 100-continue\r\nConnection: close\r\n\r\n";
  REQUIRE_EQ(send(fd, head, sizeof(head) - 1, MSG_NOSIGNAL),
             (ssize_t)(sizeof(head) - 1));
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
  char interim[sizeof(cont)] = {0};
  size_t got = 0;
  while (got < sizeof(cont) - 1) {
    ssize_t r = recv(fd, interim + got, sizeof(cont) - 1 - got, 0);
    if (r <= 0) break;
    got += (size_t)r;
  }
  REQUIRE_STREQ(interim, cont);
  REQUIRE_EQ(send(fd, "hello", 5, MSG_NOSIGNAL), (ssize_t)5);
  char buf[2048];
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  int total = 0;
  REQUIRE_EQ(_responses_with_one_date(buf, time(NULL), 5, &total), 1);
  REQUIRE_EQ(total, 1);
  REQUIRE_TRUE(strstr(buf, "hello") != NULL);
}

TEST(chttpserver, a_date_that_the_handler_sets_wins) {
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/own-date",
                                            _date_own_handler, NULL),
             (int)ccol_success);
  char buf[2048];
  _raw_request_to_test_server(
      "GET /own-date HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", buf,
      sizeof(buf));
  char val[64];
  REQUIRE_TRUE(_first_date_value(buf, val, sizeof(val)));
  REQUIRE_STREQ(val, "Tue, 02 Jan 2001 00:00:00 GMT");
  REQUIRE_TRUE(strstr(buf, "own-date") != NULL);
}

TEST(chttpserver, date_follows_the_clock_across_a_second_boundary) {
  static const struct {
    int64_t sec;
    const char *text;
  } steps[] = {
      {784111777, "Sun, 06 Nov 1994 08:49:37 GMT"},
      {784111778, "Sun, 06 Nov 1994 08:49:38 GMT"},
      {784166399, "Sun, 06 Nov 1994 23:59:59 GMT"},
      {784166400, "Mon, 07 Nov 1994 00:00:00 GMT"},
      {951782400, "Tue, 29 Feb 2000 00:00:00 GMT"},
      {4102444799, "Thu, 31 Dec 2099 23:59:59 GMT"},
  };
  size_t n = sizeof(steps) / sizeof(steps[0]);
  /* On a target with a 32-bit time_t the last step doesn't fit and the library
     writes the epoch, so it's only checked where it fits. */
  if (sizeof(time_t) < 8) n--;
  char got[8][64];
  bool ok[8];
  for (size_t i = 0; i < n; i++) {
    _chttpsvr_set_date_override_for_tests(steps[i].sec);
    char buf[2048];
    _raw_request_to_test_server(
        "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", buf,
        sizeof(buf));
    ok[i] = _first_date_value(buf, got[i], sizeof(got[i]));
  }
  _chttpsvr_set_date_override_for_tests(INT64_MIN);
  for (size_t i = 0; i < n; i++) {
    REQUIRE_TRUE(ok[i]);
    REQUIRE_STREQ(got[i], steps[i].text);
  }
}

/* ========================================================================== */
/*          AN UNKNOWN METHOD WITH THE ASTERISK-FORM TARGET                   */
/* ========================================================================== */

TEST(chttpserver, unknown_method_with_asterisk_target_gets_400) {
  /* Go's net/http answers 400 for the target "*" with any method other than
     OPTIONS. An unknown method with an ordinary path keeps its 501. */
  char b1[1024], b2[1024], b3[1024];
  _raw_request_to_test_server("PROPFIND * HTTP/1.1\r\nHost: h\r\n\r\n", b1,
                              sizeof(b1));
  _raw_request_to_test_server("XYZZY-42 * HTTP/1.1\r\nHost: h\r\n\r\n", b2,
                              sizeof(b2));
  _raw_request_to_test_server(
      "PROPFIND /x HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", b3,
      sizeof(b3));
  REQUIRE_EQ(strncmp(b1, "HTTP/1.1 400", 12), 0);
  REQUIRE_TRUE(strstr(b1, "connection:close") != NULL);
  REQUIRE_EQ(strncmp(b2, "HTTP/1.1 400", 12), 0);
  REQUIRE_TRUE(strstr(b2, "connection:close") != NULL);
  REQUIRE_EQ(strncmp(b3, "HTTP/1.1 501", 12), 0);
}

static void _status_103_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 103);
}

TEST(chttpserver, a_1xx_status_from_a_handler_goes_out_as_500_with_a_date) {
  /* A handler that sets 103 as its final status gets a complete 500 with a
     Date, never an interim 103 that leaves the client waiting. */
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/status-103",
                                            _status_103_handler, NULL),
             (int)ccol_success);
  char buf[1024];
  _raw_request_to_test_server(
      "GET /status-103 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", buf,
      sizeof(buf));
  REQUIRE_EQ(strncmp(buf, "HTTP/1.1 500", 12), 0);
  char val[64];
  REQUIRE_TRUE(_first_date_value(buf, val, sizeof(val)));
}

/* ========================================================================== */
/*                    SLOW CLIENTS                                            */
/* ========================================================================== */

/* These tests drive the parking of slow bodies and slow readers, the rate
 * floor, the partial-body memory limit and the streaming pool. Every wait on
 * the server is a bounded poll of some white-box state (never a sleep that
 * correctness depends on), and every time limit the sweep enforces is crossed
 * with the injected clock of _chttpsvr_advance_slow_clock_for_tests and a sweep
 * tick run on the test thread with _chttpsvr_sweep_now_for_tests. Each test
 * ends by asserting that no body memory stays reserved. */

#define _SC_PORT (TEST_PORT + 150)
#define _SC_BIG_RESPONSE ((size_t)8 * 1024 * 1024)

static long long _sc_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000L;
}

static void _sc_nap_ms(long ms) {
  struct timespec nap = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&nap, NULL);
}

/* Connects to 127.0.0.1:port. The socket gets a 5 s receive timeout, so that a
 * regression fails a read instead of hanging the binary, and TCP_NODELAY, so
 * every write goes out without delay: with Nagle on, a second small write waits
 * for the acknowledgement of the first, which a kernel that delays
 * acknowledgements on loopback (FreeBSD does) holds back, and the bytes a test
 * has "sent" haven't reached the server yet. */
static int _sc_connect(int port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  /* A test sends a body that a server which has stopped reading leaves in the
   * kernel, so the send buffer holds it whatever the system default is (OpenBSD
   * gives 16 KiB), and a send that still stalls fails after 30 s instead of
   * waiting forever. */
  int sndbuf = 256 * 1024;
  setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
  struct timeval sndto = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sndto, sizeof(sndto));
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int one = 1;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  return fd;
}

static bool _sc_send(int fd, const char *s, size_t n) {
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) return false;
    s += w;
    n -= (size_t)w;
  }
  return true;
}

static bool _sc_send_str(int fd, const char *s) {
  return _sc_send(fd, s, strlen(s));
}

/* Reads one response from fd into buf (the headers, and as much of the body as
 * fits, NUL-terminated) and returns its status, or -1 on a timeout or an end of
 * stream before a complete response. *body_len_out gets the length of the body
 * that arrived, which can be more than buf holds, since the rest is read and
 * thrown away. timeout_ms bounds the whole read. */
static int _sc_read_response_x(int fd, char *buf, size_t cap,
                               size_t *body_len_out, int timeout_ms,
                               bool head_only) {
  long long deadline = _sc_now_ms() + timeout_ms;
  size_t got = 0;
  size_t head_len = 0;
  size_t content_length = 0;
  size_t body = 0;
  bool no_body = false;
  if (body_len_out) *body_len_out = 0;
  for (;;) {
    long long left = deadline - _sc_now_ms();
    if (left <= 0) return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, (int)left) <= 0) return -1;
    char tmp[65536];
    size_t want = sizeof(tmp);
    if (head_len) {
      size_t rest = content_length - body;
      if (rest < want) want = rest;
    } else {
      want = 1; /* byte by byte through the head, so no pipelined byte of a
                   next response is consumed */
    }
    ssize_t r = read(fd, tmp, want);
    if (r <= 0) return -1;
    if (!head_len) {
      if (got + 1 < cap) buf[got] = tmp[0];
      got++;
      buf[got < cap ? got : cap - 1] = '\0';
      if (got >= 4 && got < cap && memcmp(buf + got - 4, "\r\n\r\n", 4) == 0) {
        head_len = got;
        const char *cl = strstr(buf, "content-length:");
        content_length = cl ? strtoul(cl + 15, NULL, 10) : 0;
        int status = atoi(buf + 9);
        no_body = head_only || status == 204 || status == 304 ||
                  (status >= 100 && status < 200);
        if (no_body || content_length == 0) break;
      }
      continue;
    }
    for (ssize_t i = 0; i < r; i++, body++)
      if (head_len + body + 1 < cap) buf[head_len + body] = tmp[i];
    buf[head_len + body + 1 < cap ? head_len + body : cap - 1] = '\0';
    if (body >= content_length) break;
  }
  if (body_len_out) *body_len_out = body;
  return atoi(buf + 9);
}

static int _sc_read_response(int fd, char *buf, size_t cap,
                             size_t *body_len_out, int timeout_ms) {
  return _sc_read_response_x(fd, buf, cap, body_len_out, timeout_ms, false);
}

/* Reads until the end of the stream, for at most timeout_ms, and returns the
 * number of bytes, or -1 if the stream didn't end in time. */
static long _sc_drain_to_eof(int fd, int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  long total = 0;
  for (;;) {
    long long left = deadline - _sc_now_ms();
    if (left <= 0) return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, (int)left) <= 0) return -1;
    char tmp[65536];
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r == 0) return total;
    if (r < 0) return errno == ECONNRESET ? total : -1;
    total += r;
  }
}

/* Waits, for at most timeout_ms, until a server counter reaches at least want.
 * It polls a white-box accessor, and the bound only guards against a hang. */
typedef size_t (*_sc_probe_fn)(chttpsvr h, void *arg);
static bool _sc_wait_for(chttpsvr h, _sc_probe_fn probe, void *arg, size_t want,
                         int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  while (probe(h, arg) < want) {
    if (_sc_now_ms() > deadline) return false;
    _sc_nap_ms(1);
  }
  return true;
}
static bool _sc_wait_for_zero(chttpsvr h, _sc_probe_fn probe, void *arg,
                              int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  while (probe(h, arg) != 0) {
    if (_sc_now_ms() > deadline) return false;
    _sc_nap_ms(1);
  }
  return true;
}
static size_t _sc_parked_bodies(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_parked_count_for_tests(h, 1);
}
static size_t _sc_parked_writes(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_parked_count_for_tests(h, 2);
}
static size_t _sc_mem_waiting(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_mem_waiting_for_tests(h);
}
static size_t _sc_mem_in_use(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_mem_in_use_for_tests(h);
}
static size_t _sc_stream_queue(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_stream_queue_len_for_tests(h);
}
static size_t _sc_stream_active(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_pool_active_for_tests(h, true);
}

/* Answers with the length of the body it got. */
static void _sc_len_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t n = 0;
  chttpsvr_req_body(req, &n);
  chttpsvr_resp_printf(resp, "%zu", n);
}

/* Answers with _SC_BIG_RESPONSE bytes, far more than the socket buffers of a
 * client that doesn't read can hold. */
static void _sc_big_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  char chunk[65536];
  memset(chunk, 'b', sizeof(chunk));
  for (size_t i = 0; i < _SC_BIG_RESPONSE / sizeof(chunk); i++)
    chttpsvr_resp_write(resp, chunk, sizeof(chunk));
}

static void _sc_hello_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

/* Answers with 100 bytes of known text. */
static void _sc_text_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  (void)req;
  (void)ctx;
  char text[101];
  for (int i = 0; i < 100; i++) text[i] = (char)('a' + i % 26);
  text[100] = '\0';
  chttpsvr_resp_write_str(resp, text);
}

static void _sc_204_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 204);
  chttpsvr_resp_write_str(resp, "never on the wire");
}

/* A streaming handler that counts its runs and then holds on to its thread
 * until the test opens the gate, for at most 10 s. */
static pthread_mutex_t g_sc_gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sc_gate_cv = PTHREAD_COND_INITIALIZER;
static bool g_sc_gate_open = false;
static _Atomic int g_sc_gate_runs = 0;
static void _sc_gate_open(bool open) {
  pthread_mutex_lock(&g_sc_gate_mtx);
  g_sc_gate_open = open;
  pthread_cond_broadcast(&g_sc_gate_cv);
  pthread_mutex_unlock(&g_sc_gate_mtx);
}
static void _sc_gate_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  atomic_fetch_add(&g_sc_gate_runs, 1);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 10;
  pthread_mutex_lock(&g_sc_gate_mtx);
  while (!g_sc_gate_open)
    if (pthread_cond_timedwait(&g_sc_gate_cv, &g_sc_gate_mtx, &deadline) ==
        ETIMEDOUT)
      break;
  pthread_mutex_unlock(&g_sc_gate_mtx);
  chttpsvr_resp_write_str(resp, "gated");
}

/* The same gate for a buffered route, which runs on the worker pool. */
static void _sc_gate_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  _sc_gate_stream_handler(req, resp, ctx);
}

/* Starts a fresh server on port with the routes these tests use. */
static chttpsvr _sc_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return CHTTPSVR_INVALID;
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _sc_len_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_OPTIONS, "/len", _sc_len_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/big", _sc_big_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _sc_hello_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/text", _sc_text_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/no-content", _sc_204_handler,
                            NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/gate",
                                      _sc_gate_stream_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/busy", _sc_gate_handler, NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/stream-err",
                                      _stream_error_report_handler, NULL);
  chttpsvr_config_t cfg = *extra;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  return srv;
}

/* Sends the headers of a POST that declares cl body bytes, plus the first part
 * bytes of that body, and nothing else. */
static int _sc_open_partial_post(int port, const char *path, size_t cl,
                                 size_t part, const char *extra_hdr) {
  int fd = _sc_connect(port);
  if (fd < 0) return -1;
  char hdr[512];
  int n =
      snprintf(hdr, sizeof(hdr),
               "POST %s HTTP/1.1\r\nHost: h\r\n%sContent-Length: %zu\r\n\r\n",
               path, extra_hdr ? extra_hdr : "", cl);
  char *body = malloc(part ? part : 1);
  memset(body, 'p', part ? part : 1);
  bool ok = _sc_send(fd, hdr, (size_t)n) && _sc_send(fd, body, part);
  free(body);
  if (!ok) {
    close(fd);
    return -1;
  }
  return fd;
}

static bool _sc_send_fill(int fd, size_t n, char c) {
  char *b = malloc(n ? n : 1);
  memset(b, c, n ? n : 1);
  bool ok = _sc_send(fd, b, n);
  free(b);
  return ok;
}

/* The declared length of a body-memory holder. A body this short reserves its
 * whole buffer before its first byte and never grows it. */
#define _SC_HOLD_LEN ((size_t)16384)

/* Opens n POSTs to /len that each declare _SC_HOLD_LEN bytes and send 10 of
 * them, and waits until every one holds its reservation. fds gets every socket
 * that opened (-1 for the others). Returns true when all n hold. */
static bool _sc_hold_memory(chttpsvr srv, int port, int *fds, int n) {
  bool ok = true;
  for (int i = 0; i < n; i++) {
    fds[i] = _sc_open_partial_post(port, "/len", _SC_HOLD_LEN, 10, NULL);
    if (fds[i] < 0) ok = false;
  }
  return ok && _sc_wait_for(srv, _sc_mem_in_use, NULL, (size_t)n * _SC_HOLD_LEN,
                            5000);
}

/* Sends the rest of the body of every holder that _sc_hold_memory opened and
 * reads its response. Returns how many holders got 200. */
static int _sc_release_holders(int *fds, int n) {
  int served = 0;
  char buf[4096];
  for (int i = 0; i < n; i++)
    if (fds[i] >= 0 && _sc_send_fill(fds[i], _SC_HOLD_LEN - 10, 'h') &&
        _sc_read_response(fds[i], buf, sizeof(buf), NULL, 5000) == 200)
      served++;
  return served;
}

static void _sc_close_all(int *fds, int n) {
  for (int i = 0; i < n; i++)
    if (fds[i] >= 0) close(fds[i]);
}

TEST(slow_clients, slow_bodies_park_and_hold_no_worker_thread) {
  /* Six clients stop halfway through their bodies on a server with two workers.
   * Each one has to park without a thread, so the worker pool stays idle and an
   * ordinary request still gets served. A body reader that waited on the socket
   * would block both workers, the six connections would never reach the parked
   * state, and the GET would wait behind them. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  int fds[6];
  bool opened = true;
  for (int i = 0; i < 6; i++) {
    fds[i] = _sc_open_partial_post(_SC_PORT, "/len", 1000, 10, NULL);
    if (fds[i] < 0) opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, 6, 5000);
  size_t active = _chttpsvr_pool_active_for_tests(srv, false);

  char buf[4096];
  int hello_status = -1;
  int fd = _sc_connect(_SC_PORT);
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello_status = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  if (fd >= 0) close(fd);

  int completed = 0;
  for (int i = 0; i < 6; i++) {
    if (fds[i] < 0) continue;
    if (_sc_send_fill(fds[i], 990, 'q') &&
        _sc_read_response(fds[i], buf, sizeof(buf), NULL, 5000) == 200 &&
        strstr(buf, "\r\n\r\n1000") != NULL)
      completed++;
    close(fds[i]);
  }
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(active, (size_t)0);
  REQUIRE_EQ(hello_status, 200);
  REQUIRE_EQ(completed, 6);
  REQUIRE_TRUE(mem_zero);
}

/* Connects with a small receive buffer, so a client that doesn't read leaves
 * most of a large response in the server. */
static int _sc_connect_small_rcvbuf(int port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int small = 4096;
  setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

/* One client of slow_readers_park_and_hold_no_worker_thread. It reads its large
 * response and then asks for /hello on the same connection. */
typedef struct {
  int fd;
  bool whole;
} _sc_big_reader_t;

static void *_sc_big_reader_main(void *arg) {
  _sc_big_reader_t *r = arg;
  char buf[4096];
  size_t body = 0;
  r->whole = _sc_read_response(r->fd, buf, sizeof(buf), &body, 60000) == 200 &&
             body == _SC_BIG_RESPONSE &&
             _sc_send_str(r->fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") &&
             _sc_read_response(r->fd, buf, sizeof(buf), NULL, 5000) == 200;
  return NULL;
}

TEST(slow_clients, slow_readers_park_and_hold_no_worker_thread) {
  /* Four clients ask for a response far bigger than the socket buffers and read
   * none of it, on a server with two workers. Each response parks on a full
   * socket without a thread. A response writer that waited on the socket
   * would block both workers, and the parked count would never reach four. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 1, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  int fds[4];
  bool opened = true;
  for (int i = 0; i < 4; i++) {
    fds[i] = _sc_connect_small_rcvbuf(_SC_PORT + 1);
    if (fds[i] < 0 ||
        !_sc_send_str(fds[i], "GET /big HTTP/1.1\r\nHost: h\r\n\r\n"))
      opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_writes, NULL, 4, 10000);
  size_t active = _chttpsvr_pool_active_for_tests(srv, false);

  char buf[4096];
  int hello_status = -1;
  int fd = _sc_connect(_SC_PORT + 1);
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello_status = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  if (fd >= 0) close(fd);

  /* Then every response arrives whole and the connection stays usable. The four
   * clients all read at once, because one that waited idle while the others
   * read would drop below the transfer-rate floor once that wait passes its
   * grace, which is exactly what the floor is for. */
  _sc_big_reader_t readers[4];
  pthread_t tids[4];
  bool started[4] = {false, false, false, false};
  for (int i = 0; i < 4; i++) {
    readers[i].fd = fds[i];
    readers[i].whole = false;
    if (fds[i] >= 0)
      started[i] =
          pthread_create(&tids[i], NULL, _sc_big_reader_main, &readers[i]) == 0;
  }
  int whole = 0;
  for (int i = 0; i < 4; i++) {
    if (started[i]) {
      pthread_join(tids[i], NULL);
      if (readers[i].whole) whole++;
    }
    if (fds[i] >= 0) close(fds[i]);
  }

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(active, (size_t)0);
  REQUIRE_EQ(hello_status, 200);
  REQUIRE_EQ(whole, 4);
}

TEST(slow_clients, rate_floor_keeps_a_body_just_above_it) {
  /* 1000 of 2000 declared bytes in 5 s is 200 B/s, above a floor of 100, so the
   * sweep keeps the connection and the rest of the body completes it. A floor
   * that measured the wrong span or the wrong byte count would close it. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 2, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 2, "/len", 2000, 1000, NULL);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  _chttpsvr_advance_slow_clock_for_tests(5000);
  _chttpsvr_sweep_now_for_tests(srv);
  size_t still_parked = _chttpsvr_parked_count_for_tests(srv, 1);
  char buf[4096];
  int status = -1;
  if (_sc_send_fill(fd, 1000, 'r'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(still_parked, (size_t)1);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n2000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

static size_t _sc_workers_active(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_pool_active_for_tests(h, false);
}
static size_t _sc_resume_dispatches(chttpsvr h, void *arg) {
  (void)h;
  (void)arg;
  return _chttpsvr_resume_dispatch_count_for_tests();
}

TEST(slow_clients, queue_wait_of_a_resumed_body_is_never_charged) {
  /* A parked body whose rest has arrived waits in the only worker's queue while
   * that worker is busy. The clock moves 60 s during that wait, past the 30 s
   * gap limit and past the rate floor, but the bytes were already in the socket
   * when the reactor claimed the connection, so the request completes with 200.
   * A resume that judged the limits when the worker gets to it, and not when
   * the connection became ready, would answer 408. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 1;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 30, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _sc_gate_open(false);

  int fd = _sc_open_partial_post(_SC_PORT + 30, "/len", 1000, 500, NULL);
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);

  int busy = _sc_connect(_SC_PORT + 30);
  bool busy_sent =
      busy >= 0 && _sc_send_str(busy, "GET /busy HTTP/1.1\r\nHost: h\r\n\r\n");
  bool worker_held =
      busy_sent && _sc_wait_for(srv, _sc_workers_active, NULL, 1, 5000);

  size_t dispatches = _chttpsvr_resume_dispatch_count_for_tests();
  bool rest_sent = fd >= 0 && _sc_send_fill(fd, 500, 'q');
  bool claimed = rest_sent && _sc_wait_for(srv, _sc_resume_dispatches, NULL,
                                           dispatches + 1, 5000);
  if (claimed) _chttpsvr_advance_slow_clock_for_tests(60000);
  _sc_gate_open(true);

  char buf[4096];
  char busy_buf[4096];
  int status =
      fd >= 0 ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int busy_status = busy >= 0 ? _sc_read_response(busy, busy_buf,
                                                  sizeof(busy_buf), NULL, 5000)
                              : -1;
  if (fd >= 0) close(fd);
  if (busy >= 0) close(busy);
  _sc_gate_open(false);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);

  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(worker_held);
  REQUIRE_TRUE(claimed);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n1000") != NULL);
  REQUIRE_EQ(busy_status, 200);
  REQUIRE_TRUE(mem_zero);
}

TEST(slow_clients, rate_floor_answers_a_slow_body_with_408) {
  /* 1000 bytes in 11 s is about 91 B/s, below a floor of 100, so the sweep
   * answers 408 and closes. Without the floor both the 30 s per-gap limit and
   * the 5 minute total limit still hold, and the connection would stay parked.
   * */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 3, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 3, "/len", 2000, 1000, NULL);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  _chttpsvr_advance_slow_clock_for_tests(11000);
  _chttpsvr_sweep_now_for_tests(srv);
  char buf[4096];
  int status = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  long rest = _sc_drain_to_eof(fd, 3000);
  REQUIRE_EQ(status, 408);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
  REQUIRE_TRUE(rest >= 0);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, rate_floor_closes_a_slow_reader) {
  /* A client that reads none of a large response falls below the write floor
   * once the grace period is over, and the sweep closes the connection with the
   * response cut off. The per-gap write limit is 10 minutes here and the total
   * one 5 minutes of real time, so without the floor nothing else would close
   * it and the drain would get the whole response. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 1024 * 1024;
  cfg.min_transfer_rate_grace_us = 1000000;
  /* Only the floor may close it, since the per-gap write limit is far above the
   * 60 s the clock moves. */
  cfg.response_write_timeout_us = 600000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 4, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_connect_small_rcvbuf(_SC_PORT + 4);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_send_str(fd, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n"));
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_writes, NULL, 1, 10000));
  _chttpsvr_advance_slow_clock_for_tests(60000);
  _chttpsvr_sweep_now_for_tests(srv);
  size_t parked_after = _chttpsvr_parked_count_for_tests(srv, 2);
  long got = _sc_drain_to_eof(fd, 10000);
  REQUIRE_EQ(parked_after, (size_t)0);
  REQUIRE_TRUE(got >= 0);
  REQUIRE_LT((size_t)got, _SC_BIG_RESPONSE);
}

TEST(slow_clients, rate_floor_ends_a_slow_streaming_read) {
  /* A streaming handler waiting for a stalled body gives up when the rate floor
   * is crossed, well before the 30 s of stream_read_timeout_us: 10 bytes buy
   * 10 ms at 1000 B/s, so the floor fires when the 300 ms grace ends.
   * Without the floor, the read would wait the full 30 s and the 5 s bound of
   * this read would fail. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 1000;
  cfg.min_transfer_rate_grace_us = 300000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 5, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 5, "/stream-err", 1000, 10, NULL);
  REQUIRE_TRUE(fd >= 0);
  long long t0 = _sc_now_ms();
  char buf[4096];
  int status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  long long took = _sc_now_ms() - t0;
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);
  REQUIRE_LT(took, 5000LL);
}

TEST(slow_clients, time_waiting_for_memory_is_not_charged_to_the_client) {
  /* A chunked body reads into its reservation, runs out and waits for memory
   * while a body with a Content-Length holds the rest. 60 s of that wait pass
   * on the clock, then the other body fails the floor and gives its memory
   * back, and the chunked body goes on. Those 60 s were the server's, so the
   * floor of 10000 B/s must not count them, and the body completes.
   * Counting the wait puts about 57 KiB over 60 s, under the floor, and the
   * resumed body would get 408. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 10000;
  cfg.min_transfer_rate_grace_us = 1000000;
  cfg.max_partial_body_memory = 128 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 6, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  int holder _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 6, "/len", 30000, 10, NULL);
  REQUIRE_TRUE(holder >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));

  int chunked _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 6);
  REQUIRE_TRUE(chunked >= 0);
  const size_t chunk = 100000;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /len HTTP/1.1\r\nHost: h\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n%zx\r\n",
                    chunk);
  REQUIRE_TRUE(_sc_send(chunked, head, (size_t)hn));
  REQUIRE_TRUE(_sc_send_fill(chunked, chunk, 'c'));
  REQUIRE_TRUE(_sc_send_str(chunked, "\r\n0\r\n\r\n"));
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000));

  _chttpsvr_advance_slow_clock_for_tests(60000);
  _chttpsvr_sweep_now_for_tests(srv);
  char buf[4096];
  int holder_status = _sc_read_response(holder, buf, sizeof(buf), NULL, 3000);
  int status = _sc_read_response(chunked, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(holder_status, 408);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n100000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, a_chunked_body_near_the_limit_needs_no_more_than_the_limit) {
  /* A chunked body of exactly max_body_size arrives while another body holds
   * most of the memory. Its last reads come within one 8 KiB read of the limit,
   * and the buffer can never grow past the limit because every byte beyond it
   * is refused, so its reservation never asks for more than the limit: it fits
   * next to the holder and completes without waiting. A reservation sized for
   * growth past the limit would ask for three times the limit, wait for
   * memory, and end in 503 at its 2 s memory deadline. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 256 * 1024;
  cfg.max_partial_body_memory = 700 * 1024;
  cfg.body_memory_wait_timeout_us = 2000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 32, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  int holder _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 32, "/len", 256 * 1024, 1000, NULL);
  REQUIRE_TRUE(holder >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));

  int chunked _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 32);
  REQUIRE_TRUE(chunked >= 0);
  const size_t body = 256 * 1024;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /len HTTP/1.1\r\nHost: h\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n%zx\r\n",
                    body);
  REQUIRE_TRUE(_sc_send(chunked, head, (size_t)hn));
  REQUIRE_TRUE(_sc_send_fill(chunked, body, 'c'));
  REQUIRE_TRUE(_sc_send_str(chunked, "\r\n0\r\n\r\n"));
  char buf[4096];
  int status = _sc_read_response(chunked, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n262144") != NULL);
}

TEST(slow_clients, queue_wait_after_a_memory_wait_is_never_charged) {
  /* A chunked body waits for memory, as in the test above. When the memory
   * frees up the only worker is busy, so the admitted body waits in the pool's
   * queue, and 60 s pass on the clock during that wait too, past the 30 s gap
   * limit and the rate floor. The body completes with 200, where a resume that
   * judged the limits when the worker gets to it, and not when the admission
   * handed the body to the pool, would answer 408. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 1;
  cfg.min_transfer_rate_bps = 10000;
  cfg.min_transfer_rate_grace_us = 1000000;
  cfg.max_partial_body_memory = 128 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 31, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _sc_gate_open(false);

  int holder = _sc_open_partial_post(_SC_PORT + 31, "/len", 30000, 10, NULL);
  bool holder_parked =
      holder >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);

  int chunked = _sc_connect(_SC_PORT + 31);
  const size_t chunk = 100000;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /len HTTP/1.1\r\nHost: h\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n%zx\r\n",
                    chunk);
  bool chunked_sent = chunked >= 0 && _sc_send(chunked, head, (size_t)hn) &&
                      _sc_send_fill(chunked, chunk, 'c') &&
                      _sc_send_str(chunked, "\r\n0\r\n\r\n");
  bool mem_wait =
      chunked_sent && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);

  int busy = _sc_connect(_SC_PORT + 31);
  bool worker_held =
      busy >= 0 &&
      _sc_send_str(busy, "GET /busy HTTP/1.1\r\nHost: h\r\n\r\n") &&
      _sc_wait_for(srv, _sc_workers_active, NULL, 1, 5000);

  /* The holder drops below the floor, the sweep answers it, and its memory
   * admits the chunked body into the busy worker's queue. */
  bool admitted = false;
  if (holder_parked && mem_wait && worker_held) {
    _chttpsvr_advance_slow_clock_for_tests(60000);
    _chttpsvr_sweep_now_for_tests(srv);
    admitted = _sc_wait_for_zero(srv, _sc_mem_waiting, NULL, 5000);
    if (admitted) _chttpsvr_advance_slow_clock_for_tests(60000);
  }
  _sc_gate_open(true);

  char buf[4096];
  char other[4096];
  int holder_status =
      holder >= 0 ? _sc_read_response(holder, other, sizeof(other), NULL, 5000)
                  : -1;
  int status = chunked >= 0
                   ? _sc_read_response(chunked, buf, sizeof(buf), NULL, 5000)
                   : -1;
  int busy_status =
      busy >= 0 ? _sc_read_response(busy, other, sizeof(other), NULL, 5000)
                : -1;
  if (holder >= 0) close(holder);
  if (chunked >= 0) close(chunked);
  if (busy >= 0) close(busy);
  _sc_gate_open(false);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);

  REQUIRE_TRUE(holder_parked);
  REQUIRE_TRUE(mem_wait);
  REQUIRE_TRUE(worker_held);
  REQUIRE_TRUE(admitted);
  REQUIRE_EQ(holder_status, 408);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n100000") != NULL);
  REQUIRE_EQ(busy_status, 200);
  REQUIRE_TRUE(mem_zero);
}

TEST(slow_clients, a_body_that_does_not_fit_waits_unread_then_completes) {
  /* A body framed by a Content-Length reserves its first buffer before anything
   * reads it. With 98304 of 100000 bytes held by six other bodies, a body of
   * 50000 waits: no worker takes it and it gets no "100 Continue". Once the
   * other bodies complete it's admitted, invited, read and answered.
   * Without the reservation the second request would be invited immediately and
   * nothing would wait. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 7, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 7, holders, 6);
  int b = held ? _sc_open_partial_post(_SC_PORT + 7, "/len", 50000, 0,
                                       "Expect: 100-continue\r\n")
               : -1;
  bool waiting = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  struct pollfd p = {.fd = b, .events = POLLIN};
  int invited_early = b >= 0 ? poll(&p, 1, 0) : -1;

  char buf[4096];
  int served = held ? _sc_release_holders(holders, 6) : 0;
  int interim =
      b >= 0 ? _sc_read_response(b, buf, sizeof(buf), NULL, 5000) : -1;
  int b_status = -1;
  if (interim == 100 && _sc_send_fill(b, 50000, 'b'))
    b_status = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  _sc_close_all(holders, 6);
  if (b >= 0) close(b);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(waiting);
  REQUIRE_EQ(invited_early, 0);
  REQUIRE_EQ(served, 6);
  REQUIRE_EQ(interim, 100);
  REQUIRE_EQ(b_status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n50000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

/* Writes every buffer to its socket, round robin and without blocking, until
 * they're all out or timeout_ms passes. A server that reads one socket only
 * after another one completes must not stall the writer. */
static bool _sc_pump(int *fds, const char **bufs, const size_t *lens, int n,
                     int timeout_ms) {
  size_t done[8] = {0};
  for (int i = 0; i < n; i++) fcntl(fds[i], F_SETFL, O_NONBLOCK);
  long long deadline = _sc_now_ms() + timeout_ms;
  for (;;) {
    bool all = true;
    for (int i = 0; i < n; i++) {
      if (done[i] == lens[i]) continue;
      all = false;
      ssize_t w = write(fds[i], bufs[i] + done[i], lens[i] - done[i]);
      if (w > 0) done[i] += (size_t)w;
    }
    if (all) break;
    if (_sc_now_ms() > deadline) break;
    _sc_nap_ms(1);
  }
  bool ok = true;
  for (int i = 0; i < n; i++) {
    fcntl(fds[i], F_SETFL, 0);
    if (done[i] != lens[i]) ok = false;
  }
  return ok;
}

static char *_sc_chunked_request(size_t body, size_t *len_out) {
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /len HTTP/1.1\r\nHost: h\r\n"
                    "Transfer-Encoding: chunked\r\n\r\n%zx\r\n",
                    body);
  size_t total = (size_t)hn + body + 7;
  char *m = malloc(total);
  memcpy(m, head, (size_t)hn);
  memset(m + hn, 'k', body);
  memcpy(m + hn + body, "\r\n0\r\n\r\n", 7);
  *len_out = total;
  return m;
}

TEST(slow_clients, chunked_bodies_that_fill_the_limit_still_progress) {
  /* Two chunked bodies of 200000 bytes against a 64 KiB limit, with no deadline
   * on the wait. The first reserves the whole limit and then needs more, and
   * the second waits for its first reservation. Every memory holder is then
   * waiting, and the progress rule lets the oldest waiter go past the limit.
   * Both complete, and the peak stays within one max_body_size of the limit.
   * Without the progress rule both would wait forever and neither response
   * would arrive. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 64 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 8, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_mem_peak_reset_for_tests();
  int a _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 8);
  int b _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 8);
  REQUIRE_TRUE(a >= 0 && b >= 0);
  size_t la, lb;
  char *ma = _sc_chunked_request(200000, &la);
  char *mb = _sc_chunked_request(200000, &lb);
  int fds[2] = {a, b};
  const char *bufs[2] = {ma, mb};
  size_t lens[2] = {la, lb};
  bool sent = _sc_pump(fds, bufs, lens, 2, 10000);
  free(ma);
  free(mb);
  char buf[4096];
  int sa = _sc_read_response(a, buf, sizeof(buf), NULL, 10000);
  bool a_len = strstr(buf, "\r\n\r\n200000") != NULL;
  int sb = _sc_read_response(b, buf, sizeof(buf), NULL, 10000);
  bool b_len = strstr(buf, "\r\n\r\n200000") != NULL;
  size_t peak = _chttpsvr_mem_peak_for_tests();
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(sa, 200);
  REQUIRE_TRUE(a_len);
  REQUIRE_EQ(sb, 200);
  REQUIRE_TRUE(b_len);
  REQUIRE_LE(peak, (size_t)(64 * 1024 + 1024 * 1024));
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, a_memory_wait_past_its_deadline_gets_503) {
  /* The sweep answers a request that waited for memory longer than
   * body_memory_wait_timeout_us with 503 and a Retry-After header, and closes
   * it. Without that part of the sweep the waiter would never be answered and
   * the read would time out. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  /* the 31 s below must not expire it */
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 9, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 9, holders, 6);
  int b =
      held ? _sc_open_partial_post(_SC_PORT + 9, "/len", 50000, 0, NULL) : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  int status = -1;
  long rest = -1;
  char buf[4096];
  if (b_waits) {
    _chttpsvr_advance_slow_clock_for_tests(31000);
    _chttpsvr_sweep_now_for_tests(srv);
    status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
    rest = _sc_drain_to_eof(b, 3000);
  }
  size_t waiting = _chttpsvr_mem_waiting_for_tests(srv);
  int served = held ? _sc_release_holders(holders, 6) : 0;
  _sc_close_all(holders, 6);
  if (b >= 0) close(b);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(b_waits);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(rest >= 0);
  REQUIRE_EQ(waiting, (size_t)0);
  REQUIRE_EQ(served, 6);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, the_503_of_a_memory_wait_carries_retry_after) {
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  /* the 31 s below must not expire it */
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 10, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 10, holders, 6);
  int b _ccol_destructor(_close_scoped_fd) =
      held ? _sc_open_partial_post(_SC_PORT + 10, "/len", 50000, 0, NULL) : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  char buf[4096];
  int status = -1;
  if (b_waits) {
    _chttpsvr_advance_slow_clock_for_tests(31000);
    _chttpsvr_sweep_now_for_tests(srv);
    status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
  }
  _sc_close_all(holders, 6);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(b_waits);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(strstr(buf, "retry-after:5\r\n") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(slow_clients, memory_waiters_are_admitted_in_order) {
  /* With 98304 of 100000 bytes held, a waiter that needs a first buffer of
   * 16384 comes first and one that needs 1000 second. The second would fit, but
   * it must not overtake the first, or a stream of small bodies could starve a
   * large one. Once the holders complete both are admitted and served.
   * If the reservation ignored the waiters, it would admit the small body at
   * once. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 11, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 11, holders, 6);
  int b _ccol_destructor(_close_scoped_fd) =
      held ? _sc_open_partial_post(_SC_PORT + 11, "/len", 50000, 0,
                                   "Expect: 100-continue\r\n")
           : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  int c _ccol_destructor(_close_scoped_fd) =
      b_waits ? _sc_open_partial_post(_SC_PORT + 11, "/len", 1000, 0,
                                      "Expect: 100-continue\r\n")
              : -1;
  bool both_wait = c >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 2, 5000);
  struct pollfd p = {.fd = c, .events = POLLIN};
  int c_invited_early = c >= 0 ? poll(&p, 1, 0) : -1;

  char buf[4096];
  int b_status = -1, c_status = -1;
  int served = held ? _sc_release_holders(holders, 6) : 0;
  if (b >= 0 && _sc_read_response(b, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_fill(b, 50000, 'b'))
    b_status = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  if (c >= 0 && _sc_read_response(c, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_fill(c, 1000, 'c'))
    c_status = _sc_read_response(c, buf, sizeof(buf), NULL, 5000);
  _sc_close_all(holders, 6);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(b_waits);
  REQUIRE_TRUE(both_wait);
  REQUIRE_EQ(c_invited_early, 0);
  REQUIRE_EQ(served, 6);
  REQUIRE_EQ(b_status, 200);
  REQUIRE_EQ(c_status, 200);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

static int _sc_post_gate(int port) {
  int fd = _sc_connect(port);
  if (fd < 0) return -1;
  if (!_sc_send_str(fd,
                    "POST /gate HTTP/1.1\r\nHost: h\r\n"
                    "Content-Length: 0\r\n\r\n")) {
    close(fd);
    return -1;
  }
  return fd;
}

TEST(slow_clients, a_saturated_streaming_pool_leaves_buffered_routes_served) {
  /* Two streaming handlers hold both streaming threads and a third waits in the
   * streaming queue, while a buffered route is still served promptly by the
   * worker pool. With streaming handlers on the worker pool, the two gated
   * handlers would hold both workers and the GET wouldn't be answered in
   * time. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 2;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 12, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int g[3];
  for (int i = 0; i < 3; i++) g[i] = _sc_post_gate(_SC_PORT + 12);
  bool busy = _sc_wait_for(srv, _sc_stream_active, NULL, 2, 5000) &&
              _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  char buf[4096];
  int hello = -1;
  int fd = _sc_connect(_SC_PORT + 12);
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 2000);
  if (fd >= 0) close(fd);
  _sc_gate_open(true);
  int ok = 0;
  for (int i = 0; i < 3; i++) {
    if (g[i] < 0) continue;
    if (_sc_read_response(g[i], buf, sizeof(buf), NULL, 5000) == 200) ok++;
    close(g[i]);
  }
  _sc_gate_open(false);
  REQUIRE_TRUE(busy);
  REQUIRE_EQ(hello, 200);
  REQUIRE_EQ(ok, 3);
}

TEST(slow_clients, one_streaming_request_starts_one_task) {
  /* Requests that come in one at a time on an idle streaming pool of eight
   * threads each start one task of the pool, never one per thread. If a
   * request started tasks until the run's count reached the thread count, each
   * request would start several, which would exit with nothing to take. */
  _sc_gate_open(true);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 8;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 33, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t before = _chttpsvr_stream_tokens_started_for_tests();
  char buf[4096];
  int ok = 0;
  for (int i = 0; i < 10; i++) {
    int fd = _sc_post_gate(_SC_PORT + 33);
    if (fd < 0) continue;
    if (_sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200) ok++;
    close(fd);
    /* Lets this request's task find the queue empty and exit. */
    _sc_wait_for_zero(srv, _sc_stream_active, NULL, 5000);
  }
  size_t started = _chttpsvr_stream_tokens_started_for_tests() - before;
  _sc_gate_open(false);
  REQUIRE_EQ(ok, 10);
  REQUIRE_LE(started, (size_t)10);
}

TEST(slow_clients, a_request_behind_a_task_on_its_way_starts_one_task) {
  /* The first task of an idle streaming pool is held before it takes the first
   * request, so that request stays queued, and a second request then starts
   * exactly one more task, because the held task will take one of the two.
   * A start that counted only the requests it started tasks for in this call
   * would make the second request start two tasks. */
  _sc_gate_open(true);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 8;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 34, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_arm_stream_claim_hook_for_tests(1);
  size_t before = _chttpsvr_stream_tokens_started_for_tests();
  int r1 = _sc_post_gate(_SC_PORT + 34);
  bool held = _chttpsvr_wait_stream_claim_hook_entered_for_tests(5000);
  int r2 = _sc_post_gate(_SC_PORT + 34);
  char buf[4096];
  int st2 = r2 >= 0 ? _sc_read_response(r2, buf, sizeof(buf), NULL, 5000) : -1;
  size_t started = _chttpsvr_stream_tokens_started_for_tests() - before;
  _chttpsvr_release_stream_claim_hook_for_tests();
  int st1 = r1 >= 0 ? _sc_read_response(r1, buf, sizeof(buf), NULL, 5000) : -1;
  if (r1 >= 0) close(r1);
  if (r2 >= 0) close(r2);
  _chttpsvr_arm_stream_claim_hook_for_tests(0);
  _sc_gate_open(false);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(st1, 200);
  REQUIRE_EQ(st2, 200);
  REQUIRE_EQ(started, (size_t)2);
}

TEST(slow_clients, a_full_streaming_queue_answers_503_at_once) {
  /* One streaming thread and a queue of one. The third streaming request finds
   * the queue full and gets 503 with a Retry-After header, without waiting.
   * An unbounded queue would take it, and it would wait for the gate. */
  _sc_gate_open(false);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_capacity = 1;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 13, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int r1 = _sc_post_gate(_SC_PORT + 13);
  bool running = _sc_wait_for(srv, _sc_stream_active, NULL, 1, 5000);
  int r2 = _sc_post_gate(_SC_PORT + 13);
  bool queued = _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  int r3 = _sc_post_gate(_SC_PORT + 13);
  char buf[4096];
  int s3 = r3 >= 0 ? _sc_read_response(r3, buf, sizeof(buf), NULL, 2000) : -1;
  bool retry_after = strstr(buf, "retry-after:5\r\n") != NULL;
  _sc_gate_open(true);
  int s1 = r1 >= 0 ? _sc_read_response(r1, buf, sizeof(buf), NULL, 5000) : -1;
  int s2 = r2 >= 0 ? _sc_read_response(r2, buf, sizeof(buf), NULL, 5000) : -1;
  if (r1 >= 0) close(r1);
  if (r2 >= 0) close(r2);
  if (r3 >= 0) close(r3);
  _sc_gate_open(false);
  REQUIRE_TRUE(running);
  REQUIRE_TRUE(queued);
  REQUIRE_EQ(s3, 503);
  REQUIRE_TRUE(retry_after);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(s2, 200);
}

TEST(slow_clients, a_queued_streaming_request_past_its_deadline_gets_503) {
  /* While the only streaming thread stays busy, the sweep answers a queued
   * request whose deadline has passed with 503, and its handler never runs.
   * Without that part of the sweep, the request would wait for the gate and
   * its read would time out. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 14, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int r1 = _sc_post_gate(_SC_PORT + 14);
  bool running = _sc_wait_for(srv, _sc_stream_active, NULL, 1, 5000);
  int r2 = _sc_post_gate(_SC_PORT + 14);
  bool queued = _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  _chttpsvr_advance_slow_clock_for_tests(6000);
  _chttpsvr_sweep_now_for_tests(srv);
  char buf[4096];
  int s2 = r2 >= 0 ? _sc_read_response(r2, buf, sizeof(buf), NULL, 2000) : -1;
  long eof2 = r2 >= 0 ? _sc_drain_to_eof(r2, 2000) : -1;
  _sc_gate_open(true);
  int s1 = r1 >= 0 ? _sc_read_response(r1, buf, sizeof(buf), NULL, 5000) : -1;
  if (r1 >= 0) close(r1);
  if (r2 >= 0) close(r2);
  _sc_gate_open(false);
  REQUIRE_TRUE(running);
  REQUIRE_TRUE(queued);
  REQUIRE_EQ(s2, 503);
  REQUIRE_TRUE(eof2 >= 0);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(atomic_load(&g_sc_gate_runs), 1);
}

/* One round of the claim race. The streaming task stops just before (where = 1)
 * or just after (where = 2) it takes the queued request, that request's
 * deadline passes, and the sweep runs. Exactly one of the two answers, with one
 * 503, and the handler never runs for it. */
static void _sc_claim_race_round(int port, int where, int *status_out,
                                 long *eof_out, int *runs_out, bool *setup_ok) {
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(port, &cfg);
  *setup_ok = false;
  *status_out = -1;
  *eof_out = -1;
  if (srv == CHTTPSVR_INVALID) return;
  int r1 = _sc_post_gate(port);
  bool running = _sc_wait_for(srv, _sc_stream_active, NULL, 1, 5000);
  int r2 = _sc_post_gate(port);
  bool queued = _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  _chttpsvr_arm_stream_claim_hook_for_tests(where);
  _sc_gate_open(true);
  bool held = _chttpsvr_wait_stream_claim_hook_entered_for_tests(5000);
  _chttpsvr_advance_slow_clock_for_tests(6000);
  _chttpsvr_sweep_now_for_tests(srv);
  _chttpsvr_release_stream_claim_hook_for_tests();
  char buf[4096];
  if (r2 >= 0) {
    *status_out = _sc_read_response(r2, buf, sizeof(buf), NULL, 3000);
    *eof_out = _sc_drain_to_eof(r2, 3000);
    close(r2);
  }
  if (r1 >= 0) {
    _sc_read_response(r1, buf, sizeof(buf), NULL, 3000);
    close(r1);
  }
  *runs_out = atomic_load(&g_sc_gate_runs);
  _sc_gate_open(false);
  _chttpsvr_arm_stream_claim_hook_for_tests(0);
  chttpsvr_destroy(srv);
  *setup_ok = r1 >= 0 && r2 >= 0 && running && queued && held;
}

TEST(slow_clients, the_sweep_and_a_streaming_task_answer_a_request_once) {
  int st1, runs1, st2, runs2;
  long eof1, eof2;
  bool ok1, ok2;
  _sc_claim_race_round(_SC_PORT + 15, 1, &st1, &eof1, &runs1, &ok1);
  _sc_claim_race_round(_SC_PORT + 16, 2, &st2, &eof2, &runs2, &ok2);
  REQUIRE_TRUE(ok1);
  REQUIRE_EQ(st1, 503);
  REQUIRE_TRUE(eof1 >= 0);
  REQUIRE_EQ(runs1, 1);
  REQUIRE_TRUE(ok2);
  REQUIRE_EQ(st2, 503);
  REQUIRE_TRUE(eof2 >= 0);
  REQUIRE_EQ(runs2, 1);
}

TEST(slow_clients, the_streaming_pool_starts_at_the_first_streaming_request) {
  /* A server whose streaming routes nobody calls starts no thread for them.
   * A pool created at chttpsvr_start would exist before any request. */
  _sc_gate_open(true);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 17, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  bool before = _chttpsvr_stream_pool_created_for_tests(srv);
  char buf[4096];
  int fd = _sc_connect(_SC_PORT + 17);
  int hello = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  bool after_buffered = _chttpsvr_stream_pool_created_for_tests(srv);
  int gate = -1;
  if (fd >= 0 && _sc_send_str(fd,
                              "POST /gate HTTP/1.1\r\nHost: h\r\n"
                              "Content-Length: 0\r\n\r\n"))
    gate = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  bool after_streaming = _chttpsvr_stream_pool_created_for_tests(srv);
  if (fd >= 0) close(fd);
  _sc_gate_open(false);
  REQUIRE_FALSE(before);
  REQUIRE_EQ(hello, 200);
  REQUIRE_FALSE(after_buffered);
  REQUIRE_EQ(gate, 200);
  REQUIRE_TRUE(after_streaming);
}

/* Drips body bytes one at a time with a short pause and returns how many times
 * the reactor handed the parked body to a worker in the meantime. */
static long _sc_drip_dispatches(chttpsvr srv, int port, size_t n,
                                int *status_out) {
  *status_out = -1;
  int fd = _sc_open_partial_post(port, "/len", n, 0, NULL);
  if (fd < 0) return -1;
  if (!_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000)) {
    close(fd);
    return -1;
  }
  size_t before = _chttpsvr_resume_dispatch_count_for_tests();
  for (size_t i = 0; i < n; i++) {
    if (!_sc_send(fd, "d", 1)) break;
    _sc_nap_ms(2);
  }
  char buf[4096];
  *status_out = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  size_t after = _chttpsvr_resume_dispatch_count_for_tests();
  close(fd);
  return (long)(after - before);
}

/* Uploads n bytes in one fast write and counts the dispatches. */
static long _sc_upload_dispatches(chttpsvr srv, int port, size_t n,
                                  int *status_out) {
  (void)srv;
  *status_out = -1;
  size_t before = _chttpsvr_resume_dispatch_count_for_tests();
  int fd = _sc_open_partial_post(port, "/len", n, n, NULL);
  if (fd < 0) return -1;
  char buf[4096];
  *status_out = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  size_t after = _chttpsvr_resume_dispatch_count_for_tests();
  close(fd);
  return (long)(after - before);
}

TEST(slow_clients, the_receive_low_water_mark_wakes_a_drip_once) {
  /* A 200-byte body arriving one byte at a time wakes the reactor once with
   * SO_RCVLOWAT, when the whole rest is queued, and about once per byte without
   * it. A fast 4 MiB upload is counted both ways too, and the counts are
   * printed for the record. The run without the mark is the counterexample
   * that shows the test can fail. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 8 * 1024 * 1024;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 18, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int s_with, s_without, u_with, u_without;
  _chttpsvr_set_rcvlowat_enabled_for_tests(true);
  long drip_with = _sc_drip_dispatches(srv, _SC_PORT + 18, 200, &s_with);
  long up_with =
      _sc_upload_dispatches(srv, _SC_PORT + 18, 4 * 1024 * 1024, &u_with);
  _chttpsvr_set_rcvlowat_enabled_for_tests(false);
  long drip_without = _sc_drip_dispatches(srv, _SC_PORT + 18, 200, &s_without);
  long up_without =
      _sc_upload_dispatches(srv, _SC_PORT + 18, 4 * 1024 * 1024, &u_without);
  _chttpsvr_set_rcvlowat_enabled_for_tests(true);
  fprintf(stderr,
          "rcvlowat dispatches: drip of 200 bytes with=%ld without=%ld; "
          "upload of 4 MiB with=%ld without=%ld\n",
          drip_with, drip_without, up_with, up_without);
  REQUIRE_EQ(s_with, 200);
  REQUIRE_EQ(s_without, 200);
  REQUIRE_EQ(u_with, 200);
  REQUIRE_EQ(u_without, 200);
  REQUIRE_TRUE(drip_with >= 0 && drip_with <= 2);
  REQUIRE_GT(drip_without, 20L);
  REQUIRE_TRUE(up_with >= 0 && up_with <= 4 * 1024 * 1024 / 16384 + 8);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, a_pipelined_request_after_a_parked_body_is_served) {
  /* The rest of a parked body arrives in one write together with the next
   * request, and the worker that resumes the body needs to pass the extra bytes
   * on as the next request instead of dropping them. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 19, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 19, "/len", 100, 10, NULL);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  char tail[256];
  memset(tail, 't', 90);
  int n = snprintf(tail + 90, sizeof(tail) - 90,
                   "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n");
  REQUIRE_TRUE(_sc_send(fd, tail, 90 + (size_t)n));
  char buf[4096];
  int s1 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool len_ok = strstr(buf, "\r\n\r\n100") != NULL;
  int s2 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool hello_ok = strstr(buf, "\r\n\r\nhello") != NULL;
  REQUIRE_EQ(s1, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(s2, 200);
  REQUIRE_TRUE(hello_ok);
}

TEST(slow_clients, pipelined_responses_after_a_parked_write_keep_order) {
  /* Four pipelined requests, the first with a response far bigger than the
   * socket buffers: the write parks, and the others follow it in order once the
   * client reads. HEAD carries its content-length and no body, and a 204
   * carries no body, although both handlers wrote one. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 20, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) =
      _sc_connect_small_rcvbuf(_SC_PORT + 20);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_send_str(fd,
                            "GET /big HTTP/1.1\r\nHost: h\r\n\r\n"
                            "HEAD /big HTTP/1.1\r\nHost: h\r\n\r\n"
                            "GET /no-content HTTP/1.1\r\nHost: h\r\n\r\n"
                            "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"));
  bool parked = _sc_wait_for(srv, _sc_parked_writes, NULL, 1, 10000);
  char buf[4096];
  size_t body = 0;
  int s1 = _sc_read_response(fd, buf, sizeof(buf), &body, 20000);
  size_t big_body = body;
  int s2 = _sc_read_response_x(fd, buf, sizeof(buf), &body, 5000, true);
  char expect_len[64];
  snprintf(expect_len, sizeof(expect_len), "content-length:%zu\r\n",
           _SC_BIG_RESPONSE);
  bool head_len = strstr(buf, expect_len) != NULL;
  int s3 = _sc_read_response(fd, buf, sizeof(buf), &body, 5000);
  size_t no_content_body = body;
  int s4 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool hello_ok = strstr(buf, "\r\n\r\nhello") != NULL;
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(big_body, _SC_BIG_RESPONSE);
  REQUIRE_EQ(s2, 200);
  REQUIRE_TRUE(head_len);
  REQUIRE_EQ(s3, 204);
  REQUIRE_EQ(no_content_body, (size_t)0);
  REQUIRE_EQ(s4, 200);
  REQUIRE_TRUE(hello_ok);
}

TEST(slow_clients, a_parked_body_after_100_continue_completes) {
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 21, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) = _sc_open_partial_post(
      _SC_PORT + 21, "/len", 3000, 0, "Expect: 100-continue\r\n");
  REQUIRE_TRUE(fd >= 0);
  char buf[4096];
  int interim = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool sent = interim == 100 && _sc_send_fill(fd, 1000, 'e');
  bool parked = sent && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  int status = -1;
  if (parked && _sc_send_fill(fd, 2000, 'f'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(interim, 100);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n3000") != NULL);
}

TEST(slow_clients, options_star_with_a_parked_body_keeps_the_connection) {
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 22, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 22);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_send_str(fd,
                            "OPTIONS * HTTP/1.1\r\nHost: h\r\n"
                            "Content-Length: 100\r\n\r\n0123456789"));
  bool parked = _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  char buf[4096];
  int s1 = -1, s2 = -1;
  if (parked && _sc_send_fill(fd, 90, 'o'))
    s1 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (_sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    s2 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(s2, 200);
}

TEST(slow_clients, a_chunked_body_over_the_limit_after_a_park_gets_413) {
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 1000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 23, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 23);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_send_str(fd,
                            "POST /len HTTP/1.1\r\nHost: h\r\n"
                            "Transfer-Encoding: chunked\r\n\r\n200\r\n"));
  REQUIRE_TRUE(_sc_send_fill(fd, 0x200, 'x'));
  REQUIRE_TRUE(_sc_send_str(fd, "\r\n"));
  bool parked = _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  char buf[4096];
  int status = -1;
  if (parked && _sc_send_str(fd, "300\r\n") && _sc_send_fill(fd, 0x300, 'y'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(status, 413);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

typedef struct {
  chttpsvr srv;
  _Atomic bool done;
} _sc_destroy_ctx_t;

static void *_sc_destroy_thread(void *arg) {
  _sc_destroy_ctx_t *ctx = (_sc_destroy_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  atomic_store(&ctx->done, true);
  return NULL;
}

TEST(slow_clients, destroy_closes_or_answers_every_waiting_connection) {
  /* A parked body, a request waiting for memory, a parked response, a streaming
   * handler blocked on its client and a streaming request in the queue.
   * Destroying the server ends every one of them in bounded time, with no
   * leak and no use-after-free (memtest and test_tsan run this same test). The
   * queued streaming request gets a 503. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);
  _sc_gate_open(false);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  /* The parked body holds its first buffer of 16384 bytes, so the second one
   * doesn't fit next to it and waits. */
  cfg.max_partial_body_memory = 20000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 0;
  cfg.max_body_read_duration_us = 0;
  _sc_destroy_ctx_t *ctx = calloc(1, sizeof(*ctx));
  ctx->srv = _sc_server(_SC_PORT + 24, &cfg);
  bool started = ctx->srv != CHTTPSVR_INVALID;
  int a = -1, b = -1, c = -1, d = -1, e = -1;
  bool staged = false;
  if (started) {
    a = _sc_open_partial_post(_SC_PORT + 24, "/len", 90000, 10, NULL);
    staged = a >= 0 && _sc_wait_for(ctx->srv, _sc_parked_bodies, NULL, 1, 5000);
    b = _sc_open_partial_post(_SC_PORT + 24, "/len", 50000, 0, NULL);
    staged = staged && b >= 0 &&
             _sc_wait_for(ctx->srv, _sc_mem_waiting, NULL, 1, 5000);
    c = _sc_connect_small_rcvbuf(_SC_PORT + 24);
    staged = staged && c >= 0 &&
             _sc_send_str(c, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n") &&
             _sc_wait_for(ctx->srv, _sc_parked_writes, NULL, 1, 10000);
    d = _sc_open_partial_post(_SC_PORT + 24, "/stream-err", 1000, 10, NULL);
    staged = staged && d >= 0 &&
             _sc_wait_for(ctx->srv, _sc_stream_active, NULL, 1, 5000);
    e = _sc_post_gate(_SC_PORT + 24);
    staged = staged && e >= 0 &&
             _sc_wait_for(ctx->srv, _sc_stream_queue, NULL, 1, 5000);
  }
  pthread_t th;
  bool joined = false;
  long long t0 = _sc_now_ms();
  if (started && pthread_create(&th, NULL, _sc_destroy_thread, ctx) == 0)
    joined = _bounded_join(th, NULL);
  long long took = _sc_now_ms() - t0;
  char buf[4096];
  int e_status =
      e >= 0 ? _sc_read_response(e, buf, sizeof(buf), NULL, 3000) : -1;
  long ea = a >= 0 ? _sc_drain_to_eof(a, 3000) : -1;
  long eb = b >= 0 ? _sc_drain_to_eof(b, 3000) : -1;
  long ec = c >= 0 ? _sc_drain_to_eof(c, 10000) : -1;
  long ed = d >= 0 ? _sc_drain_to_eof(d, 3000) : -1;
  int fds[5] = {a, b, c, d, e};
  for (int i = 0; i < 5; i++)
    if (fds[i] >= 0) close(fds[i]);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  if (joined) free(ctx);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(staged);
  REQUIRE_TRUE(joined);
  REQUIRE_LT(took, 10000LL);
  REQUIRE_EQ(e_status, 503);
  REQUIRE_TRUE(ea >= 0);
  REQUIRE_TRUE(eb >= 0);
  REQUIRE_TRUE(ec >= 0);
  REQUIRE_TRUE(ed >= 0);
}

/* Sends req on fd and reads one response into buf. Returns its whole length
 * (head and body), or 0 on a failure. */
static size_t _sc_exchange(int fd, const char *req, char *buf, size_t cap,
                           bool head_only) {
  size_t body = 0;
  if (!_sc_send_str(fd, req)) return 0;
  if (_sc_read_response_x(fd, buf, cap, &body, 5000, head_only) < 0) return 0;
  const char *end = strstr(buf, "\r\n\r\n");
  return end ? (size_t)(end + 4 - buf) + body : 0;
}

TEST(slow_clients, a_write_cut_inside_the_head_or_the_body_resumes_intact) {
  /* A response write that stops inside the header block or inside the body
   * parks and resumes on another worker from the exact byte where it stopped.
   * The client must get the same bytes as for a response that went out in one
   * write, and the connection has to stay usable. HEAD is checked the same way.
   * A continuation that restarted the body, or lost a byte of the head, would
   * change the bytes. */
  _chttpsvr_set_date_override_for_tests(1700000000);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 25, &cfg);
  int fd = _sc_connect(_SC_PORT + 25);
  static char ref[4096], got[4096], ref_head[4096], got_head[4096];
  const char *get = "GET /text HTTP/1.1\r\nHost: h\r\n\r\n";
  const char *head = "HEAD /text HTTP/1.1\r\nHost: h\r\n\r\n";
  size_t ref_len = fd >= 0 ? _sc_exchange(fd, get, ref, sizeof(ref), false) : 0;
  size_t ref_head_len =
      fd >= 0 ? _sc_exchange(fd, head, ref_head, sizeof(ref_head), true) : 0;
  size_t head_len = ref_len > 100 ? ref_len - 100 : 0;
  size_t cuts[3] = {10, head_len + 37, 1};
  int intact = 0;
  for (int i = 0; i < 3 && head_len; i++) {
    size_t parks = _chttpsvr_park_count_for_tests();
    _chttpsvr_force_short_response_write_for_tests(cuts[i]);
    size_t n = _sc_exchange(fd, get, got, sizeof(got), false);
    if (n == ref_len && memcmp(got, ref, n) == 0 &&
        _chttpsvr_park_count_for_tests() > parks)
      intact++;
  }
  _chttpsvr_force_short_response_write_for_tests(7);
  size_t hn =
      fd >= 0 ? _sc_exchange(fd, head, got_head, sizeof(got_head), true) : 0;
  bool head_intact =
      hn == ref_head_len && hn > 0 && memcmp(got_head, ref_head, hn) == 0;
  if (fd >= 0) close(fd);
  _chttpsvr_set_date_override_for_tests(INT64_MIN);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_GT(ref_len, (size_t)100);
  REQUIRE_EQ(intact, 3);
  REQUIRE_TRUE(head_intact);
}

TEST(slow_clients,
     the_sweep_walks_only_parked_connections_when_idle_limits_are_off) {
  /* With the idle timeout and the header-phase limit off, a sweep tick has to
   * look at the parked connections and nothing else, however many idle
   * keep-alive connections the server holds. A sweep that walked the idle list
   * anyway would visit the 20 idle connections too. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.read_timeout_us = 0;
  cfg.idle_timeout_us = 0;
  cfg.max_header_read_duration_us = 0;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 26, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int idle[20], parked[3];
  char buf[4096];
  bool ok = true;
  for (int i = 0; i < 20; i++) {
    idle[i] = _sc_connect(_SC_PORT + 26);
    if (idle[i] < 0 ||
        _sc_exchange(idle[i], "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n", buf,
                     sizeof(buf), false) == 0)
      ok = false;
  }
  for (int i = 0; i < 3; i++) {
    parked[i] = _sc_open_partial_post(_SC_PORT + 26, "/len", 1000, 10, NULL);
    if (parked[i] < 0) ok = false;
  }
  ok = ok && _sc_wait_for(srv, _sc_parked_bodies, NULL, 3, 5000);
  size_t visited = ok ? _chttpsvr_sweep_now_for_tests(srv) : 0;
  for (int i = 0; i < 20; i++)
    if (idle[i] >= 0) close(idle[i]);
  for (int i = 0; i < 3; i++)
    if (parked[i] >= 0) close(parked[i]);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(visited, (size_t)3);
}

/* ========================================================================== */
/*                    MOUNT OWNERSHIP                                         */
/* ========================================================================== */

/* A mounted prefix owns every path under it. The server below mounts "/admin"
 * behind a guard, mounts "/admin/deep" on its own, and registers root routes
 * broad enough to catch any path of one to three segments. A request for a path
 * under "/admin", in any spelling that decodes to one, meets the guard,
 * and a root route must never answer it. */

#define _OWN_PORT (_SC_PORT + 27)

static _Atomic int g_own_root_runs = 0;
static _Atomic int g_own_guard_runs = 0;

static void _own_text_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  if (strncmp((const char *)ctx, "root", 4) == 0)
    atomic_fetch_add(&g_own_root_runs, 1);
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

/* Lets a request through only with "x-key: ok" and answers 403 otherwise. */
static void _own_guard_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                          chttpsvr_next_fn next) {
  (void)ctx;
  atomic_fetch_add(&g_own_guard_runs, 1);
  const char *k = chttpsvr_req_header(req, "x-key");
  if (!k || strcmp(k, "ok") != 0) {
    chttpsvr_resp_set_status(resp, 403);
    chttpsvr_resp_write_str(resp, "GUARD");
    return;
  }
  next(req, resp);
}

static chttpsvr _own_server(void) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return CHTTPSVR_INVALID;
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}", _own_text_handler, "root1");
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}/{b}", _own_text_handler,
                            "root2");
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}/{b}/{c}", _own_text_handler,
                            "root3");
  chttpsvr_router *adm = chttpsvr_subrouter(srv, "/admin");
  chttpsvr_router *deep = chttpsvr_subrouter(srv, "/admin/deep");
  if (!adm || !deep) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  chttpsvr_router_use(adm, _own_guard_mw, NULL);
  chttpsvr_router_on(adm, CHTTP_GET, "/secret", _own_text_handler,
                     "admin-secret");
  chttpsvr_router_on(adm, CHTTP_POST, "/post-only", _own_text_handler,
                     "admin-post");
  chttpsvr_router_on(adm, CHTTP_ANY, "/{a}/{b}", _own_text_handler,
                     "admin-two");
  chttpsvr_router_on(deep, CHTTP_GET, "/x", _own_text_handler, "deep-x");
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = _OWN_PORT;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  return srv;
}

/* Sends one request for target on a fresh connection and returns the status, or
 * -1. The response, body included, lands in buf. */
static int _own_request(const char *method, const char *target,
                        const char *extra, char *buf, size_t cap) {
  int fd = _sc_connect(_OWN_PORT);
  if (fd < 0) return -1;
  char req[1024];
  snprintf(req, sizeof(req), "%s %s HTTP/1.1\r\nHost: h\r\n%s\r\n", method,
           target, extra ? extra : "");
  int status = -1;
  if (_sc_send_str(fd, req))
    status = _sc_read_response(fd, buf, cap, NULL, 5000);
  close(fd);
  return status;
}

TEST(route_ownership, every_spelling_under_a_mount_meets_its_guard) {
  /* Each target decodes to a path under "/admin". Without ownership the ones
   * that no route of the "/admin" router matches would fall through to a root
   * route and be served without the guard: "/admin%2Fsecret" (decoded path
   * "/admin/secret") would reach "/{a}", and "/admin/a/b" would reach
   * "/{a}/{b}/{c}". Those targets answer 200 from a root route when the owning
   * router lets them fall through, and the test then fails. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  static const char *const owned[] = {
      "/admin",          "/admin/",           "/admin/secret",
      "/admin/secret/",  "/admin/unknown",    "/admin/a/b",
      "/admin/a/b/c",    "/admin%2Fsecret",   "/admin%2fsecret",
      "/%61dmin/secret", "/admin/%2e%2e/x",   "/admin/./secret",
      "/admin//secret",  "/admin/secret?q=1", "/adm%69n",
      "/admin/%zz"};
  atomic_store(&g_own_root_runs, 0);
  int bad = -1;
  int bad_status = 0;
  for (size_t i = 0; i < sizeof(owned) / sizeof(owned[0]); i++) {
    char buf[2048] = {0};
    int st = _own_request("GET", owned[i], NULL, buf, sizeof(buf));
    if (st != 403 || !strstr(buf, "GUARD")) {
      bad = (int)i;
      bad_status = st;
      break;
    }
  }
  if (bad >= 0) fprintf(stderr, "target %s -> %d\n", owned[bad], bad_status);
  REQUIRE_EQ(bad, -1);
  REQUIRE_EQ(atomic_load(&g_own_root_runs), 0);
}

TEST(route_ownership, the_owner_answers_404_and_405_itself) {
  /* With the guard satisfied, a path under "/admin" that no route of the owner
   * matches gets a 404 from the owner, and a method its routes don't accept
   * gets a 405 whose Allow names only the owner's methods. A root CHTTP_ANY
   * route never serves either of them. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  atomic_store(&g_own_root_runs, 0);
  char buf[2048] = {0};
  int st_ok =
      _own_request("GET", "/admin/secret", "x-key: ok\r\n", buf, sizeof(buf));
  bool ok_body = strstr(buf, "admin-secret") != NULL;
  int guard_before = atomic_load(&g_own_guard_runs);
  memset(buf, 0, sizeof(buf));
  int st_404 =
      _own_request("GET", "/admin/a/b/c", "x-key: ok\r\n", buf, sizeof(buf));
  int guard_after_404 = atomic_load(&g_own_guard_runs);
  memset(buf, 0, sizeof(buf));
  int st_slash =
      _own_request("GET", "/admin%2Fsecret", "x-key: ok\r\n", buf, sizeof(buf));
  memset(buf, 0, sizeof(buf));
  int st_405 = _own_request("GET", "/admin/post-only", "x-key: ok\r\n", buf,
                            sizeof(buf));
  bool allow_post = strstr(buf, "allow:POST\r\n") != NULL ||
                    strstr(buf, "allow: POST\r\n") != NULL;
  REQUIRE_EQ(st_ok, 200);
  REQUIRE_TRUE(ok_body);
  REQUIRE_EQ(st_404, 404);
  REQUIRE_EQ(guard_after_404, guard_before + 1);
  REQUIRE_EQ(st_slash, 404);
  REQUIRE_EQ(st_405, 405);
  REQUIRE_TRUE(allow_post);
  REQUIRE_EQ(atomic_load(&g_own_root_runs), 0);
}

TEST(route_ownership, a_nested_mount_owns_its_own_prefix) {
  /* "/admin/deep" is the more specific mount, so it owns every path under it:
   * its own routes answer, its own 404 answers the rest, and neither the
   * "/admin" guard nor the "/{a}/{b}" route of "/admin" ever sees such a
   * request. A path next to it stays with "/admin". With a fall-through to
   * "/admin", "/admin/deep/zz" would reach "/{a}/{b}" there and be guarded. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int g0 = atomic_load(&g_own_guard_runs);
  int st_x = _own_request("GET", "/admin/deep/x", NULL, buf, sizeof(buf));
  bool deep_body = strstr(buf, "deep-x") != NULL;
  memset(buf, 0, sizeof(buf));
  int st_zz = _own_request("GET", "/admin/deep/zz", NULL, buf, sizeof(buf));
  int g1 = atomic_load(&g_own_guard_runs);
  memset(buf, 0, sizeof(buf));
  int st_beside =
      _own_request("GET", "/admin/deeper/zz", NULL, buf, sizeof(buf));
  int g2 = atomic_load(&g_own_guard_runs);
  REQUIRE_EQ(st_x, 200);
  REQUIRE_TRUE(deep_body);
  REQUIRE_EQ(st_zz, 404);
  REQUIRE_EQ(g1, g0);
  REQUIRE_EQ(st_beside, 403);
  REQUIRE_EQ(g2, g1 + 1);
}

TEST(route_ownership, a_path_outside_every_mount_reaches_the_root) {
  /* Only a path under a mount is owned. "/administrator" and "/ADMIN" share no
   * segment boundary with "/admin", and ";" isn't a separator. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  static const char *const free_paths[] = {"/administrator", "/ADMIN/secret",
                                           "/admin;x/secret", "/x/admin",
                                           "/admin%252fsecret"};
  int bad = -1;
  for (size_t i = 0; i < sizeof(free_paths) / sizeof(free_paths[0]); i++) {
    char buf[2048] = {0};
    int st = _own_request("GET", free_paths[i], NULL, buf, sizeof(buf));
    if (st != 200 || !strstr(buf, "root")) {
      bad = (int)i;
      break;
    }
  }
  REQUIRE_EQ(bad, -1);
}

/* ========================================================================== */
/*                    STEADY READER OF A PARKED RESPONSE                      */
/* ========================================================================== */

/* Reads one response of known body size from fd at about rate bytes a second,
 * and returns how many body bytes arrived before the end of the stream or
 * timeout_ms, or -1 when the head didn't arrive. */
static long _sc_paced_read_body(int fd, long rate, int timeout_ms,
                                size_t want_body) {
  char head[4096];
  int status = _sc_read_response_x(fd, head, sizeof(head), NULL, 5000, true);
  if (status != 200) return -1;
  long long start = _sc_now_ms();
  long long deadline = start + timeout_ms;
  size_t got = 0;
  char tmp[16384];
  while (got < want_body) {
    long long now = _sc_now_ms();
    if (now > deadline) break;
    long long due_ms = (long long)((double)got * 1000.0 / (double)rate);
    if (due_ms > now - start) {
      _sc_nap_ms((long)(due_ms - (now - start)));
      continue;
    }
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, (int)(deadline - now)) <= 0) break;
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r <= 0) break;
    got += (size_t)r;
  }
  return (long)got;
}

TEST(slow_clients, a_steady_reader_of_a_parked_response_is_never_cut) {
  /* A reader that takes a large response steadily, far above the rate floor,
   * drains the send queue all the time, but the socket only becomes writable
   * again once about a third of a send buffer the kernel autotunes to several
   * MB has drained. With a gap limit shorter than that drain, the wake-up alone
   * would judge such a reader stalled and cut the response. A shrinking send
   * queue is the progress that counts, and judging a parked response by its
   * wake-ups alone cuts this one after about 4 MB. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.response_write_timeout_us = 200000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 28, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd = _sc_connect(_SC_PORT + 28);
  REQUIRE_TRUE(fd >= 0);
  bool sent = _sc_send_str(fd, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
  long got =
      sent ? _sc_paced_read_body(fd, 4L * 1024 * 1024, 30000, _SC_BIG_RESPONSE)
           : -1;
  close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(got, (long)_SC_BIG_RESPONSE);
}

TEST(slow_clients, the_sweep_queries_the_kernel_outside_its_lock) {
  /* While the rate floor is on, a parked response is judged by its send queue
   * on every sweep tick, which is one ioctl per connection. The sweep makes
   * those calls after releasing the lock that every dispatch and every park
   * takes. If the parked list were judged under that lock, every sweep query
   * would count as locked. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.response_write_timeout_us = 30000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 35, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t total0 = 0, locked0 = 0;
  _chttpsvr_parked_queries_for_tests(&total0, &locked0);
  int fd = _sc_connect_small_rcvbuf(_SC_PORT + 35);
  REQUIRE_TRUE(fd >= 0);
  bool sent = _sc_send_str(fd, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
  /* The response parks immediately on a reader that takes nothing, and three
   * sweep ticks judge it. */
  _sc_nap_ms(3500);
  size_t total1 = 0, locked1 = 0;
  _chttpsvr_parked_queries_for_tests(&total1, &locked1);
  close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_GE(total1 - total0, (size_t)2);
  REQUIRE_EQ(locked1 - locked0, (size_t)0);
}

TEST(slow_clients, keep_alive_requests_make_no_direction_change) {
  /* A keep-alive connection waits for its next request in the read direction,
     which is the direction its registration already has, so the server makes no
     ccol_event_loop_modify call for it between requests. A tail that set the
     direction on every request would make at least one such call for each
     request after the first. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 36, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd = _sc_connect(_SC_PORT + 36);
  REQUIRE_TRUE(fd >= 0);
  /* The first request creates the connection's registration. */
  char buf[2048];
  bool first = _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") &&
               _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200;
  size_t before = _chttpsvr_conn_reg_modify_calls_for_tests();
  int ok = 0;
  for (int i = 0; first && i < 20; i++) {
    _sc_nap_ms(2);
    if (_sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") &&
        _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200)
      ok++;
  }
  size_t calls = _chttpsvr_conn_reg_modify_calls_for_tests() - before;
  close(fd);
  REQUIRE_TRUE(first);
  REQUIRE_EQ(ok, 20);
  REQUIRE_EQ(calls, (size_t)0);
}

/* ========================================================================== */
/*                    REQUEST CONFORMANCE                                     */
/* ========================================================================== */

/* Sends a whole request to g_srv on a fresh connection and returns the status
 * of the first response, or -1. The response bytes land in buf. */
static int _conf_request(const char *req, char *buf, size_t cap) {
  _raw_request_to_test_server(req, buf, cap);
  if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
  return atoi(buf + 9);
}

TEST(conformance, a_major_version_other_than_1_gets_505) {
  /* RFC 9110 SS15.6.6. Without the version check, HTTP/2.0 and HTTP/3.0 would
   * be served as HTTP/1.1 and answer 200, and HTTP/0.9 as HTTP/1.0. */
  static const char *const reqs[] = {
      "GET /hello HTTP/2.0\r\nHost: h\r\n\r\n",
      "GET /hello HTTP/3.0\r\nHost: h\r\n\r\n",
      "GET /hello HTTP/0.9\r\nHost: h\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    char buf[4096];
    REQUIRE_EQ(_conf_request(reqs[i], buf, sizeof(buf)),
               CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED);
    /* The reason phrase that goes with the public constant. */
    REQUIRE_TRUE(
        strncmp(buf, "HTTP/1.1 505 HTTP Version Not Supported\r\n", 41) == 0);
  }
}

TEST(conformance, the_505_status_constant_is_505) {
  /* The public constant for the status the server sends for a major version
   * other than 1. */
  REQUIRE_EQ(CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED, 505);
}

TEST(conformance, a_higher_minor_version_of_http_1_is_served_as_1_1) {
  /* RFC 9110 SS2.5: a recipient that implements the major version serves a
   * higher minor version as the highest minor version it implements. */
  char buf[4096];
  REQUIRE_EQ(_conf_request("GET /hello HTTP/1.9\r\nHost: h\r\nConnection: "
                           "close\r\n\r\n",
                           buf, sizeof(buf)),
             200);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(conformance, a_host_outside_the_uri_host_grammar_gets_400) {
  /* RFC 9112 SS3.2 and RFC 3986 SS3.2.2. A Host check that refused only an
   * empty value and whitespace would serve every one of these. */
  static const char *const bad[] = {
      "a/b", "u@h", "h?x", "h#x",   "h:8x", "[::1", "[zz]",     "h:80:81",
      ":80", "h\"", "%4",  "[v1.]", "h\\x", "h{}",  "[1.2.3.4]"};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    char req[256];
    snprintf(req, sizeof(req),
             "GET /hello HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             bad[i]);
    char buf[4096];
    int st = _conf_request(req, buf, sizeof(buf));
    if (st != 400) fprintf(stderr, "Host %s -> %d\n", bad[i], st);
    REQUIRE_EQ(st, 400);
  }
  static const char *const good[] = {
      "h",        "h:80",   "h:",       "127.0.0.1:8080", "[::1]",
      "[::1]:80", "%41b.c", "a-b.c_d~", "[v1.x:y]",       "!$&'()*+,;="};
  for (size_t i = 0; i < sizeof(good) / sizeof(good[0]); i++) {
    char req[256];
    snprintf(req, sizeof(req),
             "GET /hello HTTP/1.1\r\nHost: %s\r\nConnection: close\r\n\r\n",
             good[i]);
    char buf[4096];
    int st = _conf_request(req, buf, sizeof(buf));
    if (st != 200) fprintf(stderr, "Host %s -> %d\n", good[i], st);
    REQUIRE_EQ(st, 200);
  }
}

TEST(conformance, expect_100_continue_is_ignored_for_http_1_0) {
  /* RFC 9110 SS10.1.1: a server MUST ignore 100-continue in an HTTP/1.0
   * request. Honouring it would write an interim "100 Continue" in front of the
   * response. */
  char buf[4096];
  int st = _conf_request(
      "POST /echo-body HTTP/1.0\r\nExpect: 100-continue\r\n"
      "Content-Length: 4\r\n\r\nping",
      buf, sizeof(buf));
  REQUIRE_EQ(st, 200);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "ping") != NULL);
}

TEST(conformance, a_transfer_coding_other_than_chunked_gets_501) {
  /* RFC 9112 SS6.1: the server decodes no coding other than chunked, so a
   * request that applies another one would hand the handler an encoded body.
   * Without the check, "gzip, chunked" would reach the handler and answer 200.
   * A list that doesn't end in chunked stays a 400, because its length can't
   * be determined (RFC 9112 SS6.3). */
  char buf[4096];
  REQUIRE_EQ(_conf_request("POST /echo-body HTTP/1.1\r\nHost: h\r\n"
                           "Transfer-Encoding: gzip, chunked\r\n\r\n"
                           "4\r\nping\r\n0\r\n\r\n",
                           buf, sizeof(buf)),
             501);
  REQUIRE_EQ(_conf_request("POST /echo-body HTTP/1.1\r\nHost: h\r\n"
                           "Transfer-Encoding: gzip\r\n"
                           "Transfer-Encoding: chunked\r\n\r\n"
                           "4\r\nping\r\n0\r\n\r\n",
                           buf, sizeof(buf)),
             501);
  REQUIRE_EQ(_conf_request("POST /echo-body HTTP/1.1\r\nHost: h\r\n"
                           "Transfer-Encoding: gzip\r\n\r\nping",
                           buf, sizeof(buf)),
             400);
  REQUIRE_EQ(_conf_request("POST /echo-body HTTP/1.1\r\nHost: h\r\n"
                           "Transfer-Encoding: chunked\r\nConnection: close"
                           "\r\n\r\n4\r\nping\r\n0\r\n\r\n",
                           buf, sizeof(buf)),
             200);
}

/* Builds a chunked POST to /echo-body of n one-byte chunks, each with the chunk
 * extension ext. The caller frees the result. */
static char *_conf_chunked_with_ext(size_t n, const char *ext) {
  size_t cap = 256 + n * (strlen(ext) + 16);
  char *req = malloc(cap);
  if (!req) return NULL;
  size_t len = (size_t)snprintf(req, cap,
                                "POST /echo-body HTTP/1.1\r\nHost: h\r\n"
                                "Transfer-Encoding: chunked\r\n"
                                "Connection: close\r\n\r\n");
  for (size_t i = 0; i < n; i++)
    len += (size_t)snprintf(req + len, cap - len, "1%s\r\nx\r\n", ext);
  snprintf(req + len, cap - len, "0\r\n\r\n");
  return req;
}

TEST(conformance, chunk_extensions_are_bounded_by_the_data_they_carry) {
  /* The server throws chunk extensions away, so no body limit counts them, and
   * without the bound 200 one-byte chunks with a 200-byte extension each (40 KB
   * of framing for 200 bytes of data) would answer 200. A short extension on
   * every chunk is still accepted. */
  char ext[256];
  ext[0] = ';';
  ext[1] = 'e';
  ext[2] = '=';
  memset(ext + 3, 'v', 197);
  ext[200] = '\0';
  char *heavy = _conf_chunked_with_ext(200, ext);
  char *light = _conf_chunked_with_ext(2000, ";a=b");
  char buf[16384];
  int st_heavy = heavy ? _conf_request(heavy, buf, sizeof(buf)) : -2;
  int st_light = light ? _conf_request(light, buf, sizeof(buf)) : -2;
  free(heavy);
  free(light);
  REQUIRE_EQ(st_heavy, 400);
  REQUIRE_EQ(st_light, 200);
}

/* ========================================================================== */
/*                    RESPONSE LINE SPILL OF chttp1_parser                    */
/* ========================================================================== */

#include <internal/chttp1_parser.h>

typedef struct {
  size_t longest_value;
  size_t headers;
} _spill_ctx_t;

static int _spill_on_header(chttp1_parser_t *p, const char *name,
                            size_t name_len, const char *value,
                            size_t value_len) {
  (void)name;
  (void)name_len;
  (void)value;
  _spill_ctx_t *c = (_spill_ctx_t *)p->data;
  c->headers++;
  if (value_len > c->longest_value) c->longest_value = value_len;
  return 0;
}

static _Atomic int g_spill_allocs = 0;
static _Atomic int g_spill_frees = 0;
static void *_spill_malloc(size_t n) {
  atomic_fetch_add(&g_spill_allocs, 1);
  return malloc(n);
}
static void _spill_free(void *p) {
  if (p) atomic_fetch_add(&g_spill_frees, 1);
  free(p);
}

/* A response whose single header line carries value_len bytes of value. */
static char *_spill_response(size_t value_len) {
  size_t cap = value_len + 128;
  char *m = malloc(cap);
  if (!m) return NULL;
  size_t n = (size_t)snprintf(m, cap, "HTTP/1.1 200 OK\r\nX-Big: ");
  memset(m + n, 'v', value_len);
  n += value_len;
  n += (size_t)snprintf(m + n, cap - n, "\r\nContent-Length: 0\r\n\r\n");
  return m;
}

/* Feeds msg to a fresh response parser in pieces of step bytes. */
static chttp1_errno_t _spill_feed(chttp1_parser_t *p, const char *msg,
                                  size_t step) {
  size_t len = strlen(msg);
  chttp1_errno_t r = CHTTP1_OK;
  for (size_t off = 0; off < len && r == CHTTP1_OK; off += step) {
    size_t n = len - off < step ? len - off : step;
    r = chttp1_parser_execute(p, msg + off, n);
  }
  return r;
}

TEST(chttp1_parser, a_response_header_line_spills_to_the_heap_up_to_64_kib) {
  /* A header line longer than the 8 KiB line buffer is refused unless the
   * response parser has enabled the spill, in which case it moves to one heap
   * buffer taken from the caller's procs, and chttp1_parser_release() gives it
   * back. The line is fed in small pieces so that it crosses the spill point in
   * the middle of a call. */
  chttp1_settings_t st;
  chttp1_settings_init(&st);
  st.on_header = _spill_on_header;
  char *msg = _spill_response(40000);
  REQUIRE_TRUE(msg != NULL);

  chttp1_parser_t plain;
  chttp1_parser_init(&plain, &st);
  _spill_ctx_t c0 = {0, 0};
  plain.data = &c0;
  chttp1_errno_t r_plain = _spill_feed(&plain, msg, 1000);
  chttp1_parser_release(&plain);

  ccol_memmgmt_procs_t procs = {0};
  procs.malloc = _spill_malloc;
  procs.free = _spill_free;
  int a0 = atomic_load(&g_spill_allocs);
  int f0 = atomic_load(&g_spill_frees);
  chttp1_parser_t sp;
  chttp1_parser_init(&sp, &st);
  bool enabled = chttp1_parser_enable_line_spill(&sp, &procs);
  _spill_ctx_t c1 = {0, 0};
  sp.data = &c1;
  chttp1_errno_t r_spill = _spill_feed(&sp, msg, 1000);
  int allocs = atomic_load(&g_spill_allocs) - a0;
  chttp1_parser_release(&sp);
  chttp1_parser_release(&sp);
  int frees = atomic_load(&g_spill_frees) - f0;
  free(msg);

  REQUIRE_EQ((int)r_plain, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(enabled);
  REQUIRE_EQ((int)r_spill, (int)CHTTP1_PAUSED);
  REQUIRE_EQ(c1.longest_value, (size_t)40000);
  REQUIRE_EQ(c1.headers, (size_t)2);
  REQUIRE_EQ(allocs, 1);
  REQUIRE_EQ(frees, 1);
}

TEST(chttp1_parser, a_spilled_line_stays_bounded) {
  /* The spill buffer holds 64 KiB, and the total header cap bounds the line
   * too. A line past either is refused and nothing leaks, and a request parser
   * never spills. */
  chttp1_settings_t st;
  chttp1_settings_init(&st);
  char *msg = _spill_response(70000);
  REQUIRE_TRUE(msg != NULL);
  chttp1_parser_t sp;
  chttp1_parser_init(&sp, &st);
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno_t r_long = _spill_feed(&sp, msg, 4096);
  chttp1_parser_release(&sp);
  free(msg);

  /* A budget that can hold a longer line spills and refuses the line once it
   * goes past the budget. A budget that can't never spills. */
  msg = _spill_response(20000);
  REQUIRE_TRUE(msg != NULL);
  chttp1_parser_init(&sp, &st);
  sp.max_total_header_bytes_override = 12000;
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno_t r_budget = _spill_feed(&sp, msg, 4096);
  size_t budget_line = sp.line_len;
  chttp1_parser_release(&sp);
  chttp1_parser_init(&sp, &st);
  sp.max_total_header_bytes_override = 8000;
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno_t r_small = _spill_feed(&sp, msg, 4096);
  bool spilled = sp.line_spill != NULL;
  chttp1_parser_release(&sp);
  free(msg);

  chttp1_parser_t rq;
  chttp1_parser_init_request(&rq, &st);
  bool req_enabled = chttp1_parser_enable_line_spill(&rq, NULL);

  REQUIRE_EQ((int)r_long, (int)CHTTP1_ERROR);
  REQUIRE_EQ((int)r_budget, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(budget_line <= (size_t)12000);
  REQUIRE_EQ((int)r_small, (int)CHTTP1_ERROR);
  REQUIRE_FALSE(spilled);
  REQUIRE_FALSE(req_enabled);
}

TEST(chttp1_parser, chunk_framing_is_bounded_in_a_response_too) {
  /* The bound on chunk framing is a property of the parser, so a client reading
   * a response gets it as well. */
  chttp1_settings_t st;
  chttp1_settings_init(&st);
  const char *head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  char ext[128];
  memset(ext, 'v', sizeof(ext) - 1);
  ext[0] = ';';
  ext[1] = 'e';
  ext[2] = '=';
  ext[sizeof(ext) - 1] = '\0';
  chttp1_parser_t p;
  chttp1_parser_init(&p, &st);
  chttp1_errno_t r = chttp1_parser_execute(&p, head, strlen(head));
  for (int i = 0; i < 400 && r == CHTTP1_OK; i++) {
    char chunk[256];
    int n = snprintf(chunk, sizeof(chunk), "1%s\r\nx\r\n", ext);
    r = chttp1_parser_execute(&p, chunk, (size_t)n);
  }
  REQUIRE_EQ((int)r, (int)CHTTP1_ERROR);
  REQUIRE_STREQ(p.reason, "Chunk framing exceeds its data");
}

/* ========================================================================== */
/*          LISTENERS, LINGERING CLOSE, ACCEPT BATCHES, BODY MEMORY           */
/* ========================================================================== */

#define _LC_PORT (TEST_PORT + 200)

/* The lingering-close counter moves on the closing thread, which can run just
   after the client has already seen the end of the stream, so a test waits for
   it, for at most 5 s. */
static bool _linger_count_passes(size_t before) {
  for (int i = 0; i < 5000; i++) {
    if (_chttpsvr_linger_count_for_tests() > before) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

/* Starts a server with one route, GET and POST /lc, on host and port. */
static chttpsvr _lc_server(const char *host, int port,
                           const chttpsvr_config_t *base) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return CHTTPSVR_INVALID;
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/lc", _sc_len_handler, NULL);
  chttpsvr_config_t cfg = base ? *base : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  return srv;
}

/* Sends GET /lc over a fresh connection to sa and returns the status, or -1
   when the connection or the exchange fails. */
static int _lc_get_via(const struct sockaddr *sa, socklen_t salen) {
  int fd = socket(sa->sa_family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  int status = -1;
  if (test_connect(fd, sa, salen) == 0 &&
      _sc_send_str(fd, "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n")) {
    char buf[1024];
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  }
  close(fd);
  return status;
}

static int _lc_get_v4(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

static int _lc_get_v6(int port) {
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  a.sin6_port = htons((uint16_t)port);
  inet_pton(AF_INET6, "::1", &a.sin6_addr);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

/* Whether this host can bind the IPv6 loopback at all. */
static bool _lc_have_ipv6_loopback(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  inet_pton(AF_INET6, "::1", &a.sin6_addr);
  bool ok = bind(fd, (struct sockaddr *)&a, sizeof(a)) == 0;
  close(fd);
  return ok;
}

/* Whether an IPv6 socket can take IPv4 clients here. OpenBSD has no such
 * socket, and there the default listener is 0.0.0.0, as in Go. */
static bool _lc_have_dual_stack(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int off = 0;
  bool ok = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0;
  close(fd);
  return ok;
}

TEST(listener_host, a_null_or_empty_host_listens_on_ipv4_and_ipv6) {
  /* A NULL or empty host is a single dual-stack listener: IPv4 clients reach it
     through v4-mapped addresses and IPv6 clients directly. On a system without
     dual-stack sockets it listens on IPv4 alone. A listener on
     0.0.0.0 would refuse the IPv6 connection where the system has dual-stack
     sockets. */
  bool v6 = _lc_have_ipv6_loopback() && _lc_have_dual_stack();
  if (!v6) fprintf(stderr, "SKIP: no dual-stack IPv6; ::1 not checked\n");
  const char *hosts[2] = {NULL, ""};
  int v4_status[2] = {-1, -1}, v6_status[2] = {-1, -1};
  for (int i = 0; i < 2; i++) {
    chttpsvr srv = _lc_server(hosts[i], _LC_PORT + i, NULL);
    if (srv == CHTTPSVR_INVALID) continue;
    v4_status[i] = _lc_get_v4(_LC_PORT + i);
    v6_status[i] = v6 ? _lc_get_v6(_LC_PORT + i) : 200;
    chttpsvr_destroy(srv);
  }
  for (int i = 0; i < 2; i++) {
    REQUIRE_EQ(v4_status[i], 200);
    REQUIRE_EQ(v6_status[i], 200);
  }
}

TEST(listener_host, the_default_configuration_listens_on_ipv4_and_ipv6) {
  /* CHTTPSVR_CONFIG_DEFAULT leaves host NULL, so a server started from it
     answers on 127.0.0.1 and, where the host has IPv6 and dual-stack sockets,
     on ::1. The IPv6 half is skipped on any other host. A default of
     "0.0.0.0" would refuse the IPv6 connection. */
  bool v6 = _lc_have_ipv6_loopback() && _lc_have_dual_stack();
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  bool host_is_null = cfg.host == NULL;
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started = false;
  if (srv != CHTTPSVR_INVALID) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    cfg.port = (uint16_t)(_LC_PORT + 14);
    started = chttpsvr_start(srv, &cfg) == ccol_success;
  }
  int v4 = started ? _lc_get_v4(_LC_PORT + 14) : -2;
  int v6_status = !v6 ? 200 : (started ? _lc_get_v6(_LC_PORT + 14) : -2);
  if (!v6) fprintf(stderr, "SKIP: no dual-stack IPv6; ::1 not checked\n");
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(host_is_null);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6_status, 200);
}

TEST(listener_host, localhost_listens_on_the_ipv4_loopback) {
  /* A host name listens on its first IPv4 address when it has one, so
     "localhost" answers on 127.0.0.1 whatever order the resolver gives.
     Non-vacuous where the resolver puts ::1 first for "localhost", as on most
     Linux systems: the resolver's first address would be ::1, and 127.0.0.1
     would then be refused. */
  chttpsvr srv = _lc_server("localhost", _LC_PORT + 2, NULL);
  int status = srv != CHTTPSVR_INVALID ? _lc_get_v4(_LC_PORT + 2) : -2;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_EQ(status, 200);
}

TEST(listener_host, a_literal_address_binds_exactly_that_address) {
  bool v6 = _lc_have_ipv6_loopback();
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 3, NULL);
  int v4 = srv != CHTTPSVR_INVALID ? _lc_get_v4(_LC_PORT + 3) : -2;
  int v6_status =
      (srv != CHTTPSVR_INVALID && v6) ? _lc_get_v6(_LC_PORT + 3) : -1;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  chttpsvr srv6 = v6 ? _lc_server("::1", _LC_PORT + 4, NULL) : CHTTPSVR_INVALID;
  int v6_on_v6 = srv6 != CHTTPSVR_INVALID ? _lc_get_v6(_LC_PORT + 4) : 200;
  int v4_on_v6 = srv6 != CHTTPSVR_INVALID ? _lc_get_v4(_LC_PORT + 4) : -1;
  if (srv6 != CHTTPSVR_INVALID) chttpsvr_destroy(srv6);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6_status, -1);
  REQUIRE_EQ(v6_on_v6, 200);
  REQUIRE_EQ(v4_on_v6, -1);
}

/* ----- unix:// listeners ----- */

static void _lc_unix_host(char *host, size_t cap, const char *path) {
  snprintf(host, cap, "unix://%s", path);
}

/* Sends GET /lc over the Unix socket at path and returns the status. */
static int _lc_get_unix(const char *path) {
  struct sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

TEST(listener_unix, a_regular_file_at_the_path_survives_a_failed_start) {
  /* A start never deletes a file that isn't a socket, where an unconditional
     unlink before the bind would remove the file and start. */
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_file_%d", (int)getpid());
  unlink(path);
  FILE *f = fopen(path, "w");
  if (f) {
    fputs("precious data\n", f);
    fclose(f);
  }
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  ccol_retval_t rv = srv != CHTTPSVR_INVALID ? chttpsvr_start(srv, &cfg)
                                             : ccol_unexpected_failure;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  struct stat st;
  bool regular = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
  char content[32] = {0};
  f = fopen(path, "r");
  if (f) {
    if (!fgets(content, sizeof(content), f)) content[0] = '\0';
    fclose(f);
  }
  unlink(path);
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_TRUE(regular);
  REQUIRE_STREQ(content, "precious data\n");
}

TEST(listener_unix, a_live_listener_keeps_its_socket) {
  /* A second server on the path of a live one fails to start, and the first one
     keeps both its socket file and its clients. An unconditional unlink before
     the bind would take the path over, so the first server would lose every new
     client. */
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_live_%d", (int)getpid());
  unlink(path);
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr first = _lc_server(host, 0, NULL);
  bool first_started = first != CHTTPSVR_INVALID;
  chttpsvr second = chttpsvr_create(g_test_logger, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  ccol_retval_t rv = (first != CHTTPSVR_INVALID && second != CHTTPSVR_INVALID)
                         ? chttpsvr_start(second, &cfg)
                         : ccol_unexpected_failure;
  if (second != CHTTPSVR_INVALID) chttpsvr_destroy(second);
  int status = first != CHTTPSVR_INVALID ? _lc_get_unix(path) : -2;
  if (first != CHTTPSVR_INVALID) chttpsvr_destroy(first);
  struct stat st;
  bool gone = lstat(path, &st) != 0;
  unlink(path);
  REQUIRE_TRUE(first_started);
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
  /* The first server removes its own socket when it goes. */
  REQUIRE_TRUE(gone);
}

TEST(listener_unix, a_stop_leaves_a_path_that_names_another_file_alone) {
  /* Somebody else's file replaces the socket file of a running server. The
     server's stop should remove only its own socket, so the replacement
     survives. Unlinking the path by name would delete the replacement. */
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_repl_%d", (int)getpid());
  unlink(path);
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr srv = _lc_server(host, 0, NULL);
  bool started = srv != CHTTPSVR_INVALID;
  bool replaced = false;
  if (srv != CHTTPSVR_INVALID && unlink(path) == 0) {
    FILE *f = fopen(path, "w");
    if (f) {
      fputs("not yours\n", f);
      fclose(f);
      replaced = true;
    }
  }
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  struct stat st;
  bool survived = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
  unlink(path);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(replaced);
  REQUIRE_TRUE(survived);
}

TEST(listener_unix, a_relative_path_is_cleaned_up_after_a_chdir) {
  /* A server started on a relative path removes that socket file when it stops,
     even after the application has changed its working directory, and touches
     nothing in the new one. A cleanup that resolved the relative path again
     would look in the new directory, find nothing of its own there and leave
     its socket behind. */
  char dir_a[64], dir_b[64], sock_a[128], sock_b[128], cwd[512];
  snprintf(dir_a, sizeof(dir_a), "/tmp/chttpsvr_lc_a_%d", (int)getpid());
  snprintf(dir_b, sizeof(dir_b), "/tmp/chttpsvr_lc_b_%d", (int)getpid());
  snprintf(sock_a, sizeof(sock_a), "%s/rel.sock", dir_a);
  snprintf(sock_b, sizeof(sock_b), "%s/rel.sock", dir_b);
  bool have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
  mkdir(dir_a, 0700);
  mkdir(dir_b, 0700);
  bool in_a = have_cwd && chdir(dir_a) == 0;
  chttpsvr srv =
      in_a ? _lc_server("unix://rel.sock", 0, NULL) : CHTTPSVR_INVALID;
  bool started = srv != CHTTPSVR_INVALID;
  bool in_b = chdir(dir_b) == 0;
  /* A file with the same name in the new directory, which isn't ours. */
  bool made_b = in_b && _make_stale_unix_socket("rel.sock");
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  struct stat st;
  bool a_gone = lstat(sock_a, &st) != 0;
  bool b_kept = lstat(sock_b, &st) == 0;
  bool back = have_cwd && chdir(cwd) == 0;
  unlink(sock_a);
  unlink(sock_b);
  rmdir(dir_a);
  rmdir(dir_b);
  REQUIRE_TRUE(back);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(made_b);
  REQUIRE_TRUE(a_gone);
  REQUIRE_TRUE(b_kept);
}

/* ----- lingering close ----- */

static size_t _lc_lingering(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_parked_count_for_tests(h, 6);
}

/* Sends the head of a POST to path with a body of declared bytes, plus
   first_bytes of that body, in one write. */
static bool _lc_send_upload_start(int fd, const char *path, size_t declared,
                                  size_t first_bytes) {
  char head[256];
  int hn =
      snprintf(head, sizeof(head),
               "POST %s HTTP/1.1\r\nHost: h\r\nContent-Length: %zu\r\n\r\n",
               path, declared);
  size_t total = (size_t)hn + first_bytes;
  char *msg = (char *)malloc(total);
  if (!msg) return false;
  memcpy(msg, head, (size_t)hn);
  memset(msg + hn, 'u', first_bytes);
  bool ok = _sc_send(fd, msg, total);
  free(msg);
  return ok;
}

/* Reads until the end of the stream for up to timeout_ms and reports how it
   ended: 0 for an orderly end of stream, the errno of a failed read, or -1 for
   a timeout. */
static int _lc_stream_end(int fd, int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  for (;;) {
    long long left = deadline - _sc_now_ms();
    if (left <= 0) return -1;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, (int)left) <= 0) return -1;
    char tmp[4096];
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r == 0) return 0;
    if (r < 0) return errno;
  }
}

TEST(lingering_close, a_refused_upload_gets_its_response_and_an_orderly_end) {
  /* The client sends the head of a large upload to a route that doesn't exist,
     with part of the body. The server refuses it with 404 and never reads the
     body, and the client must read the whole response followed by an orderly
     end of stream, not a reset. Closing with the unread body bytes still in the
     socket makes the kernel send a reset, which the read after the response
     reports as ECONNRESET. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 5, NULL);
  size_t lingers = _chttpsvr_linger_count_for_tests();
  int fd = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 5) : -1;
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope", 1048576, 32768);
  char buf[2048] = {0};
  size_t body = 0;
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), &body, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  /* Once the client closes, the linger ends right away. */
  bool drained = srv != CHTTPSVR_INVALID &&
                 _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
  REQUIRE_TRUE(drained);
}

TEST(lingering_close, a_linger_ends_at_its_deadline) {
  /* A client that neither closes nor stops sending holds a lingering connection
     only for a bounded time. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 6, NULL);
  /* 1. A client that goes quiet and keeps its end open: the sweep closes the
     linger at its deadline. */
  int quiet = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 6) : -1;
  bool q_sent =
      quiet >= 0 && _lc_send_upload_start(quiet, "/nope", 1048576, 32768);
  char buf[2048];
  int q_status =
      q_sent ? _sc_read_response(quiet, buf, sizeof(buf), NULL, 5000) : -1;
  bool q_parked =
      q_status == 404 && _sc_wait_for(srv, _lc_lingering, NULL, 1, 5000);
  long long t0 = _sc_now_ms();
  bool q_closed = q_parked && _sc_wait_for_zero(srv, _lc_lingering, NULL, 8000);
  long long q_ms = _sc_now_ms() - t0;
  if (quiet >= 0) close(quiet);

  /* 2. A client that keeps sending after the response, without end: the linger
     throws away what it sends until its deadline and then closes, so the
     client's writes fail within a few seconds. */
  int loud = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 6) : -1;
  bool l_sent =
      loud >= 0 && _lc_send_upload_start(loud, "/nope", 1048576, 4096);
  int l_status =
      l_sent ? _sc_read_response(loud, buf, sizeof(buf), NULL, 5000) : -1;
  bool write_failed = false;
  long long l_ms = -1;
  if (l_status == 404) {
    static char chunk[16384];
    memset(chunk, 'v', sizeof(chunk));
    long long l_t0 = _sc_now_ms();
    while (_sc_now_ms() - l_t0 < 10000) {
      ssize_t w = send(loud, chunk, sizeof(chunk), MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EINTR) continue;
        write_failed = errno == EPIPE || errno == ECONNRESET;
        break;
      }
    }
    l_ms = _sc_now_ms() - l_t0;
  }
  if (loud >= 0) close(loud);
  bool l_closed = srv != CHTTPSVR_INVALID &&
                  _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_EQ(q_status, 404);
  REQUIRE_TRUE(q_parked);
  REQUIRE_TRUE(q_closed);
  /* The deadline is 2 s and the sweep runs once a second. */
  REQUIRE_LT(q_ms, 4500LL);
  REQUIRE_EQ(l_status, 404);
  REQUIRE_TRUE(write_failed);
  REQUIRE_LT(l_ms, 8000LL);
  REQUIRE_TRUE(l_closed);
}

TEST(lingering_close, a_request_with_no_body_closes_at_once) {
  /* A refused request whose framing has no body, with nothing else in the
     socket, closes without a linger. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 7, NULL);
  size_t lingers = _chttpsvr_linger_count_for_tests();
  int fd = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 7) : -1;
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /nope HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _chttpsvr_linger_count_for_tests() > lingers;
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_FALSE(lingered);
}

TEST(lingering_close, a_handler_that_leaves_the_body_unread_lingers_too) {
  /* A streaming handler answers 401 without reading the body of an upload. The
     worker closes the connection after that response, and it is the same
     orderly end as for a refusal by the server; a plain close with the unread
     body in the socket would send a reset. */
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started =
      srv != CHTTPSVR_INVALID &&
      chttpsvr_register_streaming_handler(
          srv, CHTTP_POST, "/lc-refuse", _stream_reject_without_reading_handler,
          NULL) == ccol_success;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = _LC_PORT + 13;
  started = started && chttpsvr_start(srv, &cfg) == ccol_success;
  size_t lingers = _chttpsvr_linger_count_for_tests();
  int fd = started ? _sc_connect(_LC_PORT + 13) : -1;
  bool sent =
      fd >= 0 && _lc_send_upload_start(fd, "/lc-refuse", 1048576, 32768);
  char buf[2048] = {0};
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(status, 401);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
}

/* ----- accept batches ----- */

TEST(listener_accept, one_dispatch_accepts_a_bounded_batch) {
  /* 120 connections wait in the backlog while the single reactor thread is
     held. Once it's released, each listener dispatch accepts at most 64 of them
     before returning to the reactor, and every one of them still gets served.
     120 is more than one batch and fits the default backlog ceiling of the BSDs
     and macOS (kern.ipc.somaxconn, 128), which resets any connection beyond it.
     An unbounded accept loop would take all 120 in one dispatch.
 */
  enum { N = 120 };
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 8, NULL);
  int fds[N];
  for (int i = 0; i < N; i++) fds[i] = -1;
  bool armed = false;
  int opened = 0, served = 0;
  if (srv != CHTTPSVR_INVALID) {
    _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests();
    armed = true;
    fds[0] = _sc_connect(_LC_PORT + 8);
    if (fds[0] >= 0) {
      /* The dispatch for the first connection now holds the reactor. */
      _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests();
      opened = 1;
      for (int i = 1; i < N; i++) {
        fds[i] = _sc_connect(_LC_PORT + 8);
        if (fds[i] < 0) break;
        opened++;
      }
    }
    _chttpsvr_listener_max_accepts_per_dispatch_reset_for_tests();
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    for (int i = 0; i < opened; i++) {
      char buf[1024];
      if (_sc_send_str(fds[i], "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n") &&
          _sc_read_response(fds[i], buf, sizeof(buf), NULL, 10000) == 200)
        served++;
    }
  }
  size_t max_batch = _chttpsvr_listener_max_accepts_per_dispatch_for_tests();
  for (int i = 0; i < N; i++)
    if (fds[i] >= 0) close(fds[i]);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(armed);
  REQUIRE_EQ(opened, N);
  REQUIRE_EQ(served, N);
  REQUIRE_GT(max_batch, (size_t)1);
  REQUIRE_LE(max_batch, (size_t)64);
}

/* ----- OPTIONS * against a concurrent subrouter registration ----- */

typedef struct {
  int port;
  _Atomic bool stop;
  _Atomic int answered;
} _lc_star_arg_t;

static void *_lc_star_thread(void *arg) {
  _lc_star_arg_t *a = (_lc_star_arg_t *)arg;
  int fd = _sc_connect(a->port);
  while (fd >= 0 && !atomic_load(&a->stop)) {
    char buf[1024];
    if (!_sc_send_str(fd, "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n") ||
        _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) != 200)
      break;
    atomic_fetch_add(&a->answered, 1);
  }
  if (fd >= 0) close(fd);
  return NULL;
}

TEST(chttpserver, options_star_is_safe_beside_a_subrouter_registration) {
  /* "OPTIONS *" requests arrive while the application adds sub-routers, which
     grows the router array under its lock. The OPTIONS * path must not read
     that array without the lock. ThreadSanitizer reports the unlocked read in
     the test_tsan build, and the ordinary build checks that both sides keep
     working. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 9, NULL);
  _lc_star_arg_t arg = {.port = _LC_PORT + 9, .stop = false, .answered = 0};
  pthread_t tid;
  bool started = srv != CHTTPSVR_INVALID &&
                 pthread_create(&tid, NULL, _lc_star_thread, &arg) == 0;
  int made = 0;
  for (int i = 0; started && i < 300; i++) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "/lc-sub-%d", i);
    if (chttpsvr_subrouter(srv, prefix)) made++;
    if (i % 50 == 0) _sc_nap_ms(1);
  }
  /* Let the client finish a few more exchanges after the last one. */
  long long until = _sc_now_ms() + 5000;
  while (started && atomic_load(&arg.answered) < 5 && _sc_now_ms() < until)
    _sc_nap_ms(1);
  atomic_store(&arg.stop, true);
  bool joined = started ? _bounded_join(tid, NULL) : false;
  if (started && !joined) pthread_detach(tid);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(joined);
  REQUIRE_EQ(made, 300);
  REQUIRE_GE(atomic_load(&arg.answered), 5);
}

/* ----- body memory with no body limit ----- */

TEST(slow_clients, a_huge_declared_length_never_wraps_the_memory_count) {
  /* With max_body_size 0 a Content-Length can declare almost SIZE_MAX bytes.
     Such a request goes ahead under the progress rule, and its charge has to
     stay within the limit instead of wrapping the sum of the charges.
     Charging the declared length would push the count far above the limit. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 0;
  cfg.max_partial_body_memory = 65536;
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 10, &cfg);
  _chttpsvr_mem_peak_reset_for_tests();
  int fd = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 10) : -1;
  bool sent = fd >= 0 &&
              _sc_send_str(fd,
                           "POST /lc HTTP/1.1\r\nHost: h\r\n"
                           "Content-Length: 18446744073709551000\r\n\r\nabc");
  bool charged = sent && _sc_wait_for(srv, _sc_mem_in_use, NULL, 1, 5000);
  size_t in_use =
      srv != CHTTPSVR_INVALID ? _chttpsvr_mem_in_use_for_tests(srv) : 0;
  size_t peak = _chttpsvr_mem_peak_for_tests();
  if (fd >= 0) close(fd);
  bool released = srv != CHTTPSVR_INVALID &&
                  _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  /* An ordinary request still gets served normally afterwards. */
  int after = -1;
  int fd2 = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 10) : -1;
  if (fd2 >= 0 && _sc_send_str(fd2,
                               "POST /lc HTTP/1.1\r\nHost: h\r\n"
                               "Content-Length: 3\r\n\r\nxyz")) {
    char buf[1024];
    after = _sc_read_response(fd2, buf, sizeof(buf), NULL, 5000);
  }
  if (fd2 >= 0) close(fd2);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(charged);
  REQUIRE_LE(in_use, (size_t)65536);
  REQUIRE_LE(peak, (size_t)65536);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(after, 200);
}

/* ----- a lost copy of pipelined bytes closes the connection ----- */

/* An allocator that fails the nth allocation of one size after it's armed and
   passes every other call through. */
static _Atomic size_t g_lc_fail_size = 0;
static _Atomic int g_lc_fail_nth = 0;
static _Atomic int g_lc_fail_seen = 0;
static _Atomic int g_lc_failed = 0;
static bool _lc_should_fail(size_t n) {
  if (n != atomic_load(&g_lc_fail_size)) return false;
  if (atomic_fetch_add(&g_lc_fail_seen, 1) + 1 != atomic_load(&g_lc_fail_nth))
    return false;
  atomic_fetch_add(&g_lc_failed, 1);
  return true;
}
static void *_lc_malloc(size_t n) {
  return _lc_should_fail(n) ? NULL : malloc(n);
}
static void _lc_free(void *p) { free(p); }
static void *_lc_calloc(size_t n, size_t s) {
  return _lc_should_fail(n * s) ? NULL : calloc(n, s);
}
static void *_lc_realloc(void *p, size_t s) {
  return _lc_should_fail(s) ? NULL : realloc(p, s);
}
static void _lc_arm_failure(size_t size, int nth) {
  atomic_store(&g_lc_fail_seen, 0);
  atomic_store(&g_lc_failed, 0);
  atomic_store(&g_lc_fail_nth, nth);
  atomic_store(&g_lc_fail_size, size);
}

/* Sends two pipelined GETs in one write, the second one exactly second_len
   bytes long, and reports what the client sees: the status of the first
   response, the status of the second (-1 when none came), and how the stream
   ended after the first one when no second one came. The third allocation of
   second_len bytes fails, after the divert's carry-over and the stream's copy,
   so it's the reclaim of the leftover. */
static void _lc_pipeline_with_lost_leftover(int port, bool park_write,
                                            int *first, int *second, int *end) {
  ccol_memmgmt_procs_t mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  *first = *second = *end = -2;
  if (srv == CHTTPSVR_INVALID) return;
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return;
  }
  const char *req1 = "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n";
  char req2[1024];
  int n2 = snprintf(req2, sizeof(req2),
                    "GET /lc HTTP/1.1\r\nHost: h\r\nX-Pad: %0730d\r\n\r\n", 0);
  char both[2048];
  size_t l1 = strlen(req1);
  memcpy(both, req1, l1);
  memcpy(both + l1, req2, (size_t)n2);
  int fd = _sc_connect(port);
  if (fd >= 0) {
    _lc_arm_failure((size_t)n2, 3);
    if (park_write) _chttpsvr_force_short_response_write_for_tests(10);
    if (_sc_send(fd, both, l1 + (size_t)n2)) {
      char buf[2048];
      *first = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
      struct pollfd p = {.fd = fd, .events = POLLIN};
      if (*first == 200 && poll(&p, 1, 3000) > 0) {
        char peek;
        ssize_t r = recv(fd, &peek, 1, MSG_PEEK);
        if (r > 0)
          *second = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
        else
          *end = r == 0 ? 0 : errno;
      } else if (*first == 200) {
        *end = -1; /* nothing within 3 s, the server is waiting for a request */
      }
    }
    close(fd);
  }
  _chttpsvr_force_short_response_write_for_tests(0);
  atomic_store(&g_lc_fail_size, 0);
  chttpsvr_destroy(srv);
}

TEST(chttpserver, a_lost_copy_of_pipelined_bytes_closes_the_connection) {
  /* Reclaiming the pipelined bytes of a next request fails for lack of memory.
     Those bytes are gone, so the connection has to close after the first
     response instead of waiting for a request its client has already sent.
     A connection kept alive would answer nothing within 3 s. The second pass
     parks the first response on a full socket, which reclaims the bytes on
     another path. */
  for (int pass = 0; pass < 2; pass++) {
    int first, second, end;
    _lc_pipeline_with_lost_leftover(_LC_PORT + 11 + pass, pass == 1, &first,
                                    &second, &end);
    int failed = atomic_load(&g_lc_failed);
    REQUIRE_EQ(failed, 1);
    REQUIRE_EQ(first, 200);
    REQUIRE_EQ(second, -2);
    REQUIRE_EQ(end, 0);
  }
}

/* ========================================================================== */
/*        SWEEP COVERAGE, RESTART, LISTEN ADDRESS, LINGER, PUSH-BACK          */
/* ========================================================================== */

static size_t _rx_idle(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_idle_count_for_tests(h);
}

#define _RX_IDLE_N 300
#define _RX_PARKED_N 3
#define _RX_BATCH 16

/* Opens a POST of /len that declares 1000 bytes and sends its head and the
 * first 10 body bytes in one send(2), so they arrive together and the server
 * parks the body with nothing else on the way. Bytes arriving after the park
 * would wake the connection, and a resume running while the test ticks the
 * sweep would take it off the parked list for that tick. */
static int _rx_open_parked_post(int port) {
  int fd = _sc_connect(port);
  if (fd < 0) return -1;
  char req[256];
  int n = snprintf(req, sizeof(req),
                   "POST /len HTTP/1.1\r\nHost: h\r\nContent-Length: "
                   "1000\r\n\r\n0123456789");
  if (n <= 0 || (size_t)n >= sizeof(req) || !_sc_send(fd, req, (size_t)n)) {
    close(fd);
    return -1;
  }
  return fd;
}

TEST(sweep, one_tick_closes_every_expired_idle_and_parked_connection) {
  /* 300 connections sit idle past the idle timeout and three bodies sit parked
     past their gap limit. One sweep tick must close every idle one and answer
     every parked one with 408. A sweep that took a fixed batch per tick would
     close 64 of the idle ones and never get to the parked list in that tick,
     and the background sweep adds at most one more batch during this test's
     window. Both limits are 30 s and the rate floor is off, so no connection
     expires while a slow host opens the others (the floor would close the
     parked bodies 5 s after they stop), and the test's tick runs 60 s ahead on
     both clocks. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.idle_timeout_us = 30000000;
  cfg.read_timeout_us = 30000000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  chttpsvr srv = _sc_server(_LC_PORT + 15, &cfg);
  int idle_fds[_RX_IDLE_N];
  int parked_fds[_RX_PARKED_N];
  for (int i = 0; i < _RX_IDLE_N; i++) idle_fds[i] = -1;
  for (int i = 0; i < _RX_PARKED_N; i++) parked_fds[i] = -1;
  bool opened = srv != CHTTPSVR_INVALID;
  for (int i = 0; opened && i < _RX_PARKED_N; i++) {
    parked_fds[i] = _rx_open_parked_post(_LC_PORT + 15);
    opened = parked_fds[i] >= 0;
  }
  opened =
      opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, _RX_PARKED_N, 10000);
  /* The connections open in batches, and each batch waits until the server
     holds it as idle. A burst of 300 overflows the accept queue of a server
     that accepts slowly (under valgrind), and FreeBSD then drops or refuses the
     connections that don't fit. */
  for (int i = 0; opened && i < _RX_IDLE_N; i++) {
    idle_fds[i] = _sc_connect(_LC_PORT + 15);
    opened = idle_fds[i] >= 0;
    if (opened && ((i + 1) % _RX_BATCH == 0 || i + 1 == _RX_IDLE_N))
      opened = _sc_wait_for(srv, _rx_idle, NULL, (size_t)(i + 1), 20000);
  }
  size_t idle_after = SIZE_MAX, parked_after = SIZE_MAX;
  if (opened) {
    _chttpsvr_advance_slow_clock_for_tests(60000); /* past the body gap */
    _chttpsvr_sweep_ahead_for_tests(srv, 60000);   /* past the idle limit */
    idle_after = _chttpsvr_idle_count_for_tests(srv);
    parked_after = _chttpsvr_parked_count_for_tests(srv, 1);
  }
  int timeouts = 0;
  char buf[2048];
  for (int i = 0; i < _RX_PARKED_N; i++) {
    if (parked_fds[i] < 0) continue;
    if (opened &&
        _sc_read_response(parked_fds[i], buf, sizeof(buf), NULL, 5000) == 408)
      timeouts++;
    close(parked_fds[i]);
  }
  for (int i = 0; i < _RX_IDLE_N; i++)
    if (idle_fds[i] >= 0) close(idle_fds[i]);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(idle_after, (size_t)0);
  REQUIRE_EQ(parked_after, (size_t)0);
  REQUIRE_EQ(timeouts, _RX_PARKED_N);
}

/* ----- a restart retires the pools of the previous run ----- */

static size_t _rx_workers_active(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_pool_active_for_tests(h, false);
}

typedef struct {
  chttpsvr srv;
  _Atomic bool done;
} _rx_destroy_ctx_t;

static void *_rx_destroy_thread(void *arg) {
  _rx_destroy_ctx_t *ctx = (_rx_destroy_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  atomic_store(&ctx->done, true);
  return NULL;
}

TEST(restart, neither_waits_for_nor_cuts_a_request_of_the_previous_run) {
  /* A handler of the previous run holds its worker when the server is stopped
     and started again. The restart must return and listen immediately, the
     running request must still complete with its own response, and the pool
     that ran it must drain in the background. A destroy while that request
     still runs waits for it. A restart that drained the previous pools first
     would wait for the handler, which here only ends when the test opens its
     gate after the restart, or on its own 10 s bound. */
  _sc_gate_open(false);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  _rx_destroy_ctx_t *ctx = calloc(1, sizeof(*ctx));
  ctx->srv = _sc_server(_LC_PORT + 16, &cfg);
  chttpsvr srv = ctx->srv;
  bool started = srv != CHTTPSVR_INVALID;
  size_t drained_before = _chttpsvr_retired_drained_for_tests();
  int a = started ? _sc_connect(_LC_PORT + 16) : -1;
  bool running = a >= 0 &&
                 _sc_send_str(a, "GET /busy HTTP/1.1\r\nHost: h\r\n\r\n") &&
                 _sc_wait_for(srv, _rx_workers_active, NULL, 1, 5000);
  long long restart_ms = -1;
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  int hello = -1;
  char buf[4096];
  if (running) {
    chttpsvr_stop(srv);
    chttpsvr_config_t again = cfg;
    again.host = "127.0.0.1";
    again.port = (uint16_t)(_LC_PORT + 16);
    long long t0 = _sc_now_ms();
    restart_rv = chttpsvr_start(srv, &again);
    restart_ms = _sc_now_ms() - t0;
    int b = _sc_connect(_LC_PORT + 16);
    if (b >= 0 && _sc_send_str(b, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
      hello = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
    if (b >= 0) close(b);
  }
  /* A destroy now waits for the request still running on the retired
     pool, and must not free the server under it. */
  pthread_t th;
  bool destroy_started =
      started && pthread_create(&th, NULL, _rx_destroy_thread, ctx) == 0;
  if (destroy_started) _sc_nap_ms(300);
  bool destroy_waited = destroy_started && !atomic_load(&ctx->done);
  _sc_gate_open(true);
  int a_status =
      running ? _sc_read_response(a, buf, sizeof(buf), NULL, 10000) : -1;
  bool a_whole = a_status == 200 && strstr(buf, "gated") != NULL;
  bool joined = destroy_started ? _bounded_join(th, NULL) : false;
  if (!destroy_started && started) chttpsvr_destroy(srv);
  size_t drained_after = _chttpsvr_retired_drained_for_tests();
  if (a >= 0) close(a);
  if (joined || !destroy_started) free(ctx);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(running);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_LT(restart_ms, 3000LL);
  REQUIRE_EQ(hello, 200);
  REQUIRE_TRUE(destroy_waited);
  REQUIRE_TRUE(a_whole);
  REQUIRE_TRUE(joined);
  REQUIRE_GT(drained_after, drained_before);
}

TEST(restart, a_failed_registration_fails_the_start) {
  /* The registry of servers can't grow, so the start fails cleanly, nothing
     listens, and a later start succeeds. A start that ignored a failed
     registration would report success for a server the sweep never reaches. */
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool created = srv != CHTTPSVR_INVALID;
  if (created)
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_LC_PORT + 17);
  ccol_retval_t rv1 = ccol_unexpected_failure, rv2 = ccol_unexpected_failure;
  int status_after_fail = -2, status_after_start = -2;
  if (created) {
    _chttpsvr_fail_next_servers_register_for_tests();
    rv1 = chttpsvr_start(srv, &cfg);
    status_after_fail = _lc_get_v4(_LC_PORT + 17);
    rv2 = chttpsvr_start(srv, &cfg);
    status_after_start = _lc_get_v4(_LC_PORT + 17);
    chttpsvr_destroy(srv);
  }
  REQUIRE_TRUE(created);
  REQUIRE_EQ((int)rv1, (int)ccol_not_enough_memory);
  REQUIRE_EQ(status_after_fail, -1);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
  REQUIRE_EQ(status_after_start, 200);
}

/* ----- the listen address ----- */

TEST(listener_host, a_host_name_reports_a_port_taken_on_its_address) {
  /* Another socket holds 127.0.0.1 on the port. "localhost" listens on
     127.0.0.1, so the start has to fail, and nothing may listen on ::1 in its
     place. Non-vacuous where localhost also resolves to ::1: a start that moved
     on to the next address would listen there and report success. The holding
     socket binds port 0 and the test uses the port the kernel picks, because a
     fixed port may be in use: FreeBSD gives client connections ephemeral ports
     from 10000 up, so an earlier connection of this suite can hold any fixed
     port in TIME_WAIT, and a bind without SO_REUSEADDR then fails. */
  int other = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = 0;
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  socklen_t alen = sizeof(a);
  bool held = other >= 0 &&
              bind(other, (struct sockaddr *)&a, sizeof(a)) == 0 &&
              listen(other, 4) == 0 &&
              getsockname(other, (struct sockaddr *)&a, &alen) == 0;
  uint16_t port = held ? ntohs(a.sin_port) : 0;
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  ccol_retval_t rv = ccol_success;
  if (held && srv != CHTTPSVR_INVALID) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "localhost";
    cfg.port = port;
    rv = chttpsvr_start(srv, &cfg);
  }
  int v6 = held && _lc_have_ipv6_loopback() ? _lc_get_v6(port) : -1;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  if (other >= 0) close(other);
  REQUIRE_TRUE(held);
  REQUIRE_EQ((int)rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ(v6, -1);
}

/* Whether "localhost" resolves to an IPv6 address on this host. */
static bool _rx_localhost_has_ipv6(void) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_INET6;
  hints.ai_socktype = SOCK_STREAM;
  if (getaddrinfo("localhost", "80", &hints, &res) != 0 || !res) return false;
  freeaddrinfo(res);
  return true;
}

TEST(listener_host, ipv6_only_makes_a_host_name_listen_on_its_ipv6_address) {
  /* With ipv6_only, "localhost" listens on ::1 and not on 127.0.0.1. Skipped on
     a host where localhost has no IPv6 address or ::1 can't be bound.
     A host name that always took its IPv4 address first would listen on
     127.0.0.1 and refuse the IPv6 client. */
  if (!_lc_have_ipv6_loopback() || !_rx_localhost_has_ipv6()) {
    fprintf(stderr, "SKIP: localhost has no usable IPv6 address\n");
    return;
  }
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.ipv6_only = true;
  chttpsvr srv = _lc_server("localhost", _LC_PORT + 19, &cfg);
  int v6 = srv != CHTTPSVR_INVALID ? _lc_get_v6(_LC_PORT + 19) : -2;
  int v4 = srv != CHTTPSVR_INVALID ? _lc_get_v4(_LC_PORT + 19) : -2;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_EQ(v6, 200);
  REQUIRE_EQ(v4, -1);
}

TEST(listener_host, a_bracketed_address_names_the_address_inside) {
  /* "[127.0.0.1]" and "[::1]" listen on the address inside the brackets, and an
     unclosed bracket fails the start. The brackets have to come off first,
     because the resolver refuses a bracketed address. */
  chttpsvr srv4 = _lc_server("[127.0.0.1]", _LC_PORT + 20, NULL);
  int v4 = srv4 != CHTTPSVR_INVALID ? _lc_get_v4(_LC_PORT + 20) : -2;
  if (srv4 != CHTTPSVR_INVALID) chttpsvr_destroy(srv4);
  bool have_v6 = _lc_have_ipv6_loopback();
  chttpsvr srv6 =
      have_v6 ? _lc_server("[::1]", _LC_PORT + 21, NULL) : CHTTPSVR_INVALID;
  int v6 = !have_v6
               ? 200
               : (srv6 != CHTTPSVR_INVALID ? _lc_get_v6(_LC_PORT + 21) : -2);
  if (srv6 != CHTTPSVR_INVALID) chttpsvr_destroy(srv6);
  chttpsvr bad = _lc_server("[::1", _LC_PORT + 22, NULL);
  bool bad_refused = bad == CHTTPSVR_INVALID;
  if (bad != CHTTPSVR_INVALID) chttpsvr_destroy(bad);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6, 200);
  REQUIRE_TRUE(bad_refused);
}

TEST(listener_host, a_host_that_does_not_resolve_is_logged_with_its_reason) {
  /* The engine log names the reason the resolver gave. A name inside brackets
     is refused by the resolver itself without any lookup, which keeps the
     host's name-service modules out of the test. A failure that logged only
     the start's errno, which the resolver doesn't set, would carry no reason at
     all. */
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  int gai = getaddrinfo("a..b", "80", &hints, &res);
  if (res) freeaddrinfo(res);
  int pfd[2] = {-1, -1};
  bool piped = gai != 0 && pipe(pfd) == 0;
  if (piped) fcntl(pfd[0], F_SETFL, O_NONBLOCK);
  clog plog =
      piped ? clog_create_from_fd(pfd[1], CLOG_INFO, NULL) : CLOG_INVALID;
  bool installed = plog && chttpsvr_set_engine_logger(plog) == ccol_success;
  chttpsvr srv =
      installed ? _lc_server("[a..b]", _LC_PORT + 23, NULL) : CHTTPSVR_INVALID;
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  char text[8192] = {0};
  size_t got = 0;
  if (piped) {
    ssize_t r;
    while (got + 1 < sizeof(text) &&
           (r = read(pfd[0], text + got, sizeof(text) - 1 - got)) > 0)
      got += (size_t)r;
  }
  chttpsvr_set_engine_logger(g_test_logger);
  if (plog) clog_close(plog);
  if (pfd[0] >= 0) close(pfd[0]);
  if (pfd[1] >= 0) close(pfd[1]);
  REQUIRE_TRUE(piped);
  REQUIRE_TRUE(installed);
  REQUIRE_TRUE(srv == CHTTPSVR_INVALID);
  REQUIRE_TRUE(strstr(text, gai_strerror(gai)) != NULL);
}

/* ----- query keys ----- */

static void _rx_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t n_plain = 0, n_encoded = 0;
  chttpsvr_req_query(req, "a b", &n_plain);
  chttpsvr_req_query(req, "a%20b", &n_encoded);
  const char *one = NULL;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "x&y", &one);
  chttpsvr_resp_printf(resp, "%zu %zu %d %s", n_plain, n_encoded, (int)rv,
                       one ? one : "-");
}

TEST(chttpserver, a_query_key_is_matched_against_the_decoded_keys) {
  /* The request's key is decoded and the argument is taken as it is, as Go's
     url.Values does: "a b" finds a+b and a%20b, "a%20b" finds nothing, and
     "x&y" finds x%26y. */
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started =
      srv != CHTTPSVR_INVALID &&
      chttpsvr_register_handler(srv, CHTTP_GET, "/q", _rx_query_handler,
                                NULL) == ccol_success;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_LC_PORT + 24);
  started = started && chttpsvr_start(srv, &cfg) == ccol_success;
  int fd = started ? _sc_connect(_LC_PORT + 24) : -1;
  char buf[4096] = {0};
  int status = -1;
  if (fd >= 0 &&
      _sc_send_str(fd,
                   "GET /q?a+b=1&a%20b=2&x%26y=3 HTTP/1.1\r\nHost: h\r\n\r\n"))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n2 0 0 3") != NULL);
}

/* ----- lingering close ----- */

TEST(lingering_close, a_linger_gives_the_body_memory_back_at_once) {
  /* A parked body that the sweep answers with 408 lingers, and its body memory
     reservation goes back when the linger starts, not when it ends: once the
     connection lingers nothing is reserved, and it's still lingering. The
     linger drops the reservation a few steps before it parks, so the test waits
     for the linger and only then looks at the memory. A reservation held until
     the close would still be held while the connection lingers. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 25, &cfg);
  int fd = srv != CHTTPSVR_INVALID
               ? _sc_open_partial_post(_LC_PORT + 25, "/len", 8000, 100, NULL)
               : -1;
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged = parked ? _chttpsvr_mem_in_use_for_tests(srv) : 0;
  int status = -1;
  bool released = false;
  size_t lingering = 0;
  if (parked) {
    _chttpsvr_advance_slow_clock_for_tests(60000);
    _chttpsvr_sweep_now_for_tests(srv);
    char buf[2048];
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
    if (_sc_wait_for(srv, _lc_lingering, NULL, 1, 5000)) {
      released = _chttpsvr_mem_in_use_for_tests(srv) == 0;
      lingering = _chttpsvr_parked_count_for_tests(srv, 6);
    }
  }
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(charged, (size_t)8000);
  REQUIRE_EQ(status, 408);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(lingering, (size_t)1);
}

TEST(lingering_close, a_large_refused_upload_is_taken_whole_before_the_close) {
  /* The client sends the whole 4 MiB body of a refused upload before it reads
     the response, as many clients do. The linger should take every byte, and
     the client then reads the 404 and an orderly end of stream. A linger that
     stopped after a fixed byte count would close under the client, whose write
     would then fail with a reset. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 26, NULL);
  int fd = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 26) : -1;
  const size_t total = (size_t)4 * 1024 * 1024;
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope", total, 0);
  size_t pushed = 0;
  int write_errno = 0;
  if (sent) {
    static char chunk[65536];
    memset(chunk, 'w', sizeof(chunk));
    while (pushed < total) {
      size_t want =
          total - pushed < sizeof(chunk) ? total - pushed : sizeof(chunk);
      ssize_t w = send(fd, chunk, want, MSG_NOSIGNAL);
      if (w < 0) {
        if (errno == EINTR) continue;
        write_errno = errno;
        break;
      }
      pushed += (size_t)w;
    }
  }
  char buf[2048] = {0};
  int status = pushed == total
                   ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000)
                   : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(write_errno, 0);
  REQUIRE_EQ(pushed, total);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
}

TEST(lingering_close, a_close_after_the_response_lingers_on_unread_bytes) {
  /* The request asks for Connection: close, and the client sends more bytes
     while the handler runs, which the server never reads. The client only reads
     after a pause, and by then a plain close would have answered those bytes
     with a reset that destroys the response in the client. The server has to
     linger instead, and the client then reads the whole response and an orderly
     end. If the linger were decided from the parser alone, the server would
     close at once and the read would report ECONNRESET. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 41, &cfg);
  _sc_gate_open(false);
  int runs = atomic_load(&g_sc_gate_runs);
  size_t lingers = _chttpsvr_linger_count_for_tests();
  int fd = srv != CHTTPSVR_INVALID ? _sc_connect(_LC_PORT + 41) : -1;
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "GET /busy HTTP/1.1\r\nHost: h\r\n"
                                      "Connection: close\r\n\r\n");
  bool entered = false;
  for (int i = 0; sent && i < 5000 && !entered; i++) {
    entered = atomic_load(&g_sc_gate_runs) > runs;
    if (!entered) _sc_nap_ms(1);
  }
  /* The start of a pipelined request that the server never reads. */
  bool extra = entered && _sc_send_str(fd,
                                       "GET /hello HTTP/1.1\r\nHost: h\r\n"
                                       "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  _sc_gate_open(true);
  _sc_nap_ms(300);
  char buf[2048] = {0};
  int status = extra ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = extra && _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  _sc_gate_open(false);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(extra);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "gated") != NULL);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
}

TEST(listeners, a_tcp_listener_does_not_turn_on_fast_open) {
  /* TCP Fast Open lets a SYN carry request bytes that a retransmission can
     replay, so the server leaves it off. The test finds the listening socket
     among the process's descriptors by its port, and tells it from an
     accepted socket on the same port by getpeername(), which fails only on a
     socket with no peer. SO_ACCEPTCONN would say the same thing more
     directly, but macOS refuses that option. A listener that set
     TCP_FASTOPEN would report a queue length above 0. */
#if defined(TCP_FASTOPEN)
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 42, &cfg);
  int found = 0, qlen_seen = -1;
  for (int fd = 3; srv != CHTTPSVR_INVALID && fd < 4096 && !found; fd++) {
    struct sockaddr_storage ss;
    socklen_t sl = sizeof(ss);
    if (getsockname(fd, (struct sockaddr *)&ss, &sl) != 0) continue;
    if (ss.ss_family != AF_INET) continue;
    struct sockaddr_in *sin = (struct sockaddr_in *)&ss;
    if (ntohs(sin->sin_port) != _LC_PORT + 42) continue;
    struct sockaddr_storage peer;
    socklen_t pl = sizeof(peer);
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) == 0) continue;
    int q = -1;
    socklen_t ql = sizeof(q);
    if (getsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &q, &ql) == 0) qlen_seen = q;
    found = 1;
  }
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_EQ(found, 1);
  REQUIRE_EQ(qlen_seen, 0);
#else
  fprintf(stderr, "SKIP: this system has no TCP_FASTOPEN to check\n");
#endif
}

/* ----- a lost push-back of pipelined bytes closes the connection ----- */

/* Sends the head of a POST whose 5-byte body comes later, in one write together
   with a pipelined GET of exactly req2 bytes, and reports what the client sees,
   like _lc_pipeline_with_lost_leftover does. The first allocation of the GET's
   size, which is the push-back of those bytes by the body read, fails. */
static void _rx_pipeline_after_body(int port, const char *path, int *first,
                                    int *second, int *end) {
  ccol_memmgmt_procs_t mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  *first = *second = *end = -2;
  if (srv == CHTTPSVR_INVALID) return;
  chttpsvr_register_handler(srv, CHTTP_POST, "/rx-buffered", _sc_len_handler,
                            NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/rx-streaming",
                                      _stream_echo_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return;
  }
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST %s HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n",
                    path);
  char req2[1024];
  int n2 = snprintf(req2, sizeof(req2),
                    "GET /lc HTTP/1.1\r\nHost: h\r\nX-Pad: %0741d\r\n\r\n", 0);
  char rest[2048];
  memcpy(rest, "hello", 5);
  memcpy(rest + 5, req2, (size_t)n2);
  int fd = _sc_connect(port);
  if (fd >= 0) {
    _lc_arm_failure((size_t)n2, 1);
    /* The head must be read and diverted on its own before the rest is sent,
       so wait, with a bound, until the buffered route parks its body or the
       streaming handler runs on its pool. On a timeout the rest is never sent,
       *first stays -2 and the caller fails. */
    _sc_probe_fn diverted = strcmp(path, "/rx-streaming") == 0
                                ? _sc_stream_active
                                : _sc_parked_bodies;
    if (_sc_send(fd, head, (size_t)hn) &&
        _sc_wait_for(srv, diverted, NULL, 1, 5000)) {
      if (_sc_send(fd, rest, 5 + (size_t)n2)) {
        char buf[2048];
        *first = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
        struct pollfd p = {.fd = fd, .events = POLLIN};
        if (*first == 200 && poll(&p, 1, 3000) > 0) {
          char peek;
          ssize_t r = recv(fd, &peek, 1, MSG_PEEK);
          if (r > 0)
            *second = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
          else
            *end = r == 0 ? 0 : errno;
        } else if (*first == 200) {
          *end = -1; /* nothing within 3 s: the server waits for a request */
        }
      }
    }
    close(fd);
  }
  atomic_store(&g_lc_fail_size, 0);
  chttpsvr_destroy(srv);
}

TEST(chttpserver, a_lost_push_back_of_pipelined_bytes_closes_the_connection) {
  /* Reading a body takes the start of a pipelined next request off the socket,
     and keeping those bytes fails for lack of memory. They're gone, so the
     connection closes after the response instead of waiting for a request
     its client has already sent, on a buffered route and on a streaming route.
     A connection kept alive would answer nothing within 3 s. */
  const char *paths[2] = {"/rx-buffered", "/rx-streaming"};
  for (int pass = 0; pass < 2; pass++) {
    int first, second, end;
    _rx_pipeline_after_body(_LC_PORT + 27 + pass, paths[pass], &first, &second,
                            &end);
    int failed = atomic_load(&g_lc_failed);
    REQUIRE_EQ(failed, 1);
    REQUIRE_EQ(first, 200);
    REQUIRE_EQ(second, -2);
    REQUIRE_EQ(end, 0);
  }
}

/* ----- a wait that a signal interrupts keeps its deadline ----- */

static void _rx_sigusr1_noop(int sig) { (void)sig; }

typedef struct {
  int fd;
  _Atomic bool done;
  ssize_t got;
  bool timed_out;
  long long ms;
} _rx_read_ctx_t;

static void *_rx_blocking_read(void *arg) {
  _rx_read_ctx_t *ctx = (_rx_read_ctx_t *)arg;
  chttp1_stream_t st;
  chttp1_stream_prepare(&st, ctx->fd, NULL, 0, NULL);
  char buf[64];
  long long t0 = _sc_now_ms();
  ctx->got = chttp1_stream_read(&st, buf, sizeof(buf), 300);
  ctx->ms = _sc_now_ms() - t0;
  ctx->timed_out = chttp1_stream_timed_out(&st);
  chttp1_stream_release(&st);
  atomic_store(&ctx->done, true);
  return NULL;
}

TEST(chttp1_parser, a_wait_that_signals_interrupt_still_ends_at_its_timeout) {
  /* A blocking read with a 300 ms timeout on a socket that never gets data, on
     a thread that a signal interrupts every 20 ms, must time out after about
     300 ms. A wait that restarted its whole timeout after every signal would
     never time out while the signals keep coming, which here is 3 s. */
  struct sigaction sa, old;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _rx_sigusr1_noop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  sigaction(SIGUSR1, &sa, &old);
  int sv[2] = {-1, -1};
  bool paired = socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0;
  _rx_read_ctx_t ctx = {.fd = sv[0], .done = false, .got = 0};
  pthread_t th;
  bool created =
      paired && pthread_create(&th, NULL, _rx_blocking_read, &ctx) == 0;
  long long t0 = _sc_now_ms();
  while (created && !atomic_load(&ctx.done) && _sc_now_ms() - t0 < 3000) {
    pthread_kill(th, SIGUSR1);
    _sc_nap_ms(20);
  }
  bool joined = created && _bounded_join(th, NULL);
  sigaction(SIGUSR1, &old, NULL);
  if (sv[0] >= 0) close(sv[0]);
  if (sv[1] >= 0) close(sv[1]);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(joined);
  REQUIRE_EQ(ctx.got, (ssize_t)-1);
  REQUIRE_TRUE(ctx.timed_out);
  REQUIRE_GE(ctx.ms, 250LL);
  REQUIRE_LT(ctx.ms, 2000LL);
}

TEST(restart, a_request_whose_submit_meets_a_retired_pool_is_still_served) {
  /* Two pipelined requests. The worker running the first one reads the worker
     pool for the second one and is held there while the server is stopped and
     started again, which retires that pool and starts its drain, so the held
     submit then meets a pool in shutdown. The second request has to go to the
     new run's pool and get 200. A submit that took the retired pool's refusal
     as the answer would give 503, and the counter of moved submits shows that
     the window was reached. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 29, NULL);
  bool started = srv != CHTTPSVR_INVALID;
  size_t drains_before = _chttpsvr_retired_drain_started_for_tests();
  size_t retried_before = _chttpsvr_submit_retried_for_tests();
  int fd = started ? _sc_connect(_LC_PORT + 29) : -1;
  bool entered = false, draining = false, shut_down = false;
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  if (fd >= 0) {
    _chttpsvr_arm_divert_submit_hook_for_tests(2);
    if (_sc_send_str(fd,
                     "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n"
                     "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n")) {
      for (int i = 0; i < 5000 && !entered; i++) {
        entered = _chttpsvr_divert_submit_hook_entered_for_tests();
        if (!entered) _sc_nap_ms(1);
      }
    }
    if (entered) {
      chttpsvr_stop(srv);
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = (uint16_t)(_LC_PORT + 29);
      restart_rv = chttpsvr_start(srv, &cfg);
      for (int i = 0; i < 5000 && !draining; i++) {
        draining = _chttpsvr_retired_drain_started_for_tests() > drains_before;
        if (!draining) _sc_nap_ms(1);
      }
      /* Wait until the drain thread has started shutting down the retired
         worker pool, so that the held submit meets a pool that refuses it. The
         held worker belongs to that pool, so the drain can't end and the pool
         can't be destroyed while this loop reads it. */
      for (int i = 0; draining && i < 5000 && !shut_down; i++) {
        shut_down = _chttpsvr_retired_worker_shutdown_started_for_tests();
        if (!shut_down) _sc_nap_ms(1);
      }
    }
  }
  _chttpsvr_release_divert_submit_hook_for_tests();
  char buf[2048];
  int first =
      entered ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int second =
      first == 200 ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  size_t retried = _chttpsvr_submit_retried_for_tests() - retried_before;
  if (fd >= 0) close(fd);
  if (started) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(entered);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_TRUE(draining);
  REQUIRE_TRUE(shut_down);
  REQUIRE_EQ(first, 200);
  REQUIRE_EQ(second, 200);
  REQUIRE_EQ(retried, (size_t)1);
}

typedef struct {
  chttpsvr srv;
  chttpsvr_config_t cfg;
  ccol_retval_t rv;
} _rx_start_ctx_t;

static void *_rx_start_thread(void *arg) {
  _rx_start_ctx_t *ctx = (_rx_start_ctx_t *)arg;
  ctx->rv = chttpsvr_start(ctx->srv, &ctx->cfg);
  return NULL;
}

TEST(restart, a_streaming_request_during_the_start_uses_the_new_settings) {
  /* A keep-alive connection of the first run, which has one streaming thread,
     sends the first streaming request of the second run, which asks for three,
     while that start is held right after it swapped in the second run's pools.
     The streaming pool the request creates needs to have three threads.
     A start that stored its streaming settings after the swap would let that
     request create the pool with the first run's one thread. */
  _sc_gate_open(true);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 31, &cfg);
  bool started = srv != CHTTPSVR_INVALID;
  char buf[2048];
  int fd = started ? _sc_connect(_LC_PORT + 31) : -1;
  int hello = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  _rx_start_ctx_t sctx = {
      .srv = srv, .cfg = cfg, .rv = ccol_unexpected_failure};
  sctx.cfg.host = "127.0.0.1";
  sctx.cfg.port = (uint16_t)(_LC_PORT + 32);
  sctx.cfg.streaming_thread_count = 3;
  bool held = false, start_started = false, start_joined = false;
  int threads = 0, gated = -1;
  pthread_t start_tid;
  if (hello == 200) {
    chttpsvr_stop(srv);
    (void)_chttpsvr_take_stream_pool_threads_for_tests();
    _chttpsvr_arm_start_hold_for_tests();
    start_started =
        pthread_create(&start_tid, NULL, _rx_start_thread, &sctx) == 0;
    for (int i = 0; start_started && i < 5000 && !held; i++) {
      held = _chttpsvr_start_hold_entered_for_tests();
      if (!held) _sc_nap_ms(1);
    }
    if (held && _sc_send_str(fd,
                             "POST /gate HTTP/1.1\r\nHost: h\r\n"
                             "Content-Length: 0\r\n\r\n")) {
      for (int i = 0; i < 5000 && threads == 0; i++) {
        threads = _chttpsvr_take_stream_pool_threads_for_tests();
        if (threads == 0) _sc_nap_ms(1);
      }
    }
  }
  _chttpsvr_release_start_hold_for_tests();
  if (start_started) start_joined = _bounded_join(start_tid, NULL);
  if (threads != 0) gated = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (fd >= 0) close(fd);
  if (started && (start_joined || !start_started)) chttpsvr_destroy(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(hello, 200);
  REQUIRE_TRUE(start_started);
  REQUIRE_TRUE(start_joined);
  REQUIRE_TRUE(held);
  REQUIRE_EQ((int)sctx.rv, (int)ccol_success);
  REQUIRE_EQ(threads, 3);
  REQUIRE_EQ(gated, 200);
}

TEST(restart, a_streaming_pool_built_across_a_restart_uses_the_new_size) {
  /* The first streaming request of a run with one streaming thread is held
     after it has read that size and before it builds the pool, and a restart
     asking for three threads runs in that window. The pool the request then
     installs has three threads. A build that installed whatever it created, as
     long as the new run has no pool yet, would give the new run the first run's
     single thread. */
  _sc_gate_open(true);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 39, &cfg);
  bool started = srv != CHTTPSVR_INVALID;
  char buf[2048];
  int fd = started ? _sc_connect(_LC_PORT + 39) : -1;
  int hello = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool held = false;
  int threads = 0, gated = -1;
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  if (hello == 200) {
    (void)_chttpsvr_take_stream_pool_threads_for_tests();
    _chttpsvr_arm_stream_create_hold_for_tests();
    if (_sc_send_str(fd,
                     "POST /gate HTTP/1.1\r\nHost: h\r\n"
                     "Content-Length: 0\r\n\r\n")) {
      for (int i = 0; i < 5000 && !held; i++) {
        held = _chttpsvr_stream_create_hold_entered_for_tests();
        if (!held) _sc_nap_ms(1);
      }
    }
    if (held) {
      chttpsvr_stop(srv);
      chttpsvr_config_t cfg2 = cfg;
      cfg2.host = "127.0.0.1";
      cfg2.port = (uint16_t)(_LC_PORT + 40);
      cfg2.streaming_thread_count = 3;
      restart_rv = chttpsvr_start(srv, &cfg2);
    }
    _chttpsvr_release_stream_create_hold_for_tests();
    for (int i = 0; held && i < 5000 && threads == 0; i++) {
      threads = _chttpsvr_take_stream_pool_threads_for_tests();
      if (threads == 0) _sc_nap_ms(1);
    }
    if (held) gated = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  }
  _chttpsvr_release_stream_create_hold_for_tests();
  if (fd >= 0) close(fd);
  if (started) chttpsvr_destroy(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(hello, 200);
  REQUIRE_TRUE(held);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_EQ(threads, 3);
  REQUIRE_EQ(gated, 200);
}

TEST(restart, queued_streaming_requests_move_to_the_new_streaming_pool) {
  /* A run with one streaming thread holds one streaming request in its handler
     and two in the streaming queue when a restart asks for two threads. Once
     the held request ends, the queued ones run on the new run's streaming pool,
     which is created with two threads, and the retired pool stops taking
     requests and can drain and go away. If a task of the retired pool kept
     taking requests from the queue, it would run both queued requests itself,
     and no pool of the new run would ever be created. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 33, &cfg);
  bool started = srv != CHTTPSVR_INVALID;
  int fds[3] = {-1, -1, -1};
  bool sent = started;
  for (int i = 0; i < 3 && sent; i++) {
    fds[i] = _sc_connect(_LC_PORT + 33);
    sent = fds[i] >= 0 && _sc_send_str(fds[i],
                                       "POST /gate HTTP/1.1\r\nHost: h\r\n"
                                       "Content-Length: 0\r\n\r\n");
  }
  bool queued = false;
  for (int i = 0; sent && i < 5000 && !queued; i++) {
    queued = atomic_load(&g_sc_gate_runs) == 1 &&
             _chttpsvr_stream_queue_len_for_tests(srv) == 2;
    if (!queued) _sc_nap_ms(1);
  }
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  if (queued) {
    chttpsvr_stop(srv);
    (void)_chttpsvr_take_stream_pool_threads_for_tests();
    chttpsvr_config_t cfg2 = cfg;
    cfg2.host = "127.0.0.1";
    cfg2.port = (uint16_t)(_LC_PORT + 34);
    cfg2.streaming_thread_count = 2;
    restart_rv = chttpsvr_start(srv, &cfg2);
  }
  _sc_gate_open(true);
  int threads = 0;
  for (int i = 0; restart_rv == ccol_success && i < 5000 && threads == 0; i++) {
    threads = _chttpsvr_take_stream_pool_threads_for_tests();
    if (threads == 0) _sc_nap_ms(1);
  }
  int status[3] = {-1, -1, -1};
  char buf[2048];
  for (int i = 0; i < 3; i++)
    if (fds[i] >= 0)
      status[i] = _sc_read_response(fds[i], buf, sizeof(buf), NULL, 10000);
  for (int i = 0; i < 3; i++)
    if (fds[i] >= 0) close(fds[i]);
  if (started) chttpsvr_destroy(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(queued);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_EQ(threads, 2);
  for (int i = 0; i < 3; i++) REQUIRE_EQ(status[i], 200);
}

static void _rx_quick_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "quick");
}

TEST(restart, a_new_streaming_request_does_not_wait_for_an_old_one) {
  /* A run with one streaming thread holds one streaming request in its handler
     when a restart starts the next run. A streaming request of the new run is
     served immediately by that run's streaming pool, while the old run's
     request still holds the only thread of the old pool. If the count of busy
     streaming tasks spanned runs, the old request would fill it, no
     task of the new run would start, and the new request would wait in the
     queue until its 2 s deadline answers it with 503. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_timeout_us = 2000000;
  chttpsvr srv = _sc_server(_LC_PORT + 35, &cfg);
  bool started = srv != CHTTPSVR_INVALID;
  if (started)
    (void)chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/stream-quick",
                                              _rx_quick_stream_handler, NULL);
  int old_fd = started ? _sc_connect(_LC_PORT + 35) : -1;
  bool held = old_fd >= 0 && _sc_send_str(old_fd,
                                          "POST /gate HTTP/1.1\r\nHost: h\r\n"
                                          "Content-Length: 0\r\n\r\n");
  for (int i = 0; held && i < 5000 && atomic_load(&g_sc_gate_runs) == 0; i++)
    _sc_nap_ms(1);
  held = held && atomic_load(&g_sc_gate_runs) == 1;
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  if (held) {
    chttpsvr_stop(srv);
    chttpsvr_config_t cfg2 = cfg;
    cfg2.host = "127.0.0.1";
    cfg2.port = (uint16_t)(_LC_PORT + 36);
    restart_rv = chttpsvr_start(srv, &cfg2);
  }
  int new_status = -1;
  char buf[2048];
  if (restart_rv == ccol_success) {
    int new_fd = _sc_connect(_LC_PORT + 36);
    if (new_fd >= 0 && _sc_send_str(new_fd,
                                    "POST /stream-quick HTTP/1.1\r\nHost: h\r\n"
                                    "Content-Length: 0\r\n\r\n"))
      new_status = _sc_read_response(new_fd, buf, sizeof(buf), NULL, 1000);
    if (new_fd >= 0) close(new_fd);
  }
  _sc_gate_open(true);
  int old_status = old_fd >= 0
                       ? _sc_read_response(old_fd, buf, sizeof(buf), NULL, 5000)
                       : -1;
  if (old_fd >= 0) close(old_fd);
  if (started) chttpsvr_destroy(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(held);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_EQ(new_status, 200);
  REQUIRE_EQ(old_status, 200);
}

TEST(restart, a_queued_streaming_request_starts_on_the_new_run_at_once) {
  /* A run with one streaming thread holds one streaming request in its handler
     and one in the streaming queue when a restart starts the next run. The
     queued request starts on the new run's streaming pool at once, while the
     old request still holds the only thread of the old pool. If nothing
     started the new pool at the restart, the queued request would wait for
     the old handler to end. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_timeout_us = 30000000;
  chttpsvr srv = _sc_server(_LC_PORT + 37, &cfg);
  bool started = srv != CHTTPSVR_INVALID;
  if (started)
    (void)chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/stream-quick",
                                              _rx_quick_stream_handler, NULL);
  int old_fd = started ? _sc_connect(_LC_PORT + 37) : -1;
  bool held = old_fd >= 0 && _sc_send_str(old_fd,
                                          "POST /gate HTTP/1.1\r\nHost: h\r\n"
                                          "Content-Length: 0\r\n\r\n");
  for (int i = 0; held && i < 5000 && atomic_load(&g_sc_gate_runs) == 0; i++)
    _sc_nap_ms(1);
  held = held && atomic_load(&g_sc_gate_runs) == 1;
  int queued_fd = held ? _sc_connect(_LC_PORT + 37) : -1;
  bool queued = queued_fd >= 0 &&
                _sc_send_str(queued_fd,
                             "POST /stream-quick HTTP/1.1\r\nHost: h\r\n"
                             "Content-Length: 0\r\n\r\n");
  for (int i = 0;
       queued && i < 5000 && _chttpsvr_stream_queue_len_for_tests(srv) != 1;
       i++)
    _sc_nap_ms(1);
  queued = queued && _chttpsvr_stream_queue_len_for_tests(srv) == 1;
  ccol_retval_t restart_rv = ccol_unexpected_failure;
  if (queued) {
    chttpsvr_stop(srv);
    chttpsvr_config_t cfg2 = cfg;
    cfg2.host = "127.0.0.1";
    cfg2.port = (uint16_t)(_LC_PORT + 38);
    restart_rv = chttpsvr_start(srv, &cfg2);
  }
  char buf[2048];
  int queued_status =
      restart_rv == ccol_success
          ? _sc_read_response(queued_fd, buf, sizeof(buf), NULL, 1000)
          : -1;
  _sc_gate_open(true);
  int old_status = old_fd >= 0
                       ? _sc_read_response(old_fd, buf, sizeof(buf), NULL, 5000)
                       : -1;
  if (queued_fd >= 0) close(queued_fd);
  if (old_fd >= 0) close(old_fd);
  if (started) chttpsvr_destroy(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(queued);
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  REQUIRE_EQ(queued_status, 200);
  REQUIRE_EQ(old_status, 200);
}

/* ----- what a waiting connection keeps of its request ----- */

#define _FX_PORT (TEST_PORT + 260)

/* Reads the body and answers with _SC_BIG_RESPONSE bytes. */
static void _fx_body_then_big_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_big_handler(req, resp, ctx);
}

static chttpsvr _fx_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return CHTTPSVR_INVALID;
  chttpsvr_register_handler(srv, CHTTP_POST, "/upload-big/{id}",
                            _fx_body_then_big_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _sc_len_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _sc_hello_handler, NULL);
  chttpsvr_config_t cfg = extra ? *extra : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  return srv;
}

TEST(request_state, a_parked_response_keeps_nothing_of_its_request) {
  /* Three clients each upload 1 MiB, get a response far bigger than the socket
     buffers and read none of it, so the responses park on a full socket.
     Nothing reads the bodies, the targets or the header fields of those
     requests again, so none of the parked connections may keep them: a parked
     connection holds no thread, so their number isn't bounded by anything the
     body memory limit covers. A parked response that kept its request would
     hold the 1 MiB body buffer, the header fields and the target, and the state
     count would then be above 3 MiB. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  const size_t body_len = (size_t)1 << 20;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /upload-big/7?q=1 HTTP/1.1\r\nHost: h\r\n"
                    "X-A: 1\r\nContent-Length: %zu\r\n\r\n",
                    body_len);
  int fds[3];
  bool opened = true;
  for (int i = 0; i < 3; i++) {
    fds[i] = _sc_connect_small_rcvbuf(_FX_PORT);
    if (fds[i] < 0 || !_sc_send(fds[i], head, (size_t)hn) ||
        !_sc_send_fill(fds[i], body_len, 'u'))
      opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_writes, NULL, 3, 10000);
  size_t kept = _chttpsvr_parked_request_state_for_tests(srv);
  size_t mem = _chttpsvr_mem_in_use_for_tests(srv);

  /* Every response still arrives whole. */
  int whole = 0;
  char buf[4096];
  for (int i = 0; i < 3; i++) {
    if (fds[i] < 0) continue;
    size_t got = 0;
    if (_sc_read_response(fds[i], buf, sizeof(buf), &got, 20000) == 200 &&
        got == _SC_BIG_RESPONSE)
      whole++;
    close(fds[i]);
  }

  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(kept, (size_t)0);
  REQUIRE_EQ(mem, (size_t)0);
  REQUIRE_EQ(whole, 3);
}

TEST(request_state, a_lingering_connection_keeps_nothing_of_its_request) {
  /* A refused upload lingers, and the lingering connection must keep nothing
     of the request it refused. A linger that kept its request would hold the
     target and the header fields. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 1, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd = _sc_connect(_FX_PORT + 1);
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope?x=1", 1048576, 4096);
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  bool lingering =
      status == 404 && _sc_wait_for(srv, _lc_lingering, NULL, 1, 5000);
  size_t kept = _chttpsvr_parked_request_state_for_tests(srv);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_TRUE(lingering);
  REQUIRE_EQ(kept, (size_t)0);
}

TEST(request_state, a_lingering_connection_keeps_nothing_of_its_response) {
  /* A response that went out whole before a lingering close is never read
     again, so the lingering connection, which can wait for up to 2 s with no
     thread, must not keep its body or its header fields. The client sends bytes
     the server never reads, which makes the close linger. A linger that kept
     the response would hold its body buffer. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_FX_PORT + 15, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _sc_gate_open(false);
  int runs = atomic_load(&g_sc_gate_runs);
  int fd = _sc_connect(_FX_PORT + 15);
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "GET /busy HTTP/1.1\r\nHost: h\r\n"
                                      "Connection: close\r\n\r\n");
  bool entered = false;
  for (int i = 0; sent && i < 5000 && !entered; i++) {
    entered = atomic_load(&g_sc_gate_runs) > runs;
    if (!entered) _sc_nap_ms(1);
  }
  bool extra = entered && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nX-Pad: a");
  _sc_gate_open(true);
  char buf[2048] = {0};
  int status = extra ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  bool lingering =
      status == 200 && _sc_wait_for(srv, _lc_lingering, NULL, 1, 5000);
  size_t kept = _chttpsvr_parked_request_state_for_tests(srv);
  _sc_gate_open(false);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(extra);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(lingering);
  REQUIRE_EQ(kept, (size_t)0);
}

/* ----- body buffers hold no more than their reservation ----- */

TEST(body_memory, a_framed_body_reserves_all_that_its_buffer_holds) {
  /* A body of 70000 declared bytes reserves its buffer as the buffer grows,
     before the read that needs it, and while a growth copies the reservation
     covers the old block and the new one together. Its buffer must never hold
     more than that reservation, and never grows past the declared length. The
     client stops 4000 bytes short, so the body parks with its buffer in place,
     and then sends the rest. A body that read its bytes before reserving the
     growth they need would hold 65536 + 70000 bytes during its last growth
     against a smaller reservation. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 2, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_body_overshoot_reset_for_tests();
  int fd = _sc_open_partial_post(_FX_PORT + 2, "/len", 70000, 66000, NULL);
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t over_parked = _chttpsvr_body_overshoot_for_tests();
  char buf[4096];
  int status = -1;
  if (parked && _sc_send_fill(fd, 4000, 'r'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n70000") != NULL;
  if (fd >= 0) close(fd);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(over_parked, (size_t)0);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(_chttpsvr_body_overshoot_for_tests(), (size_t)0);
  REQUIRE_TRUE(mem_zero);
}

TEST(body_memory, a_chunked_body_reserves_all_that_its_buffer_holds) {
  /* A chunked body of 300000 bytes grows its buffer as it arrives. At every
     growth its body memory reservation has to cover the old block and the new
     one together, since both live until the copy ends. A body that reserved
     64 KiB at a time while its buffer doubles would hold 131072 + 262144 bytes
     against a reservation of 196608. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 3, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_body_overshoot_reset_for_tests();
  int fd = _sc_connect(_FX_PORT + 3);
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "POST /len HTTP/1.1\r\nHost: h\r\n"
                                      "Transfer-Encoding: chunked\r\n\r\n");
  char chunk[10000 + 16];
  int hn = snprintf(chunk, sizeof(chunk), "%x\r\n", 10000);
  memset(chunk + hn, 'c', 10000);
  memcpy(chunk + hn + 10000, "\r\n", 2);
  for (int i = 0; sent && i < 30; i++) {
    sent = _sc_send(fd, chunk, (size_t)hn + 10000 + 2);
    /* A pause now and then makes the server read the body in several parts,
       across several growths of its buffer. */
    if (i % 5 == 4) _sc_nap_ms(5);
  }
  if (sent) sent = _sc_send_str(fd, "0\r\n\r\n");
  char buf[4096];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n300000") != NULL;
  if (fd >= 0) close(fd);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(_chttpsvr_body_overshoot_for_tests(), (size_t)0);
  REQUIRE_TRUE(mem_zero);
}

/* ----- the writes of the library itself never wait on the peer ----- */

#define _FX_BIG_REJECT ((size_t)8 * 1024 * 1024)

/* Answers every request under /big-reject itself with a 404 far bigger than the
   socket buffers, whose bytes ctx holds, and passes every other request on. It
   runs for a rejected request, on reject_pool. */
static void _fx_big_reject_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                              chttpsvr_next_fn next) {
  const char *path = chttpsvr_req_path(req);
  if (path && strncmp(path, "/big-reject", 11) == 0) {
    chttpsvr_resp_set_status(resp, 404);
    chttpsvr_resp_write(resp, ctx, _FX_BIG_REJECT);
    return;
  }
  next(req, resp);
}

TEST(internal_writes, a_rejection_to_a_client_that_does_not_read_parks) {
  /* Two clients get a 404 whose body a middleware made far bigger than the
     socket buffers, and read none of it. The rejection parks with the
     unsent rest, holding no reject_pool thread, and then arrive whole once the
     client reads, followed by the end of the stream. A rejection that waited on
     the socket would hold a reject_pool thread until its per-write timeout or
     its total ceiling gives up, so no rejection would ever park and the clients
     would never get the whole body. The ceiling of the library's own writes is
     raised for this test, which is about the park and not about how long a
     rejection may take, because under valgrind two such bodies take more than 2
     s to cross a small receive window. */
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) free(big);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_set_internal_write_ceiling_ms_for_tests(120000);
  chttpsvr_use(srv, _fx_big_reject_mw, big);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _sc_hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_FX_PORT + 4);
  cfg.worker_thread_count = 2;
  bool started = chttpsvr_start(srv, &cfg) == ccol_success;
  int fds[2] = {-1, -1};
  bool opened = started;
  for (int i = 0; opened && i < 2; i++) {
    fds[i] = _sc_connect_small_rcvbuf(_FX_PORT + 4);
    if (fds[i] < 0 ||
        !_sc_send_str(fds[i], "GET /big-reject HTTP/1.1\r\nHost: h\r\n\r\n"))
      opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_writes, NULL, 2, 10000);
  int whole = 0;
  char buf[4096];
  for (int i = 0; i < 2; i++) {
    if (fds[i] < 0) continue;
    size_t got = 0;
    if (parked &&
        _sc_read_response(fds[i], buf, sizeof(buf), &got, 20000) == 404 &&
        got == _FX_BIG_REJECT && _lc_stream_end(fds[i], 5000) == 0)
      whole++;
    close(fds[i]);
  }
  bool none_parked =
      started && _sc_wait_for_zero(srv, _sc_parked_writes, NULL, 5000);
  chttpsvr_destroy(srv);
  free(big);
  _chttpsvr_set_internal_write_ceiling_ms_for_tests(0);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(whole, 2);
  REQUIRE_TRUE(none_parked);
}

TEST(internal_writes, a_parked_rejection_ends_at_its_ceiling) {
  /* A client that never reads holds a parked rejection only until the ceiling
     of the library's internal writes (2 s), even with
     max_response_write_duration turned off. A parked rejection with no
     deadline would stay parked. */
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) free(big);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_use(srv, _fx_big_reject_mw, big);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_FX_PORT + 5);
  cfg.max_response_write_duration_us = 0;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  bool started = chttpsvr_start(srv, &cfg) == ccol_success;
  int fd = started ? _sc_connect_small_rcvbuf(_FX_PORT + 5) : -1;
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "GET /big-reject HTTP/1.1\r\n"
                                      "Host: h\r\n\r\n");
  bool parked = sent && _sc_wait_for(srv, _sc_parked_writes, NULL, 1, 10000);
  /* Past the ceiling, one sweep tick closes it. */
  if (parked) _sc_nap_ms(2200);
  if (parked) _chttpsvr_sweep_now_for_tests(srv);
  size_t still = started ? _chttpsvr_parked_count_for_tests(srv, 2) : 1;
  if (fd >= 0) close(fd);
  chttpsvr_destroy(srv);
  free(big);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(still, (size_t)0);
}

TEST(internal_writes, a_short_bare_rejection_parks_and_completes) {
  /* A bare 404 whose write stops after 10 bytes on a full socket parks and then
     goes out whole, followed by the end of the stream. That's the write of
     every rejection, whatever thread runs it. A rejection written with waiting
     writes would never take the forced stop, and the count of parked writes
     wouldn't move. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 6, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t parks = _chttpsvr_park_kind_count_for_tests(2);
  int fd = _sc_connect(_FX_PORT + 6);
  _chttpsvr_force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = _chttpsvr_park_kind_count_for_tests(2) - parks;
  _chttpsvr_force_short_response_write_for_tests(0);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_GE(parked, (size_t)1);
}

TEST(internal_writes, an_interim_line_on_a_full_socket_parks_and_completes) {
  /* The "100 Continue" line of a buffered route stops after 5 bytes on a full
     socket. The connection parks with the rest of the line and holds no thread,
     and once the line is out the body is read and the request ends normally.
     An interim line written with waiting writes would never take the forced
     stop, and the count of parked writes wouldn't move. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 7, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t parks = _chttpsvr_park_kind_count_for_tests(2);
  _chttpsvr_force_interim_park_after_for_tests(5);
  int fd = _sc_connect(_FX_PORT + 7);
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "POST /len HTTP/1.1\r\nHost: h\r\n"
                                      "Expect: 100-continue\r\n"
                                      "Content-Length: 10\r\n\r\n");
  char line[64] = {0};
  size_t got = 0;
  long long deadline = _sc_now_ms() + 5000;
  while (sent && got < 25 && _sc_now_ms() < deadline) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, 100) <= 0) continue;
    ssize_t r = read(fd, line + got, 25 - got);
    if (r <= 0) break;
    got += (size_t)r;
  }
  bool interim_ok =
      got == 25 && memcmp(line, "HTTP/1.1 100 Continue\r\n\r\n", 25) == 0;
  size_t parked = _chttpsvr_park_kind_count_for_tests(2) - parks;
  char buf[2048];
  int status = -1;
  if (interim_ok && _sc_send_str(fd, "0123456789"))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n10") != NULL;
  /* The connection stays usable. */
  int again = -1;
  if (len_ok && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    again = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  _chttpsvr_force_interim_park_after_for_tests((size_t)-1);
  if (fd >= 0) close(fd);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(interim_ok);
  REQUIRE_GE(parked, (size_t)1);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(again, 200);
  REQUIRE_TRUE(mem_zero);
}

/* ----- the identity of a client over plaintext ----- */

static void _fx_identity_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  size_t len = 7;
  const void *der = chttpsvr_req_peer_cert_der(req, &len);
  unsigned char sha[CHTTPSVR_PEER_CERT_SHA256_LEN];
  memset(sha, 0xAB, sizeof(sha));
  ccol_retval_t rv = chttpsvr_req_peer_cert_sha256(req, sha);
  int zero = 1;
  for (int i = 0; i < CHTTPSVR_PEER_CERT_SHA256_LEN; i++)
    zero = zero && sha[i] == 0;
  const char *subj = chttpsvr_req_peer_cert_subject(req);
  chttpsvr_resp_printf(resp, "der=%d len=%zu rv=%d zero=%d subj=%d",
                       der != NULL, len, (int)rv, zero, subj != NULL);
}

TEST(request_state, a_plaintext_request_has_no_client_identity) {
  /* Over plaintext there's no client certificate, and every identity accessor
     says so: no DER, no subject, ccol_key_not_found and a digest buffer left
     all zeroes. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/id",
                                       _fx_identity_handler, NULL),
             ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_FX_PORT + 8);
  REQUIRE_EQ(chttpsvr_start(srv, &cfg), ccol_success);
  int fd = _sc_connect(_FX_PORT + 8);
  char buf[2048] = {0};
  int status = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /id HTTP/1.1\r\nHost: h\r\n\r\n"))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (fd >= 0) close(fd);
  char want[128];
  snprintf(want, sizeof(want), "\r\n\r\nder=0 len=0 rv=%d zero=1 subj=0",
           (int)ccol_key_not_found);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, want) != NULL);
}

TEST(internal_writes, an_inline_rejection_on_the_reactor_parks_and_completes) {
  /* The inline answer a rejection gets when reject_pool can't take it runs on
     the reactor thread, for a route that doesn't match. Its write stops after
     10 bytes on a full socket, and it should park, so that the reactor never
     waits on the peer, and then go out whole, followed by the end of the
     stream. An ordinary request on another connection gets served meanwhile.
     If the rejection were written with waiting writes, it would never take the
     forced stop and the count of parked writes wouldn't move. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_FX_PORT + 9, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t parks = _chttpsvr_park_kind_count_for_tests(2);
  size_t rejects = _chttpsvr_reject_pool_task_count_for_tests();
  int fd = _sc_connect(_FX_PORT + 9);
  _chttpsvr_force_reject_inline_for_tests(true);
  _chttpsvr_force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = _chttpsvr_park_kind_count_for_tests(2) - parks;
  size_t on_pool = _chttpsvr_reject_pool_task_count_for_tests() - rejects;
  _chttpsvr_force_short_response_write_for_tests(0);
  _chttpsvr_force_reject_inline_for_tests(false);
  if (fd >= 0) close(fd);
  int hello = -1;
  int fd2 = _sc_connect(_FX_PORT + 9);
  if (fd2 >= 0 && _sc_send_str(fd2, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd2, buf, sizeof(buf), NULL, 5000);
  if (fd2 >= 0) close(fd2);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_GE(parked, (size_t)1);
  REQUIRE_EQ(on_pool, (size_t)0);
  REQUIRE_EQ(hello, 200);
}

/* ----- a registration that a pause could not keep ----- */

#define _PF_PORT (TEST_PORT + 280)

/* Each test of this group makes every pause through which a thread takes a
   connection from the reactor fail. The connection then loses its registration,
   and its next wait (a parked write, a parked body, a lingering close or the
   next request of a keep-alive connection) adds a fresh registration for the
   same descriptor while the old one's on_removed may still be pending. Each
   test checks through the count of those removals that the path really ran. */

/* Connects to port and waits, for at most 5 s, until the server has registered
   the connection with its reactor and holds it idle, so that its first request
   arrives through a registration and not through the first read after the
   accept. */
static int _pf_connect_registered(chttpsvr srv, int port) {
  int fd = _sc_connect(port);
  if (fd >= 0 && !_sc_wait_for(srv, _rx_idle, NULL, 1, 5000)) {
    close(fd);
    fd = -1;
  }
  return fd;
}

TEST(pause_fallback, a_rejection_that_parks_completes) {
  /* A bare 404 whose write stops on a full socket parks with a fresh
     registration and then goes out whole, followed by the end of the stream.
     A fallback that kept the removed registration couldn't arm the parked
     write, and the response would stop after 10 bytes. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_PF_PORT + 0, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t parks = _chttpsvr_park_kind_count_for_tests(2);
  size_t fallbacks = _chttpsvr_pause_fallback_count_for_tests();
  int fd = _pf_connect_registered(srv, _PF_PORT + 0);
  _chttpsvr_force_pause_fail_for_tests(true);
  _chttpsvr_force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = _chttpsvr_park_kind_count_for_tests(2) - parks;
  _chttpsvr_force_short_response_write_for_tests(0);
  _chttpsvr_force_pause_fail_for_tests(false);
  size_t removed = _chttpsvr_pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_GE(parked, (size_t)1);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_refused_upload_lingers_and_ends_in_order) {
  /* A refused upload lingers on a fresh registration, so the client reads the
     404 and then an orderly end of stream. A fallback that kept the removed
     registration couldn't arm the linger, and the connection would close with
     the body unread, which the client sees as a reset. */
  chttpsvr srv = _lc_server("127.0.0.1", _PF_PORT + 1, NULL);
  size_t lingers = _chttpsvr_linger_count_for_tests();
  size_t fallbacks = _chttpsvr_pause_fallback_count_for_tests();
  _chttpsvr_force_pause_fail_for_tests(true);
  int fd =
      srv != CHTTPSVR_INVALID ? _pf_connect_registered(srv, _PF_PORT + 1) : -1;
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope", 1048576, 32768);
  char buf[2048] = {0};
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  _chttpsvr_force_pause_fail_for_tests(false);
  size_t removed = _chttpsvr_pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  bool drained = srv != CHTTPSVR_INVALID &&
                 _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
  REQUIRE_TRUE(drained);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_keep_alive_connection_serves_its_next_requests) {
  /* A keep-alive connection serves a request and then a refused one on fresh
     registrations, and a second connection is served next to it. Keeping the
     removed registration in the fallback would make the keep-alive tail fail
     its modify and close the connection, so the second request would get no
     answer. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_PF_PORT + 2, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t fallbacks = _chttpsvr_pause_fallback_count_for_tests();
  _chttpsvr_force_pause_fail_for_tests(true);
  int fd = _pf_connect_registered(srv, _PF_PORT + 2);
  char buf[2048];
  int s1 = -1, s2 = -1, s3 = -1, s4 = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    s1 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (s1 == 200 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    s2 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (s2 == 200 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n"))
    s3 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  int fd2 = _sc_connect(_PF_PORT + 2);
  if (fd2 >= 0 && _sc_send_str(fd2, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    s4 = _sc_read_response(fd2, buf, sizeof(buf), NULL, 5000);
  _chttpsvr_force_pause_fail_for_tests(false);
  size_t removed = _chttpsvr_pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  if (fd2 >= 0) close(fd2);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(s2, 200);
  REQUIRE_EQ(s3, 404);
  REQUIRE_EQ(s4, 200);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_parked_body_resumes_and_completes) {
  /* A body that stops halfway parks, resumes on readiness, loses its
     registration again as the worker takes it, and completes, and then the
     connection serves its next request. A fallback that kept the removed
     registration would make the park or the keep-alive tail fail and close the
     connection. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _fx_server(_PF_PORT + 3, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  size_t fallbacks = _chttpsvr_pause_fallback_count_for_tests();
  _chttpsvr_force_pause_fail_for_tests(true);
  int fd = _sc_open_partial_post(_PF_PORT + 3, "/len", 20000, 100, NULL);
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  char buf[2048];
  int status = -1;
  if (parked && _sc_send_fill(fd, 20000 - 100, 'r'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n20000") != NULL;
  int again = -1;
  if (len_ok && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    again = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  _chttpsvr_force_pause_fail_for_tests(false);
  size_t removed = _chttpsvr_pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(again, 200);
  REQUIRE_GE(removed, (size_t)2);
  REQUIRE_TRUE(mem_zero);
}

/* ----- the reservation of body memory lasts as long as the body ----- */

#define _BM_PORT (TEST_PORT + 300)

/* What _bm_probe_handler saw while it ran: its own request's reservation, its
   body buffer's capacity, and the reservations of the whole server. */
static _Atomic uint64_t g_bm_srv = 0;
static _Atomic size_t g_bm_charged = 0;
static _Atomic size_t g_bm_held = 0;
static _Atomic size_t g_bm_in_use = 0;

static void _bm_probe_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  size_t charged = 0, held = 0;
  _chttpsvr_req_body_memory_for_tests(req, &charged, &held);
  atomic_store(&g_bm_charged, charged);
  atomic_store(&g_bm_held, held);
  atomic_store(&g_bm_in_use, _chttpsvr_mem_in_use_for_tests(
                                 (chttpsvr)atomic_load(&g_bm_srv)));
  chttpsvr_resp_printf(resp, "%zu", n);
}

/* Reads the body, then holds on to its thread at the gate of the slow-client
   tests. */
static void _bm_hold_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_gate_stream_handler(req, resp, ctx);
}

static chttpsvr _bm_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return CHTTPSVR_INVALID;
  chttpsvr_register_handler(srv, CHTTP_POST, "/probe", _bm_probe_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/hold", _bm_hold_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _sc_len_handler, NULL);
  chttpsvr_config_t cfg = extra ? *extra : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_INVALID;
  }
  atomic_store(&g_bm_srv, (uint64_t)srv);
  return srv;
}

/* Sends the head of a POST to path with "Expect: 100-continue", either with a
   Content-Length of len or chunked, waits for the interim line, and then sends
   a body of len bytes, so the body never arrives with the head and its memory
   is reserved before the server reads it. Returns the final status, with the
   response in buf, or -1. */
static int _bm_post_after_continue(int fd, const char *path, size_t len,
                                   bool chunked, char *buf, size_t cap) {
  char head[256];
  int hn;
  if (chunked)
    hn = snprintf(head, sizeof(head),
                  "POST %s HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                  "Transfer-Encoding: chunked\r\n\r\n",
                  path);
  else
    hn = snprintf(head, sizeof(head),
                  "POST %s HTTP/1.1\r\nHost: h\r\nExpect: 100-continue\r\n"
                  "Content-Length: %zu\r\n\r\n",
                  path, len);
  if (!_sc_send(fd, head, (size_t)hn)) return -1;
  if (_sc_read_response(fd, buf, cap, NULL, 5000) != 100) return -1;
  bool ok = true;
  if (chunked) {
    char size_line[32];
    int sn = snprintf(size_line, sizeof(size_line), "%zx\r\n", len);
    ok = _sc_send(fd, size_line, (size_t)sn) && _sc_send_fill(fd, len, 'q') &&
         _sc_send_str(fd, "\r\n0\r\n\r\n");
  } else {
    ok = _sc_send_fill(fd, len, 'q');
  }
  return ok ? _sc_read_response(fd, buf, cap, NULL, 5000) : -1;
}

/* No reservation, no holder and no exemption are left. */
static bool _bm_nothing_reserved(chttpsvr srv) {
  if (!_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000)) return false;
  bool exempt = true;
  size_t holders = _chttpsvr_mem_holders_for_tests(srv, &exempt);
  return holders == 0 && !exempt;
}

TEST(body_memory, a_running_handler_holds_exactly_the_memory_of_its_body) {
  /* While a buffered route's handler runs its body is still held, and the body
     memory reservation covers exactly the buffer holding it, both for a body
     framed by a Content-Length and for a chunked one. Once the response is out,
     nothing stays reserved. A reservation that went back when the body
     completed would leave the handler with a reservation of 0 next to a body
     buffer of tens of kilobytes. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 1024 * 1024;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 0, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int fd _ccol_destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 0);
  REQUIRE_TRUE(fd >= 0);
  char buf[4096];
  int framed =
      _bm_post_after_continue(fd, "/probe", 50000, false, buf, sizeof(buf));
  bool framed_len = framed == 200 && strstr(buf, "\r\n\r\n50000") != NULL;
  size_t f_charged = atomic_load(&g_bm_charged);
  size_t f_held = atomic_load(&g_bm_held);
  size_t f_in_use = atomic_load(&g_bm_in_use);
  bool f_none = _bm_nothing_reserved(srv);
  int chunked =
      _bm_post_after_continue(fd, "/probe", 30000, true, buf, sizeof(buf));
  bool chunked_len = chunked == 200 && strstr(buf, "\r\n\r\n30000") != NULL;
  size_t c_charged = atomic_load(&g_bm_charged);
  size_t c_held = atomic_load(&g_bm_held);
  size_t c_in_use = atomic_load(&g_bm_in_use);
  bool c_none = _bm_nothing_reserved(srv);
  REQUIRE_EQ(framed, 200);
  REQUIRE_TRUE(framed_len);
  REQUIRE_EQ(f_held, (size_t)50000);
  REQUIRE_EQ(f_charged, f_held);
  REQUIRE_EQ(f_in_use, f_held);
  REQUIRE_TRUE(f_none);
  REQUIRE_EQ(chunked, 200);
  REQUIRE_TRUE(chunked_len);
  REQUIRE_GE(c_held, (size_t)30000);
  REQUIRE_EQ(c_charged, c_held);
  REQUIRE_EQ(c_in_use, c_held);
  REQUIRE_TRUE(c_none);
}

TEST(body_memory, a_body_waits_while_a_handler_holds_the_limit) {
  /* The handler of a first request holds a body of 90000 bytes against a limit
     of 100000 and doesn't return. A second body of 50000 bytes then waits for
     memory, getting no "100 Continue" and no worker reading it. Once the
     handler returns and its body is freed, the second body is admitted,
     invited, read and answered. If the reservation went back when the first
     body completed, the second one would be admitted at once, while the first
     body is still held. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 1, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  int a _ccol_destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 1);
  int b _ccol_destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 1);
  bool opened = a >= 0 && b >= 0;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /hold HTTP/1.1\r\nHost: h\r\n"
                    "Expect: 100-continue\r\nContent-Length: 90000\r\n\r\n");
  char buf[4096];
  bool a_sent = opened && _sc_send(a, head, (size_t)hn) &&
                _sc_read_response(a, buf, sizeof(buf), NULL, 5000) == 100 &&
                _sc_send_fill(a, 90000, 'a');
  /* a's handler runs, with its body read whole. */
  long long deadline = _sc_now_ms() + 5000;
  while (a_sent && atomic_load(&g_sc_gate_runs) < 1 && _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  bool a_running = atomic_load(&g_sc_gate_runs) == 1;
  size_t held_by_a = _chttpsvr_mem_in_use_for_tests(srv);
  hn = snprintf(head, sizeof(head),
                "POST /len HTTP/1.1\r\nHost: h\r\n"
                "Expect: 100-continue\r\nContent-Length: 50000\r\n\r\n");
  bool b_sent = a_running && _sc_send(b, head, (size_t)hn);
  bool b_waits = b_sent && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  struct pollfd p = {.fd = b, .events = POLLIN};
  int b_invited_early = b_sent ? poll(&p, 1, 50) : -1;
  _sc_gate_open(true);
  int a_status =
      a_sent ? _sc_read_response(a, buf, sizeof(buf), NULL, 5000) : -1;
  int interim =
      b_sent ? _sc_read_response(b, buf, sizeof(buf), NULL, 5000) : -1;
  int b_status = -1;
  if (interim == 100 && _sc_send_fill(b, 50000, 'b'))
    b_status = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  bool b_len = b_status == 200 && strstr(buf, "\r\n\r\n50000") != NULL;
  bool none = _bm_nothing_reserved(srv);
  _sc_gate_open(false);
  REQUIRE_TRUE(a_sent);
  REQUIRE_TRUE(a_running);
  REQUIRE_EQ(held_by_a, (size_t)90000);
  REQUIRE_TRUE(b_waits);
  REQUIRE_EQ(b_invited_early, 0);
  REQUIRE_EQ(a_status, 200);
  REQUIRE_EQ(interim, 100);
  REQUIRE_EQ(b_status, 200);
  REQUIRE_TRUE(b_len);
  REQUIRE_TRUE(none);
}

TEST(body_memory, no_path_that_ends_a_body_keeps_its_reservation) {
  /* Every way a body holding a body memory reservation can end gives the whole
     reservation back and leaves no holder and no exemption behind: a client
     that leaves in the middle of its body, a rate floor that answers 408, a
     chunked body over max_body_size (413), a malformed chunk (400), a client
     that leaves while the handler holds its body, and a chunked body whose wait
     for more memory ends in 503 while it already holds part of its reservation.
     A release that didn't run on one of these paths would leave that
     reservation counted, and the check after that path would fail. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 65536;
  cfg.max_body_size = 100000;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 2, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  char buf[4096];

  /* 1. The client leaves in the middle of a parked body. */
  int fd = _sc_open_partial_post(_BM_PORT + 2, "/len", 8000, 100, NULL);
  bool parked1 = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged1 = _chttpsvr_mem_in_use_for_tests(srv);
  if (fd >= 0) close(fd);
  bool none1 = _bm_nothing_reserved(srv);

  /* 2. The rate floor answers a parked body with 408. */
  fd = _sc_open_partial_post(_BM_PORT + 2, "/len", 8000, 100, NULL);
  bool parked2 = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged2 = _chttpsvr_mem_in_use_for_tests(srv);
  int s2 = -1;
  if (parked2) {
    _chttpsvr_advance_slow_clock_for_tests(60000);
    _chttpsvr_sweep_now_for_tests(srv);
    s2 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  }
  bool none2 = _bm_nothing_reserved(srv);
  if (fd >= 0) close(fd);

  /* 3. A chunked body over max_body_size gets 413. */
  fd = _sc_connect(_BM_PORT + 2);
  int s3 = fd >= 0 ? _bm_post_after_continue(fd, "/len", 150000, true, buf,
                                             sizeof(buf))
                   : -1;
  if (fd >= 0) close(fd);
  bool none3 = _bm_nothing_reserved(srv);

  /* 4. A malformed chunk gets 400. */
  fd = _sc_connect(_BM_PORT + 2);
  int s4 = -1;
  if (fd >= 0 &&
      _sc_send_str(fd,
                   "POST /len HTTP/1.1\r\nHost: h\r\nExpect: "
                   "100-continue\r\nTransfer-Encoding: chunked\r\n\r\n") &&
      _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_str(fd, "zz\r\nabc\r\n0\r\n\r\n"))
    s4 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  if (fd >= 0) close(fd);
  bool none4 = _bm_nothing_reserved(srv);

  /* 5. The client leaves while the handler holds its body. */
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  fd = _sc_connect(_BM_PORT + 2);
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /hold HTTP/1.1\r\nHost: h\r\n"
                    "Expect: 100-continue\r\nContent-Length: 30000\r\n\r\n");
  bool sent5 = fd >= 0 && _sc_send(fd, head, (size_t)hn) &&
               _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 100 &&
               _sc_send_fill(fd, 30000, 'h');
  long long deadline = _sc_now_ms() + 5000;
  while (sent5 && atomic_load(&g_sc_gate_runs) < 1 && _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  size_t charged5 = _chttpsvr_mem_in_use_for_tests(srv);
  if (fd >= 0) close(fd);
  _sc_gate_open(true);
  bool none5 = _bm_nothing_reserved(srv);
  _sc_gate_open(false);

  REQUIRE_TRUE(parked1);
  REQUIRE_EQ(charged1, (size_t)8000);
  REQUIRE_TRUE(none1);
  REQUIRE_TRUE(parked2);
  REQUIRE_EQ(charged2, (size_t)8000);
  REQUIRE_EQ(s2, 408);
  REQUIRE_TRUE(none2);
  REQUIRE_EQ(s3, 413);
  REQUIRE_TRUE(none3);
  REQUIRE_EQ(s4, 400);
  REQUIRE_TRUE(none4);
  REQUIRE_TRUE(sent5);
  REQUIRE_EQ(charged5, (size_t)30000);
  REQUIRE_TRUE(none5);
}

TEST(body_memory, a_chunked_wait_that_ends_in_503_gives_its_part_back) {
  /* A body holding 16384 bytes takes part of a 65536 limit. A chunked body then
     reserves the first growths of its buffer, needs more than the rest of the
     limit, and waits for memory while holding part of it. The sweep answers it
     with 503 at the deadline of the wait, and only the first body's reservation
     is left. A 503 path that didn't give back the waiter's part would leave it
     counted next to the 16384 bytes. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 65536;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  /* the 31 s below must not expire the first body */
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 3, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int h _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_BM_PORT + 3, "/len", _SC_HOLD_LEN, 100, NULL);
  bool parked = h >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  int c _ccol_destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 3);
  char buf[4096];
  bool c_sent =
      parked && c >= 0 &&
      _sc_send_str(c,
                   "POST /len HTTP/1.1\r\nHost: h\r\nExpect: "
                   "100-continue\r\nTransfer-Encoding: chunked\r\n\r\n") &&
      _sc_read_response(c, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_str(c, "9c40\r\n") && _sc_send_fill(c, 40000, 'c');
  bool waits = c_sent && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  size_t in_use_waiting = _chttpsvr_mem_in_use_for_tests(srv);
  int status = -1;
  if (waits) {
    _chttpsvr_advance_slow_clock_for_tests(31000);
    _chttpsvr_sweep_now_for_tests(srv);
    status = _sc_read_response(c, buf, sizeof(buf), NULL, 5000);
  }
  long long deadline = _sc_now_ms() + 5000;
  while (_chttpsvr_mem_in_use_for_tests(srv) != _SC_HOLD_LEN &&
         _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  size_t in_use_after = _chttpsvr_mem_in_use_for_tests(srv);
  size_t holders_after = _chttpsvr_mem_holders_for_tests(srv, NULL);
  int h_status = -1;
  if (parked && _sc_send_fill(h, _SC_HOLD_LEN - 100, 'h'))
    h_status = _sc_read_response(h, buf, sizeof(buf), NULL, 5000);
  bool none = _bm_nothing_reserved(srv);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(c_sent);
  REQUIRE_TRUE(waits);
  REQUIRE_GT(in_use_waiting, _SC_HOLD_LEN);
  REQUIRE_EQ(status, 503);
  REQUIRE_EQ(in_use_after, _SC_HOLD_LEN);
  REQUIRE_EQ(holders_after, (size_t)1);
  REQUIRE_EQ(h_status, 200);
  REQUIRE_TRUE(none);
}

TEST(body_memory, bodies_that_trickle_hold_only_what_arrived) {
  /* Thirty clients each declare a 4 MiB body and send 1000 bytes of it, and
     then an honest client uploads 200000 bytes, against a limit of 1 MiB. A
     body reserves the buffer holding what arrived plus one read window, so each
     of the thirty holds 16384 bytes, and the upload is read and answered right
     away. No reservation ever goes past the limit. A body that reserved its
     whole declared length before its first byte would let the first client
     take the whole limit, the other clients and the upload would wait for
     memory, and the upload would get 503 at the 2 s deadline of its wait. */
  enum { trickles = 30 };
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 1024 * 1024;
  cfg.max_body_size = 8 * 1024 * 1024;
  cfg.body_memory_wait_timeout_us = 2000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 4, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  _chttpsvr_mem_peak_reset_for_tests();
  int fds[trickles];
  bool opened = true;
  for (int i = 0; i < trickles; i++) {
    fds[i] = _sc_open_partial_post(_BM_PORT + 4, "/len", 4 * 1024 * 1024, 1000,
                                   NULL);
    if (fds[i] < 0) opened = false;
  }
  bool all_parked =
      opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, trickles, 5000);
  size_t held = _chttpsvr_mem_in_use_for_tests(srv);
  long long t0 = _sc_now_ms();
  int up = all_parked ? _sc_open_partial_post(_BM_PORT + 4, "/len", 200000,
                                              200000, NULL)
                      : -1;
  char buf[4096];
  int status =
      up >= 0 ? _sc_read_response(up, buf, sizeof(buf), NULL, 5000) : -1;
  long long took = _sc_now_ms() - t0;
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n200000") != NULL;
  size_t peak = _chttpsvr_mem_peak_for_tests();
  if (up >= 0) close(up);
  _sc_close_all(fds, trickles);
  bool none = _bm_nothing_reserved(srv);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(all_parked);
  REQUIRE_EQ(held, (size_t)trickles * 16384);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_LT(took, 2000LL);
  REQUIRE_LE(peak, (size_t)(1024 * 1024));
  REQUIRE_TRUE(none);
}

/* ========================================================================== */
/*          A TEARDOWN STOPS TAKING REQUESTS FROM KEEP-ALIVE CLIENTS          */
/* ========================================================================== */

static _Atomic int g_queued_drain_entries = 0;
static _Atomic bool g_queued_drain_release = false;

/* The first request waits here until the test releases it, and any later one
 * returns at once. */
static void _queued_drain_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)req;
  (void)ctx;
  if (atomic_fetch_add(&g_queued_drain_entries, 1) == 0) {
    while (!atomic_load(&g_queued_drain_release)) {
      struct timespec nap = {0, 1000000L};
      nanosleep(&nap, NULL);
    }
  }
  chttpsvr_resp_write_str(resp, "done");
}

typedef struct {
  const char *sock_path;
  int status; /* the status code of the response, or -1 */
} _queued_drain_client_t;

static void *_destroy_srv_thread_fn(void *arg) {
  chttpsvr *srv = (chttpsvr *)arg;
  chttpsvr_destroy(*srv);
  return NULL;
}

static void *_queued_drain_client_fn(void *arg) {
  _queued_drain_client_t *c = (_queued_drain_client_t *)arg;
  c->status = -1;
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  struct timeval tv = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, c->sock_path, sizeof(addr.sun_path) - 1);
  if (test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
    static const char req[] = "GET /queued HTTP/1.1\r\nHost: x\r\n\r\n";
    if (write(fd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1)) {
      char buf[1024];
      ssize_t r = read(fd, buf, sizeof(buf) - 1);
      if (r > 12) {
        buf[r] = '\0';
        c->status = atoi(buf + 9);
      }
    }
  }
  close(fd);
  return NULL;
}

/* A request waiting in the worker pool's queue when the teardown starts runs no
 * handler, it gets 503. One worker holds the first request in its handler, the
 * second request waits in the queue behind it, and the teardown starts before
 * the first handler returns. Without the draining-state check when a worker
 * task starts, the second handler would run after chttpsvr_destroy() was called
 * and the client would get 200, and with fewer workers than active requests the
 * teardown's wait would then grow with the queue. */
TEST(drain_on_teardown, a_request_queued_for_a_worker_gets_503) {
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_qdrain_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  atomic_store(&g_queued_drain_entries, 0);
  atomic_store(&g_queued_drain_release, false);

  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/queued",
                                               _queued_drain_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  cfg.worker_thread_count = 1;
  if (rv == ccol_success) rv = chttpsvr_start(srv, &cfg);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  _queued_drain_client_t a = {.sock_path = sock_path};
  _queued_drain_client_t b = {.sock_path = sock_path};
  pthread_t ta, tb;
  bool a_started = pthread_create(&ta, NULL, _queued_drain_client_fn, &a) == 0;
  bool held = false;
  for (int i = 0; a_started && i < 10000 && !held; i++) {
    held = atomic_load(&g_queued_drain_entries) == 1;
    if (!held) {
      struct timespec nap = {0, 1000000L};
      nanosleep(&nap, NULL);
    }
  }
  size_t submits = atomic_load(&_chttpsvr_worker_submits_for_tests);
  bool b_started =
      held && pthread_create(&tb, NULL, _queued_drain_client_fn, &b) == 0;
  bool queued = false;
  for (int i = 0; b_started && i < 10000 && !queued; i++) {
    queued = atomic_load(&_chttpsvr_worker_submits_for_tests) > submits;
    if (!queued) {
      struct timespec nap = {0, 1000000L};
      nanosleep(&nap, NULL);
    }
  }

  /* The teardown runs on its own thread, because it waits for the first
   * handler, which only returns once the test releases it. */
  _chttpsvr_arm_quiesce_claimed_signal_for_tests();
  pthread_t td;
  bool d_started = false;
  bool draining = false;
  if (queued) {
    d_started = pthread_create(&td, NULL, _destroy_srv_thread_fn, &srv) == 0;
    if (d_started)
      draining = _chttpsvr_wait_quiesce_claimed_signal_entered_for_tests();
  }
  atomic_store(&g_queued_drain_release, true);
  if (d_started) pthread_join(td, NULL);
  if (a_started) pthread_join(ta, NULL);
  if (b_started) pthread_join(tb, NULL);
  if (!d_started) chttpsvr_destroy(srv);
  unlink(sock_path);

  REQUIRE_TRUE(held);
  REQUIRE_TRUE(queued);
  REQUIRE_TRUE(draining);
  REQUIRE_EQ(atomic_load(&g_queued_drain_entries), 1);
  REQUIRE_EQ(a.status, 200);
  REQUIRE_EQ(b.status, 503);
}

static long long _drain_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The moment chttpsvr_destroy() is called, or 0 before that. */
static _Atomic long long g_drain_destroy_start_ms = 0;
static _Atomic int g_drain_handler_entries = 0;
/* Handler entries that start at least 100 ms after chttpsvr_destroy() was
 * called. A request the reactor dispatched just before the call can still enter
 * its handler a moment after it, and the margin allows for that. */
static _Atomic int g_drain_entries_after_destroy = 0;

static void _drain_slow_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  atomic_fetch_add(&g_drain_handler_entries, 1);
  long long start = atomic_load(&g_drain_destroy_start_ms);
  if (start != 0 && _drain_now_ms() >= start + 100)
    atomic_fetch_add(&g_drain_entries_after_destroy, 1);
  struct timespec nap = {0, 300 * 1000000L};
  nanosleep(&nap, NULL);
  chttpsvr_resp_write_str(resp, "done");
}

typedef struct {
  const char *sock_path;
  int pipeline; /* requests sent back to back in each round */
  _Atomic bool *stop;
  _Atomic bool *go;
  _Atomic int *arrived;
  _Atomic int completed; /* responses with the body "done" */
} _drain_client_t;

static size_t _drain_count(const char *buf, const char *needle) {
  size_t n = 0;
  for (const char *p = strstr(buf, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

/* A client that keeps sending requests on one keep-alive connection for as long
 * as the server answers, and connects again when the server closes it. It ends
 * when a connect fails, which happens once the teardown has stopped the
 * listener, or when the test sets stop. */
static void *_drain_client_fn(void *arg) {
  _drain_client_t *c = (_drain_client_t *)arg;
  atomic_fetch_add(c->arrived, 1);
  while (!atomic_load(c->go)) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  int fd = -1;
  while (!atomic_load(c->stop)) {
    if (fd < 0) {
      fd = socket(AF_UNIX, SOCK_STREAM, 0);
      if (fd < 0) break;
      struct timeval tv = {5, 0};
      setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
      struct sockaddr_un addr;
      memset(&addr, 0, sizeof(addr));
      addr.sun_family = AF_UNIX;
      strncpy(addr.sun_path, c->sock_path, sizeof(addr.sun_path) - 1);
      if (test_connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        fd = -1;
        break;
      }
    }
    char req[512];
    size_t req_len = 0;
    for (int i = 0; i < c->pipeline; i++)
      req_len +=
          (size_t)snprintf(req + req_len, sizeof(req) - req_len,
                           "GET /drain-slow HTTP/1.1\r\nHost: x\r\n\r\n");
    if (write(fd, req, req_len) != (ssize_t)req_len) {
      close(fd);
      fd = -1;
      continue;
    }
    char buf[8192];
    size_t got = 0;
    buf[0] = '\0';
    size_t done = 0;
    while (done < (size_t)c->pipeline && got < sizeof(buf) - 1) {
      ssize_t r = read(fd, buf + got, sizeof(buf) - 1 - got);
      if (r <= 0) break;
      got += (size_t)r;
      buf[got] = '\0';
      done = _drain_count(buf, "\r\n\r\ndone");
    }
    atomic_fetch_add(&c->completed, (int)done);
    if (done < (size_t)c->pipeline || strstr(buf, "onnection: close")) {
      close(fd);
      fd = -1;
    }
  }
  if (fd >= 0) close(fd);
  return NULL;
}

/* Keep-alive clients sending requests in a loop, plus one client pipelining
   three requests at a time, keep a slow handler busy while chttpsvr_destroy()
   runs. The destroy only waits for the handlers that are already running: no
   handler starts after the call, and the call returns in about the time of one
   handler instead of the 30 s bound of the in-flight wait. A teardown that kept
   keep-alive connections serving would let the clients keep the in-flight count
   above zero, the handler would keep being entered after the call, and the
   destroy would take the full 30 s. */
TEST(drain_on_teardown,
     keep_alive_and_pipelined_clients_do_not_extend_destroy) {
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_drain_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);

  atomic_store(&g_drain_destroy_start_ms, 0);
  atomic_store(&g_drain_handler_entries, 0);
  atomic_store(&g_drain_entries_after_destroy, 0);

  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t reg_rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/drain-slow", _drain_slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  ccol_retval_t start_rv =
      reg_rv == ccol_success ? chttpsvr_start(srv, &cfg) : reg_rv;
  if (start_rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  enum { DRAIN_CLIENTS = 5 };
  _Atomic bool stop = false;
  _Atomic bool go = false;
  _Atomic int arrived = 0;
  _drain_client_t clients[DRAIN_CLIENTS];
  pthread_t tids[DRAIN_CLIENTS];
  int started = 0;
  for (int i = 0; i < DRAIN_CLIENTS; i++) {
    clients[i].sock_path = sock_path;
    clients[i].pipeline = i == DRAIN_CLIENTS - 1 ? 3 : 1;
    clients[i].stop = &stop;
    clients[i].go = &go;
    clients[i].arrived = &arrived;
    atomic_init(&clients[i].completed, 0);
    if (pthread_create(&tids[i], NULL, _drain_client_fn, &clients[i]) != 0)
      break;
    started++;
  }
  long long gate_deadline = _drain_now_ms() + 10000;
  while (atomic_load(&arrived) < started && _drain_now_ms() < gate_deadline) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  atomic_store(&go, true);

  /* Every client is in its loop once each one has had a response. */
  bool all_busy = false;
  long long busy_deadline = _drain_now_ms() + 10000;
  while (!all_busy && _drain_now_ms() < busy_deadline) {
    all_busy = true;
    for (int i = 0; i < started; i++)
      if (atomic_load(&clients[i].completed) < 1) all_busy = false;
    if (!all_busy) {
      struct timespec nap = {0, 5000000L};
      nanosleep(&nap, NULL);
    }
  }

  long long t0 = _drain_now_ms();
  atomic_store(&g_drain_destroy_start_ms, t0);
  chttpsvr_destroy(srv);
  long long destroy_ms = _drain_now_ms() - t0;

  atomic_store(&stop, true);
  for (int i = 0; i < started; i++) pthread_join(tids[i], NULL);
  unlink(sock_path);

  int entries_after = atomic_load(&g_drain_entries_after_destroy);
  REQUIRE_EQ(started, (int)DRAIN_CLIENTS);
  REQUIRE_TRUE(all_busy);
  REQUIRE_EQ(entries_after, 0);
  REQUIRE_LT(destroy_ms, 5000LL);
}

/* ========================================================================== */
/*                  PROTOCOL TEXT FOLDS CASE AS ASCII ONLY                    */
/* ========================================================================== */

#include <locale.h>

/* The tests below run the server under a Turkish LC_CTYPE, where strncasecmp()
   and tolower() don't fold 'I' to 'i'. Field names like "TRANSFER-ENCODING",
   "CONNECTION" and "EXPECT: 100-CONTINUE" then stop matching their lower-case
   spellings in any code that folds case through the locale. The locale is set
   for the whole process, because the parser runs on the reactor and worker
   threads, and it's put back before any assertion. The Makefile builds
   tr_TR.UTF-8 into tests_locale/ with localedef when it can. AddressSanitizer
   replaces strncasecmp() with a version that ignores the locale, so only a
   build without it can tell the two apart. */
static char *_tr_locale_enter(void) {
  const char *old_ctype = setlocale(LC_CTYPE, NULL);
  char *saved_ctype = old_ctype ? strdup(old_ctype) : NULL;
  if (!saved_ctype) return NULL;
#if defined(__GLIBC__)
  /* Only glibc reads LOCPATH. Elsewhere setenv() would change nothing about
   * locale loading and still leave the C library's environment blocks
   * reachable at exit, which the memtest of musl reports. */
  const char *old_path = getenv("LOCPATH");
  char *saved_path = old_path ? strdup(old_path) : NULL;
  setenv("LOCPATH", "tests_locale", 1);
#endif
  bool entered = setlocale(LC_CTYPE, "tr_TR.UTF-8") != NULL ||
                 setlocale(LC_CTYPE, "tr_TR.utf8") != NULL;
#if defined(__GLIBC__)
  if (saved_path) {
    setenv("LOCPATH", saved_path, 1);
    free(saved_path);
  } else {
    unsetenv("LOCPATH");
  }
#endif
  /* The locale really must fold 'I' differently, or the tests prove nothing.
   */
  if (entered && strncasecmp("I", "i", 1) == 0) {
    setlocale(LC_CTYPE, saved_ctype);
    entered = false;
  }
  if (!entered) {
    free(saved_ctype);
    printf(
        "    [skipped: no tr_TR locale that folds 'I' apart from 'i'; "
        "tests_locale/tr_TR.UTF-8 is missing]\n");
    return NULL;
  }
  return saved_ctype;
}

static void _tr_locale_leave(char *saved_ctype) {
  setlocale(LC_CTYPE, saved_ctype);
  free(saved_ctype);
}

/* Sends req to the test server and reads until EOF or for at most 5 s. */
static void _locale_raw_exchange(const char *req, char *buf, size_t cap) {
  buf[0] = '\0';
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
      write(fd, req, strlen(req)) == (ssize_t)strlen(req))
    _drain_socket_until_eof(fd, buf, cap);
  close(fd);
}

TEST(locale_folding, the_parser_matches_field_names_in_any_case) {
  chttp1_settings_t st;
  chttp1_settings_init(&st);
  const char *smuggle =
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n"
      "TRANSFER-ENCODING: chunked\r\n\r\n";
  const char *close_req =
      "GET / HTTP/1.1\r\nHost: a\r\nCONNECTION: CLOSE\r\n\r\n";
  const char *keep_req =
      "GET / HTTP/1.0\r\nHost: a\r\nCONNECTION: KEEP-ALIVE\r\n\r\n";
  const char *expect_req =
      "POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 4\r\n"
      "EXPECT: 100-CONTINUE\r\n\r\n";
  char *saved = _tr_locale_enter();
  if (!saved) return;
  chttp1_parser_t p;
  chttp1_parser_init_request(&p, &st);
  chttp1_errno_t r_smuggle =
      chttp1_parser_execute(&p, smuggle, strlen(smuggle));
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno_t r_close =
      chttp1_parser_execute(&p, close_req, strlen(close_req));
  bool close_keeps = chttp1_should_keep_alive(&p);
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno_t r_keep = chttp1_parser_execute(&p, keep_req, strlen(keep_req));
  bool keep_keeps = chttp1_should_keep_alive(&p);
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno_t r_expect =
      chttp1_parser_execute(&p, expect_req, strlen(expect_req));
  bool expects = chttp1_expects_continue(&p);
  chttp1_parser_release(&p);
  _tr_locale_leave(saved);

  /* A Content-Length next to a chunked Transfer-Encoding is refused, whatever
     case the field name is in. */
  REQUIRE_EQ((int)r_smuggle, (int)CHTTP1_ERROR);
  REQUIRE_NE((int)r_close, (int)CHTTP1_ERROR);
  REQUIRE_FALSE(close_keeps);
  REQUIRE_NE((int)r_keep, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(keep_keeps);
  REQUIRE_NE((int)r_expect, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(expects);
}

static _Atomic int g_locale_set_te_rv = 1;

static void _locale_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_header(req, "x-client-id");
  atomic_store(&g_locale_set_te_rv, (int)chttpsvr_resp_set_header(
                                        resp, "TRANSFER-ENCODING", "chunked"));
  chttpsvr_resp_set_header(resp, "CONNECTION", "x-from-handler");
  chttpsvr_resp_write_str(resp, v ? v : "(none)");
}

/* If the server compared names through strncasecmp() under this locale, the
   smuggled request would be served by its Content-Length (200),
   "CONNECTION: close" would keep the connection open, no interim 100 would be
   sent, the header lookup would miss, a handler's "TRANSFER-ENCODING" would be
   accepted and its "CONNECTION" would reach the wire. */
TEST(locale_folding, the_server_matches_field_names_in_any_case) {
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/locale-folding",
                                            _locale_header_handler, NULL),
             (int)ccol_success);
  char *saved = _tr_locale_enter();
  if (!saved) return;

  char smuggle_buf[1024];
  _locale_raw_exchange(
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Length: 5\r\n"
      "TRANSFER-ENCODING: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n",
      smuggle_buf, sizeof(smuggle_buf));

  char close_buf[1024];
  _locale_raw_exchange(
      "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\nCONNECTION: close\r\n\r\n",
      close_buf, sizeof(close_buf));

  char header_buf[1024];
  _locale_raw_exchange(
      "GET /locale-folding HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "X-CLIENT-ID: abc123\r\nConnection: close\r\n\r\n",
      header_buf, sizeof(header_buf));
  int set_te_rv = atomic_load(&g_locale_set_te_rv);

  /* Expect: 100-continue in upper case still gets the interim response before
     the body is sent. */
  char interim[256] = {0};
  char final_buf[1024] = {0};
  {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd >= 0) {
      const char *head =
          "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
          "Content-Length: 4\r\nEXPECT: 100-CONTINUE\r\n"
          "Connection: close\r\n\r\n";
      if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
          write(fd, head, strlen(head)) == (ssize_t)strlen(head)) {
        struct pollfd pfd = {fd, POLLIN, 0};
        if (poll(&pfd, 1, 3000) == 1) {
          ssize_t n = read(fd, interim, sizeof(interim) - 1);
          if (n > 0) interim[n] = '\0';
        }
        if (write(fd, "ping", 4) == 4)
          _drain_socket_until_eof(fd, final_buf, sizeof(final_buf));
      }
      close(fd);
    }
  }
  _tr_locale_leave(saved);

  REQUIRE_TRUE(strncmp(smuggle_buf, "HTTP/1.1 400", 12) == 0);
  REQUIRE_TRUE(strstr(smuggle_buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(close_buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(close_buf, "connection:close") != NULL);
  REQUIRE_TRUE(strstr(header_buf, "abc123") != NULL);
  REQUIRE_TRUE(strstr(header_buf, "x-from-handler") == NULL);
  REQUIRE_EQ(set_te_rv, (int)ccol_invalid_args);
  REQUIRE_TRUE(strncmp(interim, "HTTP/1.1 100 Continue", 21) == 0);
  REQUIRE_TRUE(strstr(final_buf, "ping") != NULL);
}

/* ========================================================================== */
/*                  ROUTE METHODS, 205 AND FIELD VALUE BYTES                  */
/* ========================================================================== */

/* A route keeps its method as one bit of a mask, so a value outside the seven
   methods and CHTTP_ANY means nothing, and every registration entry point
   refuses it. Without the check each call would store the route and return
   ccol_success, and a value of 32 or more would make the route match shift by
   the width of the mask or more. */
TEST(chttpserver, a_method_outside_the_enum_is_refused_by_every_registration) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(CLOG_INVALID, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_router *sub = chttpsvr_subrouter(srv, "/sub");
  REQUIRE_TRUE(sub != NULL);
  const int bad[] = {(int)CHTTP_ANY + 1, 8, 31, 32, 100, -1};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    chttp_method_t m = (chttp_method_t)bad[i];
    REQUIRE_EQ(
        (int)chttpsvr_register_handler(srv, m, "/a", _hello_handler, NULL),
        (int)ccol_invalid_args);
    REQUIRE_EQ((int)chttpsvr_register_streaming_handler(srv, m, "/a",
                                                        _hello_handler, NULL),
               (int)ccol_invalid_args);
    REQUIRE_EQ((int)chttpsvr_router_on(sub, m, "/a", _hello_handler, NULL),
               (int)ccol_invalid_args);
    REQUIRE_EQ(
        (int)chttpsvr_router_on_stream(sub, m, "/a", _hello_handler, NULL),
        (int)ccol_invalid_args);
  }
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/any",
                                            _hello_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(sub, CHTTP_OPTIONS, "/o", _hello_handler, NULL),
      (int)ccol_success);
}

static void _reset_content_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 205);
  chttpsvr_resp_write_str(resp, "this body must not be sent");
}

static void _status_from_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx) {
  (void)ctx;
  const char *code = NULL;
  chttpsvr_req_query_one(req, "code", &code);
  chttpsvr_resp_set_status(resp, code ? atoi(code) : 500);
  chttpsvr_resp_write_str(resp, "x");
}

/* 205 Reset Content carries no content (RFC 9110 SS15.3.6). The server drops
   what the handler wrote and says so with "content-length:0". Without 205 in
   the rule, the body would go out with its own length. */
TEST(chttpserver, reset_content_sends_no_body_and_a_zero_length) {
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/reset-content",
                                            _reset_content_handler, NULL),
             (int)ccol_success);
  char buf[2048];
  int status = _raw_request("GET", "/reset-content", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 205);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 205 Reset Content\r\n") != NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:0\r\n") != NULL);
  REQUIRE_TRUE(strstr(buf, "must not be sent") == NULL);
  const char *end = strstr(buf, "\r\n\r\n");
  REQUIRE_TRUE(end != NULL);
  REQUIRE_EQ(end[4], '\0');
}

TEST(chttpserver, every_registered_status_code_has_its_reason_phrase) {
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/status-code",
                                            _status_from_query_handler, NULL),
             (int)ccol_success);
  static const struct {
    int code;
    const char *line;
  } cases[] = {
      {203, "HTTP/1.1 203 Non-Authoritative Information\r\n"},
      {300, "HTTP/1.1 300 Multiple Choices\r\n"},
      {303, "HTTP/1.1 303 See Other\r\n"},
      {402, "HTTP/1.1 402 Payment Required\r\n"},
      {407, "HTTP/1.1 407 Proxy Authentication Required\r\n"},
      {411, "HTTP/1.1 411 Length Required\r\n"},
      {412, "HTTP/1.1 412 Precondition Failed\r\n"},
      {414, "HTTP/1.1 414 URI Too Long\r\n"},
      {416, "HTTP/1.1 416 Range Not Satisfiable\r\n"},
      {421, "HTTP/1.1 421 Misdirected Request\r\n"},
      {426, "HTTP/1.1 426 Upgrade Required\r\n"},
      {428, "HTTP/1.1 428 Precondition Required\r\n"},
      {431, "HTTP/1.1 431 Request Header Fields Too Large\r\n"},
      {451, "HTTP/1.1 451 Unavailable For Legal Reasons\r\n"},
      {511, "HTTP/1.1 511 Network Authentication Required\r\n"},
      {599, "HTTP/1.1 599 Unknown\r\n"},
  };
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    char path[64];
    snprintf(path, sizeof(path), "/status-code?code=%d", cases[i].code);
    char buf[2048];
    int status = _raw_request("GET", path, NULL, buf, sizeof(buf));
    REQUIRE_EQ(status, cases[i].code);
    REQUIRE_TRUE(strncmp(buf, cases[i].line, strlen(cases[i].line)) == 0);
  }
}

static void _control_byte_value_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x01z");
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x1fz");
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x7fz");
  results[3] = (int)chttpsvr_resp_add_header(resp, "x-ctl", "a\x0bz");
  results[4] = (int)chttpsvr_resp_add_header(resp, "x-ctl", "\x7f");
  /* HTAB, SP and obs-text are field-value bytes. */
  results[5] = (int)chttpsvr_resp_set_header(resp, "x-tab", "a\tb c");
  results[6] = (int)chttpsvr_resp_add_header(resp, "x-obs", "caf\xc3\xa9");
  chttpsvr_resp_write_str(resp, "ok");
}

/* RFC 9110 SS5.5 allows VCHAR, SP, HTAB and obs-text in a field value, and a
   client that follows it refuses a response with any other control byte. With
   a check for CR and LF only, the first five calls would succeed and the bytes
   would reach the wire. */
TEST(chttpserver, a_header_value_with_a_control_byte_is_refused) {
  static _Atomic int results[7];
  for (int i = 0; i < 7; i++) atomic_store(&results[i], -100);
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/ctl-value",
                                            _control_byte_value_handler,
                                            (void *)results),
             (int)ccol_success);
  chttpcli_response *resp = _get("/ctl-value");
  int status = resp ? resp->status_code : -1;
  if (resp) chttpclient_resp_free(resp);
  REQUIRE_EQ(status, 200);
  for (int i = 0; i < 5; i++)
    REQUIRE_EQ(atomic_load(&results[i]), (int)ccol_invalid_args);
  REQUIRE_EQ(atomic_load(&results[5]), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&results[6]), (int)ccol_success);
}

/* ========================================================================== */
/*                    LISTENER STATUS AND PORT 0                              */
/* ========================================================================== */

TEST(listen_info, port_zero_binds_an_ephemeral_port_that_a_client_reaches) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/port-zero",
                                            _hello_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  uint16_t port = 0;
  ccol_retval_t port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_MAX];
  ccol_retval_t addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
  char buf[2048] = {0};
  int status = port != 0 ? _raw_request_on_port(port, "GET", "/port-zero", NULL,
                                                buf, sizeof(buf))
                         : -1;
  bool body_ok = strstr(buf, "Hello, world!") != NULL;
  chttpsvr_stop(srv);

  REQUIRE_EQ((int)port_rv, (int)ccol_success);
  REQUIRE_NE((int)port, 0);
  REQUIRE_EQ((int)addr_rv, (int)ccol_success);
  REQUIRE_STREQ(addr, "127.0.0.1");
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(body_ok);
}

TEST(listen_info, a_fixed_port_is_reported_as_bound) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 101;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  ccol_retval_t rv = chttpsvr_bound_port(srv, &port);
  chttpsvr_stop(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ((int)port, TEST_PORT + 101);
}

TEST(listen_info, the_default_host_reports_an_address_of_every_interface) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/any-host",
                                            _hello_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  ccol_retval_t port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_MAX];
  ccol_retval_t addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
  char buf[2048] = {0};
  int status = port != 0 ? _raw_request_on_port(port, "GET", "/any-host", NULL,
                                                buf, sizeof(buf))
                         : -1;
  chttpsvr_stop(srv);
  REQUIRE_EQ((int)port_rv, (int)ccol_success);
  REQUIRE_NE((int)port, 0);
  REQUIRE_EQ((int)addr_rv, (int)ccol_success);
  REQUIRE_TRUE(strcmp(addr, "::") == 0 || strcmp(addr, "0.0.0.0") == 0);
  REQUIRE_EQ(status, 200);
}

TEST(listen_info, the_running_flag_follows_start_stop_restart_and_destroy) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  bool before = true, started = false, stopped = true, restarted = false;
  uint16_t first_port = 0, stopped_port = 7, second_port = 0;
  ccol_retval_t stopped_port_rv, before_addr_rv;
  char before_addr[CHTTPSVR_BOUND_ADDRESS_MAX] = "x";
  chttpsvr_is_running(srv, &before);
  before_addr_rv =
      chttpsvr_bound_address(srv, before_addr, sizeof(before_addr));
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  ccol_retval_t start1 = chttpsvr_start(srv, &cfg);
  chttpsvr_is_running(srv, &started);
  chttpsvr_bound_port(srv, &first_port);
  chttpsvr_stop(srv);
  chttpsvr_is_running(srv, &stopped);
  stopped_port_rv = chttpsvr_bound_port(srv, &stopped_port);
  ccol_retval_t start2 = chttpsvr_start(srv, &cfg);
  chttpsvr_is_running(srv, &restarted);
  chttpsvr_bound_port(srv, &second_port);
  chttpsvr_destroy(srv);
  REQUIRE_TRUE(srv == CHTTPSVR_INVALID);

  REQUIRE_FALSE(before);
  REQUIRE_EQ((int)before_addr_rv, (int)ccol_not_permitted);
  REQUIRE_STREQ(before_addr, "");
  REQUIRE_EQ((int)start1, (int)ccol_success);
  REQUIRE_TRUE(started);
  REQUIRE_NE((int)first_port, 0);
  REQUIRE_FALSE(stopped);
  REQUIRE_EQ((int)stopped_port_rv, (int)ccol_not_permitted);
  REQUIRE_EQ((int)stopped_port, 0);
  REQUIRE_EQ((int)start2, (int)ccol_success);
  REQUIRE_TRUE(restarted);
  REQUIRE_NE((int)second_port, 0);
}

TEST(listen_info, a_stale_handle_and_bad_arguments_are_refused) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr stale = srv;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  ccol_retval_t start = chttpsvr_start(srv, &cfg);
  /* A buffer one byte too short for "127.0.0.1" and its NUL byte. */
  char small[9] = "xxxxxxxx";
  ccol_retval_t small_rv = chttpsvr_bound_address(srv, small, sizeof(small));
  char exact[10];
  ccol_retval_t exact_rv = chttpsvr_bound_address(srv, exact, sizeof(exact));
  ccol_retval_t null_run = chttpsvr_is_running(srv, NULL);
  ccol_retval_t null_port = chttpsvr_bound_port(srv, NULL);
  ccol_retval_t null_buf = chttpsvr_bound_address(srv, NULL, 16);
  char zero[4] = "abc";
  ccol_retval_t zero_len = chttpsvr_bound_address(srv, zero, 0);
  chttpsvr_destroy(srv);

  bool running = true;
  uint16_t port = 9;
  char addr[CHTTPSVR_BOUND_ADDRESS_MAX] = "x";
  REQUIRE_EQ((int)chttpsvr_is_running(stale, &running), (int)ccol_invalid_args);
  REQUIRE_FALSE(running);
  REQUIRE_EQ((int)chttpsvr_bound_port(stale, &port), (int)ccol_invalid_args);
  REQUIRE_EQ((int)port, 0);
  REQUIRE_EQ((int)chttpsvr_bound_address(stale, addr, sizeof(addr)),
             (int)ccol_invalid_args);
  REQUIRE_STREQ(addr, "");
  running = true;
  REQUIRE_EQ((int)chttpsvr_is_running(CHTTPSVR_INVALID, &running),
             (int)ccol_invalid_args);
  REQUIRE_FALSE(running);

  REQUIRE_EQ((int)start, (int)ccol_success);
  REQUIRE_EQ((int)small_rv, (int)ccol_msg_too_large);
  REQUIRE_STREQ(small, "");
  REQUIRE_EQ((int)exact_rv, (int)ccol_success);
  REQUIRE_STREQ(exact, "127.0.0.1");
  REQUIRE_EQ((int)null_run, (int)ccol_invalid_args);
  REQUIRE_EQ((int)null_port, (int)ccol_invalid_args);
  REQUIRE_EQ((int)null_buf, (int)ccol_invalid_args);
  REQUIRE_EQ((int)zero_len, (int)ccol_invalid_args);
  REQUIRE_STREQ(zero, "abc");
}

TEST(listen_info, a_unix_listener_reports_its_path_and_no_port) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_info_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  cfg.port = 0;
  ccol_retval_t start = chttpsvr_start(srv, &cfg);
  uint16_t port = 5;
  ccol_retval_t port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_MAX];
  ccol_retval_t addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
  bool running = false;
  chttpsvr_is_running(srv, &running);
  chttpsvr_stop(srv);
  unlink(sock_path);
  REQUIRE_EQ((int)start, (int)ccol_success);
  REQUIRE_TRUE(running);
  REQUIRE_EQ((int)port_rv, (int)ccol_not_permitted);
  REQUIRE_EQ((int)port, 0);
  REQUIRE_EQ((int)addr_rv, (int)ccol_success);
  REQUIRE_STREQ(addr, host_buf);
}

/* ========================================================================== */
/*                    PER-REQUEST CONTEXT VALUES                              */
/* ========================================================================== */

static const char g_ctx_key_a = 0, g_ctx_key_b = 0, g_ctx_key_c = 0,
                  g_ctx_key_d = 0, g_ctx_key_e = 0, g_ctx_key_unset = 0,
                  g_ctx_key_handler = 0;
static char g_ctx_val_global_a[] = "global-a";
static char g_ctx_val_sub_a[] = "sub-a";
static char g_ctx_val_b[] = "b";
static char g_ctx_val_c[] = "c";
static char g_ctx_val_c2[] = "c2";
static char g_ctx_val_d[] = "d";
static char g_ctx_val_e[] = "e";
static char g_ctx_val_handler[] = "from-handler";

/* What the global middleware saw after next() returned, by kind of request. */
typedef struct {
  _Atomic int handler_value_seen;  /* matched routes */
  _Atomic int rejected_value_seen; /* the 404 under the sub-router */
  _Atomic int calls_after_next;
} _ctx_probe_t;

static bool _ctx_skip(const chttpsvr_req *req) {
  return chttpsvr_req_header(req, "x-ctx-skip") != NULL;
}

static void _ctx_global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                           chttpsvr_next_fn next) {
  _ctx_probe_t *probe = (_ctx_probe_t *)ctx;
  if (!_ctx_skip(req))
    chttpsvr_req_set_context(req, &g_ctx_key_a, g_ctx_val_global_a);
  next(req, resp);
  atomic_fetch_add(&probe->calls_after_next, 1);
  if (_ctx_skip(req)) return;
  if (strcmp(chttpsvr_req_path(req), "/ctx/missing") == 0) {
    /* The values stay readable until the outermost middleware returns. */
    if (chttpsvr_req_context(req, &g_ctx_key_a) == g_ctx_val_sub_a &&
        chttpsvr_req_context(req, &g_ctx_key_e) == g_ctx_val_e)
      atomic_store(&probe->rejected_value_seen, 1);
  } else if (chttpsvr_req_context(req, &g_ctx_key_handler) ==
             g_ctx_val_handler) {
    atomic_fetch_add(&probe->handler_value_seen, 1);
  }
}

static void _ctx_sub_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                        chttpsvr_next_fn next) {
  (void)ctx;
  if (!_ctx_skip(req)) {
    /* The inline value gets replaced, and C is replaced inside the block that
     * holds the values after the first one. E is the fifth key, so the block
     * grows past its first capacity. */
    chttpsvr_req_set_context(req, &g_ctx_key_b, g_ctx_val_b);
    chttpsvr_req_set_context(req, &g_ctx_key_c, g_ctx_val_c);
    chttpsvr_req_set_context(req, &g_ctx_key_d, g_ctx_val_d);
    chttpsvr_req_set_context(req, &g_ctx_key_a, g_ctx_val_sub_a);
    chttpsvr_req_set_context(req, &g_ctx_key_c, g_ctx_val_c2);
    chttpsvr_req_set_context(req, &g_ctx_key_e, g_ctx_val_e);
    chttpsvr_req_set_context(req, &g_ctx_key_unset, NULL);
  }
  next(req, resp);
}

static const char *_ctx_text(const chttpsvr_req *req, const void *key) {
  const char *v = (const char *)chttpsvr_req_context(req, key);
  return v ? v : "(null)";
}

static void _ctx_report(chttpsvr_req *req, chttpsvr_resp *resp,
                        const char *kind) {
  chttpsvr_resp_printf(
      resp, "%s a=%s b=%s c=%s d=%s e=%s unset=%s", kind,
      _ctx_text(req, &g_ctx_key_a), _ctx_text(req, &g_ctx_key_b),
      _ctx_text(req, &g_ctx_key_c), _ctx_text(req, &g_ctx_key_d),
      _ctx_text(req, &g_ctx_key_e), _ctx_text(req, &g_ctx_key_unset));
  chttpsvr_req_set_context(req, &g_ctx_key_handler, g_ctx_val_handler);
}

static void _ctx_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  _ctx_report(req, resp, "buffered");
}

static void _ctx_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  char buf[256];
  while (chttpsvr_req_read(req, buf, sizeof(buf)) > 0) {
  }
  _ctx_report(req, resp, "stream");
}

TEST(req_context, middleware_values_reach_the_handler_through_a_subrouter) {
  static _ctx_probe_t probe;
  atomic_store(&probe.handler_value_seen, 0);
  atomic_store(&probe.rejected_value_seen, 0);
  atomic_store(&probe.calls_after_next, 0);
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _ctx_global_mw, &probe), (int)ccol_success);
  chttpsvr_router *sub = chttpsvr_subrouter(srv, "/ctx");
  REQUIRE_TRUE(sub != NULL);
  REQUIRE_EQ((int)chttpsvr_router_use(sub, _ctx_sub_mw, NULL),
             (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_router_on(sub, CHTTP_GET, "/plain", _ctx_handler, NULL),
      (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_router_on_stream(sub, CHTTP_GET, "/stream",
                                            _ctx_stream_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  REQUIRE_EQ((int)chttpsvr_bound_port(srv, &port), (int)ccol_success);

  char plain[2048] = {0}, stream[2048] = {0}, skipped[2048] = {0},
       missing[2048] = {0};
  int plain_status = _raw_request_on_port(port, "GET", "/ctx/plain", NULL,
                                          plain, sizeof(plain));
  int stream_status = _raw_request_on_port(port, "GET", "/ctx/stream", NULL,
                                           stream, sizeof(stream));
  int skipped_status = _raw_request_on_port(
      port, "GET", "/ctx/plain", "X-Ctx-Skip: 1\r\n", skipped, sizeof(skipped));
  int missing_status = _raw_request_on_port(port, "GET", "/ctx/missing", NULL,
                                            missing, sizeof(missing));
  chttpsvr_stop(srv);

  REQUIRE_EQ(plain_status, 200);
  REQUIRE_TRUE(
      strstr(plain, "buffered a=sub-a b=b c=c2 d=d e=e unset=(null)") != NULL);
  REQUIRE_EQ(stream_status, 200);
  REQUIRE_TRUE(strstr(stream, "stream a=sub-a b=b c=c2 d=d e=e unset=(null)") !=
               NULL);
  /* A request whose middleware set nothing sees nothing from the requests
   * before it. */
  REQUIRE_EQ(skipped_status, 200);
  REQUIRE_TRUE(strstr(skipped,
                      "buffered a=(null) b=(null) c=(null) d=(null) "
                      "e=(null) unset=(null)") != NULL);
  REQUIRE_EQ(missing_status, 404);
  REQUIRE_EQ(atomic_load(&probe.calls_after_next), 4);
  REQUIRE_EQ(atomic_load(&probe.handler_value_seen), 2);
  REQUIRE_EQ(atomic_load(&probe.rejected_value_seen), 1);
}

TEST(req_context, null_arguments_are_refused) {
  static const char key = 0;
  REQUIRE_EQ((int)chttpsvr_req_set_context(NULL, &key, NULL),
             (int)ccol_invalid_args);
  REQUIRE_TRUE(chttpsvr_req_context(NULL, &key) == NULL);
}

/* An allocator whose realloc fails while the calling thread asks it to, so a
 * middleware can make exactly one growth of the context storage fail. */
static _Thread_local bool g_ctx_fail_realloc = false;
static void *_ctx_fail_realloc(void *p, size_t n) {
  if (g_ctx_fail_realloc) return NULL;
  return realloc(p, n);
}

static _Atomic int g_ctx_oom_results[6];

static void _ctx_oom_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                        chttpsvr_next_fn next) {
  (void)ctx;
  static const char key_null = 0;
  atomic_store(
      &g_ctx_oom_results[0],
      (int)chttpsvr_req_set_context(req, &g_ctx_key_a, g_ctx_val_global_a));
  g_ctx_fail_realloc = true;
  atomic_store(&g_ctx_oom_results[1],
               (int)chttpsvr_req_set_context(req, &g_ctx_key_b, g_ctx_val_b));
  g_ctx_fail_realloc = false;
  atomic_store(&g_ctx_oom_results[2],
               chttpsvr_req_context(req, &g_ctx_key_a) == g_ctx_val_global_a);
  atomic_store(&g_ctx_oom_results[3],
               chttpsvr_req_context(req, &g_ctx_key_b) == NULL);
  atomic_store(&g_ctx_oom_results[4],
               (int)chttpsvr_req_set_context(req, &g_ctx_key_b, g_ctx_val_b));
  atomic_store(&g_ctx_oom_results[5],
               (int)chttpsvr_req_set_context(req, NULL, g_ctx_val_b) ==
                       (int)ccol_invalid_args &&
                   chttpsvr_req_context(req, NULL) == NULL &&
                   chttpsvr_req_context(req, &key_null) == NULL);
  next(req, resp);
}

TEST(req_context, a_failed_allocation_keeps_the_earlier_values) {
  static ccol_memmgmt_procs_t mp = {malloc, free, calloc, _ctx_fail_realloc};
  for (int i = 0; i < 6; i++) atomic_store(&g_ctx_oom_results[i], -100);
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _ctx_oom_mw, NULL), (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ctx-oom",
                                            _ctx_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  REQUIRE_EQ((int)chttpsvr_bound_port(srv, &port), (int)ccol_success);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(port, "GET", "/ctx-oom", NULL, buf, sizeof(buf));
  chttpsvr_stop(srv);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[0]), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[1]), (int)ccol_not_enough_memory);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[2]), 1);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[3]), 1);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[4]), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&g_ctx_oom_results[5]), 1);
  REQUIRE_TRUE(strstr(buf, "buffered a=global-a b=b") != NULL);
}
