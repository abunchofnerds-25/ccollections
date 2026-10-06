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
 * @file bench_http.c
 * @brief End-to-end benchmarks for chttpserver and chttpclient.
 *
 * These cases start a server on the loopback interface and drive it with the
 * client of this library. One case therefore measures the whole path. That
 * path holds the serialization of the request, the loopback round trip through
 * the kernel, the readiness dispatch of the reactor, the routing, the handler
 * on a worker thread, the serialization of the response, and the parse by the
 * client. That is the number that a user of this library sees, and it is the
 * one worth watching for a regression. On its own it cannot say which of the
 * two sides a change belongs to.
 *
 * Loopback is not the internet. Do not read these figures as throughput over a
 * real network. What they measure well is the cost of this library for each
 * request. A change to the routing, to the connection lifecycle, or to the
 * parser moves exactly that number.
 *
 * These cases take a port by trying a range of numbers, and not one fixed
 * number. A benchmark run therefore does not fail only because something else
 * on the machine already listens on that port.
 */

#include <chttpclient.h>
#include <chttpserver.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bench.h"

#define BENCH_HTTP_N 20000
#define BENCH_HTTP_PORT_BASE 42800
#define BENCH_HTTP_PORT_TRIES 64
#define BENCH_HTTP_CONCURRENCY 4

/* This is the number of requests for each client thread in the forms that run
 * several clients together. The form with 4 threads therefore sends the same
 * total as the case that runs one request at a time. The wider forms raise the
 * offered load with the number of clients. They do not divide one fixed total
 * into smaller parts. This is what makes the figures for each request
 * comparable across all of them. */
#define BENCH_HTTP_MT_N 5000

/* Request and response bodies for the cases that move a body. The small one
 * fits in the first read together with the headers, which is the shape of an
 * ordinary API call. The large one is larger than the socket buffers of the
 * loopback path, so the server reads it and writes it in several passes. */
#define BENCH_HTTP_SMALL_BODY 1024
#define BENCH_HTTP_LARGE_BODY (1024 * 1024)
#define BENCH_HTTP_SMALL_N 20000
#define BENCH_HTTP_LARGE_N 200

typedef struct {
  chttpsvr srv;
  chttpcli cli;
  char url[64];
  atomic_size_t ok;
  char url_echo_len[64];
  char url_large[64];
  unsigned char *small_body;
  unsigned char *large_body;
} http_state_t;

/* The response states the length that the server received, so the client can
 * check that the whole body arrived. A body that arrived in part would
 * otherwise measure less work at the full price. */
static void bench_echo_len_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                   void *ctx) {
  (void)ctx;
  size_t len = 0;
  (void)chttpsvr_req_body(req, &len);
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_OK);
  chttpsvr_resp_printf(resp, "%zu", len);
}

static void bench_large_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  const unsigned char *body = ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_OK);
  chttpsvr_resp_write(resp, body, BENCH_HTTP_LARGE_BODY);
}

static void bench_hello_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_set_status(resp, CHTTP_STATUS_OK);
  chttpsvr_resp_write_str(resp, "{\"status\":\"ok\",\"value\":42}");
}

static void http_teardown(void *state) {
  http_state_t *st = state;
  if (st->cli != CHTTPCLI_INVALID) {
    chttpcli c = st->cli;
    chttpclient_destroy(c);
  }
  if (st->srv != CHTTPSVR_INVALID) {
    chttpsvr s = st->srv;
    chttpsvr_stop(s);
    chttpsvr_destroy(s);
  }
  free(st->small_body);
  free(st->large_body);
  free(st);
}

static void *http_setup(size_t n) {
  (void)n;
  http_state_t *st = calloc(1, sizeof(*st));
  if (!st) return NULL;
  atomic_init(&st->ok, 0);
  st->srv = CHTTPSVR_INVALID;
  st->cli = CHTTPCLI_INVALID;

  /* CLOG_INVALID means no logger. A logger here would put the cost of
   * formatting and writing one line for each request inside a measurement
   * that is about the request path. */
  /* Nothing frees this, and nothing must. These two constructors report
   * through CCOL_ERR_STR, which is a static string that the macro builds from
   * __FILE__ and __LINE__. A free() on it is undefined behavior, and not only
   * a waste. The parse error strings of the serializers are library-owned in
   * the same way: each thread has its own, and the next parse reuses it. */
  char *err = NULL;
  st->srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (st->srv == CHTTPSVR_INVALID) {
    if (err) fprintf(stderr, "bench: chttpsvr creation failed: %s\n", err);
    http_teardown(st);
    return NULL;
  }
  st->small_body = malloc(BENCH_HTTP_SMALL_BODY);
  st->large_body = malloc(BENCH_HTTP_LARGE_BODY);
  if (!st->small_body || !st->large_body) {
    http_teardown(st);
    return NULL;
  }
  memset(st->small_body, 'a', BENCH_HTTP_SMALL_BODY);
  memset(st->large_body, 'b', BENCH_HTTP_LARGE_BODY);
  if (chttpsvr_register_handler(st->srv, CHTTP_GET, "/bench",
                                bench_hello_handler, NULL) != ccol_success ||
      chttpsvr_register_handler(st->srv, CHTTP_POST, "/echo_len",
                                bench_echo_len_handler, NULL) != ccol_success ||
      chttpsvr_register_handler(st->srv, CHTTP_GET, "/large",
                                bench_large_handler,
                                st->large_body) != ccol_success) {
    http_teardown(st);
    return NULL;
  }

  bool started = false;
  for (int i = 0; i < BENCH_HTTP_PORT_TRIES; i++) {
    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = "127.0.0.1";
    cfg.port = (uint16_t)(BENCH_HTTP_PORT_BASE + i);
    cfg.enable_keepalive = true;
    if (chttpsvr_start(st->srv, &cfg) == ccol_success) {
      snprintf(st->url, sizeof(st->url), "http://127.0.0.1:%u/bench",
               (unsigned)cfg.port);
      snprintf(st->url_echo_len, sizeof(st->url_echo_len),
               "http://127.0.0.1:%u/echo_len", (unsigned)cfg.port);
      snprintf(st->url_large, sizeof(st->url_large),
               "http://127.0.0.1:%u/large", (unsigned)cfg.port);
      started = true;
      break;
    }
  }
  if (!started) {
    http_teardown(st);
    return NULL;
  }

  st->cli = ccol_create_chttpclient(&err);
  if (st->cli == CHTTPCLI_INVALID) {
    if (err) fprintf(stderr, "bench: chttpclient creation failed: %s\n", err);
    http_teardown(st);
    return NULL;
  }
  chttpclient_set_pool_size(st->cli, BENCH_HTTP_CONCURRENCY);
  return st;
}

/* Every failure stops the run. The code does not count a failure and continue.
 * A request that fails costs much less than a request that succeeds. A server
 * that stopped answering would therefore turn this into a loop that measures
 * nothing and reports a remarkable figure for it. The harness also divides the
 * measured time by the full count in both cases, so an early stop would report
 * part of the work at the full price. */
static void http_do_requests(http_state_t *st, size_t count) {
  for (size_t i = 0; i < count; i++) {
    chttp_request_t *req = chttp_request_new(CHTTP_GET, st->url, NULL, NULL);
    if (!req) bench_die("http request allocation failed");
    chttpcli_response *resp = NULL;
    if (chttpclient_do(st->cli, req, &resp) != ccol_success) {
      chttpclient_resp_free(resp);
      chttp_request_free(req);
      bench_die("http request failed");
    }
    atomic_fetch_add_explicit(&st->ok, 1, memory_order_relaxed);
    chttpclient_resp_free(resp);
    chttp_request_free(req);
  }
}

/* This sends one request at a time on a connection that stays open. It
 * therefore separates the cost of one request from every effect of
 * concurrency. */
static void http_sequential_run(void *state, size_t n) {
  http_state_t *st = state;
  http_do_requests(st, n);
  bench_sink(&st->ok);
}

/* Sends a POST with a body of body_len bytes and checks that the server saw
 * exactly that many. */
static void http_do_posts(http_state_t *st, const unsigned char *body,
                          size_t body_len, size_t count) {
  char expect[32];
  snprintf(expect, sizeof(expect), "%zu", body_len);
  for (size_t i = 0; i < count; i++) {
    chttp_request_body_t b =
        CHTTP_BODY(body, body_len, "application/octet-stream");
    chttp_request_t *req =
        chttp_request_new(CHTTP_POST, st->url_echo_len, &b, NULL);
    if (!req) bench_die("http request allocation failed");
    chttpcli_response *resp = NULL;
    if (chttpclient_do(st->cli, req, &resp) != ccol_success || !resp ||
        resp->status_code != 200 || resp->body_len != strlen(expect) ||
        memcmp(resp->body, expect, resp->body_len) != 0) {
      chttpclient_resp_free(resp);
      chttp_request_free(req);
      bench_die("http post failed or the server saw a partial body");
    }
    atomic_fetch_add_explicit(&st->ok, 1, memory_order_relaxed);
    chttpclient_resp_free(resp);
    chttp_request_free(req);
  }
}

/* A small body arrives with the headers: the ordinary API call. */
static void http_post_small_run(void *state, size_t n) {
  http_state_t *st = state;
  http_do_posts(st, st->small_body, BENCH_HTTP_SMALL_BODY, n);
  bench_sink(&st->ok);
}

/* A body larger than the socket buffers: the server reads it in several
 * passes. */
static void http_post_large_run(void *state, size_t n) {
  http_state_t *st = state;
  http_do_posts(st, st->large_body, BENCH_HTTP_LARGE_BODY, n);
  bench_sink(&st->ok);
}

/* A response larger than the socket buffers: the server writes it in several
 * passes. */
static void http_get_large_run(void *state, size_t n) {
  http_state_t *st = state;
  for (size_t i = 0; i < n; i++) {
    chttp_request_t *req =
        chttp_request_new(CHTTP_GET, st->url_large, NULL, NULL);
    if (!req) bench_die("http request allocation failed");
    chttpcli_response *resp = NULL;
    if (chttpclient_do(st->cli, req, &resp) != ccol_success || !resp ||
        resp->status_code != 200 || resp->body_len != BENCH_HTTP_LARGE_BODY) {
      chttpclient_resp_free(resp);
      chttp_request_free(req);
      bench_die("http large response failed or arrived in part");
    }
    atomic_fetch_add_explicit(&st->ok, 1, memory_order_relaxed);
    chttpclient_resp_free(resp);
    chttp_request_free(req);
  }
  bench_sink(&st->ok);
}

/* One server and one client handle shared by every thread, which is the shape
 * a real service has and where the reactor and its worker pool actually have to
 * overlap work. The connection pool is sized to the thread count so that the
 * clients are not queueing behind each other on the way out. */
static void *http_setup_mt(size_t n, unsigned threads) {
  http_state_t *st = http_setup(n);
  if (st) chttpclient_set_pool_size(st->cli, threads);
  return st;
}

void bench_register_http(void) {
  bench_add(&(bench_case_t){.group = "http",
                            .name = "get_sequential_keepalive",
                            .setup = http_setup,
                            .run = http_sequential_run,
                            .teardown = http_teardown,
                            .n = BENCH_HTTP_N});
  bench_add_mt(&(bench_case_t){.group = "http",
                               .name = "get_concurrent_clients",
                               .setup_mt = http_setup_mt,
                               .run = http_sequential_run,
                               .teardown = http_teardown,
                               .n = BENCH_HTTP_MT_N,
                               .shared_fixture = true});
  bench_add(&(bench_case_t){.group = "http",
                            .name = "post_1kb_sequential_keepalive",
                            .setup = http_setup,
                            .run = http_post_small_run,
                            .teardown = http_teardown,
                            .n = BENCH_HTTP_SMALL_N});
  bench_add(&(bench_case_t){.group = "http",
                            .name = "post_1mb_sequential_keepalive",
                            .setup = http_setup,
                            .run = http_post_large_run,
                            .teardown = http_teardown,
                            .n = BENCH_HTTP_LARGE_N});
  bench_add(&(bench_case_t){.group = "http",
                            .name = "get_1mb_sequential_keepalive",
                            .setup = http_setup,
                            .run = http_get_large_run,
                            .teardown = http_teardown,
                            .n = BENCH_HTTP_LARGE_N});
}
