/*
 * MIT License
 *
 * Copyright (c) 2026 - A bunch of nerds
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to
 * deal in the Software without restriction, including without limitation the
 * rights to use, copy, modify, merge, publish, distribute, sublicense, and/or
 * sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 */

/**
 * @file bench_serialization.c
 * @brief Benchmarks for cjson and cyaml.
 *
 * Both parsers are measured on one document each, built once at startup and
 * shared by every case so that the two formats are compared on the same
 * content. The document is a nested structure of objects, arrays, strings and
 * numbers rather than a flat list, because a parser's recursion and its
 * container growth are where its cost actually is.
 *
 * Timings are reported per document, not per byte, so the figure moves with
 * the document size constant chosen here; the same constant is used for every
 * case and for the third-party comparisons.
 *
 * Where jansson or libyaml is installed, each is measured parsing the exact
 * same bytes. Both build a DOM of their own, so the comparison is between
 * comparable amounts of work, unlike a streaming or callback-based parser.
 */

#include <cjson.h>
#include <cyaml.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#ifdef BENCH_HAVE_JANSSON
#include <jansson.h>
#endif
#ifdef BENCH_HAVE_YAML
#include <yaml.h>
#endif

/* Records in the generated document. Large enough that per-call overhead does
 * not dominate, small enough that a repetition stays in the millisecond range
 * and the suite finishes in reasonable time. */
#define BENCH_DOC_RECORDS 200

typedef struct {
  char *text;
  size_t len;
  cjson json_dom;
  cyaml yaml_dom;
#ifdef BENCH_HAVE_JANSSON
  json_t *jansson_dom;
#endif
} doc_state_t;

/* ------------------------------------------------------------------------ */
/* Document generation                                                       */
/* ------------------------------------------------------------------------ */

/* snprintf reports the length it would have written, not the length it wrote,
 * so adding its return value straight onto an offset takes that offset past the
 * end of the buffer the instant anything is truncated, and the remaining-space
 * expression then wraps to an enormous size_t that tells the next call it may
 * write far beyond it. Every append below goes through this instead, which
 * refuses a truncation rather than absorbing it: these documents are built here
 * from a record count this file chooses, so a buffer that does not fit is a
 * sizing mistake in this file and not something a measurement should continue
 * past. */
__attribute__((format(printf, 4, 5))) static size_t doc_append(
    char *buf, size_t cap, size_t off, const char *fmt, ...) {
  if (off >= cap) bench_die("document buffer overflow");
  va_list ap;
  va_start(ap, fmt);
  int written = vsnprintf(buf + off, cap - off, fmt, ap);
  va_end(ap);
  if (written < 0 || (size_t)written >= cap - off)
    bench_die("document buffer too small");
  return off + (size_t)written;
}

static char *build_json_doc(size_t records, size_t *out_len) {
  size_t cap = records * 256 + 256;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  size_t off = 0;
  off = doc_append(buf, cap, off,
                   "{\"service\":\"bench\",\"version\":3,"
                   "\"enabled\":true,\"records\":[");
  for (size_t i = 0; i < records; i++) {
    off = doc_append(
        buf, cap, off,
        "%s{\"id\":%zu,\"name\":\"record_%zu\",\"score\":%zu.%02zu,"
        "\"tags\":[\"alpha\",\"beta\",\"gamma\"],"
        "\"meta\":{\"active\":%s,\"weight\":%zu,\"label\":\"node-%zu\"}}",
        i ? "," : "", i, i, i % 100, i % 100, (i % 2) ? "true" : "false", i * 7,
        i % 17);
  }
  off = doc_append(buf, cap, off, "]}");
  if (out_len) *out_len = off;
  return buf;
}

static char *build_yaml_doc(size_t records, size_t *out_len) {
  size_t cap = records * 320 + 256;
  char *buf = malloc(cap);
  if (!buf) return NULL;
  size_t off = 0;
  off = doc_append(buf, cap, off,
                   "service: bench\nversion: 3\nenabled: true\n"
                   "records:\n");
  for (size_t i = 0; i < records; i++) {
    off = doc_append(buf, cap, off,
                     "  - id: %zu\n"
                     "    name: record_%zu\n"
                     "    score: %zu.%02zu\n"
                     "    tags:\n"
                     "      - alpha\n"
                     "      - beta\n"
                     "      - gamma\n"
                     "    meta:\n"
                     "      active: %s\n"
                     "      weight: %zu\n"
                     "      label: node-%zu\n",
                     i, i, i % 100, i % 100, (i % 2) ? "true" : "false", i * 7,
                     i % 17);
  }
  if (out_len) *out_len = off;
  return buf;
}

static void doc_teardown(void *state) {
  doc_state_t *st = state;
  if (st->json_dom) {
    cjson d = st->json_dom;
    cjson_destroy(d);
  }
  if (st->yaml_dom) {
    cyaml d = st->yaml_dom;
    cyaml_destroy(d);
  }
#ifdef BENCH_HAVE_JANSSON
  if (st->jansson_dom) json_decref(st->jansson_dom);
#endif
  free(st->text);
  free(st);
}

static void *json_text_setup(size_t n) {
  (void)n;
  doc_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->text = build_json_doc(BENCH_DOC_RECORDS, &st->len);
  if (!st->text) {
    free(st);
    return NULL;
  }
  return st;
}

static void *yaml_text_setup(size_t n) {
  (void)n;
  doc_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  st->text = build_yaml_doc(BENCH_DOC_RECORDS, &st->len);
  if (!st->text) {
    free(st);
    return NULL;
  }
  return st;
}

/* ------------------------------------------------------------------------ */
/* cjson                                                                     */
/* ------------------------------------------------------------------------ */

static void cjson_parse_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    char *err = NULL;
    cjson d = cjson_parse(st->text, &err);
    if (!d) {
      cjson_serialize_free(err);
      /* Fatal rather than a quiet stop: the harness divides the elapsed time
       * by the full count, so returning early reports partial work at full
       * price, and a parse that fails is far cheaper than one that works. */
      bench_die("cjson parse failed");
    }
    bench_sink(d);
    cjson_destroy(d);
  }
}

static void *cjson_dom_setup(size_t n) {
  doc_state_t *st = json_text_setup(n);
  if (!st) return NULL;
  char *err = NULL;
  st->json_dom = cjson_parse(st->text, &err);
  cjson_serialize_free(err);
  if (!st->json_dom) {
    doc_teardown(st);
    return NULL;
  }
  return st;
}

static void cjson_serialize_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    char *s = cjson_serialize(st->json_dom);
    if (!s) bench_die("cjson serialize failed"); /* see cjson_parse_run */
    bench_sink(s);
    cjson_serialize_free(s);
  }
}

/* ------------------------------------------------------------------------ */
/* cyaml                                                                     */
/* ------------------------------------------------------------------------ */

static void cyaml_parse_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    char *err = NULL;
    cyaml d = cyaml_parse(st->text, &err);
    if (!d) {
      cyaml_serialize_free(err);
      bench_die("cyaml parse failed"); /* see cjson_parse_run */
    }
    bench_sink(d);
    cyaml_destroy(d);
  }
}

static void *cyaml_dom_setup(size_t n) {
  doc_state_t *st = yaml_text_setup(n);
  if (!st) return NULL;
  char *err = NULL;
  st->yaml_dom = cyaml_parse(st->text, &err);
  cyaml_serialize_free(err);
  if (!st->yaml_dom) {
    doc_teardown(st);
    return NULL;
  }
  return st;
}

static void cyaml_serialize_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    char *s = cyaml_serialize(st->yaml_dom);
    if (!s) bench_die("cyaml serialize failed"); /* see cjson_parse_run */
    bench_sink(s);
    cyaml_serialize_free(s);
  }
}

/* ------------------------------------------------------------------------ */
/* Third-party comparisons                                                   */
/* ------------------------------------------------------------------------ */

#ifdef BENCH_HAVE_JANSSON
/* The serialize arm's fixture: the same bytes every other case in this group
 * uses, parsed into jansson's own DOM so that what is timed below is the
 * serializer alone, exactly as cjson_dom_setup does for the case this is
 * compared against. */
static void *jansson_dom_setup(size_t n) {
  doc_state_t *st = json_text_setup(n);
  if (!st) return NULL;
  json_error_t err;
  st->jansson_dom = json_loads(st->text, 0, &err);
  if (!st->jansson_dom) {
    doc_teardown(st);
    return NULL;
  }
  return st;
}

/* Both flags exist to make the two sides emit the same bytes, since a
 * serializer's cost tracks how much it writes.
 *
 * JSON_COMPACT matches cjson_serialize's own output shape: jansson's default
 * puts a space after every comma and colon, and cjson emits no gratuitous
 * whitespace. JSON_REAL_PRECISION(15) matches how cjson renders a double,
 * which tries 15 significant digits and only widens to 17 when that fails to
 * round-trip; jansson's default is a flat 17, so 33.33 comes back out as
 * 33.329999999999998. Without it jansson is timed producing about 8 percent
 * more bytes than cjson on this document, and the ratio reports that
 * difference as speed. */
static void jansson_serialize_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    char *s =
        json_dumps(st->jansson_dom, JSON_COMPACT | JSON_REAL_PRECISION(15));
    /* Fatal for the same reason as jansson_parse_run. */
    if (!s) bench_die("jansson serialize failed");
    bench_sink(s);
    free(s);
  }
}

static void jansson_parse_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    json_error_t err;
    json_t *root = json_loads(st->text, 0, &err);
    /* Fatal, exactly as the c_collections arms are: the harness divides the
     * elapsed time by the full count either way, and a parse that fails is far
     * cheaper than one that works, so a quiet stop reports a flattering figure
     * for the library this one is compared against. */
    if (!root) bench_die("jansson parse failed");
    bench_sink(root);
    json_decref(root);
  }
}
#endif

#ifdef BENCH_HAVE_YAML
/* libyaml has no DOM of its own in the sense cyaml does; its document API is
 * the closest equivalent, so that is what is measured rather than the raw
 * event stream, which would be comparing a tokenizer against a tree builder. */
static void libyaml_parse_run(void *state, size_t n) {
  doc_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    yaml_parser_t parser;
    yaml_document_t doc;
    /* Fatal for the same reason as jansson_parse_run. */
    if (!yaml_parser_initialize(&parser)) bench_die("libyaml init failed");
    yaml_parser_set_input_string(&parser, (const unsigned char *)st->text,
                                 st->len);
    if (!yaml_parser_load(&parser, &doc)) {
      yaml_parser_delete(&parser);
      bench_die("libyaml parse failed");
    }
    bench_sink(&doc);
    yaml_document_delete(&doc);
    yaml_parser_delete(&parser);
  }
}
#endif

/* ------------------------------------------------------------------------ */

#define BENCH_DOC_N 2000

/* The multi-threaded variants parse a tenth as many documents per repetition.
 * Each thread works on its own copy of the tree, so the per-repetition cost
 * scales with the thread count, and the smaller count keeps a repetition short
 * enough that the sampler collects a useful number of them. */
#define BENCH_DOC_MT_N 200

/* cjson and cyaml hold no internal locks by design. Concurrent use means one
 * document tree per thread, which is the realistic shape (a request handler
 * parsing its own payload) and the only valid one. */
BENCH_MT_SETUP(json_text_setup)
BENCH_MT_SETUP(cjson_dom_setup)
BENCH_MT_SETUP(yaml_text_setup)
BENCH_MT_SETUP(cyaml_dom_setup)

void bench_register_serialization(void) {
  bench_add(&(bench_case_t){.group = "cjson",
                            .name = "parse_document",
                            .setup = json_text_setup,
                            .run = cjson_parse_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
  bench_add_mt(&(bench_case_t){.group = "cjson",
                               .name = "parse_document",
                               .setup_mt = json_text_setup_mt,
                               .run = cjson_parse_run,
                               .teardown = doc_teardown,
                               .n = BENCH_DOC_MT_N});
  bench_add(&(bench_case_t){.group = "cjson",
                            .name = "serialize_document",
                            .setup = cjson_dom_setup,
                            .run = cjson_serialize_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
  bench_add_mt(&(bench_case_t){.group = "cjson",
                               .name = "serialize_document",
                               .setup_mt = cjson_dom_setup_mt,
                               .run = cjson_serialize_run,
                               .teardown = doc_teardown,
                               .n = BENCH_DOC_MT_N});
#ifdef BENCH_HAVE_JANSSON
  bench_add(&(bench_case_t){.group = "cjson",
                            .name = "parse_document",
                            .vs = "jansson",
                            .setup = json_text_setup,
                            .run = jansson_parse_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
  bench_add(&(bench_case_t){.group = "cjson",
                            .name = "serialize_document",
                            .vs = "jansson",
                            .setup = jansson_dom_setup,
                            .run = jansson_serialize_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
#endif

  bench_add(&(bench_case_t){.group = "cyaml",
                            .name = "parse_document",
                            .setup = yaml_text_setup,
                            .run = cyaml_parse_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
  bench_add_mt(&(bench_case_t){.group = "cyaml",
                               .name = "parse_document",
                               .setup_mt = yaml_text_setup_mt,
                               .run = cyaml_parse_run,
                               .teardown = doc_teardown,
                               .n = BENCH_DOC_MT_N});
  bench_add(&(bench_case_t){.group = "cyaml",
                            .name = "serialize_document",
                            .setup = cyaml_dom_setup,
                            .run = cyaml_serialize_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
  bench_add_mt(&(bench_case_t){.group = "cyaml",
                               .name = "serialize_document",
                               .setup_mt = cyaml_dom_setup_mt,
                               .run = cyaml_serialize_run,
                               .teardown = doc_teardown,
                               .n = BENCH_DOC_MT_N});
#ifdef BENCH_HAVE_YAML
  bench_add(&(bench_case_t){.group = "cyaml",
                            .name = "parse_document",
                            .vs = "libyaml",
                            .setup = yaml_text_setup,
                            .run = libyaml_parse_run,
                            .teardown = doc_teardown,
                            .n = BENCH_DOC_N});
#endif
}
