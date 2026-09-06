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
 * libFuzzer target for chttp1_parser.c in request mode (the grammar
 * chttpserver.c drives: a request line, not a status line). See
 * fuzz_common.h for the shared fragmented-feed driver and fuzz_chttp1_
 * response.c for the response-mode sibling target.
 */

#include <string.h>

#include "fuzz_common.h"

static int on_request_line(chttp1_parser_t *p, const char *method,
                           size_t method_len, const char *target,
                           size_t target_len) {
  (void)p;
  (void)method;
  (void)method_len;
  (void)target;
  (void)target_len;
  return 0;
}

static int on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                     const char *value, size_t value_len) {
  (void)p;
  (void)name;
  (void)name_len;
  (void)value;
  (void)value_len;
  return 0;
}

/* Alternates between the two request-specific outcomes on every call
 * (CHTTP1_HEADERS_DIVERT_BODY, exercising the divert/resume path
 * fuzz_common.h's own driver threads correctly, and ordinary
 * CHTTP1_HEADERS_HAS_BODY), so a single fuzzing run covers both instead of
 * only ever exercising one of the two request-mode-specific behaviors. */
static bool g_divert_next = false;

static int on_headers_complete(chttp1_parser_t *p) {
  (void)p;
  g_divert_next = !g_divert_next;
  return g_divert_next ? CHTTP1_HEADERS_DIVERT_BODY : CHTTP1_HEADERS_HAS_BODY;
}

static int on_body(chttp1_parser_t *p, const char *at, size_t len) {
  (void)p;
  (void)at;
  (void)len;
  return 0;
}

static int on_message_complete(chttp1_parser_t *p) {
  (void)p;
  return 0;
}

static const chttp1_settings_t g_settings = {
    .on_request_line = on_request_line,
    .on_header = on_header,
    .on_headers_complete = on_headers_complete,
    .on_body = on_body,
    .on_message_complete = on_message_complete,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  chttp1_parser_t parser;
  chttp1_parser_init_request(&parser, &g_settings);
  parser.data = NULL;

  fuzz_feed_fragmented(&parser, data, size);
  return 0;
}
