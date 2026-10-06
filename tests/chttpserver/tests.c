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

/* pthread_timedjoin_np (a glibc extension) makes _bounded_join below a
 * bounded join and not a blind pthread_join. Without it, a background thread
 * can hang the join forever. This happens when a real regression in the
 * mechanism that a race-hook test checks leaves that thread permanently
 * stuck. The whole binary then goes down, and that one test does not fail
 * cleanly. You must define this macro before the first #include that can
 * pull in <pthread.h> indirectly. tests_engine_stop.c uses the same
 * placement for the same reason, and so do src/chttpserver.c and
 * src/clogger.c. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
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
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <pthread_np.h> /* pthread_timedjoin_np */
#endif

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_sanitizer.h>
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
/*                         GLOBAL TEST SERVER STATE                           */
/* ========================================================================== */

#define TEST_PORT 18765
#define BASE_URL "http://127.0.0.1:18765"

static clog g_test_logger = CLOG_INVALID;
static chttpsvr g_srv = CHTTPSVR_INVALID;

/* Scope-exit cleanup for a plain client-side test socket fd. Use it as
   `int fd _ccol_destructor(_close_scoped_fd) = socket(...);`. A later
   REQUIRE_* check in the test can return early, before the final explicit
   close(fd) of the test. The fd then leaks for the rest of the run of this
   binary, and this cleanup prevents that. This function accepts a negative
   fd without a complaint, because socket() or connect() can fail. This
   matches the close(2) convention that an EBADF is harmless to ignore
   here, which this file also uses in other places. */
static void _close_scoped_fd(int *fd) {
  if (fd && *fd >= 0) close(*fd);
}

/* Bounded join for a background thread that a test just released from a
   white-box race-test hook (_chttpsvr_release_*_race_hook_for_tests()). It
   is the same as the _fx_timed_join of tests_engine_stop.c: the same
   mechanism, the same bound of 30s and the same reason. A regression can
   leave the released thread stuck in the mechanism under test. Such a
   regression must fail the REQUIRE_TRUE of this one test below. It must
   not hang this whole binary in an unbounded pthread_join. 30s is much
   more than every real join in the race-hook tests of this file. Each hook
   release unblocks its thread in microseconds, so this bound never fires
   against a thread that behaves correctly. The function returns true only
   when tid stopped and this call joined it. On a false return, the caller
   must call pthread_detach(tid). Without that call, the thread that still
   runs leaks as permanently joinable. */
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

/* Writes ctx, a literal string that the caller owns, without a change. The
   per-router segment-decode cache tests below use it. It tells you which
   route matched, out of several routes that share one decoded prefix. */
static void _cache_literal_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

/* Writes "<ctx>:<id param>". The per-router segment-decode cache tests
   below use it. Several OTHER candidate routes try the position of this
   param first, and each one fails at a LATER segment. The tests prove that
   the captured param still reads back correctly for the route that
   matches. */
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
  /* Join values with comma. */
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

/* Middleware call-count tracking. _Atomic to avoid data race between reactor
   threads (which increment) and the test thread (which reads). */
static _Atomic int g_global_mw_count = 0;
static _Atomic int g_router_mw_count = 0;

static void _global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
  (void)ctx;
  g_global_mw_count++;
  /* Inject a response header to prove middleware ran. */
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

/* Middleware that short-circuits (does not call next). */
static void _blocking_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                         chttpsvr_next_fn next) {
  (void)req;
  (void)ctx;
  (void)next;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_FORBIDDEN);
  chttpsvr_resp_write_str(resp, "blocked");
}

/* Two middlewares registered at the same router level to verify that both run
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

/* Streaming handler that counts the separate chttpsvr_req_read() calls
   that gave data before EOF. It reports the count in a response header.
   This proves that the server delivers the body in separate batches as the
   bytes arrive on the wire. The server does not buffer the whole body
   before the handler and then hand it over as one blob. */
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

/* Streaming handler that drains the body. It reports in a response header
   why the final chttpsvr_req_read() call returned -1, if it did. This
   tests stream_read_timeout_us and the report of a mid-stream abort. */
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

/* Buffered handler that reports the answer of chttpsvr_req_header for one
   name. The client sends that name only as the trailer field of a chunked
   body (RFC 7230 SS4.1.2), never as a regular header. This pins the
   documented contract of chttpsvr_req_header: a trailer field is not a
   request header, and it reads as absent. A buffered handler runs only
   after the server reads the whole body, with the trailers. This is the
   strongest form of the check. The parser reads the trailer before the
   first statement here runs, and the trailer is still not readable. */
static void _buffered_trailer_echo_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-value", val ? val : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

/* Streaming handler that reports the answer of chttpsvr_req_header for
   "X-Trailer". That name is again the trailer field of a chunked body, not
   a regular header. The handler asks BEFORE it drains the body to EOF, and
   again AFTER. Both answers must be "absent". A streaming route drives the
   parse of the trailer itself, with its own chttpsvr_req_read calls. This
   check therefore covers both sides of the point where the parser reads
   the trailer. */
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

/* Buffered handler that echoes back the answer of chttpsvr_req_header for
   two names. A reverse proxy or an authentication gateway sets those two
   names for the client. This pins the property that the trailer fields of
   a chunked body cannot displace them. The client sends both names twice,
   once as a real header and once as a trailer with a different value. The
   handler must report the value of the header. */
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

/* Writes a large response body of several megabytes. The body is much
   larger than any socket send buffer. A send of it to a client that never
   reads must therefore block chttp1_stream_write in the middle. This
   exercises max_response_write_duration_us, which is the write-side
   equivalent of max_body_read_duration_us. */
/* 16 MiB. This size is much larger than the sum of two buffers on any
   system with a reasonable configuration. The first is the explicit
   128 KiB SO_SNDBUF floor of the server (_apply_accepted_socket_options).
   The second is the default receive buffer and TCP window of the test
   client, which the OS tunes automatically. The client in
   max_response_write_duration_exceeded_closes_connection below never reads
   at all. The receive-buffer auto-tuning of the kernel then has no read
   pattern to grow the window from, so the real ceiling stays close to the
   small default. A send of this body to such a client must therefore block
   chttp1_stream_write on backpressure. It blocks long before the whole
   body fits in flight. The send must not look instant because the body fit
   in some generous pipeline of buffers from end to end. */
#define _CHTTPSVR_TEST_LARGE_BODY_SIZE (16 * 1024 * 1024)

static void _large_response_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  /* A plain static array in BSS, and not a heap allocation. A single
     malloc() that nothing frees for the rest of the life of the process
     shows up as a real "still reachable" block. make memtest reports such
     a block with --errors-for-leak-kinds=all. A BSS array never shows up
     there. */
  static char big_buf[_CHTTPSVR_TEST_LARGE_BODY_SIZE];
  static bool filled = false;
  if (!filled) {
    memset(big_buf, 'x', sizeof(big_buf));
    filled = true;
  }
  chttpsvr_resp_write(resp, big_buf, sizeof(big_buf));
}

/* Streaming handler that never calls chttpsvr_req_read(). It always
   rejects the request immediately. This exercises the case that
   Expect: 100-continue (RFC 7231 SS5.1.1) exists for. That case is a
   server that rejects a request and never wants the body of it. */
static void _stream_reject_without_reading_handler(chttpsvr_req *req,
                                                   chttpsvr_resp *resp,
                                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "no thanks");
}

/* Streaming handler that reads exactly one small batch of the body. It
   then returns and does not drain the rest. This proves that a connection
   is still safe for the next request when the handler leaves part of the
   body unread. http1_stream_release must discard the unread remainder. It
   must not keep that remainder. */
static void _stream_read_once_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[8];
  chttpsvr_req_read(req, buf, sizeof(buf));
  chttpsvr_resp_write_str(resp, "early-stop");
}

/* Raw query echo handler. */
static void _raw_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *rq = chttpsvr_req_raw_query(req);
  chttpsvr_resp_write_str(resp, rq ? rq : "(none)");
}

/* Path + method echo for quick checks. */
static void _path_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, chttpsvr_req_path(req));
}

/* Handler that exercises chttpsvr_req_query_one directly. */
static void _query_one_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *val = NULL;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "q", &val);
  if (rv == ccol_success) {
    chttpsvr_resp_write_str(resp, val ? val : "(null)");
  } else if (rv == ccol_key_not_found) {
    chttpsvr_resp_write_str(resp, "not_found");
  } else if (rv == ccol_not_permitted) {
    chttpsvr_resp_write_str(resp, "multi_value");
  } else {
    chttpsvr_resp_write_str(resp, "error");
  }
}

/* Streaming handler that echoes a request header; tests the pre-extracted
   header array path used by chttpsvr_req_header on streaming routes. */
static void _stream_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "x-stream-test");
  chttpsvr_resp_write_str(resp, val ? val : "(none)");
}

/* Handler that calls chttpsvr_resp_write_str three times to verify that
   multiple writes accumulate into a single response body. */
static void _multi_write_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "foo");
  chttpsvr_resp_write_str(resp, "bar");
  chttpsvr_resp_write_str(resp, "baz");
}

/* Handler that exercises chttpsvr_resp_printf. It makes a short call with
   a mix of scalar types, which fits the internal stack buffer. It then
   makes a long call whose formatted output is larger than that stack
   buffer, so the function must take the heap fallback path. Both calls
   must add to the same body, in the same way as chttpsvr_resp_write_str. */
static void _printf_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_printf(resp, "n=%d s=%s f=%.2f ", 42, "hi", 3.5);
  for (int i = 0; i < 40; i++) {
    chttpsvr_resp_printf(resp, "%08d-", i);
  }
}

/* Handler that queries two distinct keys in one request to verify that the
   _qresult scratch array is correctly reused across calls. */
static void _multi_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  size_t na = 0, nb = 0;
  const char **va = chttpsvr_req_query(req, "a", &na);
  /* Capture the string value before the next query call recycles _qresult. */
  const char *a_val = (va && na > 0) ? va[0] : "nil";
  const char **vb = chttpsvr_req_query(req, "b", &nb);
  const char *b_val = (vb && nb > 0) ? vb[0] : "nil";
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "a=%s b=%s", a_val, b_val);
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

/* Handler that calls chttpsvr_req_read() on a buffered (non-streaming) route.
   chttpsvr_req_read() must return -1 when is_streaming is false. */
static void _read_on_buffered_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that reads the body with chttpsvr_req_body() and not
   with chttpsvr_req_read(). A streaming route reads its body live off the
   socket with chttpsvr_req_read(). The documentation of
   chttpsvr_req_body() says that it works for a buffered route only. It
   must therefore return NULL and 0 here. It must not return a body that
   the handler never asked the server to read. */
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

/* Streaming handler that calls chttpsvr_req_read() FIRST. That call drains
   part of the body off the socket and fills the internal cursor and
   growbuf state of conn->body. The handler then calls chttpsvr_req_body().
   This exercises the case after a read, which _stream_body_check_handler
   above cannot reach. That handler never calls chttpsvr_req_read(), so
   conn->body is still empty when it checks. chttpsvr_req_body() must
   report NULL and 0 here too. It must not report the internal accounting
   state that stays in conn->body. Without the is_streaming guard, that
   state holds bytes that the server already delivered, and a length that
   shows internal bookkeeping. That length is neither the true total nor
   the true count of the bytes that stay unread. */
static void _stream_body_after_read_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[8];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  size_t len = (size_t)-1; /* poisoned: chttpsvr_req_body must overwrite it */
  const void *body = chttpsvr_req_body(req, &len);
  char s[64];
  snprintf(s, sizeof(s), "read=%ld body=%s len=%zu", (long)n,
           body ? "non-null" : "null", len);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that calls chttpsvr_req_read with buflen == 0.
   A zero-length read must return 0 (no-op), not -1 (error). */
static void _stream_zero_buflen_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  char buf[4];
  ssize_t n = chttpsvr_req_read(req, buf, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler on a route with no request body.
   The first call to chttpsvr_req_read must return 0 (EOF) immediately. */
static void _stream_read_empty_body_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler that sets the same response header twice; the second value must win.
 */
static void _dup_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-dup", "first");
  chttpsvr_resp_set_header(resp, "x-dup", "second");
  chttpsvr_resp_write_str(resp, "ok");
}

/* Handler that sets a status code outside the valid range. The server must
   clamp it to 500. See the [100,999] range check in _send_response. The
   server must not write the raw out-of-range value onto the wire. */
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
  /* NULL key must return ccol_invalid_args, not crash. */
  ccol_retval_t rv = chttpsvr_req_query_one(req, NULL, &val);
  char s[16];
  snprintf(s, sizeof(s), "%d", (int)rv);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler that calls chttpsvr_req_query_one with a NULL val_out when the key
   is present.  The function must still return ccol_success (it has a value to
   report but the caller elected not to receive it) and must not crash. */
static void _query_one_null_val_out_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ccol_retval_t rv = chttpsvr_req_query_one(req, "q", NULL);
  chttpsvr_resp_write_str(resp, rv == ccol_success ? "ok" : "fail");
}

/* Streaming handler that calls chttpsvr_req_read with a NULL buffer.
   The NULL buffer guard must fire before the is_streaming check and return -1.
 */
static void _stream_null_buf_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 4);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Streaming handler that calls chttpsvr_req_read(req, NULL, 0).
   When both buf is NULL and buflen is 0 the buflen==0 guard must win
   and return 0, not -1 (which the NULL buf guard would return). */
static void _stream_null_buf_zero_len_handler(chttpsvr_req *req,
                                              chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

/* Handler for the headers-only response test.
   Sets a status code and a response header but writes NO body bytes.  Used to
   verify that _finalize_response calls http_finish (not http_send_body) when
   the body buffer is empty. */
static void _headers_only_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_NO_CONTENT);
  chttpsvr_resp_set_header(resp, "x-no-body", "1");
  /* Intentionally no chttpsvr_resp_write* call. */
}

/* Always writes a body that is not empty. It then overwrites the status
   code with the numeric value that the "x-force-status" request header
   names. The status code is 200 when that header is absent or unparsable.
   This verifies that _send_response drops the body for a status code that
   must never carry one. For a 1xx or a 204 it also drops the automatic
   Content-Length. It does this whatever the handler itself wrote. When the
   request carries "x-force-cl", the handler also sets that value as its own
   Content-Length. */
static void _forced_status_with_body_handler(chttpsvr_req *req,
                                             chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, "this-body-must-never-reach-the-wire");
  const char *forced = chttpsvr_req_header(req, "x-force-status");
  chttpsvr_resp_set_status(resp, forced ? atoi(forced) : 200);
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
}

/* A route registered for HEAD itself. It writes no body. When the request
   carries "x-force-cl", it sets that value as the Content-Length of the
   representation. */
static void _bodyless_head_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
  chttpsvr_resp_set_header(resp, "x-explicit-head", "1");
}

/* Tries a list of Content-Length values through both header calls, and
   answers with one character for each: 'y' for ccol_success and 'n' for
   ccol_invalid_args. The value that the last accepted call left behind is
   discarded again, because the handler writes a body. */
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
   chttpsvr_resp_set_header; see resp_add_header_keeps_every_field_in_order.
   The handler reports every return value that is not ccol_success in the
   body. */
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
  /* Date is one field whichever call sets it. */
  bad += chttpsvr_resp_add_header(resp, "Date", "d1") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "date", "d2") != ccol_success;
  /* Rejected exactly as set_header rejects them, with nothing changed. */
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

/* Sets an explicit "Connection" response header that the caller gives in
   the "x-force-connection" request header. That value can have nothing to
   do with the decision that the framing logic of the server makes later.
   This verifies that a handler cannot make the Connection header on the
   wire disagree with the real behaviour of the server after the response.
   That behaviour is a keep-alive or a close. See the doc comment of
   _send_response on this point. */
static void _explicit_connection_header_handler(chttpsvr_req *req,
                                                chttpsvr_resp *resp,
                                                void *ctx) {
  (void)ctx;
  const char *forced = chttpsvr_req_header(req, "x-force-connection");
  if (forced) chttpsvr_resp_set_header(resp, "Connection", forced);
  chttpsvr_resp_write_str(resp, "explicit-connection-header-ok");
}

/* Sets enough response headers, each one with a large value, to make the
   combined header block larger than a few KiB. _send_response must build
   the header block in a buffer that grows on the heap and doubles when it
   needs to. A fixed stack buffer of 4096 bytes with no fallback loses the
   ENTIRE response once the headers alone pass that size. There is not even
   a graceful 500. The server closes the connection, writes zero bytes and
   logs nothing. This call must succeed with every header intact, whatever
   the size is. */
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

/* Handler for the SECOND registration of the same path and method. It
   verifies that the server accepts a duplicate registration without a
   complaint. Only the FIRST handler ever runs, because the first one
   wins. */
static void _dup_second_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "second");
}

/* Handler that calls chttpsvr_req_header(req, NULL) and writes "null" if the
   return value is NULL, "non-null" otherwise.  Used to exercise the !name
   guard from an HTTP round-trip (chttpsvr_req is opaque in the public API). */
static void _null_name_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_header(req, NULL);
  chttpsvr_resp_write_str(resp, v == NULL ? "null" : "non-null");
}

/* Handler that calls chttpsvr_req_query(req, NULL, &n) and writes "null" if
   the return value is NULL, "non-null" otherwise. */
static void _null_key_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)ctx;
  size_t n = 99;
  const char **v = chttpsvr_req_query(req, NULL, &n);
  chttpsvr_resp_write_str(resp, (v == NULL && n == 0) ? "null" : "non-null");
}

/* Second server for "already running" test (never actually served). */
static chttpsvr g_srv2 = CHTTPSVR_INVALID;

/* ========================================================================== */
/*       SHARED RESULT ARRAYS FOR HANDLERS REGISTERED IN _setup              */
/* ========================================================================== */

/* Owned by _setup-registered routes; tests reset these before each request.
 * Declared _Atomic to avoid data races between the handler thread (writer) and
 * the test thread (reader).  The HTTP round-trip provides the required
 * happens-before ordering, but _Atomic int eliminates the formal UB that plain
 * int would carry under the C memory model. */
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
  /* A CRLF embedded in the NAME, attempting to inject a second header
   * line ("x-injected: evil") ahead of the real value. */
  results[0] =
      (int)chttpsvr_resp_set_header(resp, "x-evil\r\nx-injected: evil", "v");
  /* A CRLF embedded in the VALUE, attempting to inject a bogus status line
   * for a second, attacker-controlled response. */
  results[1] = (int)chttpsvr_resp_set_header(
      resp, "x-evil", "v\r\n\r\nHTTP/1.1 200 OK\r\nx-injected: evil");
  /* A legitimate header must still work fine after the two rejections
   * above (rejection must not corrupt resp's header list). */
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_bare_cr_lf_guards_handler(chttpsvr_req *req,
                                                  chttpsvr_resp *resp,
                                                  void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* A lone CR with no LF after it. strpbrk(x, "\r\n") must catch a single
   * byte that is not allowed. It must not catch only the combined "\r\n"
   * pair, which the resp_set_header_crlf_injection_rejected test above
   * already covers. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "x-bare-cr", "v\rinjected");
  /* Same guard, the other single disallowed byte: a lone LF with no
   * paired CR. */
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-bare-lf", "v\ninjected");
  /* A legitimate header must still work fine after both rejections above
   * (rejection must not corrupt resp's header list). */
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_disallowed_name_guards_handler(chttpsvr_req *req,
                                                       chttpsvr_resp *resp,
                                                       void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* An empty name has no valid on-the-wire representation. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "", "v");
  /* "Transfer-Encoding" must be rejected outright, lower-case ... */
  results[1] =
      (int)chttpsvr_resp_set_header(resp, "transfer-encoding", "chunked");
  /* ... and mixed-case, since the check is case-insensitive. */
  results[2] =
      (int)chttpsvr_resp_set_header(resp, "Transfer-Encoding", "chunked");
  /* A legitimate header must still work fine after all three rejections
   * above (rejection must not corrupt resp's header list). */
  results[3] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

static void _set_header_non_tchar_name_guards_handler(chttpsvr_req *req,
                                                      chttpsvr_resp *resp,
                                                      void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  /* A space inside the name. The name holds no CR and no LF, so a check
   * for CR and LF only lets it through. But it is not a real RFC 7230
   * token of tchar bytes. The name "X Foo: bar" puts "X Foo:bar:baz\r\n"
   * on the wire. That is not a real split into two fields. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "X Foo", "bar");
  /* A literal colon inside the name, for the same reason. */
  results[1] = (int)chttpsvr_resp_set_header(resp, "X:Foo", "bar");
  /* A byte that is not a CR, not an LF, and not a printable byte outside
   * the tchar set. A bare NUL cannot happen, because the name is a C
   * string that ends with a NUL. This case therefore uses a DEL byte
   * (0x7F), which is also outside the tchar set. */
  results[2] = (int)chttpsvr_resp_set_header(resp,
                                             "X\x7F"
                                             "Foo",
                                             "bar");
  /* A legitimate header (including one using every non-alnum tchar byte
   * RFC 7230 SS3.2.6 permits) must still work fine after all three
   * rejections above. */
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

/* Reports the counts of the values for the keys "a", "b" and "". It
   reports them in that order and separates them with a comma. This
   verifies that the parser skips a stray '&' in the raw query string, and
   also two '&' characters together. The parser must not read them as a
   false entry with an empty key and an empty value. */
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

/* Streaming GET handler that reads the body with chttpsvr_req_body().
   When a GET carries no body, the function must return NULL and len=0. */
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

/* Pair of handlers for the test of root-router shadowing. Both of them are
   registered on /shadow-test/ping. The root handler is registered directly
   on g_srv, and the server always checks it first. The sub-router handler
   is registered on a /shadow-test sub-router. The server checks that one
   second and never reaches it. */
/* Marks its response so a test can tell which of the two routes registered
   for /head-pref (a CHTTP_GET one and a CHTTP_HEAD one) actually ran. */
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

/* Echoes the method name of the request so tests can verify which method was
   dispatched when a CHTTP_ANY route is matched. */
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

/* A dedicated server with a bounded pool of 1 thread and queue_capacity=1.
   _setup starts it. It verifies that a full ctpool makes ctpool_try_submit
   return ccol_container_full, and that the server then answers with a 503.
   This server listens on TEST_PORT+2. */
static chttpsvr g_bounded_srv = CHTTPSVR_INVALID;

/* Dedicated server with a small max_body_size (64 bytes) for boundary tests
   of the 413/PAYLOAD_TOO_LARGE enforcement (buffered and streaming).
   Listening on TEST_PORT+3. */
static chttpsvr g_small_body_srv = CHTTPSVR_INVALID;
#define SMALL_BODY_MAX 64

/* Mutex and condition variable that synchronize the handler that blocks in
   the 503 test. The test sends two HTTP requests at the same time. Both of
   them block inside _bounded_blk_handler and fill the pool of 1 thread and
   the queue of 1 slot. The test then sends a third request, which must get
   a 503. */
static pthread_mutex_t g_blk_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_blk_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_blk_count = 0; /* handlers currently blocking */
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

/* The client of bounded_pool_full_returns_503. Its own pool has room for
 * every request of the test: the default client allows one connection for
 * each CPU, so on a machine with two CPUs the two requests that block in the
 * handler would hold every slot and the third one would wait for ever. */
static chttpcli g_bounded_cli = CHTTPCLI_INVALID;

/* Sends one GET /bounded-503 to g_bounded_srv through g_bounded_cli. */
static chttpcli_response *_bounded_get(void) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bounded-503", TEST_PORT + 2);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  if (req) chttpclient_do(g_bounded_cli, req, &resp);
  chttp_request_free(req);
  return resp;
}

/* Thread entry: send one GET /bounded-503 to g_bounded_srv and discard the
   response.  Used to fill pool slots from background threads. */
static void *_send_bounded_req(void *arg) {
  (void)arg;
  chttpcli_response *resp = _bounded_get();
  if (resp) chttpclient_resp_free(resp);
  return NULL;
}

/* Thread entry: send one GET to an unmatched route on g_srv (a guaranteed
   404) and discard the response. Used to fire a concurrent burst of route
   rejections; see concurrent_route_rejections_do_not_starve_other_requests. */
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
  /* Unblock any handlers still blocking inside _bounded_blk_handler so that
   * g_bounded_srv can drain its in-flight requests cleanly. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  /* Stop all listeners first so no new requests are accepted. */
  if (g_bounded_srv) chttpsvr_stop(g_bounded_srv);
  if (g_small_body_srv) chttpsvr_stop(g_small_body_srv);
  if (g_srv) chttpsvr_stop(g_srv);
  if (g_srv2) chttpsvr_stop(g_srv2);

  /* Destroy each server. Each destroy drains the in-flight requests and
   * frees the single shared-engine reference of that server. The shared
   * ccol_event_loop reactor of chttpserver stops asynchronously, on a
   * joinable reaper thread that is fully independent of the engine of
   * chttpclient. The last destroy does not join that thread inline. This
   * function is an atexit handler, so chttpsvr_engine_wait() below must
   * block until the reactor really stops. Without that wait, a reactor
   * thread can still use the logger that _setup installs in the engine
   * with chttpsvr_set_engine_logger (g_test_logger) when this function
   * closes it just below. */
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
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_INVALID;
  }
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  /* Logger for the engine and both server instances. */
  g_test_logger = clog_open_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  /* Create and configure the test server. */
  g_srv = ccol_create_chttpsvr(g_test_logger, &err);
  if (!g_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n", err ? err : "?");
    exit(1);
  }

  /* Global middleware (active for ALL routes). */
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
  /* /echo-path/{v} reuses _path_handler to test URL-decoding of req->path. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/echo-path/{v}", _path_handler,
                            NULL);

  /* Buffered route that exercises chttpsvr_req_read(); must return -1. */
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/req-read-buffered",
                            _read_on_buffered_handler, NULL);

  /* Routes with more than one method. The same path is registered for both
     GET and POST. This verifies that the server can route each one on its
     own. _find_route must continue the search past a path match whose
     method does not match. A stop at the first path match gives a POST a
     405, because the GET route matched first. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dual", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/dual", _echo_body_handler,
                            NULL);

  /* Routes for additional coverage tests. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-header",
                            _dup_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/bad-status",
                            _bad_status_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-key",
                            _query_one_null_key_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-val-out",
                            _query_one_null_val_out_handler, NULL);

  /* Duplicate route: same method+path registered twice.  First handler wins. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
                            _dup_second_handler, NULL);

  /* Routes for null-guard HTTP round-trip tests. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-name-header",
                            _null_name_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-key-query",
                            _null_key_query_handler, NULL);

  /* Streaming routes. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-echo",
                                      _stream_echo_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-header",
                                      _stream_header_handler, NULL);
  /* Streaming route that reads the body with chttpsvr_req_body() and not
   * with req_read. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-body-api",
                                      _stream_body_check_handler, NULL);
  /* Streaming route exercising chttpsvr_req_body() AFTER chttpsvr_req_read()
   * has already been used on it; see _stream_body_after_read_handler's own
   * doc comment. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST,
                                      "/stream-body-after-read",
                                      _stream_body_after_read_handler, NULL);
  /* Streaming GET route that calls chttpsvr_req_body() on a request with
   * no body. It exercises the else branch of (body && len > 0). It must
   * return "(empty)". It must not crash and must not give undefined
   * output. */
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
  /* Streaming routes for chttpsvr_req_read edge-case coverage. */
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

  /* Sub-router for /api/v1 with its own middleware. */
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

  /* Sub-router with blocking middleware. */
  chttpsvr_router *blocked_r = chttpsvr_subrouter(g_srv, "/blocked-area");
  if (!blocked_r) {
    fprintf(stderr, "FATAL: could not create blocked subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(blocked_r, _blocking_mw, NULL);
  chttpsvr_router_on(blocked_r, CHTTP_GET, "/secret", _hello_handler, NULL);

  /* Sub-router with two chained middlewares to verify both run in order. */
  chttpsvr_router *mw_chain = chttpsvr_subrouter(g_srv, "/mw-chain");
  if (!mw_chain) {
    fprintf(stderr, "FATAL: could not create mw-chain subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(mw_chain, _mw_a, NULL);
  chttpsvr_router_use(mw_chain, _mw_b, NULL);
  chttpsvr_router_on(mw_chain, CHTTP_GET, "/ping", _hello_handler, NULL);

  /* Sub-router registered with a trailing-slash prefix; the server normalises
     it to "/api/v3" so routing behaves identically to a clean prefix. */
  chttpsvr_router *api_v3 = chttpsvr_subrouter(g_srv, "/api/v3/");
  if (!api_v3) {
    fprintf(stderr, "FATAL: could not create /api/v3/ subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(api_v3, CHTTP_GET, "/ping", _hello_handler, NULL);

  /* Routes for tests that would otherwise register routes mid-test.
   * Registering them here guarantees each route exists exactly once for the
   * lifetime of the process and avoids cross-test contamination from
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

  /* "/" sub-router: only the exact root path "/" is matched.  See the
   * IMPORTANT edge-case note in chttpserver.h for the routing mechanic. */
  chttpsvr_router *root_r = chttpsvr_subrouter(g_srv, "/");
  if (!root_r) {
    fprintf(stderr, "FATAL: could not create / subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(root_r, CHTTP_GET, "/", _root_subrouter_handler, NULL);

  /* Routes for explicit_head_route_wins_over_get_route. The CHTTP_GET
   * route is registered FIRST on purpose. The fallback where a GET serves
   * a HEAD finds it first. A result that depends on the order of the
   * registrations therefore picks it, and not the explicit CHTTP_HEAD
   * route that comes next. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/head-pref", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_HEAD, "/head-pref",
                            _explicit_head_handler, NULL);

  /* Routes for the subrouter_wins_over_root_route_at_same_path test. They
   * are registered here and not in the body of the test. They therefore
   * exist exactly once for the life of the process, and the test does not
   * change g_srv while it runs. */
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

  /* Sub-router for the middleware-overflow-produces-500 test.
   * g_srv has 1 global middleware (_global_mw).  Registering 32 middlewares on
   * this router brings the combined dispatch-time count to 33 >
   * _CHTTPSVR_MAX_MW (32), so every request to /mw-overflow-live/<any> must
   * receive a 500. */
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

  /* Catch-all: any method is dispatched to _any_method_handler. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method",
                            _any_method_handler, NULL);

  /* Catch-all with a path parameter. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method/{id}",
                            _any_method_param_handler, NULL);

  /* Specific GET registered BEFORE CHTTP_ANY on the same pattern.
     GET uses _hello_handler; all other methods fall through to CHTTP_ANY. */
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-with-specific",
                            _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-with-specific",
                            _any_method_handler, NULL);

  /* CHTTP_ANY registered BEFORE a specific GET on the same pattern.
     First-wins: CHTTP_ANY wins for every method including GET. */
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-first", _any_method_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-first", _hello_handler,
                            NULL);

  /* Second server instance (not started) for route-registration edge-case
   * tests and double-start rejection tests. */
  g_srv2 = ccol_create_chttpsvr(g_test_logger, NULL);

  /* Route engine logger to g_test_logger before the first chttpsvr_start. */
  chttpsvr_set_engine_logger(g_test_logger);

  /* Start the primary test server.  4 worker threads with default (unbounded)
   * queue so streaming and buffered handlers both run on the server's ctpool.
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
  /* No readiness poll needed: the first chttpsvr_start blocks until the
   * engine fires FIO_CALL_ON_START (port bound, reactor in event loop). */

  /* Create and start the bounded server for the bounded_pool_full_returns_503
   * test.  1 worker thread + queue_capacity=1 so total capacity = 2 (1 active
   * + 1 queued).  A third concurrent request must receive 503. */
  g_bounded_srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
    /* The routes of this server are streaming routes, and the tests that use
     * it fill the WORKER pool. Streaming handlers therefore share that pool
     * here, instead of running on a streaming pool of their own. */
    bcfg.streaming_thread_count = CHTTPSVR_STREAMING_POOL_OFF;
    /* Short enough that a client which stops sending mid-body triggers a
     * timeout quickly in tests, without affecting /bounded-503 (which never
     * calls chttpsvr_req_read). */
    bcfg.stream_read_timeout_us = 300000;
    ccol_retval_t brv = chttpsvr_start(g_bounded_srv, &bcfg);
    if (brv != ccol_success) {
      fprintf(stderr, "FATAL: bounded chttpsvr_start failed: %d\n", brv);
      exit(1);
    }
  }
  /* This code needs no poll for readiness. http_listen binds the socket
   * synchronously on a later chttpsvr_start, because the engine already
   * runs. */

  /* Create and start the small-max_body_size server for the
   * max_body_size/413 boundary tests (both buffered and streaming). */
  g_small_body_srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

/* Forward declaration; defined in the RAW SOCKET HELPER section below. */
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
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
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
  /* A '+' in a URL path segment must stay a literal '+' in the captured
   * parameter value. The encoding rules for a path segment (RFC 3986) give
   * '+' no special meaning. Only application/x-www-form-urlencoded, which
   * query strings use, maps '+' to a space. The match of a segment and the
   * capture of a parameter therefore both decode with
   * http_decode_path_unsafe. They never decode with
   * http_decode_url_unsafe, which uses the query-string rules. That
   * function turns '+' into ' ' and gives values that disagree with
   * chttpsvr_req_path. */
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
  /* /hello is registered for GET only; POST should be 405. */
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
  /* x-global-mw header is injected by _global_mw. */
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
     percent-encoded. It must still match the /api/v1 sub-router in exactly
     the same way as the request without the encoding. A registration of
     the same effective pattern at the root level matches it. The server
     decodes the segments of a route pattern before it compares them. The
     prefix of a sub-router must be aware of the encoding in the same way.
     It must not be a raw comparison byte for byte. */
  chttpcli_response *resp = _get("/%61pi/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "item:99");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_root_matches) {
  /* Same as above but for the prefix-only path (no trailing route
     segments); /api/v1/ vs /%61pi/v1/. */
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
  /* Both global and router middleware should have run. */
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
  /* /blocked-area/secret uses _blocking_mw which does not call next. */
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
  /* Raw query is the un-decoded query string. */
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
  /* Starting an already-started server instance must fail with not_permitted.
   * g_srv is started by _setup; attempting to start it a second time must be
   * rejected regardless of the port chosen. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval_t rv = chttpsvr_start(g_srv, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

TEST(chttpserver, multi_server_start_stop) {
  /* A second server on a different port must start correctly. It must also
   * serve its requests on its own. This shows that the library supports
   * more than one server.
   *
   * g_srv2 is a shared fixture handle with a long life. Several other
   * tests in this file use it again, and this test does not own it
   * locally. The test therefore cannot give it an RAII scope here. Every
   * outcome below goes into a local instead. The test then calls
   * chttpsvr_stop(g_srv2) without a condition, before any REQUIRE_* that
   * can return early. A real regression in the subject of this test is the
   * start and the service of a second concurrent server. That regression
   * must not leave the listener of g_srv2 alive for every later test in
   * this binary to trip over. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  chttpsvr_register_handler(g_srv2, CHTTP_GET, "/ping-srv2", _hello_handler,
                            NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  /* No readiness poll: http_listen binds the socket synchronously. */

  bool resp1_ok = false, body1_ok = false;
  int status1 = -1;
  char body1_copy[128] = {0};
  bool resp2_ok = false;
  int status2 = -1;

  if (rv == ccol_success) {
    /* Request to srv2 must succeed. */
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

    /* Verify g_srv (on TEST_PORT) is unaffected. */
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
  /* chttpsvr_req_query_one returns the value when exactly one pair exists. */
  chttpcli_response *resp = _get("/query-one?q=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "only");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_404_outside_prefix) {
  /* /api/v2/ is not registered so should be 404. */
  chttpcli_response *resp = _get("/api/v2/items/1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, large_body_post) {
  /* POST a 64 KiB body and verify echo. */
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
  /* chttpsvr_req_query_one must report ccol_not_permitted for multi-value keys.
   */
  chttpcli_response *resp = _get("/query-one?q=a&q=b");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "multi_value");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_missing) {
  /* chttpsvr_req_query_one must report ccol_key_not_found when key is absent.
   */
  chttpcli_response *resp = _get("/query-one");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_plus_decoded_to_space) {
  /* '+' in a query value must decode to a space character. */
  chttpcli_response *resp = _get("/query?q=hello+world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_percent_decoded) {
  /* Percent-encoded characters in a query value must be decoded. */
  chttpcli_response *resp = _get("/query?q=hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_present) {
  /* chttpsvr_req_header reads conn->hdr_names/hdr_values, the same flat
     header array populated for every request regardless of route kind;
     this confirms it works identically for a streaming route. */
  chttpcli_response *resp =
      _get_with_header("/stream-header", "x-stream-test", "stream-val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "stream-val");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_absent) {
  /* Absent header on a streaming route returns NULL from chttpsvr_req_header.
   */
  chttpcli_response *resp = _get("/stream-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, stream_handler_registration_accepted) {
  /* chttpsvr_register_streaming_handler must accept a valid registration on
     an unstarted server.  The server owns its ctpool, so no pool is passed by
     the caller. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, subrouter_stream_route) {
  /* A streaming route registered on a sub-router must receive the body,
     run through global + router middleware, and echo the response. */
  const char *payload = "api-stream-payload";
  chttpcli_response *resp = _post("/api/v1/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, payload);
  /* Global middleware must still run on streaming sub-router routes. */
  const char *gv = chttpclient_resp_header(resp, "x-global-mw");
  REQUIRE_TRUE(gv != NULL);
  REQUIRE_STREQ(gv, "1");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_method_not_allowed) {
  /* A method that does not match any registered route on a sub-router whose
     path does match must return 405, not 404. */
  chttpcli_response *resp = _post("/api/v1/items/1", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 405);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_mw_in_chain_runs_both) {
  /* Two middlewares registered in sequence on the same sub-router must both
     run and must run in registration order (x-mw-a first, x-mw-b second). */
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
  /* A route pattern with an unclosed '{' must be rejected at registration time
     with ccol_invalid_args rather than silently extracting a wrong param name.
     g_srv2 is used because it is never started; adding invalid routes to it
     does not affect the running test server. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{unclosed",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* Well-formed patterns must still be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{valid}/end",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, invalid_param_name_chars_rejected) {
  /* The server must reject a route pattern at registration time when its
   * {name} segment holds a character outside [A-Za-z0-9_]. It must reject
   * it with ccol_invalid_args. chttpsvr_req_param can never read such a
   * name back, so the pattern creates parameters that nothing can reach.
   * This test uses g_srv2, which nothing starts, so it does not pollute
   * the test server that runs. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  /* Space inside param name. */
  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/{hello world}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Hyphen inside param name. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{user-id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Plus inside param name. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{a+b}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Names that contain only [A-Za-z0-9_] must still be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{userId}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{order_id}/items",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, duplicate_param_name_in_pattern_rejected) {
  /* The server must reject a pattern that uses the same {name} more than
   * once, for example /a/{id}/b/{id}. It must reject it at registration
   * time with ccol_invalid_args. Without this check, the route compiles
   * correctly. Nothing can then reach the value that the SECOND use
   * captures, because chttpsvr_req_param always returns on the first match
   * of the name. This test uses g_srv2, which nothing starts, so it does
   * not pollute the test server that runs. */
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

  /* A pattern with distinct param names, even reusing the same literal
   * segment shape, must still be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/dup/{id}/b/{other_id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                    MULTI-METHOD SAME-PATH TESTS                            */
/* ========================================================================== */

TEST(chttpserver, multi_method_get) {
  /* GET /dual must reach the GET handler. This is true although POST /dual
     is registered too. _find_route matches on the path AND the method
     together. This property therefore holds whatever the order of the two
     registrations is. It is not an accident of that order. */
  chttpcli_response *resp = _get("/dual");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_method_post) {
  /* POST /dual must reach the POST handler. This is true although GET
     /dual is registered first. A _find_route that stops at the first path
     match breaks this. The GET route matches the path, the method check
     fails and the search ends there. The client then gets a 405 and not
     the response of the POST handler. */
  chttpcli_response *resp = _post("/dual", "dual-body", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "dual-body");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_method_unregistered_405) {
  /* A method not registered for /dual must still return 405, not 404. */
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

/* RFC 9110 SS15.5.6: a 405 carries an Allow header that lists the methods
   that the target resource supports. /dual is registered for GET and POST,
   and the GET route also serves HEAD. */
TEST(chttpserver, method_not_allowed_lists_the_allowed_methods) {
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:GET, HEAD, POST\r\n") != NULL);
}

/* /echo-body is registered for POST alone, so HEAD is not allowed and the
   header names POST alone. */
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
  /* The server must reject a route pattern that holds two slashes
     together, for example /foo//bar. It must reject it at registration
     time with ccol_invalid_args. A quiet change of such a pattern to
     /foo/bar surprises the caller and causes errors. This test uses
     g_srv2, which nothing starts. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo//bar",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* A leading double-slash (after stripping one) must also be rejected. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "//leading", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  /* A well-formed pattern must still succeed. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                         NEW COVERAGE TESTS                                 */
/* ========================================================================== */

TEST(chttpserver, multi_write_accumulates) {
  /* Multiple chttpsvr_resp_write_str calls on the same response must
     concatenate into a single body. */
  chttpcli_response *resp = _get("/multi-write");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "foobarbaz");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, printf_accumulates) {
  /* chttpsvr_resp_printf must format in the same way as printf. It must
     also add to the body across several calls. One call gives a formatted
     output that is long enough to force the heap fallback path inside
     chttpsvr_resp_printf. */
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
  /* chttpsvr_req_path() must return the URL-decoded path, not the raw
     percent-encoded form received from the client. */
  chttpcli_response *resp = _get("/echo-path/hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "/echo-path/hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_literal_segment_is_route_mismatch) {
  /* A path segment can hold a NUL byte that comes from a %00 escape. The
   * match of a literal segment uses strcmp. That match must not read
   * "hello" plus the garbage after it as an exact match for the registered
   * literal segment "hello". /hello%00xyz must NOT match the literal route
   * /hello. Without the guard against an embedded NUL, the literal-segment
   * comparison of _match_route_cached decodes "hello%00xyz" to
   * "hello\0xyz". It then compares that with "hello" through strcmp.
   * strcmp stops at the first NUL in either operand and reports a match. A
   * suffix that an attacker chooses can then hide behind a route match.
   * The caller expects that match to cover the whole segment. */
  chttpcli_response *resp = _get("/hello%00xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_param_segment_is_route_mismatch) {
  /* The same class of hazard, through a {param} segment and not a literal
   * one. _seg_cache_get must treat an embedded NUL in the decoded value as
   * a decode failure. That is the same class as a malformed %XX escape. It
   * must not hand a truncated value to a match. */
  chttpcli_response *resp = _get("/echo-path/hello%00world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_query_key_does_not_alias) {
  /* Same class of hazard in the query-string decoder: a key "q%00x" must
   * not decode-then-strcmp its way into aliasing the unrelated key "q".
   * Without the guard, _parse_qparams stores the fully-decoded "q\0x" and
   * chttpsvr_req_query_one's strcmp(qp->keys[i], "q") stops at the embedded
   * NUL and reports a spurious match. */
  chttpcli_response *resp = _get("/query-one?q%00x=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query) {
  /* A call to chttpsvr_req_query for two different keys in one request
     must return correct values for both keys. This is true although the
     two calls share the scratch _qresult array and use it again. */
  chttpcli_response *resp = _get("/multi-query?a=hello&b=world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=hello b=world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query_missing) {
  /* Absent keys must yield the nil sentinel regardless of call order. */
  chttpcli_response *resp = _get("/multi-query?a=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=only b=nil");
  chttpclient_resp_free(resp);
}

/* A pass-through allocator that counts its calls. It exercises
 * ccol_create_chttpsvr_mp. It also lets the test prove that the procs that
 * the caller gives handle every allocation and every free. The default
 * allocator of the library must handle none of them. A create and a
 * destroy that merely succeed do not prove this. */
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
  /* ccol_create_chttpsvr_mp must succeed with a custom allocator. It must
     accept route registrations. On destroy it must free all the memory
     through the same allocator. This path of success exercises the same
     copy of m_procs that the failure path of ccol_mutex_init depends on.
     The test counts the calls to the wrapper and does not use bare
     pass-through functions. The counts prove that ccol_create_chttpsvr_mp
     gives the supplied procs to every place that allocates. They prove
     that it does not fall back to the default of the library. */
  g_wrap_malloc_count = 0;
  g_wrap_free_count = 0;
  g_wrap_calloc_count = 0;
  g_wrap_realloc_count = 0;

  ccol_memmgmt_procs_t mp = {_wrap_malloc, _wrap_free, _wrap_calloc,
                             _wrap_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  /* This test drives a create and three route registrations. The same
     wrapper must free every one of those allocations before destroy
     returns. A custom allocator that the library ignores leaves
     g_wrap_free_count at 0 here. */
  REQUIRE_GT(g_wrap_free_count, (size_t)0);
  REQUIRE_GE(g_wrap_free_count, allocs_before_destroy);
}

/* ========================================================================== */
/*                         ADDITIONAL COVERAGE TESTS (NEW)                    */
/* ========================================================================== */

TEST(chttpserver, subrouter_root_no_trailing_slash) {
  /* /api/v1, with no slash at the end, must match the "/" route on the
     /api/v1 sub-router. _find_route changes the empty sub-path to "/"
     before it matches. Both /api/v1 and /api/v1/ must therefore reach
     _api_root_handler. */
  chttpcli_response *resp = _get("/api/v1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, long_path_heap_alloc) {
  /* A URL path longer than 511 bytes triggers the heap allocation branch in
     _on_request (instead of the 512-byte stack buffer).  The /echo-path/{v}
     route echoes the decoded path back; a 510-char param gives a 521-char
     path which exceeds the threshold. */
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

extern size_t _chttpsvr_reject_pool_task_count_for_tests(void);

/* A thread of reject_pool increments g_reject_task_run_count_for_tests.
   That increment happens AFTER the server writes the rejection response to
   the client and closes the connection. The comment of _reject_task
   explains why the release of in_flight_requests comes last, and this
   counter goes up with it. A client that just read its response can
   therefore get control back in the test before that increment runs. This
   is a harmless scheduling race, and not an order that the library
   promises. This function polls for the expected delta and does not assert
   at once. That matches the pattern that this codebase uses for this class
   of timing after an asynchronous completion. See the wait loop for
   g_mm_free_count in tests_mem_mgmt.c. The function returns the count that
   it saw, so the caller can still assert on it. */
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

/* Parses an IMF-fixdate of RFC 9110 SS5.6.7, such as
   "Sun, 06 Nov 1994 08:49:37 GMT", strictly and without the locale. The
   weekday must agree with the date. */
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

/* Walks every response in buf. For each one it counts the Date header
   lines of the header block, and checks that each parses and lies within
   max_skew seconds of now, when now is not 0. It returns the number of
   responses whose header block holds exactly one such line, and it reports
   the total number of responses in *total. */
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

/* Returns the value of the one Date header of the first response in buf, in
   out, or false when there is not exactly one. */
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
  /* ctpool_try_submit returns ccol_container_full when the ctpool of the
   * server is full. The server must then answer with a 503.
   *
   * g_bounded_srv has 1 worker thread and queue_capacity=1. Its total
   * capacity is therefore 2: one active task and one queued task. This
   * test sends two background HTTP requests that block inside
   * _bounded_blk_handler and fill both slots. It then sends a third
   * request, which must get a 503.
   *
   * The white-box counter g_reject_task_run_count_for_tests also confirms
   * that a reject_pool thread wrote the 503. It confirms that the
   * synchronous last-resort fallback did not write it. A black-box client
   * cannot tell the two apart, because both put the same response on the
   * wire. The counter counts for the whole process and not for one server.
   * But tau runs one test at a time on this thread. Nothing else in this
   * process rejects a connection by itself, and the idle sweep and the
   * handlers of other servers do not. A plain delta from before to after
   * across the window of this test is therefore clear.
   *
   * Every outcome below goes into a local. No REQUIRE_* asserts on it at
   * once. The test releases and joins every background thread WITHOUT A
   * CONDITION, before any REQUIRE_* runs at all. g_bounded_srv has only
   * ONE worker thread. A REQUIRE_* that returns early from this function
   * before that release leaves that one worker blocked inside
   * _bounded_blk_handler for the rest of the life of this process. Nothing
   * else in this test binary unblocks it before the process exits. Every
   * later test that reaches g_bounded_srv then waits behind a worker that
   * can never make progress. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();
  g_bounded_cli = ccol_create_chttpclient(NULL);
  bool cli_ok = g_bounded_cli != CHTTPCLI_INVALID &&
                chttpclient_set_pool_size(g_bounded_cli, 8) == ccol_success;

  /* Reset state from any previous run of this test. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  g_blk_count = 0;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Request 1: fills the active worker slot. */
  pthread_t t1;
  bool t1_created =
      cli_ok && pthread_create(&t1, NULL, _send_bounded_req, NULL) == 0;

  /* Spin until the worker is actively running the handler (blocking).
   * Bail after 5 s so a stalled environment fails rather than hanging. */
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
        break; /* timed out; worker_blocked stays false */
      nanosleep(&nap, NULL);
    }
  }

  /* Request 2: fills the single queue slot.  Worker is blocked, so this
   * request stays queued until g_blk_go is set. */
  pthread_t t2;
  bool t2_created = false;
  if (worker_blocked) {
    t2_created = pthread_create(&t2, NULL, _send_bounded_req, NULL) == 0;
    if (t2_created) {
      /* Give Request 2 time to traverse the network stack and enter the
       * queue. Worker is blocked so Request 2 cannot start executing; 50 ms
       * is generous for a local loopback round-trip. */
      struct timespec queue_wait = {0, 50000000L}; /* 50 ms */
      nanosleep(&queue_wait, NULL);
    }
  }

  /* Pool is now full (1 active + 1 queued).  Third request must get 503. */
  int status = -1;
  bool got_resp = false;
  bool date_ok = false;
  size_t reject_count_after = reject_count_before;
  if (worker_blocked && t2_created) {
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

  /* Release the handlers that block, and join every background thread that
   * started. Do this before any REQUIRE_* below runs. The opening comment
   * of this test explains why this order is load-bearing and not only
   * tidy. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  if (t1_created) pthread_join(t1, NULL);
  if (t2_created) pthread_join(t2, NULL);
  if (g_bounded_cli != CHTTPCLI_INVALID) chttpclient_destroy(g_bounded_cli);
  g_bounded_cli = CHTTPCLI_INVALID;

  /* Reset g_blk_go for a possible rerun, for example the safety broadcast
   * of _teardown. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Every background thread is now guaranteed joined and the shared worker
   * guaranteed unblocked; safe to fail from here on. */
  REQUIRE_TRUE(t1_created);
  REQUIRE_TRUE(worker_blocked);
  REQUIRE_TRUE(t2_created);
  REQUIRE_TRUE(got_resp);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(date_ok);
  REQUIRE_EQ(reject_count_after, reject_count_before + 1);
}

TEST(chttpserver, stop_on_unstarted_server_is_safe) {
  /* chttpsvr_stop on a server with started==false must be a safe no-op.
   * The caller can call it many times and it must not crash. A server has
   * started==false when nothing ever started it. It also has it when an
   * earlier chttpsvr_stop() call put it in that state. _setup does not
   * start g_srv2. But at this point in the order of the suite,
   * multi_server_start_stop already started it and stopped it once. In
   * both cases started==false here, and that is the one property that this
   * test depends on. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2); /* must not block or crash */
  chttpsvr_stop(g_srv2); /* second call: must also be a no-op */
}

/* ========================================================================== */
/*                         RAW SOCKET HELPER                                  */
/* ========================================================================== */

/* Sends a hand-made HTTP/1.1 request over a raw TCP socket. It returns the
   HTTP status code, or -1 on a socket error. extra_headers must already
   hold the \r\n at the end of each header line, or it must be NULL. This
   function writes the full raw response into buf[0..buf_sz-1]. That
   response is the status line, the headers and the body. Use it for cases
   that the chttpclient abstraction cannot express. One such case is a
   header name that the request sends twice, which exercises the documented
   "last occurrence wins" behaviour of chttpsvr_req_header. */
static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* This bounds the read loop below. A regression can make the server
     never respond and never close. One example is a server that reads a
     body that the client declares but never sends, before it rejects an
     unmatched route. That is the class of bug that
     unmatched_route_rejected_without_reading_body catches. Without this
     bound, the read(2) call blocks forever. The caller of this helper then
     cannot fail its own assertion cleanly. Every caller of this shared
     helper gets this protection, not only the one named here. */
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
  /* snprintf returns the number of bytes that WOULD have been written,
   * which may exceed sizeof(req) on truncation.  Clamp to the actual
   * buffer length so the write loop does not read past the array. */
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

/* Connects and sends a request. It writes the body of that request in
   several chunks with a delay between them. The server must therefore see
   separate reads off the socket, one after the other. It must not see one
   blob that arrived in a single recv. The function then reads the full
   response. It returns the HTTP status code, or -1 on a socket failure. It
   always closes after one response. */
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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* See the same comment in _raw_request. A regression can make the server
     never respond. Without this bound, the final read loop below blocks
     forever. The caller then cannot fail its own assertion cleanly, and
     the whole binary hangs. One test must fail instead. */
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

/* Connects and sends headers that declare a Content-Length much larger
   than the bytes that it writes. It writes only `sent_len` bytes of the
   body. It then closes the socket at once and does not wait for a response
   or read one. This simulates a client that aborts in the middle of an
   upload. The function returns 0 when the connect and the write succeed,
   and -1 on a socket failure. The caller has no response to look at,
   because this function tears the connection down on purpose. */
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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
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
  close(fd); /* abort: never send the rest of the declared body */
  fd = -1;
  return 0;
}

/* The same shape as _raw_request_abort_mid_body above: connect, then send
   part of the body. But this function half-closes the write side with
   shutdown(fd, SHUT_WR) and does not close the whole socket. It then reads
   back the response that the server sends before it closes. A full close
   gives the server a connection reset. This half-close instead gives the
   server a clean EOF from read() or ctls_conn_read() in the middle of the
   body. That is the truncation path of chttpsvr_req_read(), where n == 0
   arrives before the message finishes its framing. The caller can
   therefore look at the response and does not race a connection that is
   already gone. The function returns 0 when the connect, the write and the
   read succeed, and buf then holds the raw response with a NUL at the end.
   It returns -1 on a socket failure. */
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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* See the same comment in _raw_request. A regression can make the server
     never respond. Without this bound, the final read loop below blocks
     forever. The caller then cannot fail its own assertion cleanly, and
     the whole binary hangs. One test must fail instead. */
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

/* Reads from fd until EOF, or until buf is full, whichever comes first. It
   puts a NUL at the end of the result. Tests use it to drain one whole raw
   HTTP response off a socket, or several pipelined ones. The server closes
   that socket by itself when it finishes. This function bounds every
   read(2) call with SO_RCVTIMEO. It is the same guard as in _raw_request
   and _read_one_http_response. Without it, a regression that makes the
   server never respond and never close blocks this call forever. The
   caller then cannot fail its own assertion cleanly, and the whole binary
   hangs. One test must fail instead. The function returns the total number
   of bytes that it read, and that number never counts the NUL at the end.
   It does not close fd. */
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

/* Reads exactly one HTTP/1.1 response off a socket that is already
   connected. That response is the headers and a body with a
   Content-Length. The function leaves the connection open for the next
   request. Tests use it for keep-alive across two requests on one
   connection. It assumes a short response that is not chunked, which is
   true for every fixture handler that uses this helper. It returns the
   status code, or -1 on a failure.

   This function bounds every read(2) call below. Without that bound, a
   regression that makes the server never respond and never close blocks
   this call forever. The caller then cannot fail its own assertion
   cleanly, and the whole binary hangs. One test must fail instead. This is
   the same SO_RCVTIMEO guard as in _raw_request above. Every caller of
   this shared helper gets the protection, and not only the one named
   above. The keep-alive tests, the Unix-socket tests, the max_connections
   tests and the self-restart tests of this file all use it. The call does
   nothing for a caller that already set its own SO_RCVTIMEO on fd, because
   setsockopt writes the same option with the same value. */
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

/* Takes the response body out of the raw response that _raw_request wrote,
   and decodes it. It handles a response with a Content-Length and a
   response with Transfer-Encoding: chunked. It decodes the body in place
   inside buf. It returns a pointer to the decoded body, with a NUL at the
   end. It returns NULL when the separator between the headers and the body
   is absent.

   With a Content-Length, the bytes after \r\n\r\n are already the literal
   body. chttpserver always sets Content-Length explicitly, and it never
   chunk-encodes its own responses. See the doc comment of _send_response
   in chttpserver.c. The chunked branch below is therefore a defence, and
   not a path that the responses of this server take. It stays here for two
   reasons. It keeps this helper correct if that behaviour ever changes. It
   also decodes the chunked *request* bodies that the tests of this file
   send, so the file needs no second helper that is almost the same. See
   chunked_body_with_trailer_headers_handled_once for one such test. */
static char *_decode_raw_body(char *buf) {
  char *sep = strstr(buf, "\r\n\r\n");
  if (!sep) return NULL;
  char *body = sep + 4;

  /* Check for chunked encoding by scanning the header section only. */
  char saved = sep[0];
  sep[0] = '\0'; /* temporarily terminate the header block */
  bool chunked = (strcasestr(buf, "transfer-encoding: chunked") != NULL);
  sep[0] = saved;

  if (!chunked) return body;

  /* _raw_request always NUL-terminates its buffer, so buf+strlen(buf) is the
   * safe upper bound for reads. */
  char *end_buf = buf + strlen(buf);

  /* In-place chunked decode:  chunk-size CRLF chunk-data CRLF ... 0 CRLF CRLF
   */
  char *out = body;
  char *p = body;
  while (*p) {
    char *end = NULL;
    unsigned long chunk_size = strtoul(p, &end, 16);
    if (end == p) break; /* malformed; stop */
    if (*end == '\r') end++;
    if (*end == '\n') end++;
    if (chunk_size == 0) break;
    /* Safety: do not read or write past the valid data region. */
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
  /* chttpsvr_req_read() must return -1 on buffered (non-streaming) routes
     because req->is_streaming is false and there is no stream-position
     concept for pre-buffered bodies. */
  chttpcli_response *resp =
      _post("/req-read-buffered", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "-1");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_on_streaming_route) {
  /* chttpsvr_req_body() works for a buffered route only. The server never
     extracts the body of a streaming route ahead of time. It reads that
     body live off the socket with chttpsvr_req_read(), one batch at a
     time. A call to chttpsvr_req_body() on such a route must therefore
     return NULL and 0. It must not hand back a buffered copy. */
  const char *payload = "body-via-api";
  chttpcli_response *resp = _post("/stream-body-api", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_after_req_read_on_streaming_route) {
  /* This is the gap that the is_streaming check of chttpsvr_req_body()
     closes. Without that check, the function returns conn->body.buf and
     conn->body.len as they are. For a streaming route, conn->body is a
     live cursor that chttpsvr_req_read() drains. The server compacts it
     back to empty only when the NEXT batch arrives, and not at once when
     the cursor is empty. The test above calls chttpsvr_req_body() only
     BEFORE any call to chttpsvr_req_read(). It therefore passes by
     accident, because conn->body is still all zero at that point. It does
     not pass because of a real rule for a streaming route. This test calls
     chttpsvr_req_read() first. A regression that drops the guard therefore
     shows up here as a body that is not NULL and a len that is not zero.
     That is whatever chttpsvr_req_read already delivered and left in the
     buffer, and not the documented NULL and 0. */
  const char *payload = "12345678-more-than-eight-bytes";
  chttpcli_response *resp =
      _post("/stream-body-after-read", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  /* chttpsvr_req_read() runs with an 8-byte buffer against a 31-byte
     payload, so it must read something real and give n > 0. This confirms
     that the test exercises the case after a real read. It does not
     exercise a request with no body. */
  REQUIRE_TRUE(strncmp(resp->body, "read=0 ", 7) != 0);
  REQUIRE_TRUE(strstr(resp->body, "body=null len=0") != NULL);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_repeated_header) {
  /* A client can send the same header name twice. _on_header then adds
     both of them to conn->hdr_names and conn->hdr_values, in the order of
     arrival. See the doc comment of that struct. chttpsvr_req_header must
     scan backwards and return the LAST one. The chttpclient abstraction
     overwrites a duplicate header name. This test therefore uses a raw
     socket to send the two literal lines. */
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
  /* A client writes its body in several chunks with a delay between them.
     The chttpsvr_req_read() of the streaming handler must then see more
     than one batch. This proves that the worker thread reads the body live
     off the socket as it arrives. The server does not buffer the whole
     body before the handler starts. */
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
  /* 21 bytes written 4 at a time with a delay between writes must arrive
     as multiple separate reads server-side, not one. */
  REQUIRE_GT(batches, 1);
}

TEST(chttpserver, unmatched_route_rejected_without_reading_body) {
  /* The server routes a request when the headers are complete, before it
     reads any byte of the body. It must reject an unmatched route at once.
     It must do this although the client declares a huge body and never
     sends it. A server that reads the body before it responds makes this
     test hang. The test must return quickly instead. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();

  char buf[4096] = {0};
  int status = _raw_request("POST", "/no-such-route-at-all",
                            "Content-Length: 100000000\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 404);

  /* reject_pool also handles this rejection. Its counter goes up strictly
     after the response is on the wire. See the comment of
     _wait_for_reject_pool_task_count. A wait for that counter here keeps
     the late increment out of the test that tau runs next. Without the
     wait, that next test sees one more than it expects, because this test
     returned before the asynchronous increment arrived. */
  _wait_for_reject_pool_task_count(reject_count_before + 1);
}

TEST(chttpserver, unmatched_route_rejection_routed_through_reject_pool) {
  /* reject_pool handles every rejection, and not only the 503 for a full
     pool. See the assertion on that case in bounded_pool_full_returns_503.
     A client that reads slowly and gets a message about a bad route
     therefore cannot stall the one reactor thread. A black-box client
     cannot tell an answer from reject_pool apart from an answer from the
     synchronous last-resort fallback, because both put the same 404 on the
     wire. This test therefore checks the mechanism directly, with the
     white-box counter g_reject_task_run_count_for_tests. */
  size_t before = _chttpsvr_reject_pool_task_count_for_tests();

  chttpcli_response *resp = _get("/definitely-not-a-registered-route-xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);

  REQUIRE_EQ(_wait_for_reject_pool_task_count(before + 1), before + 1);
}

TEST(chttpserver, concurrent_route_rejections_do_not_starve_other_requests) {
  /* A rejection for an unmatched route is a 404, a 405 or a 500. Such a
     rejection that runs synchronously on the one reactor thread of
     chttpserver is a hazard. A burst of many of them then delays the
     dispatch of every other connection that waits behind them on that same
     thread. g_srv and g_bounded_srv share one reactor for the whole
     process, so this concern is real. Every rejection therefore goes
     through reject_pool. That is a small dedicated pool whose size comes
     from worker_thread_count; see the comment of that pool in
     chttpserver.c. A burst of concurrent rejections must not delay an
     ordinary request that the server handles at the same time. A courtesy
     rejection response is small. It cannot reliably starve the socket
     buffer and force a write to block, and the large body of
     response_write_timeout_closes_slow_reader_connection can. This test
     therefore cannot prove the non-blocking property as tightly as that
     test does. It is a smoke check against gross starvation of the reactor
     thread. It also exercises the multi-threaded submit and dispatch path
     of reject_pool under real concurrency. The single 503 of
     bounded_pool_full_returns_503 never contends with anything else. */
  /* Every outcome below goes into a local. No REQUIRE_* asserts on it at
   * once. The test joins every background thread that started, WITHOUT A
   * CONDITION, before any REQUIRE_* runs. Each of those threads does a
   * full HTTP round trip against g_srv, which lives as long as the
   * process. A REQUIRE_* that returns early from this function while one
   * of them is still in flight leaves it unjoined for the rest of the life
   * of the binary. That thread keeps incrementing the reject-pool task
   * counter of the process. Several OTHER tests in this file need that
   * counter for assertions on an exact delta. */
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

  /* Time a single, ordinary request on the same server while the burst
     above is in flight. Generous bound: this is a starvation smoke test,
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
  /* _on_headers_complete can reject a route and set conn->req_rejected.
     The 404 case here does that. The CHTTP1_USER branch of _conn_pump then
     always calls _conn_close, without a condition. It does not keep the
     connection alive to read a next request. The comment of that branch
     explains why a rejected route gets an error response and still closes.
     This test locks that guarantee in from end to end. The bytes of a
     second, pipelined request can already sit in the same read buffer,
     right behind the rejected one. The server must never read them as the
     body of that first, finished request. It must never read them as a
     second request on the same connection. Exactly one response comes
     back, and the connection closes cleanly. The server must not give a
     second, false response, and it must not hang. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* One write() call sends both requests together. They therefore arrive
     together in the same read buffer on the server side. The first request
     hits an unmatched route, and the server rejects it and never diverts
     it. The second request hits a matched route. A wrong parse swallows
     that second request as the "body" of the first one. */
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

  /* Exactly one response arrives: the 404 for the first request, which the
     server rejected. The connection must then close by itself. The server
     must never use the bytes of the second request as the body of this
     connection, and must never use them as the next message. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_across_two_requests_on_one_connection) {
  /* The test sends two matched requests one after the other on the same
     connection, with no Connection: close. Both of them must succeed. The
     server pauses when the headers are complete, and not after the full
     body. That pause must not break ordinary keep-alive or pipelining. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* A GET here is a request with no body. It finishes its own framing
     right after the headers, with no Content-Length and no chunked
     Transfer-Encoding. Such a request never runs the chttp1_stream_read
     loop of _drain_body at all, because
     chttp1_parser_message_complete() is already true when the worker
     starts. The leftover bytes that chttp1_stream_prepare() gets must
     therefore be reclaimed explicitly. Here those bytes are the ENTIRE
     second, pipelined request of this connection. They are already off the
     wire and gone from the socket buffer of the kernel for good. Leave
     them in the carry-over of the stream, and chttp1_stream_release()
     discards them the moment it runs. The connection correctly stays
     alive, but the bytes of the second request are gone forever. The
     client then hangs and waits for a response that never comes. One
     write() call sends both requests, so they land together in the read
     buffer of the reactor on the server side. That is exactly the shape
     that exercises this path. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* Exactly two full responses arrive. Both of them are a 200, and both
     carry the body of the handler. The connection closes by itself right
     after the second one, because of Connection: close. The client must
     not have to time out and wait for a second response that never
     comes. */
  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

TEST(chttpserver, pipelined_bytes_after_buffered_body_request_not_lost) {
  /* The same contract as pipelined_bodyless_requests_both_answered, for
     the OTHER half of it. This request DOES have a body, so the
     chttp1_stream_read and chttp1_parser_execute loop of _drain_body
     really runs. That loop can still lose a pipelined next request. This
     happens when its own read pulls in bytes past the body boundary of
     this message. chttp1_parser_execute reports how many of the bytes that
     it got it consumed, through chttp1_parser_consumed(). The remainder at
     the end must be captured. Here that remainder is the raw bytes of the
     second request. The same chttp1_stream_read call that returned the
     5-byte body of the first request swept them up. The server must not
     discard them with the rest of that read buffer. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* The same test for a streaming route. chttpsvr_req_read() drives the
     same shape of chttp1_stream_read and chttp1_parser_execute loop that
     _drain_body uses for a buffered route. It therefore needs the same
     treatment: push the bytes back, then reclaim them. It has that
     treatment. Without it, a pipelined next request that arrives on the
     same read as the last bytes of a streamed body is lost in exactly the
     same way. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* Each pipelining test above puts exactly TWO requests into one write().
     Those tests cover a rejected route, two requests with no body, a
     buffered body and a streamed body. They drive _conn_start_diverted and
     _conn_feed_bytes ONCE per connection, from the reactor thread only.
     This test puts the bytes of a THIRD request behind the second one, and
     one read delivers them all together. The keep-alive tail of
     _task_worker must then feed those bytes into _conn_feed_bytes a SECOND
     time. See the comment of that tail on chttp1_stream_take_leftover. It
     does that on the worker thread and not on the reactor thread. The
     headers of that second request complete right there, as they do here.
     _conn_start_diverted therefore runs a second time too, recursively,
     from inside _task_worker itself. It submits to the same worker pool
     again for the bytes of a fourth request, and so on. A test with two
     requests never reaches that recursive hand-off. That is why this test
     exists. A chain of pipelined requests of any length, all delivered in
     one socket read, gets a full answer. The server drops none of them,
     corrupts none of them and duplicates none of them. This is true
     however many times the hand-off must happen. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* Exactly _FIVE_PIPELINED_COUNT complete responses arrive, each one with
     the correct body. Fewer means a dropped request. More means a
     duplicate or a corrupt parse. The connection closes by itself after
     the last one. */
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
  /* This guards against a use-after-free in the keep-alive tail of
     _task_worker. A worker thread can feed the bytes of a further
     pipelined request into _conn_feed_bytes. The reactor thread is not the
     only caller. See the doc comment of _conn_feed_bytes, and the comment
     of this code path in _task_worker. _conn_feed_bytes can settle the
     fate of conn before it returns, and it can free conn outright. Its own
     contract says that the caller must not touch conn again. A follow-up
     request with bad syntax makes chttp1_parser_execute return
     CHTTP1_ERROR. One example is the negative Content-Length case in
     negative_content_length_rejected above. Such a request takes the
     synchronous _conn_close branch of _conn_feed_bytes, on the same
     thread. That branch frees conn every time before _conn_feed_bytes
     returns. The cross-thread divert case frees it only sometimes, but
     this one is deterministic. A read of conn->m_procs right after the
     call, to free a local scratch buffer for example, is therefore always
     a use-after-free on this path. One write() sends both requests. The
     reactor diverts the first, matched, kept-alive request to a worker
     thread from its own first read. The bytes of the second, malformed
     request come along as leftover. They reach _conn_feed_bytes only when
     the tail of the worker thread runs. The reactor thread never touches
     the bytes of the second request at all. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* Two responses arrive, in order. The first is the 200 for the well
     formed request. The second is the 400 for the malformed one, which
     matches the documented behaviour of negative_content_length_rejected.
     The connection then closes by itself. The server process must not
     crash. */
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strncmp(first, "HTTP/1.1 200", 12) == 0);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  char *second = strstr(first + 8, "HTTP/1.1");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strncmp(second, "HTTP/1.1 400", 12) == 0);
  /* No third response: the connection closed after the rejection. */
  REQUIRE_TRUE(strstr(second + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_two_consecutive_streaming_requests) {
  /* _conn_reset_for_request() resets the ingestion state of each message
     between every request on a keep-alive connection. That state is
     conn->body, body_too_large, transfer_aborted, deadline_exceeded,
     _carry_over and conn->parser itself. The reset is needed because one
     chttpsvr_conn_t serves the whole life of the connection. Every other
     keep-alive test in this file pairs at most one streaming request with
     a buffered GET on the same connection. This test sends two full
     streaming POSTs one after the other on one connection. That is the
     case most likely to show stale streaming state from an earlier
     message. One example is a leftover body buffer or an error flag from
     request 1 that corrupts the ingestion of request 2. Each request has a
     different body. The two responses can therefore match only when the
     reset of the ingestion state really happened between them. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* /stream-read-once reads only the first 8 bytes of the body. It then
     returns and does not call chttpsvr_req_read() again. Here one write
     sends the whole 64-byte body, and it arrives on the wire before the
     single read call of the handler runs. The internal pass of
     chttpsvr_req_read, which is chttp1_stream_read plus
     chttp1_parser_execute, therefore parses the ENTIRE declared body into
     conn->body in that one internal call. See the loop of that function in
     chttpserver.c. This happens however few bytes the buffer of the
     handler captured. The server consumes the whole Content-Length, so
     chttp1_parser_message_complete(&conn->parser) is already true before
     the handler returns. _task_worker forces Connection: close only when
     that check is still false at the end of the request. The paired
     ..._with_undrained_body_forces_close test below covers that case. Here
     the check is true, so the connection must stay usable for a second,
     separate request. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Declare and send a 64-byte body; the handler only reads the first 8. */
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

  /* Second, unrelated request on the same connection. */
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
  /* The case above has a body that fully arrives. Here the client sends
     only the first 8 bytes of a declared 64-byte body. That is exactly
     what the single chttpsvr_req_read(req, buf, 8) call of
     /stream-read-once consumes. The content_length of 64 is larger than
     the 8 bytes read when the handler returns, so the body really is
     undrained. The safety net of http1_stream_release must therefore force
     Connection: close; see http1.c. Without that, reuse of this connection
     lets the server read the 56 bytes that never came as the start of a
     new pipelined request. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* A client sends headers that declare more body than it ever delivers,
     and then it stalls. chttpsvr_req_read() must then return -1, and
     chttpsvr_req_stream_error() must give ccol_timed_out.
     stream_read_timeout_us bounds that wait, and the setup of this test
     sets it to 300ms for this server. The call must not block the worker
     thread forever. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 2);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *hdr =
      "POST /stream-error-report-slow HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n"
      "only-ten-"; /* 9 of the declared 100 bytes; never send more */
  REQUIRE_EQ(write(fd, hdr, strlen(hdr)), (ssize_t)strlen(hdr));

  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);
}

TEST(chttpserver, client_disconnect_mid_body_does_not_hang_server) {
  /* A client sends part of its body and then disconnects completely. That
     must not hang the worker thread, and it must not leak the resources of
     the connection. There is no response to check, because the client tore
     the connection down. The test therefore proves that the one worker
     that handled the aborted request came back to the pool.

     This test uses a dedicated local server of its own with one worker. It
     does not use g_srv, which has 4 worker threads. It also does not share
     g_bounded_srv, which has one worker and serves the bounded-503 test
     elsewhere in this file. Here is why. g_srv has 4 workers, so a
     follow-up request still succeeds quickly through one of the other 3
     workers. This is true even when the bug that this test catches comes
     back, where the worker of the aborted connection wedges forever. Such
     a check therefore passes for no reason. The queue_capacity of
     g_bounded_srv is 1, and that OTHER test uses two concurrent blocking
     requests to fill it exactly. There is no room left for the follow-up
     request of this test to race the abort cleanup of the server, so an
     occasional false 503 appears. That effect is real under valgrind,
     where concurrency is heavily serialized and the wakeup of a worker can
     come long after a check of the queue capacity runs. A generous
     queue_capacity here removes that race completely, because this test
     ever uses only two connections. It also keeps the one property that
     makes the test meaningful. With only one worker thread, the follow-up
     below can succeed only when that same worker recovered from the abrupt
     disconnect and returned to the pool. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* This value is short. The test therefore waits only a short time for
     this fallback path to see the abrupt disconnect. That matters when the
     fast detection, which the EOF drives, is ever late. */
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* This read is bounded and not indefinite. The worker can really fail to
     recover. The read then times out, and the REQUIRE below fails cleanly.
     The whole test binary must not hang. */
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
  /* The documentation of chttpsvr_req_stream_error() says that it returns
     ccol_http_transfer_aborted for a closed connection or bad framing in
     the middle of a body. The exit paths of chttpsvr_req_read() for a
     truncated body and for a hard I/O error must therefore record that on
     the connection. A quiet fall-through to ccol_success tells a correct
     streaming handler that a truncated upload was a clean read. For
     example, a POST declares Content-Length: 100, the client sends 20
     bytes and half-closes, chttpsvr_req_read() returns -1, and
     chttpsvr_req_stream_error() reports ccol_success. The server then
     replies 200 OK. The test
     client_disconnect_mid_body_does_not_hang_server above does a full
     close, so no response is left to read back. This test half-closes only
     the write side, so it can look at the response directly. */
  char buf[4096] = {0};
  int rc = _raw_request_truncate_body_half_close(
      "/stream-error-report", "only-part-of-the-declared-body", 1000, buf,
      sizeof(buf));
  REQUIRE_EQ(rc, 0);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_http_transfer_aborted") != NULL);
}

TEST(chttpserver, expect_100_continue_interim_response_sent_before_body) {
  /* The interim "100 Continue" write for conn->expects_continue happens on
     the worker thread, in _task_worker, right before the drain of the body
     starts. It does not happen on the reactor thread, in _conn_pump, when
     the headers finish. A client that reads slowly therefore cannot stall
     the one reactor thread of the server during this write. This test pins
     the wire behaviour that the placement must keep. The interim response
     arrives before the client sends its body. The real final response
     follows after the client sends the body. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

  /* Read the interim "100 Continue" response; it must arrive before we ever
     send the body (proving the body wasn't required to unblock it). */
  char interim[128] = {0};
  size_t got = 0;
  while (got < sizeof(interim) - 1 && strstr(interim, "\r\n\r\n") == NULL) {
    ssize_t r = read(fd, interim + got, sizeof(interim) - 1 - got);
    REQUIRE_GT(r, 0);
    got += (size_t)r;
  }
  REQUIRE_TRUE(strstr(interim, "HTTP/1.1 100 Continue") != NULL);

  /* Now send the body, and confirm the real final response still follows
     correctly on the same connection. */
  REQUIRE_EQ(write(fd, body, strlen(body)), (ssize_t)strlen(body));
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  close(fd);
  fd = -1;
}

/* The handler of a streaming route can reject a request and never call
   chttpsvr_req_read(). It must then give the real final response directly.
   NO interim "100 Continue" response must reach the client first. A
   _task_worker that sends "100 Continue" without a condition, before the
   handler runs, for a buffered route and a streaming route alike, breaks
   this. It tells the client to upload a body that the server was never
   going to read. That defeats the whole point of Expect: 100-continue. RFC
   7231 SS5.1.1 lets the server answer with a final status in place of "100
   Continue", and the client then skips the upload. For a streaming route,
   the interim send waits for chttpsvr_req_read() itself; see the comment
   of that function. That is what keeps it from firing until the handler
   asks to read. */
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* This test never sends `body`, on purpose. A correct Expect:
     100-continue client waits for a "100 Continue" or for a final response
     before it uploads. This test proves that the server never asks it to
     upload. */
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 401);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "no thanks") != NULL);
  close(fd);
  fd = -1;
}

/* A request can carry "Expect: 100-continue" and have no body at all, with
   no Content-Length and no chunked Transfer-Encoding. Such a request must
   never get the interim "100 Continue" response. This holds for a
   STREAMING route whose handler does call chttpsvr_req_read(). The lazy
   send inside chttpsvr_req_read() must not fire on its first call without
   a condition. It must check whether chttp1_parser_message_complete() is
   already true, that is, whether there was ever a body to invite. A send
   here tells the client to upload a body that was never coming. */
TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_streaming_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* No Content-Length, no Transfer-Encoding: per RFC 7230 SS3.3's
     request-specific framing rule, this is an ordinary, valid, bodyless
     POST; not truncated or malformed. */
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

/* The same contract, for a BUFFERED route. _task_worker sends the interim
   response early, before _drain_body, for a route that
   chttpsvr_register_handler registers and not the streaming variant. That
   send must carry exactly the same condition. */
TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_buffered_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

/* The interim line "HTTP/1.1 100 Continue\r\n\r\n" is 25 bytes. A real
   short write of it leaves a truncated status line on the wire that no
   parser can read. Such a short write happens when a peer that reads
   slowly stalls the write part way through. That is exactly the case that
   max_response_write_duration_us defends against. _write_interim_continue
   therefore reports whether it wrote the complete line. Both call sites
   treat a false return as fatal for this connection. Those sites are
   _task_worker, which this test uses for the buffered route, and
   chttpsvr_req_read, which the streaming-route test right below uses. They
   skip the send of the real response completely and close. They must not
   put more bytes on top of a stream that may be corrupt. A handler that
   runs and sends a complete, valid final response right after the
   truncated prefix gives a byte stream like "HTTP/1.1 100 ConHTTP/1.1 200
   OK\r\n...". No HTTP/1.1 client that follows the standard can parse
   that.

   _chttpsvr_force_short_interim_write_for_tests() reproduces the short
   write every time. It makes a real partial write to the real socket, and
   it does not simulate one. The test does not try to make the socket
   buffering of the OS produce a real partial write of a message this
   small. */
TEST(chttpserver, interim_continue_short_write_buffered_route_forces_close) {
  extern void _chttpsvr_force_short_interim_write_for_tests(size_t n);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
     forced short write genuinely put on the wire; possibly split across
     more than one read(2) call. */
  char got[64] = {0};
  size_t got_n = 0;
  while (got_n < 10) {
    ssize_t r = read(fd, got + got_n, sizeof(got) - 1 - got_n);
    if (r <= 0) break;
    got_n += (size_t)r;
  }
  REQUIRE_EQ(got_n, (size_t)10);
  REQUIRE_EQ(memcmp(got, "HTTP/1.1 1", 10), 0);

  /* No further bytes are ever sent (in particular, no real final response
     glued onto the truncated line): the connection is simply closed. */
  char extra[64];
  ssize_t r2 = read(fd, extra, sizeof(extra));
  REQUIRE_EQ(r2, (ssize_t)0);

  close(fd);
  fd = -1;
}

/* The streaming-route version of the test above. It causes the same
   corruption from a short write. It reaches that corruption through the
   lazy interim send of chttpsvr_req_read, and not through the eager one of
   _task_worker. It also confirms that chttpsvr_req_read itself returns -1
   at once. That function must not try to read a body over a connection
   whose response channel is now corrupt. The test uses the existing
   _stream_error_report_handler. The "done" and x-stream-err response of
   that handler must never reach the wire either. */
TEST(chttpserver, interim_continue_short_write_streaming_route_forces_close) {
  extern void _chttpsvr_force_short_interim_write_for_tests(size_t n);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  /* _write_interim_continue writes the "100 Continue" interim line of RFC
     7231 SS5.1.1. The server sends it for an ordinary "Expect:
     100-continue" request, which is standard client behaviour; curl sends
     one by default for a large upload. That write must NOT pass through
     conn->write_deadline with a bare _shrink_timeout_to_deadline call. It
     needs the same _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS ceiling that the
     rejection-response path of _send_response carries, and that ceiling
     applies without a condition. A small internal write of a fixed shape
     must never stay fully unbounded. That is what happens when the
     operator leaves max_response_write_duration_us at its default of 0,
     which turns it off. Without the ceiling, a peer can send an ordinary
     Expect: 100-continue request and then read the interim line one byte
     at a time. That peer holds a thread of the worker pool forever, at the
     default configuration that this project ships. A handful of such
     connections then exhausts the whole pool. That is the same
     Slowloris-class denial of service that the same ceiling closes for the
     rejection response, which is also an internal write. This test uses
     g_srv directly. g_srv never sets max_response_write_duration_us, so it
     keeps the real shipped default of 0 here. The test therefore proves
     that the ceiling really applies without a condition. It does not
     merely repeat a cap that the test itself set up. */
  extern void _chttpsvr_force_next_response_write_deadline_expired_for_tests(
      void);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* This bounds the read below. A regression can stop the server from
     asking the hook at all on this path. The server then sends an ordinary
     "100 Continue" interim line, and the read below still returns quickly
     with those bytes. It does not hang. This timeout is therefore only an
     extra defence. No likely regression here hangs the read. */
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

  /* The write loop of the interim continue makes its first check of the
     write deadline before any real write(2) call. The forced hook makes
     that check report "already expired". The interim write therefore
     returns false and sends zero bytes. The documented contract of
     _write_interim_continue says that any false return makes the caller
     skip the send of the real response and close the connection. A short
     write is not the only such case. The client therefore sees an EOF,
     where read returns 0, and gets nothing at all. This test is not
     vacuous. A build that never asks the hook on this path sends a
     complete, ordinary "100 Continue" line, which shows up here as a byte
     count above zero. */
  REQUIRE_EQ((int)r, 0);
}

TEST(chttpserver, stream_read_timeout_ms_zero_means_wait_indefinitely) {
  /* The documentation of chttpsvr_config_t.stream_read_timeout_us says
     that 0 means "wait indefinitely". Every call site in chttpserver.c
     must therefore translate that value into the int timeout_ms parameter
     of chttp1_stream_read and chttp1_stream_write. That parameter follows
     the convention of poll(2), where a negative value blocks forever and 0
     makes a single non-blocking try. A plain cast of the value skips the
     translation. A configured 0 then means the opposite: give up at once,
     on every call, without a try. Two symptoms follow. The
     chttpsvr_req_read() of a streaming handler returns -1 at about t=0,
     however long the client takes to send the body. A GET with no body
     also gets no response at all. That happens because
     response_write_timeout_us has its own default where 0 means "use the
     value of stream_read_timeout_us". It inherits the same 0, so every
     response write fails at once too. This test covers both cases. A
     client delays the send of its body well past any accidental "instant"
     failure, and the request must still succeed once the bytes arrive. The
     server must also deliver the response. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* This bounds the final loop that reads the response below. A regression
     can make the server hang forever and never respond. The configuration
     under test here is "wait indefinitely", so a real server bug has no
     other bound to catch it. Without this bound, that read(2) blocks
     forever and hangs the whole test binary. This one assertion must fail
     cleanly instead. 10s is generous next to the delay of 300ms that this
     test adds below. */
  struct timeval rcvtimeo = {10, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* This delay is long, and it goes well past any accidental "instant"
     failure. It happens before the send of the body. A correct "wait
     indefinitely" implementation must still take these bytes once they
     arrive. */
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
  /* Global middleware must inject x-global-mw on every streaming response. */
  if (a->ok && resp) {
    const char *gv = chttpclient_resp_header(resp, "x-global-mw");
    a->ok = (gv != NULL && strcmp(gv, "1") == 0);
  }
  if (resp) chttpclient_resp_free(resp);
  return NULL;
}

TEST(chttpserver, concurrent_streaming) {
  /* Fire N_CONCURRENT_STREAMS simultaneous streaming POSTs and verify that
     each response echoes its own distinct payload without cross-contamination.
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
  /* The test must join every thread that started before any REQUIRE_*
   * below can return early. A REQUIRE_* of Tau returns from this test
   * function at once on a failure, and args[] and threads[] live on the
   * stack. An unjoined thread that still runs _conc_stream_thread keeps
   * writing into its own a->ok. By then the stack frame of this function
   * is gone, and the test that runs next reuses it. That is a real
   * stack-use-after-return. It can corrupt a later, separate test in
   * silence, and it is not only a leaked thread. The test therefore joins
   * first and asserts in a separate loop afterwards. Every thread is
   * finished before any REQUIRE_* in this function can return. */
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(created, N_CONCURRENT_STREAMS);
  for (int i = 0; i < created; i++) REQUIRE_TRUE(args[i].ok);
}

/* ========================================================================== */
/*                       SUBROUTER TRAILING-SLASH TEST                        */
/* ========================================================================== */

TEST(chttpserver, subrouter_trailing_slash_prefix) {
  /* A sub-router can have a slash at the end of its prefix, for example
     "/api/v3/". It must behave in exactly the same way as one with no such
     slash, "/api/v3". The library removes that slash from the stored
     prefix. A request to /api/v3/ping must therefore route correctly. */
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
  /* chttpsvr_req_header reads the same conn->hdr_names and
     conn->hdr_values arrays for every kind of route. The
     streaming_repeated_header test above covers the same "last occurrence
     wins" behaviour through a streaming route. This test exercises it
     through a buffered route. The /header route is buffered, and
     chttpclient overwrites a duplicate name. This test therefore uses a
     raw socket too. */
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
  /* chttpsvr_resp_write_str must return ccol_invalid_args (not crash) when
     called with a NULL resp or a NULL str argument.  The /null-guard route is
     registered in _setup; g_null_guard_results is reset before each call so
     the test is repeatable. */
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
  /* A path with a trailing slash must NOT match a route whose pattern has no
   * trailing slash.  /api/v1/items/99/ must yield 404 even though
   * /api/v1/items/{id} is registered. */
  chttpcli_response *resp = _get("/api/v1/items/99/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*   ROUTE-MATCHING PER-ROUTER SEGMENT-DECODE CACHE TESTS                     */
/*                                                                            */
/* The route matcher of chttpserver.c percent-decodes each raw path segment  */
/* at most once for each router and each request. It keeps the decoded value */
/* in a small cache. Every candidate route that the matcher tries against    */
/* that router shares that cache. The matcher does not decode the segment    */
/* again for each candidate route that needs it. The decoded value of a raw  */
/* segment, or its decode failure, is the same answer whatever route asks.   */
/* The tests below exercise that sharing directly. They register several     */
/* routes on the same root router. A try of an earlier candidate can fail,   */
/* and that failure must never corrupt what a later candidate reads back for */
/* a shared position. That position can be a literal segment, a captured     */
/* param, or one whose percent-encoding is malformed. Every route pattern    */
/* below belongs to this test group only. A dynamic registration here can    */
/* therefore not shadow anything that _setup() already registered, and       */
/* nothing can shadow it.                                                    */
/* ========================================================================== */

TEST(chttpserver, cache_shared_literal_prefix_segment_multiple_candidates) {
  /* Three routes share the same first two segments. The second segment
     reaches the server percent-encoded, as "%61lpha" for "alpha". A real
     decode must therefore be shared correctly across every candidate, and
     not a raw comparison byte for byte. The test requests the path of the
     THIRD route. The server tries the first two candidates and rejects
     them on their own different third segment, before it reaches the third
     one. Those tries and rejections must not corrupt the shared "alpha"
     cache entry. A corrupt entry is one that is freed, stale, or never
     computed again. It makes this test crash or return the wrong body. */
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
  /* Two routes both capture {id} at segment 0. They differ only at segment
     1, which is a literal. The test requests the path of the SECOND route,
     so the server tries the first route first. That route captures
     id="xyz" into its own param array, and the wire carries that value
     percent-encoded as "x%79z". It then fails on segment 1, because
     "first" is not "second", and it frees that array. The second route
     must still read back the correct value "xyz" for the same segment 0.
     This proves that the cache keeps its own decoded copy. That copy is
     independent of what an earlier candidate did with a copy of it. It is
     not a reference into memory that an earlier failed match already
     freed. */
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
  /* Two routes share a param at segment 0 and differ only at segment 1.
     Segment 0 of the request carries a malformed percent-encoding, "%ZZ",
     which is not valid hex. The server must reject both routes with a 404.
     It must not reject only the one that it tries first. A decode failure
     for a raw segment is a permanent fact about that segment: it can never
     match. That fact does not depend on which candidate route asks. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/x",
                                     _cache_param_handler, (void *)"x"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/y",
                                     _cache_param_handler, (void *)"y"),
      (int)ccol_success);

  /* The request goes out raw. An HTTP client refuses to send a malformed
     escape in the first place, and this test is about what the server does
     when one arrives anyway. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /cache-bad/%ZZ/y HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
}

/* ========================================================================== */
/*   SUB-ROUTER PREFIX-MATCHING SEGMENT-DECODE CACHE TESTS                    */
/*                                                                            */
/* _find_route also percent-decodes each raw request-path segment at most    */
/* once for each request. It keeps the decoded value in a cache that every   */
/* _prefix_matches call of every non-root sub-router shares. The cache of    */
/* the group above is a different one, and its scope is the candidate routes */
/* of one router. The decoded value of a raw segment is the same answer      */
/* whatever prefix of whatever sub-router the matcher compares it against.   */
/* The tests below register several sibling sub-routers that share one       */
/* leading prefix segment. The wire carries that segment percent-encoded. A  */
/* try of an earlier sub-router can fail on a later, different prefix        */
/* segment. That failure must never corrupt what a later sub-router reads    */
/* back for the same shared leading position. Every prefix below belongs to  */
/* this test group only. A dynamic registration here can therefore not       */
/* shadow anything that _setup() already registered, and nothing can shadow  */
/* it.                                                                       */
/* ========================================================================== */

TEST(chttpserver, subrouter_prefix_cache_shared_across_candidates) {
  /* Three sub-routers share the same first prefix segment. That segment
     reaches the server percent-encoded, as "pfx-sh%61re" for "pfx-share".
     A real decode must therefore be shared correctly across the
     _prefix_matches call of every sub-router. A raw comparison byte for
     byte is not enough. The test requests the route of the THIRD
     sub-router. The server tries the first two and rejects them on their
     own different second prefix segment, before it reaches the third one.
     Those tries and rejections must not corrupt the shared "pfx-share"
     cache entry. A corrupt entry is one that is freed, stale, or never
     computed again. It makes this test crash or route to the wrong
     handler. */
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
  /* Two sub-routers share a leading prefix segment. In the request, that
     segment carries a malformed percent-encoding, "%ZZ", which is not
     valid hex. The server must reject both sub-routers with a 404. It must
     not reject only the one that it tries first. A decode failure for a
     raw path segment is a permanent fact about that segment: it can never
     match. That fact does not depend on which prefix of which sub-router
     asks. */
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

  /* The request goes out raw, for the reason that
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
  /* chttpsvr_register_handler must return ccol_invalid_args when fn is NULL so
   * that a NULL handler can never be installed and later cause a crash at
   * dispatch time.  Uses g_srv2 which is created but never started. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, "/test-null-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_use_null_fn_rejected) {
  /* chttpsvr_router_use must return ccol_invalid_args when fn is NULL to
   * prevent a NULL middleware from being appended to the chain. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-mw-test");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_use(r, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, use_null_fn_rejected) {
  /* chttpsvr_use must return ccol_invalid_args when fn is NULL, consistent
   * with chttpsvr_register_handler and chttpsvr_router_use.  Uses g_srv2 which
   * is never started so the running server is unaffected. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_use(g_srv2, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    NULL PATTERN GUARD TESTS */
/* ========================================================================== */

TEST(chttpserver, on_null_pattern_rejected) {
  /* chttpsvr_register_handler must enforce the NULL-pattern precondition at the
   * public API boundary, not delegate it silently to _router_add_route.  Uses
   * g_srv2. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, on_stream_null_pattern_rejected) {
  /* chttpsvr_register_streaming_handler must reject NULL pattern at the public
   * API boundary. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_pattern_rejected) {
  /* chttpsvr_router_on must also guard against NULL pattern, consistently with
   * chttpsvr_register_handler.  Uses a sub-router on g_srv2. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-pat-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv =
      chttpsvr_router_on(r, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_pattern_rejected) {
  /* chttpsvr_router_on_stream must enforce the NULL-pattern precondition at
   * the public API boundary, consistently with chttpsvr_router_on and
   * chttpsvr_register_streaming_handler.  Uses a sub-router on g_srv2. */
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
  /* Two calls to chttpsvr_stop on a server that nothing ever started must
   * be safe. The first call is a no-op, because the was_started check of
   * chttpsvr_stop is false. The second call is the same. Neither call must
   * crash, and neither call must touch the shared engine reactor. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2);
  chttpsvr_stop(g_srv2);
}

/* ========================================================================== */
/*                    DUPLICATE RESPONSE HEADER TEST                          */
/* ========================================================================== */

TEST(chttpserver, resp_header_duplicate_last_wins) {
  /* When chttpsvr_resp_set_header is called twice with the same header name
   * the second call must overwrite the first value, not append a second entry.
   * The /dup-header route sets x-dup twice. */
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
  /* chttpsvr_resp_set_status(resp, 0) stores an out-of-range value.
   * _send_response must clamp it to 500 (its own [100,999] range check)
   * rather than writing the raw out-of-range value onto the wire. The
   * /bad-status route exercises this. */
  chttpcli_response *resp = _get("/bad-status");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_null_key_returns_invalid_args) {
  /* chttpsvr_req_query_one must return ccol_invalid_args when the key
   * argument is NULL.  The handler calls it with key=NULL and writes the
   * numeric return value as the response body so we can assert it here. */
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
  /* chttpsvr_req_read(req, buf, 0) must return 0 (no-op), not -1 (error),
   * on a streaming route.  Bundling the buflen == 0 check with the
   * hard-error guards instead would incorrectly return -1. */
  chttpcli_response *resp = _get("/stream-zero-buflen");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_read_empty_body_streaming_returns_zero) {
  /* The server can call a streaming handler for a request that carries no
   * body. The first call to chttpsvr_req_read must then return 0 for EOF,
   * at once. It must not return a negative value, and it must not hang. */
  chttpcli_response *resp = _get("/stream-empty-read");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_null_req_returns_invalid_args) {
  /* chttpsvr_req_query_one must return ccol_invalid_args when req is NULL.
   * This is a pure C-level unit test; no HTTP round-trip is needed because
   * the function's NULL guard fires before any HTTP state is accessed. */
  const char *val = NULL;
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "key", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    TRAILING-SLASH PATTERN REJECTION TESTS                  */
/* ========================================================================== */

TEST(chttpserver, trailing_slash_pattern_rejected) {
  /* The server must reject a pattern that ends with '/' at registration
   * time, with ccol_invalid_args. Nothing can ever reach such a pattern.
   * The split into segments in _compile_pattern cannot tell an empty
   * segment at the end apart from the variant with no slash at the end.
   * Such a pattern therefore matches the same requests as that variant,
   * and the API contract then misleads the caller. A rejection is the only
   * honest behaviour.
   *
   * "/" (the root) is a special case, and the server always accepts it.
   *
   * This test uses g_srv2, which nothing starts, so it does not pollute
   * the routing table of g_srv. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  /* Rejected: trailing slash on a single-segment pattern. */
  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Rejected: trailing slash on a multi-segment pattern. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar/", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* Accepted: the root pattern "/" has no trailing segment. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  /* Accepted: normal multi-segment pattern without trailing slash. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* ========================================================================== */
/*                    chttpsvr_req_read NULL-GUARD TESTS                      */
/* ========================================================================== */

TEST(chttpserver, req_read_null_req_returns_minus_one) {
  /* chttpsvr_req_read must return -1 when req is NULL rather than
   * dereferencing it.  This is a pure C-level unit test. */
  char buf[16];
  ssize_t n = chttpsvr_req_read(NULL, buf, sizeof(buf));
  REQUIRE_EQ(n, (ssize_t)-1);
}

TEST(chttpserver, req_read_null_buf_returns_minus_one) {
  /* chttpsvr_req_read must return -1 when buf is NULL (and len > 0).
   * The /stream-null-buf route calls req_read(req, NULL, 4) and echoes the
   * return value as a decimal string so we can assert it here. */
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
  /* chttpsvr_subrouter must return NULL when prefix is NULL to protect against
   * the server later dereferencing the prefix for path matching.
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing tables. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, NULL);
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_no_leading_slash_rejected) {
  /* chttpsvr_subrouter must return NULL when the prefix does not start with
   * '/', since all valid HTTP paths start with '/'.
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing tables. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "api/v1");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_at_start_rejected) {
  /* A prefix of "//api" starts with '/' but has consecutive slashes beginning
   * at index 0-1.  Such a prefix can never match a valid HTTP path and would
   * create a permanently unreachable router.  chttpsvr_subrouter must reject
   * it by returning NULL.
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing tables. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "//api");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_in_middle_rejected) {
  /* A prefix with two slashes together in the middle, for example
   * "/api//v1", is unreachable in the same way. The server must reject it
   * too. This test uses g_srv2, which nothing starts. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/api//v1");
  REQUIRE_TRUE(r == NULL);
}

/* ========================================================================== */
/*                    on_stream / router_on_stream NULL-FN TESTS              */
/* ========================================================================== */

TEST(chttpserver, on_stream_null_fn_rejected) {
  /* chttpsvr_register_streaming_handler must return ccol_invalid_args when fn
   * is NULL, consistent with chttpsvr_register_handler which already rejects a
   * NULL handler. A NULL streaming handler would crash when the ctpool tries to
   * call it. Uses g_srv2 (never started). */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_fn_rejected) {
  /* chttpsvr_router_on_stream must return ccol_invalid_args when fn is NULL,
   * consistent with chttpsvr_register_streaming_handler and chttpsvr_router_on.
   * Uses a sub-router on g_srv2 (never started). */
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
  /* chttpsvr_resp_write must:
   *   - return ccol_invalid_args when data is NULL and len > 0, and
   *   - return ccol_success when data is NULL and len == 0 (no-op).
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
  /* chttpsvr_req_read(req, NULL, 0) must return 0 and not -1. The guard
   * for buflen==0 must run before the guard for a NULL buf. A NULL buffer
   * with a length of zero is then a harmless no-op. It is not a hard
   * error. */
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
  /* chttpsvr_resp_write_json must return ccol_invalid_args when len == 0.
   * The /json-zero-len route is registered in _setup. */
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
  /* chttpsvr_resp_write_json(NULL, ...) must return ccol_invalid_args without
   * dereferencing the NULL response pointer.  This complements the zero-len
   * rejection test and the resp_write_str null-resp test by covering the
   * specific code path in resp_write_json that guards the resp pointer. */
  ccol_retval_t rv = chttpsvr_resp_write_json(NULL, "{}", 2);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_method_null_returns_chttp_get) {
  /* chttpsvr_req_method(NULL) must not crash and must return CHTTP_GET (the
   * zero value of the enum, indistinguishable from a real method when no
   * request is available). */
  REQUIRE_EQ((int)chttpsvr_req_method(NULL), (int)CHTTP_GET);
}

TEST(chttpserver, req_path_null_returns_null) {
  /* chttpsvr_req_path(NULL) must return NULL rather than dereferencing a NULL
   * request pointer. */
  REQUIRE_TRUE(chttpsvr_req_path(NULL) == NULL);
}

TEST(chttpserver, req_body_null_returns_null_and_zeroes_len) {
  /* chttpsvr_req_body(NULL, &len) must return NULL and write 0 into len rather
   * than dereferencing a NULL request pointer. */
  size_t len = 99;
  const void *p = chttpsvr_req_body(NULL, &len);
  REQUIRE_TRUE(p == NULL);
  REQUIRE_EQ(len, (size_t)0);
}

TEST(chttpserver, req_body_null_req_null_len_out_is_safe) {
  /* chttpsvr_req_body(NULL, NULL) must return NULL without crashing even when
   * both the request pointer and the len_out pointer are NULL.  The function
   * must guard the write to *len_out before dereferencing it. */
  REQUIRE_TRUE(chttpsvr_req_body(NULL, NULL) == NULL);
}

TEST(chttpserver, req_raw_query_null_returns_null) {
  /* chttpsvr_req_raw_query(NULL) must return NULL rather than crashing. */
  REQUIRE_TRUE(chttpsvr_req_raw_query(NULL) == NULL);
}

TEST(chttpserver, req_param_null_req_returns_null) {
  /* chttpsvr_req_param(NULL, key) must return NULL rather than crashing. */
  REQUIRE_TRUE(chttpsvr_req_param(NULL, "id") == NULL);
}

TEST(chttpserver, req_header_null_req_returns_null) {
  /* chttpsvr_req_header(NULL, name) must return NULL rather than
   * dereferencing a NULL request pointer. */
  REQUIRE_TRUE(chttpsvr_req_header(NULL, "x-foo") == NULL);
}

TEST(chttpserver, req_header_null_name_returns_null) {
  /* chttpsvr_req_header(req, NULL) must return NULL. It must not crash.
   * chttpsvr_req is opaque, so this test exercises the guard through a
   * full HTTP round trip. _null_name_header_handler calls
   * chttpsvr_req_header(req, NULL) on a real live request. It writes
   * "null" only when the result is NULL. */
  chttpcli_response *resp = _get("/null-name-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_query_null_req_returns_null_and_zero_count) {
  /* chttpsvr_req_query(NULL, key, &n) must return NULL and write 0 into
   * the count rather than dereferencing a NULL request pointer. */
  size_t n = 99;
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", &n) == NULL);
  REQUIRE_EQ(n, (size_t)0);
}

TEST(chttpserver, req_query_null_req_null_count_out_is_safe) {
  /* chttpsvr_req_query(NULL, key, NULL) must return NULL without crashing
   * even when count_out is NULL.  The guard must check count_out before
   * writing 0 into it. */
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", NULL) == NULL);
}

TEST(chttpserver, req_query_one_null_req_null_val_out_is_safe) {
  /* chttpsvr_req_query_one(NULL, key, NULL) must return ccol_invalid_args
   * (NULL req triggers the early-exit guard) without attempting to write
   * through a NULL val_out pointer. */
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "q", NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_query_one_invalid_args_resets_poisoned_val_out) {
  /* The doc comment of chttpsvr_req_query_one in the header says that
   * *val_out resets to NULL on every failure return. That includes
   * ccol_invalid_args. This test therefore poisons it first with a
   * sentinel that is not NULL. A regression that leaves it untouched is
   * then visible. A start at NULL makes the test pass whether or not the
   * function touches it. */
  const char *val = (const char *)0xdeadbeefUL;
  ccol_retval_t rv = chttpsvr_req_query_one(NULL, "q", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ((void *)val, (void *)NULL);
}

TEST(chttpserver, req_query_one_null_val_out_with_hit_returns_success) {
  /* chttpsvr_req_query_one(req, key, NULL) must return ccol_success when the
   * key is present even though the caller passed NULL for val_out (pure
   * existence check).  The function must not crash. */
  chttpcli_response *resp = _get("/query-one-null-val-out?q=present");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_query_oom_null_req_returns_false) {
  /* chttpsvr_req_query_oom(NULL) must return false rather than crashing on a
   * NULL request pointer. */
  REQUIRE_FALSE(chttpsvr_req_query_oom(NULL));
}

TEST(chttpserver, req_query_null_key_returns_null_and_zero_count) {
  /* chttpsvr_req_query(req, NULL, &n) must return NULL and write 0 into
   * the count. It must not crash. chttpsvr_req is opaque, so this test
   * exercises the guard through a full HTTP round trip.
   * _null_key_query_handler calls chttpsvr_req_query(req, NULL, &n) on a
   * real live request. It writes "null" only when the result is NULL and
   * the count is 0. */
  chttpcli_response *resp = _get("/null-key-query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, resp_set_status_null_is_noop) {
  /* chttpsvr_resp_set_status(NULL, ...) must not crash.  There is no return
   * value to check; absence of a crash is the only assertion. */
  chttpsvr_resp_set_status(NULL, 200);
}

TEST(chttpserver, resp_set_header_null_resp_returns_invalid_args) {
  /* chttpsvr_resp_set_header(NULL, ...) must return ccol_invalid_args to
   * allow callers to detect the error rather than silently doing nothing. */
  ccol_retval_t rv = chttpsvr_resp_set_header(NULL, "x-foo", "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, serve_null_srv_returns_invalid_args) {
  /* chttpsvr_start(NULL, ...) must return ccol_invalid_args rather than
   * crashing on a NULL server pointer. */
  ccol_retval_t rv = chttpsvr_start(CHTTPSVR_INVALID, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, stop_null_is_noop) {
  /* chttpsvr_stop(CHTTPSVR_INVALID) must not crash. */
  chttpsvr_stop(CHTTPSVR_INVALID);
}

/* (The engine lifecycle is managed implicitly: the engine starts on the
   first chttpsvr_start call and stops when the last server is destroyed.) */

/* ========================================================================== */
/*                    RAW-QUERY ABSENT STRING TEST */
/* ========================================================================== */

TEST(chttpserver, raw_query_absent) {
  /* When the request URL has no query string, chttpsvr_req_raw_query returns
   * NULL and the _raw_query_handler writes the literal "(none)". */
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
  /* A handler can set a 204 status and a response header and never call a
   * chttpsvr_resp_write* function. The server must still deliver the
   * correct status code and header to the client. _finalize_response must
   * take the branch with no body, which is http_finish. It must not take
   * the body branch, which is http_send_body. */
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
   * routing table keeps BOTH entries, in the order of the registrations.
   * The first one wins at match time. The server accepts the second
   * registration and returns ccol_success. Its handler never runs, because
   * _find_route returns on the first full match.
   *
   * _setup registers the /dup-first-wins route in advance:
   *   chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
   * _hello_handler, NULL); chttpsvr_register_handler(g_srv, CHTTP_GET,
   * "/dup-first-wins", _dup_second_handler, NULL); _hello_handler answers
   * with "Hello, world!", and _dup_second_handler answers with "second".
   * The expected body is "Hello, world!", because the first registration
   * wins. */
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
  /* chttpsvr_router_on must return ccol_invalid_args when fn is NULL,
   * consistent with chttpsvr_register_handler,
   * chttpsvr_register_streaming_handler, and chttpsvr_router_on_stream which
   * already have corresponding tests. Uses a sub-router on g_srv2 (never
   * started). */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-fn-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_on(r, CHTTP_GET, "/x", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_null_resp_set_status_no_crash_and_dual_null_header) {
  /* chttpsvr_resp_set_status(NULL, ...) must not crash. It returns void,
   * so there is no guard to check. chttpsvr_resp_set_header(NULL, NULL,
   * ...) must return ccol_invalid_args from the guard for a NULL resp.
   * That guard fires before the guard for a NULL name, so the result is
   * the same as the case with only a NULL resp. The companion test
   * resp_set_header_null_name_value_live exercises a NULL name and a NULL
   * value with a live resp inside a handler. */
  chttpsvr_resp_set_status(NULL, 200); /* must not crash */
  ccol_retval_t rv = chttpsvr_resp_set_header(NULL, NULL, "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args); /* NULL resp guard fires first */
}

TEST(chttpserver, resp_set_header_null_name_value_live) {
  /* Exercise the NULL name and NULL value guards of chttpsvr_resp_set_header
   * with a real live chttpsvr_resp (available only inside a handler).
   * The /set-header-null-guards route is registered in _setup. */
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
  /* chttpsvr_resp_set_header must reject a name or a value that holds an
   * embedded CR or LF byte. It must return ccol_invalid_args and must not
   * write the bytes onto the wire. _send_response emits "name:value\r\n"
   * and escapes nothing. Without this check, a handler that puts data from
   * the request into a response header lets an attacker add extra header
   * lines. That attacker can also split the response in two, which is the
   * classic HTTP response splitting. This test also checks that a valid
   * header set afterwards still works. The two rejections must not corrupt
   * the header list of resp. _setup registers the
   * /set-header-crlf-guards route. */
  g_crlf_hdr_results[0] = g_crlf_hdr_results[1] = g_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status =
      _raw_request("GET", "/set-header-crlf-guards", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Neither injection attempt made it onto the wire at all: no second
   * status line, no injected header name anywhere in the response. */
  REQUIRE_TRUE(strstr(buf, "x-injected") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-evil") == NULL);

  REQUIRE_EQ(g_crlf_hdr_results[0], (int)ccol_invalid_args); /* CRLF in name */
  REQUIRE_EQ(g_crlf_hdr_results[1], (int)ccol_invalid_args); /* CRLF in value */
  REQUIRE_EQ(g_crlf_hdr_results[2],
             (int)ccol_success); /* legit header still works */
}

TEST(chttpserver, resp_set_header_bare_cr_and_lf_rejected) {
  /* The guard of chttpsvr_resp_set_header against CRLF injection uses
   * strpbrk(x, "\r\n"). That is what makes it catch a BARE CR or a BARE LF
   * on its own. It catches more than the combined "\r\n" pair that the
   * test above exercises. This test checks that directly. It does not
   * assume that the paired case covers it. A regression that checks only
   * for the literal two-byte "\r\n" substring, with strstr in place of
   * strpbrk for example, still passes that test and fails this one. _setup
   * registers the /set-header-bare-crlf-guards route. */
  g_bare_crlf_hdr_results[0] = g_bare_crlf_hdr_results[1] =
      g_bare_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-bare-crlf-guards", NULL, buf,
                            sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Neither injection attempt made it onto the wire at all. */
  REQUIRE_TRUE(strstr(buf, "injected") == NULL);

  REQUIRE_EQ(g_bare_crlf_hdr_results[0], (int)ccol_invalid_args); /* bare CR */
  REQUIRE_EQ(g_bare_crlf_hdr_results[1], (int)ccol_invalid_args); /* bare LF */
  REQUIRE_EQ(g_bare_crlf_hdr_results[2],
             (int)ccol_success); /* legit header still works */
}

TEST(chttpserver, resp_set_header_disallowed_names_rejected) {
  /* chttpsvr_resp_set_header must reject an empty name with
   * ccol_invalid_args, because an empty name has no valid form on the
   * wire. It must also reject a "Transfer-Encoding" name in any letter
   * case. It must not store either one. _send_response always computes its
   * own Content-Length header from the real response body and emits it. It
   * never chunk-encodes that body. A Transfer-Encoding header from a
   * handler that reaches the wire therefore pairs a false "chunked" claim
   * with a real Content-Length, over a body with no transfer coding at
   * all. That framing is ambiguous. An intermediary that prefers
   * Transfer-Encoding over Content-Length (RFC 7230 SS3.3.3) can then
   * parse it wrongly. chttp_request_set_header makes the same rejection on
   * the client side. This test also checks that a valid header set
   * afterwards still works. The three rejections must not corrupt the
   * header list of resp. _setup registers the
   * /set-header-disallowed-name-guards route. */
  g_disallowed_hdr_results[0] = g_disallowed_hdr_results[1] =
      g_disallowed_hdr_results[2] = g_disallowed_hdr_results[3] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-disallowed-name-guards", NULL,
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  /* Transfer-Encoding never reached the wire in either casing. */
  REQUIRE_TRUE(strcasestr(buf, "transfer-encoding") == NULL);

  REQUIRE_EQ(g_disallowed_hdr_results[0], (int)ccol_invalid_args); /* "" */
  REQUIRE_EQ(g_disallowed_hdr_results[1],
             (int)ccol_invalid_args); /* transfer-encoding */
  REQUIRE_EQ(g_disallowed_hdr_results[2],
             (int)ccol_invalid_args); /* Transfer-Encoding */
  REQUIRE_EQ(g_disallowed_hdr_results[3],
             (int)ccol_success); /* legit header still works */
}

TEST(chttpserver, resp_set_header_non_tchar_name_rejected) {
  /* chttpsvr_resp_set_header must reject a header NAME that holds a byte
   * outside the tchar set of RFC 7230 SS3.2.6. A check for a CR or an LF
   * alone is not enough. A space, a literal colon or a control byte such
   * as DEL (0x7F) is not a vector for CRLF injection. But each one still
   * gives a malformed line on the wire that a strict parser downstream can
   * read wrongly. For example, the name "X Foo: bar" puts
   * "X Foo:bar:baz\r\n" on the wire. That is not a real split into two
   * fields. This test also checks that a valid header with every tchar
   * byte that is not a letter or a digit still works after the three
   * rejections. Those rejections must not corrupt the header list of resp.
   * _setup registers the /set-header-non-tchar-name-guards route. */
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
             (int)ccol_success); /* legit header still works */
}

TEST(chttpserver, subrouter_null_srv_rejected) {
  /* chttpsvr_subrouter must return NULL when srv is NULL to prevent a
   * dangling back-pointer in the new router from causing crashes at routing
   * time.  The NULL prefix and no-leading-slash cases are already tested;
   * this test completes the guard coverage. */
  chttpsvr_router *r = chttpsvr_subrouter(CHTTPSVR_INVALID, "/any");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, req_param_null_name_returns_null) {
  /* chttpsvr_req_param(req, NULL) must return NULL rather than crashing.
   * The /param-null-name route is registered in _setup. */
  chttpcli_response *resp = _get("/param-null-name");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                    chttpsvr_start PORT-0 REJECTION TEST                    */
/* ========================================================================== */

TEST(chttpserver, serve_port_zero_rejected) {
  /* chttpsvr_start must return ccol_invalid_args when cfg->port == 0.
   * Port 0 causes the OS to assign an ephemeral port, but the caller has no
   * way to discover which port was chosen, making the server unreachable.
   * Uses g_srv2 (never started) so the running server is unaffected. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    NEW COVERAGE GAP TESTS                                  */
/* ========================================================================== */

/* 1. on_stream with NULL server returns ccol_invalid_args */

TEST(chttpserver, on_stream_null_srv_returns_invalid_args) {
  /* chttpsvr_register_streaming_handler must validate srv != CHTTPSVR_INVALID
   * before doing anything. If it doesn't, the server pointer dereference will
   * crash. This is a pure API validation test (no live server needed). */
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      CHTTPSVR_INVALID, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* 2. chttpsvr_req_query can read an empty query key (?=value) */

TEST(chttpserver, query_empty_key) {
  /* A query string such as "?=hello" has an empty key. The server must
   * parse it correctly. chttpsvr_req_query(req, "", &n) must then read it
   * back. _setup registers the /query-empty-key route. */
  chttpcli_response *resp = _get("/query-empty-key?=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_stray_ampersand_does_not_produce_phantom_empty_key) {
  /* "a=1&&b=2" has a truly empty pair between the two '&' characters.
     There is nothing at all there, not even an '='. The parser must skip
     it. It must not read it as a false entry with an empty key and an
     empty value next to the real "a" and "b" pairs. This case differs from
     "?=hello" above, which is a pair that is not empty and whose key half
     is empty. That case must stay as it is. */
  chttpcli_response *resp = _get("/query-stray-amp-counts?a=1&&b=2");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver,
     query_leading_and_trailing_ampersand_does_not_produce_phantom_empty_key) {
  /* "&a=1" has an empty pair before the first real pair. "b=2&" has one
     after the last pair. The parser must skip both of them in the same
     way. It must not count either one as a false "" key. */
  chttpcli_response *resp = _get("/query-stray-amp-counts?&a=1&b=2&");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpclient_resp_free(resp);
}

/* 3. chttpsvr_req_read after EOF keeps returning 0 */

TEST(chttpserver, req_read_eof_is_idempotent) {
  /* After chttpsvr_req_read returns 0 (EOF), calling it again must continue
   * to return 0.  The /stream-read-eof-twice route is registered in _setup. */
  chttpcli_response *resp = _get("/stream-read-eof-twice");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0 0");
  chttpclient_resp_free(resp);
}

/* 4. chttpsvr_subrouter("/") edge case */

TEST(chttpserver, subrouter_slash_prefix_matches_only_root) {
  /* A sub-router created with prefix "/" retains prefix_len = 1 (the
   * trailing-slash strip loop only runs when plen > 1).  It matches only
   * the exact request path "/"; any other path does not match and the
   * router is skipped entirely.  The "/" sub-router and its routes are
   * registered in _setup. */

  /* The exact path "/" must be handled by the sub-router. */
  chttpcli_response *resp_root = _get("/");
  REQUIRE_TRUE(resp_root != NULL);
  REQUIRE_EQ(resp_root->status_code, 200);
  REQUIRE_TRUE(resp_root->body != NULL);
  REQUIRE_STREQ(resp_root->body, "root_subrouter");
  chttpclient_resp_free(resp_root);

  /* A path that starts with "/" but is not exactly "/" must NOT match. */
  chttpcli_response *resp_foo = _get("/nonexistent-path-xyz");
  REQUIRE_TRUE(resp_foo != NULL);
  REQUIRE_EQ(resp_foo->status_code, 404);
  chttpclient_resp_free(resp_foo);
}

TEST(chttpserver, subrouter_slash_prefix_rejects_doubled_leading_slash) {
  /* A path can start with a second slash right after the first one, for
   * example "//foo". Such a path must NOT match a "/" sub-router either.
   * The server strips exactly one slash at the start, and "/foo" remains.
   * That is not empty, so the path is not the exact root path "/". The
   * server therefore skips the router, as it does for any other path that
   * is not the root. This test uses a raw socket. It does not use _get,
   * which goes through the URL parse and normalisation of chttpclient. The
   * raw socket exercises the literal path match of the server on the wire
   * directly. */
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "//nonexistent-path-xyz", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                    INPUT VALIDATION TESTS */
/* ========================================================================== */

/* 1. Pattern without leading slash rejected */

TEST(chttpserver, pattern_no_leading_slash_rejected) {
  /* _compile_pattern must reject every pattern that does not start with
   * '/'. Such a pattern cannot represent a valid HTTP path. It also
   * matches the same requests as the form with the slash, because the
   * server strips the leading '/' from both before it compares the
   * segments. That breaks the documented API contract. This test uses
   * g_srv2, because nothing starts it. A registration of an invalid route
   * on it does not affect the test server that runs. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);

  ccol_retval_t rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "health",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_POST, "echo", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* An empty string also lacks a leading slash. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  /* A well-formed pattern must still be accepted. */
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/health", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* 2. Engine logger null arg rejected */

TEST(chttpserver, set_engine_logger_null_rejected) {
  /* chttpsvr_set_engine_logger(NULL) must return ccol_invalid_args rather than
   * deriving a NULL-logger.  The engine logger is set in _setup; this pure
   * API validation test does not affect the running server. */
  ccol_retval_t rv = chttpsvr_set_engine_logger(CLOG_INVALID);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* 3. Server handle valid after stop */

TEST(chttpserver, serve_stopped_server_state_valid) {
  /* chttpsvr_stop on a server that was not started is a no-op.  The handle
   * must remain in a consistent state afterwards: route registration must
   * still work without crashing or returning an error. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2); /* no-op: g_srv2 is not started */
  chttpsvr_stop(g_srv2); /* second call: must also be a no-op */

  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/state-valid-check", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

/* 4. TLS configuration arg-guard coverage */

TEST(chttpserver, serve_tls_zero_port_rejected_before_tls_init) {
  /* chttpsvr_start checks cfg->port == 0 BEFORE it sets up TLS. This test
   * confirms the order of the argument guards. A bad port returns
   * ccol_invalid_args whatever the TLS configuration holds. The server
   * never calls ctls_ctx_new_mp or ctls_ctx_cert_add.
   *
   * A full TLS integration test starts a TLS server and completes HTTPS
   * handshakes. Such a test needs valid certificate files. That coverage
   * lives in the dedicated tests_tls binary in this same directory, and
   * not here. See the Makefile of this directory. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttp_tls_config_t tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/cert.pem";
  tls.key_path = "/nonexistent/key.pem";
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.port = 0; /* invalid port; fires before TLS init */
  cfg.host = "127.0.0.1";
  cfg.tls = &tls;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* This file has no test for a block on the start of a TLS server while
 * another server runs, and that is deliberate. More than one server
 * instance can run at the same time. A start of a TLS server while g_srv
 * is active is therefore allowed. TLS integration needs valid certificate
 * files. That coverage lives in the dedicated tests_tls binary in this
 * directory. */

/* The test tls_context_cleaned_before_second_serve_attempt needs real TLS
 * certificate files, so that it exercises a real handshake. A certificate
 * that fails to load is not enough. ctls_ctx_cert_add reports a missing or
 * invalid certificate file in its own ccol_retval_t return. A load failure
 * alone never reaches the code path that this test must cover. That
 * coverage lives in the dedicated tests_tls binary in this directory. This
 * suite leaves it out. */

/* ============================================================ */
/*            COVERAGE-GAP FILL TESTS                           */
/* ============================================================ */

TEST(chttpserver, streaming_get_no_body_via_req_body) {
  /* A streaming GET request has no body.  Calling chttpsvr_req_body() inside
   * the handler must return NULL/0, and the handler must produce a valid
   * "(empty)" response rather than crashing or returning garbage.
   * This exercises the (body && len > 0) else branch in a streaming route. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/stream-body-get-no-body");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, dynamic_route_registration_while_running) {
  /* chttpsvr_register_handler must succeed on a server that already serves
   * requests. This exercises the write path of the rwlock while reactor
   * threads hold read locks. A later request in the same test process must
   * reach the new route at once. */
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
  /* The server tries the routers in order of how specific the mount prefix
   * is, and it tries the most specific one first. A sub-router mounted at
   * /shadow-test therefore answers a request inside its own prefix. This
   * holds even when the root router carries a route for the same effective
   * path. The prefix of the root matches every path, so it is the least
   * specific mount, and the server tries it last. The order of the
   * registrations has no part in the outcome.
   *
   * _setup registers both routes in advance, so the test does not change
   * g_srv while it runs. It registers the ROOT one FIRST:
   *   1. Root route  /shadow-test/ping -> _shadow_root_handler
   *   2. Sub-router  /shadow-test  with /ping -> _shadow_sub_handler
   * Request: GET /shadow-test/ping -> must return "sub-wins". */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/shadow-test/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "sub-wins");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_registration_rejected) {
  /* The cap on the middleware chain is _CHTTPSVR_MAX_MW = 32, and the
   * server applies it at registration time. The first 32 calls to
   * chttpsvr_router_use must return ccol_success. The 33rd must return
   * ccol_not_permitted. A rejection of the registration itself stops one
   * router from ever building a chain that overflows. The public API
   * therefore cannot reach the quiet 500 path at dispatch time this
   * way. */
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
  /* Verify that a request whose combined global+router middleware count
   * exceeds _CHTTPSVR_MAX_MW (32) receives a 500 Internal Server Error.
   *
   * The /mw-overflow-live sub-router is pre-registered in _setup with 32
   * router-level middlewares.  g_srv also has 1 global middleware (_global_mw),
   * so the combined dispatch-time count is 33 > 32, which triggers the
   * overflow guard in _on_request before the route handler is invoked. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/mw-overflow-live/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_streaming_produces_500) {
  /* The middleware-overflow check in _on_request fires before the
   * is_streaming branch; both buffered and streaming routes share the same
   * overflow detection code path.  This test exercises the streaming variant
   * so that any future code-path divergence is caught by the test suite.
   *
   * The /mw-overflow-live sub-router is registered in _setup with 32
   * router-level middlewares; combined with 1 global middleware the total
   * is 33 > _CHTTPSVR_MAX_MW (32).  The streaming route /mw-overflow-live/
   * stream-ping must receive the same 500 response as the buffered
   * /mw-overflow-live/ping route. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp =
      _post("/mw-overflow-live/stream-ping", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, malformed_encoding_in_param_correct_method_returns_404) {
  /* A request with a bad percent-encoding in a {param} segment must return
   * 404. This holds even when the method matches the registered route. It
   * exercises the capture-mode path of _match_route_cached. There
   * _seg_cache_get returns NULL for a bad encoding, and the server treats
   * the route as no match.
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
  /* The server uses a dry-run match when the registered method of the
   * route does not match the method of the request. The dry run and the
   * capture match must check the percent-encoding in the same way. A dry
   * run that accepts any token that is not empty for a {param} segment,
   * with no check of its percent-encoding, breaks this. It sets
   * method_mismatch_seen=true and answers a POST to the GET-only
   * /api/v1/items/{id} with a 405, while a GET to the same malformed URL
   * correctly returns a 404.
   *
   * The design of _match_route_cached and _seg_cache_get makes that
   * difference impossible. _seg_cache_get decodes every segment, without a
   * condition. It does this whether or not the caller gave a pv_out that
   * is not NULL to capture the value. A malformed encoding is therefore
   * rejected in the same way in both cases.
   *
   * This request must return 404 and not 405. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status =
      _raw_request("POST", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, valid_percent_encoded_literal_segment_matches_route) {
  /* A request path can hold a percent-encoded character in a LITERAL route
   * segment, and not in a {param}. It must still match the route.
   *
   * _setup registers /hello for GET. /hel%6Co percent-encodes 'l', because
   * %6C is 0x6C, which is 'l'. After the decode it is the same as /hello,
   * so it must match.
   *
   * This exercises the literal-segment comparison of _match_route_cached.
   * http_decode_path_unsafe must put a NUL at the end of what it writes.
   * Without that NUL, the strcmp that follows reads past the valid content
   * into uninitialised stack memory. On a stack where that byte is not
   * zero, the route fails to match and the server returns 404. */
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
  /* Same path as valid_percent_encoded_literal_segment_matches_route but with
   * the wrong method.  /hel%6Co decodes to /hello; /hello is GET-only.  The
   * path must match (literal-segment decode succeeds) so the server sets the
   * method-mismatch flag and replies 405, not 404. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

TEST(chttpserver, malformed_encoding_in_literal_segment_returns_404) {
  /* A bad percent-encoding in a LITERAL route segment must give a 404.
   * /hel%ZZo has a bad encoding where the "hello" literal sits. No route
   * can match it, so the server returns 404. It must not return 500, and
   * it must not crash. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_literal_segment_wrong_method_returns_404_not_405) {
  /* When a literal segment has invalid percent-encoding, the path cannot
   * match any route even in dry-run mode (method-check only).  Therefore
   * the method-mismatch flag is never set and the server must return 404,
   * not 405, regardless of which methods are registered for the pattern. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                    NULL-GUARD COMPLETENESS TESTS                           */
/* ========================================================================== */

TEST(chttpserver, on_null_srv_returns_invalid_args) {
  /* chttpsvr_register_handler must validate srv != CHTTPSVR_INVALID and return
   * ccol_invalid_args before touching the root router.  Symmetric with
   * on_stream_null_srv_returns_invalid_args which already covers the streaming
   * variant. */
  ccol_retval_t rv = chttpsvr_register_handler(CHTTPSVR_INVALID, CHTTP_GET,
                                               "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_router_returns_invalid_args) {
  /* chttpsvr_router_on must validate router != NULL.  A NULL router dereference
   * inside _router_add_route would crash when writing to router->routes; this
   * guard catches it at the API boundary. */
  ccol_retval_t rv =
      chttpsvr_router_on(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_router_returns_invalid_args) {
  /* chttpsvr_router_on_stream must validate router != NULL, consistent with
   * chttpsvr_router_on. */
  ccol_retval_t rv =
      chttpsvr_router_on_stream(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*                    chttpsvr_start NULL-CONFIG TEST                         */
/* ========================================================================== */

TEST(chttpserver, serve_null_cfg_uses_default) {
  /* chttpsvr_start(srv, NULL) must fall back to CHTTPSVR_CONFIG_DEFAULT.
   * It must not dereference the NULL cfg and crash. This test uses g_srv,
   * which is already started, so the guard against a second start returns
   * ccol_not_permitted. It must NOT return ccol_invalid_args. That code
   * says wrongly that NULL is an invalid argument, and not a default that
   * the function handles. */
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
  /* CHTTP_ANY route dispatches a POST request and the handler observes
     CHTTP_POST. */
  chttpcli_response *resp = _post("/any-method", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "POST");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_put) {
  /* CHTTP_ANY route dispatches a PUT request. */
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_delete) {
  /* CHTTP_ANY route dispatches a DELETE request. */
  char buf[2048] = {0};
  int status = _raw_request("DELETE", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "DELETE");
}

TEST(chttpserver, any_method_with_path_param) {
  /* CHTTP_ANY route with a path parameter captures the param correctly and the
     handler sees the actual method. */
  chttpcli_response *resp = _get("/any-method/42");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET:42");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_specific_wins_for_registered_method) {
  /* A method-specific route registered BEFORE CHTTP_ANY on the same pattern
     wins for its own method (first-wins policy). */
  chttpcli_response *resp = _get("/any-with-specific");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_catches_unregistered_method) {
  /* CHTTP_ANY registered after a specific GET catches methods that do not have
     their own route entry on the same pattern. */
  char buf[2048] = {0};
  int status =
      _raw_request("PUT", "/any-with-specific", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_first_wins_over_specific) {
  /* CHTTP_ANY registered BEFORE a specific GET on the same pattern wins for
     every method including GET (standard first-wins policy). */
  chttpcli_response *resp = _get("/any-first");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, unrecognized_method_rejected_with_501_not_dispatched) {
  /* CHTTP_ANY is a placeholder for registration time only. It means "match
     any of the seven real methods that this server knows". It is never the
     method of a real incoming request. A method token can be valid syntax
     and still be unknown here. Examples are a WebDAV verb such as
     PROPFIND, and TRACE, CONNECT or a custom verb. Such a token must never
     reach a CHTTP_ANY handler at all. The method_ok test of _find_route is
     route->method == CHTTP_ANY. It must not answer true whatever the real
     method is. A test that does lets such a request reach
     _any_method_handler. That handler calls
     chttp_method_str(chttpsvr_req_method(req)) and gets "UNKNOWN" back
     from the default case of the switch, and not a real method name. The
     client then gets a 200 response that reports the request wrongly. The
     server rejects the request up front instead, with a 501 (RFC 7231
     SS6.6.2). It does this before it parses the path or the headers and
     before it matches any route. The handler never runs. An empty body
     proves this, because _any_method_handler always writes a method name
     that is not empty. */
  char buf[2048] = {0};
  int status = _raw_request("PROPFIND", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
  REQUIRE_TRUE(strstr(buf, "content-length:0") != NULL);
}

TEST(chttpserver, unrecognized_method_rejected_even_on_unmatched_path) {
  /* The rejection happens before the route match, and even before the
     parse of the path. An unknown method on a path with no route at all
     therefore still reports 501 and not 404. */
  char buf[2048] = {0};
  int status =
      _raw_request("PROPFIND", "/no-such-route-at-all", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
}

/* ========================================================================== */
/*                    max_body_size / 413 BOUNDARY TESTS                      */
/* ========================================================================== */

/* Connects to `port` and sends a POST to `path` with Connection: close.
   The body of that POST is exactly `body_len` 'a' bytes. The function then
   reads the response, or reads until the connection closes. It returns the
   parsed status code. It returns -1 when no valid status line arrived, for
   example after a reset of the connection before any response bytes came
   back. */
static int _raw_post_fixed_body(int port, const char *path, size_t body_len,
                                char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
  /* Bounds the read loop below; mirrors _raw_request's own identical
     SO_RCVTIMEO guard, for the same reason. */
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
  /* The server must accept a body of exactly max_body_size, which is 64
     bytes, and echo it back in full. The check inside _on_body
     (chttpserver.c) uses a strict "greater than", so the boundary value
     itself must succeed. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_EQ(strlen(body), (size_t)SMALL_BODY_MAX);
}

TEST(chttpserver, buffered_max_body_size_exceeded_rejected) {
  /* The server must reject a body one byte above max_body_size with a real
     413 Payload Too Large response. It must not send a bare connection
     reset. The connection must then close, with Connection: close. It must
     not stay alive for a next request that is corrupt. The body bytes
     above the limit stay unread on the wire. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 413);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, streaming_max_body_size_at_limit_succeeds) {
  /* A streaming route reads a body of exactly max_body_size bytes with
     chttpsvr_req_read. It must see the full body and get no stream
     error. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, streaming_max_body_size_exceeded_reported) {
  /* On the buffered path, the library itself turns this into a 413 and
     never calls the handler. The handler of a streaming route always runs
     instead, and it decides its own response. chttpsvr_req_read() returns
     -1, and chttpsvr_req_stream_error() reports ccol_msg_too_large. That
     is the same shape as the ccol_timed_out case of the existing
     stream_read_timeout_reports_ccol_timed_out test. The connection must
     still carry a real, complete HTTP response and not a bare reset. It
     must then close, with Connection: close. It must not stay alive for a
     next request that is corrupt, because the body bytes above the limit
     stay unread. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_msg_too_large") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

/* The field comment of max_body_size in chttpserver.h documents a value of
   0 as "unlimited". max_connections and max_header_bytes already use that
   same convention in this config struct. The Content-Length pre-check of
   _on_headers_complete and the cumulative check of _on_body must therefore
   both read it as no cap at all. A bare `> limit` comparison reads
   limit == 0 as a cap at zero instead. It then rejects every request that
   carries a body. A caller that sets "no limit" this way, which is a real
   choice next to the sibling fields of this struct, gets a server that
   answers every POST, PUT and PATCH with a body with a 413. This test uses
   a body of 200000 bytes, which is well above any ordinary default or
   small test limit. It sends that body to a buffered route and to a
   streaming route. Neither path may cap at zero. The test uses its own
   dedicated server. It does not use the shared g_small_body_srv fixture,
   whose max_body_size is fixed at SMALL_BODY_MAX. The max_body_size of 0
   here therefore cannot be confused with the boundary tests of that
   fixture, and those tests cannot disturb it. */
TEST(chttpserver, buffered_max_body_size_zero_means_unlimited) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_POST, "/unlimited-body-echo", _echo_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 80;
  cfg.max_body_size = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* _echo_body_handler writes the whole body back, so the response is as
     large as the request. This is why buf is on the heap and not a large
     stack array. Its size is body_len plus extra room for the headers. */
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
      ccol_create_chttpsvr(g_test_logger, NULL);
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
     diverted request must not force a synchronous close of the socket. The
     413 and ccol_msg_too_large tests above are the only other place in
     this file that exercises it. _drain_body reports the failure back to
     _task_worker as a ccol_retval_t. _task_worker maps that to a graceful
     synchronous status response. It must not tear the connection down at
     once, because that destroys the connection before the worker can write
     its own error response. The connection closes only after the server
     flushes that response, through the ordinary keep-alive or close
     decision downstream. This test exercises a different class of parse
     error on that same path. It sends a malformed chunk-size framing in a
     Transfer-Encoding: chunked body. The chunk-size decoder of
     chttp1_parser catches it. It is not a check of max_body_size or of a
     declared length. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* "ZZZZ" is not a valid hex chunk-size token. The parser must reject it
     while it consumes the body on the worker thread. That happens well
     after the routing, which runs when the headers are complete and has
     already matched /stream-error-report. */
  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "ZZZZ\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* This bounds the read loop below. The rule that the connection must
     close afterwards can regress. Without this bound, such a regression
     hangs this test, because it waits for more bytes or an EOF that never
     come. The test must fail its own assertion cleanly instead. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The malformed body must still give a real, complete HTTP response that
     reports the ingestion failure. It must not give a bare reset. The
     connection must then close. It must not stay alive for a next request
     that is corrupt. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") == NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, oversized_chunk_size_hex_rejected_gracefully) {
  /* "8000000000000000" is a chunk-size token of 16 hex digits with valid
     syntax. As a uint64_t it is about 9.2 exabytes. The chunk-size decoder
     of this parser is pure unsigned arithmetic with correct overflow
     checks, so there is no class of bug here about a signed negative
     value. The max_chunk_size_override of chttp1_parser_t makes a value
     this large a rejection at decode time. _conn_reset_for_request wires
     that field from chttpsvr_config_t.max_body_size. Without it, the
     parser accepts the value as the declared size of the current chunk.
     The connection then waits for that many bytes of chunk data that never
     arrive. Only the stream_read_timeout_us of the server settles it,
     which is 30s by default on g_srv, about 30 real seconds later. An
     assertion on the error value alone, that is, on any x-stream-err value
     other than "none", passes in both cases. This is why the test also
     checks the elapsed time. The server must reject the oversized chunk as
     soon as it parses the chunk-size line. It must never wait for the data
     of that chunk, which does not exist. It must report the same
     ccol_msg_too_large that a merely oversized *cumulative* body gives;
     see streaming_max_body_size_exceeded_reported. The round trip must
     finish well inside a second, and not in 30. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "8000000000000000\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* This bounds the read loop below. The fast rejection can regress. A
     regression that waits on stream_read_timeout_us instead hangs this
     test inside read() itself. The elapsed_s check below then never fails,
     because that check runs only after the loop returns. */
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
  /* The companion of the chunked case above, for plain Content-Length
     framing. A buffered route can get a declared Content-Length that is
     already above max_body_size. The server must reject it with a 413 at
     once, when the headers are complete. That is the upfront check of
     _on_headers_complete. It must reject before it diverts to a worker
     thread and before it waits for any body byte. It must not wait until
     that many bytes arrive, because the peer here never sends them at all.
     g_small_body_srv, on TEST_PORT+3, has max_body_size == 64. */
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 3);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* This request declares much more than SMALL_BODY_MAX, which is 64. It
     never sends a single body byte. _setup registers /small-body-echo as a
     plain route with chttpsvr_register_handler, and not as a streaming
     one. */
  const char *req =
      "POST /small-body-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 999999999\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* This bounds the read loop below. The upfront rejection can regress. A
     regression that waits for body bytes that never arrive hangs this test
     inside read() itself. The elapsed_s check below then never fails. */
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
  /* The "0\r\n" that ends a chunked body can carry a trailer part after
     it, before the final CRLF. That trailer part is zero or more
     header-field lines, per RFC 7230 SS4.1.2. The server dispatches a
     route with CHTTP1_HEADERS_DIVERT_BODY; see _on_headers_complete. It
     must call on_headers_complete exactly once for one request. This holds
     even when a real trailer field follows the body. This test checks two
     things. The server accepts and parses a trailer field at all. The
     server also dispatches the request to the handler exactly once. The
     test checks the second one indirectly: exactly one response comes
     back, and the connection acts as an ordinary single request and
     response on a Connection: close connection. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* Exactly one response must arrive. A second on_headers_complete
     dispatch for the end of the trailer adds extra bytes to that one
     response and corrupts it. It can also crash the worker or hang the
     connection. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, buffered_route_trailer_not_visible_via_req_header) {
  /* The documented contract of chttpsvr_req_header says that a trailer
     field of a chunked body (RFC 7230 SS4.1.2) is not a request header,
     and that it reads as absent. See the doc comment of that function in
     chttpserver.h. A regression is most visible on a BUFFERED route. The
     server always reads the whole body there, with the trailers, before
     the handler runs. The parser therefore reads the trailer before the
     handler asks. See _buffered_trailer_echo_handler. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* This is the privilege escalation that the split between headers and
     trailers closes. A reverse proxy sets X-Forwarded-For, and an
     authentication gateway sets X-Authenticated-User. The client then adds
     chunked trailer fields with those same two names and values of its own
     choice. chttpsvr_req_header resolves a repeated header name to its
     LAST use in wire order. An index that holds a trailer next to the
     headers therefore outranks both of them. The handler then gets the
     values of the client, on a request that the server already routed and
     ran its middleware over. Both header values must come back
     unchanged. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* The other half of the same isolation. The server must not interpret a
     trailer field named Connection, Content-Length or Transfer-Encoding.
     The server settles the framing before it reads the body, and the
     keep-alive decision of the connection follows from the header block. A
     "Connection: close" trailer must therefore leave this connection
     usable again. The check is that the server answers a second request on
     the same socket. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* RFC 7230 SS3.1.1 and SS5.3 allow no whitespace inside a
     request-target. A HTAB that gets through is a primitive for request
     smuggling. A front end that splits the request line on whitespace
     reads "GET /hello", where this server reads "GET /hello<HTAB>x". The
     parser rejects the request line outright, so the server answers 400
     Bad Request and closes. An accepted target gives a ROUTED answer
     instead. That answer would be a 404 here, because no route matches the
     path with the tab in it. The two are told apart by the status, and not
     by the presence of a response. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* A 404 here would mean that the tab reached the route search as
     ordinary target content, which is the smuggling primitive that this
     test exists to refuse. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

/* A small helper for the rejection tests below. It opens a connection to the
   shared test server, writes one raw request, reads until the server closes,
   and gives the bytes back. The read is bounded, so a regression that keeps
   the connection open fails the assertion instead of hanging the binary. */
static void _raw_request_to_test_server(const char *req, char *buf,
                                        size_t buflen) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  memset(buf, 0, buflen);
  _drain_socket_until_eof(fd, buf, buflen);
  close(fd);
  fd = -1;
}

TEST(chttpserver, http_1_1_request_without_host_gets_400) {
  /* RFC 7230 SS5.4 makes this a MUST for a server. A request with no Host
     names no authority, and a front end and a back end can resolve that
     differently. The server must refuse it before any routing, so the 200
     of the /hello route must not appear. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, request_with_two_host_headers_gets_400) {
  /* The same MUST, and the smuggling-relevant half of it. Two Host lines let
     a front end forward one authority while this server reads another. */
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
  /* The missing-Host half of the rule names HTTP/1.1 alone. An HTTP/1.0
     request predates the field and must still be served. */
  char buf[1024];
  _raw_request_to_test_server("GET /hello HTTP/1.0\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(chttpserver, absolute_form_request_target_is_served) {
  /* RFC 7230 SS5.3.2: "a server MUST accept the absolute-form in requests".
     A client configured with an explicit proxy sends one, and so does a
     proxy that forwards the target it received unchanged. Without the strip,
     the route search sees the four segments "http:", "", "host" and "hello",
     matches nothing, and answers 404 for a path that this very server
     serves. */
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
  /* RFC 7230 SS5.3.1: an empty path component stands for "/". The "/"
     sub-router of this suite answers the exact root path with the body
     "root_subrouter", so that body is the proof that the empty component
     really became "/". A 404 would mean that the server routed on an empty
     path, and a 400 would mean that it could not read the target at all. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "root_subrouter") != NULL);
}

TEST(chttpserver, absolute_form_with_query_only_is_the_root_path) {
  /* The same rule with a query attached: "http://host?a=1" has an empty
     path component and the query belongs to it. */
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
     scheme is not an absolute-form target that this server strips, and it
     does not begin with "/", so it is no origin-form target either. The
     server refuses it with a 400 and must not invent a path out of it. */
  char buf[1024];
  _raw_request_to_test_server(
      "GET ftp://example.com/hello HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, malformed_chunked_body_gets_400_and_not_500) {
  /* A chunk size that is not hexadecimal is a client error, and RFC 7231
     SS6.5.1 gives it 400. A 500 here would blame the server for what the
     client sent, and would count a client fault into a 5xx rate. */
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
  /* max_body_size keeps its own status. The 400 of a malformed body must
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
  /* An application that tunes nothing runs CHTTPSVR_CONFIG_DEFAULT. The
     shipped values are therefore a security property of their own. With
     the idle timeout off, a connection that opens and then says nothing
     holds its slot until the peer closes it. With the caps on the total
     duration off, a peer that trickles a body, or reads a response one
     byte at a time, holds a worker thread for as long as it likes. This
     test pins only what the library ships. Other tests pin what an
     explicit 0 in each field means, and they start a real server with that
     value. See
     stream_read_timeout_ms_zero_means_wait_indefinitely,
     max_body_read_duration_disabled_allows_slow_drip and
     max_response_write_duration_disabled_allows_slow_reader). */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_GT(cfg.read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.idle_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.stream_read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_body_read_duration_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_response_write_duration_us, (uint64_t)0);
  REQUIRE_GT((long)cfg.max_connections, 0L);
}

TEST(chttpserver, streaming_route_trailer_never_visible_via_req_header) {
  /* This is the documented contract of chttpsvr_req_header for a STREAMING
     route. A trailer field reads as absent before and after the
     chttpsvr_req_read() calls of the handler drain the body to EOF. The
     "after" answer is the load-bearing one. The parse of the body and of
     the trailer of a streaming route advances only when the handler calls
     chttpsvr_req_read(). Once that loop returns 0, the parser has read the
     trailer. The trailer must still not be readable. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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
  /* http1_atol accepts a '-' at the start, so "Content-Length: -1" parses
     correctly unless something rejects it explicitly. http1_consume_body
     reads a content_length of 0 or less as "no body, already complete",
     the moment the headers finish. The body bytes that the client did send
     are then parsed again as the start of the next pipelined request. That
     is a desync of the framing. http1_consume_header_top catches this
     during the parse of the headers, before any routing or diversion. The
     behaviour is therefore the ordinary answer of the parser to malformed
     input. The server closes the connection outright, with no HTTP
     response at all, and not a graceful error page. Every other parse
     error before the routing behaves the same way in this parser, for
     example a request line with invalid syntax. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: -1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* This bounds the read loop below. The reject-and-close behaviour can
     regress. A regression that keeps the connection open instead hangs
     this test inside read(). The test must fail its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The server answered 400 Bad Request and closed. The request never
     reached a handler, so no routed answer appears. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, chunked_not_last_in_transfer_encoding_list_rejected) {
  /* RFC 7230 SS3.3.1 says that `chunked`, when it is present, must be the
     last transfer-coding. http1_consume_header_transfer_encoding has two
     fast paths. One handles "chunked" alone, and the other handles
     "chunked" as the last item in the list. The parser must reject
     everything else outright, for example "chunked, gzip" or a `chunked`
     in the middle. A parser that stores it as an ordinary opaque header
     instead sets neither HTTP1_P_FLAG_CHUNKED nor a usable Content-Length.
     It then frames the body as an implicit message of zero length. Any
     upstream proxy that DOES honour a `chunked` anywhere in the list
     disagrees with it, which has the shape of request smuggling. The
     parser catches this while it reads the headers, before the routing.
     The behaviour is therefore an outright close of the connection with no
     HTTP response, as it is for the negative Content-Length case. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, gzip\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* This bounds the read loop below. The reject-and-close behaviour can
     regress. A regression that keeps the connection open instead hangs
     this test inside read(). The test must fail its assertion cleanly. */
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
   Transfer-Encoding uses the same treatment. "chunked," therefore names the
   one-element list "chunked", and the message is chunked-framed.

   A scan that counts the empty element as a real token sees the list as
   ending with that empty element instead. The message then is not
   chunked-framed, and a recipient that follows SS7 disagrees with it about
   where the body ends. That disagreement is what request smuggling needs.

   This test is non-vacuous. Against a scan that does not ignore an empty
   element, the trailing comma makes "chunked" a non-final coding, and the
   server answers 400 for a request that every conforming recipient accepts. */
TEST(chttpserver, trailing_comma_in_transfer_encoding_is_still_chunked) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

/* A leading empty element is the same rule seen from the other side, and an
   interior one must not turn a final "chunked" into a non-final one. */
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
    REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* The empty-element rule must not weaken the two refusals beside it. A
   "chunked" that is genuinely not last is still rejected, and a second
   application of "chunked" is still rejected, whatever empty elements sit
   around them. */
TEST(chttpserver, empty_elements_do_not_weaken_the_chunked_refusals) {
  const char *const reqs[] = {
      /* chunked genuinely not last, with an empty element in the list. */
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: ,chunked, gzip\r\n\r\n",
      /* chunked applied twice, with empty elements between. */
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
    REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* Connects to `port` and sends a POST. It writes the body of that POST one
   byte at a time, with 50ms between the bytes. A ctpool worker therefore
   stays blocked inside chttpsvr_req_read and http1_stream_read for the
   whole time. The function then drains and discards the response that it
   gets, or the abrupt close. It gives background traffic while the main
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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
  /* This bounds the drain loop below. A regression can stop destroy() from
     closing this connection. The measured time of this test is well under
     a second, which leaves a wide margin here. */
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

/* Private synchronization state for one run of each of the two tests
 * below. Those tests destroy a server from a background thread and wait
 * for it with a bound. They are
 * destroy_while_worker_reading_slow_body_is_safe and
 * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts.
 *
 * One mutex, condition variable and done flag at file scope, shared by
 * both tests, is unsafe here. The two tests run one after the other with
 * nothing in between. Each one detaches its thread on a timeout instead of
 * joining it, and that is deliberate; see the comment of either test. A
 * detached thread from an EARLIER test can therefore still run when a
 * LATER test starts. That thread can be merely slow and not a regression
 * at all. The later test then resets the shared `done` flag and waits on
 * the shared condition variable. The earlier thread finishes and
 * broadcasts while the later test waits. The later test then sees a stale
 * "done" signal that its own destroy_th never produced. It skips its own
 * safety net on the timeout path, which exists to stop it from blocking
 * without a bound on a thread that may hang. It falls through to a plain,
 * unbounded pthread_join of its OWN destroy_th. The chttpsvr_destroy()
 * call of that test can be the one that really hangs, which is the
 * regression that these tests catch. A clean, bounded, reported test
 * failure then becomes a silent, permanent hang of the whole binary. That
 * is exactly the failure that the comments of both tests say they
 * prevent. Each run of each test therefore gets its own private mutex,
 * condition variable and done flag. They sit in one struct with the srv
 * handle, on the heap, so the same timeout and detach path can leak them
 * safely, as it already leaks srv. This makes the problem impossible: no
 * thread of one test can ever see or signal the ctx of another test. */
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
  /* A ctpool worker can still hold state that the server owns, for example
   * srv->max_body_size, which chttpsvr_req_read reads. __chttpsvr_destroy
   * must not free that state while the worker is blocked and reads a slow
   * body on another connection. The _drain_and_close_all_connections of
   * chttpsvr_stop waits for in_flight_requests to reach zero before
   * anything frees the memory of the server. That is what this test
   * exercises. This test asserts on no return value. A use-after-free here
   * shows up under `make memtest` (valgrind), or as an abort or a crash in
   * a debug build, and that is the real check. The test uses its own
   * short-lived server, so it cannot disturb the shared test fixture.
   *
   * A background thread drives chttpsvr_destroy(), and this thread waits
   * for it with a bounded pthread_cond_timedwait. It does not make a bare,
   * unbounded call on this thread. That matches the pattern of
   * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts
   * below. A regression in the bounded wait and forced unblock of destroy
   * therefore fails this one test cleanly. See the comment of that test
   * for what the mechanism guards against. It must not hang the whole
   * binary with no failure to point at. ctx owns srv, and it lives on the
   * heap and not on the stack with RAII. The comment of that test gives
   * the same reason. This function must be able to leave the destroy
   * thread detached and never join it, when the bounded wait itself times
   * out. A stack local turns that bounded worst case into a use-after-free
   * as soon as a later test reuses the stack frame of this function. ctx
   * is private synchronization state of this test. See the comment of
   * _destroy_hang_ctx_t for why the sibling test below must not share
   * it. */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  REQUIRE_NE((void *)ctx, NULL);
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* This polls until the server accepts the background connection, parses
     its headers and dispatches it to a ctpool worker. in_flight_requests
     then goes above 0, which means that the worker is blocked inside
     chttpsvr_req_read in the middle of the drip. Only then does the test
     destroy the server under it. The test does not guess a fixed sleep. A
     bound of 5s is well clear of the drip of about 1s in this test. A slow
     run under CI or valgrind therefore cannot turn this into a destroy on
     a server that is already idle. The race that this test is named for is
     a destroy while a worker reads. On a timeout, the test joins bg before
     the REQUIRE_TRUE below. The read loop of bg is bounded by its own 5s
     SO_RCVTIMEO; see its comment. This follows the convention of this file
     to clean up without a condition before a REQUIRE. */
  extern int _chttpsvr_in_flight_requests_for_tests(chttpsvr h);
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

  /* This bound is generous. The drip of this test takes about 1s, which is
     20 bytes at 50ms each. The bounded-wait defaults of 30s and 5s keep
     their production size, because this test does not shrink them. The
     disabled-timeouts test below does shrink them. Here
     stream_read_timeout_us=2000000 is already enough for the graceful wait of
     destroy to succeed quickly. The bound is wide enough to fail cleanly
     on a loaded CI machine, and it does not mask a real hang. */
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

  /* The test joins bg without a condition, before the destroyed_in_time
     REQUIRE_* below, which can return early. The read loop of bg is
     already bounded by a 5s SO_RCVTIMEO; see the comment of
     _drip_body_bg_thread. A join here is therefore always safe, whatever
     the outcome of destroy_th is. A skip of it on a timeout leaves bg
     unreclaimed for the rest of the run of this binary, for no reason. */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach the thread and do not join it. On a real regression, the
       destroy call can never return at all. A block here then turns one
       clean, bounded, reported test failure into a second, silent hang.
       This one path already fails, and it leaks ctx and the srv that ctx
       owns on purpose. Nothing frees them. The detached thread still runs,
       so its own read of ctx can then never be a use-after-free. */
    pthread_detach(destroy_th);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);
}

/* Connects to `port` and sends a POST. That POST declares a
   Content-Length much larger than what it sends. It sends only a few bytes
   of the body. It then never sends the rest and never closes the
   connection. This is a peer that stalls forever, by design. The test
   destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts below
   uses it. That test sets both stream_read_timeout_us and
   max_body_read_duration_us to 0, which means "wait indefinitely". Only
   the forced-unblock mechanism of the server can then make the worker
   thread that reads the body of this connection return. See
   _force_unblock_diverted_connections in chttpserver.c. */
static void *_stall_forever_bg_thread(void *arg) {
  int port = *(int *)arg;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return NULL;

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
  /* This bounds the read loop below. A regression can stop the
     forced-unblock mechanism of the server from firing at all. The test
     that drives this thread shrinks the bounds of that mechanism to about
     2.3s in total, with _chttpsvr_set_wait_in_flight_bounds_for_tests.
     That is well clear of this margin, so this backstop cannot mask a real
     regression there. */
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
  write(fd, partial, strlen(partial)); /* far short of the declared length */

  /* This thread never sends the rest, and it never closes from this side.
     It blocks and waits for whatever the server does. The unblock itself
     is part of what this test proves. Without it, this thread hangs
     forever, and the pthread_join below hangs with it. */
  char buf[16];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
  }
  close(fd);
  fd = -1;
  return NULL;
}

/* chttpsvr_config_t.stream_read_timeout_us and max_body_read_duration_us
   both document a value of 0 as "wait indefinitely". That is a real,
   supported configuration, for a slow but legitimate upload for example.
   chttpsvr_destroy(), through _quiesce_server_once, always calls
   ctpool_shutdown_drain() on the worker pool. It does so shortly after its
   own careful 30s wait for the in-flight requests. ctpool_shutdown_drain()
   has no timeout of its own at all, and it blocks until every queued and
   active task finishes. Under this configuration, one peer can stall in
   the middle of a body. It sends part of the body, declares much more in
   the Content-Length, then never sends the rest and never closes. Against
   a streaming route, with the rate floor off as well, that peer holds the
   thread of the handler blocked forever inside chttpsvr_req_read, with
   nothing to unblock it. chttpsvr_destroy() then hangs forever, in
   spite of that careful bounded wait. What prevents this is a forced
   shutdown(2) of any connection that a worker thread still holds, once the
   bounded wait runs out. See _wait_in_flight_bounded and
   _force_unblock_diverted_connections in chttpserver.c. The real assertion
   of this test is that chttpsvr_destroy() returns at all. The test checks
   that with a bounded wait on a condition variable, and not with a bare
   pthread_join. A regression therefore fails this one test cleanly and
   does not hang the whole binary. The test uses its own short-lived server
   and not the shared g_srv. It turns both timeouts off, which is exactly
   the documented configuration that it needs. It also uses the white-box
   hook _chttpsvr_set_wait_in_flight_bounds_for_tests(). That hook shrinks
   the graceful-wait and grace-period bounds of _wait_in_flight_bounded
   from tens of real seconds to a few hundred milliseconds. The test
   therefore exercises the same escalation logic every time, and it does
   not wait out the real, production-sized timers. */
extern void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                          unsigned grace_ms);

TEST(chttpserver,
     destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts) {
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  /* ctx lives on the heap and not on the stack. _destroy_hang_thread_fn
     runs on a background thread. This function must be able to leave that
     thread running and never join it, when the bounded wait below times
     out. See the comment of that branch for why a join without a condition
     there can turn a real regression into a second hang that is harder to
     diagnose. A clean, reported test failure is what it must give instead.
     A stack local turns that bounded worst case into a use-after-free, as
     soon as a later test reuses the stack frame of this function. Every
     early-exit path below destroys and frees ctx itself. It does not rely
     on the early return of a REQUIRE_* plus a cleanup at scope exit, which
     cannot run here. A failure part way through this test therefore never
     leaks the reference that the shared engine holds to this server. A
     leaked server that the engine still references hangs this whole binary
     in the chttpsvr_engine_wait() call of _teardown. ctx is private
     synchronization state of this test. See the comment of
     _destroy_hang_ctx_t for why the sibling test above must not share
     it. */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  if (ctx == NULL) {
    /* Every early-exit path below also resets this process-wide override
       back to (0, 0), before its own REQUIRE_*. That matches the final
       reset at the very end of the path of success of this test, which
       runs without a condition. A value left shrunk here corrupts the
       graceful-wait and grace-period bounds for the rest of the run of
       this process. Every OTHER test in this binary that meets a really
       slow worker depends on those bounds. */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_NE((void *)ctx, NULL);
  }
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = ccol_create_chttpsvr(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_INVALID) {
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  /* A streaming handler is the one place where a thread still waits on a
     client for as long as the configuration lets it: it reads its body at
     the pace of the client. A buffered route parks instead, and holds no
     thread at all. */
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

  /* This polls until the server accepts the background connection, parses
     its headers and diverts it to a ctpool worker. in_flight_requests then
     goes above 0. The worker is now blocked inside chttp1_stream_read, and
     without this guard it stays blocked forever. Only then does the test
     destroy the server. The test does not guess a fixed sleep. A bound of
     5s is well clear of the shrunk wait-in-flight overrides of 300ms and
     2000ms above. A slow run under CI or valgrind therefore cannot turn
     this into a destroy on a server that is already idle. The race that
     this test is named for is a destroy while a worker is blocked forever.
     On a timeout, the test joins bg and resets the wait-in-flight override
     before the REQUIRE_TRUE below. The read loop of bg is bounded by its
     own 5s SO_RCVTIMEO; see its comment. This follows the convention of
     this test to clean up without a condition before a REQUIRE. */
  extern int _chttpsvr_in_flight_requests_for_tests(chttpsvr h);
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

  /* This bound is generous next to the shrunk overrides of 300ms and
     2000ms above. It gives a real margin for a loaded CI machine. It is
     nowhere near the real defaults of 30s and 5s that this test would
     otherwise need. */
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

  /* The test joins bg without a condition, before the destroyed_in_time
     REQUIRE_* below, which can return early. The read loop of bg is
     already bounded by a 5s SO_RCVTIMEO; see the comment of
     _stall_forever_bg_thread. A join here is therefore always safe,
     whatever the outcome of destroy_th is. A skip of it on a timeout
     leaves bg unreclaimed for the rest of the run of this binary, for no
     reason. */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach the thread and do not join it. On a real regression, the
       destroy call can never return at all. A block here then turns one
       clean, bounded, reported test failure into a second, silent hang.
       This one path already fails, and it leaks ctx and the srv that ctx
       owns on purpose. Nothing frees them. The detached thread still runs,
       so its own read of ctx can then never be a use-after-free. */
    pthread_detach(destroy_th);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);

  /* Restore the real, production-sized defaults for every later test in
     this same process. The override covers the whole process, and not one
     server. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
}

static _Atomic bool g_restart_race_stop;

/* Pipelines GET requests over `fd`, which is a keep-alive connection that
   is already open. It repeats until something sets g_restart_race_stop. It
   accepts any write or read failure and exits on it, because the main
   thread tears the listener side of the connection down and replaces it
   many times while this runs. The test
   restart_races_live_keep_alive_connection_is_safe below uses it. It keeps
   real pressure on the read of srv->worker_pool in _conn_start_diverted
   through a storm of restarts. That pressure also reaches the reads of
   srv->max_header_bytes and srv->max_body_size in _conn_reset_for_request
   and _on_body, on the worker thread that the call diverts to. */
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
  /* Every restart and teardown path of chttpsvr_start must write
   * srv->worker_pool under srv->mutex. That write drains, destroys or
   * creates the pool. The lock is needed because _conn_start_diverted
   * reads that field under it. A write with no synchronization is a real
   * data race. It also risks a use-after-free. A leftover pool can be
   * destroyed while the next pipelined request of a keep-alive connection
   * that is still open reads it or submits to it. chttpsvr_stop closes
   * only the listener, and the connections that the server already
   * accepted keep running. Their _on_headers_complete does not check
   * srv->lifecycle, so _conn_start_diverted can fire at any point during a
   * restart. _wait_and_detach_pools prevents this. It waits for
   * in_flight_requests to drain to zero before it detaches or destroys a
   * pool. No _conn_start_diverted call can then be in flight with a stale
   * copy of the pointer that goes to ctpool_destroy.
   *
   * This test also covers a close relative of that data race on the same
   * restart path. srv->max_header_bytes and srv->max_body_size must be
   * _Atomic, like stream_read_timeout_us and its neighbours on the same
   * struct. As plain fields, chttpsvr_start rewrites them with no
   * synchronization. It does so in the small gap after it publishes
   * srv->worker_pool and before it registers the listener again. The
   * worker thread of a keep-alive connection that already exists can read
   * either field inside that same gap, in _conn_reset_for_request or
   * _on_body. This test varies both fields on every restart, to exercise
   * that path. The window is narrow, so a plain CI run does not reliably
   * trip it; see the comment on the struct field for why. The real check
   * for both races is a clean run of this test under `make memtest`
   * (valgrind) and under `-fsanitize=thread`. A debug build aborts or
   * crashes outright if either race comes back.
   *
   * For the same reason, most of the body of this test asserts on no
   * return value. The hazards are data races and a use-after-free, and not
   * a wrong result. A background thread keeps one keep-alive connection
   * pipelining requests all the time. The main thread restarts the server
   * many times in a tight loop. Each cycle uses a fresh port, so the
   * timing of a bind on a port that just closed can never make this test
   * flaky. The race under test lives entirely on the srv side and not on
   * the port of the listener. The test uses its own short-lived server, so
   * it cannot disturb the shared test fixture. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* This bounds each read(2) inside the loop of
     _restart_race_pipeline_thread. That loop already accepts any read or
     write failure and exits on it; see the doc comment of that function.
     This bound therefore matters only for a real regression that hangs the
     connection and gives neither an error nor a response. Such a
     regression otherwise hangs the pthread_join below, and this whole
     binary, forever. The bound never fires during correct operation, and
     every real response here arrives well inside a second. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  atomic_store(&g_restart_race_stop, false);
  pthread_t pipeline_thread;
  bool pipeline_thread_created =
      pthread_create(&pipeline_thread, NULL, _restart_race_pipeline_thread,
                     &fd) == 0;

  /* The result of every restart goes into a local. No REQUIRE_* asserts on
     it at once. A REQUIRE_* that returns early in the middle of the loop,
     on a real regression, leaves pipeline_thread unjoined forever. That
     thread exits only when something sets g_restart_race_stop, which
     happens below. It also leaves fd open. The test then leaks a thread
     and a socket for the rest of the run of this binary, and it must
     merely fail this one test. */
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
  /* stream_read_timeout_us bounds only each single gap between batches of
   * body bytes. A client that sends a little data and then stalls inside
   * that gap never trips it. max_body_read_duration_us bounds the *total*
   * time that the server spends to read the body of one request. It does
   * so whatever the progress in each gap is, and it closes that loophole.
   * This test sets a generous timeout for each gap, so that one cannot
   * fire first. It sets a short cap on the overall duration next to it.
   * The client sends part of the declared body once and then never sends
   * the rest. That is the proven send-once-then-just-read pattern of
   * stream_read_timeout_reports_ccol_timed_out. A client that drips its
   * writes risks a server that responds and closes the connection in the
   * middle of the upload. The later writes of that client then fail with
   * EPIPE, before it ever reads the response. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/deadline-test", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 5;
  cfg.stream_read_timeout_us = 5000000;   /* generous; must not fire first */
  cfg.max_body_read_duration_us = 300000; /* the cap actually under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 5));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Well clear of the 300ms cap under test; bounds the read loop below in
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
      "abc"; /* 3 of the declared 10 bytes; never send the rest */
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

  /* The response body alone cannot tell which of the two caps fired. Both
   * stream_read_timeout_us and max_body_read_duration_us give the same
   * ccol_timed_out value. See _stream_err_to_retval and
   * req->_deadline_exceeded in src/chttpserver.c. Without a check of the
   * wall clock, a max_body_read_duration_us that is a no-op still passes
   * this test. The connection then sits until the 5000ms cap of
   * stream_read_timeout_us fires instead. That takes about 5s and not
   * about 300ms. This test therefore bounds the elapsed time well below
   * the 5000ms cap for each gap. The bound also leaves generous room above
   * the 300ms overall cap, for scheduling jitter under load or valgrind.
   * This proves that the *short* cap is what fired. */
  long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                    (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
  REQUIRE_LT(elapsed_ms, 2500L);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_body_read_duration_disabled_allows_slow_drip) {
  /* A max_body_read_duration_us of 0 means that there is no overall cap. A
   * slow but steady drip trips a short overall cap. Such a drip must still
   * succeed here. The stream_read_timeout_us of this server is generous
   * enough that the timeout for each gap does not fire either. This guards
   * against a deadline check that fires when it must do nothing. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* response_write_timeout_us bounds only each single call that is
   * equivalent to a write(2). A client that reads a byte or two just
   * before the timeout of each such call expires never trips it.
   * max_response_write_duration_us bounds the *total* time that the server
   * spends to send one response. It does so whatever the progress in each
   * call is, and it closes that loophole. It is the write-side equivalent
   * of max_body_read_duration_us. This test sets a generous timeout for
   * each call, so that one cannot fire first. It sets a short cap on the
   * overall duration next to it. The client sends its request and then
   * reads nothing at all, not even a slow trickle, for much longer than
   * the cap. The socket buffers of the kernel therefore fill up, and
   * chttp1_stream_write blocks and waits for space that never frees during
   * that silence. The silence of the client is load-bearing. A client that
   * reads the response back in a tight loop with no throttle, right after
   * it sends the request, drains the socket fast enough that the write
   * side never blocks at all. This test then passes and exercises nothing.
   * The symptom of that mistake is a measured elapsed time of a few
   * milliseconds, in place of the 300ms that a deadline which really fires
   * produces.
   *
   * The assertion itself compares byte counts and not times. A measure of
   * how long the read loop of the client takes once it reads again cannot
   * separate two cases on a fast loopback connection. One is "the deadline
   * already closed this connection". The other is "the deadline never
   * fired, but the write that is now unblocked finishes fast once the
   * reads resume". The rest of the transfer completes quickly in both
   * cases. The arrival of the FULL response does separate them. A deadline
   * that really fires closes the connection in the middle of the write,
   * during the silent window. No read afterwards can recover the rest of
   * the response, however long it tries. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 60;
  cfg.response_write_timeout_us = 5000000; /* generous; must not fire first */
  cfg.max_response_write_duration_us = 300000; /* the cap actually under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 60));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Genuine silence: no read() call of any kind for comfortably longer than
     the 300ms cap, so the server's own writes have every opportunity to
     fill the available buffers and actually block on backpressure well
     before the deadline is reached. */
  struct timespec nap = {0, 700000000L}; /* 700ms */
  nanosleep(&nap, NULL);

  /* Bounds every read call below in case this regresses (the connection
     would then still be open, and the previously-blocked write would only
     resume once we start draining, eventually delivering the full
     response): without this, a regression would hang this test instead of
     merely failing its assertion. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  size_t total = 0;
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) total += (size_t)r;
  close(fd);
  fd = -1;

  /* A working cap closes the connection mid-write during the silent
     window, so what (if anything) arrived afterward is necessarily a
     truncated prefix of the status line + headers + full body; well
     under the 16 MiB body alone. A regressed (no-op) cap would instead
     eventually deliver the complete response once reading resumes here,
     since nothing else in this setup closes the connection early (compare
     max_response_write_duration_default_disabled_allows_slow_reader, which
     confirms this exact handler/setup delivers the full response when the
     cap is off). */
  REQUIRE_LT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_disabled_allows_slow_reader) {
  /* max_response_write_duration_us set to 0 means "no overall cap"; a
     response the client reads slowly but steadily must still be delivered
     in full there, on a server whose response_write_timeout_us is generous
     enough that the per-call timeout doesn't fire either. Guards against
     the deadline check misfiring when it's supposed to be a no-op. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
     rather than deliver the response (max_response_write_duration_us is
     disabled here specifically, so there is no server-side deadline left to
     catch that class of bug): without this, read(2) would block forever,
     hanging the whole test binary instead of failing this one assertion
     cleanly. Generous relative to the pacing this test itself introduces
     below (up to ~2.5s of deliberate 5ms naps for a 16 MiB body). */
  struct timeval rcvtimeo = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response-slow-ok HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Reads in small, deliberately-paced chunks (well within response_write_
     timeout_ms's own 5000ms per-call bound on each individual gap) until
     EOF, accumulating the total byte count actually received. */
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

  /* Must have received the entire response (status line + headers + the
     full 16 MiB body), not a truncated one cut short by a wrongly-firing
     deadline. */
  REQUIRE_GT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_bounds_rejection_response) {
  /* max_response_write_duration_us must bound a courtesy rejection response
     (404/405/413/500/...) exactly like it bounds a real, matched-route
     response: both are sent through the same _send_response, and a
     slow-read peer can stretch either one out indefinitely otherwise. Unlike
     max_response_write_duration_exceeded_closes_connection, a rejection's
     header block (status line + a handful of fixed headers, no body at all)
     is always well under a kilobyte and completes in a single write(2) call
     in practice, so there is no way to coax a genuine short write/expired-
     deadline outcome out of real socket buffering for it the way that other
     test does for a real, large, handler-supplied body; a dedicated
     RUNNING_UNIT_TESTS-only hook forces the very next write-deadline check
     inside _send_response to report "already expired" instead. */
  extern void _chttpsvr_force_next_response_write_deadline_expired_for_tests(
      void);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 65;
  cfg.response_write_timeout_us = 5000000;     /* generous; must not matter */
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Bounds the read below in case a regression makes the guarded
     write-deadline check stop being consulted at all for this path (the
     exact regressed behavior): without this, if the hook is silently never
     consumed, the server just sends an ordinary, complete 404 response and
     the read below would still return promptly with that response's bytes
     rather than hang; this timeout exists purely as defensive
     belt-and-suspenders, not because a plausible regression here would
     actually hang. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  _chttpsvr_force_next_response_write_deadline_expired_for_tests();

  const char *req =
      "GET /no-such-route-xyz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  /* fd carries its own scope-exit cleanup (_close_scoped_fd above), so an
     essentially-never-expected write(2) failure against this fresh, just-
     connected local socket does not leak it for the rest of this binary's
     run even if the REQUIRE_* below returns early. */
  ssize_t wn = write(fd, req, strlen(req));

  char buf[256];
  ssize_t r = (wn == (ssize_t)strlen(req)) ? read(fd, buf, sizeof(buf)) : -1;

  REQUIRE_EQ(wn, (ssize_t)strlen(req));

  /* The header-write loop's very first write-deadline check (before any
     real write(2) call is ever attempted) reports "already expired"
     through the forced hook, so _send_response returns false having sent zero
     bytes; the connection is then closed immediately, so the client observes
     EOF (read returns 0) with nothing at all received. This test is
     non-vacuous: a build that passes NULL for conn here never consults the
     hook at all (conn && ... short-circuits) and sends a complete, ordinary
     404 response instead, observed here as a positive byte count. */
  REQUIRE_EQ((int)r, 0);

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                         SERVER-OWNED LOGGER TESTS                          */
/* ========================================================================== */

TEST(chttpserver, create_with_null_logger_uses_internal_fatal_only_logger) {
  /* cl == NULL must be accepted: the server creates its own internal logger
   * (stderr, FATAL-only) instead of requiring a caller-supplied one. The
   * server must still be fully functional. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(CLOG_INVALID, NULL);
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
  /* cl != CLOG_INVALID must not be stored directly: the server derives its
   * own logger from it (tagged component=http-server) and closes only that
   * derived logger on destroy, leaving the caller's handle open and
   * reusable. */
  clog parent = clog_open_fd(2, CLOG_INFO, NULL);
  REQUIRE_TRUE(parent != CLOG_INVALID);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(parent, NULL);
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

  /* parent must still be alive: write through it, and derive another
   * (unrelated) server from it, both of which would misbehave under
   * valgrind/ASan if the server had wrongly closed the caller's handle. */
  ccol_log_info(parent, "parent logger still usable after server destroy");

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(parent, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_destroy(srv2);

  clog_close(parent);
}

/* ========================================================================== */
/*                    UNIX DOMAIN SOCKET TESTS                                */
/* ========================================================================== */

TEST(chttpserver, unix_socket_listen_and_round_trip) {
  /* "unix://path" on chttpsvr_config_t.host must bind a Unix domain socket
     instead of a TCP listener, and a request over that socket must be
     routed and answered exactly like a TCP connection would be. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

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

/* A TCP connection keeps the socket buffer sizes of the kernel: an explicit
   SO_SNDBUF or SO_RCVBUF locks that size and turns the buffer autotuning of
   Linux off for the socket (tcp(7)). A unix:// connection has no autotuning,
   and the server raises its buffers. A white-box counter in the library
   counts the connections whose buffers it raised. This test is non-vacuous:
   raising the buffers of a TCP connection makes the first half fail. */
extern size_t _chttpsvr_accepted_sockbuf_raise_count_for_tests(void);

TEST(chttpserver, accepted_tcp_socket_keeps_kernel_buffer_autotuning) {
  size_t before_tcp = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hello", NULL, buf, sizeof(buf));
  size_t after_tcp = _chttpsvr_accepted_sockbuf_raise_count_for_tests();
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(after_tcp, before_tcp);

  /* The counter is live: a unix:// connection does raise its buffers. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);
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

/* Leaves a stale socket file at path: a socket that was bound there and
   closed, with no process listening on it any more. Returns whether it
   did. */
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
  /* A stale socket file at the configured path, one that no process listens
     on, is replaced by the start instead of making bind() fail. */
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_stale_%d.sock",
           (int)getpid());
  REQUIRE_TRUE(_make_stale_unix_socket(sock_path));

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&addr, sizeof(addr)), 0);

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
  /* A directory component that doesn't exist must fail chttpsvr_start
     gracefully (bind() fails) rather than crashing or silently succeeding. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "unix:///chttpsvr_test_nonexistent_dir_xyz/socket.sock";
  ccol_retval_t rv = chttpsvr_start(srv, &cfg);
  REQUIRE_NE((int)rv, (int)ccol_success);
  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    MAX_CONNECTIONS TESTS                                   */
/* ========================================================================== */

TEST(chttpserver, max_connections_enforced) {
  /* With max_connections == 1, a second concurrent connection must be left
     pending in the kernel's listen backlog (never accept()'d, never
     served) until the first connection closes and frees the one slot. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Give the server a moment to accept() fd_a and occupy the one slot
     before fd_b tries to connect. */
  struct timespec nap = {0, 150000000L}; /* 150ms */
  nanosleep(&nap, NULL);

  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  /* Bounds the final _read_one_http_response call below in case a regression
     in the listener's own resume-on-capacity-freed path breaks it: without
     this, fd_b's underlying read(2) would block forever waiting for bytes
     that would then never arrive, hanging the whole test binary instead of
     failing this one assertion cleanly; exactly the class of bug this test
     exists to catch. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req =
      "GET /maxconn-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_b, req, strlen(req)), (ssize_t)strlen(req));

  /* fd_b's request must NOT be served yet: the server is at capacity. */
  struct pollfd pfd = {.fd = fd_b, .events = POLLIN};
  int pr = poll(&pfd, 1, 300);
  REQUIRE_EQ(pr, 0);

  close(fd_a); /* frees the one connection slot */
  fd_a = -1;

  /* Now fd_b must be accepted and served. */
  char buf[1024] = {0};
  int status = _read_one_http_response(fd_b, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  close(fd_b);
  fd_b = -1;
  chttpsvr_destroy(srv);
}

/* White-box counter from chttpserver.c; see its own doc comment above
   _listener_on_readable. */
extern size_t _chttpsvr_listener_dispatch_count_for_tests(void);

TEST(chttpserver, max_connections_at_capacity_does_not_busy_loop) {
  /* While a connection is left pending in the backlog at capacity, the
     server's listener registration must be paused, not merely left to be
     re-dispatched with nothing to do: level-triggered epoll re-reports a
     listen socket with a non-empty accept backlog as ready on every single
     epoll_wait call, so a listener that keeps returning without pausing
     would be re-dispatched continuously, pinning the reactor thread at
     ~100% CPU for as long as the server stays at capacity (directly
     measurable as CPU ticks in /proc/<pid>/stat; not itself asserted on
     here, since raw CPU-time measurement is inherently noisy on a
     shared/loaded machine). This is caught instead by
     directly counting how many times the listener was actually dispatched
     during a held-at-capacity window, with a white-box counter: a correctly
     paused listener produces at most a small, fixed handful of dispatches
     (the one that decided to pause), while a busy-looping one produces many
     thousands within a fraction of a second. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  REQUIRE_EQ(connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Give the server a moment to accept() both and occupy the two slots
     before fd_c tries to connect. */
  struct timespec nap = {0, 150000000L}; /* 150ms */
  nanosleep(&nap, NULL);

  int fd_c _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_c >= 0);
  /* Bounds the final _read_one_http_response call below in case a regression
     in the listener's own resume-on-capacity-freed path breaks it: without
     this, fd_c's underlying read(2) would block forever waiting for bytes
     that would then never arrive, hanging the whole test binary instead of
     failing this one assertion cleanly. Mirrors max_connections_enforced's
     own identical fd_b protection. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_c, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(connect(fd_c, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req_c =
      "GET /busyloop-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_c, req_c, strlen(req_c)), (ssize_t)strlen(req_c));

  /* Give the (at most one) capacity-triggered pause dispatch time to
     actually happen before starting the measurement window, then hold at
     capacity for a further 300ms with nothing else touching the server, and
     count how many further times the listener was dispatched during that
     hold. Everything below, up to and including __chttpsvr_destroy, runs
     unconditionally (results are captured into locals rather than asserted
     on immediately) so that even a genuine regression here (the listener
     staying live and busy-looping) gets this server torn down before any
     REQUIRE_* can end the test function early; leaving a still-registered,
     still-at-capacity listener behind would otherwise busy-loop forever on
     the one process-wide shared reactor thread this whole binary uses,
     hanging every later test that also needs it. */
  nanosleep(&nap, NULL); /* another 150ms */
  size_t before = _chttpsvr_listener_dispatch_count_for_tests();
  struct timespec hold = {0, 300000000L}; /* 300ms */
  nanosleep(&hold, NULL);
  size_t after = _chttpsvr_listener_dispatch_count_for_tests();
  size_t dispatch_delta = after - before;

  /* fd_c must not have been served during the hold either: still at
     capacity throughout. */
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

  /* A generous but still sharply discriminating bound: a correctly paused
     listener produces 0 further dispatches during the hold (it was already
     paused before the window started); a busy-looping one produces many
     thousands in 300ms (sub-microsecond dispatches at ~100% CPU). 200 is
     far above any legitimate jitter yet orders of magnitude below what a
     real regression here would produce. */
  REQUIRE_LT(dispatch_delta, (size_t)200);
  REQUIRE_EQ(pr_c, 0);
  REQUIRE_EQ(status_c, 200);
}

/* Allocator whose calloc() unconditionally fails (returns NULL) once armed,
   and otherwise behaves like a plain pass-through; malloc/free/realloc are
   always plain pass-throughs. _conn_create's own conn struct allocation is
   the only calloc() call this module ever routes through a server's mp
   during ordinary connection acceptance (see _listener_on_readable), so
   arming this after chttpsvr_start() has already returned (all of the
   server's own setup allocations are done by then) targets exactly that one
   allocation for every connection accepted afterward, with no need to guess
   or match an exact byte size the way the fail-at-size allocator elsewhere
   in this file does. */
static _Atomic bool g_fail_every_calloc_for_backoff_test = false;
static void *_backoff_test_malloc(size_t n) { return malloc(n); }
static void _backoff_test_free(void *p) { free(p); }
static void *_backoff_test_calloc(size_t n, size_t s) {
  if (atomic_load(&g_fail_every_calloc_for_backoff_test)) return NULL;
  return calloc(n, s);
}
static void *_backoff_test_realloc(void *p, size_t s) { return realloc(p, s); }

extern size_t _chttpsvr_listener_alloc_failure_pause_count_for_tests(void);

/* After a successful accept4(), a _conn_create()/ctls_conn_create_server()
   failure (allocation failure) must not loop straight back to accept4()
   again with zero backoff. It needs the same backoff accept4()'s own
   EMFILE/ENFILE/ENOBUFS/ENOMEM handling a few lines up in the same loop
   already applies, for the same
   reason: to avoid busy-looping a persistent resource-exhaustion condition.
   Under a sustained allocation-failure condition (a custom, bounded
   ccol_memmgmt_procs_t is the realistic trigger) combined with connections
   continuing to arrive, no backoff spins the reactor thread with zero
   progress.

   srv's listener registration is paused on the very first allocation
   failure (see _listener_pause_for_resource_pressure), rather than merely
   sleeping before retrying inline: sleeping still blocks the one process-
   wide shared reactor thread for that sleep's duration on every single
   re-dispatch, starving every OTHER connection on every OTHER server
   sharing that reactor thread for as long as the condition persists,
   whereas pausing removes the reactor from the picture entirely until the
   idle-timeout sweep thread resumes it (see _listener_resume_if_resource_
   pressure_cleared), within at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS of the
   condition clearing.

   Verifies both halves with deterministic state checks rather than an
   inference from
   them from network activity observed over some timing window: the
   idle-timeout sweep thread's own real, ~1s timer runs unsynchronized with
   this (or any) test's clock, and can legitimately
   resume, and (with nothing queued behind fd_a to re-fail against) leave
   resumed, this exact listener at any point during the test, making a
   network-observed window an inherently flaky signal for "is it currently
   paused". (1) the pause is genuinely reached and genuinely takes effect:
   both the white-box counter and the listener_paused_for_resource_pressure
   flag itself are checked immediately after fd_a's own failure. (2) the
   pause is not permanent: once the failure condition clears, a fresh
   connection is still eventually accepted and served, through the
   idle-timeout
   sweep thread's own resume. */
TEST(chttpserver,
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping) {
  extern bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(
      chttpsvr h);

  ccol_memmgmt_procs_t mp = {_backoff_test_malloc, _backoff_test_free,
                             _backoff_test_calloc, _backoff_test_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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

  /* Armed only now, after chttpsvr_start() has already returned: every
     connection this test itself makes from here on is the only thing that
     can possibly hit _conn_create's calloc from this point forward. */
  atomic_store(&g_fail_every_calloc_for_backoff_test, true);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* fd_a must be accepted, fail its conn allocation, and be closed with no
     bytes ever sent; confirmed by reading EOF (0), bounded by the
     SO_RCVTIMEO set above so a regression (the connection never actually
     processed) fails this REQUIRE_* cleanly rather than hanging the whole
     binary. */
  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  /* close(cfd) on the server side (which is what delivers this EOF to fd_a)
     runs BEFORE _listener_log_and_pause_for_alloc_failure's own pause and
     counter increment, not after: observing EOF here only proves the
     connection was closed, not that the server thread has already gone on
     to actually pause and increment the counter too. Poll for the counter
     bounded to 2s (comfortably past ordinary scheduling jitter, sharply
     below a hang) rather than reading it exactly once immediately after the
     EOF read returns, which races TSan's own scheduling and fails
     intermittently (the two threads run on genuinely different CPUs; EOF
     delivery does not wait for the server thread to reach its next
     statement). */
  /* Poll the pause flag itself, directly and repeatedly, rather than
     inferring "the pause happened" from a single read of the counter and
     then doing one, one-shot flag read immediately after: the counter is
     incremented BEFORE _listener_pause_for_resource_pressure is even called
     (see _listener_log_and_pause_for_alloc_failure), and that call performs
     real work of its own (a mutex lock/unlock plus a full
     ccol_event_loop_pause() call, itself a real syscall, not merely a few
     uncontended instructions) before the flag is actually stored. A single flag
     read timed off the counter's own change can therefore land in that real,
     non-negligible gap and observe `false` even though the pause is about to
     succeed a moment later; a failure that does occur in practice. Polling the
     flag directly, bounded to 2s, has no such gap: once it reads true, the
     pause has genuinely already taken effect, and since the counter increment
     strictly precedes it in program order on the same thread, the counter is
     guaranteed to already reflect the change too by that point.

     One further, narrower residual race is worth naming explicitly: the
     real idle-timeout sweep thread runs continuously for this whole test
     binary on its own ~1s timer, entirely independent of this test's own
     timing, and _listener_resume_if_resource_pressure_cleared unconditionally
     atomic_exchanges the flag back to false on every tick. If a tick happens
     to land in the handful of microseconds between the flag being stored
     true and this loop's very first read, that read would observe false
     for the entire 2s bound. Every later read would too, since nothing
     else re-triggers the failure once fd_a alone has been processed.
     Unlike the gap this polling loop closes (a real syscall's worth of
     width, and one that does fail in practice), this window is bounded by
     this loop's own per-iteration overhead against a fixed ~1000ms period,
     several orders of magnitude narrower: 30 consecutive runs under
     valgrind and 25 under ThreadSanitizer show zero failures, matching this
     file's own tolerance for a comparably narrow, already-documented timing
     window elsewhere (see start_racing_engine_stop_and_
     destroy_does_not_free_raw_too_early in tests_engine_stop.c). */
  bool paused_flag = false;
  {
    struct timespec pause_deadline;
    clock_gettime(CLOCK_MONOTONIC, &pause_deadline);
    pause_deadline.tv_sec += 2;
    for (;;) {
      paused_flag =
          _chttpsvr_listener_paused_for_resource_pressure_for_tests(srv);
      if (paused_flag) break;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!(now.tv_sec < pause_deadline.tv_sec ||
            (now.tv_sec == pause_deadline.tv_sec &&
             now.tv_nsec < pause_deadline.tv_nsec)))
        break;
      struct timespec nap = {0, 1000000L}; /* 1 ms */
      nanosleep(&nap, NULL);
    }
  }
  size_t pause_after = _chttpsvr_listener_alloc_failure_pause_count_for_tests();

  /* Clear the failure condition and confirm the pause is not permanent: the
     idle-timeout sweep thread must eventually resume the listener (within
     at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, which is about 1s) and let
     a fresh
     connection be accepted and served normally. Bounded by a generous
     SO_RCVTIMEO so a regression in the resume path fails this one
     assertion cleanly instead of hanging the whole binary, mirroring
     max_connections_at_capacity_does_not_busy_loop's own fd_c protection. */
  atomic_store(&g_fail_every_calloc_for_backoff_test, false);
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  struct timeval rcvtimeo_b = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_b, sizeof(rcvtimeo_b));
  REQUIRE_EQ(connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  /* Exactly one pause triggered by fd_a's own allocation failure. */
  REQUIRE_EQ(pause_after - pause_before, (size_t)1);
  REQUIRE_TRUE(paused_flag);
  REQUIRE_EQ(status_b, 200);
}

/* The accept loop pauses unconditionally for any accept4() errno that isn't
   EWOULDBLOCK/EAGAIN/EINTR/one of _accept_errno_is_transient's own per-
   connection cases - not only the specific EMFILE/ENFILE/ENOBUFS/ENOMEM
   resource-exhaustion allow-list _accept_errno_is_resource_exhaustion names
   (see that helper's own comment and the one call site's own comment in
   _listener_on_readable_impl). This is the only test that drives a real
   accept4() failure with an UNCLASSIFIED errno (EBADF/EINVAL/ENOTSOCK/
   EFAULT and similar) through the loop to confirm it still pauses rather
   than busy-loops for one of those, and it is non-vacuous: an allow-list-
   gated design (pausing only for the four named resource-exhaustion errnos)
   passes every other test in this file untouched and fails only this one.

   Uses _chttpsvr_force_next_accept_errno_for_tests (a white-box hook scoped
   to one specific server, so this file's own long-lived shared fixture
   server or any other concurrently active server in this process can never
   steal the forced errno intended for srv; an unscoped hook hits exactly
   that race intermittently under valgrind) to make the very next accept4()
   dispatch for srv report EINVAL - deliberately NOT one of the four
   resource-exhaustion errnos, so this specifically exercises the
   "unclassified" branch - without needing to actually corrupt a real fd or
   exhaust a real system resource, which would be environment-dependent and
   disproportionate for what is, underneath, a simple control-flow decision.
   Confirms the LISTENER's own reaction
   directly with the deterministic pause-flag accessor (immune to the real
   idle-sweep timer's own independent schedule, unlike inferring pausedness
   from network-observed timing; see post_accept_alloc_failure_pauses_and_
   resumes_instead_of_busy_looping's own identical reasoning), then confirms
   the pause is not permanent, because a fresh connection succeeds once the
   real sweep resumes the listener. */
TEST(chttpserver,
     accept_unclassified_errno_pauses_and_resumes_instead_of_busy_looping) {
  extern void _chttpsvr_force_next_accept_errno_for_tests(chttpsvr h,
                                                          int errno_val);
  extern bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(
      chttpsvr h);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* Armed only now, after chttpsvr_start() has already returned, and scoped
     to srv specifically: the very next accept4() dispatch for THIS server
     is the only one affected, so this test's own long-lived shared fixture
     server (or any other server concurrently active in this process) never
     steals it. */
  _chttpsvr_force_next_accept_errno_for_tests(srv, EINVAL);

  int fd_a _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Poll the pause flag directly and repeatedly, bounded, STARTING
     IMMEDIATELY after connect() rather than only after first blocking on
     fd_a's own EOF read: this hook's own close(cfd) call (which is what
     delivers EOF to fd_a below) runs before _listener_log_accept_err_rate_
     limited/_listener_pause_for_resource_pressure even begin (a real,
     rate-gated log write, potentially slow under valgrind/a loaded CI
     runner), so waiting for that blocking read to return first - rather
     than starting this poll loop right away - measurably widens the real
     gap between "connection closed" and "flag actually stored", giving the
     real idle-sweep timer's own independent ~1s schedule more room to fire
     and clear the flag again before this loop's very first iteration ever
     observes it true (an intermittent, real failure that way; see
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping's
     own analogous, narrower residual-race discussion - that ordering makes
     the same class of race meaningfully wider here). Starting the poll
     immediately closes that self-inflicted gap down to this loop's own
     per-iteration overhead, matching that sibling test's own tight margin:
     120 consecutive runs under valgrind and 40 under ThreadSanitizer pass
     with zero failures this way, versus multiple failures within roughly
     100 runs when the poll starts only after the blocking read. */
  bool paused_flag = false;
  {
    struct timespec pause_deadline;
    clock_gettime(CLOCK_MONOTONIC, &pause_deadline);
    pause_deadline.tv_sec += 2;
    for (;;) {
      paused_flag =
          _chttpsvr_listener_paused_for_resource_pressure_for_tests(srv);
      if (paused_flag) break;
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      if (!(now.tv_sec < pause_deadline.tv_sec ||
            (now.tv_sec == pause_deadline.tv_sec &&
             now.tv_nsec < pause_deadline.tv_nsec)))
        break;
      struct timespec nap = {0, 1000000L}; /* 1 ms */
      nanosleep(&nap, NULL);
    }
  }

  /* fd_a's own real accept4() succeeds internally, but the hook discards
     that real connection and simulates EINVAL instead; the server closes
     the real (never read-from/written-to) fd immediately (strictly before
     the poll loop above could have observed the pause flag at all, per its
     own comment), so fd_a observes a clean EOF, not an error, and this read
     returns immediately regardless of how long the poll above took. */
  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  /* Confirm the pause is not permanent: the idle-timeout sweep thread must
     eventually resume the listener (within at most
     _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, which is about 1s) and let a fresh
     connection
     be accepted and served normally. Bounded by a generous SO_RCVTIMEO so a
     regression in the resume path fails this one assertion cleanly instead
     of hanging the whole binary. */
  int fd_b _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  struct timeval rcvtimeo_b = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_b, sizeof(rcvtimeo_b));
  REQUIRE_EQ(connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  REQUIRE_TRUE(paused_flag);
  REQUIRE_EQ(status_b, 200);
}

/* ========================================================================== */
/*                    MAX_HEADER_BYTES TESTS                                  */
/* ========================================================================== */

TEST(chttpserver, max_header_bytes_within_limit_succeeds) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* A header block exceeding the configured cap must be rejected before
     routing. The server answers 400 Bad Request and then closes, which is
     what every other pre-routing parse error in this parser does (see
     negative_content_length_rejected above). The rejection must never reach
     the handler, so no routed 200 can appear.

     The 400 is best effort, and it is deliberately not the whole assertion.
     A peer that is still streaming its oversized header block when the
     server closes can meet a reset before it reads anything, so the read
     below can legitimately come back empty. What must NEVER appear is a
     routed answer. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* This bounds the read loop below. The reject-and-close behaviour can
     regress. A regression that keeps the connection open instead hangs
     this test inside read(). The test must fail its assertion cleanly. */
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
  /* Neither max_header_bytes_within_limit_succeeds nor _exceeded_closes_
     connection above sends a header block of EXACTLY max_header_bytes
     bytes; both stay comfortably clear of the boundary (a small ordinary
     request against a 512-byte cap; ~330 bytes against a 128-byte cap).
     An off-by-one regression in chttp1_parser.c's own strictly-greater-than
     comparison (process_header_line's "total_header_bytes + contribution >
     max_bytes" check, and the identical check for the request line's own
     contribution) would pass both existing tests unnoticed. This test
     constructs a request whose request-line-plus-headers contribution is
     computed to land EXACTLY on the configured cap (must succeed) and,
     separately, exactly one byte over it (must fail), mirroring
     buffered_max_body_size_at_limit_succeeds/_exceeded_rejected's own
     exact-boundary pattern for max_body_size. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* Every line's own contribution is strlen(line) + 2 (the CRLF the
     accumulator strips), including the request line itself; see
     process_header_line's and its request-line sibling's identical
     "+ 2" comment in chttp1_parser.c. */
  const char *request_line = "GET /hdrcap-boundary HTTP/1.1";
  const char *host_line = "Host: 127.0.0.1";
  size_t used = (strlen(request_line) + 2) + (strlen(host_line) + 2);
  REQUIRE_LT(used + 9, (size_t)MAX_HDR_BYTES); /* sanity: room for a filler */
  size_t budget_left = (size_t)MAX_HDR_BYTES - used;
  /* "X-Pad: " (7 bytes) + N filler bytes + 2 (CRLF) must equal budget_left
     exactly for the at-the-limit case. */
  size_t pad_len = budget_left - 9;

  char padding[256];
  REQUIRE_LT(pad_len, sizeof(padding));
  memset(padding, 'a', pad_len);
  padding[pad_len] = '\0';

  /* Exactly at the limit: must succeed. */
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
    REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

  /* One byte over the limit: must fail (connection closed, no
     response), reusing the exact same shape with one extra filler byte. */
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
    REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);
    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    _drain_socket_until_eof(fd, buf, sizeof(buf));
    close(fd);
    fd = -1;
    /* One byte over the cap is a rejection: a 400 and never a routed
       answer. The byte exactly at the cap gave the 200 above. */
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
  for (int i = 0; i < 400; i++) /* ~25 MiB total: comfortably bigger than any
                                    default OS socket buffer, so a client
                                    that never reads is guaranteed to
                                    eventually stall the server's write. */
    chttpsvr_resp_write(resp, chunk, sizeof(chunk));
}

TEST(chttpserver, response_write_timeout_closes_slow_reader_connection) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req = "GET /big-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Deliberately never read while the server is writing: its send must
     eventually stall against our never-drained receive buffer, hit
     response_write_timeout_us, and force the connection closed rather than
     pinning a thread forever. The sweep judges a parked response once a
     second, and a send queue that shrank since its last look counts as
     progress, so the limit is exact to one sweep interval. Sleep past the
     timeout plus two sweep ticks before touching the socket, so that the
     server has already made its decision by the time we look: the first
     tick can still see the queue shrink as the kernel fills our receive
     buffer, and the second one sees a queue that no reader drains. */
  struct timespec wait_past_timeout = {3, 0}; /* > 200ms cfg + 2 ticks */
  nanosleep(&wait_past_timeout, NULL);

  /* Bounds every read call below in case this regresses (the connection
     would then still be open, and this test would otherwise block in read()
     forever instead of failing its assertion cleanly): matches
     max_response_write_duration_exceeded_closes_connection's own identical
     precaution, for exactly the same reason. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) >
         0) { /* drain whatever got through */
  }
  REQUIRE_EQ(r, 0); /* EOF: server closed the connection */

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP TEST                                 */
/* ========================================================================== */

TEST(chttpserver, idle_timeout_closes_unused_connection) {
  /* A connection that never sends a request at all must eventually be
     closed by the module-local idle-timeout sweep thread once
     idle_timeout_us has elapsed, rather than being held open forever. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  /* Never send anything: nothing but the idle-timeout sweep can possibly
     make this fd readable (an EOF), since the server has no data of its
     own to proactively push. */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 3000); /* generous vs. idle_timeout_us=300000 and the
                                   sweep's own ~1s interval */
  REQUIRE_TRUE(pr > 0);
  char buf[16];
  ssize_t r = read(fd, buf, sizeof(buf));
  REQUIRE_EQ(r, 0);

  chttpsvr_destroy(srv);
}

/* The idle sweep captures its own `now` snapshot once, before it ever takes
   idle_mutex or examines any specific connection; a connection's
   last_activity is refreshed by _idle_list_add from a DIFFERENT thread
   every time it lands back in the idle list, including the ordinary case of
   finishing a keep-alive request concurrently with a sweep tick. A
   connection that gains fresh activity in the window between the sweep's
   own `now` snapshot and the sweep actually reaching that connection
   therefore legitimately has last_activity > now. The elapsed-time
   computation must therefore not be plain `long` arithmetic compared via
   `(unsigned long)elapsed_ms >= idle_ms`: that silently wraps a small
   negative elapsed_ms to a huge unsigned value (unconditionally >= idle_ms
   for any realistic timeout), spuriously evicting a connection that has
   just become idle instead of recognizing it as having zero (or negative)
   elapsed idle time. Exercised directly through the pure decision function
   rather than by trying to actually win a real scheduling race against a
   live sweep thread. */
TEST(chttpserver, idle_sweep_negative_elapsed_never_flagged_as_timed_out) {
  extern bool _chttpsvr_conn_idle_timed_out_for_tests(
      struct timespec now, struct timespec last_activity, unsigned idle_ms);

  /* The racing case itself: last_activity is "in the future" relative
     to the sweep's own now snapshot (last_activity gained fresh activity
     after now was captured but before this connection was examined). Must
     never be reported as timed out, regardless of how large idle_ms is (a
     small idle_ms is exactly what an unsigned wraparound would make
     unconditionally true). */
  struct timespec now = {.tv_sec = 1000, .tv_nsec = 0};
  struct timespec last_activity_future_by_1ms = {.tv_sec = 1000,
                                                 .tv_nsec = 1000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 1));
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 0 /* still must not underflow */));

  struct timespec last_activity_future_by_5s = {.tv_sec = 1005, .tv_nsec = 0};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_future_by_5s, 300));

  /* A future last_activity crossing a tv_nsec borrow (now.tv_nsec <
     last_activity.tv_nsec but now.tv_sec == last_activity.tv_sec) must also
     resolve to a genuinely negative elapsed, not a spurious wrap from the
     nanosecond subtraction alone. */
  struct timespec now_zero_nsec = {.tv_sec = 2000, .tv_nsec = 0};
  struct timespec last_activity_same_sec_later_nsec = {.tv_sec = 2000,
                                                       .tv_nsec = 500000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now_zero_nsec, last_activity_same_sec_later_nsec, 300));

  /* Ordinary, non-racing cases must still behave exactly as documented. */
  struct timespec last_activity_past_by_500ms = {.tv_sec = 999,
                                                 .tv_nsec = 500000000L};
  /* elapsed == 500ms: >= a 300ms timeout is a real timeout. */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 300));
  /* elapsed == 500ms: < a 1000ms timeout is not yet timed out. */
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 1000));

  /* The exact boundary: elapsed_ms == idle_ms satisfies >=, so it has
     timed out. */
  struct timespec last_activity_past_by_exactly_idle_ms = {
      .tv_sec = 999, .tv_nsec = 700000000L}; /* exactly 300ms before now */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_exactly_idle_ms, 300));

  /* One millisecond short of the boundary is not yet timed out. */
  struct timespec last_activity_past_by_299ms = {.tv_sec = 999,
                                                 .tv_nsec = 701000000L};
  REQUIRE_FALSE(_chttpsvr_conn_idle_timed_out_for_tests(
      now, last_activity_past_by_299ms, 300));

  /* Identical now/last_activity (elapsed == 0) with idle_ms == 0 (the
     caller-side guarantee is that idle_ms is always nonzero in practice,
     since _idle_sweep_fn skips a server entirely when it is 0, but the
     function itself must still behave sanely rather than relying on that):
     0 >= 0 is a real, if degenerate, timeout. */
  REQUIRE_TRUE(_chttpsvr_conn_idle_timed_out_for_tests(now, now, 0));
}

TEST(chttpserver, idle_timeout_us_overflow_saturates_not_wrapped) {
  /* chttpsvr_config_t.idle_timeout_us and read_timeout_us are both
     uint64_t microsecond counts. The internal field that chttpsvr_start()
     feeds them into is an `unsigned` count of milliseconds. A value of
     UINT_MAX milliseconds or more must saturate to UINT_MAX - 1. It must
     not wrap silently. A multiple of 2^32 milliseconds wraps to exactly 0.
     That value is the "the idle timeout is off" sentinel of this field.
     Such a wrap silently turns off the idle-timeout sweep that the caller
     configured, instead of applying the very long timeout that the caller
     asked for. This test checks the result through the white-box accessor.
     It does not wait out a real idle timeout, because the whole point here
     is that the configured value is enormous. The arithmetic is the same
     on ILP32 and LP64, so the test runs on every platform. */
  extern unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 24;
  /* exactly 2^32 milliseconds; wraps to 0 uncorrected */
  cfg.idle_timeout_us = UINT64_C(4294967296000);
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv), UINT_MAX - 1u);
  chttpsvr_destroy(srv);

  /* The same saturation, through the read_timeout_us fallback path, with
     the largest value. This code leaves idle_timeout_us at 0. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg2 = CHTTPSVR_CONFIG_DEFAULT;
  cfg2.host = "127.0.0.1";
  cfg2.port = TEST_PORT + 25;
  cfg2.idle_timeout_us = 0;
  cfg2.read_timeout_us = UINT64_MAX;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg2), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv2), UINT_MAX - 1u);
  chttpsvr_destroy(srv2);

  /* A value below one millisecond rounds up to one, and never to the 0
     that turns the sweep off. */
  chttpsvr srv4 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv4 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg4 = CHTTPSVR_CONFIG_DEFAULT;
  cfg4.host = "127.0.0.1";
  cfg4.port = TEST_PORT + 25;
  cfg4.idle_timeout_us = 1;
  REQUIRE_EQ((int)chttpsvr_start(srv4, &cfg4), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv4), 1u);
  chttpsvr_destroy(srv4);

  /* An ordinary, small value must still pass through untouched, on every
   * platform regardless of the above. */
  chttpsvr srv3 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv3 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg3 = CHTTPSVR_CONFIG_DEFAULT;
  cfg3.host = "127.0.0.1";
  cfg3.port = TEST_PORT + 26;
  cfg3.idle_timeout_us = 5000000;
  REQUIRE_EQ((int)chttpsvr_start(srv3, &cfg3), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv3), (unsigned)5000);
  chttpsvr_destroy(srv3);
}

/* Starts a server on port with cfg and reads back every duration that the
 * start resolved, in milliseconds. */
static bool _resolved_ms(chttpsvr_config_t cfg, int port, unsigned out[9]) {
  extern bool _chttpsvr_resolved_ms_for_tests(chttpsvr h, unsigned out[9]);
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
  if (srv == CHTTPSVR_INVALID) return false;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  bool ok = chttpsvr_start(srv, &cfg) == ccol_success &&
            _chttpsvr_resolved_ms_for_tests(srv, out);
  chttpsvr_destroy(srv);
  return ok;
}

/* Every duration of chttpsvr_config_t is microseconds, and the server keeps
 * milliseconds. The defaults convert exactly, a 0 that means "the default"
 * selects it, a 0 in response_write_timeout_us takes stream_read_timeout_us,
 * a value below a millisecond rounds up and never reaches 0,
 * CHTTPSVR_NO_DEADLINE stays the internal "no deadline", and the largest
 * finite value saturates one below it. The expected values are literals. */
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
  /* SO_KEEPALIVE is set on an accepted connection's own fd, which a client
     has no portable way to observe from the outside (getsockopt only ever
     reports the calling process's own socket state); this is therefore a
     black-box smoke test that the setsockopt(2) call itself neither fails
     nor disturbs the normal request/response path, matching this file's
     own established pattern for config knobs whose effect is otherwise
     unobservable from a client (for example, max_header_bytes_within_limit_
     succeeds above, for the byte cap itself). */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* Without SO_REUSEPORT, a second bind to the same host:port fails with
     EADDRINUSE (chttpsvr_start returns ccol_unexpected_failure); this is a
     real, externally observable effect of the option, unlike
     enable_keepalive/ipv6_only above. */
  chttpsvr srv1 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv1 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 15;
  cfg.enable_reuseport = true;
  REQUIRE_EQ((int)chttpsvr_start(srv1, &cfg), (int)ccol_success);

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv2, CHTTP_GET, "/reuseport-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  /* Confirm the second listener genuinely serves traffic (not merely that
     bind() itself succeeded): the kernel load-balances new connections
     across every SO_REUSEPORT listener on this port, so which of the two
     servers actually answers is not deterministic; only srv2 has the route
     registered, so a 404 (srv1 answered) is treated as inconclusive-but-
     acceptable rather than a hard failure, while any successful 200 proves
     the mechanism works end to end.
     Each attempt uses chttp_do() with an explicit "Connection: close"
     header, NOT chttp_get(): chttp_get() goes through the shared default
     client's keep-alive idle pool, keyed by "scheme://host:port"; since
     srv1 and srv2 are both bound to the exact same 127.0.0.1:port pair,
     the pool cannot distinguish them, so once one connection is
     established every later chttp_get() call to this URL silently
     reuses it instead of asking the kernel to select a listener again.
     Logging every attempt's status code shows this directly: with
     chttp_get() the outcomes are not independent trials at all, but a short
     run of one repeated result followed by an unbroken streak of the other
     for the rest of the loop (the kept-alive connection), while forcing
     Connection: close on every attempt produces genuinely interleaved
     200/404 results matching a fair coin. Retried 64 times purely as
     insurance against ordinary coin-flip variance; with each attempt a
     real, independent trial, this is already comfortably below a
     1-in-10^18 chance of a false failure. */
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/reuseport-hello",
           TEST_PORT + 15);
  bool got_200 = false;
  for (int i = 0; i < 64 && !got_200; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
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
  /* Best-effort, matching this codebase's own established IPv6 convention
     elsewhere (not every sandbox/CI environment has an IPv6 stack): skip
     rather than hard-fail if binding "::1" itself doesn't work at all,
     since that's an environment limitation unrelated to ipv6_only. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Bounds the read(2) call below in case a regression makes the server
     never respond and never close; mirrors _raw_request's/
     _read_one_http_response's own identical SO_RCVTIMEO guard elsewhere in
     this file. */
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
  /* The sibling test above only ever exercises genuine IPv6 traffic, which
     would pass identically whether or not IPV6_V6ONLY was actually applied
     at all; a regression that silently dropped the setsockopt() call (or
     applied it to the wrong socket, or at the wrong point relative to
     bind()) would still pass it. ipv6_only's whole documented purpose
     (chttpserver.h) is that a dual-stack-capable listener bound to the
     IPv6 wildcard must NOT also silently accept IPv4 traffic once the flag
     is set; this test targets that guarantee directly. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/v6only-blocks-v4", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "::"; /* the IPv6 wildcard, dual-stack-capable when ipv6_only
                       is left false; what actually makes this a
                       meaningful test of the flag, unlike "::1" (IPv6
                       loopback), which never accepts IPv4 traffic in the
                       first place regardless of ipv6_only. */
  cfg.port = TEST_PORT + 77;
  cfg.ipv6_only = true;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    fprintf(stderr,
            "[SKIP] ipv6_only_blocks_ipv4_mapped_connections: no IPv6 "
            "wildcard bind available in this environment\n");
    return;
  }

  /* Control: a genuine IPv6 connection must still work, confirming this
     listener is otherwise healthy and the port really is bound (mirrors
     the sibling test's own request). */
  int fd6 _ccol_destructor(_close_scoped_fd) = socket(AF_INET6, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd6 >= 0);
  struct sockaddr_in6 sa6;
  memset(&sa6, 0, sizeof(sa6));
  sa6.sin6_family = AF_INET6;
  sa6.sin6_port = htons((uint16_t)(TEST_PORT + 77));
  REQUIRE_EQ(inet_pton(AF_INET6, "::1", &sa6.sin6_addr), 1);
  REQUIRE_EQ(connect(fd6, (struct sockaddr *)&sa6, sizeof(sa6)), 0);
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

  /* The real assertion: a PLAIN IPv4 connection to the identical port must
     be refused outright, not silently accepted by the same listener. With
     IPV6_V6ONLY genuinely applied, there is no IPv4-reachable socket bound
     to this port at all, so connect() itself must fail immediately with
     ECONNREFUSED, never merely time out waiting for a response that a
     regression's own accepted-but-never-routed connection would produce. */
  int fd4 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd4 >= 0);
  struct sockaddr_in sa4;
  memset(&sa4, 0, sizeof(sa4));
  sa4.sin_family = AF_INET;
  sa4.sin_port = htons((uint16_t)(TEST_PORT + 77));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa4.sin_addr), 1);
  int rc = connect(fd4, (struct sockaddr *)&sa4, sizeof(sa4));
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
     (Content-Length included) a GET would, but must never actually send the
     message body. _send_response must not write conn->resp.body to the wire
     unconditionally, regardless of conn->method; doing so streams a body
     back for a HEAD request that reached a handler which writes one. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req = "HEAD /head-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Read only up through the header/body separator. */
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

  /* _hello_handler writes a non-empty "Hello, world!" body; Content-Length
     must still report its real length even though it is never sent. */
  char *cl = strstr(buf, "content-length:");
  REQUIRE_TRUE(cl != NULL);
  unsigned long declared_len = strtoul(cl + 15, NULL, 10);
  REQUIRE_EQ(declared_len, (unsigned long)strlen("Hello, world!"));

  /* Nothing beyond the header terminator may have already been read: a
     buggy server writes the header block and the body back to back (often
     within the same or the very next TCP segment), so simply waiting for
     the next read() to time out is not enough; the body bytes could
     already be sitting in buf, past hdr_end, from the very read() call(s)
     that found the header terminator itself. */
  REQUIRE_EQ(total, (size_t)(hdr_end - buf) + 4);

  /* No further body bytes must ever follow either: a short poll() must see
     nothing more arrive. */
  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 200);
  REQUIRE_EQ(pr, 0);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
}

TEST(chttpserver, head_request_on_rejected_route_still_has_no_body) {
  /* _conn_reject_and_close builds resp with a bare memset to zero. It
     never writes any body content onto it, for ANY reject_status.
     resp.body_len is therefore always 0 here, whatever the request method
     is. This call site passes conn->method == CHTTP_HEAD as the
     suppress_body argument of _send_response, and not a hardcoded false.
     That argument can never change what reaches the wire today. The early
     return of _send_response that no_body gates only skips a loop that
     writes the body, and that loop was already going to run zero times.
     To pass the real method through is still correct. It is defensive
     consistency with every other _send_response call site in this file,
     for the case where a reject response grows a body later, such as a
     message with the detail of an error. But this test cannot drive that
     specific wiring, and it cannot verify it. An assertion around "the
     bytes on the wire differ between suppress_body=true and false" would
     be vacuous here, because they provably do not differ either way. This
     test verifies the real behaviour that you can observe instead. A HEAD
     request to a rejected route gets back a well-formed 404 with truly no
     body. That response carries an explicit "content-length:0" header. 404
     is neither 1xx nor 204, and a rejection that the server writes itself
     reports the length of its own body, which is always 0 here, for HEAD as
     for GET. It also carries no body byte
     of any kind after the CRLF that ends the header block. It is not
     merely "some text that holds 404". */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* Allocator that fails malloc/calloc/realloc exactly when the requested size
   equals g_fail_alloc_size (0 = never fail), and otherwise behaves like a
   plain pass-through. Lets a test target one specific allocation (here,
   _conn_start_diverted's carry-over copy of pipelined leftover body bytes)
   without disturbing every other allocation the server makes while serving
   the same request. */
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
     read() (the leftover/carry-over bytes past the header block),
     _conn_start_diverted must not set conn->_carry_over_len to the leftover
     length if the matching _ccol_mem_alloc for conn->_carry_over failed.
     The worker thread would
     then call chttp1_stream_prepare with a NULL pointer and a nonzero
     length, which unconditionally memcpy()s from that NULL pointer,
     crashing. This test drives that exact allocation to fail with a custom
     allocator and asserts
     the connection is instead rejected gracefully (500) with the server
     (and the rest of this test process) still alive and functional
     afterward. */
  size_t body_len = 6151; /* distinctive; unlikely to collide with any other
                           * allocation size this request triggers */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

  /* Headers and the whole body in one buffer/one write() call, so the
     reactor's single read() sees the body bytes as "leftover" past the
     header block in the very same call that triggers the diversion path
     under test; only now does g_fail_alloc_size get armed, so server
     startup/route registration/connect above are unaffected by it. */
  char *wire = (char *)malloc((size_t)hn + body_len);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, head, (size_t)hn);
  memcpy(wire + hn, body, body_len);
  g_fail_alloc_size = body_len;
  ssize_t written = write(fd, wire, (size_t)hn + body_len);
  /* Left armed on success: the fault is injected server-side, asynchronously,
     once the worker thread actually processes these bytes; not observable
     until the read() below returns the resulting response, which is where
     the real disarm already lives. Only disarmed here on the (rare) write()
     failure path, where nothing further will ever trigger that allocation,
     so leaving it armed would otherwise affect every later test in this
     same process once the REQUIRE_EQ below returns early. */
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

  /* The server (and this process) must still be fully usable afterward: a
     fresh, ordinary request on a brand-new connection/port must succeed,
     proving the earlier allocation failure was contained to that one
     request rather than corrupting shared state. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-carry-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 21;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  sa.sin_port = htons((uint16_t)(TEST_PORT + 21));
  REQUIRE_EQ(connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* A genuine _on_body allocation failure while growing a streaming route's
   own body-accumulation buffer must NOT be reported to the handler as
   ccol_http_transfer_aborted ("connection closed or
   malformed framing") through chttpsvr_req_stream_error(). That is
   indistinguishable from an actual dropped connection or malformed chunk
   framing even though nothing is wrong with the peer or its framing at all,
   misclassifying a server-side OOM as a client-caused transfer error. This
   specific failure therefore has its own flag (conn->body_alloc_failed) and
   its own, distinct chttpsvr_req_stream_error()
   outcome (ccol_not_enough_memory), so a handler that reacts differently to
   "the peer misbehaved" versus "the server is out of memory" (for example,
   it logs or alerts differently, or it retries a downstream call only for
   the first one)
   can actually tell the two apart.

   Targets the very first body-buffer growth, which unconditionally attempts
   exactly 8192 bytes regardless of how much body data actually arrived
   (conn->body starts with cap == 0, and _on_body's own growth loop takes the
   "b->cap ? b->cap : 8192" branch on that first call): a tiny, distinctive
   body is enough to trigger it, no need to actually send anywhere near 8192
   bytes. */
TEST(chttpserver,
     streaming_body_buffer_alloc_failure_reports_not_enough_memory) {
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "hi";
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /body-buf-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n",
                    strlen(body));
  REQUIRE_GT(hn, 0);

  /* Armed only now, after chttpsvr_start()/route registration/connect are
     already done, so none of those allocations are affected. 8192 matches
     _on_body's own first-growth target exactly (see this test's own comment
     above); left armed on a write() failure's own disarm path exactly like
     this file's other g_fail_alloc_size tests. */
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

/* _on_header: an allocation failure while copying a header's name/value
   (before routing has even run) must not return 1 (aborting the parse with
   CHTTP1_USER) without also setting
   conn->req_rejected/reject_status. Doing so drops _conn_feed_bytes into
   its "else" branch, silently closing the connection with zero response
   bytes, unlike the identical OOM failure mode _on_headers_complete
   handles gracefully (a 500 through reject_pool) a few callbacks
   later. This test drives that exact allocation to fail with the same
   fail-at-size custom allocator the carry-over OOM test above uses, and
   asserts a
   graceful 500 (not a bare closed connection) is delivered instead. */
TEST(
    chttpserver,
    on_header_alloc_failure_rejects_gracefully_instead_of_dropping_connection) {
  size_t value_strlen = 6201; /* distinctive; value_strlen+1 targets the
                               * header value allocation specifically */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  /* Left armed on success; see carry_over_alloc_failure_rejects_gracefully_
     instead_of_crashing's own identical write() guard for the full
     reasoning. Only disarmed here on the (rare) write() failure path. */
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

  /* The server (and this process) must still be fully usable afterward: an
     ordinary request on a fresh connection must succeed, proving the
     earlier allocation failure was contained to that one request rather
     than corrupting shared state. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  REQUIRE_EQ(connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* The identical contract as the test just above, this time in
   _on_request_line: an allocation failure while copying the raw (still
   percent-encoded) request path (which happens before any route has been
   matched at all) must not abort the parse with CHTTP1_USER without also
   setting conn->req_rejected/reject_status, which silently drops the
   connection instead of sending a graceful 500. */
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
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  /* Left armed on success; see carry_over_alloc_failure_rejects_gracefully_
     instead_of_crashing's own identical write() guard for the full
     reasoning. Only disarmed here on the (rare) write() failure path. */
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

  /* Still usable afterward, exactly like the sibling _on_header test above. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  REQUIRE_EQ(connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* chttpsvr_req_query_oom() is documented (chttpserver.h) as latching true
   once chttpsvr_req_query()'s own result-array allocation has failed under
   memory pressure, and staying true "for the lifetime of the request
   regardless of later query calls". This is the only test that
   triggers a real OOM in that path and checks the flag (the other coverage
   of it exercises just the trivial NULL-req guard), so without it a
   regression that broke the latch (never setting it, or resetting it on a
   later successful call) passes the whole suite undetected. */
static _Atomic bool g_query_oom_flag_right_after_failure = false;
static _Atomic bool g_query_oom_flag_after_later_success = false;
static _Atomic size_t g_query_oom_failed_call_count = (size_t)-1;
static _Atomic size_t g_query_oom_later_call_count = (size_t)-1;
static _Atomic bool g_query_oom_failed_call_result_is_null = false;

#define QUERY_OOM_REPEATED_KEY_COUNT 91 /* distinctive result-array size */

static void _query_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  /* Warm up: force _ensure_qparams to fully parse the query string BEFORE
     the allocator is ever armed below, isolating the fault injection to
     ONLY chttpsvr_req_query's own _qresult realloc, not any of
     _ensure_qparams's own internal parsing allocations (which could
     otherwise coincidentally need the identical byte count). */
  size_t warmup_n = 0;
  chttpsvr_req_query(req, "does-not-exist", &warmup_n);

  /* +1 for the NULL-terminator slot chttpsvr_req_query always reserves. */
  g_fail_alloc_size = (QUERY_OOM_REPEATED_KEY_COUNT + 1) * sizeof(char *);
  size_t n = (size_t)-1;
  const char **vals = chttpsvr_req_query(req, "a", &n);
  g_fail_alloc_size = 0; /* disarm immediately after the one targeted call */

  atomic_store(&g_query_oom_failed_call_result_is_null, vals == NULL);
  atomic_store(&g_query_oom_failed_call_count, n);
  atomic_store(&g_query_oom_flag_right_after_failure,
               chttpsvr_req_query_oom(req));

  /* A second, ordinary (non-failing) query call afterward, for a different
     key: the flag must stay latched true regardless, per its own
     documented "for the lifetime of the request" contract. */
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
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/query-oom",
                                               _query_oom_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 76;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Build "?a=1&a=2&...&a=91&b=only" so key "a" has exactly
     QUERY_OOM_REPEATED_KEY_COUNT occurrences (forcing the targeted realloc
     size above) and key "b" has exactly one, for the post-failure call. */
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
  /* The real assertion: still latched true after a LATER, genuinely
     successful query call for an unrelated key. */
  REQUIRE_EQ(g_query_oom_later_call_count, (size_t)1);
  REQUIRE_TRUE(g_query_oom_flag_after_later_success);

  chttpsvr_destroy(srv);
}

/* chttpsvr_req_query_one's own documented ccol_not_enough_memory return
   (chttpserver.h) is covered only here: the OOM coverage above exercises
   chttpsvr_req_query's/chttpsvr_req_query_oom's own flag-based contract,
   never chttpsvr_req_query_one's direct return-value contract, which is a
   structurally different code path (its own
   `if (!qp) return ccol_not_enough_memory; if (req->_qparams_parse_oom)
   return ccol_not_enough_memory;` checks, both ahead of any "was the key
   even found" logic) that a regression collapsing those two checks into
   the ordinary ccol_key_not_found path would not be caught by anything
   else in this suite. */
#define QUERY_ONE_OOM_KEY_LEN                    \
  700 /* forces _parse_qparams's own per-pair    \
         key_decoded allocation onto the heap    \
         with a distinctive, unlikely-to-collide \
         byte count */

static _Atomic int g_query_one_oom_rv = -999;

static void _query_one_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  /* chttpsvr_req_query_one is the very first query accessor called on this
     request, so _ensure_qparams's lazy first-call parse of the whole query
     string runs synchronously inside this armed window; nothing else on
     this thread has a reason to need the identical, distinctive byte
     count, isolating the fault to _parse_qparams's own key_decoded
     allocation for this request's single, deliberately long query key. */
  g_fail_alloc_size = QUERY_ONE_OOM_KEY_LEN + 1;
  /* Poisoned with a non-NULL sentinel first, not left at NULL: a bare NULL
     starting value cannot distinguish "the OOM path actually resets
     *val_out" from "it simply never touched an already-NULL local", so a
     regression that never touches it would read identically (see
     chttpsvr_req_query_one's own header doc comment: every failure return,
     OOM included, must leave *val_out reset to NULL). */
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
      ccol_create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

/* Unlike the carry-over copy above (which goes through this module's own
   custom-allocator convention and is therefore rejected gracefully by
   _conn_start_diverted before ever
   diverting), _task_worker's OWN chttp1_stream_prepare()/_tls() call (a
   second, separate copy of the same carry-over bytes, made on the worker
   thread) goes through plain malloc() and has no custom-allocator hook to
   fail it from a test; _chttpsvr_force_stream_prepare_fail_for_tests()
   deterministically exercises that same failure path instead. Gating every
   response path in _task_worker on `prepared` would close the connection
   with zero response bytes for a buffered route (the intended 500 set but
   never sent), and for a streaming route would run the handler against a
   NULL req->stream, with chttpsvr_req_stream_error() falsely reporting
   ccol_success instead of the real failure. This drives that exact scenario
   against a streaming route
   and asserts the connection instead receives a graceful 500 with the
   handler never invoked at all (no x-stream-err header, which only the
   handler itself ever sets). */
TEST(chttpserver, stream_prepare_failure_in_worker_sends_500_not_bare_close) {
  extern void _chttpsvr_force_stream_prepare_fail_for_tests(bool force);

  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, &err);
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
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  /* Headers and body in one write() call, so the reactor's single read()
     sees the body bytes as carry-over past the header block, exactly like
     the allocation-failure test above; only now is the worker-thread-side
     prepare forced to fail, so server startup/route registration/connect
     above are unaffected by it. */
  const char *body = "hello";
  char wire[256];
  int wn = snprintf(wire, sizeof(wire),
                    "POST /stream-prepare-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n%s",
                    strlen(body), body);
  REQUIRE_GT(wn, 0);

  _chttpsvr_force_stream_prepare_fail_for_tests(true);
  ssize_t written = write(fd, wire, (size_t)wn);
  /* Left armed on success: the fault is injected server-side, asynchronously,
     once the worker thread actually processes these bytes; not observable
     until the read() below returns the resulting response, which is where
     the real disarm already lives. Only disarmed here on the (rare) write()
     failure path, where nothing further will ever trigger that path, so
     leaving it armed would otherwise affect every later test in this same
     process once the REQUIRE_EQ below returns early. */
  if (written != (ssize_t)wn)
    _chttpsvr_force_stream_prepare_fail_for_tests(false);
  REQUIRE_EQ(written, (ssize_t)wn);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  _chttpsvr_force_stream_prepare_fail_for_tests(false); /* disarm first */
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);
  /* The handler was never invoked: it is the only thing that would have set
     this header. */
  REQUIRE_TRUE(strstr(buf, "x-stream-err") == NULL);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  /* The server (and this process) must still be fully usable afterward. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-stream-prepare-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 23;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd2 >= 0);
  sa.sin_port = htons((uint16_t)(TEST_PORT + 23));
  REQUIRE_EQ(connect(fd2, (struct sockaddr *)&sa, sizeof(sa)), 0);
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
/*         1xx/204/304 BODY-SUPPRESSION TESTS (RFC 9110 SS6.4.1/15.2.1/15.4.5)
 */
/* ========================================================================== */

TEST(chttpserver, response_204_never_sends_body_or_content_length) {
  /* _send_response's body suppression must cover a 204 as well as HEAD: a
     204 can never carry a body either, regardless of method (RFC 9110
     SS15.2.1). Suppressing only for suppress_body (HEAD) streams
     /status-with-body's own 36-byte body back verbatim on a plain GET
     whose handler happens to set 204 after writing it; any client that
     correctly treats 204 as bodyless (including this library's own
     chttpclient parser; see chttp1_parser.c's own CHTTP1_ST_HEADERS
     handling) would then misparse that leaked body as the start of the
     next pipelined response on a keep-alive connection.
     A 204 additionally MUST NOT carry a Content-Length at all (RFC 9110
     SS6.4.1), unlike HEAD/304 where reporting one is expected/permitted. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 204\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 204);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

/* Counts the occurrences of needle in the header block of the response in
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
  /* Same body-suppression contract as response_204_never_sends_body_or_
     content_length, but for 304 (RFC 9110 SS15.4.5). A 304 describes the
     stored representation, not this message, and a Content-Length there
     must equal the length of that representation (RFC 9110 SS8.6). The
     server does not know that length, so it writes none: a
     "content-length:0" would tell a cache that the representation is
     empty. This test is non-vacuous: a server that computes the field from
     the body that the handler wrote sends "content-length:35" here. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 304\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 304);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, response_304_sends_the_content_length_that_the_handler_set) {
  /* A handler that knows the length of the representation sets it, and the
     304 carries exactly that value, once, and still no body. Non-vacuous:
     a server that filters every Content-Length of a handler sends either
     no field or the length of the body that the handler wrote. */
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
  /* Where a body goes out, its length is the framing, whatever the handler
     set: the 200 carries the 35 bytes that the handler wrote and a
     content-length of 35. */
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
  /* A HEAD route that writes no body answers for a representation that is
     not in the message. Without a length from the handler the response
     carries no Content-Length at all, as Go's net/http server does, and not
     a "content-length:0" that describes an empty representation.
     Non-vacuous: a server that always computes the field from the body
     that the handler wrote sends "content-length:0" here. */
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/head-explicit", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-explicit-head:1") != NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, explicit_head_route_sends_the_content_length_of_the_handler) {
  /* The same HEAD route with a length from the handler carries that length,
     once, and no body. Non-vacuous: a server that filters every
     Content-Length of a handler sends "content-length:0" here. */
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
  /* The client of this library sees each Set-Cookie field that
     chttpsvr_resp_add_header added, in the order of the calls, and one
     x-single field after chttpsvr_resp_set_header collapsed the three that
     the handler had. Non-vacuous: an add_header that replaces leaves one
     Set-Cookie, and a set_header that replaces only the first match leaves
     x-single with three fields. */
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
  /* Both header calls accept one or more ASCII digits whose number fits in
     a signed 64-bit integer, leading zeros included, and refuse every other
     value with ccol_invalid_args. Non-vacuous: without the check both calls
     accept every value in the list. */
  chttpcli_response *resp = _get("/content-length-validation");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "yyyyyyyynnnnnnnnnnnnnnnnyy");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, resp_add_header_keeps_every_field_in_order) {
  /* chttpsvr_resp_add_header puts one more field on the wire for each call,
     in the order of the calls, so a response can carry several Set-Cookie
     lines. chttpsvr_resp_set_header replaces every field of its name with
     one, at the place of the first. Connection, Content-Length and Date stay
     single whichever call sets them. Non-vacuous: without add_header this
     does not link; with an add_header that replaces, only one Set-Cookie
     arrives; with a set_header that replaces only the first match, three
     x-single lines arrive. */
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

TEST(chttpserver, response_1xx_never_sends_body_or_content_length) {
  /* Same reasoning as the 204 case, for the informational (1xx) class (RFC
     9110 SS15.2.1/SS6.4.1): a handler is not realistically expected to set
     one of these as a FINAL status in ordinary use (the interim 100
     Continue response this server itself may send is handled entirely
     separately in _task_worker, never through _send_response at all), but
     _send_response's own suppression logic is keyed purely on the numeric
     status range, with no special-casing of "how did we get here"; this
     locks that in rather than leaving 1xx as an untested corner of the
     same rule. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 199\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 199);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, large_response_headers_still_delivered_in_full) {
  /* _send_response must not assemble the response header block into a fixed
     4096-byte stack buffer with no fallback. A response whose headers alone
     cross that size then silently loses the ENTIRE
     response: not a graceful 500, not a truncated write, just a bare
     connection close with zero bytes ever written and nothing logged.
     /large-response-headers sets 20 headers of ~300 bytes each (~6.2 KiB of
     header block, well past a 4096-byte cap); every one of them, the 200
     status, and the body must all still reach the client intact. */
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
  /* _send_response must not emit a handler-supplied "Connection" response
     header verbatim, independent of the keep_alive value _task_worker
     actually uses afterward to decide whether the
     connection stays open. A handler here explicitly sets "Connection:
     close" while nothing about this ordinary HTTP/1.1 request gives the
     server any real reason to close (fully parsed, no body_too_large, no
     abort); the real decision is therefore keep-alive, and the wire must say
     so (not whatever the handler happened to set) with the connection
     genuinely still usable for a second request right after. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* The connection must genuinely still be alive: a second request on the
     same socket must succeed, not hang or reset. */
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
  /* The other direction of the same contract: a handler explicitly sets
     "Connection: keep-alive" on an HTTP/1.0 request that carries no
     "Connection: keep-alive" token of its own, so chttp1_should_keep_alive()
     correctly reports this connection is not eligible for reuse regardless
     of what the handler wants. The wire must say "close" (matching what the
     server actually does), never the handler's own overridden value. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  /* The read loop above only stops on EOF (r <= 0), which for a still-open
     keep-alive connection would instead have blocked forever; reaching here
     at all already proves the server closed its end. */
  close(fd);
  fd = -1;
}

/* ========================================================================== */
/*         CHTTPSVR HANDLE LIFECYCLE (GENERATION-TAGGED SLOT TABLE)          */
/* ========================================================================== */

/*
 * chttpsvr is a value handle with a generation tag. It holds a slot index
 * and a generation. The library resolves it through a slot table that it
 * owns, before anything touches the struct chttpserver* below it. See the
 * "CHTTPSVR HANDLE SLOT TABLE" section of src/chttpserver.c. chttpclient.c
 * already uses the exact same mechanism for its own chttpcli handle, for
 * the same reason. Without it, __chttpsvr_destroy has no protection at all
 * against a second run on the same handle. That second run can come one
 * after the other, or at the same time, and either one is a real double
 * free. This section tests that design directly.
 */
extern struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h);
extern size_t _chttpsvr_slot_table_capacity_for_tests(void);

static void _noop_middleware_for_lifecycle_tests(chttpsvr_req *req,
                                                 chttpsvr_resp *resp, void *ctx,
                                                 chttpsvr_next_fn next) {
  (void)ctx;
  if (next) next(req, resp);
}

/* Take a destroy that finished in full. A second destroy call after it, on
 * a separate copy of the same original handle value, must be a fatal
 * error. This test runs in a forked child, because ccol_fatal_err aborts
 * the whole process. tests/clogger/tests.c sets the same precedent for a
 * fork test in this codebase. Each server in this section takes a NULL
 * logger, and not g_test_logger. ccol_create_chttpsvr then uses its own
 * internal logger, which writes only FATAL messages to stderr. These tests
 * therefore depend on none of the shared test server state and logger state
 * for the whole process that _setup and _teardown manage. */
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
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    chttpsvr stale = srv;  /* an independently-held copy of the handle value,
         distinct from the local the macro below NULLs out */
    chttpsvr_destroy(srv); /* completes normally; the local `srv` is now
        CHTTPSVR_INVALID, but `stale` still holds the original value */
    __chttpsvr_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check the return value of waitpid before you trust status. An EINTR
   * failure, or any other failure, leaves status at its initial value of 0.
   * Without this check, the code silently reads that as "the child exited
   * normally with status 0". The truth is that waitpid never reported the
   * real outcome of this child. That produces a confusing WIFSIGNALED
   * failure instead of a clear one. Every later and more elaborate fork
   * test below makes the same explicit `!= pid` check. */
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

/* Take two threads that each call destroy on their own copy of the SAME
 * handle, which is still valid. They call it as close to the same moment as
 * the test can arrange. That must also be fatal. It is the double free on
 * the heap that the slot table with its generation tag exists to turn into
 * a failure that the library finds and reports loudly. */
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
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    concurrent_svr_destroy_arg_t a1 = {.h = srv};
    concurrent_svr_destroy_arg_t a2 = {.h = srv};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, concurrent_svr_destroy_thread, &a1);
    pthread_create(&t2, NULL, concurrent_svr_destroy_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit ccol_fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check the return value of waitpid before you trust status. An EINTR
   * failure, or any other failure, leaves status at its initial value of 0.
   * Without this check, the code silently reads that as "the child exited
   * normally with status 0". The truth is that waitpid never reported the
   * real outcome of this child. That produces a confusing WIFSIGNALED
   * failure instead of a clear one. Every later and more elaborate fork
   * test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* The library exposes this only under RUNNING_UNIT_TESTS; see its own doc
 * comment in chttpserver.c. This code declares it here, and not in a
 * header. That matches the convention of this test file for a white-box
 * hook. The forward declarations of _chttpsvr_resolve_for_tests elsewhere
 * in this file are one example. */
extern void _chttpsvr_mark_self_as_worker_for_tests(chttpsvr h);

/* A request handler, or a middleware, can destroy the very server whose
 * worker pool runs it. That must be a fatal error, exactly like a second
 * destroy on a stale handle. See the comment of chttpsvr_worker_key_bundle
 * in chttpserver.c for the real hazard that this guards against. Without
 * that guard, this call hangs for about a minute, as it waits for its own
 * in-flight request to finish. It then aborts from deep inside the
 * unrelated self-destroy guard of cthreadpool.c. If it races a concurrent
 * chttpsvr_engine_stop() for the same server, it deadlocks permanently
 * instead.
 *
 * This test drives the case directly, with
 * _chttpsvr_mark_self_as_worker_for_tests. It does not use a real request
 * from end to end that a real worker thread dispatches. The test needs
 * fork() either way, because ccol_fatal_err() aborts the whole process. A
 * real chttpsvr_start() on top of that fork would need the shared reactor
 * of the forked child to service a new listener registration. fork(2)
 * duplicates no thread other than the one that calls it. The shared reactor
 * of this suite already runs real OS threads by this point, because
 * hundreds of earlier tests started servers. A child that this suite forks
 * therefore inherits a hollow reactor handle that nothing ever services. A
 * chttpsvr_start() call after the fork hangs for ever there instead of an
 * abort, with every thread idle and the listener never dispatched once. The
 * white-box hook avoids that interaction between fork and the reactor
 * completely. It still drives the exact same comparison that
 * __chttpsvr_destroy itself makes. */
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
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    __chttpsvr_destroy(srv); /* the actual misuse under test */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check the return value of waitpid before you trust status. An EINTR
   * failure, or any other failure, leaves status at its initial value of 0.
   * Without this check, the code silently reads that as "the child exited
   * normally with status 0". The truth is that waitpid never reported the
   * real outcome of this child. That produces a confusing WIFSIGNALED
   * failure instead of a clear one. Every later and more elaborate fork
   * test below makes the same explicit `!= pid` check. */
  REQUIRE_EQ(waitpid(pid, &status, 0), pid);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A request handler, or a middleware, can block in chttpsvr_engine_wait().
 * That must be a fatal error too. The underlying reason is the same as for
 * a handler that destroys its own server from inside itself; see the
 * previous test. The ctpool_shutdown_drain of
 * _engine_force_stop_quiesce_all has no timeout. It can never finish the
 * drain of the pool of this exact worker, because this call stack is what
 * would finally let the task return. The shared engine could therefore
 * never finish its teardown, and this call could never wake. That is a
 * permanent deadlock across the whole engine, and not merely one server
 * that is stuck. A caller reaches it through the exact pattern that
 * chttpserver.h itself documents for an endpoint that handles
 * administration or shutdown. That pattern calls chttpsvr_engine_stop(),
 * and then chttpsvr_engine_wait() to block the response until the drain
 * finishes. The self-call guard of chttpsvr_destroy() covers one specific
 * server. chttpsvr_engine_wait() is different, because it covers the whole
 * engine. It is enough to mark the thread that calls as a worker of ANY
 * server, even one that nothing ever started. This test therefore needs no
 * real dispatch and no reactor that runs. */
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
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    chttpsvr_engine_wait(); /* the actual misuse under test */
    _exit(0); /* unreachable if ccol_fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  /* Check the return value of waitpid before you trust status. An EINTR
   * failure, or any other failure, leaves status at its initial value of 0.
   * Without this check, the code silently reads that as "the child exited
   * normally with status 0". The truth is that waitpid never reported the
   * real outcome of this child. That produces a confusing WIFSIGNALED
   * failure instead of a clear one. Every later and more elaborate fork
   * test below makes the same explicit `!= pid` check. */
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
  /* This call is safe on its own. chttpsvr_stop() only closes the listener.
   * It never touches worker_pool or reject_pool. It therefore never meets
   * the self-call hazard that the restart path of chttpsvr_start() below
   * meets. */
  chttpsvr_stop(g_self_restart_srv_for_lifecycle_test);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval_t rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  atomic_store(&g_self_restart_result_for_lifecycle_test, (int)rv);
  chttpsvr_resp_write_str(resp, "done");
}

/* A handler can stop the very server whose worker pool runs it, and then
 * restart that server at once. That meets the same self-call hazard that
 * chttpsvr_destroy() meets; see the comment of chttpsvr_worker_key_bundle
 * in chttpserver.c. But chttpsvr_start() has a real ccol_retval_t to report
 * through. It must therefore refuse gracefully, with ccol_not_permitted,
 * and it must not hang and must not abort. It must also leave the server in
 * a state that a later, legitimate restart from another thread can still
 * recover cleanly. */
/* g_self_restart_srv_for_lifecycle_test is a global for the whole process,
 * and not a local. The self-restarting handler above must reach it from any
 * worker thread. Unlike every other server handle in this file, it
 * therefore cannot carry a scope-exit _ccol_destructor as a safety net. The
 * cleanup attribute of GCC applies only to a local variable with automatic
 * storage. This code guards every REQUIRE_* below that can return early
 * after the server starts successfully. On a failure it destroys the
 * server, which releases the shared-engine reference of that server, before
 * the REQUIRE_* itself runs and returns. That matches the "capture, clean
 * up, then assert" idiom that this file uses for exactly this class of
 * leak. Without it, a REQUIRE_* failure here leaks a global server that
 * still holds an engine reference. The REQUIRE_EQ(..., ccol_not_permitted)
 * below is the critical one, because it is the real regression that this
 * test exists to catch. Such a leak then hangs the chttpsvr_engine_wait()
 * call in the _teardown() of this file at the exit of the process. A real
 * regression in the exact feature under test becomes a silent hang of the
 * whole binary, instead of a clean, reported failure. */
static void _destroy_self_restart_test_server_if_live(void) {
  if (g_self_restart_srv_for_lifecycle_test != CHTTPSVR_INVALID)
    chttpsvr_destroy(g_self_restart_srv_for_lifecycle_test);
}

TEST(chttpsvr_handle_lifecycle,
     restart_from_within_own_handler_returns_not_permitted) {
  atomic_store(&g_self_restart_result_for_lifecycle_test, -1);
  g_self_restart_srv_for_lifecycle_test =
      ccol_create_chttpsvr(CLOG_INVALID, NULL);
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
  int connect_rv = connect(fd, (struct sockaddr *)&sa, sizeof(sa));
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

  /* The in-flight request that triggered the doomed self-restart must still
   * complete normally: chttpsvr_stop() alone never affects an already-
   * accepted connection. */
  if (status != 200) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(status, 200);
  int self_restart_result =
      atomic_load(&g_self_restart_result_for_lifecycle_test);
  if (self_restart_result != (int)ccol_not_permitted)
    _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(self_restart_result, (int)ccol_not_permitted);

  /* The server itself must be left in a perfectly safe, recoverable state:
   * an ordinary, legitimate restart from THIS (non-worker) thread must now
   * succeed. */
  ccol_retval_t restart_rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  if (restart_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);

  /* This second request runs the very same self-restarting handler again.
   * That handler therefore tries once more to restart the server from
   * inside its own worker thread, and the library correctly refuses it
   * again. The request must still get a normal 200 response. That confirms
   * that the server keeps working correctly, however many times a caller
   * retries this doomed pattern. */
  int fd2 _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd2 < 0) _destroy_self_restart_test_server_if_live();
  REQUIRE_TRUE(fd2 >= 0);
  int connect_rv2 = connect(fd2, (struct sockaddr *)&sa, sizeof(sa));
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
  /* This code ignores the return value on purpose. A legitimate race with
   * a concurrent destroy can make this resolve fail with
   * ccol_invalid_args, instead of succeed. Both outcomes are correct. This
   * thread exists only to generate resolve, pin and unpin traffic at the
   * same time as the destroy thread below. chttpsvr_use stands for the
   * whole public API of this module apart from destroy. chttpsvr_start,
   * chttpsvr_stop, chttpsvr_register_handler,
   * chttpsvr_register_streaming_handler, chttpsvr_use and
   * chttpsvr_subrouter all resolve, pin for their own short synchronous
   * duration, and unpin before they return. To drive any one of them
   * against a concurrent destroy therefore covers the same race window as
   * the others. */
  chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
  return NULL;
}

/* This test races a fast entry point that does not block against a
 * concurrent destroy, and it repeats that under stress. That entry point is
 * chttpsvr_use. It resolves, pins, runs a short critical section, unpins
 * and returns, with no blocking I/O at all. A real regression here is a
 * resolve-then-use race, or a use-after-free in the unpin path itself. The
 * comment of _chttpcli_resolve_unpin in chttpclient.c gives a real example
 * of the second one. Such a regression is reachable only in a window a few
 * instructions wide. A single run with no stress does not reproduce it
 * reliably. Each iteration uses a fresh server. Every repetition therefore
 * gets its own independent race, and none of them reuses a handle that
 * something already destroyed. */
TEST(chttpsvr_handle_lifecycle, resolve_unpin_race_stress) {
  /* This test captures each intermediate outcome below into a local. It
   * does not assert on that outcome at once with a REQUIRE_*. It also
   * always either gives srv to destroy_tid, or destroys srv directly,
   * before any REQUIRE_* can return early. This test creates a fresh
   * chttpsvr handle on each of its 30 iterations, and each one holds a live
   * shared-engine reference. A REQUIRE_* that returns early with srv not
   * destroyed would therefore hang the _teardown() of this whole binary for
   * ever at the exit of the process. That _teardown() blocks in
   * chttpsvr_engine_wait() until something destroys every server. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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
      /* Join setter_tid either way. It already started, and it does not
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
  /* This code ignores the return value on purpose. A legitimate race with
   * a concurrent destroy of the server that owns the router can make this
   * resolve fail with ccol_invalid_args, instead of succeed. Both outcomes
   * are correct. chttpsvr_router_on, chttpsvr_router_on_stream and
   * chttpsvr_router_use must not dereference router->srv directly. They
   * must not change router->routes or router->mw_head directly either. They
   * must first resolve and pin against the handle of the server that owns
   * the router, as every other entry point in this API that changes state
   * does. Without that pin they race a concurrent chttpsvr_destroy(). That
   * destroy frees srv and every router below it, through _destroy_router,
   * once it sees pending_resolve_count == 0. They would add nothing to that
   * count. */
  chttpsvr_router_on(a->router, CHTTP_GET, "/race", _hello_handler, NULL);
  return NULL;
}

/* This is the sub-router version of resolve_unpin_race_stress above. It
 * races chttpsvr_router_on against a concurrent chttpsvr_destroy() of the
 * same server. chttpsvr_router_on stands for chttpsvr_router_on,
 * chttpsvr_router_on_stream and chttpsvr_router_use. All three resolve and
 * pin the server that owns the router in the same way. The real
 * verification here is `make memtest`, which runs valgrind, and a build
 * with -fsanitize=thread. Either one reports a real use-after-free when the
 * resolve and pin protection regresses. The job of this test is only to
 * manufacture the race window reliably. */
TEST(chttpsvr_handle_lifecycle, router_resolve_unpin_race_stress) {
  /* This test captures each intermediate outcome below into a local. It
   * does not assert on that outcome at once with a REQUIRE_*. It also
   * always either gives srv to destroy_tid, or destroys srv directly,
   * before any REQUIRE_* can return early. See the same comment in
   * resolve_unpin_race_stress just above for why a leaked chttpsvr handle
   * here would hang the teardown of this whole binary at the exit of the
   * process. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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

/* This whole group of fork-safety regression tests drives the
 * pthread_atfork() protection of chttpserver.c. The build compiles that
 * protection out completely when CCOL_FORK_SAFETY_REQUIRED is 0; see the
 * doc comment of that macro in common.h. Without the protection, the
 * premise of these tests does not hold. That premise is that the
 * protection prevents a hang. A fork while the feeder thread below is in
 * the middle of a critical section then becomes a real source of
 * flakiness, although the probability is low. It is no longer a meaningful
 * regression check. tests/cthreadpool and tests/cthreadcomm wrap their own
 * fork-safety groups in the same `#if CCOL_FORK_SAFETY_REQUIRED` exactly
 * this way. */
#if CCOL_FORK_SAFETY_REQUIRED
typedef struct {
  chttpsvr h;
  _Atomic int stop;
} fork_feeder_arg_t;

/* This thread drives chttpsvr_use against one long-lived server that it
 * shares with the fork trials below. chttpsvr_use resolves through
 * chttpsvr_slot_table.mutex, and then runs a critical section under a write
 * lock on routes_lock. The sched_yield() after every call is load-bearing,
 * and not a nicety. The ctp_fork_feeder_thread of tests/cthreadpool does
 * the same. A bare `while (!stop) chttpsvr_use(...);` loop reproduces the
 * exact same contention natively. But it iterates fast enough that valgrind
 * cannot keep up. memcheck gives threads no real parallelism across cores;
 * it schedules every thread through one single instrumented execution
 * engine. An unthrottled spin therefore turns into an astronomically larger
 * total count of instrumented instructions for the same wall-clock
 * duration of the test. Unthrottled, this test does not finish a single one
 * of its 60 trials in ten minutes under valgrind, although it passes
 * natively in well under a second. A yield after every call caps the call
 * rate of this thread to what the time-slice granularity of the scheduler
 * allows. The race stays reproducible, and the test does not pay that
 * multiplier. */
static void *fork_feeder_thread(void *arg) {
  fork_feeder_arg_t *a = (fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
    sched_yield();
  }
  return NULL;
}

/* This test covers the pthread_atfork() protection of chttpserver.c.
 * fork() duplicates only the thread that calls it. Some OTHER thread can
 * hold one of the locks of this module at the exact instant of the fork.
 * Those locks are chttpsvr_slot_table.mutex, srv_engine_bundler.mutex,
 * servers_bundler.mutex, chttpsvr_router_shell_registry.mutex, and the
 * mutex, idle_mutex, diverted_mutex and routes_lock of any live server.
 * Without the protection, the child inherits such a lock in the locked
 * state, permanently. No thread survives in the child that could ever
 * unlock it. Any later chttpsvr_* call in the child then hangs for ever.
 * The fork_does_not_inherit_a_locked_ctpool_mutex test of
 * tests/cthreadpool has the same shape. A feeder thread that calls
 * chttpsvr_use, throttled with a yield, runs against one shared, long-lived
 * server. That keeps chttpsvr_slot_table.mutex and the routes_lock of that
 * server under realistic contention that does not spin, while this test
 * forks again and again. A small, synchronous burst of real create and
 * destroy cycles, of a fixed size, runs right before each fork. The ctpool
 * sibling test uses the same shape for its own burst of submits before a
 * fork. That burst also drives chttpsvr_router_shell_registry.mutex, and
 * the mutex, idle_mutex, diverted_mutex and routes_lock of a fresh
 * instance. It needs no second background thread that spins continuously.
 * An alarm(1) bounds each child, so a real regression here fails this test
 * instead of a hang of the whole suite. Each child makes the exact call
 * that a real application would make right after it inherits a live server
 * handle across a fork, which is chttpsvr_use on the shared feeder server.
 * That call must return promptly either way. Its own resolve may
 * legitimately race the concurrent use of the identical handle in the
 * parent, but it must never simply hang.
 *
 * The burst before the fork below is synchronous and bounded. It is not a
 * second background thread that creates and destroys throwaway servers
 * continuously, with no throttle at all. The churn thread of the ctpool
 * sibling test has that shape, and it is confirmed cheap there. Churn with
 * no throttle here does not finish a single one of its 60 trials in ten
 * minutes under valgrind, although it passes natively in well under a
 * second. A standalone reproduction outside this suite measures that
 * directly, and nothing assumes it. That reproduction isolates each
 * background thread in turn. A chttpsvr_use feeder thread that a yield
 * throttles costs about 0.03s for each fork under valgrind. The churn
 * thread with no throttle does not finish even one fork within 60 seconds,
 * and it consumes CPU continuously. It is not blocked and not deadlocked;
 * it truly makes forward progress, only catastrophically slowly. The reason
 * is the cost of each iteration of ccol_create_chttpsvr_mp. That cost
 * covers a full root router, several mutexes and condition variables, and a
 * derived logger. That logger itself registers into, and unregisters from,
 * the separate global slot table of clogger. It is far heavier than the
 * minimal pool creation of ctpool. memcheck also gives threads no real
 * parallelism across cores at all; it time-slices every thread through one
 * single instrumented execution engine. A loop with no throttle over this
 * much heavier work therefore does not divide across cores the way that it
 * does natively. It only hands valgrind an astronomically larger total
 * count of instrumented instructions to simulate, for the same wall-clock
 * duration of the test. The cost of the burst scales with TRIALS instead.
 * It does not scale with how much work a background thread crams into a
 * window of wall-clock time with no bound. TRIALS is deliberately smaller
 * than the 60 of the ctpool sibling test. fork() itself already carries a
 * large, roughly fixed cost for each call under valgrind, whatever the
 * count of trials. This test also covers the hazard where routes_lock
 * tracks a TID; see the doc comment of _chttpsvr_atfork_release_impl. That
 * hazard is deterministic, and not probabilistic, so it needs no repetition
 * at all to catch. Only the other half of the coverage of this test
 * benefits from repeated trials, which is the case where a lock is truly
 * still held at the instant of the fork. */
TEST(chttpsvr_handle_lifecycle, fork_does_not_inherit_a_locked_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  /* This test captures each intermediate outcome below into a local. It
   * does not assert on that outcome at once with a REQUIRE_*. It also stops
   * feeder_tid, joins it, and destroys feeder_srv, unconditionally, before
   * any REQUIRE_* can return early. feeder_tid runs a bare
   * `while (!stop) ...` loop; see the comment of fork_feeder_thread. That
   * loop ends only after something sets feeder.stop. An early return here
   * would therefore leave it spinning for ever. feeder_srv still holds a
   * live shared-engine reference. If nothing destroys it, the teardown of
   * this whole binary hangs at the exit of the process. */
  chttpsvr feeder_srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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
      chttpsvr burst_srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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
      /* This bounds the lifetime of this child, for the case where the
       * hazard that this test guards against somehow still fires. It
       * therefore does not hang the whole suite. The parent below tells
       * this case apart from a clean exit with WIFEXITED. */
      alarm(1);
      chttpsvr_use(feeder_srv, _noop_middleware_for_lifecycle_tests, NULL);
      _exit(0); /* the code reaches this only when the call above returned */
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

extern void _chttpsvr_arm_stop_race_hook_for_tests(void);
extern void _chttpsvr_wait_stop_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_stop_race_hook_for_tests(void);

static chttpsvr g_fork_stop_race_srv = CHTTPSVR_INVALID;

static void *_fork_stop_race_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_stop(g_fork_stop_race_srv);
  return NULL;
}

/* Covers _chttpsvr_atfork_release_impl's own unconditional child-side reset
   of a server's lifecycle from CHTTPSVR_LC_STOPPING back to
   CHTTPSVR_LC_IDLE (see that function's own doc comment for the full
   account, including why an unconditional reset is safe here specifically,
   unlike quiesce_state above). fork() can land while a parent-side thread is
   genuinely mid-way through _chttpsvr_stop_internal's own real teardown work
   for some server (lifecycle already CHTTPSVR_LC_STOPPING), and that thread
   is not duplicated into the child; without that reset, chttpsvr_start()'s
   own retry loop (a plain 1ms-sleep poll on this exact state, not a condvar
   wait) spins forever for this handle in the child. Reuses g_stop_race_hook
   (already exercised by tests_engine_stop.c's own analogous, single-process
   scenario) to pause a real chttpsvr_stop() call at exactly that point, in
   the parent, before forking. */
TEST(chttpsvr_handle_lifecycle,
     fork_mid_stop_internal_does_not_hang_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  g_fork_stop_race_srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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

  /* Deterministic: this returns only once stop_th's own _chttpsvr_stop_
     internal() call has already entered CHTTPSVR_LC_STOPPING and is now
     paused right there, strictly before its own
     ccol_event_loop_remove()/close() call for the listener. */
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
    /* Bounds this child's own lifetime in case the hazard this test guards
       against still fires (chttpsvr_start() would otherwise block forever
       polling for lifecycle to leave CHTTPSVR_LC_STOPPING); the parent
       below tells this apart from a clean exit with WIFEXITED. */
    alarm(3);
    chttpsvr_config_t restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 73;
    /* Not asserted on: a genuinely fresh listener on this exact host:port
       may or may not bind cleanly here (the OLD, now-orphaned listener fd
       from the vanished parent-side thread may still be open in this
       child; see this test's own doc comment above), which is an accepted,
       already-documented, gracefully-handled outcome of this, not what
       this test itself checks. The only thing under test is whether the
       call returns at all. */
    chttpsvr_start(g_fork_stop_race_srv, &restart_cfg);
    _exit(0); /* reached only if the call above actually returned */
  } else if (!fork_failed) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
      waitpid_failed = true;
    } else {
      child_hung = !WIFEXITED(status);
    }
  }

  /* Parent side: let the real, paused stop call finish normally, then clean
     everything up here too, regardless of the child's own outcome (or
     whether fork()/waitpid() itself failed above); every REQUIRE_* below
     runs only after this unconditional cleanup, since g_fork_stop_race_srv
     still holds a live shared-engine reference and stop_th is still
     genuinely paused inside a real chttpsvr_stop() call until the hook is
     released. Deliberately no chttpsvr_engine_wait() here, mirroring
     fork_does_not_inherit_a_locked_mutex's own identical precedent just
     above: this file's own shared g_srv holds an engine reference for this
     whole binary's run, so waiting for the shared reactor to fully stop
     here would hang forever. */
  _chttpsvr_release_stop_race_hook_for_tests();
  bool stop_th_joined = _bounded_join(stop_th, NULL);
  if (!stop_th_joined) pthread_detach(stop_th);
  chttpsvr_destroy(g_fork_stop_race_srv);

  REQUIRE_FALSE(fork_failed);
  REQUIRE_FALSE(waitpid_failed);
  REQUIRE_FALSE(child_hung);
  REQUIRE_TRUE(stop_th_joined);
}

extern void _chttpsvr_arm_start_race_hook_for_tests(void);
extern void _chttpsvr_wait_start_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_start_race_hook_for_tests(void);

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

/* Covers _chttpsvr_atfork_release_impl's own unconditional child-side reset
   of a server's lifecycle from CHTTPSVR_LC_STARTING back to
   CHTTPSVR_LC_IDLE (see that function's own doc comment for the full
   account, including why an unconditional reset is safe here specifically,
   mirroring the CHTTPSVR_LC_STOPPING case right above). fork() can land
   while a parent-side thread is genuinely mid-way through chttpsvr_start()'s
   own real work for some server (lifecycle already CHTTPSVR_LC_STARTING:
   worker/reject pools created, an engine reference confirmed, but strictly
   before the listener socket itself is ever created), and that thread is
   not duplicated into the child. Without that reset, this handle is left
   permanently unusable in the child: chttpsvr_start()'s own retry loop
   treats CHTTPSVR_LC_STARTING exactly like a second, genuinely concurrent
   chttpsvr_start() call and refuses outright with ccol_not_permitted (no
   retry at all, unlike the CHTTPSVR_LC_STOPPING case above), and
   chttpsvr_stop() cannot unstick it either (CHTTPSVR_LC_STARTING resolves
   to a silent was_started == false no-op that never touches lifecycle).
   Reuses g_start_race_hook (already exercised by tests_engine_stop.c's own
   analogous, single-process scenario) to pause a real chttpsvr_start() call
   at exactly that point, in the parent, before forking. */
TEST(chttpsvr_handle_lifecycle,
     fork_mid_start_internal_does_not_disable_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  g_fork_starting_race_srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
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

  /* Deterministic: this returns only once start_th's own chttpsvr_start()
     call has already entered CHTTPSVR_LC_STARTING, confirmed its engine
     reference, and registered with servers_bundler, and is now paused right
     there, strictly before it ever creates the listener socket. */
  _chttpsvr_wait_start_race_hook_entered_for_tests();

  int pipefd[2];
  int pipe_rv = pipe(pipefd);
  if (pipe_rv != 0) {
    /* Nothing opened pipefd, so this code has nothing of its own to close.
     * But start_th is still paused, and the hook is still armed. Clean both
     * of those up before the REQUIRE_* below can return early. Every other
     * early-failure path in this test does the same. */
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
    /* Bounds this child's own lifetime purely as a safety net (unlike the
       CHTTPSVR_LC_STOPPING scenario above, this hazard does not hang: an
       unfixed CHTTPSVR_LC_STARTING refuses immediately with
       ccol_not_permitted rather than blocking); the parent below
       tells a genuine hang apart from a clean exit with WIFEXITED. The
       child reports its own outcome through the pipe rather than through
       its own process exit code: under make memtest, valgrind overrides a
       forked child's real exit code with its own --error-exitcode the
       instant it finds ANY "still reachable" allocation in that child's
       inherited process image at exit time (which every child forked
       mid-suite always has, since the rest of this suite has not quiesced
       yet), so the exit code cannot reliably carry this result; see
       tests_engine_stop.c's own fork_mid_quiesce_teardown_does_not_hang_
       child (and, further afield, tests/cthreadpool's own wait_from_
       within_own_task_does_not_hang and tests/clogger's own fork_safety
       group) for the original, independently-confirmed account of this
       exact valgrind behaviour. */
    alarm(3);
    chttpsvr_config_t restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 75;
    /* The property under test: without that reset, this call returns
       ccol_not_permitted (lifecycle still stuck at CHTTPSVR_LC_STARTING
       from the vanished parent-side thread) instead of genuinely being able
       to proceed. Whether it succeeds outright here is not itself asserted
       on (only that it is not the one specific, permanent failure mode that
       reset prevents) since a fresh listener may
       still legitimately fail to bind for unrelated environmental reasons;
       see this test's own doc comment above. */
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

  /* Parent side: let the real, paused start call finish normally, then
     clean everything up here too, regardless of the child's own outcome (or
     whether waitpid() itself failed above). Deliberately no
     chttpsvr_engine_wait() here, mirroring fork_does_not_inherit_a_locked_
     mutex's own identical precedent above: this file's own shared g_srv
     holds an engine reference for this whole binary's run, so waiting for
     the shared reactor to fully stop here would hang forever. */
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

/* _listener_on_readable() receives srv as a bare void* ccol_event_loop
   callback arg, entirely outside the ordinary chttpsvr-handle resolve/pin
   mechanism every other entry point into this server goes through.
   ccol_event_loop_remove()'s own
   documented "callers may free whatever the registration's own arg points
   to immediately after this call returns" promise does not cover a
   callback already in progress at the moment of the call (ccol_event_loop never
   waits on the registration's own dispatch_lock during removal), so
   without a pin of its own chttpsvr_destroy() can free srv while a
   still-running listener dispatch keeps reading its fields. The window is
   normally a handful of
   instructions, real but not reliably reproducible; it widens to one
   valgrind can catch whenever a dispatch legitimately takes longer (the
   backoff after a persistent post-accept allocation failure, for instance).
   Reproduced deterministically here with a dedicated white-box race hook
   rather than by trying to win that real-timing race. */
extern void _chttpsvr_arm_listener_dispatch_race_hook_for_tests(void);
extern void _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_listener_dispatch_race_hook_for_tests(void);

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
  /* Every intermediate step (releasing the hook, joining the destroy
     thread, closing fd_a, destroying srv) runs unconditionally before any
     REQUIRE_* that could return early, so a failing assertion here can
     never leave the race hook permanently armed (hanging every later test
     in this binary that dispatches a listener), a destroy thread
     permanently parked waiting for a release that would otherwise never
     come, or srv leaked with its own engine reference still held (which
     would hang chttpsvr_engine_wait() in this file's own _teardown() at
     process exit, converting a rare setup failure into a silent
     whole-binary hang instead of a clean, reported failure). */
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* One real connection attempt is enough to trigger a genuine dispatch of
     _listener_on_readable on the shared reactor thread; the armed hook
     parks that dispatch right after it has pinned srv (listener_dispatch_
     pins) but strictly before it does anything else with it. */
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
  int connect_rv = connect(fd_a, (struct sockaddr *)&sa, sizeof(sa));
  /* Checked, unlike a bare fire-and-forget call: _wait_..._entered_for_tests
     just below blocks on an unbounded ccol_cond_var_wait (no deadline, by
     design matched to this hook's every other caller, which all rely on a
     genuine connect() having actually triggered a real dispatch). A failed
     connect() here would mean the listener never dispatches at all, hanging
     this test (and, since the hook would stay armed, every later test in this
     binary that dispatches a listener) forever instead of failing cleanly. */
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
    /* No thread was ever created to own the destroy, so nothing else will
       ever release the listener dispatch (already parked in the hook, per
       the _wait_..._entered_for_tests() call just above) or destroy srv:
       release the hook and destroy srv here instead, before the
       REQUIRE_TRUE(destroy_th_created) below can return early; matches this
       test's own opening comment and every other pthread_create-failure
       branch elsewhere in this file. */
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }

  bool still_blocked = false;
  if (destroy_th_created) {
    /* Give chttpsvr_destroy() time to actually run and reach (and block
       inside) its own listener_dispatch_pins wait; matches this file's own
       established 150ms precedent for "let the other side reach its own
       blocking point" synchronization elsewhere. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* __chttpsvr_destroy() must still be blocked here, waiting for
       listener_dispatch_pins to reach 0; a destroy that has already
       returned would mean srv was freed while the listener dispatch is
       still paused holding a bare pointer to it. */
    still_blocked = !atomic_load(&destroy_returned);
  }

  /* Release the hook so the parked dispatch can finish (observe srv->
     listen_fd already cleared by then, or not, either is fine; this
     dispatch's own outcome is not what is under test) and, in turn, let
     chttpsvr_destroy()'s own wait proceed. Harmless even if the dispatch
     never actually reached the hook (would mean fd_a's connection was
     somehow never dispatched at all, itself a real bug the "entered" wait
     above would already have caught by hanging). */
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

/* A listener dispatch can start before chttpsvr_stop removes the listener
   and reach its first statement only afterwards. At that point it holds no
   pin, so neither the stop nor the destroy that follows it waits for it.
   The memory of the server must still stay valid for that dispatch: the
   listener registration holds a reference on the server until its
   on_removed fires, which happens only after the dispatch returns. The
   dispatch must also see that its listener is gone, and return without
   running the accept loop. The entry race hook parks the dispatch at its
   first statement, which makes the window deterministic. This test is
   non-vacuous: without the reference, the final free of the server runs
   before the parked dispatch returns, and without the check of the
   listener fd, the parked dispatch runs the accept loop. */
extern void _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests(void);
extern void _chttpsvr_watch_finish_destroy_for_tests(chttpsvr h);
extern bool _chttpsvr_watched_finish_destroy_ran_for_tests(void);

/* Polls, for up to ms milliseconds, until the watched server reaches its
   final free. Returns whether it did. */
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

/* Polls, for up to ms milliseconds, until the destroy thread returns. */
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
  /* Every step that can leave the hook armed, the reactor thread parked or
     the server alive runs before the first REQUIRE_* below. */
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
      fd >= 0 && connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  if (!connected) {
    /* No dispatch will ever reach the hook. */
    _chttpsvr_release_listener_dispatch_race_hook_for_tests();
    if (fd >= 0) close(fd);
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(connected);
  }

  /* The dispatch now sits at its first statement, with no pin taken. */
  _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests();
  size_t dispatches_before = _chttpsvr_listener_dispatch_count_for_tests();

  entry_race_destroy_arg_t darg = {.h = srv, .returned = false};
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, _entry_race_destroy_thread, &darg) == 0;
  if (!created) chttpsvr_destroy(srv); /* returns; the dispatch is unpinned */

  /* Nothing in the destroy waits for an unpinned dispatch, so it returns
     while the dispatch is still parked. The final free of the server must
     not have run by then. */
  bool destroy_returned =
      created ? _wait_entry_race_destroy_returned(&darg, 20000) : true;
  bool freed_under_dispatch = _chttpsvr_watched_finish_destroy_ran_for_tests();

  _chttpsvr_release_listener_dispatch_race_hook_for_tests();
  bool joined = created ? _bounded_join(tid, NULL) : true;
  if (created && !joined) pthread_detach(tid);
  /* The on_removed of the listener fires once the dispatch returns, and it
     drops the last reference. */
  bool finished = _wait_watched_finish_destroy(10000);
  size_t dispatches_after = _chttpsvr_listener_dispatch_count_for_tests();
  close(fd);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(destroy_returned);
  REQUIRE_FALSE(freed_under_dispatch);
  REQUIRE_TRUE(joined);
  REQUIRE_TRUE(finished);
  /* The parked dispatch found its listener gone and never ran the accept
     loop. */
  REQUIRE_EQ(dispatches_after, dispatches_before);
}

/* A connection whose handler outlives the bounded drain of a destroy is
   freed later, from the on_removed of its registration. Until then it can
   still read its matched route: its handler, its middleware chain and its
   parameter names and count. The route data of the server must therefore
   live until the final free of the server, which waits for every such
   connection. A white-box counter in the library records a teardown of the
   route data that finds a connection of the server still allocated, and it
   must stay at 0.

   The test makes the order deterministic. The slow request is the second
   one on its keep-alive connection, so the connection holds a registration
   of its own. The drain bounds are shrunk, so the destroy gives up on the
   slow handler at once and shuts its socket down. The single reactor thread
   is parked in the entry race hook of the listener, so the on_removed of the
   connection cannot fire until the test releases it, after the destroy has
   returned. This test is non-vacuous: with the route data freed inside
   chttpsvr_destroy, the counter goes up by one. */
extern void _chttpsvr_set_drain_connections_wait_ms_for_tests(unsigned ms);
extern size_t _chttpsvr_route_teardown_with_live_connections_for_tests(void);

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

  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* The first request completes and leaves the connection registered with
     the reactor; the second one reaches the slow handler. */
  int fd_a = socket(AF_INET, SOCK_STREAM, 0);
  bool ok = fd_a >= 0 && connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)) == 0;
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
    ok = fd_b >= 0 && connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)) == 0;
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

  /* The destroy gives up on the slow handler and shuts its connection
     down, which the client sees as the end of the stream. */
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

/* The library must never confuse legitimate reuse of a slot with a stale
 * handle to whatever held that slot before. A naive design that keys on an
 * address, and that remembers every pointer that it destroyed for ever,
 * cannot handle that scenario safely. The tcache of glibc routinely reuses
 * the exact address of a struct chttpserver that something just freed, for
 * the very next one that it allocates. It does not guarantee that, but it
 * does it often. */
TEST(chttpsvr_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpsvr a = ccol_create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(a, CHTTPSVR_INVALID);
  chttpsvr stale_a = a;
  chttpsvr_destroy(a);

  chttpsvr b = ccol_create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(b, CHTTPSVR_INVALID);

  /* Every operation on b must succeed normally. That holds whether or not
   * the allocator reused the exact address of a for b. */
  REQUIRE_EQ(chttpsvr_use(b, _noop_middleware_for_lifecycle_tests, NULL),
             ccol_success);

  /* The stale handle of a must never resolve to b. That holds even when b
   * reuses the same address below it. That is the whole point of the
   * generation counter. */
  REQUIRE_EQ((void *)_chttpsvr_resolve_for_tests(stale_a), NULL);

  chttpsvr_destroy(b);
}

/* The slot table is bounded, and it does not grow for ever. Take a churn
 * loop of create and destroy with only one slot in flight at a time. It
 * must reuse that one freed slot on every iteration, and the table must not
 * grow further. This test captures the capacity right after the first pair
 * of a create and a destroy. It does not assert a fixed absolute value.
 * Other tests earlier in this same process may already have grown the table
 * to some N > 1. What this test must prove is that ITS OWN churn adds no
 * more growth. */
TEST(chttpsvr_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 100 };

  chttpsvr s0 = ccol_create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(s0, CHTTPSVR_INVALID);
  chttpsvr_destroy(s0);
  size_t capacity_after_first = _chttpsvr_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    REQUIRE_NE(srv, CHTTPSVR_INVALID);
    chttpsvr_destroy(srv);
  }

  REQUIRE_EQ(_chttpsvr_slot_table_capacity_for_tests(), capacity_after_first);
}

/* ========================================================================== */
/*             DOUBLING-GROWTH REGISTRY OVERFLOW GUARD                        */
/* ========================================================================== */

extern size_t _chttpsvr_doubling_growth_cap_for_tests(size_t cap,
                                                      size_t elem_size,
                                                      size_t initial);

TEST(doubling_growth_cap, first_growth_returns_initial) {
  /* cap == 0 (nothing allocated yet) must always return `initial` verbatim,
   * regardless of elem_size, matching every caller's own "cap ? cap*2 : 8"
   * shape. */
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
  /* cap * 2 itself wraps past SIZE_MAX. The function must return 0, which
   * is the documented "cannot grow" sentinel. That matches the guard idiom
   * of this project, which compares against SIZE_MAX and which this file
   * uses throughout. _router_add_route, chttpsvr_subrouter,
   * chttpsvr_resp_set_header and _parse_qparams all make the same check. */
  REQUIRE_EQ(
      _chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX, sizeof(void *), 8),
      (size_t)0);
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX / 2 + 1,
                                                     sizeof(void *), 8),
             (size_t)0);
}

TEST(doubling_growth_cap, byte_size_multiplication_overflow_rejected) {
  /* cap * 2 alone does not overflow. But the new_cap * elem_size multiply
   * that the caller makes after it does overflow. The function must still
   * return 0. It must not return a new_cap that looks valid on its own and
   * that silently makes the real allocation too small once the caller
   * multiplies it by elem_size. */
  size_t cap = SIZE_MAX / 2; /* cap*2 == SIZE_MAX-1, no overflow on its own */
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(cap, 4096, 8), (size_t)0);
}

/* ========================================================================== */
/*             ACCEPT()-FAILURE ERRNO CLASSIFICATION                          */
/*                                                                            */
/* The accept4() error handling of _listener_on_readable must not treat     */
/* every unexpected failure in the same way. That treatment writes no log,  */
/* and it returns at once with no retry. Consider a resource-exhaustion     */
/* condition that persists. EMFILE, ENFILE, ENOBUFS and ENOMEM are such     */
/* conditions, where the process or the system truly has no file descriptor */
/* left. That is a realistic state under a sustained flood of connections   */
/* with a modest ulimit -n. The listen backlog then stays non-empty,        */
/* because nothing ever accepted anything. The level-triggered epoll of the */
/* reactor therefore dispatches this same handler again at once. That is a  */
/* silent busy loop with no bound. It burns 100% of a CPU, and it gives an  */
/* operator nothing to see, for as long as the condition lasts. Two helpers */
/* that classify the errno handle this. For the resource-exhaustion class,  */
/* the code sleeps briefly to back off, and it writes a log that a rate     */
/* limit governs. For a truly transient error of one connection, such as    */
/* ECONNABORTED, it retries at once, and it does not make a fresh round     */
/* trip through epoll. To drive the real EMFILE and ECONNABORTED conditions */
/* from end to end would need a change to the fd limits of this whole test  */
/* process, or to connection state inside the kernel. That depends on the   */
/* environment, and it is out of proportion for a test of a simple,         */
/* deterministic classification of an integer. These tests therefore drive  */
/* that classification directly, through a white-box hook.                  */
/* ========================================================================== */

extern bool _chttpsvr_accept_errno_is_transient_for_tests(int e);
extern bool _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(int e);

TEST(accept_errno_classification,
     econnaborted_and_pending_network_errors_are_transient) {
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ECONNABORTED));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPROTO));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENETDOWN));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENETUNREACH));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENOPROTOOPT));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EHOSTDOWN));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EHOSTUNREACH));
  /* accept(2)'s own ERRORS section names this exact set verbatim: "In the
     case of TCP/IP, these are ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN,
     ENONET, EHOSTUNREACH, EOPNOTSUPP, and ENETUNREACH." */
#ifdef ENONET /* Linux only */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENONET));
#endif
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EOPNOTSUPP));
  /* EPERM ("Firewall rules forbid connection", per accept(2)'s own ERRORS
     section) is the same per-pending-connection-rejection shape as
     ECONNABORTED, just sourced from a firewall rule instead of the peer;
     a realistic condition for a server behind connection-rate-limiting/
     fail2ban-style REJECT rules, not merely a theoretical one. */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPERM));
  /* ETIMEDOUT/ENOSR/ESOCKTNOSUPPORT/EPROTONOSUPPORT: accept(2)'s own "in
     addition, network errors for the new socket... may be returned; various
     Linux kernels can return other errors such as..." sentence, the same
     per-pending-connection category as the others above. */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ETIMEDOUT));
#ifdef ENOSR /* Linux only */
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENOSR));
#endif
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ESOCKTNOSUPPORT));
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(EPROTONOSUPPORT));

  /* None of these are also (incorrectly) classified as resource exhaustion. */
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

  /* None of these are also (incorrectly) classified as transient. */
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(EMFILE));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENFILE));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENOBUFS));
  REQUIRE_FALSE(_chttpsvr_accept_errno_is_transient_for_tests(ENOMEM));
}

TEST(accept_errno_classification, unrelated_errnos_are_neither) {
  /* EWOULDBLOCK/EAGAIN/EINTR are handled entirely separately, before either
     classification helper is ever consulted (see _listener_on_readable);
     a handful of unrelated errno values, and both of those two themselves,
     must not be swept into either bucket. */
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

/* Opens a connection to port and returns the fd, or -1. Sends nothing. */
static int _connect_silent(uint16_t port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }
  /* Bounded so that a regression which stops serving this connection fails
     the test rather than wedging the whole binary in recv(). */
  struct timeval rcv = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  return fd;
}

/* Waits, bounded, for the peer to close fd. Returns true once it has. The
 * bound is a hang-safety net, not the mechanism: the loop returns as soon as
 * the sweep actually fires. */
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
 * read_timeout_us == 0 turns the idle sweep off. A connection that opens
 * and then says nothing is left alone. The server does not close it out
 * from under the peer. include/chttpserver.h promises exactly that for a
 * connection that "sends a partial header block (or nothing at all) and
 * then goes quiet".
 *
 * This test calibrates itself, and it does not race a clock. It idles a
 * second server beside the first, with a short positive read_timeout_us.
 * The check then waits for THAT connection to close. That is a real
 * transition of state, and not an elapsed duration. The sweep runs on its
 * own period, so no fixed sleep would be correct here. Only after the sweep
 * demonstrably fires does the test require the connection of the disabled
 * server to still work. This test therefore cannot pass merely because
 * nothing ever closes anything.
 */
TEST(chttpserver, read_timeout_ms_zero_leaves_an_idle_connection_alone) {
  chttpsvr disabled _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  chttpsvr calibrated _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  off.idle_timeout_us = 0; /* 0 here means "same as read_timeout_us" */
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
  /* The calibration: the sweep really does close an idle connection when a
     positive timeout is configured. */
  REQUIRE_TRUE(sweep_fired);
  REQUIRE_EQ(served_when_disabled, 200);
}

TEST(chttpserver, max_connections_zero_means_unlimited) {
  enum { CONNS = 12 };
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/many",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 92;
  cfg.max_connections = 0; /* the unlimited state under test */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Held open together, so the cap (if any were applied) is in force for the
     later ones rather than each connection being reused serially. */
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
    if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
      close(fd);
      break;
    }
    /* Bounded, so a cap wrongly applied at 0 fails this test rather than
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

/* Sends one request on its own connection to an arbitrary port and returns
   the status code, or -1 on a socket-level failure. _raw_request above is
   the same thing pinned to TEST_PORT; these tests each stand up their own
   server, so they need the port as an argument. */
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

/* A sub-router middleware that answers 401 and does not call next. That is
   the real shape of an authenticator that you mount on a prefix. */
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
  /* A root-level pattern with named parameters overlaps every path of the
     same arity in the process, a sub-router's own mount included. Routers
     are tried by mount-prefix specificity, so the sub-router answers inside
     its own prefix and its middleware chain runs; the root pattern serves
     everything outside that prefix.

     The sub-router and its route are registered FIRST and the broad root
     pattern afterwards. A router choice that follows registration order
     would pick the sub-router here too, so this order alone cannot tell the
     two rules apart. subrouter_wins_over_root_
     route_at_same_path pins the opposite order on g_srv. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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

  /* The sub-router's own chain answered, so nothing reached the root
     handler: without the specificity ordering this is 200 "root-catch-all"
     and the mounted authenticator never runs at all. */
  REQUIRE_EQ(mounted, 401);
  REQUIRE_TRUE(mounted_body_is_auth);
  /* A path outside every mount still reaches the root pattern. */
  REQUIRE_EQ(outside, 200);
  REQUIRE_TRUE(outside_body_is_root);
}

TEST(chttpserver, more_specific_mount_wins_over_less_specific_one) {
  /* Two mounts both match /api/v1/thing; the one with more prefix segments
     answers, whichever was registered first. The shorter prefix is
     registered first here, so registration order alone would pick it. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* "Hello, world!" is what the GET body would be; a HEAD response must not
     contain it. */
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, explicit_head_route_wins_over_get_route) {
  /* Both a CHTTP_GET and a CHTTP_HEAD route are registered for /head-pref,
     the GET one first (see _setup). The explicit HEAD route must serve a
     HEAD request, and must not serve a GET one. */
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
  /* The fallback is GET-to-HEAD only: /echo-body is registered for
     CHTTP_POST alone, so a HEAD request for it matches the path but no
     method and is rejected with 405, exactly as before. */
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
   threads) and read by the test body once the response has been received.
   g_rej_mw_runs is the synchronisation edge: the middleware fills the two
   observation buffers and only then increments it, and every test reads
   those buffers only after observing the increment. */
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
  /* A rate limiter's own shape: answer the request itself and never call
     next, so the chain short-circuits and the rejection this request was
     heading for is replaced by this response. */
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

/* One server shared by every rejection-middleware test below, started on
   first use and stopped by _teardown's atexit hook, through the destroy
   below.
   Kept out of _setup so the tests that do not need it pay nothing for it. */
static void _rej_srv_destroy_at_exit(void) {
  if (g_rej_srv != CHTTPSVR_INVALID) {
    chttpsvr_destroy(g_rej_srv);
  }
}

static bool _rej_srv_ready(void) {
  if (g_rej_srv != CHTTPSVR_INVALID) return true;
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
    /* Small enough that an ordinary Content-Length is already over the
       limit, so the 413 pre-check fires at headers-complete without any
       body ever being sent. */
    cfg.max_body_size = 64;
    if (chttpsvr_start(srv, &cfg) != ccol_success) goto done;
  }
  ok = true;
done:
  /* Destroyed right here on any failure: the caller's REQUIRE_TRUE returns
     from the test on a false, so nothing else would ever reclaim it and
     memtest reports the whole server as definitely lost. */
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
     middleware exists to see: without the chain running here, a flood of
     probes for paths that do not exist is invisible to the application. */
  REQUIRE_TRUE(_rej_srv_ready());
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "GET", "/nope", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(after, before + 1);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") != NULL);
  /* The middleware can observe the request it is being asked about. */
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
  /* The middleware answers /rate-limited itself and never calls next, so
     the 404 this request was heading for is replaced by its own 429. */
  REQUIRE_TRUE(_rej_srv_ready());
  char buf[2048] = {0};
  int status = _raw_request_on_port(REJ_PORT, "GET", "/rate-limited", NULL, buf,
                                    sizeof(buf));
  REQUIRE_EQ(status, 429);
  REQUIRE_TRUE(strstr(buf, "slow down") != NULL);
}

TEST(chttpserver, payload_too_large_rejection_runs_the_matched_routers_chain) {
  /* A 413 is decided after a route has already matched, so the chain that
     match produced runs in full: the global middleware and the mounted
     sub-router's own. No handler runs. */
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
  /* 501 is decided from the request line, for a method token outside the
     seven chttp_method_t names and before the request target has been
     parsed, so there is no request object a middleware could observe; the
     chain deliberately does not run. */
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

/* A rejection decides the decoded path a middleware reads lazily, with one
   allocation that can fail under memory pressure. chttpsvr_req_path is
   documented to hand a middleware a readable path, and a middleware written
   the documented way (an access log, a rate limiter) reaches straight for
   strlen/strcmp/printf("%s") on it, so a rejection whose path could not be
   decoded must not run the chain at all; without that, a scanner generating
   404s against a process short of memory takes the server down.

   Targeting that one allocation: the request target below is
   REJ_OOM_PATH_LEN bytes long, and both the raw copy the request-line parse
   stores and the decoded copy made for the middleware are exactly
   REJ_OOM_PATH_LEN + 1 bytes. The raw copy always comes first, and nothing
   else this server allocates while serving this one request is that size, so
   refusing the SECOND allocation of exactly that size after arming refuses
   the decode and nothing else. The test asserts that both sites were reached
   and that exactly one allocation was refused, so a change that moves either
   allocation fails this test rather than making it pass vacuously. */

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

/* Records what it was handed rather than dereferencing it, so a regression
   is a failed assertion here instead of a SIGSEGV that would take every
   other test's result in this binary down with it. */
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
    srv = ccol_create_chttpsvr_mp(&procs, g_test_logger, NULL);
  }
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _rej_oom_mw, NULL), (int)ccol_success);
  /* One registered route, on a path the request below deliberately misses,
     so the request earns a 404: the status the chain does run for. */
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
  /* Disarmed before anything else runs, so the server's own teardown below
     is served by an honest allocator. */
  atomic_store(&g_rej_oom_armed, false);

  bool saw_null_path = atomic_load(&g_rej_oom_mw_saw_null_path);
  int mw_runs = atomic_load(&g_rej_oom_mw_runs);
  int target_seen = atomic_load(&g_rej_oom_target_seen);
  int refused = atomic_load(&g_rej_oom_refused);

  chttpsvr_stop(srv);

  /* The contract: a middleware is never handed a NULL path. */
  REQUIRE_FALSE(saw_null_path);
  /* And the way that is kept: the chain does not run at all, exactly as it
     does not when the rejection pool is saturated. */
  REQUIRE_EQ(mw_runs, 0);
  /* 404, not 500: the raw copy of the target succeeded, so the refusal
     landed on the decode and not on the request-line parse. */
  REQUIRE_EQ(status, 404);
  /* Both allocation sites were reached, and exactly one was refused. */
  REQUIRE_EQ(target_seen, 2);
  REQUIRE_EQ(refused, 1);
}

/* ========================================================================== */
/*                 HEADER-PHASE TOTAL DURATION CEILING                        */
/* ========================================================================== */

/* Sends one byte of a deliberately never-terminated header block every
   interval_ms and reports whether the server closed the connection within
   bound_ms.

   Every byte is a legitimate part of a well-formed header block, so nothing
   but a total-duration bound can end this: the parser is always waiting for
   more input rather than rejecting anything, read_timeout_us/idle_timeout_us
   measure only the gap between bytes and are reset by every one of them, and
   the handful of bytes this sends stays orders of magnitude below
   max_header_bytes. The poll doubles as the inter-byte delay and as the
   close detector, so a server that closes the connection is noticed on the
   same iteration rather than one interval later. */
static bool _trickle_headers_until_closed(int fd, int interval_ms,
                                          int bound_ms) {
  static const char prefix[] = "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
  const int plen = (int)(sizeof(prefix) - 1);
  for (int waited = 0, i = 0; waited < bound_ms; waited += interval_ms, i++) {
    /* Past the prefix the same header line simply keeps growing, so the
       block never completes however long this runs. */
    char c = (i < plen) ? prefix[i] : 'x';
    if (send(fd, &c, 1, MSG_NOSIGNAL) != 1) return true; /* peer is gone */
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, interval_ms) > 0) {
      char probe[1];
      if (recv(fd, probe, sizeof(probe), MSG_PEEK) <= 0) return true;
    }
  }
  return false;
}

/* Sends one request and consumes its WHOLE response (status line, headers
   and exactly content-length body bytes), returning the status code or -1.
   _get_on above stops at the first recv, which is enough when the caller
   closes straight afterward; a caller that sends a SECOND request on the
   same keep-alive connection has to drain the first response in full, or a
   response that arrived split across two segments leaves its tail in the
   socket buffer and the next read parses that tail as the second response's
   status line. */
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

/* Sends a complete, well-formed request one byte at a time, interval_ms
   apart, then reads the status line. Returns the status code, or -1. */
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
  /* The shipped defaults have to be safe on their own: a deployment that
     never touches this knob is still bounded against a trickled header
     block. Read off the macro rather than the server, so a change to the
     default is a change to this assertion. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_EQ(cfg.max_header_read_duration_us, (uint64_t)30000000);
}

TEST(chttpserver, trickled_header_block_is_closed_at_the_duration_ceiling) {
  /* The connection sends real header bytes continuously, so the idle
     timeout (deliberately left at a minute here) never fires and the byte
     count never approaches max_header_bytes. Only the total-duration
     ceiling can end it. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
    /* The bound is many times the ceiling plus the sweep's own interval, so
       it only expires when the ceiling genuinely is not enforced. */
    closed = _trickle_headers_until_closed(fd, 100, 15000);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_TRUE(closed);
}

TEST(chttpserver, header_read_duration_zero_leaves_a_trickling_peer_alone) {
  /* 0 keeps this knob's documented "no limit" meaning, matching every other
     limit in chttpsvr_config_t. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
    /* Twice the ceiling the sibling test uses, plus the sweep's interval:
       long enough that an enforced ceiling of any comparable size would
       already have closed this. */
    closed = _trickle_headers_until_closed(fd, 100, 2 * HDR_DURATION_MS + 1200);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_FALSE(closed);
}

TEST(chttpserver, slow_but_completing_header_block_is_served) {
  /* The ceiling must not break a client that is merely slow. This one sends
     its whole header block a byte at a time, taking a few hundred
     milliseconds against a ceiling of HDR_DURATION_MS, and is served
     normally. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 105;
  cfg.max_header_read_duration_us = (uint64_t)HDR_DURATION_MS * 1000u;
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
  /* The budget covers one request's reactor-owned phase, so a keep-alive
     connection's second request gets its own. The gap between the two is
     deliberately longer than the ceiling and far shorter than the idle
     timeout: a budget anchored at the connection instead of the request
     would have closed this connection during that gap, and the second
     request would never be answered. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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
    /* Split across both fields: tv_nsec must stay below one second, and a
       value above it makes nanosleep fail with EINVAL immediately, which
       would leave no gap at all and make this test pass vacuously. */
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
  /* The exit-time destructor releases the slot table's own vectors only when
     no slot is still live. __chttpsvr_destroy clears slot->in_use as its
     very FIRST step, so that a second destroy or a new resolve is rejected
     immediately, and then runs the whole rest of its teardown (quiescing the
     server, draining pins, destroying routers) before its last step indexes
     that same table to clear slot->ptr and push the index back onto the free
     list. Throughout that window the slot is not in use and still names a
     server, and it must count as live: a process exit landing in it
     otherwise frees both vectors while the teardown is still going to write
     through them.

     The window is constructed directly rather than raced against a real
     teardown, which is not reachable deterministically from a test. */
  extern bool _chttpsvr_slot_live_for_tests(chttpsvr h);
  extern void _chttpsvr_slot_set_in_use_for_tests(chttpsvr h, bool in_use);

  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  bool live_while_resolvable = _chttpsvr_slot_live_for_tests(srv);
  _chttpsvr_slot_set_in_use_for_tests(srv, false);
  bool live_mid_teardown = _chttpsvr_slot_live_for_tests(srv);
  /* Restored before destroying: every public entry point, chttpsvr_destroy
     included, refuses a handle whose slot is not in use. */
  _chttpsvr_slot_set_in_use_for_tests(srv, true);
  /* Kept because chttpsvr_destroy NULLs its argument, and a NULL handle
     names slot 0, which belongs to another server entirely. */
  chttpsvr destroyed = srv;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(live_while_resolvable);
  REQUIRE_TRUE(live_mid_teardown);
  /* The slot is released as the last step of the destroy above, so it is no
     longer live and no longer holds the table open. */
  REQUIRE_FALSE(_chttpsvr_slot_live_for_tests(destroyed));
}

/* ========================================================================== */
/*               REQUEST-TARGET FORM (RFC 9112 SS3.2)                         */
/* ========================================================================== */

/* A middleware that guards a path prefix the way an application writes one:
   it reads chttpsvr_req_path() and refuses everything under "/admin". */
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
      ccol_create_chttpsvr(g_test_logger, NULL);
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
    const char *body; /* NULL: the body is not checked */
  } cases[] = {
      /* The guard sees the real path and refuses it. */
      {"GET", "/admin/secret", 403, "forbidden"},
      /* A target with no leading "/" would route to /admin/secret while
         the guard reads "admin/secret" and lets it through. */
      {"GET", "admin/secret", 400, NULL},
      {"GET", "admin", 400, NULL},
      {"GET", "?x=1", 400, NULL},
      /* The asterisk-form: see options_star_is_answered_by_the_server. */
      {"OPTIONS", "*", 200, ""},
      {"GET", "*", 400, NULL},
      {"POST", "*", 400, NULL},
      /* An absolute-form target with an empty path stands for "/". */
      {"GET", "http://127.0.0.1", 200, "/"},
      {"GET", "http://127.0.0.1?x=1", 200, "/"},
      {"GET", "http://127.0.0.1/", 200, "/"},
      /* The absolute-form path passes through the same guard. */
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
  /* The chunked body ends before the second request. A server that
     de-chunks and honours the keep-alive answers that second request as a
     request of its own, which a front end that framed the first request
     differently never sent. */
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

/* Removes the first "date:" header line of the first response in buf, in
   place, and returns buf. The Date tests check that line; the tests that
   compare a whole response byte for byte compare the rest. */
static char *_strip_date_line(char *buf) {
  char *end = strstr(buf, "\r\n\r\n");
  char *d = strstr(buf, "\r\ndate:");
  if (d && end && d < end) {
    char *eol = strstr(d + 2, "\r\n");
    memmove(d, eol, strlen(eol) + 1);
  }
  return buf;
}

/* Writes wire on a fresh connection and reads until the server closes it,
   or until the 5 s receive timeout. */
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
      ccol_create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  REQUIRE_EQ((int)chttpsvr_use(srv, _star_counting_mw, NULL),
             (int)ccol_success);
  /* A catch-all for every method. If the server routed "*" at all, the
     request would either reach this handler or be refused with a 404 that
     runs the middleware; either one moves a counter. */
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/",
                                            _star_counting_handler, NULL),
             (int)ccol_success);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = STAR_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  /* Every response also carries a Date line, which the Date tests pin. The
     helper below drops it so that the rest compares exactly. */
  static const char follow[] =
      "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  /* 1. Alone: 200, Content-Length 0, and nothing else runs. */
  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b1[2048];
  static const char w1[] =
      "OPTIONS * HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
  _star_exchange(w1, sizeof(w1) - 1, b1, sizeof(b1));
  int mw1 = atomic_load(&g_star_mw_calls);
  int h1 = atomic_load(&g_star_handler_calls);

  /* 2. Keep-alive: a request that follows on the same connection is served
     as usual. */
  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b2[4096];
  char w2[512];
  int n2 = snprintf(w2, sizeof(w2), "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n%s",
                    follow);
  _star_exchange(w2, (size_t)n2, b2, sizeof(b2));
  int mw2 = atomic_load(&g_star_mw_calls);
  int h2 = atomic_load(&g_star_handler_calls);

  /* 3. A small body, framed by Content-Length and by chunks, is read and
     discarded, and the connection stays usable. */
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

  /* 4. A body above 4 KiB still gets 200, and the connection closes, so the
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

extern void _chttpsvr_set_date_override_for_tests(int64_t sec);

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
      /* Two pipelined requests: the keep-alive response and the last one. */
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
  /* Only a final response carries a Date, as in Go's net/http server. */
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
  /* On a target whose time_t is 32 bits the last step does not fit, and
     the library then writes the epoch; it is checked only where it fits. */
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
  /* Go's net/http answers 400 for the target "*" with any method except
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

TEST(chttpserver, a_1xx_status_from_a_handler_carries_no_date) {
  /* Go's net/http writes no Date on a 1xx response, and neither does this
     server. */
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/status-103",
                                            _status_103_handler, NULL),
             (int)ccol_success);
  char buf[1024];
  _raw_request_to_test_server(
      "GET /status-103 HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", buf,
      sizeof(buf));
  REQUIRE_EQ(strncmp(buf, "HTTP/1.1 103", 12), 0);
  char val[64];
  REQUIRE_FALSE(_first_date_value(buf, val, sizeof(val)));
  int total = 0;
  REQUIRE_EQ(_responses_with_one_date(buf, 0, 0, &total), 0);
}

/* ========================================================================== */
/*                    SLOW CLIENTS                                            */
/* ========================================================================== */

/* These tests drive the parking of slow bodies and slow readers, the rate
 * floor, the partial-body memory limit and the streaming pool. Every wait on
 * the server is a bounded poll of a white-box state (never a sleep that the
 * correctness depends on), and every time limit that the sweep enforces is
 * crossed with the injected clock of _chttpsvr_advance_slow_clock_for_tests
 * and a sweep tick run on the test thread with _chttpsvr_sweep_now_for_tests.
 * Each test ends by asserting that no body memory stays reserved. */

extern void _chttpsvr_advance_slow_clock_for_tests(long long ms);
extern size_t _chttpsvr_resume_dispatch_count_for_tests(void);
extern size_t _chttpsvr_park_count_for_tests(void);
extern void _chttpsvr_set_rcvlowat_enabled_for_tests(bool enabled);
extern void _chttpsvr_arm_stream_claim_hook_for_tests(int where);
extern bool _chttpsvr_wait_stream_claim_hook_entered_for_tests(
    unsigned timeout_ms);
extern void _chttpsvr_release_stream_claim_hook_for_tests(void);
extern size_t _chttpsvr_mem_in_use_for_tests(chttpsvr h);
extern size_t _chttpsvr_mem_waiting_for_tests(chttpsvr h);
extern size_t _chttpsvr_stream_queue_len_for_tests(chttpsvr h);
extern bool _chttpsvr_stream_pool_created_for_tests(chttpsvr h);
extern size_t _chttpsvr_pool_active_for_tests(chttpsvr h, bool streaming);
extern size_t _chttpsvr_parked_count_for_tests(chttpsvr h, int kind);
extern size_t _chttpsvr_sweep_now_for_tests(chttpsvr h);
extern size_t _chttpsvr_mem_peak_for_tests(void);
extern void _chttpsvr_mem_peak_reset_for_tests(void);

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

/* Connects to 127.0.0.1:port. The socket has a receive timeout of 5 s, so
 * that a regression fails a read instead of hanging the binary, and
 * TCP_NODELAY, so that each write leaves at once: with Nagle on, a second
 * small write waits for the acknowledgement of the first, which a kernel
 * that delays acknowledgements on loopback (FreeBSD does) holds back, and
 * the bytes that a test has "sent" are not at the server yet. */
static int _sc_connect(int port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
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

/* Reads one response from fd into buf (headers, and as much of the body as
 * fits, NUL-terminated) and returns its status, or -1 on a timeout or an end
 * of stream before a complete response. *body_len_out gets the length of the
 * body that arrived, which can exceed the capacity of buf: the rest is read
 * and discarded. timeout_ms bounds the whole read. */
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

/* Reads until the end of the stream, for timeout_ms at most, and returns the
 * count of bytes, or -1 when the stream did not end in time. */
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

/* Waits, for timeout_ms at most, until a counter of the server reaches at
 * least want. It polls a white-box accessor; the bound only guards against a
 * hang. */
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

/* Answers with the length of the body that it received. */
static void _sc_len_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t n = 0;
  chttpsvr_req_body(req, &n);
  chttpsvr_resp_printf(resp, "%zu", n);
}

/* Answers with _SC_BIG_RESPONSE bytes, far more than the socket buffers of
 * a client that does not read can hold. */
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

/* A streaming handler that counts its runs and then holds its thread until
 * the test opens the gate, for 10 s at most. */
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

/* Starts a fresh server on port with the routes of these tests. */
static chttpsvr _sc_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

/* Sends the headers of a POST that declares cl body bytes, plus the first
 * part bytes of that body, and nothing more. */
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

TEST(slow_clients, slow_bodies_park_and_hold_no_worker_thread) {
  /* Six clients stop half way through their bodies, on a server with two
   * workers. Each one must park with no thread, so that the worker pool is
   * idle and an ordinary request is still served. This test is non-vacuous:
   * a body reader that waits on the socket blocks both workers, the six
   * connections never reach the parked state, and the GET waits behind
   * them. */
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

/* Connects with a small receive buffer, so that a client that does not read
 * leaves most of a large response in the server. */
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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

/* One client of slow_readers_park_and_hold_no_worker_thread: it reads its
 * large response, then asks for /hello on the same connection. */
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
  /* Four clients ask for a response far larger than the socket buffers and
   * read none of it, on a server with two workers. Each response parks on a
   * full socket with no thread. Non-vacuous: a response writer that waits on
   * the socket blocks both workers, and the parked count never reaches
   * four. */
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

  /* Every response then arrives whole, and the connection stays usable.
   * The four clients read at once: one that waits idle while the others
   * read falls below the transfer-rate floor once that wait passes its
   * grace, which is what the floor is for. */
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
  /* 1000 of 2000 declared bytes in 5 s is 200 B/s, above a floor of 100:
   * the sweep keeps the connection, and the rest of the body completes it.
   * A floor that measured the wrong span, or the wrong byte count, closes
   * it. */
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
  /* A parked body whose rest arrived waits in the queue of the only worker
   * while that worker is busy. The clock moves 60 s during that wait, past
   * the 30 s gap limit and past the rate floor, but the bytes were already
   * in the socket when the reactor claimed the connection: the request
   * completes with 200. This test is non-vacuous: a resume that judges the
   * limits when the worker gets to it, and not when the connection became
   * ready, answers 408. */
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
  /* 1000 bytes in 11 s is about 91 B/s, below a floor of 100: the sweep
   * answers 408 and closes. Non-vacuous: without the floor, the per-gap
   * limit of 30 s and the total limit of 5 minutes both still hold, and the
   * connection stays parked. */
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
  /* A client that reads none of a large response falls below the floor of
   * the write once the grace period is over, and the sweep closes the
   * connection with the response cut off. Non-vacuous: the per-gap write
   * limit is 10 minutes here and the total one 5 minutes of real time, so
   * nothing else closes it, and the drain then gets the whole response. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 1024 * 1024;
  cfg.min_transfer_rate_grace_us = 1000000;
  /* Only the floor may close it: the per-gap write limit is far above the
   * 60 s that the clock moves. */
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
  /* A streaming handler that waits for a stalled body gives up when the
   * rate floor is crossed, well before the 30 s of stream_read_timeout_us:
   * 10 bytes buy 10 ms at 1000 B/s, so the floor fires when the grace of
   * 300 ms ends. Non-vacuous: without the floor the read waits the full 30 s
   * and the 5 s bound of this read fails. */
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
  /* A chunked body reads into its reservation, runs out, and waits for
   * memory while a body with a Content-Length holds the rest. 60 s of that
   * wait pass on the clock; then the other body fails the floor, gives its
   * memory back, and the chunked body goes on. The 60 s were the server's,
   * so the floor of 10000 B/s must not count them, and the body completes.
   * Non-vacuous: counting the wait puts about 57 KiB over 60 s, under the
   * floor, and the resumed body gets 408. */
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

TEST(slow_clients, queue_wait_after_a_memory_wait_is_never_charged) {
  /* A chunked body waits for memory, as in the test above. When the memory
   * comes free the only worker is busy, so the admitted body waits in the
   * queue of the pool, and 60 s pass on the clock during that wait too, past
   * the 30 s gap limit and the rate floor. The body completes with 200.
   * Non-vacuous: a resume that judges the limits when the worker gets to it,
   * and not when the admission handed the body to the pool, answers 408. */
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

  /* The holder falls below the floor, the sweep answers it, and its memory
   * admits the chunked body into the queue of the busy worker. */
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
  /* A body that a Content-Length frames reserves its whole length before
   * anything reads it. With 90000 of 100000 bytes reserved by a first body,
   * a second body of 50000 waits: no worker takes it and it gets no
   * "100 Continue". Once the first body completes, the second one is
   * admitted, invited, read and answered. Non-vacuous: without the
   * reservation the second request is invited at once and nothing waits. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 7, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int a _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 7, "/len", 90000, 10, NULL);
  REQUIRE_TRUE(a >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  int b _ccol_destructor(_close_scoped_fd) = _sc_open_partial_post(
      _SC_PORT + 7, "/len", 50000, 0, "Expect: 100-continue\r\n");
  REQUIRE_TRUE(b >= 0);
  bool waiting = _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  struct pollfd p = {.fd = b, .events = POLLIN};
  int invited_early = poll(&p, 1, 0);

  char buf[4096];
  int a_status = -1;
  if (_sc_send_fill(a, 90000 - 10, 'a'))
    a_status = _sc_read_response(a, buf, sizeof(buf), NULL, 5000);
  int interim = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  int b_status = -1;
  if (interim == 100 && _sc_send_fill(b, 50000, 'b'))
    b_status = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  REQUIRE_TRUE(waiting);
  REQUIRE_EQ(invited_early, 0);
  REQUIRE_EQ(a_status, 200);
  REQUIRE_EQ(interim, 100);
  REQUIRE_EQ(b_status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n50000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

/* Writes every buffer to its socket, round robin and without blocking, until
 * all of them are out or timeout_ms passes. A server that reads one socket
 * only after another completes must not stall the writer. */
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
  /* Two chunked bodies of 200000 bytes against a limit of 64 KiB, with no
   * deadline on the wait. The first reserves the whole limit and then needs
   * more; the second waits for its first reservation. Every holder of
   * memory then waits, and the progress rule lets the oldest waiter go
   * ahead past the limit. Both complete, and the peak stays within one
   * max_body_size of the limit. Non-vacuous: without the progress rule both
   * wait for ever and neither response arrives. */
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
   * body_memory_wait_timeout_us with 503 and a Retry-After header, and
   * closes it. Non-vacuous: without that part of the sweep the waiter is
   * never answered and the read times out. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  /* the 31 s below must not expire it */
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 9, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int a _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 9, "/len", 90000, 10, NULL);
  REQUIRE_TRUE(a >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  int b _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 9, "/len", 50000, 0, NULL);
  REQUIRE_TRUE(b >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000));
  _chttpsvr_advance_slow_clock_for_tests(31000);
  _chttpsvr_sweep_now_for_tests(srv);
  char buf[4096];
  int status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
  long rest = _sc_drain_to_eof(b, 3000);
  size_t waiting = _chttpsvr_mem_waiting_for_tests(srv);
  int a_status = -1;
  if (_sc_send_fill(a, 90000 - 10, 'a'))
    a_status = _sc_read_response(a, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(rest >= 0);
  REQUIRE_EQ(waiting, (size_t)0);
  REQUIRE_EQ(a_status, 200);
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
  int a _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 10, "/len", 90000, 10, NULL);
  REQUIRE_TRUE(a >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  int b _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 10, "/len", 50000, 0, NULL);
  REQUIRE_TRUE(b >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000));
  _chttpsvr_advance_slow_clock_for_tests(31000);
  _chttpsvr_sweep_now_for_tests(srv);
  char buf[4096];
  int status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(strstr(buf, "retry-after:5\r\n") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(slow_clients, memory_waiters_are_admitted_in_order) {
  /* With 90000 of 100000 bytes held, a waiter that needs 50000 comes first
   * and one that needs 5000 second. The second would fit, yet it must not
   * pass the first, or a stream of small bodies could starve a large one.
   * Once the holder completes, both are admitted and served. Non-vacuous: a
   * reservation that ignores the waiters admits the small body at once. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _sc_server(_SC_PORT + 11, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int a _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 11, "/len", 90000, 10, NULL);
  REQUIRE_TRUE(a >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  int b _ccol_destructor(_close_scoped_fd) = _sc_open_partial_post(
      _SC_PORT + 11, "/len", 50000, 0, "Expect: 100-continue\r\n");
  REQUIRE_TRUE(b >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000));
  int c _ccol_destructor(_close_scoped_fd) = _sc_open_partial_post(
      _SC_PORT + 11, "/len", 5000, 0, "Expect: 100-continue\r\n");
  REQUIRE_TRUE(c >= 0);
  bool both_wait = _sc_wait_for(srv, _sc_mem_waiting, NULL, 2, 5000);
  struct pollfd p = {.fd = c, .events = POLLIN};
  int c_invited_early = poll(&p, 1, 0);

  char buf[4096];
  int a_status = -1, b_status = -1, c_status = -1;
  if (_sc_send_fill(a, 90000 - 10, 'a'))
    a_status = _sc_read_response(a, buf, sizeof(buf), NULL, 5000);
  if (_sc_read_response(b, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_fill(b, 50000, 'b'))
    b_status = _sc_read_response(b, buf, sizeof(buf), NULL, 5000);
  if (_sc_read_response(c, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_fill(c, 5000, 'c'))
    c_status = _sc_read_response(c, buf, sizeof(buf), NULL, 5000);
  REQUIRE_TRUE(both_wait);
  REQUIRE_EQ(c_invited_early, 0);
  REQUIRE_EQ(a_status, 200);
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
  /* Two streaming handlers hold both streaming threads and a third waits in
   * the streaming queue; a buffered route is still served at once by the
   * worker pool. Non-vacuous: with streaming handlers on the worker pool,
   * the two gated handlers hold both workers and the GET is not answered
   * within its bound. */
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

TEST(slow_clients, a_full_streaming_queue_answers_503_at_once) {
  /* One streaming thread, a queue of one. The third streaming request finds
   * the queue full and gets 503 with a Retry-After header, without waiting.
   * Non-vacuous: an unbounded queue takes it, and it waits for the gate. */
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
   * request whose deadline passed with 503, and its handler never runs.
   * Non-vacuous: without that part of the sweep the request waits for the
   * gate and its read times out. */
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

/* One round of the claim race: the streaming task stops just before (where
 * = 1) or just after (where = 2) it takes the queued request, the deadline of
 * that request passes, and the sweep runs. Exactly one of the two answers,
 * with one 503, and the handler never runs for it. */
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
   * Non-vacuous: a pool created at chttpsvr_start exists before any
   * request. */
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
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  bool after_buffered = _chttpsvr_stream_pool_created_for_tests(srv);
  int gate = -1;
  if (fd >= 0 && _sc_send_str(fd,
                              "POST /gate HTTP/1.1\r\nHost: h\r\n"
                              "Content-Length: 0\r\n\r\n"))
    gate = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  bool after_streaming = _chttpsvr_stream_pool_created_for_tests(srv);
  if (fd >= 0) close(fd);
  _sc_gate_open(false);
  REQUIRE_FALSE(before);
  REQUIRE_EQ(hello, 200);
  REQUIRE_FALSE(after_buffered);
  REQUIRE_EQ(gate, 200);
  REQUIRE_TRUE(after_streaming);
}

/* Drips body bytes one at a time with a short pause, and returns how many
 * times the reactor handed the parked body to a worker meanwhile. */
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
  /* A body of 200 bytes that arrives one byte at a time wakes the reactor
   * once with SO_RCVLOWAT, when the whole rest is queued, and about once a
   * byte without it. A fast upload of 4 MiB is counted both ways too. The
   * counts are printed for the record. Non-vacuous: the run without the
   * mark is the counterexample. */
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
   * request. The worker that resumes the body must hand the extra bytes on
   * as the next request, and not drop them. */
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
  /* Four pipelined requests, the first with a response far larger than the
   * socket buffers: the write parks, and the rest follow it in order once
   * the client reads. HEAD carries its content-length and no body, and a
   * 204 carries no body, although both handlers wrote one. */
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
  /* A parked body, a request that waits for memory, a parked response, a
   * streaming handler blocked on its client, and a streaming request in the
   * queue. Destroying the server must end every one of them, in bounded
   * time, with no leak and no use after free (memtest and test_tsan run this
   * same test). The queued streaming request is answered with 503. */
  extern void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned, unsigned);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);
  _sc_gate_open(false);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  cfg.max_partial_body_memory = 100000;
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

extern void _chttpsvr_force_short_response_write_for_tests(size_t n);
extern void _chttpsvr_set_date_override_for_tests(int64_t sec);

/* Sends req on fd and reads one response into buf; returns its whole length
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
  /* A response write that stops inside the header block, or inside the
   * body, parks and resumes on another worker from the exact byte where it
   * stopped. The client must receive the same bytes as a response that went
   * out in one write, and the connection must stay usable. HEAD is checked
   * the same way. Non-vacuous: a continuation that restarts the body, or
   * that loses a byte of the head, changes the bytes. */
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
  /* With the idle timeout and the header-phase limit off, a sweep tick must
   * examine the parked connections and nothing else, however many idle
   * keep-alive connections the server holds. Non-vacuous: a sweep that
   * walks the idle list regardless visits the 20 idle connections too. */
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

/* A mounted prefix owns every path under it. The server below mounts
 * "/admin" behind a guard, mounts "/admin/deep" on its own, and registers
 * root routes broad enough to catch any path of one to three segments. A
 * request for a path under "/admin", in any spelling that decodes to one,
 * must meet the guard, and a root route must never answer it. */

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

/* Lets a request through only with "x-key: ok"; answers 403 otherwise. */
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
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

/* Sends one request for target on a fresh connection and returns the status,
 * or -1. The response, with its body, lands in buf. */
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
  /* Each target decodes to a path under "/admin". Without ownership, the
   * ones that no route of the "/admin" router matches fall through to a root
   * route, which serves them without the guard: "/admin%2Fsecret" (whose
   * decoded path is "/admin/secret") reaches "/{a}", and "/admin/a/b"
   * reaches "/{a}/{b}/{c}". Non-vacuous: those targets answer 200 from a
   * root route when the owning router lets them fall through. */
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
  /* With the guard satisfied, a path under "/admin" that no route of the
   * owner matches is a 404 from the owner, and a method that its routes do
   * not accept is a 405 whose Allow names the owner's methods alone; a root
   * CHTTP_ANY route never serves either. */
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
  /* "/admin/deep" is the more specific mount, so it owns every path under
   * it: its own routes answer, its own 404 answers the rest, and neither the
   * "/admin" guard nor the "/{a}/{b}" route of "/admin" sees such a
   * request. A path beside it stays with "/admin". Non-vacuous: with a fall
   * through to "/admin", "/admin/deep/zz" reaches "/{a}/{b}" there and is
   * guarded. */
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
  /* Only a path under a mount is owned. "/administrator" and "/ADMIN" share
   * no segment boundary with "/admin", and ";" is not a separator. */
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

/* Reads one response of known body size from fd at about rate bytes a
 * second, and returns the count of body bytes that arrived before the end
 * of the stream or timeout_ms, or -1 when the head did not arrive. */
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
  /* A reader that takes a large response steadily, far above the rate
   * floor, drains the send queue continuously, yet the socket becomes
   * writable again only once about a third of a send buffer that the kernel
   * autotunes to several MB has drained. With a gap limit shorter than that
   * drain, the wake-up alone judges such a reader stalled and cuts the
   * response. The send queue shrinking is the progress that counts.
   * Non-vacuous: judging a parked response by its wake-ups alone cuts this
   * response after about 4 MB. */
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

/* ========================================================================== */
/*                    REQUEST CONFORMANCE                                     */
/* ========================================================================== */

/* Sends a whole request to g_srv on a fresh connection and returns the
 * status of the first response, or -1. The response bytes land in buf. */
static int _conf_request(const char *req, char *buf, size_t cap) {
  _raw_request_to_test_server(req, buf, cap);
  if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
  return atoi(buf + 9);
}

TEST(conformance, a_major_version_other_than_1_gets_505) {
  /* RFC 9110 SS15.6.6. Non-vacuous: without the version check, HTTP/2.0
   * and HTTP/3.0 are served as HTTP/1.1 and answer 200, and HTTP/0.9 as
   * HTTP/1.0. */
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
  /* The public constant for the status that the server sends for a major
   * version other than 1. */
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
  /* RFC 9112 SS3.2 and RFC 3986 SS3.2.2. Non-vacuous: a Host check that
   * refuses only an empty value and whitespace serves every one of these. */
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
   * request. Non-vacuous: honouring it writes an interim "100 Continue" in
   * front of the response. */
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
  /* RFC 9112 SS6.1: the server decodes no coding but chunked, so a request
   * that applies another one would hand the handler an encoded body.
   * Non-vacuous: without the check, "gzip, chunked" reaches the handler and
   * answers 200. A list that does not end in chunked stays a 400, since its
   * length cannot be determined (RFC 9112 SS6.3). */
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

/* Builds a chunked POST to /echo-body of n one-byte chunks, each with the
 * chunk extension ext. The caller frees the result. */
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
  /* The server discards chunk extensions, so no limit on the body counts
   * them. Non-vacuous: without the bound, 200 one-byte chunks with a
   * 200-byte extension each (40 KB of framing for 200 bytes of data) answer
   * 200. A short extension on every chunk stays accepted. */
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

/* A response whose one header line carries value_len bytes of value. */
static char *_spill_response(size_t value_len) {
  char *m = malloc(value_len + 128);
  if (!m) return NULL;
  size_t n = (size_t)sprintf(m, "HTTP/1.1 200 OK\r\nX-Big: ");
  memset(m + n, 'v', value_len);
  n += value_len;
  n += (size_t)sprintf(m + n, "\r\nContent-Length: 0\r\n\r\n");
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
  /* A header line longer than the 8 KiB line buffer is refused, unless the
   * response parser enabled the spill; then it moves to one heap buffer
   * from the procs of the caller, and chttp1_parser_release() gives it back.
   * The line is fed in small pieces, so that it crosses the spill point in
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
  /* The spill buffer holds 64 KiB, and the total header cap bounds the
   * line too: a line past either is refused, and nothing leaks. A request
   * parser never spills. */
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

  /* A budget that can hold a longer line spills, and refuses the line once
   * it passes the budget. A budget that cannot never spills. */
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
  /* The bound on chunk framing is a property of the parser, so a client
   * reading a response gets it as well. */
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

extern size_t _chttpsvr_linger_count_for_tests(void);

/* The counter of lingering closes moves on the thread that closes, which
   can run just after the client already saw the end of the stream, so a
   test waits for it, for 5 s at most. */
static bool _linger_count_passes(size_t before) {
  for (int i = 0; i < 5000; i++) {
    if (_chttpsvr_linger_count_for_tests() > before) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}
extern size_t _chttpsvr_listener_max_accepts_per_dispatch_for_tests(void);
extern void _chttpsvr_listener_max_accepts_per_dispatch_reset_for_tests(void);

/* Starts a server with one route, GET and POST /lc, on host and port. */
static chttpsvr _lc_server(const char *host, int port,
                           const chttpsvr_config_t *base) {
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  if (connect(fd, sa, salen) == 0 &&
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

TEST(listener_host, a_null_or_empty_host_listens_on_ipv4_and_ipv6) {
  /* A NULL or empty host is one dual-stack listener: IPv4 clients reach it
     through v4-mapped addresses, and IPv6 clients directly. Non-vacuous:
     a listener on 0.0.0.0 refuses the IPv6 connection. */
  bool v6 = _lc_have_ipv6_loopback();
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
     answers on 127.0.0.1 and, where the host has IPv6, on ::1. The IPv6
     half is skipped on a host without IPv6. Non-vacuous: a default of
     "0.0.0.0" refuses the IPv6 connection. */
  bool v6 = _lc_have_ipv6_loopback();
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  bool host_is_null = cfg.host == NULL;
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
  bool started = false;
  if (srv != CHTTPSVR_INVALID) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    cfg.port = (uint16_t)(_LC_PORT + 14);
    started = chttpsvr_start(srv, &cfg) == ccol_success;
  }
  int v4 = started ? _lc_get_v4(_LC_PORT + 14) : -2;
  int v6_status = !v6 ? 200 : (started ? _lc_get_v6(_LC_PORT + 14) : -2);
  if (!v6) fprintf(stderr, "SKIP: no IPv6 loopback; ::1 not checked\n");
  if (srv != CHTTPSVR_INVALID) chttpsvr_destroy(srv);
  REQUIRE_TRUE(host_is_null);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6_status, 200);
}

TEST(listener_host, localhost_listens_on_the_ipv4_loopback) {
  /* A host name listens on its first IPv4 address when it has one, so
     "localhost" answers on 127.0.0.1 whatever order the resolver gives.
     Non-vacuous where the resolver puts ::1 first for "localhost", as it
     does on most Linux systems: the first address of the resolver is
     ::1, and 127.0.0.1 is then refused. */
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
  /* A start never deletes a file that is not a socket. Non-vacuous: an
     unconditional unlink before the bind removes the file and starts. */
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_file_%d", (int)getpid());
  unlink(path);
  FILE *f = fopen(path, "w");
  if (f) {
    fputs("precious data\n", f);
    fclose(f);
  }
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* A second server on the path of a live one fails to start, and the first
     keeps both its socket file and its clients. Non-vacuous: an
     unconditional unlink before the bind takes the path over, so the first
     server loses every new client. */
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_live_%d", (int)getpid());
  unlink(path);
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr first = _lc_server(host, 0, NULL);
  bool first_started = first != CHTTPSVR_INVALID;
  chttpsvr second = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* The socket file of a running server is replaced by somebody else's
     file. The stop of the server must remove only its own socket, so the
     replacement survives. Non-vacuous: an unlink of the path by name
     deletes the replacement. */
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
  /* A server started on a relative path removes that socket file on its
     stop even after the application changed its working directory, and it
     touches nothing in the new one. Non-vacuous: a cleanup that resolves
     the relative path again looks in the new directory, finds nothing of
     its own there, and leaves its socket behind. */
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
  /* A file of the same name in the new directory, which is not ours. */
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

/* Sends the head of a POST to path with a body of declared bytes, and then
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
   ended: 0 for an orderly end of stream, the errno of a failed read, or -1
   for a timeout. */
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
  /* The client sends the head of a large upload to a route that does not
     exist, with part of the body. The server refuses it with 404 and never
     reads the body. The client must read the whole response and then an
     orderly end of stream, not a reset. Non-vacuous: a close with the
     unread body bytes in the socket makes the kernel send a reset, which the
     read after the response reports as ECONNRESET. */
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
  /* Once the client closes, the linger ends at once. */
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
  /* A client that neither closes nor stops sending holds a lingering
     connection for a bounded time only. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 6, NULL);
  /* 1. A client that goes quiet and keeps its end open: the sweep closes
     the linger at its deadline. */
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

  /* 2. A client that keeps sending after the response, without end: the
     linger discards what it sends until its deadline, and then closes, so
     the writes of the client fail within a few seconds. */
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
  /* The deadline is 2 s, and the sweep runs once a second. */
  REQUIRE_LT(q_ms, 4500LL);
  REQUIRE_EQ(l_status, 404);
  REQUIRE_TRUE(write_failed);
  REQUIRE_LT(l_ms, 8000LL);
  REQUIRE_TRUE(l_closed);
}

TEST(lingering_close, a_request_with_no_body_closes_at_once) {
  /* A refused request whose framing has no body, with nothing more in the
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
  /* A streaming handler answers 401 without reading the body of an upload.
     The worker closes the connection after that response, and it must be
     the same orderly end as for a refusal by the server. Non-vacuous: a
     plain close with the unread body in the socket sends a reset. */
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* 120 connections wait in the backlog while the one reactor thread is
     held. Once it is released, each dispatch of the listener accepts at
     most 64 of them before it returns to the reactor, and every one of
     them is still served. 120 is more than one batch and fits the default
     backlog ceiling of the BSDs and macOS (kern.ipc.somaxconn, 128), which
     resets a connection beyond it. Non-vacuous: an unbounded accept loop
     takes all 120 in one dispatch. */
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
  /* "OPTIONS *" requests arrive while the application adds sub-routers,
     which grows the array of routers under its lock. The OPTIONS * path
     must not read that array without the lock. ThreadSanitizer reports the
     unlocked read in the test_tsan build; the ordinary build checks that
     both sides keep working. */
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
  /* With max_body_size 0, a Content-Length can declare almost SIZE_MAX
     bytes. Such a request goes ahead under the progress rule, and its
     charge must stay within the limit instead of wrapping the sum of the
     charges. Non-vacuous: charging the declared length raises the count
     far above the limit. */
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
  /* An ordinary request is still served normally afterwards. */
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

/* An allocator that fails the nth allocation of one size after it is
   armed, and passes every other call through. */
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

/* Sends two pipelined GETs in one write, where the second one is exactly
   second_len bytes, and reports what the client sees: the status of the
   first response, the status of the second one (-1 when none came), and
   how the stream ended after the first one when no second one came. The
   third allocation of second_len bytes fails: the carry-over of the
   divert, the copy of the stream, and then the reclaim of the leftover. */
static void _lc_pipeline_with_lost_leftover(int port, bool park_write,
                                            int *first, int *second, int *end) {
  ccol_memmgmt_procs_t mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = ccol_create_chttpsvr_mp(&mp, g_test_logger, NULL);
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
        *end = -1; /* nothing within 3 s: the server waits for a request */
      }
    }
    close(fd);
  }
  _chttpsvr_force_short_response_write_for_tests(0);
  atomic_store(&g_lc_fail_size, 0);
  chttpsvr_destroy(srv);
}

TEST(chttpserver, a_lost_copy_of_pipelined_bytes_closes_the_connection) {
  /* The reclaim of the pipelined bytes of a next request fails for memory.
     Those bytes are gone, so the connection must close after the first
     response instead of waiting for a request that its client already
     sent. Non-vacuous: a connection kept alive answers nothing within 3 s.
     The second pass parks the first response on a full socket, which
     reclaims the bytes on another path. */
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

extern size_t _chttpsvr_idle_count_for_tests(chttpsvr h);
extern size_t _chttpsvr_retired_drained_for_tests(void);
extern void _chttpsvr_fail_next_servers_register_for_tests(void);

static size_t _rx_idle(chttpsvr h, void *arg) {
  (void)arg;
  return _chttpsvr_idle_count_for_tests(h);
}

#define _RX_IDLE_N 300
#define _RX_PARKED_N 3

TEST(sweep, one_tick_closes_every_expired_idle_and_parked_connection) {
  /* 300 connections sit idle past the idle timeout and three bodies sit
     parked past their gap limit. One tick of the sweep must close every
     idle one and answer every parked one with 408. Non-vacuous: a sweep
     that takes a fixed batch per tick closes 64 of the idle ones and never
     reaches the parked list on that tick; the background sweep adds at most
     one more batch in the window of this test. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.idle_timeout_us = 300000;
  cfg.read_timeout_us = 300000;
  chttpsvr srv = _sc_server(_LC_PORT + 15, &cfg);
  int idle_fds[_RX_IDLE_N];
  int parked_fds[_RX_PARKED_N];
  for (int i = 0; i < _RX_IDLE_N; i++) idle_fds[i] = -1;
  for (int i = 0; i < _RX_PARKED_N; i++) parked_fds[i] = -1;
  bool opened = srv != CHTTPSVR_INVALID;
  for (int i = 0; opened && i < _RX_PARKED_N; i++) {
    parked_fds[i] =
        _sc_open_partial_post(_LC_PORT + 15, "/len", 1000, 10, NULL);
    opened = parked_fds[i] >= 0;
  }
  opened =
      opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, _RX_PARKED_N, 10000);
  for (int i = 0; opened && i < _RX_IDLE_N; i++) {
    idle_fds[i] = _sc_connect(_LC_PORT + 15);
    opened = idle_fds[i] >= 0;
  }
  opened = opened && _sc_wait_for(srv, _rx_idle, NULL, _RX_IDLE_N, 20000);
  size_t idle_after = SIZE_MAX, parked_after = SIZE_MAX;
  if (opened) {
    _sc_nap_ms(400); /* past the idle timeout on the real clock */
    _chttpsvr_advance_slow_clock_for_tests(60000); /* past the body gap */
    _chttpsvr_sweep_now_for_tests(srv);
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
  /* A handler of the previous run holds its worker when the server is
     stopped and started again. The restart must return at once and listen
     at once, the running request must still complete with its own
     response, and the pool that ran it must drain in the background. A
     destroy while that request still runs waits for it. Non-vacuous: a
     restart that drains the previous pools first waits for the handler,
     which here ends only when the test opens its gate after the restart,
     or on its own 10 s bound. */
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
  /* A destroy now must wait for the request that still runs on the retired
     pool, and not free the server under it. */
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
  /* The registry of servers cannot grow: the start fails cleanly, nothing
     listens, and a later start succeeds. Non-vacuous: a registration
     failure that the start ignores reports success for a server that the
     sweep never reaches. */
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
     127.0.0.1, so the start must fail, and nothing may listen on ::1 in
     its place. Non-vacuous where localhost also resolves to ::1: a start
     that goes on to the next address listens there and reports success. */
  int other = socket(AF_INET, SOCK_STREAM, 0);
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)(_LC_PORT + 18));
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  bool held = other >= 0 &&
              bind(other, (struct sockaddr *)&a, sizeof(a)) == 0 &&
              listen(other, 4) == 0;
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
  ccol_retval_t rv = ccol_success;
  if (held && srv != CHTTPSVR_INVALID) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "localhost";
    cfg.port = (uint16_t)(_LC_PORT + 18);
    rv = chttpsvr_start(srv, &cfg);
  }
  int v6 = _lc_have_ipv6_loopback() ? _lc_get_v6(_LC_PORT + 18) : -1;
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
  /* With ipv6_only, "localhost" listens on ::1 and not on 127.0.0.1.
     Skipped on a host where localhost has no IPv6 address or ::1 cannot
     be bound. Non-vacuous: a host name that always takes its IPv4 address
     first listens on 127.0.0.1 and refuses the IPv6 client. */
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
  /* "[127.0.0.1]" and "[::1]" listen on the address inside the brackets,
     and an unclosed bracket fails the start. Non-vacuous: the resolver
     refuses a bracketed address. */
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
  /* The engine log names the reason that the resolver gave. A name inside
     brackets is refused by the resolver itself, with no lookup, which keeps
     the name-service modules of the host out of the test. Non-vacuous: a
     failure that logs only the errno of the start, which the resolver does
     not set, carries no reason at all. */
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  int gai = getaddrinfo("a..b", "80", &hints, &res);
  if (res) freeaddrinfo(res);
  int pfd[2] = {-1, -1};
  bool piped = gai != 0 && pipe(pfd) == 0;
  if (piped) fcntl(pfd[0], F_SETFL, O_NONBLOCK);
  clog plog = piped ? clog_open_fd(pfd[1], CLOG_INFO, NULL) : CLOG_INVALID;
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
  /* The key of the request is decoded and the argument is taken as it is,
     as url.Values of Go does: "a b" finds a+b and a%20b, "a%20b" finds
     nothing, and "x&y" finds x%26y. */
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* A parked body that the sweep answers with 408 lingers, and its
     reservation of body memory goes back when the linger starts, and not
     when it ends: once the connection lingers, nothing is reserved, and it
     still lingers. The linger drops the reservation a few steps before it
     parks, so the test waits for the linger and only then looks at the
     memory. Non-vacuous: a reservation held until the close is still held
     while the connection lingers. */
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
  /* The client sends the whole 4 MiB body of a refused upload before it
     reads the response, as many clients do. The linger must take every
     byte, and the client then reads the 404 and an orderly end of stream.
     Non-vacuous: a linger that stops after a fixed byte count closes under
     the client, whose write then fails with a reset. */
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

/* ----- a lost push-back of pipelined bytes closes the connection ----- */

/* Sends the head of a POST whose 5-byte body arrives later, in one write
   together with a pipelined GET of exactly req2 bytes, and reports what the
   client sees, as _lc_pipeline_with_lost_leftover does. The first allocation
   of the size of the GET, which is the push-back of those bytes by the read
   of the body, fails. */
static void _rx_pipeline_after_body(int port, const char *path, int *first,
                                    int *second, int *end) {
  ccol_memmgmt_procs_t mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = ccol_create_chttpsvr_mp(&mp, g_test_logger, NULL);
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
    if (_sc_send(fd, head, (size_t)hn)) {
      _sc_nap_ms(300); /* the head is read and diverted alone */
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
  /* The read of a body takes the start of a pipelined next request off the
     socket, and keeping those bytes fails for memory. They are gone, so the
     connection must close after the response instead of waiting for a
     request that its client already sent, on a buffered route and on a
     streaming route. Non-vacuous: a connection kept alive answers nothing
     within 3 s. */
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
  /* A blocking read with a 300 ms timeout on a socket that never gets data,
     on a thread that a signal interrupts every 20 ms. The read must time
     out after about 300 ms. Non-vacuous: a wait that starts its whole
     timeout again after each signal never times out while the signals
     arrive, which here is 3 s. */
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

extern void _chttpsvr_arm_divert_submit_hook_for_tests(int nth);
extern bool _chttpsvr_divert_submit_hook_entered_for_tests(void);
extern void _chttpsvr_release_divert_submit_hook_for_tests(void);
extern size_t _chttpsvr_submit_retried_for_tests(void);
extern size_t _chttpsvr_retired_drain_started_for_tests(void);

TEST(restart, a_request_whose_submit_meets_a_retired_pool_is_still_served) {
  /* Two pipelined requests: the worker that runs the first one reads the
     worker pool for the second one, and is held there while the server is
     stopped and started again, which retires that pool and starts its
     drain. The held submit then meets a pool in shutdown. The second
     request must go to the pool of the new run and get 200. Non-vacuous: a
     submit that takes the refusal of the retired pool as the answer gives
     503; the counter of moved submits proves the window was reached. */
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 29, NULL);
  bool started = srv != CHTTPSVR_INVALID;
  size_t drains_before = _chttpsvr_retired_drain_started_for_tests();
  size_t retried_before = _chttpsvr_submit_retried_for_tests();
  int fd = started ? _sc_connect(_LC_PORT + 29) : -1;
  bool entered = false, draining = false;
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
      /* The drain thread shuts the pool down right after it starts. */
      _sc_nap_ms(200);
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
  REQUIRE_EQ(first, 200);
  REQUIRE_EQ(second, 200);
  REQUIRE_EQ(retried, (size_t)1);
}

extern int _chttpsvr_take_stream_pool_threads_for_tests(void);
extern void _chttpsvr_arm_start_hold_for_tests(void);
extern bool _chttpsvr_start_hold_entered_for_tests(void);
extern void _chttpsvr_release_start_hold_for_tests(void);

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
  /* A keep-alive connection of the first run, which has one streaming
     thread, sends the first streaming request of the second run, which asks
     for three, while that start is held right after it swapped in the pools
     of the second run. The streaming pool that the request creates must
     have three threads. Non-vacuous: a start that stores its streaming
     settings after the swap lets that request create the pool with the one
     thread of the first run. */
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

/* ----- what a waiting connection keeps of its request ----- */

#define _FX_PORT (TEST_PORT + 260)

extern size_t _chttpsvr_parked_request_state_for_tests(chttpsvr h);

/* Reads the body and answers with _SC_BIG_RESPONSE bytes. */
static void _fx_body_then_big_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_big_handler(req, resp, ctx);
}

static chttpsvr _fx_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* Three clients each upload 1 MiB, get a response far larger than the
     socket buffers, and read none of it. The responses park on a full
     socket. Nothing reads the bodies, the targets or the header fields of
     those requests again, so none of the parked connections may keep them:
     a parked connection holds no thread, so the number of them is not
     bounded by anything that the memory limit of the bodies covers.
     Non-vacuous: a parked response that keeps its request holds the 1 MiB
     body buffer, the header fields and the target, and the state count is
     then above 3 MiB. */
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
  /* A refused upload lingers. The lingering connection must keep nothing of
     the request that it refused. Non-vacuous: a linger that keeps its
     request holds the target and the header fields. */
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

/* ----- body buffers hold no more than their reservation ----- */

extern size_t _chttpsvr_body_overshoot_for_tests(void);
extern void _chttpsvr_body_overshoot_reset_for_tests(void);

TEST(body_memory, a_framed_body_is_held_in_exactly_its_reservation) {
  /* A body of 70000 declared bytes reserves 70000 bytes before its first
     byte is read. Its buffer must never hold more than that: no slack past
     the declared length and no second block during a copy. The client stops
     4000 bytes short, so the body parks with its buffer in place, and then
     sends the rest. Non-vacuous: a buffer that doubles from 8 KiB holds
     65536 + 131072 bytes during its last growth, 126608 above the
     reservation. */
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
     growth, its reservation of body memory must cover the old block and the
     new one together, which both live until the copy ends. Non-vacuous: a
     body that reserves 64 KiB at a time while its buffer doubles holds
     131072 + 262144 bytes against a reservation of 196608. */
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
    /* A pause now and then makes the server read the body in several
       parts, across several growths of its buffer. */
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

extern void _chttpsvr_force_interim_park_after_for_tests(size_t n);
extern void _chttpsvr_force_short_response_write_for_tests(size_t n);
extern size_t _chttpsvr_park_kind_count_for_tests(int kind);
extern void _chttpsvr_set_internal_write_ceiling_ms_for_tests(unsigned ms);

#define _FX_BIG_REJECT ((size_t)8 * 1024 * 1024)

/* Answers every request under /big-reject itself, with a 404 far larger
   than the socket buffers, whose bytes ctx holds, and passes every other
   request on. It runs for a rejected request, on reject_pool. */
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
  /* Two clients get a 404 whose body a middleware made far larger than the
     socket buffers, and read none of it. The rejection must park with the
     unsent rest, holding no thread of reject_pool, and must then arrive whole
     once the client reads, followed by the end of the stream. Non-vacuous: a
     rejection that waits on the socket holds a thread of reject_pool until
     its per-write timeout or its total ceiling gives up, so no rejection
     ever parks and the clients never get the whole body. The ceiling of
     the writes of the library is raised for this test, which is about the
     park and not about how long a rejection may take: two such bodies cross
     a small receive window in more than 2 s under valgrind. */
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* A client that never reads holds a parked rejection only until the
     ceiling of the internal writes of the library (2 s), even with
     max_response_write_duration turned off. Non-vacuous: a parked rejection
     with no deadline stays parked. */
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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
  /* Past the ceiling, one tick of the sweep closes it. */
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
  /* A bare 404 whose write stops after 10 bytes on a full socket parks and
     then goes out whole, followed by the end of the stream. This is the
     write of every rejection, whichever thread runs it. Non-vacuous: a
     rejection written with waiting writes never takes the forced stop, and
     the count of parked writes does not move. */
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
  /* The "100 Continue" line of a buffered route stops after 5 bytes on a
     full socket. The connection parks with the rest of the line and holds no
     thread; once the line is out, the body is read and the request ends
     normally. Non-vacuous: an interim line written with waiting writes never
     takes the forced stop, and the count of parked writes does not move. */
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
  /* Over plaintext no client certificate exists, and every identity
     accessor says so: no DER, no subject, ccol_key_not_found and a digest
     buffer left all zeroes. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(g_test_logger, NULL);
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

extern void _chttpsvr_force_reject_inline_for_tests(bool force);

TEST(internal_writes, an_inline_rejection_on_the_reactor_parks_and_completes) {
  /* The inline answer that a rejection gets when reject_pool cannot take
     it runs on the reactor thread for a route that does not match. Its
     write stops after 10 bytes on a full socket; it must park, so that the
     reactor never waits on the peer, and then go out whole, followed by the
     end of the stream. An ordinary request on another connection is served
     meanwhile. Non-vacuous: a rejection written with waiting writes never
     takes the forced stop, and the count of parked writes does not move. */
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

extern void _chttpsvr_force_pause_fail_for_tests(bool force);
extern size_t _chttpsvr_pause_fallback_count_for_tests(void);

#define _PF_PORT (TEST_PORT + 280)

/* Each test of this group makes every pause with which a thread takes a
   connection from the reactor fail. The connection then loses its
   registration, and its next wait (a parked write, a parked body, a
   lingering close or the next request of a keep-alive connection) adds a
   fresh registration for the same descriptor while the on_removed of the
   old one can still be pending. Each test checks that the path really ran,
   through the count of those removals. */

/* Connects to port and waits, for 5 s at most, until the server registered
   the connection with its reactor and holds it idle, so that its first
   request arrives through a registration and not through the first read
   that follows the accept. */
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
     registration and then goes out whole, followed by the end of the
     stream. Non-vacuous: a fallback that keeps the removed registration
     cannot arm the parked write, and the response stops after 10 bytes. */
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
  /* A refused upload lingers on a fresh registration, so the client reads
     the 404 and then an orderly end of the stream. Non-vacuous: a fallback
     that keeps the removed registration cannot arm the linger, and the
     connection closes with the body unread, which the client sees as a
     reset. */
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
  /* A keep-alive connection serves a request, then a refused one, on fresh
     registrations, and a second connection is served beside it.
     Non-vacuous: a fallback that keeps the removed registration makes the
     keep-alive tail fail its modify and close the connection, so the
     second request gets no answer. */
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
  /* A body that stops half way parks, resumes on readiness, loses its
     registration again as the worker takes it, and completes; the
     connection then serves its next request. Non-vacuous: a fallback that
     keeps the removed registration makes the park or the keep-alive tail
     fail and close the connection. */
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

extern size_t _chttpsvr_mem_holders_for_tests(chttpsvr h, bool *exempt_out);
extern void _chttpsvr_req_body_memory_for_tests(chttpsvr_req *req,
                                                size_t *charged, size_t *held);

#define _BM_PORT (TEST_PORT + 300)

/* What _bm_probe_handler saw while it ran: the reservation of its own
   request, the capacity of its body buffer, and the reservations of the
   whole server. */
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

/* Reads the body, then holds its thread on the gate of the slow-client
   tests. */
static void _bm_hold_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_gate_stream_handler(req, resp, ctx);
}

static chttpsvr _bm_server(int port, const chttpsvr_config_t *extra) {
  chttpsvr srv = ccol_create_chttpsvr(g_test_logger, NULL);
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

/* Sends the head of a POST to path with "Expect: 100-continue", either with
   a Content-Length of len or chunked, waits for the interim line, and then
   sends a body of len bytes. The body therefore never arrives with the
   head, and its memory is reserved before the server reads it. It returns
   the final status, with the response in buf, or -1. */
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

/* No reservation, no holder and no exemption remain. */
static bool _bm_nothing_reserved(chttpsvr srv) {
  if (!_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000)) return false;
  bool exempt = true;
  size_t holders = _chttpsvr_mem_holders_for_tests(srv, &exempt);
  return holders == 0 && !exempt;
}

TEST(body_memory, a_running_handler_holds_exactly_the_memory_of_its_body) {
  /* While the handler of a buffered route runs, its body is still held, and
     the reservation of body memory covers exactly the buffer that holds it,
     for a body that a Content-Length frames and for a chunked one. Once the
     response is out, nothing stays reserved. Non-vacuous: a reservation that
     goes back when the body completes leaves the handler with a reservation
     of 0 beside a body buffer of tens of kilobytes. */
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
  /* The handler of a first request holds a body of 90000 bytes against a
     limit of 100000 and does not return. A second body of 50000 bytes then
     waits for memory: it gets no "100 Continue" and no worker reads it.
     Once the handler returns and its body is freed, the second body is
     admitted, invited, read and answered. Non-vacuous: a reservation that
     goes back when the first body completes admits the second one at once,
     while the first body is still held. */
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
  /* The handler of a runs, with its body read whole. */
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
  /* Every way in which a body that holds a reservation of body memory can
     end gives the whole reservation back, and leaves no holder and no
     exemption behind: a client that leaves in the middle of its body, a
     rate floor that answers 408, a chunked body over max_body_size (413), a
     malformed chunk (400), a client that leaves while the handler holds its
     body, and a chunked body whose wait for more memory ends in 503 while it
     already holds part of its reservation. Non-vacuous: a release that does
     not run on one of these paths leaves that reservation counted, and the
     check after that path fails. */
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
  /* A body of 40000 bytes holds part of a limit of 65536. A chunked body
     then reserves the first growths of its buffer, needs more than the rest
     of the limit, and waits for memory while it holds part of it. The sweep
     answers it with 503 at the deadline of the wait, and only the
     reservation of the first body remains. Non-vacuous: a 503 path that does
     not give back the part that the waiter holds leaves it counted beside
     the 40000 bytes. */
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 65536;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  /* the 31 s below must not expire the first body */
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _bm_server(_BM_PORT + 3, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  int h _ccol_destructor(_close_scoped_fd) =
      _sc_open_partial_post(_BM_PORT + 3, "/len", 40000, 100, NULL);
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
  while (_chttpsvr_mem_in_use_for_tests(srv) != 40000 &&
         _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  size_t in_use_after = _chttpsvr_mem_in_use_for_tests(srv);
  size_t holders_after = _chttpsvr_mem_holders_for_tests(srv, NULL);
  int h_status = -1;
  if (parked && _sc_send_fill(h, 40000 - 100, 'h'))
    h_status = _sc_read_response(h, buf, sizeof(buf), NULL, 5000);
  bool none = _bm_nothing_reserved(srv);
  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(c_sent);
  REQUIRE_TRUE(waits);
  REQUIRE_GT(in_use_waiting, (size_t)40000);
  REQUIRE_EQ(status, 503);
  REQUIRE_EQ(in_use_after, (size_t)40000);
  REQUIRE_EQ(holders_after, (size_t)1);
  REQUIRE_EQ(h_status, 200);
  REQUIRE_TRUE(none);
}
