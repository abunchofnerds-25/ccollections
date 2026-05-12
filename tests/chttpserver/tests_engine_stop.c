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

#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*     chttpsvr_engine_stop() FORCE-STOP COVERAGE (dedicated binary)          */
/*                                                                            */
/* chttpsvr_engine_stop() is documented (chttpserver.h) as the mechanism a    */
/* signal handler uses to trigger a process-wide engine shutdown, in the     */
/* exact pattern:                                                            */
/*   chttpsvr_start(srv, &cfg);                                              */
/*   chttpsvr_engine_wait();  // returns once a signal calls engine_stop()   */
/*   chttpsvr_destroy(srv);                                                  */
/* This is process-wide, forced, state (it tears down the one shared         */
/* event_loop reactor every chttpsvr instance in the process depends on),    */
/* so it cannot share tests.c's own long-lived g_srv/g_srv2/... (that suite's */
/* servers stay started across its entire run; force-stopping the shared     */
/* engine mid-suite would yank the reactor out from under every one of them  */
/* and fail unrelated tests). Given its own binary, matching this directory's */
/* existing tests_tls.c/tests_mem_mgmt.c precedent for scenarios that need a  */
/* process largely to themselves.                                            */
/* ========================================================================== */

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, engine-stop!");
}

static ccol_retval_t _get(const char *url, int *status_out) {
  chttpcli cli = create_chttpclient(NULL);
  if (!cli) return ccol_not_enough_memory;
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpclient_destroy(cli);
    return ccol_not_enough_memory;
  }
  chttpcli_response *resp = NULL;
  ccol_retval_t rv = chttpclient_do(cli, req, &resp);
  if (rv == ccol_success && resp) *status_out = resp->status_code;
  if (resp) chttpclient_resp_free(resp);
  chttp_request_free(req);
  chttpclient_destroy(cli);
  return rv;
}

static chttpsvr _start_server(const char *port_str, uint16_t port) {
  (void)port_str;
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(NULL, &err);
  if (!srv) {
    fprintf(stderr, "FATAL: create_chttpsvr failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  ccol_retval_t rv = chttpsvr_start(srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }
  return srv;
}

/* ========================================================================== */
/*                                 TESTS                                      */
/* ========================================================================== */

/* Direct regression test for the exact crash reported against a real
 * application: chttpsvr_engine_stop() (the forced/signal-handler path) was
 * called while srv was still fully started (never itself chttpsvr_stop()'d
 * or chttpsvr_destroy()'d first). Before the fix, the reaper this spawns
 * freed g_servers' backing array without resetting g_servers_count, so a
 * later chttpsvr_destroy(srv) -> _servers_unregister(srv) dereferenced a
 * NULL g_servers[0] -- a guaranteed, deterministic SIGSEGV, not a rare race.
 * A clean run of this test (especially under valgrind's memtest target) is
 * the direct proof the crash is gone and nothing was leaked or
 * double-freed along the way. */
TEST(engine_stop, force_stop_while_started_then_destroy_is_safe) {
  chttpsvr srv = _start_server("18796", 18796);

  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();

  /* The crash this test guards against happens inside chttpsvr_destroy()
   * itself; reaching the line after it is the actual assertion. */
  chttpsvr_destroy(srv);

  /* Prove the engine came back up cleanly (no corrupted global state left
   * behind by the force-stop) by starting a fresh server on the same port
   * and driving one real request through it. */
  chttpsvr srv2 = _start_server("18796", 18796);
  status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

/* The reaper's own quiesce pass (_engine_force_stop_quiesce_all) walks every
 * server still registered in g_servers, not just one; this exercises that
 * loop with two independently-started servers still live (each with a
 * completed, keep-alive-eligible connection sitting in its own idle list) at
 * the moment the forced stop fires. */
TEST(engine_stop, force_stop_quiesces_multiple_servers) {
  chttpsvr srv_a = _start_server("18797", 18797);
  chttpsvr srv_b = _start_server("18798", 18798);

  int status_a = 0, status_b = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18797/hello", &status_a),
             (int)ccol_success);
  REQUIRE_EQ((int)_get("http://127.0.0.1:18798/hello", &status_b),
             (int)ccol_success);
  REQUIRE_EQ(status_a, 200);
  REQUIRE_EQ(status_b, 200);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv_a);
  chttpsvr_destroy(srv_b);
  chttpsvr_engine_wait();
}

typedef struct {
  const char *url;
  int status;
  ccol_retval_t rv;
} _async_get_ctx;

static void *_async_get_thread(void *arg) {
  _async_get_ctx *ctx = (_async_get_ctx *)arg;
  ctx->rv = _get(ctx->url, &ctx->status);
  return NULL;
}

static pthread_mutex_t g_slow_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_slow_cv = PTHREAD_COND_INITIALIZER;
static bool g_slow_go = false;
static bool g_slow_entered = false;

static void _slow_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_entered = true;
  pthread_cond_broadcast(&g_slow_cv);
  while (!g_slow_go) pthread_cond_wait(&g_slow_cv, &g_slow_mtx);
  pthread_mutex_unlock(&g_slow_mtx);
  chttpsvr_resp_write_str(resp, "Hello, slow!");
}

/* chttpsvr_engine_stop() must still drain genuinely in-flight requests
 * (_drain_and_close_all_connections's own documented contract) before the
 * reactor is actually torn down, exactly like a graceful chttpsvr_destroy()
 * already does -- the forced path is not supposed to abandon or corrupt a
 * request that is already being handled by a worker thread at the moment the
 * signal fires. */
TEST(engine_stop, force_stop_drains_in_flight_request_before_reactor_teardown) {
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(NULL, &err);
  REQUIRE_TRUE(srv != NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow", _slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18799;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  _async_get_ctx ctx = {
      .url = "http://127.0.0.1:18799/slow", .status = 0, .rv = ccol_success};
  pthread_t th;
  REQUIRE_EQ(pthread_create(&th, NULL, _async_get_thread, &ctx), 0);

  /* Wait for confirmation the request actually reached the worker thread and
   * is blocking inside _slow_handler (i.e. genuinely in-flight) before we
   * force-stop the engine -- a fixed sleep here would be a flaky proxy for
   * this under a slow/loaded environment (e.g. under valgrind, where the
   * accept/parse/dispatch path can easily take longer than a "should be
   * plenty" fixed delay), so wait on the handler's own entry signal instead,
   * bounded generously rather than blocked on forever. */
  {
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 10;
    pthread_mutex_lock(&g_slow_mtx);
    while (!g_slow_entered) {
      if (pthread_cond_timedwait(&g_slow_cv, &g_slow_mtx, &deadline) ==
          ETIMEDOUT)
        break;
    }
    pthread_mutex_unlock(&g_slow_mtx);
  }
  REQUIRE_TRUE(g_slow_entered);

  chttpsvr_engine_stop();

  /* Only now let the handler finish; if the forced stop had abandoned this
   * in-flight request instead of draining it, the client thread below would
   * hang past its own request timeout / never observe a 200. */
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);

  pthread_join(th, NULL);
  REQUIRE_EQ((int)ctx.rv, (int)ccol_success);
  REQUIRE_EQ(ctx.status, 200);

  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);
}
