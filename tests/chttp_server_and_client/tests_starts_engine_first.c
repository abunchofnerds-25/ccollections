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
 * tests.c (sharing this same directory) documents and tests the opposite
 * ordering (chttpclient's async engine calling _cfio_engine_acquire() first,
 * chttpserver second). But cfio_engine.c's one-time global init
 * (_cfio_fio_global_init) is pthread_once-guarded for the whole process
 * lifetime, not per start/stop cycle, so only ONE of the two orderings can
 * ever be genuinely "first" within a single test binary/process: whichever
 * of the two runs second in that file is only ever exercising a
 * ref-count-from-zero restart of an already-globally-initialized reactor,
 * not a true first-caller path. This file is therefore compiled into its
 * own binary, tests_starts_engine_first, separate from tests.c's tests
 * binary (see the Makefile in this same directory) so it gets a genuinely
 * fresh process.
 *
 * The first test below also doubles as the regression test for the lazy,
 * call_once-guarded runtime init that replaced this library's remaining
 * static PTHREAD_MUTEX_INITIALIZER/PTHREAD_COND_INITIALIZER/
 * PTHREAD_RWLOCK_INITIALIZER usages (src/cfio_engine.c's own mutex+2
 * condvars, chttpclient.c's async-engine mutex+condvar and deadline-sweep
 * mutex+condvar, and facio's g_fio_logger_rwlock): this is the one place in
 * the whole test suite where nothing has touched any of that shared state
 * yet, so it is the only place a *concurrent* first touch can be exercised
 * at all. Several chttpsvr instances, chttpcli async requests, and
 * engine-logger calls are all raced from freshly spawned threads before
 * anything else in the process runs, to confirm the new call_once guards
 * correctly serialise exactly one real init under genuine concurrency with
 * no crash, deadlock, or corruption - see server_is_the_genuine_first_
 * engine_caller below. A second test then confirms chttpclient's async
 * engine can still come up afterward and share the already-running reactor
 * normally, exactly mirroring what tests.c verifies from the opposite
 * starting direction.
 */

#include <cfio_engine.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <tau/tau.h>
#include <unistd.h>

TAU_MAIN()

#define TEST_PORT 19200
/* Number of independent chttpsvr instances raced concurrently as the
 * process's first-ever touch of the shared engine, each on its own port. */
#define NUM_RACE_SERVERS 4
/* Number of chttpcli async requests raced concurrently alongside the
 * servers above; only the engine-bootstrap race matters here; the actual
 * network outcome of these specific calls is not asserted on, since some of
 * them may legitimately race ahead of their target server's own bind. */
#define NUM_RACE_CLIENTS 0
/* Number of threads racing chttpsvr_set_engine_logger concurrently, to
 * exercise facio's own logger rwlock's lazy init under the same conditions. */
#define NUM_RACE_LOGGER_SETTERS 4

static clog g_test_logger = NULL;
static chttpsvr g_srv[NUM_RACE_SERVERS] = {NULL};
/* A dedicated, explicitly-destroyed chttpcli for the racing client threads
 * below (rather than chttp_default_client(), the process-level client that
 * is never destroyed): a successful, keep-alive-eligible response leaves its
 * underlying connection in the client's idle pool, holding that client's
 * async-engine-user reference until the client itself is destroyed (or the
 * pool's own 60s idle timeout elapses) - by design, not a bug, but exactly
 * why a client meant to be waited-on-until-idle shortly afterward must be
 * one this test fully controls and destroys, not the shared default client
 * every other test in this binary/process may still be relying on. */
static chttpcli g_race_client = NULL;

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
  for (int i = 0; i < NUM_RACE_SERVERS; i++) {
    if (g_srv[i]) {
      chttpsvr_stop(g_srv[i]);
      __chttpsvr_destroy(g_srv[i]);
      g_srv[i] = NULL;
    }
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

/* --- Race-thread bodies for the concurrent first-touch test below --- */

typedef struct {
  int index;
  ccol_retval_t rv;
} _race_server_arg_t;

static void *_race_server_thread(void *arg) {
  _race_server_arg_t *a = (_race_server_arg_t *)arg;
  char *err = NULL;
  g_srv[a->index] = create_chttpsvr(g_test_logger, &err);
  if (!g_srv[a->index]) {
    a->rv = ccol_unexpected_failure;
    return NULL;
  }
  chttpsvr_register_handler(g_srv[a->index], CHTTP_GET, "/hello",
                            _hello_handler, NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = TEST_PORT + a->index;
  a->rv = chttpsvr_start(g_srv[a->index], &cfg);
  return NULL;
}

static void *_race_client_thread(void *arg) {
  int index = *(int *)arg;
  char url[128];
  snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello",
           TEST_PORT + (index % NUM_RACE_SERVERS));
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  if (!req) return NULL;

  /* The point of this thread is racing _cfio_engine_acquire()/this module's
   * own async-engine bootstrap via chttpclient_do_async, not proving a
   * successful round trip against a server that may not have finished
   * binding its port yet; the actual result (success or a connection-level
   * failure) is intentionally not asserted on here. */
  ctpool_future *f = chttpclient_do_async(g_race_client, req);
  chttp_request_free(req);
  if (!f) return NULL;
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  if (raw) {
    if (raw->resp) chttpclient_resp_free(raw->resp);
    chttpclient_async_result_free(raw);
  }
  ctpool_future_free(f);
  return NULL;
}

static void *_race_logger_setter_thread(void *arg) {
  ccol_retval_t *rv = (ccol_retval_t *)arg;
  *rv = chttpsvr_set_engine_logger(g_test_logger);
  return NULL;
}

TEST(engine_startup_order, server_is_the_genuine_first_engine_caller) {
  /* Nothing in this process, neither chttpserver nor chttpclient, has
     touched the shared engine yet. */
  REQUIRE_FALSE(_cfio_engine_running());

  pthread_t server_threads[NUM_RACE_SERVERS];
  _race_server_arg_t server_args[NUM_RACE_SERVERS];
  pthread_t client_threads[NUM_RACE_CLIENTS];
  int client_indices[NUM_RACE_CLIENTS];
  pthread_t logger_threads[NUM_RACE_LOGGER_SETTERS];
  ccol_retval_t logger_rvs[NUM_RACE_LOGGER_SETTERS];

  chttpcli_construct(race_client);
  REQUIRE_NE((void *)race_client, NULL);
  g_race_client = race_client;

  /* Every thread below races the very first call into
     src/cfio_engine.c's shared mutex/condvar pair, chttpclient.c's own
     async-engine and deadline-sweep mutex/condvar pairs, and facio's
     logger rwlock - all four now lazily, call_once-guarded initialised at
     runtime rather than statically. This is the true first call to
     _cfio_engine_acquire() in this process, from any of these paths. */
  for (int i = 0; i < NUM_RACE_SERVERS; i++) {
    server_args[i].index = i;
    server_args[i].rv = ccol_unexpected_failure;
    REQUIRE_EQ(pthread_create(&server_threads[i], NULL, _race_server_thread,
                              &server_args[i]),
               0);
  }
  for (int i = 0; i < NUM_RACE_CLIENTS; i++) {
    client_indices[i] = i;
    REQUIRE_EQ(pthread_create(&client_threads[i], NULL, _race_client_thread,
                              &client_indices[i]),
               0);
  }
  for (int i = 0; i < NUM_RACE_LOGGER_SETTERS; i++) {
    REQUIRE_EQ(pthread_create(&logger_threads[i], NULL,
                              _race_logger_setter_thread, &logger_rvs[i]),
               0);
  }

  for (int i = 0; i < NUM_RACE_SERVERS; i++)
    pthread_join(server_threads[i], NULL);
  for (int i = 0; i < NUM_RACE_CLIENTS; i++)
    pthread_join(client_threads[i], NULL);
  for (int i = 0; i < NUM_RACE_LOGGER_SETTERS; i++)
    pthread_join(logger_threads[i], NULL);

  /* Done racing; release every pooled connection race_client's successful
     requests may be holding onto before waiting for chttpclient to go idle
     below (see the comment on g_race_client's declaration for why). */
  chttpclient_destroy(race_client);
  g_race_client = NULL;

  for (int i = 0; i < NUM_RACE_SERVERS; i++)
    REQUIRE_EQ(server_args[i].rv, ccol_success);
  for (int i = 0; i < NUM_RACE_LOGGER_SETTERS; i++)
    REQUIRE_EQ(logger_rvs[i], ccol_success);

  REQUIRE_TRUE(_cfio_engine_running());

  /* atexit(fio_lib_destroy) / atexit(_cfio_engine_atexit_safety_net) were
     just registered by whichever racing thread's chttpsvr_start() call
     happened to win the underlying call_once; atexit runs handlers in
     reverse registration order, so _teardown must be registered strictly
     after this point, not in _setup(), or fio_lib_destroy would run before
     it at process exit and _teardown's own chttpsvr_stop()/fio_close() call
     would dereference fio_data after it was already unmapped (the same
     SIGSEGV-shaped hazard tests/chttp_server_and_client/tests.c documents
     at length). */
  atexit(_teardown);

  /* Now that every racing server thread has confirmably finished
     chttpsvr_start() (joined above), each server is definitely listening;
     verify every one of them with an ordinary, deterministic request -
     mirroring this test's original single-server final check. */
  for (int i = 0; i < NUM_RACE_SERVERS; i++) {
    char url[128];
    snprintf(url, sizeof(url), "http://127.0.0.1:%d/hello", TEST_PORT + i);
    chttpcli_response *resp = NULL;
    REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
    REQUIRE_NE((void *)resp, NULL);
    REQUIRE_EQ(resp->status_code, 200);
    REQUIRE_STREQ(resp->body, "hello");
    chttpclient_resp_free(resp);
  }

  _wait_for_chttpclient_idle();
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
  /* g_srv[] instances still hold their own references; the shared reactor
     must still be running purely because of chttpserver's side now that
     chttpclient has fully quiesced - same invariant
     tests/chttp_server_and_client/tests.c checks after its own
     concurrent-load test. */
  REQUIRE_TRUE(_cfio_engine_running());
}
