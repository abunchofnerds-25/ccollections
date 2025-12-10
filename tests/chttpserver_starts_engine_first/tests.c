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
 * Dedicated, minimal suite whose sole purpose is to have chttpserver be the
 * genuinely first-ever caller into the shared facio reactor
 * (src/cfio_engine.c) in a fresh process.
 *
 * tests/chttp_server_and_client/tests.c documents and tests the opposite
 * ordering (chttpclient's async engine calling _cfio_engine_acquire() first,
 * chttpserver second). But cfio_engine.c's one-time global init
 * (_cfio_fio_global_init) is pthread_once-guarded for the whole process
 * lifetime, not per start/stop cycle, so only ONE of the two orderings can
 * ever be genuinely "first" within a single test binary/process: whichever
 * of the two runs second in that file is only ever exercising a
 * ref-count-from-zero restart of an already-globally-initialized reactor,
 * not a true first-caller path. This suite is the other half of that claim,
 * in its own process: chttpserver calls _cfio_engine_acquire() (via
 * chttpsvr_start()) before anything else in this process has ever touched
 * the shared engine, and a second test then confirms chttpclient's async
 * engine can still come up afterward and share the already-running reactor
 * normally, exactly mirroring what tests/chttp_server_and_client/tests.c
 * verifies from the opposite starting direction.
 */

#include <cfio_engine.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <unistd.h>

TAU_MAIN()

#define TEST_PORT 19200

static clog g_test_logger = NULL;
static chttpsvr g_srv = NULL;

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

/* White-box helpers exposing chttpclient's own async-engine-user refcount;
 * compiled into chttpclient.c under RUNNING_UNIT_TESTS (same gate this
 * suite's Makefile passes), reused here exactly as
 * tests/chttp_server_and_client/tests.c does. */
extern int _chttpclient_engine_ref_count_for_tests(void);
extern void _chttpclient_engine_wait_for_quiescence_for_tests(void);

static void _wait_for_chttpclient_idle(void) {
  for (int i = 0; i < 5000 && _chttpclient_engine_ref_count_for_tests() > 0;
       i++) {
    usleep(1000);
  }
  _chttpclient_engine_wait_for_quiescence_for_tests();
}

static void _teardown(void) {
  if (g_srv) {
    chttpsvr_stop(g_srv);
    __chttpsvr_destroy(g_srv);
    g_srv = NULL;
  }
  chttpsvr_engine_wait();
  if (g_test_logger) {
    clog_close(g_test_logger);
    g_test_logger = NULL;
  }
}

__attribute__((constructor)) static void _setup(void) {
  g_test_logger = clog_open_fd(2, CLOG_INFO);
  if (!g_test_logger) {
    fprintf(stderr, "FATAL: could not create test logger\n");
    exit(1);
  }
  /* _teardown is deliberately NOT registered here; see the comment inside
   * server_is_the_genuine_first_engine_caller below for why, mirroring
   * tests/chttp_server_and_client/tests.c's identical reasoning. */
}

TEST(engine_startup_order, server_is_the_genuine_first_engine_caller) {
  /* Nothing in this process, neither chttpserver nor chttpclient, has
     touched the shared engine yet. */
  REQUIRE_FALSE(_cfio_engine_running());

  char *err = NULL;
  g_srv = create_chttpsvr(g_test_logger, &err);
  REQUIRE_NE((void *)g_srv, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/hello", _hello_handler, NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT;

  /* This is the true first call to _cfio_engine_acquire() in this process,
     from either module: exercises http_lib_constructor always running
     before fio_lib_init (cfio_engine.c's _cfio_fio_global_init) when
     chttpserver, not chttpclient, is the one to trigger the one-time global
     init. */
  REQUIRE_EQ(chttpsvr_start(g_srv, &cfg), ccol_success);
  REQUIRE_TRUE(_cfio_engine_running());

  /* atexit(fio_lib_destroy) / atexit(_cfio_engine_atexit_safety_net) were
     just registered by the chttpsvr_start() call above (its own first-ever
     _cfio_engine_acquire()); atexit runs handlers in reverse registration
     order, so _teardown must be registered strictly after this point, not
     in _setup(), or fio_lib_destroy would run before it at process exit and
     _teardown's own chttpsvr_stop()/fio_close() call would dereference
     fio_data after it was already unmapped (the same SIGSEGV-shaped hazard
     tests/chttp_server_and_client/tests.c documents at length). */
  atexit(_teardown);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", TEST_PORT);
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

TEST(engine_startup_order,
     chttpclient_async_shares_engine_server_already_started) {
  /* By this point chttpserver already brought the shared engine up as the
     genuine first caller (previous test). Confirm chttpclient's async
     engine (Tier 2) can still come up afterward and share the
     already-running reactor normally - completing, from the opposite
     starting direction, the "either order" claim that
     tests/chttp_server_and_client/tests.c's own client-starts-first test
     covers. */
  REQUIRE_TRUE(_cfio_engine_running());

  chttpcli_construct(cli);
  REQUIRE_NE((void *)cli, NULL);

  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", TEST_PORT);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  REQUIRE_STREQ(raw->resp->body, "hello");
  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  chttpclient_destroy(cli);

  _wait_for_chttpclient_idle();
  /* g_srv still holds its own reference; the shared reactor must still be
     running purely because of chttpserver's side now that chttpclient has
     fully quiesced - same invariant
     tests/chttp_server_and_client/tests.c checks after its own
     concurrent-load test. */
  REQUIRE_TRUE(_cfio_engine_running());
}
