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

/* pthread_timedjoin_np (a glibc extension) is what makes _bounded_join below
 * a bounded join instead of a blind pthread_join: without it, a background
 * thread genuinely, permanently stuck by a real regression in the exact
 * mechanism a race-hook test is checking would make the join itself hang
 * forever too, taking the whole binary down instead of failing that one test
 * cleanly. Must be defined before the first #include that could pull in
 * <pthread.h> transitively; mirrors tests_engine_stop.c's own identical
 * placement and rationale, and src/chttpserver.c's/src/clogger.c's. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                         GLOBAL TEST SERVER STATE                           */
/* ========================================================================== */

#define TEST_PORT 18765
#define BASE_URL "http://127.0.0.1:18765"

static clog g_test_logger = CLOG_INVALID;
static chttpsvr g_srv = CHTTPSVR_INVALID;

/* Scope-exit cleanup for a plain client-side test socket fd, used via
   `int fd _ccol_destructor(_close_scoped_fd) = socket(...);` where a test's
   own later REQUIRE_* checks could otherwise return early (before the test's
   own final, explicit close(fd)) and leak the fd for the remainder of this
   binary's run. A negative fd (socket()/connect() itself failed) is silently
   tolerated, matching close(2)'s own EBADF-is-harmless-to-ignore-here
   convention already used elsewhere in this file. */
static void _close_scoped_fd(int *fd) {
  if (fd && *fd >= 0) close(*fd);
}

/* Bounded join for a background thread that a test has just released from a
   white-box race-test hook (_chttpsvr_release_*_race_hook_for_tests()):
   mirrors tests_engine_stop.c's own _fx_timed_join exactly (same mechanism,
   same 30s bound, same rationale). A regression that leaves the released
   thread genuinely stuck in the exact mechanism under test must fail this
   one test's own REQUIRE_TRUE below instead of hanging this whole binary in
   an unbounded pthread_join; 30s is comfortably beyond every real join in
   this file's own race-hook tests (each hook release is expected to unblock
   its thread within microseconds), so this never fires spuriously against a
   correctly-behaving thread. Returns true iff tid actually terminated and
   was joined; the caller must pthread_detach(tid) on a false return so the
   still-running thread is not leaked as permanently joinable. */
static bool _bounded_join(pthread_t tid, void **retval) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 30;
  return pthread_timedjoin_np(tid, retval, &deadline) == 0;
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

/* Writes ctx (a literal string owned by the caller) verbatim; used by the
   per-router segment-decode cache regression tests below to tell which of
   several routes sharing a common decoded prefix actually matched. */
static void _cache_literal_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

/* Writes "<ctx>:<id param>"; used by the per-router segment-decode cache
   regression tests below to prove a param captured at a position several
   OTHER, non-matching candidate routes already tried (and failed on, at a
   LATER segment) still reads back correctly for the route that actually
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

/* Streaming handler that counts how many separate chttpsvr_req_read() calls
   returned data before EOF, reporting the count via a response header.
   Used to prove the body is delivered in genuinely separate batches as it
   arrives on the wire, rather than being fully buffered ahead of the
   handler and handed over as one blob. */
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

/* Streaming handler that drains the body and reports why the final
   chttpsvr_req_read() call returned -1 (if it did), via a response header.
   Used to test stream_read_timeout_ms and mid-stream abort reporting. */
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

/* Buffered handler that echoes the "X-Trailer" request header (present only
   as a chunked body's RFC 7230 SS4.1.2 trailer field, never as a regular
   header) back via a response header; used to pin down chttpsvr_req_header's
   own documented trailer-visibility contract for a BUFFERED route: since the
   whole body (trailers included) is always read before a buffered handler
   ever runs, the trailer field must already be visible from the very first
   statement here. */
static void _buffered_trailer_echo_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-value", val ? val : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

/* Streaming handler that reports whether "X-Trailer" (again, a chunked
   body's trailer field, not a regular header) is visible via
   chttpsvr_req_header BEFORE the body has been drained to EOF, and again
   AFTER; used to pin down chttpsvr_req_header's own documented
   trailer-visibility contract for a STREAMING route, where (unlike the
   buffered case above) the handler runs concurrently with body parsing, so
   the trailer only becomes visible once chttpsvr_req_read has actually
   returned 0. */
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

/* Writes a large (multi-megabyte) response body, well beyond any socket
   send buffer, so actually sending it to a client that never reads
   necessarily blocks chttp1_stream_write mid-send; used to exercise
   max_response_write_duration_ms, the write-side analogue of
   max_body_read_duration_ms. */
/* 16 MiB: deliberately far larger than any combination of the server's own
   explicit 128 KiB SO_SNDBUF floor (_apply_accepted_socket_options) and the
   test client's own default (OS-auto-tuned) receive buffer/TCP window on
   any reasonably-configured system (especially since a client that never
   reads at all (see max_response_write_duration_exceeded_closes_connection
   below) gives the kernel's receive-buffer auto-tuning no read pattern to
   grow the window from in the first place, so the effective ceiling stays
   close to the small, un-tuned default) so that actually sending this
   body to such a client is guaranteed to genuinely block chttp1_stream_write
   on backpressure well before the whole thing fits in flight, not merely
   appear to send instantly because it happened to fit in some generous
   buffer pipeline end to end. */
#define _CHTTPSVR_TEST_LARGE_BODY_SIZE (16 * 1024 * 1024)

static void _large_response_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  /* A plain static array (BSS), not a heap allocation: a one-time malloc()
     never freed for the rest of the process's life would show up as a real
     "still reachable" block under make memtest's --errors-for-leak-kinds=all,
     which a BSS array never does. */
  static char big_buf[_CHTTPSVR_TEST_LARGE_BODY_SIZE];
  static bool filled = false;
  if (!filled) {
    memset(big_buf, 'x', sizeof(big_buf));
    filled = true;
  }
  chttpsvr_resp_write(resp, big_buf, sizeof(big_buf));
}

/* Streaming handler that deliberately never calls chttpsvr_req_read():
   always rejects immediately, exercising the case Expect: 100-continue
   (RFC 7231 SS5.1.1) exists for; a server that decides to reject a
   request without ever wanting its body. */
static void _stream_reject_without_reading_handler(chttpsvr_req *req,
                                                   chttpsvr_resp *resp,
                                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "no thanks");
}

/* Streaming handler that reads exactly one small batch of the body and then
   returns without draining the rest; used to prove that a connection
   whose body is left partially unread by the handler is still safely usable
   for a subsequent request (http1_stream_release must discard, not stash,
   the unread remainder). */
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

/* Handler that exercises chttpsvr_resp_printf: a mix of scalar types on a
   short call (fits the internal stack buffer) followed by a long call whose
   formatted output exceeds that stack buffer, forcing the heap fallback
   path. Both calls must accumulate into the same body, exactly like
   chttpsvr_resp_write_str. */
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

/* Streaming handler that accesses the body via chttpsvr_req_body() (not
   chttpsvr_req_read()).  Streaming routes read their body live off the
   socket via chttpsvr_req_read(); chttpsvr_req_body() is documented as
   buffered-route-only and must return NULL/0 here rather than the body a
   handler never asked to have read. */
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

/* Streaming handler that reads via chttpsvr_req_read() FIRST (draining part
   of the body off the socket, which populates conn->body's own internal
   cursor/growbuf state), then calls chttpsvr_req_body(); exercising the
   post-read case _stream_body_check_handler above cannot reach at all,
   since that handler never calls chttpsvr_req_read() and so conn->body is
   still genuinely empty by the time it checks. chttpsvr_req_body() must
   still report NULL/0 here too, not conn->body's own leftover internal
   accounting state (which, without the is_streaming guard, would include
   already-delivered bytes still sitting in the buffer alongside a length
   that reflects internal bookkeeping rather than either the true total or
   the true remaining unread count). */
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
   With the fix, this must return 0 (no-op), not -1 (error). */
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

/* Handler that sets an invalid (out-of-range) status code; the server must
   clamp it to 500 (see _send_response's own [100,999] range check) rather
   than writing the raw out-of-range value onto the wire. */
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

/* Always writes a non-empty body, then overwrites the status code to
   whatever numeric value the "x-force-status" request header names (200 if
   absent/unparseable). Used to verify _send_response suppresses the body
   (and, for a 1xx/204, the auto Content-Length too) for statuses that must
   never carry one regardless of what the handler itself already wrote. */
static void _forced_status_with_body_handler(chttpsvr_req *req,
                                             chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, "this-body-must-never-reach-the-wire");
  const char *forced = chttpsvr_req_header(req, "x-force-status");
  chttpsvr_resp_set_status(resp, forced ? atoi(forced) : 200);
}

/* Sets an explicit, caller-supplied "Connection" response header (via the
   "x-force-connection" request header) that may have nothing to do with
   whatever this server's own framing logic actually decides afterward.
   Used to verify that a handler can no longer make the wire Connection
   header disagree with the server's real post-response keep-alive/close
   behavior (see _send_response's own doc comment on this). */
static void _explicit_connection_header_handler(chttpsvr_req *req,
                                                chttpsvr_resp *resp,
                                                void *ctx) {
  (void)ctx;
  const char *forced = chttpsvr_req_header(req, "x-force-connection");
  if (forced) chttpsvr_resp_set_header(resp, "Connection", forced);
  chttpsvr_resp_write_str(resp, "explicit-connection-header-ok");
}

/* Sets enough response headers, each with a large value, that the combined
   header block comfortably exceeds a few KiB. Regression test for a real bug
   in _send_response: it used to assemble the header block into a fixed
   4096-byte stack buffer with no fallback, so a response whose headers alone
   crossed that size silently lost its ENTIRE response (not even a graceful
   500; the connection was simply closed with zero bytes ever written, and
   nothing logged). _send_response now assembles the header block into a
   buffer that grows (heap-allocated, doubling) as needed, so this must
   succeed with every header intact regardless of size. */
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

/* Handler used as the SECOND registration of the same path+method to verify
   that duplicate registrations are silently accepted but only the FIRST handler
   ever runs (first-wins policy). */
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
  /* A lone CR with no paired LF: strpbrk(x, "\r\n") must catch a single
   * disallowed byte on its own, not just the combined "\r\n" pair the
   * resp_set_header_crlf_injection_rejected test above already covers. */
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
  /* A space inside the name: contains no CR/LF of its own, so the
   * pre-existing CRLF-only check would have let this through, but it is
   * not a genuine RFC 7230 tchar-only token; "X Foo: bar" as a name would
   * put "X Foo:bar:baz\r\n" on the wire, not a real two-field split. */
  results[0] = (int)chttpsvr_resp_set_header(resp, "X Foo", "bar");
  /* A literal colon inside the name, for the same reason. */
  results[1] = (int)chttpsvr_resp_set_header(resp, "X:Foo", "bar");
  /* A byte that is neither CR/LF nor a printable-but-non-tchar character:
   * a bare NUL cannot occur (the name is a NUL-terminated C string), so
   * this uses a DEL byte (0x7F), also outside the tchar set. */
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

/* Reports the value counts for keys "a", "b", and "" (in that order,
   comma-separated); used to verify that a stray/consecutive '&' in the raw
   query string is skipped outright rather than parsed as a spurious
   key=""/value="" entry. */
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

/* Streaming GET handler that accesses the body via chttpsvr_req_body().
   When a GET carries no body the function must return (NULL, len=0). */
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

/* Pair of handlers used by the root-router-shadowing test.
   Both are registered to /shadow-test/ping: the root handler is registered
   directly on g_srv (always checked first) and the sub-router handler is
   registered on a /shadow-test sub-router (checked second, never reached). */
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

/* Dedicated server with a bounded pool (1 thread, queue_capacity=1) started
   in _setup to verify that a full ctpool causes ctpool_try_submit to return
   ccol_container_full and the server responds 503.  Listening on TEST_PORT+2.
*/
static chttpsvr g_bounded_srv = CHTTPSVR_INVALID;

/* Dedicated server with a small max_body_size (64 bytes) for boundary tests
   of the 413/PAYLOAD_TOO_LARGE enforcement (buffered and streaming).
   Listening on TEST_PORT+3. */
static chttpsvr g_small_body_srv = CHTTPSVR_INVALID;
#define SMALL_BODY_MAX 64

/* Mutex/condvar for synchronising the blocking handler used in the 503 test.
   The test fires two concurrent HTTP requests that block inside
   _bounded_blk_handler, filling the 1-thread pool and the 1-slot queue, then
   sends a third request which must receive 503. */
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

/* Thread entry: send one GET /bounded-503 to g_bounded_srv and discard the
   response.  Used to fill pool slots from background threads. */
static void *_send_bounded_req(void *arg) {
  (void)arg;
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bounded-503", TEST_PORT + 2);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
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

  /* Destroy each server: drains in-flight requests and releases this
   * server's single shared-engine reference. chttpserver's own shared
   * event_loop reactor is stopped asynchronously (a joinable reaper thread,
   * fully independent of chttpclient's own engine) rather than being joined
   * inline by the last destroy, so chttpsvr_engine_wait() below is required
   * to deterministically block until it has actually finished before this
   * function (an atexit handler) returns; otherwise the engine-installed
   * logger (g_test_logger, routed via chttpsvr_set_engine_logger in
   * _setup) could still be in use by a reactor thread when this function
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
  g_srv = create_chttpsvr(g_test_logger, &err);
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

  /* Multi-method routes: same path registered for both GET and POST to verify
     that both are routable independently (was broken before the _find_route
     fix; a POST would get 405 because the GET route matched the path first
     and the search stopped there). */
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
  /* Streaming route that reads the body via chttpsvr_req_body() (not req_read).
   */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-body-api",
                                      _stream_body_check_handler, NULL);
  /* Streaming route exercising chttpsvr_req_body() AFTER chttpsvr_req_read()
   * has already been used on it; see _stream_body_after_read_handler's own
   * doc comment. */
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST,
                                      "/stream-body-after-read",
                                      _stream_body_after_read_handler, NULL);
  /* Streaming GET route that calls chttpsvr_req_body() on a request with no
   * body; exercises the (body && len > 0) else branch and must return
   * "(empty)" rather than crashing or producing undefined output. */
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

  /* Routes for tests that previously registered routes mid-test.  Registering
   * them here guarantees each route exists exactly once for the lifetime of the
   * process and avoids cross-test contamination from duplicate registrations.
   */
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

  /* Routes for root_router_shadows_subrouter_at_same_path test.
   * Registered here (not in the test body) so they exist exactly once for the
   * lifetime of the process and the test does not mutate g_srv at runtime. */
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
  g_srv2 = create_chttpsvr(g_test_logger, NULL);

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
  g_bounded_srv = create_chttpsvr(g_test_logger, NULL);
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
    /* Short enough that a client which stops sending mid-body triggers a
     * timeout quickly in tests, without affecting /bounded-503 (which never
     * calls chttpsvr_req_read). */
    bcfg.stream_read_timeout_ms = 300;
    ccol_retval_t brv = chttpsvr_start(g_bounded_srv, &bcfg);
    if (brv != ccol_success) {
      fprintf(stderr, "FATAL: bounded chttpsvr_start failed: %d\n", brv);
      exit(1);
    }
  }
  /* No readiness poll needed: http_listen binds the socket synchronously on
   * a subsequent chttpsvr_start (engine already running). */

  /* Create and start the small-max_body_size server for the
   * max_body_size/413 boundary tests (both buffered and streaming). */
  g_small_body_srv = create_chttpsvr(g_test_logger, NULL);
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
  /* A '+' in a URL path segment must survive as a literal '+' in the captured
   * parameter value.  Path-segment encoding rules (RFC 3986) do not assign
   * special meaning to '+'; only application/x-www-form-urlencoded (query
   * strings) maps '+' to space.  The previous implementation used
   * http_decode_url_unsafe (query-string semantics) for segment matching and
   * parameter capture, causing '+' to become ' ' and producing values
   * inconsistent with chttpsvr_req_path.  After the fix both decode via
   * http_decode_path_unsafe. */
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
  /* "%61" decodes to 'a'; the request path's prefix portion is
     percent-encoded but must still match the /api/v1 sub-router exactly
     like the unencoded request does, since a root-level registration of the
     same effective pattern would match it (route-pattern segments are
     already decoded before comparison; the sub-router prefix must be
     equally decode-aware, not a raw byte-for-byte comparison). */
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
  /* A second server on a different port must start successfully and serve
   * requests independently, demonstrating multiple-server support.
   *
   * g_srv2 is a long-lived, shared fixture handle (reused by several other
   * tests in this file), not one this test locally owns, so it cannot be
   * RAII-scoped here. Every outcome below is instead captured into a local
   * and chttpsvr_stop(g_srv2) is called unconditionally before any
   * REQUIRE_* that could return early, so a genuine regression in this
   * test's own subject (starting/serving a second concurrent server) cannot
   * leave g_srv2's listener running for every later test in this binary to
   * potentially trip over. */
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
  /* Route patterns whose {name} segment contains characters outside
   * [A-Za-z0-9_] must be rejected at registration time with ccol_invalid_args.
   * Such names can never be retrieved via chttpsvr_req_param and would create
   * silently unreachable parameters.  g_srv2 (never started) is used so the
   * running test server is not polluted. */
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
  /* A pattern reusing the same {name} more than once (e.g. /a/{id}/b/{id})
   * must be rejected at registration time with ccol_invalid_args: without
   * this check the route would compile successfully and then silently make
   * the SECOND occurrence's captured value unreachable, since
   * chttpsvr_req_param always returns on the first name match. g_srv2 (never
   * started) is used so the running test server is not polluted. */
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
  /* GET /dual must reach the GET handler even though POST /dual is also
     registered.  Before the _find_route fix this would have worked only by
     accident of registration order; now correctness is guaranteed regardless
     of order. */
  chttpcli_response *resp = _get("/dual");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_method_post) {
  /* POST /dual must reach the POST handler even though GET /dual was
     registered first.  Before the _find_route fix the GET route matched the
     path, the method check failed, the search stopped, and 405 was returned
     instead of dispatching to the POST handler. */
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

/* ========================================================================== */
/*                    DOUBLE-SLASH PATTERN REJECTION TEST                     */
/* ========================================================================== */

TEST(chttpserver, double_slash_pattern_rejected) {
  /* A route pattern containing consecutive slashes (e.g. /foo//bar) must be
     rejected at registration time with ccol_invalid_args.  Such patterns were
     previously silently normalised to /foo/bar, which is unexpected and
     error-prone.  Uses g_srv2 which is never started. */
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
  /* chttpsvr_resp_printf must format like printf and accumulate across
     calls, including a call whose formatted output is long enough to force
     the heap fallback path inside chttpsvr_resp_printf. */
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
  /* A %00-encoded NUL byte inside a path segment must not let strcmp-based
   * literal segment matching silently treat "hello" + trailing garbage as
   * an exact match for the registered literal segment "hello": /hello%00xyz
   * must NOT match the literal route /hello.  Before this was fixed,
   * _match_route_cached's own literal-segment comparison decoded
   * "hello%00xyz" to "hello\0xyz" and compared it against "hello" via
   * strcmp, which stops at the first NUL in either operand and reported a
   * match; letting an attacker-chosen suffix hide behind a route match a
   * caller reasonably expects to reflect the whole segment. */
  chttpcli_response *resp = _get("/hello%00xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_param_segment_is_route_mismatch) {
  /* Same class of bug, exercised through a {param} segment instead of a
   * literal one: _seg_cache_get must treat a decoded embedded NUL as a
   * decode failure (the same bucket a malformed %XX escape already falls
   * into), not silently hand a truncated value to a would-be match. */
  chttpcli_response *resp = _get("/echo-path/hello%00world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_query_key_does_not_alias) {
  /* Same class of bug in the query-string decoder: a key "q%00x" must not
   * decode-then-strcmp its way into aliasing the unrelated key "q". Before
   * the fix, _parse_qparams stored the fully-decoded "q\0x" and
   * chttpsvr_req_query_one's strcmp(qp->keys[i], "q") stopped at the
   * embedded NUL and reported a spurious match. */
  chttpcli_response *resp = _get("/query-one?q%00x=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query) {
  /* Calling chttpsvr_req_query for two different keys in one request must
     return correct values for both, even though the scratch _qresult array
     is shared and reused between calls. */
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

/* Counting pass-through allocator so create_chttpsvr_mp can be exercised and
 * the test can prove the supplied procs (not the library default allocator)
 * actually handled every allocation/free, not just that create/destroy
 * happened to succeed. */
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
  /* create_chttpsvr_mp must succeed with a custom allocator, accept route
     registrations, and release all memory through the same allocator on
     destroy.  Previously bug #1 caused UB in the mutex_init failure path;
     the happy-path test here exercises the same m_procs copying code.
     Counting the wrapper calls (rather than using bare passthroughs) proves
     create_chttpsvr_mp actually threaded the supplied procs through to every
     allocation site instead of silently falling back to the library
     default. */
  g_wrap_malloc_count = 0;
  g_wrap_free_count = 0;
  g_wrap_calloc_count = 0;
  g_wrap_realloc_count = 0;

  ccol_memmgmt_procs_t mp = {_wrap_malloc, _wrap_free, _wrap_calloc,
                             _wrap_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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
  /* Every allocation this test drove (create, three route registrations)
     must have been released through the same wrapper by the time destroy
     returns; a silently-ignored custom allocator would leave
     g_wrap_free_count at 0 here. */
  REQUIRE_GT(g_wrap_free_count, (size_t)0);
  REQUIRE_GE(g_wrap_free_count, allocs_before_destroy);
}

/* ========================================================================== */
/*                         ADDITIONAL COVERAGE TESTS (NEW)                    */
/* ========================================================================== */

TEST(chttpserver, subrouter_root_no_trailing_slash) {
  /* /api/v1 (without trailing slash) must match the "/" route registered on
     the /api/v1 sub-router.  _find_route normalises the empty sub-path to "/"
     before matching, so both /api/v1 and /api/v1/ should reach
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

/* g_reject_task_run_count_for_tests's own increment happens on reject_pool's
   thread AFTER the rejection response has already been written to the
   client and the connection closed (see _reject_task's own comment on why
   the in_flight_requests release, and by extension this counter bump, comes
   last); so a client that has just finished reading its response can, in
   principle, observe control back in the test before the server-side
   counter bump has actually run yet, a benign scheduling race rather than
   any ordering the library itself promises. Polls for the expected delta
   instead of asserting immediately, matching this codebase's own
   established pattern for exactly this class of async-completion timing
   (see tests_mem_mgmt.c's g_mm_free_count wait loop). Returns the observed
   count so the caller can still assert on it. */
static size_t _wait_for_reject_pool_task_count(size_t expected) {
  for (int attempt = 0; attempt < 50; attempt++) {
    if (_chttpsvr_reject_pool_task_count_for_tests() >= expected) break;
    usleep(20000);
  }
  return _chttpsvr_reject_pool_task_count_for_tests();
}

TEST(chttpserver, bounded_pool_full_returns_503) {
  /* When the server's ctpool is at capacity, ctpool_try_submit returns
   * ccol_container_full and the server must respond 503.
   *
   * g_bounded_srv has 1 worker thread and queue_capacity=1 (total capacity 2:
   * 1 active + 1 queued).  We fire two background HTTP requests that block
   * inside _bounded_blk_handler to saturate both slots, then send a third
   * request which must receive 503.
   *
   * This also confirms (via g_reject_task_run_count_for_tests's own
   * white-box counter) that the 503 was actually carried out on a
   * reject_pool thread rather than the synchronous last-resort fallback;
   * a black-box client has no way to distinguish the two, since both
   * produce an identical response on the wire. The counter is process-wide,
   * not per-server, but tau runs one test at a time on this thread and
   * nothing else in this process rejects a connection on its own (the idle
   * sweep and other servers' handlers do not), so a plain before/after
   * delta across this test's own window is unambiguous.
   *
   * Every intermediate outcome below is captured into a local instead of
   * asserted on immediately with REQUIRE_*, and every background thread is
   * released and joined UNCONDITIONALLY before any REQUIRE_* runs at all:
   * g_bounded_srv has only ONE worker thread, so a REQUIRE_* returning early
   * from this function anywhere before that release would leave that single
   * worker permanently blocked inside _bounded_blk_handler for the rest of
   * this process's life (nothing else in this test binary unblocks it before
   * process exit), wedging every later test that happens to hit
   * g_bounded_srv behind a worker that can never make progress. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();

  /* Reset state from any previous run of this test. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  g_blk_count = 0;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Request 1: fills the active worker slot. */
  pthread_t t1;
  bool t1_created = pthread_create(&t1, NULL, _send_bounded_req, NULL) == 0;

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
  size_t reject_count_after = reject_count_before;
  if (worker_blocked && t2_created) {
    char burl[128];
    snprintf(burl, sizeof(burl), "http://127.0.0.1:%d/bounded-503",
             TEST_PORT + 2);
    chttpcli_response *resp = NULL;
    chttp_get(burl, &resp);
    if (resp) {
      got_resp = true;
      status = resp->status_code;
      chttpclient_resp_free(resp);
    }
    reject_count_after =
        _wait_for_reject_pool_task_count(reject_count_before + 1);
  }

  /* Release blocking handlers and join every successfully-created background
   * thread before any REQUIRE_* below runs; see this test's own opening
   * comment for why this ordering is load-bearing, not merely tidy. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  if (t1_created) pthread_join(t1, NULL);
  if (t2_created) pthread_join(t2, NULL);

  /* Reset g_blk_go for potential reruns (e.g. _teardown safety broadcast). */
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
  REQUIRE_EQ(reject_count_after, reject_count_before + 1);
}

TEST(chttpserver, stop_on_unstarted_server_is_safe) {
  /* chttpsvr_stop on a server that is not currently started (started==false,
   * whether because it was never started at all, or because an earlier
   * chttpsvr_stop() call already put it in that state) must be a safe no-op
   * that can be called multiple times without crashing. g_srv2 is not
   * started by _setup itself, but by this point in the suite's own
   * execution order it has already been started and stopped once by
   * multi_server_start_stop; either way, started==false here, which is the
   * one property this test actually depends on. */
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_INVALID);
  chttpsvr_stop(g_srv2); /* must not block or crash */
  chttpsvr_stop(g_srv2); /* second call: must also be a no-op */
}

/* ========================================================================== */
/*                         RAW SOCKET HELPER                                  */
/* ========================================================================== */

/* Send a hand-crafted HTTP/1.1 request over a raw TCP socket and return the
   HTTP status code (or -1 on socket error).  extra_headers must already
   include the trailing \r\n for each header line, or be NULL.  The full raw
   response (status line + headers + body) is written into buf[0..buf_sz-1].
   Used for scenarios that the chttpclient abstraction cannot express, such as
   sending the same header name twice to exercise duplicate-header handling
   (chttpsvr_req_header's documented "last occurrence wins" behavior). */
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
  /* Bounds the read loop below in case a regression makes the server never
     respond and never close (e.g. it tried to read a declared-but-never-sent
     body before rejecting an unmatched route; exactly the class of bug
     unmatched_route_rejected_without_reading_body exists to catch): without
     this, that read(2) call would block forever instead of this helper's
     caller failing its own assertion cleanly. Every caller of this shared
     helper benefits, not just the one that originally motivated it. */
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

/* Connects, sends a request whose body is written in several delayed
   chunks (forcing the server to observe separate incremental reads off the
   socket rather than one blob that already arrived in a single recv), then
   reads the full response. Returns the HTTP status code, or -1 on a
   socket-level failure. Always closes after one response. */
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
  /* See _raw_request's own identical comment: without this, the final read
     loop below would block forever instead of the caller's own assertion
     failing cleanly, hanging the whole binary rather than reporting one
     test as failed, if a regression makes the server never respond. */
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

/* Connects, sends headers declaring a Content-Length far larger than the
   bytes actually written, writes only `sent_len` of the body, then closes
   the socket immediately without waiting for (or reading) any response;
   simulating a client that aborts mid-upload. Returns 0 on a successful
   connect+write, -1 on a socket-level failure; the caller has no response
   to inspect since the connection was torn down deliberately. */
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

/* Same connect/send-partial-body shape as _raw_request_abort_mid_body above,
   but half-closes the write side (shutdown(fd, SHUT_WR)) instead of closing
   the whole socket, then reads back whatever response the server sends
   before closing. Unlike a full close, the server observes a clean
   read()/ctls_conn_read() EOF mid-body (chttpsvr_req_read()'s "n == 0 before
   the message finished framing" truncation path) rather than a connection
   reset, and the caller gets to inspect the response instead of racing a
   torn-down connection. Returns 0 on a successful connect+write+read
   (buf receives the raw response, always NUL-terminated), -1 on a
   socket-level failure. */
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
  /* See _raw_request's own identical comment: without this, the final read
     loop below would block forever instead of the caller's own assertion
     failing cleanly, hanging the whole binary rather than reporting one
     test as failed, if a regression makes the server never respond. */
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

/* Reads from fd until EOF (or buf is exhausted, whichever comes first) and
   NUL-terminates the result; used by tests that drain an entire raw HTTP
   response (or several pipelined ones) off a socket the server is expected
   to close on its own once done. Bounds every underlying read(2) call via
   SO_RCVTIMEO, mirroring _raw_request's/_read_one_http_response's own
   identical guard: without it, a regression that makes the server never
   respond and never close would block this call forever instead of the
   caller's own assertion failing cleanly, hanging the whole binary rather
   than reporting one test as failed. Returns the total number of bytes
   read (never including the trailing NUL). Does not close fd. */
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

/* Reads exactly one HTTP/1.1 response (headers + Content-Length body) off
   an already-connected socket, leaving the connection open for a
   subsequent request; used to test keep-alive across two requests on one
   connection. Assumes a short, non-chunked response (true for every
   fixture handler used with this helper). Returns the status code, or -1
   on failure.

   Bounds every read(2) call below: without this, a regression that makes
   the server never respond and never close would block this call forever
   instead of this helper's caller failing its own assertion cleanly,
   hanging the whole binary rather than reporting one test as failed.
   Mirrors _raw_request's own identical SO_RCVTIMEO guard above; every
   caller of this shared helper (used at every one of this file's
   keep-alive/Unix-socket/max_connections/self-restart tests) benefits, not
   just the one that originally motivated it. A no-op for a caller that
   already set its own SO_RCVTIMEO on fd (setsockopt simply overwrites the
   same option with an identical value). */
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

/* Extract and decode the response body from the raw response written by
   _raw_request.  Handles both Content-Length and Transfer-Encoding: chunked
   responses.  Decodes the body in-place inside buf; returns a pointer to the
   decoded body (NUL-terminated) or NULL if the header/body separator is absent.

   With Content-Length the bytes after \r\n\r\n are already the literal body.
   chttpserver always sets Content-Length explicitly and never chunk-encodes
   its own responses (see _send_response's own doc comment in
   chttpserver.c), so the chunked branch below is defensive rather than a
   currently exercised path for this server's responses; kept so this
   helper stays correct if that ever changes, and so it can decode the
   chunked *request* bodies this file's own tests send (see e.g.
   chunked_body_with_trailer_headers_handled_once) without needing a
   second, near-duplicate helper. */
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
  /* chttpsvr_req_body() is buffered-route-only: a streaming route's body is
     never pre-extracted (it's read live via chttpsvr_req_read(), batch by
     batch, off the socket), so calling chttpsvr_req_body() on one must
     return NULL/0 rather than silently handing back a buffered copy. */
  const char *payload = "body-via-api";
  chttpcli_response *resp = _post("/stream-body-api", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_after_req_read_on_streaming_route) {
  /* A real, previously-unguarded gap: chttpsvr_req_body() had no
     is_streaming check at all, so it always returned conn->body.buf/.len
     verbatim; for a streaming route, conn->body is a live cursor
     chttpsvr_req_read() drains, only lazily compacted back to empty on the
     NEXT batch's arrival, not immediately once fully drained. The previous
     test above only ever calls chttpsvr_req_body() BEFORE any
     chttpsvr_req_read() call, so it passed for the coincidental reason that
     conn->body just happens to still be all-zero at that point, not because
     of any real streaming-route enforcement. This test calls
     chttpsvr_req_read() first, so a regression that dropped the guard would
     surface here as a non-NULL body/nonzero len (whatever chttpsvr_req_read
     had already delivered, still sitting in the buffer) instead of the
     documented NULL/0. */
  const char *payload = "12345678-more-than-eight-bytes";
  chttpcli_response *resp =
      _post("/stream-body-after-read", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  /* chttpsvr_req_read() with an 8-byte buffer against a 31-byte payload
     must have actually read something real (n > 0), confirming this test
     genuinely exercises the after-a-real-read case, not a bodyless one. */
  REQUIRE_TRUE(strncmp(resp->body, "read=0 ", 7) != 0);
  REQUIRE_TRUE(strstr(resp->body, "body=null len=0") != NULL);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_repeated_header) {
  /* When a client sends the same header name twice, _on_header appends both
     occurrences, in arrival order, to conn->hdr_names/hdr_values (see that
     struct's own doc comment); chttpsvr_req_header must scan backward and
     return the LAST occurrence. The chttpclient abstraction overwrites
     duplicate header names, so we use a raw socket to send the literal
     duplicate lines. */
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
  /* A client that writes its body in several delayed chunks must cause the
     streaming handler's chttpsvr_req_read() to observe more than one batch
     ; proving the body is read live off the socket by the worker thread
     as it arrives, not pre-buffered whole before the handler starts. */
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
  /* Routing now happens at headers-complete time, before any body byte is
     read; an unmatched route must be rejected immediately even though
     the client claims (but never sends) a huge body. If the server tried
     to read the body before responding, this would hang instead of
     returning promptly. */
  size_t reject_count_before = _chttpsvr_reject_pool_task_count_for_tests();

  char buf[4096] = {0};
  int status = _raw_request("POST", "/no-such-route-at-all",
                            "Content-Length: 100000000\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 404);

  /* This rejection is also routed through reject_pool, whose own counter
     bump lands strictly after the response is already on the wire (see
     _wait_for_reject_pool_task_count's own comment). Waiting for it here
     stops that straggler increment from leaking into whichever test tau
     runs next, which previously could observe one more than it expected
     if this test returned before the async bump landed. */
  _wait_for_reject_pool_task_count(reject_count_before + 1);
}

TEST(chttpserver, unmatched_route_rejection_routed_through_reject_pool) {
  /* Every rejection is now routed through reject_pool, not just the
     pool-full 503 case (see bounded_pool_full_returns_503's own assertion
     on that); so a slow-reading client being told about a bad route can
     no longer stall the sole reactor thread either. A black-box client
     cannot distinguish "answered via reject_pool" from "answered via the
     synchronous last-resort fallback" (both produce an identical 404 on the
     wire), so this checks the mechanism directly via
     g_reject_task_run_count_for_tests's own white-box counter. */
  size_t before = _chttpsvr_reject_pool_task_count_for_tests();

  chttpcli_response *resp = _get("/definitely-not-a-registered-route-xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);

  REQUIRE_EQ(_wait_for_reject_pool_task_count(before + 1), before + 1);
}

TEST(chttpserver, concurrent_route_rejections_do_not_starve_other_requests) {
  /* Previously, every unmatched-route rejection (404/405/500) ran
     synchronously on chttpserver's own sole reactor thread; a burst of many
     such rejections could delay dispatch for every other, unrelated
     connection queued behind them on that same thread; g_srv and
     g_bounded_srv share one process-wide reactor, so this is not a
     hypothetical concern. Now that every rejection is routed through
     reject_pool (a small dedicated pool, sized from worker_thread_count; see
     that pool's own comment in chttpserver.c), a burst of concurrent
     rejections must not meaningfully delay an ordinary, unrelated request
     served concurrently. A courtesy rejection response is
     too small to reliably force the old code's actual blocking write to
     manifest via socket-buffer starvation (unlike
     response_write_timeout_closes_slow_reader_connection's large body), so
     this cannot tightly prove non-blocking the way that test does; it is a
     smoke/regression check against gross reactor-thread starvation, and it
     exercises reject_pool's own multi-threaded submission/dispatch path
     under real concurrency (unlike bounded_pool_full_returns_503's single
     503, which never contends with anything else). */
  /* Every intermediate outcome below is captured into a local instead of
   * asserted on immediately with REQUIRE_*, and every successfully-created
   * background thread is joined UNCONDITIONALLY before any REQUIRE_* runs:
   * these threads each perform a full HTTP round-trip against the
   * process-lifetime g_srv, so a REQUIRE_* returning early from this
   * function while any of them is still in flight would leave it running
   * unjoined for the rest of the binary's life, continuing to bump the
   * process-wide reject-pool task counter several OTHER tests in this file
   * rely on for exact-delta assertions. */
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
  /* _on_headers_complete rejecting a route (conn->req_rejected set, e.g. the
     404 case here) always results in _conn_close in _conn_pump's CHTTP1_USER
     branch, unconditionally, rather than the connection being kept alive to
     read a next request; see that branch's own comment on why a rejected
     route gets an error response but still closes rather than staying
     keep-alive. This locks in that guarantee end to end: if a second,
     pipelined request's bytes are already sitting in the same read buffer
     right behind the rejected one, they must never be misread as that
     first, already-finished request's body/a second request on the same
     connection - exactly one response comes back, and the connection closes
     cleanly rather than producing a spurious second response or hanging. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* Both requests concatenated into one write() call so they are guaranteed
     to arrive together in the same read buffer server-side: the first hits
     an unmatched route (rejected without ever diverting), the second hits
     a matched one and would misparse as its "body" if the bug were still
     present. */
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

  /* Exactly one response (the 404 for the rejected first request); the
     connection must close on its own right after, without ever touching
     the second request's bytes as this connection's body/next message. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_across_two_requests_on_one_connection) {
  /* Two matched requests sent back to back on the same connection (no
     Connection: close) must both succeed; verifies that pausing at
     headers-complete time (instead of after the full body, as before)
     does not break normal keep-alive/pipelining. */
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
  /* Regression test for a real bug: a bodyless request (GET, here) that
     completes its own framing immediately after headers (no Content-Length,
     no chunked Transfer-Encoding) never calls _drain_body's own
     chttp1_stream_read loop at all, since chttp1_parser_message_complete()
     is already true the instant the worker starts. Before this was fixed,
     whatever chttp1_stream_prepare() was given as leftover (here: this
     connection's ENTIRE second, pipelined request, already off the wire and
     gone from the kernel's socket buffer for good) sat untouched in the
     stream's own carry-over and was silently discarded the moment
     chttp1_stream_release() ran, even though the connection was correctly
     being kept alive; the second request's bytes vanished forever and the
     client hung waiting for a response that would never come. Both requests
     are concatenated into a single write() call so they are guaranteed to
     land together in the reactor's own read buffer server-side, exactly
     the shape that triggered the bug. */
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

  /* Exactly two full responses, both 200, both carrying the handler's body;
     the connection closes on its own right after the second (Connection:
     close), rather than the client having to time out waiting on a second
     response that never arrives. */
  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

TEST(chttpserver, pipelined_bytes_after_buffered_body_request_not_lost) {
  /* Same regression as pipelined_bodyless_requests_both_answered, but for
     the OTHER half of the same underlying bug: a request that DOES have a
     body (so _drain_body's own chttp1_stream_read/chttp1_parser_execute
     loop genuinely runs) can still lose a pipelined next request if that
     loop's own read happens to pull in bytes past this message's body
     boundary; chttp1_parser_execute reports back exactly how many of the
     bytes it was given were actually consumed (chttp1_parser_consumed()),
     but the trailing remainder (here: the second request's own raw bytes,
     swept up in the very same chttp1_stream_read call that returned the
     first request's 5-byte body) was previously never captured anywhere and
     vanished right along with the rest of that read buffer. */
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
  /* Streaming-route counterpart: chttpsvr_req_read() drives the identical
     chttp1_stream_read/chttp1_parser_execute loop shape _drain_body uses
     for a buffered route, so it needs (and, with this fix, has) the same
     push-back-then-reclaim treatment; without it, a pipelined next request
     riding in on the same read as a streamed body's own trailing bytes
     would be lost exactly the same way. */
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
  /* The three pipelining tests above (rejected-route, bodyless x2, buffered-
     body, streaming-body) each concatenate exactly TWO requests into one
     write(), which only ever exercises _conn_start_diverted/_conn_feed_bytes
     being driven ONCE per connection, from the reactor thread. With a
     THIRD request's bytes already sitting behind the second one (all
     delivered together in a single read), _task_worker's own keep-alive
     tail (see its own comment on chttp1_stream_take_leftover) must feed
     those bytes into _conn_feed_bytes a SECOND time, on the worker thread
     rather than the reactor thread; if that second request's own headers
     complete right there (as they do here), _conn_start_diverted runs a
     second time too, recursively, from inside _task_worker itself,
     re-submitting to the very same worker pool for a fourth request's
     bytes, and so on. This is the one code path in the pipelining fix that
     was never exercised by any two-requests-only test: it locks in that
     an arbitrarily long chain of pipelined requests, all delivered in one
     socket read, is fully answered with none dropped, corrupted, or
     duplicated, regardless of how many times that recursive hand-off has
     to happen. */
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

  /* Exactly _FIVE_PIPELINED_COUNT complete, correctly-bodied responses; no
     fewer (a dropped request), no more (a duplicated/corrupted parse), and
     the connection closes on its own after the last one. */
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
  /* Regression test for a real use-after-free in _task_worker's own
     keep-alive tail: when a further pipelined request's bytes are fed into
     _conn_feed_bytes from a worker thread (rather than the reactor thread;
     see _conn_feed_bytes's own doc comment and this exact code path's
     comment in _task_worker), _conn_feed_bytes may fully resolve conn's
     fate (including freeing it outright) before returning, per its own "the
     caller must not touch conn again" contract. A syntactically malformed
     follow-up request (chttp1_parser_execute returning CHTTP1_ERROR, e.g.
     the negative Content-Length case in negative_content_length_rejected
     above) takes _conn_feed_bytes's synchronous, same-thread _conn_close
     branch, freeing conn deterministically before _conn_feed_bytes returns;
     not merely racily, unlike the cross-thread-divert case. A prior
     version of this code read conn->m_procs immediately afterward to free
     a local scratch buffer, a guaranteed use-after-free on this exact path.
     Both requests are concatenated into one write() so the first (matched,
     kept-alive) request is diverted to a worker thread from the reactor's
     own initial read, and the second (malformed) request's bytes ride
     along as leftover, reaching _conn_feed_bytes only once the worker
     thread's own tail runs; the reactor thread never touches the second
     request's bytes at all. */
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

  /* Exactly one response (200 for the first, well-formed request); the
     malformed second request gets no response at all (matching
     negative_content_length_rejected's own documented behavior) and the
     connection closes on its own right after, rather than crashing the
     server process. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_two_consecutive_streaming_requests) {
  /* Per-message ingestion state (conn->body, body_too_large,
     transfer_aborted, deadline_exceeded, _carry_over, and conn->parser
     itself) is reset by _conn_reset_for_request() between every request on
     a keep-alive connection, since chttpsvr_conn_t is reused across the
     connection's whole lifetime. Every other keep-alive test in this file
     pairs at most one streaming request with a buffered GET on the same
     connection; this sends two full streaming POSTs back to back on one
     connection, the scenario most likely to expose stale per-message
     streaming state (e.g. a leftover body buffer or error flag from
     request 1 corrupting request 2's ingestion). Each request has a
     different body so the two responses can only match if the ingestion
     state was genuinely reset in between. */
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
  /* /stream-read-once reads only the first 8 bytes of the body and returns
     without calling chttpsvr_req_read() again. Here the whole 64-byte body
     is written in one shot and arrives on the wire before the handler's
     single read call runs, so chttpsvr_req_read's own internal
     chttp1_stream_read + chttp1_parser_execute pass (see that function's
     loop in chttpserver.c) ends up parsing the ENTIRE declared body into
     conn->body in that one internal call regardless of how few bytes the
     handler's own buffer captured; Content-Length is fully consumed, so
     chttp1_parser_message_complete(&conn->parser) is already true before
     the handler even returns. _task_worker only forces Connection: close
     when that message-complete check is still false at the end of the
     request (see the paired ..._with_undrained_body_forces_close test
     below for that case); here it is true, so the connection must remain
     usable for a second, unrelated request. */
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
  /* Unlike the fully-arrived-body case above, here the client only ever
     sends the first 8 bytes of a declared 64-byte body; the exact amount
     /stream-read-once's single chttpsvr_req_read(req, buf, 8) call
     consumes. content_length(64) > read(8) when the handler returns, so
     the body is genuinely undrained: http1_stream_release's own safety net
     (see http1.c) must force Connection: close, since the remaining
     (never-sent) 56 bytes could otherwise be misread as the start of a new
     pipelined request if this connection were reused. */
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
  /* A client that sends headers declaring more body than it ever delivers,
     then stalls, must eventually cause chttpsvr_req_read() to return -1
     with chttpsvr_req_stream_error() == ccol_timed_out (bounded by
     stream_read_timeout_ms, set to 300ms for this server in test setup);
     rather than blocking the worker thread forever. */
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
  /* A client that sends part of its body then disconnects entirely must
     not hang the worker thread or leak the connection's resources. There
     is no response to check (the client tore the connection down), so the
     test instead proves the specific worker that handled the aborted
     request actually returned to the pool.
     Uses a dedicated, local, single-worker server (not g_srv, which has 4
     worker threads) rather than sharing g_bounded_srv (also single-worker,
     used elsewhere in this file for the bounded-503 test): with 4 workers
     available on g_srv, a follow-up request would still succeed promptly
     via one of the other 3 workers even if the exact bug this test exists
     to catch were reintroduced (the worker that served the aborted
     connection wedged forever), making such a check pass vacuously; and
     g_bounded_srv's own queue_capacity=1 is deliberately sized to be
     exactly exhausted by that OTHER test's own two concurrent blocking
     requests, leaving no slack for this test's own follow-up request to
     race the server's own not-yet-complete abort-cleanup without an
     occasional, spurious 503 (observed in practice under valgrind, where
     genuine concurrency is heavily serialized and a worker's own wakeup can
     be delayed well past the moment a queue-capacity check runs). A
     generous queue_capacity here (this test's only two connections are ever
     going to use it) removes that capacity race entirely while keeping the
     one property that makes the test meaningful: with only one worker
     thread, the follow-up below can only succeed if that same worker
     recovered from the abrupt disconnect and returned to the pool. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
  /* Short enough that the test does not need to wait long for the abrupt
     disconnect to be noticed via this fallback path if the fast (EOF-
     driven) detection is ever delayed. */
  cfg.stream_read_timeout_ms = 300;
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
  /* Bounded, not indefinite: if the worker genuinely never recovers, this
     read times out and the REQUIRE below fails cleanly instead of hanging
     the whole test binary. */
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
  /* chttpsvr_req_stream_error() is documented to return
     ccol_http_transfer_aborted for "connection closed or malformed
     framing" mid-body, but chttpsvr_req_read()'s truncated-body and
     hard-I/O-error exit paths never actually recorded that on the
     connection: every such case silently fell through to ccol_success,
     telling a well-behaved streaming handler that a truncated upload was a
     clean read. Confirmed via a standalone repro against the built library
     before fixing: POST Content-Length: 100, send 20 bytes, half-close;
     the handler observed chttpsvr_req_read() return -1 but
     chttpsvr_req_stream_error() report ccol_success and the server replied
     200 OK. Unlike client_disconnect_mid_body_does_not_hang_server above
     (a full close, so there is no response left to read back), this half-
     closes only the write side so the response can be inspected directly. */
  char buf[4096] = {0};
  int rc = _raw_request_truncate_body_half_close(
      "/stream-error-report", "only-part-of-the-declared-body", 1000, buf,
      sizeof(buf));
  REQUIRE_EQ(rc, 0);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_http_transfer_aborted") != NULL);
}

TEST(chttpserver, expect_100_continue_interim_response_sent_before_body) {
  /* conn->expects_continue's interim "100 Continue" write moved from the
     reactor thread (_conn_pump, right when headers finish) to the worker
     thread (_task_worker, right before body draining begins) so a slow-
     reading client can no longer stall the server's sole reactor thread
     during this write; no test exercised the server's Expect: 100-continue
     behavior at all before this change. This proves the move preserved the
     actual wire behavior: the interim response still arrives before the
     client sends its body, and the real final response still follows once
     the body is sent. */
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

/* Regression test: a streaming route's handler that decides to reject a
   request without ever calling chttpsvr_req_read() must produce the real
   final response directly, with NO "100 Continue" interim response ever
   reaching the client first. Before the fix, _task_worker sent "100
   Continue" unconditionally, before the handler ever ran, for both buffered
   and streaming routes alike; silently telling the client to go ahead and
   upload a body the server was never going to read, defeating the entire
   point of Expect: 100-continue (RFC 7231 SS5.1.1: let the server answer
   with a final status instead of "100 Continue" and skip the upload
   entirely). Fixed by deferring the interim send for a streaming route to
   chttpsvr_req_read() itself (see that function's own comment), so it fires
   only if the handler actually asks to read. */
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

  /* Deliberately never send `body`: a well-behaved Expect: 100-continue
     client waits for either "100 Continue" or a final response before
     uploading, and this test proves the server never asks it to. */
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 401);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "no thanks") != NULL);
  close(fd);
  fd = -1;
}

/* Regression test: a request that carries "Expect: 100-continue" but has no
   body at all (no Content-Length, no chunked Transfer-Encoding) must never
   receive the interim "100 Continue" response, for a STREAMING route whose
   handler does call chttpsvr_req_read(). Before the fix, the lazy send in
   chttpsvr_req_read() fired unconditionally on its first call regardless of
   whether chttp1_parser_message_complete() was already true (i.e.
   regardless of whether there was ever a body to invite); telling the
   client to go ahead and upload a body that was never coming. */
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

/* Same regression, for a BUFFERED route: _task_worker's own eager interim
   send (fired before _drain_body, for routes registered with
   chttpsvr_register_handler rather than the streaming variant) had the
   identical unconditional-send bug. */
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

/* Regression test for a real bug in _write_interim_continue: a genuine short
   write of the 25-byte "HTTP/1.1 100 Continue\r\n\r\n" interim line (a slow-
   reading peer stalling the write partway through, exactly the scenario
   max_response_write_duration_ms exists to defend against) left a truncated,
   unparseable status line on the wire, and neither _task_worker (this
   test's own buffered-route path) nor chttpsvr_req_read (the sibling
   streaming-route test right below) ever checked for it: both proceeded to
   run the handler and send a complete, valid final response immediately
   after the truncated prefix, producing a byte stream like "HTTP/1.1 100
   ConHTTP/1.1 200 OK\r\n..." no compliant HTTP/1.1 client could parse. Fixed
   by having _write_interim_continue report whether it wrote the complete
   line, and having both call sites treat a false return as fatal for this
   connection: skip the real response send entirely and close instead of
   layering more bytes on top of a possibly-corrupted stream.

   _chttpsvr_force_short_interim_write_for_tests() reproduces the short
   write deterministically (a real, partial write to the actual socket, not
   a simulated one) rather than trying to coax the OS's own socket buffering
   into a genuine partial write of a message this small. */
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

/* Streaming-route counterpart of the test above: the same short-write
   corruption, reached via chttpsvr_req_read's own lazy interim send instead
   of _task_worker's eager one. Also confirms chttpsvr_req_read itself
   returns -1 immediately (rather than attempting to read a body over a
   connection whose response channel is now corrupted), by using the
   existing _stream_error_report_handler, whose own "done"/x-stream-err
   response must never reach the wire either. */
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
  /* Regression test for a real gap: _write_interim_continue's own "100
     Continue" interim write (RFC 7231 SS5.1.1, sent for an ordinary
     "Expect: 100-continue" request, standard client behavior; e.g. curl's
     own default for large uploads) used to thread through conn->write_
     deadline via a bare _shrink_timeout_to_deadline call, unlike
     _send_response's own rejection-response path, which was given an
     unconditional _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS ceiling in an
     earlier fix specifically so a small, fixed-shape, internally-generated
     write is never left fully unbounded when the operator leaves
     max_response_write_duration_ms at its own default-disabled value of 0.
     Left unfixed, a peer sending an ordinary Expect: 100-continue request
     and then trickling reads of the interim line could hold a worker-pool
     thread hostage indefinitely at this project's own shipped default
     configuration, exhausting the entire pool with only a handful of such
     connections; the identical Slowloris-class denial-of-service the
     rejection-response fix already closed for a different internally-
     generated write. g_srv (used here directly) never configures
     max_response_write_duration_ms, so it stays at its real, shipped
     default of 0 for this test, proving the ceiling really is unconditional
     rather than merely mirroring whatever cap the test itself sets up. */
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
  /* Bounds the read below in case a regression makes the hook stop being
     consulted at all for this path (the exact pre-fix behavior): without
     this, the server just sends an ordinary "100 Continue" interim line and
     the read below would still return promptly with those bytes rather
     than hang; this timeout exists purely as defensive
     belt-and-suspenders, not because a plausible regression here would
     actually hang. */
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

  /* A working fix means the interim-continue write loop's very first
     write-deadline check (before any real write(2) call is ever attempted)
     reports "already expired" via the forced hook, so the interim write
     returns false having sent zero bytes; per _write_interim_continue's own
     documented contract, any false return (not just a short one) makes the
     caller skip the real response send and close the connection, so the
     client observes EOF (read returns 0) with nothing at all received. A
     regressed (pre-fix) build never consults the hook for this path at
     all, so the server sends a complete, ordinary "100 Continue" line
     instead; observed here as a positive byte count. */
  REQUIRE_EQ((int)r, 0);
}

TEST(chttpserver, stream_read_timeout_ms_zero_means_wait_indefinitely) {
  /* chttpsvr_config_t.stream_read_timeout_ms is documented "0 = wait
     indefinitely", but every chttpserver.c call site that read it cast the
     value straight through to chttp1_stream_read/_write's own int
     timeout_ms parameter, which instead follows poll(2)'s convention
     (negative = block forever, 0 = a single non-blocking attempt, no
     translation of a configured "0 means forever" into "-1 means
     forever"). A configured 0 therefore silently meant the opposite: give
     up immediately, on every single call, without even trying. Confirmed
     via a standalone repro against the built library before this was
     fixed: a streaming handler's chttpsvr_req_read() returned -1 at
     essentially t=0 regardless of how long the client actually took to
     send the body, and even a completely bodyless GET never received a
     response at all, since response_write_timeout_ms's own "0 = use
     stream_read_timeout_ms's value" default silently inherited the same
     broken 0 and every response write failed instantly too. This test
     proves both are fixed: a client that delays sending its body well past
     any accidental "instant" failure must still succeed once the bytes
     actually arrive, and the resulting response must actually be
     delivered. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/zero-timeout-wait",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 17;
  cfg.stream_read_timeout_ms = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 17));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  /* Bounds the final response-read loop below in case a regression makes the
     server genuinely hang forever instead of responding (the "wait
     indefinitely" configuration under test here means a real server bug
     would otherwise have no other bound to catch it against): without this,
     that read(2) would block forever, hanging the whole test binary instead
     of failing this one assertion cleanly. 10s is generous relative to the
     300ms delay this test itself introduces below. */
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

  /* Deliberately delay well past any accidental "instant" failure before
     sending the body; a correct "wait indefinitely" implementation must
     still pick these bytes up once they arrive. */
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
  /* Every successfully-created thread must be joined before any REQUIRE_*
   * below can possibly return early: Tau's REQUIRE_* returns from this test
   * function immediately on failure, and args[]/threads[] are stack-local.
   * An unjoined thread still running _conc_stream_thread would keep writing
   * into its own a->ok after this function's stack frame has already been
   * popped and reused by whatever test runs next; a real stack-use-after-
   * return that can silently corrupt a later, unrelated test, not merely a
   * leaked thread. Joining first, then asserting in a separate loop
   * afterward, guarantees every thread has fully finished before any
   * REQUIRE_* in this function can possibly return. */
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(created, N_CONCURRENT_STREAMS);
  for (int i = 0; i < created; i++) REQUIRE_TRUE(args[i].ok);
}

/* ========================================================================== */
/*                       SUBROUTER TRAILING-SLASH TEST                        */
/* ========================================================================== */

TEST(chttpserver, subrouter_trailing_slash_prefix) {
  /* A sub-router registered with a trailing slash in its prefix (e.g.
     "/api/v3/") must behave identically to one registered without it
     ("/api/v3").  The implementation normalises the stored prefix, so
     requests to /api/v3/ping must be routed correctly. */
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
  /* chttpsvr_req_header reads the same conn->hdr_names/hdr_values array
     regardless of route kind (see streaming_repeated_header above, which
     covers the identical "last occurrence wins" behavior through a
     streaming route); this exercises it through a buffered route instead.
     The /header route is buffered, and chttpclient would overwrite
     duplicate names, so we use a raw socket here too. */
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
/*   ROUTE-MATCHING PER-ROUTER SEGMENT-DECODE CACHE REGRESSION TESTS          */
/*                                                                            */
/* chttpserver.c's route matcher percent-decodes each raw path segment at    */
/* most once per router per request (via a small cache), shared by every     */
/* candidate route tried against that router, rather than once per candidate */
/* route that happens to need it; a raw segment's decoded value (or its      */
/* decode failure) is the same answer regardless of which route asks. The    */
/* tests below exercise that sharing directly: several routes registered on  */
/* the same (root) router, so that trying and rejecting earlier candidates   */
/* must never corrupt what a later candidate reads back for a shared         */
/* position, whether that position is a literal segment, a captured param,   */
/* or one whose percent-encoding is outright malformed. Every route pattern  */
/* below is unique to this test group, so registering it dynamically here    */
/* cannot shadow or be shadowed by anything _setup() already registered.     */
/* ========================================================================== */

TEST(chttpserver, cache_shared_literal_prefix_segment_multiple_candidates) {
  /* Three routes share the identical first two segments; the second reaches
     the server percent-encoded ("%61lpha" -> "alpha"), so an actual decode
     (not just a byte-for-byte raw comparison) is what must be shared
     correctly across every candidate. Requesting the THIRD route's own path
     means the first two candidates are tried and rejected (on their own
     differing third segment) before the third is ever reached; if trying
     and rejecting them left the shared "alpha" cache entry corrupted (freed,
     stale, or simply never re-computed), this would either crash or return
     the wrong body. */
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
  /* Two routes both capture {id} at segment 0 and differ only at segment 1
     (a literal). Requesting the SECOND route's own path means the first
     route is tried first: it captures id="xyz" (percent-encoded on the
     wire as "x%79z" -> "xyz") into its own, now-discarded param array, then
     fails on segment 1 ("first" != "second") and frees that array. The
     second route must still read back the correct, uncorrupted "xyz" value
     for the identical segment 0; proving the cache retains its own
     decoded copy independently of what any earlier candidate did with a
     copy of it, not a reference into memory an earlier failed match already
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
  /* Two routes share a param at segment 0 and differ only at segment 1;
     segment 0 in the actual request carries a malformed percent-encoding
     ("%ZZ", not valid hex). Both routes must be rejected (404), not just
     whichever one happens to be tried first: a decode failure for a given
     raw segment is a permanent "this can never match" fact about that
     segment, independent of which candidate route is asking. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/x",
                                     _cache_param_handler, (void *)"x"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/y",
                                     _cache_param_handler, (void *)"y"),
      (int)ccol_success);

  chttpcli_response *resp = _get("/cache-bad/%ZZ/y");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*   SUB-ROUTER PREFIX-MATCHING SEGMENT-DECODE CACHE REGRESSION TESTS         */
/*                                                                            */
/* _find_route also percent-decodes each raw request-path segment at most    */
/* once per request, via a cache shared across every non-root sub-router's   */
/* own _prefix_matches call (as opposed to the per-router route-matching     */
/* cache the group above exercises, which is scoped to one router's own      */
/* candidate routes): a raw segment's decoded value is the same answer       */
/* regardless of which sub-router's own prefix is being compared against it. */
/* The tests below register several sibling sub-routers sharing a common,    */
/* percent-encoded-on-the-wire leading prefix segment, so that trying and    */
/* rejecting an earlier sub-router (on a later, differing prefix segment)    */
/* must never corrupt what a later sub-router reads back for that same,     */
/* shared leading position. Every prefix below is unique to this test group, */
/* so registering it dynamically here cannot shadow or be shadowed by        */
/* anything _setup() already registered.                                    */
/* ========================================================================== */

TEST(chttpserver, subrouter_prefix_cache_shared_across_candidates) {
  /* Three sub-routers share the identical first prefix segment; it reaches
     the server percent-encoded ("pfx-sh%61re" -> "pfx-share"), so an actual
     decode (not just a byte-for-byte raw comparison) is what must be shared
     correctly across every sub-router's own _prefix_matches call. Requesting
     the THIRD sub-router's own route means the first two are tried and
     rejected (on their own differing second prefix segment) before the third
     is ever reached; if trying and rejecting them left the shared
     "pfx-share" cache entry corrupted (freed, stale, or simply never
     re-computed), this would either crash or route to the wrong handler. */
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
  /* Two sub-routers share a leading prefix segment; that segment carries a
     malformed percent-encoding ("%ZZ", not valid hex) in the actual request.
     Both sub-routers must be rejected (404), not just whichever is tried
     first: a decode failure for a given raw path segment is a permanent
     "this can never match" fact about that segment, independent of which
     sub-router's own prefix is asking. */
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

  chttpcli_response *resp = _get("/pfx-b%ZZd/two/leaf");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpclient_resp_free(resp);
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
  /* Calling chttpsvr_stop twice on a server that was never started must be
   * safe: the first call is a no-op (chttpsvr_stop's own was_started check
   * is false) and the second is identical. Neither call should crash or
   * touch the shared engine reactor. */
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
   * on a streaming route.  Before the fix the buflen == 0 check was bundled
   * with the hard-error guards and incorrectly returned -1. */
  chttpcli_response *resp = _get("/stream-zero-buflen");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, req_read_empty_body_streaming_returns_zero) {
  /* When a streaming handler is invoked for a request that carries no body,
   * the first call to chttpsvr_req_read must return 0 (EOF) immediately
   * rather than a negative value or hanging. */
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
  /* Patterns ending with '/' must be rejected at registration time with
   * ccol_invalid_args.  Such patterns would be permanently unreachable:
   * _compile_pattern's own segment splitting has no way to represent a
   * trailing empty segment distinctly from the no-trailing-slash variant, so
   * they would silently match the same requests as the no-trailing-slash
   * variant; a misleading API contract.  Rejection is the only honest
   * behaviour.
   *
   * "/"  (the root) is a special case that is always accepted.
   *
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing table. */
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
  /* A prefix with consecutive slashes in the middle (e.g. "/api//v1") is
   * equally unreachable and must also be rejected.
   * Uses g_srv2 (never started). */
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
  /* chttpsvr_req_read(req, NULL, 0) must return 0, not -1.
   * The buflen==0 no-op guard must be evaluated before the NULL-buf guard so
   * that passing a NULL buffer with a zero length is treated as a harmless
   * no-op rather than a hard error. */
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
  /* chttpsvr_req_header(req, NULL) must return NULL rather than crashing.
   * chttpsvr_req is opaque, so the guard is exercised via an HTTP round-trip:
   * the _null_name_header_handler calls chttpsvr_req_header(req, NULL) on a
   * real live request and writes "null" iff the result is NULL. */
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
  /* chttpsvr_req_query_one's own header doc comment documents *val_out as
   * reset to NULL on every failure return, including ccol_invalid_args;
   * poison it with a non-NULL sentinel first so a regression that leaves
   * it untouched is actually observable, rather than starting at NULL and
   * trivially passing regardless of whether the function touches it. */
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
   * the count rather than crashing.  chttpsvr_req is opaque, so the guard
   * is exercised via an HTTP round-trip: _null_key_query_handler calls
   * chttpsvr_req_query(req, NULL, &n) on a real live request and writes
   * "null" iff both the result is NULL and the count is 0. */
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

/* (The engine lifecycle is now managed implicitly: the engine starts on the
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
  /* A handler that sets a 204 status and a response header but never calls
   * chttpsvr_resp_write* must still deliver the correct status code and header
   * to the client.  _finalize_response must take the no-body branch
   * (http_finish) rather than the body branch (http_send_body). */
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
  /* When the same method+path combination is registered twice on the same
   * router, the routing table keeps BOTH entries in registration order and the
   * first one wins at match time (first-wins policy).  The second registration
   * is silently accepted (returns ccol_success) but its handler is never
   * reached because _find_route returns on the first full match.
   *
   * The /dup-first-wins route is pre-registered in _setup:
   *   chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
   * _hello_handler, NULL); chttpsvr_register_handler(g_srv, CHTTP_GET,
   * "/dup-first-wins", _dup_second_handler, NULL); _hello_handler responds with
   * "Hello, world!"; _dup_second_handler with "second". The expected body is
   * "Hello, world!" (the first registration wins). */
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
  /* chttpsvr_resp_set_status(NULL, ...) must not crash (void return, no guard
   * to verify).  chttpsvr_resp_set_header(NULL, NULL, ...) must return
   * ccol_invalid_args via the NULL resp guard (it fires before the NULL name
   * guard, so the result is the same as the single-NULL-resp case).  The
   * companion test resp_set_header_null_name_value_live exercises NULL name
   * and NULL value with a live resp inside a handler. */
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
  /* chttpsvr_resp_set_header must reject a name or value containing an
   * embedded CR/LF byte with ccol_invalid_args rather than writing it
   * verbatim onto the wire: _send_response emits "name:value\r\n" with no
   * escaping, so an unvalidated CRLF would let a handler that reflects
   * request-controlled data into a response header inject arbitrary extra
   * header lines, or split the response into two (classic HTTP response
   * splitting). Also verifies a legitimate header set afterward is
   * unaffected; the two rejections must not corrupt resp's header list.
   * The /set-header-crlf-guards route is registered in _setup. */
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
  /* chttpsvr_resp_set_header's own CRLF-injection guard uses
   * strpbrk(x, "\r\n"), which is what makes it catch a BARE CR or a BARE
   * LF on their own, not just the combined "\r\n" pair the test above
   * already exercises. Verified directly here rather than assumed to
   * generalize correctly from the paired case, since a hypothetical
   * regression that checked only for the literal two-byte "\r\n"
   * substring (e.g. via strstr instead of strpbrk) would still pass that
   * test while missing this one. The /set-header-bare-crlf-guards route
   * is registered in _setup. */
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
  /* chttpsvr_resp_set_header must reject an empty name (no valid on-the-wire
   * representation) and a "Transfer-Encoding" name of any casing with
   * ccol_invalid_args, rather than storing it: _send_response always
   * computes and emits its own Content-Length header from the actual
   * response body, never chunk-encoding it, so a handler-set
   * Transfer-Encoding header reaching the wire would pair an untruthful
   * "chunked" claim with a real Content-Length over a body that was never
   * actually transfer-coded; an ambiguous framing an intermediary honoring
   * Transfer-Encoding over Content-Length (RFC 7230 SS3.3.3) could misparse,
   * mirroring chttp_request_set_header's identical rejection on the client
   * side. Also verifies a legitimate header set afterward is unaffected;
   * the three rejections must not corrupt resp's header list.
   * The /set-header-disallowed-name-guards route is registered in _setup. */
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
  /* chttpsvr_resp_set_header must reject a header NAME containing a byte
   * outside RFC 7230 SS3.2.6's tchar set, not merely one containing no
   * CR/LF: a space, a literal colon, or a control byte like DEL (0x7F) is
   * not itself a CRLF-injection vector, but still produces a structurally
   * malformed wire line a strict downstream parser could misread (e.g. "X
   * Foo: bar" as a name puts "X Foo:bar:baz\r\n" on the wire, not a real
   * two-field split). Also verifies a legitimate header using every
   * non-alphanumeric tchar byte still works after the three rejections;
   * the rejections must not corrupt resp's header list.
   * The /set-header-non-tchar-name-guards route is registered in _setup. */
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

/* 2. Empty query key (?=value) is accessible via chttpsvr_req_query */

TEST(chttpserver, query_empty_key) {
  /* A query string like "?=hello" has an empty key.  The server must parse it
   * correctly and make it retrievable via chttpsvr_req_query(req, "", &n).
   * The /query-empty-key route is registered in _setup. */
  chttpcli_response *resp = _get("/query-empty-key?=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_stray_ampersand_does_not_produce_phantom_empty_key) {
  /* "a=1&&b=2" has a genuinely empty pair between the two '&' characters
     (nothing at all, not even an '='); this must be skipped outright, not
     parsed as a spurious key=""/value="" entry alongside the real "a" and
     "b" pairs. Distinct from "?=hello" above (a non-empty pair whose key
     half happens to be empty), which must remain unaffected. */
  chttpcli_response *resp = _get("/query-stray-amp-counts?a=1&&b=2");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpclient_resp_free(resp);
}

TEST(chttpserver,
     query_leading_and_trailing_ampersand_does_not_produce_phantom_empty_key) {
  /* A leading "&a=1" and a trailing "b=2&" each have their own empty pair
     (before the first real pair / after the last one) that must likewise be
     skipped rather than counted as a spurious "" key. */
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
  /* A path starting with a second slash right after the first (e.g.
   * "//foo") must NOT match a "/" sub-router either: after stripping
   * exactly one leading slash, what remains ("/foo") is not empty, so
   * this is not the exact root path "/" and the router is skipped just
   * like any other non-root path. Sent via a raw socket (rather than
   * _get, which would go through chttpclient's own URL parsing/
   * normalisation) to exercise the server's literal wire-level path
   * matching directly. */
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
  /* _compile_pattern must reject any pattern that does not begin with '/'.
   * Such a pattern cannot represent a valid HTTP path and would silently match
   * the same requests as the slash-prefixed form (both have the leading '/'
   * stripped before segment comparison), violating the documented API contract.
   * g_srv2 is used because it is never started; registering invalid routes on
   * it does not affect the running test server. */
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
  /* chttpsvr_start validates cfg->port == 0 BEFORE attempting TLS setup.
   * This confirms the arg-guard order: a bogus port returns ccol_invalid_args
   * regardless of what the TLS config contains, and no ctls_ctx_new_mp /
   * ctls_ctx_cert_add call is ever made.
   *
   * A full TLS integration test (starting a TLS server and completing HTTPS
   * handshakes) requires valid certificate files; that coverage lives in the
   * dedicated tests_tls binary in this same directory (see this directory's
   * Makefile), not here. */
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

/* serve_tls_blocked_by_running_server: removed.  Multiple server instances
 * may now run concurrently, so starting a TLS server while g_srv is active
 * is permitted.  TLS integration requires valid certificate files; that
 * coverage lives in the dedicated tests_tls binary in this directory. */

/* tls_context_cleaned_before_second_serve_attempt: this regression test
 * requires real TLS certificate files to exercise a genuine handshake, not
 * merely a certificate that fails to load (ctls_ctx_cert_add reports a
 * missing/invalid cert file via its own ccol_retval_t return; a load
 * failure alone doesn't reach the code path this test is meant to cover).
 * Coverage lives in the dedicated tests_tls binary in this directory and
 * is omitted from this suite. */

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
  /* chttpsvr_register_handler must succeed on a server that is already actively
   * serving requests (the write path of the rwlock is exercised while reactor
   * threads hold read locks).  The newly registered route must be reachable
   * immediately by subsequent requests in the same test process. */
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

TEST(chttpserver, root_router_shadows_subrouter_at_same_path) {
  /* A route registered directly on the server (root router, always checked
   * first) shadows a route with the same effective path registered on a
   * sub-router (checked second).  The root route must always win regardless
   * of registration order.
   *
   * Both routes are pre-registered in _setup to avoid mutating g_srv at test
   * runtime:
   *   1. Root route  /shadow-test/ping -> _shadow_root_handler
   *   2. Sub-router  /shadow-test  with /ping -> _shadow_sub_handler
   * Request: GET /shadow-test/ping -> must return "root-wins". */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  chttpcli_response *resp = _get("/shadow-test/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "root-wins");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_registration_rejected) {
  /* The middleware-chain cap (_CHTTPSVR_MAX_MW = 32) is enforced at
   * registration time: the first 32 calls to chttpsvr_router_use must return
   * ccol_success; the 33rd must return ccol_not_permitted.  The silent-500
   * behavior that was previously observable only at request-dispatch time is no
   * longer reachable through the public API because registration is rejected
   * before the overflow can occur. */
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
  /* A request with invalid percent-encoding in a {param} segment must return
   * 404 even when the method matches the registered route.  This exercises the
   * capture-mode path of _match_route_cached, where _seg_cache_get returns
   * NULL for bad encoding and the route is treated as a no-match.
   *
   * /api/v1/items/{id} is registered as GET only in _setup. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_param_wrong_method_returns_404_not_405) {
  /* Regression test for a dry-run asymmetry bug in this module's route
   * matching (predating the current _match_route_cached/_seg_cache_get
   * design, which structurally cannot reintroduce it: every segment is
   * decoded via _seg_cache_get unconditionally, regardless of whether the
   * caller passed a non-NULL pv_out to actually capture the value, so a
   * malformed encoding is rejected identically either way).
   *
   * In the old implementation, dry-run mode (used when the route's
   * registered method does not match the request method) accepted any
   * non-empty token for {param} segments without validating
   * percent-encoding.  A POST to the GET-only /api/v1/items/{id} with a
   * malformed parameter value would set method_mismatch_seen=true and
   * produce 405, while a GET to the same URL would correctly return 404
   * (capture mode rejected bad encoding).
   *
   * Both modes must validate encoding consistently: this request must
   * return 404, not 405. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  char buf[2048] = {0};
  int status =
      _raw_request("POST", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, valid_percent_encoded_literal_segment_matches_route) {
  /* A request whose path contains a percent-encoded character in a LITERAL
   * route segment (not a {param}) must still match the route.
   *
   * /hello is registered for GET in _setup.  /hel%6Co percent-encodes 'l'
   * (%6C == 0x6C == 'l'), so after decoding it is identical to /hello and
   * must match.
   *
   * This exercises _match_route_cached's own literal-segment comparison.
   * Before the NUL-termination fix, http_decode_path_unsafe wrote decoded
   * bytes but did not place a '\0';
   * the subsequent strcmp read past the valid content into uninitialised
   * stack memory.  On stacks where that byte happened to be non-zero the
   * route would fail to match and the server would return 404. */
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
  /* Invalid percent-encoding in a LITERAL route segment must yield 404.
   * /hel%ZZo has bad encoding where the "hello" literal sits; no route can
   * match so the server returns 404 (not 500, not a crash). */
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
  /* chttpsvr_start(srv, NULL) must fall back to CHTTPSVR_CONFIG_DEFAULT rather
   * than crashing on a NULL cfg dereference.  Uses g_srv (already started) so
   * the double-start guard returns ccol_not_permitted; NOT ccol_invalid_args,
   * which would incorrectly signal that NULL is an invalid argument rather than
   * a handled default. */
  REQUIRE_TRUE(g_srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_start(g_srv, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

/* ========================================================================== */
/*                         CHTTP_ANY WILDCARD TESTS                           */
/* ========================================================================== */

TEST(chttpserver, any_method_get) {
  /* CHTTP_ANY route dispatches a GET request and the handler can observe
     CHTTP_GET via chttpsvr_req_method(). */
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
  /* CHTTP_ANY is a registration-time-only placeholder ("match any of the
     seven concrete methods this server recognizes"), never a real incoming
     request's method; a syntactically valid but unrecognized method token
     (a WebDAV verb like PROPFIND, TRACE, CONNECT, a custom verb, ...) must
     never reach a CHTTP_ANY handler at all. Regression test for a real bug:
     _find_route's method_ok test (route->method == CHTTP_ANY) used to
     short-circuit to true regardless of the actual method, so such a
     request reached _any_method_handler anyway, which called
     chttp_method_str(chttpsvr_req_method(req)) and got back "UNKNOWN" (the
     switch's default case) instead of a real method name: a 200 response
     silently misreporting the request. Now rejected up front, before path/
     header parsing or route matching ever runs, as 501 (RFC 7231 SS6.6.2),
     and the handler is never invoked at all (an empty body proves this:
     _any_method_handler always writes a non-empty method-name string). */
  char buf[2048] = {0};
  int status = _raw_request("PROPFIND", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
  REQUIRE_TRUE(strstr(buf, "content-length:0") != NULL);
}

TEST(chttpserver, unrecognized_method_rejected_even_on_unmatched_path) {
  /* The rejection happens before route matching (and even before path
     parsing) runs at all, so an unrecognized method on a path with no route
     whatsoever still reports 501, not 404. */
  char buf[2048] = {0};
  int status =
      _raw_request("PROPFIND", "/no-such-route-at-all", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
}

/* ========================================================================== */
/*                    max_body_size / 413 BOUNDARY TESTS                      */
/* ========================================================================== */

/* Connects to `port`, POSTs a body of exactly `body_len` 'a' bytes to `path`
   with Connection: close, reads the response (or until the connection
   closes), and returns the parsed status code, or -1 if no valid status
   line was ever received (e.g. the connection was reset before any response
   bytes arrived). */
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
  /* A body of exactly max_body_size (64) bytes must be accepted and echoed
     back in full; the enforcement check inside _on_body (chttpserver.c) is
     strictly-greater-than, so the boundary value itself must succeed. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_EQ(strlen(body), (size_t)SMALL_BODY_MAX);
}

TEST(chttpserver, buffered_max_body_size_exceeded_rejected) {
  /* A body one byte over max_body_size must be rejected with a real
     413 Payload Too Large response (not a bare connection reset) and the
     connection must close afterward (Connection: close) rather than stay
     alive for a corrupted next request, since excess body bytes beyond the
     limit were left unread on the wire. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 413);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, streaming_max_body_size_at_limit_succeeds) {
  /* A streaming route reading a body of exactly max_body_size bytes via
     chttpsvr_req_read must see the full body with no stream error. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, streaming_max_body_size_exceeded_reported) {
  /* Unlike the buffered path (which the framework itself turns into a 413
     before ever calling the handler), a streaming route's handler is always
     invoked and decides its own response; chttpsvr_req_read() simply
     returns -1 and chttpsvr_req_stream_error() reports ccol_msg_too_large,
     exactly like the existing stream_read_timeout_reports_ccol_timed_out
     test's ccol_timed_out case. The connection must still carry a real,
     complete HTTP response (not a bare reset) and must close afterward
     (Connection: close) rather than staying alive for a corrupted next
     request, since excess body bytes beyond the limit were left unread. */
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_msg_too_large") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

/* Regression test for a real bug found via code review: max_body_size == 0
   is documented (chttpserver.h's own field comment) as "unlimited", matching
   the same convention max_connections/max_header_bytes already use in this
   same config struct, but _on_headers_complete's Content-Length pre-check
   and _on_body's cumulative check both used a bare `> limit` comparison,
   silently treating limit == 0 as "cap at zero" instead: any request with a
   body at all was rejected. A caller explicitly configuring "no limit" this
   way (a realistic choice, by direct analogy with this struct's own sibling
   fields) got a server that 413s every POST/PUT/PATCH with any body.
   Exercises a body well beyond any historical default/small-test limit
   (200000 bytes) on both a buffered and a streaming route to prove neither
   enforcement path silently caps at zero. Uses its own dedicated server
   (rather than the shared g_small_body_srv fixture, whose max_body_size is
   fixed at SMALL_BODY_MAX) so this test's own max_body_size = 0 cannot be
   confused with or disturbed by that fixture's own boundary tests. */
TEST(chttpserver, buffered_max_body_size_zero_means_unlimited) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
     large as the request; buf is heap-allocated (body_len plus headroom for
     headers) rather than a large stack array for exactly that reason. */
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
      create_chttpsvr(g_test_logger, NULL);
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
  /* The 413/ccol_msg_too_large tests above are the only other exercise in
     this file of the "a body error found while draining a diverted request
     does not force-close the socket synchronously" mechanism: _drain_body
     reports the failure back to _task_worker as a ccol_retval_t, which maps
     it to a graceful synchronous status response (rather than tearing the
     connection down immediately, which would destroy the connection before
     the worker's own error response could ever be written); the connection
     then closes only after that response is flushed, via the ordinary
     keep-alive/close decision downstream. This test exercises a distinct
     parse-error class hitting that same path: malformed chunk-size framing
     in a Transfer-Encoding: chunked body, caught by chttp1_parser's own
     chunk-size decoder, not a max_body_size/declared-length check. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  /* "ZZZZ" is not a valid hex chunk-size token; the parser must reject it
     while consuming the body on the worker thread, well after routing (at
     headers-complete time) has already matched /stream-error-report. */
  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "ZZZZ\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below in case the "must close afterward" fix
     regresses: without this, a regression would hang this test (waiting for
     more bytes/EOF that never come) instead of failing its own assertion
     cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* The malformed body must still produce a real, complete HTTP response
     (not a bare reset) reporting the ingestion failure, and the connection
     must close afterward rather than staying alive for a corrupted next
     request. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") == NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, oversized_chunk_size_hex_rejected_gracefully) {
  /* "8000000000000000" is a syntactically valid 16-hex-digit chunk-size
     token (~9.2 exabytes as a uint64_t; this parser's chunk-size decoder is
     pure unsigned arithmetic with proper overflow checks, so there is no
     signed-negative-value class of bug here to protect against). Before
     chttp1_parser_t gained max_chunk_size_override (wired from
     chttpsvr_config_t.max_body_size in _conn_reset_for_request), a value
     this large was simply accepted as the declared size of the current
     chunk, and the connection then sat waiting for that many bytes of chunk
     data that were never actually sent; resolved only once the server's
     stream_read_timeout_ms (30s by default on g_srv) finally fired. That
     made this exact test take ~30 real seconds to pass, for the wrong
     reason: its assertions were loose enough (some non-"none" x-stream-err
     value) to pass equally whether the server rejected the oversized chunk
     promptly or silently timed out half a minute later.
     This now verifies the real, fast-path property: the oversized chunk is
     rejected as soon as its chunk-size line is parsed, before any of its
     (nonexistent) data is ever waited for, reported as the same
     ccol_msg_too_large a merely-oversized *cumulative* body already
     produces (see streaming_max_body_size_exceeded_reported), and the
     round trip completes in well under a second; not 30. */
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

  /* Bounds the read loop below in case the fast-rejection fix regresses:
     without this, a regression that goes back to waiting on
     stream_read_timeout_ms would hang this test in read() itself rather
     than merely making the elapsed_s check below fail (that check can only
     ever run once the loop actually returns). */
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
  /* Companion to the chunked case above, for plain Content-Length framing:
     a buffered route whose declared Content-Length already exceeds
     max_body_size must be rejected with 413 immediately at headers-complete
     time (_on_headers_complete's new upfront check), before ever diverting
     to a worker thread or waiting for any body byte; not only once that
     many bytes have actually streamed in, which the peer here never sends
     at all. g_small_body_srv (TEST_PORT+3) has max_body_size == 64. */
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

  /* Declares far more than SMALL_BODY_MAX (64) and never sends a single
     body byte. /small-body-echo is registered as a plain (non-streaming)
     route via chttpsvr_register_handler in _setup. */
  const char *req =
      "POST /small-body-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 999999999\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  /* Bounds the read loop below in case the upfront-rejection fix regresses:
     without this, a regression that instead waited for body bytes that
     never arrive would hang this test in read() itself rather than merely
     making the elapsed_s check below fail. */
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
  /* A chunked body's terminating "0\r\n" can be followed by a trailer-part
     (zero or more header-field lines) before the final CRLF, per RFC 7230
     SS4.1.2. chttp1_parser shares the exact same process_header_line() code
     path for a message's primary headers and a chunked body's trailer
     headers (see the chttp1_parser module notes: no name whitelist, no
     separate trailer allowance), so a route dispatched via
     CHTTP1_HEADERS_DIVERT_BODY (see _on_headers_complete) must only ever
     have on_headers_complete invoked once for a given request, even when a
     real trailer field is present after the body. This test verifies a
     trailer field is parsed correctly at all and that the request is
     dispatched to the handler exactly once (checked indirectly: exactly one
     response comes back, and the connection behaves as an ordinary single
     request/response on a Connection: close connection). */
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

  /* Exactly one response: a second on_headers_complete dispatch for the
     trailer completion would, at best, corrupt the single response with
     extra bytes, and at worst crash the worker or hang the connection. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, buffered_route_trailer_visible_via_req_header) {
  /* chttpsvr_req_header's own documented trailer-visibility contract (see
     that function's own doc comment in chttpserver.h): for a BUFFERED
     route, the whole body (a chunked body's own RFC 7230 SS4.1.2 trailer
     field included) is always read before the handler ever runs, so the
     trailer must already be retrievable from the handler's very first
     statement; see _buffered_trailer_echo_handler. */
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
  REQUIRE_TRUE(strstr(buf, "x-trailer-value:some-value") != NULL);
}

TEST(chttpserver, streaming_route_trailer_visible_only_after_body_drained) {
  /* chttpsvr_req_header's own documented trailer-visibility contract for a
     STREAMING route: a trailer field only becomes retrievable once the
     handler's own chttpsvr_req_read() calls have drained the body all the
     way to EOF; querying it any earlier must return NULL. This is not a
     timing-dependent race to win: a streaming route's body/trailer parsing
     only ever advances as chttpsvr_req_read() is actually called (whether
     the bytes arrived on the wire before or after the handler started
     running makes no difference), so _stream_trailer_visibility_handler's
     "before" check, made before its own first chttpsvr_req_read() call, is
     deterministically "not yet parsed" every time. */
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
  REQUIRE_TRUE(strstr(buf, "x-trailer-after:some-value") != NULL);
}

TEST(chttpserver, negative_content_length_rejected) {
  /* http1_atol honors a leading '-', so "Content-Length: -1" used to parse
     successfully; http1_consume_body treats content_length <= 0 as "no
     body, already complete" the instant headers finish, so any body bytes
     the client actually sent would have been silently reparsed as the
     start of the next pipelined request; a framing desync. This is
     caught during header parsing itself (http1_consume_header_top), before
     routing/diversion ever happens, so the fixed behavior is the parser's
     ordinary malformed-input response: the connection is closed outright
     with no HTTP response at all, not a graceful error page (matching
     every other pre-routing parse error in this parser, e.g. a
     syntactically invalid request line). */
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

  /* Bounds the read loop below in case the reject-and-close fix regresses:
     without this, a regression that instead kept the connection open would
     hang this test in read() rather than failing its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  /* No HTTP response was ever produced; the connection was closed as soon
     as the malformed header was parsed. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1") == NULL);
}

TEST(chttpserver, chunked_not_last_in_transfer_encoding_list_rejected) {
  /* RFC 7230 SS3.3.1 requires `chunked`, when present, to be the final
     transfer-coding. http1_consume_header_transfer_encoding's two fast
     paths handle "chunked" alone or "chunked" as the last item in the
     list; previously, anything else (e.g. "chunked, gzip", chunked
     appearing in the middle) fell through and was silently stored as an
     ordinary opaque header with neither HTTP1_P_FLAG_CHUNKED nor a usable
     Content-Length set, framing the body as an implicit zero-length
     message instead of rejecting it, a request-smuggling-shaped front/back
     disagreement with any upstream proxy that DOES honor `chunked`
     appearing anywhere in the list. This is caught during header parsing,
     before routing, so (like the negative Content-Length case) the fixed
     behavior is an outright connection close with no HTTP response. */
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

  /* Bounds the read loop below in case the reject-and-close fix regresses:
     without this, a regression that instead kept the connection open would
     hang this test in read() rather than failing its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1") == NULL);
}

/* ========================================================================== */
/*                    SERVER DESTROY-WHILE-IN-FLIGHT TESTS                    */
/* ========================================================================== */

/* Connects to `port`, sends a POST whose body is dripped one byte at a time
   (50ms apart) so a ctpool worker stays blocked inside chttpsvr_req_read /
   http1_stream_read for the whole drip, and finally drains and discards
   whatever response (or abrupt close) it gets. Used as background traffic
   while the main thread destroys the server out from under it. */
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
  /* Bounds the drain loop below in case a regression makes destroy() never
     actually close this connection; this test's own observed completion
     time is well under a second, comfortably clear of this margin. */
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

/* Private per-test-invocation synchronization state for the two
 * destroy-from-a-background-thread-with-a-bounded-wait tests below
 * (destroy_while_worker_reading_slow_body_is_safe and
 * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts).
 *
 * A single, file-scope-global mutex/cv/done triple was used here originally
 * and found to be a real bug: both tests run back-to-back with nothing else
 * in between, and each test's own detach-rather-than-join-on-timeout path is
 * deliberate (see either test's own comment), so a detached thread from an
 * EARLIER test that is merely slow (not genuinely hung) rather than a
 * regression can still be running when a LATER test starts, resets the
 * shared `done` flag, and starts waiting on the shared condvar. If that
 * earlier thread then finishes and broadcasts while the later test is
 * waiting, the later test observes a stale "done" signal that was never
 * actually produced by its own destroy_th, silently skipping its own
 * timeout-path safety net (which exists specifically to avoid ever blocking
 * unconditionally on a possibly-hung thread) and falling through to an
 * unconditional, unbounded pthread_join of its OWN destroy_th. If that
 * test's own chttpsvr_destroy() call is the one genuinely hung (exactly the
 * regression these tests exist to catch), this converts a clean, bounded,
 * reported test failure into a silent, permanent hang of the whole binary;
 * precisely the failure mode both tests' own comments say they are designed
 * to prevent. Giving each test invocation its own private mutex/cv/done
 * (bundled with the srv handle itself into one struct, heap-allocated so it
 * can still be safely leaked on the same timeout/detach path srv itself
 * already is) makes this structurally impossible: no other test's thread can
 * ever observe or signal a different test's own ctx. */
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
  /* Regression/coverage test: __chttpsvr_destroy must not free any
   * server-owned state a ctpool worker could still be dereferencing (e.g.
   * srv->max_body_size, read via chttpsvr_req_read) while that worker is
   * still blocked reading a slow/dripped body on another connection.
   * chttpsvr_stop's own _drain_and_close_all_connections waits for
   * in_flight_requests to reach zero before the server's own memory is
   * freed, which is what this test exercises. This test doesn't assert on a
   * return value; a use-after-free here would show up under `make memtest`
   * (valgrind) or as an outright abort/crash in a debug build, which is the
   * real verification. Uses its own short-lived server so it cannot disturb
   * the shared test fixture.
   *
   * chttpsvr_destroy() is driven from a background thread and awaited via a
   * bounded pthread_cond_timedwait, not a bare, unbounded call on this
   * thread: mirrors destroy_does_not_hang_when_worker_blocked_with_disabled_
   * timeouts's own established pattern below, so a regression in destroy's
   * own bounded-wait/forced-unblock machinery (see that test's own comment
   * for the exact historical bug this guards against) fails this one test
   * cleanly instead of hanging the entire binary with no attributable
   * failure. ctx (which owns srv) is heap-allocated, not a stack local with
   * RAII, for the same reason that test's own comment gives: this function
   * must be able to safely leave the destroy thread running detached (never
   * joined) if the bounded wait itself times out, and a stack local would
   * turn that already-bounded worst case into a use-after-free the moment
   * this function's own stack frame is reused by a later test. ctx is its
   * own, private synchronization state (see _destroy_hang_ctx_t's own
   * comment for why this must not be shared with the sibling test below). */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  REQUIRE_NE((void *)ctx, NULL);
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = create_chttpsvr(g_test_logger, NULL);
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
  cfg.stream_read_timeout_ms = 2000;
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

  /* Poll for the background connection to actually be accepted, headers
     parsed, and dispatched to a ctpool worker (in_flight_requests > 0,
     meaning it is now blocked mid-drip inside chttpsvr_req_read) before
     destroying the server underneath it, rather than guessing a fixed sleep
     duration: bounded to 5s, comfortably clear of this test's own ~1s drip
     duration, so a slow CI/valgrind run cannot silently turn this into
     "destroy on an already-idle server" instead of the worker-mid-read race
     this test is named for. bg is joined (bounded by its own 5s SO_RCVTIMEO,
     per its own comment) before the REQUIRE_TRUE below on a timeout, per
     this file's own unconditional-cleanup-before-REQUIRE convention. */
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

  /* Generous relative to this test's own ~1s drip duration (20 bytes at
     50ms each) plus the production-sized 30s/5s bounded-wait defaults this
     test leaves untouched (unlike the disabled-timeouts test below, this
     one never needed to shrink them, since stream_read_timeout_ms=2000 was
     already enough to make destroy's own graceful wait succeed quickly in
     practice); wide enough to still fail cleanly, rather than mask a real
     hang, on a loaded CI machine. */
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

  /* Joined unconditionally, before the destroyed_in_time REQUIRE_* below
     that could otherwise return early: bg's own read loop is already
     bounded by a 5s SO_RCVTIMEO (see _drip_body_bg_thread's own comment),
     so joining it here is always safe regardless of destroy_th's own
     outcome, and skipping it on a timeout would leave it unreclaimed for
     the remainder of this binary's run for no reason. */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach rather than join: if this is a genuine regression, the destroy
       call may never return at all, and blocking here would just convert
       one clean, bounded, reported test failure into a second, silent
       hang. ctx (and the srv it owns) is deliberately leaked in this one,
       already-failing path (never freed) so the still-running detached
       thread's own dereference of it can never be a use-after-free. */
    pthread_detach(destroy_th);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);
}

/* Connects to `port`, sends a POST declaring a Content-Length far larger
   than what it actually sends, sends only a few bytes of it, and then never
   sends the rest and never closes the connection; a permanently stalled
   peer, by design. Used by destroy_does_not_hang_when_worker_blocked_with_
   disabled_timeouts below, which configures both stream_read_timeout_ms and
   max_body_read_duration_ms to 0 ("wait indefinitely"), so nothing but the
   server's own forced-unblock mechanism (see _force_unblock_diverted_
   connections in chttpserver.c) can ever make the worker thread blocked
   reading this connection's body return. */
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
  /* Bounds the read loop below in case a regression makes the server's own
     forced-unblock mechanism never actually fire; the test that drives this
     thread shrinks that mechanism's own bounds to ~2.3s total via
     _chttpsvr_set_wait_in_flight_bounds_for_tests, comfortably clear of
     this margin, so this backstop cannot mask a genuine regression there. */
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

  /* Never send the rest, never close from this side. Block waiting for
     whatever the server eventually does; unblocking at all (rather than
     this thread, and therefore the pthread_join below, hanging forever) is
     itself part of what this test proves. */
  char buf[16];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
  }
  close(fd);
  fd = -1;
  return NULL;
}

/* Regression test for a real gap: chttpsvr_config_t.stream_read_timeout_ms
   and max_body_read_duration_ms both document "0 = wait indefinitely" as a
   legitimate, supported configuration (e.g. for slow legitimate uploads);
   but chttpsvr_destroy() (via _quiesce_server_once) unconditionally calls
   ctpool_shutdown_drain() on the worker pool shortly after its own,
   carefully bounded (30s) wait for in-flight requests; and that function
   has no timeout of its own at all ("blocks until every queued and active
   task has completed"). Before the fix, a single peer that stalled mid-body
   under this configuration (sent a partial body, declared far more via
   Content-Length, then never sent the rest and never closed) left a worker
   thread permanently blocked inside chttp1_stream_read with nothing to ever
   unblock it, so chttpsvr_destroy() would hang forever despite that
   elaborate bounded wait. Fixed by forcibly shutdown(2)-ing any connection
   still diverted to a worker thread once the bounded wait is exhausted; see
   _wait_in_flight_bounded/_force_unblock_diverted_connections in
   chttpserver.c. This test's real assertion is that chttpsvr_destroy()
   actually returns at all, checked via a bounded condition-variable wait
   (not a bare pthread_join) so a regression fails this one test cleanly
   instead of hanging the whole binary. Uses its own short-lived server (not
   the shared g_srv) with both timeouts disabled, exactly the documented
   configuration this bug required. Uses the white-box
   _chttpsvr_set_wait_in_flight_bounds_for_tests() hook to shrink
   _wait_in_flight_bounded's own graceful-wait/grace-period bounds from tens
   of real seconds down to a few hundred milliseconds, so this test
   exercises the exact same escalation logic deterministically without
   actually waiting out the real, production-sized timers. */
extern void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                          unsigned grace_ms);

TEST(chttpserver,
     destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts) {
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  /* Heap-allocated, not a stack local: _destroy_hang_thread_fn runs on a
     background thread that this function must be able to safely leave
     running (never joined) if the bounded wait below itself times out;
     see that branch's own comment for why joining unconditionally there
     would risk turning a genuine regression into a second, harder-to-
     diagnose hang instead of a clean, reported test failure. A stack local
     would turn that already-bounded worst case into a use-after-free
     instead, the moment this function's own stack frame is reused by a
     later test. Every early-exit path below explicitly destroys/frees this
     itself (rather than relying on REQUIRE_*'s early return plus scope-exit
     cleanup, which cannot run here) so a failure partway through this test
     can never leak the shared engine's own reference to this server; see
     this file's own history for the class of whole-binary hang that leaving
     a leaked, still-engine-referencing server behind causes in _teardown's
     own chttpsvr_engine_wait() call. ctx is its own, private synchronization
     state (see _destroy_hang_ctx_t's own comment for why this must not be
     shared with the sibling test above). */
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  if (ctx == NULL) {
    /* Every early-exit path below also resets this process-wide override
       back to (0, 0) before its own REQUIRE_*, matching the final,
       unconditional reset at the very end of this test's own happy path:
       leaving it shrunk here would silently corrupt the graceful-wait/
       grace-period bounds every OTHER test in this binary that itself hits
       a genuinely slow worker relies on, for the remainder of this
       process's run. */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_NE((void *)ctx, NULL);
  }
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = create_chttpsvr(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_INVALID) {
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ccol_retval_t rv = chttpsvr_register_handler(
      ctx->srv, CHTTP_POST, "/stall-forever", _echo_body_handler, NULL);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 29;
  cfg.stream_read_timeout_ms = 0;    /* "wait indefinitely" */
  cfg.max_body_read_duration_ms = 0; /* "wait indefinitely" */
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

  /* Poll for the background connection to actually be accepted, headers
     parsed, and diverted to a ctpool worker (in_flight_requests > 0, meaning
     it is now genuinely, permanently blocked inside chttp1_stream_read,
     absent the fix) before destroying, rather than guessing a fixed sleep
     duration: bounded to 5s, comfortably clear of the shrunk 300ms/2000ms
     wait-in-flight overrides above, so a slow CI/valgrind run cannot
     silently turn this into "destroy on an already-idle server" instead of
     the worker-permanently-blocked race this test is named for. bg is
     joined (bounded by its own 5s SO_RCVTIMEO, per its own comment) and the
     wait-in-flight override is reset before the REQUIRE_TRUE below on a
     timeout, per this test's own unconditional-cleanup-before-REQUIRE
     convention. */
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

  /* Generous bound relative to the shrunk 300ms/2000ms overrides above,
     with real margin for a loaded CI machine; nowhere near the real,
     production-sized 30s/5s defaults this would otherwise need. */
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

  /* Joined unconditionally, before the destroyed_in_time REQUIRE_* below
     that could otherwise return early: bg's own read loop is already
     bounded by a 5s SO_RCVTIMEO (see _stall_forever_bg_thread's own
     comment), so joining it here is always safe regardless of destroy_th's
     own outcome, and skipping it on a timeout would leave it unreclaimed
     for the remainder of this binary's run for no reason. */
  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    /* Detach rather than join: if this is a genuine regression, the destroy
       call may never return at all, and blocking here would just convert
       one clean, bounded, reported test failure into a second, silent
       hang. ctx (and the srv it owns) is deliberately leaked in this one,
       already-failing path (never freed) so the still-running detached
       thread's own dereference of it can never be a use-after-free. */
    pthread_detach(destroy_th);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);

  /* Restore the real, production-sized defaults for every subsequent test
     in this same process (the override is process-wide, not per-server). */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
}

static _Atomic bool g_restart_race_stop;

/* Repeatedly pipelines GET requests over `fd` (an already-open keep-alive
   connection) until g_restart_race_stop is set, tolerating any write/read
   failure by exiting (the listener side of the connection is expected to be
   torn down and replaced repeatedly by the main thread while this runs).
   Used by restart_races_live_keep_alive_connection_is_safe below to keep
   real pressure on _conn_start_diverted's read of srv->worker_pool (and,
   transitively, _conn_reset_for_request/_on_body's reads of
   srv->max_header_bytes/max_body_size on the worker thread that call
   diverts to) throughout a restart storm. */
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
  /* Regression test for a srv->worker_pool data race: worker_pool used to be
   * written (drained/destroyed/recreated) without srv->mutex in every
   * chttpsvr_start restart/teardown path, while _conn_start_diverted read it
   * under the lock - a genuine data race, and a real use-after-free risk if a
   * leftover pool were destroyed while a still-open keep-alive connection's
   * next pipelined request was concurrently reading/submitting to it:
   * chttpsvr_stop only closes the listener, already-accepted connections
   * keep running, and their _on_headers_complete does not check
   * srv->lifecycle, so _conn_start_diverted can fire at any point during a
   * restart. Fixed by _wait_and_detach_pools, which waits for
   * in_flight_requests to drain to zero before ever detaching/destroying a
   * pool, guaranteeing no _conn_start_diverted call can be mid-flight holding
   * a stale copy of the pointer being handed to ctpool_destroy.
   *
   * Also covers a closely related, later-found data race on the same
   * restart path: srv->max_header_bytes/max_body_size used to be plain
   * (non-_Atomic) fields chttpsvr_start rewrote, unsynchronized, in the
   * small gap after srv->worker_pool is published but before the listener is
   * re-registered; a pre-existing keep-alive connection's worker thread
   * could read either field (_conn_reset_for_request/_on_body) inside that
   * same gap. Fixed by giving them the same _Atomic treatment as
   * stream_read_timeout_ms/etc. on the same struct. This test varies both
   * across every restart iteration specifically to exercise that path, even
   * though the window is narrow enough that it is not expected to reliably
   * trip a plain CI run (see the struct field's own comment for why); the
   * real verification for both races is `make memtest` (valgrind) and
   * `-fsanitize=thread` running this test clean, and a debug build
   * aborting/crashing outright if either race were reintroduced.
   *
   * This test doesn't assert on a return value for most of its body, for the
   * same reason: the bugs are data races / a UAF, not a wrong result. A
   * background thread keeps one keep-alive connection continuously
   * pipelining requests
   * while the main thread restarts the server many times in a tight loop (a
   * fresh port each cycle, so bind timing on a just-closed port can never
   * make this flaky; the race under test lives entirely on the srv side, not
   * the listener's port). Uses its own short-lived server so it cannot
   * disturb the shared test fixture. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
  /* Bounds each read(2) inside _restart_race_pipeline_thread's own loop;
     the loop already tolerates any read/write failure by exiting on its
     own (see that function's own doc comment), so this only matters for a
     genuine regression that hangs the connection instead of erroring or
     responding, which would otherwise hang the pthread_join below (and
     this whole binary) forever. Never fires during correct operation:
     every real response here arrives in well under a second. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  atomic_store(&g_restart_race_stop, false);
  pthread_t pipeline_thread;
  bool pipeline_thread_created =
      pthread_create(&pipeline_thread, NULL, _restart_race_pipeline_thread,
                     &fd) == 0;

  /* Every restart's own result is captured into a local instead of
     asserted on immediately: a REQUIRE_* returning early mid-loop on a
     genuine regression would otherwise leave pipeline_thread permanently
     unjoined (it only ever exits via g_restart_race_stop, set below) and
     fd never closed, leaking a thread and a socket for the rest of this
     binary's run instead of merely failing this one test. */
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
  /* stream_read_timeout_ms only bounds each individual gap between batches
   * of body bytes; a client that sends a little data and then stalls
   * *within* that gap never trips it. max_body_read_duration_ms bounds the
   * *total* time spent reading one request's body regardless of per-gap
   * progress, closing that loophole. Configure a generous per-gap timeout
   * (so it cannot possibly fire first) alongside a short overall duration
   * cap; send part of the declared body once and then never send the rest,
   * mirroring stream_read_timeout_reports_ccol_timed_out's proven
   * send-once-then-just-read pattern (a drip-writing client risks the
   * server responding and closing the connection mid-upload, which would
   * fail the client's own subsequent writes with EPIPE before it ever gets
   * to read the response). */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/deadline-test", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 5;
  cfg.stream_read_timeout_ms = 5000;   /* generous; must not fire first */
  cfg.max_body_read_duration_ms = 300; /* the cap actually under test */
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

  /* The response body alone cannot distinguish which of the two caps fired:
   * both stream_read_timeout_ms and max_body_read_duration_ms collapse to
   * the identical ccol_timed_out value (see _stream_err_to_retval /
   * req->_deadline_exceeded in src/chttpserver.c). Without a wall-clock
   * check, a max_body_read_duration_ms implementation that was silently a
   * no-op would still pass this test: the connection would simply sit until
   * the 5000ms stream_read_timeout_ms cap eventually fired instead, taking
   * roughly 5s instead of roughly 300ms. Bound elapsed time well below the
   * 5000ms per-gap cap (leaving generous headroom above the 300ms overall
   * cap for scheduling jitter under load/valgrind) to prove the *short*
   * cap is what actually fired. */
  long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                    (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
  REQUIRE_LT(elapsed_ms, 2500L);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_body_read_duration_default_disabled_allows_slow_drip) {
  /* max_body_read_duration_ms defaults to 0 (disabled); a slow-but-steady
   * drip that would trip a short overall cap must still succeed when the
   * cap is left unset, on a server whose stream_read_timeout_ms is generous
   * enough that the per-gap timeout doesn't fire either. Guards against the
   * deadline check misfiring when it's supposed to be a no-op. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/deadline-disabled",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 6;
  cfg.stream_read_timeout_ms = 5000;
  REQUIRE_EQ((int)cfg.max_body_read_duration_ms, 0);
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
  /* response_write_timeout_ms only bounds each individual write(2)-
   * equivalent call; a client that reads a byte or two just before every
   * such call's own timeout expires never trips it. max_response_write_
   * duration_ms bounds the *total* time spent sending one response
   * regardless of per-call progress, closing that loophole; the write-
   * side analogue of max_body_read_duration_ms. Configure a generous
   * per-call timeout (so it cannot possibly fire first) alongside a short
   * overall duration cap; the client sends its request and then genuinely
   * reads nothing at all (not even a slow trickle) for well longer than the
   * cap, so the kernel socket buffers fill and chttp1_stream_write blocks
   * waiting for space that will never free up during that silence; an
   * earlier draft of this test read the response back in a tight,
   * unthrottled loop immediately after sending the request, which drained
   * the socket fast enough that the write side never actually blocked at
   * all, silently testing nothing; caught by the observed elapsed time
   * being a few milliseconds, not the ~300ms a genuinely-firing deadline
   * would produce.
   *
   * The assertion itself is a byte-count comparison, not a timing one:
   * timing how long the client's own read loop takes once it resumes
   * reading cannot reliably distinguish "the deadline already closed this
   * connection" from "the deadline never fired, but the now-unblocked
   * write finishes fast anyway once reading resumes" on a fast loopback
   * connection, where the remaining transfer completes quickly either way.
   * Whether the FULL response ever arrives, however, cannot: a genuinely-
   * firing deadline closes the connection mid-write during the silent
   * window, so no read afterward can ever recover the untransmitted
   * remainder, however long it is given to try. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 60;
  cfg.response_write_timeout_ms = 5000;     /* generous; must not fire first */
  cfg.max_response_write_duration_ms = 300; /* the cap actually under test */
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

  /* Bounds every read call below in case the fix regresses (the connection
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

TEST(chttpserver,
     max_response_write_duration_default_disabled_allows_slow_reader) {
  /* max_response_write_duration_ms defaults to 0 (disabled); a response
     the client reads slowly but steadily must still be delivered in full
     when the cap is left unset, on a server whose response_write_timeout_ms
     is generous enough that the per-call timeout doesn't fire either.
     Guards against the deadline check misfiring when it's supposed to be a
     no-op. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response-slow-ok", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 61;
  cfg.response_write_timeout_ms = 5000;
  REQUIRE_EQ((int)cfg.max_response_write_duration_ms, 0);
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 61));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd _ccol_destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  /* Bounds the read loop below in case a regression makes the server hang
     rather than deliver the response (max_response_write_duration_ms is
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
  /* max_response_write_duration_ms must bound a courtesy rejection response
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
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 65;
  cfg.response_write_timeout_ms = 5000;     /* generous; must not matter */
  cfg.max_response_write_duration_ms = 300; /* value itself is irrelevant:
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

  /* Bounds the read below in case a regression makes the fix's own guarded
     write-deadline check stop being consulted at all for this path (the
     exact pre-fix behavior): without this, if the hook is silently never
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

  /* A working fix means the header-write loop's very first write-deadline
     check (before any real write(2) call is ever attempted) reports
     "already expired" via the forced hook, so _send_response returns false
     having sent zero bytes; the connection is then closed immediately, so
     the client observes EOF (read returns 0) with nothing at all received.
     A regressed (pre-fix) build passes NULL for conn here, so the hook is
     never even consulted (conn && ... short-circuits), and the server sends
     a complete, ordinary 404 response instead: observed here as a
     positive byte count. */
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
      create_chttpsvr(CLOG_INVALID, NULL);
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
      create_chttpsvr(parent, NULL);
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
  log_info(parent, "parent logger still usable after server destroy");

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(parent, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_destroy(srv2);

  clog_close(parent);
}

/* ========================================================================== */
/*                    UNIX DOMAIN SOCKET TESTS (Phase 1, new capability)      */
/* ========================================================================== */

TEST(chttpserver, unix_socket_listen_and_round_trip) {
  /* "unix://path" on chttpsvr_config_t.host must bind a Unix domain socket
     instead of a TCP listener, and a request over that socket must be
     routed and answered exactly like a TCP connection would be. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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

TEST(chttpserver, unix_socket_stale_file_replaced_on_start) {
  /* A leftover file (regular file, not even a socket) already sitting at
     the configured path must be removed automatically rather than causing
     bind() to fail; the documented "a stale socket file already at that
     path is removed automatically before binding" behavior. */
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_stale_%d.sock",
           (int)getpid());
  unlink(sock_path);
  int stale_fd = open(sock_path, O_CREAT | O_WRONLY, 0600);
  REQUIRE_TRUE(stale_fd >= 0);
  close(stale_fd);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "unix:///chttpsvr_test_nonexistent_dir_xyz/socket.sock";
  ccol_retval_t rv = chttpsvr_start(srv, &cfg);
  REQUIRE_NE((int)rv, (int)ccol_success);
  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    MAX_CONNECTIONS TESTS (Phase 1, new knob)               */
/* ========================================================================== */

TEST(chttpserver, max_connections_enforced) {
  /* With max_connections == 1, a second concurrent connection must be left
     pending in the kernel's listen backlog (never accept()'d, never
     served) until the first connection closes and frees the one slot. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
     ~100% CPU for as long as the server stays at capacity (confirmed via a
     standalone /proc/<pid>/stat CPU-tick measurement during development;
     not itself asserted on here, since raw CPU-time measurement is
     inherently noisy on a shared/loaded machine). This is caught instead by
     directly counting how many times the listener was actually dispatched
     during a held-at-capacity window, via a white-box counter: a correctly
     paused listener produces at most a small, fixed handful of dispatches
     (the one that decided to pause), while a busy-looping one produces many
     thousands within a fraction of a second. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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

/* Regression test for a real bug found via code review: after a successful
   accept4(), _conn_create()/ctls_conn_create_server() failing (allocation
   failure) used to loop straight back to accept4() again with zero backoff;
   unlike accept4()'s own EMFILE/ENFILE/ENOBUFS/ENOMEM handling a few lines
   up in the same loop, which already backed off specifically to avoid
   busy-looping a persistent resource-exhaustion condition. Under a sustained
   allocation-failure condition (a custom, bounded ccol_memmgmt_procs_t is
   the realistic trigger) combined with connections continuing to arrive,
   this spun the reactor thread with zero progress; the identical
   unbounded-busy-loop-under-a-persistent-condition shape already fixed for
   accept4() itself.

   The fix pauses srv's listener registration on the very first allocation
   failure (see _listener_pause_for_resource_pressure), rather than merely
   sleeping before retrying inline: sleeping still blocks the one process-
   wide shared reactor thread for that sleep's duration on every single
   re-dispatch, starving every OTHER connection on every OTHER server
   sharing that reactor thread for as long as the condition persists,
   whereas pausing removes the reactor from the picture entirely until the
   idle-timeout sweep thread resumes it (see _listener_resume_if_resource_
   pressure_cleared), within at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS of the
   condition clearing.

   Verifies both halves of that fix via deterministic state checks rather
   than inferring them from network activity observed over some timing
   window: the idle-timeout sweep thread's own real, ~1s timer runs
   unsynchronized with this (or any) test's clock, and can legitimately
   resume, and (with nothing queued behind fd_a to re-fail against) leave
   resumed, this exact listener at any point during the test, making a
   network-observed window an inherently flaky signal for "is it currently
   paused". (1) the pause is genuinely reached and genuinely takes effect:
   both the white-box counter and the listener_paused_for_resource_pressure
   flag itself are checked immediately after fd_a's own failure. (2) the
   pause is not permanent: once the failure condition clears, a fresh
   connection is still eventually accepted and served, via the idle-timeout
   sweep thread's own resume. */
TEST(chttpserver,
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping) {
  extern bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(
      chttpsvr h);

  ccol_memmgmt_procs_t mp = {_backoff_test_malloc, _backoff_test_free,
                             _backoff_test_calloc, _backoff_test_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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
     EOF read returns, which raced and failed intermittently under TSan's
     own scheduling (the two threads run on genuinely different CPUs; EOF
     delivery does not wait for the server thread to reach its next
     statement). */
  /* Poll the pause flag itself, directly and repeatedly, rather than
     inferring "the pause happened" from a single read of the counter and
     then doing one, one-shot flag read immediately after: the counter is
     incremented BEFORE _listener_pause_for_resource_pressure is even called
     (see _listener_log_and_pause_for_alloc_failure), and that call performs
     real work of its own (a mutex lock/unlock plus a full event_loop_pause()
     call, itself a real syscall, not merely a few uncontended instructions)
     before the flag is actually stored. A single flag read timed off the
     counter's own change can therefore land in that real, non-negligible gap
     and observe `false` even though the pause is about to succeed a moment
     later - confirmed by exactly this failure occurring in practice. Polling
     the flag directly, bounded to 2s, has no such gap: once it reads true,
     the pause has genuinely already taken effect, and since the counter
     increment strictly precedes it in program order on the same thread, the
     counter is guaranteed to already reflect the change too by that point.

     One further, narrower residual race is worth naming explicitly: the
     real idle-timeout sweep thread runs continuously for this whole test
     binary on its own ~1s timer, entirely independent of this test's own
     timing, and _listener_resume_if_resource_pressure_cleared unconditionally
     atomic_exchanges the flag back to false on every tick. If a tick happens
     to land in the handful of microseconds between the flag being stored
     true and this loop's very first read, that read (and every subsequent
     one, since nothing else re-triggers the failure once fd_a alone has
     already been processed) would observe false for the entire 2s bound.
     Unlike the gap this fix itself closes (a real syscall's worth of width,
     confirmed to fail in practice), this window is bounded by this loop's
     own per-iteration overhead against a fixed ~1000ms period, several
     orders of magnitude narrower; stress-tested 30 consecutive runs under
     valgrind and 25 under ThreadSanitizer with zero failures before accepting
     it, matching this file's own tolerance for a comparably narrow, already-
     documented timing window elsewhere (see start_racing_engine_stop_and_
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
     at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, i.e. ~1s) and let a fresh
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

/* Regression test for a real test-coverage gap found via code review: the
   accept loop pauses unconditionally for any accept4() errno that isn't
   EWOULDBLOCK/EAGAIN/EINTR/one of _accept_errno_is_transient's own per-
   connection cases - not only the specific EMFILE/ENFILE/ENOBUFS/ENOMEM
   resource-exhaustion allow-list _accept_errno_is_resource_exhaustion names
   (see that helper's own comment and the one call site's own comment in
   _listener_on_readable_impl). Before this test, nothing actually drove a
   real accept4() failure with an UNCLASSIFIED errno (EBADF/EINVAL/ENOTSOCK/
   EFAULT and similar) through the loop to confirm it still pauses rather
   than busy-loops for one of those; reverting to the old allow-list-gated
   design (pausing only for the four named resource-exhaustion errnos) would
   have passed every other test in this file untouched.

   Uses _chttpsvr_force_next_accept_errno_for_tests (a white-box hook added
   specifically for this test, scoped to one specific server so this test's
   own long-lived shared fixture server or any other concurrently active
   server in this process can never steal the forced errno intended for
   srv - an early, unscoped version of this hook hit exactly that race
   intermittently under valgrind) to make the very next accept4() dispatch
   for srv report EINVAL - deliberately NOT one of the four resource-
   exhaustion errnos, so this specifically exercises the "unclassified"
   branch the fix closes - without needing to actually corrupt a real fd or
   exhaust a real system resource, which would be environment-dependent and
   disproportionate for what is, underneath, a simple control-flow decision.
   Confirms the LISTENER's own reaction
   directly via the deterministic pause-flag accessor (immune to the real
   idle-sweep timer's own independent schedule, unlike inferring pausedness
   from network-observed timing; see post_accept_alloc_failure_pauses_and_
   resumes_instead_of_busy_looping's own identical reasoning), then confirms
   the pause is not permanent via a fresh connection succeeding once the
   real sweep resumes the listener. */
TEST(chttpserver,
     accept_unclassified_errno_pauses_and_resumes_instead_of_busy_looping) {
  extern void _chttpsvr_force_next_accept_errno_for_tests(chttpsvr h,
                                                          int errno_val);
  extern bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(
      chttpsvr h);

  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
     observes it true (confirmed intermittently in practice this way; see
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping's
     own analogous, narrower residual-race discussion - this ordering
     mistake made that same class of race meaningfully wider here). Starting
     the poll immediately closes that self-inflicted gap down to this loop's
     own per-iteration overhead, matching that sibling test's own tight
     margin; stress-tested 120 consecutive runs under valgrind and 40 under
     ThreadSanitizer with zero failures after this reordering, versus
     multiple failures within roughly 100 runs before it. */
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
     _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, i.e. ~1s) and let a fresh connection
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
/*                    MAX_HEADER_BYTES TESTS (Phase 1, new knob)              */
/* ========================================================================== */

TEST(chttpserver, max_header_bytes_within_limit_succeeds) {
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
     routing; an outright connection close with no HTTP response at all,
     matching every other pre-routing parse error in this parser (see
     negative_content_length_rejected above). */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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

  /* Bounds the read loop below in case the reject-and-close fix regresses:
     without this, a regression that instead kept the connection open would
     hang this test in read() rather than failing its assertion cleanly. */
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[512] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1") == NULL);

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
      create_chttpsvr(g_test_logger, NULL);
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
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1") == NULL);
  }

  chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    RESPONSE_WRITE_TIMEOUT_MS TEST (Phase 1, new knob)      */
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
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/big-body",
                                               _large_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 12;
  cfg.response_write_timeout_ms = 200;
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
     response_write_timeout_ms, and force the connection closed rather than
     pinning the worker thread forever. Sleep well past the configured
     timeout before ever touching the socket, so the server has already
     made its close-or-succeed decision by the time we look; reading (or
     even polling for readability) any earlier risks a false "ready" signal
     from data the server already buffered successfully before stalling. */
  struct timespec wait_past_timeout = {0, 600000000L}; /* 600ms > 200ms cfg */
  nanosleep(&wait_past_timeout, NULL);

  /* Bounds every read call below in case the fix regresses (the connection
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
/*                    IDLE-TIMEOUT SWEEP TEST (Phase 1, new mechanism)        */
/* ========================================================================== */

TEST(chttpserver, idle_timeout_closes_unused_connection) {
  /* A connection that never sends a request at all must eventually be
     closed by the module-local idle-timeout sweep thread once
     idle_timeout_ms has elapsed, rather than being held open forever. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/idle-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 13;
  cfg.idle_timeout_ms = 300;
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
  int pr = poll(&pfd, 1, 3000); /* generous vs. idle_timeout_ms=300 and the
                                   sweep's own ~1s interval */
  REQUIRE_TRUE(pr > 0);
  char buf[16];
  ssize_t r = read(fd, buf, sizeof(buf));
  REQUIRE_EQ(r, 0);

  chttpsvr_destroy(srv);
}

/* Regression coverage for a real bug found via code review: the idle sweep
   captures its own `now` snapshot once, before it ever takes idle_mutex or
   examines any specific connection; a connection's last_activity is
   refreshed by _idle_list_add from a DIFFERENT thread every time it lands
   back in the idle list, including the ordinary case of finishing a
   keep-alive request concurrently with a sweep tick. A connection that
   gains fresh activity in the window between the sweep's own `now` snapshot
   and the sweep actually reaching that connection therefore legitimately
   has last_activity > now; the elapsed-time computation used to be plain
   `long` arithmetic compared via `(unsigned long)elapsed_ms >= idle_ms`,
   which silently wraps a small negative elapsed_ms to a huge unsigned
   value (unconditionally >= idle_ms for any realistic timeout) spuriously
   evicting a connection that had just become idle instead of correctly
   recognizing it as having zero (or negative) elapsed idle time. Exercised
   directly via the extracted pure decision function rather than by trying
   to actually win a real scheduling race against a live sweep thread. */
TEST(chttpserver, idle_sweep_negative_elapsed_never_flagged_as_timed_out) {
  extern bool _chttpsvr_conn_idle_timed_out_for_tests(
      struct timespec now, struct timespec last_activity, unsigned idle_ms);

  /* The regression case itself: last_activity is "in the future" relative
     to the sweep's own now snapshot (last_activity gained fresh activity
     after now was captured but before this connection was examined). Must
     never be reported as timed out, regardless of how large idle_ms is (a
     small idle_ms is exactly what the pre-fix unsigned wraparound made
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

  /* Exact boundary: elapsed_ms == idle_ms is >=, i.e. timed out. */
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

TEST(chttpserver, idle_timeout_ms_overflow_clamped_not_wrapped) {
  /* chttpsvr_config_t.idle_timeout_ms/read_timeout_ms are `long`, but the
     internal field chttpsvr_start() feeds them into is `unsigned`; a value
     >= 2^32 must clamp to UINT_MAX rather than silently wrap (a multiple of
     2^32 would wrap to exactly 0, this field's own "idle timeout disabled"
     sentinel, silently turning off the idle-timeout sweep the caller
     explicitly configured instead of applying the very long timeout that
     was actually asked for). Verified via the white-box accessor rather
     than waiting out a real idle timeout, since the whole point here is
     that the configured value is enormous. */
  extern unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h);

#if LONG_MAX >= 4294967296L
  /* On an ILP32 platform (e.g. i386), `long` is only 32 bits, so a literal
   * >= 2^32 cannot even be assigned to it; the overflow this test exists
   * to catch (a `long` value that overflows the internal 32-bit `unsigned`
   * field it's fed into) is structurally impossible to construct there in
   * the first place, not merely difficult; this whole scenario only exists
   * on an LP64 platform, where `long` is itself 64 bits. Guarded with a
   * compile-time #if, not a runtime check: the offending literals are a
   * compile error under -Werror=overflow on ILP32 regardless of what
   * runtime branch would have contained them. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 24;
  cfg.idle_timeout_ms = 4294967296L; /* exactly 2^32; wraps to 0 uncorrected */
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv), UINT_MAX);
  chttpsvr_destroy(srv);

  /* Same clamp exercised via the read_timeout_ms fallback path
     (idle_timeout_ms left at 0). */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg2 = CHTTPSVR_CONFIG_DEFAULT;
  cfg2.host = "127.0.0.1";
  cfg2.port = TEST_PORT + 25;
  cfg2.idle_timeout_ms = 0;
  cfg2.read_timeout_ms = 8589934592L; /* 2^33; also wraps to 0 uncorrected */
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg2), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv2), UINT_MAX);
  chttpsvr_destroy(srv2);
#else
  fprintf(stderr,
          "[SKIP] idle_timeout_ms_overflow_clamped_not_wrapped: `long` is "
          "only 32 bits on this platform, so the >= 2^32 overflow this "
          "test exercises cannot be constructed here at all\n");
#endif

  /* An ordinary, small value must still pass through untouched, on every
   * platform regardless of the above. */
  chttpsvr srv3 _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv3 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg3 = CHTTPSVR_CONFIG_DEFAULT;
  cfg3.host = "127.0.0.1";
  cfg3.port = TEST_PORT + 26;
  cfg3.idle_timeout_ms = 5000;
  REQUIRE_EQ((int)chttpsvr_start(srv3, &cfg3), (int)ccol_success);
  REQUIRE_EQ(_chttpsvr_idle_timeout_ms_for_tests(srv3), (unsigned)5000);
  chttpsvr_destroy(srv3);
}

TEST(chttpserver, enable_keepalive_does_not_break_normal_requests) {
  /* SO_KEEPALIVE is set on an accepted connection's own fd, which a client
     has no portable way to observe from the outside (getsockopt only ever
     reports the calling process's own socket state); this is therefore a
     black-box smoke test that the setsockopt(2) call itself neither fails
     nor disturbs the normal request/response path, matching this file's
     own established pattern for config knobs whose effect is otherwise
     unobservable from a client (e.g. max_header_bytes_within_limit_
     succeeds above, for the byte cap itself). */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
      create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv1 != CHTTPSVR_INVALID);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 15;
  cfg.enable_reuseport = true;
  REQUIRE_EQ((int)chttpsvr_start(srv1, &cfg), (int)ccol_success);

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
     the mechanism works end to end. */
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/reuseport-hello",
           TEST_PORT + 15);
  bool got_200 = false;
  for (int i = 0; i < 8 && !got_200; i++) {
    chttpcli_response *resp = NULL;
    chttp_get(url, &resp);
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
      create_chttpsvr(g_test_logger, NULL);
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
      create_chttpsvr(g_test_logger, NULL);
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
/*         HEAD METHOD + CARRY-OVER ALLOCATION-FAILURE REGRESSION TESTS       */
/* ========================================================================== */

TEST(chttpserver, head_request_suppresses_response_body) {
  /* RFC 7231 SS4.3.2: a HEAD response reports the same header fields
     (Content-Length included) a GET would, but must never actually send the
     message body. Regression test for a real bug: _send_response used to
     write conn->resp.body to the wire unconditionally, regardless of
     conn->method, so a HEAD request reaching a handler that writes a body
     got that body streamed back anyway. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
  /* _conn_reject_and_close builds resp via a bare memset-to-zero and never
     writes any body content onto it for ANY reject_status, so resp.body_len
     is unconditionally 0 here regardless of the request method, meaning
     the suppress_body argument this call site passes to _send_response
     (conn->method == CHTTP_HEAD, rather than a hardcoded false) can never
     actually change what reaches the wire today: _send_response's own
     no_body-gated early return only ever skips a body-write loop that was
     already going to execute zero iterations. Threading the real method
     through is still correct, defensive consistency with every other
     _send_response call site in this file (in case a reject response ever
     grows a body, e.g. an error-detail message, in the future), but this
     test cannot exercise or verify that specific wiring: an assertion built
     around "the wire bytes differ between suppress_body=true/false" would
     be vacuous here, since they provably do not differ either way. What
     this test verifies instead is the actually-observable, real behavior:
     a HEAD request to a rejected route gets back a well-formed, genuinely
     bodyless 404: an explicit "content-length:0" header (404 is neither
     1xx nor 204, so may_report_length is true; _send_response always
     computes this from the real resp->body_len, which is unconditionally 0
     here) and no body bytes of any kind after the header block's
     terminating CRLF, not merely "some text containing 404". */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, NULL);
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
  /* Regression test for a real bug in _conn_start_diverted: when a request's
     headers and the start of its body arrive in the same read() (the
     leftover/carry-over bytes past the header block), the function used to
     set conn->_carry_over_len to the leftover length even if the matching
     _mem_alloc for conn->_carry_over failed. The worker thread would then
     call chttp1_stream_prepare with a NULL pointer and a nonzero length,
     which unconditionally memcpy()s from that NULL pointer, crashing. This
     drives that exact allocation to fail via a custom allocator and asserts
     the connection is instead rejected gracefully (500) with the server
     (and the rest of this test process) still alive and functional
     afterward. */
  size_t body_len = 6151; /* distinctive; unlikely to collide with any other
                           * allocation size this request triggers */
  ccol_memmgmt_procs_t mp = {_fail_at_size_malloc, _fail_at_size_free,
                             _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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
      create_chttpsvr(g_test_logger, NULL);
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

/* Regression test for a real bug found via code review: a genuine _on_body
   allocation failure while growing a streaming route's own body-accumulation
   buffer used to be reported to the handler as ccol_http_transfer_aborted
   ("connection closed or malformed framing") via chttpsvr_req_stream_error(),
   indistinguishable from an actual dropped connection or malformed chunk
   framing, even though nothing was wrong with the peer or its framing at
   all - a genuine server-side OOM was silently misclassified as a client-
   caused transfer error. Fixed by giving this specific failure its own flag
   (conn->body_alloc_failed) and its own, distinct chttpsvr_req_stream_error()
   outcome (ccol_not_enough_memory), so a handler that reacts differently to
   "the peer misbehaved" versus "the server is out of memory" (e.g. logging/
   alerting differently, or only retrying a downstream call for the former)
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
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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

/* Regression test for a real bug in _on_header: an allocation failure while
   copying a header's name/value (before routing has even run) used to
   return 1 (aborting the parse with CHTTP1_USER) without ever setting
   conn->req_rejected/reject_status, so _conn_feed_bytes fell into its
   "else" branch and silently closed the connection with zero response
   bytes; unlike the identical OOM failure mode _on_headers_complete
   already handles gracefully (a 500 via reject_pool) a few callbacks
   later. Drives that exact allocation to fail via the same fail-at-size
   custom allocator the carry-over OOM test above uses, and asserts a
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
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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

/* Regression test for the identical bug class as the one just above, this
   time in _on_request_line: an allocation failure while copying the raw
   (still percent-encoded) request path (which happens before any route
   has been matched at all) used to abort the parse with CHTTP1_USER
   without ever setting conn->req_rejected/reject_status, silently dropping
   the connection instead of sending a graceful 500. */
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
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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
   regardless of subsequent query calls"; but no existing test ever
   triggers a real OOM in that path and checks the flag; the only prior
   test exercises just the trivial NULL-req guard. A regression that broke
   the latch (never setting it, or resetting it on a later successful call)
   would pass the whole suite undetected without this. */
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
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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
   (chttpserver.h) had no test at all: the pre-existing OOM coverage above
   only ever exercises chttpsvr_req_query's/chttpsvr_req_query_oom's own
   flag-based contract, never chttpsvr_req_query_one's direct return-value
   contract, which is a structurally different code path (its own
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
     starting value could not distinguish "the OOM path actually resets
     *val_out" from "it simply never touched an already-NULL local", the
     exact gap a real bug once hid behind (see chttpsvr_req_query_one's own
     header doc comment: every failure return, OOM included, must leave
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
      create_chttpsvr_mp(&mp, g_test_logger, &err);
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

/* Regression test for a real bug in _task_worker: unlike the carry-over copy
   above (which goes through this module's own custom-allocator convention
   and is therefore rejected gracefully by _conn_start_diverted before ever
   diverting), _task_worker's OWN chttp1_stream_prepare()/_tls() call (a
   second, separate copy of the same carry-over bytes, made on the worker
   thread) goes through plain malloc() and has no custom-allocator hook to
   fail it from a test; _chttpsvr_force_stream_prepare_fail_for_tests()
   deterministically exercises that same failure path instead. Before the
   fix: every response path in _task_worker was gated on `prepared`, so on
   this failure the connection was simply closed with zero response bytes
   for a buffered route (the intended 500 was set but never sent), and for a
   streaming route the handler still ran against a NULL req->stream, with
   chttpsvr_req_stream_error() falsely reporting ccol_success instead of the
   real failure. This drives that exact scenario against a streaming route
   and asserts the connection instead receives a graceful 500 with the
   handler never invoked at all (no x-stream-err header, which only the
   handler itself ever sets). */
TEST(chttpserver, stream_prepare_failure_in_worker_sends_500_not_bare_close) {
  extern void _chttpsvr_force_stream_prepare_fail_for_tests(bool force);

  char *err = NULL;
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(g_test_logger, &err);
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
      create_chttpsvr(g_test_logger, NULL);
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
  /* Regression test for a real gap left behind by the HEAD body-suppression
     fix: _send_response only ever suppressed the body for suppress_body
     (HEAD), even though a 204 can never carry one either, regardless of
     method (RFC 9110 SS15.2.1). Before this fix, /status-with-body's own
     36-byte body would have been streamed back verbatim on a plain GET
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

TEST(chttpserver, response_304_suppresses_body_but_may_report_content_length) {
  /* Same body-suppression fix as response_204_never_sends_body_or_
     content_length, but for 304 (RFC 9110 SS15.4.5), which (unlike 1xx/204)
     is still permitted to report a Content-Length (mirroring HEAD's own
     treatment); this locks in that the auto-injected header is not also
     suppressed for this particular status. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 304\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 304);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  char *cl = strstr(buf, "content-length:");
  REQUIRE_TRUE(cl != NULL);
  unsigned long declared_len = strtoul(cl + 15, NULL, 10);
  REQUIRE_EQ(declared_len,
             (unsigned long)strlen("this-body-must-never-reach-the-wire"));
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
     same fix. */
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 199\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 199);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, large_response_headers_still_delivered_in_full) {
  /* Regression test: _send_response used to assemble the response header
     block into a fixed 4096-byte stack buffer with no fallback, so a
     response whose headers alone crossed that size silently lost the
     ENTIRE response; not a graceful 500, not a truncated write, just a
     bare connection close with zero bytes ever written and nothing logged.
     /large-response-headers sets 20 headers of ~300 bytes each (~6.2 KiB of
     header block, well past the old fixed cap); every one of them, the
     200 status, and the body must all still reach the client intact. */
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
  /* Regression test: _send_response used to emit a handler-supplied
     "Connection" response header verbatim, completely independent of the
     keep_alive value _task_worker actually used afterward to decide whether
     the connection stays open. A handler here explicitly sets "Connection:
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
  /* The other direction of the same fix: a handler explicitly sets
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
 * chttpsvr is a generation-tagged {slot index, generation} value handle
 * resolved through a library-owned slot table before the underlying struct
 * chttpserver* is ever touched (see the "CHTTPSVR HANDLE SLOT TABLE"
 * section of src/chttpserver.c; the exact same mechanism chttpclient.c
 * already uses for its own chttpcli handle, ported over for the identical
 * reason: __chttpsvr_destroy used to have no protection at all against
 * being run twice on the same handle, sequentially or concurrently, a
 * genuine double-free). This section tests that redesign directly.
 */
extern struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h);
extern size_t _chttpsvr_slot_table_capacity_for_tests(void);

static void _noop_middleware_for_lifecycle_tests(chttpsvr_req *req,
                                                 chttpsvr_resp *resp, void *ctx,
                                                 chttpsvr_next_fn next) {
  (void)ctx;
  if (next) next(req, resp);
}

/* A fully completed destroy, followed later by a second destroy call on an
 * independently-held copy of the same original handle value, must be a
 * fatal error. Run in a forked child (mirroring this codebase's own
 * fork-test precedent, e.g. tests/clogger/tests.c) since fatal_err aborts
 * the whole process. Each server in this section is created with a NULL
 * logger (create_chttpsvr's own internal stderr/FATAL-only logger) rather
 * than g_test_logger, so these tests have no dependency on the shared,
 * process-wide test server/logger state _setup/_teardown manage. */
TEST(chttpsvr_handle_lifecycle, sequential_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    chttpsvr stale = srv;  /* an independently-held copy of the handle value,
         distinct from the local the macro below NULLs out */
    chttpsvr_destroy(srv); /* completes normally; the local `srv` is now
        CHTTPSVR_INVALID, but `stale` still holds the original value */
    __chttpsvr_destroy(stale); /* the actual misuse under test: a second,
        purely sequential destroy of a handle already fully torn down */
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
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

/* Two threads calling destroy on two independently-held copies of the SAME,
 * still-valid handle at (as close to) the same moment as possible must also
 * be fatal; regression coverage for the original bug this redesign
 * exists to fix (a genuine heap double-free). */
TEST(chttpsvr_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    concurrent_svr_destroy_arg_t a1 = {.h = srv};
    concurrent_svr_destroy_arg_t a2 = {.h = srv};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, concurrent_svr_destroy_thread, &a1);
    pthread_create(&t2, NULL, concurrent_svr_destroy_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    _exit(0); /* unreachable: whichever of the two destroy calls loses the
                  race must hit fatal_err() */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* Exposed only under RUNNING_UNIT_TESTS; see its own doc comment in
 * chttpserver.c. Declared here (rather than in a header) matching this
 * test file's own established convention for white-box hooks (see e.g.
 * _chttpsvr_resolve_for_tests's own forward declarations elsewhere in this
 * file). */
extern void _chttpsvr_mark_self_as_worker_for_tests(chttpsvr h);

/* A request handler (or middleware) that destroys the very server whose
 * worker pool is running it must be a fatal error, exactly like a
 * stale-handle double-destroy: see chttpsvr_worker_key_bundle's own comment
 * in chttpserver.c for the real hazard this guards against (without it,
 * this call would hang for roughly a minute waiting for its own in-flight
 * request to finish, then abort from deep inside cthreadpool.c's own,
 * unrelated self-destroy guard; or, if racing a concurrent
 * chttpsvr_engine_stop() for the same server, deadlock permanently
 * instead). Driven directly via _chttpsvr_mark_self_as_worker_for_tests
 * rather than a real end-to-end request dispatched through a real worker
 * thread: doing this via fork() (needed either way, since fatal_err()
 * aborts the whole process) plus a real chttpsvr_start() would require the
 * forked child's own shared reactor to actually service a new listener
 * registration, but fork(2) does not duplicate any thread other than the
 * caller, so a child forked from this suite (whose shared reactor is
 * already running real OS threads by this point, given the hundreds of
 * earlier tests that started servers) inherits a hollow, permanently
 * un-serviced reactor handle (confirmed directly: an earlier version of
 * this test that did call chttpsvr_start() post-fork hung forever instead
 * of aborting, with gdb showing every thread idle and the listener never
 * once dispatched). The white-box hook sidesteps that fork/reactor
 * interaction entirely while still exercising the exact same comparison
 * __chttpsvr_destroy itself performs. */
TEST(chttpsvr_handle_lifecycle, destroy_from_within_own_handler_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    __chttpsvr_destroy(srv); /* the actual misuse under test */
    _exit(0); /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

/* A request handler (or middleware) that blocks in chttpsvr_engine_wait()
 * must be a fatal error too, for the identical underlying reason as
 * destroying its own server from within itself (see the previous test):
 * _engine_force_stop_quiesce_all's own timeout-less ctpool_shutdown_drain
 * can never finish draining this exact worker's own pool while this call
 * stack is what would eventually let the task return, so the shared engine
 * could never finish tearing down and this call could never wake (a
 * permanent, engine-wide deadlock, not merely one stuck server), reachable
 * via the exact pattern chttpserver.h itself documents for an admin/
 * shutdown endpoint (call chttpsvr_engine_stop(), then chttpsvr_engine_
 * wait() to block the response until the drain completes). Unlike
 * chttpsvr_destroy()'s own self-call guard, which is scoped to one
 * specific server, chttpsvr_engine_wait() is engine-wide: marking the
 * calling thread as a worker of ANY server (even one that was never
 * started) is enough to trigger it, so no real dispatch or running reactor
 * is needed here either. */
TEST(chttpsvr_handle_lifecycle, engine_wait_from_within_own_handler_is_fatal) {
  pid_t pid = fork();
  if (pid == 0) {
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) _exit(2);
    _chttpsvr_mark_self_as_worker_for_tests(srv);
    chttpsvr_engine_wait(); /* the actual misuse under test */
    _exit(0);               /* unreachable if fatal_err() aborted as expected */
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  waitpid(pid, &status, 0);
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
  /* Safe on its own: chttpsvr_stop() only closes the listener and never
   * touches worker_pool/reject_pool, so it never hits the self-call hazard
   * chttpsvr_start()'s own restart path below does. */
  chttpsvr_stop(g_self_restart_srv_for_lifecycle_test);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval_t rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  atomic_store(&g_self_restart_result_for_lifecycle_test, (int)rv);
  chttpsvr_resp_write_str(resp, "done");
}

/* A handler that stops and then immediately restarts the very server whose
 * worker pool is running it hits the identical self-call hazard
 * chttpsvr_destroy() does (see chttpsvr_worker_key_bundle's own comment in
 * chttpserver.c), but chttpsvr_start() has a real ccol_retval_t to report
 * through, so it must refuse gracefully with ccol_not_permitted rather than
 * hanging/aborting, and must leave the server in a state a later,
 * legitimate restart from a different thread can still recover cleanly. */
/* g_self_restart_srv_for_lifecycle_test is a process-wide global (not a
 * local), because the self-restarting handler above needs to reach it from
 * an arbitrary worker thread; unlike every other server handle in this file,
 * it therefore cannot carry a scope-exit _ccol_destructor safety net (a GCC
 * cleanup attribute only applies to automatic-storage/local variables). Every
 * REQUIRE_* below that could return early after the server has been
 * successfully started is instead guarded explicitly: on failure, this
 * destroys the server (releasing its shared-engine reference) before the
 * REQUIRE_* itself runs and returns, matching this file's own established
 * "capture, clean up, then assert" idiom for exactly this leak class. Without
 * this, a REQUIRE_* failure here (including, critically, the one at
 * REQUIRE_EQ(..., ccol_not_permitted) below, which is the actual regression
 * this test exists to catch) would leak an engine-referencing global
 * server and hang chttpsvr_engine_wait() in this file's own _teardown() at
 * process exit, converting a real regression in the exact feature under
 * test into a silent whole-binary hang instead of a clean, reported
 * failure. */
static void _destroy_self_restart_test_server_if_live(void) {
  if (g_self_restart_srv_for_lifecycle_test != CHTTPSVR_INVALID)
    chttpsvr_destroy(g_self_restart_srv_for_lifecycle_test);
}

TEST(chttpsvr_handle_lifecycle,
     restart_from_within_own_handler_returns_not_permitted) {
  atomic_store(&g_self_restart_result_for_lifecycle_test, -1);
  g_self_restart_srv_for_lifecycle_test = create_chttpsvr(CLOG_INVALID, NULL);
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

  /* This second request runs the very same self-restarting handler again,
   * so it will itself also try, and again correctly be refused, to restart
   * the server from within its own worker thread, yet it must still
   * receive a normal 200 response, confirming the server keeps working
   * correctly no matter how many times this doomed pattern is retried. */
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
  /* Return value intentionally ignored: a legitimate race with a concurrent
   * destroy can make this resolve fail (ccol_invalid_args) instead of
   * succeeding; both outcomes are correct. This thread exists purely to
   * generate resolve/pin/unpin traffic concurrent with the destroy thread
   * below. chttpsvr_use is representative of this module's entire non-
   * destroy public API: every one of chttpsvr_start/_stop/
   * _register_handler/_register_streaming_handler/_use/_subrouter resolves,
   * pins for its own short synchronous duration, and unpins before
   * returning, so exercising any one of them against a concurrent destroy
   * covers the same race window the others would. */
  chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
  return NULL;
}

/* Races a fast, non-blocking entry point (chttpsvr_use: resolve, pin, a
 * short critical section, unpin, return; no blocking I/O at all) against
 * a concurrent destroy, repeated under stress, since a real regression here
 * (a resolve-then-use race, or a use-after-free in the unpin path itself;
 * see chttpclient.c's own _chttpcli_resolve_unpin comment for a real
 * example of the latter, found only by tracing a precise interleaving by
 * hand) would only be reachable in a handful-of-instructions-wide window
 * that will not reproduce reliably under a single unstressed run. A fresh
 * server is used each iteration so every repetition gets its own
 * independent race rather than reusing one already-destroyed handle. */
TEST(chttpsvr_handle_lifecycle, resolve_unpin_race_stress) {
  /* Every intermediate outcome below is captured into a local instead of
   * asserted on immediately with REQUIRE_*, and srv is always either handed
   * off to destroy_tid or destroyed directly before any REQUIRE_* can
   * return early: this test creates a fresh chttpsvr handle every one of
   * 30 iterations, and each one holds a live shared-engine reference, so a
   * REQUIRE_* returning early with srv still undestroyed would make this
   * whole binary's own _teardown() (which blocks in chttpsvr_engine_wait()
   * until every server has been destroyed) hang forever at process exit. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
    if (srv == CHTTPSVR_INVALID) REQUIRE_NE(srv, CHTTPSVR_INVALID);

    use_setter_arg_t setter_arg = {.h = srv};
    concurrent_svr_destroy_arg_t destroy_arg = {.h = srv};
    pthread_t setter_tid, destroy_tid;
    int setter_rc =
        pthread_create(&setter_tid, NULL, use_setter_thread, &setter_arg);
    int destroy_rc = 0;
    if (setter_rc != 0) {
      /* Neither thread ever started: destroy srv directly. */
      chttpsvr_destroy(srv);
    } else {
      destroy_rc = pthread_create(&destroy_tid, NULL,
                                  concurrent_svr_destroy_thread, &destroy_arg);
      /* Either way, join the already-started, non-blocking setter_tid; if
       * destroy_tid never started, destroy srv directly ourselves. */
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
  /* Return value intentionally ignored: a legitimate race with a concurrent
   * destroy of the owning server can make this resolve fail
   * (ccol_invalid_args) instead of succeeding; both outcomes are correct.
   * This is a regression test for a real bug: chttpsvr_router_on/_on_stream/
   * _use used to dereference router->srv (and mutate router->routes/mw_head)
   * directly, with no resolve/pin against the owning server's handle at all,
   * unlike every other mutating entry point in this API; racing a
   * concurrent chttpsvr_destroy() that frees both srv and every router (via
   * _destroy_router) once it observes pending_resolve_count == 0, a count
   * these three functions never used to contribute to. */
  chttpsvr_router_on(a->router, CHTTP_GET, "/race", _hello_handler, NULL);
  return NULL;
}

/* Sub-router analogue of resolve_unpin_race_stress above: races
 * chttpsvr_router_on (representative of chttpsvr_router_on/_on_stream/_use,
 * which all now resolve/pin the router's owning server the same way) against
 * a concurrent chttpsvr_destroy() of that same server. Real verification for
 * this is `make memtest` (valgrind) / -fsanitize=thread, which would report a
 * genuine use-after-free if the resolve/pin protection regressed; this test's
 * own job is just to reliably manufacture the race window. */
TEST(chttpsvr_handle_lifecycle, router_resolve_unpin_race_stress) {
  /* Every intermediate outcome below is captured into a local instead of
   * asserted on immediately with REQUIRE_*, and srv is always either handed
   * off to destroy_tid or destroyed directly before any REQUIRE_* can
   * return early: see resolve_unpin_race_stress's own identical comment
   * immediately above for why a leaked chttpsvr handle here would hang this
   * whole binary's own teardown at process exit. */
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
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

/* This entire fork-safety regression group exercises chttpserver.c's own
 * pthread_atfork() protection, which is compiled out entirely when
 * FORK_SAFETY_REQUIRED is defined to 0 (see that macro's own doc comment in
 * common.h); without it, this test's own premise (verifying that
 * protection prevents a hang) no longer holds, and forking while the feeder
 * thread below is mid-critical-section becomes a genuine, if low-
 * probability, source of flakiness rather than a meaningful regression
 * check. Mirrors tests/cthreadpool's and tests/cthreadcomm's own identical
 * `#if FORK_SAFETY_REQUIRED` wrapping of their own analogous fork-safety
 * test groups exactly. */
#if FORK_SAFETY_REQUIRED
typedef struct {
  chttpsvr h;
  _Atomic int stop;
} fork_feeder_arg_t;

/* Exercises chttpsvr_use (resolve via chttpsvr_slot_table.mutex, then a
 * write-locked routes_lock critical section) against one, long-lived server
 * shared with the fork trials below. sched_yield() after every call is
 * load-bearing, not a nicety, mirroring tests/cthreadpool's own identical
 * ctp_fork_feeder_thread: a bare `while (!stop) chttpsvr_use(...);` loop
 * (tried first) reproduces the exact same contention natively, but iterates
 * fast enough that valgrind's own single-instrumented-execution-engine
 * scheduling (no real multi-core parallelism under memcheck) turns the
 * unthrottled spin into an astronomically larger total instrumented
 * instruction count for the same wall-clock test duration; confirmed
 * directly: an earlier, unthrottled version of this exact test still had
 * not finished a single one of its 60 trials after ten minutes under
 * valgrind, despite passing natively in well under a second. Yielding after
 * every call caps this thread's own achievable call rate to whatever the
 * scheduler's own time-slice granularity allows, keeping the race
 * reproducible without paying that multiplier. */
static void *fork_feeder_thread(void *arg) {
  fork_feeder_arg_t *a = (fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
    sched_yield();
  }
  return NULL;
}

/* Regression coverage for chttpserver.c's own pthread_atfork() protection:
 * fork() duplicates only the calling thread, so any of this module's own
 * locks (chttpsvr_slot_table.mutex, srv_engine_bundler.mutex, servers_
 * bundler.mutex, chttpsvr_router_shell_registry.mutex, or any live server's
 * own mutex/idle_mutex/diverted_mutex/routes_lock) held by some OTHER
 * thread at the exact instant of fork() would otherwise be inherited by the
 * child already, and permanently, locked; no thread survives in the child
 * that could ever unlock it, hanging any later chttpsvr_* call in the child
 * forever. Mirrors tests/cthreadpool's own identical fork_does_not_inherit_
 * a_locked_ctpool_mutex test: a yield-throttled chttpsvr_use-calling feeder
 * thread against one shared, long-lived server keeps chttpsvr_slot_table.
 * mutex and that server's own routes_lock under realistic, non-spinning
 * contention while this test repeatedly forks; a small, synchronous,
 * fixed-size burst of real create+destroy cycles immediately before each
 * fork (mirroring the ctpool sibling test's own pre-fork submit burst)
 * additionally exercises chttpsvr_router_shell_registry.mutex and a fresh
 * instance's own mutex/idle_mutex/diverted_mutex/routes_lock, without
 * needing a second continuously-spinning background thread of its own.
 * Each child, bounded by alarm(1) so a real regression here fails this test
 * rather than hanging the whole suite, performs the exact call shape a real
 * application would use right after inheriting a live server handle across
 * a fork (chttpsvr_use on the shared feeder server) and is expected to
 * return promptly either way (its own resolve may legitimately race the
 * parent's own concurrent use of the identical handle, but must never
 * simply hang).
 *
 * A first draft of this test used a second background thread here too,
 * continuously creating and destroying throwaway servers with no throttling
 * at all (mirroring the ctpool sibling test's own churn thread, which is
 * confirmed cheap there). That draft still had not completed a single one
 * of its 60 trials after ten minutes under valgrind, despite passing
 * natively in well under a second; measured directly (not assumed) via a
 * standalone reproduction outside this suite, isolating each background
 * thread in turn: a yield-throttled chttpsvr_use feeder thread alone costs
 * ~0.03s per fork under valgrind, while the unthrottled create/destroy
 * churn thread alone did not complete even one fork within 60 seconds,
 * continuously consuming CPU (not blocked/deadlocked; genuinely still
 * making forward progress, just catastrophically slowly). Root cause:
 * create_chttpsvr_mp's own per-iteration cost (a full root router, several
 * mutexes/condvars, and a derived logger, itself registering into and
 * unregistering from clogger's own separate global slot table) is far
 * heavier than ctpool's own minimal pool creation, and valgrind's memcheck
 * gives threads no real multi-core parallelism at all (it time-slices every
 * thread through one single instrumented execution engine), so an
 * unthrottled loop of this much heavier work does not divide across cores
 * the way it would natively; it just hands valgrind an astronomically
 * larger total instrumented instruction count to simulate for the same
 * wall-clock test duration. Replaced with the synchronous, bounded
 * pre-fork burst below, whose own cost scales with TRIALS rather than with
 * how much of it a background thread manages to cram into an unbounded
 * wall-clock window. TRIALS is deliberately smaller than the ctpool sibling
 * test's own 60, since fork() itself already carries a large, roughly fixed
 * per-call cost under valgrind regardless of trial count; the routes_lock
 * TID-tracking hazard this test also covers (see _chttpsvr_atfork_release_
 * impl's own doc comment) is deterministic, not probabilistic, so it needs
 * no repetition at all to catch; only the "genuinely still locked at the
 * instant of fork()" half of this test's coverage benefits from repeated
 * trials. */
TEST(chttpsvr_handle_lifecycle, fork_does_not_inherit_a_locked_mutex) {
  /* Every intermediate outcome below is captured into a local instead of
   * asserted on immediately with REQUIRE_*, and feeder_tid/feeder_srv are
   * always stopped/joined/destroyed unconditionally before any REQUIRE_*
   * can return early: feeder_tid runs a bare `while (!stop) ...` loop (see
   * fork_feeder_thread's own comment) that only terminates once feeder.stop
   * is set, so an early return here would leave it spinning forever, and
   * feeder_srv (still holding a live shared-engine reference) undestroyed
   * would hang this whole binary's own teardown at process exit. */
  chttpsvr feeder_srv = create_chttpsvr(CLOG_INVALID, NULL);
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
      chttpsvr burst_srv = create_chttpsvr(CLOG_INVALID, NULL);
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
      /* Bounds this child's own lifetime in case the hazard this test
       * guards against somehow still fires, rather than hanging the whole
       * suite; the parent below distinguishes this from a clean exit via
       * WIFEXITED. */
      alarm(1);
      chttpsvr_use(feeder_srv, _noop_middleware_for_lifecycle_tests, NULL);
      _exit(0); /* reached only if the call above returned at all */
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

/* Regression coverage for _chttpsvr_atfork_release_impl's own unconditional
   child-side reset of a server's lifecycle from CHTTPSVR_LC_STOPPING back to
   CHTTPSVR_LC_IDLE (see that function's own doc comment for the full
   account, including why an unconditional reset is safe here specifically,
   unlike quiesce_state above). fork() can land while a parent-side thread is
   genuinely mid-way through _chttpsvr_stop_internal's own real teardown work
   for some server (lifecycle already CHTTPSVR_LC_STOPPING), and that thread
   is not duplicated into the child; without the fixup, chttpsvr_start()'s
   own retry loop (a plain 1ms-sleep poll on this exact state, not a condvar
   wait) spins forever for this handle in the
   child. Reuses g_stop_race_hook (already exercised by tests_engine_stop.c's
   own analogous, single-process scenario) to pause a real chttpsvr_stop()
   call at exactly that point, in the parent, before forking. */
TEST(chttpsvr_handle_lifecycle,
     fork_mid_stop_internal_does_not_hang_start_in_child) {
  g_fork_stop_race_srv = create_chttpsvr(CLOG_INVALID, NULL);
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
     paused right there, strictly before its own event_loop_remove()/close()
     call for the listener. */
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
       below distinguishes this from a clean exit via WIFEXITED. */
    alarm(3);
    chttpsvr_config_t restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 73;
    /* Not asserted on: a genuinely fresh listener on this exact host:port
       may or may not bind cleanly here (the OLD, now-orphaned listener fd
       from the vanished parent-side thread may still be open in this
       child; see this test's own doc comment above), which is an accepted,
       already-documented, gracefully-handled outcome of this fix, not what
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

/* Regression coverage for _chttpsvr_atfork_release_impl's own unconditional
   child-side reset of a server's lifecycle from CHTTPSVR_LC_STARTING back to
   CHTTPSVR_LC_IDLE (see that function's own doc comment for the full
   account, including why an unconditional reset is safe here specifically,
   mirroring the CHTTPSVR_LC_STOPPING case right above). fork() can land
   while a parent-side thread is genuinely mid-way through chttpsvr_start()'s
   own real work for some server (lifecycle already CHTTPSVR_LC_STARTING:
   worker/reject pools created, an engine reference confirmed, but strictly
   before the listener socket itself is ever created), and that thread is
   not duplicated into the child. Without the fixup, this handle is left
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
  g_fork_starting_race_srv = create_chttpsvr(CLOG_INVALID, NULL);
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
    /* pipefd was never opened: nothing of our own to close, but start_th is
     * still paused and the hook still armed; clean those up before the
     * REQUIRE_* below can return early, mirroring every other early-failure
     * path in this test. */
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
       distinguishes a genuine hang from a clean exit via WIFEXITED. The
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
    /* The actual regression this test exists to catch: without the fix,
       this call returns ccol_not_permitted (lifecycle still stuck at
       CHTTPSVR_LC_STARTING from the vanished parent-side thread) instead of
       genuinely being able to proceed. Whether it succeeds outright here is
       not itself asserted on (only that it is not the one specific,
       permanent failure mode this fix closes) since a fresh listener may
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
#endif /* FORK_SAFETY_REQUIRED */

/* Regression coverage for a real bug found via code review: _listener_on_
   readable() receives srv as a bare void* event_loop callback arg, entirely
   outside the ordinary chttpsvr-handle resolve/pin mechanism every other
   entry point into this server goes through. event_loop_remove()'s own
   documented "callers may free whatever the registration's own arg points
   to immediately after this call returns" promise does not cover a
   callback already in progress at the moment of the call (event_loop never
   waits on the registration's own dispatch_lock during removal); this made
   chttpsvr_destroy() capable of freeing srv while a still-running listener
   dispatch kept reading its fields, a window that used to be a handful of
   instructions (real, but never reliably reproducible) until a needed fix
   elsewhere (backing off instead of busy-looping after a persistent
   post-accept allocation failure) widened it into one valgrind could catch
   on demand. Reproduced deterministically here via a dedicated white-box
   race hook rather than relying on winning that same real-timing race. */
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
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
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
     just below blocks on an unbounded cond_var_wait (no deadline, by design
     matched to this hook's every other caller, which all rely on a genuine
     connect() having actually triggered a real dispatch). A failed connect()
     here would mean the listener never dispatches at all, hanging this test
     (and, since the hook would stay armed, every later test in this binary
     that dispatches a listener) forever instead of failing cleanly. */
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
    /* Give chttpsvr_destroy() time to actually run and reach (and, with the
       fix in place, block inside) its own listener_dispatch_pins wait;
       matches this file's own established 150ms precedent for "let the
       other side reach its own blocking point" synchronization elsewhere. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* With the fix, __chttpsvr_destroy() must still be blocked here,
       waiting for listener_dispatch_pins to reach 0; not returned
       already, which (without the fix) would mean it already freed srv
       while the listener dispatch was still paused holding a bare pointer
       to it. */
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

/* Legitimate slot reuse must never be confused with a stale handle to the
 * slot's previous occupant; exactly the scenario a naive address-keyed
 * "remember every destroyed pointer forever" design could not handle
 * safely, since glibc's tcache routinely (though not guaranteedly) reuses a
 * just-freed struct chttpserver's exact address for the very next one
 * allocated. */
TEST(chttpsvr_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpsvr a = create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(a, CHTTPSVR_INVALID);
  chttpsvr stale_a = a;
  chttpsvr_destroy(a);

  chttpsvr b = create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(b, CHTTPSVR_INVALID);

  /* B's operations must succeed normally regardless of whether the
   * allocator happened to reuse A's exact address for B. */
  REQUIRE_EQ(chttpsvr_use(b, _noop_middleware_for_lifecycle_tests, NULL),
             ccol_success);

  /* A's stale handle must never resolve to B, even if it reused the same
   * underlying address; the whole point of the generation counter. */
  REQUIRE_EQ((void *)_chttpsvr_resolve_for_tests(stale_a), NULL);

  chttpsvr_destroy(b);
}

/* The slot table is bounded, not ever-growing: a create/destroy churn loop
 * with only a single slot ever in flight at a time must reuse that one
 * freed slot on every iteration rather than growing the table further.
 * Captures capacity right after the first create/destroy pair (rather than
 * asserting a fixed absolute value) since other tests earlier in this same
 * process may have already grown the table to some N > 1; what this test
 * actually needs to prove is that ITS OWN churn adds no further growth. */
TEST(chttpsvr_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 100 };

  chttpsvr s0 = create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_NE(s0, CHTTPSVR_INVALID);
  chttpsvr_destroy(s0);
  size_t capacity_after_first = _chttpsvr_slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpsvr srv = create_chttpsvr(CLOG_INVALID, NULL);
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
   * regardless of elem_size, mirroring every caller's own "cap ? cap*2 : 8"
   * shape before this helper existed. */
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
  /* cap * 2 itself wraps past SIZE_MAX; must return 0 (the documented
   * "cannot grow" sentinel), matching this project's own established
   * SIZE_MAX-relative guard idiom used throughout this file (e.g.
   * _router_add_route/chttpsvr_subrouter/chttpsvr_resp_set_header/
   * _parse_qparams' own identical checks). */
  REQUIRE_EQ(
      _chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX, sizeof(void *), 8),
      (size_t)0);
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(SIZE_MAX / 2 + 1,
                                                     sizeof(void *), 8),
             (size_t)0);
}

TEST(doubling_growth_cap, byte_size_multiplication_overflow_rejected) {
  /* cap * 2 alone would not overflow, but the caller's own subsequent
   * new_cap * elem_size multiplication would; must still return 0 rather
   * than a new_cap that looks valid on its own but silently under-sizes the
   * real allocation once multiplied by elem_size. */
  size_t cap = SIZE_MAX / 2; /* cap*2 == SIZE_MAX-1, no overflow on its own */
  REQUIRE_EQ(_chttpsvr_doubling_growth_cap_for_tests(cap, 4096, 8), (size_t)0);
}

/* ========================================================================== */
/*             ACCEPT()-FAILURE ERRNO CLASSIFICATION                          */
/*                                                                            */
/* _listener_on_readable's own accept4() error handling used to treat every  */
/* unexpected failure identically: no log, an immediate return with no       */
/* retry. For a *persistent* resource-exhaustion condition (EMFILE/ENFILE/   */
/* ENOBUFS/ENOMEM: the process or system genuinely out of file descriptors,  */
/* a realistic state under sustained connection-flood load with a modest    */
/* ulimit -n), the listen backlog stays non-empty (nothing was ever          */
/* accepted), so the reactor's own level-triggered epoll immediately         */
/* re-dispatches this same handler again; an unbounded, silent busy-loop     */
/* burning 100% CPU with zero operator-visible diagnostics for as long as    */
/* the condition persists. Fixed with two errno-classification helpers (a   */
/* brief backoff sleep plus a rate-limited log for the resource-exhaustion   */
/* class; an immediate retry, not a fresh epoll round-trip, for a genuinely  */
/* transient single-connection error like ECONNABORTED). Exercising the     */
/* real EMFILE/ECONNABORTED conditions end to end would require mutating    */
/* this whole test process's own fd limits or kernel-level connection       */
/* state, environment-dependent and disproportionate for testing simple,    */
/* deterministic integer classification; these tests exercise that          */
/* classification directly via a white-box hook instead.                    */
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
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENONET));
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
  REQUIRE_TRUE(_chttpsvr_accept_errno_is_transient_for_tests(ENOSR));
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
