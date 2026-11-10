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

// For pthread_timedjoin_np, which _bounded_join uses. It must come before any
// include that can pull in <pthread.h>.
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
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>
#include <test_wait.h>

TAU_MAIN()

// Ignores SIGPIPE and resets the rest of the inherited signal state, before any
// thread starts. The test clients write with plain write(2) and the server can
// close a connection in the middle of one, which would otherwise kill the
// binary; the library leaves SIGPIPE to the application (tests_sigpipe.c covers
// the default disposition).
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

// The fixed port of g_srv and its URL. The other servers of _setup listen on
// TEST_PORT + 2 and TEST_PORT + 3, so two runs of this binary can't overlap.
#define TEST_PORT 18765
#define BASE_URL "http://127.0.0.1:18765"

// The logger of every server in this file, opened by _setup and closed by
// _teardown.
static clog g_test_logger = CLOG_NULL;
// The main test server on TEST_PORT, with every route that _setup registers.
static chttpsvr g_srv = CHTTPSVR_NULL;

// Closes *fd unless it is negative. Tests declare their client sockets with it
// as the ccol__destructor cleanup, so a REQUIRE_* that returns early doesn't
// leak the fd for the rest of the run.
static void _close_scoped_fd(int *fd) {
  if (fd && *fd >= 0) close(*fd);
}

// Joins tid, waiting at most 30 s where pthread_timedjoin_np works. The
// race-hook tests join their released threads with it, so a regression that
// leaves a thread stuck fails one test instead of hanging the binary. Returns
// true only when the thread was joined (its result in *retval); on false the
// caller must pthread_detach() it.
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

// Writes "Hello, world!". Many routes of this file use it when all a test needs
// is a known answer.
static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, world!");
}

// Echoes the request body, or "(empty)". The body tests (/echo-body, the small
// and unlimited body servers, the carry-over tests) use it to check what
// arrived.
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

// Writes "id=<id> sub=<sub>" from the path params, "(nil)" for a missing one.
// It serves /params/{id}/{sub} for the path param tests.
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

// Writes ctx, a literal string. The segment-decode cache tests register it on
// several routes with one decoded prefix and tell from the answer which route
// matched.
static void _cache_literal_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

// Writes "<ctx>:<id param>". The cache tests use it to show that the param
// reads back correctly after other candidate routes failed at a later segment.
static void _cache_param_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  const char *id = chttpsvr_req_param(req, "id");
  char buf[256];
  int n =
      snprintf(buf, sizeof(buf), "%s:%s", (const char *)ctx, id ? id : "(nil)");
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

// Writes every value of the query key "q", separated by commas, or "no_q". It
// serves /query for the query parsing tests.
static void _echo_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  size_t n = 0;
  const char **vals = chttpsvr_req_query(req, "q", &n);
  if (!vals || n == 0) {
    chttpsvr_resp_write_str(resp, "no_q");
    return;
  }
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

// Echoes the x-test-header request header, or "(none)". It serves /header for
// the request header tests.
static void _echo_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "x-test-header");
  chttpsvr_resp_write_str(resp, val ? val : "(none)");
}

// Sets the response header x-custom and writes "ok", for
// custom_response_header.
static void _set_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-custom", "my-value");
  chttpsvr_resp_write_str(resp, "ok");
}

// Answers 201 with "created", for custom_status_code.
static void _status_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_CREATED);
  chttpsvr_resp_write_str(resp, "created");
}

// Writes {"ok":true} with chttpsvr_resp_write_json, for json_response.
static void _json_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  const char *body = "{\"ok\":true}";
  chttpsvr_resp_write_json(resp, body, strlen(body));
}

// Writes "item:<id>". It serves /items/{id} of the /api/v1 sub-router.
static void _api_items_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *id = chttpsvr_req_param(req, "id");
  char buf[64];
  snprintf(buf, sizeof(buf), "item:%s", id ? id : "nil");
  chttpsvr_resp_write_str(resp, buf);
}

// Writes "api_root". It serves the root of the /api/v1 sub-router.
static void _api_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "api_root");
}

// Middleware call counters. They are _Atomic because the server threads write
// them and the test thread reads them.
static _Atomic int g_global_mw_count = 0;
static _Atomic int g_router_mw_count = 0;

// Counts the call in g_global_mw_count, sets x-global-mw and calls next. It is
// the global middleware of g_srv, for the middleware tests.
static void _global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
  (void)ctx;
  g_global_mw_count++;
  chttpsvr_resp_set_header(resp, "x-global-mw", "1");
  next(req, resp);
}

// Counts the call in g_router_mw_count, sets x-router-mw and calls next. It is
// the middleware of the /api/v1 sub-router.
static void _router_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                       chttpsvr_next_fn next) {
  (void)ctx;
  g_router_mw_count++;
  chttpsvr_resp_set_header(resp, "x-router-mw", "1");
  next(req, resp);
}

// Answers 403 "blocked" itself and never calls next. It guards the
// /blocked-area sub-router, for middleware_short_circuit.
static void _blocking_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                         chttpsvr_next_fn next) {
  (void)req;
  (void)ctx;
  (void)next;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_FORBIDDEN);
  chttpsvr_resp_write_str(resp, "blocked");
}

// Counters of two middlewares on one router level, to check that both run in
// registration order.
static _Atomic int g_mw_a_count = 0;
static _Atomic int g_mw_b_count = 0;

// Counts the call in g_mw_a_count, sets x-mw-a and calls next. It is the first
// middleware of the /mw-chain sub-router.
static void _mw_a(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                  chttpsvr_next_fn next) {
  (void)ctx;
  g_mw_a_count++;
  chttpsvr_resp_set_header(resp, "x-mw-a", "1");
  next(req, resp);
}

// Counts the call in g_mw_b_count, sets x-mw-b and calls next. It is the second
// middleware of the /mw-chain sub-router.
static void _mw_b(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                  chttpsvr_next_fn next) {
  (void)ctx;
  g_mw_b_count++;
  chttpsvr_resp_set_header(resp, "x-mw-b", "1");
  next(req, resp);
}

// Echoes the body as it reads it. Most streaming routes of this file use it
// when a test needs a working streaming handler.
static void _stream_echo_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  char buf[256];
  ssize_t n;
  while ((n = chttpsvr_req_read(req, buf, sizeof(buf))) > 0) {
    chttpsvr_resp_write(resp, buf, (size_t)n);
  }
}

// Counts the chttpsvr_req_read() calls that returned data and reports the count
// in x-batch-count. It shows that a streaming body arrives in batches as the
// bytes come off the wire (streaming_body_delivered_in_separate_batches).
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

// Drains the body and reports in "x-stream-err" why the last read returned -1,
// or "none". The tests of stream_read_timeout_us, of an abort mid-stream and of
// body limits on streaming routes read the reason from it.
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

// Reports in x-trailer-value what chttpsvr_req_header says for "X-Trailer",
// which the client sends only as a trailer of a chunked body. It pins that a
// trailer isn't a request header: it reads as absent even though the whole body
// is read before a buffered handler runs.
static void _buffered_trailer_echo_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "X-Trailer");
  chttpsvr_resp_set_header(resp, "x-trailer-value", val ? val : "(absent)");
  chttpsvr_resp_write_str(resp, "done");
}

// Reports what chttpsvr_req_header says for "X-Trailer" before and after it
// drains the streaming body. It is the streaming form of the trailer check, and
// both answers must be "(absent)".
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

// Echoes X-Forwarded-For and X-Authenticated-User in response headers. The
// client sends each as a header and again as a trailer with another value, to
// check that a trailer can't replace a header that a proxy set.
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

// 16 MiB, far more than the send buffer and the window of a client that never
// reads, so writing it must block on backpressure.
#define _CHTTPSVR_TEST_LARGE_BODY_SIZE (16 * 1024 * 1024)

// How many times _large_response_handler has started.
static atomic_int g_large_response_runs = 0;

// Counts its run in g_large_response_runs and writes a body of
// _CHTTPSVR_TEST_LARGE_BODY_SIZE bytes. The max_response_write_duration_us
// tests send it to a client that doesn't read.
static void _large_response_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  atomic_fetch_add(&g_large_response_runs, 1);
  static char big_buf[_CHTTPSVR_TEST_LARGE_BODY_SIZE];
  static bool filled = false;
  if (!filled) {
    memset(big_buf, 'x', sizeof(big_buf));
    filled = true;
  }
  chttpsvr_resp_write(resp, big_buf, sizeof(big_buf));
}

// Answers 401 without reading the body. It is the case Expect: 100-continue
// exists for, and the lingering close tests use it to refuse an upload.
static void _stream_reject_without_reading_handler(chttpsvr_req *req,
                                                   chttpsvr_resp *resp,
                                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "no thanks");
}

// Reads one small batch of the body and answers "early-stop". It shows that the
// connection is safe for the next request when a handler leaves part of the
// body unread.
static void _stream_read_once_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[8];
  chttpsvr_req_read(req, buf, sizeof(buf));
  chttpsvr_resp_write_str(resp, "early-stop");
}

// Echoes the raw query, or "(none)", for the raw query tests.
static void _raw_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *rq = chttpsvr_req_raw_query(req);
  chttpsvr_resp_write_str(resp, rq ? rq : "(none)");
}

// Echoes the decoded path, for the path and URL decoding tests.
static void _path_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, chttpsvr_req_path(req));
}

// Writes what chttpsvr_req_query_one gives for "q": the value, "not_found",
// "multi_value" or "error". It serves /query-one for the query_one tests.
static void _query_one_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  const char *val = NULL;
  ccol_retval rv = chttpsvr_req_query_one(req, "q", &val);
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

// Echoes x-stream-test on a streaming route, where the header comes from the
// pre-extracted header array (the streaming_header tests).
static void _stream_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *val = chttpsvr_req_header(req, "x-stream-test");
  chttpsvr_resp_write_str(resp, val ? val : "(none)");
}

// Writes the body "foobarbaz" in three calls, for multi_write_accumulates.
static void _multi_write_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "foo");
  chttpsvr_resp_write_str(resp, "bar");
  chttpsvr_resp_write_str(resp, "baz");
}

// Writes the body with chttpsvr_resp_printf: one call that fits its stack
// buffer and forty more that add up past it, for printf_accumulates.
static void _printf_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_printf(resp, "n=%d s=%s f=%.2f ", 42, "hi", 3.5);
  for (int i = 0; i < 40; i++) {
    chttpsvr_resp_printf(resp, "%08d-", i);
  }
}

// Writes "a=<a> b=<b>" from two queries in one request, to check that the query
// scratch array is reused correctly.
static void _multi_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  size_t na = 0, nb = 0;
  const char **va = chttpsvr_req_query(req, "a", &na);
  const char *a_val = (va && na > 0) ? va[0] : "nil";
  const char **vb = chttpsvr_req_query(req, "b", &nb);
  const char *b_val = (vb && nb > 0) ? vb[0] : "nil";
  char buf[128];
  int n = snprintf(buf, sizeof(buf), "a=%s b=%s", a_val, b_val);
  if (n < 0 || (size_t)n >= sizeof(buf)) n = (int)(sizeof(buf) - 1);
  chttpsvr_resp_write(resp, buf, (size_t)n);
}

// Writes what chttpsvr_req_read() returns on a buffered route, which must be -1
// (req_read_on_buffered_returns_minus_one).
static void _read_on_buffered_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

// Writes what chttpsvr_req_body() gives on a streaming route. It must give NULL
// and 0 there, so the answer is "(empty)".
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

// Reads part of the streaming body, then writes what chttpsvr_req_body() gives.
// It checks that the answer is still NULL and 0 and not the state the read left
// behind.
static void _stream_body_after_read_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[8];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  size_t len = (size_t)-1;
  const void *body = chttpsvr_req_body(req, &len);
  char s[64];
  snprintf(s, sizeof(s), "read=%ld body=%s len=%zu", (long)n,
           body ? "non-null" : "null", len);
  chttpsvr_resp_write_str(resp, s);
}

// Writes the result of a streaming read with buflen 0, to check that it is 0
// and not -1.
static void _stream_zero_buflen_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  char buf[4];
  ssize_t n = chttpsvr_req_read(req, buf, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

// Writes the result of the first streaming read of a request with no body, to
// check that it is 0 (EOF).
static void _stream_read_empty_body_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  char buf[16];
  ssize_t n = chttpsvr_req_read(req, buf, sizeof(buf));
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

// Sets x-dup twice, to check that the second value wins.
static void _dup_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-dup", "first");
  chttpsvr_resp_set_header(resp, "x-dup", "second");
  chttpsvr_resp_write_str(resp, "ok");
}

// Sets status 0, to check that the server turns it into 500.
static void _bad_status_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 0);
  chttpsvr_resp_write_str(resp, "bad");
}

// Writes the code that chttpsvr_req_query_one returns for a NULL key, for its
// argument guard test.
static void _query_one_null_key_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)ctx;
  const char *val = NULL;
  ccol_retval rv = chttpsvr_req_query_one(req, NULL, &val);
  char s[16];
  snprintf(s, sizeof(s), "%d", (int)rv);
  chttpsvr_resp_write_str(resp, s);
}

// Writes "ok" when chttpsvr_req_query_one with a NULL val_out returns
// ccol_success for a present key, and "fail" otherwise.
static void _query_one_null_val_out_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ccol_retval rv = chttpsvr_req_query_one(req, "q", NULL);
  chttpsvr_resp_write_str(resp, rv == ccol_success ? "ok" : "fail");
}

// Writes the result of a streaming read into a NULL buffer, to check that it is
// -1.
static void _stream_null_buf_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 4);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

// Writes the result of chttpsvr_req_read(req, NULL, 0), to check that the zero
// length wins over the NULL buffer and gives 0.
static void _stream_null_buf_zero_len_handler(chttpsvr_req *req,
                                              chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  ssize_t n = chttpsvr_req_read(req, NULL, 0);
  char s[32];
  snprintf(s, sizeof(s), "%ld", (long)n);
  chttpsvr_resp_write_str(resp, s);
}

// Answers 204 with one header and no body, to check a response that is a head
// alone.
static void _headers_only_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                  void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_NO_CONTENT);
  chttpsvr_resp_set_header(resp, "x-no-body", "1");
}

// Writes a body, then sets the status from "x-force-status" (200 by default)
// and, with "x-force-cl", its own Content-Length. The 1xx, 204 and 304 tests
// use it to check that the server drops the body for a status that must not
// carry one.
static void _forced_status_with_body_handler(chttpsvr_req *req,
                                             chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, "this-body-must-never-reach-the-wire");
  const char *forced = chttpsvr_req_header(req, "x-force-status");
  chttpsvr_resp_set_status(resp, forced ? atoi(forced) : 200);
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
}

// Answers on a HEAD route with no body and, with "x-force-cl", its own
// Content-Length, for the HEAD Content-Length tests.
static void _bodyless_head_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *cl = chttpsvr_req_header(req, "x-force-cl");
  if (cl) chttpsvr_resp_set_header(resp, "Content-Length", cl);
  chttpsvr_resp_set_header(resp, "x-explicit-head", "1");
}

// Tries a list of Content-Length values through both header calls and writes
// 'y' (ccol_success) or 'n' (ccol_invalid_args) for each, to pin which
// spellings the setters accept. The body it writes then replaces whatever value
// was accepted last.
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

// Builds a head with repeated names through chttpsvr_resp_add_header and
// chttpsvr_resp_set_header, for resp_add_header_keeps_every_field_in_order.
// Writes "bad=<n>", the number of calls whose return was not the expected one.
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
  bad += chttpsvr_resp_set_header(resp, "x-single", "final") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "Set-Cookie", "c=3") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "Date", "d1") != ccol_success;
  bad += chttpsvr_resp_add_header(resp, "date", "d2") != ccol_success;
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

// Sets the Connection header to the value of "x-force-connection", which may
// disagree with what the server decides. The tests check that the wire matches
// what the server really does with the connection.
static void _explicit_connection_header_handler(chttpsvr_req *req,
                                                chttpsvr_resp *resp,
                                                void *ctx) {
  (void)ctx;
  const char *forced = chttpsvr_req_header(req, "x-force-connection");
  if (forced) chttpsvr_resp_set_header(resp, "Connection", forced);
  chttpsvr_resp_write_str(resp, "explicit-connection-header-ok");
}

// Sets twenty headers of 300 bytes each, to check that the server grows its
// head buffer instead of failing.
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

// Writes "second". It is the second registration of an existing path and
// method, which must never run because the first registration wins.
static void _dup_second_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "second");
}

// Writes "null" or "non-null" for chttpsvr_req_header(req, NULL), for its
// argument guard test.
static void _null_name_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_header(req, NULL);
  chttpsvr_resp_write_str(resp, v == NULL ? "null" : "non-null");
}

// Writes "null" when chttpsvr_req_query(req, NULL, &n) gives NULL and 0, and
// "non-null" otherwise, for its argument guard test.
static void _null_key_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)ctx;
  size_t n = 99;
  const char **v = chttpsvr_req_query(req, NULL, &n);
  chttpsvr_resp_write_str(resp, (v == NULL && n == 0) ? "null" : "non-null");
}

// A second server for the "already running" test and the registration edge
// cases. It never serves.
static chttpsvr g_srv2 = CHTTPSVR_NULL;

// Results of the guard handlers below, one array per route of _setup. A test
// resets its array before the request. They are _Atomic because the handler
// thread writes them and the test thread reads them.
static _Atomic int g_null_guard_results[2];
static _Atomic int g_null_data_results[2];
static _Atomic int g_null_hdr_results[2];
static _Atomic int g_crlf_hdr_results[3];
static _Atomic int g_bare_crlf_hdr_results[3];
static _Atomic int g_disallowed_hdr_results[4];
static _Atomic int g_non_tchar_hdr_results[4];
static _Atomic int g_json_zero_len_result;

// Stores in the ctx array what chttpsvr_resp_write_str returns for a NULL resp
// and a NULL string, then answers "ok". Its test checks the argument guards
// through a real request, since a resp exists only inside a handler.
static void _null_guard_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_write_str(NULL, "hi");
  results[1] = (int)chttpsvr_resp_write_str(resp, NULL);
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_write returns for NULL data with a
// length of 5 and of 0, then answers "ok", for the write guard test.
static void _resp_write_null_guard_handler(chttpsvr_req *req,
                                           chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_write(resp, NULL, 5);
  results[1] = (int)chttpsvr_resp_write(resp, NULL, 0);
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_set_header returns for a NULL name
// and a NULL value, then answers "ok", for the setter guard test.
static void _set_header_null_guards_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, NULL, "v");
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-null-v", NULL);
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_set_header returns for a CRLF in
// the name, a CRLF in the value and a legal pair, then answers "ok". Its test
// checks that header injection is refused.
static void _set_header_crlf_guards_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] =
      (int)chttpsvr_resp_set_header(resp, "x-evil\r\nx-injected: evil", "v");
  results[1] = (int)chttpsvr_resp_set_header(
      resp, "x-evil", "v\r\n\r\nHTTP/1.1 200 OK\r\nx-injected: evil");
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_set_header returns for a bare CR,
// a bare LF and a legal pair, then answers "ok", for the bare CR and LF test.
static void _set_header_bare_cr_lf_guards_handler(chttpsvr_req *req,
                                                  chttpsvr_resp *resp,
                                                  void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, "x-bare-cr", "v\rinjected");
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-bare-lf", "v\ninjected");
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_set_header returns for an empty
// name, two spellings of Transfer-Encoding and a legal pair, then answers "ok".
// The server frames the body itself, so a handler can't set those names.
static void _set_header_disallowed_name_guards_handler(chttpsvr_req *req,
                                                       chttpsvr_resp *resp,
                                                       void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, "", "v");
  results[1] =
      (int)chttpsvr_resp_set_header(resp, "transfer-encoding", "chunked");
  results[2] =
      (int)chttpsvr_resp_set_header(resp, "Transfer-Encoding", "chunked");
  results[3] = (int)chttpsvr_resp_set_header(resp, "x-legit", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in the ctx array what chttpsvr_resp_set_header returns for names with
// a space, a colon and a DEL, and for a name made of every tchar, then answers
// "ok", for the field name grammar test.
static void _set_header_non_tchar_name_guards_handler(chttpsvr_req *req,
                                                      chttpsvr_resp *resp,
                                                      void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, "X Foo", "bar");
  results[1] = (int)chttpsvr_resp_set_header(resp, "X:Foo", "bar");
  results[2] = (int)chttpsvr_resp_set_header(resp,
                                             "X\x7F"
                                             "Foo",
                                             "bar");
  results[3] =
      (int)chttpsvr_resp_set_header(resp, "x-legit!#$%&'*+-.^_`|~", "fine");
  chttpsvr_resp_write_str(resp, "ok");
}

// Stores in *ctx what chttpsvr_resp_write_json returns for a length of 0, for
// resp_write_json_zero_len_rejected.
static void _json_zero_len_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  _Atomic int *result = (_Atomic int *)ctx;
  *result = (int)chttpsvr_resp_write_json(resp, "{}", 0);
  chttpsvr_resp_write_str(resp, "done");
}

// Writes "root_subrouter". It serves the "/" sub-router, which matches only the
// root path.
static void _root_subrouter_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                    void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root_subrouter");
}

// Writes the first value of the empty query key, or "not_found", for
// query_empty_key.
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

// Writes the value counts of "a", "b" and "". Its test checks that a stray '&'
// or "&&" in the query makes no entry with an empty key.
static void _query_stray_amp_counts_handler(chttpsvr_req *req,
                                            chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t na = 0, nb = 0, nempty = 0;
  chttpsvr_req_query(req, "a", &na);
  chttpsvr_req_query(req, "b", &nb);
  chttpsvr_req_query(req, "", &nempty);
  chttpsvr_resp_printf(resp, "%zu,%zu,%zu", na, nb, nempty);
}

// Writes the results of two streaming reads, to show that EOF repeats.
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

// Writes "null" or "non-null" for chttpsvr_req_param(req, NULL), for its
// argument guard test.
static void _param_null_name_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_param(req, NULL);
  chttpsvr_resp_write_str(resp, v == NULL ? "null" : "non-null");
}

// Writes what chttpsvr_req_body() gives on a streaming GET with no body, to
// check that it is NULL and 0 ("(empty)").
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

// Marks its response with x-head-route. It is the HEAD route of /head-pref, so
// a test can tell whether the GET or the HEAD route ran.
static void _explicit_head_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-head-route", "1");
  chttpsvr_resp_write_str(resp, "head-route");
}

// Writes "root-wins". It and _shadow_sub_handler are both registered on
// /shadow-test/ping, on g_srv and on a /shadow-test sub-router, to check which
// router the server asks first.
static void _shadow_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root-wins");
}
// Writes "sub-wins", for the sub-router route of /shadow-test/ping.
static void _shadow_sub_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "sub-wins");
}

// Echoes the dispatched method. The CHTTP_ANY tests use it to see which method
// a catch-all route got.
static void _any_method_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)ctx;
  chttpsvr_resp_write_str(resp, chttp_method_str(chttpsvr_req_method(req)));
}

// Writes "<method>:<id>", for the CHTTP_ANY route with a path param.
static void _any_method_param_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *id = chttpsvr_req_param(req, "id");
  char buf[64];
  snprintf(buf, sizeof(buf), "%s:%s",
           chttp_method_str(chttpsvr_req_method(req)), id ? id : "nil");
  chttpsvr_resp_write_str(resp, buf);
}

// A server with one worker and a queue of one on TEST_PORT + 2. A third
// concurrent request there must get a 503.
static chttpsvr g_bounded_srv = CHTTPSVR_NULL;

// A server with max_body_size SMALL_BODY_MAX on TEST_PORT + 3, for the 413
// boundary tests.
static chttpsvr g_small_body_srv = CHTTPSVR_NULL;
#define SMALL_BODY_MAX 64

// Hold the requests of the 503 test inside _bounded_blk_handler until a test
// (or the teardown) sets g_blk_go. g_blk_count counts the requests held there.
static pthread_mutex_t g_blk_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_blk_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_blk_count = 0;
static bool g_blk_go = false;

// Waits under g_blk_mtx until g_blk_go is set, then answers "ok". The 503 test
// fills g_bounded_srv's worker and queue with it, and watches g_blk_count to
// know when a request is held.
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

// The client of the 503 test. Its own pool has room for every request; the
// default client allows one connection per CPU, which isn't enough on a machine
// with two CPUs.
static chttpcli g_bounded_cli = CHTTPCLI_NULL;

// Sends one GET /bounded-503 to g_bounded_srv through g_bounded_cli, for the
// 503 test. Returns the response, or NULL.
static chttpcli_response *_bounded_get(void) {
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/bounded-503", TEST_PORT + 2);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  chttpcli_response *resp = NULL;
  if (req) chttpcli_do(g_bounded_cli, req, &resp);
  chttp_request_free(req);
  return resp;
}

// Thread entry: sends one GET /bounded-503 and throws the response away. The
// 503 test fills the pool from such threads.
static void *_send_bounded_req(void *arg) {
  (void)arg;
  chttpcli_response *resp = _bounded_get();
  if (resp) chttpcli_resp_free(resp);
  return NULL;
}

// Thread entry: sends a GET that g_srv answers with 404 and stores the status,
// or -1, in the int that arg points to. It makes the burst of rejections of
// concurrent_route_rejections_do_not_starve_other_requests.
static void *_send_unmatched_route_req(void *arg) {
  chttpcli_response *resp = NULL;
  chttp_get(BASE_URL "/definitely-not-a-registered-route-xyz", &resp);
  *(int *)arg = resp ? resp->status_code : -1;
  if (resp) chttpcli_resp_free(resp);
  return NULL;
}

// Releases the blocked handlers, stops and destroys every server of _setup,
// then waits for the engine to stop and closes g_test_logger. _setup registers
// it as the exit handler. The wait comes first because the engine's threads may
// still use the logger. On OpenBSD an exit handler runs while the loader holds
// a lock that pthread_join needs, so the wait and the close are skipped there.
static void _teardown(void) {
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  if (g_bounded_srv) chttpsvr_stop(g_bounded_srv);
  if (g_small_body_srv) chttpsvr_stop(g_small_body_srv);
  if (g_srv) chttpsvr_stop(g_srv);
  if (g_srv2) chttpsvr_stop(g_srv2);

  if (g_bounded_srv) {
    chttpsvr__destroy(g_bounded_srv);
    g_bounded_srv = CHTTPSVR_NULL;
  }
  if (g_small_body_srv) {
    chttpsvr__destroy(g_small_body_srv);
    g_small_body_srv = CHTTPSVR_NULL;
  }
  if (g_srv) {
    chttpsvr__destroy(g_srv);
    g_srv = CHTTPSVR_NULL;
  }
  if (g_srv2) {
    chttpsvr__destroy(g_srv2);
    g_srv2 = CHTTPSVR_NULL;
  }
#if !defined(__OpenBSD__)
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = CLOG_NULL;
  }
#endif
}

// Builds and starts g_bounded_srv, for _setup and _restore_unused_servers.
// Returns false, with no server left, when the create or the start fails.
static bool _create_bounded_srv(void) {
  g_bounded_srv = chttpsvr_create(g_test_logger, NULL);
  if (!g_bounded_srv) return false;
  chttpsvr_register_streaming_handler(g_bounded_srv, CHTTP_GET, "/bounded-503",
                                      _bounded_blk_handler, NULL);
  chttpsvr_register_streaming_handler(g_bounded_srv, CHTTP_POST,
                                      "/stream-error-report-slow",
                                      _stream_error_report_handler, NULL);
  chttpsvr_config bcfg = CHTTPSVR_CONFIG_DEFAULT;
  bcfg.host = "127.0.0.1";
  bcfg.port = TEST_PORT + 2;
  bcfg.worker_thread_count = 1;
  bcfg.worker_queue_capacity = 1;
  bcfg.streaming_thread_count = CHTTPSVR_STREAMING_POOL_OFF;
  bcfg.stream_read_timeout_us = 300000;
  ccol_retval brv = chttpsvr_start(g_bounded_srv, &bcfg);
  if (brv != ccol_success) {
    fprintf(stderr, "bounded chttpsvr_start failed: %d\n", brv);
    chttpsvr_destroy(g_bounded_srv);
    return false;
  }
  return true;
}

// Builds and starts g_small_body_srv, for _setup and _restore_unused_servers.
// Returns false, with no server left, when the create or the start fails.
static bool _create_small_body_srv(void) {
  g_small_body_srv = chttpsvr_create(g_test_logger, NULL);
  if (!g_small_body_srv) return false;
  chttpsvr_register_handler(g_small_body_srv, CHTTP_POST, "/small-body-echo",
                            _echo_body_handler, NULL);
  chttpsvr_register_streaming_handler(g_small_body_srv, CHTTP_POST,
                                      "/small-body-stream",
                                      _stream_error_report_handler, NULL);
  chttpsvr_config scfg = CHTTPSVR_CONFIG_DEFAULT;
  scfg.host = "127.0.0.1";
  scfg.port = TEST_PORT + 3;
  scfg.max_body_size = SMALL_BODY_MAX;
  ccol_retval srv_rv = chttpsvr_start(g_small_body_srv, &scfg);
  if (srv_rv != ccol_success) {
    fprintf(stderr, "small-body chttpsvr_start failed: %d\n", srv_rv);
    chttpsvr_destroy(g_small_body_srv);
    return false;
  }
  return true;
}

// Releases the blocked handlers, then stops and destroys g_srv2, g_bounded_srv
// and g_small_body_srv, and returns 0. The fork handlers lock every live server
// (five locks each), pool, logger and event loop at once, and ThreadSanitizer
// stops a thread that holds 64 locks, so a test that forks drops the servers it
// doesn't use through FORK_TEST_SCOPE.
static int _release_unused_servers(void) {
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);
  if (g_srv2) chttpsvr_stop(g_srv2);
  if (g_bounded_srv) chttpsvr_stop(g_bounded_srv);
  if (g_small_body_srv) chttpsvr_stop(g_small_body_srv);
  if (g_srv2) chttpsvr_destroy(g_srv2);
  if (g_bounded_srv) chttpsvr_destroy(g_bounded_srv);
  if (g_small_body_srv) chttpsvr_destroy(g_small_body_srv);
  return 0;
}

// Builds again the servers that _release_unused_servers destroyed, as the
// cleanup of FORK_TEST_SCOPE, so the tests after a fork test find them. It ends
// the run when it can't, because every later test would fail for no good
// reason.
static void _restore_unused_servers(int *scope) {
  (void)scope;
  if (!g_srv2) g_srv2 = chttpsvr_create(g_test_logger, NULL);
  bool ok = g_srv2 && (g_bounded_srv || _create_bounded_srv()) &&
            (g_small_body_srv || _create_small_body_srv());
  if (!ok) {
    fprintf(stderr, "FATAL: could not build the servers of _setup again\n");
    exit(1);
  }
}

// Declares an unused int whose initializer calls _release_unused_servers and
// whose cleanup calls _restore_unused_servers. A test that forks puts it first,
// so the fork holds fewer locks and the servers of _setup come back when the
// test's scope ends. It takes no arguments.
#define FORK_TEST_SCOPE()                          \
  __attribute__((cleanup(_restore_unused_servers), \
                 unused)) int _fork_test_scope = _release_unused_servers()

// Builds every server and route of this binary before the first test: raises
// the fd soft limit (some tests hold hundreds of connections), opens
// g_test_logger, registers the routes of g_srv and its sub-routers, installs
// the engine logger and starts g_srv, g_bounded_srv and g_small_body_srv.
// chttpsvr_start binds synchronously, so no readiness poll is needed. Any
// failure ends the run.
__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  struct rlimit nofile;
  if (getrlimit(RLIMIT_NOFILE, &nofile) == 0 && nofile.rlim_cur < 4096) {
    nofile.rlim_cur = nofile.rlim_max == RLIM_INFINITY || nofile.rlim_max > 4096
                          ? 4096
                          : nofile.rlim_max;
    (void)setrlimit(RLIMIT_NOFILE, &nofile);
  }

  g_test_logger = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }

  g_srv = chttpsvr_create(g_test_logger, &err);
  if (!g_srv) {
    fprintf(stderr, "FATAL: could not create chttpsvr: %s\n", err ? err : "?");
    exit(1);
  }

  chttpsvr_use(g_srv, _global_mw, NULL);

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
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/echo-path/{v}", _path_handler,
                            NULL);

  chttpsvr_register_handler(g_srv, CHTTP_POST, "/req-read-buffered",
                            _read_on_buffered_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dual", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/dual", _echo_body_handler,
                            NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-header",
                            _dup_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/bad-status",
                            _bad_status_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-key",
                            _query_one_null_key_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-one-null-val-out",
                            _query_one_null_val_out_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/dup-first-wins",
                            _dup_second_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-name-header",
                            _null_name_header_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/null-key-query",
                            _null_key_query_handler, NULL);

  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-echo",
                                      _stream_echo_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_GET, "/stream-header",
                                      _stream_header_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST, "/stream-body-api",
                                      _stream_body_check_handler, NULL);
  chttpsvr_register_streaming_handler(g_srv, CHTTP_POST,
                                      "/stream-body-after-read",
                                      _stream_body_after_read_handler, NULL);
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

  chttpsvr_router *blocked_r = chttpsvr_subrouter(g_srv, "/blocked-area");
  if (!blocked_r) {
    fprintf(stderr, "FATAL: could not create blocked subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(blocked_r, _blocking_mw, NULL);
  chttpsvr_router_on(blocked_r, CHTTP_GET, "/secret", _hello_handler, NULL);

  chttpsvr_router *mw_chain = chttpsvr_subrouter(g_srv, "/mw-chain");
  if (!mw_chain) {
    fprintf(stderr, "FATAL: could not create mw-chain subrouter\n");
    exit(1);
  }
  chttpsvr_router_use(mw_chain, _mw_a, NULL);
  chttpsvr_router_use(mw_chain, _mw_b, NULL);
  chttpsvr_router_on(mw_chain, CHTTP_GET, "/ping", _hello_handler, NULL);

  chttpsvr_router *api_v3 = chttpsvr_subrouter(g_srv, "/api/v3/");
  if (!api_v3) {
    fprintf(stderr, "FATAL: could not create /api/v3/ subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(api_v3, CHTTP_GET, "/ping", _hello_handler, NULL);

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

  chttpsvr_router *root_r = chttpsvr_subrouter(g_srv, "/");
  if (!root_r) {
    fprintf(stderr, "FATAL: could not create / subrouter\n");
    exit(1);
  }
  chttpsvr_router_on(root_r, CHTTP_GET, "/", _root_subrouter_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/head-pref", _hello_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_HEAD, "/head-pref",
                            _explicit_head_handler, NULL);

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

  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method",
                            _any_method_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-method/{id}",
                            _any_method_param_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-with-specific",
                            _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-with-specific",
                            _any_method_handler, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_ANY, "/any-first", _any_method_handler,
                            NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/any-first", _hello_handler,
                            NULL);

  g_srv2 = chttpsvr_create(g_test_logger, NULL);
  if (!g_srv2) {
    fprintf(stderr, "FATAL: could not create the second chttpsvr\n");
    exit(1);
  }

  chttpsvr_set_engine_logger(g_test_logger);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;
  cfg.worker_thread_count = 4;

  ccol_retval rv = chttpsvr_start(g_srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }

  if (!_create_bounded_srv()) {
    fprintf(stderr, "FATAL: could not create and start the bounded chttpsvr\n");
    exit(1);
  }

  if (!_create_small_body_srv()) {
    fprintf(stderr,
            "FATAL: could not create and start the small-body chttpsvr\n");
    exit(1);
  }

  atexit(_teardown);
}

static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz);
static void _raw_request_to_test_server(const char *req, char *buf,
                                        size_t buflen);

// Sends a GET for path to g_srv through the default client. Most tests reach
// g_srv through it. Returns the response, or NULL.
static chttpcli_response *_get(const char *path) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  return resp;
}

// Sends a POST of body with content type ct to g_srv, for the body tests.
// Returns the response, or NULL.
static chttpcli_response *_post(const char *path, const char *body,
                                const char *ct) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttp_request_body b = {body, body ? strlen(body) : 0, ct};
  chttpcli_response *resp = NULL;
  chttp_post(url, &b, &resp);
  return resp;
}

// Sends a GET for path to g_srv with one extra header, for the request header
// tests. Returns the response, or NULL.
static chttpcli_response *_get_with_header(const char *path, const char *name,
                                           const char *value) {
  char url[256];
  snprintf(url, sizeof(url), BASE_URL "%s", path);
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;
  chttp_request_set_header(req, name, value);
  chttpcli_response *resp = NULL;
  chttpcli_do(chttp_default_client(), req, &resp);
  chttp_request_free(req);
  return resp;
}

TEST(chttpserver, get_hello) {
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, post_echo_body) {
  chttpcli_response *resp = _post("/echo-body", "ping", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ping");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, post_echo_body_empty) {
  chttpcli_response *resp = _post("/echo-body", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, path_params) {
  chttpcli_response *resp = _get("/params/42/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=42 sub=hello");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, path_params_url_encoded) {
  chttpcli_response *resp = _get("/params/hello%20world/foo%2Fbar");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=hello world sub=foo/bar");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, path_param_plus_literal) {
  chttpcli_response *resp = _get("/params/hello+world/foo");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "id=hello+world sub=foo");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_single_value) {
  chttpcli_response *resp = _get("/query?q=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_multi_value) {
  chttpcli_response *resp = _get("/query?q=one&q=two&q=three");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "one,two,three");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_no_value) {
  chttpcli_response *resp = _get("/query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "no_q");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, request_header_read) {
  chttpcli_response *resp =
      _get_with_header("/header", "x-test-header", "myvalue");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "myvalue");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, missing_request_header) {
  chttpcli_response *resp = _get("/header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, custom_response_header) {
  chttpcli_response *resp = _get("/set-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *v = chttpcli_resp_header(resp, "x-custom");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "my-value");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, custom_status_code) {
  chttpcli_response *resp = _get("/status");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 201);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "created");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, json_response) {
  chttpcli_response *resp = _get("/json");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *ct = chttpcli_resp_header(resp, "content-type");
  REQUIRE_TRUE(ct != NULL);
  REQUIRE_TRUE(strstr(ct, "application/json") != NULL);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "{\"ok\":true}");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, not_found) {
  chttpcli_response *resp = _get("/does/not/exist");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, method_not_allowed) {
  chttpcli_response *resp = _post("/hello", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 405);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, global_middleware_runs) {
  int before = g_global_mw_count;
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *v = chttpcli_resp_header(resp, "x-global-mw");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "1");
  REQUIRE_GT(g_global_mw_count, before);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_get_root) {
  chttpcli_response *resp = _get("/api/v1/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_with_param) {
  chttpcli_response *resp = _get("/api/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "item:99");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_segment_matches) {
  chttpcli_response *resp = _get("/%61pi/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "item:99");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_root_matches) {
  chttpcli_response *resp = _get("/%61pi/v1/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_middleware_runs) {
  int before = g_router_mw_count;
  chttpcli_response *resp = _get("/api/v1/items/1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *gv = chttpcli_resp_header(resp, "x-global-mw");
  const char *rv = chttpcli_resp_header(resp, "x-router-mw");
  REQUIRE_TRUE(gv != NULL);
  REQUIRE_TRUE(rv != NULL);
  REQUIRE_STREQ(gv, "1");
  REQUIRE_STREQ(rv, "1");
  REQUIRE_GT(g_router_mw_count, before);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, global_mw_not_for_unrelated) {
  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *rv = chttpcli_resp_header(resp, "x-router-mw");
  REQUIRE_TRUE(rv == NULL);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, middleware_short_circuit) {
  chttpcli_response *resp = _get("/blocked-area/secret");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 403);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "blocked");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, streaming_echo) {
  const char *payload = "streaming payload data";
  chttpcli_response *resp = _post("/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, payload);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, raw_query_string) {
  chttpcli_response *resp = _get("/raw-query?foo=bar&baz=1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_TRUE(strstr(resp->body, "foo=bar") != NULL);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, path_accessor) {
  chttpcli_response *resp = _get("/path");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "/path");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, serve_double_start) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval rv = chttpsvr_start(g_srv, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

TEST(chttpserver, multi_server_start_stop) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);

  chttpsvr_register_handler(g_srv2, CHTTP_GET, "/ping-srv2", _hello_handler,
                            NULL);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval rv = chttpsvr_start(g_srv2, &cfg);

  bool resp1_ok = false, body1_ok = false;
  int status1 = -1;
  char body1_copy[128] = {0};
  bool resp2_ok = false;
  int status2 = -1;

  if (rv == ccol_success) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/ping-srv2", TEST_PORT + 1);
    chttpcli_response *resp = NULL;
    chttp_get(url, &resp);
    resp1_ok = resp != NULL;
    if (resp1_ok) {
      status1 = resp->status_code;
      body1_ok = resp->body != NULL;
      if (body1_ok) snprintf(body1_copy, sizeof(body1_copy), "%s", resp->body);
      chttpcli_resp_free(resp);
    }

    resp = _get("/hello");
    resp2_ok = resp != NULL;
    if (resp2_ok) {
      status2 = resp->status_code;
      chttpcli_resp_free(resp);
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
  chttpcli_response *resp = _get("/query-one?q=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "only");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_404_outside_prefix) {
  chttpcli_response *resp = _get("/api/v2/items/1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, large_body_post) {
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
  chttpcli_resp_free(resp);
  free(big);
}

TEST(chttpserver, query_one_multi_rejected) {
  chttpcli_response *resp = _get("/query-one?q=a&q=b");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "multi_value");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_one_missing) {
  chttpcli_response *resp = _get("/query-one");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_plus_decoded_to_space) {
  chttpcli_response *resp = _get("/query?q=hello+world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_percent_decoded) {
  chttpcli_response *resp = _get("/query?q=hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, streaming_header_present) {
  chttpcli_response *resp =
      _get_with_header("/stream-header", "x-stream-test", "stream-val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "stream-val");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, streaming_header_absent) {
  chttpcli_response *resp = _get("/stream-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, stream_handler_registration_accepted) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, subrouter_stream_route) {
  const char *payload = "api-stream-payload";
  chttpcli_response *resp = _post("/api/v1/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, payload);
  const char *gv = chttpcli_resp_header(resp, "x-global-mw");
  REQUIRE_TRUE(gv != NULL);
  REQUIRE_STREQ(gv, "1");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_method_not_allowed) {
  chttpcli_response *resp = _post("/api/v1/items/1", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 405);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, multi_mw_in_chain_runs_both) {
  int ba = g_mw_a_count, bb = g_mw_b_count;
  chttpcli_response *resp = _get("/mw-chain/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *va = chttpcli_resp_header(resp, "x-mw-a");
  const char *vb = chttpcli_resp_header(resp, "x-mw-b");
  REQUIRE_TRUE(va != NULL);
  REQUIRE_TRUE(vb != NULL);
  REQUIRE_STREQ(va, "1");
  REQUIRE_STREQ(vb, "1");
  REQUIRE_GT(g_mw_a_count, ba);
  REQUIRE_GT(g_mw_b_count, bb);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, invalid_param_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{unclosed",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{valid}/end",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, invalid_param_name_chars_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/{hello world}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{user-id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{a+b}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{userId}", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/{order_id}/items",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, duplicate_param_name_in_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/dup/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/dup-stream/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/dup-router");
  REQUIRE_TRUE(r != NULL);
  rv = chttpsvr_router_on(r, CHTTP_GET, "/{id}/b/{id}", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/dup/{id}/b/{other_id}",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, multi_method_get) {
  chttpcli_response *resp = _get("/dual");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

// POST /dual must reach the POST route although the GET route of the same path
// comes first; the search must not stop at the first path match.
TEST(chttpserver, multi_method_post) {
  chttpcli_response *resp = _post("/dual", "dual-body", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "dual-body");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, multi_method_unregistered_405) {
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

// /dual has a GET and a POST route, and the GET route serves HEAD too, so the
// Allow header of the 405 names all three (RFC 9110 SS15.5.6).
TEST(chttpserver, method_not_allowed_lists_the_allowed_methods) {
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/dual", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:GET, HEAD, POST\r\n") != NULL);
}

TEST(chttpserver, method_not_allowed_without_a_get_route_omits_head) {
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/echo-body", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:POST\r\n") != NULL);
}

TEST(chttpserver, not_found_carries_no_allow_header) {
  char buf[2048] = {0};
  int status =
      _raw_request("PUT", "/no-such-path-for-allow", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
  REQUIRE_TRUE(strstr(buf, "\r\nallow:") == NULL);
}

TEST(chttpserver, double_slash_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo//bar",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "//leading", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, multi_write_accumulates) {
  chttpcli_response *resp = _get("/multi-write");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "foobarbaz");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, printf_accumulates) {
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
  chttpcli_resp_free(resp);
}

TEST(chttpserver, path_url_decoded) {
  chttpcli_response *resp = _get("/echo-path/hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "/echo-path/hello world");
  chttpcli_resp_free(resp);
}

// /hello%00xyz must not match the literal route /hello through a strcmp that
// stops at the embedded NUL.
TEST(chttpserver, percent_encoded_nul_in_literal_segment_is_route_mismatch) {
  chttpcli_response *resp = _get("/hello%00xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_param_segment_is_route_mismatch) {
  chttpcli_response *resp = _get("/echo-path/hello%00world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, percent_encoded_nul_in_query_key_does_not_alias) {
  chttpcli_response *resp = _get("/query-one?q%00x=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, multi_key_query) {
  chttpcli_response *resp = _get("/multi-query?a=hello&b=world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=hello b=world");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, multi_key_query_missing) {
  chttpcli_response *resp = _get("/multi-query?a=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "a=only b=nil");
  chttpcli_resp_free(resp);
}

// Call counters of the pass-through allocator below, which shows that every
// allocation and free of chttpsvr_create_mp goes through the caller's procs.
static size_t g_wrap_malloc_count;
static size_t g_wrap_free_count;
static size_t g_wrap_calloc_count;
static size_t g_wrap_realloc_count;

// The counting allocator of custom_allocator_lifecycle: each function counts
// its call in its g_wrap_*_count and passes it to the C library (free counts
// only non-NULL pointers).
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
  g_wrap_malloc_count = 0;
  g_wrap_free_count = 0;
  g_wrap_calloc_count = 0;
  g_wrap_realloc_count = 0;

  ccol_memmgmt_procs mp = {_wrap_malloc, _wrap_free, _wrap_calloc,
                           _wrap_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_GT(g_wrap_malloc_count + g_wrap_calloc_count, (size_t)0);
  REQUIRE_EQ(g_wrap_free_count, (size_t)0);

  ccol_retval rv =
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
  REQUIRE_GT(g_wrap_free_count, (size_t)0);
  REQUIRE_GE(g_wrap_free_count, allocs_before_destroy);
}

TEST(chttpserver, subrouter_root_no_trailing_slash) {
  chttpcli_response *resp = _get("/api/v1");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "api_root");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, long_path_heap_alloc) {
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
  chttpcli_resp_free(resp);
}

// Polls the reject-pool task counter until it reaches expected, for at most 5
// s, and returns what it saw. The rejection tests need it because a reject task
// counts itself after the client already has its response.
static size_t _wait_for_reject_pool_task_count(size_t expected) {
  for (int attempt = 0; attempt < 250; attempt++) {
    if (chttpsvr__reject_pool_task_count_for_tests() >= expected) break;
    usleep(20000);
  }
  return chttpsvr__reject_pool_task_count_for_tests();
}

// Waits (at most 5 s) until none of the servers of _setup has a request in
// flight or a connection parked on a write or lingering. The rejection tests
// call it before they take the process-wide reject counter as a baseline, so
// that a late increment from an earlier test doesn't land in their window.
static void _wait_for_reject_counter_to_settle(void) {
  const chttpsvr *servers[] = {&g_srv, &g_srv2, &g_bounded_srv,
                               &g_small_body_srv};
  for (int attempt = 0; attempt < 250; attempt++) {
    bool busy = false;
    for (size_t i = 0; i < sizeof(servers) / sizeof(servers[0]); i++)
      if (chttpsvr__in_flight_requests_for_tests(*servers[i]) > 0 ||
          chttpsvr__parked_count_for_tests(*servers[i], 2) > 0 ||
          chttpsvr__parked_count_for_tests(*servers[i], 6) > 0)
        busy = true;
    if (!busy) return;
    usleep(20000);
  }
}

// Parses an IMF-fixdate (RFC 9110 SS5.6.7) of len bytes at v into *out,
// strictly and without the locale; the weekday must agree with the date. The
// Date header tests use it. Returns false for anything else.
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

// Counts the responses in buf that carry exactly one Date header that parses
// and, when now isn't 0, lies within max_skew seconds of now. The Date tests
// send many requests at once and check every answer with it. *total gets the
// number of responses.
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
    size_t rest = strlen(end + 4);
    p = end + 4 + (body_len < rest ? body_len : rest);
  }
  return good;
}

// Copies the single Date value of the first response in buf into out, for the
// tests that compare a Date with an expected value. Returns false when there
// isn't exactly one.
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

// Request 1 blocks the only worker and request 2 takes the only queue slot, so
// request 3 must get a 503 with a valid Date, through the reject pool.
TEST(chttpserver, bounded_pool_full_returns_503) {
  _wait_for_reject_counter_to_settle();
  size_t reject_count_before = chttpsvr__reject_pool_task_count_for_tests();
  g_bounded_cli = chttpcli_create(NULL);
  bool cli_ok =
      g_bounded_cli != CHTTPCLI_NULL &&
      chttpcli_set_pool_size(g_bounded_cli, 8) == ccol_success &&
      chttpcli_set_request_timeout(g_bounded_cli, 30000000) == ccol_success;

  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  g_blk_count = 0;
  pthread_mutex_unlock(&g_blk_mtx);

  pthread_t t1;
  bool t1_created =
      cli_ok && pthread_create(&t1, NULL, _send_bounded_req, NULL) == 0;

  bool worker_blocked = false;
  if (t1_created) {
    struct timespec nap = {0, 1000000L};
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
        break;
      nanosleep(&nap, NULL);
    }
  }

  pthread_t t2;
  bool t2_created = false;
  bool request2_queued = false;
  if (worker_blocked) {
    t2_created = pthread_create(&t2, NULL, _send_bounded_req, NULL) == 0;
    if (t2_created) {
      struct timespec nap = {0, 1000000L};
      struct timespec deadline;
      clock_gettime(CLOCK_MONOTONIC, &deadline);
      deadline.tv_sec += 5;
      for (;;) {
        if (chttpsvr__in_flight_requests_for_tests(g_bounded_srv) >= 2) {
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

  int status = -1;
  bool got_resp = false;
  bool date_ok = false;
  size_t reject_count_after = reject_count_before;
  if (worker_blocked && request2_queued) {
    chttpcli_response *resp = _bounded_get();
    if (resp) {
      got_resp = true;
      status = resp->status_code;
      const char *d = chttpcli_resp_header(resp, "date");
      time_t t;
      date_ok = d && _parse_imf_fixdate(d, strlen(d), &t) &&
                labs((long)(t - time(NULL))) <= 5;
      chttpcli_resp_free(resp);
    }
    reject_count_after =
        _wait_for_reject_pool_task_count(reject_count_before + 1);
  }

  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  if (t1_created) pthread_join(t1, NULL);
  if (t2_created) pthread_join(t2, NULL);
  if (g_bounded_cli != CHTTPCLI_NULL) chttpcli_destroy(g_bounded_cli);
  g_bounded_cli = CHTTPCLI_NULL;

  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  pthread_mutex_unlock(&g_blk_mtx);

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
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_stop(g_srv2);
  chttpsvr_stop(g_srv2);
}

// Sends a hand-made request to g_srv over a raw socket and returns the status,
// or -1 on a socket error. Tests use it for what the client can't send, such as
// a repeated header name or an unknown method. extra_headers holds whole header
// lines with their CRLF, and buf gets the whole response.
static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
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

// Sends a request whose body goes out in several pieces with a pause between
// them and returns the status, or -1. The batch and slow-drip tests use it so
// the server sees separate reads.
static int _raw_request_drip_body(int port, const char *method,
                                  const char *path, const char *body,
                                  size_t chunk_len, unsigned delay_us,
                                  char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
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

// Declares a large Content-Length, sends only sent_len bytes of the body and
// closes, like a client that aborts an upload
// (client_disconnect_mid_body_does_not_hang_server). Returns 0, or -1 on a
// socket error.
static int _raw_request_abort_mid_body(uint16_t port, const char *path,
                                       const char *partial_body,
                                       size_t declared_len) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  close(fd);
  fd = -1;
  return 0;
}

// Sends part of a body, half-closes with shutdown(SHUT_WR) and reads the
// response into buf. The server then sees a clean EOF in the middle of the
// body, the truncation path of chttpsvr_req_read() that
// truncated_body_reports_ccol_http_transfer_aborted checks. Returns 0, or -1.
static int _raw_request_truncate_body_half_close(const char *path,
                                                 const char *partial_body,
                                                 size_t declared_len, char *buf,
                                                 size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
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

// Reads fd into buf until EOF or a full buf and NUL-terminates it. The
// pipelining and rejection tests drain whole answers with it. Each read is
// bounded by SO_RCVTIMEO, so a server that never answers fails the test instead
// of hanging it. Returns the bytes read and leaves fd open.
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

// Reads one short response with a Content-Length off fd into buf and returns
// its status, or -1. The keep-alive tests use it because it leaves the
// connection open for the next request. Reads are bounded by SO_RCVTIMEO.
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

// Decodes in place the body of a raw response in buf, framed by Content-Length
// or chunked, for the tests that compare the body of a raw exchange (the server
// never chunks, but tests also decode chunked request bodies with it). Returns
// the NUL-terminated body, or NULL when the head has no end.
static char *_decode_raw_body(char *buf) {
  char *sep = strstr(buf, "\r\n\r\n");
  if (!sep) return NULL;
  char *body = sep + 4;

  char saved = sep[0];
  sep[0] = '\0';
  bool chunked = (strcasestr(buf, "transfer-encoding: chunked") != NULL);
  sep[0] = saved;

  if (!chunked) return body;

  char *end_buf = buf + strlen(buf);

  char *out = body;
  char *p = body;
  while (*p) {
    char *end = NULL;
    unsigned long chunk_size = strtoul(p, &end, 16);
    if (end == p) break;
    if (*end == '\r') end++;
    if (*end == '\n') end++;
    if (chunk_size == 0) break;
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

TEST(chttpserver, req_read_on_buffered_returns_minus_one) {
  chttpcli_response *resp =
      _post("/req-read-buffered", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "-1");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_on_streaming_route) {
  const char *payload = "body-via-api";
  chttpcli_response *resp = _post("/stream-body-api", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_body_via_api_after_req_read_on_streaming_route) {
  const char *payload = "12345678-more-than-eight-bytes";
  chttpcli_response *resp =
      _post("/stream-body-after-read", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_TRUE(strncmp(resp->body, "read=0 ", 7) != 0);
  REQUIRE_TRUE(strstr(resp->body, "body=null len=0") != NULL);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, streaming_repeated_header) {
  char buf[4096] = {0};
  int status = _raw_request("GET", "/stream-header",
                            "x-stream-test: first\r\nx-stream-test: second\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "second");
}

TEST(chttpserver, streaming_body_delivered_in_separate_batches) {
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
  REQUIRE_GT(batches, 1);
}

TEST(chttpserver, unmatched_route_rejected_without_reading_body) {
  _wait_for_reject_counter_to_settle();
  size_t reject_count_before = chttpsvr__reject_pool_task_count_for_tests();

  char buf[4096] = {0};
  int status = _raw_request("POST", "/no-such-route-at-all",
                            "Content-Length: 100000000\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 404);

  _wait_for_reject_pool_task_count(reject_count_before + 1);
}

TEST(chttpserver, unmatched_route_rejection_routed_through_reject_pool) {
  _wait_for_reject_counter_to_settle();
  size_t before = chttpsvr__reject_pool_task_count_for_tests();

  chttpcli_response *resp = _get("/definitely-not-a-registered-route-xyz");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);

  REQUIRE_EQ(_wait_for_reject_pool_task_count(before + 1), before + 1);
}

// Every rejection runs on the reject pool, so a burst of 404s doesn't delay an
// ordinary request on the shared reactor. It is a smoke check of starvation and
// of the pool's concurrent submit path.
TEST(chttpserver, concurrent_route_rejections_do_not_starve_other_requests) {
  enum { N_REJECTIONS = 40 };
  pthread_t threads[N_REJECTIONS];
  int statuses[N_REJECTIONS];
  int created = 0;
  ccol_retval create_rv = ccol_success;
  _wait_for_reject_counter_to_settle();
  size_t rejects_before = chttpsvr__reject_pool_task_count_for_tests();
  for (; created < N_REJECTIONS; created++) {
    statuses[created] = -1;
    int rc = pthread_create(&threads[created], NULL, _send_unmatched_route_req,
                            &statuses[created]);
    if (rc != 0) {
      create_rv = ccol_unexpected_failure;
      break;
    }
  }

  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  chttpcli_response *resp = _get("/hello");
  clock_gettime(CLOCK_MONOTONIC, &t1);
  int status_code = resp ? resp->status_code : -1;
  if (resp) chttpcli_resp_free(resp);

  long elapsed_ms =
      (t1.tv_sec - t0.tv_sec) * 1000 + (t1.tv_nsec - t0.tv_nsec) / 1000000L;

  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  _wait_for_reject_counter_to_settle();
  size_t on_pool =
      chttpsvr__reject_pool_task_count_for_tests() - rejects_before;
  int not_found = 0;
  for (int i = 0; i < created; i++)
    if (statuses[i] == 404) not_found++;

  REQUIRE_EQ((int)create_rv, (int)ccol_success);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(status_code, 200);
  REQUIRE_EQ(not_found, (int)N_REJECTIONS);
  REQUIRE_EQ(on_pool, (size_t)N_REJECTIONS);
  REQUIRE_LT(elapsed_ms, 10000L);
}

TEST(chttpserver, pipelined_bytes_after_rejected_route_not_misparsed) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
}

TEST(chttpserver, keep_alive_across_two_requests_on_one_connection) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A bodyless request never runs the body loop, so the pipelined bytes behind it
// must be reclaimed explicitly, or the second request is lost and its client
// waits for ever.
TEST(chttpserver, pipelined_bodyless_requests_both_answered) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  char *first = strstr(buf, "HTTP/1.1 200");
  REQUIRE_TRUE(first != NULL);
  char *second = strstr(first + 1, "HTTP/1.1 200");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "Hello, world!") != NULL);
  REQUIRE_TRUE(strstr(second, "connection:close") != NULL);
}

// The read that takes the end of a body also takes the start of the pipelined
// next request, and those bytes must be kept.
TEST(chttpserver, pipelined_bytes_after_buffered_body_request_not_lost) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The same for a streaming route, whose reads have the same shape.
TEST(chttpserver, pipelined_bytes_after_streaming_body_request_not_lost) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// Five requests in one write make the worker's keep-alive tail hand over the
// next request again and again from inside itself, which two requests never
// reach. Every request must be answered exactly once.
TEST(chttpserver, five_pipelined_bodyless_requests_all_answered) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The worker's keep-alive tail feeds a malformed pipelined request into the
// parser, which closes and frees the connection at once; the tail must not
// touch the connection after that call.
TEST(chttpserver, pipelined_malformed_request_after_worker_processed_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strncmp(first, "HTTP/1.1 200", 12) == 0);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
  char *second = strstr(first + 8, "HTTP/1.1");
  REQUIRE_TRUE(second != NULL);
  REQUIRE_TRUE(strncmp(second, "HTTP/1.1 400", 12) == 0);
  REQUIRE_TRUE(strstr(second + 8, "HTTP/1.1") == NULL);
}

// Two streaming POSTs with different bodies on one connection; both answers
// match only if the per-request ingestion state is reset in between.
TEST(chttpserver, keep_alive_two_consecutive_streaming_requests) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The handler reads 8 bytes of a 64-byte body that arrived whole. The server
// has parsed the whole body anyway, so the connection stays usable.
TEST(chttpserver,
     streaming_handler_early_stop_of_fully_arrived_body_stays_keepalive) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  const char *req2 = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req2, strlen(req2)), (ssize_t)strlen(req2));
  memset(buf, 0, sizeof(buf));
  int status2 = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status2, 200);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);

  close(fd);
  fd = -1;
}

// Here only 8 of 64 bytes arrive, so the body is undrained when the handler
// returns and the server must close, or the missing bytes would be read as the
// next request.
TEST(chttpserver,
     streaming_handler_early_stop_with_undrained_body_forces_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 2);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *hdr =
      "POST /stream-error-report-slow HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 100\r\n"
      "Connection: close\r\n"
      "\r\n"
      "only-ten-";
  REQUIRE_EQ(write(fd, hdr, strlen(hdr)), (ssize_t)strlen(hdr));

  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);
}

// The server has a single worker, so a follow-up request succeeds only if that
// worker recovered from the client that left in the middle of its body.
TEST(chttpserver, client_disconnect_mid_body_does_not_hang_server) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval reg_rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/disconnect-mid-body", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 30;
  cfg.worker_thread_count = 1;
  cfg.worker_queue_capacity = 4;
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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

// A client that half-closes in the middle of its body must make
// chttpsvr_req_stream_error() report ccol_http_transfer_aborted, not
// ccol_success.
TEST(chttpserver, truncated_body_reports_ccol_http_transfer_aborted) {
  char buf[4096] = {0};
  int rc = _raw_request_truncate_body_half_close(
      "/stream-error-report", "only-part-of-the-declared-body", 1000, buf,
      sizeof(buf));
  REQUIRE_EQ(rc, 0);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_http_transfer_aborted") != NULL);
}

TEST(chttpserver, expect_100_continue_interim_response_sent_before_body) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  char interim[128] = {0};
  size_t got = 0;
  while (got < sizeof(interim) - 1 && strstr(interim, "\r\n\r\n") == NULL) {
    ssize_t r = read(fd, interim + got, sizeof(interim) - 1 - got);
    REQUIRE_GT(r, 0);
    got += (size_t)r;
  }
  REQUIRE_TRUE(strstr(interim, "HTTP/1.1 100 Continue") != NULL);

  REQUIRE_EQ(write(fd, body, strlen(body)), (ssize_t)strlen(body));
  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  close(fd);
  fd = -1;
}

// A streaming route sends the interim "100 Continue" only when the handler
// first reads, so a handler that rejects at once gives the final response with
// no interim line, and the client skips the upload (RFC 7231 SS5.1.1).
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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  char buf[1024];
  int status = _read_one_http_response(fd, buf, sizeof(buf));
  REQUIRE_EQ(status, 401);
  REQUIRE_TRUE(strstr(buf, "100 Continue") == NULL);
  REQUIRE_TRUE(strstr(buf, "no thanks") != NULL);
  close(fd);
  fd = -1;
}

// A request with Expect: 100-continue and no body must not get "100 Continue",
// even when the streaming handler reads: there is no body to invite.
TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_streaming_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

TEST(chttpserver, expect_100_continue_not_sent_for_bodyless_buffered_request) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A short write of the interim line leaves a broken status line on the wire, so
// the server must close instead of writing the real response after it. The test
// hook makes a real partial write to the socket.
TEST(chttpserver, interim_continue_short_write_buffered_route_forces_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  chttpsvr__force_short_interim_write_for_tests(10);
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

// The same through the lazy interim send of chttpsvr_req_read, which must also
// return -1.
TEST(chttpserver, interim_continue_short_write_streaming_route_forces_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  chttpsvr__force_short_interim_write_for_tests(10);
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

// g_srv keeps the default max_response_write_duration_us of 0, and the interim
// "100 Continue" line must still be bounded by the internal write ceiling, or a
// peer reading it one byte at a time holds a worker for ever.
TEST(chttpserver,
     max_response_write_duration_bounds_interim_continue_response) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  chttpsvr__force_next_response_write_deadline_expired_for_tests();

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

  REQUIRE_EQ((int)r, 0);
}

// stream_read_timeout_us 0 means "wait for ever" and must not be cast into a
// poll timeout of 0 (try once). A late body must still be read, and a bodyless
// GET must still get its response, whose write timeout inherits the 0.
TEST(chttpserver, stream_read_timeout_ms_zero_means_wait_indefinitely) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/zero-timeout-wait",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 17;
  cfg.stream_read_timeout_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 17));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
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

  struct timespec delay = {0, 300000000L};
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

// Concurrent streaming echo: each thread posts its own payload and checks the
// echo and the global middleware header.
#define N_CONCURRENT_STREAMS 8

// One thread's id and verdict.
typedef struct {
  int id;
  int ok;
} conc_stream_arg_t;

// Posts "concurrent-<id>" to /stream-echo and sets a->ok when the echo and the
// x-global-mw header are right. It is the thread body of concurrent_streaming.
static void *_conc_stream_thread(void *arg) {
  conc_stream_arg_t *a = (conc_stream_arg_t *)arg;
  char payload[64];
  snprintf(payload, sizeof(payload), "concurrent-%d", a->id);
  chttpcli_response *resp = _post("/stream-echo", payload, "text/plain");
  a->ok = (resp != NULL && resp->status_code == 200 && resp->body != NULL &&
           strcmp(resp->body, payload) == 0);
  if (a->ok && resp) {
    const char *gv = chttpcli_resp_header(resp, "x-global-mw");
    a->ok = (gv != NULL && strcmp(gv, "1") == 0);
  }
  if (resp) chttpcli_resp_free(resp);
  return NULL;
}

TEST(chttpserver, concurrent_streaming) {
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
  for (int i = 0; i < created; i++) pthread_join(threads[i], NULL);
  REQUIRE_EQ(created, N_CONCURRENT_STREAMS);
  for (int i = 0; i < created; i++) REQUIRE_TRUE(args[i].ok);
}

TEST(chttpserver, subrouter_trailing_slash_prefix) {
  chttpcli_response *resp = _get("/api/v3/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, buffered_repeated_header) {
  char buf[4096] = {0};
  int status = _raw_request("GET", "/header",
                            "x-test-header: first\r\nx-test-header: second\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "second");
}

TEST(chttpserver, resp_write_str_null_guard) {
  g_null_guard_results[0] = g_null_guard_results[1] = -1;
  chttpcli_response *resp = _get("/null-guard");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpcli_resp_free(resp);
  REQUIRE_EQ(g_null_guard_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_null_guard_results[1], (int)ccol_invalid_args);
}

TEST(chttpserver, trailing_slash_not_matched) {
  chttpcli_response *resp = _get("/api/v1/items/99/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 404);
  chttpcli_resp_free(resp);
}

// The route matcher decodes each path segment at most once per router and
// request and shares the result between candidate routes. The cache tests
// register several routes where an earlier candidate fails, and that failure
// must not change what a later candidate reads for a shared segment (a literal,
// a param, or bad percent-encoding). Their patterns belong to this group alone.
TEST(chttpserver, cache_shared_literal_prefix_segment_multiple_candidates) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
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
  chttpcli_resp_free(resp);
}

// The first candidate captures and frees its copy of the param before it fails;
// the second must still read "xyz", so the cache keeps its own copy.
TEST(chttpserver,
     cache_shared_param_segment_survives_earlier_failed_candidate) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
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
  chttpcli_resp_free(resp);
}

TEST(chttpserver, cache_malformed_segment_rejects_every_sharing_candidate) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/x",
                                     _cache_param_handler, (void *)"x"),
      (int)ccol_success);
  REQUIRE_EQ(
      (int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/cache-bad/{id}/y",
                                     _cache_param_handler, (void *)"y"),
      (int)ccol_success);

  char buf[1024];
  _raw_request_to_test_server(
      "GET /cache-bad/%ZZ/y HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
}

// The sub-router prefix matcher has its own per-request cache of decoded
// segments. Sibling sub-routers share an encoded first segment, and a sibling
// that fails on a later segment must not change what the next one reads.
TEST(chttpserver, subrouter_prefix_cache_shared_across_candidates) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
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
  chttpcli_resp_free(resp);
}

TEST(chttpserver,
     subrouter_prefix_cache_malformed_segment_rejects_every_sharing_router) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
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

  char buf[1024];
  _raw_request_to_test_server(
      "GET /pfx-b%ZZd/two/leaf HTTP/1.1\r\nHost: h\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") != NULL);
}

TEST(chttpserver, on_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, "/test-null-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_use_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-mw-test");
  REQUIRE_TRUE(r != NULL);
  ccol_retval rv = chttpsvr_router_use(r, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, use_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_use(g_srv2, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, on_null_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, on_stream_null_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(g_srv2, CHTTP_POST, NULL,
                                                       _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-pat-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval rv = chttpsvr_router_on(r, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-pat-stream");
  REQUIRE_TRUE(r != NULL);
  ccol_retval rv =
      chttpsvr_router_on_stream(r, CHTTP_POST, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, stop_twice_safe) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_stop(g_srv2);
  chttpsvr_stop(g_srv2);
}

TEST(chttpserver, resp_header_duplicate_last_wins) {
  chttpcli_response *resp = _get("/dup-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  const char *v = chttpcli_resp_header(resp, "x-dup");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "second");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, invalid_status_code_clamped_to_500) {
  chttpcli_response *resp = _get("/bad-status");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_one_null_key_returns_invalid_args) {
  chttpcli_response *resp = _get("/query-one-null-key?q=val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  char expected[16];
  snprintf(expected, sizeof(expected), "%d", (int)ccol_invalid_args);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, expected);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_read_zero_buflen_returns_zero) {
  chttpcli_response *resp = _get("/stream-zero-buflen");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_read_empty_body_streaming_returns_zero) {
  chttpcli_response *resp = _get("/stream-empty-read");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_one_null_req_returns_invalid_args) {
  const char *val = NULL;
  ccol_retval rv = chttpsvr_req_query_one(NULL, "key", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, trailing_slash_pattern_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar/", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/foo/bar", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, req_read_null_req_returns_minus_one) {
  char buf[16];
  ssize_t n = chttpsvr_req_read(NULL, buf, sizeof(buf));
  REQUIRE_EQ(n, (ssize_t)-1);
}

TEST(chttpserver, req_read_null_buf_returns_minus_one) {
  chttpcli_response *resp = _get("/stream-null-buf");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "-1");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_null_prefix_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, NULL);
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_no_leading_slash_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "api/v1");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_at_start_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "//api");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_in_middle_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/api//v1");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, on_stream_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(g_srv2, CHTTP_POST,
                                                       "/noop-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-stream-fn");
  REQUIRE_TRUE(r != NULL);
  ccol_retval rv = chttpsvr_router_on_stream(r, CHTTP_POST, "/x", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_write_null_data_edge_cases) {
  g_null_data_results[0] = g_null_data_results[1] = -1;
  chttpcli_response *resp = _get("/resp-write-null-guard");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpcli_resp_free(resp);
  REQUIRE_EQ(g_null_data_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_null_data_results[1], (int)ccol_success);
}

TEST(chttpserver, req_read_null_buf_zero_buflen_returns_zero) {
  chttpcli_response *resp = _get("/stream-null-buf-zero-len");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, resp_write_json_zero_len_rejected) {
  g_json_zero_len_result = -1;
  chttpcli_response *resp = _get("/json-zero-len");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "done");
  chttpcli_resp_free(resp);
  REQUIRE_EQ(g_json_zero_len_result, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_write_json_null_resp_returns_invalid_args) {
  ccol_retval rv = chttpsvr_resp_write_json(NULL, "{}", 2);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_method_null_returns_chttp_get) {
  REQUIRE_EQ((int)chttpsvr_req_method(NULL), (int)CHTTP_GET);
}

TEST(chttpserver, req_path_null_returns_null) {
  REQUIRE_TRUE(chttpsvr_req_path(NULL) == NULL);
}

TEST(chttpserver, req_body_null_returns_null_and_zeroes_len) {
  size_t len = 99;
  const void *p = chttpsvr_req_body(NULL, &len);
  REQUIRE_TRUE(p == NULL);
  REQUIRE_EQ(len, (size_t)0);
}

TEST(chttpserver, req_body_null_req_null_len_out_is_safe) {
  REQUIRE_TRUE(chttpsvr_req_body(NULL, NULL) == NULL);
}

TEST(chttpserver, req_raw_query_null_returns_null) {
  REQUIRE_TRUE(chttpsvr_req_raw_query(NULL) == NULL);
}

TEST(chttpserver, req_param_null_req_returns_null) {
  REQUIRE_TRUE(chttpsvr_req_param(NULL, "id") == NULL);
}

TEST(chttpserver, req_header_null_req_returns_null) {
  REQUIRE_TRUE(chttpsvr_req_header(NULL, "x-foo") == NULL);
}

TEST(chttpserver, req_header_null_name_returns_null) {
  chttpcli_response *resp = _get("/null-name-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_query_null_req_returns_null_and_zero_count) {
  size_t n = 99;
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", &n) == NULL);
  REQUIRE_EQ(n, (size_t)0);
}

TEST(chttpserver, req_query_null_req_null_count_out_is_safe) {
  REQUIRE_TRUE(chttpsvr_req_query(NULL, "q", NULL) == NULL);
}

TEST(chttpserver, req_query_one_null_req_null_val_out_is_safe) {
  ccol_retval rv = chttpsvr_req_query_one(NULL, "q", NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, req_query_one_invalid_args_resets_poisoned_val_out) {
  const char *val = (const char *)0xdeadbeefUL;
  ccol_retval rv = chttpsvr_req_query_one(NULL, "q", &val);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
  REQUIRE_EQ((void *)val, (void *)NULL);
}

TEST(chttpserver, req_query_one_null_val_out_with_hit_returns_success) {
  chttpcli_response *resp = _get("/query-one-null-val-out?q=present");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_query_oom_null_req_returns_false) {
  REQUIRE_FALSE(chttpsvr_req_query_oom(NULL));
}

TEST(chttpserver, req_query_null_key_returns_null_and_zero_count) {
  chttpcli_response *resp = _get("/null-key-query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, resp_set_status_null_is_noop) {
  chttpsvr_resp_set_status(NULL, 200);
}

TEST(chttpserver, resp_set_header_null_resp_returns_invalid_args) {
  ccol_retval rv = chttpsvr_resp_set_header(NULL, "x-foo", "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, serve_null_srv_returns_invalid_args) {
  ccol_retval rv = chttpsvr_start(CHTTPSVR_NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, stop_null_is_noop) { chttpsvr_stop(CHTTPSVR_NULL); }

TEST(chttpserver, raw_query_absent) {
  chttpcli_response *resp = _get("/raw-query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, headers_only_response) {
  chttpcli_response *resp = _get("/headers-only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 204);
  const char *v = chttpcli_resp_header(resp, "x-no-body");
  REQUIRE_TRUE(v != NULL);
  REQUIRE_STREQ(v, "1");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, duplicate_route_first_wins) {
  chttpcli_response *resp = _get("/dup-first-wins");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, router_on_null_fn_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-fn-rt");
  REQUIRE_TRUE(r != NULL);
  ccol_retval rv = chttpsvr_router_on(r, CHTTP_GET, "/x", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_null_resp_set_status_no_crash_and_dual_null_header) {
  chttpsvr_resp_set_status(NULL, 200);
  ccol_retval rv = chttpsvr_resp_set_header(NULL, NULL, "bar");
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, resp_set_header_null_name_value_live) {
  g_null_hdr_results[0] = g_null_hdr_results[1] = -1;
  chttpcli_response *resp = _get("/set-header-null-guards");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "ok");
  chttpcli_resp_free(resp);
  REQUIRE_EQ(g_null_hdr_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_null_hdr_results[1], (int)ccol_invalid_args);
}

TEST(chttpserver, resp_set_header_crlf_injection_rejected) {
  g_crlf_hdr_results[0] = g_crlf_hdr_results[1] = g_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status =
      _raw_request("GET", "/set-header-crlf-guards", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-injected") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-evil") == NULL);

  REQUIRE_EQ(g_crlf_hdr_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_crlf_hdr_results[1], (int)ccol_invalid_args);
  REQUIRE_EQ(g_crlf_hdr_results[2], (int)ccol_success);
}

TEST(chttpserver, resp_set_header_bare_cr_and_lf_rejected) {
  g_bare_crlf_hdr_results[0] = g_bare_crlf_hdr_results[1] =
      g_bare_crlf_hdr_results[2] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-bare-crlf-guards", NULL, buf,
                            sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  REQUIRE_TRUE(strstr(buf, "injected") == NULL);

  REQUIRE_EQ(g_bare_crlf_hdr_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_bare_crlf_hdr_results[1], (int)ccol_invalid_args);
  REQUIRE_EQ(g_bare_crlf_hdr_results[2], (int)ccol_success);
}

TEST(chttpserver, resp_set_header_disallowed_names_rejected) {
  g_disallowed_hdr_results[0] = g_disallowed_hdr_results[1] =
      g_disallowed_hdr_results[2] = g_disallowed_hdr_results[3] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-disallowed-name-guards", NULL,
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit:fine") != NULL);
  REQUIRE_TRUE(strcasestr(buf, "transfer-encoding") == NULL);

  REQUIRE_EQ(g_disallowed_hdr_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_disallowed_hdr_results[1], (int)ccol_invalid_args);
  REQUIRE_EQ(g_disallowed_hdr_results[2], (int)ccol_invalid_args);
  REQUIRE_EQ(g_disallowed_hdr_results[3], (int)ccol_success);
}

TEST(chttpserver, resp_set_header_non_tchar_name_rejected) {
  g_non_tchar_hdr_results[0] = g_non_tchar_hdr_results[1] =
      g_non_tchar_hdr_results[2] = g_non_tchar_hdr_results[3] = -1;
  char buf[4096] = {0};
  int status = _raw_request("GET", "/set-header-non-tchar-name-guards", NULL,
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  REQUIRE_TRUE(strstr(buf, "x-legit!#$%&'*+-.^_`|~:fine") != NULL);
  REQUIRE_TRUE(strstr(buf, "X Foo") == NULL);
  REQUIRE_TRUE(strstr(buf, "X:Foo") == NULL);

  REQUIRE_EQ(g_non_tchar_hdr_results[0], (int)ccol_invalid_args);
  REQUIRE_EQ(g_non_tchar_hdr_results[1], (int)ccol_invalid_args);
  REQUIRE_EQ(g_non_tchar_hdr_results[2], (int)ccol_invalid_args);
  REQUIRE_EQ(g_non_tchar_hdr_results[3], (int)ccol_success);
}

TEST(chttpserver, subrouter_null_srv_rejected) {
  chttpsvr_router *r = chttpsvr_subrouter(CHTTPSVR_NULL, "/any");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, req_param_null_name_returns_null) {
  chttpcli_response *resp = _get("/param-null-name");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "null");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, on_stream_null_srv_returns_invalid_args) {
  ccol_retval rv = chttpsvr_register_streaming_handler(
      CHTTPSVR_NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, query_empty_key) {
  chttpcli_response *resp = _get("/query-empty-key?=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "hello");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, query_stray_ampersand_does_not_produce_phantom_empty_key) {
  chttpcli_response *resp = _get("/query-stray-amp-counts?a=1&&b=2");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver,
     query_leading_and_trailing_ampersand_does_not_produce_phantom_empty_key) {
  chttpcli_response *resp = _get("/query-stray-amp-counts?&a=1&b=2&");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "1,1,0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, req_read_eof_is_idempotent) {
  chttpcli_response *resp = _get("/stream-read-eof-twice");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "0 0");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, subrouter_slash_prefix_matches_only_root) {
  chttpcli_response *resp_root = _get("/");
  REQUIRE_TRUE(resp_root != NULL);
  REQUIRE_EQ(resp_root->status_code, 200);
  REQUIRE_TRUE(resp_root->body != NULL);
  REQUIRE_STREQ(resp_root->body, "root_subrouter");
  chttpcli_resp_free(resp_root);

  chttpcli_response *resp_foo = _get("/nonexistent-path-xyz");
  REQUIRE_TRUE(resp_foo != NULL);
  REQUIRE_EQ(resp_foo->status_code, 404);
  chttpcli_resp_free(resp_foo);
}

TEST(chttpserver, subrouter_slash_prefix_rejects_doubled_leading_slash) {
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "//nonexistent-path-xyz", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, pattern_no_leading_slash_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "health",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_POST, "echo", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);

  rv = chttpsvr_register_handler(g_srv2, CHTTP_GET, "/health", _hello_handler,
                                 NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, set_engine_logger_release_refused_while_running) {
  ccol_retval rv = chttpsvr_set_engine_logger(CLOG_NULL);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

TEST(chttpserver, serve_stopped_server_state_valid) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_stop(g_srv2);
  chttpsvr_stop(g_srv2);

  ccol_retval rv = chttpsvr_register_handler(
      g_srv2, CHTTP_GET, "/state-valid-check", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

TEST(chttpserver, serve_tls_with_missing_certificate_files_fails_cleanly) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttp_tls_config tls = CHTTP_TLS_DEFAULT;
  tls.cert_path = "/nonexistent/cert.pem";
  tls.key_path = "/nonexistent/key.pem";
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.port = 0;
  cfg.host = "127.0.0.1";
  cfg.tls = &tls;
  ccol_retval rv = chttpsvr_start(g_srv2, &cfg);
  bool running = true;
  ccol_retval run_rv = chttpsvr_is_running(g_srv2, &running);
  REQUIRE_EQ((int)rv, (int)ccol_http_tls_cert_load_failed);
  REQUIRE_EQ((int)run_rv, (int)ccol_success);
  REQUIRE_FALSE(running);
}

TEST(chttpserver, streaming_get_no_body_via_req_body) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  chttpcli_response *resp = _get("/stream-body-get-no-body");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, dynamic_route_registration_while_running) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      g_srv, CHTTP_GET, "/late-dynamic-route", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  chttpcli_response *resp = _get("/late-dynamic-route");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

// Routers are tried from the most specific mount, so the /shadow-test
// sub-router answers /shadow-test/ping although the root route of that path is
// registered first.
TEST(chttpserver, subrouter_wins_over_root_route_at_same_path) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  chttpcli_response *resp = _get("/shadow-test/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "sub-wins");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_registration_rejected) {
  REQUIRE_TRUE(g_srv2 != CHTTPSVR_NULL);
  chttpsvr_router *ov_r = chttpsvr_subrouter(g_srv2, "/overflow-mw-test");
  REQUIRE_TRUE(ov_r != NULL);

  for (int i = 0; i < 32; i++) {
    ccol_retval rv = chttpsvr_router_use(ov_r, _global_mw, NULL);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }
  ccol_retval rv33 = chttpsvr_router_use(ov_r, _global_mw, NULL);
  REQUIRE_EQ((int)rv33, (int)ccol_not_permitted);

  ccol_retval rv =
      chttpsvr_router_on(ov_r, CHTTP_GET, "/ping", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
}

// The /mw-overflow-live sub-router has 32 middlewares plus the global one, one
// over CHTTPSVR_MAX_MW, so dispatch answers 500.
TEST(chttpserver, middleware_overflow_produces_500) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  chttpcli_response *resp = _get("/mw-overflow-live/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, middleware_overflow_streaming_produces_500) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  chttpcli_response *resp =
      _post("/mw-overflow-live/stream-ping", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpcli_resp_free(resp);
}

TEST(chttpserver, malformed_encoding_in_param_correct_method_returns_404) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

// The method-mismatch dry run must check percent-encoding like the real match,
// or a malformed param on the wrong method answers 405 instead of 404.
TEST(chttpserver,
     malformed_encoding_in_param_wrong_method_returns_404_not_405) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status =
      _raw_request("POST", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

// /hel%6Co decodes to /hello and must match it; the decoded segment must be NUL
// terminated before the comparison.
TEST(chttpserver, valid_percent_encoded_literal_segment_matches_route) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[4096] = {0};
  int status = _raw_request("GET", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "Hello, world!");
}

TEST(chttpserver,
     valid_percent_encoded_literal_segment_wrong_method_returns_405) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

TEST(chttpserver, malformed_encoding_in_literal_segment_returns_404) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_literal_segment_wrong_method_returns_404_not_405) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, on_null_srv_returns_invalid_args) {
  ccol_retval rv = chttpsvr_register_handler(CHTTPSVR_NULL, CHTTP_GET, "/any",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_router_returns_invalid_args) {
  ccol_retval rv =
      chttpsvr_router_on(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_router_returns_invalid_args) {
  ccol_retval rv =
      chttpsvr_router_on_stream(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, serve_null_cfg_uses_default) {
  REQUIRE_TRUE(g_srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_start(g_srv, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_not_permitted);
}

TEST(chttpserver, any_method_get) {
  chttpcli_response *resp = _get("/any-method");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, any_method_post) {
  chttpcli_response *resp = _post("/any-method", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "POST");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, any_method_put) {
  char buf[2048] = {0};
  int status = _raw_request("PUT", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_delete) {
  char buf[2048] = {0};
  int status = _raw_request("DELETE", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "DELETE");
}

TEST(chttpserver, any_method_with_path_param) {
  chttpcli_response *resp = _get("/any-method/42");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET:42");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, any_method_specific_wins_for_registered_method) {
  chttpcli_response *resp = _get("/any-with-specific");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, any_method_catches_unregistered_method) {
  char buf[2048] = {0};
  int status =
      _raw_request("PUT", "/any-with-specific", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_STREQ(body, "PUT");
}

TEST(chttpserver, any_method_first_wins_over_specific) {
  chttpcli_response *resp = _get("/any-first");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "GET");
  chttpcli_resp_free(resp);
}

// CHTTP_ANY matches only the seven known methods. An unknown method token gets
// a 501 before routing, with an empty body, so _any_method_handler never runs.
TEST(chttpserver, unrecognized_method_rejected_with_501_not_dispatched) {
  char buf[2048] = {0};
  int status = _raw_request("PROPFIND", "/any-method", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
  REQUIRE_TRUE(strstr(buf, "content-length:0") != NULL);
}

TEST(chttpserver, unrecognized_method_rejected_even_on_unmatched_path) {
  char buf[2048] = {0};
  int status =
      _raw_request("PROPFIND", "/no-such-route-at-all", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 501);
}

// Sends a POST with Connection: close and a body of body_len 'a' bytes to port
// and reads the response into buf. The max_body_size tests use it to send a
// body of an exact size. Returns the status, or -1 when no status line came
// back (a reset before the response, for example).
static int _raw_post_fixed_body(int port, const char *path, size_t body_len,
                                char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return -1;
  }
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
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  char *body = _decode_raw_body(buf);
  REQUIRE_TRUE(body != NULL);
  REQUIRE_EQ(strlen(body), (size_t)SMALL_BODY_MAX);
}

TEST(chttpserver, buffered_max_body_size_exceeded_rejected) {
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-echo",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 413);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, streaming_max_body_size_at_limit_succeeds) {
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, streaming_max_body_size_exceeded_reported) {
  char buf[4096] = {0};
  int status = _raw_post_fixed_body(TEST_PORT + 3, "/small-body-stream",
                                    SMALL_BODY_MAX + 1, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_msg_too_large") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

// max_body_size 0 means no limit, so a 200000-byte body must pass on a buffered
// and on a streaming route. A plain "> limit" check would answer 413 to every
// body.
TEST(chttpserver, buffered_max_body_size_zero_means_unlimited) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_POST, "/unlimited-body-echo", _echo_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 80;
  cfg.max_body_size = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/unlimited-body-stream", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// A malformed chunk size found by the worker becomes a 400 that the worker
// writes before the connection closes, instead of an early close with no
// response.
TEST(chttpserver, malformed_chunked_encoding_forces_connection_close) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /stream-error-report HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\n"
      "\r\n"
      "ZZZZ\r\n"
      "garbage-chunk-data\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[1024] = {0};
  _drain_socket_until_eof(fd, buf, sizeof(buf));
  close(fd);
  fd = -1;

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") == NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

// A valid 16-digit chunk size far above max_body_size must be refused when the
// size line is parsed, within a second, instead of waiting 30 s for data that
// never comes. The elapsed time tells the two apart.
TEST(chttpserver, oversized_chunk_size_hex_rejected_gracefully) {
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  struct timespec t0, t1;
  clock_gettime(CLOCK_MONOTONIC, &t0);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT + 3);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /small-body-echo HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: 999999999\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

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

// A trailer after the last chunk is parsed and the request is dispatched once:
// exactly one response comes back.
TEST(chttpserver, chunked_body_with_trailer_headers_handled_once) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
}

TEST(chttpserver, buffered_route_trailer_not_visible_via_req_header) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A trailer with the name of a header that a proxy set must not replace it,
// even though a repeated header resolves to its last occurrence.
TEST(chttpserver, trailer_cannot_override_a_real_request_header) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A "Connection: close" trailer must not change the keep-alive decision made
// from the header block.
TEST(chttpserver, trailer_cannot_turn_a_keep_alive_connection_into_closing) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A HTAB inside the target splits the request line differently in a front end;
// the parser refuses it with 400, not a routed 404.
TEST(chttpserver, htab_in_request_target_rejected) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 404") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

// Writes one raw request to g_srv and reads into buf until the server closes,
// with a bounded read. The rejection tests use it for requests that the server
// must refuse and close.
static void _raw_request_to_test_server(const char *req, char *buf,
                                        size_t buflen) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  char buf[1024];
  _raw_request_to_test_server(
      "GET /hello HTTP/1.1\r\nConnection: close\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, request_with_two_host_headers_gets_400) {
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
  char buf[1024];
  _raw_request_to_test_server("GET /hello HTTP/1.0\r\n\r\n", buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

// A server must accept an absolute-form target (RFC 7230 SS5.3.2) and route on
// its path.
TEST(chttpserver, absolute_form_request_target_is_served) {
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
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "root_subrouter") != NULL);
}

TEST(chttpserver, absolute_form_with_query_only_is_the_root_path) {
  char buf[1024];
  _raw_request_to_test_server(
      "GET http://example.com?a=1 HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "root_subrouter") != NULL);
}

TEST(chttpserver, unknown_scheme_in_target_is_not_stripped) {
  char buf[1024];
  _raw_request_to_test_server(
      "GET ftp://example.com/hello HTTP/1.1\r\nHost: example.com\r\n"
      "Connection: close\r\n\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, malformed_chunked_body_gets_400_and_not_500) {
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
  char buf[1024];
  _raw_request_to_test_server(
      "POST /echo-body HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked\r\nConnection: close\r\n\r\n"
      "F0000000\r\nx\r\n",
      buf, sizeof(buf));
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 413") != NULL);
}

TEST(chttpserver, config_default_ships_finite_timeouts_and_limits) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_GT(cfg.read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.idle_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.stream_read_timeout_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_body_read_duration_us, (uint64_t)0);
  REQUIRE_GT(cfg.max_response_write_duration_us, (uint64_t)0);
  REQUIRE_GT((long)cfg.max_connections, 0L);
}

TEST(chttpserver, streaming_route_trailer_never_visible_via_req_header) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A negative Content-Length would frame the body as the next request; the
// parser refuses it with 400 before routing.
TEST(chttpserver, negative_content_length_rejected) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: -1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

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

// A chunked coding that isn't last (RFC 7230 SS3.3.1) is refused with 400; a
// proxy that honours it anywhere would frame the body differently.
TEST(chttpserver, chunked_not_last_in_transfer_encoding_list_rejected) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, gzip\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

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

// An empty list element is ignored (RFC 7230 SS7), so "chunked," is the list
// "chunked" and the body is chunked. Counting the empty element would make
// chunked non-final, a 400 here and a framing disagreement with other
// recipients, which is what request smuggling needs.
TEST(chttpserver, trailing_comma_in_transfer_encoding_is_still_chunked) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "hello") != NULL);
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") == NULL);
}

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

    int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A chunked that isn't last, and a second chunked, stay refused whatever empty
// elements surround them.
TEST(chttpserver, empty_elements_do_not_weaken_the_chunked_refusals) {
  const char *const reqs[] = {
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: ,chunked, gzip\r\n\r\n",
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, ,chunked\r\n\r\n",
      "POST /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked,\r\nTransfer-Encoding: chunked\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons(TEST_PORT);
    REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

    int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// Posts a 20-byte body to /destroy-slow-body on the port that arg points to,
// one byte every 50 ms, then drains whatever comes back. It is the background
// traffic of destroy_while_worker_reading_slow_body_is_safe, keeping a worker
// in chttpsvr_req_read while the test destroys the server.
static void *_drip_body_bg_thread(void *arg) {
  int port = *(int *)arg;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return NULL;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
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

// The server and the done signal of one destroy-from-a-thread test. Each run
// has its own block on the heap: a test that times out detaches its thread and
// leaks the block, and a thread left over from an earlier test can then never
// signal a later test's wait.
typedef struct {
  chttpsvr srv;
  pthread_mutex_t mtx;
  pthread_cond_t cv;
  bool done;
} _destroy_hang_ctx_t;

// Destroys ctx->srv and then sets ctx->done under ctx->mtx. The destroy tests
// run it on a thread so they can wait for the destroy with a bound.
static void *_destroy_hang_thread_fn(void *arg) {
  _destroy_hang_ctx_t *ctx = (_destroy_hang_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  pthread_mutex_lock(&ctx->mtx);
  ctx->done = true;
  pthread_cond_broadcast(&ctx->cv);
  pthread_mutex_unlock(&ctx->mtx);
  return NULL;
}

// A worker blocked in a slow body still reads server state, so the destroy must
// wait for it. valgrind catches a use-after-free; the destroy runs on a thread
// with a bounded wait, so a hang fails this test only.
TEST(chttpserver, destroy_while_worker_reading_slow_body_is_safe) {
  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  REQUIRE_NE((void *)ctx, NULL);
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = chttpsvr_create(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_NULL) {
    free(ctx);
    REQUIRE_TRUE(false);
  }
  ccol_retval rv = chttpsvr_register_streaming_handler(
      ctx->srv, CHTTP_POST, "/destroy-slow-body", _stream_error_report_handler,
      NULL);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

  bool dispatched = false;
  struct timespec dispatch_deadline;
  clock_gettime(CLOCK_MONOTONIC, &dispatch_deadline);
  dispatch_deadline.tv_sec += 5;
  for (;;) {
    if (chttpsvr__in_flight_requests_for_tests(ctx->srv) > 0) {
      dispatched = true;
      break;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < dispatch_deadline.tv_sec ||
          (now.tv_sec == dispatch_deadline.tv_sec &&
           now.tv_nsec < dispatch_deadline.tv_nsec)))
      break;
    struct timespec nap = {0, 1000000L};
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
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    pthread_join(bg, NULL);
    REQUIRE_TRUE(false);
  }

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

  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    pthread_detach(destroy_th);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);
}

// Declares a large Content-Length on the port that arg points to, sends a few
// bytes and then neither sends nor closes. It is the stalled peer of
// destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts: with both
// read timeouts at 0 only the forced unblock of the destroy can free the worker
// that reads it.
static void *_stall_forever_bg_thread(void *arg) {
  int port = *(int *)arg;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return NULL;

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    fd = -1;
    return NULL;
  }
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
  if (write(fd, partial, strlen(partial)) != (ssize_t)strlen(partial)) {
    close(fd);
    fd = -1;
    return NULL;
  }

  char buf[16];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
  }
  close(fd);
  fd = -1;
  return NULL;
}

// With stream_read_timeout_us and max_body_read_duration_us at 0 (wait
// forever), a stalled peer holds a worker in chttpsvr_req_read. The destroy
// must still return, because it shuts down the connections that workers hold
// once its bounded wait runs out. A test hook shrinks those bounds to a few
// hundred milliseconds.
TEST(chttpserver,
     destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(300, 2000);

  _destroy_hang_ctx_t *ctx = malloc(sizeof(*ctx));
  if (ctx == NULL) {
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_NE((void *)ctx, NULL);
  }
  ctx->done = false;
  pthread_mutex_init(&ctx->mtx, NULL);
  pthread_cond_init(&ctx->cv, NULL);
  ctx->srv = chttpsvr_create(g_test_logger, NULL);
  if (ctx->srv == CHTTPSVR_NULL) {
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ccol_retval rv = chttpsvr_register_streaming_handler(
      ctx->srv, CHTTP_POST, "/stall-forever", _stream_echo_handler, NULL);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 29;
  cfg.stream_read_timeout_us = 0;
  cfg.max_body_read_duration_us = 0;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  rv = chttpsvr_start(ctx->srv, &cfg);
  if (rv != ccol_success) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_EQ((int)rv, (int)ccol_success);
  }

  int port = TEST_PORT + 29;
  pthread_t bg;
  if (pthread_create(&bg, NULL, _stall_forever_bg_thread, &port) != 0) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }

  bool dispatched = false;
  struct timespec dispatch_deadline;
  clock_gettime(CLOCK_MONOTONIC, &dispatch_deadline);
  dispatch_deadline.tv_sec += 5;
  for (;;) {
    if (chttpsvr__in_flight_requests_for_tests(ctx->srv) > 0) {
      dispatched = true;
      break;
    }
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < dispatch_deadline.tv_sec ||
          (now.tv_sec == dispatch_deadline.tv_sec &&
           now.tv_nsec < dispatch_deadline.tv_nsec)))
      break;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  if (!dispatched) {
    pthread_join(bg, NULL);
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(dispatched);
  }

  pthread_t destroy_th;
  if (pthread_create(&destroy_th, NULL, _destroy_hang_thread_fn, ctx) != 0) {
    chttpsvr_destroy(ctx->srv);
    free(ctx);
    pthread_join(bg, NULL);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }

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

  pthread_join(bg, NULL);

  if (!destroyed_in_time) {
    pthread_detach(destroy_th);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(destroyed_in_time);
  }

  pthread_join(destroy_th, NULL);
  pthread_mutex_destroy(&ctx->mtx);
  pthread_cond_destroy(&ctx->cv);
  free(ctx);

  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
}

// Tells _restart_race_pipeline_thread to stop.
static _Atomic bool g_restart_race_stop;

// Pipelines GETs over the keep-alive fd that arg points to until
// g_restart_race_stop is set or the connection fails. It keeps live requests
// going while restart_races_live_keep_alive_connection_is_safe restarts the
// server.
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

// A keep-alive connection pipelines requests while the server restarts many
// times. Every restart writes the worker pool pointer and the size limits that
// the connection's worker reads, so the check is a clean run under valgrind and
// ThreadSanitizer; each cycle uses a fresh port.
TEST(chttpserver, restart_races_live_keep_alive_connection_is_safe) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/restart-race",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 50;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 50));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  atomic_store(&g_restart_race_stop, false);
  pthread_t pipeline_thread;
  bool pipeline_thread_created =
      pthread_create(&pipeline_thread, NULL, _restart_race_pipeline_thread,
                     &fd) == 0;

  ccol_retval restart_rv[5];
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

// The per-gap read timeout is generous and the total cap short; the client
// sends part of the body once and stalls.
TEST(chttpserver, max_body_read_duration_exceeded_reports_ccol_timed_out) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/deadline-test", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 5;
  cfg.stream_read_timeout_us = 60000000;
  cfg.max_body_read_duration_us = 300000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 5));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *hdr =
      "POST /deadline-test HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Type: text/plain\r\n"
      "Content-Length: 10\r\n"
      "Connection: close\r\n"
      "\r\n"
      "abc";
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

  long elapsed_ms = (t_end.tv_sec - t_start.tv_sec) * 1000 +
                    (t_end.tv_nsec - t_start.tv_nsec) / 1000000;
  REQUIRE_LT(elapsed_ms, 20000L);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_body_read_duration_disabled_allows_slow_drip) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/deadline-disabled",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 6;
  cfg.stream_read_timeout_us = 5000000;
  cfg.max_body_read_duration_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  char buf[4096] = {0};
  int status =
      _raw_request_drip_body(TEST_PORT + 6, "POST", "/deadline-disabled",
                             "0123456789", 1, 80000, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);

  chttpsvr_destroy(srv);
}

// The per-call write timeout is generous and the total cap short. The client
// reads nothing for longer than the cap, so the write blocks; then it reads
// again. The deadline must have cut the response, so the client never gets all
// of it. The test compares byte counts, not times.
TEST(chttpserver, max_response_write_duration_exceeded_closes_connection) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/large-response",
                                             _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 60;
  cfg.response_write_timeout_us = 5000000;
  cfg.max_response_write_duration_us = 300000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 60));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  size_t write_parks_before = chttpsvr__park_kind_count_for_tests(2);
  int runs_before = atomic_load(&g_large_response_runs);
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  bool write_ended = false;
  for (int i = 0; i < 30000 && !write_ended; i++) {
    write_ended = chttpsvr__park_kind_count_for_tests(2) > write_parks_before ||
                  (atomic_load(&g_large_response_runs) > runs_before &&
                   chttpsvr__in_flight_requests_for_tests(srv) <= 0);
    if (!write_ended) usleep(1000);
  }
  struct timespec nap = {0, 700000000L};
  nanosleep(&nap, NULL);

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  size_t total = 0;
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) > 0) total += (size_t)r;
  close(fd);
  fd = -1;

  REQUIRE_TRUE(write_ended);
  REQUIRE_LT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_response_write_duration_disabled_allows_slow_reader) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/large-response-slow-ok", _large_response_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 61;
  cfg.response_write_timeout_us = 5000000;
  cfg.max_response_write_duration_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 61));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct timeval rcvtimeo = {30, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "GET /large-response-slow-ok HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[4096];
  size_t total = 0;
  ssize_t r;
  int reads = 0;
  while ((r = read(fd, buf, sizeof(buf))) > 0) {
    total += (size_t)r;
    reads++;
    if (reads % 8 == 0) {
      struct timespec nap = {0, 5000000L};
      nanosleep(&nap, NULL);
    }
  }
  close(fd);
  fd = -1;

  REQUIRE_GT(total, (size_t)_CHTTPSVR_TEST_LARGE_BODY_SIZE);

  chttpsvr_destroy(srv);
}

// A rejection is too small to block, so a test hook makes the next write
// deadline check of send_response report "expired".
TEST(chttpserver, max_response_write_duration_bounds_rejection_response) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 65;
  cfg.response_write_timeout_us = 5000000;
  cfg.max_response_write_duration_us = 300000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 65));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  chttpsvr__force_next_response_write_deadline_expired_for_tests();

  const char *req =
      "GET /no-such-route-xyz HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  ssize_t wn = write(fd, req, strlen(req));

  char buf[256];
  ssize_t r = (wn == (ssize_t)strlen(req)) ? read(fd, buf, sizeof(buf)) : -1;

  REQUIRE_EQ(wn, (ssize_t)strlen(req));

  REQUIRE_EQ((int)r, 0);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, create_with_null_logger_uses_internal_fatal_only_logger) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/null-logger-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpcli_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, create_with_logger_derives_and_leaves_parent_open) {
  clog parent = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_TRUE(parent != CLOG_NULL);

  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(parent, NULL);
  if (srv == CHTTPSVR_NULL) {
    clog_close(parent);
    REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  }

  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/derived-logger-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpcli_resp_free(resp);

  chttpsvr_destroy(srv);

  clog_info(parent, "parent logger still usable after server destroy");

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(parent, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_NULL);
  chttpsvr_destroy(srv2);

  clog_close(parent);
}

TEST(chttpserver, unix_socket_listen_and_round_trip) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/unix-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_%d.sock",
           (int)getpid());
  unlink(sock_path);

  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
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

// An explicit SO_SNDBUF or SO_RCVBUF turns Linux autotuning off, so a TCP
// connection keeps the kernel's sizes and only a unix:// connection gets raised
// buffers, which a library counter counts.
TEST(chttpserver, accepted_tcp_socket_keeps_kernel_buffer_autotuning) {
  size_t before_tcp = chttpsvr__accepted_sockbuf_raise_count_for_tests();
  char buf[2048] = {0};
  int status = _raw_request("GET", "/hello", NULL, buf, sizeof(buf));
  size_t after_tcp = chttpsvr__accepted_sockbuf_raise_count_for_tests();
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(after_tcp, before_tcp);

  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/unix-bufs",
                                            _hello_handler, NULL),
             (int)ccol_success);
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_bufs_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  size_t before_unix = chttpsvr__accepted_sockbuf_raise_count_for_tests();
  int fd ccol__destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
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
  size_t after_unix = chttpsvr__accepted_sockbuf_raise_count_for_tests();
  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
  unlink(sock_path);
  REQUIRE_EQ(ustatus, 200);
  REQUIRE_EQ(after_unix, before_unix + 1);
}

// Leaves a socket file at path that was bound and closed, with nobody
// listening, for the tests that start a server over a stale socket. Returns
// whether it worked.
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
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_test_stale_%d.sock",
           (int)getpid());
  REQUIRE_TRUE(_make_stale_unix_socket(sock_path));

  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/stale-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_UNIX, SOCK_STREAM, 0);
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "unix:///chttpsvr_test_nonexistent_dir_xyz/socket.sock";
  ccol_retval rv = chttpsvr_start(srv, &cfg);
  REQUIRE_NE((int)rv, (int)ccol_success);
  chttpsvr_destroy(srv);
}

// A path too long for sun_path fails the start with ENAMETOOLONG in the engine
// log, and an empty path with EINVAL.
TEST(chttpserver, unix_socket_path_too_long_reports_enametoolong) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  char host[512];
  memcpy(host, "unix:///tmp/", 12);
  memset(host + 12, 'p', sizeof(host) - 13);
  host[sizeof(host) - 1] = '\0';
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  errno = EBADF;
  ccol_retval too_long = chttpsvr_start(srv, &cfg);
  int too_long_errno = chttpsvr__last_listen_errno_for_tests();
  cfg.host = "unix://";
  errno = EBADF;
  ccol_retval empty = chttpsvr_start(srv, &cfg);
  int empty_errno = chttpsvr__last_listen_errno_for_tests();
  REQUIRE_NE((int)too_long, (int)ccol_success);
  REQUIRE_EQ(too_long_errno, ENAMETOOLONG);
  REQUIRE_NE((int)empty, (int)ccol_success);
  REQUIRE_EQ(empty_errno, EINVAL);
}

TEST(chttpserver, max_connections_enforced) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/maxconn-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 9;
  cfg.max_connections = 1;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 9));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd_a ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);

  int fd_b ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_b, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req =
      "GET /maxconn-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_b, req, strlen(req)), (ssize_t)strlen(req));

  struct pollfd pfd = {.fd = fd_b, .events = POLLIN};
  int pr = poll(&pfd, 1, 300);
  REQUIRE_EQ(pr, 0);

  close(fd_a);
  fd_a = -1;

  char buf[1024] = {0};
  int status = _read_one_http_response(fd_b, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  close(fd_b);
  fd_b = -1;
  chttpsvr_destroy(srv);
}

// At capacity the listener must pause, or level-triggered readiness of the
// backlog dispatches it over and over. A white-box counter of listener
// dispatches shows the difference.
TEST(chttpserver, max_connections_at_capacity_does_not_busy_loop) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/busyloop-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 62;
  cfg.max_connections = 2;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 62));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd_a ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  int fd_b ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
  REQUIRE_EQ(test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);

  int fd_c ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_c >= 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd_c, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  REQUIRE_EQ(test_connect(fd_c, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req_c =
      "GET /busyloop-hello HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: "
      "close\r\n\r\n";
  REQUIRE_EQ(write(fd_c, req_c, strlen(req_c)), (ssize_t)strlen(req_c));

  nanosleep(&nap, NULL);
  size_t before = chttpsvr__listener_dispatch_count_for_tests();
  struct timespec hold = {0, 300000000L};
  nanosleep(&hold, NULL);
  size_t after = chttpsvr__listener_dispatch_count_for_tests();
  size_t dispatch_delta = after - before;

  struct pollfd pfd_c = {.fd = fd_c, .events = POLLIN};
  int pr_c = poll(&pfd_c, 1, 0);

  close(fd_a);
  fd_a = -1;
  char buf[1024] = {0};
  int status_c = _read_one_http_response(fd_c, buf, sizeof(buf));

  close(fd_b);
  fd_b = -1;
  close(fd_c);
  fd_c = -1;
  chttpsvr_destroy(srv);

  REQUIRE_LT(dispatch_delta, (size_t)200);
  REQUIRE_EQ(pr_c, 0);
  REQUIRE_EQ(status_c, 200);
}

// Arms the allocator below: once set, every calloc fails. Accepting a
// connection makes exactly one calloc through the server's procs (the conn
// struct), so arming it after chttpsvr_start makes every later accept fail its
// allocation.
static _Atomic bool g_fail_every_calloc_for_backoff_test = false;
// The allocator of
// post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping: calloc
// fails while g_fail_every_calloc_for_backoff_test is set, and every other call
// passes through to the C library.
static void *_backoff_test_malloc(size_t n) { return malloc(n); }
static void _backoff_test_free(void *p) { free(p); }
static void *_backoff_test_calloc(size_t n, size_t s) {
  if (atomic_load(&g_fail_every_calloc_for_backoff_test)) return NULL;
  return calloc(n, s);
}
static void *_backoff_test_realloc(void *p, size_t s) { return realloc(p, s); }

// Waits at most 5 s until the process-wide count of listener pauses for
// resource pressure goes above before, and tells whether it did. The two accept
// backoff tests use it; the sweep never takes the count back, so a late poll
// can't miss a pause.
static bool _wait_for_listener_resource_pause(size_t before) {
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  deadline.tv_sec += 5;
  for (;;) {
    if (chttpsvr__listener_resource_pause_count_for_tests() > before)
      return true;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (!(now.tv_sec < deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec)))
      return false;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
}

// An allocation failure after a successful accept must not loop straight back
// to accept: it pauses the listener (a sleep would block the shared reactor)
// and the idle sweep resumes it once allocations work again. White-box counters
// show the pause, because the sweep can resume the listener at any moment.
TEST(chttpserver,
     post_accept_alloc_failure_pauses_and_resumes_instead_of_busy_looping) {
  ccol_memmgmt_procs mp = {_backoff_test_malloc, _backoff_test_free,
                           _backoff_test_calloc, _backoff_test_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/backoff-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 63;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 63));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  size_t pause_before =
      chttpsvr__listener_alloc_failure_pause_count_for_tests();
  size_t resource_pauses_before =
      chttpsvr__listener_resource_pause_count_for_tests();

  atomic_store(&g_fail_every_calloc_for_backoff_test, true);

  int fd_a ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  bool paused = _wait_for_listener_resource_pause(resource_pauses_before);
  size_t pause_after = chttpsvr__listener_alloc_failure_pause_count_for_tests();

  atomic_store(&g_fail_every_calloc_for_backoff_test, false);
  int fd_b ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  REQUIRE_EQ(pause_after - pause_before, (size_t)1);
  REQUIRE_TRUE(paused);
  REQUIRE_EQ(status_b, 200);
}

// The accept loop pauses for every errno that isn't transient, not only for the
// four resource-exhaustion ones. A per-server hook forces EINVAL on the next
// accept, and the pause counter (which the sweep can't take back) shows the
// reaction. A fresh connection then succeeds after the sweep resumes it.
TEST(chttpserver,
     accept_unclassified_errno_pauses_and_resumes_instead_of_busy_looping) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/accept-errno-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 83;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 83));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  size_t resource_pauses_before =
      chttpsvr__listener_resource_pause_count_for_tests();
  chttpsvr__force_next_accept_errno_for_tests(srv, EINVAL);

  int fd_a ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  struct timeval rcvtimeo_a = {5, 0};
  setsockopt(fd_a, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo_a, sizeof(rcvtimeo_a));
  REQUIRE_EQ(test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);

  bool paused = _wait_for_listener_resource_pause(resource_pauses_before);

  char eof_buf[16];
  ssize_t eof_r = read(fd_a, eof_buf, sizeof(eof_buf));
  close(fd_a);
  fd_a = -1;

  int fd_b ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

TEST(chttpserver, max_header_bytes_within_limit_succeeds) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdrcap-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpcli_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, max_header_bytes_exceeded_closes_connection) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdrcap-hello2",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 11;
  cfg.max_header_bytes = 128;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 11));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A request whose header contribution lands exactly on max_header_bytes passes,
// and one byte more fails.
TEST(chttpserver, max_header_bytes_exact_boundary) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdrcap-boundary",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  enum { MAX_HDR_BYTES = 200 };
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 79;
  cfg.max_header_bytes = MAX_HDR_BYTES;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  const char *request_line = "GET /hdrcap-boundary HTTP/1.1";
  const char *host_line = "Host: 127.0.0.1";
  size_t used = (strlen(request_line) + 2) + (strlen(host_line) + 2);
  REQUIRE_LT(used + 9, (size_t)MAX_HDR_BYTES);
  size_t budget_left = (size_t)MAX_HDR_BYTES - used;
  size_t pad_len = budget_left - 9;

  char padding[256];
  REQUIRE_LT(pad_len, sizeof(padding));
  memset(padding, 'a', pad_len);
  padding[pad_len] = '\0';

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
    int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
    int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE_TRUE(fd >= 0);
    REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
    REQUIRE_EQ(write(fd, req, (size_t)n), (ssize_t)n);
    struct timeval rcvtimeo = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
    char buf[512] = {0};
    _drain_socket_until_eof(fd, buf, sizeof(buf));
    close(fd);
    fd = -1;
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 400") != NULL);
    REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") == NULL);
  }

  chttpsvr_destroy(srv);
}

// Writes a body of about 26 MB in 64 KiB pieces, more than a slow reader takes
// in, for response_write_timeout_closes_slow_reader_connection.
static void _large_body_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  char chunk[65536];
  memset(chunk, 'x', sizeof(chunk));
  for (int i = 0; i < 400; i++) chttpsvr_resp_write(resp, chunk, sizeof(chunk));
}

// Tells whether the server's end of client_fd's TCP connection is still open in
// this process: a socket other than client_fd whose ports mirror client_fd's.
// response_write_timeout_closes_slow_reader_connection uses it to see that the
// server really closed.
static bool _server_end_of_conn_open(int client_fd) {
  struct sockaddr_in me, them;
  socklen_t ml = sizeof(me), tl = sizeof(them);
  if (getsockname(client_fd, (struct sockaddr *)&me, &ml) != 0 ||
      getpeername(client_fd, (struct sockaddr *)&them, &tl) != 0)
    return false;
  for (int fd = 3; fd < 4096; fd++) {
    struct sockaddr_in a, b;
    socklen_t al = sizeof(a), bl = sizeof(b);
    if (fd == client_fd || getsockname(fd, (struct sockaddr *)&a, &al) != 0 ||
        a.sin_family != AF_INET || a.sin_port != them.sin_port)
      continue;
    if (getpeername(fd, (struct sockaddr *)&b, &bl) == 0 &&
        b.sin_port == me.sin_port)
      return true;
  }
  return false;
}

TEST(chttpserver, response_write_timeout_closes_slow_reader_connection) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/big-body",
                                             _large_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 12;
  cfg.response_write_timeout_us = 200000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 12));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req = "GET /big-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  bool parked = false;
  for (int i = 0; i < 10000 && !parked; i++) {
    parked = chttpsvr__parked_count_for_tests(srv, 2) >= 1;
    if (!parked) usleep(1000);
  }
  bool closed = false;
  for (int i = 0; parked && i < 20 && !closed; i++) {
    chttpsvr__advance_slow_clock_for_tests(1000);
    chttpsvr__sweep_now_for_tests(srv);
    closed = chttpsvr__parked_count_for_tests(srv, 2) == 0 &&
             !_server_end_of_conn_open(fd);
    if (!closed) usleep(10000);
  }

  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));
  char buf[65536];
  size_t total = 0;
  ssize_t r;
  int read_errno = 0;
  long long last_read_ms = 0;
  for (;;) {
    struct timespec a, b;
    clock_gettime(CLOCK_MONOTONIC, &a);
    r = read(fd, buf, sizeof(buf));
    read_errno = r < 0 ? errno : 0;
    clock_gettime(CLOCK_MONOTONIC, &b);
    last_read_ms = (long long)(b.tv_sec - a.tv_sec) * 1000 +
                   (b.tv_nsec - a.tv_nsec) / 1000000;
    if (r <= 0) break;
    total += (size_t)r;
  }
  if (r != 0)
    fprintf(stderr,
            "[WRITE_TIMEOUT_DIAG] last read %zd errno %d (%s) after %lld ms, "
            "%zu bytes read\n",
            r, read_errno, strerror(read_errno), last_read_ms, total);

  REQUIRE_TRUE(parked);
  REQUIRE_TRUE(closed);
  REQUIRE_EQ(r, 0);
  REQUIRE_LT(total, (size_t)400 * 65536);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, idle_timeout_closes_unused_connection) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/idle-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 13;
  cfg.idle_timeout_us = 300000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 13));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);
  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 3000);
  REQUIRE_TRUE(pr > 0);
  char buf[16];
  ssize_t r = read(fd, buf, sizeof(buf));
  REQUIRE_EQ(r, 0);

  chttpsvr_destroy(srv);
}

// The sweep takes "now" once, and another thread can refresh a connection's
// last activity after that, so the elapsed time can be negative. Unsigned
// arithmetic would turn that into a huge value and evict a connection that has
// just become idle. The test calls the decision function directly.
TEST(chttpserver, idle_sweep_negative_elapsed_never_flagged_as_timed_out) {
  struct timespec now = {.tv_sec = 1000, .tv_nsec = 0};
  struct timespec last_activity_future_by_1ms = {.tv_sec = 1000,
                                                 .tv_nsec = 1000000L};
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 1));
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_future_by_1ms, 0));

  struct timespec last_activity_future_by_5s = {.tv_sec = 1005, .tv_nsec = 0};
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_future_by_5s, 300));

  struct timespec now_zero_nsec = {.tv_sec = 2000, .tv_nsec = 0};
  struct timespec last_activity_same_sec_later_nsec = {.tv_sec = 2000,
                                                       .tv_nsec = 500000000L};
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now_zero_nsec, last_activity_same_sec_later_nsec, 300));

  struct timespec last_activity_past_by_500ms = {.tv_sec = 999,
                                                 .tv_nsec = 500000000L};
  REQUIRE_TRUE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 300));
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_past_by_500ms, 1000));

  struct timespec last_activity_past_by_exactly_idle_ms = {
      .tv_sec = 999, .tv_nsec = 700000000L};
  REQUIRE_TRUE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_past_by_exactly_idle_ms, 300));

  struct timespec last_activity_past_by_299ms = {.tv_sec = 999,
                                                 .tv_nsec = 701000000L};
  REQUIRE_FALSE(chttpsvr__conn_idle_timed_out_for_tests(
      now, last_activity_past_by_299ms, 300));

  REQUIRE_TRUE(chttpsvr__conn_idle_timed_out_for_tests(now, now, 0));
}

// A cap expires only when its whole span has passed: time left rounds up,
// elapsed time rounds down. Each case is less than a millisecond from the
// limit, some across a tv_sec boundary, where truncating arithmetic fires
// early.
TEST(chttpserver, duration_caps_never_expire_early) {
  struct timespec deadline = {.tv_sec = 10, .tv_nsec = 900000L};
  struct timespec half_ms_before = {.tv_sec = 10, .tv_nsec = 400000L};
  unsigned t1 = 0;
  bool live1 =
      chttpsvr__shrink_timeout_at_for_tests(deadline, half_ms_before, &t1);
  struct timespec one_ns_before = {.tv_sec = 10, .tv_nsec = 899999L};
  unsigned t2 = 0;
  bool live2 =
      chttpsvr__shrink_timeout_at_for_tests(deadline, one_ns_before, &t2);
  unsigned t3 = 0;
  bool live3 = chttpsvr__shrink_timeout_at_for_tests(deadline, deadline, &t3);
  struct timespec far_deadline = {.tv_sec = 11, .tv_nsec = 500000001L};
  struct timespec start = {.tv_sec = 10, .tv_nsec = 0};
  unsigned t4 = 0;
  bool live4 = chttpsvr__shrink_timeout_at_for_tests(far_deadline, start, &t4);
  unsigned t5 = 100;
  bool live5 = chttpsvr__shrink_timeout_at_for_tests(far_deadline, start, &t5);

  struct timespec after_boundary = {.tv_sec = 1, .tv_nsec = 100000L};
  struct timespec before_boundary = {.tv_sec = 0, .tv_nsec = 999600000L};
  long long floor_ms =
      chttpsvr__ms_since_for_tests(after_boundary, before_boundary, false);
  long long ceil_ms =
      chttpsvr__ms_since_for_tests(after_boundary, before_boundary, true);
  long long ceil_same_sec =
      chttpsvr__ms_since_for_tests(deadline, half_ms_before, true);
  long long exact = chttpsvr__ms_since_for_tests(
      (struct timespec){.tv_sec = 5, .tv_nsec = 0},
      (struct timespec){.tv_sec = 3, .tv_nsec = 0}, true);
  bool idle_early = chttpsvr__conn_idle_timed_out_for_tests(after_boundary,
                                                            before_boundary, 1);
  bool idle_due = chttpsvr__conn_idle_timed_out_for_tests(
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

// A huge idle timeout must saturate at UINT_MAX - 1 ms; a multiple of 2^32 ms
// would wrap to 0, which turns the idle sweep off.
TEST(chttpserver, idle_timeout_us_overflow_saturates_not_wrapped) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 24;
  cfg.idle_timeout_us = UINT64_C(4294967296000);
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  REQUIRE_EQ(chttpsvr__idle_timeout_ms_for_tests(srv), UINT_MAX - 1u);
  chttpsvr_destroy(srv);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_NULL);
  chttpsvr_config cfg2 = CHTTPSVR_CONFIG_DEFAULT;
  cfg2.host = "127.0.0.1";
  cfg2.port = TEST_PORT + 25;
  cfg2.idle_timeout_us = 0;
  cfg2.read_timeout_us = UINT64_MAX;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg2), (int)ccol_success);
  REQUIRE_EQ(chttpsvr__idle_timeout_ms_for_tests(srv2), UINT_MAX - 1u);
  chttpsvr_destroy(srv2);

  chttpsvr srv4 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv4 != CHTTPSVR_NULL);
  chttpsvr_config cfg4 = CHTTPSVR_CONFIG_DEFAULT;
  cfg4.host = "127.0.0.1";
  cfg4.port = TEST_PORT + 25;
  cfg4.idle_timeout_us = 1;
  REQUIRE_EQ((int)chttpsvr_start(srv4, &cfg4), (int)ccol_success);
  REQUIRE_EQ(chttpsvr__idle_timeout_ms_for_tests(srv4), 1u);
  chttpsvr_destroy(srv4);

  chttpsvr srv3 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv3 != CHTTPSVR_NULL);
  chttpsvr_config cfg3 = CHTTPSVR_CONFIG_DEFAULT;
  cfg3.host = "127.0.0.1";
  cfg3.port = TEST_PORT + 26;
  cfg3.idle_timeout_us = 5000000;
  REQUIRE_EQ((int)chttpsvr_start(srv3, &cfg3), (int)ccol_success);
  REQUIRE_EQ(chttpsvr__idle_timeout_ms_for_tests(srv3), (unsigned)5000);
  chttpsvr_destroy(srv3);
}

// Starts a server on port with cfg and reads back into out the nine durations
// the start resolved, in milliseconds, for
// every_duration_converts_from_microseconds. Returns false when the start or
// the read fails.
static bool _resolved_ms(chttpsvr_config cfg, int port, unsigned out[9]) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return false;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  bool ok = chttpsvr_start(srv, &cfg) == ccol_success &&
            chttpsvr__resolved_ms_for_tests(srv, out);
  chttpsvr_destroy(srv);
  return ok;
}

// The config is in microseconds and the server keeps milliseconds. Defaults
// convert exactly, 0 picks the default (response_write_timeout_us 0 takes
// stream_read_timeout_us), sub-millisecond values round up,
// CHTTPSVR_NO_DEADLINE stays "no deadline" and the largest finite value
// saturates one below it.
TEST(chttpserver, every_duration_converts_from_microseconds) {
  unsigned def[9], zero[9], edge[9];
  chttpsvr_config c_def = CHTTPSVR_CONFIG_DEFAULT;

  chttpsvr_config c_zero = CHTTPSVR_CONFIG_DEFAULT;
  c_zero.response_write_timeout_us = 0;
  c_zero.min_transfer_rate_grace_us = 0;
  c_zero.body_memory_wait_timeout_us = 0;
  c_zero.streaming_queue_timeout_us = 0;
  c_zero.stream_read_timeout_us = 2500;
  c_zero.idle_timeout_us = 0;
  c_zero.read_timeout_us = 7000;

  chttpsvr_config c_edge = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/keepalive-opt-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpcli_resp_free(resp);

  chttpsvr_destroy(srv);
}

TEST(chttpserver, enable_reuseport_allows_second_listener_on_same_port) {
  chttpsvr srv1 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv1 != CHTTPSVR_NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 15;
  cfg.enable_reuseport = true;
  REQUIRE_EQ((int)chttpsvr_start(srv1, &cfg), (int)ccol_success);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv2, CHTTP_GET, "/reuseport-hello", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/reuseport-hello",
           TEST_PORT + 15);
  bool got_200 = false;
  for (int i = 0; i < 64 && !got_200; i++) {
    chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
    REQUIRE_NE((void *)req, NULL);
    chttp_request_set_header(req, "Connection", "close");
    chttpcli_response *resp = NULL;
    chttp_do(req, &resp);
    chttp_request_free(req);
    if (resp && resp->status_code == 200) got_200 = true;
    if (resp) chttpcli_resp_free(resp);
  }
  REQUIRE_TRUE(got_200);

  chttpsvr_destroy(srv1);
  chttpsvr_destroy(srv2);
}

TEST(chttpserver, ipv6_only_listener_still_serves_ipv6_traffic) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/v6only-hello",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET6, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_port = htons((uint16_t)(TEST_PORT + 16));
  REQUIRE_EQ(inet_pton(AF_INET6, "::1", &sa.sin6_addr), 1);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
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

// With ipv6_only an IPv6 wildcard listener must refuse IPv4 clients, which
// plain IPv6 traffic can't show.
TEST(chttpserver, ipv6_only_blocks_ipv4_mapped_connections) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/v6only-blocks-v4", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "::";
  cfg.port = TEST_PORT + 77;
  cfg.ipv6_only = true;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    fprintf(stderr,
            "[SKIP] ipv6_only_blocks_ipv4_mapped_connections: no IPv6 "
            "wildcard bind available in this environment\n");
    return;
  }

  int fd6 ccol__destructor(_close_scoped_fd) = socket(AF_INET6, SOCK_STREAM, 0);
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

  int fd4 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

TEST(chttpserver, head_request_suppresses_response_body) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_ANY, "/head-body",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 18;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 18));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req = "HEAD /head-body HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

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

  char *cl = strstr(buf, "content-length:");
  REQUIRE_TRUE(cl != NULL);
  unsigned long declared_len = strtoul(cl + 15, NULL, 10);
  REQUIRE_EQ(declared_len, (unsigned long)strlen("Hello, world!"));

  REQUIRE_EQ(total, (size_t)(hdr_end - buf) + 4);

  struct pollfd pfd = {.fd = fd, .events = POLLIN};
  int pr = poll(&pfd, 1, 200);
  REQUIRE_EQ(pr, 0);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);
}

TEST(chttpserver, head_request_on_rejected_route_still_has_no_body) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 19;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 19));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The fail-at-size allocator: malloc, calloc and realloc fail when the size
// equals g_fail_alloc_size (0 means never), so a test can fail one specific
// allocation of the server and leave the rest alone.
static _Atomic size_t g_fail_alloc_size = 0;

// The fail-at-size allocator: each allocating call returns NULL when its size
// equals g_fail_alloc_size and passes through to the C library otherwise. The
// allocation failure tests use it to fail one chosen allocation of the server.
static void *_fail_at_size_malloc(size_t n) {
  if (g_fail_alloc_size != 0 && n == g_fail_alloc_size) return NULL;
  return malloc(n);
}
static void _fail_at_size_free(void *p) { free(p); }
static void *_fail_at_size_calloc(size_t n, size_t s) {
  if (g_fail_alloc_size != 0 && n * s == g_fail_alloc_size) return NULL;
  return calloc(n, s);
}
static void *_fail_at_size_realloc(void *p, size_t s) {
  if (g_fail_alloc_size != 0 && s == g_fail_alloc_size) return NULL;
  return realloc(p, s);
}

// A failed copy of the body bytes that came with the headers must end in a 500,
// not in a worker that copies from a NULL pointer.
TEST(chttpserver,
     carry_over_alloc_failure_rejects_gracefully_instead_of_crashing) {
  size_t body_len = 6151;
  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_POST, "/carry-oom",
                                             _echo_body_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 20;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 20));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  char *wire = (char *)malloc((size_t)hn + body_len);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, head, (size_t)hn);
  memcpy(wire + hn, body, body_len);
  g_fail_alloc_size = body_len;
  ssize_t written = write(fd, wire, (size_t)hn + body_len);
  if (written != (ssize_t)((size_t)hn + body_len)) g_fail_alloc_size = 0;
  free(wire);
  free(body);
  REQUIRE_EQ(written, (ssize_t)((size_t)hn + body_len));

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0;
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_NULL);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-carry-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 21;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A failed growth of a streaming body buffer must reach the handler as
// ccol_not_enough_memory, not ccol_http_transfer_aborted, so a handler can tell
// a server out of memory from a misbehaving peer. The first growth always asks
// for 8192 bytes, so a tiny body triggers it.
TEST(chttpserver,
     streaming_body_buffer_alloc_failure_reports_not_enough_memory) {
  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/body-buf-oom", _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 82;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 82));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  g_fail_alloc_size = 0;
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_not_enough_memory") != NULL);
}

// A failed copy of a header name or value must end in a 500 through the reject
// path, not in a connection closed with no response.
TEST(
    chttpserver,
    on_header_alloc_failure_rejects_gracefully_instead_of_dropping_connection) {
  size_t value_strlen = 6201;
  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/hdr-oom",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 27;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 27));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  if (written != (ssize_t)total) g_fail_alloc_size = 0;
  REQUIRE_EQ(written, (ssize_t)total);
  free(wire);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0;
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;

  int fd2 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The same for the copy of the raw request path.
TEST(
    chttpserver,
    on_request_line_path_alloc_failure_rejects_gracefully_instead_of_dropping_connection) {
  size_t path_len = 6221;
  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/after-path-oom",
                                             _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 28;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 28));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *req_head = "GET /";
  const char *req_tail = " HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  size_t path_body_len = path_len - 1;
  size_t total = strlen(req_head) + path_body_len + strlen(req_tail);
  char *wire = (char *)malloc(total + 1);
  REQUIRE_TRUE(wire != NULL);
  memcpy(wire, req_head, strlen(req_head));
  memset(wire + strlen(req_head), 'p', path_body_len);
  memcpy(wire + strlen(req_head) + path_body_len, req_tail, strlen(req_tail));
  wire[total] = '\0';

  g_fail_alloc_size = path_len + 1;
  ssize_t written = write(fd, wire, total);
  if (written != (ssize_t)total) g_fail_alloc_size = 0;
  REQUIRE_EQ(written, (ssize_t)total);
  free(wire);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  g_fail_alloc_size = 0;
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);

  close(fd);
  fd = -1;

  int fd2 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// What _query_oom_handler saw. chttpsvr_req_query_oom must latch once a query
// result allocation fails and stay true for the rest of the request.
static _Atomic bool g_query_oom_flag_right_after_failure = false;
static _Atomic bool g_query_oom_flag_after_later_success = false;
static _Atomic size_t g_query_oom_failed_call_count = (size_t)-1;
static _Atomic size_t g_query_oom_later_call_count = (size_t)-1;
static _Atomic bool g_query_oom_failed_call_result_is_null = false;

// The number of values of "a" in the query, which gives the result array a
// distinctive size for the fail-at-size allocator.
#define QUERY_OOM_REPEATED_KEY_COUNT 91

// Fails the allocation of the result array of chttpsvr_req_query for "a", then
// records the result, the oom latch and what a later successful query sees in
// the g_query_oom_* globals. It drives the test of the chttpsvr_req_query_oom
// latch.
static void _query_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)ctx;
  size_t warmup_n = 0;
  chttpsvr_req_query(req, "does-not-exist", &warmup_n);

  g_fail_alloc_size = (QUERY_OOM_REPEATED_KEY_COUNT + 1) * sizeof(char *);
  size_t n = (size_t)-1;
  const char **vals = chttpsvr_req_query(req, "a", &n);
  g_fail_alloc_size = 0;

  atomic_store(&g_query_oom_failed_call_result_is_null, vals == NULL);
  atomic_store(&g_query_oom_failed_call_count, n);
  atomic_store(&g_query_oom_flag_right_after_failure,
               chttpsvr_req_query_oom(req));

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

  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/query-oom",
                                             _query_oom_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 76;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  REQUIRE_EQ(g_query_oom_later_call_count, (size_t)1);
  REQUIRE_TRUE(g_query_oom_flag_after_later_success);

  chttpsvr_destroy(srv);
}

// A key long enough that the query parser decodes it on the heap, with a size
// unlikely to match any other allocation.
#define QUERY_ONE_OOM_KEY_LEN 700

// What chttpsvr_req_query_one returned in _query_one_oom_handler.
static _Atomic int g_query_one_oom_rv = -999;

// Fails the key allocation of the lazy query parse inside
// chttpsvr_req_query_one and stores its return in g_query_one_oom_rv, for the
// test that it reports ccol_not_enough_memory and not ccol_key_not_found.
static void _query_one_oom_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  g_fail_alloc_size = QUERY_ONE_OOM_KEY_LEN + 1;
  const char *val = (const char *)0xdeadbeefUL;
  ccol_retval rv = chttpsvr_req_query_one(req, "irrelevant-key", &val);
  g_fail_alloc_size = 0;
  atomic_store(&g_query_one_oom_rv, (int)rv);
  chttpsvr_resp_write_str(resp, val ? "unexpected-value" : "ok");
}

TEST(chttpserver, req_query_one_reports_not_enough_memory_not_key_not_found) {
  g_query_one_oom_rv = -999;

  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/query-one-oom",
                                             _query_one_oom_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// When the worker's own stream preparation fails (forced by a test hook), a
// streaming route must answer 500 and must not run the handler against a
// missing stream.
TEST(chttpserver, stream_prepare_failure_in_worker_sends_500_not_bare_close) {
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_streaming_handler(
      srv, CHTTP_POST, "/stream-prepare-oom", _stream_error_report_handler,
      NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 22;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 22));
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  struct timeval rcvtimeo = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcvtimeo, sizeof(rcvtimeo));

  const char *body = "hello";
  char wire[256];
  int wn = snprintf(wire, sizeof(wire),
                    "POST /stream-prepare-oom HTTP/1.1\r\nHost: 127.0.0.1\r\n"
                    "Content-Length: %zu\r\n\r\n%s",
                    strlen(body), body);
  REQUIRE_GT(wn, 0);

  chttpsvr__force_stream_prepare_fail_for_tests(true);
  ssize_t written = write(fd, wire, (size_t)wn);
  if (written != (ssize_t)wn)
    chttpsvr__force_stream_prepare_fail_for_tests(false);
  REQUIRE_EQ(written, (ssize_t)wn);

  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  chttpsvr__force_stream_prepare_fail_for_tests(false);
  REQUIRE_GT(n, (ssize_t)0);
  buf[n] = '\0';
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 500") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err") == NULL);

  close(fd);
  fd = -1;
  chttpsvr_destroy(srv);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != CHTTPSVR_NULL);
  rv = chttpsvr_register_handler(srv2, CHTTP_GET, "/after-stream-prepare-oom",
                                 _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  cfg.port = TEST_PORT + 23;
  REQUIRE_EQ((int)chttpsvr_start(srv2, &cfg), (int)ccol_success);

  int fd2 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// A handler that writes a body and then sets 204 must not get the body on the
// wire, nor a Content-Length (RFC 9110 SS6.4.1).
TEST(chttpserver, response_204_never_sends_body_or_content_length) {
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 204\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 204);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

// Counts the occurrences of needle in the head of the response in buf, for the
// tests that check how many Content-Length or other fields a head carries.
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

// A 304 describes the stored representation, whose length the server doesn't
// know, so it writes no Content-Length.
TEST(chttpserver, response_304_carries_no_content_length_of_its_own) {
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 304\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 304);
  REQUIRE_TRUE(strstr(buf, "this-body-must-never-reach-the-wire") == NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, response_304_sends_the_content_length_that_the_handler_set) {
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
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body", "x-force-cl: 5\r\n",
                            buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "content-length:35\r\n") != NULL);
  REQUIRE_EQ(_count_in_head(buf, "ontent-"), 1);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\nthis-body-must-never-reach-the-wire") !=
               NULL);
}

// A HEAD route with no body and no length from the handler sends no
// Content-Length, not "content-length:0".
TEST(chttpserver, explicit_head_route_without_a_body_invents_no_length) {
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/head-explicit", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "x-explicit-head:1") != NULL);
  REQUIRE_TRUE(strstr(buf, "content-length:") == NULL);
}

TEST(chttpserver, explicit_head_route_sends_the_content_length_of_the_handler) {
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
  chttpcli_response *resp = _get("/add-header");
  REQUIRE_TRUE(resp != NULL);
  int status = resp->status_code;
  size_t cookies = chttpcli_resp_header_count(resp, "set-cookie");
  const char *c0 = chttpcli_resp_header_at(resp, "set-cookie", 0);
  const char *c1 = chttpcli_resp_header_at(resp, "set-cookie", 1);
  const char *c2 = chttpcli_resp_header_at(resp, "set-cookie", 2);
  bool order = c0 && c1 && c2 && strcmp(c0, "a=1") == 0 &&
               strcmp(c1, "b=2") == 0 && strcmp(c2, "c=3") == 0;
  size_t singles = chttpcli_resp_header_count(resp, "x-single");
  const char *single = chttpcli_resp_header(resp, "x-single");
  bool single_final = single && strcmp(single, "final") == 0;
  size_t dates = chttpcli_resp_header_count(resp, "date");
  chttpcli_resp_free(resp);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(cookies, (size_t)3);
  REQUIRE_TRUE(order);
  REQUIRE_EQ(singles, (size_t)1);
  REQUIRE_TRUE(single_final);
  REQUIRE_EQ(dates, (size_t)1);
}

TEST(chttpserver, resp_header_content_length_accepts_only_a_number) {
  chttpcli_response *resp = _get("/content-length-validation");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_TRUE(resp->body != NULL);
  REQUIRE_STREQ(resp->body, "yyyyyyyynnnnnnnnnnnnnnnnyy");
  chttpcli_resp_free(resp);
}

TEST(chttpserver, resp_add_header_keeps_every_field_in_order) {
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
  char buf[2048] = {0};
  int status = _raw_request("GET", "/status-with-body",
                            "x-force-status: 199\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 500);
  REQUIRE_TRUE(strstr(buf, "content-length:") != NULL);
}

TEST(chttpserver, large_response_headers_still_delivered_in_full) {
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

// A handler's "Connection: close" on a request the server keeps alive must not
// reach the wire, and the connection serves a second request.
TEST(chttpserver,
     explicit_connection_close_header_ignored_when_actually_kept_alive) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  close(fd);
  fd = -1;
}

// Calls next and nothing else. The handle lifecycle tests below register it
// when they need a call that resolves the server. Those tests check the
// generation-tagged handle: a second destroy, concurrent or not, and a destroy
// from the server's own worker are fatal errors instead of a double free. They
// fork, because ccol__fatal_err aborts, and use NULL loggers so they don't
// depend on the state of _setup.
static void _noop_middleware_for_lifecycle_tests(chttpsvr_req *req,
                                                 chttpsvr_resp *resp, void *ctx,
                                                 chttpsvr_next_fn next) {
  (void)ctx;
  if (next) next(req, resp);
}

TEST(chttpserver, stream_error_of_no_request_is_invalid_args) {
  REQUIRE_EQ((int)chttpsvr_req_stream_error(NULL), (int)ccol_invalid_args);
}

// A closed parent logger fails the create with an err_str; clog_derive() would
// end the process for it.
TEST(chttpsvr_handle_lifecycle, create_with_a_closed_logger_fails_cleanly) {
  clog parent = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_NE(parent, CLOG_NULL);
  clog stale = parent;
  clog_close(parent);

  char *err = NULL;
  chttpsvr srv = chttpsvr_create(stale, &err);
  REQUIRE_EQ(srv, CHTTPSVR_NULL);
  REQUIRE_NE((void *)err, NULL);
  REQUIRE_NE((void *)strstr(err, "the parent logger is not open"), NULL);

  clog live = clog_create_from_fd(2, CLOG_INFO, NULL);
  REQUIRE_NE(live, CLOG_NULL);
  err = NULL;
  chttpsvr ok = chttpsvr_create(live, &err);
  bool created = ok != CHTTPSVR_NULL;
  if (created) chttpsvr_destroy(ok);
  clog_close(live);
  REQUIRE_TRUE(created);
}

TEST(chttpsvr_handle_lifecycle, a_destroyed_server_keeps_no_root_router) {
  size_t before = 0, after_plain = 0, after_sub = 0;
  bool array = false;
  chttpsvr__router_shell_registry_state_for_tests(&before, &array);
  bool created = true;
  for (int i = 0; i < 20; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) {
      created = false;
      continue;
    }
    chttpsvr_destroy(srv);
  }
  chttpsvr__router_shell_registry_state_for_tests(&after_plain, &array);
  bool subrouted = true;
  for (int i = 0; i < 5; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) {
      created = false;
      continue;
    }
    if (!chttpsvr_subrouter(srv, "/api")) subrouted = false;
    chttpsvr_destroy(srv);
  }
  chttpsvr__router_shell_registry_state_for_tests(&after_sub, &array);
  REQUIRE_TRUE(created);
  REQUIRE_TRUE(subrouted);
  REQUIRE_EQ(after_plain, before);
  REQUIRE_EQ(after_sub, before + 5);
}

TEST(chttpsvr_handle_lifecycle, sequential_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  FORK_TEST_SCOPE();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) test_child_exit(2);
    chttpsvr stale = srv;
    chttpsvr_destroy(srv);
    chttpsvr__destroy(stale);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The handle that one destroying thread gets.
typedef struct {
  chttpsvr h;
} concurrent_svr_destroy_arg_t;

// Destroys a->h. concurrent_double_destroy_is_fatal runs two of these on one
// handle.
static void *concurrent_svr_destroy_thread(void *arg) {
  concurrent_svr_destroy_arg_t *a = (concurrent_svr_destroy_arg_t *)arg;
  chttpsvr__destroy(a->h);
  return NULL;
}

TEST(chttpsvr_handle_lifecycle, concurrent_double_destroy_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  FORK_TEST_SCOPE();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) test_child_exit(2);
    concurrent_svr_destroy_arg_t a1 = {.h = srv};
    concurrent_svr_destroy_arg_t a2 = {.h = srv};
    pthread_t t1, t2;
    pthread_create(&t1, NULL, concurrent_svr_destroy_thread, &a1);
    pthread_create(&t2, NULL, concurrent_svr_destroy_thread, &a2);
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A handler that destroys its own server would wait for its own request. The
// test marks the thread as a worker with a hook, because a real start in a
// forked child would hang on the inherited reactor, which no thread runs.
TEST(chttpsvr_handle_lifecycle, destroy_from_within_own_handler_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  FORK_TEST_SCOPE();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) test_child_exit(2);
    chttpsvr__mark_self_as_worker_for_tests(srv);
    chttpsvr__destroy(srv);
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// A handler that waits for the engine would wait for its own pool to drain, a
// deadlock of the whole engine. Marking the thread as a worker of any server is
// enough, so no dispatch or reactor is needed.
TEST(chttpsvr_handle_lifecycle, engine_wait_from_within_own_handler_is_fatal) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  FORK_TEST_SCOPE();
  pid_t pid = fork();
  if (pid == 0) {
    test_child_no_core();
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) test_child_exit(2);
    chttpsvr__mark_self_as_worker_for_tests(srv);
    chttpsvr_engine_wait();
    test_child_exit(0);
  }
  REQUIRE_NE(pid, -1);
  int status = 0;
  REQUIRE_TRUE(test_wait_child_bounded(pid, 30000, &status));
  REQUIRE_TRUE(WIFSIGNALED(status));
  REQUIRE_EQ(WTERMSIG(status), SIGABRT);
}

// The server of the self-restart test and what the restart returned. The
// handler reaches the server from a worker thread, so it is a global.
static chttpsvr g_self_restart_srv_for_lifecycle_test = CHTTPSVR_NULL;
static _Atomic int g_self_restart_result_for_lifecycle_test = -1;

// Stops its own server, starts it again and stores the start's result in
// g_self_restart_result_for_lifecycle_test, for
// restart_from_within_own_handler_returns_not_permitted.
static void _self_restarting_handler_for_lifecycle_test(chttpsvr_req *req,
                                                        chttpsvr_resp *resp,
                                                        void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_stop(g_self_restart_srv_for_lifecycle_test);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval rv = chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  atomic_store(&g_self_restart_result_for_lifecycle_test, (int)rv);
  chttpsvr_resp_write_str(resp, "done");
}

// Destroys g_self_restart_srv_for_lifecycle_test if it exists. The self-restart
// test calls it before a failing REQUIRE returns: a global can't carry a
// cleanup attribute, and a leaked server would hang the engine wait of
// _teardown.
static void _destroy_self_restart_test_server_if_live(void) {
  if (g_self_restart_srv_for_lifecycle_test != CHTTPSVR_NULL)
    chttpsvr_destroy(g_self_restart_srv_for_lifecycle_test);
}

// A start from the server's own handler refuses with ccol_not_permitted instead
// of hanging, and a later restart from another thread still works.
TEST(chttpsvr_handle_lifecycle,
     restart_from_within_own_handler_returns_not_permitted) {
  atomic_store(&g_self_restart_result_for_lifecycle_test, -1);
  g_self_restart_srv_for_lifecycle_test = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(g_self_restart_srv_for_lifecycle_test, CHTTPSVR_NULL);
  ccol_retval reg_rv = chttpsvr_register_handler(
      g_self_restart_srv_for_lifecycle_test, CHTTP_GET, "/self-restart",
      _self_restarting_handler_for_lifecycle_test, NULL);
  if (reg_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)reg_rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 71;
  ccol_retval start_rv =
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

  int fd ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

  if (status != 200) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(status, 200);
  int self_restart_result =
      atomic_load(&g_self_restart_result_for_lifecycle_test);
  if (self_restart_result != (int)ccol_not_permitted)
    _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ(self_restart_result, (int)ccol_not_permitted);

  ccol_retval restart_rv =
      chttpsvr_start(g_self_restart_srv_for_lifecycle_test, &cfg);
  if (restart_rv != ccol_success) _destroy_self_restart_test_server_if_live();
  REQUIRE_EQ((int)restart_rv, (int)ccol_success);

  int fd2 ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
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

// The handle that use_setter_thread races against a destroy.
typedef struct {
  chttpsvr h;
} use_setter_arg_t;

// Calls chttpsvr_use on a->h. resolve_unpin_race_stress races it against a
// destroy.
static void *use_setter_thread(void *arg) {
  use_setter_arg_t *a = (use_setter_arg_t *)arg;
  chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
  return NULL;
}

// Races chttpsvr_use (resolve, pin, short critical section, unpin) against a
// destroy, on a fresh server each time. A use-after-free shows under valgrind
// or a sanitizer.
TEST(chttpsvr_handle_lifecycle, resolve_unpin_race_stress) {
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) REQUIRE_NE(srv, CHTTPSVR_NULL);

    use_setter_arg_t setter_arg = {.h = srv};
    concurrent_svr_destroy_arg_t destroy_arg = {.h = srv};
    pthread_t setter_tid, destroy_tid;
    int setter_rc =
        pthread_create(&setter_tid, NULL, use_setter_thread, &setter_arg);
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

// The sub-router that router_use_setter_thread uses.
typedef struct {
  chttpsvr_router *router;
} router_use_setter_arg_t;

// Adds a route to a->router. router_resolve_unpin_race_stress races it against
// a destroy.
static void *router_use_setter_thread(void *arg) {
  router_use_setter_arg_t *a = (router_use_setter_arg_t *)arg;
  chttpsvr_router_on(a->router, CHTTP_GET, "/race", _hello_handler, NULL);
  return NULL;
}

// The same race through chttpsvr_router_on, which resolves the owning server
// like the other router calls.
TEST(chttpsvr_handle_lifecycle, router_resolve_unpin_race_stress) {
  enum { ITERATIONS = 30 };
  for (int i = 0; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) REQUIRE_NE(srv, CHTTPSVR_NULL);
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

#if CCOL_FORK_SAFETY_REQUIRED
// The fork-safety tests: they need the atfork handlers, which a
// CCOL_FORK_SAFETY_REQUIRED=0 build leaves out. This is the feeder thread's
// server and its stop flag.
typedef struct {
  chttpsvr h;
  _Atomic int stop;
} fork_feeder_arg_t;

// Calls chttpsvr_use on a->h until a->stop is set, to keep the server's locks
// busy while fork_does_not_inherit_a_locked_mutex forks. The yield after each
// call keeps the loop affordable under valgrind, which runs one thread at a
// time.
static void *fork_feeder_thread(void *arg) {
  fork_feeder_arg_t *a = (fork_feeder_arg_t *)arg;
  while (!atomic_load(&a->stop)) {
    chttpsvr_use(a->h, _noop_middleware_for_lifecycle_tests, NULL);
    sched_yield();
  }
  return NULL;
}

// Forks again and again while a feeder thread keeps the slot table and a
// server's locks busy, after a short burst of creates and destroys. A child
// that inherits a locked mutex hangs in its chttpsvr_use; an alarm bounds each
// child.
TEST(chttpsvr_handle_lifecycle, fork_does_not_inherit_a_locked_mutex) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  FORK_TEST_SCOPE();
  chttpsvr feeder_srv = chttpsvr_create(CLOG_NULL, NULL);
  if (feeder_srv == CHTTPSVR_NULL) REQUIRE_NE(feeder_srv, CHTTPSVR_NULL);
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
  pid_t bad_fork_pid = 0;
  pid_t bad_waitpid_pid = 0;
  for (int i = 0; i < TRIALS && bad_fork_pid == 0 && bad_waitpid_pid == 0;
       i++) {
    for (int b = 0; b < BURST; b++) {
      chttpsvr burst_srv = chttpsvr_create(CLOG_NULL, NULL);
      if (burst_srv != CHTTPSVR_NULL) chttpsvr_destroy(burst_srv);
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
      alarm(5);
      chttpsvr_use(feeder_srv, _noop_middleware_for_lifecycle_tests, NULL);
      test_child_exit(0);
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

// The server that _fork_stop_race_stop_thread stops.
static chttpsvr g_fork_stop_race_srv = CHTTPSVR_NULL;

// Stops g_fork_stop_race_srv. The test parks this stop in a race hook and forks
// while it is there.
static void *_fork_stop_race_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_stop(g_fork_stop_race_srv);
  return NULL;
}

// A fork that lands in the middle of a stop must leave the child's server idle,
// or a start in the child polls the stopping state for ever. A race hook parks
// the stop there before the fork.
TEST(chttpsvr_handle_lifecycle,
     fork_mid_stop_internal_does_not_hang_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  FORK_TEST_SCOPE();
  g_fork_stop_race_srv = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(g_fork_stop_race_srv, CHTTPSVR_NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 73;
  ccol_retval start_rv = chttpsvr_start(g_fork_stop_race_srv, &cfg);
  if (start_rv != ccol_success) chttpsvr_destroy(g_fork_stop_race_srv);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  chttpsvr__arm_stop_race_hook_for_tests();

  pthread_t stop_th;
  int create_rv =
      pthread_create(&stop_th, NULL, _fork_stop_race_stop_thread, NULL);
  if (create_rv != 0) {
    chttpsvr__release_stop_race_hook_for_tests();
    chttpsvr_destroy(g_fork_stop_race_srv);
  }
  REQUIRE_EQ(create_rv, 0);

  chttpsvr__wait_stop_race_hook_entered_for_tests();

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
    alarm(30);
    chttpsvr_config restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 73;
    chttpsvr_start(g_fork_stop_race_srv, &restart_cfg);
    test_child_exit(0);
  } else if (!fork_failed) {
    int status = 0;
    if (waitpid(pid, &status, 0) != pid) {
      waitpid_failed = true;
    } else {
      child_hung = !WIFEXITED(status);
    }
  }

  chttpsvr__release_stop_race_hook_for_tests();
  bool stop_th_joined = _bounded_join(stop_th, NULL);
  if (!stop_th_joined) pthread_detach(stop_th);
  chttpsvr_destroy(g_fork_stop_race_srv);

  REQUIRE_FALSE(fork_failed);
  REQUIRE_FALSE(waitpid_failed);
  REQUIRE_FALSE(child_hung);
  REQUIRE_TRUE(stop_th_joined);
}

// The server that _fork_starting_race_start_thread starts, and the result.
static chttpsvr g_fork_starting_race_srv = CHTTPSVR_NULL;
static ccol_retval g_fork_starting_race_start_rv = ccol_success;

// Starts g_fork_starting_race_srv on TEST_PORT + 74 and stores the result. The
// test parks this start in a race hook and forks while it is there.
static void *_fork_starting_race_start_thread(void *arg) {
  (void)arg;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 74;
  g_fork_starting_race_start_rv =
      chttpsvr_start(g_fork_starting_race_srv, &cfg);
  return NULL;
}

// A fork in the middle of a start must leave the child's server idle, or every
// start in the child refuses with ccol_not_permitted. A race hook parks the
// start there before the fork.
TEST(chttpsvr_handle_lifecycle,
     fork_mid_start_internal_does_not_disable_start_in_child) {
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  FORK_TEST_SCOPE();
  g_fork_starting_race_srv = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(g_fork_starting_race_srv, CHTTPSVR_NULL);
  g_fork_starting_race_start_rv = ccol_success;

  chttpsvr__arm_start_race_hook_for_tests();

  pthread_t start_th;
  int create_rv =
      pthread_create(&start_th, NULL, _fork_starting_race_start_thread, NULL);
  if (create_rv != 0) {
    chttpsvr__release_start_race_hook_for_tests();
    chttpsvr_destroy(g_fork_starting_race_srv);
  }
  REQUIRE_EQ(create_rv, 0);

  chttpsvr__wait_start_race_hook_entered_for_tests();

  int pipefd[2];
  int pipe_rv = pipe(pipefd);
  if (pipe_rv != 0) {
    chttpsvr__release_start_race_hook_for_tests();
    bool start_th_joined = _bounded_join(start_th, NULL);
    if (!start_th_joined) pthread_detach(start_th);
    chttpsvr_destroy(g_fork_starting_race_srv);
    REQUIRE_EQ(pipe_rv, 0);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    chttpsvr__release_start_race_hook_for_tests();
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
    alarm(30);
    chttpsvr_config restart_cfg = CHTTPSVR_CONFIG_DEFAULT;
    restart_cfg.host = "127.0.0.1";
    restart_cfg.port = TEST_PORT + 75;
    ccol_retval child_rv =
        chttpsvr_start(g_fork_starting_race_srv, &restart_cfg);
    char ok = (child_rv != ccol_not_permitted) ? 1 : 0;
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  bool waitpid_failed = waitpid(pid, &status, 0) != pid;
  bool child_hung = !waitpid_failed && !WIFEXITED(status);

  chttpsvr__release_start_race_hook_for_tests();
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
#endif

// The server to destroy and where to report that the destroy returned.
typedef struct {
  chttpsvr h;
  _Atomic bool *returned;
} listener_race_destroy_arg_t;

// Destroys a->h and sets *a->returned, so
// destroy_waits_for_in_progress_listener_dispatch can see whether the destroy
// returned while a dispatch was held.
static void *listener_race_destroy_thread(void *arg) {
  listener_race_destroy_arg_t *a = (listener_race_destroy_arg_t *)arg;
  chttpsvr__destroy(a->h);
  atomic_store(a->returned, true);
  return NULL;
}

// The listener callback gets the server as a bare pointer, outside the handle's
// pin. A race hook holds a dispatch in progress and the destroy must not free
// the server under it.
TEST(chttpsvr_handle_lifecycle,
     destroy_waits_for_in_progress_listener_dispatch) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_NULL);

  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/listener-race-hello", _hello_handler, NULL);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 64;
  ccol_retval start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  chttpsvr__arm_listener_dispatch_race_hook_for_tests();

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 64));
  int pton_rv = inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  if (pton_rv != 1) {
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_EQ(pton_rv, 1);
  int fd_a ccol__destructor(_close_scoped_fd) = socket(AF_INET, SOCK_STREAM, 0);
  if (fd_a < 0) {
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_TRUE(fd_a >= 0);
  int connect_rv = test_connect(fd_a, (struct sockaddr *)&sa, sizeof(sa));
  if (connect_rv != 0) {
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }
  REQUIRE_EQ(connect_rv, 0);

  chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests();

  _Atomic bool destroy_returned = false;
  listener_race_destroy_arg_t darg = {.h = srv, .returned = &destroy_returned};
  pthread_t destroy_tid;
  bool destroy_th_created =
      pthread_create(&destroy_tid, NULL, listener_race_destroy_thread, &darg) ==
      0;
  if (!destroy_th_created) {
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    chttpsvr_destroy(srv);
  }

  bool still_blocked = false;
  if (destroy_th_created) {
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    still_blocked = !atomic_load(&destroy_returned);
  }

  chttpsvr__release_listener_dispatch_race_hook_for_tests();
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

// Polls for at most ms milliseconds until the watched server's final free has
// run, and tells whether it did. The tests of late listener dispatches and of
// route lifetime use it to see when the server is really gone.
static bool _wait_watched_finish_destroy(unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    if (chttpsvr__watched_finish_destroy_ran_for_tests()) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return chttpsvr__watched_finish_destroy_ran_for_tests();
}

// The server to destroy and whether the destroy returned.
typedef struct {
  chttpsvr h;
  _Atomic bool returned;
} entry_race_destroy_arg_t;

// Destroys a->h and sets a->returned, so the test of a dispatch that has not
// pinned can see when the destroy returns.
static void *_entry_race_destroy_thread(void *arg) {
  entry_race_destroy_arg_t *a = (entry_race_destroy_arg_t *)arg;
  chttpsvr__destroy(a->h);
  atomic_store(&a->returned, true);
  return NULL;
}

// Polls for at most ms milliseconds until the destroy thread of a sets
// a->returned, and tells whether it did.
static bool _wait_entry_race_destroy_returned(entry_race_destroy_arg_t *a,
                                              unsigned ms) {
  for (unsigned i = 0; i < ms; i++) {
    if (atomic_load(&a->returned)) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return atomic_load(&a->returned);
}

// A dispatch that starts before the stop removes the listener holds no pin. The
// listener registration keeps a reference on the server until its on_removed,
// so the final free waits for the parked dispatch, and the dispatch sees its
// listener is gone and runs no accept loop.
TEST(chttpsvr_handle_lifecycle,
     destroy_keeps_server_alive_for_a_listener_dispatch_that_has_not_pinned) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(
      srv, CHTTP_GET, "/entry-race-hello", _hello_handler, NULL);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 110;
  rv = chttpsvr_start(srv, &cfg);
  if (rv != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr__watch_finish_destroy_for_tests(srv);
  chttpsvr__arm_listener_dispatch_entry_race_hook_for_tests();

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 110));
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  bool connected =
      fd >= 0 && test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0;
  if (!connected) {
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    if (fd >= 0) close(fd);
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(connected);
  }

  chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests();
  size_t dispatches_before = chttpsvr__listener_dispatch_count_for_tests();

  entry_race_destroy_arg_t darg = {.h = srv, .returned = false};
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, _entry_race_destroy_thread, &darg) == 0;
  if (!created) chttpsvr_destroy(srv);

  bool destroy_returned =
      created ? _wait_entry_race_destroy_returned(&darg, 20000) : true;
  bool freed_under_dispatch = chttpsvr__watched_finish_destroy_ran_for_tests();

  chttpsvr__release_listener_dispatch_race_hook_for_tests();
  bool joined = created ? _bounded_join(tid, NULL) : true;
  if (created && !joined) pthread_detach(tid);
  bool finished = _wait_watched_finish_destroy(10000);
  size_t dispatches_after = chttpsvr__listener_dispatch_count_for_tests();
  close(fd);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(destroy_returned);
  REQUIRE_FALSE(freed_under_dispatch);
  REQUIRE_TRUE(joined);
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(dispatches_after, dispatches_before);
}

// Signals between the test and _route_lifetime_slow_handler.
static _Atomic bool g_route_lifetime_handler_entered = false;
static _Atomic bool g_route_lifetime_handler_release = false;

// Reads its param, sets g_route_lifetime_handler_entered and waits (at most 30
// s) for g_route_lifetime_handler_release. It is the handler that outlives the
// destroy's drain in
// route_data_outlives_a_connection_that_outlives_the_destroy_drain.
static void _route_lifetime_slow_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                         void *ctx) {
  (void)ctx;
  (void)chttpsvr_req_param(req, "id");
  atomic_store(&g_route_lifetime_handler_entered, true);
  for (int i = 0; i < 30000 && !atomic_load(&g_route_lifetime_handler_release);
       i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  chttpsvr_resp_write_str(resp, "late");
}

// A connection whose handler outlives the destroy's drain is freed later and
// can still read its matched route, so the route data must live until the final
// free. The reactor is parked in a race hook until after the destroy returns,
// and a library counter of early route teardowns must stay 0.
TEST(chttpsvr_handle_lifecycle,
     route_data_outlives_a_connection_that_outlives_the_destroy_drain) {
  atomic_store(&g_route_lifetime_handler_entered, false);
  atomic_store(&g_route_lifetime_handler_release, false);

  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_NE(srv, CHTTPSVR_NULL);
  ccol_retval rv1 =
      chttpsvr_register_handler(srv, CHTTP_GET, "/route-lifetime/{id}",
                                _route_lifetime_slow_handler, NULL);
  ccol_retval rv2 = chttpsvr_register_handler(
      srv, CHTTP_GET, "/route-lifetime-fast", _hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 111;
  ccol_retval rv3 = (rv1 == ccol_success && rv2 == ccol_success)
                        ? chttpsvr_start(srv, &cfg)
                        : ccol_invalid_args;
  if (rv3 != ccol_success) chttpsvr_destroy(srv);
  REQUIRE_EQ((int)rv1, (int)ccol_success);
  REQUIRE_EQ((int)rv2, (int)ccol_success);
  REQUIRE_EQ((int)rv3, (int)ccol_success);
  chttpsvr__watch_finish_destroy_for_tests(srv);
  size_t violations_before =
      chttpsvr__route_teardown_with_live_connections_for_tests();

  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)(TEST_PORT + 111));
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);

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

  int fd_b = -1;
  if (ok) {
    chttpsvr__arm_listener_dispatch_entry_race_hook_for_tests();
    fd_b = socket(AF_INET, SOCK_STREAM, 0);
    ok = fd_b >= 0 &&
         test_connect(fd_b, (struct sockaddr *)&sa, sizeof(sa)) == 0;
    if (ok)
      chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests();
    else
      chttpsvr__release_listener_dispatch_race_hook_for_tests();
  }
  if (!ok) {
    atomic_store(&g_route_lifetime_handler_release, true);
    if (fd_a >= 0) close(fd_a);
    if (fd_b >= 0) close(fd_b);
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(ok);
  }

  chttpsvr__set_wait_in_flight_bounds_for_tests(50, 50);
  chttpsvr__set_drain_connections_wait_ms_for_tests(50);
  entry_race_destroy_arg_t darg = {.h = srv, .returned = false};
  pthread_t tid;
  bool created =
      pthread_create(&tid, NULL, _entry_race_destroy_thread, &darg) == 0;

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
      chttpsvr__route_teardown_with_live_connections_for_tests();

  chttpsvr__release_listener_dispatch_race_hook_for_tests();
  bool joined = created ? _bounded_join(tid, NULL) : true;
  if (created && !joined) pthread_detach(tid);
  bool finished = _wait_watched_finish_destroy(10000);
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  chttpsvr__set_drain_connections_wait_ms_for_tests(0);
  close(fd_a);
  close(fd_b);

  REQUIRE_TRUE(created);
  REQUIRE_TRUE(saw_shutdown);
  REQUIRE_TRUE(destroy_returned);
  REQUIRE_TRUE(joined);
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(violations_after, violations_before);
}

// glibc often gives a freed server's address to the next one, so a design keyed
// on addresses would mistake the new server for the destroyed one.
TEST(chttpsvr_handle_lifecycle,
     legitimate_slot_reuse_not_confused_with_stale_handle) {
  chttpsvr a = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(a, CHTTPSVR_NULL);
  chttpsvr stale_a = a;
  chttpsvr_destroy(a);

  chttpsvr b = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(b, CHTTPSVR_NULL);

  REQUIRE_EQ(chttpsvr_use(b, _noop_middleware_for_lifecycle_tests, NULL),
             ccol_success);

  REQUIRE_EQ((void *)chttpsvr__resolve_for_tests(stale_a), NULL);

  chttpsvr_destroy(b);
}

// Create and destroy in a loop must reuse one slot; the table must not grow.
TEST(chttpsvr_handle_lifecycle, bounded_slot_reuse_under_churn) {
  enum { ITERATIONS = 100 };

  chttpsvr s0 = chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_NE(s0, CHTTPSVR_NULL);
  chttpsvr_destroy(s0);
  size_t capacity_after_first = chttpsvr__slot_table_capacity_for_tests();

  for (int i = 1; i < ITERATIONS; i++) {
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    REQUIRE_NE(srv, CHTTPSVR_NULL);
    chttpsvr_destroy(srv);
  }

  REQUIRE_EQ(chttpsvr__slot_table_capacity_for_tests(), capacity_after_first);
}

TEST(doubling_growth_cap, first_growth_returns_initial) {
  REQUIRE_EQ(chttpsvr__doubling_growth_cap_for_tests(0, sizeof(void *), 8),
             (size_t)8);
}

TEST(doubling_growth_cap, ordinary_growth_doubles) {
  REQUIRE_EQ(chttpsvr__doubling_growth_cap_for_tests(8, sizeof(void *), 8),
             (size_t)16);
  REQUIRE_EQ(chttpsvr__doubling_growth_cap_for_tests(1024, sizeof(void *), 8),
             (size_t)2048);
}

TEST(doubling_growth_cap, cap_doubling_overflow_rejected) {
  REQUIRE_EQ(
      chttpsvr__doubling_growth_cap_for_tests(SIZE_MAX, sizeof(void *), 8),
      (size_t)0);
  REQUIRE_EQ(chttpsvr__doubling_growth_cap_for_tests(SIZE_MAX / 2 + 1,
                                                     sizeof(void *), 8),
             (size_t)0);
}

TEST(doubling_growth_cap, byte_size_multiplication_overflow_rejected) {
  size_t cap = SIZE_MAX / 2;
  REQUIRE_EQ(chttpsvr__doubling_growth_cap_for_tests(cap, 4096, 8), (size_t)0);
}

// The accept errno classification: resource exhaustion (EMFILE and friends)
// backs off with a rate-limited log, a per-connection error such as
// ECONNABORTED is retried at once. The tests call the classifier directly.
TEST(accept_errno_classification,
     econnaborted_and_pending_network_errors_are_transient) {
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ECONNABORTED));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EPROTO));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ENETDOWN));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ENETUNREACH));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ENOPROTOOPT));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EHOSTDOWN));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EHOSTUNREACH));
#ifdef ENONET
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ENONET));
#endif
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EOPNOTSUPP));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EPERM));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ETIMEDOUT));
#ifdef ENOSR
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ENOSR));
#endif
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(ESOCKTNOSUPPORT));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_transient_for_tests(EPROTONOSUPPORT));

  REQUIRE_FALSE(
      chttpsvr__accept_errno_is_resource_exhaustion_for_tests(ECONNABORTED));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_resource_exhaustion_for_tests(EPERM));
  REQUIRE_FALSE(
      chttpsvr__accept_errno_is_resource_exhaustion_for_tests(ETIMEDOUT));
}

TEST(accept_errno_classification,
     resource_exhaustion_errors_classified_correctly) {
  REQUIRE_TRUE(chttpsvr__accept_errno_is_resource_exhaustion_for_tests(EMFILE));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_resource_exhaustion_for_tests(ENFILE));
  REQUIRE_TRUE(
      chttpsvr__accept_errno_is_resource_exhaustion_for_tests(ENOBUFS));
  REQUIRE_TRUE(chttpsvr__accept_errno_is_resource_exhaustion_for_tests(ENOMEM));

  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(EMFILE));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(ENFILE));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(ENOBUFS));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(ENOMEM));
}

TEST(accept_errno_classification, unrelated_errnos_are_neither) {
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(EWOULDBLOCK));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(EINTR));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_transient_for_tests(EINVAL));
  REQUIRE_FALSE(
      chttpsvr__accept_errno_is_resource_exhaustion_for_tests(EWOULDBLOCK));
  REQUIRE_FALSE(chttpsvr__accept_errno_is_resource_exhaustion_for_tests(EINTR));
  REQUIRE_FALSE(
      chttpsvr__accept_errno_is_resource_exhaustion_for_tests(EINVAL));
}

// Opens a connection to port and sends nothing, for the idle and header timeout
// tests. Returns the fd, or -1.
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
  struct timeval rcv = {.tv_sec = 10, .tv_usec = 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &rcv, sizeof(rcv));
  return fd;
}

// Waits at most bound_ms for the peer to close fd and tells whether it did.
// read_timeout_ms_zero_leaves_an_idle_connection_alone uses it to see the sweep
// fire.
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

// Sends a GET for path on an open fd and returns the status, or -1, to check
// that an idle connection still works.
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

// read_timeout_us 0 turns the idle sweep off. A second server with a short
// timeout shows that the sweep has fired before the test checks that the
// connection of the first one still works.
TEST(chttpserver, read_timeout_ms_zero_leaves_an_idle_connection_alone) {
  chttpsvr disabled ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  chttpsvr calibrated ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(disabled != CHTTPSVR_NULL);
  REQUIRE_TRUE(calibrated != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(disabled, CHTTP_GET, "/idle",
                                            _hello_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(calibrated, CHTTP_GET, "/idle",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config off = CHTTPSVR_CONFIG_DEFAULT;
  off.host = "127.0.0.1";
  off.port = TEST_PORT + 90;
  off.read_timeout_us = 0;
  off.idle_timeout_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(disabled, &off), (int)ccol_success);

  chttpsvr_config on = CHTTPSVR_CONFIG_DEFAULT;
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
  REQUIRE_TRUE(sweep_fired);
  REQUIRE_EQ(served_when_disabled, 200);
}

TEST(chttpserver, max_connections_zero_means_unlimited) {
  enum { CONNS = 12 };
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/many",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 92;
  cfg.max_connections = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

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
      break;
    served++;
  }
  for (int i = 0; i < opened; i++) close(fds[i]);

  chttpsvr_stop(srv);

  REQUIRE_EQ(opened, (int)CONNS);
  REQUIRE_EQ(served, (int)CONNS);
}

// Sends one request with Connection: close to port on a fresh connection, reads
// the response into buf and returns the status, or -1 on a socket failure. It
// is _raw_request for the tests below that start their own server on their own
// port.
static int _raw_request_on_port(uint16_t port, const char *method,
                                const char *path, const char *extra_headers,
                                char *buf, size_t buf_sz) {
  int fd ccol__destructor(_close_scoped_fd) = _connect_silent(port);
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

// Answers 401 "auth-required" and never calls next, like an authenticator
// mounted on a prefix. subrouter_middleware_not_bypassed_by_broad_root_pattern
// mounts it to check that a broad root route can't get around it.
static void _prec_auth_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                          chttpsvr_next_fn next) {
  (void)ctx;
  (void)next;
  (void)req;
  chttpsvr_resp_set_status(resp, 401);
  chttpsvr_resp_write_str(resp, "auth-required");
}

// Writes "root-catch-all", for the broad root routes of the precedence tests.
static void _prec_root_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                               void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "root-catch-all");
}

// Writes "secret", for the routes behind a mounted guard in the precedence
// tests.
static void _prec_secret_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "secret");
}

// A broad root pattern overlaps the sub-router's mount, but the sub-router
// answers inside its prefix and its guard runs.
TEST(chttpserver, subrouter_middleware_not_bypassed_by_broad_root_pattern) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

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

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

  REQUIRE_EQ(mounted, 401);
  REQUIRE_TRUE(mounted_body_is_auth);
  REQUIRE_EQ(outside, 200);
  REQUIRE_TRUE(outside_body_is_root);
}

TEST(chttpserver, more_specific_mount_wins_over_less_specific_one) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

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

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  char buf[2048] = {0};
  int status = _raw_request("HEAD", "/hello", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "content-length:13") != NULL);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") == NULL);
}

TEST(chttpserver, explicit_head_route_wins_over_get_route) {
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

// What the middlewares of the rejection tests saw. They run on a reject-pool
// thread; _rej_global_mw fills g_rej_mw_path and g_rej_mw_method before it
// bumps g_rej_mw_runs, and a test reads them only after it has seen the bump.
static _Atomic int g_rej_mw_runs = 0;
static _Atomic int g_mount_mw_runs = 0;
static char g_rej_mw_path[256];
static chttp_method g_rej_mw_method;

// Records the path and the method in g_rej_mw_path and g_rej_mw_method, marks
// the response with x-rej-mw and bumps g_rej_mw_runs. It answers 429 itself for
// /rate-limited and calls next otherwise. It is the global middleware of the
// rejection tests, which check that the chain runs for a rejected request.
static void _rej_global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                           chttpsvr_next_fn next) {
  (void)ctx;
  const char *path = chttpsvr_req_path(req);
  snprintf(g_rej_mw_path, sizeof(g_rej_mw_path), "%s", path ? path : "(nil)");
  g_rej_mw_method = chttpsvr_req_method(req);
  chttpsvr_resp_set_header(resp, "x-rej-mw", "1");
  atomic_fetch_add(&g_rej_mw_runs, 1);
  if (path && strcmp(path, "/rate-limited") == 0) {
    chttpsvr_resp_set_status(resp, 429);
    chttpsvr_resp_write_str(resp, "slow down");
    return;
  }
  next(req, resp);
}

// Marks the response with x-mount-mw, bumps g_mount_mw_runs and calls next. It
// is the middleware of the /mounted sub-router of the rejection tests.
static void _rej_mount_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                          chttpsvr_next_fn next) {
  (void)ctx;
  chttpsvr_resp_set_header(resp, "x-mount-mw", "1");
  atomic_fetch_add(&g_mount_mw_runs, 1);
  next(req, resp);
}

// The port of _rej_srv_start's server.
#define REJ_PORT ((uint16_t)(TEST_PORT + 102))

// Starts the server of the rejection-middleware tests on REJ_PORT:
// _rej_global_mw, GET /ok, and a /mounted sub-router with _rej_mount_mw and
// POST /up, with a max_body_size of 64. Each test starts its own and destroys
// it, because a server kept for the run would add its locks to every later
// fork. Returns the server, or CHTTPSVR_NULL.
static chttpsvr _rej_srv_start(void) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (!srv) return CHTTPSVR_NULL;
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
    chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = REJ_PORT;
    cfg.max_body_size = 64;
    if (chttpsvr_start(srv, &cfg) != ccol_success) goto done;
  }
  ok = true;
done:
  if (!ok) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

TEST(chttpserver, global_middleware_runs_for_an_unmatched_route) {
  chttpsvr rej_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _rej_srv_start();
  REQUIRE_TRUE(rej_srv != CHTTPSVR_NULL);
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "GET", "/nope", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(after, before + 1);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") != NULL);
  REQUIRE_STREQ(g_rej_mw_path, "/nope");
  REQUIRE_EQ((int)g_rej_mw_method, (int)CHTTP_GET);
}

TEST(chttpserver, global_middleware_runs_for_a_method_rejection) {
  chttpsvr rej_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _rej_srv_start();
  REQUIRE_TRUE(rej_srv != CHTTPSVR_NULL);
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
  chttpsvr rej_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _rej_srv_start();
  REQUIRE_TRUE(rej_srv != CHTTPSVR_NULL);
  char buf[2048] = {0};
  int status = _raw_request_on_port(REJ_PORT, "GET", "/rate-limited", NULL, buf,
                                    sizeof(buf));
  REQUIRE_EQ(status, 429);
  REQUIRE_TRUE(strstr(buf, "slow down") != NULL);
}

TEST(chttpserver, payload_too_large_rejection_runs_the_matched_routers_chain) {
  chttpsvr rej_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _rej_srv_start();
  REQUIRE_TRUE(rej_srv != CHTTPSVR_NULL);
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
  chttpsvr rej_srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _rej_srv_start();
  REQUIRE_TRUE(rej_srv != CHTTPSVR_NULL);
  int before = atomic_load(&g_rej_mw_runs);
  char buf[2048] = {0};
  int status =
      _raw_request_on_port(REJ_PORT, "PROPFIND", "/ok", NULL, buf, sizeof(buf));
  int after = atomic_load(&g_rej_mw_runs);

  REQUIRE_EQ(status, 501);
  REQUIRE_EQ(after, before);
  REQUIRE_TRUE(strstr(buf, "x-rej-mw") == NULL);
}

// The port and the request target length of
// rejection_middleware_never_sees_an_undecodable_path.
#define REJ_OOM_PORT ((uint16_t)(TEST_PORT + 107))
#define REJ_OOM_PATH_LEN 251

// State of the allocator below. A rejected request has its path decoded for the
// middleware lazily, with an allocation of REJ_OOM_PATH_LEN + 1 bytes; the raw
// copy of the target, which comes first, has the same size. So once armed, the
// allocator refuses the second allocation of that size and nothing else, and
// the test checks that both sites were reached and one allocation was refused.
static _Atomic bool g_rej_oom_armed = false;
static _Atomic int g_rej_oom_target_seen = 0;
static _Atomic int g_rej_oom_refused = 0;

// The allocator of rejection_middleware_never_sees_an_undecodable_path: malloc
// refuses the second allocation of REJ_OOM_PATH_LEN + 1 bytes after
// g_rej_oom_armed is set and counts it in g_rej_oom_refused; every other call
// passes through to the C library.
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

// How often _rej_oom_mw ran, and whether it ever saw a NULL path.
static _Atomic int g_rej_oom_mw_runs = 0;
static _Atomic bool g_rej_oom_mw_saw_null_path = false;

// Counts its run and records a NULL path instead of using it, then calls next.
// A middleware written the documented way would read the path, so a rejection
// whose path couldn't be decoded must not run the chain at all, and the test
// checks that through these records instead of a crash.
static void _rej_oom_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                        chttpsvr_next_fn next) {
  (void)ctx;
  if (chttpsvr_req_path(req) == NULL)
    atomic_store(&g_rej_oom_mw_saw_null_path, true);
  atomic_fetch_add(&g_rej_oom_mw_runs, 1);
  next(req, resp);
}

TEST(chttpserver, rejection_middleware_never_sees_an_undecodable_path) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) = CHTTPSVR_NULL;
  {
    ccol_memmgmt_procs procs = {
        .malloc = _rej_oom_malloc,
        .free = _rej_oom_free,
        .calloc = _rej_oom_calloc,
        .realloc = _rej_oom_realloc,
    };
    srv = chttpsvr_create_mp(&procs, g_test_logger, NULL);
  }
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_use(srv, _rej_oom_mw, NULL), (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  atomic_store(&g_rej_oom_armed, false);

  bool saw_null_path = atomic_load(&g_rej_oom_mw_saw_null_path);
  int mw_runs = atomic_load(&g_rej_oom_mw_runs);
  int target_seen = atomic_load(&g_rej_oom_target_seen);
  int refused = atomic_load(&g_rej_oom_refused);

  chttpsvr_stop(srv);

  REQUIRE_FALSE(saw_null_path);
  REQUIRE_EQ(mw_runs, 0);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(target_seen, 2);
  REQUIRE_EQ(refused, 1);
}

// Sends one byte of a never-ending header block every interval_ms and tells
// whether the server closed the connection within bound_ms. Every byte is valid
// and resets the gap timers, so only the total duration limit can end it; the
// header_read_duration tests use it.
static bool _trickle_headers_until_closed(int fd, int interval_ms,
                                          int bound_ms) {
  static const char prefix[] = "GET /ok HTTP/1.1\r\nHost: 127.0.0.1\r\nX-Pad: ";
  const int plen = (int)(sizeof(prefix) - 1);
  for (int waited = 0, i = 0; waited < bound_ms; waited += interval_ms, i++) {
    char c = (i < plen) ? prefix[i] : 'x';
    if (send(fd, &c, 1, MSG_NOSIGNAL) != 1) return true;
    struct pollfd pfd = {.fd = fd, .events = POLLIN};
    if (poll(&pfd, 1, interval_ms) > 0) {
      char probe[1];
      if (recv(fd, probe, sizeof(probe), MSG_PEEK) <= 0) return true;
    }
  }
  return false;
}

// Sends a GET for path on fd and reads its whole response, body included, then
// returns the status, or -1.
// header_read_duration_budget_is_fresh_per_keepalive_request sends a second
// request on the same connection, so the first answer must be drained
// completely.
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

// Sends a complete GET for path one byte every interval_ms, then reads the
// status line and returns the status, or -1, for
// slow_but_completing_header_block_is_served.
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

// The header read duration limit of the header_read_duration tests.
#define HDR_DURATION_MS 1200

TEST(chttpserver, config_default_bounds_the_header_read_duration) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  REQUIRE_EQ(cfg.max_header_read_duration_us, (uint64_t)30000000);
}

TEST(chttpserver, trickled_header_block_is_closed_at_the_duration_ceiling) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
    closed = _trickle_headers_until_closed(fd, 100, 15000);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_TRUE(closed);
}

TEST(chttpserver, header_read_duration_zero_leaves_a_trickling_peer_alone) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
    closed = _trickle_headers_until_closed(fd, 100, 2 * HDR_DURATION_MS + 1200);
    close(fd);
  }

  chttpsvr_stop(srv);

  REQUIRE_TRUE(connected);
  REQUIRE_FALSE(closed);
}

TEST(chttpserver, slow_but_completing_header_block_is_served) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// The gap between two keep-alive requests is longer than the header read limit,
// so a budget counted from the connection would close it.
TEST(chttpserver, header_read_duration_budget_is_fresh_per_keepalive_request) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ok",
                                            _hello_handler, NULL),
             (int)ccol_success);

  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// A destroy clears in_use first and writes to the slot table last, so a slot in
// between must still count as live, or the exit-time release frees the table
// under the destroy. The window is set up directly.
TEST(chttpsvr_handle_lifecycle, slot_table_counts_a_mid_teardown_server_live) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  bool live_while_resolvable = chttpsvr__slot_live_for_tests(srv);
  chttpsvr__slot_set_in_use_for_tests(srv, false);
  bool live_mid_teardown = chttpsvr__slot_live_for_tests(srv);
  chttpsvr__slot_set_in_use_for_tests(srv, true);
  chttpsvr destroyed = srv;
  chttpsvr_destroy(srv);

  REQUIRE_TRUE(live_while_resolvable);
  REQUIRE_TRUE(live_mid_teardown);
  REQUIRE_FALSE(chttpsvr__slot_live_for_tests(destroyed));
}

// Answers 403 for every path under "/admin" and calls next otherwise, the way
// an application guards a prefix.
// request_target_must_be_origin_or_absolute_form checks that no request-target
// form gets around it.
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

// Echoes the decoded path, or "(null)", for the request-target form test.
static void _target_path_echo_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  (void)ctx;
  const char *path = chttpsvr_req_path(req);
  chttpsvr_resp_write_str(resp, path ? path : "(null)");
}

// The port of the request-target form test.
#define TARGET_FORM_PORT ((uint16_t)(TEST_PORT + 108))

TEST(chttpserver, request_target_must_be_origin_or_absolute_form) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_use(srv, _admin_guard_mw, NULL), (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/admin/secret",
                                            _prec_secret_handler, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/",
                                            _target_path_echo_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TARGET_FORM_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  struct {
    const char *method;
    const char *target;
    int status;
    const char *body;
  } cases[] = {
      {"GET", "/admin/secret", 403, "forbidden"},
      {"GET", "admin/secret", 400, NULL},
      {"GET", "admin", 400, NULL},
      {"GET", "?x=1", 400, NULL},
      {"OPTIONS", "*", 200, ""},
      {"GET", "*", 400, NULL},
      {"POST", "*", 400, NULL},
      {"GET", "http://127.0.0.1", 200, "/"},
      {"GET", "http://127.0.0.1?x=1", 200, "/"},
      {"GET", "http://127.0.0.1/", 200, "/"},
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

// An HTTP/1.0 request with Transfer-Encoding has faulty framing; the server
// must refuse it and close instead of answering the request behind it.
TEST(chttpserver, http_1_0_request_with_transfer_encoding_gets_400_and_close) {
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

// How often the middleware and the handler of the "OPTIONS *" test ran; the
// server must answer "OPTIONS *" itself and run neither.
static _Atomic int g_star_mw_calls = 0;
static _Atomic int g_star_handler_calls = 0;

// Counts its call in g_star_mw_calls and calls next, for the "OPTIONS *" test.
static void _star_counting_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                              chttpsvr_next_fn next) {
  (void)ctx;
  atomic_fetch_add(&g_star_mw_calls, 1);
  next(req, resp);
}

// Counts its call in g_star_handler_calls and writes "routed", for the "OPTIONS
// *" test.
static void _star_counting_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  atomic_fetch_add(&g_star_handler_calls, 1);
  chttpsvr_resp_write_str(resp, "routed");
}

// The port of the "OPTIONS *" test.
#define STAR_PORT ((uint16_t)(TEST_PORT + 109))

// The exact answer that the server gives to "OPTIONS *", without its Date line.
static const char star_ok[] =
    "HTTP/1.1 200 OK\r\ncontent-length:0\r\nconnection:keep-alive\r\n\r\n";

// Removes the first "date:" line of the first response in buf, in place, and
// returns buf, so options_star_is_answered_by_the_server can compare the rest
// byte for byte.
static char *_strip_date_line(char *buf) {
  char *end = strstr(buf, "\r\n\r\n");
  char *d = strstr(buf, "\r\ndate:");
  if (d && end && d < end) {
    char *eol = strstr(d + 2, "\r\n");
    memmove(d, eol, strlen(eol) + 1);
  }
  return buf;
}

// Writes wire on a fresh connection to STAR_PORT and reads into buf until the
// server closes or the 5 s receive timeout runs out, for the "OPTIONS *" test.
static void _star_exchange(const char *wire, size_t wire_len, char *buf,
                           size_t buf_sz) {
  buf[0] = '\0';
  int fd ccol__destructor(_close_scoped_fd) = _connect_silent(STAR_PORT);
  if (fd < 0) return;
  if (send(fd, wire, wire_len, MSG_NOSIGNAL) != (ssize_t)wire_len) return;
  _drain_socket_until_eof(fd, buf, buf_sz);
}

TEST(chttpserver, options_star_is_answered_by_the_server) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_use(srv, _star_counting_mw, NULL),
             (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_ANY, "/",
                                            _star_counting_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = STAR_PORT;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  static const char follow[] =
      "GET / HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";

  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b1[2048];
  static const char w1[] =
      "OPTIONS * HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n";
  _star_exchange(w1, sizeof(w1) - 1, b1, sizeof(b1));
  int mw1 = atomic_load(&g_star_mw_calls);
  int h1 = atomic_load(&g_star_handler_calls);

  atomic_store(&g_star_mw_calls, 0);
  atomic_store(&g_star_handler_calls, 0);
  char b2[4096];
  char w2[512];
  int n2 = snprintf(w2, sizeof(w2), "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n%s",
                    follow);
  _star_exchange(w2, (size_t)n2, b2, sizeof(b2));
  int mw2 = atomic_load(&g_star_mw_calls);
  int h2 = atomic_load(&g_star_handler_calls);

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

// Sets the Date header twice and writes "own-date", for
// a_date_that_the_handler_sets_wins.
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
  int fd ccol__destructor(_close_scoped_fd) = _connect_silent(TEST_PORT);
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
  if (sizeof(time_t) < 8) n--;
  char got[8][64];
  bool ok[8];
  for (size_t i = 0; i < n; i++) {
    chttpsvr__set_date_override_for_tests(steps[i].sec);
    char buf[2048];
    _raw_request_to_test_server(
        "GET /hello HTTP/1.1\r\nHost: h\r\nConnection: close\r\n\r\n", buf,
        sizeof(buf));
    ok[i] = _first_date_value(buf, got[i], sizeof(got[i]));
  }
  chttpsvr__set_date_override_for_tests(INT64_MIN);
  for (size_t i = 0; i < n; i++) {
    REQUIRE_TRUE(ok[i]);
    REQUIRE_STREQ(got[i], steps[i].text);
  }
}

TEST(chttpserver, unknown_method_with_asterisk_target_gets_400) {
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

// Sets status 103 and writes nothing, for
// a_1xx_status_from_a_handler_goes_out_as_500_with_a_date.
static void _status_103_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 103);
}

TEST(chttpserver, a_1xx_status_from_a_handler_goes_out_as_500_with_a_date) {
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

// The slow-client tests: the parking of slow bodies and slow readers, the rate
// floor, the body memory limit and the streaming pool. Every wait on the server
// is a bounded poll of white-box state, every time limit is crossed with the
// injected clock and a sweep tick run on the test thread, and each test ends by
// checking that no body memory stays reserved. _SC_PORT is their first port,
// and _SC_BIG_RESPONSE the size of /big, far more than the socket buffers of a
// client that doesn't read can hold.
#define _SC_PORT (TEST_PORT + 150)
#define _SC_BIG_RESPONSE ((size_t)8 * 1024 * 1024)

// Returns CLOCK_MONOTONIC in milliseconds, for the deadlines of the slow-client
// helpers.
static long long _sc_now_ms(void) {
  struct timespec t;
  clock_gettime(CLOCK_MONOTONIC, &t);
  return (long long)t.tv_sec * 1000LL + t.tv_nsec / 1000000L;
}

// Sleeps ms milliseconds, between the polls of the slow-client helpers.
static void _sc_nap_ms(long ms) {
  struct timespec nap = {ms / 1000, (ms % 1000) * 1000000L};
  nanosleep(&nap, NULL);
}

// Connects to 127.0.0.1:port and returns the fd, or -1. It is the client socket
// of the slow-client tests: a 5 s receive timeout makes a regression fail a
// read instead of hanging, and TCP_NODELAY sends every write at once, because a
// kernel that delays acknowledgements on loopback (FreeBSD) would hold a second
// small write back.
static int _sc_connect(int port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
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

// Writes all n bytes of s to fd and tells whether it managed.
static bool _sc_send(int fd, const char *s, size_t n) {
  while (n) {
    ssize_t w = write(fd, s, n);
    if (w <= 0) return false;
    s += w;
    n -= (size_t)w;
  }
  return true;
}

// Writes the string s to fd with _sc_send.
static bool _sc_send_str(int fd, const char *s) {
  return _sc_send(fd, s, strlen(s));
}

// Reads one response from fd into buf and returns its status, or -1 on a
// timeout or an end of stream before the response is complete. The slow-client
// tests read every answer with it. buf gets the head and as much of the body as
// fits, NUL-terminated; the rest is read and thrown away, and *body_len_out
// gets the whole body length. timeout_ms bounds the whole read, and head_only
// skips the body (for a HEAD request).
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
      want = 1;
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

// Reads one response with a body, see _sc_read_response_x.
static int _sc_read_response(int fd, char *buf, size_t cap,
                             size_t *body_len_out, int timeout_ms) {
  return _sc_read_response_x(fd, buf, cap, body_len_out, timeout_ms, false);
}

// Reads fd until the end of the stream, for at most timeout_ms, and returns the
// number of bytes, or -1 if the stream didn't end in time. A reset counts as an
// end. Tests use it to see that the server closed a connection.
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

typedef size_t (*_sc_probe_fn)(chttpsvr h, void *arg);
// Waits, for at most timeout_ms, until probe(h, arg) reaches at least want, and
// tells whether it did. The slow-client tests wait for a server state with it;
// the bound only guards against a hang.
static bool _sc_wait_for(chttpsvr h, _sc_probe_fn probe, void *arg, size_t want,
                         int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  while (probe(h, arg) < want) {
    if (_sc_now_ms() > deadline) return false;
    _sc_nap_ms(1);
  }
  return true;
}
// Waits, for at most timeout_ms, until probe(h, arg) is 0, and tells whether it
// did.
static bool _sc_wait_for_zero(chttpsvr h, _sc_probe_fn probe, void *arg,
                              int timeout_ms) {
  long long deadline = _sc_now_ms() + timeout_ms;
  while (probe(h, arg) != 0) {
    if (_sc_now_ms() > deadline) return false;
    _sc_nap_ms(1);
  }
  return true;
}
// Probe: the number of connections of h parked on a body. Each of these small
// probes returns one white-box count of h for _sc_wait_for; arg is unused.
static size_t _sc_parked_bodies(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__parked_count_for_tests(h, 1);
}
// Probe: the number of connections of h parked on a write.
static size_t _sc_parked_writes(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__parked_count_for_tests(h, 2);
}
// Probe: the number of requests of h waiting for body memory.
static size_t _sc_mem_waiting(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__mem_waiting_for_tests(h);
}
// Probe: the body memory that h has reserved.
static size_t _sc_mem_in_use(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__mem_in_use_for_tests(h);
}
// Probe: the number of requests in h's streaming queue.
static size_t _sc_stream_queue(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__stream_queue_len_for_tests(h);
}
// Probe: the number of active threads of h's streaming pool.
static size_t _sc_stream_active(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__pool_active_for_tests(h, true);
}

// Answers with the length of the body it got. Most slow-body tests post to it
// and check that the whole body arrived.
static void _sc_len_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t n = 0;
  chttpsvr_req_body(req, &n);
  chttpsvr_resp_printf(resp, "%zu", n);
}

// Answers with _SC_BIG_RESPONSE bytes, for the slow-reader tests.
static void _sc_big_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  char chunk[65536];
  memset(chunk, 'b', sizeof(chunk));
  for (size_t i = 0; i < _SC_BIG_RESPONSE / sizeof(chunk); i++)
    chttpsvr_resp_write(resp, chunk, sizeof(chunk));
}

// Writes "hello", the short known answer of the slow-client and listener
// servers.
static void _sc_hello_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

// Answers with 100 bytes of known text, for the tests that check a body byte
// for byte.
static void _sc_text_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  (void)req;
  (void)ctx;
  char text[101];
  for (int i = 0; i < 100; i++) text[i] = (char)('a' + i % 26);
  text[100] = '\0';
  chttpsvr_resp_write_str(resp, text);
}

// Answers 204 with a body that must never reach the wire.
static void _sc_204_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 204);
  chttpsvr_resp_write_str(resp, "never on the wire");
}

// The gate that _sc_gate_stream_handler waits on. g_sc_gate_runs counts the
// handler runs that reached it.
static pthread_mutex_t g_sc_gate_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_sc_gate_cv = PTHREAD_COND_INITIALIZER;
static bool g_sc_gate_open = false;
static _Atomic int g_sc_gate_runs = 0;
// Opens or closes the gate under g_sc_gate_mtx and wakes the handlers waiting
// on it.
static void _sc_gate_open(bool open) {
  pthread_mutex_lock(&g_sc_gate_mtx);
  g_sc_gate_open = open;
  pthread_cond_broadcast(&g_sc_gate_cv);
  pthread_mutex_unlock(&g_sc_gate_mtx);
}
// Counts its run in g_sc_gate_runs, then waits (at most 10 s) until the gate is
// open and writes "gated". The streaming pool tests use it to hold a pool
// thread for as long as they need.
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

// The same gate for a buffered route, which holds a worker thread.
static void _sc_gate_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  _sc_gate_stream_handler(req, resp, ctx);
}

// Starts a fresh server on port with extra's settings and the routes of the
// slow-client tests (/len, /big, /hello, /text, /no-content, /gate, /busy,
// /stream-err). Returns the server, or CHTTPSVR_NULL.
static chttpsvr _sc_server(int port, const chttpsvr_config *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
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
  chttpsvr_config cfg = *extra;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

// Sends the head of a POST to path that declares cl body bytes (with extra_hdr
// added), plus the first part bytes of that body, and nothing else. The tests
// use it to start a body that parks. Returns the fd, or -1.
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

// Writes n bytes of c to fd, to send the rest of a body. Tells whether it
// managed.
static bool _sc_send_fill(int fd, size_t n, char c) {
  char *b = malloc(n ? n : 1);
  memset(b, c, n ? n : 1);
  bool ok = _sc_send(fd, b, n);
  free(b);
  return ok;
}

// The declared length of a body-memory holder. A body this short reserves its
// whole buffer before its first byte and never grows it.
#define _SC_HOLD_LEN ((size_t)16384)

// Opens n POSTs to /len that each declare _SC_HOLD_LEN bytes and send 10 of
// them, then waits until every one holds its reservation. The memory limit
// tests use it to fill the budget. fds gets every socket that opened (-1 for
// the others). Returns true when all n hold.
static bool _sc_hold_memory(chttpsvr srv, int port, int *fds, int n) {
  bool ok = true;
  for (int i = 0; i < n; i++) {
    fds[i] = _sc_open_partial_post(port, "/len", _SC_HOLD_LEN, 10, NULL);
    if (fds[i] < 0) ok = false;
  }
  return ok && _sc_wait_for(srv, _sc_mem_in_use, NULL, (size_t)n * _SC_HOLD_LEN,
                            5000);
}

// Sends the rest of the body of every holder that _sc_hold_memory opened and
// reads its response, to free the budget again. Returns how many got 200.
static int _sc_release_holders(int *fds, int n) {
  int served = 0;
  char buf[4096];
  for (int i = 0; i < n; i++)
    if (fds[i] >= 0 && _sc_send_fill(fds[i], _SC_HOLD_LEN - 10, 'h') &&
        _sc_read_response(fds[i], buf, sizeof(buf), NULL, 5000) == 200)
      served++;
  return served;
}

// Closes every fd of fds that opened.
static void _sc_close_all(int *fds, int n) {
  for (int i = 0; i < n; i++)
    if (fds[i] >= 0) close(fds[i]);
}

// Six bodies stop halfway on a server with two workers; each parks without a
// thread, so an ordinary GET is still served.
TEST(slow_clients, slow_bodies_park_and_hold_no_worker_thread) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  int fds[6];
  bool opened = true;
  for (int i = 0; i < 6; i++) {
    fds[i] = _sc_open_partial_post(_SC_PORT, "/len", 1000, 10, NULL);
    if (fds[i] < 0) opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, 6, 5000);
  size_t active = chttpsvr__pool_active_for_tests(srv, false);

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

// Connects to port with a 4096-byte receive buffer and returns the fd, or -1.
// The slow-reader tests use it so a client that doesn't read leaves most of a
// large response in the server.
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

// One client of slow_readers_park_and_hold_no_worker_thread and its verdict.
typedef struct {
  int fd;
  bool whole;
} _sc_big_reader_t;

// Reads the large response on r->fd, then asks for /hello on the same
// connection, and sets r->whole when both answers are complete. It is the
// thread body of slow_readers_park_and_hold_no_worker_thread.
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 1, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  int fds[4];
  bool opened = true;
  for (int i = 0; i < 4; i++) {
    fds[i] = _sc_connect_small_rcvbuf(_SC_PORT + 1);
    if (fds[i] < 0 ||
        !_sc_send_str(fds[i], "GET /big HTTP/1.1\r\nHost: h\r\n\r\n"))
      opened = false;
  }
  bool parked = opened && _sc_wait_for(srv, _sc_parked_writes, NULL, 4, 10000);
  size_t active = chttpsvr__pool_active_for_tests(srv, false);

  char buf[4096];
  int hello_status = -1;
  int fd = _sc_connect(_SC_PORT + 1);
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello_status = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  if (fd >= 0) close(fd);

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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 2, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 2, "/len", 2000, 1000, NULL);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  chttpsvr__advance_slow_clock_for_tests(5000);
  chttpsvr__sweep_now_for_tests(srv);
  size_t still_parked = chttpsvr__parked_count_for_tests(srv, 1);
  char buf[4096];
  int status = -1;
  if (_sc_send_fill(fd, 1000, 'r'))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(still_parked, (size_t)1);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n2000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

// Probe: the number of active threads of h's worker pool.
static size_t _sc_workers_active(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__pool_active_for_tests(h, false);
}
// Probe: the process-wide count of parked connections handed to a worker.
static size_t _sc_resume_dispatches(chttpsvr h, void *arg) {
  (void)h;
  (void)arg;
  return chttpsvr__resume_dispatch_count_for_tests();
}

// A resumed body waits 60 s on the clock in the busy worker's queue; its limits
// are judged at the moment it became ready, so it completes.
TEST(slow_clients, queue_wait_of_a_resumed_body_is_never_charged) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 1;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 30, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  _sc_gate_open(false);

  int fd = _sc_open_partial_post(_SC_PORT + 30, "/len", 1000, 500, NULL);
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);

  int busy = _sc_connect(_SC_PORT + 30);
  bool busy_sent =
      busy >= 0 && _sc_send_str(busy, "GET /busy HTTP/1.1\r\nHost: h\r\n\r\n");
  bool worker_held =
      busy_sent && _sc_wait_for(srv, _sc_workers_active, NULL, 1, 5000);

  size_t dispatches = chttpsvr__resume_dispatch_count_for_tests();
  bool rest_sent = fd >= 0 && _sc_send_fill(fd, 500, 'q');
  bool claimed = rest_sent && _sc_wait_for(srv, _sc_resume_dispatches, NULL,
                                           dispatches + 1, 5000);
  if (claimed) chttpsvr__advance_slow_clock_for_tests(60000);
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 3, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 3, "/len", 2000, 1000, NULL);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));
  chttpsvr__advance_slow_clock_for_tests(11000);
  chttpsvr__sweep_now_for_tests(srv);
  char buf[4096];
  int status = _sc_read_response(fd, buf, sizeof(buf), NULL, 3000);
  long rest = _sc_drain_to_eof(fd, 3000);
  REQUIRE_EQ(status, 408);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
  REQUIRE_TRUE(rest >= 0);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, rate_floor_closes_a_slow_reader) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 1024 * 1024;
  cfg.min_transfer_rate_grace_us = 1000000;
  cfg.response_write_timeout_us = 600000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 4, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
      _sc_connect_small_rcvbuf(_SC_PORT + 4);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_TRUE(_sc_send_str(fd, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n"));
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_writes, NULL, 1, 10000));
  chttpsvr__advance_slow_clock_for_tests(60000);
  chttpsvr__sweep_now_for_tests(srv);
  size_t parked_after = chttpsvr__parked_count_for_tests(srv, 2);
  long got = _sc_drain_to_eof(fd, 10000);
  REQUIRE_EQ(parked_after, (size_t)0);
  REQUIRE_TRUE(got >= 0);
  REQUIRE_LT((size_t)got, _SC_BIG_RESPONSE);
}

TEST(slow_clients, rate_floor_ends_a_slow_streaming_read) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 1000;
  cfg.min_transfer_rate_grace_us = 300000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 5, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
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

// 60 s of waiting for memory are the server's time, so the rate floor doesn't
// count them and the body completes.
TEST(slow_clients, time_waiting_for_memory_is_not_charged_to_the_client) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.min_transfer_rate_bps = 10000;
  cfg.min_transfer_rate_grace_us = 1000000;
  cfg.max_partial_body_memory = 128 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 6, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  int holder ccol__destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 6, "/len", 30000, 10, NULL);
  REQUIRE_TRUE(holder >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));

  int chunked ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 6);
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

  chttpsvr__advance_slow_clock_for_tests(60000);
  chttpsvr__sweep_now_for_tests(srv);
  char buf[4096];
  int holder_status = _sc_read_response(holder, buf, sizeof(buf), NULL, 3000);
  int status = _sc_read_response(chunked, buf, sizeof(buf), NULL, 5000);
  REQUIRE_EQ(holder_status, 408);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n100000") != NULL);
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

// A chunked body of exactly max_body_size never reserves past the limit, so it
// fits beside another holder without waiting.
TEST(slow_clients, a_chunked_body_near_the_limit_needs_no_more_than_the_limit) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 256 * 1024;
  cfg.max_partial_body_memory = 700 * 1024;
  cfg.body_memory_wait_timeout_us = 2000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 32, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

  int holder ccol__destructor(_close_scoped_fd) =
      _sc_open_partial_post(_SC_PORT + 32, "/len", 256 * 1024, 1000, NULL);
  REQUIRE_TRUE(holder >= 0);
  REQUIRE_TRUE(_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000));

  int chunked ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 32);
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

// The same for a body that waits in the queue after a memory wait.
TEST(slow_clients, queue_wait_after_a_memory_wait_is_never_charged) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 1;
  cfg.min_transfer_rate_bps = 10000;
  cfg.min_transfer_rate_grace_us = 1000000;
  cfg.max_partial_body_memory = 128 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 31, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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

  bool admitted = false;
  if (holder_parked && mem_wait && worker_held) {
    chttpsvr__advance_slow_clock_for_tests(60000);
    chttpsvr__sweep_now_for_tests(srv);
    admitted = _sc_wait_for_zero(srv, _sc_mem_waiting, NULL, 5000);
    if (admitted) chttpsvr__advance_slow_clock_for_tests(60000);
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

// A body that can't reserve its first buffer waits with no reader and no
// interim line, and completes once the others free their memory.
TEST(slow_clients, a_body_that_does_not_fit_waits_unread_then_completes) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 7, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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

// Writes every buffer to its socket, round robin and without blocking, until
// all are out or timeout_ms passes, and tells whether all went out.
// chunked_bodies_that_fill_the_limit_still_progress uses it so a server that
// reads one socket only after another completes can't stall the writer.
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

// Builds a chunked POST to /len with one chunk of body bytes and returns it
// (the caller frees it), with its length in *len_out.
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

// Two chunked bodies larger than the limit both end up waiting for memory; the
// oldest waiter may then go past the limit, so both complete.
TEST(slow_clients, chunked_bodies_that_fill_the_limit_still_progress) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 64 * 1024;
  cfg.body_memory_wait_timeout_us = CHTTPSVR_NO_DEADLINE;
  cfg.max_body_size = 1024 * 1024;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 8, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__mem_peak_reset_for_tests();
  int a ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 8);
  int b ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 8);
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
  size_t peak = chttpsvr__mem_peak_for_tests();
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(sa, 200);
  REQUIRE_TRUE(a_len);
  REQUIRE_EQ(sb, 200);
  REQUIRE_TRUE(b_len);
  REQUIRE_LE(peak, (size_t)(64 * 1024 + 1024 * 1024));
  REQUIRE_TRUE(_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000));
}

TEST(slow_clients, a_memory_wait_past_its_deadline_gets_503) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 9, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 9, holders, 6);
  int b =
      held ? _sc_open_partial_post(_SC_PORT + 9, "/len", 50000, 0, NULL) : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  int status = -1;
  long rest = -1;
  char buf[4096];
  if (b_waits) {
    chttpsvr__advance_slow_clock_for_tests(31000);
    chttpsvr__sweep_now_for_tests(srv);
    status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
    rest = _sc_drain_to_eof(b, 3000);
  }
  size_t waiting = chttpsvr__mem_waiting_for_tests(srv);
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 10, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 10, holders, 6);
  int b ccol__destructor(_close_scoped_fd) =
      held ? _sc_open_partial_post(_SC_PORT + 10, "/len", 50000, 0, NULL) : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  char buf[4096];
  int status = -1;
  if (b_waits) {
    chttpsvr__advance_slow_clock_for_tests(31000);
    chttpsvr__sweep_now_for_tests(srv);
    status = _sc_read_response(b, buf, sizeof(buf), NULL, 3000);
  }
  _sc_close_all(holders, 6);
  REQUIRE_TRUE(held);
  REQUIRE_TRUE(b_waits);
  REQUIRE_EQ(status, 503);
  REQUIRE_TRUE(strstr(buf, "retry-after:5\r\n") != NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

// A small body that would fit must not overtake a large waiter ahead of it.
TEST(slow_clients, memory_waiters_are_admitted_in_order) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 11, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int holders[6];
  bool held = _sc_hold_memory(srv, _SC_PORT + 11, holders, 6);
  int b ccol__destructor(_close_scoped_fd) =
      held ? _sc_open_partial_post(_SC_PORT + 11, "/len", 50000, 0,
                                   "Expect: 100-continue\r\n")
           : -1;
  bool b_waits = b >= 0 && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  int c ccol__destructor(_close_scoped_fd) =
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

// Opens a connection and sends an empty POST to /gate, which holds a streaming
// thread. Returns the fd, or -1.
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
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 2;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 12, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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

// Requests arriving one at a time on an idle pool of eight threads each start
// one task, never one per thread.
TEST(slow_clients, one_streaming_request_starts_one_task) {
  _sc_gate_open(true);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 8;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 33, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t before = chttpsvr__stream_tokens_started_for_tests();
  char buf[4096];
  int ok = 0;
  for (int i = 0; i < 10; i++) {
    int fd = _sc_post_gate(_SC_PORT + 33);
    if (fd < 0) continue;
    if (_sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200) ok++;
    close(fd);
    _sc_wait_for_zero(srv, _sc_stream_active, NULL, 5000);
  }
  size_t started = chttpsvr__stream_tokens_started_for_tests() - before;
  _sc_gate_open(false);
  REQUIRE_EQ(ok, 10);
  REQUIRE_LE(started, (size_t)10);
}

// With one task held before it takes its request, a second request starts only
// one more task, because the held task will take one of the two.
TEST(slow_clients, a_request_behind_a_task_on_its_way_starts_one_task) {
  _sc_gate_open(true);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 8;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 34, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__arm_stream_claim_hook_for_tests(1);
  size_t before = chttpsvr__stream_tokens_started_for_tests();
  int r1 = _sc_post_gate(_SC_PORT + 34);
  bool held = chttpsvr__wait_stream_claim_hook_entered_for_tests(5000);
  int r2 = _sc_post_gate(_SC_PORT + 34);
  char buf[4096];
  int st2 = r2 >= 0 ? _sc_read_response(r2, buf, sizeof(buf), NULL, 5000) : -1;
  size_t started = chttpsvr__stream_tokens_started_for_tests() - before;
  chttpsvr__release_stream_claim_hook_for_tests();
  int st1 = r1 >= 0 ? _sc_read_response(r1, buf, sizeof(buf), NULL, 5000) : -1;
  if (r1 >= 0) close(r1);
  if (r2 >= 0) close(r2);
  chttpsvr__arm_stream_claim_hook_for_tests(0);
  _sc_gate_open(false);
  REQUIRE_TRUE(held);
  REQUIRE_EQ(st1, 200);
  REQUIRE_EQ(st2, 200);
  REQUIRE_EQ(started, (size_t)2);
}

TEST(slow_clients, a_full_streaming_queue_answers_503_at_once) {
  _sc_gate_open(false);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_capacity = 1;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 13, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 14, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int r1 = _sc_post_gate(_SC_PORT + 14);
  bool running = _sc_wait_for(srv, _sc_stream_active, NULL, 1, 5000);
  int r2 = _sc_post_gate(_SC_PORT + 14);
  bool queued = _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  chttpsvr__advance_slow_clock_for_tests(6000);
  chttpsvr__sweep_now_for_tests(srv);
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

// Runs one round of the claim race of
// the_sweep_and_a_streaming_task_answer_a_request_once: the streaming task
// stops just before (where = 1) or just after (where = 2) it takes the queued
// request, that request's deadline passes and the sweep runs. Exactly one of
// the two must answer, with one 503, and the handler must never run for it. The
// status, the end of stream and the handler runs come back through the out
// parameters, and *setup_ok tells whether every step of the setup worked.
static void _sc_claim_race_round(int port, int where, int *status_out,
                                 long *eof_out, int *runs_out, bool *setup_ok) {
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(port, &cfg);
  *setup_ok = false;
  *status_out = -1;
  *eof_out = -1;
  if (srv == CHTTPSVR_NULL) return;
  int r1 = _sc_post_gate(port);
  bool running = _sc_wait_for(srv, _sc_stream_active, NULL, 1, 5000);
  int r2 = _sc_post_gate(port);
  bool queued = _sc_wait_for(srv, _sc_stream_queue, NULL, 1, 5000);
  chttpsvr__arm_stream_claim_hook_for_tests(where);
  _sc_gate_open(true);
  bool held = chttpsvr__wait_stream_claim_hook_entered_for_tests(5000);
  chttpsvr__advance_slow_clock_for_tests(6000);
  chttpsvr__sweep_now_for_tests(srv);
  chttpsvr__release_stream_claim_hook_for_tests();
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
  chttpsvr__arm_stream_claim_hook_for_tests(0);
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
  _sc_gate_open(true);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 17, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  bool before = chttpsvr__stream_pool_created_for_tests(srv);
  char buf[4096];
  int fd = _sc_connect(_SC_PORT + 17);
  int hello = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  bool after_buffered = chttpsvr__stream_pool_created_for_tests(srv);
  int gate = -1;
  if (fd >= 0 && _sc_send_str(fd,
                              "POST /gate HTTP/1.1\r\nHost: h\r\n"
                              "Content-Length: 0\r\n\r\n"))
    gate = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  bool after_streaming = chttpsvr__stream_pool_created_for_tests(srv);
  if (fd >= 0) close(fd);
  _sc_gate_open(false);
  REQUIRE_FALSE(before);
  REQUIRE_EQ(hello, 200);
  REQUIRE_FALSE(after_buffered);
  REQUIRE_EQ(gate, 200);
  REQUIRE_TRUE(after_streaming);
}

// Drips n body bytes one at a time with a short pause and returns how many
// times the reactor handed the parked body to a worker meanwhile, or -1.
// the_receive_low_water_mark_wakes_a_drip_once compares it with
// _sc_upload_dispatches. The response status goes to *status_out.
static long _sc_drip_dispatches(chttpsvr srv, int port, size_t n,
                                int *status_out) {
  *status_out = -1;
  int fd = _sc_open_partial_post(port, "/len", n, 0, NULL);
  if (fd < 0) return -1;
  if (!_sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000)) {
    close(fd);
    return -1;
  }
  size_t before = chttpsvr__resume_dispatch_count_for_tests();
  for (size_t i = 0; i < n; i++) {
    if (!_sc_send(fd, "d", 1)) break;
    _sc_nap_ms(2);
  }
  char buf[4096];
  *status_out = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  size_t after = chttpsvr__resume_dispatch_count_for_tests();
  close(fd);
  return (long)(after - before);
}

// Uploads n bytes in one fast write and returns the number of dispatches, or
// -1; the response status goes to *status_out.
static long _sc_upload_dispatches(chttpsvr srv, int port, size_t n,
                                  int *status_out) {
  (void)srv;
  *status_out = -1;
  size_t before = chttpsvr__resume_dispatch_count_for_tests();
  int fd = _sc_open_partial_post(port, "/len", n, n, NULL);
  if (fd < 0) return -1;
  char buf[4096];
  *status_out = _sc_read_response(fd, buf, sizeof(buf), NULL, 10000);
  size_t after = chttpsvr__resume_dispatch_count_for_tests();
  close(fd);
  return (long)(after - before);
}

// With SO_RCVLOWAT a body arriving one byte at a time wakes the reactor once,
// when the whole rest is queued, instead of once per byte.
TEST(slow_clients, the_receive_low_water_mark_wakes_a_drip_once) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 8 * 1024 * 1024;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 18, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int s_with, s_without, u_with, u_without;
  chttpsvr__set_rcvlowat_enabled_for_tests(true);
  long drip_with = _sc_drip_dispatches(srv, _SC_PORT + 18, 200, &s_with);
  long up_with =
      _sc_upload_dispatches(srv, _SC_PORT + 18, 4 * 1024 * 1024, &u_with);
  chttpsvr__set_rcvlowat_enabled_for_tests(false);
  long drip_without = _sc_drip_dispatches(srv, _SC_PORT + 18, 200, &s_without);
  long up_without =
      _sc_upload_dispatches(srv, _SC_PORT + 18, 4 * 1024 * 1024, &u_without);
  chttpsvr__set_rcvlowat_enabled_for_tests(true);
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 19, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 20, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) =
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 21, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) = _sc_open_partial_post(
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 22, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 22);
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 1000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 23, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) = _sc_connect(_SC_PORT + 23);
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

// The server that _sc_destroy_thread destroys and whether it returned.
typedef struct {
  chttpsvr srv;
  _Atomic bool done;
} _sc_destroy_ctx_t;

// Destroys ctx->srv and sets ctx->done, so
// destroy_closes_or_answers_every_waiting_connection can wait for the destroy
// with a bound.
static void *_sc_destroy_thread(void *arg) {
  _sc_destroy_ctx_t *ctx = (_sc_destroy_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  atomic_store(&ctx->done, true);
  return NULL;
}

// A destroy ends a parked body, a memory waiter, a parked response, a blocked
// streaming handler and a queued streaming request (503) in bounded time.
TEST(slow_clients, destroy_closes_or_answers_every_waiting_connection) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(300, 2000);
  _sc_gate_open(false);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  cfg.max_partial_body_memory = 20000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 0;
  cfg.max_body_read_duration_us = 0;
  _sc_destroy_ctx_t *ctx = calloc(1, sizeof(*ctx));
  ctx->srv = _sc_server(_SC_PORT + 24, &cfg);
  bool started = ctx->srv != CHTTPSVR_NULL;
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
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
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

// Sends req on fd and reads one response into buf (only the head when
// head_only). Returns its whole length (head and body), or 0 on a failure. The
// tests that compare two exchanges byte for byte use it.
static size_t _sc_exchange(int fd, const char *req, char *buf, size_t cap,
                           bool head_only) {
  size_t body = 0;
  if (!_sc_send_str(fd, req)) return 0;
  if (_sc_read_response_x(fd, buf, cap, &body, 5000, head_only) < 0) return 0;
  const char *end = strstr(buf, "\r\n\r\n");
  return end ? (size_t)(end + 4 - buf) + body : 0;
}

// A write that stops inside the head or the body resumes from the exact byte on
// another worker, so the client gets the same bytes as from one write.
TEST(slow_clients, a_write_cut_inside_the_head_or_the_body_resumes_intact) {
  chttpsvr__set_date_override_for_tests(1700000000);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
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
    size_t parks = chttpsvr__park_count_for_tests();
    chttpsvr__force_short_response_write_for_tests(cuts[i]);
    size_t n = _sc_exchange(fd, get, got, sizeof(got), false);
    if (n == ref_len && memcmp(got, ref, n) == 0 &&
        chttpsvr__park_count_for_tests() > parks)
      intact++;
  }
  chttpsvr__force_short_response_write_for_tests(7);
  size_t hn =
      fd >= 0 ? _sc_exchange(fd, head, got_head, sizeof(got_head), true) : 0;
  bool head_intact =
      hn == ref_head_len && hn > 0 && memcmp(got_head, ref_head, hn) == 0;
  if (fd >= 0) close(fd);
  chttpsvr__set_date_override_for_tests(INT64_MIN);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_GT(ref_len, (size_t)100);
  REQUIRE_EQ(intact, 3);
  REQUIRE_TRUE(head_intact);
}

TEST(slow_clients,
     the_sweep_walks_only_parked_connections_when_idle_limits_are_off) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.read_timeout_us = 0;
  cfg.idle_timeout_us = 0;
  cfg.max_header_read_duration_us = 0;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 26, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  size_t visited = ok ? chttpsvr__sweep_now_for_tests(srv) : 0;
  for (int i = 0; i < 20; i++)
    if (idle[i] >= 0) close(idle[i]);
  for (int i = 0; i < 3; i++)
    if (parked[i] >= 0) close(parked[i]);
  REQUIRE_TRUE(ok);
  REQUIRE_EQ(visited, (size_t)3);
}

// The port of the mount ownership tests. A mounted prefix owns every path under
// it: _own_server mounts "/admin" behind a guard, mounts "/admin/deep" on its
// own, and has root routes broad enough to catch any path of one to three
// segments. A request for a path under "/admin", in any spelling that decodes
// to one, must meet the guard, and a root route must never answer it.
#define _OWN_PORT (_SC_PORT + 27)

// How often a root route and the guard of the ownership tests ran.
static _Atomic int g_own_root_runs = 0;
static _Atomic int g_own_guard_runs = 0;

// Writes ctx, the name of the route, and counts the runs of the root routes in
// g_own_root_runs, so the ownership tests can tell which route answered.
static void _own_text_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  if (strncmp((const char *)ctx, "root", 4) == 0)
    atomic_fetch_add(&g_own_root_runs, 1);
  chttpsvr_resp_write_str(resp, (const char *)ctx);
}

// Counts its run in g_own_guard_runs and lets a request through only with
// "x-key: ok"; it answers 403 "GUARD" otherwise. It guards the "/admin" mount.
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

// Starts the server of the ownership tests on _OWN_PORT, see _OWN_PORT. Returns
// it, or CHTTPSVR_NULL.
static chttpsvr _own_server(void) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}", _own_text_handler, "root1");
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}/{b}", _own_text_handler,
                            "root2");
  chttpsvr_register_handler(srv, CHTTP_ANY, "/{a}/{b}/{c}", _own_text_handler,
                            "root3");
  chttpsvr_router *adm = chttpsvr_subrouter(srv, "/admin");
  chttpsvr_router *deep = chttpsvr_subrouter(srv, "/admin/deep");
  if (!adm || !deep) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  chttpsvr_router_use(adm, _own_guard_mw, NULL);
  chttpsvr_router_on(adm, CHTTP_GET, "/secret", _own_text_handler,
                     "admin-secret");
  chttpsvr_router_on(adm, CHTTP_POST, "/post-only", _own_text_handler,
                     "admin-post");
  chttpsvr_router_on(adm, CHTTP_ANY, "/{a}/{b}", _own_text_handler,
                     "admin-two");
  chttpsvr_router_on(deep, CHTTP_GET, "/x", _own_text_handler, "deep-x");
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = _OWN_PORT;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

// Sends one request for target (with the header lines in extra) to _OWN_PORT on
// a fresh connection and returns the status, or -1. The response, body
// included, lands in buf.
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

// Spellings such as "/admin%2Fsecret" decode under /admin and must meet the
// guard instead of falling through to a root route.
TEST(route_ownership, every_spelling_under_a_mount_meets_its_guard) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) = _own_server();
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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

// Reads one response of want_body body bytes from fd at about rate bytes a
// second, and returns how many body bytes arrived before the end of the stream
// or timeout_ms, or -1 when the head didn't arrive. It plays the steady slow
// reader of a_steady_reader_of_a_parked_response_is_never_cut.
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

// The socket becomes writable only after a third of a large autotuned send
// buffer drains, so a steady reader must be judged by its shrinking send queue,
// not by wake-ups.
TEST(slow_clients, a_steady_reader_of_a_parked_response_is_never_cut) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.response_write_timeout_us = 200000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 28, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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

// The rate floor makes the sweep ask the kernel about each parked response; it
// must do that after releasing the lock that every dispatch takes.
TEST(slow_clients, the_sweep_queries_the_kernel_outside_its_lock) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.response_write_timeout_us = 30000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 35, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t total0 = 0, locked0 = 0;
  chttpsvr__parked_queries_for_tests(&total0, &locked0);
  int fd = _sc_connect_small_rcvbuf(_SC_PORT + 35);
  REQUIRE_TRUE(fd >= 0);
  bool sent = _sc_send_str(fd, "GET /big HTTP/1.1\r\nHost: h\r\n\r\n");
  bool parked = sent && _sc_wait_for(srv, _sc_parked_writes, NULL, 1, 10000);
  size_t at_park = 0, locked_at_park = 0;
  chttpsvr__parked_queries_for_tests(&at_park, &locked_at_park);
  size_t total1 = at_park, locked1 = locked_at_park;
  long long until = _sc_now_ms() + 20000;
  while (parked && total1 - at_park < 2 && _sc_now_ms() <= until) {
    _sc_nap_ms(10);
    chttpsvr__parked_queries_for_tests(&total1, &locked1);
  }
  close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(parked);
  REQUIRE_GE(total1 - at_park, (size_t)2);
  REQUIRE_EQ(locked1 - locked0, (size_t)0);
}

TEST(slow_clients, keep_alive_requests_make_no_direction_change) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_SC_PORT + 36, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd = _sc_connect(_SC_PORT + 36);
  REQUIRE_TRUE(fd >= 0);
  char buf[2048];
  bool first = _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") &&
               _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200;
  size_t before = chttpsvr__conn_reg_modify_calls_for_tests();
  int ok = 0;
  for (int i = 0; first && i < 20; i++) {
    _sc_nap_ms(2);
    if (_sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n") &&
        _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) == 200)
      ok++;
  }
  size_t calls = chttpsvr__conn_reg_modify_calls_for_tests() - before;
  close(fd);
  REQUIRE_TRUE(first);
  REQUIRE_EQ(ok, 20);
  REQUIRE_EQ(calls, (size_t)0);
}

// Sends a whole request to g_srv on a fresh connection and returns the status
// of the first response, or -1, for the protocol conformance tests. The
// response bytes land in buf.
static int _conf_request(const char *req, char *buf, size_t cap) {
  _raw_request_to_test_server(req, buf, cap);
  if (strncmp(buf, "HTTP/1.1 ", 9) != 0) return -1;
  return atoi(buf + 9);
}

TEST(conformance, a_major_version_other_than_1_gets_505) {
  static const char *const reqs[] = {
      "GET /hello HTTP/2.0\r\nHost: h\r\n\r\n",
      "GET /hello HTTP/3.0\r\nHost: h\r\n\r\n",
      "GET /hello HTTP/0.9\r\nHost: h\r\n\r\n",
  };
  for (size_t i = 0; i < sizeof(reqs) / sizeof(reqs[0]); i++) {
    char buf[4096];
    REQUIRE_EQ(_conf_request(reqs[i], buf, sizeof(buf)),
               CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED);
    REQUIRE_TRUE(
        strncmp(buf, "HTTP/1.1 505 HTTP Version Not Supported\r\n", 41) == 0);
  }
}

TEST(conformance, the_505_status_constant_is_505) {
  REQUIRE_EQ(CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED, 505);
}

TEST(conformance, a_higher_minor_version_of_http_1_is_served_as_1_1) {
  char buf[4096];
  REQUIRE_EQ(_conf_request("GET /hello HTTP/1.9\r\nHost: h\r\nConnection: "
                           "close\r\n\r\n",
                           buf, sizeof(buf)),
             200);
  REQUIRE_TRUE(strstr(buf, "Hello, world!") != NULL);
}

TEST(conformance, a_host_outside_the_uri_host_grammar_gets_400) {
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

// Builds a chunked POST to /echo-body of n one-byte chunks, each with the chunk
// extension ext, for chunk_extensions_are_bounded_by_the_data_they_carry. The
// caller frees the result; NULL when it can't be allocated.
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

#include <internal/chttp1_parser.h>

// What _spill_on_header saw: the longest value and the number of headers.
typedef struct {
  size_t longest_value;
  size_t headers;
} _spill_ctx_t;

// Header callback of the response line spill tests: counts the header and keeps
// the longest value length in the _spill_ctx_t of the parser.
static int _spill_on_header(chttp1_parser *p, const char *name, size_t name_len,
                            const char *value, size_t value_len) {
  (void)name;
  (void)name_len;
  (void)value;
  _spill_ctx_t *c = (_spill_ctx_t *)p->data;
  c->headers++;
  if (value_len > c->longest_value) c->longest_value = value_len;
  return 0;
}

// Allocations and frees of the spill allocator below.
static _Atomic int g_spill_allocs = 0;
static _Atomic int g_spill_frees = 0;
// The counting allocator of the spill tests: each call is counted in
// g_spill_allocs or g_spill_frees and passed to the C library.
static void *_spill_malloc(size_t n) {
  atomic_fetch_add(&g_spill_allocs, 1);
  return malloc(n);
}
static void _spill_free(void *p) {
  if (p) atomic_fetch_add(&g_spill_frees, 1);
  free(p);
}

// Builds a response whose single header line carries value_len bytes of value,
// for the spill tests. The caller frees it; NULL when it can't be allocated.
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

// Feeds msg to the parser p in pieces of step bytes, so a line crosses many
// reads, and returns the first error, or CHTTP1_OK.
static chttp1_errno _spill_feed(chttp1_parser *p, const char *msg,
                                size_t step) {
  size_t len = strlen(msg);
  chttp1_errno r = CHTTP1_OK;
  for (size_t off = 0; off < len && r == CHTTP1_OK; off += step) {
    size_t n = len - off < step ? len - off : step;
    r = chttp1_parser_execute(p, msg + off, n);
  }
  return r;
}

TEST(chttp1_parser, a_response_header_line_spills_to_the_heap_up_to_64_kib) {
  chttp1_settings st;
  chttp1_settings_init(&st);
  st.on_header = _spill_on_header;
  char *msg = _spill_response(40000);
  REQUIRE_TRUE(msg != NULL);

  chttp1_parser plain;
  chttp1_parser_init(&plain, &st);
  _spill_ctx_t c0 = {0, 0};
  plain.data = &c0;
  chttp1_errno r_plain = _spill_feed(&plain, msg, 1000);
  chttp1_parser_release(&plain);

  ccol_memmgmt_procs procs = {0};
  procs.malloc = _spill_malloc;
  procs.free = _spill_free;
  int a0 = atomic_load(&g_spill_allocs);
  int f0 = atomic_load(&g_spill_frees);
  chttp1_parser sp;
  chttp1_parser_init(&sp, &st);
  bool enabled = chttp1_parser_enable_line_spill(&sp, &procs);
  _spill_ctx_t c1 = {0, 0};
  sp.data = &c1;
  chttp1_errno r_spill = _spill_feed(&sp, msg, 1000);
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
  chttp1_settings st;
  chttp1_settings_init(&st);
  char *msg = _spill_response(70000);
  REQUIRE_TRUE(msg != NULL);
  chttp1_parser sp;
  chttp1_parser_init(&sp, &st);
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno r_long = _spill_feed(&sp, msg, 4096);
  chttp1_parser_release(&sp);
  free(msg);

  msg = _spill_response(20000);
  REQUIRE_TRUE(msg != NULL);
  chttp1_parser_init(&sp, &st);
  sp.max_total_header_bytes_override = 12000;
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno r_budget = _spill_feed(&sp, msg, 4096);
  size_t budget_line = sp.line_len;
  chttp1_parser_release(&sp);
  chttp1_parser_init(&sp, &st);
  sp.max_total_header_bytes_override = 8000;
  REQUIRE_TRUE(chttp1_parser_enable_line_spill(&sp, NULL));
  chttp1_errno r_small = _spill_feed(&sp, msg, 4096);
  bool spilled = sp.line_spill != NULL;
  chttp1_parser_release(&sp);
  free(msg);

  chttp1_parser rq;
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
  chttp1_settings st;
  chttp1_settings_init(&st);
  const char *head = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
  char ext[128];
  memset(ext, 'v', sizeof(ext) - 1);
  ext[0] = ';';
  ext[1] = 'e';
  ext[2] = '=';
  ext[sizeof(ext) - 1] = '\0';
  chttp1_parser p;
  chttp1_parser_init(&p, &st);
  chttp1_errno r = chttp1_parser_execute(&p, head, strlen(head));
  for (int i = 0; i < 400 && r == CHTTP1_OK; i++) {
    char chunk[256];
    int n = snprintf(chunk, sizeof(chunk), "1%s\r\nx\r\n", ext);
    r = chttp1_parser_execute(&p, chunk, (size_t)n);
  }
  REQUIRE_EQ((int)r, (int)CHTTP1_ERROR);
  REQUIRE_STREQ(p.reason, "Chunk framing exceeds its data");
}

// The first port of the listener, lingering close, accept batch and body memory
// tests.
#define _LC_PORT (TEST_PORT + 200)

// Waits, for at most 5 s, until the process-wide lingering-close counter goes
// above before, and tells whether it did. The counter moves on the closing
// thread, which can run just after the client has seen the end of the stream,
// so the lingering close tests can't read it at once.
static bool _linger_count_passes(size_t before) {
  for (int i = 0; i < 5000; i++) {
    if (chttpsvr__linger_count_for_tests() > before) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

// Starts a server with GET and POST /lc on host and port, with base's settings
// (the defaults when NULL), for the listener and lingering close tests. Returns
// it, or CHTTPSVR_NULL.
static chttpsvr _lc_server(const char *host, int port,
                           const chttpsvr_config *base) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/lc", _sc_len_handler, NULL);
  chttpsvr_config cfg = base ? *base : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

// Sends GET /lc over a fresh connection to sa and returns the status, or -1
// when the connection or the exchange fails. The listener tests reach each
// address family through it.
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

// Sends GET /lc to 127.0.0.1:port and returns the status, or -1.
static int _lc_get_v4(int port) {
  struct sockaddr_in a;
  memset(&a, 0, sizeof(a));
  a.sin_family = AF_INET;
  a.sin_port = htons((uint16_t)port);
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

// Sends GET /lc to [::1]:port and returns the status, or -1.
static int _lc_get_v6(int port) {
  struct sockaddr_in6 a;
  memset(&a, 0, sizeof(a));
  a.sin6_family = AF_INET6;
  a.sin6_port = htons((uint16_t)port);
  inet_pton(AF_INET6, "::1", &a.sin6_addr);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

// Tells whether this host can bind the IPv6 loopback at all, so the IPv6
// listener tests can skip where it can't.
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

// Tells whether an IPv6 socket can take IPv4 clients here. OpenBSD has no such
// socket, and there the default listener is 0.0.0.0, as in Go, so the
// dual-stack tests skip.
static bool _lc_have_dual_stack(void) {
  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return false;
  int off = 0;
  bool ok = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0;
  close(fd);
  return ok;
}

TEST(listener_host, a_null_or_empty_host_listens_on_ipv4_and_ipv6) {
  bool v6 = _lc_have_ipv6_loopback() && _lc_have_dual_stack();
  if (!v6) fprintf(stderr, "SKIP: no dual-stack IPv6; ::1 not checked\n");
  const char *hosts[2] = {NULL, ""};
  int v4_status[2] = {-1, -1}, v6_status[2] = {-1, -1};
  for (int i = 0; i < 2; i++) {
    chttpsvr srv = _lc_server(hosts[i], _LC_PORT + i, NULL);
    if (srv == CHTTPSVR_NULL) continue;
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
  bool v6 = _lc_have_ipv6_loopback() && _lc_have_dual_stack();
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  bool host_is_null = cfg.host == NULL;
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started = false;
  if (srv != CHTTPSVR_NULL) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    cfg.port = (uint16_t)(_LC_PORT + 14);
    started = chttpsvr_start(srv, &cfg) == ccol_success;
  }
  int v4 = started ? _lc_get_v4(_LC_PORT + 14) : -2;
  int v6_status = !v6 ? 200 : (started ? _lc_get_v6(_LC_PORT + 14) : -2);
  if (!v6) fprintf(stderr, "SKIP: no dual-stack IPv6; ::1 not checked\n");
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(host_is_null);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6_status, 200);
}

// A host name binds its first IPv4 address, so "localhost" answers on 127.0.0.1
// even where the resolver gives ::1 first.
TEST(listener_host, localhost_listens_on_the_ipv4_loopback) {
  chttpsvr srv = _lc_server("localhost", _LC_PORT + 2, NULL);
  int status = srv != CHTTPSVR_NULL ? _lc_get_v4(_LC_PORT + 2) : -2;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_EQ(status, 200);
}

TEST(listener_host, a_literal_address_binds_exactly_that_address) {
  bool v6 = _lc_have_ipv6_loopback();
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 3, NULL);
  int v4 = srv != CHTTPSVR_NULL ? _lc_get_v4(_LC_PORT + 3) : -2;
  int v6_status = (srv != CHTTPSVR_NULL && v6) ? _lc_get_v6(_LC_PORT + 3) : -1;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  chttpsvr srv6 = v6 ? _lc_server("::1", _LC_PORT + 4, NULL) : CHTTPSVR_NULL;
  int v6_on_v6 = srv6 != CHTTPSVR_NULL ? _lc_get_v6(_LC_PORT + 4) : 200;
  int v4_on_v6 = srv6 != CHTTPSVR_NULL ? _lc_get_v4(_LC_PORT + 4) : -1;
  if (srv6 != CHTTPSVR_NULL) chttpsvr_destroy(srv6);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6_status, -1);
  REQUIRE_EQ(v6_on_v6, 200);
  REQUIRE_EQ(v4_on_v6, -1);
}

// Writes "unix://<path>" into host, the host string of a unix:// listener.
static void _lc_unix_host(char *host, size_t cap, const char *path) {
  snprintf(host, cap, "unix://%s", path);
}

// Sends GET /lc over the Unix socket at path and returns the status, or -1.
static int _lc_get_unix(const char *path) {
  struct sockaddr_un a;
  memset(&a, 0, sizeof(a));
  a.sun_family = AF_UNIX;
  strncpy(a.sun_path, path, sizeof(a.sun_path) - 1);
  return _lc_get_via((struct sockaddr *)&a, sizeof(a));
}

TEST(listener_unix, a_regular_file_at_the_path_survives_a_failed_start) {
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  ccol_retval rv = srv != CHTTPSVR_NULL ? chttpsvr_start(srv, &cfg)
                                        : ccol_unexpected_failure;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
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
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_live_%d", (int)getpid());
  unlink(path);
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr first = _lc_server(host, 0, NULL);
  bool first_started = first != CHTTPSVR_NULL;
  chttpsvr second = chttpsvr_create(g_test_logger, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host;
  ccol_retval rv = (first != CHTTPSVR_NULL && second != CHTTPSVR_NULL)
                       ? chttpsvr_start(second, &cfg)
                       : ccol_unexpected_failure;
  if (second != CHTTPSVR_NULL) chttpsvr_destroy(second);
  int status = first != CHTTPSVR_NULL ? _lc_get_unix(path) : -2;
  if (first != CHTTPSVR_NULL) chttpsvr_destroy(first);
  struct stat st;
  bool gone = lstat(path, &st) != 0;
  unlink(path);
  REQUIRE_TRUE(first_started);
  REQUIRE_NE((int)rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(gone);
}

TEST(listener_unix, a_stop_leaves_a_path_that_names_another_file_alone) {
  char path[64], host[96];
  snprintf(path, sizeof(path), "/tmp/chttpsvr_lc_repl_%d", (int)getpid());
  unlink(path);
  _lc_unix_host(host, sizeof(host), path);
  chttpsvr srv = _lc_server(host, 0, NULL);
  bool started = srv != CHTTPSVR_NULL;
  bool replaced = false;
  if (srv != CHTTPSVR_NULL && unlink(path) == 0) {
    FILE *f = fopen(path, "w");
    if (f) {
      fputs("not yours\n", f);
      fclose(f);
      replaced = true;
    }
  }
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  struct stat st;
  bool survived = lstat(path, &st) == 0 && S_ISREG(st.st_mode);
  unlink(path);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(replaced);
  REQUIRE_TRUE(survived);
}

// A unix:// listener on a relative path removes its socket file at stop even
// after a chdir, through the directory it opened at start.
TEST(listener_unix, a_relative_path_is_cleaned_up_after_a_chdir) {
  char dir_a[64], dir_b[64], sock_a[128], sock_b[128], cwd[512];
  snprintf(dir_a, sizeof(dir_a), "/tmp/chttpsvr_lc_a_%d", (int)getpid());
  snprintf(dir_b, sizeof(dir_b), "/tmp/chttpsvr_lc_b_%d", (int)getpid());
  snprintf(sock_a, sizeof(sock_a), "%s/rel.sock", dir_a);
  snprintf(sock_b, sizeof(sock_b), "%s/rel.sock", dir_b);
  bool have_cwd = getcwd(cwd, sizeof(cwd)) != NULL;
  mkdir(dir_a, 0700);
  mkdir(dir_b, 0700);
  bool in_a = have_cwd && chdir(dir_a) == 0;
  chttpsvr srv = in_a ? _lc_server("unix://rel.sock", 0, NULL) : CHTTPSVR_NULL;
  bool started = srv != CHTTPSVR_NULL;
  bool in_b = chdir(dir_b) == 0;
  bool made_b = in_b && _make_stale_unix_socket("rel.sock");
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
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

// Probe: the number of connections of h in a lingering close.
static size_t _lc_lingering(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__parked_count_for_tests(h, 6);
}

// Probe: the requests of h that a thread is serving or rejecting right now.
static size_t _lc_in_flight(chttpsvr h, void *arg) {
  (void)arg;
  int n = chttpsvr__in_flight_requests_for_tests(h);
  return n > 0 ? (size_t)n : 0;
}

// Sends the head of a POST to path with a body of declared bytes, plus
// first_bytes of that body, in one write. The lingering close tests use it to
// start an upload that the server refuses. Tells whether the write worked.
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

// Reads fd until the end of the stream for up to timeout_ms and reports how it
// ended: 0 for an orderly end, the errno of a failed read, or -1 for a timeout.
// The lingering close tests tell an orderly close from a reset with it.
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

// A refused upload leaves body bytes unread; the client must still read the 404
// and an orderly end, not ECONNRESET.
TEST(lingering_close, a_refused_upload_gets_its_response_and_an_orderly_end) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 5, NULL);
  size_t lingers = chttpsvr__linger_count_for_tests();
  int fd = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 5) : -1;
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope", 1048576, 32768);
  char buf[2048] = {0};
  size_t body = 0;
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), &body, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  bool drained =
      srv != CHTTPSVR_NULL && _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
  REQUIRE_TRUE(drained);
}

TEST(lingering_close, a_linger_ends_at_its_deadline) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 6, NULL);
  int quiet = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 6) : -1;
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

  int loud = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 6) : -1;
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
  bool l_closed =
      srv != CHTTPSVR_NULL && _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_EQ(q_status, 404);
  REQUIRE_TRUE(q_parked);
  REQUIRE_TRUE(q_closed);
  REQUIRE_LT(q_ms, 4500LL);
  REQUIRE_EQ(l_status, 404);
  REQUIRE_TRUE(write_failed);
  REQUIRE_LT(l_ms, 8000LL);
  REQUIRE_TRUE(l_closed);
}

TEST(lingering_close, a_request_with_no_body_closes_at_once) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 7, NULL);
  _wait_for_reject_counter_to_settle();
  size_t lingers = chttpsvr__linger_count_for_tests();
  int fd = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 7) : -1;
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /nope HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool settled =
      srv != CHTTPSVR_NULL && _sc_wait_for_zero(srv, _lc_in_flight, NULL, 5000);
  bool lingered = chttpsvr__linger_count_for_tests() > lingers;
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(settled);
  REQUIRE_FALSE(lingered);
}

TEST(lingering_close, a_handler_that_leaves_the_body_unread_lingers_too) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started =
      srv != CHTTPSVR_NULL &&
      chttpsvr_register_streaming_handler(
          srv, CHTTP_POST, "/lc-refuse", _stream_reject_without_reading_handler,
          NULL) == ccol_success;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = _LC_PORT + 13;
  started = started && chttpsvr_start(srv, &cfg) == ccol_success;
  size_t lingers = chttpsvr__linger_count_for_tests();
  int fd = started ? _sc_connect(_LC_PORT + 13) : -1;
  bool sent =
      fd >= 0 && _lc_send_upload_start(fd, "/lc-refuse", 1048576, 32768);
  char buf[2048] = {0};
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(status, 401);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
}

// 120 connections wait while the reactor is held; each listener dispatch
// accepts at most 64, and every connection is served. 120 fits the BSD and
// macOS backlog limit of 128.
TEST(listener_accept, one_dispatch_accepts_a_bounded_batch) {
  enum { N = 120 };
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 8, NULL);
  int fds[N];
  for (int i = 0; i < N; i++) fds[i] = -1;
  bool armed = false;
  int opened = 0, served = 0;
  if (srv != CHTTPSVR_NULL) {
    chttpsvr__arm_listener_dispatch_entry_race_hook_for_tests();
    armed = true;
    fds[0] = _sc_connect(_LC_PORT + 8);
    if (fds[0] >= 0) {
      chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests();
      opened = 1;
      for (int i = 1; i < N; i++) {
        fds[i] = _sc_connect(_LC_PORT + 8);
        if (fds[i] < 0) break;
        opened++;
      }
    }
    chttpsvr__listener_max_accepts_per_dispatch_reset_for_tests();
    chttpsvr__release_listener_dispatch_race_hook_for_tests();
    for (int i = 0; i < opened; i++) {
      char buf[1024];
      if (_sc_send_str(fds[i], "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n") &&
          _sc_read_response(fds[i], buf, sizeof(buf), NULL, 10000) == 200)
        served++;
    }
  }
  size_t max_batch = chttpsvr__listener_max_accepts_per_dispatch_for_tests();
  for (int i = 0; i < N; i++)
    if (fds[i] >= 0) close(fds[i]);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(armed);
  REQUIRE_EQ(opened, N);
  REQUIRE_EQ(served, N);
  REQUIRE_GT(max_batch, (size_t)1);
  REQUIRE_LE(max_batch, (size_t)64);
}

// The port, the stop flag and the answer count of _lc_star_thread.
typedef struct {
  int port;
  _Atomic bool stop;
  _Atomic int answered;
} _lc_star_arg_t;

// Sends "OPTIONS *" on one connection to a->port in a loop until a->stop is set
// or an answer isn't 200, counting the answers in a->answered.
// options_star_is_safe_beside_a_subrouter_registration runs it while it
// registers sub-routers.
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

// "OPTIONS *" must read the router array under its lock while sub-routers are
// added; ThreadSanitizer checks it in the test_tsan build.
TEST(chttpserver, options_star_is_safe_beside_a_subrouter_registration) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 9, NULL);
  _lc_star_arg_t arg = {.port = _LC_PORT + 9, .stop = false, .answered = 0};
  pthread_t tid;
  bool started = srv != CHTTPSVR_NULL &&
                 pthread_create(&tid, NULL, _lc_star_thread, &arg) == 0;
  int made = 0;
  for (int i = 0; started && i < 300; i++) {
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "/lc-sub-%d", i);
    if (chttpsvr_subrouter(srv, prefix)) made++;
    if (i % 50 == 0) _sc_nap_ms(1);
  }
  long long until = _sc_now_ms() + 5000;
  while (started && atomic_load(&arg.answered) < 5 && _sc_now_ms() < until)
    _sc_nap_ms(1);
  atomic_store(&arg.stop, true);
  bool joined = started ? _bounded_join(tid, NULL) : false;
  if (started && !joined) pthread_detach(tid);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(joined);
  REQUIRE_EQ(made, 300);
  REQUIRE_GE(atomic_load(&arg.answered), 5);
}

TEST(slow_clients, a_huge_declared_length_never_wraps_the_memory_count) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_body_size = 0;
  cfg.max_partial_body_memory = 65536;
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 10, &cfg);
  chttpsvr__mem_peak_reset_for_tests();
  int fd = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 10) : -1;
  bool sent = fd >= 0 &&
              _sc_send_str(fd,
                           "POST /lc HTTP/1.1\r\nHost: h\r\n"
                           "Content-Length: 18446744073709551000\r\n\r\nabc");
  bool charged = sent && _sc_wait_for(srv, _sc_mem_in_use, NULL, 1, 5000);
  size_t in_use =
      srv != CHTTPSVR_NULL ? chttpsvr__mem_in_use_for_tests(srv) : 0;
  size_t peak = chttpsvr__mem_peak_for_tests();
  if (fd >= 0) close(fd);
  bool released = srv != CHTTPSVR_NULL &&
                  _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  int after = -1;
  int fd2 = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 10) : -1;
  if (fd2 >= 0 && _sc_send_str(fd2,
                               "POST /lc HTTP/1.1\r\nHost: h\r\n"
                               "Content-Length: 3\r\n\r\nxyz")) {
    char buf[1024];
    after = _sc_read_response(fd2, buf, sizeof(buf), NULL, 5000);
  }
  if (fd2 >= 0) close(fd2);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(charged);
  REQUIRE_LE(in_use, (size_t)65536);
  REQUIRE_LE(peak, (size_t)65536);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(after, 200);
}

// State of the allocator below: once armed, it fails the g_lc_fail_nth-th
// allocation of exactly g_lc_fail_size bytes and passes every other call
// through. g_lc_fail_seen counts the allocations of that size and g_lc_failed
// the refusals.
static _Atomic size_t g_lc_fail_size = 0;
static _Atomic int g_lc_fail_nth = 0;
static _Atomic int g_lc_fail_seen = 0;
static _Atomic int g_lc_failed = 0;
// Tells whether an allocation of n bytes is the one to refuse, counting it.
static bool _lc_should_fail(size_t n) {
  if (n != atomic_load(&g_lc_fail_size)) return false;
  if (atomic_fetch_add(&g_lc_fail_seen, 1) + 1 != atomic_load(&g_lc_fail_nth))
    return false;
  atomic_fetch_add(&g_lc_failed, 1);
  return true;
}
// The fail-nth-of-size allocator of the lost-copy tests: each allocating call
// returns NULL when _lc_should_fail says so and passes through otherwise.
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
// Arms the allocator to fail the nth allocation of size bytes and clears its
// counters.
static void _lc_arm_failure(size_t size, int nth) {
  atomic_store(&g_lc_fail_seen, 0);
  atomic_store(&g_lc_failed, 0);
  atomic_store(&g_lc_fail_nth, nth);
  atomic_store(&g_lc_fail_size, size);
}

// Starts a server on port with the failing allocator and sends two pipelined
// GETs in one write, the second one exactly n2 bytes long. The third allocation
// of that size (after the divert's carry-over and the stream's copy) is the
// reclaim of the leftover, and it fails. With park_write the first response is
// also parked by a forced short write. *first and *second get the two statuses
// (-1 when no second came) and *end how the stream ended after the first.
// a_lost_copy_of_pipelined_bytes_closes_the_connection checks that the
// connection closes instead of losing a request.
static void _lc_pipeline_with_lost_leftover(int port, bool park_write,
                                            int *first, int *second, int *end) {
  ccol_memmgmt_procs mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  *first = *second = *end = -2;
  if (srv == CHTTPSVR_NULL) return;
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
    if (park_write) chttpsvr__force_short_response_write_for_tests(10);
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
        *end = -1;
      }
    }
    close(fd);
  }
  chttpsvr__force_short_response_write_for_tests(0);
  atomic_store(&g_lc_fail_size, 0);
  chttpsvr_destroy(srv);
}

// When reclaiming the pipelined bytes fails for lack of memory, the connection
// must close after the first response instead of waiting for a request that is
// already gone. The second pass reclaims them on the parked-write path.
TEST(chttpserver, a_lost_copy_of_pipelined_bytes_closes_the_connection) {
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

// Probe: the number of idle connections of h.
static size_t _rx_idle(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__idle_count_for_tests(h);
}

// The connections of one_tick_closes_every_expired_idle_and_parked_connection:
// idle ones, parked ones, and how many it opens at once.
#define _RX_IDLE_N 300
#define _RX_PARKED_N 3
#define _RX_BATCH 16

// Opens a POST to /len that declares 1000 bytes and sends its head and the
// first 10 body bytes in one send(2), so the server parks the body with nothing
// else on the way; later bytes would wake the connection during the sweep tick.
// Returns the fd, or -1.
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

// 300 idle connections and three parked bodies are all past their limits, and
// one sweep tick must close every idle one and answer every parked one with
// 408. A fixed batch per tick would never reach the parked list in time.
TEST(sweep, one_tick_closes_every_expired_idle_and_parked_connection) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.idle_timeout_us = 30000000;
  cfg.read_timeout_us = 30000000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  chttpsvr srv = _sc_server(_LC_PORT + 15, &cfg);
  int idle_fds[_RX_IDLE_N];
  int parked_fds[_RX_PARKED_N];
  for (int i = 0; i < _RX_IDLE_N; i++) idle_fds[i] = -1;
  for (int i = 0; i < _RX_PARKED_N; i++) parked_fds[i] = -1;
  bool opened = srv != CHTTPSVR_NULL;
  for (int i = 0; opened && i < _RX_PARKED_N; i++) {
    parked_fds[i] = _rx_open_parked_post(_LC_PORT + 15);
    opened = parked_fds[i] >= 0;
  }
  opened =
      opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, _RX_PARKED_N, 10000);
  for (int i = 0; opened && i < _RX_IDLE_N; i++) {
    idle_fds[i] = _sc_connect(_LC_PORT + 15);
    opened = idle_fds[i] >= 0;
    if (opened && ((i + 1) % _RX_BATCH == 0 || i + 1 == _RX_IDLE_N))
      opened = _sc_wait_for(srv, _rx_idle, NULL, (size_t)(i + 1), 20000);
  }
  size_t idle_after = SIZE_MAX, parked_after = SIZE_MAX;
  if (opened) {
    chttpsvr__advance_slow_clock_for_tests(60000);
    chttpsvr__sweep_ahead_for_tests(srv, 60000);
    idle_after = chttpsvr__idle_count_for_tests(srv);
    parked_after = chttpsvr__parked_count_for_tests(srv, 1);
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
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(opened);
  REQUIRE_EQ(idle_after, (size_t)0);
  REQUIRE_EQ(parked_after, (size_t)0);
  REQUIRE_EQ(timeouts, _RX_PARKED_N);
}

// Probe: the number of active threads of h's worker pool.
static size_t _rx_workers_active(chttpsvr h, void *arg) {
  (void)arg;
  return chttpsvr__pool_active_for_tests(h, false);
}

// The server that _rx_destroy_thread destroys and whether it returned.
typedef struct {
  chttpsvr srv;
  _Atomic bool done;
} _rx_destroy_ctx_t;

// Destroys ctx->srv and sets ctx->done, so the restart test can wait for the
// destroy with a bound.
static void *_rx_destroy_thread(void *arg) {
  _rx_destroy_ctx_t *ctx = (_rx_destroy_ctx_t *)arg;
  chttpsvr_destroy(ctx->srv);
  atomic_store(&ctx->done, true);
  return NULL;
}

// A restart returns at once while a handler of the previous run holds its
// worker; that request completes and its pool drains in the background.
TEST(restart, neither_waits_for_nor_cuts_a_request_of_the_previous_run) {
  _sc_gate_open(false);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  _rx_destroy_ctx_t *ctx = calloc(1, sizeof(*ctx));
  ctx->srv = _sc_server(_LC_PORT + 16, &cfg);
  chttpsvr srv = ctx->srv;
  bool started = srv != CHTTPSVR_NULL;
  size_t drained_before = chttpsvr__retired_drained_for_tests();
  int a = started ? _sc_connect(_LC_PORT + 16) : -1;
  bool running = a >= 0 &&
                 _sc_send_str(a, "GET /busy HTTP/1.1\r\nHost: h\r\n\r\n") &&
                 _sc_wait_for(srv, _rx_workers_active, NULL, 1, 5000);
  long long restart_ms = -1;
  ccol_retval restart_rv = ccol_unexpected_failure;
  int hello = -1;
  char buf[4096];
  if (running) {
    chttpsvr_stop(srv);
    chttpsvr_config again = cfg;
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
  size_t drained_after = chttpsvr__retired_drained_for_tests();
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
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool created = srv != CHTTPSVR_NULL;
  if (created)
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)(_LC_PORT + 17);
  ccol_retval rv1 = ccol_unexpected_failure, rv2 = ccol_unexpected_failure;
  int status_after_fail = -2, status_after_start = -2;
  if (created) {
    chttpsvr__fail_next_servers_register_for_tests();
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

// "localhost" binds 127.0.0.1, whose port another socket holds, so the start
// fails and nothing listens on ::1 instead. The holder uses an ephemeral port,
// because a fixed one may be in TIME_WAIT.
TEST(listener_host, a_host_name_reports_a_port_taken_on_its_address) {
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
  ccol_retval rv = ccol_success;
  if (held && srv != CHTTPSVR_NULL) {
    chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
    chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "localhost";
    cfg.port = port;
    rv = chttpsvr_start(srv, &cfg);
  }
  int v6 = held && _lc_have_ipv6_loopback() ? _lc_get_v6(port) : -1;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  if (other >= 0) close(other);
  REQUIRE_TRUE(held);
  REQUIRE_EQ((int)rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ(v6, -1);
}

// Tells whether "localhost" resolves to an IPv6 address on this host, for the
// ipv6_only listener test.
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
  if (!_lc_have_ipv6_loopback() || !_rx_localhost_has_ipv6()) {
    fprintf(stderr, "SKIP: localhost has no usable IPv6 address\n");
    return;
  }
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.ipv6_only = true;
  chttpsvr srv = _lc_server("localhost", _LC_PORT + 19, &cfg);
  int v6 = srv != CHTTPSVR_NULL ? _lc_get_v6(_LC_PORT + 19) : -2;
  int v4 = srv != CHTTPSVR_NULL ? _lc_get_v4(_LC_PORT + 19) : -2;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_EQ(v6, 200);
  REQUIRE_EQ(v4, -1);
}

TEST(listener_host, a_bracketed_address_names_the_address_inside) {
  chttpsvr srv4 = _lc_server("[127.0.0.1]", _LC_PORT + 20, NULL);
  int v4 = srv4 != CHTTPSVR_NULL ? _lc_get_v4(_LC_PORT + 20) : -2;
  if (srv4 != CHTTPSVR_NULL) chttpsvr_destroy(srv4);
  bool have_v6 = _lc_have_ipv6_loopback();
  chttpsvr srv6 =
      have_v6 ? _lc_server("[::1]", _LC_PORT + 21, NULL) : CHTTPSVR_NULL;
  int v6 =
      !have_v6 ? 200 : (srv6 != CHTTPSVR_NULL ? _lc_get_v6(_LC_PORT + 21) : -2);
  if (srv6 != CHTTPSVR_NULL) chttpsvr_destroy(srv6);
  chttpsvr bad = _lc_server("[::1", _LC_PORT + 22, NULL);
  bool bad_refused = bad == CHTTPSVR_NULL;
  if (bad != CHTTPSVR_NULL) chttpsvr_destroy(bad);
  REQUIRE_EQ(v4, 200);
  REQUIRE_EQ(v6, 200);
  REQUIRE_TRUE(bad_refused);
}

TEST(listener_host, a_host_that_does_not_resolve_is_logged_with_its_reason) {
  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_NUMERICHOST;
  int gai = getaddrinfo("a..b", "80", &hints, &res);
  if (res) freeaddrinfo(res);
  int pfd[2] = {-1, -1};
  bool piped = gai != 0 && pipe(pfd) == 0;
  if (piped) fcntl(pfd[0], F_SETFL, O_NONBLOCK);
  clog plog = piped ? clog_create_from_fd(pfd[1], CLOG_INFO, NULL) : CLOG_NULL;
  bool installed = plog && chttpsvr_set_engine_logger(plog) == ccol_success;
  chttpsvr srv =
      installed ? _lc_server("[a..b]", _LC_PORT + 23, NULL) : CHTTPSVR_NULL;
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
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
  REQUIRE_TRUE(srv == CHTTPSVR_NULL);
  REQUIRE_TRUE(strstr(text, gai_strerror(gai)) != NULL);
}

// Writes the counts of the query keys "a b" and "a%20b" and the result and
// value of chttpsvr_req_query_one for "x&y", to check that keys are matched
// against their decoded form.
static void _rx_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t n_plain = 0, n_encoded = 0;
  chttpsvr_req_query(req, "a b", &n_plain);
  chttpsvr_req_query(req, "a%20b", &n_encoded);
  const char *one = NULL;
  ccol_retval rv = chttpsvr_req_query_one(req, "x&y", &one);
  chttpsvr_resp_printf(resp, "%zu %zu %d %s", n_plain, n_encoded, (int)rv,
                       one ? one : "-");
}

TEST(chttpserver, a_query_key_is_matched_against_the_decoded_keys) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  bool started =
      srv != CHTTPSVR_NULL &&
      chttpsvr_register_handler(srv, CHTTP_GET, "/q", _rx_query_handler,
                                NULL) == ccol_success;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(started);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "\r\n\r\n2 0 0 3") != NULL);
}

// A body answered with 408 gives its reservation back when the linger starts,
// not when it ends.
TEST(lingering_close, a_linger_gives_the_body_memory_back_at_once) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 25, &cfg);
  int fd = srv != CHTTPSVR_NULL
               ? _sc_open_partial_post(_LC_PORT + 25, "/len", 8000, 100, NULL)
               : -1;
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged = parked ? chttpsvr__mem_in_use_for_tests(srv) : 0;
  int status = -1;
  bool released = false;
  size_t lingering = 0;
  if (parked) {
    chttpsvr__advance_slow_clock_for_tests(60000);
    chttpsvr__sweep_now_for_tests(srv);
    char buf[2048];
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
    if (_sc_wait_for(srv, _lc_lingering, NULL, 1, 5000)) {
      released = chttpsvr__mem_in_use_for_tests(srv) == 0;
      lingering = chttpsvr__parked_count_for_tests(srv, 6);
    }
  }
  if (fd >= 0) close(fd);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(charged, (size_t)8000);
  REQUIRE_EQ(status, 408);
  REQUIRE_TRUE(released);
  REQUIRE_EQ(lingering, (size_t)1);
}

TEST(lingering_close, a_large_refused_upload_is_taken_whole_before_the_close) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 26, NULL);
  int fd = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 26) : -1;
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
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(write_errno, 0);
  REQUIRE_EQ(pushed, total);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
}

// Waits, for at most timeout_ms, until the server's end of client_fd's
// connection (the socket of this process on port, other than client_fd, that
// has a peer) holds unread bytes. The linger tests wait for it before they let
// the server decide, because on OpenBSD a loopback packet can still be on its
// way after send() returns.
static bool _lc_server_end_holds_bytes(int port, int client_fd,
                                       int timeout_ms) {
  int server_fd = -1;
  for (int fd = 3; fd < 4096 && server_fd < 0; fd++) {
    struct sockaddr_storage ss, peer;
    socklen_t sl = sizeof(ss), pl = sizeof(peer);
    if (fd == client_fd || getsockname(fd, (struct sockaddr *)&ss, &sl) != 0)
      continue;
    if (ss.ss_family != AF_INET ||
        ntohs(((struct sockaddr_in *)&ss)->sin_port) != port)
      continue;
    if (getpeername(fd, (struct sockaddr *)&peer, &pl) == 0) server_fd = fd;
  }
  if (server_fd < 0) return false;
  long long deadline = _sc_now_ms() + timeout_ms;
  for (;;) {
    int queued = 0;
    if (ioctl(server_fd, FIONREAD, &queued) == 0 && queued > 0) return true;
    if (_sc_now_ms() >= deadline) return false;
    _sc_nap_ms(1);
  }
}

// The client sent bytes that the server never reads, so a plain close would
// reset the connection and destroy the response; the server lingers instead.
TEST(lingering_close, a_close_after_the_response_lingers_on_unread_bytes) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 41, &cfg);
  _sc_gate_open(false);
  int runs = atomic_load(&g_sc_gate_runs);
  size_t lingers = chttpsvr__linger_count_for_tests();
  int fd = srv != CHTTPSVR_NULL ? _sc_connect(_LC_PORT + 41) : -1;
  bool sent = fd >= 0 && _sc_send_str(fd,
                                      "GET /busy HTTP/1.1\r\nHost: h\r\n"
                                      "Connection: close\r\n\r\n");
  bool entered = false;
  for (int i = 0; sent && i < 5000 && !entered; i++) {
    entered = atomic_load(&g_sc_gate_runs) > runs;
    if (!entered) _sc_nap_ms(1);
  }
  bool extra = entered && _sc_send_str(fd,
                                       "GET /hello HTTP/1.1\r\nHost: h\r\n"
                                       "X-Pad: aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
  bool arrived = extra && _lc_server_end_holds_bytes(_LC_PORT + 41, fd, 5000);
  _sc_gate_open(true);
  _sc_nap_ms(300);
  char buf[2048] = {0};
  int status = extra ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = extra && _linger_count_passes(lingers);
  if (fd >= 0) close(fd);
  _sc_gate_open(false);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(extra);
  REQUIRE_TRUE(arrived);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(strstr(buf, "gated") != NULL);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
}

// TCP Fast Open lets a SYN carry request bytes that can be replayed, so the
// listener must leave it off. The listener is the socket on the port with no
// peer.
TEST(listeners, a_tcp_listener_does_not_turn_on_fast_open) {
#if defined(TCP_FASTOPEN)
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv = _sc_server(_LC_PORT + 42, &cfg);
  int found = 0, qlen_seen = -1;
  for (int fd = 3; srv != CHTTPSVR_NULL && fd < 4096 && !found; fd++) {
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
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_EQ(found, 1);
  REQUIRE_EQ(qlen_seen, 0);
#else
  fprintf(stderr, "SKIP: this system has no TCP_FASTOPEN to check\n");
#endif
}

// Starts a server on port with the failing allocator, sends the head of a POST
// to path, and once the body is diverted sends its 5-byte body together with a
// pipelined GET of exactly n2 bytes. The first allocation of that size, the
// push-back of those bytes by the body read, fails. Reports like
// _lc_pipeline_with_lost_leftover, for
// a_lost_push_back_of_pipelined_bytes_closes_the_connection.
static void _rx_pipeline_after_body(int port, const char *path, int *first,
                                    int *second, int *end) {
  ccol_memmgmt_procs mp = {_lc_malloc, _lc_free, _lc_calloc, _lc_realloc};
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  *first = *second = *end = -2;
  if (srv == CHTTPSVR_NULL) return;
  chttpsvr_register_handler(srv, CHTTP_POST, "/rx-buffered", _sc_len_handler,
                            NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/rx-streaming",
                                      _stream_echo_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/lc", _sc_hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
          *end = -1;
        }
      }
    }
    close(fd);
  }
  atomic_store(&g_lc_fail_size, 0);
  chttpsvr_destroy(srv);
}

TEST(chttpserver, a_lost_push_back_of_pipelined_bytes_closes_the_connection) {
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

// A SIGUSR1 handler that does nothing, so the signal only interrupts a wait.
static void _rx_sigusr1_noop(int sig) { (void)sig; }

// The input and the results of _rx_blocking_read.
typedef struct {
  int fd;
  _Atomic bool done;
  ssize_t got;
  bool timed_out;
  long long ms;
} _rx_read_ctx_t;

// Reads ctx->fd through chttp1_stream_read with a 300 ms timeout and records
// the result, how long it took and whether it timed out. The signal test
// interrupts it with SIGUSR1 and checks that the wait still ends at its
// timeout.
static void *_rx_blocking_read(void *arg) {
  _rx_read_ctx_t *ctx = (_rx_read_ctx_t *)arg;
  chttp1_stream st;
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

// A submit held in a test hook meets a pool that a restart retired; the request
// must go to the new pool and get 200, not 503.
TEST(restart, a_request_whose_submit_meets_a_retired_pool_is_still_served) {
  chttpsvr srv = _lc_server("127.0.0.1", _LC_PORT + 29, NULL);
  bool started = srv != CHTTPSVR_NULL;
  size_t drains_before = chttpsvr__retired_drain_started_for_tests();
  size_t retried_before = chttpsvr__submit_retried_for_tests();
  int fd = started ? _sc_connect(_LC_PORT + 29) : -1;
  bool entered = false, draining = false, shut_down = false;
  ccol_retval restart_rv = ccol_unexpected_failure;
  if (fd >= 0) {
    chttpsvr__arm_divert_submit_hook_for_tests(2);
    if (_sc_send_str(fd,
                     "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n"
                     "GET /lc HTTP/1.1\r\nHost: h\r\n\r\n")) {
      for (int i = 0; i < 5000 && !entered; i++) {
        entered = chttpsvr__divert_submit_hook_entered_for_tests();
        if (!entered) _sc_nap_ms(1);
      }
    }
    if (entered) {
      chttpsvr_stop(srv);
      chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = (uint16_t)(_LC_PORT + 29);
      restart_rv = chttpsvr_start(srv, &cfg);
      for (int i = 0; i < 5000 && !draining; i++) {
        draining = chttpsvr__retired_drain_started_for_tests() > drains_before;
        if (!draining) _sc_nap_ms(1);
      }
      for (int i = 0; draining && i < 5000 && !shut_down; i++) {
        shut_down = chttpsvr__retired_worker_shutdown_started_for_tests();
        if (!shut_down) _sc_nap_ms(1);
      }
    }
  }
  chttpsvr__release_divert_submit_hook_for_tests();
  char buf[2048];
  int first =
      entered ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int second =
      first == 200 ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  size_t retried = chttpsvr__submit_retried_for_tests() - retried_before;
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

// The server, the config and the result of _rx_start_thread.
typedef struct {
  chttpsvr srv;
  chttpsvr_config cfg;
  ccol_retval rv;
} _rx_start_ctx_t;

// Starts ctx->srv with ctx->cfg and stores the result, so a test can send a
// request while the start runs.
static void *_rx_start_thread(void *arg) {
  _rx_start_ctx_t *ctx = (_rx_start_ctx_t *)arg;
  ctx->rv = chttpsvr_start(ctx->srv, &ctx->cfg);
  return NULL;
}

// A streaming request that arrives while a start is held after its pool swap
// must build its pool with the new thread count.
TEST(restart, a_streaming_request_during_the_start_uses_the_new_settings) {
  _sc_gate_open(true);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 31, &cfg);
  bool started = srv != CHTTPSVR_NULL;
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
    (void)chttpsvr__take_stream_pool_threads_for_tests();
    chttpsvr__arm_start_hold_for_tests();
    start_started =
        pthread_create(&start_tid, NULL, _rx_start_thread, &sctx) == 0;
    for (int i = 0; start_started && i < 5000 && !held; i++) {
      held = chttpsvr__start_hold_entered_for_tests();
      if (!held) _sc_nap_ms(1);
    }
    if (held && _sc_send_str(fd,
                             "POST /gate HTTP/1.1\r\nHost: h\r\n"
                             "Content-Length: 0\r\n\r\n")) {
      for (int i = 0; i < 5000 && threads == 0; i++) {
        threads = chttpsvr__take_stream_pool_threads_for_tests();
        if (threads == 0) _sc_nap_ms(1);
      }
    }
  }
  chttpsvr__release_start_hold_for_tests();
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

// A pool built by a request held across a restart is installed only if the run
// didn't change, so the new run gets its own size.
TEST(restart, a_streaming_pool_built_across_a_restart_uses_the_new_size) {
  _sc_gate_open(true);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 39, &cfg);
  bool started = srv != CHTTPSVR_NULL;
  char buf[2048];
  int fd = started ? _sc_connect(_LC_PORT + 39) : -1;
  int hello = -1;
  if (fd >= 0 && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    hello = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool held = false;
  int threads = 0, gated = -1;
  ccol_retval restart_rv = ccol_unexpected_failure;
  if (hello == 200) {
    (void)chttpsvr__take_stream_pool_threads_for_tests();
    chttpsvr__arm_stream_create_hold_for_tests();
    if (_sc_send_str(fd,
                     "POST /gate HTTP/1.1\r\nHost: h\r\n"
                     "Content-Length: 0\r\n\r\n")) {
      for (int i = 0; i < 5000 && !held; i++) {
        held = chttpsvr__stream_create_hold_entered_for_tests();
        if (!held) _sc_nap_ms(1);
      }
    }
    if (held) {
      chttpsvr_stop(srv);
      chttpsvr_config cfg2 = cfg;
      cfg2.host = "127.0.0.1";
      cfg2.port = (uint16_t)(_LC_PORT + 40);
      cfg2.streaming_thread_count = 3;
      restart_rv = chttpsvr_start(srv, &cfg2);
    }
    chttpsvr__release_stream_create_hold_for_tests();
    for (int i = 0; held && i < 5000 && threads == 0; i++) {
      threads = chttpsvr__take_stream_pool_threads_for_tests();
      if (threads == 0) _sc_nap_ms(1);
    }
    if (held) gated = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  }
  chttpsvr__release_stream_create_hold_for_tests();
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
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  chttpsvr srv = _sc_server(_LC_PORT + 33, &cfg);
  bool started = srv != CHTTPSVR_NULL;
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
             chttpsvr__stream_queue_len_for_tests(srv) == 2;
    if (!queued) _sc_nap_ms(1);
  }
  ccol_retval restart_rv = ccol_unexpected_failure;
  if (queued) {
    chttpsvr_stop(srv);
    (void)chttpsvr__take_stream_pool_threads_for_tests();
    chttpsvr_config cfg2 = cfg;
    cfg2.host = "127.0.0.1";
    cfg2.port = (uint16_t)(_LC_PORT + 34);
    cfg2.streaming_thread_count = 2;
    restart_rv = chttpsvr_start(srv, &cfg2);
  }
  _sc_gate_open(true);
  int threads = 0;
  for (int i = 0; restart_rv == ccol_success && i < 5000 && threads == 0; i++) {
    threads = chttpsvr__take_stream_pool_threads_for_tests();
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

// Streaming handler that writes "quick" and returns at once, for the tests of
// streaming requests across a restart.
static void _rx_quick_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "quick");
}

// The busy streaming task count belongs to one run, so a request of the new run
// starts at once while an old request holds the old pool's only thread.
TEST(restart, a_new_streaming_request_does_not_wait_for_an_old_one) {
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_timeout_us = 2000000;
  chttpsvr srv = _sc_server(_LC_PORT + 35, &cfg);
  bool started = srv != CHTTPSVR_NULL;
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
  ccol_retval restart_rv = ccol_unexpected_failure;
  if (held) {
    chttpsvr_stop(srv);
    chttpsvr_config cfg2 = cfg;
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
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.streaming_thread_count = 1;
  cfg.streaming_queue_timeout_us = 30000000;
  chttpsvr srv = _sc_server(_LC_PORT + 37, &cfg);
  bool started = srv != CHTTPSVR_NULL;
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
       queued && i < 5000 && chttpsvr__stream_queue_len_for_tests(srv) != 1;
       i++)
    _sc_nap_ms(1);
  queued = queued && chttpsvr__stream_queue_len_for_tests(srv) == 1;
  ccol_retval restart_rv = ccol_unexpected_failure;
  if (queued) {
    chttpsvr_stop(srv);
    chttpsvr_config cfg2 = cfg;
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

// The first port of the tests of what a waiting connection keeps of its request
// and of how body buffers are reserved.
#define _FX_PORT (TEST_PORT + 260)

// Reads the body and answers with _SC_BIG_RESPONSE bytes, so the response parks
// on a client that doesn't read while the request is still around.
static void _fx_body_then_big_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                      void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_big_handler(req, resp, ctx);
}

// Starts a server on port with extra's settings (the defaults when NULL) and
// the routes POST /upload-big/{id}, POST /len and GET /hello, for the tests of
// what a parked or lingering connection keeps. Returns it, or CHTTPSVR_NULL.
static chttpsvr _fx_server(int port, const chttpsvr_config *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_POST, "/upload-big/{id}",
                            _fx_body_then_big_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _sc_len_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _sc_hello_handler, NULL);
  chttpsvr_config cfg = extra ? *extra : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  return srv;
}

// A parked connection holds no thread, so nothing bounds how many there are; it
// must drop its request (body, target, header fields) while it waits.
TEST(request_state, a_parked_response_keeps_nothing_of_its_request) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.worker_thread_count = 2;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);

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
  size_t kept = chttpsvr__parked_request_state_for_tests(srv);
  size_t mem = chttpsvr__mem_in_use_for_tests(srv);

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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 1, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd = _sc_connect(_FX_PORT + 1);
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope?x=1", 1048576, 4096);
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  bool lingering =
      status == 404 && _sc_wait_for(srv, _lc_lingering, NULL, 1, 5000);
  size_t kept = chttpsvr__parked_request_state_for_tests(srv);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_TRUE(lingering);
  REQUIRE_EQ(kept, (size_t)0);
}

TEST(request_state, a_lingering_connection_keeps_nothing_of_its_response) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _sc_server(_FX_PORT + 15, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  bool arrived = extra && _lc_server_end_holds_bytes(_FX_PORT + 15, fd, 5000);
  _sc_gate_open(true);
  char buf[2048] = {0};
  int status = extra ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  bool lingering =
      status == 200 && _sc_wait_for(srv, _lc_lingering, NULL, 1, 5000);
  size_t kept = chttpsvr__parked_request_state_for_tests(srv);
  _sc_gate_open(false);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(extra);
  REQUIRE_TRUE(arrived);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(lingering);
  REQUIRE_EQ(kept, (size_t)0);
}

// The reservation of a 70000-byte body covers each growth of its buffer (old
// and new block during the copy) before the read that needs it.
TEST(body_memory, a_framed_body_reserves_all_that_its_buffer_holds) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 2, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__body_overshoot_reset_for_tests();
  int fd = _sc_open_partial_post(_FX_PORT + 2, "/len", 70000, 66000, NULL);
  bool parked = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t over_parked = chttpsvr__body_overshoot_for_tests();
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
  REQUIRE_EQ(chttpsvr__body_overshoot_for_tests(), (size_t)0);
  REQUIRE_TRUE(mem_zero);
}

TEST(body_memory, a_chunked_body_reserves_all_that_its_buffer_holds) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 3, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__body_overshoot_reset_for_tests();
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
  REQUIRE_EQ(chttpsvr__body_overshoot_for_tests(), (size_t)0);
  REQUIRE_TRUE(mem_zero);
}

// The size of the 404 of _fx_big_reject_mw, far bigger than the socket buffers.
#define _FX_BIG_REJECT ((size_t)8 * 1024 * 1024)

// Answers every request under /big-reject itself with a 404 of _FX_BIG_REJECT
// bytes taken from ctx, and passes every other request on. It runs for a
// rejected request, on the reject pool, so the rejection tests can check that
// the library's own writes park instead of waiting on the peer.
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

// A 404 far bigger than the socket buffers parks without holding a reject pool
// thread and arrives whole once the client reads. The internal write ceiling is
// raised, because under valgrind the bodies take longer than 2 s.
TEST(internal_writes, a_rejection_to_a_client_that_does_not_read_parks) {
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) free(big);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__set_internal_write_ceiling_ms_for_tests(120000);
  chttpsvr_use(srv, _fx_big_reject_mw, big);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _sc_hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  chttpsvr__set_internal_write_ceiling_ms_for_tests(0);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(opened);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(whole, 2);
  REQUIRE_TRUE(none_parked);
}

TEST(internal_writes, a_parked_rejection_ends_at_its_ceiling) {
  char *big = malloc(_FX_BIG_REJECT);
  REQUIRE_TRUE(big != NULL);
  memset(big, 'z', _FX_BIG_REJECT);
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) free(big);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_use(srv, _fx_big_reject_mw, big);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  if (parked) _sc_nap_ms(2200);
  if (parked) chttpsvr__sweep_now_for_tests(srv);
  size_t still = started ? chttpsvr__parked_count_for_tests(srv, 2) : 1;
  if (fd >= 0) close(fd);
  chttpsvr_destroy(srv);
  free(big);
  REQUIRE_TRUE(started);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(still, (size_t)0);
}

TEST(internal_writes, a_short_bare_rejection_parks_and_completes) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 6, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t parks = chttpsvr__park_kind_count_for_tests(2);
  int fd = _sc_connect(_FX_PORT + 6);
  chttpsvr__force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = chttpsvr__park_kind_count_for_tests(2) - parks;
  chttpsvr__force_short_response_write_for_tests(0);
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_GE(parked, (size_t)1);
}

TEST(internal_writes, an_interim_line_on_a_full_socket_parks_and_completes) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 7, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t parks = chttpsvr__park_kind_count_for_tests(2);
  chttpsvr__force_interim_park_after_for_tests(5);
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
  size_t parked = chttpsvr__park_kind_count_for_tests(2) - parks;
  char buf[2048];
  int status = -1;
  if (interim_ok && _sc_send_str(fd, "0123456789"))
    status = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n10") != NULL;
  int again = -1;
  if (len_ok && _sc_send_str(fd, "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n"))
    again = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  chttpsvr__force_interim_park_after_for_tests((size_t)-1);
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

// Writes what the peer certificate calls give over plaintext (DER pointer and
// length, SHA-256 return and whether it zeroed the buffer, subject), for
// a_plaintext_request_has_no_client_identity.
static void _fx_identity_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)ctx;
  size_t len = 7;
  const void *der = chttpsvr_req_peer_cert_der(req, &len);
  unsigned char sha[CHTTPSVR_PEER_CERT_SHA256_LEN];
  memset(sha, 0xAB, sizeof(sha));
  ccol_retval rv = chttpsvr_req_peer_cert_sha256(req, sha);
  int zero = 1;
  for (int i = 0; i < CHTTPSVR_PEER_CERT_SHA256_LEN; i++)
    zero = zero && sha[i] == 0;
  const char *subj = chttpsvr_req_peer_cert_subject(req);
  chttpsvr_resp_printf(resp, "der=%d len=%zu rv=%d zero=%d subj=%d",
                       der != NULL, len, (int)rv, zero, subj != NULL);
}

TEST(request_state, a_plaintext_request_has_no_client_identity) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ(chttpsvr_register_handler(srv, CHTTP_GET, "/id",
                                       _fx_identity_handler, NULL),
             ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// A rejection that the reject pool couldn't take is written on the reactor; its
// write is forced short and must park instead of waiting on the peer.
TEST(internal_writes, an_inline_rejection_on_the_reactor_parks_and_completes) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_FX_PORT + 9, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  _wait_for_reject_counter_to_settle();
  size_t parks = chttpsvr__park_kind_count_for_tests(2);
  size_t rejects = chttpsvr__reject_pool_task_count_for_tests();
  int fd = _sc_connect(_FX_PORT + 9);
  chttpsvr__force_reject_inline_for_tests(true);
  chttpsvr__force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = chttpsvr__park_kind_count_for_tests(2) - parks;
  size_t on_pool = chttpsvr__reject_pool_task_count_for_tests() - rejects;
  chttpsvr__force_short_response_write_for_tests(0);
  chttpsvr__force_reject_inline_for_tests(false);
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

// The first port of the pause failure tests. Each of them makes every pause
// that hands a connection from the reactor to a thread fail, so the connection
// loses its registration and its next wait adds a fresh one for the same
// descriptor while the old one's on_removed may still be pending. A count of
// those removals shows that the path really ran.
#define _PF_PORT (TEST_PORT + 280)

// Connects to port and waits, for at most 5 s, until srv holds the connection
// idle under a registration, so its first request arrives through a
// registration and not through the first read after the accept. Returns the fd,
// or -1.
static int _pf_connect_registered(chttpsvr srv, int port) {
  int fd = _sc_connect(port);
  if (fd >= 0 && !_sc_wait_for(srv, _rx_idle, NULL, 1, 5000)) {
    close(fd);
    fd = -1;
  }
  return fd;
}

TEST(pause_fallback, a_rejection_that_parks_completes) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_PF_PORT + 0, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t parks = chttpsvr__park_kind_count_for_tests(2);
  size_t fallbacks = chttpsvr__pause_fallback_count_for_tests();
  int fd = _pf_connect_registered(srv, _PF_PORT + 0);
  chttpsvr__force_pause_fail_for_tests(true);
  chttpsvr__force_short_response_write_for_tests(10);
  bool sent =
      fd >= 0 && _sc_send_str(fd, "GET /missing HTTP/1.1\r\nHost: h\r\n\r\n");
  char buf[2048];
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status == 404 ? _lc_stream_end(fd, 5000) : -2;
  size_t parked = chttpsvr__park_kind_count_for_tests(2) - parks;
  chttpsvr__force_short_response_write_for_tests(0);
  chttpsvr__force_pause_fail_for_tests(false);
  size_t removed = chttpsvr__pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_GE(parked, (size_t)1);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_refused_upload_lingers_and_ends_in_order) {
  chttpsvr srv = _lc_server("127.0.0.1", _PF_PORT + 1, NULL);
  size_t lingers = chttpsvr__linger_count_for_tests();
  size_t fallbacks = chttpsvr__pause_fallback_count_for_tests();
  chttpsvr__force_pause_fail_for_tests(true);
  int fd =
      srv != CHTTPSVR_NULL ? _pf_connect_registered(srv, _PF_PORT + 1) : -1;
  bool sent = fd >= 0 && _lc_send_upload_start(fd, "/nope", 1048576, 32768);
  char buf[2048] = {0};
  int status = sent ? _sc_read_response(fd, buf, sizeof(buf), NULL, 5000) : -1;
  int end = status > 0 ? _lc_stream_end(fd, 5000) : -2;
  bool lingered = _linger_count_passes(lingers);
  chttpsvr__force_pause_fail_for_tests(false);
  size_t removed = chttpsvr__pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  bool drained =
      srv != CHTTPSVR_NULL && _sc_wait_for_zero(srv, _lc_lingering, NULL, 5000);
  if (srv != CHTTPSVR_NULL) chttpsvr_destroy(srv);
  REQUIRE_TRUE(sent);
  REQUIRE_EQ(status, 404);
  REQUIRE_EQ(end, 0);
  REQUIRE_TRUE(lingered);
  REQUIRE_TRUE(drained);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_keep_alive_connection_serves_its_next_requests) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_PF_PORT + 2, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t fallbacks = chttpsvr__pause_fallback_count_for_tests();
  chttpsvr__force_pause_fail_for_tests(true);
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
  chttpsvr__force_pause_fail_for_tests(false);
  size_t removed = chttpsvr__pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  if (fd2 >= 0) close(fd2);
  REQUIRE_EQ(s1, 200);
  REQUIRE_EQ(s2, 200);
  REQUIRE_EQ(s3, 404);
  REQUIRE_EQ(s4, 200);
  REQUIRE_GE(removed, (size_t)1);
}

TEST(pause_fallback, a_parked_body_resumes_and_completes) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _fx_server(_PF_PORT + 3, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  size_t fallbacks = chttpsvr__pause_fallback_count_for_tests();
  chttpsvr__force_pause_fail_for_tests(true);
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
  chttpsvr__force_pause_fail_for_tests(false);
  size_t removed = chttpsvr__pause_fallback_count_for_tests() - fallbacks;
  if (fd >= 0) close(fd);
  bool mem_zero = _sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000);
  REQUIRE_TRUE(parked);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(len_ok);
  REQUIRE_EQ(again, 200);
  REQUIRE_GE(removed, (size_t)2);
  REQUIRE_TRUE(mem_zero);
}

// The first port of the tests that a body's memory reservation lasts exactly as
// long as the body.
#define _BM_PORT (TEST_PORT + 300)

// The server of _bm_server, and what _bm_probe_handler saw while it ran: its
// own request's reservation, its body buffer's capacity, and the reservations
// of the whole server.
static _Atomic uint64_t g_bm_srv = 0;
static _Atomic size_t g_bm_charged = 0;
static _Atomic size_t g_bm_held = 0;
static _Atomic size_t g_bm_in_use = 0;

// Records its request's reservation, its buffer capacity and the server's total
// in the g_bm_* globals, then writes the body length. It lets a test see the
// memory of a body while its handler runs.
static void _bm_probe_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)ctx;
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  size_t charged = 0, held = 0;
  chttpsvr__req_body_memory_for_tests(req, &charged, &held);
  atomic_store(&g_bm_charged, charged);
  atomic_store(&g_bm_held, held);
  atomic_store(&g_bm_in_use, chttpsvr__mem_in_use_for_tests(
                                 (chttpsvr)atomic_load(&g_bm_srv)));
  chttpsvr_resp_printf(resp, "%zu", n);
}

// Reads the body, then holds its thread at the gate of the slow-client tests,
// so a test can keep a body's memory held as long as it needs.
static void _bm_hold_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                             void *ctx) {
  size_t n = 0;
  (void)chttpsvr_req_body(req, &n);
  _sc_gate_stream_handler(req, resp, ctx);
}

// Starts a server on port with extra's settings and the routes /probe, /hold
// and /len, and stores it in g_bm_srv for _bm_probe_handler. Returns it, or
// CHTTPSVR_NULL.
static chttpsvr _bm_server(int port, const chttpsvr_config *extra) {
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) return CHTTPSVR_NULL;
  chttpsvr_register_handler(srv, CHTTP_POST, "/probe", _bm_probe_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/hold", _bm_hold_handler, NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _sc_len_handler, NULL);
  chttpsvr_config cfg = extra ? *extra : CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = (uint16_t)port;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    chttpsvr_destroy(srv);
    return CHTTPSVR_NULL;
  }
  atomic_store(&g_bm_srv, (uint64_t)srv);
  return srv;
}

// Sends the head of a POST to path with "Expect: 100-continue" (framed by a
// Content-Length of len, or chunked), waits for the interim line and then sends
// len body bytes, so the body never arrives with the head and its memory is
// reserved before the server reads it. Returns the final status, with the
// response in buf, or -1.
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

// Tells whether srv has no reservation, no memory holder and no exemption left.
// The body memory tests check it at the end.
static bool _bm_nothing_reserved(chttpsvr srv) {
  if (!_sc_wait_for_zero(srv, _sc_mem_in_use, NULL, 5000)) return false;
  bool exempt = true;
  size_t holders = chttpsvr__mem_holders_for_tests(srv, &exempt);
  return holders == 0 && !exempt;
}

TEST(body_memory, a_running_handler_holds_exactly_the_memory_of_its_body) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 1024 * 1024;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _bm_server(_BM_PORT + 0, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int fd ccol__destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 0);
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

// A running handler keeps its body's reservation, so a second body waits (no
// interim line, no reader) until that handler returns.
TEST(body_memory, a_body_waits_while_a_handler_holds_the_limit) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 100000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _bm_server(_BM_PORT + 1, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  _sc_gate_open(false);
  atomic_store(&g_sc_gate_runs, 0);
  int a ccol__destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 1);
  int b ccol__destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 1);
  bool opened = a >= 0 && b >= 0;
  char head[256];
  int hn = snprintf(head, sizeof(head),
                    "POST /hold HTTP/1.1\r\nHost: h\r\n"
                    "Expect: 100-continue\r\nContent-Length: 90000\r\n\r\n");
  char buf[4096];
  bool a_sent = opened && _sc_send(a, head, (size_t)hn) &&
                _sc_read_response(a, buf, sizeof(buf), NULL, 5000) == 100 &&
                _sc_send_fill(a, 90000, 'a');
  long long deadline = _sc_now_ms() + 5000;
  while (a_sent && atomic_load(&g_sc_gate_runs) < 1 && _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  bool a_running = atomic_load(&g_sc_gate_runs) == 1;
  size_t held_by_a = chttpsvr__mem_in_use_for_tests(srv);
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

// Each way a body can end (client leaves, rate floor 408, 413, malformed chunk,
// client leaves during the handler, 503 of a memory wait) gives its whole
// memory reservation back.
TEST(body_memory, no_path_that_ends_a_body_keeps_its_reservation) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 65536;
  cfg.max_body_size = 100000;
  cfg.min_transfer_rate_bps = 100;
  cfg.min_transfer_rate_grace_us = 1000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _bm_server(_BM_PORT + 2, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  char buf[4096];

  int fd = _sc_open_partial_post(_BM_PORT + 2, "/len", 8000, 100, NULL);
  bool parked1 = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged1 = chttpsvr__mem_in_use_for_tests(srv);
  if (fd >= 0) close(fd);
  bool none1 = _bm_nothing_reserved(srv);

  fd = _sc_open_partial_post(_BM_PORT + 2, "/len", 8000, 100, NULL);
  bool parked2 = fd >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  size_t charged2 = chttpsvr__mem_in_use_for_tests(srv);
  int s2 = -1;
  if (parked2) {
    chttpsvr__advance_slow_clock_for_tests(60000);
    chttpsvr__sweep_now_for_tests(srv);
    s2 = _sc_read_response(fd, buf, sizeof(buf), NULL, 5000);
  }
  bool none2 = _bm_nothing_reserved(srv);
  if (fd >= 0) close(fd);

  fd = _sc_connect(_BM_PORT + 2);
  int s3 = fd >= 0 ? _bm_post_after_continue(fd, "/len", 150000, true, buf,
                                             sizeof(buf))
                   : -1;
  if (fd >= 0) close(fd);
  bool none3 = _bm_nothing_reserved(srv);

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
  size_t charged5 = chttpsvr__mem_in_use_for_tests(srv);
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

// A chunked body that holds part of the limit while it waits for more gives
// that part back with its 503.
TEST(body_memory, a_chunked_wait_that_ends_in_503_gives_its_part_back) {
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 65536;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 120000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _bm_server(_BM_PORT + 3, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  int h ccol__destructor(_close_scoped_fd) =
      _sc_open_partial_post(_BM_PORT + 3, "/len", _SC_HOLD_LEN, 100, NULL);
  bool parked = h >= 0 && _sc_wait_for(srv, _sc_parked_bodies, NULL, 1, 5000);
  int c ccol__destructor(_close_scoped_fd) = _sc_connect(_BM_PORT + 3);
  char buf[4096];
  bool c_sent =
      parked && c >= 0 &&
      _sc_send_str(c,
                   "POST /len HTTP/1.1\r\nHost: h\r\nExpect: "
                   "100-continue\r\nTransfer-Encoding: chunked\r\n\r\n") &&
      _sc_read_response(c, buf, sizeof(buf), NULL, 5000) == 100 &&
      _sc_send_str(c, "9c40\r\n") && _sc_send_fill(c, 40000, 'c');
  bool waits = c_sent && _sc_wait_for(srv, _sc_mem_waiting, NULL, 1, 5000);
  size_t in_use_waiting = chttpsvr__mem_in_use_for_tests(srv);
  int status = -1;
  if (waits) {
    chttpsvr__advance_slow_clock_for_tests(31000);
    chttpsvr__sweep_now_for_tests(srv);
    status = _sc_read_response(c, buf, sizeof(buf), NULL, 5000);
  }
  long long deadline = _sc_now_ms() + 5000;
  while (chttpsvr__mem_in_use_for_tests(srv) != _SC_HOLD_LEN &&
         _sc_now_ms() < deadline)
    _sc_nap_ms(1);
  size_t in_use_after = chttpsvr__mem_in_use_for_tests(srv);
  size_t holders_after = chttpsvr__mem_holders_for_tests(srv, NULL);
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

// Thirty bodies declare 4 MiB and send 1000 bytes, against a 1 MiB limit. Each
// reserves only what arrived plus one read window, so an honest upload after
// them is served at once.
TEST(body_memory, bodies_that_trickle_hold_only_what_arrived) {
  enum { trickles = 30 };
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.max_partial_body_memory = 1024 * 1024;
  cfg.max_body_size = 8 * 1024 * 1024;
  cfg.body_memory_wait_timeout_us = 2000000;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _bm_server(_BM_PORT + 4, &cfg);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr__mem_peak_reset_for_tests();
  int fds[trickles];
  bool opened = true;
  for (int i = 0; i < trickles; i++) {
    fds[i] = _sc_open_partial_post(_BM_PORT + 4, "/len", 4 * 1024 * 1024, 1000,
                                   NULL);
    if (fds[i] < 0) opened = false;
  }
  bool all_parked =
      opened && _sc_wait_for(srv, _sc_parked_bodies, NULL, trickles, 5000);
  size_t held = chttpsvr__mem_in_use_for_tests(srv);
  long long t0 = _sc_now_ms();
  int up = all_parked ? _sc_open_partial_post(_BM_PORT + 4, "/len", 200000,
                                              200000, NULL)
                      : -1;
  char buf[4096];
  int status =
      up >= 0 ? _sc_read_response(up, buf, sizeof(buf), NULL, 5000) : -1;
  long long took = _sc_now_ms() - t0;
  bool len_ok = status == 200 && strstr(buf, "\r\n\r\n200000") != NULL;
  size_t peak = chttpsvr__mem_peak_for_tests();
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

// Handler entries of a_request_queued_for_a_worker_gets_503, and the flag that
// releases the first one.
static _Atomic int g_queued_drain_entries = 0;
static _Atomic bool g_queued_drain_release = false;

// Holds the first request until g_queued_drain_release is set and lets every
// later one return at once. It keeps the only worker busy so the next request
// waits in the queue.
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

// The socket path and the status that _queued_drain_client_fn got.
typedef struct {
  const char *sock_path;
  int status;
} _queued_drain_client_t;

// Destroys the server that arg points to, so the queued-request test can start
// the teardown while a handler runs.
static void *_destroy_srv_thread_fn(void *arg) {
  chttpsvr *srv = (chttpsvr *)arg;
  chttpsvr_destroy(*srv);
  return NULL;
}

// Sends GET /queued over the Unix socket at c->sock_path and stores the status
// in c->status, or -1. It is the client of the queued-request test.
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

// One worker holds the first request in its handler, the second waits in the
// queue, and the teardown starts before the first handler returns. The queued
// request must get 503 instead of running its handler after the destroy call.
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
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval rv = chttpsvr_register_handler(srv, CHTTP_GET, "/queued",
                                             _queued_drain_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  size_t submits = atomic_load(&chttpsvr__worker_submits_for_tests);
  bool b_started =
      held && pthread_create(&tb, NULL, _queued_drain_client_fn, &b) == 0;
  bool queued = false;
  for (int i = 0; b_started && i < 10000 && !queued; i++) {
    queued = atomic_load(&chttpsvr__worker_submits_for_tests) > submits;
    if (!queued) {
      struct timespec nap = {0, 1000000L};
      nanosleep(&nap, NULL);
    }
  }

  chttpsvr__arm_quiesce_claimed_signal_for_tests();
  pthread_t td;
  bool d_started = false;
  bool draining = false;
  if (queued) {
    d_started = pthread_create(&td, NULL, _destroy_srv_thread_fn, &srv) == 0;
    if (d_started)
      draining = chttpsvr__wait_quiesce_claimed_signal_entered_for_tests();
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

// Returns CLOCK_MONOTONIC in milliseconds, for the drain test.
static long long _drain_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// When chttpsvr_destroy() was called (0 before that), the handler entries, and
// the entries that started at least 100 ms after the call. A request dispatched
// just before the call can still enter its handler a moment after it, and the
// margin allows for that.
static _Atomic long long g_drain_destroy_start_ms = 0;
static _Atomic int g_drain_handler_entries = 0;
static _Atomic int g_drain_entries_after_destroy = 0;

// Counts its entry (and an entry late after the destroy), sleeps 300 ms and
// answers "done". It keeps the server busy in
// keep_alive_and_pipelined_clients_do_not_extend_destroy.
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

// One client of the drain test: its socket path, how many requests it
// pipelines, the shared start and stop flags, and how many answers it got.
typedef struct {
  const char *sock_path;
  int pipeline;
  _Atomic bool *stop;
  _Atomic bool *go;
  _Atomic int *arrived;
  _Atomic int completed;
} _drain_client_t;

// Counts the occurrences of needle in buf.
static size_t _drain_count(const char *buf, const char *needle) {
  size_t n = 0;
  for (const char *p = strstr(buf, needle); p; p = strstr(p + 1, needle)) n++;
  return n;
}

// Sends requests on one keep-alive connection, c->pipeline at a time, for as
// long as the server answers, and connects again when the server closes. It
// ends when a connect fails (the teardown stopped the listener) or c->stop is
// set. It is a client of the drain test, which checks that such clients don't
// extend the destroy.
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

// Keep-alive and pipelining clients keep a slow handler busy while
// chttpsvr_destroy() runs. No handler may start after the call, and the call
// returns in about the time of one handler instead of the 30 s bound of the
// in-flight wait.
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
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  ccol_retval reg_rv = chttpsvr_register_handler(srv, CHTTP_GET, "/drain-slow",
                                                 _drain_slow_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  ccol_retval start_rv =
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

#include <locale.h>

// Switches LC_CTYPE to a Turkish locale, where strncasecmp() and tolower()
// don't fold 'I' to 'i', and returns the previous LC_CTYPE (the caller passes
// it to _tr_locale_leave), or NULL with a skip message when no such locale
// exists. The locale tests run the server under it, because a name like
// "TRANSFER-ENCODING" then stops matching wherever code folds case through the
// locale. It is set for the whole process, since the parser runs on the
// server's threads. The Makefile builds tr_TR.UTF-8 into tests_locale/.
// AddressSanitizer replaces strncasecmp() with a version that ignores the
// locale, so only a build without it can tell the difference.
static char *_tr_locale_enter(void) {
  const char *old_ctype = setlocale(LC_CTYPE, NULL);
  char *saved_ctype = old_ctype ? strdup(old_ctype) : NULL;
  if (!saved_ctype) return NULL;
#if defined(__GLIBC__)
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

// Restores the LC_CTYPE that _tr_locale_enter saved and frees it.
static void _tr_locale_leave(char *saved_ctype) {
  setlocale(LC_CTYPE, saved_ctype);
  free(saved_ctype);
}

// Sends req to g_srv and reads until EOF (bounded by the read timeout) into
// buf, for the locale test.
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
  chttp1_settings st;
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
  chttp1_parser p;
  chttp1_parser_init_request(&p, &st);
  chttp1_errno r_smuggle = chttp1_parser_execute(&p, smuggle, strlen(smuggle));
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno r_close =
      chttp1_parser_execute(&p, close_req, strlen(close_req));
  bool close_keeps = chttp1_should_keep_alive(&p);
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno r_keep = chttp1_parser_execute(&p, keep_req, strlen(keep_req));
  bool keep_keeps = chttp1_should_keep_alive(&p);
  chttp1_parser_release(&p);

  chttp1_parser_init_request(&p, &st);
  chttp1_errno r_expect =
      chttp1_parser_execute(&p, expect_req, strlen(expect_req));
  bool expects = chttp1_expects_continue(&p);
  chttp1_parser_release(&p);
  _tr_locale_leave(saved);

  REQUIRE_EQ((int)r_smuggle, (int)CHTTP1_ERROR);
  REQUIRE_NE((int)r_close, (int)CHTTP1_ERROR);
  REQUIRE_FALSE(close_keeps);
  REQUIRE_NE((int)r_keep, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(keep_keeps);
  REQUIRE_NE((int)r_expect, (int)CHTTP1_ERROR);
  REQUIRE_TRUE(expects);
}

// What chttpsvr_resp_set_header returned for "TRANSFER-ENCODING" in
// _locale_header_handler.
static _Atomic int g_locale_set_te_rv = 1;

// Echoes x-client-id, tries to set "TRANSFER-ENCODING" (storing the result in
// g_locale_set_te_rv) and sets "CONNECTION". Under the Turkish locale both
// names must still be recognised: the first refused, the second kept off the
// wire.
static void _locale_header_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  const char *v = chttpsvr_req_header(req, "x-client-id");
  atomic_store(&g_locale_set_te_rv, (int)chttpsvr_resp_set_header(
                                        resp, "TRANSFER-ENCODING", "chunked"));
  chttpsvr_resp_set_header(resp, "CONNECTION", "x-from-handler");
  chttpsvr_resp_write_str(resp, v ? v : "(none)");
}

// Under the Turkish locale a server that folded names through strncasecmp()
// would serve a smuggled request by its Content-Length, keep "CONNECTION:
// close" open, send no interim 100, miss a header lookup, and accept a
// handler's "TRANSFER-ENCODING".
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

// A route keeps its method as one bit of a mask, so every registration entry
// point refuses a value outside the seven methods and CHTTP_ANY; a value of 32
// or more would otherwise shift past the mask.
TEST(chttpserver, a_method_outside_the_enum_is_refused_by_every_registration) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_router *sub = chttpsvr_subrouter(srv, "/sub");
  REQUIRE_TRUE(sub != NULL);
  const int bad[] = {(int)CHTTP_ANY + 1, 8, 31, 32, 100, -1};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
    chttp_method m = (chttp_method)bad[i];
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

// Answers 205 with a body, for reset_content_sends_no_body_and_a_zero_length.
static void _reset_content_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, 205);
  chttpsvr_resp_write_str(resp, "this body must not be sent");
}

// Sets the status named by the query parameter "code" (500 without it), so one
// route can answer with any code, for
// every_registered_status_code_has_its_reason_phrase.
static void _status_from_query_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                       void *ctx) {
  (void)ctx;
  const char *code = NULL;
  chttpsvr_req_query_one(req, "code", &code);
  chttpsvr_resp_set_status(resp, code ? atoi(code) : 500);
  chttpsvr_resp_write_str(resp, "x");
}

// 205 carries no content (RFC 9110 SS15.3.6), so the server drops the handler's
// body and sends "content-length:0".
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

// Stores in the ctx array what the header setters return for values with
// control bytes, a TAB and obs-text, then answers "ok", for
// a_header_value_with_a_control_byte_is_refused.
static void _control_byte_value_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)req;
  _Atomic int *results = (_Atomic int *)ctx;
  results[0] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x01z");
  results[1] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x1fz");
  results[2] = (int)chttpsvr_resp_set_header(resp, "x-ctl", "a\x7fz");
  results[3] = (int)chttpsvr_resp_add_header(resp, "x-ctl", "a\x0bz");
  results[4] = (int)chttpsvr_resp_add_header(resp, "x-ctl", "\x7f");
  results[5] = (int)chttpsvr_resp_set_header(resp, "x-tab", "a\tb c");
  results[6] = (int)chttpsvr_resp_add_header(resp, "x-obs", "caf\xc3\xa9");
  chttpsvr_resp_write_str(resp, "ok");
}

// A field value may hold VCHAR, SP, HTAB and obs-text (RFC 9110 SS5.5), and a
// conforming client refuses any other control byte, so the setters refuse them
// all, not only CR and LF.
TEST(chttpserver, a_header_value_with_a_control_byte_is_refused) {
  static _Atomic int results[7];
  for (int i = 0; i < 7; i++) atomic_store(&results[i], -100);
  REQUIRE_EQ((int)chttpsvr_register_handler(g_srv, CHTTP_GET, "/ctl-value",
                                            _control_byte_value_handler,
                                            (void *)results),
             (int)ccol_success);
  chttpcli_response *resp = _get("/ctl-value");
  int status = resp ? resp->status_code : -1;
  if (resp) chttpcli_resp_free(resp);
  REQUIRE_EQ(status, 200);
  for (int i = 0; i < 5; i++)
    REQUIRE_EQ(atomic_load(&results[i]), (int)ccol_invalid_args);
  REQUIRE_EQ(atomic_load(&results[5]), (int)ccol_success);
  REQUIRE_EQ(atomic_load(&results[6]), (int)ccol_success);
}

TEST(listen_info, port_zero_binds_an_ephemeral_port_that_a_client_reaches) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/port-zero",
                                            _hello_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  uint16_t port = 0;
  ccol_retval port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_SIZE];
  ccol_retval addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 101;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  ccol_retval rv = chttpsvr_bound_port(srv, &port);
  chttpsvr_stop(srv);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  REQUIRE_EQ((int)port, TEST_PORT + 101);
}

TEST(listen_info, the_default_host_reports_an_address_of_every_interface) {
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/any-host",
                                            _hello_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  ccol_retval port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_SIZE];
  ccol_retval addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
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
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  bool before = true, started = false, stopped = true, restarted = false;
  uint16_t first_port = 0, stopped_port = 7, second_port = 0;
  ccol_retval stopped_port_rv, before_addr_rv;
  char before_addr[CHTTPSVR_BOUND_ADDRESS_SIZE] = "x";
  chttpsvr_is_running(srv, &before);
  before_addr_rv =
      chttpsvr_bound_address(srv, before_addr, sizeof(before_addr));
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  ccol_retval start1 = chttpsvr_start(srv, &cfg);
  chttpsvr_is_running(srv, &started);
  chttpsvr_bound_port(srv, &first_port);
  chttpsvr_stop(srv);
  chttpsvr_is_running(srv, &stopped);
  stopped_port_rv = chttpsvr_bound_port(srv, &stopped_port);
  ccol_retval start2 = chttpsvr_start(srv, &cfg);
  chttpsvr_is_running(srv, &restarted);
  chttpsvr_bound_port(srv, &second_port);
  chttpsvr_destroy(srv);
  REQUIRE_TRUE(srv == CHTTPSVR_NULL);

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
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr stale = srv;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  ccol_retval start = chttpsvr_start(srv, &cfg);
  char small[9] = "xxxxxxxx";
  ccol_retval small_rv = chttpsvr_bound_address(srv, small, sizeof(small));
  char exact[10];
  ccol_retval exact_rv = chttpsvr_bound_address(srv, exact, sizeof(exact));
  ccol_retval null_run = chttpsvr_is_running(srv, NULL);
  ccol_retval null_port = chttpsvr_bound_port(srv, NULL);
  ccol_retval null_buf = chttpsvr_bound_address(srv, NULL, 16);
  char zero[4] = "abc";
  ccol_retval zero_len = chttpsvr_bound_address(srv, zero, 0);
  chttpsvr_destroy(srv);

  bool running = true;
  uint16_t port = 9;
  char addr[CHTTPSVR_BOUND_ADDRESS_SIZE] = "x";
  REQUIRE_EQ((int)chttpsvr_is_running(stale, &running), (int)ccol_invalid_args);
  REQUIRE_FALSE(running);
  REQUIRE_EQ((int)chttpsvr_bound_port(stale, &port), (int)ccol_invalid_args);
  REQUIRE_EQ((int)port, 0);
  REQUIRE_EQ((int)chttpsvr_bound_address(stale, addr, sizeof(addr)),
             (int)ccol_invalid_args);
  REQUIRE_STREQ(addr, "");
  running = true;
  REQUIRE_EQ((int)chttpsvr_is_running(CHTTPSVR_NULL, &running),
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_info_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  cfg.port = 0;
  ccol_retval start = chttpsvr_start(srv, &cfg);
  uint16_t port = 5;
  ccol_retval port_rv = chttpsvr_bound_port(srv, &port);
  char addr[CHTTPSVR_BOUND_ADDRESS_SIZE];
  ccol_retval addr_rv = chttpsvr_bound_address(srv, addr, sizeof(addr));
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

// The request context tests: the keys are the addresses of these constants, and
// the values are these strings, which the tests compare by address.
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

// What the global middleware saw after next() returned, by kind of request.
typedef struct {
  _Atomic int handler_value_seen;
  _Atomic int rejected_value_seen;
  _Atomic int calls_after_next;
} _ctx_probe_t;

// Tells whether the request carries x-ctx-skip, which makes the context
// middlewares set nothing, to check that values don't leak between requests.
static bool _ctx_skip(const chttpsvr_req *req) {
  return chttpsvr_req_header(req, "x-ctx-skip") != NULL;
}

// Sets key a (unless skipped), calls next, and then records in the _ctx_probe_t
// of ctx whether it sees the value the handler set, or for /ctx/missing the
// values of the sub-router. It is the global middleware of
// middleware_values_reach_the_handler_through_a_subrouter.
static void _ctx_global_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                           chttpsvr_next_fn next) {
  _ctx_probe_t *probe = (_ctx_probe_t *)ctx;
  if (!_ctx_skip(req))
    chttpsvr_req_set_context(req, &g_ctx_key_a, g_ctx_val_global_a);
  next(req, resp);
  atomic_fetch_add(&probe->calls_after_next, 1);
  if (_ctx_skip(req)) return;
  if (strcmp(chttpsvr_req_path(req), "/ctx/missing") == 0) {
    if (chttpsvr_req_context(req, &g_ctx_key_a) == g_ctx_val_sub_a &&
        chttpsvr_req_context(req, &g_ctx_key_e) == g_ctx_val_e)
      atomic_store(&probe->rejected_value_seen, 1);
  } else if (chttpsvr_req_context(req, &g_ctx_key_handler) ==
             g_ctx_val_handler) {
    atomic_fetch_add(&probe->handler_value_seen, 1);
  }
}

// Sets keys b, c, d and e, replaces a and c, and sets an unset key to NULL
// (unless skipped), then calls next. It is the sub-router middleware of the
// context test.
static void _ctx_sub_mw(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx,
                        chttpsvr_next_fn next) {
  (void)ctx;
  if (!_ctx_skip(req)) {
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

// Returns the context value of key as text, or "(null)", for _ctx_report.
static const char *_ctx_text(const chttpsvr_req *req, const void *key) {
  const char *v = (const char *)chttpsvr_req_context(req, key);
  return v ? v : "(null)";
}

// Writes kind and every context value the request carries, then sets the key
// that the global middleware looks for after next().
static void _ctx_report(chttpsvr_req *req, chttpsvr_resp *resp,
                        const char *kind) {
  chttpsvr_resp_printf(
      resp, "%s a=%s b=%s c=%s d=%s e=%s unset=%s", kind,
      _ctx_text(req, &g_ctx_key_a), _ctx_text(req, &g_ctx_key_b),
      _ctx_text(req, &g_ctx_key_c), _ctx_text(req, &g_ctx_key_d),
      _ctx_text(req, &g_ctx_key_e), _ctx_text(req, &g_ctx_key_unset));
  chttpsvr_req_set_context(req, &g_ctx_key_handler, g_ctx_val_handler);
}

// Reports the context of a buffered request, see _ctx_report.
static void _ctx_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  _ctx_report(req, resp, "buffered");
}

// Drains the body and reports the context of a streaming request, see
// _ctx_report.
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
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
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
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// Makes _ctx_fail_realloc fail on the calling thread only.
static _Thread_local bool g_ctx_fail_realloc = false;
// A realloc that fails while the calling thread sets g_ctx_fail_realloc, so a
// middleware can make exactly one growth of the context storage fail.
static void *_ctx_fail_realloc(void *p, size_t n) {
  if (g_ctx_fail_realloc) return NULL;
  return realloc(p, n);
}

// What _ctx_oom_mw saw, for a_failed_allocation_keeps_the_earlier_values.
static _Atomic int g_ctx_oom_results[6];

// Sets key a, makes the growth for key b fail, checks that a is kept and b is
// absent, sets b again, and checks the NULL key guards, storing each result in
// g_ctx_oom_results. Then it calls next.
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
  static ccol_memmgmt_procs mp = {malloc, free, calloc, _ctx_fail_realloc};
  for (int i = 0; i < 6; i++) atomic_store(&g_ctx_oom_results[i], -100);
  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_use(srv, _ctx_oom_mw, NULL), (int)ccol_success);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/ctx-oom",
                                            _ctx_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
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

// Sets the status that the query parameter s names (400 without it), for the
// reason phrase test and the busy port test.
static void _reason_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  const char *s = NULL;
  if (chttpsvr_req_query_one(req, "s", &s) != ccol_success) {
    chttpsvr_resp_set_status(resp, 400);
    return;
  }
  chttpsvr_resp_set_status(resp, atoi(s));
  chttpsvr_resp_write_str(resp, "x");
}

// Every status line carries the reason phrase of the IANA registry, and a code
// the registry doesn't list gets "Unknown".
TEST(status_line, every_code_carries_the_reason_phrase_of_the_registry) {
  static const struct {
    int code;
    const char *phrase;
  } table[] = {
      {200, "OK"},
      {201, "Created"},
      {202, "Accepted"},
      {203, "Non-Authoritative Information"},
      {206, "Partial Content"},
      {207, "Multi-Status"},
      {208, "Already Reported"},
      {226, "IM Used"},
      {300, "Multiple Choices"},
      {301, "Moved Permanently"},
      {302, "Found"},
      {303, "See Other"},
      {305, "Use Proxy"},
      {307, "Temporary Redirect"},
      {308, "Permanent Redirect"},
      {400, "Bad Request"},
      {402, "Payment Required"},
      {406, "Not Acceptable"},
      {409, "Conflict"},
      {410, "Gone"},
      {415, "Unsupported Media Type"},
      {417, "Expectation Failed"},
      {422, "Unprocessable Entity"},
      {423, "Locked"},
      {424, "Failed Dependency"},
      {425, "Too Early"},
      {426, "Upgrade Required"},
      {451, "Unavailable For Legal Reasons"},
      {500, "Internal Server Error"},
      {502, "Bad Gateway"},
      {504, "Gateway Timeout"},
      {506, "Variant Also Negotiates"},
      {507, "Insufficient Storage"},
      {508, "Loop Detected"},
      {510, "Not Extended"},
      {511, "Network Authentication Required"},
      {299, "Unknown"},
      {999, "Unknown"},
  };
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/reason",
                                            _reason_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  REQUIRE_EQ((int)chttpsvr_bound_port(srv, &port), (int)ccol_success);
  size_t wrong = 0;
  for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
    char path[32];
    snprintf(path, sizeof(path), "/reason?s=%d", table[i].code);
    char buf[1024] = {0};
    _raw_request_on_port(port, "GET", path, NULL, buf, sizeof(buf));
    char want[96];
    snprintf(want, sizeof(want), "HTTP/1.1 %d %s\r\n", table[i].code,
             table[i].phrase);
    if (strncmp(buf, want, strlen(want)) != 0) {
      fprintf(stderr, "status %d: got \"%.60s\", want \"%s\"\n", table[i].code,
              buf, want);
      wrong++;
    }
  }
  chttpsvr_stop(srv);
  REQUIRE_EQ(wrong, (size_t)0);
}

// 3000 bytes of 'p', far past the 256-byte buffer that chttpsvr_resp_printf
// formats into first.
#define LONG_PRINTF_FILL 3000
// The fill text of the long printf tests and the result of the long call.
static char g_long_printf_fill[LONG_PRINTF_FILL + 1];
static _Atomic int g_long_printf_rv = -100;

// Formats the fill text into the body with chttpsvr_resp_printf, stores the
// result in g_long_printf_rv and adds "end", for
// a_body_longer_than_the_stack_buffer_arrives_whole.
static void _long_printf_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                 void *ctx) {
  (void)req;
  (void)ctx;
  atomic_store(
      &g_long_printf_rv,
      (int)chttpsvr_resp_printf(resp, "<%s|%d>", g_long_printf_fill, 4242));
  chttpsvr_resp_write_str(resp, "end");
}

// When the longer buffer can't be allocated the call returns
// ccol_not_enough_memory and adds nothing, so the body holds only what the
// handler wrote after it.
TEST(resp_printf, a_body_longer_than_the_stack_buffer_arrives_whole) {
  memset(g_long_printf_fill, 'p', LONG_PRINTF_FILL);
  g_long_printf_fill[LONG_PRINTF_FILL] = '\0';
  ccol_memmgmt_procs mp = {_fail_at_size_malloc, _fail_at_size_free,
                           _fail_at_size_calloc, _fail_at_size_realloc};
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create_mp(&mp, g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  REQUIRE_EQ((int)chttpsvr_register_handler(srv, CHTTP_GET, "/long-printf",
                                            _long_printf_handler, NULL),
             (int)ccol_success);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  uint16_t port = 0;
  REQUIRE_EQ((int)chttpsvr_bound_port(srv, &port), (int)ccol_success);

  static char buf[8192];
  memset(buf, 0, sizeof(buf));
  int status =
      _raw_request_on_port(port, "GET", "/long-printf", NULL, buf, sizeof(buf));
  int rv_whole = atomic_load(&g_long_printf_rv);
  char *body = strstr(buf, "\r\n\r\n");
  bool whole = false;
  if (body) {
    body += 4;
    whole = strlen(body) == (size_t)LONG_PRINTF_FILL + 7 + 3 &&
            body[0] == '<' && body[LONG_PRINTF_FILL + 1] == '|' &&
            strspn(body + 1, "p") == (size_t)LONG_PRINTF_FILL &&
            strcmp(body + LONG_PRINTF_FILL + 1, "|4242>end") == 0;
  }

  atomic_store(&g_long_printf_rv, -100);
  memset(buf, 0, sizeof(buf));
  g_fail_alloc_size = LONG_PRINTF_FILL + 7 + 1;
  int status_oom =
      _raw_request_on_port(port, "GET", "/long-printf", NULL, buf, sizeof(buf));
  g_fail_alloc_size = 0;
  int rv_oom = atomic_load(&g_long_printf_rv);
  char *body_oom = strstr(buf, "\r\n\r\n");
  chttpsvr_stop(srv);

  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(rv_whole, (int)ccol_success);
  REQUIRE_TRUE(whole);
  REQUIRE_EQ(status_oom, 200);
  REQUIRE_EQ(rv_oom, (int)ccol_not_enough_memory);
  REQUIRE_TRUE(body_oom != NULL);
  REQUIRE_STREQ(body_oom + 4, "end");
}

// Holds an IPv6-only listener on an ephemeral port whose IPv4 twin is free, so
// a dual-stack listener can take the port once the holder closes; it tries
// again when the IPv4 port is taken. The busy port test uses it. Returns the
// descriptor with the port in *port_out, or -1 with *why naming what the host
// lacks.
static int _hold_v6only_port(uint16_t *port_out, const char **why) {
  for (int attempt = 0; attempt < 20; attempt++) {
    int holder = socket(AF_INET6, SOCK_STREAM, 0);
    if (holder < 0) {
      *why = "no IPv6 sockets on this host";
      return -1;
    }
    int zero = 0, one = 1;
    if (setsockopt(holder, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero)) !=
        0) {
      close(holder);
      *why = "no dual-stack IPv6 sockets on this host";
      return -1;
    }
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_addr = in6addr_any;
    socklen_t alen = sizeof(a6);
    if (setsockopt(holder, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one)) != 0 ||
        bind(holder, (struct sockaddr *)&a6, sizeof(a6)) != 0 ||
        listen(holder, 1) != 0 ||
        getsockname(holder, (struct sockaddr *)&a6, &alen) != 0) {
      close(holder);
      *why = "can't hold an IPv6-only port on this host";
      return -1;
    }
    int probe = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    a4.sin_port = a6.sin6_port;
    bool v4_free =
        probe >= 0 &&
        setsockopt(probe, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) == 0 &&
        bind(probe, (struct sockaddr *)&a4, sizeof(a4)) == 0;
    if (probe >= 0) close(probe);
    if (v4_free) {
      *port_out = ntohs(a6.sin6_port);
      return holder;
    }
    close(holder);
  }
  *why = "no port whose IPv4 twin is free after 20 tries";
  return -1;
}

// A start on the default host whose port another socket holds on [::] fails
// with EADDRINUSE and doesn't fall back to IPv4. The holder is IPv6-only, so a
// fallback would bind and hide the failure. OpenBSD has no dual-stack sockets
// and skips.
TEST(listen_collision, a_busy_port_on_the_default_host_fails_with_eaddrinuse) {
  uint16_t port = 0;
  const char *why = NULL;
  int holder = _hold_v6only_port(&port, &why);
  if (holder < 0) {
    fprintf(stderr, "SKIP: %s\n", why);
    return;
  }
  chttpsvr srv = chttpsvr_create(g_test_logger, NULL);
  if (srv == CHTTPSVR_NULL) close(holder);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/busy", _reason_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = NULL;
  cfg.port = port;
  ccol_retval busy_rv = chttpsvr_start(srv, &cfg);
  int busy_errno = chttpsvr__last_listen_errno_for_tests();
  bool running = true;
  chttpsvr_is_running(srv, &running);
  if (busy_rv == ccol_success) chttpsvr_stop(srv);
  close(holder);
  ccol_retval free_rv = chttpsvr_start(srv, &cfg);
  char buf[512] = {0};
  int status = free_rv == ccol_success
                   ? _raw_request_on_port(port, "GET", "/busy?s=204", NULL, buf,
                                          sizeof(buf))
                   : -1;
  chttpsvr_destroy(srv);
  REQUIRE_EQ((int)busy_rv, (int)ccol_unexpected_failure);
  REQUIRE_EQ(busy_errno, EADDRINUSE);
  REQUIRE_FALSE(running);
  REQUIRE_EQ((int)free_rv, (int)ccol_success);
  REQUIRE_EQ(status, 204);
}

// State of the sweep allocator below. It counts the blocks it has handed out
// and not taken back in g_sw_live, and fails the one call that brings
// g_sw_countdown from 0 to -1 (counted in g_sw_failed); below 0 it fails
// nothing. While g_sw_owner_only is set, only a thread with t_sw_owner set
// counts down, so the threads that a start leaves running can't take the
// failure after the call has returned.
static _Atomic long g_sw_countdown = -1;
static _Atomic long g_sw_live = 0;
static _Atomic long g_sw_failed = 0;
static _Atomic bool g_sw_owner_only = false;
static __thread bool t_sw_owner = false;

// Tells whether this allocation is the one to fail, counting it down.
static bool _sw_fail_now(void) {
  if (atomic_load(&g_sw_countdown) < 0) return false;
  if (atomic_load(&g_sw_owner_only) && !t_sw_owner) return false;
  if (atomic_fetch_sub(&g_sw_countdown, 1) != 0) return false;
  atomic_fetch_add(&g_sw_failed, 1);
  return true;
}
// The allocator of the allocation sweeps: each allocating call fails when
// _sw_fail_now says so and keeps g_sw_live up to date otherwise.
static void *_sw_malloc(size_t n) {
  if (_sw_fail_now()) return NULL;
  void *p = malloc(n);
  if (p) atomic_fetch_add(&g_sw_live, 1);
  return p;
}
static void *_sw_calloc(size_t n, size_t s) {
  if (_sw_fail_now()) return NULL;
  void *p = calloc(n, s);
  if (p) atomic_fetch_add(&g_sw_live, 1);
  return p;
}
static void *_sw_realloc(void *p, size_t s) {
  if (_sw_fail_now()) return NULL;
  void *np = realloc(p, s);
  if (np && !p) atomic_fetch_add(&g_sw_live, 1);
  return np;
}
// The free of the sweep allocator; it keeps g_sw_live up to date.
static void _sw_free(void *p) {
  if (p) atomic_fetch_sub(&g_sw_live, 1);
  free(p);
}

// Waits up to 10 s for g_sw_live to reach 0 and returns what it reads last. The
// sweeps call it after a destroy, because the last frees of a server that was
// started run on the reactor thread once its registrations are reclaimed.
static long _sw_wait_for_no_live_blocks(void) {
  struct timespec start, now;
  clock_gettime(CLOCK_MONOTONIC, &start);
  long live = atomic_load(&g_sw_live);
  while (live != 0) {
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec - start.tv_sec >= 10) break;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
    live = atomic_load(&g_sw_live);
  }
  return live;
}

// Writes "served", the known answer of the allocation sweeps.
static void _sw_plain_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                              void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "served");
}

// Fails each allocation of server setup in turn (create, a route with two
// params, a sub-router with a route, start). Every failure is reported the
// documented way, a failed start leaves the server able to start later, and the
// destroy gives back every block. The sweep ends at the first index that no
// setup allocation reaches, and that server serves a request.
TEST(oom_sweep, every_failed_allocation_of_a_server_setup_is_reported) {
  ccol_memmgmt_procs mp = {_sw_malloc, _sw_free, _sw_calloc, _sw_realloc};
  t_sw_owner = true;
  atomic_store(&g_sw_owner_only, true);
  size_t create_fails = 0, route_fails = 0, sub_fails = 0, start_fails = 0;
  size_t bad = 0, leaked = 0, start_later_bad = 0;
  int final_status = -1;
  bool finished = false;
  for (long n = 0; n < 4000 && !finished; n++) {
    atomic_store(&g_sw_failed, 0);
    atomic_store(&g_sw_live, 0);
    atomic_store(&g_sw_countdown, n);
    char *err = NULL;
    chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, &err);
    if (srv == CHTTPSVR_NULL) {
      atomic_store(&g_sw_countdown, -1);
      if (!err || !*err) bad++;
      create_fails++;
      if (atomic_load(&g_sw_live) != 0) leaked++;
      continue;
    }
    bool stopped = false;
    ccol_retval rv = chttpsvr_register_handler(
        srv, CHTTP_GET, "/items/{id}/parts/{part}", _sw_plain_handler, NULL);
    if (rv != ccol_success) {
      stopped = true;
      route_fails++;
      if (rv != ccol_not_enough_memory) bad++;
    }
    chttpsvr_router *sub = NULL;
    if (!stopped) {
      sub = chttpsvr_subrouter(srv, "/api/v1");
      if (!sub) {
        stopped = true;
        sub_fails++;
      }
    }
    if (!stopped) {
      rv = chttpsvr_router_on(sub, CHTTP_GET, "/things/{x}", _sw_plain_handler,
                              NULL);
      if (rv != ccol_success) {
        stopped = true;
        route_fails++;
        if (rv != ccol_not_enough_memory) bad++;
      }
    }
    chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = 0;
    cfg.worker_thread_count = 1;
    if (!stopped) {
      rv = chttpsvr_start(srv, &cfg);
      if (rv != ccol_success) {
        start_fails++;
        if (rv != ccol_not_enough_memory) bad++;
        atomic_store(&g_sw_countdown, -1);
        bool running = true;
        chttpsvr_is_running(srv, &running);
        if (running || chttpsvr_start(srv, &cfg) != ccol_success)
          start_later_bad++;
        stopped = true;
      }
    }
    atomic_store(&g_sw_countdown, -1);
    if (atomic_load(&g_sw_failed) == 0) {
      finished = true;
      uint16_t port = 0;
      char buf[1024] = {0};
      if (chttpsvr_bound_port(srv, &port) == ccol_success)
        final_status = _raw_request_on_port(port, "GET", "/api/v1/things/3",
                                            NULL, buf, sizeof(buf));
    }
    chttpsvr_destroy(srv);
    long live = _sw_wait_for_no_live_blocks();
    if (live != 0) {
      fprintf(stderr, "setup sweep: %ld blocks left after index %ld\n", live,
              n);
      leaked++;
    }
  }
  atomic_store(&g_sw_owner_only, false);
  t_sw_owner = false;
  REQUIRE_TRUE(finished);
  REQUIRE_EQ(bad, (size_t)0);
  REQUIRE_EQ(leaked, (size_t)0);
  REQUIRE_EQ(start_later_bad, (size_t)0);
  REQUIRE_GT(create_fails, (size_t)0);
  REQUIRE_GT(route_fails, (size_t)0);
  REQUIRE_GT(sub_fails, (size_t)0);
  REQUIRE_GT(start_fails, (size_t)0);
  REQUIRE_EQ(final_status, 200);
}

// The length of the long header value that the request sweep sets.
#define RSW_LONG_HEADER 5000
// What the handler of the request sweep saw, for the test to compare with the
// response that reached the client.
static char g_rsw_long_value[RSW_LONG_HEADER + 1];
static _Atomic int g_rsw_ran = 0;
static _Atomic int g_rsw_params_ok = -100;
static _Atomic int g_rsw_query_ok = -100;
static _Atomic int g_rsw_query_one_rv = -100;
static _Atomic int g_rsw_set_rv = -100;
static _Atomic int g_rsw_replace_rv = -100;
static _Atomic int g_rsw_add_rv = -100;
static _Atomic int g_rsw_long_rv = -100;
static _Atomic int g_rsw_printf_rv = -100;

// Tells whether rv is ccol_success or ccol_not_enough_memory, the only results
// a call may give under the sweep.
static bool _rsw_rv_allowed(int rv) {
  return rv == (int)ccol_success || rv == (int)ccol_not_enough_memory;
}

// Reads both path params and four query params (a single value, a repeated key,
// a key with no '=', a value longer than 512 bytes), sets a header and replaces
// it, adds another, sets one long enough to push the head past its stack buffer
// and formats a long body. Every result goes to the g_rsw_* globals, and it
// answers 500 when anything ran out of memory. It is the handler of
// every_failed_allocation_of_a_request_ends_in_a_whole_answer.
static void _rsw_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  atomic_fetch_add(&g_rsw_ran, 1);
  const char *id = chttpsvr_req_param(req, "id");
  const char *part = chttpsvr_req_param(req, "part");
  atomic_store(&g_rsw_params_ok,
               id && part && strcmp(id, "7") == 0 && strcmp(part, "9") == 0);
  const char *k = NULL;
  ccol_retval q1 = chttpsvr_req_query_one(req, "k", &k);
  atomic_store(&g_rsw_query_one_rv, (int)q1);
  bool query_ok = q1 == ccol_success && k && strcmp(k, "v1") == 0;
  size_t count = 0;
  const char **m = chttpsvr_req_query(req, "m", &count);
  query_ok = query_ok && m && count == 2 && strcmp(m[0], "a") == 0 &&
             strcmp(m[1], "b") == 0;
  const char **e = chttpsvr_req_query(req, "e", &count);
  query_ok = query_ok && e && count == 1 && e[0][0] == '\0';
  const char **l = chttpsvr_req_query(req, "long", &count);
  query_ok = query_ok && l && count == 1 && strlen(l[0]) == 600;
  atomic_store(&g_rsw_query_ok,
               query_ok ? 1
               : (q1 == ccol_not_enough_memory || chttpsvr_req_query_oom(req))
                   ? 2
                   : 0);
  ccol_retval set_rv = chttpsvr_resp_set_header(resp, "X-One", "1");
  ccol_retval replace_rv = chttpsvr_resp_set_header(resp, "X-One", "uno");
  ccol_retval add_rv = chttpsvr_resp_add_header(resp, "X-Two", "2");
  ccol_retval long_rv =
      chttpsvr_resp_set_header(resp, "X-Long", g_rsw_long_value);
  ccol_retval printf_rv = chttpsvr_resp_printf(resp, "%s", g_long_printf_fill);
  atomic_store(&g_rsw_set_rv, (int)set_rv);
  atomic_store(&g_rsw_replace_rv, (int)replace_rv);
  atomic_store(&g_rsw_add_rv, (int)add_rv);
  atomic_store(&g_rsw_long_rv, (int)long_rv);
  atomic_store(&g_rsw_printf_rv, (int)printf_rv);
  if (!query_ok || set_rv != ccol_success || replace_rv != ccol_success ||
      add_rv != ccol_success || long_rv != ccol_success ||
      printf_rv != ccol_success)
    chttpsvr_resp_set_status(resp, 500);
}

// Tells whether the handler's results and the response in buf agree: every call
// gave ccol_success or ccol_not_enough_memory, and a header is on the wire
// exactly when the call that set it worked.
static bool _rsw_response_agrees(const char *buf) {
  int set_rv = atomic_load(&g_rsw_set_rv);
  int replace_rv = atomic_load(&g_rsw_replace_rv);
  int add_rv = atomic_load(&g_rsw_add_rv);
  int long_rv = atomic_load(&g_rsw_long_rv);
  if (!_rsw_rv_allowed(set_rv) || !_rsw_rv_allowed(replace_rv) ||
      !_rsw_rv_allowed(add_rv) || !_rsw_rv_allowed(long_rv) ||
      !_rsw_rv_allowed(atomic_load(&g_rsw_printf_rv)) ||
      !_rsw_rv_allowed(atomic_load(&g_rsw_query_one_rv)) ||
      atomic_load(&g_rsw_query_ok) == 0)
    return false;
  bool want_uno = replace_rv == (int)ccol_success;
  bool want_one = set_rv == (int)ccol_success && !want_uno;
  return (strstr(buf, "X-One:uno\r\n") != NULL) == want_uno &&
         (strstr(buf, "X-One:1\r\n") != NULL) == want_one &&
         (strstr(buf, "X-Two:2\r\n") != NULL) ==
             (add_rv == (int)ccol_success) &&
         (strstr(buf, "X-Long:") != NULL) == (long_rv == (int)ccol_success);
}

// Fails each allocation of one request in turn, from the accept to the close.
// The query and header calls report ccol_not_enough_memory and change nothing,
// the client gets a 500, the whole response, or a close when the connection or
// the head can't be allocated, the server serves the next request, and the
// destroy gives back every block.
TEST(oom_sweep, every_failed_allocation_of_a_request_ends_in_a_whole_answer) {
  memset(g_long_printf_fill, 'p', LONG_PRINTF_FILL);
  g_long_printf_fill[LONG_PRINTF_FILL] = '\0';
  memset(g_rsw_long_value, 'h', RSW_LONG_HEADER);
  g_rsw_long_value[RSW_LONG_HEADER] = '\0';
  ccol_memmgmt_procs mp = {_sw_malloc, _sw_free, _sw_calloc, _sw_realloc};
  atomic_store(&g_sw_countdown, -1);
  atomic_store(&g_sw_live, 0);
  chttpsvr srv = chttpsvr_create_mp(&mp, g_test_logger, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_router *sub = chttpsvr_subrouter(srv, "/api");
  bool setup_ok =
      sub &&
      chttpsvr_router_on(sub, CHTTP_GET, "/items/{id}/parts/{part}",
                         _rsw_handler, NULL) == ccol_success &&
      chttpsvr_register_handler(srv, CHTTP_GET, "/plain", _sw_plain_handler,
                                NULL) == ccol_success;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 0;
  cfg.worker_thread_count = 1;
  uint16_t port = 0;
  setup_ok = setup_ok && chttpsvr_start(srv, &cfg) == ccol_success &&
             chttpsvr_bound_port(srv, &port) == ccol_success;
  if (!setup_ok) chttpsvr_destroy(srv);
  REQUIRE_TRUE(setup_ok);

  static char target[1024];
  {
    char long_val[601];
    memset(long_val, 'L', 600);
    long_val[600] = '\0';
    snprintf(target, sizeof(target),
             "/api/items/7/parts/9?k=v1&m=a&m=b&e&long=%s", long_val);
  }
  static char extra[512];
  {
    size_t off = 0;
    for (int i = 0; i < 12; i++)
      off += (size_t)snprintf(extra + off, sizeof(extra) - off,
                              "X-Req-%d: %d\r\n", i, i);
  }
  static char buf[16384];
  size_t server_500 = 0, handler_500 = 0, dropped = 0, closed = 0, other = 0;
  size_t disagree = 0, next_failed = 0;
  bool finished = false, whole = false;
  for (long n = 0; n < 4000 && !finished; n++) {
    atomic_store(&g_rsw_ran, 0);
    atomic_store(&g_sw_failed, 0);
    memset(buf, 0, sizeof(buf));
    atomic_store(&g_sw_countdown, n);
    int status =
        _raw_request_on_port(port, "GET", target, extra, buf, sizeof(buf));
    atomic_store(&g_sw_countdown, -1);
    bool failed = atomic_load(&g_sw_failed) != 0;
    bool ran = atomic_load(&g_rsw_ran) != 0;
    if (!failed) {
      finished = true;
      char *body = strstr(buf, "\r\n\r\n");
      whole = status == 200 && ran && body &&
              strcmp(body + 4, g_long_printf_fill) == 0 &&
              strstr(buf, "X-One:uno\r\n") && strstr(buf, "X-Two:2\r\n") &&
              strstr(buf, "X-Long:hhhh") &&
              atomic_load(&g_rsw_params_ok) == 1 &&
              atomic_load(&g_rsw_query_ok) == 1;
      break;
    }
    if ((status == 200 || status == 500) && ran) {
      if (status == 500) handler_500++;
      if (!_rsw_response_agrees(buf)) disagree++;
    } else if (status == 500) {
      server_500++;
    } else if (status == -1 && buf[0] == '\0' && !ran) {
      dropped++;
    } else if (status == -1 && buf[0] == '\0') {
      closed++;
    } else {
      fprintf(stderr, "request sweep: index %ld gave status %d (\"%.40s\")\n",
              n, status, buf);
      other++;
    }
    char pbuf[512] = {0};
    if (_raw_request_on_port(port, "GET", "/plain", NULL, pbuf, sizeof(pbuf)) !=
        200)
      next_failed++;
  }
  chttpsvr_stop(srv);
  chttpsvr_destroy(srv);
  long live = _sw_wait_for_no_live_blocks();
  REQUIRE_TRUE(finished);
  REQUIRE_TRUE(whole);
  REQUIRE_EQ(other, (size_t)0);
  REQUIRE_LE(dropped, (size_t)1);
  REQUIRE_LE(closed, (size_t)1);
  REQUIRE_EQ(disagree, (size_t)0);
  REQUIRE_EQ(next_failed, (size_t)0);
  REQUIRE_GT(server_500, (size_t)0);
  REQUIRE_GT(handler_500, (size_t)0);
  REQUIRE_EQ(live, 0L);
}
