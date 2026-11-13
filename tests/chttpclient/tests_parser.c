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

// White-box tests of chttp1_parser at the byte level. They drive the parser
// directly (no TLS, no chttpclient.c) on hand-built input that is sometimes
// malformed or hostile, which the well-formed mock server of tests.c never
// sends. The "stream" group uses local AF_UNIX socketpairs only to drive the
// read, write and poll logic of chttp1_stream.

#include <fcntl.h>
#include <internal/chttp1_parser.h>
#include <internal/ctls.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "../tau/tau.h"

TAU_MAIN()

// The capacity of test_ctx_t: The header and trailer lines and the body bytes
// it keeps, large enough for every message whose fields the tests check.
#define MAX_TEST_HEADERS 32
#define TEST_BODY_CAP (256 * 1024)

// What the test callbacks record from one message, and the switches that make
// them fail or divert.
typedef struct {
  int status_code;
  bool is_head;
  bool force_on_header_error;
  bool force_on_trailer_error;
  bool force_on_body_error;
  bool force_on_headers_complete_error;
  // Makes on_headers_complete return CHTTP1_HEADERS_DIVERT_BODY.
  bool want_divert;

  char header_names[MAX_TEST_HEADERS][256];
  char header_values[MAX_TEST_HEADERS][256];
  size_t header_count;

  // Trailers are kept apart from the headers, so a test can check that a
  // trailer never reaches on_header.
  char trailer_names[MAX_TEST_HEADERS][256];
  char trailer_values[MAX_TEST_HEADERS][256];
  size_t trailer_count;

  char body[TEST_BODY_CAP];
  size_t body_len;

  bool headers_complete_called;
  bool message_complete_called;

  // Request mode only.
  bool request_line_called;
  bool force_on_request_line_error;
  char method[32];
  char target[256];
} test_ctx_t;

// Parser callbacks that record what they get in the test_ctx_t behind p->data
// (names and values cut at 255 bytes), so the tests can check what the parser
// delivered. Each one fails when its force_on_*_error flag is set, for the
// tests of the CHTTP1_USER path. on_headers_complete also picks the body
// outcome from want_divert and is_head.
static int t_on_header(chttp1_parser *p, const char *name, size_t name_len,
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

static int t_on_trailer(chttp1_parser *p, const char *name, size_t name_len,
                        const char *value, size_t value_len) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  if (ctx->force_on_trailer_error) return 1;
  if (ctx->trailer_count < MAX_TEST_HEADERS) {
    size_t nl = name_len < 255 ? name_len : 255;
    size_t vl = value_len < 255 ? value_len : 255;
    memcpy(ctx->trailer_names[ctx->trailer_count], name, nl);
    ctx->trailer_names[ctx->trailer_count][nl] = '\0';
    memcpy(ctx->trailer_values[ctx->trailer_count], value, vl);
    ctx->trailer_values[ctx->trailer_count][vl] = '\0';
    ctx->trailer_count++;
  }
  return 0;
}

static int t_on_headers_complete(chttp1_parser *p) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  ctx->headers_complete_called = true;
  ctx->status_code = p->status_code;
  if (ctx->force_on_headers_complete_error) return -1;
  if (ctx->want_divert) return CHTTP1_HEADERS_DIVERT_BODY;
  return ctx->is_head ? CHTTP1_HEADERS_NO_BODY : CHTTP1_HEADERS_HAS_BODY;
}

static int t_on_request_line(chttp1_parser *p, const char *method,
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

static int t_on_body(chttp1_parser *p, const char *at, size_t len) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  if (ctx->force_on_body_error) return 1;
  if (ctx->body_len + len <= sizeof(ctx->body)) {
    memcpy(ctx->body + ctx->body_len, at, len);
    ctx->body_len += len;
  }
  return 0;
}

static int t_on_message_complete(chttp1_parser *p) {
  test_ctx_t *ctx = (test_ctx_t *)p->data;
  ctx->message_complete_called = true;
  return 0;
}

// The response-mode callbacks. They have static storage because
// chttp1_parser_init borrows the pointer and the settings must outlive the
// parser.
static const chttp1_settings g_test_settings = {
    .on_header = t_on_header,
    .on_trailer = t_on_trailer,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

// Clears ctx and initializes parser in response mode on it; every response test
// starts here.
static void init_test(chttp1_parser *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init(parser, &g_test_settings);
  parser->data = ctx;
}

// The request-mode callbacks, static for the same reason.
static const chttp1_settings g_test_request_settings = {
    .on_request_line = t_on_request_line,
    .on_header = t_on_header,
    .on_trailer = t_on_trailer,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

// Clears ctx and initializes parser in request mode on it; every request test
// starts here.
static void init_test_request(chttp1_parser *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init_request(parser, &g_test_request_settings);
  parser->data = ctx;
}

// Gives the value of the first recorded header called name, or NULL; the tests
// use it to check single fields.
static const char *find_header(const test_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->header_count; i++)
    if (strcmp(ctx->header_names[i], name) == 0) return ctx->header_values[i];
  return NULL;
}

// Gives the value of the first recorded trailer called name, or NULL; the
// trailer tests use it.
static const char *find_trailer(const test_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->trailer_count; i++)
    if (strcmp(ctx->trailer_names[i], name) == 0) return ctx->trailer_values[i];
  return NULL;
}

// Gives the value of header name, or a recognizable text when it's missing.
// REQUIRE_STREQ is strcmp, and a NULL there would crash the whole binary
// instead of failing one test.
static const char *header_or_absent(const test_ctx_t *ctx, const char *name) {
  const char *v = find_header(ctx, name);
  return v ? v : "(absent)";
}

// Gives the value of trailer name, or a recognizable text when it's missing,
// for the same reason as header_or_absent().
static const char *trailer_or_absent(const test_ctx_t *ctx, const char *name) {
  const char *v = find_trailer(ctx, name);
  return v ? v : "(absent)";
}

TEST(status_line, basic_with_reason) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 200);
}

// RFC 7230 wants the SP even before an empty reason phrase, but real servers
// send "HTTP/1.1 304\r\n".
TEST(status_line, missing_reason_and_trailing_space_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 304\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 304);
}

TEST(status_line, any_major_minor_digit_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/2.0 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 200);
}

// Codes up to 599 are in real use (599 Network Connect Timeout Error), so any
// 3-digit code is accepted.
TEST(status_line, nonstandard_but_wellformed_code_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 599 Network Connect Timeout\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.status_code, 599);
}

TEST(status_line, lowercase_protocol_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "http/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, garbled_prefix_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "NOPE/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, two_digit_code_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 20 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, four_digit_code_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 2000 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, non_numeric_code_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 2AB OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, bare_lf_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(status_line, split_across_every_byte_boundary) {
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  size_t len = strlen(msg);
  for (size_t split = 1; split < len; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED) continue;
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno rv2 = chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_EQ(ctx.status_code, 200);
    REQUIRE_EQ(ctx.body_len, (size_t)5);
    REQUIRE_STREQ(ctx.body, "hello");
  }
}

TEST(keep_alive, http_1_1_default_is_keep_alive) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_1_connection_close_overrides_default) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_0_default_is_not_keep_alive) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(keep_alive, http_1_0_connection_keep_alive_overrides_default) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.0 200 OK\r\nContent-Length: 5\r\nConnection: keep-alive\r\n\r\n"
      "hello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

// "HTTP/1.1 or later" means major > 1, or major 1 with minor >= 1. Reading a
// zero minor as HTTP/1.0 would make "HTTP/2.0" (which the grammar accepts) ask
// for an explicit keep-alive token, and connection reuse would stop.
TEST(keep_alive, version_greater_than_1_with_zero_minor_defaults_keep_alive) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/3.0 200 OK\r\nContent-Length: 5\r\nConnection: close\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(headers, basic_and_case_preserved_on_name) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: Bar\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "Bar");
}

TEST(headers, ows_trimmed_from_value) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Foo: \t  Bar  \t\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "Bar");
}

TEST(headers, missing_colon_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, empty_name_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\n: Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, control_char_in_name_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char msg[] = "HTTP/1.1 200 OK\r\nX-\x01Foo: Bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, sizeof(msg) - 1),
             CHTTP1_ERROR);
}

TEST(headers, embedded_nul_in_value_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char msg[] = "HTTP/1.1 200 OK\r\nX-Foo: a\0b\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, sizeof(msg) - 1),
             CHTTP1_ERROR);
}

TEST(headers, obsolete_line_folding_rejected) {
  chttp1_parser parser;
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
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED) continue;
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno rv2 = chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_STREQ(find_header(&ctx, "X-Foo"), "bar");
    REQUIRE_STREQ(ctx.body, "abc");
  }
}

TEST(headers, on_header_callback_error_reports_user) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_header_error = true;
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo: bar\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(headers, on_headers_complete_callback_error_reports_user) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_headers_complete_error = true;
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

// Writes "X-Pad: ", value_len 'a' bytes and CRLF into buf, for the tests of
// the line length cap.
static void build_header_line(char *buf, size_t buf_size, size_t value_len) {
  size_t n = (size_t)snprintf(buf, buf_size, "X-Pad: ");
  memset(buf + n, 'a', value_len);
  n += value_len;
  buf[n++] = '\r';
  buf[n++] = '\n';
  buf[n] = '\0';
}

// The raw line is "X-Pad: " (7) + value + CRLF (2), so with
// CHTTP1_MAX_LINE_LEN 8192 the biggest value is 8183 bytes.
TEST(headers, line_at_cap_boundary_accepted_one_more_rejected) {
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

  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_PAUSED);
}

TEST(headers, line_one_byte_over_cap_rejected) {
  char status_and_headers_prefix[64];
  snprintf(status_and_headers_prefix, sizeof(status_and_headers_prefix),
           "HTTP/1.1 200 OK\r\n");

  char line[8300];
  build_header_line(line, sizeof(line), 8184);

  char msg[8600];
  size_t off =
      (size_t)snprintf(msg, sizeof(msg), "%s", status_and_headers_prefix);
  memcpy(msg + off, line, strlen(line));
  off += strlen(line);

  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

TEST(headers, too_many_headers_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);

  char msg[16384];
  size_t off = (size_t)snprintf(msg, sizeof(msg), "HTTP/1.1 200 OK\r\n");
  for (int i = 0; i < 101; i++) {
    off += (size_t)snprintf(msg + off, sizeof(msg) - off, "X-H%d: v\r\n", i);
  }
  off += (size_t)snprintf(msg + off, sizeof(msg) - off, "\r\n");

  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, off), CHTTP1_ERROR);
}

// 80 headers of about 1000 bytes pass the 64 KB total cap and neither the count
// cap nor the line cap. One line goes in per call, so the test stops at the
// first error.
TEST(headers, total_header_bytes_cap_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);

  const char *status_line = "HTTP/1.1 200 OK\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, status_line, strlen(status_line)),
             CHTTP1_OK);

  bool got_error = false;
  for (int i = 0; i < 80; i++) {
    char line[1100];
    build_header_line(line, sizeof(line), 1000);
    chttp1_errno rv = chttp1_parser_execute(&parser, line, strlen(line));
    if (rv == CHTTP1_ERROR) {
      got_error = true;
      break;
    }
    REQUIRE_EQ(rv, CHTTP1_OK);
  }
  REQUIRE_TRUE(got_error);
}

TEST(headers, max_header_count_override_rejects_below_builtin_default) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_header_count_override = 2;

  const char *msg = "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(headers, max_header_count_override_allows_up_to_the_override) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_header_count_override = 4;

  const char *msg =
      "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-B: 2\r\nX-C: 3\r\nContent-Length: "
      "0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
}

TEST(headers, max_total_header_bytes_override_rejects_below_builtin_default) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  parser.max_total_header_bytes_override = 32;

  const char *msg =
      "HTTP/1.1 200 OK\r\nX-Long-Header-Name: some longer value here\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// max_header_bytes bounds the request line plus the header lines, so the
// request line adds to the total itself. Otherwise a long request-target is
// bounded only by CHTTP1_MAX_LINE_LEN.
TEST(headers,
     request_line_itself_counts_against_max_total_header_bytes_override) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_total_header_bytes_override = 32;

  char target[512];
  memset(target, 'a', sizeof(target) - 1);
  target[sizeof(target) - 1] = '\0';
  char msg[600];
  snprintf(msg, sizeof(msg), "GET /%s HTTP/1.1\r\n\r\n", target);
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// Each line fits alone and their sum doesn't, which shows that both add to one
// running total.
TEST(headers, request_line_and_headers_share_the_same_total_bytes_budget) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_total_header_bytes_override = 40;

  const char *first_line = "GET / HTTP/1.1\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, first_line, strlen(first_line)),
             CHTTP1_OK);
  const char *header_line = "X-Long-Header-Name: some longer value\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, header_line, strlen(header_line)),
             CHTTP1_ERROR);
}

TEST(framing, duplicate_content_length_same_value_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, duplicate_content_length_differing_value_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nContent-Length: 6\r\n\r\nhello!";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, content_length_and_chunked_conflict_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 5\r\nTransfer-Encoding: "
      "chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing, content_length_decimal_overflow_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 99999999999999999999999\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// RFC 7230 SS3.3.3: A response whose final coding isn't chunked is read until
// the connection closes. A request in that state is refused (see
// request_body_framing).
TEST(framing, transfer_encoding_not_chunked_reads_until_eof) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nsome-bytes";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "some-bytes");
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

// A Transfer-Encoding beside a Content-Length is ambiguous (RFC 7230 SS3.3.3).
// A recipient that framed it at Content-Length would leave the rest on a pooled
// connection, where the next request reads it as its own response. Narrowed to
// request mode, this test fails with CHTTP1_PAUSED, a 3-byte body and
// keep-alive.
TEST(framing,
     response_content_length_with_non_chunked_transfer_encoding_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nContent-Length: "
      "3\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// The order doesn't matter: The decision is made after every
// Transfer-Encoding line.
TEST(framing,
     response_non_chunked_transfer_encoding_before_content_length_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: "
      "gzip\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// The merged list ends in gzip, so this is the same ambiguous pair although
// "chunked" appears in it.
TEST(framing,
     response_content_length_with_chunked_then_non_chunked_te_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "gzip\r\nContent-Length: 3\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// The legal counterpart: With no Content-Length nothing disagrees, and the
// response is read until EOF and never kept alive.
TEST(framing,
     response_non_chunked_transfer_encoding_alone_still_reads_until_eof) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "gzip\r\n\r\nsome-bytes";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "some-bytes");
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

// The two lines merge into "chunked, gzip, chunked", which applies chunked
// twice (RFC 7230 SS3.3.1). Recipients that de-chunk once and twice disagree on
// where the body ends, which is what response smuggling needs. Without counting
// over the merged list the parser takes an ordinary chunked response.
TEST(framing, response_chunked_applied_twice_across_lines_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, "
      "gzip\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(framing, response_chunked_repeated_on_two_lines_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

// The parser counts codings, not header lines.
TEST(framing, response_chunked_twice_within_one_line_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(framing, response_chunked_applied_twice_is_case_insensitive) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\nTransfer-Encoding: "
      "CHUNKED\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

// The list ends in chunked but still applies it twice.
TEST(framing, response_chunked_last_after_an_earlier_chunked_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, "
      "chunked\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

// A response may apply chunked once somewhere other than last and is then read
// until EOF, which is why the parser counts occurrences instead of checking the
// position.
TEST(framing, response_single_nonfinal_chunked_still_reads_until_eof) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked, gzip\r\n\r\nsome-bytes";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "some-bytes");
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(framing, response_transfer_encoding_ending_in_chunked_still_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "abc");
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

// A Transfer-Encoding line that names no coding only adds empty elements to the
// merged list, which RFC 7230 SS7 has a recipient ignore. So "chunked" followed
// by such a line is still "chunked", like the single line "chunked,". These
// cases pin that in both modes, and that the rules about the merged list see
// through the empty line wherever it sits.
//
// One case: A label and the Transfer-Encoding lines.
typedef struct {
  const char *label;
  // Each line ends in CRLF.
  const char *te_lines;
} te_empty_case_t;

// Lists that are "chunked" once the empty elements are dropped.
static const te_empty_case_t g_te_lists_equal_to_chunked[] = {
    {"chunked, then empty",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding:\r\n"},
    {"chunked, then comma",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding: ,\r\n"},
    {"chunked, then OWS and commas",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding:  , \t,\r\n"},
    {"empty, then chunked",
     "Transfer-Encoding:\r\n"
     "Transfer-Encoding: chunked\r\n"},
    {"gzip, empty, chunked",
     "Transfer-Encoding: gzip\r\n"
     "Transfer-Encoding: ,\r\n"
     "Transfer-Encoding: chunked\r\n"},
    {"gzip then chunked, then empty",
     "Transfer-Encoding: gzip\r\n"
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding:\r\n"},
};

// Lists that end in a coding other than chunked.
static const te_empty_case_t g_te_lists_not_ending_in_chunked[] = {
    {"gzip, then empty",
     "Transfer-Encoding: gzip\r\n"
     "Transfer-Encoding:\r\n"},
    {"chunked, empty, gzip",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding: ,\r\n"
     "Transfer-Encoding: gzip\r\n"},
};

// Lists that apply chunked twice.
static const te_empty_case_t g_te_lists_chunked_twice[] = {
    {"chunked, empty, chunked",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding:\r\n"
     "Transfer-Encoding: chunked\r\n"},
    {"chunked, comma, CHUNKED",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding: ,\r\n"
     "Transfer-Encoding: CHUNKED\r\n"},
};

// Evaluates to the number of cases in the case array a, so the loops over the
// arrays don't repeat their sizes.
#define TE_CASE_COUNT(a) (sizeof(a) / sizeof((a)[0]))

// Builds a message from a start line, the Transfer-Encoding lines of a case,
// extra headers and a body into out, for the empty-element tests. out is empty
// when the message doesn't fit in cap.
static void build_te_message(char *out, size_t cap, const char *start_line,
                             const char *te_lines, const char *extra_headers,
                             const char *body) {
  int n = snprintf(out, cap, "%s%s%s\r\n%s", start_line, te_lines,
                   extra_headers, body);
  if (n < 0 || (size_t)n >= cap) out[0] = '\0';
}

// The start lines and the chunked body that build_te_message() puts around the
// Transfer-Encoding lines of a case.
#define TE_RESPONSE_START "HTTP/1.1 200 OK\r\n"
#define TE_REQUEST_START "POST /u HTTP/1.1\r\nHost: h\r\n"
#define TE_CHUNKED_BODY "3\r\nabc\r\n0\r\n\r\n"

// If an empty line re-derived the final coding, these lists would take EOF
// framing and the chunk framing would reach the caller as body bytes.
TEST(framing, response_empty_te_line_keeps_the_list_chunked) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_RESPONSE_START,
                     g_te_lists_equal_to_chunked[i].te_lines, "",
                     TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    chttp1_errno rc = chttp1_parser_execute(&parser, msg, strlen(msg));
    if (rc != CHTTP1_PAUSED || strcmp(ctx.body, "abc") != 0)
      printf("case '%s': rc=%d body=[%s]\n",
             g_te_lists_equal_to_chunked[i].label, (int)rc, ctx.body);
    REQUIRE_EQ(rc, CHTTP1_PAUSED);
    REQUIRE_STREQ(ctx.body, "abc");
    REQUIRE_TRUE(ctx.message_complete_called);
    REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
  }
}

TEST(framing, response_empty_te_line_does_not_make_a_list_chunked) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_not_ending_in_chunked); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_RESPONSE_START,
                     g_te_lists_not_ending_in_chunked[i].te_lines, "",
                     "some-bytes");
    REQUIRE_NE(msg[0], '\0');
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
    REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
    REQUIRE_STREQ(ctx.body, "some-bytes");
    REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
  }
}

TEST(framing, response_empty_te_line_does_not_hide_chunked_applied_twice) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_chunked_twice); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_RESPONSE_START,
                     g_te_lists_chunked_twice[i].te_lines, "", TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
    REQUIRE_EQ(ctx.body_len, (size_t)0);
    REQUIRE_FALSE(ctx.message_complete_called);
  }
}

TEST(framing, response_empty_te_line_with_content_length_rejected) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    for (int cl_first = 0; cl_first < 2; cl_first++) {
      chttp1_parser parser;
      test_ctx_t ctx;
      init_test(&parser, &ctx);
      char msg[512];
      char start[128];
      snprintf(start, sizeof(start), "%s%s", TE_RESPONSE_START,
               cl_first ? "Content-Length: 3\r\n" : "");
      build_te_message(
          msg, sizeof(msg), start, g_te_lists_equal_to_chunked[i].te_lines,
          cl_first ? "" : "Content-Length: 3\r\n", TE_CHUNKED_BODY);
      REQUIRE_NE(msg[0], '\0');
      REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)),
                 CHTTP1_ERROR);
      REQUIRE_FALSE(ctx.message_complete_called);
    }
  }
}

// If an empty line re-derived the final coding, the request would read as a
// Transfer-Encoding that doesn't end in chunked and be refused.
TEST(request_body_framing, empty_te_line_keeps_the_list_chunked) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_REQUEST_START,
                     g_te_lists_equal_to_chunked[i].te_lines, "",
                     TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    chttp1_errno rc = chttp1_parser_execute(&parser, msg, strlen(msg));
    if (rc != CHTTP1_PAUSED)
      printf("case '%s': rc=%d reason=%s\n",
             g_te_lists_equal_to_chunked[i].label, (int)rc,
             parser.reason ? parser.reason : "-");
    REQUIRE_EQ(rc, CHTTP1_PAUSED);
    REQUIRE_STREQ(ctx.body, "abc");
    REQUIRE_TRUE(ctx.message_complete_called);
    REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
  }
}

TEST(request_body_framing, empty_te_line_does_not_make_a_list_chunked) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_not_ending_in_chunked); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_REQUEST_START,
                     g_te_lists_not_ending_in_chunked[i].te_lines, "",
                     TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
    REQUIRE_FALSE(ctx.message_complete_called);
  }
}

TEST(request_body_framing, empty_te_line_does_not_hide_chunked_applied_twice) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_chunked_twice); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_REQUEST_START,
                     g_te_lists_chunked_twice[i].te_lines, "", TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
    REQUIRE_FALSE(ctx.message_complete_called);
  }
}

TEST(request_body_framing, empty_te_line_with_content_length_rejected) {
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_REQUEST_START,
                     g_te_lists_equal_to_chunked[i].te_lines,
                     "Content-Length: 3\r\n", TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
    REQUIRE_FALSE(ctx.message_complete_called);
  }
}

TEST(chunked, basic) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "3\r\nfoo\r\n3\r\nbar\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "foobar");
}

TEST(chunked, non_hex_chunk_size_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\nZZZZ\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, hex_multiply_overflow_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "FFFFFFFFFFFFFFFFFFFFF\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, extension_accepted_and_discarded) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5;ext=value\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(chunked, terminal_chunk_with_trailers) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Trailer: trailer-value\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Trailer"), "trailer-value");
  REQUIRE_TRUE(find_header(&ctx, "X-Trailer") == NULL);
}

TEST(chunked, terminal_chunk_without_trailers) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(chunked, missing_crlf_after_chunk_data_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhelloXX0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_chunk_size_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_chunk_data_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(chunked, eof_mid_trailer_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Trail";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

// Trailers (RFC 7230 SS4.1.2) arrive once the framing is settled and, on a
// server, after routing. They come through their own callback and change
// nothing in how the parser sees the message. chttpclient.c puts every header
// in one map, whose insert overwrites.
TEST(trailer_isolation, trailer_does_not_reach_on_header_in_response_mode) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "X-Origin: upstream\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Origin: forged\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Origin"), "upstream");
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Origin"), "forged");
}

// The case the split closes: A proxy sets X-Forwarded-For and a gateway sets
// X-Authenticated-User, and the client appends trailers of the same names. Both
// header values must survive. The Host line is required in request mode.
TEST(trailer_isolation, trailer_does_not_reach_on_header_in_request_mode) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
      "X-Forwarded-For: 203.0.113.9\r\n"
      "X-Authenticated-User: guest\r\n\r\n"
      "5\r\nhello\r\n"
      "0\r\nX-Forwarded-For: 127.0.0.1\r\nX-Authenticated-User: admin\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Forwarded-For"), "203.0.113.9");
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Authenticated-User"), "guest");
  REQUIRE_EQ(ctx.header_count, (size_t)4);
  REQUIRE_EQ(ctx.trailer_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Forwarded-For"), "127.0.0.1");
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Authenticated-User"), "admin");
}

// A trailing Transfer-Encoding, Content-Length or Connection must not clear
// chunked framing, slip past the mutual-exclusion check or close a keep-alive
// connection.
TEST(trailer_isolation, trailer_cannot_change_framing_or_connection_state) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nTransfer-Encoding: identity\r\nContent-Length: 100\r\n"
      "Connection: close\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ((int)chttp1_has_content_length(&parser), 0);
  REQUIRE_EQ(chttp1_declared_content_length(&parser), (uint64_t)0);
  REQUIRE_EQ((int)chttp1_should_keep_alive(&parser), 1);
  REQUIRE_EQ(ctx.trailer_count, (size_t)3);
}

TEST(trailer_isolation, trailer_cannot_raise_expect_100_continue) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST / HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nExpect: 100-continue\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ((int)chttp1_expects_continue(&parser), 0);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "Expect"), "100-continue");
}

TEST(trailer_isolation, trailer_grammar_is_still_validated) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *bad_name =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX Bad: value\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, bad_name, strlen(bad_name)),
             CHTTP1_ERROR);

  chttp1_parser parser2;
  test_ctx_t ctx2;
  init_test(&parser2, &ctx2);
  const char *bad_value =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX-Bad: va\x01lue\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser2, bad_value, strlen(bad_value)),
             CHTTP1_ERROR);

  chttp1_parser parser3;
  test_ctx_t ctx3;
  init_test(&parser3, &ctx3);
  const char *no_colon =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nnot-a-field-line\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser3, no_colon, strlen(no_colon)),
             CHTTP1_ERROR);
}

TEST(trailer_isolation, on_trailer_error_reports_user) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_trailer_error = true;
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX-T: v\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

// The configuration that chttpclient.c and chttpserver.c ship: With no trailer
// callback a trailer is validated, charged against the caps and dropped.
TEST(trailer_isolation, unset_on_trailer_discards_trailers_without_error) {
  static const chttp1_settings no_trailer_settings = {
      .on_header = t_on_header,
      .on_headers_complete = t_on_headers_complete,
      .on_body = t_on_body,
      .on_message_complete = t_on_message_complete,
  };
  chttp1_parser parser;
  test_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  chttp1_parser_init(&parser, &no_trailer_settings);
  parser.data = &ctx;
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "X-Origin: upstream\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Origin: forged\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Origin"), "upstream");
  REQUIRE_EQ(ctx.trailer_count, (size_t)0);
}

// Trailers have no budget of their own.
TEST(chunked, trailer_bytes_count_against_same_header_caps) {
  chttp1_parser parser;
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

TEST(content_length_body, exact_match) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(msg));
  REQUIRE_STREQ(ctx.body, "hello");
}

TEST(content_length_body, eof_before_satisfied_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(content_length_body, trailing_garbage_not_consumed) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhelloEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  size_t consumed = chttp1_parser_consumed(&parser);
  REQUIRE_TRUE(consumed < strlen(msg));
  REQUIRE_STREQ(ctx.body, "hello");
}

// An EOF-framed body is never kept alive, whatever its Connection header says,
// because its end is the end of the connection.
TEST(eof_body, clean_termination_and_not_keep_alive) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\nConnection: keep-alive\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "hello");
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_FALSE(chttp1_should_keep_alive(&parser));
}

TEST(no_body, head_with_content_length_suppresses_body) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.is_head = true;
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 12345\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_1xx_no_body_even_with_content_length) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 102 Processing\r\nContent-Length: 5\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_204_no_body_without_content_length) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 204 No Content\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(no_body, status_304_no_body_with_content_length) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 304 Not Modified\r\nContent-Length: 100\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

// Parses msg once whole and again split at every byte, and requires the same
// status, headers, body, consumed count and keep-alive answer each time.
// Message-splitting bugs show up only with input cut at some boundary.
// execute() never gives CHTTP1_OK with bytes left over, so a first call that
// gives it consumed the whole first piece.
static void assert_fragmented_matches_oneshot(const char *msg, size_t len) {
  chttp1_parser ref_parser;
  test_ctx_t ref_ctx;
  init_test(&ref_parser, &ref_ctx);
  chttp1_errno ref_rv = chttp1_parser_execute(&ref_parser, msg, len);
  REQUIRE_EQ(ref_rv, CHTTP1_PAUSED);
  size_t ref_consumed = chttp1_parser_consumed(&ref_parser);
  bool ref_keep_alive = chttp1_should_keep_alive(&ref_parser);

  for (size_t split = 1; split < len; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    chttp1_errno rv1 = chttp1_parser_execute(&parser, msg, split);
    if (rv1 == CHTTP1_PAUSED) {
      REQUIRE_EQ(chttp1_parser_consumed(&parser), ref_consumed);
      continue;
    }
    REQUIRE_EQ(rv1, CHTTP1_OK);
    chttp1_errno rv2 = chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(rv2, CHTTP1_PAUSED);
    REQUIRE_EQ(ctx.status_code, ref_ctx.status_code);
    REQUIRE_EQ(ctx.header_count, ref_ctx.header_count);
    REQUIRE_EQ(ctx.body_len, ref_ctx.body_len);
    REQUIRE_TRUE(memcmp(ctx.body, ref_ctx.body, ctx.body_len) == 0);
    REQUIRE_EQ((int)chttp1_should_keep_alive(&parser), (int)ref_keep_alive);
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

TEST(finish_matrix, before_status_line_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_headers_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nX-Foo: bar\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_content_length_body_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nabc";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

TEST(finish_matrix, mid_chunk_is_unsafe) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n5\r\nhel";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_ERROR);
}

// chttpclient.c never calls finish after CHTTP1_PAUSED; this drives the
// CHTTP1_FINISH_SAFE case directly, which must not fire on_message_complete
// again.
TEST(finish_matrix, at_clean_boundary_is_safe_no_callback) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.0 200 OK\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

// CHTTP1_FINISH_SAFE_WITH_CB must update finish_state as well as the state.
// Otherwise a second finish() fires on_message_complete again, which must fire
// exactly once.
TEST(finish_matrix,
     calling_finish_twice_after_eof_delimited_body_does_not_refire_callback) {
  chttp1_parser parser;
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

TEST(misc, zero_length_execute_is_a_defined_noop) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_EQ(chttp1_parser_execute(&parser, "", 0), CHTTP1_OK);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
}

TEST(request_line, basic_get_no_body) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.method, "POST");
  REQUIRE_STREQ(ctx.target, "/submit");
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_line, query_string_preserved_in_target) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path?a=1&b=2 HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/path?a=1&b=2");
}

TEST(request_line, asterisk_target_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.method, "OPTIONS");
  REQUIRE_STREQ(ctx.target, "*");
}

TEST(request_line, http_1_0_request_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(parser.http_major, 1);
  REQUIRE_EQ(parser.http_minor, 0);
}

TEST(request_line, invalid_method_char_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GE/T /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, missing_target_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, missing_version_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, bad_version_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path FOO/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_line, control_char_in_target_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /pa\x01th HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// The request-target holds no whitespace (RFC 7230 SS3.1.1, SS5.3). A front end
// that splits on whitespace would disagree with this server about the resource,
// which is a smuggling primitive.
TEST(request_line, htab_in_target_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /a\tb HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.request_line_called);
}

TEST(request_line, del_in_target_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "GET /a\x7f"
      "b HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.request_line_called);
}

// The target check refuses bytes at or below SP and DEL, and nothing else.
TEST(request_line, ordinary_target_bytes_still_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /a~b%7Cc!$&'()*+,;=:@/-._ HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/a~b%7Cc!$&'()*+,;=:@/-._");
}

TEST(request_line, on_request_line_callback_error_reports_user) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.force_on_request_line_error = true;
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(request_line, split_across_every_byte_boundary) {
  const char *msg =
      "PUT /a/b/c HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nxyz";
  size_t len = strlen(msg);
  for (size_t split = 0; split <= len; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    chttp1_errno r1 = chttp1_parser_execute(&parser, msg, split);
    if (r1 == CHTTP1_PAUSED) {
      REQUIRE_EQ(split, len);
      continue;
    }
    REQUIRE_EQ(r1, CHTTP1_OK);
    chttp1_errno r2 = chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(r2, CHTTP1_PAUSED);
  }
}

// RFC 7230 SS3.5: A server should ignore an empty line before the
// request-line, and some clients send a stray CRLF after a POST body. Refusing
// it would close a healthy keep-alive connection.
TEST(request_line, single_leading_blank_line_tolerated) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "\r\nGET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.request_line_called);
  REQUIRE_STREQ(ctx.method, "GET");
  REQUIRE_STREQ(ctx.target, "/path");
}

TEST(request_line, multiple_leading_blank_lines_tolerated) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "\r\n\r\n\r\nGET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/path");
}

TEST(request_line, leading_blank_lines_split_across_byte_boundaries) {
  const char *msg = "\r\n\r\nGET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  size_t len = strlen(msg);
  for (size_t split = 1; split < len; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    chttp1_errno r1 = chttp1_parser_execute(&parser, msg, split);
    if (r1 == CHTTP1_PAUSED) continue;
    REQUIRE_EQ(r1, CHTTP1_OK);
    chttp1_errno r2 = chttp1_parser_execute(&parser, msg + split, len - split);
    REQUIRE_EQ(r2, CHTTP1_PAUSED);
    REQUIRE_STREQ(ctx.target, "/path");
  }
}

// The blank lines are bounded, not skipped for ever.
TEST(request_line, excessive_leading_blank_lines_rejected) {
  chttp1_parser parser;
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

// A request with no Content-Length and no chunked coding has no body; it
// completes after the headers instead of waiting for an EOF that never comes.
TEST(request_body_framing, no_framing_headers_means_no_body_not_eof) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

// RFC 7230 SS5.4: A server answers 400 to an HTTP/1.1 request without Host and
// to any request with more than one Host or an invalid one. The parser checks
// at the end of the header block, where the count is final. Two Host lines let
// a front end and a back end disagree about the authority. Response mode
// ignores the rule.
TEST(host_header, http_1_1_without_host_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(host_header, http_1_1_with_one_host_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: example.com\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(host_header, duplicate_host_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "GET /path HTTP/1.1\r\nHost: a.example\r\nHost: b.example\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.message_complete_called);
}

// The duplicate rule names no version.
TEST(host_header, duplicate_host_rejected_for_http_1_0_too) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// The missing-Host rule names only HTTP/1.1.
TEST(host_header, http_1_0_without_host_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

// Two spellings of the name still count as two.
TEST(host_header, host_matched_without_regard_to_case) {
  {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    const char *msg = "GET / HTTP/1.1\r\nHOST: example.com\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
  {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    const char *msg = "GET / HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  }
}

TEST(host_header, empty_host_value_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost:\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// An authority holds no whitespace (RFC 3986 SS3.2); surrounding OWS is
// trimmed first, so only an interior byte gets here.
TEST(host_header, host_value_with_interior_whitespace_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost: a.example b.example\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(host_header, surrounding_optional_whitespace_still_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost:   example.com\t \r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "Host"), "example.com");
}

TEST(host_header, response_mode_ignores_the_rule_completely) {
  {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
  {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    const char *msg =
        "HTTP/1.1 200 OK\r\nHost: a\r\nHost: b\r\nContent-Length: 0\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
}

// A trailer must neither supply a missing Host nor make a single Host a
// duplicate.
TEST(host_header, a_trailer_named_host_cannot_satisfy_or_break_the_rule) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nHost: smuggled\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(request_body_framing, content_length_zero_completes_immediately) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(request_body_framing, chunked_request_body) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: "
      "chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing, trailer_name_not_whitelisted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nCustom-Trailer: allowed\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "Custom-Trailer"), "allowed");
  REQUIRE_TRUE(find_header(&ctx, "Custom-Trailer") == NULL);
}

// RFC 7230 SS3.3.3: A request whose final coding isn't chunked has no length
// and must be refused, because a request has no EOF framing to fall back on.
TEST(request_body_framing,
     transfer_encoding_present_but_final_coding_not_chunked_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

// An earlier line can end in chunked while a later Transfer-Encoding line
// follows. The lines merge into one list (RFC 7230 SS3.2.2), so this must be
// refused although each line passes on its own.
TEST(
    request_body_framing,
    transfer_encoding_chunked_not_last_split_across_two_header_lines_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n"
      "Transfer-Encoding: identity\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing,
     transfer_encoding_chunked_last_split_across_two_header_lines_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip\r\n"
      "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing,
     response_mode_final_coding_not_chunked_still_reads_until_eof) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_OK);
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

// chunked before the end of one line's list is always wrong in a request,
// whatever the last token is.
TEST(request_body_framing, single_line_chunked_then_gzip_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing, single_line_gzip_then_chunked_accepted) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

// The final-coding rule binds only requests; this response falls back to EOF
// framing.
TEST(request_body_framing,
     response_mode_single_line_chunked_then_gzip_reads_until_eof) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

// RFC 7230 SS3.3.1 binds a request too. Without counting over the merged list,
// the final coding is chunked and an ordinary chunked request completes.
TEST(request_body_framing, chunked_applied_twice_across_lines_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

// Two rules refuse this in request mode (the count and the final-coding rule);
// framing.response_chunked_twice_within_one_line_rejected isolates the count.
TEST(request_body_framing, chunked_twice_within_one_line_rejected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(divert, content_length_body_diverts_before_consuming) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n";
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

// Head and body come in one execute call; the divert must still stop at the
// header boundary and leave the body for the caller's carry-over.
TEST(divert, body_already_in_same_buffer_is_not_consumed_by_divert) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\n";
  const char *whole =
      "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_EQ(chttp1_parser_consumed(&parser), strlen(headers));

  const char *rest = "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, rest, strlen(rest)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(divert, no_body_downgrades_to_immediate_complete) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "GET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(divert, content_length_zero_downgrades_to_immediate_complete) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

// CHTTP1_HEADERS_DIVERT_BODY exists only in request mode; chttpclient.c has
// no worker to hand a body to.
TEST(divert, response_mode_rejects_divert_hint) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(content_length_accessor, false_before_headers_complete) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
  REQUIRE_EQ(chttp1_declared_content_length(&parser), (uint64_t)0);
}

// The divert pauses at headers-complete before any body byte, the window in
// which chttpserver.c reads the accessor and its chunked-body caveat doesn't
// apply yet.
TEST(content_length_accessor, true_with_declared_value_once_headers_complete) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 42\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_TRUE(chttp1_has_content_length(&parser));
  REQUIRE_EQ(chttp1_declared_content_length(&parser), (uint64_t)42);
}

TEST(content_length_accessor, false_for_chunked_body) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
}

TEST(content_length_accessor, false_for_no_body) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_has_content_length(&parser));
}

TEST(content_length_accessor, null_parser_returns_safe_defaults) {
  REQUIRE_FALSE(chttp1_has_content_length(NULL));
  REQUIRE_EQ(chttp1_declared_content_length(NULL), (uint64_t)0);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(NULL));
}

TEST(message_complete_accessor, false_before_completion) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
  const char *partial = "HTTP/1.1 200 OK\r\n";
  chttp1_parser_execute(&parser, partial, strlen(partial));
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
}

TEST(message_complete_accessor, true_after_execute_returns_paused) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_parser_message_complete(&parser));
}

TEST(message_complete_accessor,
     true_after_finish_completes_eof_delimited_body) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *headers = "HTTP/1.1 200 OK\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  REQUIRE_FALSE(chttp1_parser_message_complete(&parser));
  REQUIRE_EQ(chttp1_parser_finish(&parser), CHTTP1_PAUSED);
  REQUIRE_TRUE(chttp1_parser_message_complete(&parser));
}

TEST(chunk_size_limit, unset_override_accepts_any_size_that_fits) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  const char *chunk_size = "ffffffff\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, chunk_size, strlen(chunk_size)),
             CHTTP1_OK);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
}

// The refusal comes at the chunk-size line, not after waiting for data that
// never arrives.
TEST(chunk_size_limit, override_rejects_oversized_chunk_before_reading_data) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_chunk_size_override = 1024;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  const char *chunk_size = "8000000000000000\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, chunk_size, strlen(chunk_size)),
             CHTTP1_ERROR);
  REQUIRE_TRUE(chttp1_chunk_size_limit_exceeded(&parser));
}

TEST(chunk_size_limit, override_accepts_chunk_at_exactly_the_limit) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  parser.max_chunk_size_override = 5;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  const char *rest = "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, rest, strlen(rest)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

// The flag must be false for every other cause of CHTTP1_ERROR, here a
// malformed status line.
TEST(chunk_size_limit, not_set_for_an_unrelated_parse_error) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg = "GARBAGE\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
}

TEST(expect_continue, detected_with_body) {
  chttp1_parser parser;
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
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  chttp1_parser_execute(&parser, msg, strlen(msg));
  REQUIRE_FALSE(chttp1_expects_continue(&parser));
}

TEST(expect_continue, other_expect_value_not_detected) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nExpect: something-else\r\n\r\n";
  chttp1_parser_execute(&parser, msg, strlen(msg));
  REQUIRE_FALSE(chttp1_expects_continue(&parser));
}

// A caller that moves the body read to a worker still learns at
// CHTTP1_HEADERS_ONLY whether it owes a "100 Continue".
TEST(expect_continue, works_with_divert) {
  chttp1_parser parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *headers =
      "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\nExpect: "
      "100-continue\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_HEADERS_ONLY);
  REQUIRE_TRUE(chttp1_expects_continue(&parser));
}

// Makes a blocking AF_UNIX socketpair for the plaintext stream tests.
static void make_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

// Makes a non-blocking AF_UNIX socketpair for the stream_tls tests, failing
// the test when fcntl fails. tls_drive_handshake() steps both sides on one
// thread, and on a blocking pair a BIO_read waits for bytes the peer never gets
// to send, so the loop deadlocks.
static void make_nonblocking_pair(int fds[2]) {
  make_pair(fds);
  for (int i = 0; i < 2; i++) {
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

TEST(stream, prepare_no_leftover) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  REQUIRE_EQ(s.carry_len, (size_t)0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, leftover_drained_before_touching_fd) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "hello", 5, NULL));

  char buf[3] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, 3, 100), (ssize_t)3);
  REQUIRE_EQ(memcmp(buf, "hel", 3), 0);

  char buf2[10] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf2, 10, 100), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf2, "lo", 2), 0);

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
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
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
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  close(fds[1]);
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 1000), (ssize_t)0);
  chttp1_stream_release(&s);
  close(fds[0]);
}

TEST(stream, read_timeout_when_nothing_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 50), (ssize_t)-1);
  REQUIRE_TRUE(chttp1_stream_timed_out(&s));
  REQUIRE_EQ(chttp1_stream_last_error(&s), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

// timeout_ms == 0 means "only if ready right now". A path that computes a
// deadline and then reads the clock again finds the budget spent and reports a
// timeout without trying the I/O. These tests set up a fd that is already ready
// (data in the receive buffer, or an empty send buffer), so a made-up timeout
// fails them.
TEST(stream, read_timeout_zero_returns_data_when_already_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  REQUIRE_EQ(write(fds[1], "hi", 2), (ssize_t)2);
  char buf[4] = {0};
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 0), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  REQUIRE_FALSE(chttp1_stream_timed_out(&s));
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

// With nothing available one immediate check reports a timeout: No hang and no
// fake read of zero bytes.
TEST(stream, read_timeout_zero_times_out_when_nothing_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
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
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[1], NULL, 0, NULL));
  REQUIRE_EQ(chttp1_stream_write(&s, "hi", 2, 1000), (ssize_t)2);
  char buf[4] = {0};
  REQUIRE_EQ(read(fds[0], buf, sizeof(buf)), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

// A fresh socketpair end is writable at once.
TEST(stream, write_timeout_zero_succeeds_when_already_writable) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[1], NULL, 0, NULL));
  REQUIRE_EQ(chttp1_stream_write(&s, "hi", 2, 0), (ssize_t)2);
  REQUIRE_FALSE(chttp1_stream_timed_out(&s));
  char buf[4] = {0};
  REQUIRE_EQ(read(fds[0], buf, sizeof(buf)), (ssize_t)2);
  REQUIRE_EQ(memcmp(buf, "hi", 2), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

// poll(2) ignores a negative fd, so the test uses a closed fd number, which
// poll reports with POLLNVAL and read(2) fails with EBADF.
TEST(stream, read_error_on_bad_fd) {
  int fds[2];
  make_pair(fds);
  int bad_fd = fds[0];
  close(fds[0]);
  close(fds[1]);

  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, bad_fd, NULL, 0, NULL));
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
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1, NULL));
  chttp1_stream_release(&s);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_prepends_ahead_of_existing_carry) {
  int fds[2];
  make_pair(fds);
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "world", 5, NULL));
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
  chttp1_stream s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1, NULL));
  REQUIRE_TRUE(chttp1_stream_push_back_leftover(&s, "unused", 0));
  REQUIRE_EQ(s.carry_len, (size_t)1);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

// len + existing needs its own overflow check before the allocation, or the sum
// wraps and the copy overruns. The test fakes existing as SIZE_MAX with carry
// NULL, so nothing large is allocated, and the carry-over stays untouched.
TEST(stream, push_back_leftover_overflow_guard_rejects_without_allocating) {
  chttp1_stream s;
  memset(&s, 0, sizeof(s));
  s.prepared = true;
  s.carry = NULL;
  s.carry_len = SIZE_MAX;
  s.carry_pos = 0;

  char buf[4] = {'a', 'b', 'c', 'd'};
  REQUIRE_FALSE(chttp1_stream_push_back_leftover(&s, buf, sizeof(buf)));
  REQUIRE_EQ((void *)s.carry, NULL);
  REQUIRE_EQ(s.carry_len, (size_t)SIZE_MAX);
}

// Steps the handshakes of a and b in turn on one thread until both finish, so
// the stream_tls tests get a connected TLS pair without threads. Gives false on
// an error or after 200 rounds.
static bool tls_drive_handshake(ctls_conn *a, ctls_conn *b) {
  bool a_done = false, b_done = false;
  for (int i = 0; i < 200 && !(a_done && b_done); i++) {
    if (!a_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(a);
      if (r == CTLS_HANDSHAKE_DONE) a_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) return false;
    }
    if (!b_done) {
      ctls_handshake_result r = ctls_conn_handshake_step(b);
      if (r == CTLS_HANDSHAKE_DONE) b_done = true;
      if (r == CTLS_HANDSHAKE_ERROR) return false;
    }
  }
  return a_done && b_done;
}

TEST(stream_tls, read_over_real_tls_connection) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  REQUIRE_EQ(ctls_conn_write(client_conn, "hello", 5), (ssize_t)5);

  chttp1_stream s;
  REQUIRE_TRUE(
      chttp1_stream_prepare_tls(&s, fds[0], server_conn, NULL, 0, NULL));
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
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  chttp1_stream s;
  REQUIRE_TRUE(
      chttp1_stream_prepare_tls(&s, fds[0], server_conn, NULL, 0, NULL));
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

// The leftover stands for bytes the reactor already decrypted with
// ctls_conn_read(), never raw bytes off the wire.
TEST(stream_tls, leftover_decrypted_bytes_drained_before_ctls_conn_read) {
  ctls_ctx *server_ctx = ctls_ctx_new(NULL);
  REQUIRE_EQ(ctls_ctx_cert_add(server_ctx, "srv.test", NULL, NULL, NULL, NULL),
             ccol_success);
  ctls_ctx *client_ctx = ctls_ctx_new(NULL);

  int fds[2];
  make_nonblocking_pair(fds);
  ctls_conn *server_conn =
      ctls_conn_create_server(server_ctx, fds[0], NULL, NULL);
  ctls_conn *client_conn =
      ctls_conn_create_client(client_ctx, fds[1], "srv.test", false, NULL);
  REQUIRE_TRUE(tls_drive_handshake(client_conn, server_conn));

  chttp1_stream s;
  REQUIRE_TRUE(
      chttp1_stream_prepare_tls(&s, fds[0], server_conn, "carried", 7, NULL));
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

// chttp1_parser_init() and chttp1_parser_init_request() clear the state and
// leave the line buffer alone. The parser_reset tests parse a message after a
// reset over a line buffer that holds a long earlier message, or poison bytes,
// and check every result.
//
// The poison bytes: The ones the line readers act on (NUL, CR, LF, colon,
// space, a digit, a token byte, DEL and a byte above 0x7F).
static const unsigned char g_reset_poison[] = {0x00, '\r', '\n', ':', ' ',
                                               '0',  'X',  0x7f, 0xff};
// The number of poison bytes, for the loops over them.
#define RESET_POISON_COUNT (sizeof(g_reset_poison) / sizeof(g_reset_poison[0]))

// Fills the parser's line buffer with b, to show that a reset parser never
// reads it.
static void reset_poison_line_buf(chttp1_parser *parser, unsigned char b) {
  memset(parser->line_buf, b, sizeof(parser->line_buf));
}

// Appends n copies of c to buf at *pos and moves *pos; the reset tests build
// their long messages with it.
static void reset_put_run(char *buf, size_t *pos, char c, size_t n) {
  memset(buf + *pos, c, n);
  *pos += n;
}

// Appends the text s to buf at *pos and moves *pos; the companion of
// reset_put_run().
static void reset_put(char *buf, size_t *pos, const char *s) {
  size_t n = strlen(s);
  memcpy(buf + *pos, s, n);
  *pos += n;
}

// Builds a request whose request line and one header line each nearly fill the
// 8 KiB line buffer, and which sets every flag a short request must not inherit
// (Connection: close, Expect: 100-continue, Content-Length). Gives a malloc'd
// buffer and its length in *len_out, or NULL.
static char *reset_dirty_request(size_t *len_out) {
  char *m = malloc(32 * 1024);
  if (!m) return NULL;
  size_t pos = 0;
  reset_put(m, &pos, "POST /");
  reset_put_run(m, &pos, 'a', 8000);
  reset_put(m, &pos, " HTTP/1.1\r\nHost: dirty\r\nConnection: close\r\n");
  reset_put(m, &pos, "Expect: 100-continue\r\nX-Fill: ");
  reset_put_run(m, &pos, 'b', 8000);
  reset_put(m, &pos, "\r\nContent-Length: 3\r\n\r\nabc");
  *len_out = pos;
  return m;
}

// Builds a chunked response whose reason phrase, one header line, a chunk-size
// line and a trailer line each nearly fill the line buffer, and which asks to
// close. Gives a malloc'd buffer and its length in *len_out, or NULL.
static char *reset_dirty_response(size_t *len_out) {
  char *m = malloc(48 * 1024);
  if (!m) return NULL;
  size_t pos = 0;
  reset_put(m, &pos, "HTTP/1.1 599 ");
  reset_put_run(m, &pos, 'r', 8000);
  reset_put(m, &pos, "\r\nConnection: close\r\nX-Fill: ");
  reset_put_run(m, &pos, 'b', 8000);
  reset_put(m, &pos, "\r\nTransfer-Encoding: chunked\r\n\r\n3;");
  reset_put_run(m, &pos, 'e', 7990);
  reset_put(m, &pos, "\r\nabc\r\n0\r\nX-Dirty: ");
  reset_put_run(m, &pos, 't', 8000);
  reset_put(m, &pos, "\r\n\r\n");
  *len_out = pos;
  return m;
}

// Feeds msg in two pieces split at split (0 < split <= len), so the reset tests
// cross the buffer at chosen points. Gives the result of the call that ended
// the message, or the first failure.
static chttp1_errno reset_feed_split(chttp1_parser *parser, const char *msg,
                                     size_t len, size_t split) {
  chttp1_errno r = chttp1_parser_execute(parser, msg, split);
  if (r != CHTTP1_OK || split == len) return r;
  return chttp1_parser_execute(parser, msg + split, len - split);
}

// The short request of the request-mode reset tests.
static const char g_reset_short_request[] =
    "PUT /b HTTP/1.1\r\nHost: y\r\nX-A: 1\r\nContent-Length: 2\r\n\r\nhi";

// Checks every result of g_reset_short_request, so nothing of an earlier
// message can leak through unseen. Gives the number of failed checks, so the
// caller can release what it holds before it asserts.
static int reset_check_short_request(chttp1_parser *parser,
                                     const test_ctx_t *ctx, chttp1_errno r,
                                     size_t last_piece_len) {
  int bad = 0;
  bad += r != CHTTP1_PAUSED;
  bad += !ctx->request_line_called;
  bad += strcmp(ctx->method, "PUT") != 0;
  bad += strcmp(ctx->target, "/b") != 0;
  bad += ctx->header_count != 3;
  bad += strcmp(header_or_absent(ctx, "Host"), "y") != 0;
  bad += strcmp(header_or_absent(ctx, "X-A"), "1") != 0;
  bad += strcmp(header_or_absent(ctx, "Content-Length"), "2") != 0;
  bad += ctx->body_len != 2 || memcmp(ctx->body, "hi", 2) != 0;
  bad += !ctx->headers_complete_called || !ctx->message_complete_called;
  bad += !chttp1_should_keep_alive(parser);
  bad += chttp1_expects_continue(parser);
  bad += !chttp1_has_content_length(parser);
  bad += chttp1_has_other_transfer_coding(parser);
  bad += chttp1_parser_consumed(parser) != last_piece_len;
  bad += parser->http_major != 1 || parser->http_minor != 1;
  bad += parser->reason != NULL;
  return bad;
}

TEST(parser_reset, a_request_after_a_long_request_reads_only_its_own_bytes) {
  size_t dirty_len = 0;
  char *dirty = reset_dirty_request(&dirty_len);
  REQUIRE_TRUE(dirty != NULL);
  const size_t len = sizeof(g_reset_short_request) - 1;
  int failures = 0;
  chttp1_errno dirty_r = CHTTP1_OK;
  for (size_t split = 1; split <= len; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    dirty_r = chttp1_parser_execute(&parser, dirty, dirty_len);
    if (dirty_r != CHTTP1_PAUSED) break;
    init_test_request(&parser, &ctx);
    chttp1_errno r =
        reset_feed_split(&parser, g_reset_short_request, len, split);
    size_t last = split == len ? len : len - split;
    if (reset_check_short_request(&parser, &ctx, r, last) != 0) failures++;
  }
  free(dirty);
  REQUIRE_EQ(dirty_r, CHTTP1_PAUSED);
  REQUIRE_EQ(failures, 0);
}

TEST(parser_reset, a_request_after_a_reset_ignores_any_poison_in_the_buffer) {
  const size_t len = sizeof(g_reset_short_request) - 1;
  int failures = 0;
  for (size_t k = 0; k < RESET_POISON_COUNT; k++) {
    for (size_t split = 1; split <= len; split++) {
      chttp1_parser parser;
      test_ctx_t ctx;
      init_test_request(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno r =
          reset_feed_split(&parser, g_reset_short_request, len, split);
      size_t last = split == len ? len : len - split;
      if (reset_check_short_request(&parser, &ctx, r, last) != 0) failures++;
    }
  }
  REQUIRE_EQ(failures, 0);
}

// The short responses of the response-mode reset tests.
static const char g_reset_short_response[] =
    "HTTP/1.1 200 OK\r\nX-A: 1\r\nContent-Length: 2\r\n\r\nhi";
static const char g_reset_short_chunked[] =
    "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n"
    "2;x=y\r\nhi\r\n1\r\n!\r\n0\r\nX-T: 9\r\n\r\n";

// Checks every result of g_reset_short_response after the bytes of an earlier
// message, so nothing of it leaks through unseen. Gives the number of failed
// checks.
static int reset_check_short_response(chttp1_parser *parser,
                                      const test_ctx_t *ctx, chttp1_errno r,
                                      size_t last_piece_len) {
  int bad = 0;
  bad += r != CHTTP1_PAUSED;
  bad += ctx->status_code != 200 || parser->status_code != 200;
  bad += ctx->header_count != 2;
  bad += strcmp(header_or_absent(ctx, "X-A"), "1") != 0;
  bad += strcmp(header_or_absent(ctx, "Content-Length"), "2") != 0;
  bad += ctx->trailer_count != 0;
  bad += ctx->body_len != 2 || memcmp(ctx->body, "hi", 2) != 0;
  bad += !ctx->message_complete_called;
  bad += !chttp1_should_keep_alive(parser);
  bad += !chttp1_has_content_length(parser);
  bad += chttp1_parser_consumed(parser) != last_piece_len;
  bad += parser->reason != NULL;
  return bad;
}

// Checks every result of g_reset_short_chunked the same way; gives the number
// of failed checks.
static int reset_check_short_chunked(chttp1_parser *parser,
                                     const test_ctx_t *ctx, chttp1_errno r,
                                     size_t last_piece_len) {
  int bad = 0;
  bad += r != CHTTP1_PAUSED;
  bad += ctx->status_code != 201;
  bad += ctx->header_count != 1;
  bad += strcmp(header_or_absent(ctx, "Transfer-Encoding"), "chunked") != 0;
  bad += ctx->trailer_count != 1;
  bad += strcmp(trailer_or_absent(ctx, "X-T"), "9") != 0;
  bad += ctx->body_len != 3 || memcmp(ctx->body, "hi!", 3) != 0;
  bad += !ctx->message_complete_called;
  bad += !chttp1_should_keep_alive(parser);
  bad += chttp1_has_content_length(parser);
  bad += chttp1_parser_consumed(parser) != last_piece_len;
  bad += parser->reason != NULL;
  return bad;
}

TEST(parser_reset, a_response_after_a_long_response_reads_only_its_own_bytes) {
  size_t dirty_len = 0;
  char *dirty = reset_dirty_response(&dirty_len);
  REQUIRE_TRUE(dirty != NULL);
  const size_t len = sizeof(g_reset_short_response) - 1;
  const size_t clen = sizeof(g_reset_short_chunked) - 1;
  int failures = 0;
  chttp1_errno dirty_r = CHTTP1_OK;
  for (size_t split = 1; split <= clen; split++) {
    chttp1_parser parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    dirty_r = chttp1_parser_execute(&parser, dirty, dirty_len);
    if (dirty_r != CHTTP1_PAUSED) break;
    if (split <= len) {
      init_test(&parser, &ctx);
      chttp1_errno r =
          reset_feed_split(&parser, g_reset_short_response, len, split);
      size_t last = split == len ? len : len - split;
      if (reset_check_short_response(&parser, &ctx, r, last) != 0) failures++;
    }
    init_test(&parser, &ctx);
    chttp1_errno r =
        reset_feed_split(&parser, g_reset_short_chunked, clen, split);
    size_t last = split == clen ? clen : clen - split;
    if (reset_check_short_chunked(&parser, &ctx, r, last) != 0) failures++;
  }
  free(dirty);
  REQUIRE_EQ(dirty_r, CHTTP1_PAUSED);
  REQUIRE_EQ(failures, 0);
}

TEST(parser_reset, a_response_after_a_reset_ignores_any_poison_in_the_buffer) {
  const size_t len = sizeof(g_reset_short_response) - 1;
  const size_t clen = sizeof(g_reset_short_chunked) - 1;
  int failures = 0;
  for (size_t k = 0; k < RESET_POISON_COUNT; k++) {
    for (size_t split = 1; split <= clen; split++) {
      chttp1_parser parser;
      test_ctx_t ctx;
      if (split <= len) {
        init_test(&parser, &ctx);
        reset_poison_line_buf(&parser, g_reset_poison[k]);
        chttp1_errno r =
            reset_feed_split(&parser, g_reset_short_response, len, split);
        size_t last = split == len ? len : len - split;
        if (reset_check_short_response(&parser, &ctx, r, last) != 0) failures++;
      }
      init_test(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno r =
          reset_feed_split(&parser, g_reset_short_chunked, clen, split);
      size_t last = split == clen ? clen : clen - split;
      if (reset_check_short_chunked(&parser, &ctx, r, last) != 0) failures++;
    }
  }
  REQUIRE_EQ(failures, 0);
}

// What the spill tests record about the one long header of their response,
// whose value the ordinary callback cuts at 255 bytes.
static size_t g_reset_long_value_len;
static bool g_reset_long_value_ok;

// Records the length of X-Long and whether it is all 'v' in
// g_reset_long_value_len and g_reset_long_value_ok, then records the header as
// t_on_header does; the spill tests check a value longer than test_ctx_t
// keeps.
static int reset_spill_on_header(chttp1_parser *p, const char *name,
                                 size_t name_len, const char *value,
                                 size_t value_len) {
  if (name_len == 6 && memcmp(name, "X-Long", 6) == 0) {
    g_reset_long_value_len = value_len;
    bool ok = true;
    for (size_t i = 0; i < value_len; i++)
      if (value[i] != 'v') ok = false;
    g_reset_long_value_ok = ok;
  }
  return t_on_header(p, name, name_len, value, value_len);
}

// The callbacks of the spill tests.
static const chttp1_settings g_reset_spill_settings = {
    .on_header = reset_spill_on_header,
    .on_trailer = t_on_trailer,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

// Builds a response with one header line of value_len 'v' bytes between two
// short ones, which spills once it passes the 8 KiB line buffer. Gives a
// malloc'd buffer and its length in *len_out, or NULL.
static char *reset_spill_response(size_t value_len, size_t *len_out) {
  char *m = malloc(value_len + 256);
  if (!m) return NULL;
  size_t pos = 0;
  reset_put(m, &pos, "HTTP/1.1 200 OK\r\nX-A: 1\r\nX-Long: ");
  reset_put_run(m, &pos, 'v', value_len);
  reset_put(m, &pos, "\r\nContent-Length: 2\r\n\r\nhi");
  *len_out = pos;
  return m;
}

// Initializes parser in response mode with line spill on and clears ctx and
// the long-value records, for the spill tests.
static void reset_spill_init(chttp1_parser *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init(parser, &g_reset_spill_settings);
  parser->data = ctx;
  (void)chttp1_parser_enable_line_spill(parser, NULL);
  g_reset_long_value_len = 0;
  g_reset_long_value_ok = false;
}

// The first response spills a 20000-byte line. After the release and the reset
// the buffer is poisoned and a 10000-byte line is fed in two pieces, split
// inside the fixed buffer, at its end and inside the spill buffer.
TEST(parser_reset, a_spilled_line_after_a_reset_keeps_exactly_its_own_bytes) {
  size_t first_len = 0, second_len = 0;
  char *first = reset_spill_response(20000, &first_len);
  char *second = reset_spill_response(10000, &second_len);
  REQUIRE_TRUE(first != NULL && second != NULL);
  int failures = 0;
  chttp1_errno first_r = CHTTP1_OK;
  bool first_spilled = false;
  size_t splits[] = {1,    2,    20,   25,    26,    33,    34,
                     100,  4096, 8190, 8191,  8192,  8216,  8217,
                     8218, 9000, 9999, 10033, 10034, 10035, second_len};
  for (size_t k = 0; k < RESET_POISON_COUNT; k++) {
    for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
      size_t split = splits[s];
      chttp1_parser parser;
      test_ctx_t ctx;
      reset_spill_init(&parser, &ctx);
      first_r = chttp1_parser_execute(&parser, first, first_len);
      first_spilled = parser.line_spill != NULL;
      chttp1_parser_release(&parser);
      if (first_r != CHTTP1_PAUSED || !first_spilled) break;
      reset_spill_init(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno r = reset_feed_split(&parser, second, second_len, split);
      bool spilled = parser.line_spill != NULL;
      size_t last = split == second_len ? second_len : second_len - split;
      chttp1_parser_release(&parser);
      int bad = 0;
      bad += r != CHTTP1_PAUSED;
      bad += !spilled;
      bad += ctx.status_code != 200;
      bad += ctx.header_count != 3;
      bad += strcmp(header_or_absent(&ctx, "X-A"), "1") != 0;
      bad += strcmp(header_or_absent(&ctx, "Content-Length"), "2") != 0;
      bad += g_reset_long_value_len != 10000 || !g_reset_long_value_ok;
      bad += ctx.body_len != 2 || memcmp(ctx.body, "hi", 2) != 0;
      bad += !ctx.message_complete_called;
      bad += !chttp1_should_keep_alive(&parser);
      bad += chttp1_parser_consumed(&parser) != last;
      if (bad != 0) failures++;
    }
    if (first_r != CHTTP1_PAUSED || !first_spilled) break;
  }
  free(first);
  free(second);
  REQUIRE_EQ(first_r, CHTTP1_PAUSED);
  REQUIRE_TRUE(first_spilled);
  REQUIRE_EQ(failures, 0);
}

// Each parser lives in a fresh malloc block. Under valgrind a branch on a byte
// the parser hasn't written since its init is an error of the run.
TEST(parser_reset, a_parser_in_uninitialized_memory_reads_no_unwritten_byte) {
  const size_t rlen = sizeof(g_reset_short_request) - 1;
  const size_t clen = sizeof(g_reset_short_chunked) - 1;
  int failures = 0;
  bool allocated = true;
  for (size_t split = 1; split <= clen && allocated; split++) {
    chttp1_parser *parser = malloc(sizeof(*parser));
    test_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!parser || !ctx) {
      free(parser);
      free(ctx);
      allocated = false;
      break;
    }
    if (split <= rlen) {
      init_test_request(parser, ctx);
      chttp1_errno r =
          reset_feed_split(parser, g_reset_short_request, rlen, split);
      size_t last = split == rlen ? rlen : rlen - split;
      if (reset_check_short_request(parser, ctx, r, last) != 0) failures++;
    }
    free(parser);
    parser = malloc(sizeof(*parser));
    if (!parser) {
      free(ctx);
      allocated = false;
      break;
    }
    init_test(parser, ctx);
    chttp1_errno r =
        reset_feed_split(parser, g_reset_short_chunked, clen, split);
    size_t last = split == clen ? clen : clen - split;
    if (reset_check_short_chunked(parser, ctx, r, last) != 0) failures++;
    free(parser);
    free(ctx);
  }
  REQUIRE_TRUE(allocated);
  REQUIRE_EQ(failures, 0);
}
