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
 * White-box tests of chttp1_parser at the byte level. They drive the parser
 * directly, with no TLS and no chttpclient.c at all, on hand-built input that
 * is sometimes malformed or hostile. The suite in tests.c works on a real
 * socket and its mock server only ever sends well-formed responses, so it can't
 * reach input like this. tests.c and tests_tls.c cover the end-to-end "does
 * real traffic still work" question; this file only cares whether the parser
 * itself is correct.
 *
 * The "stream" test group, which covers chttp1_stream_t, does use local AF_UNIX
 * socketpairs, but only to drive its own read, write and poll logic. There is
 * no chttpclient.c, no TLS and no real network traffic in it.
 */

#include <fcntl.h>
#include <internal/chttp1_parser.h>
#include <internal/ctls.h>
#include <stdlib.h>
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

  /* Trailer fields go here and never into header_names/header_values. The
     parser hands them over through their own callback exactly so that a caller
     with one collection can't have a real header replaced by a trailer of the
     same name, and keeping two separate arrays here lets the tests assert on
     that split directly. */
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

/* Static storage duration, because chttp1_parser_init only borrows the settings
 * pointer and doesn't copy it, and its documented contract says the settings
 * must outlive the parser. A stack local here would leave every parser's
 * ->settings dangling as soon as init_test() returns. */
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

/* Static storage duration for the same reason as g_test_settings above. */
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

/* REQUIRE_STREQ expands to strcmp, which is undefined on NULL, and a SIGSEGV
   there takes down the whole binary instead of failing one test, so every other
   test of the run loses its pass/fail signal. These two return a recognizable
   string for a missing field instead, so an assertion about a field's value
   fails cleanly when the field isn't there. */
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
  /* Real world compatibility: the RFC 7230 ABNF wants a trailing SP even for an
   * empty reason phrase, but real servers leave it out, as in "HTTP/1.1
   * 304\r\n". */
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
  /* We don't restrict the size of the number, because nonstandard status codes
   * up to 599 are in real use (599 Network Connect Timeout Error, for example,
   * which some load balancers and proxies send). So the parser accepts any
   * 3-digit code. */
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
  /* A regression test. chttp1_should_keep_alive must not decide "HTTP/1.1 or
   * later" with "http_major > 0 && http_minor > 0", which reads any version
   * with a zero minor part as HTTP/1.0 or older. A literal "HTTP/2.0" status
   * line is such a version and this parser's grammar accepts it (see
   * status_line.any_major_minor_digit_accepted). The parser would then want an
   * explicit "Connection: keep-alive" token, which no real server of that
   * version sends, and connection reuse would silently stop. The right test is
   * "major > 1, or major == 1 with minor >= 1". */
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
  /* A continuation line that starts with whitespace ("obs-fold") must be
   * rejected outright and not unfolded into the previous header's value. */
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
  /* CHTTP1_MAX_LINE_LEN is 8192 and the raw accumulated line is "X-Pad: " (7
   * bytes) + value + CRLF (2 bytes), so the biggest value that still fits is
   * 8192 - 7 - 2 = 8183 bytes. */
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
  /* 80 headers of about 1000 bytes each make 80KB, well above the 64KB total
   * cap but well below the cap of 100 headers and the 8192-byte cap per line,
   * so only the limit on total bytes is in play. We feed the bytes piece by
   * piece, the status line first and then one header line per
   * chttp1_parser_execute call, so we can stop as soon as the parser reports
   * the error instead of building one huge buffer up front. */
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
   * bound "request line + all header lines", so the request line (or the status
   * line) has to add to total_header_bytes itself, and process_header_line
   * can't be the only thing adding to it. Otherwise a request-target far above
   * the configured cap gets accepted, bounded only by the much bigger
   * CHTTP1_MAX_LINE_LEN. The request line alone here is well over the small
   * override below and nowhere near CHTTP1_MAX_LINE_LEN, and the parser must
   * reject it before it sees any header. */
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
  /* A request line and a header line can each fit by themselves while their SUM
   * is above the override, and the parser still rejects them. This shows
   * the request line really adds to the same running total as a header line,
   * and isn't tracked separately or ignored. */
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
   * Transfer-Encoding isn't "chunked", so the recipient reads the body until
   * the connection closes and the parser doesn't reject it. It does reject a
   * REQUEST in the same situation (see the request_body_framing group), because
   * a request has no EOF framing to fall back on. */
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
  /* RFC 7230 SS3.3.3 covers this one. A Transfer-Encoding and a Content-Length
   * framing the same message are an ambiguous pair, which a recipient has to
   * resolve in favour of the Transfer-Encoding or reject. A recipient that
   * accepts it frames the response at exactly Content-Length bytes, leaves
   * whatever follows unread and reports the message as cleanly complete and
   * fine for keep-alive. The surplus bytes then sit on a connection that the
   * client gives back to its idle pool, and the next request reusing that
   * connection reads them as its own response.
   *
   * Narrow this rejection to request mode and the test fails with
   * CHTTP1_PAUSED, a 3-byte body and a chttp1_should_keep_alive() that answers
   * true, with "EXTRA" left unread. */
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
  /* The order of the headers doesn't change the ambiguity, because the parser
   * decides on the rejection after it has read every Transfer-Encoding line and
   * not on the line that comes second. */
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
   * ends in "gzip" here. So this is the same ambiguous pair even though
   * "chunked" appears in the list, and a per-line test that only looked for
   * that word would let it through. */
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
  /* The counterpart that the rejection above must not swallow. With no
   * Content-Length there is nothing to disagree with, and RFC 7230 SS3.3.3
   * frames exactly this response: the recipient reads until the connection
   * closes. Such a message is never fine for keep-alive, so it can't hurt a
   * pooled connection. */
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
  /* RFC 7230 SS3.3.1 says "A sender MUST NOT apply chunked more than once to a
   * message body". RFC 7230 SS3.2.2 merges these two lines into the single list
   * "chunked, gzip, chunked", which applies chunked twice. One recipient
   * de-chunks once and another twice, so they disagree on where the body ends,
   * and that disagreement in framing is all response smuggling needs. So the
   * parser refuses the message instead of resolving it either way.
   *
   * Without counting occurrences over the merged list the parser
   * takes the message as an ordinary chunked response, which completes with
   * CHTTP1_PAUSED, a 3-byte body and a chttp1_should_keep_alive() that answers
   * true. */
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
  /* The same rule in its plainest form: two Transfer-Encoding lines that each
   * say "chunked" merge into the list "chunked, chunked". */
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
  /* Both occurrences on one line make the same message and the parser
   * refuses it the same way, because it counts transfer-codings and not
   * header lines. */
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
  /* A transfer-coding name is case-insensitive, so other spellings count as the
   * same coding, and a literal match would let this message through. */
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
  /* Here the merged list ends in "chunked" ("gzip, chunked, chunked"), so
   * otherwise this message would get chunked framing. It still applies chunked
   * twice, and the parser still refuses it. */
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
  /* The counterpart that the rejection above must not swallow. RFC 7230 SS3.3.1
   * lets a response apply chunked ONCE somewhere other than last, and RFC 7230
   * SS3.3.3 frames such a message by reading until the connection closes. A
   * rule that refused every "chunked" before the end of the list would refuse
   * this legal message, which is why the parser counts occurrences and doesn't
   * look at the position. */
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
  /* Merging across lines works: "gzip" and then "chunked" is a valid way to
   * spell the list "gzip, chunked", which is chunked-framed and fine for
   * keep-alive. */
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

/* A Transfer-Encoding line that names no coding only adds empty elements to the
 * merged list, and RFC 7230 SS7 has a recipient ignore those. So the list
 * "chunked" followed by such a line is still the list "chunked", just like the
 * single line "chunked," is. The tests below pin that for both modes, and pin
 * that the rules about the merged list (chunked applied twice, chunked beside
 * Content-Length, a final coding other than chunked) see through the empty line
 * wherever it sits. */

typedef struct {
  const char *label;
  const char *te_lines; /* the Transfer-Encoding lines, each ending in CRLF */
} te_empty_case_t;

/* Once the empty elements are dropped each of these merged lists is
 * "chunked". */
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

/* Each of these merged lists ends in a coding other than chunked. */
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
  /* Suppose an empty line re-derives the final coding: "chunked, then
   * empty" and its siblings take EOF framing, the execute call returns
   * CHTTP1_OK instead of CHTTP1_PAUSED and the chunk framing reaches the caller
   * as body bytes. */
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
  /* An empty line changes nothing in the other direction either, a list that
   * ends in "gzip" stays EOF-framed wherever the empty line is. */
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
  /* Whatever the order of the lines the merged list is "chunked", so a
   * Content-Length beside it makes the ambiguous pair every time. */
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
  /* If an empty line re-derives the final coding, the request
   * below reads as a Transfer-Encoding that doesn't end in chunked and the
   * parser refuses it with CHTTP1_ERROR. */
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
  /* A trailer field comes through the trailer callback and never through
     on_header, so a caller with one headers collection can't get a header
     replaced by a trailer of the same name. */
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

/* RFC 7230 SS4.1.2 trailer fields arrive once the framing of a message is
   settled and, on the server side, after the request has been routed. These
   tests pin the two properties that follow: a trailer comes through a callback
   of its own (never on_header), and it changes nothing in how the parser itself
   sees the message. */

TEST(trailer_isolation, trailer_does_not_reach_on_header_in_response_mode) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test(&parser, &ctx);
  const char *msg =
      "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n"
      "X-Origin: upstream\r\n\r\n"
      "5\r\nhello\r\n0\r\nX-Origin: forged\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  /* The response header keeps its own value. A consumer that puts every
     on_header line into one map (which is exactly what chttpclient.c does, and
     its insert overwrites) can never see it replaced by the trailer. */
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Origin"), "upstream");
  REQUIRE_EQ(ctx.header_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Origin"), "forged");
}

TEST(trailer_isolation, trailer_does_not_reach_on_header_in_request_mode) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  /* The privilege-escalation case this split is there to close: a request
     behind a proxy that sets X-Forwarded-For and an authentication gateway that
     sets X-Authenticated-User, while the client appends trailers with the same
     names. Both header values must survive. */
  const char *msg =
      "POST /x HTTP/1.1\r\nHost: h\r\nTransfer-Encoding: chunked\r\n"
      "X-Forwarded-For: 203.0.113.9\r\n"
      "X-Authenticated-User: guest\r\n\r\n"
      "5\r\nhello\r\n"
      "0\r\nX-Forwarded-For: 127.0.0.1\r\nX-Authenticated-User: admin\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Forwarded-For"), "203.0.113.9");
  REQUIRE_STREQ(header_or_absent(&ctx, "X-Authenticated-User"), "guest");
  /* Host, Transfer-Encoding, X-Forwarded-For and X-Authenticated-User. The Host
     line isn't optional, RFC 7230 SS5.4 makes a request without one a 400 and
     this parser enforces that in request mode. */
  REQUIRE_EQ(ctx.header_count, (size_t)4);
  REQUIRE_EQ(ctx.trailer_count, (size_t)2);
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Forwarded-For"), "127.0.0.1");
  REQUIRE_STREQ(trailer_or_absent(&ctx, "X-Authenticated-User"), "admin");
}

TEST(trailer_isolation, trailer_cannot_change_framing_or_connection_state) {
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  /* Every header name that matters for framing, all in the trailer part: a
     Transfer-Encoding that would clear chunked framing, a Content-Length that
     would then get accepted without tripping the mutual-exclusion check, and a
     Connection token that would turn a keep-alive connection into a closing
     one. None of them may be interpreted. */
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
  /* Inert doesn't mean unchecked: a trailer line is held to exactly the same
     name-token and value-byte grammar as a header line. */
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
  /* The configuration that both chttpclient.c and chttpserver.c ship with, no
     trailer callback at all, so a trailer is validated, charged against the
     caps and then dropped. Neither the message nor the header set changes. */
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
  /* Trailers don't get a budget of their own: a trailer section that goes past
   * CHTTP1_MAX_HEADER_COUNT (100) must be rejected exactly like too many
   * regular headers. */
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
  /* "Connection: keep-alive" was present, but an EOF-delimited body is never
   * fine for keep-alive since ending the body already meant ending the
   * connection. */
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
       * big split, for example), so nothing is left to feed. */
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
    /* tau's REQUIRE_EQ _Generic printer doesn't handle bool, hence the cast to
     * int. */
    REQUIRE_EQ((int)chttp1_should_keep_alive(&parser), (int)ref_keep_alive);
    /* A hard contract: execute() never returns CHTTP1_OK with bytes left
     * over, so "rv1 == CHTTP1_OK" above already means the first call
     * consumed all `split` bytes. Nothing else to check here, getting this
     * far is the check. */
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
  /* chttpclient.c itself never calls chttp1_parser_finish once execute() has
   * returned CHTTP1_PAUSED, it reports success immediately, as the documented
   * contract of chttp1_parser_execute says. We call finish anyway, only to
   * drive the CHTTP1_FINISH_SAFE case of the switch directly. A message that
   * completed on a normal path, not framed by EOF, leaves finish_state at
   * CHTTP1_FINISH_SAFE, and finish() reports that as CHTTP1_OK without calling
   * on_message_complete again. */
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
  /* The CHTTP1_FINISH_SAFE_WITH_CB case must update finish_state after firing
   * on_message_complete, and not only parser->state. Otherwise finish_state
   * stays at CHTTP1_FINISH_SAFE_WITH_CB, and a second finish() call on a parser
   * already at CHTTP1_ST_MESSAGE_DONE takes the same branch again and calls
   * on_message_complete again. That breaks two contracts: the callback is
   * documented to fire exactly once, and this function is documented to give
   * CHTTP1_OK for a parser that is already at a clean boundary. The two extra
   * finish() calls below make the test meaningful, each one should report
   * CHTTP1_OK and neither may fire the callback again. */
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
  /* RFC 7230 SS3.1.1 separates the request-line fields with single spaces and
     SS5.3 allows no whitespace inside the request-target, so an HTAB isn't
     content here the way it is inside a header value. Accepting one would let a
     front end that splits the request-line on whitespace disagree with this
     server about which resource was asked for, and that is a request smuggling
     primitive, not leniency. */
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
  /* The target check rejects every byte at or below SP, plus DEL, and nothing
     else, so a target made of printable bytes, punctuation included, should get
     through. */
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
  /* RFC 7230 SS3.5 says a server SHOULD ignore at least one empty line before
   * the request-line, and some clients send a stray CRLF after a POST body. A
   * regression test: the parser must not reject such a line outright with
   * PH_ERROR (method_len == 0), because that would close a healthy keep-alive
   * or pipelined connection over a single stray CRLF. */
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
  /* Bounded and not skipped for ever: a client that keeps sending blank lines
   * has to be treated as malformed input at some point instead of being
   * tolerated indefinitely. */
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
  /* Unlike a response, a request with no Content-Length and no chunked
   * Transfer-Encoding has NO body at all. The message completes right after the
   * headers instead of waiting for an EOF, which would never come anyway
   * because the connection doesn't close. */
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

   The parser enforces this in request mode at the blank line that ends the
   header block, since only there is the number of Host lines final. Two Host
   lines let a front end and a back end disagree about the authority a request
   names, which is a request-smuggling primitive. Response mode must not enforce
   any of it, a Host header in a response is just an ordinary field with no
   meaning. */

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
  /* The rule's text puts no version condition on the duplicate case ("any
     request message that contains more than one Host header field"), and the
     reason behind it doesn't depend on a version either. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\nHost: a\r\nHost: b\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(host_header, http_1_0_without_host_accepted) {
  /* The missing-Host half of the rule only names HTTP/1.1. HTTP/1.0 is older
     than the field, and a request without one is ordinary there. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "GET /path HTTP/1.0\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_PAUSED);
  REQUIRE_TRUE(ctx.message_complete_called);
}

TEST(host_header, host_matched_without_regard_to_case) {
  /* Field names are case-insensitive (RFC 7230 SS3.2). A sender spelling it
     "HOST" or "hOsT" satisfies the rule, and two such spellings still count as
     two. */
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
  /* RFC 3986 SS3.2 allows no whitespace in an authority, and a front end that
     splits on it forwards another authority than the one a back end reads.
     Leading and trailing optional whitespace is trimmed before this check, so
     only an interior byte gets here. */
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
  /* Response mode must not pick up any of this, a response with no Host and a
     response with two are both ordinary messages. */
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
  /* A trailer field arrives after the framing is settled. It must not supply
     the Host of a request that had none, and it must not turn a request with
     exactly one into a duplicate. process_header_line() only reaches the
     counter for a real header line. */
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
  /* Requests have no whitelist of trailer names, any trailer name is accepted,
   * the same as in response mode where trailers aren't restricted either. */
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
  /* RFC 7230 SS3.3.3 covers this. When the final coding of a request's
   * Transfer-Encoding is anything but "chunked" the message length is
   * undetermined, and a server that follows the RFC MUST reject the request
   * outright instead of quietly reading it as a request with no body. Requests
   * and responses differ here, since a request has no EOF framing to fall back
   * on and a response does. A regression test: the parser must reject the
   * request and not accept it as if the Transfer-Encoding weren't there. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg = "POST /x HTTP/1.1\r\nTransfer-Encoding: gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(
    request_body_framing,
    transfer_encoding_chunked_not_last_split_across_two_header_lines_rejected) {
  /* A regression test. transfer_encoding_has_nonfinal_chunked only sees one
   * header line at a time, so an EARLIER Transfer-Encoding line can claim
   * "chunked" as final while a SECOND Transfer-Encoding line follows. RFC 7230
   * SS3.2.2 joins repeated header lines into one comma-separated list, in
   * order, so such a pair must not get past that check, even though checking
   * each line separately lets it through. */
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
  /* The legal counterpart of the case above: "chunked" is the LAST token of
   * the LAST Transfer-Encoding line (the second one here), and the first
   * line doesn't end in chunked. That's valid framing and the parser must
   * accept it. */
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
  /* Response mode has its own documented reduction of scope (see the doc
   * comment of value_ends_with_chunked), which has nothing to do with the
   * rejection above because that rejection only exists in request mode. A
   * response whose Transfer-Encoding doesn't end in "chunked" falls back to EOF
   * framing instead of being rejected. */
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
  /* The single-line path of transfer_encoding_has_nonfinal_chunked; the path
   * above covers a list split over two header lines. Here "chunked" shows up
   * before the end of one line's token list, which is always wrong, and the
   * parser must reject it at once whatever the last token is. */
  chttp1_parser_t parser;
  test_ctx_t ctx;
  init_test_request(&parser, &ctx);
  const char *msg =
      "POST /x HTTP/1.1\r\nTransfer-Encoding: chunked, gzip\r\n\r\n";
  REQUIRE_EQ(chttp1_parser_execute(&parser, msg, strlen(msg)), CHTTP1_ERROR);
}

TEST(request_body_framing, single_line_gzip_then_chunked_accepted) {
  /* The legal counterpart on one line: several comma-separated codings on ONE
   * Transfer-Encoding line, with the list ending in "chunked". That's valid
   * framing, and the parser must accept it and parse the body as chunked. */
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
  /* Response mode's reduction of scope applies to several codings on one line
   * just as it does to the single-coding case above: the parser does NOT reject
   * a response whose Transfer-Encoding line ends in something other than
   * "chunked". transfer_encoding_has_nonfinal_chunked only runs under
   * CHTTP1_PARSE_REQUEST, and since the final coding isn't "chunked" the
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
  /* The response mode match of single_line_gzip_then_chunked_accepted above.
   * value_ends_with_chunked can't tell a request from a response, so a response
   * whose Transfer-Encoding line ends in "chunked" gets its body parsed as
   * chunked, whatever comes before it on that line. */
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
  /* RFC 7230 SS3.3.1 says "MUST NOT apply chunked more than once", and that
   * binds a request exactly like a response. RFC 7230 SS3.2.2 merges these two
   * lines into "chunked, chunked" and the parser refuses the message.
   * Without counting occurrences over the merged list the final
   * coding of the second line is "chunked", so the parser takes the message as
   * an ordinary chunked request that completes with CHTTP1_PAUSED and a 3-byte
   * body. */
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
  /* Both occurrences are on one line and the parser refuses them the same way.
   * In request mode two separate rules reject this message, the count of
   * occurrences and "chunked must be the last transfer-coding of a request", so
   * this test pins the outcome but doesn't isolate the count;
   * framing.response_chunked_twice_within_one_line_rejected is the test that
   * isolates that rule, because the final coding rule doesn't bind a response
   * at all. */
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
  /* The important carry-over case: the header block AND the body bytes come in
   * the SAME chttp1_parser_execute call. The diversion must still stop
   * exactly at the header boundary and leave the body bytes unconsumed, so the
   * caller can pass them on as carry-over, instead of swallowing them into
   * on_body just because they are already there. */
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
  /* Nothing to divert (no Content-Length, no chunked), so want_divert is
   * downgraded to an ordinary immediate completion instead of a pause. */
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
  /* CHTTP1_HEADERS_DIVERT_BODY only exists in request mode. A response-mode
   * parser treats it as an invalid hint, since chttpclient.c has no worker
   * thread to hand the body off to. */
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
   * consumes any body byte. The accessor has a documented caveat, "reused
   * internally to track the CURRENT chunk's remaining byte count once chunked
   * parsing begins... calling this after body parsing has already started
   * returns a value with a different meaning", but it doesn't apply yet, and
   * this is exactly the window in which the real caller in chttpserver.c uses
   * it. */
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
  /* max_chunk_size_override defaults to 0 (no cap), so even a large chunk-size
   * token is accepted as a valid line with nothing to compare it with. */
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
  /* Declares a chunk far bigger than the override and sends none of its data.
   * The rejection must happen as soon as the parser reads the chunk-size line,
   * and not after waiting for ever for data that never arrives. */
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
  /* chttp1_chunk_size_limit_exceeded() must be false for every OTHER cause of a
   * CHTTP1_ERROR, and not only because nobody set it, so we force a different
   * rejection with a malformed status line and then read the flag. */
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
  /* This is what the feature is for: a caller that moves the body read to a
   * worker thread still needs to know one thing at CHTTP1_HEADERS_ONLY time,
   * whether it has already written a "100 Continue" interim response. */
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

/* The non-blocking variant that the stream_tls tests below need.
 * tls_drive_handshake() drives a TLS handshake on one thread in a ping-pong
 * loop, one side's step, then the other side's, and again. On a blocking
 * socketpair that loop deadlocks: the BIO_read inside SSL_accept and
 * SSL_connect can block waiting for bytes the peer never gets to send, because
 * driving the peer's step is the next thing this very thread would do. In gdb
 * the hang shows ctls_conn_handshake_step blocked inside a plain BIO_read,
 * which calls the read(2) syscall.
 *
 * The plaintext "stream" tests above don't need any of this, since
 * chttp1_stream_read and chttp1_stream_write always poll(2) before every read
 * and every write. The few of them that call read(2) or write(2) directly only
 * touch a fd after the peer wrote to it on the same thread, so a blocking
 * socket never blocks there. */
static void make_nonblocking_pair(int fds[2]) {
  make_pair(fds);
  for (int i = 0; i < 2; i++) {
    /* If this failed unnoticed, fds[i] would stay blocking, which deadlocks the
     * single-threaded ping-pong loop of tls_drive_handshake() as the comment
     * just above this function explains. A silent fcntl() failure would turn
     * into a real hang that is hard to diagnose, and this check turns it into a
     * clean test failure at once. */
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

  /* The carry-over is fully drained at this point, so check that a further read
   * goes to the real fd and doesn't return more (nonexistent) carry-over. */
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

/* Coverage for the documented contract of timeout_ms == 0, which is "return
 * immediately if fd is not already readable/writable right now". Two code
 * paths must honour it: chttp1_stream_write, whose TLS and plaintext
 * branches share one retry loop, and the plaintext branch of
 * chttp1_stream_read.
 *
 * Neither path may compute a fresh "now + timeout_ms" deadline and then work
 * out the time left until it from a SECOND clock_gettime() call before it
 * calls poll(2) or tries the real I/O. For timeout_ms == 0 any time at all
 * between the two clock reads is already over a budget of zero, and some
 * time always passes however little, so the recomputed value is always at or
 * below 0. Both functions would then report a timeout even with the fd ready
 * right now, without ever calling poll(2) or trying the real read(2) or
 * write(2).
 *
 * The three tests below set up exactly that "already ready" situation: for a
 * read the data is already in the socket's receive buffer, and for a write a
 * freshly connected socketpair is always writable. So they are not vacuous,
 * a timeout that the code makes up instead of doing a real transfer fails
 * them. */
TEST(stream, read_timeout_zero_returns_data_when_already_available) {
  int fds[2];
  make_pair(fds);
  chttp1_stream_t s;
  REQUIRE_TRUE(chttp1_stream_prepare(&s, fds[0], NULL, 0, NULL));
  /* This thread writes these bytes itself before the read below, so when
     chttp1_stream_read runs they are already in the kernel's receive buffer for
     fds[0]. They really are available "right now", and no waiting at all is
     needed. */
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
  /* The other half of the timeout_ms == 0 contract: when nothing is available,
     one immediate non-blocking check reports a timeout (not a hang, and not a
     fake successful read of zero bytes). */
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
  /* read_timeout_zero_returns_data_when_already_available's doc comment above
     has the full story of what this pins. A freshly connected socketpair end is
     always writable at once (its send buffer starts empty), so this really is
     "writable right now" with nothing to wait for. */
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
  /* poll(2) itself ignores a negative fd (POSIX says an entry with fd < 0 is
   * never reported ready), so such an fd would only time out here without an
   * error. For a real error on an invalid fd we need a well-formed fd number
   * that is already closed: poll(2) reports it ready with POLLNVAL and the
   * read(2) after it really fails with EBADF. */
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
  /* `total = len + existing` needs its own overflow check before we allocate
   * `total` bytes. The sum is made of two separately sized values, the chunk
   * the caller has just pushed back and whatever carry-over the stream already
   * holds. Without the check a sum near SIZE_MAX wraps, the allocation is far
   * too small and the copy writes way past its end.
   *
   * The test fakes `existing` as SIZE_MAX directly on the struct and leaves
   * carry NULL, so nothing ever allocates or touches a real buffer of that
   * size. That's the usual pattern of this project for this kind of overflow
   * guard: assert that the guard rejects before any real work happens. */
  chttp1_stream_t s;
  memset(&s, 0, sizeof(s));
  s.prepared = true;
  s.carry = NULL;
  s.carry_len = SIZE_MAX;
  s.carry_pos = 0;

  char buf[4] = {'a', 'b', 'c', 'd'};
  REQUIRE_FALSE(chttp1_stream_push_back_leftover(&s, buf, sizeof(buf)));
  /* When it rejects, the carry-over that's already there stays completely
     untouched. */
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
  /* Same as the plaintext carry-over test. The leftover bytes stand for
   * application bytes that are already decrypted, which the reactor thread gets
   * with ctls_conn_read() inside its own header parsing loop, so the leftover
   * argument of chttp1_stream_prepare_tls is never raw bytes off the wire. */
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

/* ========================================================================== */
/*                     PARSER RESET                                          */
/* ========================================================================== */

/* chttp1_parser_init() and chttp1_parser_init_request() clear the parser's
 * state and leave its line buffer alone. These tests parse a message after
 * resetting a parser whose line buffer holds the bytes of a long earlier
 * message, or bytes the test poisons it with on purpose, and they check every
 * result of the new message. The poison bytes are the ones the line readers act
 * on: the end of a string, CR, LF, a header's colon, whitespace, a digit, a
 * token byte, DEL and a byte above 0x7F. */
static const unsigned char g_reset_poison[] = {0x00, '\r', '\n', ':', ' ',
                                               '0',  'X',  0x7f, 0xff};
#define RESET_POISON_COUNT (sizeof(g_reset_poison) / sizeof(g_reset_poison[0]))

static void reset_poison_line_buf(chttp1_parser_t *parser, unsigned char b) {
  memset(parser->line_buf, b, sizeof(parser->line_buf));
}

/* Appends n copies of c, or the text s, to the buffer at *pos. */
static void reset_put_run(char *buf, size_t *pos, char c, size_t n) {
  memset(buf + *pos, c, n);
  *pos += n;
}

static void reset_put(char *buf, size_t *pos, const char *s) {
  size_t n = strlen(s);
  memcpy(buf + *pos, s, n);
  *pos += n;
}

/* A request whose request line and one header line each nearly fill the 8 KiB
 * line buffer, and which sets every flag a short request must not inherit:
 * Connection: close, Expect: 100-continue and a Content-Length. */
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

/* A chunked response whose reason phrase, one header line, a chunk-size line
 * and a trailer line each nearly fill the line buffer, and which asks to close
 * the connection. */
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

/* Feeds msg in two pieces, split at split (0 < split <= len), and returns the
 * result of the call that ended the message, or the first failure. */
static chttp1_errno_t reset_feed_split(chttp1_parser_t *parser, const char *msg,
                                       size_t len, size_t split) {
  chttp1_errno_t r = chttp1_parser_execute(parser, msg, split);
  if (r != CHTTP1_OK || split == len) return r;
  return chttp1_parser_execute(parser, msg + split, len - split);
}

/* The short request of the request-mode tests, and what every check expects
 * from it. */
static const char g_reset_short_request[] =
    "PUT /b HTTP/1.1\r\nHost: y\r\nX-A: 1\r\nContent-Length: 2\r\n\r\nhi";

/* Checks every result of g_reset_short_request. It returns the number of failed
 * checks, so a caller can release what it holds before it asserts. */
static int reset_check_short_request(chttp1_parser_t *parser,
                                     const test_ctx_t *ctx, chttp1_errno_t r,
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
  chttp1_errno_t dirty_r = CHTTP1_OK;
  for (size_t split = 1; split <= len; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test_request(&parser, &ctx);
    dirty_r = chttp1_parser_execute(&parser, dirty, dirty_len);
    if (dirty_r != CHTTP1_PAUSED) break;
    init_test_request(&parser, &ctx);
    chttp1_errno_t r =
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
      chttp1_parser_t parser;
      test_ctx_t ctx;
      init_test_request(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno_t r =
          reset_feed_split(&parser, g_reset_short_request, len, split);
      size_t last = split == len ? len : len - split;
      if (reset_check_short_request(&parser, &ctx, r, last) != 0) failures++;
    }
  }
  REQUIRE_EQ(failures, 0);
}

/* The short responses of the response-mode tests. */
static const char g_reset_short_response[] =
    "HTTP/1.1 200 OK\r\nX-A: 1\r\nContent-Length: 2\r\n\r\nhi";
static const char g_reset_short_chunked[] =
    "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n"
    "2;x=y\r\nhi\r\n1\r\n!\r\n0\r\nX-T: 9\r\n\r\n";

static int reset_check_short_response(chttp1_parser_t *parser,
                                      const test_ctx_t *ctx, chttp1_errno_t r,
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

static int reset_check_short_chunked(chttp1_parser_t *parser,
                                     const test_ctx_t *ctx, chttp1_errno_t r,
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
  chttp1_errno_t dirty_r = CHTTP1_OK;
  for (size_t split = 1; split <= clen; split++) {
    chttp1_parser_t parser;
    test_ctx_t ctx;
    init_test(&parser, &ctx);
    dirty_r = chttp1_parser_execute(&parser, dirty, dirty_len);
    if (dirty_r != CHTTP1_PAUSED) break;
    if (split <= len) {
      init_test(&parser, &ctx);
      chttp1_errno_t r =
          reset_feed_split(&parser, g_reset_short_response, len, split);
      size_t last = split == len ? len : len - split;
      if (reset_check_short_response(&parser, &ctx, r, last) != 0) failures++;
      /* The chunked response comes after the first short one, on a parser that
       * two messages have written to. */
    }
    init_test(&parser, &ctx);
    chttp1_errno_t r =
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
      chttp1_parser_t parser;
      test_ctx_t ctx;
      if (split <= len) {
        init_test(&parser, &ctx);
        reset_poison_line_buf(&parser, g_reset_poison[k]);
        chttp1_errno_t r =
            reset_feed_split(&parser, g_reset_short_response, len, split);
        size_t last = split == len ? len : len - split;
        if (reset_check_short_response(&parser, &ctx, r, last) != 0) failures++;
      }
      init_test(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno_t r =
          reset_feed_split(&parser, g_reset_short_chunked, clen, split);
      size_t last = split == clen ? clen : clen - split;
      if (reset_check_short_chunked(&parser, &ctx, r, last) != 0) failures++;
    }
  }
  REQUIRE_EQ(failures, 0);
}

/* What the spill tests record about the one long header of their response,
 * whose value the ordinary callback cuts at 255 bytes. */
static size_t g_reset_long_value_len;
static bool g_reset_long_value_ok;

static int reset_spill_on_header(chttp1_parser_t *p, const char *name,
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

static const chttp1_settings_t g_reset_spill_settings = {
    .on_header = reset_spill_on_header,
    .on_trailer = t_on_trailer,
    .on_headers_complete = t_on_headers_complete,
    .on_body = t_on_body,
    .on_message_complete = t_on_message_complete,
};

/* A response with one header line of value_len bytes of 'v' between two short
 * header lines, which spills once it gets past the 8 KiB line buffer. */
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

static void reset_spill_init(chttp1_parser_t *parser, test_ctx_t *ctx) {
  memset(ctx, 0, sizeof(*ctx));
  chttp1_parser_init(parser, &g_reset_spill_settings);
  parser->data = ctx;
  (void)chttp1_parser_enable_line_spill(parser, NULL);
  g_reset_long_value_len = 0;
  g_reset_long_value_ok = false;
}

TEST(parser_reset, a_spilled_line_after_a_reset_keeps_exactly_its_own_bytes) {
  /* The first response spills a 20000-byte line and leaves the line buffer full
   * of it. After the release and the reset the buffer is poisoned and a
   * response with a 10000-byte line is fed in two pieces; a split inside the
   * fixed buffer, one at its end and one inside the spill buffer each move the
   * line's bytes through the copy into the heap buffer. */
  size_t first_len = 0, second_len = 0;
  char *first = reset_spill_response(20000, &first_len);
  char *second = reset_spill_response(10000, &second_len);
  REQUIRE_TRUE(first != NULL && second != NULL);
  int failures = 0;
  chttp1_errno_t first_r = CHTTP1_OK;
  bool first_spilled = false;
  /* The X-Long line starts at byte 25 and fills the fixed buffer at byte 8217,
   * and its CRLF is at bytes 10033 and 10034. */
  size_t splits[] = {1,    2,    20,   25,    26,    33,    34,
                     100,  4096, 8190, 8191,  8192,  8216,  8217,
                     8218, 9000, 9999, 10033, 10034, 10035, second_len};
  for (size_t k = 0; k < RESET_POISON_COUNT; k++) {
    for (size_t s = 0; s < sizeof(splits) / sizeof(splits[0]); s++) {
      size_t split = splits[s];
      chttp1_parser_t parser;
      test_ctx_t ctx;
      reset_spill_init(&parser, &ctx);
      first_r = chttp1_parser_execute(&parser, first, first_len);
      first_spilled = parser.line_spill != NULL;
      chttp1_parser_release(&parser);
      if (first_r != CHTTP1_PAUSED || !first_spilled) break;
      reset_spill_init(&parser, &ctx);
      reset_poison_line_buf(&parser, g_reset_poison[k]);
      chttp1_errno_t r = reset_feed_split(&parser, second, second_len, split);
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

TEST(parser_reset, a_parser_in_uninitialized_memory_reads_no_unwritten_byte) {
  /* Each parser lives in a fresh malloc block whose line buffer nobody has
   * written. Under valgrind, reading a byte the parser hasn't written since its
   * init, when it decides a branch, is an error of the run. */
  const size_t rlen = sizeof(g_reset_short_request) - 1;
  const size_t clen = sizeof(g_reset_short_chunked) - 1;
  int failures = 0;
  bool allocated = true;
  for (size_t split = 1; split <= clen && allocated; split++) {
    chttp1_parser_t *parser = malloc(sizeof(*parser));
    test_ctx_t *ctx = malloc(sizeof(*ctx));
    if (!parser || !ctx) {
      free(parser);
      free(ctx);
      allocated = false;
      break;
    }
    if (split <= rlen) {
      init_test_request(parser, ctx);
      chttp1_errno_t r =
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
    chttp1_errno_t r =
        reset_feed_split(parser, g_reset_short_chunked, clen, split);
    size_t last = split == clen ? clen : clen - split;
    if (reset_check_short_chunked(parser, ctx, r, last) != 0) failures++;
    free(parser);
    free(ctx);
  }
  REQUIRE_TRUE(allocated);
  REQUIRE_EQ(failures, 0);
}
