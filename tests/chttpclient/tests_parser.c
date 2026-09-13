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

/*
 * White-box, byte-sequence-level tests for chttp1_parser, exercised directly
 * (no TLS, no chttpclient.c at all) with hand-crafted input, including
 * malformed/hostile input the real-socket-based tests.c suite structurally
 * cannot reach (its mock server only ever sends well-formed responses). See
 * tests.c/tests_tls.c for the "does real traffic still work" end-to-end
 * coverage; this file is purely about the parser's own correctness in
 * isolation. The "stream" test group (chttp1_stream_t, added alongside
 * request-mode parsing) does use local AF_UNIX socketpairs, but only to
 * exercise its own read/write/poll logic directly; no chttpclient.c, no
 * TLS, no real network traffic.
 */

#include <chttp1_parser.h>
#include <ctls.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../tau/tau.h"

TAU_MAIN()

#define MAX_TEST_HEADERS 32
#define TEST_BODY_CAP (256 * 1024)

typedef struct {
  int status_code;
  bool is_head;
  bool force_on_header_error;
  bool force_on_body_error;
  bool force_on_headers_complete_error;
  bool want_divert; /* on_headers_complete returns CHTTP1_HEADERS_DIVERT_BODY */

  char header_names[MAX_TEST_HEADERS][256];
  char header_values[MAX_TEST_HEADERS][256];
  size_t header_count;

  char body[TEST_BODY_CAP];
  size_t body_len;

  bool headers_complete_called;
  bool message_complete_called;

  /* Request mode only. */
  bool request_line_called;
  bool force_on_request_line_error;
  char method[32];
  char target[256];
} test_ctx_t;

static int t_on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                       const char *value, size_t value_len) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  if (ctx->force_on_header_error) return 1;
  if (ctx->header_count < MAX_TEST_HEADERS) {
    size_t nl = name_len < 255 ? name_len : 255;
    size_t vl = value_len < 255 ? value_len : 255;
    memcpy(ctx->header_names[ctx->header_count], name, nl);
    ctx->header_names[ctx->header_count][nl] = '\0';
    memcpy(ctx->header_values[ctx->header_count], value, vl);
    ctx->header_values[ctx->header_count][vl] = '\0';
    ctx->header_count++;
  }
  return 0;
}

static int t_on_headers_complete(chttp1_parser_t *p) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  ctx->headers_complete_called = true;
  ctx->status_code = p->status_code;
  if (ctx->force_on_headers_complete_error) return -1;
  if (ctx->want_divert) return CHTTP1_HEADERS_DIVERT_BODY;
  return ctx->is_head ? CHTTP1_HEADERS_NO_BODY : CHTTP1_HEADERS_HAS_BODY;
}

static int t_on_request_line(chttp1_parser_t *p, const char *method,
                             size_t method_len, const char *target,
                             size_t target_len) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  ctx->request_line_called = true;
  if (ctx->force_on_request_line_error) return 1;
  size_t ml = method_len < sizeof(ctx->method) - 1 ? method_len
                                                   : sizeof(ctx->method) - 1;
  memcpy(ctx->method, method, ml);
  ctx->method[ml] = '\0';
  size_t tl = target_len < sizeof(ctx->target) - 1 ? target_len
                                                   : sizeof(ctx->target) - 1;
  memcpy(ctx->target, target, tl);
  ctx->target[tl] = '\0';
  return 0;
}

static int t_on_body(chttp1_parser_t *p, const char *at, size_t len) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  if (ctx->force_on_body_error) return 1;
  if (ctx->body_len + len <= sizeof(ctx->body)) {
    memcpy(ctx->body + ctx->body_len, at, len);
    ctx->body_len += len;
  }
  return 0;
}

static int t_on_message_complete(chttp1_parser_t *p) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  ctx->message_complete_called = true;
  return 0;
}

/* Static storage duration: chttp1_parser_init only borrows the settings
 * pointer (per its own documented "settings must outlive parser" contract)
 * rather than copying it, so a stack-local here would leave every parser's
 * ->settings dangling the moment init_test() returns. */
static const chttp1_settings_t g_test_settings = {
    .on_header = t_on_header,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

static void init_test(chttp1_parser_t *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init(parser, &g_test_settings);
  parser->data = ctx;
}

/* Same static-storage-duration rationale as g_test_settings above. */
static const chttp1_settings_t g_test_request_settings = {
    .on_request_line = t_on_request_line,
    .on_header = t_on_header,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

static void init_test_request(chttp1_parser_t *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init_request(parser, &g_test_request_settings);
  parser->data = ctx;
}

static const char *find_header(const test_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->header_count; i++)
    if (strcmp(ctx->header_names[i], name) == 0) return ctx->header_values[i];
  return NULL;
}

/* ========================================================================== */
/*                     STATUS LINE                                           */
/* ========================================================================== */

TEST(status_line, basic_with_reason) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 200);
}

TEST(status_line, missing_reason_and_trailing_space_accepted) {
  /* Real-world compatibility: RFC 7230's ABNF technically requires a
   * trailing SP even for an empty reason phrase, but real servers omit it
   * (e.g. "HTTP/1.1 304\r\n"). */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 304\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 304);
}

TEST(status_line, any_major_minor_digit_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 200);
}

TEST(status_line, nonstandard_but_wellformed_code_accepted) {
  /* No magnitude restriction: real, in-use nonstandard status codes exist
   * up to 599 (e.g. 599 Network Connect Timeout Error, used by some
   * load balancers/proxies), so the parser accepts any 3-digit code. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 599 Network Connect Timeout\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 599);
}

TEST(status_line, lowercase_protocol_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "http/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, garbled_prefix_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "NOPE/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, two_digit_code_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 20 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, four_digit_code_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 2000 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, non_numeric_code_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 2AB OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, bare_lf_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, split_across_every_byte_boundary) {
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  size_t len = strlen(msg);
  for (size_t split = 1; split < len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno_t rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED)
      continue; /* message already fully consumed by first split */
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno_t rv2 =
        chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_EQ(ctx.status_code, 200);
    REQUIRE_EQ(ctx.body_len, (size_t)5);
    REQUIRE_STREQ(ctx.body, "hello");
  }
}

/* ========================================================================== */
/*                     KEEP-ALIVE                                            */
/* ========================================================================== */

TEST(keep_alive, http_1_1_default_is_keep_alive) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_1_connection_close_overrides_default) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_0_default_is_not_keep_alive) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_0_connection_keep_alive_overrides_default) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\n"
      "hello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, version_greater_than_1_with_zero_minor_defaults_keep_alive) {
  /* Regression test: chttp1_should_keep_alive used to test
   * "http_major > 0 && http_minor > 0" to decide "HTTP/1.1 or later", which
   * incorrectly treated any version with a zero minor component (e.g. a
   * literal "HTTP/2.0" status line, which this parser's grammar accepts;
   * see status_line.any_major_minor_digit_accepted) as HTTP/1.0-or-earlier,
   * requiring an explicit "Connection: keep-alive" token no real server of
   * that vintage would send, and silently defeating connection reuse. The
   * correct test is "major > 1, or major == 1 with minor >= 1". */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/2.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(parser.http_major, 2);
  REQUIRE_EQ(parser.http_minor, 0);
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive,
     version_greater_than_1_with_zero_minor_connection_close_honored) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/3.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

/* ========================================================================== */
/*                     HEADERS                                               */
/* ========================================================================== */

TEST(headers, basic_and_case_preserved_on_name) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: Bar\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "Bar");
}

TEST(headers, ows_trimmed_from_value) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: \t  Bar  \t\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "Bar");
}

TEST(headers, missing_colon_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, empty_name_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\n: Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, control_char_in_name_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char msg[] = "HTTP/1.1 200 OK\r\nX-\x01Foo: Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, sizeof(msg) - 1),
             CHTTP1_ERROR);
}

TEST(headers, embedded_nul_in_value_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char msg[] = "HTTP/1.1 200 OK\r\nX-Foo: a\0b\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, sizeof(msg) - 1),
             CHTTP1_ERROR);
}

TEST(headers, obsolete_line_folding_rejected) {
  /* A continuation line starting with whitespace ("obs-fold") must be
   * rejected outright, not un-folded into the previous header's value. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo: bar\r\n baz\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, split_across_every_byte_boundary) {
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: bar\r\nContent-Length: 3\r\n\r\nabc";
  size_t len = strlen(msg);
  for (size_t split = 1; split < len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno_t rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED) continue;
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno_t rv2 =
        chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "bar");
    REQUIRE_STREQ(ctx.body, "abc");
  }
}

TEST(headers, on_header_callback_error_reports_user) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_header_error = true;
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo: bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(headers, on_headers_complete_callback_error_reports_user) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_headers_complete_error = true;
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

/* Size caps */

static void build_header_line(char *buf, size_t buf_size, size_t value_len) {
  /* "X-Pad: " (7 bytes) + value_len 'a' bytes + CRLF */
  size_t n = (size_t)snprintf(buf, buf_size, "X-Pad: ");
  memset(buf + n, 'a', value_len);
  n += value_len;
  buf[n++] = '\r';
  buf[n++] = '\n';
  buf[n] = '\0';
}

TEST(headers, line_at_cap_boundary_accepted_one_more_rejected) {
  /* CHTTP1_MAX_LINE_LEN is 8192; the raw accumulated line is
   * "X-Pad: " (7 bytes) + value + CRLF (2 bytes), so the largest value that
   * still fits is 8192 - 7 - 2 = 8183 bytes. */
  char status_and_headers_prefix[64];
  snprintf(status_and_headers_prefix, sizeof(status_and_headers_prefix),
           "HTTP/1.1 200 OK\r\n");

  char line[8300];
  build_header_line(line, sizeof(line), 8183);

  char msg[8600];
  size_t off =
      (size_t)snprintf(msg, sizeof(msg), "%s", status_and_headers_prefix);
  memcpy(msg + off, line, strlen(line));
  off += strlen(line);
  off += (size_t)snprintf(msg + off, sizeof(msg) - off,
                          "Content-Length: 0\r\n\r\n");

  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_PAUSED);
}

TEST(headers, line_one_byte_over_cap_rejected) {
  char status_and_headers_prefix[64];
  snprintf(status_and_headers_prefix, sizeof(status_and_headers_prefix),
           "HTTP/1.1 200 OK\r\n");

  char line[8300];
  build_header_line(line, sizeof(line),
                    8184); /* one byte over the 8183 boundary */

  char msg[8600];
  size_t off =
      (size_t)snprintf(msg, sizeof(msg), "%s", status_and_headers_prefix);
  memcpy(msg + off, line, strlen(line));
  off += strlen(line);

  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

TEST(headers, too_many_headers_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);

  char msg[16384];
  size_t off = (size_t)snprintf(msg, sizeof(msg), "HTTP/1.1 200 OK\r\n");
  for (int i = 0; i < 101; i++) { /* one past CHTTP1_MAX_HEADER_COUNT (100) */
    off += (size_t)snprintf(msg + off, sizeof(msg) - off, "X-H%d: v\r\n", i);
  }
  off += (size_t)snprintf(msg + off, sizeof(msg) - off, "\r\n");

  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

TEST(headers, total_header_bytes_cap_rejected) {
  /* 80 headers of ~1000 bytes each (80KB) comfortably exceeds the 64KB
   * total cap while staying well under the 100-header count cap and the
   * 8192-byte per-line cap, isolating the total-bytes limit specifically.
   * Fed incrementally (status line first, then one header line per
   * chttp1_parser_execute call) so the test can stop as soon as the parser
   * reports the error, rather than building one giant buffer up front. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);

  const char *status_line = "HTTP/1.1 200 OK\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, status_line, strlen(status_line)),
             CHTTP1_OK);

  bool got_error = false;
  for (int i = 0; i < 80; i++) {
    char line[1100];
    build_header_line(line, sizeof(line), 1000);
    chttp1_errno_t rv = chttp1_parser_execute(&parser, line, strlen(line));
    if (rv == CHTTP1_ERROR) {
      got_error = true;
      break;
    }
    REQUIRE_EQ(rv, CHTTP1_OK);
  }
  REQUIRE_TRUE(got_error);
}

TEST(headers, max_header_count_override_rejects_below_builtin_default) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_header_count_override = 2; /* well under the 100 builtin default */

  const char *msg =
      "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\n\r\n"; /* 3 headers */
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, max_header_count_override_allows_up_to_the_override) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_header_count_override = 4; /* X-A/X-B/X-C plus Content-Length */

  const char *msg =
      "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\nContent-Length: "
      "0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
}

TEST(headers, max_total_header_bytes_override_rejects_below_builtin_default) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_total_header_bytes_override = 32; /* well under the 64KB default */

  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Long-Header-Name: some longer value here\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers,
     request_line_itself_counts_against_max_total_header_bytes_override) {
  /* Regression test: chttpsvr_config_t.max_header_bytes is documented as
   * bounding "request line + all header lines", but total_header_bytes
   * used to only ever be incremented by process_header_line, never by the
   * request/status line itself; so a request-target far larger than a
   * configured cap was still accepted, bounded only by the much larger
   * CHTTP1_MAX_LINE_LEN. A request line alone, comfortably over the tiny
   * override below but nowhere near CHTTP1_MAX_LINE_LEN, must now be
   * rejected before any header is even seen. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_total_header_bytes_override = 32; /* well under the request line */

  char target[512];
  memset(target, 'a', sizeof(target) - 1);
  target[sizeof(target) - 1] = '\0';
  char msg[600];
  snprintf(msg, sizeof(msg), "GET /%s HTTP/1.1\r\n\r\n", target);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, request_line_and_headers_share_the_same_total_bytes_budget) {
  /* A request line and a header line that individually fit, but whose SUM
   * exceeds the override, must still be rejected; confirming the request
   * line's own contribution is genuinely added to the same running total a
   * header line contributes to, not tracked separately/ignored. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_total_header_bytes_override = 40; /* "GET / HTTP/1.1\r\n" is 16 */

  const char *first_line = "GET / HTTP/1.1\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, first_line, strlen(first_line)),
             CHTTP1_OK);
  const char *header_line = "X-Long-Header-Name: some longer value\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, header_line, strlen(header_line)),
             CHTTP1_ERROR);
}

/* ========================================================================== */
/*                     CONTENT-LENGTH / TRANSFER-ENCODING CONFLICTS           */
/* ========================================================================== */

TEST(framing, duplicate_content_length_same_value_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, duplicate_content_length_differing_value_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, content_length_and_chunked_conflict_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: "
      "chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, content_length_decimal_overflow_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, transfer_encoding_not_chunked_reads_until_eof) {
  /* RFC 7230 SS3.3.3: for a RESPONSE (never a request, which this parser
   * never parses), a Transfer-Encoding whose last token isn't "chunked"
   * means the body is read until the connection closes; not rejected. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nsome-bytes";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "some-bytes");
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

/* ========================================================================== */
/*                     CHUNKED ENCODING                                      */
/* ========================================================================== */

TEST(chunked, basic) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(chunked, multiple_chunks) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "foobar");
}

TEST(chunked, non_hex_chunk_size_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZZ\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, hex_multiply_overflow_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "FFFFFFFFFFFFFFFFFFFFF\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, extension_accepted_and_discarded) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5;ext=value\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(chunked, terminal_chunk_with_trailers) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Trailer: trailer-value\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_STREQ(find_header(&ctx, "X-Trailer"), "trailer-value");
}

TEST(chunked, terminal_chunk_without_trailers) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(chunked, missing_crlf_after_chunk_data_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhelloXX0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_chunk_size_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_chunk_data_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_trailer_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Trail";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(chunked, trailer_bytes_count_against_same_header_caps) {
  /* Trailers do not get a separate budget: pushing the trailer section past
   * CHTTP1_MAX_HEADER_COUNT (100) must be rejected exactly like too many
   * regular headers would be. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);

  char msg[16384];
  size_t off = (size_t)snprintf(
      msg, sizeof(msg),
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n");
  for (int i = 0; i < 101; i++) {
    off += (size_t)snprintf(msg + off, sizeof(msg) - off, "X-T%d: v\r\n", i);
  }
  off += (size_t)snprintf(msg + off, sizeof(msg) - off, "\r\n");

  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

/* ========================================================================== */
/*                     CONTENT-LENGTH BODY                                   */
/* ========================================================================== */

TEST(content_length_body, exact_match) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(msg));
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(content_length_body, eof_before_satisfied_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(content_length_body, trailing_garbage_not_consumed) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  size_t consumed = chttp1_parser_consumed(&parser);
  REQUIRE_TRUE(consumed < strlen(msg));
  REQUIRE_STREQ(ctx.body, "hello");
}

/* ========================================================================== */
/*                     EOF-DELIMITED BODY                                    */
/* ========================================================================== */

TEST(eof_body, clean_termination_and_not_keep_alive) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_TRUE(ctx.message_complete_called);
  /* Even though "Connection: keep-alive" was present, an EOF-delimited body
   * is never keep-alive eligible: ending the body already required ending
   * the connection. */
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

/* ========================================================================== */
/*                     HEAD REQUESTS / NO-BODY STATUSES                      */
/* ========================================================================== */

TEST(no_body, head_with_content_length_suppresses_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.is_head = true;
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 12345\r\n\r\n"; /* no body bytes
                                                             follow */
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_1xx_no_body_even_with_content_length) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 102 Processing\r\nContent-Length: 5\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_204_no_body_without_content_length) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 204 No Content\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_304_no_body_with_content_length) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

/* ========================================================================== */
/*                     BYTE-BOUNDARY FRAGMENTATION STRESS                    */
/* ========================================================================== */

static void assert_fragmented_matches_oneshot(const char *msg, size_t len) {
  chttp1_parser_t ref_parser;
  test_ctx_t ref_ctx;
  init_test(&ref_parser, &ref_ctx);
  chttp1_errno_t ref_rv = chttp1_parser_execute(&ref_parser, msg, len);
  REQUIRE_EQ(ref_rv, CHTTP1_PAUSED);
  size_t ref_consumed = chttp1_parser_consumed(&ref_parser);
  bool ref_keep_alive = chttp1_should_keep_alive(&ref_parser);

  for (size_t split = 1; split < len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno_t rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED) {
      /* The whole message happened to fit before this split point (e.g. a
       * short message with a large split); nothing left to feed. */
      REQUIRE_EQ(chttp1_parser_consumed(&parser), ref_consumed);
      continue;
    }
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno_t rv2 =
        chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_EQ(ctx.status_code, ref_ctx.status_code);
    REQUIRE_EQ(ctx.header_count, ref_ctx.header_count);
    REQUIRE_EQ(ctx.body_len, ref_ctx.body_len);
    REQUIRE_TRUE(memcmp(ctx.body, ref_ctx.body, ctx.body_len) == 0);
    /* bool isn't one of tau's REQUIRE_EQ _Generic printer associations, cast to
     * int. */
    REQUIRE_EQ((int)chttp1_should_keep_alive(&parser), (int)ref_keep_alive);
    /* Hard contract: execute() never returns CHTTP1_OK with bytes left
     * unconsumed; rv1 == CHTTP1_OK above already implies the first call
     * consumed all `split` bytes (nothing else to check here beyond having
     * reached this point at all). */
  }
}

TEST(fragmentation, simple_message) {
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: bar\r\nContent-Length: 5\r\n\r\nhello";
  assert_fragmented_matches_oneshot(msg, strlen(msg));
}

TEST(fragmentation, chunked_message_with_trailers) {
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nfoo\r\n3\r\nbar\r\n0\r\nX-Trailer: t\r\n\r\n";
  assert_fragmented_matches_oneshot(msg, strlen(msg));
}

TEST(fragmentation, content_length_message) {
  const char *msg =
      "HTTP/1.1 200 OK\r\nConnection: close\r\nContent-Length: 11\r\n\r\n"
      "hello world";
  assert_fragmented_matches_oneshot(msg, strlen(msg));
}

/* ========================================================================== */
/*                     FINISH() MATRIX                                       */
/* ========================================================================== */

TEST(finish_matrix, before_status_line_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_headers_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo: bar\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_content_length_body_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_chunk_is_unsafe) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, at_clean_boundary_is_safe_no_callback) {
  /* chttpclient.c itself never calls chttp1_parser_finish after execute()
   * already returned CHTTP1_PAUSED (it returns success immediately instead,
   * per chttp1_parser_execute's own documented contract); this test calls
   * it anyway, purely to exercise the CHTTP1_FINISH_SAFE switch case
   * directly: a message that completed via a normal (non-EOF-delimited)
   * path leaves finish_state at CHTTP1_FINISH_SAFE, which finish() reports
   * as CHTTP1_OK without invoking on_message_complete a second time. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  ctx.message_complete_called = false;
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_OK);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(finish_matrix, inside_eof_delimited_body_is_safe_with_callback) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(finish_matrix,
     calling_finish_twice_after_eof_delimited_body_does_not_refire_callback) {
  /* Regression test: the CHTTP1_FINISH_SAFE_WITH_CB case never updated
   * finish_state (only parser->state) after firing on_message_complete, so
   * finish_state stayed CHTTP1_FINISH_SAFE_WITH_CB forever, and a second
   * finish() call on an already-CHTTP1_ST_MESSAGE_DONE parser used to
   * re-enter that same branch and re-invoke on_message_complete, violating
   * that callback's own "fired exactly once" contract and this function's
   * own documented "already at a clean boundary returns CHTTP1_OK"
   * contract. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);

  ctx.message_complete_called = false;
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_OK);
  REQUIRE_FALSE(ctx.message_complete_called);

  ctx.message_complete_called = false;
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_OK);
  REQUIRE_FALSE(ctx.message_complete_called);
}

/* ========================================================================== */
/*                     MISC                                                  */
/* ========================================================================== */

TEST(misc, zero_length_execute_is_a_defined_noop) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, "", 0), CHTTP1_OK);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
}

/* ========================================================================== */
/*                     REQUEST LINE (request mode)                           */
/* ========================================================================== */

TEST(request_line, basic_get_no_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.request_line_called);
  REQUIRE_STREQ(ctx.method, "GET");
  REQUIRE_STREQ(ctx.target, "/path");
  REQUIRE_EQ(parser.http_major, 1);
  REQUIRE_EQ(parser.http_minor, 1);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(request_line, post_with_content_length_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /submit HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.method, "POST");
  REQUIRE_STREQ(ctx.target, "/submit");
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_line, query_string_preserved_in_target) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path?a=1&b=2 HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/path?a=1&b=2");
}

TEST(request_line, asterisk_target_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "OPTIONS * HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.method, "OPTIONS");
  REQUIRE_STREQ(ctx.target, "*");
}

TEST(request_line, http_1_0_request_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(parser.http_major, 1);
  REQUIRE_EQ(parser.http_minor, 0);
}

TEST(request_line, invalid_method_char_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GE/T /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, missing_target_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, missing_version_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, bad_version_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path FOO/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, control_char_in_target_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /pa\x01th HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, on_request_line_callback_error_reports_user) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.force_on_request_line_error = true;
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(request_line, split_across_every_byte_boundary) {
  const char *msg = "PUT /a/b/c HTTP/1.1\r\nContent-Length: 3\r\n\r\nxyz";
  size_t len = strlen(msg);
  for (size_t split = 0; split <= len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    chttp1_errno_t r1 = chttp1_parser_execute(&parser, msg, split);
    if (r1 == CHTTP1_PAUSED) {
      REQUIRE_EQ(split, len);
      continue;
    }
    REQUIRE_EQ(r1, CHTTP1_OK);
    chttp1_errno_t r2 =
        chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(r2, CHTTP1_PAUSED);
  }
}

TEST(request_line, single_leading_blank_line_tolerated) {
  /* RFC 7230 SS3.5: a server SHOULD ignore at least one empty line received
   * prior to the request-line (some clients send a stray CRLF after a POST
   * body). Regression test: this used to hard-reject with PH_ERROR
   * (method_len == 0), closing an otherwise-healthy keep-alive/pipelined
   * connection over one stray CRLF. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "\r\nGET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.request_line_called);
  REQUIRE_STREQ(ctx.method, "GET");
  REQUIRE_STREQ(ctx.target, "/path");
}

TEST(request_line, multiple_leading_blank_lines_tolerated) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "\r\n\r\n\r\nGET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/path");
}

TEST(request_line, leading_blank_lines_split_across_byte_boundaries) {
  const char *msg = "\r\n\r\nGET /path HTTP/1.1\r\n\r\n";
  size_t len = strlen(msg);
  for (size_t split = 1; split < len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    chttp1_errno_t r1 = chttp1_parser_execute(&parser, msg, split);
    if (r1 == CHTTP1_PAUSED) continue;
    REQUIRE_EQ(r1, CHTTP1_OK);
    chttp1_errno_t r2 =
        chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(r2, CHTTP1_PAUSED);
    REQUIRE_STREQ(ctx.target, "/path");
  }
}

TEST(request_line, excessive_leading_blank_lines_rejected) {
  /* Bounded, not skipped unconditionally: a client that never stops
   * sending blank lines must eventually be treated as malformed input
   * rather than tolerated indefinitely. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  char msg[4096];
  size_t off = 0;
  for (int i = 0; i < 200; i++) {
    msg[off++] = '\r';
    msg[off++] = '\n';
  }
  memcpy(msg + off, "GET / HTTP/1.1\r\n\r\n", 18);
  off += 18;
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

/* ========================================================================== */
/*        REQUEST-MODE BODY FRAMING (RFC 7230 SS3.3 asymmetry vs response)   */
/* ========================================================================== */

TEST(request_body_framing, no_framing_headers_means_no_body_not_eof) {
  /* Unlike a response, a request with neither Content-Length nor chunked
   * Transfer-Encoding has NO body at all; the message completes
   * immediately after headers, it does not wait for EOF (there would be
   * nothing to wait for anyway; the connection isn't closing). */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(request_body_framing, content_length_zero_completes_immediately) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(request_body_framing, chunked_request_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: "
      "chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing, trailer_name_not_whitelisted) {
  /* Confirmed decision: no trailer-name whitelist for requests; any
   * trailer name is accepted, matching the client parser's own
   * pre-existing, unrestricted trailer handling. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nCustom-Trailer: allowed\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(find_header(&ctx, "Custom-Trailer"), "allowed");
}

TEST(request_body_framing,
     transfer_encoding_present_but_final_coding_not_chunked_rejected) {
  /* RFC 7230 SS3.3.3: a request's Transfer-Encoding whose final coding is
   * not "chunked" leaves the message length indeterminate; a conforming
   * server MUST reject it outright rather than silently treating it as
   * bodyless (the request/response asymmetry means a request has no
   * EOF-delimited fallback the way a response does). Regression test: this
   * used to be silently accepted as if Transfer-Encoding were absent. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(
    request_body_framing,
    transfer_encoding_chunked_not_last_split_across_two_header_lines_rejected) {
  /* Regression test: transfer_encoding_has_nonfinal_chunked only sees one
   * header line at a time, so "chunked" claimed as final by an EARLIER
   * Transfer-Encoding line and then followed by a SECOND Transfer-Encoding
   * line (RFC 7230 SS3.2.2: repeated header lines are one concatenated
   * comma-separated list, in order) used to bypass that check entirely,
   * since each line was validated in isolation. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
      "Transfer-Encoding: identity\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing,
     transfer_encoding_chunked_last_split_across_two_header_lines_accepted) {
  /* The legitimate counterpart of the above: "chunked" arriving as the
   * LAST token of the LAST Transfer-Encoding line (here, on the second
   * line, following a first line that doesn't end in chunked) is valid
   * framing and must still be accepted. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n"
      "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing,
     response_mode_final_coding_not_chunked_still_reads_until_eof) {
  /* Response mode's own, separate, documented scope reduction (see
   * value_ends_with_chunked's doc comment) must be unaffected by the
   * request-mode-only rejection added above: a response with a
   * Transfer-Encoding whose final coding isn't "chunked" still falls back
   * to EOF-delimited framing, exactly as before. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

TEST(request_body_framing, single_line_chunked_then_gzip_rejected) {
  /* transfer_encoding_has_nonfinal_chunked's single-line path (as opposed
   * to the split-across-two-header-lines path already covered above):
   * "chunked" appearing before the end of one line's own token list is
   * unconditionally wrong and must be rejected immediately, regardless of
   * what the trailing token is. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing, single_line_gzip_then_chunked_accepted) {
  /* The legitimate single-line counterpart: multiple comma-separated
   * codings on ONE Transfer-Encoding line, ending in "chunked", is valid
   * framing and must be accepted with chunked body parsing. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing,
     response_mode_single_line_chunked_then_gzip_reads_until_eof) {
  /* Response mode's scope reduction applies just as much to a single-line
   * multi-coding value as to the single-coding case already covered above:
   * "chunked" not being the final token on a response's Transfer-Encoding
   * line is NOT rejected (transfer_encoding_has_nonfinal_chunked is gated
   * to CHTTP1_PARSE_REQUEST only), it just means the final coding isn't
   * "chunked", so the response falls back to EOF-delimited framing. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

TEST(request_body_framing,
     response_mode_single_line_gzip_then_chunked_accepted) {
  /* Mirrors single_line_gzip_then_chunked_accepted above for response mode:
   * value_ends_with_chunked doesn't distinguish request from response, so a
   * response whose Transfer-Encoding line ends in "chunked" uses chunked
   * body parsing regardless of what precedes it on the same line. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

/* ========================================================================== */
/*              HEADERS-COMPLETE BODY DIVERSION (CHTTP1_HEADERS_ONLY)        */
/* ========================================================================== */

TEST(divert, content_length_body_diverts_before_consuming) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(headers));
  REQUIRE_FALSE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);

  const char *body = "hello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, body, strlen(body)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(divert, body_already_in_same_buffer_is_not_consumed_by_divert) {
  /* The critical carry-over case: header block AND body bytes arrive in
   * the SAME chttp1_parser_execute call. Diversion must still stop exactly
   * at the header boundary, leaving the body bytes unconsumed for the
   * caller to hand off as carry-over; not swallow them into on_body
   * simply because they happened to already be available. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\n";
  const char *whole = "POST /x HTTP/1.1\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, whole, strlen(whole)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(headers));
  REQUIRE_FALSE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);

  size_t consumed = chttp1_parser_consumed(&parser);
  const char *leftover = whole + consumed;
  size_t leftover_len = strlen(whole) - consumed;
  REQUIRE_EQ(leftover_len, (size_t)5);
  REQUIRE_EQ(chttp1_parser_execute(&parser, leftover, leftover_len),
             CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(divert, chunked_body_diverts_before_consuming) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(headers));

  const char *rest = "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, rest, strlen(rest)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(divert, no_body_downgrades_to_immediate_complete) {
  /* Nothing to divert (no Content-Length, no chunked): want_divert is
   * downgraded to ordinary immediate completion instead of pausing. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(divert, content_length_zero_downgrades_to_immediate_complete) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "POST /x HTTP/1.1\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(divert, response_mode_rejects_divert_hint) {
  /* CHTTP1_HEADERS_DIVERT_BODY is request-mode only; a response-mode parser
   * treats it as an invalid hint (there is no worker-thread diversion
   * concept for chttpclient.c to hand off to). */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

/* ========================================================================== */
/*         chttp1_has_content_length / chttp1_declared_content_length        */
/* ========================================================================== */

TEST(content_length_accessor, false_before_headers_complete) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
  REQUIRE_EQ(chttp1_declared_content_length(&parser), (uint64_t)0);
}

TEST(content_length_accessor, true_with_declared_value_once_headers_complete) {
  /* want_divert pauses parsing right at headers-complete, before any body
   * byte is consumed, so the accessor's own documented caveat ("reused
   * internally to track the CURRENT chunk's remaining byte count once
   * chunked parsing begins... calling this after body parsing has already
   * started returns a value with a different meaning") does not yet apply;
   * this is exactly the window chttpserver.c's real caller uses it in. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers = "POST /x HTTP/1.1\r\nContent-Length: 42\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_TRUE(chttp1_has_content_length(&parser));
  REQUIRE_EQ(chttp1_declared_content_length(&parser), (uint64_t)42);
}

TEST(content_length_accessor, false_for_chunked_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
}

TEST(content_length_accessor, false_for_no_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
}

TEST(content_length_accessor, null_parser_returns_safe_defaults) {
  REQUIRE_FALSE(chttp1_has_content_length(NULL));
  REQUIRE_EQ(chttp1_declared_content_length(NULL), (uint64_t)0);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(NULL));
}

/* ========================================================================== */
/*                     chttp1_parser_message_complete                        */
/* ========================================================================== */

TEST(message_complete_accessor, false_before_completion) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
  const char *partial = "HTTP/1.1 200 OK\r\n";
  chttp1_parser_execute(&parser, partial, strlen(partial));
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
}

TEST(message_complete_accessor, true_after_execute_returns_paused) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_parser_message_complete(&parser));
}

TEST(message_complete_accessor,
     true_after_finish_completes_eof_delimited_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *headers = "HTTP/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_parser_message_complete(&parser));
}

/* ========================================================================== */
/*          max_chunk_size_override / chttp1_chunk_size_limit_exceeded       */
/* ========================================================================== */

TEST(chunk_size_limit, unset_override_accepts_any_size_that_fits) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *headers =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  /* max_chunk_size_override defaults to 0 (no cap): even a large chunk-size
   * token is accepted as a syntactically valid line with nothing to compare
   * it against. */
  const char *chunk_size = "ffffffff\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, chunk_size, strlen(chunk_size)),
             CHTTP1_OK);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
}

TEST(chunk_size_limit, override_rejects_oversized_chunk_before_reading_data) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_chunk_size_override = 1024;
  const char *headers =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  /* Declares a chunk far larger than the override and never sends any of
   * its data: rejection must happen the instant the chunk-size line itself
   * is parsed, not after waiting (forever) for data that will never come. */
  const char *chunk_size = "8000000000000000\r\n"; /* ~9.2 exabytes */
  REQUIRE_EQ(chttp1_parser_execute(&parser, chunk_size, strlen(chunk_size)),
             CHTTP1_ERROR);
  REQUIRE_TRUE(chttp1_chunk_size_limit_exceeded(&parser));
}

TEST(chunk_size_limit, override_accepts_chunk_at_exactly_the_limit) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_chunk_size_override = 5;
  const char *headers =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  const char *rest = "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, rest, strlen(rest)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

TEST(chunk_size_limit, not_set_for_an_unrelated_parse_error) {
  /* chttp1_chunk_size_limit_exceeded() must be false for every OTHER
   * CHTTP1_ERROR cause, not merely default-initialised false; confirmed by
   * triggering a genuinely different rejection (a malformed status line)
   * and checking the flag afterward. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "GARBAGE\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
}

/* ========================================================================== */
/*                     EXPECT: 100-CONTINUE DETECTION                        */
/* ========================================================================== */

TEST(expect_continue, detected_with_body) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n"
      "hello";
  REQUIRE_FALSE(chttp1_expects_continue(&parser));
  chttp1_parser_execute(&parser, msg, strlen(msg));
  REQUIRE_TRUE(chttp1_expects_continue(&parser));
}

TEST(expect_continue, absent_by_default) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  chttp1_parser_execute(&parser, msg, strlen(msg));
  REQUIRE_FALSE(chttp1_expects_continue(&parser));
}

TEST(expect_continue, other_expect_value_not_detected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nExpect: something-else\r\n\r\n";
  chttp1_parser_execute(&parser, msg, strlen(msg));
  REQUIRE_FALSE(chttp1_expects_continue(&parser));
}

TEST(expect_continue, works_with_divert) {
  /* The whole point: a caller diverting body ingestion to a worker thread
   * still needs to know, at CHTTP1_HEADERS_ONLY time, whether to have
   * already written a "100 Continue" interim response. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_TRUE(chttp1_expects_continue(&parser));
}

/* ========================================================================== */
/*                WORKER-PULL STREAMING (chttp1_stream_t)                    */
/* ========================================================================== */

static void make_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

/* Non-blocking variant, required for the stream_tls tests below: a TLS
 * handshake driven by tls_drive_handshake()'s single-threaded ping-pong
 * loop (call one side's step, then the other's, repeat) deadlocks on a
 * blocking socketpair; SSL_accept/SSL_connect's internal BIO_read can
 * itself block waiting for bytes the peer never gets a chance to send,
 * since driving that peer's own step is exactly what this thread would do
 * next, if it weren't already stuck. Confirmed via gdb (a real hang
 * reproduced during this test's own development, not a hypothetical): the
 * backtrace showed ctls_conn_handshake_step blocked inside a plain
 * BIO_read -> read(2) syscall. The plaintext "stream" tests above don't
 * need this: they always gate any read/write with chttp1_stream_read/
 * _write's own poll(2) call first (or, for the handful of direct read(2)/
 * write(2) calls, only ever touch a fd after the peer has already
 * synchronously written to it in the same thread), so blocking-mode
 * sockets never actually block there. */
static void make_nonblocking_pair(int fds[2]) {
  make_pair(fds);
  for (int i = 0; i < 2; i++) {
    /* An unchecked failure here would leave fds[i] blocking; this file's own
     * comment right above this function explains why that specifically
     * deadlocks tls_drive_handshake()'s single-threaded ping-pong loop
     * (confirmed via gdb during this test's own development), so a silent
     * fcntl() failure here would turn into a real, hard-to-diagnose hang
     * rather than a clean, immediate test failure. */
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

TEST(stream, prepare_no_leftover) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  REQUIRE_EQ(s.carry_len, (size_t)0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, leftover_drained_before_touching_fd) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "hello", 5));

  char buf[3] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, 3, 100), (ssize_t)3);
  REQUIRE_EQ(memcmp(buf, "hel", 3), 0);

  char buf2[10] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf2, 10, 100), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf2, "lo", 2), 0);

  /* Carry-over now fully drained; confirm a further read reaches the real
   * fd rather than returning more (nonexistent) carry-over. */
  REQUIRE_EQ(write(fds[1], "X", 1), (ssize_t)1);
  char buf3[4] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf3, sizeof(buf3), 1000), (ssize_t)1);
  REQUIRE_EQ(buf3[0], 'X');

  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, read_real_fd_no_leftover) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  REQUIRE_EQ(write(fds[1], "abc", 3), (ssize_t)3);
  char buf[8] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 1000), (ssize_t)3);
  REQUIRE_EQ(memcmp(buf, "abc", 3), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, read_eof_when_peer_closes) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  close(fds[1]);
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 1000), (ssize_t)0);
  chttp1_stream_release(&s);
  close(fds[0]);
}

TEST(stream, read_timeout_when_nothing_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 50), (ssize_t)-1);
  REQUIRE_TRUE(chttp1_stream_timed_out(&s));
  REQUIRE_EQ(chttp1_stream_last_error(&s), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

/* Regression coverage for timeout_ms == 0's own documented contract ("return
 * immediately if fd is not already readable/writable right now"): a bug
 * found via code review had both chttp1_stream_write (both its TLS and
 * plaintext branches, via their one shared retry loop) and chttp1_stream_
 * read's plaintext branch compute a fresh "now + timeout_ms" deadline and
 * then immediately re-derive "time remaining until it" via a SECOND
 * clock_gettime() call, before ever calling poll(2) or attempting the real
 * I/O; for timeout_ms == 0 specifically, any nonzero elapsed time between
 * those two clock reads (guaranteed, however small) already exceeds a
 * zero-length budget, so that recomputed value was unconditionally <= 0.
 * The result: both functions unconditionally reported a timeout for
 * timeout_ms == 0, even when the fd was already ready right now, without
 * ever calling poll(2) or attempting the real read(2)/write(2) at all. The
 * three tests below construct exactly that "already ready" condition (data
 * already sitting in the socket's receive buffer for read; an ordinary,
 * freshly-connected socketpair, which is always immediately writable, for
 * write) and confirm a real transfer happens instead of a synthesized
 * timeout. */
TEST(stream, read_timeout_zero_returns_data_when_already_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  /* Written synchronously, from this same thread, before the read below: by
     the time chttp1_stream_read runs, these bytes are already sitting in
     fds[0]'s own kernel receive buffer, i.e. genuinely available "right
     now" with no wait required at all. */
  REQUIRE_EQ(write(fds[1], "hi", 2), (ssize_t)2);
  char buf[4] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 0), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  REQUIRE_FALSE(chttp1_stream_timed_out(&s));
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, read_timeout_zero_times_out_when_nothing_available) {
  /* The other half of timeout_ms == 0's contract: nothing available means an
     immediate, single, non-blocking check correctly reports a timeout (not a
     hang, and not a spurious successful read of zero bytes). */
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0));
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 0), (ssize_t)-1);
  REQUIRE_TRUE(chttp1_stream_timed_out(&s));
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, write_basic) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[1], NULL, 0));
  REQUIRE_EQ(chttp1_stream_write(&s, "hi", 2, 1000), (ssize_t)2);
  char buf[4] = {0};
  REQUIRE_EQ(read(fds[0], buf, sizeof(buf)), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, write_timeout_zero_succeeds_when_already_writable) {
  /* See read_timeout_zero_returns_data_when_already_available's own doc
     comment above for the full account of the bug this pins. A freshly
     connected socketpair endpoint is always immediately writable (its send
     buffer starts empty), so this is genuinely "writable right now" with no
     wait required. */
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[1], NULL, 0));
  REQUIRE_EQ(chttp1_stream_write(&s, "hi", 2, 0), (ssize_t)2);
  REQUIRE_FALSE(chttp1_stream_timed_out(&s));
  char buf[4] = {0};
  REQUIRE_EQ(read(fds[0], buf, sizeof(buf)), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, read_error_on_bad_fd) {
  /* A negative fd is specially ignored by poll(2) itself (POSIX: an entry
   * with fd < 0 is never reported ready, so it would just silently time
   * out here, not error); a real invalid-fd error needs a syntactically
   * valid but already-closed fd number instead, which poll(2) reports
   * ready with POLLNVAL for, and the subsequent read(2) then genuinely
   * fails with EBADF. */
  int fds[2];
  make_pair(fds);
  int bad_fd = fds[0];
  close(fds[0]);
  close(fds[1]);

  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, bad_fd, NULL, 0));
  char buf[8];
  ssize_t r = chttp1_stream_read(&s, buf, sizeof(buf), 1000);
  REQUIRE_EQ(r, (ssize_t)-1);
  REQUIRE_FALSE(chttp1_stream_timed_out(&s));
  REQUIRE_NE(chttp1_stream_last_error(&s), 0);
  chttp1_stream_release(&s);
}

TEST(stream, release_is_idempotent) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1));
  chttp1_stream_release(&s);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_prepends_ahead_of_existing_carry) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "world", 5));
  REQUIRE_TRUE(chttp1_stream_push_back_leftover(&s, "hello ", 6));

  char buf[11] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 100), (ssize_t)11);
  REQUIRE_EQ(memcmp(buf, "hello world", 11), 0);

  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_zero_len_is_a_noop) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1));
  REQUIRE_TRUE(chttp1_stream_push_back_leftover(&s, "unused", 0));
  REQUIRE_EQ(s.carry_len, (size_t)1);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_overflow_guard_rejects_without_allocating) {
  /* Regression test: total = len + existing had no overflow check before
   * allocating `total` bytes; two independently sized values (a freshly
   * pushed-back chunk and whatever carry-over the stream already held)
   * summing close to SIZE_MAX would previously proceed with a wrapped
   * allocation and then write far past it. existing is faked to SIZE_MAX
   * directly on the struct (carry left NULL; no real buffer of that size is
   * ever allocated or touched), matching this project's own established
   * "assert the guard rejects before any real work happens" pattern for
   * this exact class of overflow guard. */
  chttp1_stream_t s;
  memset(&s, 0, sizeof(s));
  s.prepared = true;
  s.carry = NULL;
  s.carry_len = SIZE_MAX;
  s.carry_pos = 0;

  char buf[4] = {'a', 'b', 'c', 'd'};
  REQUIRE_FALSE(chttp1_stream_push_back_leftover(&s, buf, sizeof(buf)));
  /* Existing carry-over must be left completely untouched on rejection. */
  REQUIRE_EQ((void *)s.carry, NULL);
  REQUIRE_EQ(s.carry_len, (size_t)SIZE_MAX);
}

/* ========================================================================== */
/*              WORKER-PULL STREAMING OVER A REAL TLS CONNECTION             */
/* ========================================================================== */

static bool tls_drive_handshake(ctls_conn_t *a, ctls_conn_t *b) {
  bool a_done = false, b_done = false;
  for (int i = 0; i < 200 && !(a_done && b_done); i++) {
    if (!a_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) a_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) return false;
    }
    if (!b_done) {
      ctls_handshake_result_t r = ctls_conn_handshake_step(b);
      if (r == CTLS_HANDSHAKE_DONE) b_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) return false;
    }
  }
  return a_done && b_done;
}

TEST(stream_tls, read_over_real_tls_connection) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  REQUIRE_EQ(ctls_conn_write(client_conn, "hello", 5), (ssize_t)5);

  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare_tls(&s, fds[0], server_conn, NULL, 0));
  char buf[16] = {0};
  ssize_t got = -1;
  for (int i = 0; i < 100 && got <= 0; i++)
    got = chttp1_stream_read(&s, buf, sizeof(buf), 1000);
  REQUIRE_EQ(got, (ssize_t)5);
  REQUIRE_EQ(memcmp(buf, "hello", 5), 0);

  chttp1_stream_release(&s);
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(stream_tls, write_over_real_tls_connection) {
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare_tls(&s, fds[0], server_conn, NULL, 0));
  REQUIRE_EQ(chttp1_stream_write(&s, "response body", 14, 1000), (ssize_t)14);

  char buf[32] = {0};
  ssize_t got = -1;
  for (int i = 0; i < 100 && got <= 0; i++)
    got = ctls_conn_read(client_conn, buf, sizeof(buf));
  REQUIRE_EQ(got, (ssize_t)14);
  REQUIRE_EQ(memcmp(buf, "response body", 14), 0);

  chttp1_stream_release(&s);
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}

TEST(stream_tls, leftover_decrypted_bytes_drained_before_ctls_conn_read) {
  /* Mirrors the plaintext carry-over test: leftover bytes here represent
   * already-decrypted application bytes the reactor thread would have
   * produced via ctls_conn_read() during its own header-parsing loop;
   * chttp1_stream_prepare_tls's leftover argument is never raw wire bytes. */
  ctls_ctx_t *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx_t *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn_t *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn_t *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  chttp1_stream_t s;
  REQUIRE_TRUE(
      chttp1_stream_prepare_tls(&s, fds[0], server_conn, "carried", 7));
  char buf[16] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 1000), (ssize_t)7);
  REQUIRE_EQ(memcmp(buf, "carried", 7), 0);

  REQUIRE_EQ(ctls_conn_write(client_conn, "next", 4), (ssize_t)4);
  char buf2[16] = {0};
  ssize_t got = -1;
  for (int i = 0; i < 100 && got <= 0; i++)
    got = chttp1_stream_read(&s, buf2, sizeof(buf2), 1000);
  REQUIRE_EQ(got, (ssize_t)4);
  REQUIRE_EQ(memcmp(buf2, "next", 4), 0);

  chttp1_stream_release(&s);
  ctls_conn_destroy(client_conn);
  ctls_conn_destroy(server_conn);
  close(fds[0]);
  close(fds[1]);
  ctls_ctx_release(client_ctx);
  ctls_ctx_release(server_ctx);
}
