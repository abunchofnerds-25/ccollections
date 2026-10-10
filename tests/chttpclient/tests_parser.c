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
 * White-box tests for chttp1_parser, at the level of a byte sequence. They
 * drive the parser directly, with no TLS and no chttpclient.c at all, on
 * input that is built by hand, some of it malformed or hostile. The suite in
 * tests.c sits on a real socket, and its shape cannot reach such input,
 * because its mock server only ever sends a well-formed response. See
 * tests.c and tests_tls.c for the end-to-end coverage of "does real traffic
 * still work"; this file is only about the correctness of the parser on its
 * own.
 *
 * The "stream" test group, which covers chttp1_stream_t, does use local
 * AF_UNIX socketpairs, but only to drive its own logic for read, write and
 * poll. It uses no chttpclient.c, no TLS and no real network traffic.
 */

#include <fcntl.h>
#include <internal/chttp1_parser.h>
#include <internal/ctls.h>
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
  bool force_on_trailer_error;
  bool force_on_body_error;
  bool force_on_headers_complete_error;
  bool want_divert; /* on_headers_complete returns CHTTP1_HEADERS_DIVERT_BODY */

  char header_names[MAX_TEST_HEADERS][256];
  char header_values[MAX_TEST_HEADERS][256];
  size_t header_count;

  /* Trailer fields land here, never in header_names/header_values: the
     parser delivers them through a callback of their own precisely so a
     caller keeping one collection cannot let a trailer displace a real
     header of the same name. Keeping the two arrays separate here is what
     lets these tests assert on that split directly. */
  char trailer_names[MAX_TEST_HEADERS][256];
  char trailer_values[MAX_TEST_HEADERS][256];
  size_t trailer_count;

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

static int t_on_trailer(chttp1_parser_t *p, const char *name, size_t name_len,
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

/* This object has static storage duration because chttp1_parser_init only
 * borrows the settings pointer instead of copying it, and its documented
 * contract says that the settings must outlive the parser. A local on the
 * stack here would leave the ->settings of every parser dangling the moment
 * init_test() returns. */
static const chttp1_settings_t g_test_settings = {
    .on_header = t_on_header,
    .on_trailer = t_on_trailer,
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
    .on_trailer = t_on_trailer,
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

static const char *find_trailer(const test_ctx_t *ctx, const char *name) {
  for (size_t i = 0; i < ctx->trailer_count; i++)
    if (strcmp(ctx->trailer_names[i], name) == 0) return ctx->trailer_values[i];
  return NULL;
}

/* REQUIRE_STREQ expands to strcmp, which is undefined on NULL and takes the
   whole binary down with a SIGSEGV rather than failing one test, destroying
   every other test's pass/fail signal in the run. These two report a
   missing field as a distinctive string instead, so an assertion about a
   field's value fails cleanly when the field is absent. */
static const char *header_or_absent(const test_ctx_t *ctx, const char *name) {
  const char *v = find_header(ctx, name);
  return v ? v : "(absent)";
}

static const char *trailer_or_absent(const test_ctx_t *ctx, const char *name) {
  const char *v = find_trailer(ctx, name);
  return v ? v : "(absent)";
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
  /* This is for compatibility with the real world: the ABNF of RFC 7230
   * asks for a trailing SP, even for an empty reason phrase, but real
   * servers leave it out, as in "HTTP/1.1 304\r\n". */
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
  /* There is no restriction on the size of the number, because real
   * nonstandard status codes are in use up to 599, for example 599 Network
   * Connect Timeout Error, which some load balancers and proxies use. The
   * parser therefore accepts any code of 3 digits. */
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
  /* A regression test. chttp1_should_keep_alive must not decide "HTTP/1.1
   * or later" with the test "http_major > 0 && http_minor > 0", which reads
   * any version with a zero minor part as HTTP/1.0 or earlier. A literal
   * "HTTP/2.0" status line is one such version, and the grammar of this
   * parser accepts it; see status_line.any_major_minor_digit_accepted. The
   * parser would then need an explicit "Connection: keep-alive" token, which
   * no real server of that age sends, so connection reuse quietly stops.
   * The correct test is "major > 1, or major == 1 with minor >=
   * 1". */
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
  /* This test uses 80 headers of about 1000 bytes each, which is 80KB: well
   * above the total cap of 64KB, but well below the cap of 100 headers and
   * the cap of 8192 bytes for each line, so it isolates the limit on the
   * total bytes. The test feeds the bytes one piece at a time, sending the
   * status line first and then one header line for each
   * chttp1_parser_execute call, so that it can stop as soon as the parser
   * reports the error instead of building one huge buffer up front. */
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
  /* A regression test. chttpsvr_config_t.max_header_bytes is documented to
   * bound "request line + all header lines", so the request line, or the
   * status line, must add to total_header_bytes itself, and
   * process_header_line must not be the only thing that adds to it.
   * Otherwise a request-target far above the configured cap is accepted,
   * bounded only by the much larger CHTTP1_MAX_LINE_LEN. The request line
   * alone here is well above the small override below and nowhere near
   * CHTTP1_MAX_LINE_LEN, and the parser must reject it before it sees any
   * header. */
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
  /* A request line and a header line can each fit on their own while their
   * SUM is above the override, and the parser must still reject them. This
   * confirms that the request line really adds to the same running total
   * that a header line adds to, rather than being tracked separately or
   * ignored. */
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
  /* RFC 7230 SS3.3.3 covers a RESPONSE here: the last token of its
   * Transfer-Encoding is not "chunked", so the recipient reads the body
   * until the connection closes, and the parser does not reject it. It does
   * reject a REQUEST in the same case (see the request_body_framing group),
   * because a request has no EOF framing to fall back on. */
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

TEST(framing,
     response_content_length_with_non_chunked_transfer_encoding_rejected) {
  /* RFC 7230 SS3.3.3 covers this. A Transfer-Encoding and a Content-Length
   * that frame the same message are an ambiguous pair, which a recipient
   * must resolve in favour of the Transfer-Encoding, or reject. A recipient
   * that accepts it frames the response at exactly Content-Length bytes,
   * leaves whatever follows those bytes unread, and reports the message as
   * cleanly complete and eligible for keep-alive. The surplus bytes then sit
   * on a connection that the client gives back to its idle pool, and the
   * next request that reuses that connection reads those bytes as its own
   * response.
   *
   * This test is not vacuous: narrow this rejection to request mode and it
   * fails, reporting CHTTP1_PAUSED, a body of 3 bytes, and a
   * chttp1_should_keep_alive() that answers true, with "EXTRA" left
   * unread. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nContent-Length: "
      "3\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing,
     response_non_chunked_transfer_encoding_before_content_length_rejected) {
  /* The order of the headers does not change the ambiguity: the parser
   * decides the rejection after it has read every Transfer-Encoding line,
   * not on the line that arrives second. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nContent-Length: 3\r\nTransfer-Encoding: "
      "gzip\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing,
     response_content_length_with_chunked_then_non_chunked_te_rejected) {
  /* RFC 7230 SS3.2.2 merges the Transfer-Encoding lines into one list, which
   * ends in "gzip" here. So this is the same ambiguous pair, although
   * "chunked" appears in the list, and a test on each line that only looked
   * for that word would accept it. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "gzip\r\nContent-Length: 3\r\n\r\nabcEXTRA";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(framing,
     response_non_chunked_transfer_encoding_alone_still_reads_until_eof) {
  /* The counterpart that the rejection above must not swallow. There is no
   * Content-Length here, so there is nothing to disagree with, and RFC 7230
   * SS3.3.3 frames exactly this response: the recipient reads until the
   * connection closes. Such a message is never eligible for keep-alive, so
   * it carries no hazard of its own for a pooled connection. */
  chttp1_parser_t parser;
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

TEST(framing, response_chunked_applied_twice_across_lines_rejected) {
  /* RFC 7230 SS3.3.1 says: "A sender MUST NOT apply chunked more than
   * once to a message body". RFC 7230 SS3.2.2 merges these two lines into
   * one list, "chunked, gzip, chunked", which applies chunked twice. One
   * recipient de-chunks once and another de-chunks twice, so the two
   * disagree about where the body ends, and that framing disagreement is
   * what response smuggling needs. The parser therefore refuses the message
   * rather than resolving it in either direction.
   *
   * This test is not vacuous: without the count of occurrences over the
   * merged list, the parser accepts the message as an ordinary chunked
   * response, which completes with CHTTP1_PAUSED, a body of 3 bytes, and a
   * chttp1_should_keep_alive() that answers true. */
  chttp1_parser_t parser;
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
  /* The same rule in its plainest form: two Transfer-Encoding lines that
   * each say "chunked" merge into the list "chunked, chunked". */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(framing, response_chunked_twice_within_one_line_rejected) {
  /* Both occurrences on one line make the same message, and the parser
   * refuses it in the same way, because the count is over transfer-codings,
   * not over header lines. */
  chttp1_parser_t parser;
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
  /* The name of a transfer-coding ignores the letter case, so other spellings
   * count as the same coding; a literal match would let this message
   * through. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: ChUnKeD\r\nTransfer-Encoding: "
      "CHUNKED\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(framing, response_chunked_last_after_an_earlier_chunked_rejected) {
  /* The merged list ends in "chunked" here, as "gzip, chunked, chunked", so
   * the framing that this message would otherwise get is chunked framing.
   * It is still chunked applied twice, and the parser still refuses it. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip, "
      "chunked\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(framing, response_single_nonfinal_chunked_still_reads_until_eof) {
  /* The counterpart that the rejection above must not swallow. RFC 7230
   * SS3.3.1 lets a response apply chunked ONCE in a position that is not
   * the last one, and RFC 7230 SS3.3.3 frames such a message: the recipient
   * reads until the connection closes. A rule that refused every "chunked"
   * before the end of the list would refuse this legal message, so the
   * parser counts occurrences, not position. */
  chttp1_parser_t parser;
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
  /* Merging across lines keeps working: "gzip" then "chunked" is a valid
   * way to spell the list "gzip, chunked", which is chunked-framed and
   * keep-alive eligible. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: gzip\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.body, "abc");
  REQUIRE_TRUE(chttp1_should_keep_alive(&parser));
}

/* A Transfer-Encoding line that names no coding adds only empty elements to
 * the merged list, and RFC 7230 SS7 has a recipient ignore those. The list
 * "chunked" followed by such a line is therefore still the list "chunked",
 * exactly as the single line "chunked," is. The tests below pin that for
 * both modes, and pin that the rules about the merged list (chunked applied
 * twice, chunked beside Content-Length, a final coding that is not chunked)
 * still see through the empty line in every position. */

typedef struct {
  const char *label;
  const char *te_lines; /* the Transfer-Encoding lines, each ending in CRLF */
} te_empty_case_t;

/* Each of these merged lists is "chunked" once empty elements are dropped. */
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

/* Each of these merged lists ends in a coding that is not chunked. */
static const te_empty_case_t g_te_lists_not_ending_in_chunked[] = {
    {"gzip, then empty",
     "Transfer-Encoding: gzip\r\n"
     "Transfer-Encoding:\r\n"},
    {"chunked, empty, gzip",
     "Transfer-Encoding: chunked\r\n"
     "Transfer-Encoding: ,\r\n"
     "Transfer-Encoding: gzip\r\n"},
};

/* Each of these merged lists applies chunked twice. */
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

#define TE_CASE_COUNT(a) (sizeof(a) / sizeof((a)[0]))

static void build_te_message(char *out, size_t cap, const char *start_line,
                             const char *te_lines, const char *extra_headers,
                             const char *body) {
  int n = snprintf(out, cap, "%s%s%s\r\n%s", start_line, te_lines,
                   extra_headers, body);
  if (n < 0 || (size_t)n >= cap) out[0] = '\0';
}

#define TE_RESPONSE_START "HTTP/1.1 200 OK\r\n"
#define TE_REQUEST_START "POST /u HTTP/1.1\r\nHost: h\r\n"
#define TE_CHUNKED_BODY "3\r\nabc\r\n0\r\n\r\n"

TEST(framing, response_empty_te_line_keeps_the_list_chunked) {
  /* This test is not vacuous. When an empty line re-derives the final
   * coding, "chunked, then empty" and its siblings take EOF framing: the
   * execute call returns CHTTP1_OK instead of CHTTP1_PAUSED, and the chunk
   * framing reaches the caller as body bytes. */
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_RESPONSE_START,
                     g_te_lists_equal_to_chunked[i].te_lines, "",
                     TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    chttp1_errno_t rc = chttp1_parser_execute(&parser, msg, strlen(msg));
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
  /* An empty line changes nothing in the other direction either: a list
   * that ends in "gzip" stays framed by EOF, wherever the empty line sits. */
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_not_ending_in_chunked); i++) {
    chttp1_parser_t parser;
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
    chttp1_parser_t parser;
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
  /* The merged list is "chunked" whatever the order of the lines, so a
   * Content-Length beside it is the ambiguous pair in every case. */
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    for (int cl_first = 0; cl_first < 2; cl_first++) {
      chttp1_parser_t parser;
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

TEST(request_body_framing, empty_te_line_keeps_the_list_chunked) {
  /* This test is not vacuous. When an empty line re-derives the final
   * coding, the request below reads as a Transfer-Encoding that does not
   * end in chunked, and the parser refuses it with CHTTP1_ERROR. */
  for (size_t i = 0; i < TE_CASE_COUNT(g_te_lists_equal_to_chunked); i++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    char msg[512];
    build_te_message(msg, sizeof(msg), TE_REQUEST_START,
                     g_te_lists_equal_to_chunked[i].te_lines, "",
                     TE_CHUNKED_BODY);
    REQUIRE_NE(msg[0], '\0');
    chttp1_errno_t rc = chttp1_parser_execute(&parser, msg, strlen(msg));
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
    chttp1_parser_t parser;
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
    chttp1_parser_t parser;
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
    chttp1_parser_t parser;
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
  /* A trailer field is delivered through the trailer callback, never
     through on_header: a caller that keeps one headers collection must not
     be able to have a header displaced by a same-named trailer. */
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Trailer"), "trailer-value");
  REQUIRE_TRUE(find_header(&ctx, "X-Trailer") == NULL);
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

/* ========================================================================== */
/*                     TRAILER ISOLATION                                     */
/* ========================================================================== */

/* RFC 7230 SS4.1.2 trailer fields arrive after a message's framing has
   already been settled and, on the server side, after the request has
   already been routed. These tests pin the two properties that follow from
   that: a trailer is delivered through a callback of its own (never
   on_header), and it changes nothing about how the parser itself sees the
   message. */

TEST(trailer_isolation, trailer_does_not_reach_on_header_in_response_mode) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "X-Origin: upstream\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Origin: forged\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  /* The response header keeps its own value; a consumer inserting every
     on_header line into one map (chttpclient.c does exactly that, and its
     insert overwrites) can never have it replaced by the trailer. */
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Origin"), "upstream");
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Origin"), "forged");
}

TEST(trailer_isolation, trailer_does_not_reach_on_header_in_request_mode) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  /* The privilege-escalation shape this split exists to close: a request
     behind a proxy that sets X-Forwarded-For and an authentication gateway
     that sets X-Authenticated-User, with the client appending trailers of
     the same names. Both header values must survive. */
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
      "X-Forwarded-For: 203.0.113.9\r\n"
      "X-Authenticated-User: guest\r\n\r\n"
      "5\r\nhello\r\n"
      "0\r\nX-Forwarded-For: 127.0.0.1\r\nX-Authenticated-User: admin\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Forwarded-For"), "203.0.113.9");
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Authenticated-User"), "guest");
  /* Host, Transfer-Encoding, X-Forwarded-For and X-Authenticated-User. The
     Host line is not optional: RFC 7230 SS5.4 makes a request without one a
     400, and this parser enforces that in request mode. */
  REQUIRE_EQ(ctx.header_count, (size_t)4);
  REQUIRE_EQ(ctx.trailer_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Forwarded-For"), "127.0.0.1");
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Authenticated-User"), "admin");
}

TEST(trailer_isolation, trailer_cannot_change_framing_or_connection_state) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  /* Every framing-relevant header name, all in the trailer part: a
     Transfer-Encoding that would clear chunked framing, a Content-Length
     that would then be accepted without tripping the mutual-exclusion
     check, and a Connection token that would turn a keep-alive connection
     into a closing one. None of them may be interpreted. */
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
  chttp1_parser_t parser;
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
  /* Being inert is not the same as being unchecked: a trailer line is held
     to the identical name-token and value-byte grammar a header line is. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *bad_name =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX Bad: value\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, bad_name, strlen(bad_name)),
             CHTTP1_ERROR);

  chttp1_parser_t parser2;
  test_ctx_t ctx2;
  init_test(&parser2, &ctx2);
  const char *bad_value =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX-Bad: va\x01lue\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser2, bad_value, strlen(bad_value)),
             CHTTP1_ERROR);

  chttp1_parser_t parser3;
  test_ctx_t ctx3;
  init_test(&parser3, &ctx3);
  const char *no_colon =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nnot-a-field-line\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser3, no_colon, strlen(no_colon)),
             CHTTP1_ERROR);
}

TEST(trailer_isolation, on_trailer_error_reports_user) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  ctx.force_on_trailer_error = true;
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nX-T: v\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_USER);
}

TEST(trailer_isolation, unset_on_trailer_discards_trailers_without_error) {
  /* The configuration both chttpclient.c and chttpserver.c ship: no trailer
     callback at all, so a trailer is validated, charged against the caps,
     and then dropped. Neither the message nor the header set is affected. */
  static const chttp1_settings_t no_trailer_settings = {
      .on_header = t_on_header,
      .on_headers_complete = t_on_headers_complete,
      .on_body = t_on_body,
      .on_message_complete = t_on_message_complete,
  };
  chttp1_parser_t parser;
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
      /* The whole message fits before this split point (a short message with a
       * large split is one such case), so nothing is left to feed. */
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
    /* The _Generic printer of REQUIRE_EQ in tau does not handle bool, so this
     * code casts to int. */
    REQUIRE_EQ((int)chttp1_should_keep_alive(&parser), (int)ref_keep_alive);
    /* A hard contract: execute() never returns CHTTP1_OK with bytes left
     * over, so the "rv1 == CHTTP1_OK" above already means that the first
     * call consumed all `split` bytes. There is nothing more to check here;
     * reaching this point is the check. */
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
   * has returned CHTTP1_PAUSED: it reports success at once instead, as the
   * documented contract of chttp1_parser_execute says. This test calls
   * finish anyway, only to drive the CHTTP1_FINISH_SAFE case of the switch
   * directly. A message that completed on a normal path, which the EOF does
   * not frame, leaves finish_state at CHTTP1_FINISH_SAFE, which finish()
   * reports as CHTTP1_OK without calling on_message_complete a second
   * time. */
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
  /* The CHTTP1_FINISH_SAFE_WITH_CB case must update finish_state after it
   * fires on_message_complete, not parser->state alone. Otherwise
   * finish_state stays at CHTTP1_FINISH_SAFE_WITH_CB, and a second finish()
   * call on a parser that is already at CHTTP1_ST_MESSAGE_DONE enters that
   * same branch again and calls on_message_complete again. That breaks two
   * contracts: the callback documents that it fires exactly once, and this
   * function documents that a parser which is already at a clean boundary
   * gets CHTTP1_OK. The two extra finish() calls below are what make this
   * test non-vacuous: each must report CHTTP1_OK, and neither may fire the
   * callback again. */
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
  const char *msg =
      "POST /submit HTTP/1.1\r\nHost: h\r\nContent-Length: 5\r\n\r\nhello";
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
  const char *msg = "GET /path?a=1&b=2 HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/path?a=1&b=2");
}

TEST(request_line, asterisk_target_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "OPTIONS * HTTP/1.1\r\nHost: h\r\n\r\n";
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

TEST(request_line, htab_in_target_rejected) {
  /* RFC 7230 SS3.1.1 delimits the request-line's fields with single spaces
     and SS5.3 admits no whitespace inside the request-target itself, so a
     HTAB is not content the way it is inside a header value. Accepting one
     lets a front-end that splits the request-line on whitespace and this
     server disagree about which resource was asked for, which is a request
     smuggling primitive rather than a leniency. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /a\tb HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.request_line_called);
}

TEST(request_line, del_in_target_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "GET /a\x7f"
      "b HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.request_line_called);
}

TEST(request_line, ordinary_target_bytes_still_accepted) {
  /* The target check rejects every byte at or below SP plus DEL, and
     nothing else: a target made of printable bytes, punctuation included,
     must still get through. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /a~b%7Cc!$&'()*+,;=:@/-._ HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(ctx.target, "/a~b%7Cc!$&'()*+,;=:@/-._");
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
  const char *msg =
      "PUT /a/b/c HTTP/1.1\r\nHost: h\r\nContent-Length: 3\r\n\r\nxyz";
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
  /* RFC 7230 SS3.5 says that a server SHOULD ignore at least one empty line
   * before the request-line, and some clients send a stray CRLF after a
   * POST body. This is a regression test: the parser must not reject such a
   * line outright with PH_ERROR, where method_len == 0, because that
   * rejection closes a healthy keep-alive or pipelined connection over one
   * stray CRLF. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "\r\nGET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.request_line_called);
  REQUIRE_STREQ(ctx.method, "GET");
  REQUIRE_STREQ(ctx.target, "/path");
}

TEST(request_line, multiple_leading_blank_lines_tolerated) {
  chttp1_parser_t parser;
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
  /* A request with no Content-Length and no chunked Transfer-Encoding has
   * NO body at all, unlike a response: the message completes at once after
   * the headers instead of waiting for an EOF; there would be nothing to
   * wait for, because the connection does not close. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: x\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

/* ========================================================================== */
/*                    HOST HEADER (RFC 7230 SS5.4)                            */
/* ========================================================================== */

/* RFC 7230 SS5.4: "A server MUST respond with a 400 (Bad Request) status code
   to any HTTP/1.1 request message that lacks a Host header field and to any
   request message that contains more than one Host header field or a Host
   header field with an invalid field-value."

   The parser enforces it in request mode, at the blank line that ends the
   header block, because only there is the count of Host lines final. Two Host
   lines are what let a front end and a back end disagree about the authority
   that a request names, which is a request-smuggling primitive. Response mode
   must not enforce any of this: a Host header in a response is an ordinary,
   meaningless field. */

TEST(host_header, http_1_1_without_host_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(host_header, http_1_1_with_one_host_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.1\r\nHost: example.com\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(host_header, duplicate_host_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "GET /path HTTP/1.1\r\nHost: a.example\r\nHost: b.example\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(host_header, duplicate_host_rejected_for_http_1_0_too) {
  /* The text of the rule puts no version condition on the duplicate case:
     "any request message that contains more than one Host header field". The
     reason does not depend on a version either. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(host_header, http_1_0_without_host_accepted) {
  /* The missing-Host half of the rule names HTTP/1.1 alone. HTTP/1.0
     predates the field, and a request without one is ordinary there. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(host_header, host_matched_without_regard_to_case) {
  /* A field name is case-insensitive (RFC 7230 SS3.2). A sender that spells
     it "HOST" or "hOsT" must satisfy the rule, and two such spellings must
     still count as two. */
  {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    const char *msg = "GET / HTTP/1.1\r\nHOST: example.com\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
  {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    const char *msg = "GET / HTTP/1.1\r\nHost: a\r\nhOsT: b\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  }
}

TEST(host_header, empty_host_value_rejected) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost:\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(host_header, host_value_with_interior_whitespace_rejected) {
  /* RFC 3986 SS3.2 lets no whitespace into an authority. A front end that
     splits on it forwards a different authority than a back end reads. The
     leading and trailing optional whitespace is trimmed before this check,
     so only an interior byte can reach it. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost: a.example b.example\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(host_header, surrounding_optional_whitespace_still_accepted) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET / HTTP/1.1\r\nHost:   example.com\t \r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "Host"), "example.com");
}

TEST(host_header, response_mode_ignores_the_rule_completely) {
  /* Response mode must not gain any of this. A response with no Host, and a
     response with two, are both ordinary messages. */
  {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    const char *msg = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
  {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    const char *msg =
        "HTTP/1.1 200 OK\r\nHost: a\r\nHost: b\r\nContent-Length: 0\r\n\r\n";
    REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  }
}

TEST(host_header, a_trailer_named_host_cannot_satisfy_or_break_the_rule) {
  /* A trailer field arrives after the framing is already settled. It must
     neither supply the Host of a request that had none, nor turn a request
     with exactly one into a duplicate. process_header_line() reaches the
     counter only for a real header line. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "0\r\nHost: smuggled\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(request_body_framing, content_length_zero_completes_immediately) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
}

TEST(request_body_framing, chunked_request_body) {
  chttp1_parser_t parser;
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
  /* There is no trailer-name whitelist for requests: any trailer name is
   * accepted, matching response mode's own unrestricted trailer
   * handling. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\nCustom-Trailer: allowed\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "Custom-Trailer"), "allowed");
  REQUIRE_TRUE(find_header(&ctx, "Custom-Trailer") == NULL);
}

TEST(request_body_framing,
     transfer_encoding_present_but_final_coding_not_chunked_rejected) {
  /* RFC 7230 SS3.3.3 covers this. When the final coding of the
   * Transfer-Encoding of a request is something other than "chunked", the
   * length of the message is undetermined, and a server that obeys the RFC
   * MUST reject such a request outright rather than quietly read it as a
   * request with no body. A request and a response are asymmetric here: a
   * request has no fallback to EOF framing, while a response does. This is
   * a regression test: the parser must reject the request instead of
   * quietly accepting it as if the Transfer-Encoding were absent. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(
    request_body_framing,
    transfer_encoding_chunked_not_last_split_across_two_header_lines_rejected) {
  /* A regression test. transfer_encoding_has_nonfinal_chunked sees only
   * one header line at a time, so an EARLIER Transfer-Encoding line can
   * claim "chunked" as final while a SECOND Transfer-Encoding line follows
   * it. RFC 7230 SS3.2.2 joins repeated header lines into one
   * comma-separated list, in order, so such a pair must not get past that
   * check, although a check of each line on its own does let it past. */
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
  /* The legal counterpart of the case above: "chunked" arrives as the LAST
   * token of the LAST Transfer-Encoding line, here the second line, and the
   * first line does not end in chunked. That is valid framing, and the
   * parser must still accept it. */
  chttp1_parser_t parser;
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
  /* Response mode has a separate, documented reduction of scope of its own
   * (see the doc comment of value_ends_with_chunked), which does not depend
   * on the rejection above, since that rejection belongs to request mode
   * alone. A response whose Transfer-Encoding does not end in "chunked"
   * falls back to EOF framing instead of being rejected. */
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
  /* The single-line path of transfer_encoding_has_nonfinal_chunked, while
   * the path above covers a list that two header lines split. Here "chunked"
   * appears before the end of the token list of one line, which is always
   * wrong, and the parser must reject it at once, whatever the last token
   * is. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing, single_line_gzip_then_chunked_accepted) {
  /* The legal counterpart on one line: more than one coding, separated by
   * commas, sits on ONE Transfer-Encoding line, and the list ends in
   * "chunked". That is valid framing, which the parser must accept, parsing
   * the body as chunked. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: gzip, chunked\r\n\r\n"
      "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_EQ(ctx.body_len, (size_t)5);
  REQUIRE_EQ(memcmp(ctx.body, "hello", 5), 0);
}

TEST(request_body_framing,
     response_mode_single_line_chunked_then_gzip_reads_until_eof) {
  /* The reduction of scope in response mode applies to a value with more
   * than one coding on one line just as much as to the single-coding case
   * above: the parser does NOT reject a response whose Transfer-Encoding
   * line ends in something other than "chunked".
   * transfer_encoding_has_nonfinal_chunked runs only under
   * CHTTP1_PARSE_REQUEST, and since the final coding is not "chunked", the
   * response falls back to EOF framing. */
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
  /* This matches single_line_gzip_then_chunked_accepted above, for
   * response mode. value_ends_with_chunked does not tell a request from a
   * response, so a response whose Transfer-Encoding line ends in "chunked"
   * parses its body as chunked, whatever comes before it on the same
   * line. */
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

TEST(request_body_framing, chunked_applied_twice_across_lines_rejected) {
  /* RFC 7230 SS3.3.1 says "MUST NOT apply chunked more than once", which
   * binds a request exactly as it binds a response. RFC 7230 SS3.2.2 merges
   * these two lines into "chunked, chunked", and the parser refuses the
   * message. This test is not vacuous: without the count of occurrences
   * over the merged list, the final coding of the second line is "chunked",
   * so the parser accepts the message as an ordinary chunked request, which
   * completes with CHTTP1_PAUSED and a body of 3 bytes. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked\r\nTransfer-Encoding: "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

TEST(request_body_framing, chunked_twice_within_one_line_rejected) {
  /* Both occurrences sit on one line, and the parser refuses them in the
   * same way. Two separate rules reject this message in request mode: the
   * count of occurrences, and "chunked must be the last transfer-coding of a
   * request". This test therefore pins the outcome without isolating the
   * count of occurrences;
   * framing.response_chunked_twice_within_one_line_rejected is the
   * non-vacuous test for that rule, because the rule about the final coding
   * does not bind a response at all. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, "
      "chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
  REQUIRE_EQ(ctx.body_len, (size_t)0);
  REQUIRE_FALSE(ctx.message_complete_called);
}

/* ========================================================================== */
/*              HEADERS-COMPLETE BODY DIVERSION (CHTTP1_HEADERS_ONLY)        */
/* ========================================================================== */

TEST(divert, content_length_body_diverts_before_consuming) {
  chttp1_parser_t parser;
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

TEST(divert, body_already_in_same_buffer_is_not_consumed_by_divert) {
  /* The critical carry-over case: the header block AND the body bytes
   * arrive in the SAME chttp1_parser_execute call. The diversion must still
   * stop exactly at the header boundary and leave the body bytes unconsumed,
   * so that the caller can hand them on as carry-over, instead of swallowing
   * them into on_body because they are already available. */
  chttp1_parser_t parser;
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
  chttp1_parser_t parser;
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
  /* Nothing to divert (no Content-Length, no chunked): want_divert is
   * downgraded to ordinary immediate completion instead of pausing. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "GET /path HTTP/1.1\r\nHost: h\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(divert, content_length_zero_downgrades_to_immediate_complete) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  ctx.want_divert = true;
  const char *msg = "POST /x HTTP/1.1\r\nHost: h\r\nContent-Length: 0\r\n\r\n";
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
  /* want_divert pauses the parse at headers-complete, before the parser
   * consumes any body byte. The accessor carries a documented caveat:
   * "reused internally to track the CURRENT chunk's remaining byte count
   * once chunked parsing begins... calling this after body parsing has
   * already started returns a value with a different meaning". That caveat
   * does not apply yet, and this is exactly the window in which the real
   * caller in chttpserver.c uses it. */
  chttp1_parser_t parser;
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
  chttp1_parser_t parser;
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
  chttp1_parser_t parser;
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
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
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
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  /* This declares a chunk far larger than the override and sends none of
   * the data of that chunk. The rejection must happen the instant the
   * parser reads the chunk-size line, rather than waiting for ever for data
   * that never comes. */
  const char *chunk_size = "8000000000000000\r\n"; /* about 9.2 exabytes */
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
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, headers, strlen(headers)),
             CHTTP1_OK);
  const char *rest = "5\r\nhello\r\n0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, rest, strlen(rest)), CHTTP1_PAUSED);
  REQUIRE_FALSE(chttp1_chunk_size_limit_exceeded(&parser));
  REQUIRE_EQ(ctx.body_len, (size_t)5);
}

TEST(chunk_size_limit, not_set_for_an_unrelated_parse_error) {
  /* chttp1_chunk_size_limit_exceeded() must be false for every OTHER cause
   * of a CHTTP1_ERROR, and not merely because nobody set it, so this test
   * forces a different rejection, with a malformed status line, and then
   * reads the flag. */
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
  /* This is the point of the feature: a caller that diverts the read of the
   * body to a worker thread still needs to know one thing at
   * CHTTP1_HEADERS_ONLY time, namely whether it has already written a "100
   * Continue" interim response. */
  chttp1_parser_t parser;
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

/* ========================================================================== */
/*                WORKER-PULL STREAMING (chttp1_stream_t)                    */
/* ========================================================================== */

static void make_pair(int fds[2]) {
  REQUIRE_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, fds), 0);
}

/* The variant that does not block, which the stream_tls tests below need.
 * tls_drive_handshake() drives a TLS handshake with one thread, in a
 * ping-pong loop that calls the step of one side, then the step of the
 * other, and repeats. That loop deadlocks on a blocking socketpair: the
 * BIO_read inside SSL_accept and SSL_connect can block, waiting for bytes
 * that the peer never gets a chance to send, because the next thing this
 * thread would do is drive the step of that peer. Under gdb
 * that hang appears as ctls_conn_handshake_step blocked inside a plain
 * BIO_read, which calls the read(2) syscall.
 *
 * The plaintext "stream" tests above need none of this, because they
 * always put the poll(2) call of chttp1_stream_read or chttp1_stream_write
 * in front of every read and every write. The few of them that call read(2)
 * or write(2) directly only touch a fd after the peer has written to it, on
 * the same thread, so a socket in blocking mode never blocks there. */
static void make_nonblocking_pair(int fds[2]) {
  make_pair(fds);
  for (int i = 0; i < 2; i++) {
    /* An unchecked failure here leaves fds[i] in blocking mode, which, as the
     * comment directly above this function explains, deadlocks the
     * single-threaded ping-pong loop of tls_drive_handshake(). A quiet
     * failure of fcntl() would therefore become a real hang that is hard to
     * diagnose; this check turns it into a clean, immediate test
     * failure. */
    int flags = fcntl(fds[i], F_GETFL, 0);
    REQUIRE_GE(flags, 0);
    REQUIRE_EQ(fcntl(fds[i], F_SETFL, flags | O_NONBLOCK), 0);
  }
}

TEST(stream, prepare_no_leftover) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  REQUIRE_EQ(s.carry_len, (size_t)0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, leftover_drained_before_touching_fd) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "hello", 5, NULL));

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
  chttp1_stream_t s;
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
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  char buf[8];
  REQUIRE_EQ(chttp1_stream_read(&s, buf, sizeof(buf), 50), (ssize_t)-1);
  REQUIRE_TRUE(chttp1_stream_timed_out(&s));
  REQUIRE_EQ(chttp1_stream_last_error(&s), 0);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

/* Coverage for the documented contract of timeout_ms == 0, which is
 * "return immediately if fd is not already readable/writable right now".
 * Two code paths must obey it: chttp1_stream_write, whose TLS branch and
 * plaintext branch share one retry loop, and the plaintext branch of
 * chttp1_stream_read.
 *
 * Neither path may compute a fresh deadline of "now + timeout_ms" and then
 * derive "the time that is left until it" from a SECOND clock_gettime()
 * call before it calls poll(2) or tries the real I/O. For timeout_ms == 0,
 * any time at all between two such reads of the clock is already above a
 * budget of zero, and some time always passes, however little, so the
 * recomputed value is always at or below 0. Both functions would then
 * report a timeout even when the fd is ready right now, without ever
 * calling poll(2) or trying the real read(2) or write(2).
 *
 * The three tests below build exactly that "already ready" condition: for a
 * read, the data already sits in the receive buffer of the socket, and for
 * a write, an ordinary socketpair that was just connected is always
 * writable at once. The tests are therefore not vacuous: a timeout that the
 * code makes up in place of a real transfer fails them. */
TEST(stream, read_timeout_zero_returns_data_when_already_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  /* This thread writes these bytes itself, before the read below, so by the
     time chttp1_stream_read runs, the bytes already sit in the receive
     buffer of the kernel for fds[0]. They are therefore really available
     "right now", and no wait at all is needed. */
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
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[1], NULL, 0, NULL));
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
     comment above for the full account of the behaviour this pins. A freshly
     connected socketpair endpoint is always immediately writable (its send
     buffer starts empty), so this is genuinely "writable right now" with no
     wait required. */
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
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

TEST(stream, read_error_on_bad_fd) {
  /* poll(2) itself ignores a negative fd: POSIX says that an entry with
   * fd < 0 is never reported ready, so such an fd would only time out here,
   * with no error. A real error for an invalid fd needs an fd number that is
   * well formed and already closed, which poll(2) reports as ready with
   * POLLNVAL, and the read(2) after it then really fails with EBADF. */
  int fds[2];
  make_pair(fds);
  int bad_fd = fds[0];
  close(fds[0]);
  close(fds[1]);

  chttp1_stream_t s;
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
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1, NULL));
  chttp1_stream_release(&s);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_prepends_ahead_of_existing_carry) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
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
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], "x", 1, NULL));
  REQUIRE_TRUE(chttp1_stream_push_back_leftover(&s, "unused", 0));
  REQUIRE_EQ(s.carry_len, (size_t)1);
  chttp1_stream_release(&s);
  close(fds[0]);
  close(fds[1]);
}

TEST(stream, push_back_leftover_overflow_guard_rejects_without_allocating) {
  /* `total = len + existing` needs an overflow check of its own before the
   * code allocates `total` bytes. That sum is made of two values of separate
   * sizes: a chunk that the caller has just pushed back, and whatever
   * carry-over the stream already holds. Without that check, a sum near
   * SIZE_MAX wraps, the allocation is far too small, and the copy writes far
   * past its end.
   *
   * This test fakes `existing` to SIZE_MAX directly on the struct and leaves
   * carry NULL, so nothing ever allocates or touches a real buffer of that
   * size. This follows the pattern that this project uses for this class of
   * overflow guard: assert that the guard rejects before any real work
   * happens. */
  chttp1_stream_t s;
  memset(&s, 0, sizeof(s));
  s.prepared = true;
  s.carry = NULL;
  s.carry_len = SIZE_MAX;
  s.carry_pos = 0;

  char buf[4] = {'a', 'b', 'c', 'd'};
  REQUIRE_FALSE(chttp1_stream_push_back_leftover(&s, buf, sizeof(buf)));
  /* On a rejection, the carry-over that is already there must stay
     completely untouched. */
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

TEST(stream_tls, leftover_decrypted_bytes_drained_before_ctls_conn_read) {
  /* This matches the plaintext carry-over test. The leftover bytes here
   * stand for application bytes that are already decrypted, which the
   * reactor thread produces with ctls_conn_read() inside its own loop that
   * parses the headers; the leftover argument of
   * chttp1_stream_prepare_tls is never raw bytes off the wire. */
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
