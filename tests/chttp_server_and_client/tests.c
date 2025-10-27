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
 * The first binary in this codebase to link chttpserver.c and chttpclient.c
 * together specifically to exercise both engines running simultaneously in
 * the same process. Prior to src/cfio_engine.c, this combination was a
 * documented hard limitation: chttpserver's engine and chttpclient's async
 * engine (Tier 2/3) each ran their own independent facio reactor, and
 * facio's fio_data is a process-wide singleton, so at most one of the two
 * could ever actually be running. cfio_engine.c unifies them into one
 * shared, ref-counted, lazily-started reactor both modules acquire/release
 * references to; this suite is the regression coverage for that.
 *
 * Test order matters here (tau registers/runs TEST() cases in file
 * declaration order; see tests/chttpserver/tests.c and others for the same
 * assumption already relied on throughout this codebase's test suites): the
 * first few tests deliberately exercise the shared engine from a fully-cold
 * state, in a specific sequence, to pin down exactly which module starts it
 * first. Unlike every other chttp* suite, _setup() here does NOT pre-start
 * any server or touch chttpclient's async engine; doing so would make
 * "who starts the shared engine first" untestable.
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

#define BASE_PORT 19100

static clog g_test_logger = NULL;

/* Kept alive across most of this file's tests once started (see
 * engine_startup_order.server_starts_after_client_already_used_engine);
 * mirrors the shared-fixture pattern tests/chttpserver/tests.c uses, except
 * here the very first test intentionally runs before this exists. */
static chttpsvr g_srv = NULL;

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "hello");
}

/* Blocks until released; mirrors tests/chttpserver/tests.c's
 * _bounded_blk_handler pattern exactly, for the "destroy one server while
 * another has genuinely in-flight chttpclient work" test below. */
static pthread_mutex_t g_slow_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_slow_cv = PTHREAD_COND_INITIALIZER;
static bool g_slow_go = false;

static void _slow_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  pthread_mutex_lock(&g_slow_mtx);
  while (!g_slow_go) pthread_cond_wait(&g_slow_cv, &g_slow_mtx);
  pthread_mutex_unlock(&g_slow_mtx);
  chttpsvr_resp_write_str(resp, "slow-ok");
}

static void _release_slow_handler(void) {
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
}

static void _reset_slow_handler(void) {
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = false;
  pthread_mutex_unlock(&g_slow_mtx);
}

static void _make_srv_url(chttpsvr srv_unused, uint16_t port, const char *path,
                          char *buf, size_t buf_size) {
  (void)srv_unused;
  snprintf(buf, buf_size, "http://127.0.0.1:%u%s", (unsigned)port, path);
}

/* White-box helpers exposing chttpclient's own async-engine-user refcount;
 * compiled into chttpclient.c under RUNNING_UNIT_TESTS (same gate this
 * suite's Makefile passes), reused here exactly as tests/chttpclient/tests.c
 * does, to deterministically settle chttpclient's side between tests without
 * arbitrary sleeps. */
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
  /* Blocks until the shared reactor (if anything is still holding a
   * reference at this point) has actually stopped; required so the
   * engine-installed default logger is reclaimed before this atexit
   * handler returns, mirroring every other chttp* suite's teardown. A no-op
   * if the engine already fully stopped earlier (e.g. the last test already
   * drove it down via chttpsvr_engine_stop/wait). */
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
  /* _teardown is deliberately NOT registered here (unlike every other
   * chttp* suite's _setup()); see _register_teardown_once's own comment
   * for why. */
}

/*
 * atexit(fio_lib_destroy) and atexit(_cfio_engine_atexit_safety_net) are
 * only registered once something actually calls _cfio_engine_acquire() for
 * the first time in this process (see cfio_engine.c's own pthread_once-
 * guarded global init); and, uniquely in this suite, that first call could
 * come from either chttpclient (test 1) or chttpserver (test 2), by design.
 * atexit runs handlers in reverse registration order, so _teardown must be
 * registered strictly AFTER whichever of those two calls happens first, or
 * fio_lib_destroy would run before it at process exit and _teardown's own
 * chttpsvr_stop()/fio_close() call would dereference fio_data after it was
 * already unmapped; a real SIGSEGV, caught during development of this
 * suite (registering atexit(_teardown) in _setup(), before either engine had
 * ever been touched, put it first in registration order and therefore LAST
 * at exit, exactly backwards). Guarded by pthread_once and invoked from both
 * test 1 (right after its first chttpclient_do_async call, which has
 * already synchronously called _cfio_engine_acquire() by the time it
 * returns a non-NULL future) and test 2 (right after its chttpsvr_start
 * call) so registration happens correctly regardless of which module gets
 * there first. */
static pthread_once_t _teardown_atexit_once = PTHREAD_ONCE_INIT;
static void _register_teardown(void) { atexit(_teardown); }

/* ========================================================================== */
/*  1. chttpclient starts the shared engine first, entirely on its own.       */
/* ========================================================================== */

TEST(engine_startup_order, client_starts_shared_engine_first) {
  /* Nothing in this process has touched the shared engine yet. */
  REQUIRE_FALSE(_cfio_engine_running());

  chttpcli_construct(cli);
  REQUIRE_NE((void *)cli, NULL);
  chttpclient_set_connect_timeout(cli, 500);

  /* 127.0.0.1:1; no listener there; expected to fail fast (connection
   * refused) rather than hang, proving chttpclient's async engine can start
   * the shared reactor entirely unassisted (no chttpsvr has ever run in
   * this process) and drive a real connection attempt through it. */
  chttp_request_t *req =
      chttp_request_new(CHTTP_GET, "http://127.0.0.1:1/", NULL, NULL);
  REQUIRE_NE((void *)req, NULL);

  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);
  /* _cfio_engine_acquire() has already been called synchronously by this
   * point (chttpclient_do_async only returns a non-NULL future after it
   * succeeds); see _register_teardown's own comment for why registration
   * must happen here, not in _setup(). */
  pthread_once(&_teardown_atexit_once, _register_teardown);

  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  /* A connection-refused failure, not a hang and not ccol_success. */
  REQUIRE_NE(raw->rv, ccol_success);

  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  chttpclient_destroy(cli);

  /* The engine must still be usable (query, not just "started once"). */
  _wait_for_chttpclient_idle();
  /* chttpclient was the only user; the shared reactor should now be fully
   * torn down again; proving a clean start-then-stop cycle driven purely
   * by chttpclient, with no chttpserver involvement at all. */
  REQUIRE_FALSE(_cfio_engine_running());
}

/* ========================================================================== */
/*  2. chttpserver starts AFTER the shared engine has already been brought    */
/*     up and fully down once by chttpclient alone.                          */
/* ========================================================================== */

TEST(engine_startup_order, server_starts_after_client_already_used_engine) {
  REQUIRE_FALSE(_cfio_engine_running());

  char *err = NULL;
  g_srv = create_chttpsvr(g_test_logger, &err);
  REQUIRE_NE((void *)g_srv, NULL);

  chttpsvr_register_handler(g_srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/slow", _slow_handler, NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = BASE_PORT;
  cfg.worker_thread_count = 4;

  /* This is chttpserver's very first chttpsvr_start() call in the process;
   * exercises the http_lib_constructor-ordering fix (cfio_engine.c always
   * calls it, regardless of who acquired the shared engine first) and the
   * http_listen simplification (chttpsvr_start no longer branches on
   * first-vs-subsequent start; it always takes the engine-already-running
   * path once _cfio_engine_acquire confirms the reactor is up; true here
   * even though this really is chttpserver's first call, since
   * _cfio_engine_acquire() fully (re)started the reactor from cold before
   * returning). */
  ccol_retval_t rv = chttpsvr_start(g_srv, &cfg);
  REQUIRE_EQ(rv, ccol_success);
  REQUIRE_TRUE(_cfio_engine_running());
  /* A no-op here in practice (test 1 already registered it), but kept as a
   * defensive second call site (see _register_teardown's own comment)
   * in case this suite is ever reordered so chttpserver becomes the first
   * module to touch the shared engine. */
  pthread_once(&_teardown_atexit_once, _register_teardown);

  char url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", url, sizeof(url));
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_NE((void *)resp, NULL);
  REQUIRE_EQ(resp->status_code, 200);
  REQUIRE_STREQ(resp->body, "hello");
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*  3. Concurrent load on both sides at once.                                */
/* ========================================================================== */

#define CONCURRENT_REQS 8

typedef struct {
  char url[128];
  bool ok;
} _async_job_t;

static void *_async_get_thread(void *arg) {
  _async_job_t *job = (_async_job_t *)arg;
  chttpcli_construct(cli);
  chttp_request_t *req = chttp_request_new(CHTTP_GET, job->url, NULL, NULL);
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  job->ok = raw && raw->rv == ccol_success && raw->resp &&
            raw->resp->status_code == 200;
  if (raw) {
    if (raw->resp) chttpclient_resp_free(raw->resp);
    chttpclient_async_result_free(raw);
  }
  ctpool_future_free(f);
  chttpclient_destroy(cli);
  return NULL;
}

TEST(simultaneous_engines, server_and_client_serve_concurrently) {
  REQUIRE_NE((void *)g_srv, NULL);

  pthread_t threads[CONCURRENT_REQS];
  _async_job_t jobs[CONCURRENT_REQS];
  for (int i = 0; i < CONCURRENT_REQS; i++) {
    _make_srv_url(g_srv, BASE_PORT, "/hello", jobs[i].url, sizeof(jobs[i].url));
    jobs[i].ok = false;
    REQUIRE_EQ(pthread_create(&threads[i], NULL, _async_get_thread, &jobs[i]),
               0);
  }
  for (int i = 0; i < CONCURRENT_REQS; i++) {
    pthread_join(threads[i], NULL);
    REQUIRE_TRUE(jobs[i].ok);
  }

  /* Tier 1 (sync) keeps working unaffected, concurrently exercised above. */
  char url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", url, sizeof(url));
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);

  _wait_for_chttpclient_idle();
  /* g_srv still holds its own reference; the shared reactor must still be
   * running purely because of chttpserver's side now, chttpclient having
   * fully quiesced. */
  REQUIRE_TRUE(_cfio_engine_running());
}

/* ========================================================================== */
/*  4. Destroying one server does not affect another server's listener nor   */
/*     genuinely in-flight chttpclient work against it.                     */
/* ========================================================================== */

TEST(simultaneous_engines,
     destroying_one_server_does_not_affect_other_or_inflight_client_work) {
  REQUIRE_NE((void *)g_srv, NULL);
  _reset_slow_handler();

  char *err = NULL;
  chttpsvr srv2 = create_chttpsvr(g_test_logger, &err);
  REQUIRE_NE((void *)srv2, NULL);
  chttpsvr_register_handler(srv2, CHTTP_POST, "/slow", _slow_handler, NULL);

  chttpsvr_config_t cfg2 = CHTTPSVR_CONFIG_DEFAULT;
  cfg2.host = "127.0.0.1";
  cfg2.port = BASE_PORT + 1;
  REQUIRE_EQ(chttpsvr_start(srv2, &cfg2), ccol_success);

  char slow_url[128];
  _make_srv_url(srv2, (uint16_t)(BASE_PORT + 1), "/slow", slow_url,
                sizeof(slow_url));

  chttpcli_construct(cli);
  chttp_request_body_t body = CHTTP_TEXT_BODY("x", 1);
  chttp_request_t *req = chttp_request_new(CHTTP_POST, slow_url, &body, NULL);
  ctpool_future *f = chttpclient_do_async(cli, req);
  REQUIRE_NE((void *)f, NULL);
  chttp_request_free(req);

  /* While that request is blocked server-side (srv2's handler is parked on
   * g_slow_cv), fully create, start, and destroy a THIRD, unrelated server
   * ; this drops and re-acquires shared-engine references while srv2's
   * in-flight request is still outstanding, which must not disturb it. */
  chttpsvr srv3 = create_chttpsvr(g_test_logger, NULL);
  REQUIRE_NE((void *)srv3, NULL);
  chttpsvr_config_t cfg3 = CHTTPSVR_CONFIG_DEFAULT;
  cfg3.host = "127.0.0.1";
  cfg3.port = BASE_PORT + 2;
  REQUIRE_EQ(chttpsvr_start(srv3, &cfg3), ccol_success);
  chttpsvr_stop(srv3);
  __chttpsvr_destroy(srv3);

  /* g_srv (untouched by any of the above) must still respond normally. */
  char hello_url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", hello_url, sizeof(hello_url));
  chttpcli_response *hello_resp = NULL;
  REQUIRE_EQ(chttp_get(hello_url, &hello_resp), ccol_success);
  REQUIRE_EQ(hello_resp->status_code, 200);
  chttpclient_resp_free(hello_resp);

  /* Now let the originally in-flight request against srv2 complete. */
  _release_slow_handler();
  chttpcli_async_result_t *raw = chttpclient_async_result_get(f);
  REQUIRE_NE((void *)raw, NULL);
  REQUIRE_EQ(raw->rv, ccol_success);
  REQUIRE_NE((void *)raw->resp, NULL);
  REQUIRE_EQ(raw->resp->status_code, 200);
  REQUIRE_STREQ(raw->resp->body, "slow-ok");
  chttpclient_resp_free(raw->resp);
  chttpclient_async_result_free(raw);
  ctpool_future_free(f);
  chttpclient_destroy(cli);

  chttpsvr_stop(srv2);
  __chttpsvr_destroy(srv2);

  _wait_for_chttpclient_idle();
  REQUIRE_TRUE(_cfio_engine_running()); /* g_srv still holds a reference */
}

/* ========================================================================== */
/*  5. Destroy the last live server, explicitly wait, then immediately       */
/*     restart a fresh one on the exact same port.                          */
/* ========================================================================== */

TEST(engine_wide_shutdown,
     destroy_last_reference_then_restart_on_same_port_after_explicit_wait) {
  REQUIRE_NE((void *)g_srv, NULL);
  /* By this point chttpclient holds no references (drained at the end of
   * every prior TEST()) and g_srv is the only chttpsvr ever left running;
   * so destroying it really does drop the shared reactor's reference count
   * to zero, unlike a destroy earlier in this file where other references
   * were still outstanding. */
  chttpsvr_stop(g_srv);
  __chttpsvr_destroy(g_srv);
  g_srv = NULL;

  /* Per cfio_engine.h: __chttpsvr_destroy no longer synchronously guarantees
   * the shared reactor has fully stopped by the time it returns; callers
   * needing that guarantee must call chttpsvr_engine_wait() explicitly. This
   * is the regression test for that exact behavioral change: without this
   * call, immediately restarting on the same port below could race a
   * still-in-flight reaper thread. */
  chttpsvr_engine_wait();
  REQUIRE_FALSE(_cfio_engine_running());

  char *err = NULL;
  g_srv = create_chttpsvr(g_test_logger, &err);
  REQUIRE_NE((void *)g_srv, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_register_handler(g_srv, CHTTP_POST, "/slow", _slow_handler, NULL);

  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = BASE_PORT; /* exact same port g_srv used before */
  REQUIRE_EQ(chttpsvr_start(g_srv, &cfg), ccol_success);
  REQUIRE_TRUE(_cfio_engine_running());

  char url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", url, sizeof(url));
  chttpcli_response *resp = NULL;
  REQUIRE_EQ(chttp_get(url, &resp), ccol_success);
  REQUIRE_EQ(resp->status_code, 200);
  chttpclient_resp_free(resp);
}

/* ========================================================================== */
/*  6. chttpsvr_engine_stop()/chttpsvr_engine_wait() tear down BOTH sides    */
/*     together. Must be the LAST test in this file: it forces the shared   */
/*     reactor down for the rest of the process's lifetime.                 */
/* ========================================================================== */

typedef struct {
  chttpcli_async_result_t *raw;
} _stop_job_t;

static void *_engine_stop_get_thread(void *arg) {
  _stop_job_t *job = (_stop_job_t *)arg;
  chttpcli_construct(cli);
  char url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", url, sizeof(url));
  chttp_request_t *req = chttp_request_new(CHTTP_GET, url, NULL, NULL);
  ctpool_future *f = chttpclient_do_async(cli, req);
  chttp_request_free(req);
  /* Guaranteed to eventually be fulfilled exactly once regardless of how
   * the connection dies; see chttp_async_chain_t's fulfilled-once-guard
   * backstop, documented in guidelines.txt's chttpclient section; so blocking
   * here cannot hang even if chttpsvr_engine_stop() tears the connection
   * down mid-flight. */
  job->raw = chttpclient_async_result_get(f);
  ctpool_future_free(f);
  chttpclient_destroy(cli);
  return NULL;
}

static void *_engine_stop_thread(void *arg) {
  (void)arg;
  /* Give the request thread a head start so there is a realistic chance the
   * stop lands while the request is genuinely in flight, without making the
   * test's correctness depend on that timing (the assertions below accept
   * either outcome). */
  usleep(2000);
  chttpsvr_engine_stop();
  return NULL;
}

TEST(engine_wide_shutdown, engine_stop_tears_down_both_sides_together) {
  REQUIRE_NE((void *)g_srv, NULL);
  REQUIRE_TRUE(_cfio_engine_running());

  _stop_job_t job = {.raw = NULL};
  pthread_t req_thread, stop_thread;
  REQUIRE_EQ(pthread_create(&req_thread, NULL, _engine_stop_get_thread, &job),
             0);
  REQUIRE_EQ(pthread_create(&stop_thread, NULL, _engine_stop_thread, NULL), 0);

  pthread_join(stop_thread, NULL);
  /* chttpsvr_engine_stop() is documented as non-blocking; chttpsvr_engine_
   * wait() blocks the calling thread until the shared reactor (and, since
   * it is shared, any in-flight chttpclient async work along with it) has
   * actually finished tearing down THIS particular forced stop. */
  chttpsvr_engine_wait();
  pthread_join(req_thread, NULL);

  /* Either the request had already completed successfully before the stop
   * landed, or it was aborted by the shared reactor going down; both are
   * acceptable outcomes; a hang or crash is not. Note this may itself have
   * triggered chttpclient's dead-connection retry-once logic (_async_retry_
   * hop, the Tier 2 mirror of Tier 1's identical mechanism), which (like
   * any ordinary new acquire) is entirely free to bring the shared engine
   * back up again after this forced stop finishes; that is correct,
   * expected behavior of a shared, reference-counted engine, not something
   * chttpsvr_engine_stop() prevents for future callers. */
  REQUIRE_NE((void *)job.raw, NULL);
  if (job.raw->rv == ccol_success && job.raw->resp) {
    chttpclient_resp_free(job.raw->resp);
  }
  chttpclient_async_result_free(job.raw);

  /* Drain chttpclient's side fully (including any retry the forced stop
   * triggered) before asserting the shared engine is down; otherwise the
   * assertion below can race a legitimate, still-in-flight retry attempt
   * that hasn't released its own engine reference yet. */
  _wait_for_chttpclient_idle();
  REQUIRE_FALSE(_cfio_engine_running());

  /* The listener is gone along with the rest of the reactor; a fresh
   * request must now fail rather than silently succeed against a socket
   * that should no longer be accepting connections. */
  char url[128];
  _make_srv_url(g_srv, BASE_PORT, "/hello", url, sizeof(url));
  chttpcli_response *resp = NULL;
  REQUIRE_NE(chttp_get(url, &resp), ccol_success);
  if (resp) chttpclient_resp_free(resp);

  /* g_srv's listener socket is already gone (torn down with the rest of the
   * reactor above); this only frees the still-live srv object's own memory
   * (ctpool, routers, clog, ...). _teardown() would otherwise try this
   * again at process exit and find g_srv already NULL. */
  __chttpsvr_destroy(g_srv);
  g_srv = NULL;
}
