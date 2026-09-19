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
 * A server is started on the loopback interface and driven by this library's
 * own client, so a single case measures the whole path: request serialization,
 * the kernel's loopback round trip, the reactor's readiness dispatch, routing,
 * the handler running on a worker thread, response serialization, and the
 * client's own parse. That is the number a user of this library actually
 * experiences, and it is the one worth tracking for regressions, even though
 * it cannot attribute a change to either side on its own.
 *
 * Loopback is not the internet, and these figures should not be read as
 * throughput over a real network. What they measure well is the library's own
 * per-request cost, which is exactly what a change to routing, to the
 * connection lifecycle, or to the parser moves.
 *
 * The port is claimed by trying a range rather than a fixed number, so a
 * benchmark run does not fail merely because something else on the machine is
 * already listening.
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

/* Requests per client thread in the concurrent variants. The 4-thread variant
 * therefore issues the same total as the sequential case, and the wider ones
 * scale the offered load with the client count rather than dividing a fixed
 * total more thinly, which is what makes the per-request figures comparable
 * across them. */
#define BENCH_HTTP_MT_N 5000

typedef struct {
  chttpsvr srv;
  chttpcli cli;
  char url[64];
  atomic_size_t ok;
} http_state_t;

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
  free(st);
}

static void *http_setup(size_t n) {
  (void)n;
  http_state_t *st = calloc(1, sizeof *st);
  if (!st) return NULL;
  atomic_init(&st->ok, 0);
  st->srv = CHTTPSVR_INVALID;
  st->cli = CHTTPCLI_INVALID;

  /* CLOG_INVALID: no logger. A logger here would put the cost of formatting
   * and writing a line per request inside a measurement that is about the
   * request path. */
  /* Not freed, and must not be: these two constructors report through
   * CCOL_ERR_STR, which is a static string built from __FILE__ and __LINE__,
   * so free() on it is undefined rather than merely wasteful. Only the
   * serializers' own error strings are heap-allocated, and those have their
   * own documented deallocator. */
  char *err = NULL;
  st->srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (st->srv == CHTTPSVR_INVALID) {
    if (err) fprintf(stderr, "bench: chttpsvr creation failed: %s\n", err);
    http_teardown(st);
    return NULL;
  }
  if (chttpsvr_register_handler(st->srv, CHTTP_GET, "/bench",
                                bench_hello_handler, NULL) != ccol_success) {
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

/* Every failure is fatal to the run rather than counted and carried past. A
 * request that fails is far cheaper than one that succeeds, so a server that
 * stopped answering turns this into a loop that measures nothing and reports a
 * spectacular figure for it; and the harness divides the elapsed time by the
 * full count either way, so stopping short would report partial work at full
 * price. */
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

/* One request at a time on a kept-alive connection, which isolates the
 * per-request cost from any concurrency effect. */
static void http_sequential_run(void *state, size_t n) {
  http_state_t *st = state;
  http_do_requests(st, n);
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
}
