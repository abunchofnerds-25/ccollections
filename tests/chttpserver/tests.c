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

#include <arpa/inet.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
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

static clog g_test_logger = NULL;
static chttpsvr g_srv = NULL;

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
   clamp it to 500 rather than forwarding a garbage uintptr_t to facil.io. */
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
static chttpsvr g_srv2 = NULL;

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
static chttpsvr g_bounded_srv = NULL;

/* Dedicated server with a small max_body_size (64 bytes) for boundary tests
   of the 413/PAYLOAD_TOO_LARGE enforcement (buffered and streaming).
   Listening on TEST_PORT+3. */
static chttpsvr g_small_body_srv = NULL;
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
    g_bounded_srv = NULL;
  }
  if (g_small_body_srv) {
    __chttpsvr_destroy(g_small_body_srv);
    g_small_body_srv = NULL;
  }
  if (g_srv) {
    __chttpsvr_destroy(g_srv);
    g_srv = NULL;
  }
  if (g_srv2) {
    __chttpsvr_destroy(g_srv2);
    g_srv2 = NULL;
  }
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = NULL;
  }
}

__attribute__((constructor)) static void _setup(void) {
  char *err = NULL;

  /* Logger for the engine and both server instances. */
  g_test_logger = clog_open_fd(2, CLOG_INFO);
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
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/json-zero-len",
                            _json_zero_len_handler, &g_json_zero_len_result);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/query-empty-key",
                            _empty_query_key_handler, NULL);
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
  REQUIRE_STREQ(resp->body, "ping");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, post_echo_body_empty) {
  chttpcli_response *resp = _post("/echo-body", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_params) {
  chttpcli_response *resp = _get("/params/42/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "id=42 sub=hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_params_url_encoded) {
  chttpcli_response *resp = _get("/params/hello%20world/foo%2Fbar");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "id=hello+world sub=foo");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_single_value) {
  chttpcli_response *resp = _get("/query?q=hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_multi_value) {
  chttpcli_response *resp = _get("/query?q=one&q=two&q=three");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "one,two,three");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_no_value) {
  chttpcli_response *resp = _get("/query");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "no_q");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, request_header_read) {
  chttpcli_response *resp =
      _get_with_header("/header", "x-test-header", "myvalue");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "myvalue");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, missing_request_header) {
  chttpcli_response *resp = _get("/header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "api_root");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_with_param) {
  chttpcli_response *resp = _get("/api/v1/items/99");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "item:99");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, subrouter_percent_encoded_prefix_root_matches) {
  /* Same as above but for the prefix-only path (no trailing route
     segments); /api/v1/ vs /%61pi/v1/. */
  chttpcli_response *resp = _get("/%61pi/v1/");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "blocked");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_echo) {
  const char *payload = "streaming payload data";
  chttpcli_response *resp = _post("/stream-echo", payload, "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
   * requests independently, demonstrating multiple-server support. */
  REQUIRE_TRUE(g_srv2 != NULL);

  chttpsvr_register_handler(g_srv2, CHTTP_GET, "/ping-srv2", _hello_handler,
                            NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 1;
  ccol_retval_t rv = chttpsvr_start(g_srv2, &cfg);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  /* No readiness poll: http_listen binds the socket synchronously. */

  /* Request to srv2 must succeed. */
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/ping-srv2", TEST_PORT + 1);
  chttpcli_response *resp = NULL;
  chttp_get(url, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);

  /* Verify g_srv (on TEST_PORT) is unaffected. */
  resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);

  chttpsvr_stop(g_srv2);
}

TEST(chttpserver, query_one_single) {
  /* chttpsvr_req_query_one returns the value when exactly one pair exists. */
  chttpcli_response *resp = _get("/query-one?q=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "multi_value");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_one_missing) {
  /* chttpsvr_req_query_one must report ccol_key_not_found when key is absent.
   */
  chttpcli_response *resp = _get("/query-one");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "not_found");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_plus_decoded_to_space) {
  /* '+' in a query value must decode to a space character. */
  chttpcli_response *resp = _get("/query?q=hello+world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, query_percent_decoded) {
  /* Percent-encoded characters in a query value must be decoded. */
  chttpcli_response *resp = _get("/query?q=hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_present) {
  /* chttpsvr_req_header works on streaming routes via the pre-extracted
     header array, not the FIOBJ hash used in buffered mode. */
  chttpcli_response *resp =
      _get_with_header("/stream-header", "x-stream-test", "stream-val");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "stream-val");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_header_absent) {
  /* Absent header on a streaming route returns NULL from chttpsvr_req_header.
   */
  chttpcli_response *resp = _get("/stream-header");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "(none)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, stream_handler_registration_accepted) {
  /* chttpsvr_register_streaming_handler must accept a valid registration on
     an unstarted server.  The server owns its ctpool, so no pool is passed by
     the caller. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);

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
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, path_url_decoded) {
  /* chttpsvr_req_path() must return the URL-decoded path, not the raw
     percent-encoded form received from the client. */
  chttpcli_response *resp = _get("/echo-path/hello%20world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "/echo-path/hello world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query) {
  /* Calling chttpsvr_req_query for two different keys in one request must
     return correct values for both, even though the scratch _qresult array
     is shared and reused between calls. */
  chttpcli_response *resp = _get("/multi-query?a=hello&b=world");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "a=hello b=world");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, multi_key_query_missing) {
  /* Absent keys must yield the nil sentinel regardless of call order. */
  chttpcli_response *resp = _get("/multi-query?a=only");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  chttpsvr srv = create_chttpsvr_mp(&mp, g_test_logger, &err);
  REQUIRE_TRUE(srv != NULL);
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
  __chttpsvr_destroy(srv);
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
  REQUIRE_STREQ(resp->body, expected);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, bounded_pool_full_returns_503) {
  /* When the server's ctpool is at capacity, ctpool_try_submit returns
   * ccol_container_full and the server must respond 503.
   *
   * g_bounded_srv has 1 worker thread and queue_capacity=1 (total capacity 2:
   * 1 active + 1 queued).  We fire two background HTTP requests that block
   * inside _bounded_blk_handler to saturate both slots, then send a third
   * request which must receive 503. */

  /* Reset state from any previous run of this test. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  g_blk_count = 0;
  pthread_mutex_unlock(&g_blk_mtx);

  /* Request 1: fills the active worker slot. */
  pthread_t t1;
  REQUIRE_EQ(pthread_create(&t1, NULL, _send_bounded_req, NULL), 0);

  /* Spin until the worker is actively running the handler (blocking).
   * Bail after 5 s so a stalled environment fails rather than hanging. */
  {
    struct timespec nap = {0, 1000000L}; /* 1 ms */
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 5;
    while (g_blk_count < 1) {
      nanosleep(&nap, NULL);
      struct timespec now;
      clock_gettime(CLOCK_MONOTONIC, &now);
      REQUIRE_TRUE(
          now.tv_sec < deadline.tv_sec ||
          (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
    }
  }

  /* Request 2: fills the single queue slot.  Worker is blocked, so this
   * request stays queued until g_blk_go is set. */
  pthread_t t2;
  REQUIRE_EQ(pthread_create(&t2, NULL, _send_bounded_req, NULL), 0);

  /* Give Request 2 time to traverse the network stack and enter the queue.
   * Worker is blocked so Request 2 cannot start executing; 50 ms is generous
   * for a local loopback round-trip. */
  struct timespec queue_wait = {0, 50000000L}; /* 50 ms */
  nanosleep(&queue_wait, NULL);

  /* Pool is now full (1 active + 1 queued).  Third request must get 503. */
  char burl[128];
  snprintf(burl, sizeof(burl), "http://127.0.0.1:%d/bounded-503",
           TEST_PORT + 2);
  chttpcli_response *resp = NULL;
  chttp_get(burl, &resp);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 503);
  chttpclient_resp_free(resp);

  /* Release blocking handlers and wait for background threads to finish. */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = true;
  pthread_cond_broadcast(&g_blk_cv);
  pthread_mutex_unlock(&g_blk_mtx);

  pthread_join(t1, NULL);
  pthread_join(t2, NULL);

  /* Reset g_blk_go for potential reruns (e.g. _teardown safety broadcast). */
  pthread_mutex_lock(&g_blk_mtx);
  g_blk_go = false;
  pthread_mutex_unlock(&g_blk_mtx);
}

TEST(chttpserver, stop_on_unstarted_server_is_safe) {
  /* chttpsvr_stop on a server that was never started must be a safe no-op
   * that can be called multiple times without crashing.  g_srv2 is not
   * started by _setup, so this exercises the started==false branch. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
   sending the same header name twice to exercise the FIOBJ_T_ARRAY path. */
static int _raw_request(const char *method, const char *path,
                        const char *extra_headers, char *buf, size_t buf_sz) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }

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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }

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
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    return -1;
  }

  size_t sent = 0;
  while (sent < body_len) {
    size_t n = body_len - sent < chunk_len ? body_len - sent : chunk_len;
    ssize_t w = write(fd, body + sent, n);
    if (w < 0) {
      close(fd);
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
static int _raw_request_abort_mid_body(const char *path,
                                       const char *partial_body,
                                       size_t declared_len) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  if (inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr) != 1) return -1;

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
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
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    return -1;
  }
  size_t partial_len = strlen(partial_body);
  if (partial_len && write(fd, partial_body, partial_len) < 0) {
    close(fd);
    return -1;
  }
  close(fd); /* abort: never send the rest of the declared body */
  return 0;
}

/* Reads exactly one HTTP/1.1 response (headers + Content-Length body) off
   an already-connected socket, leaving the connection open for a
   subsequent request; used to test keep-alive across two requests on one
   connection. Assumes a short, non-chunked response (true for every
   fixture handler used with this helper). Returns the status code, or -1
   on failure. */
static int _read_one_http_response(int fd, char *buf, size_t buf_sz) {
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
   With chunked encoding the function decodes the chunk framing in place so
   the caller always gets the raw body content regardless of transport encoding.
   This prevents the test assertions from being fragile with respect to
   facil.io's encoding choice. */
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
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, streaming_repeated_header) {
  /* When a client sends the same header name twice, facil.io stores both
     values as a FIOBJ_T_ARRAY.  _extract_hdr_cb must guard the
     fiobj_ary_index() result against NULL before calling fiobj_obj2cstr,
     and must correctly pick the last value (matching _find_hdr_cb's
     behaviour).  The chttpclient abstraction overwrites duplicate header
     names, so we use a raw socket to send the literal duplicate lines. */
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
  char buf[4096] = {0};
  int status = _raw_request("POST", "/no-such-route-at-all",
                            "Content-Length: 100000000\r\n", buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver, pipelined_bytes_after_rejected_route_not_misparsed) {
  /* http1_on_headers_complete's "not diverted" branch (route rejected
     synchronously, e.g. 404) used to free the primed pipelined-bytes buffer
     but never reset parser->state, unlike the adjacent OOM branch a few
     lines above it which already carried this exact fix. p->close is
     already 1 for a rejected route, but http1_consume_data's do-while loop
     only stops on stream_diverted (never set here) or p->stop (only
     http_pause sets it, never called here either) - so if a second,
     pipelined request's bytes are already sitting in the same read buffer
     behind the rejected one, the loop's next http1_parse call re-entered
     with parser->state.reserved still holding HEADER_COMPLETE|STATUS_LINE
     from the finished request, misparsing the second request's raw bytes
     as a (zero declared length, so immediately "complete") body of the
     first, already-finished http_s, causing http1_on_request to fire a
     second time on it.
     Full disclosure on this test's actual coverage: with this exact
     two-plain-GETs reproduction, that second dispatch is fully absorbed by
     HTTP_INVALID_HANDLE's guard in http.c (an already-finished h has no
     `method`/`status_str` left, so the second http_send_error/http_finish
     call is a silent no-op) - confirmed empirically, including under
     valgrind, that reverting just this fix does not change this specific
     test's outcome. The fix is still correct and worth keeping (a
     non-empty declared body length on the misparsed "request" would not
     be as harmless), but this test's real, verified value is narrower than
     its historical motivation: it locks in that pipelined bytes behind a
     rejected route never produce more than one response and the connection
     still closes cleanly, a property worth guarding regardless. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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
}

TEST(chttpserver, keep_alive_two_consecutive_streaming_requests) {
  /* Per-message ingestion state (stream_diverted, stream_body_done,
     stream_err, primed/carry buffers) is documented to reset at the top of
     every http1_on_headers_complete call, since http1pr_s is reused across
     every request on a keep-alive connection. Every other keep-alive test in
     this file pairs at most one streaming request with a buffered GET on the
     same connection; this sends two full streaming POSTs back to back on one
     connection, the scenario most likely to expose stale per-message
     streaming state (e.g. a leftover primed buffer or stream_err from
     request 1 corrupting request 2's ingestion). Each request has a
     different body so the two responses can only match if the ingestion
     state was genuinely reset in between. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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
}

TEST(chttpserver,
     streaming_handler_early_stop_of_fully_arrived_body_stays_keepalive) {
  /* /stream-read-once reads only the first 8 bytes of the body and returns
     without calling chttpsvr_req_read() again. Here the whole 64-byte body
     is written in one shot and arrives on the wire before the handler's
     single read call runs, so the worker's underlying ingestion loop (see
     http1_stream_read/http1_on_body_chunk) ends up parsing the ENTIRE
     declared body in that one internal pass regardless of how few bytes the
     handler's own buffer captured; content_length is fully consumed, so
     http1_on_request sets stream_body_done=1 before the handler even
     returns. http1_stream_release only forces Connection: close when
     stream_body_done is still false (see the paired
     ..._with_undrained_body_forces_close test below for that case); here it
     is true, so the connection must remain usable for a second, unrelated
     request. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:ccol_timed_out") != NULL);
}

TEST(chttpserver, client_disconnect_mid_body_does_not_hang_server) {
  /* A client that sends part of its body then disconnects entirely must
     not hang the worker thread or leak the connection's resources. There
     is no response to check (the client tore the connection down), so the
     test instead proves the server is still healthy afterward: a fresh,
     unrelated request must still succeed promptly. */
  int rc = _raw_request_abort_mid_body("/stream-error-report", "partial", 1000);
  REQUIRE_EQ(rc, 0);

  chttpcli_response *resp = _get("/hello");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
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
  for (int i = 0; i < N_CONCURRENT_STREAMS; i++) {
    args[i].id = i;
    args[i].ok = 0;
    REQUIRE_EQ(pthread_create(&threads[i], NULL, _conc_stream_thread, &args[i]),
               0);
  }
  for (int i = 0; i < N_CONCURRENT_STREAMS; i++) {
    pthread_join(threads[i], NULL);
    REQUIRE_TRUE(args[i].ok);
  }
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
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*                     BUFFERED REPEATED-HEADER TEST                          */
/* ========================================================================== */

TEST(chttpserver, buffered_repeated_header) {
  /* When a client sends the same header name twice, facil.io stores both
     values as a FIOBJ_T_ARRAY.  The buffered-path helper _find_hdr_cb must
     handle this the same way _extract_hdr_cb does in the streaming path: pick
     the last value.  The /header route is buffered, and chttpclient would
     overwrite duplicate names, so we use a raw socket here too. */
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
/*                    NULL-HANDLER GUARD TESTS                                */
/* ========================================================================== */

TEST(chttpserver, on_null_fn_rejected) {
  /* chttpsvr_register_handler must return ccol_invalid_args when fn is NULL so
   * that a NULL handler can never be installed and later cause a crash at
   * dispatch time.  Uses g_srv2 which is created but never started. */
  REQUIRE_TRUE(g_srv2 != NULL);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, "/test-null-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_use_null_fn_rejected) {
  /* chttpsvr_router_use must return ccol_invalid_args when fn is NULL to
   * prevent a NULL middleware from being appended to the chain. */
  REQUIRE_TRUE(g_srv2 != NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "/null-mw-test");
  REQUIRE_TRUE(r != NULL);
  ccol_retval_t rv = chttpsvr_router_use(r, NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, use_null_fn_rejected) {
  /* chttpsvr_use must return ccol_invalid_args when fn is NULL, consistent
   * with chttpsvr_register_handler and chttpsvr_router_use.  Uses g_srv2 which
   * is never started so the running server is unaffected. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);
  ccol_retval_t rv =
      chttpsvr_register_handler(g_srv2, CHTTP_GET, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, on_stream_null_pattern_rejected) {
  /* chttpsvr_register_streaming_handler must reject NULL pattern at the public
   * API boundary. */
  REQUIRE_TRUE(g_srv2 != NULL);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, NULL, _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_null_pattern_rejected) {
  /* chttpsvr_router_on must also guard against NULL pattern, consistently with
   * chttpsvr_register_handler.  Uses a sub-router on g_srv2. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);
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
   * safe: the first call is a no-op (running == false) and the second is
   * identical.  Neither call should crash or invoke fio_stop(). */
  REQUIRE_TRUE(g_srv2 != NULL);
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
   * _finalize_response must clamp it to 500 rather than forwarding the value
   * to facil.io as a huge uintptr_t.  The /bad-status route exercises this. */
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
   * _match_segments strips trailing slashes from incoming paths, so they
   * would silently match the same requests as the no-trailing-slash variant
   * ; a misleading API contract.  Rejection is the only honest behaviour.
   *
   * "/"  (the root) is a special case that is always accepted.
   *
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing table. */
  REQUIRE_TRUE(g_srv2 != NULL);

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
  REQUIRE_TRUE(g_srv2 != NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, NULL);
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_no_leading_slash_rejected) {
  /* chttpsvr_subrouter must return NULL when the prefix does not start with
   * '/', since all valid HTTP paths start with '/'.
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing tables. */
  REQUIRE_TRUE(g_srv2 != NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "api/v1");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_at_start_rejected) {
  /* A prefix of "//api" starts with '/' but has consecutive slashes beginning
   * at index 0-1.  Such a prefix can never match a valid HTTP path and would
   * create a permanently unreachable router.  chttpsvr_subrouter must reject
   * it by returning NULL.
   * Uses g_srv2 (never started) to avoid polluting g_srv's routing tables. */
  REQUIRE_TRUE(g_srv2 != NULL);
  chttpsvr_router *r = chttpsvr_subrouter(g_srv2, "//api");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, subrouter_double_slash_in_middle_rejected) {
  /* A prefix with consecutive slashes in the middle (e.g. "/api//v1") is
   * equally unreachable and must also be rejected.
   * Uses g_srv2 (never started). */
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      g_srv2, CHTTP_POST, "/noop-fn", NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, router_on_stream_null_fn_rejected) {
  /* chttpsvr_router_on_stream must return ccol_invalid_args when fn is NULL,
   * consistent with chttpsvr_register_streaming_handler and chttpsvr_router_on.
   * Uses a sub-router on g_srv2 (never started). */
  REQUIRE_TRUE(g_srv2 != NULL);
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

TEST(chttpserver, req_query_one_null_val_out_with_hit_returns_success) {
  /* chttpsvr_req_query_one(req, key, NULL) must return ccol_success when the
   * key is present even though the caller passed NULL for val_out (pure
   * existence check).  The function must not crash. */
  chttpcli_response *resp = _get("/query-one-null-val-out?q=present");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  ccol_retval_t rv = chttpsvr_start(NULL, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

TEST(chttpserver, stop_null_is_noop) {
  /* chttpsvr_stop(NULL) must not crash. */
  chttpsvr_stop(NULL);
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
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_STREQ(resp->body, "ok");
  chttpclient_resp_free(resp);
  REQUIRE_EQ(g_null_hdr_results[0], (int)ccol_invalid_args); /* NULL name */
  REQUIRE_EQ(g_null_hdr_results[1], (int)ccol_invalid_args); /* NULL value */
}

TEST(chttpserver, subrouter_null_srv_rejected) {
  /* chttpsvr_subrouter must return NULL when srv is NULL to prevent a
   * dangling back-pointer in the new router from causing crashes at routing
   * time.  The NULL prefix and no-leading-slash cases are already tested;
   * this test completes the guard coverage. */
  chttpsvr_router *r = chttpsvr_subrouter(NULL, "/any");
  REQUIRE_TRUE(r == NULL);
}

TEST(chttpserver, req_param_null_name_returns_null) {
  /* chttpsvr_req_param(req, NULL) must return NULL rather than crashing.
   * The /param-null-name route is registered in _setup. */
  chttpcli_response *resp = _get("/param-null-name");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_TRUE(g_srv2 != NULL);
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
  /* chttpsvr_register_streaming_handler must validate srv != NULL before doing
   * anything. If it doesn't, the server pointer dereference will crash.
   * This is a pure API validation test (no live server needed). */
  ccol_retval_t rv = chttpsvr_register_streaming_handler(
      NULL, CHTTP_GET, "/any", _hello_handler, NULL);
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
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

/* 3. chttpsvr_req_read after EOF keeps returning 0 */

TEST(chttpserver, req_read_eof_is_idempotent) {
  /* After chttpsvr_req_read returns 0 (EOF), calling it again must continue
   * to return 0.  The /stream-read-eof-twice route is registered in _setup. */
  chttpcli_response *resp = _get("/stream-read-eof-twice");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "0 0");
  chttpclient_resp_free(resp);
}

/* 4. chttpsvr_subrouter("/") edge case */

TEST(chttpserver, subrouter_slash_prefix_matches_only_root) {
  /* A sub-router created with prefix "/" retains prefix_len = 1 (the
   * trailing-slash strip loop only runs when plen > 1).  The matcher checks
   * path[prefix_len] = path[1], which must be '/' or '\0'.  For the root
   * path "/" path[1] is '\0' so the router is entered; for "/foo" path[1]
   * is 'f' so the router is skipped entirely.  The "/" sub-router and its
   * routes are registered in _setup. */

  /* The exact path "/" must be handled by the sub-router. */
  chttpcli_response *resp_root = _get("/");
  REQUIRE_TRUE(resp_root != NULL);
  REQUIRE_EQ(resp_root->status_code, 200);
  REQUIRE_STREQ(resp_root->body, "root_subrouter");
  chttpclient_resp_free(resp_root);

  /* A path that starts with "/" but is not exactly "/" must NOT match. */
  chttpcli_response *resp_foo = _get("/nonexistent-path-xyz");
  REQUIRE_TRUE(resp_foo != NULL);
  REQUIRE_EQ(resp_foo->status_code, 404);
  chttpclient_resp_free(resp_foo);
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
  REQUIRE_TRUE(g_srv2 != NULL);

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
  ccol_retval_t rv = chttpsvr_set_engine_logger(NULL);
  REQUIRE_EQ((int)rv, (int)ccol_invalid_args);
}

/* 3. Server handle valid after stop */

TEST(chttpserver, serve_stopped_server_state_valid) {
  /* chttpsvr_stop on a server that was not started is a no-op.  The handle
   * must remain in a consistent state afterwards: route registration must
   * still work without crashing or returning an error. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
   * regardless of what the TLS config contains, and no fio_tls_new call is
   * ever made.
   *
   * A full TLS integration test (starting a TLS server and completing HTTPS
   * handshakes) requires valid certificate files.  That test belongs in a
   * dedicated binary and is tracked as a follow-up task. */
  REQUIRE_TRUE(g_srv2 != NULL);
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
 * is permitted.  TLS integration requires valid certificate files and belongs
 * in a dedicated test binary. */

/* tls_context_cleaned_before_second_serve_attempt: this regression test
 * requires real TLS certificate files (fio_tls_cert_add calls FIO_LOG_FATAL
 * when files are missing).  Coverage lives in a dedicated TLS integration
 * binary and is omitted from this suite. */

/* ============================================================ */
/*            COVERAGE-GAP FILL TESTS                           */
/* ============================================================ */

TEST(chttpserver, streaming_get_no_body_via_req_body) {
  /* A streaming GET request has no body.  Calling chttpsvr_req_body() inside
   * the handler must return NULL/0, and the handler must produce a valid
   * "(empty)" response rather than crashing or returning garbage.
   * This exercises the (body && len > 0) else branch in a streaming route. */
  REQUIRE_TRUE(g_srv != NULL);
  chttpcli_response *resp = _get("/stream-body-get-no-body");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "(empty)");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, dynamic_route_registration_while_running) {
  /* chttpsvr_register_handler must succeed on a server that is already actively
   * serving requests (the write path of the rwlock is exercised while reactor
   * threads hold read locks).  The newly registered route must be reachable
   * immediately by subsequent requests in the same test process. */
  REQUIRE_TRUE(g_srv != NULL);
  ccol_retval_t rv = chttpsvr_register_handler(
      g_srv, CHTTP_GET, "/late-dynamic-route", _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);
  chttpcli_response *resp = _get("/late-dynamic-route");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_TRUE(g_srv != NULL);
  chttpcli_response *resp = _get("/shadow-test/ping");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_TRUE(g_srv2 != NULL);
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
  REQUIRE_TRUE(g_srv != NULL);
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
  REQUIRE_TRUE(g_srv != NULL);
  chttpcli_response *resp =
      _post("/mw-overflow-live/stream-ping", "payload", "text/plain");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 500);
  chttpclient_resp_free(resp);
}

TEST(chttpserver, malformed_encoding_in_param_correct_method_returns_404) {
  /* A request with invalid percent-encoding in a {param} segment must return
   * 404 even when the method matches the registered route.  This exercises the
   * capture-mode path of _match_segments, where _decode_seg_alloc returns NULL
   * for bad encoding and the route is treated as a no-match.
   *
   * /api/v1/items/{id} is registered as GET only in _setup. */
  REQUIRE_TRUE(g_srv != NULL);
  char buf[2048] = {0};
  int status =
      _raw_request("GET", "/api/v1/items/bad%ZZvalue", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

TEST(chttpserver,
     malformed_encoding_in_param_wrong_method_returns_404_not_405) {
  /* Regression test for the dry-run asymmetry bug in _match_segments.
   *
   * Before the fix, dry-run mode (used when the route's registered method does
   * not match the request method) accepted any non-empty token for {param}
   * segments without validating percent-encoding.  A POST to the GET-only
   * /api/v1/items/{id} with a malformed parameter value would set
   * method_mismatch_seen=true and produce 405, while a GET to the same URL
   * would correctly return 404 (capture mode rejects bad encoding).
   *
   * After the fix, both modes validate encoding consistently: this request
   * must return 404, not 405. */
  REQUIRE_TRUE(g_srv != NULL);
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
   * This exercises _seg_matches_literal.  Before the NUL-termination fix,
   * http_decode_path_unsafe wrote decoded bytes but did not place a '\0';
   * the subsequent strcmp read past the valid content into uninitialised
   * stack memory.  On stacks where that byte happened to be non-zero the
   * route would fail to match and the server would return 404. */
  REQUIRE_TRUE(g_srv != NULL);
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
  REQUIRE_TRUE(g_srv != NULL);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%6Co", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 405);
}

TEST(chttpserver, malformed_encoding_in_literal_segment_returns_404) {
  /* Invalid percent-encoding in a LITERAL route segment must yield 404.
   * /hel%ZZo has bad encoding where the "hello" literal sits; no route can
   * match so the server returns 404 (not 500, not a crash). */
  REQUIRE_TRUE(g_srv != NULL);
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
  REQUIRE_TRUE(g_srv != NULL);
  char buf[2048] = {0};
  int status = _raw_request("POST", "/hel%ZZo", NULL, buf, sizeof(buf));
  REQUIRE_EQ(status, 404);
}

/* ========================================================================== */
/*                    NULL-GUARD COMPLETENESS TESTS                           */
/* ========================================================================== */

TEST(chttpserver, on_null_srv_returns_invalid_args) {
  /* chttpsvr_register_handler must validate srv != NULL and return
   * ccol_invalid_args before touching the root router.  Symmetric with
   * on_stream_null_srv_returns_invalid_args which already covers the streaming
   * variant. */
  ccol_retval_t rv =
      chttpsvr_register_handler(NULL, CHTTP_GET, "/any", _hello_handler, NULL);
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
  REQUIRE_TRUE(g_srv != NULL);
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
  REQUIRE_STREQ(resp->body, "GET");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_post) {
  /* CHTTP_ANY route dispatches a POST request and the handler observes
     CHTTP_POST. */
  chttpcli_response *resp = _post("/any-method", NULL, NULL);
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "GET:42");
  chttpclient_resp_free(resp);
}

TEST(chttpserver, any_method_specific_wins_for_registered_method) {
  /* A method-specific route registered BEFORE CHTTP_ANY on the same pattern
     wins for its own method (first-wins policy). */
  chttpcli_response *resp = _get("/any-with-specific");
  REQUIRE_TRUE(resp != NULL);
  REQUIRE_EQ(resp->status_code, 200);
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
  REQUIRE_STREQ(resp->body, "GET");
  chttpclient_resp_free(resp);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return -1;
  }

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
    return -1;
  }
  if (write(fd, hdr, (size_t)hn) != hn) {
    close(fd);
    return -1;
  }

  char *body = (char *)malloc(body_len ? body_len : 1);
  if (!body) {
    close(fd);
    return -1;
  }
  memset(body, 'a', body_len);
  size_t sent = 0;
  while (sent < body_len) {
    ssize_t w = write(fd, body + sent, body_len - sent);
    if (w < 0) {
      free(body);
      close(fd);
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

  int status = -1;
  sscanf(buf, "HTTP/1.1 %d", &status);
  return status;
}

TEST(chttpserver, buffered_max_body_size_at_limit_succeeds) {
  /* A body of exactly max_body_size (64) bytes must be accepted and echoed
     back in full; the enforcement check inside http1_on_body_chunk is
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  char buf[1024] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

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
  /* http1_atol16 accumulates into an unsigned long long and only stops once
     the top nibble becomes non-zero, so a 16-hex-digit chunk-size token can
     come back with bit 63 set; negative once read as `long long`.
     `0 - chunk_len` (turning a positive chunk size into this parser's
     negative "bytes remaining" sentinel) was undefined behavior for
     chunk_len == LLONG_MIN, and produced a *positive* content_length (an
     inverted, wrong sign) for any other negative chunk_len, walking the
     body cursor backward instead of forward. Verifies the fix (reject any
     chunk-size token whose accumulated value comes back negative) produces
     the exact same graceful, single-response, connection-closing behavior
     as the sibling "ZZZZ" malformed-chunk-size test above, rather than a
     crash, hang, or corrupted read. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  char buf[1024] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:") != NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") == NULL);
  REQUIRE_TRUE(strstr(buf, "connection:close") != NULL);
}

TEST(chttpserver, chunked_body_with_trailer_headers_handled_once) {
  /* A chunked body's terminating "0\r\n" can be followed by a trailer-part
     (zero or more header-field lines) before the final CRLF, per RFC 7230
     SS4.1.2. http1_consume_body_chunked used to handle "no trailers, CRLF
     immediately follows" as a fast path, but when a real trailer field WAS
     present, it re-entered the `case 1: headers` parsing state to consume
     it, landing back at the exact same `finished_headers:` label used for
     a message's *primary* header block, which unconditionally called
     http1_on_headers_complete a second time once the trailer's own blank
     line was found. For a diverted (matched-route) request, that second
     call would re-run chttpserver's routing/dispatch a second time for a
     request already handed to a worker. The fix (HTTP1_P_FLAG_TRAILER)
     skips the second on_headers_complete call for a trailer completion.
     Full disclosure on this test's actual coverage: reverting just this
     fix and running this exact test (including 25x back-to-back stress
     runs) did not reproduce an observable failure - the second dispatch
     appears to be absorbed without a visible symptom in this specific
     single-connection, single-request reproduction, plausibly because
     chttpserver's own routing for the second call still resolves
     synchronously on the same thread before any worker has a chance to
     race it. The fix is still correct (the underlying double-dispatch is
     real, confirmed by direct code reading, and its blast radius under
     different timing or a busier worker pool is not something a single
     deterministic test can rule out), but this test's verified value is
     that a real trailer field is parsed correctly at all (a distinct,
     genuine gap: the parser previously had no exercise of the trailer
     code path with an actual trailer present) - not that it specifically
     catches the double-dispatch race. */
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(TEST_PORT);
  REQUIRE_EQ(inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr), 1);

  int fd = socket(AF_INET, SOCK_STREAM, 0);
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
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

  /* Exactly one response: a second on_headers_complete dispatch for the
     trailer completion would, at best, corrupt the single response with
     extra bytes, and at worst crash the worker or hang the connection. */
  REQUIRE_TRUE(strstr(buf, "HTTP/1.1 200") != NULL);
  char *first = strstr(buf, "HTTP/1.1");
  REQUIRE_TRUE(first != NULL);
  REQUIRE_TRUE(strstr(first + 8, "HTTP/1.1") == NULL);
  REQUIRE_TRUE(strstr(buf, "x-stream-err:none") != NULL);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Content-Length: -1\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[512] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  const char *req =
      "POST /hello HTTP/1.1\r\n"
      "Host: 127.0.0.1\r\n"
      "Transfer-Encoding: chunked, gzip\r\n"
      "\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));

  char buf[512] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return NULL;
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) < 0) {
    close(fd);
    return NULL;
  }

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
   * the shared test fixture. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
  ccol_retval_t rv =
      chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/destroy-slow-body",
                                          _stream_error_report_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 4;
  cfg.stream_read_timeout_ms = 2000;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int port = TEST_PORT + 4;
  pthread_t bg;
  REQUIRE_EQ(pthread_create(&bg, NULL, _drip_body_bg_thread, &port), 0);

  /* Give the background connection time to be accepted, headers parsed, and
   * dispatched to a ctpool worker (which will then be blocked mid-drip
   * inside chttpsvr_req_read) before destroying the server underneath it. */
  usleep(100000);

  __chttpsvr_destroy(srv);

  pthread_join(bg, NULL);
}

static _Atomic bool g_restart_race_stop;

/* Repeatedly pipelines GET requests over `fd` (an already-open keep-alive
   connection) until g_restart_race_stop is set, tolerating any write/read
   failure by exiting (the listener side of the connection is expected to be
   torn down and replaced repeatedly by the main thread while this runs).
   Used by restart_races_live_keep_alive_connection_is_safe below to keep
   real pressure on _task_pause_cb's read of srv->worker_pool throughout a
   restart storm. */
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
   * chttpsvr_start restart/teardown path, while _task_pause_cb read it under
   * the lock - a genuine data race, and a real use-after-free risk if a
   * leftover pool were destroyed while a still-open keep-alive connection's
   * next pipelined request was concurrently reading/submitting to it:
   * chttpsvr_stop only closes the listener, already-accepted connections
   * keep running, and their _on_headers_complete does not check
   * srv->started, so _task_pause_cb can fire at any point during a restart.
   * Fixed by _wait_and_detach_worker_pool, which waits for
   * in_flight_requests to drain to zero before ever detaching/destroying a
   * pool, guaranteeing no _task_pause_cb call can be mid-flight holding a
   * stale copy of the pointer being handed to ctpool_destroy.
   *
   * This test doesn't assert on a return value for most of its body; the bug
   * is a data race / UAF, not a wrong result, so the real verification is
   * `make memtest` (valgrind) running this test clean, and a debug build
   * aborting/crashing outright if the race were reintroduced. A background
   * thread keeps one keep-alive connection continuously pipelining requests
   * while the main thread restarts the server many times in a tight loop (a
   * fresh port each cycle, so bind timing on a just-closed port can never
   * make this flaky; the race under test lives entirely on the srv side, not
   * the listener's port). Uses its own short-lived server so it cannot
   * disturb the shared test fixture. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

  atomic_store(&g_restart_race_stop, false);
  pthread_t pipeline_thread;
  REQUIRE_EQ(pthread_create(&pipeline_thread, NULL,
                            _restart_race_pipeline_thread, &fd),
             0);

  for (int i = 0; i < 5; i++) {
    chttpsvr_stop(srv);
    cfg.port = (uint16_t)(TEST_PORT + 51 + i);
    REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);
  }

  atomic_store(&g_restart_race_stop, true);
  pthread_join(pipeline_thread, NULL);
  close(fd);
  __chttpsvr_destroy(srv);
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
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  __chttpsvr_destroy(srv);
}

TEST(chttpserver, max_body_read_duration_default_disabled_allows_slow_drip) {
  /* max_body_read_duration_ms defaults to 0 (disabled); a slow-but-steady
   * drip that would trip a short overall cap must still succeed when the
   * cap is left unset, on a server whose stream_read_timeout_ms is generous
   * enough that the per-gap timeout doesn't fire either. Guards against the
   * deadline check misfiring when it's supposed to be a no-op. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  __chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                         SERVER-OWNED LOGGER TESTS                          */
/* ========================================================================== */

TEST(chttpserver, create_with_null_logger_uses_internal_fatal_only_logger) {
  /* cl == NULL must be accepted: the server creates its own internal logger
   * (stderr, FATAL-only) instead of requiring a caller-supplied one. The
   * server must still be fully functional. */
  chttpsvr srv = create_chttpsvr(NULL, NULL);
  REQUIRE_TRUE(srv != NULL);

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
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);

  __chttpsvr_destroy(srv);
}

TEST(chttpserver, create_with_logger_derives_and_leaves_parent_open) {
  /* cl != NULL must not be stored directly: the server derives its own
   * logger from it (tagged component=http-server) and closes only that
   * derived logger on destroy, leaving the caller's handle open and
   * reusable. */
  clog parent = clog_open_fd(2, CLOG_INFO);
  REQUIRE_TRUE(parent != NULL);

  chttpsvr srv = create_chttpsvr(parent, NULL);
  REQUIRE_TRUE(srv != NULL);

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
  REQUIRE_STREQ(resp->body, "Hello, world!");
  chttpclient_resp_free(resp);

  __chttpsvr_destroy(srv);

  /* parent must still be alive: write through it, and derive another
   * (unrelated) server from it, both of which would misbehave under
   * valgrind/ASan if the server had wrongly closed the caller's handle. */
  log_info(parent, "parent logger still usable after server destroy");

  chttpsvr srv2 = create_chttpsvr(parent, NULL);
  REQUIRE_TRUE(srv2 != NULL);
  __chttpsvr_destroy(srv2);

  clog_close(parent);
}

/* ========================================================================== */
/*                    UNIX DOMAIN SOCKET TESTS (Phase 1, new capability)      */
/* ========================================================================== */

TEST(chttpserver, unix_socket_listen_and_round_trip) {
  /* "unix://path" on chttpsvr_config_t.host must bind a Unix domain socket
     instead of a TCP listener, and a request over that socket must be
     routed and answered exactly like a TCP connection would be. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
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
  __chttpsvr_destroy(srv);
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

  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/stale-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = host_buf;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
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
  __chttpsvr_destroy(srv);
  unlink(sock_path);
}

TEST(chttpserver, unix_socket_unwritable_path_start_fails) {
  /* A directory component that doesn't exist must fail chttpsvr_start
     gracefully (bind() fails) rather than crashing or silently succeeding. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "unix:///chttpsvr_test_nonexistent_dir_xyz/socket.sock";
  ccol_retval_t rv = chttpsvr_start(srv, &cfg);
  REQUIRE_NE((int)rv, (int)ccol_success);
  __chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    MAX_CONNECTIONS TESTS (Phase 1, new knob)               */
/* ========================================================================== */

TEST(chttpserver, max_connections_enforced) {
  /* With max_connections == 1, a second concurrent connection must be left
     pending in the kernel's listen backlog (never accept()'d, never
     served) until the first connection closes and frees the one slot. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  int fd_a = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_a >= 0);
  REQUIRE_EQ(connect(fd_a, (struct sockaddr *)&sa, sizeof(sa)), 0);
  /* Give the server a moment to accept() fd_a and occupy the one slot
     before fd_b tries to connect. */
  struct timespec nap = {0, 150000000L}; /* 150ms */
  nanosleep(&nap, NULL);

  int fd_b = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd_b >= 0);
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

  /* Now fd_b must be accepted and served. */
  char buf[1024] = {0};
  int status = _read_one_http_response(fd_b, buf, sizeof(buf));
  REQUIRE_EQ(status, 200);

  close(fd_b);
  __chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    MAX_HEADER_BYTES TESTS (Phase 1, new knob)              */
/* ========================================================================== */

TEST(chttpserver, max_header_bytes_within_limit_succeeds) {
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  __chttpsvr_destroy(srv);
}

TEST(chttpserver, max_header_bytes_exceeded_closes_connection) {
  /* A header block exceeding the configured cap must be rejected before
     routing; an outright connection close with no HTTP response at all,
     matching every other pre-routing parse error in this parser (see
     negative_content_length_rejected above). */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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
  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  char buf[512] = {0};
  size_t total = 0;
  ssize_t r;
  while (total < sizeof(buf) - 1 &&
         (r = read(fd, buf + total, sizeof(buf) - 1 - total)) > 0)
    total += (size_t)r;
  buf[total] = '\0';
  close(fd);

  REQUIRE_TRUE(strstr(buf, "HTTP/1.1") == NULL);

  __chttpsvr_destroy(srv);
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
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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
  int fd = socket(AF_INET, SOCK_STREAM, 0);
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

  char buf[65536];
  ssize_t r;
  while ((r = read(fd, buf, sizeof(buf))) >
         0) { /* drain whatever got through */
  }
  REQUIRE_EQ(r, 0); /* EOF: server closed the connection */

  close(fd);
  __chttpsvr_destroy(srv);
}

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP TEST (Phase 1, new mechanism)        */
/* ========================================================================== */

TEST(chttpserver, idle_timeout_closes_unused_connection) {
  /* A connection that never sends a request at all must eventually be
     closed by the module-local idle-timeout sweep thread once
     idle_timeout_ms has elapsed, rather than being held open forever. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);

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

  close(fd);
  __chttpsvr_destroy(srv);
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
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
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

  __chttpsvr_destroy(srv);
}

TEST(chttpserver, enable_reuseport_allows_second_listener_on_same_port) {
  /* Without SO_REUSEPORT, a second bind to the same host:port fails with
     EADDRINUSE (chttpsvr_start returns ccol_unexpected_failure); this is a
     real, externally observable effect of the option, unlike
     enable_keepalive/ipv6_only above. */
  chttpsvr srv1 = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv1 != NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + 15;
  cfg.enable_reuseport = true;
  REQUIRE_EQ((int)chttpsvr_start(srv1, &cfg), (int)ccol_success);

  chttpsvr srv2 = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv2 != NULL);
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

  __chttpsvr_destroy(srv1);
  __chttpsvr_destroy(srv2);
}

TEST(chttpserver, ipv6_only_listener_still_serves_ipv6_traffic) {
  /* Best-effort, matching this codebase's own established IPv6 convention
     elsewhere (not every sandbox/CI environment has an IPv6 stack): skip
     rather than hard-fail if binding "::1" itself doesn't work at all,
     since that's an environment limitation unrelated to ipv6_only. */
  chttpsvr srv = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_TRUE(srv != NULL);
  ccol_retval_t rv = chttpsvr_register_handler(srv, CHTTP_GET, "/v6only-hello",
                                               _hello_handler, NULL);
  REQUIRE_EQ((int)rv, (int)ccol_success);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "::1";
  cfg.port = TEST_PORT + 16;
  cfg.ipv6_only = true;
  if (chttpsvr_start(srv, &cfg) != ccol_success) {
    __chttpsvr_destroy(srv);
    fprintf(stderr,
            "[SKIP] ipv6_only_listener_still_serves_ipv6_traffic: no IPv6 "
            "stack available in this environment\n");
    return;
  }

  int fd = socket(AF_INET6, SOCK_STREAM, 0);
  REQUIRE_TRUE(fd >= 0);
  struct sockaddr_in6 sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin6_family = AF_INET6;
  sa.sin6_port = htons((uint16_t)(TEST_PORT + 16));
  REQUIRE_EQ(inet_pton(AF_INET6, "::1", &sa.sin6_addr), 1);
  REQUIRE_EQ(connect(fd, (struct sockaddr *)&sa, sizeof(sa)), 0);
  const char *req =
      "GET /v6only-hello HTTP/1.1\r\nHost: [::1]\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(write(fd, req, strlen(req)), (ssize_t)strlen(req));
  char buf[512] = {0};
  ssize_t n = read(fd, buf, sizeof(buf) - 1);
  REQUIRE_GT(n, (ssize_t)0);
  REQUIRE_TRUE(strstr(buf, "200") != NULL);
  close(fd);

  __chttpsvr_destroy(srv);
}
