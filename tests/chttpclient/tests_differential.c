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
 * Differential tests: chttp1_parser.c cross-checked against picohttpparser
 * (vendored under picohttpparser/, an independent, production-deployed
 * HTTP/1.x parser used by the h2o web server), on the same raw byte
 * sequences. Two independently written parsers agreeing on request/response
 * line extraction, header extraction, and chunked-body decoding is much
 * stronger evidence against a subtle framing bug (an off-by-one in chunk-size
 * parsing, a header-value trimming mistake, and so on) than either parser's
 * own self-consistent test suite alone.
 *
 * Scope: picohttpparser only parses request/status lines, headers, and
 * decodes chunked-body octets; it does not itself enforce this project's own
 * additional RFC 7230 SS3.3.1 hardening (rejecting "chunked" unless it is the
 * final Transfer-Encoding token, the request/response framing asymmetry for
 * a non-chunked-final Transfer-Encoding, and so on), so that hardening is
 * intentionally out of scope for this file and stays covered by its own
 * dedicated regression tests in tests_parser.c. This file only compares the
 * two parsers on messages both are expected to accept.
 */

#include <chttp1_parser.h>
#include <string.h>

#include "../tau/tau.h"
#include "picohttpparser/picohttpparser.h"

TAU_MAIN()

#define MAX_HDRS 32
#define BODY_CAP (64 * 1024)

typedef struct {
  char names[MAX_HDRS][256];
  char values[MAX_HDRS][256];
  size_t count;

  char method[32];
  char target[256];
  int status_code;

  char body[BODY_CAP];
  size_t body_len;
  bool message_complete_called;
} chttp1_ctx_t;

static int c_on_request_line(chttp1_parser_t *p, const char *method,
                             size_t method_len, const char *target,
                             size_t target_len) {
  chttp1_ctx_t *ctx = (chttp1_ctx_t *)p->data;
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

static int c_on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                       const char *value, size_t value_len) {
  chttp1_ctx_t *ctx = (chttp1_ctx_t *)p->data;
  if (ctx->count < MAX_HDRS) {
    size_t nl = name_len < 255 ? name_len : 255;
    size_t vl = value_len < 255 ? value_len : 255;
    memcpy(ctx->names[ctx->count], name, nl);
    ctx->names[ctx->count][nl] = '\0';
    memcpy(ctx->values[ctx->count], value, vl);
    ctx->values[ctx->count][vl] = '\0';
    ctx->count++;
  }
  return 0;
}

static int c_on_headers_complete(chttp1_parser_t *p) {
  chttp1_ctx_t *ctx = (chttp1_ctx_t *)p->data;
  ctx->status_code = p->status_code;
  return CHTTP1_HEADERS_HAS_BODY;
}

static int c_on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttp1_ctx_t *ctx = (chttp1_ctx_t *)p->data;
  if (ctx->body_len + len <= sizeof(ctx->body)) {
    memcpy(ctx->body + ctx->body_len, at, len);
    ctx->body_len += len;
  }
  return 0;
}

static int c_on_message_complete(chttp1_parser_t *p) {
  chttp1_ctx_t *ctx = (chttp1_ctx_t *)p->data;
  ctx->message_complete_called = true;
  return 0;
}

/* Static storage duration: chttp1_parser_init{,_request} only borrows the
 * settings pointer rather than copying it, matching tests_parser.c's own
 * identical rationale. */
static const chttp1_settings_t g_response_settings = {
    .on_header = c_on_header,
    .on_headers_complete = c_on_headers_complete,
    .on_body = c_on_body,
    .on_message_complete = c_on_message_complete,
};

static const chttp1_settings_t g_request_settings = {
    .on_request_line = c_on_request_line,
    .on_header = c_on_header,
    .on_headers_complete = c_on_headers_complete,
    .on_body = c_on_body,
    .on_message_complete = c_on_message_complete,
};

/* Both parsers must agree on the exact set of (name, value) pairs and their
 * order; a differing header count or any mismatched pair fails the check. */
static bool headers_match(const chttp1_ctx_t *c1,
                          const struct phr_header *phr_headers,
                          size_t phr_num_headers) {
  if (c1->count != phr_num_headers) return false;
  for (size_t i = 0; i < c1->count; i++) {
    if (strlen(c1->names[i]) != phr_headers[i].name_len ||
        strncmp(c1->names[i], phr_headers[i].name, phr_headers[i].name_len) !=
            0)
      return false;
    if (strlen(c1->values[i]) != phr_headers[i].value_len ||
        strncmp(c1->values[i], phr_headers[i].value,
                phr_headers[i].value_len) != 0)
      return false;
  }
  return true;
}

/* ========================================================================== */
/*                    REQUEST LINE + HEADERS                                  */
/* ========================================================================== */

static void check_request(const char *msg) {
  size_t len = strlen(msg);

  chttp1_parser_t parser;
  chttp1_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  chttp1_parser_init_request(&parser, &g_request_settings);
  parser.data = &ctx;
  chttp1_parser_execute(&parser, msg, len);

  const char *phr_method, *phr_path;
  size_t phr_method_len, phr_path_len;
  int phr_minor_version;
  struct phr_header phr_headers[MAX_HDRS];
  size_t phr_num_headers = MAX_HDRS;
  int phr_ret = phr_parse_request(msg, len, &phr_method, &phr_method_len,
                                  &phr_path, &phr_path_len, &phr_minor_version,
                                  phr_headers, &phr_num_headers, 0);
  REQUIRE_GT(phr_ret, 0);

  REQUIRE_EQ(strlen(ctx.method), phr_method_len);
  REQUIRE_EQ(memcmp(ctx.method, phr_method, phr_method_len), 0);
  REQUIRE_EQ(strlen(ctx.target), phr_path_len);
  REQUIRE_EQ(memcmp(ctx.target, phr_path, phr_path_len), 0);
  REQUIRE_TRUE(headers_match(&ctx, phr_headers, phr_num_headers));
}

TEST(request_differential, basic_get) {
  check_request(
      "GET /index.html HTTP/1.1\r\nHost: example.com\r\n"
      "User-Agent: test-agent\r\n\r\n");
}

TEST(request_differential, post_with_query_and_multiple_headers) {
  check_request(
      "POST /submit?a=1&b=2 HTTP/1.1\r\nHost: example.com\r\n"
      "Content-Type: application/json\r\nAccept: */*\r\nX-Custom: value\r\n"
      "\r\n");
}

TEST(request_differential, header_value_ows_trimmed) {
  check_request(
      "GET / HTTP/1.1\r\nHost: example.com\r\n"
      "X-Padded:   value with spaces  \r\n\r\n");
}

TEST(request_differential, empty_header_value) {
  check_request("GET / HTTP/1.1\r\nHost: example.com\r\nX-Empty:\r\n\r\n");
}

TEST(request_differential, repeated_header_name_kept_as_separate_entries) {
  check_request(
      "GET / HTTP/1.1\r\nHost: example.com\r\n"
      "X-Custom: a\r\nX-Custom: b\r\n\r\n");
}

/* ========================================================================== */
/*                    STATUS LINE + HEADERS                                   */
/* ========================================================================== */

static void check_response(const char *msg) {
  size_t len = strlen(msg);

  chttp1_parser_t parser;
  chttp1_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  chttp1_parser_init(&parser, &g_response_settings);
  parser.data = &ctx;
  chttp1_parser_execute(&parser, msg, len);

  int phr_minor_version, phr_status;
  const char *phr_msg;
  size_t phr_msg_len;
  struct phr_header phr_headers[MAX_HDRS];
  size_t phr_num_headers = MAX_HDRS;
  int phr_ret =
      phr_parse_response(msg, len, &phr_minor_version, &phr_status, &phr_msg,
                         &phr_msg_len, phr_headers, &phr_num_headers, 0);
  REQUIRE_GT(phr_ret, 0);

  REQUIRE_EQ(ctx.status_code, phr_status);
  REQUIRE_TRUE(headers_match(&ctx, phr_headers, phr_num_headers));
}

TEST(response_differential, basic_200) {
  check_response(
      "HTTP/1.1 200 OK\r\nContent-Type: text/plain\r\n"
      "Content-Length: 5\r\n\r\nhello");
}

TEST(response_differential, not_found_no_body) {
  check_response("HTTP/1.1 404 Not Found\r\nServer: test\r\n\r\n");
}

TEST(response_differential, http_1_0_redirect) {
  check_response(
      "HTTP/1.0 301 Moved Permanently\r\nLocation: http://example.com/\r\n"
      "\r\n");
}

/* ========================================================================== */
/*                    CHUNKED BODY DECODING                                   */
/* ========================================================================== */

/* Decodes chunked_body (headers already stripped) via picohttpparser, and
 * decodes the same bytes, framed behind a minimal chunked response, via
 * chttp1_parser; both decoded bodies must come out byte-identical. */
static void check_chunked(const char *chunked_body) {
  size_t chunked_len = strlen(chunked_body);

  /* chttp1_parser */
  char full_msg[BODY_CAP];
  int n = snprintf(full_msg, sizeof(full_msg),
                   "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n%s",
                   chunked_body);
  REQUIRE_TRUE(n > 0 && (size_t)n < sizeof(full_msg));

  chttp1_parser_t parser;
  chttp1_ctx_t ctx;
  memset(&ctx, 0, sizeof(ctx));
  chttp1_parser_init(&parser, &g_response_settings);
  parser.data = &ctx;
  chttp1_parser_execute(&parser, full_msg, strlen(full_msg));
  REQUIRE_TRUE(ctx.message_complete_called);

  /* picohttpparser */
  char phr_buf[BODY_CAP];
  memcpy(phr_buf, chunked_body, chunked_len + 1);
  size_t phr_bufsz = chunked_len;
  struct phr_chunked_decoder decoder;
  memset(&decoder, 0, sizeof(decoder));
  decoder.consume_trailer = 1;
  ssize_t phr_ret = phr_decode_chunked(&decoder, phr_buf, &phr_bufsz);
  REQUIRE_GE(phr_ret, 0);

  REQUIRE_EQ(ctx.body_len, phr_bufsz);
  REQUIRE_EQ(memcmp(ctx.body, phr_buf, phr_bufsz), 0);
}

TEST(chunked_differential, single_chunk) {
  check_chunked("5\r\nhello\r\n0\r\n\r\n");
}

TEST(chunked_differential, multiple_chunks) {
  check_chunked("3\r\nfoo\r\n4\r\nbar!\r\n0\r\n\r\n");
}

TEST(chunked_differential, chunk_extension_discarded) {
  check_chunked("5;ext=1\r\nhello\r\n0\r\n\r\n");
}

TEST(chunked_differential, terminal_chunk_with_trailer) {
  check_chunked("5\r\nhello\r\n0\r\nX-Trailer: trailer-value\r\n\r\n");
}

TEST(chunked_differential, empty_body) { check_chunked("0\r\n\r\n"); }
