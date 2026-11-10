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

// Tests of chttpsvr_engine_stop(), the forced, process-wide stop of the shared
// engine that a signal handler starts. It tears down the one reactor that every
// server of the process uses, so it can't share the long-lived servers of
// tests.c; this file is a binary of its own.

// Asks for GNU extensions before the first include, because the fixture joins
// with pthread_timedjoin_np.
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/test_helpers/chttpserver.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#if defined(__FreeBSD__)
#include <pthread_np.h>
#endif

#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>

TAU_MAIN()

// Ignores SIGPIPE and resets the rest of the inherited signal state before any
// thread starts. The test clients write with plain write(2) and the server can
// close a connection during one; tests_sigpipe.c covers the default
// disposition.
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

// The bounded-join fixture. Every test races background threads against the
// library's locks, and a real regression can leave a thread stuck, possibly
// holding the process-wide engine mutex, so every later test would hang on it.
// The tests use TEST_F: a test tracks each thread it creates with _fx_track()
// and joins with _fx_join(), and the teardown joins what is left with a bound,
// then detaches it. A hang then fails the test at fault instead of a later one.

// The most threads one test may track, the size of the fixture's arrays; no
// test needs more than 3.
#define ENGINE_STOP_FIXTURE_MAX_THREADS 4

// The fixture's tracked threads; active[i] is true while thread i isn't joined.
// The struct carries the tag engine_stop_fixture, because TEST_F and its
// helpers expand FIXTURE to `struct FIXTURE`.
typedef struct engine_stop_fixture {
  pthread_t threads[ENGINE_STOP_FIXTURE_MAX_THREADS];
  bool active[ENGINE_STOP_FIXTURE_MAX_THREADS];
  int count;
} engine_stop_fixture;

// Registers tid with the fixture, so the teardown joins it with a bound when
// the test never does. Call it once for each thread right after
// pthread_create() succeeds. It asserts when a test tracks more than
// ENGINE_STOP_FIXTURE_MAX_THREADS threads.
static void _fx_track(engine_stop_fixture *fx, pthread_t tid) {
  assert(fx->count < ENGINE_STOP_FIXTURE_MAX_THREADS);
  fx->threads[fx->count] = tid;
  fx->active[fx->count] = true;
  fx->count++;
}

// Joins tid, waiting 30 s at most, which is far beyond every legitimate wait of
// this file, so a stuck thread can't hang the caller. Gives true only when the
// thread stopped and was joined. Where TEST_TIMEDJOIN_VISIBLE is 0 it is a
// plain join.
static bool _fx_timed_join(pthread_t tid, void **retval) {
#if TEST_TIMEDJOIN_VISIBLE
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 30;
  return pthread_timedjoin_np(tid, retval, &deadline) == 0;
#else
  return pthread_join(tid, retval) == 0;
#endif
}

// Joins tid through _fx_timed_join() and marks it handled in fx, so the
// teardown doesn't join it a second time (undefined behaviour). Every test body
// joins through it. An untracked tid is joined without a mark. Gives the result
// of the join.
static bool _fx_join(engine_stop_fixture *fx, pthread_t tid, void **retval) {
  bool ok = _fx_timed_join(tid, retval);
  if (ok) {
    for (int i = 0; i < fx->count; i++) {
      if (fx->active[i] && pthread_equal(fx->threads[i], tid)) {
        fx->active[i] = false;
        break;
      }
    }
  }
  return ok;
}

// Sets up the fixture, which TEST_F requires; Tau has zeroed its fields
// already.
TEST_F_SETUP(engine_stop_fixture) { (void)tau; }

// Joins every thread the test left unjoined, with a bound, and detaches and
// reports the ones that never finish, so a regression fails this test instead
// of hanging the next one.
TEST_F_TEARDOWN(engine_stop_fixture) {
  for (int i = 0; i < tau->count; i++) {
    if (!tau->active[i]) continue;
    if (!_fx_timed_join(tau->threads[i], NULL)) {
      fprintf(stderr,
              "[WARN] tests_engine_stop teardown: a background thread that "
              "this test started was still running 30s after the test "
              "itself finished. A real regression is suspected. Abandoning "
              "that thread instead of hanging the other tests.\n");
      pthread_detach(tau->threads[i]);
    }
  }
}

// Answers "Hello, engine-stop!"; the route that proves a server serves.
static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, engine-stop!");
}

// Makes one GET of url with a fresh client and gives the result; on success the
// status goes to *status_out. The tests use it to prove that a server serves.
static ccol_retval _get(const char *url, int *status_out) {
  chttpcli cli = chttpcli_create(NULL);
  if (!cli) return ccol_not_enough_memory;
  chttp_request *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
  if (!req) {
    chttpcli_destroy(cli);
    return ccol_not_enough_memory;
  }
  chttpcli_response *resp = NULL;
  ccol_retval rv = chttpcli_do(cli, req, &resp);
  if (rv == ccol_success && resp) *status_out = resp->status_code;
  if (resp) chttpcli_resp_free(resp);
  chttp_request_free(req);
  chttpcli_destroy(cli);
  return rv;
}

// Creates a server with /hello and starts it on 127.0.0.1:port, ending the
// process when either step fails, because every test needs it to start.
// port_str is unused.
static chttpsvr _start_server(const char *port_str, uint16_t port) {
  (void)port_str;
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  if (!srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = port;
  ccol_retval rv = chttpsvr_start(srv, &cfg);
  if (rv != ccol_success) {
    fprintf(stderr, "FATAL: chttpsvr_start failed: %d\n", rv);
    exit(1);
  }
  return srv;
}

// A forced stop of a started server, then its destroy. The reaper must reset
// the server count when it frees the registry, or the destroy dereferences a
// NULL entry. A server on the same port must then serve again.
TEST_F(engine_stop_fixture, force_stop_while_started_then_destroy_is_safe) {
  (void)tau;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18796", 18796);

  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18796", 18796);
  status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

// The reaper quiesces every registered server: two started servers, each with
// an idle keep-alive connection.
TEST_F(engine_stop_fixture, force_stop_quiesces_multiple_servers) {
  (void)tau;
  chttpsvr srv_a ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18797", 18797);
  chttpsvr srv_b ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18798", 18798);

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

// The request and the result of one GET made on a background thread.
typedef struct {
  const char *url;
  int status;
  ccol_retval rv;
} _async_get_ctx;

// Makes the GET of ctx->url and stores the result in ctx, so a test can keep a
// request in flight while it stops the engine.
static void *_async_get_thread(void *arg) {
  _async_get_ctx *ctx = (_async_get_ctx *)arg;
  ctx->rv = _get(ctx->url, &ctx->status);
  return NULL;
}

// The gate of _slow_handler: g_slow_entered is set when it starts, and it waits
// until g_slow_go is set, all under g_slow_mtx and g_slow_cv.
static pthread_mutex_t g_slow_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_slow_cv = PTHREAD_COND_INITIALIZER;
static bool g_slow_go = false;
static bool g_slow_entered = false;

// Sets g_slow_entered and waits for g_slow_go before it answers, so a test can
// hold a request in flight at a chosen moment.
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

// A forced stop must drain a request that a worker is handling, as a graceful
// destroy does, instead of abandoning it. The test waits for the handler's
// entry (not a sleep), stops the engine, then releases the handler; without the
// drain the client never sees its 200. The in-flight wait bounds are shrunk so
// a failing run ends quickly (3 s, so a late worker still finishes on a busy
// machine). ctx lives on the heap and isn't freed when its thread may still
// run.
TEST_F(engine_stop_fixture,
       force_stop_drains_in_flight_request_before_reactor_teardown) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(3000, 2000);

  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(CLOG_NULL, &err);
  if (srv == CHTTPSVR_NULL) chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow", _slow_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18799;
  ccol_retval start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success)
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ctx->url = "http://127.0.0.1:18799/slow";
  ctx->status = 0;
  ctx->rv = ccol_success;
  pthread_t th;
  int create_rv = pthread_create(&th, NULL, _async_get_thread, ctx);
  if (create_rv != 0) {
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(create_rv, 0);
  _fx_track(tau, th);

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
  if (!g_slow_entered) {
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, th, NULL);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_TRUE(g_slow_entered);

  chttpsvr_engine_stop();

  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);

  bool th_joined = _fx_join(tau, th, NULL);
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(th_joined);
  ccol_retval async_rv = ctx->rv;
  int async_status = ctx->status;
  free(ctx);
  REQUIRE_EQ((int)async_rv, (int)ccol_success);
  REQUIRE_EQ(async_status, 200);

  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);
}

// Sets g_slow_go after 150 ms. The delay lets the reaper of a concurrent
// engine stop win the quiesce of the server and block in its drain on the held
// request first.
static void *_release_slow_handler_after_delay(void *arg) {
  (void)arg;
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
  return NULL;
}

// The reaper of chttpsvr_engine_stop() and a chttpsvr_destroy() of the same
// server race with nothing between them, as the documentation allows. The loser
// of the quiesce must wait for the winner's teardown instead of freeing the
// server while the reaper drains it (a use-after-free and a mutex destroyed in
// use). A hook confirms the reaper claimed the quiesce before the destroy runs,
// so the destroy loses every time; the drain blocks on the held request until
// _release_slow_handler_after_delay. No REQUIRE runs between the stop and the
// destroy, so the scoped destructor can't join the race.
TEST_F(engine_stop_fixture, concurrent_destroy_and_engine_stop_is_safe) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(3000, 2000);

  char *err = NULL;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(CLOG_NULL, &err);
  if (srv == CHTTPSVR_NULL) chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow-race", _slow_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18800;
  ccol_retval start_rv2 = chttpsvr_start(srv, &cfg);
  if (start_rv2 != ccol_success)
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_EQ((int)start_rv2, (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ctx->url = "http://127.0.0.1:18800/slow-race";
  ctx->status = 0;
  ctx->rv = ccol_success;
  pthread_t req_th;
  int req_create_rv = pthread_create(&req_th, NULL, _async_get_thread, ctx);
  if (req_create_rv != 0) {
    free(ctx);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(req_create_rv, 0);
  _fx_track(tau, req_th);

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
  if (!g_slow_entered) {
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_TRUE(g_slow_entered);

  pthread_t releaser_th;
  int releaser_create_rv = pthread_create(
      &releaser_th, NULL, _release_slow_handler_after_delay, NULL);
  if (releaser_create_rv == 0) {
    _fx_track(tau, releaser_th);
  } else {
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(releaser_create_rv, 0);

  chttpsvr__arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests();
  chttpsvr__release_quiesce_teardown_race_hook_for_tests();

  chttpsvr_destroy(srv);

  _fx_join(tau, releaser_th, NULL);
  bool req_th_joined = _fx_join(tau, req_th, NULL);
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(req_th_joined);
  ccol_retval async_rv = ctx->rv;
  int async_status = ctx->status;
  free(ctx);
  REQUIRE_EQ((int)async_rv, (int)ccol_success);
  REQUIRE_EQ(async_status, 200);

  chttpsvr_engine_wait();

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18801", 18801);
  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18801/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

// A server started on a background thread while the engine is force-stopped,
// and the result of its start.
static chttpsvr g_race_srv = CHTTPSVR_NULL;
static ccol_retval g_race_start_rv = ccol_success;
// Set as the last step of _start_racing_server_thread. The test polls it, with
// a bound, before it reads the two plain globals above or joins the thread;
// that gives the reads a happens-before order and turns a deadlock into a
// failed check.
static _Atomic bool g_race_start_returned = false;

// Creates g_race_srv and starts it on port 18802, storing the result in
// g_race_start_rv, then sets g_race_start_returned. The test pauses the start
// inside a hook to race it against an engine stop.
static void *_start_racing_server_thread(void *arg) {
  (void)arg;
  char *err = NULL;
  g_race_srv = chttpsvr_create(CLOG_NULL, &err);
  if (!g_race_srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_register_handler(g_race_srv, CHTTP_GET, "/hello", _hello_handler,
                            NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18802;
  g_race_start_rv = chttpsvr_start(g_race_srv, &cfg);
  atomic_store(&g_race_start_returned, true);
  return NULL;
}

// A start registers in the server list as soon as it holds its engine
// reference, and a forced stop that lands between the two must still see it.
// Otherwise the quiesce skips the server, the reactor goes away under the
// start, and a later destroy hands a stale registration to ceventloop_remove().
// The quiesce waits for every in-flight call on the server. A hook pauses the
// start in that window; a 150 ms nap lets the reaper reach its wait, which
// makes the more interesting interleaving common. A hook armed but never
// entered is released, so a later start doesn't block on it.
TEST_F(engine_stop_fixture, force_stop_racing_a_concurrent_start_is_safe) {
  g_race_srv = CHTTPSVR_NULL;
  g_race_start_rv = ccol_success;
  atomic_store(&g_race_start_returned, false);

  chttpsvr__arm_start_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_racing_server_thread, NULL) != 0) {
    chttpsvr__release_start_race_hook_for_tests();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  chttpsvr__wait_start_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  chttpsvr__release_start_race_hook_for_tests();

  bool race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&g_race_start_returned)) {
      race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L};
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(race_returned);
  if (!race_returned) return;
  _fx_join(tau, start_th, NULL);
  ccol_retval race_rv = g_race_start_rv;
  if (race_rv != ccol_success) {
    if (g_race_srv != CHTTPSVR_NULL) chttpsvr_destroy(g_race_srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)race_rv, (int)ccol_success);
  }

  chttpsvr_engine_wait();

  chttpsvr_destroy(g_race_srv);

  chttpsvr srv3 ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18803", 18803);
  int status2 = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18803/hello", &status2),
             (int)ccol_success);
  REQUIRE_EQ(status2, 200);
  chttpsvr_destroy(srv3);
  chttpsvr_engine_wait();
}

// Per-run state of the start-against-reap races. Each run has its own heap
// copy, freed only after the thread is joined, so a thread abandoned by one
// test can only write into its own copy. returned is set last, and every reader
// polls it (with a bound) before it reads srv and start_rv or joins.
typedef struct {
  uint16_t port;
  chttpsvr srv;
  ccol_retval start_rv;
  _Atomic bool returned;
} _stopping_race_ctx_t;

// Creates ctx->srv and starts it on ctx->port, then sets ctx->returned. The
// start-against-reap tests pause it before its engine_acquire().
static void *_start_stopping_race_server_thread(void *arg) {
  _stopping_race_ctx_t *ctx = (_stopping_race_ctx_t *)arg;
  char *err = NULL;
  ctx->srv = chttpsvr_create(CLOG_NULL, &err);
  if (!ctx->srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_register_handler(ctx->srv, CHTTP_GET, "/hello", _hello_handler,
                            NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = ctx->port;
  ctx->start_rv = chttpsvr_start(ctx->srv, &cfg);
  atomic_store(&ctx->returned, true);
  return NULL;
}

// Opens a connection to port and waits up to 4 s for the server to close it as
// idle, which shows that a sweep runs for that server. Gives 1 when it was
// closed, 0 when not and -1 when it couldn't connect.
static int _idle_connection_closed(uint16_t port) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons(port)};
  inet_pton(AF_INET, "127.0.0.1", &a.sin_addr);
  if (test_connect(fd, (struct sockaddr *)&a, sizeof(a)) != 0) {
    close(fd);
    return -1;
  }
  struct pollfd p = {.fd = fd, .events = POLLIN};
  int closed = 0;
  if (poll(&p, 1, 4000) == 1) {
    char b;
    closed = recv(fd, &b, 1, 0) <= 0;
  }
  close(fd);
  return closed;
}

// Creates ctx->srv and starts it on ctx->port with a 1 s idle limit, then sets
// ctx->returned, for the test of a start during a reap.
static void *_start_idle_limited_server_thread(void *arg) {
  _stopping_race_ctx_t *ctx = (_stopping_race_ctx_t *)arg;
  char *err = NULL;
  ctx->srv = chttpsvr_create(CLOG_NULL, &err);
  if (!ctx->srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = ctx->port;
  cfg.idle_timeout_us = 1000000;
  cfg.max_header_read_duration_us = 1000000;
  ctx->start_rv = chttpsvr_start(ctx->srv, &cfg);
  atomic_store(&ctx->returned, true);
  return NULL;
}

// A start that registers while the previous engine's reap finishes belongs to
// the next engine, whose sweep closes its idle connection. A reap that emptied
// the registry would leave the server in none and the connection open.
TEST_F(engine_stop_fixture, a_start_during_a_reap_is_swept_by_the_next_engine) {
  chttpsvr srv1 = _start_server("18850", 18850);
  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18851;
  chttpsvr_destroy(srv1);
  struct timespec settle = {0, 200000000L};
  nanosleep(&settle, NULL);
  chttpsvr__arm_engine_stopping_race_hook_for_tests();
  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_idle_limited_server_thread, ctx) !=
      0) {
    chttpsvr__release_engine_stopping_race_hook_for_tests();
    free(ctx);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);
  chttpsvr__wait_engine_stopping_race_hook_entered_for_tests();
  chttpsvr_engine_wait();
  chttpsvr__release_engine_stopping_race_hook_for_tests();
  bool returned = false;
  for (int i = 0; i < 100 && !returned; i++) {
    returned = atomic_load(&ctx->returned);
    if (!returned) {
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
  }
  REQUIRE_TRUE(returned);
  int closed =
      ctx->start_rv == ccol_success ? _idle_connection_closed(ctx->port) : -1;
  ccol_retval start_rv = ctx->start_rv;
  if (ctx->srv) chttpsvr_destroy(ctx->srv);
  free(ctx);
  REQUIRE_EQ(start_rv, ccol_success);
  REQUIRE_EQ(closed, 1);
}

// A start that holds its resolve pin must not block on the engine's stopping
// flag, because the reap that claimed this server waits for that pin. Here a
// forced stop is the reap; a hook pauses the start right before
// engine_acquire(), which then sees stopping, returns, and the start retries
// and serves on a new reactor. Without that the two deadlock for good.
TEST_F(engine_stop_fixture,
       start_racing_a_concurrent_forced_reap_retries_and_succeeds) {
  chttpsvr srv1 = _start_server("18820", 18820);

  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18821;
  chttpsvr__arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    chttpsvr__release_engine_stopping_race_hook_for_tests();
    free(ctx);
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  chttpsvr__wait_engine_stopping_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  chttpsvr__release_engine_stopping_race_hook_for_tests();

  bool stopping_race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&ctx->returned)) {
      stopping_race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L};
    nanosleep(&nap, NULL);
  }
  if (!stopping_race_returned) {
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(stopping_race_returned);
    return;
  }
  _fx_join(tau, start_th, NULL);

  chttpsvr_destroy(srv1);

  chttpsvr srv2 = ctx->srv;
  ccol_retval start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_NULL) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18821/hello", &status);

  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// The same race, with the graceful reap that the destroy of the last other
// server starts.
TEST_F(engine_stop_fixture,
       start_racing_a_concurrent_graceful_reap_retries_and_succeeds) {
  chttpsvr srv1 = _start_server("18822", 18822);

  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18823;
  chttpsvr__arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    chttpsvr__release_engine_stopping_race_hook_for_tests();
    free(ctx);
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  chttpsvr__wait_engine_stopping_race_hook_entered_for_tests();

  chttpsvr_destroy(srv1);

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  chttpsvr__release_engine_stopping_race_hook_for_tests();

  bool stopping_race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&ctx->returned)) {
      stopping_race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L};
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(stopping_race_returned);
  if (!stopping_race_returned) return;
  _fx_join(tau, start_th, NULL);

  chttpsvr srv2 = ctx->srv;
  ccol_retval start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_NULL) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18823/hello", &status);

  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// The SIGUSR1 handler: calls chttpsvr_engine_stop(), as an application's
// SIGTERM handler would.
static void _sigusr1_calls_engine_stop(int sig) {
  (void)sig;
  chttpsvr_engine_stop();
}

// Set once _signal_test_thread returned, i.e. no deadlock happened.
static _Atomic bool g_signal_test_done = false;

// Locks the engine mutex and raises SIGUSR1 on itself while it holds it, which
// models a signal on a thread inside chttpsvr_start(), then sets
// g_signal_test_done.
static void *_signal_test_thread(void *arg) {
  (void)arg;
  chttpsvr__test_hold_engine_mutex_and_signal_self(SIGUSR1);
  atomic_store(&g_signal_test_done, true);
  return NULL;
}

// chttpsvr_engine_stop() is async-signal-safe: it only posts a semaphore, and a
// watcher thread does the locking and the thread creation. A thread that holds
// the engine mutex raises SIGUSR1, whose handler calls the stop. Without the
// watcher this deadlocks; the bounded wait turns that into a failure. srv has
// no scoped destructor, because on that failure its destroy would block on the
// same mutex.
TEST_F(engine_stop_fixture, engine_stop_from_signal_handler_does_not_deadlock) {
  chttpsvr srv = _start_server("18804", 18804);

  struct sigaction sa, old_sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _sigusr1_calls_engine_stop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  int sigaction_rv = sigaction(SIGUSR1, &sa, &old_sa);
  if (sigaction_rv != 0) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  REQUIRE_EQ(sigaction_rv, 0);

  atomic_store(&g_signal_test_done, false);
  pthread_t th;
  int pthread_create_rv = pthread_create(&th, NULL, _signal_test_thread, NULL);
  if (pthread_create_rv != 0) {
    sigaction(SIGUSR1, &old_sa, NULL);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  REQUIRE_EQ(pthread_create_rv, 0);
  _fx_track(tau, th);

  bool done = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&g_signal_test_done)) {
      done = true;
      break;
    }
    struct timespec nap = {0, 50000000L};
    nanosleep(&nap, NULL);
  }
  sigaction(SIGUSR1, &old_sa, NULL);
  REQUIRE_TRUE(done);
  if (!done) return;
  _fx_join(tau, th, NULL);

  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18805", 18805);
  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18805/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

// Per-run state of the restart-against-quiesce races, on the heap for the same
// reason as _stopping_race_ctx_t. returned is set last and polled before
// start_rv is read.
typedef struct {
  chttpsvr *srv;
  int port;
  ccol_retval start_rv;
  _Atomic bool returned;
} resolve_racing_start_arg_t;

// Starts *a->srv on a->port, stores the result and sets a->returned. It reads
// srv and port at once, before the main thread can go past its hook wait.
static void *_resolve_racing_start_thread(void *arg) {
  resolve_racing_start_arg_t *a = (resolve_racing_start_arg_t *)arg;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = a->port;
  a->start_rv = chttpsvr_start(*a->srv, &cfg);
  atomic_store(&a->returned, true);
  return NULL;
}

// A start that finds its server being quiesced must release its resolve pin
// before it waits, because the quiesce waits for that pin; otherwise the two
// deadlock across two condition variables. A stopped server is restarted (an
// ordinary restart) while a forced stop quiesces it; a hook pauses the start
// right after it pins. srv has no scoped destructor, because on a deadlock its
// destroy would hang too.
TEST_F(engine_stop_fixture,
       start_does_not_deadlock_against_concurrent_quiesce_pass) {
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18807;
  ccol_retval start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  chttpsvr_stop(srv);

  chttpsvr__arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  restart_arg->srv = &srv;
  restart_arg->port = 18807;
  pthread_t restart_th;
  if (pthread_create(&restart_th, NULL, _resolve_racing_start_thread,
                     restart_arg) != 0) {
    chttpsvr__release_start_resolve_race_hook_for_tests();
    free(restart_arg);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, restart_th);

  chttpsvr__wait_start_resolve_race_hook_entered_for_tests();

  chttpsvr__arm_quiesce_claimed_signal_for_tests();
  chttpsvr_engine_stop();

  bool quiescing_claimed =
      chttpsvr__wait_quiesce_claimed_signal_entered_for_tests();

  chttpsvr__release_start_resolve_race_hook_for_tests();

  bool returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&restart_arg->returned)) {
      returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L};
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(returned);
  if (!returned) return;
  _fx_join(tau, restart_th, NULL);

  ccol_retval restart_rv = restart_arg->start_rv;
  free(restart_arg);
  if (restart_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  }

  int status2 = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18807/hello", &status2);

  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();
  REQUIRE_TRUE(quiescing_claimed);

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status2, 200);
}

// The server that _start_uaf_race_destroy_thread destroys, and whether that
// destroy returned.
static chttpsvr g_start_uaf_race_srv = CHTTPSVR_NULL;
static _Atomic bool g_start_uaf_race_destroy_returned = false;

// Destroys g_start_uaf_race_srv and sets g_start_uaf_race_destroy_returned, so
// the test can see whether the destroy waits.
static void *_start_uaf_race_destroy_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_start_uaf_race_srv);
  atomic_store(&g_start_uaf_race_destroy_returned, true);
  return NULL;
}

// A start that backs off from a quiesce must register as a waiter before it
// releases its pin. Otherwise the quiesce finishes, a concurrent destroy frees
// the server, and the start locks freed memory. A hook pauses the start after
// the pin release; the destroy must stay blocked until the start is released.
TEST_F(engine_stop_fixture,
       start_racing_engine_stop_and_destroy_does_not_free_raw_too_early) {
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18816;
  ccol_retval start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }
  chttpsvr_stop(srv);
  g_start_uaf_race_srv = srv;

  chttpsvr__arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    chttpsvr_destroy(srv);
    g_start_uaf_race_srv = CHTTPSVR_NULL;
    REQUIRE_TRUE(false);
  }
  restart_arg->srv = &srv;
  restart_arg->port = 18816;
  pthread_t restart_th;
  bool restart_th_created =
      pthread_create(&restart_th, NULL, _resolve_racing_start_thread,
                     restart_arg) == 0;
  if (restart_th_created) {
    _fx_track(tau, restart_th);
  } else {
    free(restart_arg);
  }
  if (restart_th_created)
    chttpsvr__wait_start_resolve_race_hook_entered_for_tests();

  chttpsvr__arm_start_quiescing_unpinned_race_hook_for_tests();

  bool quiescing_claimed = false;
  if (restart_th_created) {
    chttpsvr__arm_quiesce_claimed_signal_for_tests();
    chttpsvr_engine_stop();
    quiescing_claimed =
        chttpsvr__wait_quiesce_claimed_signal_entered_for_tests();
  }

  chttpsvr__release_start_resolve_race_hook_for_tests();

  bool hook_entered = false;
  if (restart_th_created && quiescing_claimed) {
    hook_entered =
        chttpsvr__wait_start_quiescing_unpinned_race_hook_entered_for_tests();
  }
  bool paused_at_vulnerable_window = !restart_th_created || hook_entered;

  bool destroy_th_created = false;
  pthread_t destroy_th;
  if (paused_at_vulnerable_window) {
    atomic_store(&g_start_uaf_race_destroy_returned, false);
    destroy_th_created =
        pthread_create(&destroy_th, NULL, _start_uaf_race_destroy_thread,
                       NULL) == 0;
    if (destroy_th_created) _fx_track(tau, destroy_th);
  }

  bool destroy_returned_too_early = false;
  if (destroy_th_created) {
    for (int i = 0; i < 6; i++) {
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
      if (atomic_load(&g_start_uaf_race_destroy_returned)) {
        destroy_returned_too_early = true;
        break;
      }
    }
  }

  chttpsvr__release_start_quiescing_unpinned_race_hook_for_tests();

  bool restart_returned = false;
  ccol_retval restart_rv = ccol_success;
  if (restart_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&restart_arg->returned)) {
        restart_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
    if (restart_returned) {
      _fx_join(tau, restart_th, NULL);
      restart_rv = restart_arg->start_rv;
      free(restart_arg);
    }
  }

  bool destroy_returned = false;
  if (destroy_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_start_uaf_race_destroy_returned)) {
        destroy_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
    if (destroy_returned) _fx_join(tau, destroy_th, NULL);
  } else if (restart_returned) {
    chttpsvr_destroy(srv);
  }

  bool engine_refs_should_settle = (!destroy_th_created || destroy_returned) &&
                                   (!restart_th_created || restart_returned);
  if (engine_refs_should_settle) chttpsvr_engine_wait();

  REQUIRE_TRUE(restart_th_created);
  REQUIRE_TRUE(quiescing_claimed);
  REQUIRE_TRUE(hook_entered);
  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(restart_returned);
  REQUIRE_TRUE(destroy_returned);
  if (!restart_returned || !destroy_returned) return;

  REQUIRE_FALSE(destroy_returned_too_early);
  REQUIRE_EQ((int)restart_rv, (int)ccol_invalid_args);
}

// The server of the stop-against-start race, the result of its start, and a
// flag set once that start returned, which the test polls before it reads the
// result.
static chttpsvr g_stop_race_srv = CHTTPSVR_NULL;
static ccol_retval g_stop_race_start_rv = ccol_success;
static _Atomic bool g_stop_race_start_returned = false;
// Set once chttpsvr_stop() returned; polled before the test destroys the
// server, so a hung stop fails the test instead of hanging it.
static _Atomic bool g_stop_race_stop_returned = false;

// Stops g_stop_race_srv on a thread of its own, so the test can pause the stop
// in a hook, then sets g_stop_race_stop_returned.
static void *_stop_racing_server_thread(void *arg) {
  (void)arg;
  chttpsvr_stop(g_stop_race_srv);
  atomic_store(&g_stop_race_stop_returned, true);
  return NULL;
}

// Starts g_stop_race_srv on port 18808 while a stop is paused, and sets
// g_stop_race_start_returned.
static void *_start_racing_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18808;
  g_stop_race_start_rv = chttpsvr_start(g_stop_race_srv, &cfg);
  atomic_store(&g_stop_race_start_returned, true);
  return NULL;
}

// A stop leaves RUNNING before its listener is closed. A concurrent start must
// wait in CHTTPSVR_LC_STOPPING instead of binding a new listener on the same
// port while the old one is still open (a spurious EADDRINUSE). A hook pauses
// the stop in that window.
TEST_F(engine_stop_fixture,
       stop_racing_a_concurrent_start_does_not_bind_over_open_listener) {
  g_stop_race_srv = _start_server("18808", 18808);

  chttpsvr__arm_stop_race_hook_for_tests();
  atomic_store(&g_stop_race_stop_returned, false);

  pthread_t stop_th;
  bool stop_th_created =
      pthread_create(&stop_th, NULL, _stop_racing_server_thread, NULL) == 0;
  if (stop_th_created) _fx_track(tau, stop_th);

  if (stop_th_created) chttpsvr__wait_stop_race_hook_entered_for_tests();

  g_stop_race_start_rv = ccol_success;
  atomic_store(&g_stop_race_start_returned, false);
  chttpsvr__arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      stop_th_created &&
      pthread_create(&start_th, NULL, _start_racing_stop_thread, NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    chttpsvr__wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_stop_race_start_returned);
  }

  chttpsvr__release_stop_race_hook_for_tests();
  bool stop_returned = !stop_th_created;
  if (stop_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_stop_race_stop_returned)) {
        stop_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
    if (stop_returned) _fx_join(tau, stop_th, NULL);
  }

  bool start_returned = !start_th_created;
  if (start_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_stop_race_start_returned)) {
        start_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
    if (start_returned) _fx_join(tau, start_th, NULL);
  }

  int status = 0;
  ccol_retval get_rv = ccol_unexpected_failure;
  if (start_returned && g_stop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18808/hello", &status);

  bool stop_teardown_safe = !stop_th_created || stop_returned;
  if (stop_teardown_safe) {
    chttpsvr_destroy(g_stop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(stop_th_created);
  REQUIRE_TRUE(stop_returned);
  if (!stop_returned) return;
  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned) return;
  REQUIRE_EQ((int)g_stop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// The server of the quiesce-stop race, and the result of its start.
static chttpsvr g_qstop_race_srv = CHTTPSVR_NULL;
static ccol_retval g_qstop_race_start_rv = ccol_success;
// Set once the start returned; polled before the join and the read.
static _Atomic bool g_qstop_race_start_returned = false;

// Starts g_qstop_race_srv on port 18817 while a quiesce pass is paused inside
// its stop, and sets g_qstop_race_start_returned.
static void *_start_racing_quiesce_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18817;
  g_qstop_race_start_rv = chttpsvr_start(g_qstop_race_srv, &cfg);
  atomic_store(&g_qstop_race_start_returned, true);
  return NULL;
}

// The same STOPPING wait, driven by the quiesce pass of chttpsvr_engine_stop()
// instead of a plain stop: the start sees STOPPING with the quiesce already
// claimed. The test proves the interleaving is safe and the engine comes back.
TEST_F(
    engine_stop_fixture,
    stop_race_hook_driven_by_quiesce_pass_racing_a_concurrent_start_is_safe) {
  g_qstop_race_srv = _start_server("18817", 18817);

  chttpsvr__arm_stop_race_hook_for_tests();

  chttpsvr_engine_stop();

  chttpsvr__wait_stop_race_hook_entered_for_tests();

  g_qstop_race_start_rv = ccol_success;
  atomic_store(&g_qstop_race_start_returned, false);
  chttpsvr__arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      pthread_create(&start_th, NULL, _start_racing_quiesce_stop_thread,
                     NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    chttpsvr__wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_qstop_race_start_returned);
  }

  chttpsvr__release_stop_race_hook_for_tests();

  bool start_returned = !start_th_created;
  if (start_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_qstop_race_start_returned)) {
        start_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L};
      nanosleep(&nap, NULL);
    }
    if (start_returned) _fx_join(tau, start_th, NULL);
  }

  int status = 0;
  ccol_retval get_rv = ccol_unexpected_failure;
  if (start_returned && g_qstop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18817/hello", &status);

  if (start_returned) {
    chttpsvr_destroy(g_qstop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned) return;
  REQUIRE_EQ((int)g_qstop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// The server destroyed while the reaper holds a pin on it, and whether that
// destroy returned.
static chttpsvr g_reaper_race_srv = CHTTPSVR_NULL;
static _Atomic bool g_reaper_race_destroy_returned = false;

// Destroys g_reaper_race_srv and sets g_reaper_race_destroy_returned, so the
// test can see that the destroy waits for the reaper.
static void *_destroy_racing_reaper_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_reaper_race_srv);
  atomic_store(&g_reaper_race_destroy_returned, true);
  return NULL;
}

// The reaper pins a server it read from the registry before it quiesces it.
// A concurrent destroy of that server must wait for the pin, or the reaper
// touches freed memory. A hook pauses the reaper right after it pins.
TEST_F(engine_stop_fixture,
       destroy_racing_the_reaper_does_not_free_srv_out_from_under_it) {
  g_reaper_race_srv = _start_server("18809", 18809);

  chttpsvr__arm_reaper_race_hook_for_tests();

  chttpsvr_engine_stop();
  chttpsvr__wait_reaper_race_hook_entered_for_tests();

  atomic_store(&g_reaper_race_destroy_returned, false);
  pthread_t destroy_th;
  bool destroy_th_created =
      pthread_create(&destroy_th, NULL, _destroy_racing_reaper_thread, NULL) ==
      0;
  if (destroy_th_created) _fx_track(tau, destroy_th);

  bool still_blocked = false;
  if (destroy_th_created) {
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    still_blocked = !atomic_load(&g_reaper_race_destroy_returned);
  }

  chttpsvr__release_reaper_race_hook_for_tests();
  if (destroy_th_created) _fx_join(tau, destroy_th, NULL);

  chttpsvr_engine_wait();

  int status = 0;
  ccol_retval get_rv = ccol_unexpected_failure;
  chttpsvr srv2 = CHTTPSVR_NULL;
  if (destroy_th_created) {
    srv2 = _start_server("18809", 18809);
    get_rv = _get("http://127.0.0.1:18809/hello", &status);
  }
  if (srv2 != CHTTPSVR_NULL) chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

// The reactor handle is cleared only at the end of a reap, and stopping is set
// at its start. A second stop during the teardown (two SIGTERMs) must see
// stopping and do nothing; a second reaper would destroy the same reactor
// twice, which is fatal.
TEST_F(engine_stop_fixture,
       second_engine_stop_call_while_first_still_tearing_down_is_safe) {
  (void)tau;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18810", 18810);

  chttpsvr__arm_reaper_race_hook_for_tests();

  chttpsvr_engine_stop();
  chttpsvr__wait_reaper_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  chttpsvr__release_reaper_race_hook_for_tests();
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  chttpsvr srv2 ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("18810", 18810);
  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18810/hello", &status);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

#if CCOL_FORK_SAFETY_REQUIRED
// A fork lands while a parent reaper is quiescing a server, which the child
// has no copy of. The child must reset quiesce_state and the waiters, or a
// destroy there waits for a broadcast that never comes and the quiesce loop
// spins on the server. A hook pauses the reaper before the fork.
TEST_F(engine_stop_fixture, fork_mid_quiesce_teardown_does_not_hang_child) {
  (void)tau;
  chttpsvr srv = _start_server("18811", 18811);

  chttpsvr__arm_quiesce_teardown_race_hook_for_tests();

  chttpsvr_engine_stop();
  chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests();

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    char ok = 0;
    if (chttpsvr__servers_bundler_count_for_tests() == 0) {
      chttpsvr_destroy(srv);
      ok = 1;
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  chttpsvr__release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

// The same fork leaves the engine's stopping flag set. The child must reset it,
// or a fresh server's start in the child waits on it for ever.
TEST_F(engine_stop_fixture,
       fork_mid_quiesce_teardown_does_not_hang_fresh_start_in_child) {
  (void)tau;
  chttpsvr srv = _start_server("18812", 18812);

  chttpsvr__arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests();

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(30);
    chttpsvr fresh = chttpsvr_create(CLOG_NULL, NULL);
    char ok = 0;
    if (fresh != CHTTPSVR_NULL) {
      chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18813;
      chttpsvr_start(fresh, &cfg);
      ok = 1;
      chttpsvr_destroy(fresh);
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);

  chttpsvr__release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

// Sends GET /hello to 127.0.0.1:port and tells whether a 200 came back within
// timeout_ms; the fork tests use it in the child.
static bool _child_get_hello(uint16_t port, int timeout_ms) {
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return false;
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  bool ok = false;
  const char req[] = "GET /hello HTTP/1.1\r\nHost: h\r\n\r\n";
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) == 0 &&
      write(fd, req, sizeof(req) - 1) == (ssize_t)(sizeof(req) - 1)) {
    struct pollfd p = {.fd = fd, .events = POLLIN};
    char buf[64] = {0};
    if (poll(&p, 1, timeout_ms) == 1 && read(fd, buf, sizeof(buf) - 1) > 12)
      ok = strncmp(buf, "HTTP/1.1 200", 12) == 0;
  }
  close(fd);
  return ok;
}

// A child forked while the parent's reaper tears the reactor down inherits a
// reactor with no poller. Its first server must get a reactor of its own;
// otherwise the listener is never polled and the request gets no answer.
TEST_F(engine_stop_fixture, a_child_forked_during_a_teardown_serves_requests) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  chttpsvr srv = _start_server("18850", 18850);
  chttpsvr__arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests();

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(60);
    char ok = 0;
    chttpsvr fresh = chttpsvr_create(CLOG_NULL, NULL);
    if (fresh != CHTTPSVR_NULL) {
      chttpsvr_register_handler(fresh, CHTTP_GET, "/hello", _hello_handler,
                                NULL);
      chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18851;
      if (chttpsvr_start(fresh, &cfg) == ccol_success &&
          chttpsvr__engine_reactor_pid_for_tests() == getpid() &&
          _child_get_hello(18851, 20000))
        ok = 1;
      chttpsvr_destroy(fresh);
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);
  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  chttpsvr__release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}
#endif

// Without the stop watcher chttpsvr_engine_stop() does nothing, so a start
// whose watcher thread can't be created fails, and a later start brings the
// watcher up. A fork child has no watcher yet, which lets the test reach its
// creation.
TEST_F(engine_stop_fixture, a_start_fails_when_the_stop_watcher_cannot_start) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(60);
    char out[2] = {0, 0};
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv != CHTTPSVR_NULL) {
      chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
      chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18852;
      chttpsvr__force_watcher_thread_create_fail_for_tests(true);
      ccol_retval first = chttpsvr_start(srv, &cfg);
      chttpsvr__force_watcher_thread_create_fail_for_tests(false);
      out[0] = first == ccol_unexpected_failure;
      if (first == ccol_success) chttpsvr_stop(srv);
      if (chttpsvr_start(srv, &cfg) == ccol_success) {
        chttpsvr_engine_stop();
        chttpsvr_engine_wait();
        out[1] = 1;
      }
      chttpsvr_destroy(srv);
    }
    ssize_t written = write(pipefd[1], out, sizeof(out));
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);
  char out[2] = {0, 0};
  ssize_t n = read(pipefd[0], out, sizeof(out));
  close(pipefd[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)2);
  REQUIRE_EQ(out[0], 1);
  REQUIRE_EQ(out[1], 1);
}

// Idle timeouts and max_connections depend on the idle sweep, so a start whose
// sweep thread can't be created fails and leaves the server stopped. Run in a
// fork child, which has no sweep yet.
TEST_F(engine_stop_fixture, a_start_fails_when_the_idle_sweep_cannot_start) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(60);
    char out[3] = {0, 0, 0};
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv != CHTTPSVR_NULL) {
      chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
      chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 0;
      chttpsvr__force_idle_sweep_thread_create_fail_for_tests(true);
      ccol_retval first = chttpsvr_start(srv, &cfg);
      chttpsvr__force_idle_sweep_thread_create_fail_for_tests(false);
      bool running = true;
      chttpsvr_is_running(srv, &running);
      out[0] = first == ccol_unexpected_failure && !running;
      if (first == ccol_success) chttpsvr_stop(srv);
      uint16_t port = 0;
      if (chttpsvr_start(srv, &cfg) == ccol_success &&
          chttpsvr_bound_port(srv, &port) == ccol_success) {
        out[1] = 1;
        char url[64];
        snprintf(url, sizeof(url), "http://127.0.0.1:%u/hello", (unsigned)port);
        int status = 0;
        out[2] = _get(url, &status) == ccol_success && status == 200;
      }
      chttpsvr_destroy(srv);
      chttpsvr_engine_wait();
    }
    ssize_t written = write(pipefd[1], out, sizeof(out));
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);
  char out[3] = {0, 0, 0};
  ssize_t n = read(pipefd[0], out, sizeof(out));
  close(pipefd[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)3);
  REQUIRE_EQ(out[0], 1);
  REQUIRE_EQ(out[1], 1);
  REQUIRE_EQ(out[2], 1);
}

// When the reaper thread can't be created, the last destroy runs it inline. The
// reaper must not find the server whose own quiesce runs on the same thread, or
// it waits for itself. The child reports through a pipe, and an alarm ends a
// child that hangs.
TEST_F(engine_stop_fixture, a_destroy_whose_reaper_runs_inline_returns) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  TEST_SKIP_FORK_WITHOUT_FORK_SAFETY();
  int pipefd[2];
  REQUIRE_EQ(pipe(pipefd), 0);
  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    REQUIRE_TRUE(false);
  }
  if (pid == 0) {
    close(pipefd[0]);
    int dn = open("/dev/null", O_WRONLY);
    if (dn >= 0) {
      dup2(dn, STDOUT_FILENO);
      dup2(dn, STDERR_FILENO);
      close(dn);
    }
    alarm(60);
    char out[2] = {0, 0};
    chttpsvr srv = _start_server("18870", 18870);
    int status = 0;
    out[0] = _get("http://127.0.0.1:18870/hello", &status) == ccol_success &&
             status == 200;
    chttpsvr__force_reaper_thread_create_fail_for_tests(true);
    chttpsvr_destroy(srv);
    chttpsvr__force_reaper_thread_create_fail_for_tests(false);
    chttpsvr_engine_wait();
    out[1] = 1;
    ssize_t written = write(pipefd[1], out, sizeof(out));
    (void)written;
    close(pipefd[1]);
    test_child_exit(0);
  }
  close(pipefd[1]);
  char out[2] = {0, 0};
  ssize_t n = read(pipefd[0], out, sizeof(out));
  close(pipefd[0]);
  int status = 0;
  pid_t waited = waitpid(pid, &status, 0);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)2);
  REQUIRE_EQ(out[0], 1);
  REQUIRE_EQ(out[1], 1);
}

// The reaper must end up joined even when the application calls neither
// chttpsvr_engine_wait() nor another start, or its stack stays mapped. Nothing
// but the hook runs after the destroy, because those calls would join it.
TEST_F(engine_stop_fixture,
       destroying_the_last_server_leaves_no_unjoined_reaper_thread) {
  (void)tau;
  chttpsvr srv = _start_server("18824", 18824);
  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18824/hello", &status);

  chttpsvr_destroy(srv);

  bool joined = false;
  for (int i = 0; i < 6000; i++) {
    if (!chttpsvr__engine_reaper_unjoined_for_tests()) {
      joined = true;
      break;
    }
    struct timespec ts = {0, 5 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(joined);
}

// A hook holds the call that spawned the reaper for 3 s, so the reaper asks to
// be joined before it is recorded. It must still be joined.
TEST_F(engine_stop_fixture,
       a_reaper_that_ends_before_it_is_recorded_is_still_joined) {
  (void)tau;
  chttpsvr srv = _start_server("18841", 18841);
  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18841/hello", &status);

  chttpsvr__set_spawn_reaper_delay_for_tests(3000);
  chttpsvr_destroy(srv);
  chttpsvr__set_spawn_reaper_delay_for_tests(0);

  bool joined = false;
  for (int i = 0; i < 1000; i++) {
    if (!chttpsvr__engine_reaper_unjoined_for_tests()) {
      joined = true;
      break;
    }
    struct timespec ts = {0, 5 * 1000 * 1000};
    nanosleep(&ts, NULL);
  }

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_TRUE(joined);
}

// The reaper hand-off keeps the destroy asynchronous: a new server binds the
// same port at once with no engine wait between.
TEST_F(engine_stop_fixture,
       destroy_without_an_engine_wait_still_allows_an_immediate_rebind) {
  (void)tau;
  chttpsvr srv = _start_server("18825", 18825);
  int status = 0;
  ccol_retval rv_a = _get("http://127.0.0.1:18825/hello", &status);
  int status_a = status;
  chttpsvr_destroy(srv);

  chttpsvr srv2 = _start_server("18825", 18825);
  status = 0;
  ccol_retval rv_b = _get("http://127.0.0.1:18825/hello", &status);
  int status_b = status;
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)rv_a, (int)ccol_success);
  REQUIRE_EQ(status_a, 200);
  REQUIRE_EQ((int)rv_b, (int)ccol_success);
  REQUIRE_EQ(status_b, 200);
}

// Answers with the length of the body it got; the buffered route of the stop
// test.
static void _es_len_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t n = 0;
  chttpsvr_req_body(req, &n);
  chttpsvr_resp_printf(resp, "%zu", n);
}

// Reads the body until it fails or ends, then answers "drained"; the streaming
// route of the stop test.
static void _es_drain_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  char buf[256];
  while (chttpsvr_req_read(req, buf, sizeof(buf)) > 0) {
  }
  chttpsvr_resp_write_str(resp, "drained");
}

// Connects to port, sends req and body padding bytes, and gives the socket (5 s
// read timeout), or -1. The stop test uses it to leave connections waiting.
static int _es_open(uint16_t port, const char *req, size_t body) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  struct timeval tv = {5, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
      write(fd, req, strlen(req)) != (ssize_t)strlen(req)) {
    close(fd);
    return -1;
  }
  char pad[16];
  memset(pad, 'p', sizeof(pad));
  if (body && write(fd, pad, body) != (ssize_t)body) {
    close(fd);
    return -1;
  }
  return fd;
}

// Polls a server counter until it reaches want, for 5 s, so the stop test knows
// its connections are in place. what picks the counter: 0 parked bodies, 1
// memory waiters, 2 active workers, 3 queued streaming requests.
static bool _es_wait(chttpsvr srv, int what, size_t want) {
  for (int i = 0; i < 5000; i++) {
    size_t v = what == 0   ? chttpsvr__parked_count_for_tests(srv, 1)
               : what == 1 ? chttpsvr__mem_waiting_for_tests(srv)
               : what == 2 ? chttpsvr__pool_active_for_tests(srv, true)
                           : chttpsvr__stream_queue_len_for_tests(srv);
    if (v >= want) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

// Reads fd until the end of the stream, keeping the first bytes in first (cap
// bytes with the NUL), so the stop test sees that each waiting connection
// ended. Gives true on an end or a reset, false on a timeout.
static bool _es_eof(int fd, char *first, size_t cap) {
  size_t got = 0;
  for (;;) {
    char tmp[4096];
    ssize_t r = read(fd, tmp, sizeof(tmp));
    if (r == 0) break;
    if (r < 0) return errno == ECONNRESET;
    for (ssize_t i = 0; i < r && got + 1 < cap; i++) first[got++] = tmp[i];
  }
  if (cap) first[got < cap ? got : cap - 1] = '\0';
  return true;
}

// Stops the engine and waits for it on a thread of its own, so the stop test
// can watch the connections end while the stop runs.
static void *_es_stop_and_wait(void *arg) {
  (void)arg;
  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  return NULL;
}

// A forced stop must end connections that wait with no thread: a parked body, a
// memory waiter and a queued streaming request.
TEST_F(engine_stop_fixture, force_stop_ends_parked_waiting_and_queued) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(300, 2000);
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _es_len_handler, NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/drain",
                                      _es_drain_stream_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18830;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  cfg.max_partial_body_memory = 20000;
  cfg.min_transfer_rate_bps = CHTTPSVR_NO_RATE_FLOOR;
  cfg.stream_read_timeout_us = 0;
  cfg.max_body_read_duration_us = 0;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  int a = _es_open(18830,
                   "POST /len HTTP/1.1\r\nHost: h\r\n"
                   "Content-Length: 90000\r\n\r\n",
                   10);
  bool ok = a >= 0 && _es_wait(srv, 0, 1);
  int b = _es_open(18830,
                   "POST /len HTTP/1.1\r\nHost: h\r\n"
                   "Content-Length: 50000\r\n\r\n",
                   0);
  ok = ok && b >= 0 && _es_wait(srv, 1, 1);
  int d = _es_open(18830,
                   "POST /drain HTTP/1.1\r\nHost: h\r\n"
                   "Content-Length: 1000\r\n\r\n",
                   10);
  ok = ok && d >= 0 && _es_wait(srv, 2, 1);
  int e = _es_open(18830,
                   "POST /drain HTTP/1.1\r\nHost: h\r\n"
                   "Content-Length: 0\r\n\r\n",
                   0);
  ok = ok && e >= 0 && _es_wait(srv, 3, 1);

  pthread_t th;
  bool stopped = false;
  if (pthread_create(&th, NULL, _es_stop_and_wait, NULL) == 0) {
    _fx_track(tau, th);
    stopped = _fx_join(tau, th, NULL);
  }
  char first[256] = {0};
  bool e_end = e >= 0 && _es_eof(e, first, sizeof(first));
  bool e_503 = strncmp(first, "HTTP/1.1 503", 12) == 0;
  bool a_end = a >= 0 && _es_eof(a, NULL, 0);
  bool b_end = b >= 0 && _es_eof(b, NULL, 0);
  bool d_end = d >= 0 && _es_eof(d, NULL, 0);
  int fds[4] = {a, b, d, e};
  for (int i = 0; i < 4; i++)
    if (fds[i] >= 0) close(fds[i]);
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();
  REQUIRE_TRUE(ok);
  REQUIRE_TRUE(stopped);
  REQUIRE_TRUE(e_end);
  REQUIRE_TRUE(e_503);
  REQUIRE_TRUE(a_end);
  REQUIRE_TRUE(b_end);
  REQUIRE_TRUE(d_end);
}

// Counts the occurrences of needle in the file at path (read up to 64 KiB), or
// gives -1; the logger tests count engine log lines with it.
static int _es_count_in_file(const char *path, const char *needle) {
  FILE *f = fopen(path, "r");
  if (!f) return -1;
  char text[65536];
  size_t n = fread(text, 1, sizeof(text) - 1, f);
  fclose(f);
  text[n] = '\0';
  int count = 0;
  for (const char *p = text; (p = strstr(p, needle)) != NULL;
       p += strlen(needle))
    count++;
  return count;
}

// Waits up to bound_ms until no engine runs and no reaper is left, then joins
// through chttpsvr_engine_wait(), which returns at once by then. Gives false
// when an engine is still up, where the unbounded wait would hang the test.
static bool _engine_settle_bounded(int bound_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t end = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + bound_ms;
  for (;;) {
    bool idle = chttpsvr__engine_reactor_pid_for_tests() == 0 &&
                !chttpsvr__engine_reaper_unjoined_for_tests();
    if (idle) {
      chttpsvr_engine_wait();
      return true;
    }
    clock_gettime(CLOCK_MONOTONIC, &ts);
    if ((int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 >= end) return false;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
}

// The installed logger serves every later engine: two full cycles each log
// their creation and teardown through it, though the caller closed its own
// handle. A teardown that closed it would send the second cycle to the silent
// fallback. The test first settles an engine an earlier test left behind.
TEST_F(engine_stop_fixture, engine_logger_serves_every_later_engine) {
  (void)tau;
  REQUIRE_TRUE(_engine_settle_bounded(10000));
  char path[] = "chttpsvr_engine_logger_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_TRUE(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  bool installed = lg && chttpsvr_set_engine_logger(lg) == ccol_success;
  if (lg) clog_close(lg);

  int status_a = 0, status_b = 0;
  if (installed) {
    chttpsvr srv = _start_server("18840", 18840);
    (void)_get("http://127.0.0.1:18840/hello", &status_a);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    srv = _start_server("18840", 18840);
    (void)_get("http://127.0.0.1:18840/hello", &status_b);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  int created = _es_count_in_file(
      path, "New http server reactor engine has been created");
  int destroyed = _es_count_in_file(
      path, "The http server reactor engine has been destroyed");
  unlink(path);

  REQUIRE_TRUE(installed);
  REQUIRE_EQ(status_a, 200);
  REQUIRE_EQ(status_b, 200);
  REQUIRE_EQ(created, 2);
  REQUIRE_EQ(destroyed, 2);
}

// A closed handle is ccol_invalid_args and the engine keeps its logger; a
// setter that derived from it unchecked would end the process.
TEST_F(engine_stop_fixture, a_closed_engine_logger_is_refused) {
  (void)tau;
  char path[] = "chttpsvr_closed_engine_logger_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_TRUE(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  bool installed = lg && chttpsvr_set_engine_logger(lg) == ccol_success;
  if (lg) clog_close(lg);

  clog closed = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (closed) clog_close(closed);
  ccol_retval closed_rv =
      closed ? chttpsvr_set_engine_logger(closed) : ccol_unexpected_failure;

  int status = 0;
  if (installed) {
    chttpsvr srv = _start_server("18840", 18840);
    (void)_get("http://127.0.0.1:18840/hello", &status);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  ccol_retval release_rv = chttpsvr_set_engine_logger(CLOG_NULL);
  int created = _es_count_in_file(
      path, "New http server reactor engine has been created");
  unlink(path);
  close(fd);

  REQUIRE_TRUE(installed);
  REQUIRE_EQ((int)closed_rv, (int)ccol_invalid_args);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ(created, 1);
  REQUIRE_EQ((int)release_rv, (int)ccol_success);
}

// Gives CLOCK_MONOTONIC in milliseconds, for the deadlines of these tests.
static int64_t _engine_test_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

// The teardown must wake the idle sweep thread instead of waiting out its 1 s
// tick. The test counts how each sweep thread stopped instead of timing it,
// because valgrind can be slow anyway; three rounds, each bounded by 10 s.
TEST(engine_teardown, the_last_destroy_and_engine_wait_return_quickly) {
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_teardown_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);

  REQUIRE_TRUE(_engine_settle_bounded(10000));
  size_t woken0 = 0, timed_out0 = 0;
  chttpsvr__idle_sweep_exits_for_tests(&woken0, &timed_out0);
  bool all_started = true;
  int64_t slowest_round_ms = 0;
  for (int round = 0; round < 3; round++) {
    int64_t round_start = _engine_test_mono_ms();
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) {
      all_started = false;
      break;
    }
    chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
    cfg.host = host_buf;
    if (chttpsvr_start(srv, &cfg) != ccol_success) {
      chttpsvr_destroy(srv);
      all_started = false;
      break;
    }
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    int64_t took = _engine_test_mono_ms() - round_start;
    if (took > slowest_round_ms) slowest_round_ms = took;
  }
  unlink(sock_path);
  size_t woken1 = 0, timed_out1 = 0;
  chttpsvr__idle_sweep_exits_for_tests(&woken1, &timed_out1);

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(timed_out1 - timed_out0, (size_t)0);
  REQUIRE_EQ(woken1 - woken0, (size_t)3);
  REQUIRE_LT(slowest_round_ms, (int64_t)10000);
}

// Whether _slow_300ms_handler started and finished.
static _Atomic bool g_s300_entered = false;
static _Atomic bool g_s300_done = false;

// Sleeps 300 ms before it answers, so a destroy has a request to drain; it
// records its start and end in g_s300_entered and g_s300_done.
static void _slow_300ms_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                void *ctx) {
  (void)req;
  (void)ctx;
  atomic_store(&g_s300_entered, true);
  struct timespec nap = {0, 300000000L};
  nanosleep(&nap, NULL);
  atomic_store(&g_s300_done, true);
  chttpsvr_resp_write_str(resp, "slow");
}

// Destroys the server *arg, so a test can race the destroy against a stop.
static void *_destroy_handle_thread(void *arg) {
  chttpsvr *h = (chttpsvr *)arg;
  chttpsvr_destroy(*h);
  return NULL;
}

// Connects to port with a 3 s read timeout, for the tests that need a raw
// connection. Gives the socket, or -1.
static int _es_connect(uint16_t port) {
  struct sockaddr_in sa;
  memset(&sa, 0, sizeof(sa));
  sa.sin_family = AF_INET;
  sa.sin_port = htons(port);
  inet_pton(AF_INET, "127.0.0.1", &sa.sin_addr);
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  if (test_connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
    close(fd);
    return -1;
  }
  struct timeval tv = {3, 0};
  setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
  return fd;
}

// Sends one keep-alive GET /hello on fd and reads until the reply's body
// arrived; tells whether it was a 200. Leaves an idle keep-alive connection.
static bool _es_keepalive_hello(int fd) {
  const char *req = "GET /hello HTTP/1.1\r\nHost: 127.0.0.1\r\n\r\n";
  if (write(fd, req, strlen(req)) != (ssize_t)strlen(req)) return false;
  char buf[1024];
  size_t total = 0;
  while (total < sizeof(buf) - 1) {
    ssize_t n = read(fd, buf + total, sizeof(buf) - 1 - total);
    if (n <= 0) return false;
    total += (size_t)n;
    buf[total] = '\0';
    if (strstr(buf, "Hello, engine-stop!")) return strstr(buf, "200") != NULL;
  }
  return false;
}

// Tells whether the end of the stream on fd arrives within timeout_ms, so a
// test can see that the server closed an idle connection.
static bool _es_eof_within(int fd, int timeout_ms) {
  int64_t deadline = _engine_test_mono_ms() + timeout_ms;
  for (;;) {
    int64_t left = deadline - _engine_test_mono_ms();
    if (left <= 0) return false;
    struct pollfd p = {.fd = fd, .events = POLLIN};
    if (poll(&p, 1, (int)left) <= 0) return false;
    char tmp[256];
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n == 0) return true;
    if (n < 0 && errno != EINTR && errno != EAGAIN) return true;
  }
}

// The stop closes the idle connections of every server at once, while the reap
// still drains a request of the first. Closing them only at each server's turn
// would keep the second server's connection open until the first drain ends.
TEST_F(engine_stop_fixture,
       an_engine_stop_closes_every_idle_connection_at_once) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(8000, 2000);
  char *err = NULL;
  chttpsvr a = chttpsvr_create(CLOG_NULL, &err);
  bool created = a != CHTTPSVR_NULL;
  if (!created) chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(created);
  chttpsvr_register_handler(a, CHTTP_GET, "/slow", _slow_handler, NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18871;
  ccol_retval start_rv = chttpsvr_start(a, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(a);
    chttpsvr_engine_wait();
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ((int)start_rv, (int)ccol_success);
  chttpsvr b = _start_server("18872", 18872);

  int fd = _es_connect(18872);
  bool idle_ok = fd >= 0 && _es_keepalive_hello(fd);

  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = false;
  g_slow_entered = false;
  pthread_mutex_unlock(&g_slow_mtx);
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  pthread_t th;
  bool req_started = false;
  if (ctx) {
    ctx->url = "http://127.0.0.1:18871/slow";
    req_started = pthread_create(&th, NULL, _async_get_thread, ctx) == 0;
    if (req_started) _fx_track(tau, th);
  }
  bool entered = false;
  for (int i = 0; req_started && i < 10000 && !entered; i++) {
    pthread_mutex_lock(&g_slow_mtx);
    entered = g_slow_entered;
    pthread_mutex_unlock(&g_slow_mtx);
    if (!entered) {
      struct timespec nap = {0, 1000000L};
      nanosleep(&nap, NULL);
    }
  }

  bool closed_early = false;
  if (entered) {
    chttpsvr_engine_stop();
    closed_early = fd >= 0 && _es_eof_within(fd, 2000);
  }
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
  bool joined = req_started && _fx_join(tau, th, NULL);
  if (!req_started || joined) free(ctx);
  if (fd >= 0) close(fd);
  chttpsvr_engine_wait();
  chttpsvr_destroy(a);
  chttpsvr_destroy(b);
  chttpsvr_engine_wait();
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(idle_ok);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(closed_early);
}

// A stop during a destroy's drain waits for it, so chttpsvr_engine_wait()
// returns only after the handler ended, and the destroy's late release doesn't
// reap the next engine. If the destroy left the registry before its drain, the
// wait would return early; if the release weren't tied to its engine, the new
// server s2 would lose its engine.
TEST_F(engine_stop_fixture, a_stop_racing_a_destroy_waits_for_its_drain) {
  chttpsvr__set_wait_in_flight_bounds_for_tests(5000, 2000);
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  bool created = srv != CHTTPSVR_NULL;
  if (!created) chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(created);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow300", _slow_300ms_handler,
                            NULL);
  chttpsvr_config cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18860;
  ccol_retval start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  atomic_store(&g_s300_entered, false);
  atomic_store(&g_s300_done, false);
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  pthread_t req_th;
  bool req_started = false;
  if (ctx) {
    ctx->url = "http://127.0.0.1:18860/slow300";
    req_started = pthread_create(&req_th, NULL, _async_get_thread, ctx) == 0;
    if (req_started) _fx_track(tau, req_th);
  }
  for (int i = 0; req_started && i < 10000 && !atomic_load(&g_s300_entered);
       i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  bool entered = atomic_load(&g_s300_entered);

  pthread_t destroy_th;
  bool destroy_started = false;
  if (entered) {
    chttpsvr__arm_quiesce_teardown_race_hook_for_tests();
    destroy_started =
        pthread_create(&destroy_th, NULL, _destroy_handle_thread, &srv) == 0;
    if (destroy_started) {
      _fx_track(tau, destroy_th);
      chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests();
    }
    chttpsvr__release_quiesce_teardown_race_hook_for_tests();
  }
  for (int i = 0; destroy_started && i < 200 &&
                  chttpsvr__servers_bundler_count_for_tests() != 0;
       i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }

  bool handler_done_at_wait = false;
  bool first_ok = false, second_ok = false, fresh_ok = false;
  if (destroy_started) {
    chttpsvr_engine_stop();
    chttpsvr_engine_wait();
    handler_done_at_wait = atomic_load(&g_s300_done);

    chttpsvr s2 = _start_server("18861", 18861);
    int fd = _es_connect(18861);
    first_ok = fd >= 0 && _es_keepalive_hello(fd);
    _fx_join(tau, destroy_th, NULL);
    struct timespec nap = {0, 300000000L};
    nanosleep(&nap, NULL);
    second_ok = fd >= 0 && _es_keepalive_hello(fd);
    if (fd >= 0) close(fd);
    int status = 0;
    fresh_ok = _get("http://127.0.0.1:18861/hello", &status) == ccol_success &&
               status == 200;
    chttpsvr_destroy(s2);
    chttpsvr_engine_wait();
  } else {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  bool req_joined = req_started && _fx_join(tau, req_th, NULL);
  int req_status = req_joined ? ctx->status : 0;
  free(ctx);
  chttpsvr__set_wait_in_flight_bounds_for_tests(0, 0);

  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_TRUE(req_joined);
  REQUIRE_EQ(req_status, 200);
  REQUIRE_TRUE(handler_done_at_wait);
  REQUIRE_TRUE(first_ok);
  REQUIRE_TRUE(second_ok);
  REQUIRE_TRUE(fresh_ok);
}

// A stop names the engine running when it's called. A hook holds the request in
// the watcher while a server starts; a request that named no engine would stop
// the new one.
TEST_F(engine_stop_fixture, a_stop_never_reaches_an_engine_that_starts_later) {
  (void)tau;
  chttpsvr warm = _start_server("18862", 18862);
  chttpsvr_destroy(warm);
  chttpsvr_engine_wait();

  chttpsvr__set_engine_stop_watcher_delay_for_tests(300);
  chttpsvr_engine_stop();
  chttpsvr s1 = _start_server("18862", 18862);
  struct timespec nap = {0, 600000000L};
  nanosleep(&nap, NULL);
  int status_idle_stop = 0;
  ccol_retval rv_idle_stop =
      _get("http://127.0.0.1:18862/hello", &status_idle_stop);

  chttpsvr_engine_stop();
  chttpsvr_destroy(s1);
  chttpsvr_engine_wait();
  chttpsvr s2 = _start_server("18863", 18863);
  nanosleep(&nap, NULL);
  int status_old_stop = 0;
  ccol_retval rv_old_stop =
      _get("http://127.0.0.1:18863/hello", &status_old_stop);
  chttpsvr__set_engine_stop_watcher_delay_for_tests(0);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  chttpsvr_destroy(s2);

  REQUIRE_EQ((int)rv_idle_stop, (int)ccol_success);
  REQUIRE_EQ(status_idle_stop, 200);
  REQUIRE_EQ((int)rv_old_stop, (int)ccol_success);
  REQUIRE_EQ(status_old_stop, 200);
}

// An allocator that the test marks destroyed; any call after that, the process
// exit included, aborts.
static _Atomic bool g_dead_alloc_dead = false;
static _Atomic int g_dead_alloc_calls_after_death = 0;

// Aborts the process when the allocator is marked destroyed, because nothing
// may call it after that. Every allocator entry below calls it.
static void _dead_alloc_check(void) {
  if (atomic_load(&g_dead_alloc_dead)) {
    atomic_fetch_add(&g_dead_alloc_calls_after_death, 1);
    fprintf(stderr, "FATAL: allocator called after it was destroyed\n");
    abort();
  }
}
// The allocator entries: each checks g_dead_alloc_dead, then calls the C
// library, so the test can show that nothing allocates after the destroy.
static void *_dead_malloc(size_t n) {
  _dead_alloc_check();
  return malloc(n);
}
static void _dead_free(void *p) {
  _dead_alloc_check();
  free(p);
}
static void *_dead_calloc(size_t n, size_t s) {
  _dead_alloc_check();
  return calloc(n, s);
}
static void *_dead_realloc(void *p, size_t s) {
  _dead_alloc_check();
  return realloc(p, s);
}

// chttpsvr_set_engine_logger(CLOG_NULL) closes the engine logger at once, so
// an application can destroy the allocator behind it before main returns.
// Without the release the logger would be closed at exit through the destroyed
// allocator.
TEST_F(engine_stop_fixture, releasing_the_engine_logger_frees_it_at_once) {
  (void)tau;
  static ccol_memmgmt_procs procs = {.malloc = _dead_malloc,
                                     .free = _dead_free,
                                     .calloc = _dead_calloc,
                                     .realloc = _dead_realloc};
  clog lg = clog_create_from_fd_mp(2, CLOG_FATAL, NULL, &procs);
  REQUIRE_TRUE(lg != CLOG_NULL);
  ccol_retval set_rv = chttpsvr_set_engine_logger(lg);

  chttpsvr srv = _start_server("18864", 18864);
  ccol_retval release_while_running = chttpsvr_set_engine_logger(CLOG_NULL);
  int status = 0;
  ccol_retval get_rv = _get("http://127.0.0.1:18864/hello", &status);
  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  ccol_retval release_rv = chttpsvr_set_engine_logger(CLOG_NULL);
  ccol_retval release_again_rv = chttpsvr_set_engine_logger(CLOG_NULL);
  clog_close(lg);
  atomic_store(&g_dead_alloc_dead, true);

  srv = _start_server("18864", 18864);
  int status2 = 0;
  ccol_retval get2_rv = _get("http://127.0.0.1:18864/hello", &status2);
  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)set_rv, (int)ccol_success);
  REQUIRE_EQ((int)release_while_running, (int)ccol_not_permitted);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
  REQUIRE_EQ((int)release_rv, (int)ccol_success);
  REQUIRE_EQ((int)release_again_rv, (int)ccol_success);
  REQUIRE_EQ((int)get2_rv, (int)ccol_success);
  REQUIRE_EQ(status2, 200);
  REQUIRE_EQ(atomic_load(&g_dead_alloc_calls_after_death), 0);
}

TEST_F(engine_stop_fixture, an_engine_stop_ends_the_running_state) {
  (void)tau;
  chttpsvr srv ccol__destructor(chttpsvr__destroy_cleanup) =
      _start_server("0", 0);
  bool before = false, after = true;
  uint16_t port = 0, port_after = 1;
  ccol_retval before_rv = chttpsvr_is_running(srv, &before);
  ccol_retval port_rv = chttpsvr_bound_port(srv, &port);
  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  ccol_retval after_rv = chttpsvr_is_running(srv, &after);
  ccol_retval port_after_rv = chttpsvr_bound_port(srv, &port_after);
  chttpsvr_destroy(srv);
  REQUIRE_EQ((int)before_rv, (int)ccol_success);
  REQUIRE_TRUE(before);
  REQUIRE_EQ((int)port_rv, (int)ccol_success);
  REQUIRE_NE((int)port, 0);
  REQUIRE_EQ((int)after_rv, (int)ccol_success);
  REQUIRE_FALSE(after);
  REQUIRE_EQ((int)port_after_rv, (int)ccol_not_permitted);
  REQUIRE_EQ((int)port_after, 0);
}
