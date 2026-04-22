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
 * exercise its own read/write/poll logic directly -- no chttpclient.c, no
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
 * pointer (per its own documented "settings must outlive parser" contract,
 * exactly like llhttp_init's identical contract for its own settings
 * pointer) rather than copying it, so a stack-local here would leave every
 * parser's ->settings dangling the moment init_test() returns. */
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
  /* No magnitude restriction: this project's own llhttp.h HTTP_STATUS_MAP
   * lists real, in-use nonstandard codes up to 599. */
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

/* ---- size caps ---- */

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
   * means the body is read until the connection closes -- not rejected. */
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
     * unconsumed -- rv1 == CHTTP1_OK above already implies the first call
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
   * per chttp1_parser_execute's own documented contract) -- this test calls
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

/* ========================================================================== */
/*        REQUEST-MODE BODY FRAMING (RFC 7230 SS3.3 asymmetry vs response)   */
/* ========================================================================== */

TEST(request_body_framing, no_framing_headers_means_no_body_not_eof) {
  /* Unlike a response, a request with neither Content-Length nor chunked
   * Transfer-Encoding has NO body at all -- the message completes
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
  /* Confirmed decision: no facio-style x-/server-timing trailer whitelist
   * for requests -- any trailer name is accepted, matching the client
   * parser's own pre-existing, unrestricted trailer handling. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nCustom-Trailer: allowed\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(find_header(&ctx, "Custom-Trailer"), "allowed");
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
   * caller to hand off as carry-over -- not swallow them into on_body
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
 * blocking socketpair -- SSL_accept/SSL_connect's internal BIO_read can
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
    int flags = fcntl(fds[i], F_GETFL, 0);
    fcntl(fds[i], F_SETFL, flags | O_NONBLOCK);
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

TEST(stream, read_error_on_bad_fd) {
  /* A negative fd is specially ignored by poll(2) itself (POSIX: an entry
   * with fd < 0 is never reported ready, so it would just silently time
   * out here, not error) -- a real invalid-fd error needs a syntactically
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
   * produced via ctls_conn_read() during its own header-parsing loop --
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
