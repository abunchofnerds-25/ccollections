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

// The libFuzzer target for chttp1_parser.c in request mode, the grammar that
// chttpserver.c drives. The fragmenting driver is in fuzz_common.h, and
// fuzz_chttp1_response.c is the sibling target for response mode.

#include <string.h>

#include "fuzz_common.h"

// Parser callbacks that accept everything, because the fuzzer only looks for
// crashes and leaks. They read and change no state.
static int on_request_line(chttp1_parser *p, const char *method,
                           size_t method_len, const char *target,
                           size_t target_len) {
  (void)p;
  (void)method;
  (void)method_len;
  (void)target;
  (void)target_len;
  return 0;
}

static int on_header(chttp1_parser *p, const char *name, size_t name_len,
                     const char *value, size_t value_len) {
  (void)p;
  (void)name;
  (void)name_len;
  (void)value;
  (void)value_len;
  return 0;
}

// Flips on every headers-complete, so one fuzzing run sees both outcomes of
// request mode: CHTTP1_HEADERS_DIVERT_BODY (the divert and resume path) and the
// ordinary CHTTP1_HEADERS_HAS_BODY.
static bool g_divert_next = false;

// Gives CHTTP1_HEADERS_DIVERT_BODY and CHTTP1_HEADERS_HAS_BODY in turn,
// flipping g_divert_next on every call, so one run reaches both outcomes of
// request mode.
static int on_headers_complete(chttp1_parser *p) {
  (void)p;
  g_divert_next = !g_divert_next;
  return g_divert_next ? CHTTP1_HEADERS_DIVERT_BODY : CHTTP1_HEADERS_HAS_BODY;
}

static int on_body(chttp1_parser *p, const char *at, size_t len) {
  (void)p;
  (void)at;
  (void)len;
  return 0;
}

static int on_message_complete(chttp1_parser *p) {
  (void)p;
  return 0;
}

// The request-mode callbacks.
static const chttp1_settings g_settings = {
    .on_request_line = on_request_line,
    .on_header = on_header,
    .on_headers_complete = on_headers_complete,
    .on_body = on_body,
    .on_message_complete = on_message_complete,
};

// The libFuzzer entry point: parses data[0..size) as a request, fed in random
// fragments, to find crashes and leaks. Always gives 0.
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
  chttp1_parser parser;
  chttp1_parser_init_request(&parser, &g_settings);
  parser.data = NULL;

  fuzz_feed_fragmented(&parser, data, size);
  return 0;
}
