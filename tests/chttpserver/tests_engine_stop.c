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

/* pthread_timedjoin_np is a glibc extension, and it's what lets the fixture
 * teardown of this file make a bounded join instead of a blind pthread_join
 * (see engine_stop_fixture below). A real regression can leave a background
 * thread stuck for good, for example with srv_engine_bundler.mutex held
 * forever, and without the bounded join the join itself would hang forever too.
 * That would only move the "the whole binary hangs instead of one test failing
 * cleanly" problem from "the chttpsvr_start() call of the next unrelated test"
 * to "the teardown of this exact test" without solving it. This macro has to
 * come before the first #include that can pull in <pthread.h> indirectly, which
 * is also where src/chttpserver.c and src/clogger.c put it. */
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
#include <pthread_np.h> /* pthread_timedjoin_np */
#endif

#include <tau/tau.h>
#include <test_connect.h>
#include <test_coverage.h>
#include <test_sanitizer.h>
#include <test_signals.h>

TAU_MAIN()

/* The test clients of this binary write to the server with plain write(2), and
 * the server can close a connection while one of them is writing. The library
 * leaves the SIGPIPE disposition to the application, so this binary ignores it
 * itself; tests_sigpipe.c covers the library under the default disposition.
 *
 * test_reset_inherited_signals() puts back the rest of the signal state that
 * the parent process can leave behind, before any thread starts. */
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
  test_reset_inherited_signals();
}

/* ========================================================================== */
/*                    BOUNDED-JOIN TEST FIXTURE                               */
/*                                                                            */
/* Every test below starts one or more background threads, and those threads  */
/* race chttpsvr's internal locks and refcounts. Several tests leave a thread */
/* unjoined on purpose on the path that finds a regression (the comment of    */
/* engine_stop_from_signal_handler_does_not_deadlock is one example).         */
/* Abandoning a thread like that is harmless only for the very LAST test of a */
/* binary, where nothing runs after it, and this file has 10 tests.           */
/* srv_engine_bundler.mutex is a single process-wide lock that                */
/* chttpsvr_start(), _engine_acquire() and _engine_release() all take. A      */
/* thread stuck with that lock held is a real regression and not the ordinary */
/* case, and it doesn't only hurt the test that found it: every LATER test of */
/* this binary that calls chttpsvr_start() hangs too, the moment it tries to  */
/* take the same mutex. One clean, reported failure then turns into a cascade */
/* that only an external CI timeout notices, and nothing in it says which     */
/* test is really at fault.                                                   */
/*                                                                            */
/* This fixture closes that gap where it can and contains it where it can't.  */
/* The tests below use TEST_F and TEST_F_TEARDOWN for it instead of a bare    */
/* TEST(). The fixture tracks every thread that a test body creates with      */
/* pthread_create() through _fx_track(), and every join goes through          */
/* _fx_join(), both the join inside a test body and the automatic sweep of    */
/* this teardown. _fx_join() is a generous but BOUNDED join built on          */
/* pthread_timedjoin_np, not a blind pthread_join. On a real regression that  */
/* turns "some unrelated later test hangs for no clear reason" into "the      */
/* teardown of THIS test names the thread that never finished". The teardown  */
/* then abandons that thread with pthread_detach() and lets Tau go on to the  */
/* next test. It still isn't a full fix for an unbounded deadlock, since      */
/* nothing can force a thread to release a lock it never releases, but it     */
/* helps in two ways: the failure is pinned on the test at fault and not on a */
/* later victim, and it copes with the more common case of a thread that is   */
/* only slower than a test's narrow internal bound, not deadlocked.           */
/* the narrow internal bound of a test allows, and is not deadlocked.        */
#define ENGINE_STOP_FIXTURE_MAX_THREADS 4

/* The struct must carry its own tag, engine_stop_fixture, matching the
 * typedef name exactly, because TEST_F_SETUP, TEST_F_TEARDOWN and TEST_F all
 * expand FIXTURE into `struct FIXTURE`. With only a typedef over an anonymous
 * struct, each of those expansions would declare a fresh, separate and
 * incomplete struct type instead of referring back to this one. */
typedef struct engine_stop_fixture {
  pthread_t threads[ENGINE_STOP_FIXTURE_MAX_THREADS];
  bool active[ENGINE_STOP_FIXTURE_MAX_THREADS]; /* true = not joined yet */
  int count;
} engine_stop_fixture;

/* Registers tid, so that the teardown sweep below joins it with a bound if the
 * test body never calls _fx_join() for it. Call this exactly once for each
 * thread you create successfully, right after pthread_create() returns 0. A
 * test with more concurrent threads than ENGINE_STOP_FIXTURE_MAX_THREADS needs
 * a bigger fixture array; no test in this file needs more than 3. */
static void _fx_track(engine_stop_fixture *fx, pthread_t tid) {
  /* Catches a future test that tracks more threads than this fixture's
     fixed-size arrays hold, before it silently corrupts the memory past their
     end. The doc comment of this function above gives the intended fix, a
     bigger fixture array, so don't raise this bound casually. */
  assert(fx->count < ENGINE_STOP_FIXTURE_MAX_THREADS);
  fx->threads[fx->count] = tid;
  fx->active[fx->count] = true;
  fx->count++;
}

/* A generous but bounded join. 30 seconds is well beyond every real, legitimate
 * wait in this file (the longest is a bounded cond_timedwait of 10s plus a
 * little slack), so this never fires for nothing against a thread that behaves
 * correctly, while the call itself can't hang the caller forever against a
 * thread that is really stuck. Returns true only when tid really stopped and
 * this call joined it. */
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

/* The one join call that every test body below should use instead of a bare
 * pthread_join(). It is bounded (see _fx_timed_join above) and it marks tid as
 * handled, so the teardown sweep won't try to join it a second time, because a
 * second join on a pthread_t that was already joined is undefined behavior.
 * It's safe to call this on a tid the fixture never tracked: the search below
 * finds nothing and the function does nothing. We don't expect that case in
 * practice, since _fx_track() should always track every thread a test creates,
 * but on purpose we don't treat it as a fatal error of the test infrastructure
 * either. */
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

TEST_F_SETUP(engine_stop_fixture) { (void)tau; /* fields already zeroed */ }

TEST_F_TEARDOWN(engine_stop_fixture) {
  for (int i = 0; i < tau->count; i++) {
    if (!tau->active[i]) continue;
    /* The opening comment of this section explains why this has to be bounded
     * and not a blind pthread_join: a real regression here must not cascade
     * into a hang of every later test of this binary. */
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

/* ========================================================================== */
/*     chttpsvr_engine_stop() FORCE-STOP COVERAGE (dedicated binary)          */
/*                                                                            */
/* chttpserver.h documents chttpsvr_engine_stop() as the way a signal handler */
/* starts a process-wide shutdown of the engine, with exactly this pattern:   */
/*   chttpsvr_start(srv, &cfg);                                               */
/*   chttpsvr_engine_wait();  // returns once a signal calls engine_stop()    */
/*   chttpsvr_destroy(srv);                                                   */
/* That state belongs to the whole process and the stop is forced: it tears   */
/* down the one shared ccol_event_loop reactor that every chttpsvr instance   */
/* of the process depends on. So it can't share the long-lived g_srv, g_srv2  */
/* and other handles of tests.c, whose servers stay started for that suite's  */
/* whole run, and a force-stop of the shared engine in the middle of that     */
/* suite would pull the reactor out from under all of them and fail unrelated */
/* tests. That's why this file is a binary of its own, following tests_tls.c  */
/* and tests_mem_mgmt.c in this directory, for a scenario that needs most of  */
/* a process to itself.                                                       */
/* ========================================================================== */

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, engine-stop!");
}

static ccol_retval_t _get(const char *url, int *status_out) {
  chttpcli cli = chttpclient_create(NULL);
  if (!cli) return ccol_not_enough_memory;
  chttp_request_t *req = chttp_request_create(CHTTP_GET, url, NULL, NULL);
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
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  if (!srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
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

/* Covers a call to chttpsvr_engine_stop() while srv is still fully started,
 * which is the forced path a signal handler takes, with nothing calling
 * chttpsvr_stop() or chttpsvr_destroy() on srv first. The reaper that the call
 * starts needs to reset g_servers_count whenever it frees the backing array of
 * g_servers. Without that, a later chttpsvr_destroy(srv) calls
 * _servers_unregister(srv), which dereferences a NULL g_servers[0], a
 * deterministic SIGSEGV and not some rare race. A clean run of this test also
 * shows that nothing leaks and nothing is freed twice along the way, which the
 * memtest target running valgrind shows best. */
TEST_F(engine_stop_fixture, force_stop_while_started_then_destroy_is_safe) {
  (void)tau; /* this test creates no background thread of its own */
  /* _ccol_destructor is a safety net for a REQUIRE_* failure between here and
     the explicit chttpsvr_destroy() calls below, since this file has no
     _teardown() and no atexit() that would catch a leaked handle at exit. A
     leaked handle leaves a server that is still registered and still holds an
     engine reference (and, for srv2, a listening socket still bound to this
     same port), and every later test of this binary can trip over that state.
     It's safe here because nothing else destroys srv or srv2 and nothing races
     their destruction. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);

  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();

  /* The crash this test guards against would happen inside chttpsvr_destroy()
   * itself, so getting to the line after it is the real assertion. */
  chttpsvr_destroy(srv);

  /* Show that the engine came back up cleanly and that the force-stop left no
   * global state corrupt, by starting a fresh server on the same port and
   * driving one real request through it. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);
  status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

/* The reaper's quiesce pass, _engine_force_stop_quiesce_all, walks every server
 * still registered in g_servers and not just one. This test drives that loop
 * with two independently started servers that are both still live when the
 * forced stop happens, each with a finished, keep-alive eligible connection in
 * its own idle list. */
TEST_F(engine_stop_fixture, force_stop_quiesces_multiple_servers) {
  (void)tau; /* this test creates no background thread of its own */
  chttpsvr srv_a _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18797", 18797);
  chttpsvr srv_b _ccol_destructor(___chttpsvr_destroy) =
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

/* chttpsvr_engine_stop() still drains every request that is really in
 * flight before it tears the reactor down, which is the documented contract of
 * _drain_and_close_all_connections and exactly what a graceful
 * chttpsvr_destroy() already does. The forced path must neither abandon nor
 * corrupt a request that a worker thread is already handling when the signal
 * fires. */
TEST_F(engine_stop_fixture,
       force_stop_drains_in_flight_request_before_reactor_teardown) {
  /* Shrinks the timing of _wait_in_flight_bounded with the same hook, used the
   * same way, as the test
   * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts in
   * tests.c. The REQUIRE_TRUE( g_slow_entered) below can fail while a worker
   * thread is really still blocked inside _slow_handler. The chttpsvr_destroy()
   * of srv at the end of the scope, through _ccol_destructor, then forces that
   * connection unblocked in well under a second, whereas without the shrink it
   * would fall back to the real production sizes, a graceful wait of 30s plus a
   * 5s grace period before the force-unblock. So the shrink turns a slow
   * (though not infinite) stall of a test that's already failing into a quick,
   * clean report of the failure. The test puts the real defaults back before it
   * returns, on every path. The graceful bound is 3000ms and not shorter
   * because the main path needs the handler to finish inside it: the handler is
   * released right after chttpsvr_engine_stop(), and a worker that wakes up
   * late on a busy machine (valgrind, ThreadSanitizer, a loaded runner) must
   * not run into the forced unblock, which cuts the request and fails the test.
   * On the main path the wait ends as soon as the handler returns, so the bound
   * costs nothing there. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(3000, 2000);

  char *err = NULL;
  /* _ccol_destructor: see the comment of
     force_stop_while_started_then_destroy_is_safe above. It's safe here because
     the background client thread below only sends an HTTP request to srv and
     never destroys srv itself. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(CLOG_NULL, &err);
  if (srv == CHTTPSVR_NULL) _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow", _slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18799;
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success)
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_EQ((int)start_rv, (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  /* On the heap and not a local on the stack. When a real regression below
     leaves th still running as the final, bounded _fx_join times out, that must
     not turn into a later write through a pointer dangling into this function's
     stack frame, which something else has reused by then. _stopping_race_ctx_t,
     a few tests further down in this file, follows the same reasoning. ctx is
     deliberately never freed on a bail-out path where th may still be running
     and writing into it. */
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ctx->url = "http://127.0.0.1:18799/slow";
  ctx->status = 0;
  ctx->rv = ccol_success;
  pthread_t th;
  int create_rv = pthread_create(&th, NULL, _async_get_thread, ctx);
  if (create_rv != 0) {
    free(ctx); /* no thread was created, so nothing can still touch it */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(create_rv, 0);
  _fx_track(tau, th);

  /* Wait for proof that the request really reached the worker thread and is
   * blocked inside _slow_handler, which is what makes it really in flight, and
   * only then force-stop the engine. A fixed sleep would be a flaky stand-in
   * for that proof on a slow or loaded machine (under valgrind, for example,
   * the accept, parse and dispatch path can easily take longer than a delay
   * that looks like plenty), so we wait on the handler's entry signal instead,
   * with a generous bound so that it can never block forever. */
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
    /* Without this, th leaks on this path as a joinable thread nobody ever
       joins. Release the handler no matter what, which does nothing if the
       handler really never started and unblocks it if the bounded wait above
       gave a false negative. Then join th before failing, instead of leaving it
       running for the rest of this process's life. The release just above
       guarantees that th's HTTP request can finish now, so blocking here on an
       unbounded join is safe. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_TRUE(g_slow_entered);

  chttpsvr_engine_stop();

  /* Only now do we let the handler finish. The forced stop must drain this
   * in-flight request instead of abandoning it, and without that drain the
   * client thread below hangs past its own request timeout and never sees a
   * 200. */
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);

  /* We check the result instead of fire-and-forget. If th is somehow still
     running 30s later, that's a real regression, because th should unblock the
     moment the code above sets g_slow_go, and on that path nothing here may
     read or free ctx since th may still write into it. Leaving ctx unfreed
     there is on purpose, see the comment on the declaration of ctx above. */
  bool th_joined = _fx_join(tau, th, NULL);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(th_joined);
  ccol_retval_t async_rv = ctx->rv;
  int async_status = ctx->status;
  free(ctx);
  REQUIRE_EQ((int)async_rv, (int)ccol_success);
  REQUIRE_EQ(async_status, 200);

  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);
}

static void *_release_slow_handler_after_delay(void *arg) {
  (void)arg;
  /* 150ms gives the reaper thread started by chttpsvr_engine_stop() enough time
   * to win the race to quiesce srv and then block inside
   * _drain_and_close_all_connections, waiting on the /slow-race request that is
   * still in flight below. Only after that do we let the handler finish, and
   * the drain with it. */
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
  return NULL;
}

/* Covers a race between two callers: the background reaper thread of
 * chttpsvr_engine_stop(), which quiesces every server still registered,
 * including srv, and an independent chttpsvr_destroy(srv) on this thread at the
 * same time, with no chttpsvr_engine_wait() call serializing the two. That's
 * exactly the pattern the doc comment of chttpsvr_engine_stop() invites, since
 * it describes the function as safe to call from a signal handler and doesn't
 * ask any caller to synchronize it against a concurrent chttpsvr_destroy()
 * first.
 *
 * The caller that loses the race to quiesce srv must wait for the winner's
 * real teardown work to finish, instead of returning from
 * _quiesce_server_once() at once and doing nothing the moment it sees that the
 * other caller already claimed srv's quiesce state. A loser that returns early
 * goes straight into _ccol_mutex_destroy(raw->mutex),
 * _ccol_mutex_destroy(raw->idle_mutex) and free(raw) while the reaper thread is
 * still using those exact objects inside _drain_and_close_all_connections,
 * which this test forces to block for a real, measurable window with the
 * /slow-race request still in flight. The result is a use-after-free and a
 * mutex destroyed while in use, both at once. A clean run of this test shows
 * that the race stays closed, and the memtest target (valgrind) and a build
 * with -fsanitize=thread show it best. */
TEST_F(engine_stop_fixture, concurrent_destroy_and_engine_stop_is_safe) {
  /* The same safety net that
     force_stop_drains_in_flight_request_before_reactor_teardown builds with the
     same hook. A REQUIRE_* between here and chttpsvr_engine_stop() can fire
     while a worker thread is really still blocked inside _slow_handler, and
     without the shrink the chttpsvr_destroy() of srv at the end of the scope
     below would fall back to the real, production-sized _wait_in_flight_bounded
     timers. The test puts the real defaults back before it returns, on every
     path. The graceful bound must stay far above the 150ms sleep of
     _release_slow_handler_after_delay, because the main path needs the handler
     to finish inside it. A sleep on a busy macOS runner can end 150ms late, and
     a graceful bound of 300ms would then escalate to the forced unblock, which
     cuts the request and fails the test with ccol_http_transfer_aborted. A
     bound of 3000ms leaves that room and still ends a stalled run within
     seconds. On the main path the wait ends as soon as the handler returns, so
     the bound costs nothing there. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(3000, 2000);

  char *err = NULL;
  /* _ccol_destructor: see the comment of
     force_stop_while_started_then_destroy_is_safe above. It's safe here for one
     specific reason: every REQUIRE_* that can return early sits strictly before
     the chttpsvr_engine_stop() call below, and there's no REQUIRE_* between
     chttpsvr_engine_stop() and the explicit chttpsvr_destroy(srv) a few lines
     later, so this destructor can never fire during the destroy race between
     the reaper and this thread that the test drives on purpose. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(CLOG_NULL, &err);
  if (srv == CHTTPSVR_NULL) _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow-race", _slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18800;
  ccol_retval_t start_rv2 = chttpsvr_start(srv, &cfg);
  if (start_rv2 != ccol_success)
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_EQ((int)start_rv2, (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  /* On the heap and not a local on the stack, for the reason given in the
     comment on the same ctx declaration in
     force_stop_drains_in_flight_request_before_reactor_teardown above. */
  _async_get_ctx *ctx = (_async_get_ctx *)calloc(1, sizeof(*ctx));
  if (!ctx) {
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
    REQUIRE_TRUE(false);
  }
  ctx->url = "http://127.0.0.1:18800/slow-race";
  ctx->status = 0;
  ctx->rv = ccol_success;
  pthread_t req_th;
  int req_create_rv = pthread_create(&req_th, NULL, _async_get_thread, ctx);
  if (req_create_rv != 0) {
    free(ctx); /* no thread was created, so nothing can still touch it */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
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
    /* Without this, req_th leaks on this path. Release the handler no matter
       what (which does nothing, harmlessly, if the handler never started), then
       join req_th before failing. The same fallback in
       force_stop_drains_in_flight_request_before_reactor_teardown explains
       why. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_TRUE(g_slow_entered);

  pthread_t releaser_th;
  int releaser_create_rv = pthread_create(
      &releaser_th, NULL, _release_slow_handler_after_delay, NULL);
  if (releaser_create_rv == 0) {
    _fx_track(tau, releaser_th);
  } else {
    /* _release_slow_handler_after_delay was never created, so it can't release
       the handler itself. We release it here instead, so that req_th, which is
       already running, can still finish before this failure and doesn't leak.
 */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(releaser_create_rv, 0);

  /* The documentation says chttpsvr_engine_stop() doesn't block: it only starts
   * a background reaper thread, a real _ccol_thread_create(). A fixed sleep
   * here would only guess that the reaper had enough time to win the race to
   * quiesce srv before the chttpsvr_destroy() below runs, and on a loaded CI
   * run or under valgrind a guess that's too short silently lets this thread's
   * chttpsvr_destroy() win instead. The test then drives the OTHER
   * interleaving, which is already safe, and never fails: a silent false pass
   * that never retests the regression this test is here to catch.
   * _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests() proves the
   * same thing deterministically instead, with no guessed timing at all, by
   * confirming that the reaper thread has already claimed quiesce_state ==
   * CHTTPSVR_QS_QUIESCING for srv (see that hook's doc comment). Only then does
   * this thread call chttpsvr_destroy(), so this call is the losing side on
   * every single run. The hook is released right after that so the reaper can
   * go into its own real teardown work, which then blocks on the /slow-race
   * request still in flight, as described above, until the 150ms timer of
   * _release_slow_handler_after_delay releases it. This mirrors how
   * fork_mid_quiesce_teardown_does_not_hang_child below uses the same hook. */
  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();

  chttpsvr_destroy(srv);

  _fx_join(tau, releaser_th, NULL);
  /* We check the result instead of fire-and-forget. The same reasoning in
     force_stop_drains_in_flight_request_before_reactor_teardown explains why
     nothing may read or free ctx when req_th is somehow still running 30s
     later. */
  bool req_th_joined = _fx_join(tau, req_th, NULL);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(req_th_joined);
  ccol_retval_t async_rv = ctx->rv;
  int async_status = ctx->status;
  free(ctx);
  REQUIRE_EQ((int)async_rv, (int)ccol_success);
  REQUIRE_EQ(async_status, 200);

  chttpsvr_engine_wait();

  /* Show that the engine came back up cleanly after all this. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18801", 18801);
  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18801/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

/* ========================================================================== */
/* This test covers a race between chttpsvr_start() and a concurrent          */
/* chttpsvr_engine_stop(), unlike the races above, which all use a server     */
/* that is ALREADY started. A server has to register itself in                */
/* servers_bundler.servers, the list that _engine_force_stop_quiesce_all      */
/* walks, as soon as it has confirmed its engine reference, and never only at */
/* the very end of chttpsvr_start(). A forced chttpsvr_engine_stop() can land */
/* in the window between the server taking a live engine reference and that   */
/* registration, and there it can't see the server at all. Its quiesce pass   */
/* then skips the server completely, and the reactor can be torn down while   */
/* chttpsvr_start() is still using it a few lines further on, for example     */
/* when it's about to call ccol_event_loop_add for the listener. That quiesce */
/* pass also never touches the started, listen_reg and contributed_to_engine  */
/* bookkeeping of such a server, so a later chttpsvr_stop() or                */
/* chttpsvr_destroy() on it hands a stale reg to ccol_event_loop_remove(),    */
/* against a reactor that may already be torn down or gone.                   */
/* _quiesce_server_once() therefore waits for every in-flight call on this    */
/* exact server that _chttpsvr_resolve protects (this test's chttpsvr_start() */
/* call is one of them) before it touches any state of the server.            */
/* __chttpsvr_destroy makes the same wait, and so does the forced path of     */
/* chttpsvr_engine_stop(). The test uses a white-box hook to pause            */
/* chttpsvr_start() inside that exact window deterministically, instead of    */
/* relying on a fixed sleep to hit a race window a few instructions wide by   */
/* luck.                                                                      */
/* ========================================================================== */

static chttpsvr g_race_srv = CHTTPSVR_NULL;
static ccol_retval_t g_race_start_rv = ccol_success;
/* The thread function below sets this flag as the very last thing it does,
   strictly after it has written both g_race_srv and g_race_start_rv in full,
   and the test below polls the flag, with a bound, before it reads either of
   those two plain globals and before it joins the thread. Neither global is
   atomic, so this atomic store and load pair is what really gives them a
   happens-before relationship. Without the poll in front of it a bare
   pthread_join would be a real data race on both globals under the C11 memory
   model, however quickly the join returns in practice. */
static _Atomic bool g_race_start_returned = false;

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
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18802;
  g_race_start_rv = chttpsvr_start(g_race_srv, &cfg);
  atomic_store(&g_race_start_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture, force_stop_racing_a_concurrent_start_is_safe) {
  g_race_srv = CHTTPSVR_NULL;
  g_race_start_rv = ccol_success;
  atomic_store(&g_race_start_returned, false);

  _chttpsvr_arm_start_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_racing_server_thread, NULL) != 0) {
    /* The hook was armed above and nothing ever enters it.
       _start_race_hook_wait_if_armed checks its own `go` flag again only after
       it finds `armed` still true, so we set `go` here up front instead of
       leaving it unset, and a later, unrelated chttpsvr_start() call elsewhere
       in this binary that lands on this hook passes straight through. Without
       this, such a call would block forever on a release that never comes. */
    _chttpsvr_release_start_race_hook_for_tests();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until chttpsvr_start() has confirmed its engine reference, registered
   * with servers_bundler and paused right there, in the exact window this test
   * drives a forced engine stop into. */
  _chttpsvr_wait_start_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* Gives the reaper thread of chttpsvr_engine_stop() time to start and reach
   * the wait inside _quiesce_server_once, which waits for this exact server's
   * in-flight chttpsvr_start() call to finish, so with the sleep the reaper
   * blocks there, the same way the 150ms of _release_slow_handler_after_delay
   * above gives a racing reaper time to win a similar race. The test means
   * something either way and doesn't need the sleep: the comment of that call
   * explains why the hook's block, not this sleep, is what makes the race
   * deterministic. The sleep only makes the test land more often on the more
   * interesting of the two safe interleavings. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let chttpsvr_start() finish, which is exactly what a reaper that
   * reached this server is blocked waiting for: nothing can tear the shared
   * reactor down until chttpsvr_start() releases its own resolve pin. */
  _chttpsvr_release_start_race_hook_for_tests();

  /* A bounded poll instead of a blind pthread_join. A regression here makes
     start_th deadlock with itself forever, and the poll turns that otherwise
     unbounded hang into a clean REQUIRE_TRUE(race_returned) failure that names
     the test. The flag is also what really gives a happens-before relationship
     to g_race_srv and g_race_start_rv below, which start_th wrote and neither
     of which is atomic, so reading them, or joining start_th, without checking
     the flag first would be a real data race under the C11 memory model,
     however quickly the join returns in practice. */
  bool race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&g_race_start_returned)) {
      race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L}; /* 50ms */
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(race_returned);
  if (!race_returned)
    return; /* a join would hang forever; nothing more to check safely */
  _fx_join(tau, start_th, NULL);
  /* g_race_srv is a global that the racing background thread writes, so it
     can't carry a scope-exit _ccol_destructor like a local handle can. We clean
     it up explicitly before asserting on race_rv here, so that a REQUIRE_*
     failure at this exact point doesn't leak it. This file has no _teardown()
     and no atexit() to catch such a leak, and a leaked server here stays
     registered, holding an engine reference, for every later test of this
     binary. Every REQUIRE_* below this point already comes after the final
     chttpsvr_destroy() of g_race_srv further down, so none of them needs the
     same treatment. */
  ccol_retval_t race_rv = g_race_start_rv;
  if (race_rv != ccol_success) {
    if (g_race_srv != CHTTPSVR_NULL) chttpsvr_destroy(g_race_srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)race_rv, (int)ccol_success);
  }

  chttpsvr_engine_wait();

  /* The real assertion is that neither of these crashes or hangs, which the
   * early registration and the wait in the quiesce pass above guarantee.
   * Without them the listen_reg of g_race_srv could point into a
   * ccol_event_loop that was already destroyed by now, and chttpsvr_destroy()
   * would be a use-after-free. */
  chttpsvr_destroy(g_race_srv);

  /* Show that the engine came back up cleanly after all this. */
  chttpsvr srv3 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18803", 18803);
  int status2 = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18803/hello", &status2),
             (int)ccol_success);
  REQUIRE_EQ(status2, 200);
  chttpsvr_destroy(srv3);
  chttpsvr_engine_wait();
}

/* ========================================================================== */
/* These two tests cover the same kind of deadlock as the test just above, in */
/* a narrower and deeper window. There a chttpsvr_start() call has already    */
/* registered and confirmed contributed_to_engine, and it is still inside its */
/* own _engine_acquire() call or about to enter it. It races a reap that has  */
/* already claimed the quiesce of this exact server and blocks waiting for    */
/* this call's resolve pin to drop. So _engine_acquire() must not block on    */
/* srv_engine_bundler.stopping while it still holds that pin, because the     */
/* reap could then never finish, since it needs the pin to drop first. That's */
/* a permanent deadlock rather than a slow race. Two independent triggers     */
/* reach it: a forced chttpsvr_engine_stop() call, and the graceful reap that */
/* runs when some other server's chttpsvr_destroy() drops the last reference  */
/* to the shared engine. Both variants below use a dedicated white-box hook,  */
/* _chttpsvr_arm_engine_stopping_race_hook_for_tests with its wait and        */
/* release siblings, which pauses right before the racing chttpsvr_start()    */
/* call's _engine_acquire(). That lands in exactly that window                */
/* deterministically, instead of relying on a fixed sleep to hit a race a few */
/* instructions wide by luck. Each test creates its own S1 and neither relies */
/* on an earlier test of this binary leaving a reactor live, so both are      */
/* self-contained and their order doesn't matter.                             */
/* ========================================================================== */

/* Private state for each run of the two races below (a forced reap against a
 * concurrent start and a graceful reap against a concurrent start), which are
 * the tests start_racing_a_concurrent_forced_reap_retries_and_succeeds and
 * start_racing_a_concurrent_graceful_reap_retries_and_succeeds. The two must
 * not share one file-scope global triple of srv2, start_rv and start_returned,
 * for the reason _destroy_hang_ctx_t in tests.c documents for the same kind of
 * bug. The two tests run one after the other, and the bounded poll of each can
 * bail out (see the opening comment of ENGINE_STOP_FIXTURE), and that path
 * deliberately leaves start_th tracked but unjoined instead of blocking on a
 * thread that looks stuck but may only be slow. Such a thread could then write
 * into a shared global after the NEXT test has already reset it, while that
 * next test races its own fresh thread for the same globals, so the later test
 * would see a value that neither of its own two writers produced. A heap ctx
 * for each run of a test makes that impossible by construction: a ctx is freed
 * only after _fx_join confirms that the thread owning it stopped, so an
 * abandoned thread of an earlier test can only ever write into its OWN ctx,
 * which is still alive, and never into a later test's ctx. */
typedef struct {
  uint16_t port;
  chttpsvr srv;
  ccol_retval_t start_rv;
  /* The thread function below sets this flag as the very last thing it does,
     strictly after it has written both srv and start_rv in full, and every
     caller polls the flag, with a bound, before it reads either of those two
     plain fields and before it joins the thread. Neither field is atomic, so
     reading them or joining the thread without that poll is a real data race
     under the C11 memory model. */
  _Atomic bool returned;
} _stopping_race_ctx_t;

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
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = ctx->port;
  ctx->start_rv = chttpsvr_start(ctx->srv, &cfg);
  atomic_store(&ctx->returned, true);
  return NULL;
}

/* Waits up to 4 s for the server on port to close an idle connection, and
 * returns 1 when it does. */
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

static void *_start_idle_limited_server_thread(void *arg) {
  _stopping_race_ctx_t *ctx = (_stopping_race_ctx_t *)arg;
  char *err = NULL;
  ctx->srv = chttpsvr_create(CLOG_NULL, &err);
  if (!ctx->srv) {
    fprintf(stderr, "FATAL: chttpsvr_create failed: %s\n", err ? err : "?");
    exit(1);
  }
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = ctx->port;
  cfg.idle_timeout_us = 1000000;
  cfg.max_header_read_duration_us = 1000000;
  ctx->start_rv = chttpsvr_start(ctx->srv, &cfg);
  atomic_store(&ctx->returned, true);
  return NULL;
}

/* A chttpsvr_start() that registers while the reap of the previous engine is
 * finishing belongs to the next engine, whose sweep closes an idle connection
 * at the idle limit. If the reap emptied the registry of servers, the started
 * server would be in no registry, nothing would sweep it, and the idle
 * connection would stay open. */
TEST_F(engine_stop_fixture, a_start_during_a_reap_is_swept_by_the_next_engine) {
  chttpsvr srv1 = _start_server("18850", 18850);
  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18851;
  /* Destroying the only server starts the engine's reap. */
  chttpsvr_destroy(srv1);
  struct timespec settle = {0, 200000000L};
  nanosleep(&settle, NULL);
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();
  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_idle_limited_server_thread, ctx) !=
      0) {
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);
  /* The start has registered and waits right before its _engine_acquire() call,
   * and now the reap finishes. */
  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();
  chttpsvr_engine_wait();
  _chttpsvr_release_engine_stopping_race_hook_for_tests();
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
  ccol_retval_t start_rv = ctx->start_rv;
  if (ctx->srv) chttpsvr_destroy(ctx->srv);
  free(ctx);
  REQUIRE_EQ(start_rv, ccol_success);
  REQUIRE_EQ(closed, 1);
}

TEST_F(engine_stop_fixture,
       start_racing_a_concurrent_forced_reap_retries_and_succeeds) {
  chttpsvr srv1 = _start_server("18820", 18820);

  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    /* srv1 is already live here, registered and holding an engine reference, so
       destroy it before failing, or a temporary lack of memory here would leak
       it for the rest of this binary's run. It's the same cleanup as for a
       pthread_create failure a few lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18821;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* The hook was armed above and nothing ever enters it.
       _engine_stopping_race_hook_wait_if_armed checks its own `go` flag again
       only after it finds `armed` still true, so we set `go` here up front
       instead of leaving it unset, and a later, unrelated chttpsvr_start() call
       elsewhere in this binary that lands on this hook passes straight through.
       Without this, such a call would block forever on a release that never
       comes. */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* no thread was created, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until the racing chttpsvr_start() call has confirmed its engine
   * contribution, registered with servers_bundler and paused right before its
   * own _engine_acquire() call, which is the exact window this test is about.
   */
  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* Gives the reaper thread of chttpsvr_engine_stop() time to start, quiesce
   * srv1 (which is registered too, so this same call force-stops it as well)
   * and reach the wait inside _quiesce_server_once, which waits for srv2's
   * in-flight chttpsvr_start() call to finish. With the sleep in place the
   * reaper correctly does NOT block there. The test means something either way
   * and doesn't need the sleep, which only makes it land more often on the more
   * interesting of the two safe interleavings. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let the racing chttpsvr_start() call go into _engine_acquire(), where
   * it sees stopping == true and returns at once instead of blocking, so the
   * caller unwinds and retries instead of deadlocking against the reaper. */
  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* A bounded poll instead of a blind pthread_join. It runs before this thread
     reads ctx->srv and ctx->start_rv below (which start_th wrote and neither of
     which is atomic) and before it joins start_th, so it gives them a real
     happens-before relationship, as the comment on ctx->returned above
     explains. The poll also turns a real regression, where start_th deadlocks
     with itself, into a clean REQUIRE_TRUE failure that names the test instead
     of an unbounded hang. On purpose ctx is NOT freed on the bail-out branch
     below, because start_th may still be running and write into ctx later (see
     the comment on ctx's type). The fixture teardown's bounded-join sweep,
     never this function, is what finally joins start_th or abandons it. */
  bool stopping_race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&ctx->returned)) {
      stopping_race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L}; /* 50ms */
    nanosleep(&nap, NULL);
  }
  if (!stopping_race_returned) {
    /* srv1 has nothing to do with the stuck start_th, so destroying it directly
       here is still safe. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(stopping_race_returned);
    return; /* a join would hang forever, and there's nothing more to check */
  }
  _fx_join(tau, start_th, NULL);

  /* The force-stop above has already quiesced srv1, but we destroy srv1
   * explicitly to release the handle, which is what this file always does for a
   * server that was force-stopped while live. */
  chttpsvr_destroy(srv1);

  /* The real assertion is that chttpsvr_start() on srv2 retries and succeeds
   * transparently, instead of handing the temporary race against the engine
   * stop to its own caller as a hard failure. Without the retry we'd never get
   * to this line, because the racing thread would deadlock forever. The code
   * above confirmed it joined start_th, so ctx is safe to read and free from
   * here on. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_NULL) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* No chttpsvr_engine_wait() call here. The racing server of the start-race
   * test above never contributes an engine reference before the reap catches it
   * and tears it down along with itself, but this call is different: it
   * succeeds through a transparent retry AFTER the first reap is completely
   * finished, so srv2 is a correctly RUNNING server at this point, on a reactor
   * the library has just created again, and nothing asks it to stop. Waiting
   * here for the engine to stop completely would wait forever for a stop that
   * never comes. */

  /* Show that srv2 really works, since chttpsvr_start() returning success isn't
   * enough. The result goes into a local and srv2 is destroyed no matter what,
   * before either REQUIRE_* below, because a real regression in this exact
   * retry-then-serve path, which this test is here to catch, could otherwise
   * leave a live server holding an engine reference behind for every later test
   * of this binary to trip over. */
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18821/hello", &status);

  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

TEST_F(engine_stop_fixture,
       start_racing_a_concurrent_graceful_reap_retries_and_succeeds) {
  chttpsvr srv1 = _start_server("18822", 18822);

  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    /* srv1 is already live here, registered and holding an engine reference, so
       destroy it before failing, or a temporary lack of memory here would leak
       it for the rest of this binary's run. It's the same cleanup as for a
       pthread_create failure a few lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18823;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* The same comment in the sibling forced-reap test above explains why we
       release a hook that nothing ever entered, instead of just disarming it.
 */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* no thread was created, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  /* srv1 is the only OTHER live server referencing the shared engine at this
   * point, and srv2's racing chttpsvr_start() call pauses before it ever calls
   * _engine_acquire(), so it hasn't incremented the shared reactor_refs counter
   * itself. Destroying srv1 therefore drops reactor_refs to 0, which starts the
   * graceful reap path, _engine_release(), while the sibling test above drives
   * the forced chttpsvr_engine_stop() path instead. From here on both paths go
   * through the same chain (_engine_reaper_fn ->
   * _engine_force_stop_quiesce_all() -> _quiesce_server_once()), so the rest of
   * this test is the same. */
  chttpsvr_destroy(srv1);

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* A bounded poll instead of a blind pthread_join, for the reason given in the
     same comment in the sibling forced-reap test above. srv1 was already
     destroyed above, which is what starts the graceful reap this test drives,
     so there's nothing left to clean up on the bail-out path here. */
  bool stopping_race_returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&ctx->returned)) {
      stopping_race_returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L}; /* 50ms */
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(stopping_race_returned);
  if (!stopping_race_returned)
    return; /* a join would hang for ever; nothing more to check. This
               code leaks ctx here on purpose (see its type comment). */
  _fx_join(tau, start_th, NULL);

  /* The code above confirmed that it joined start_th, so ctx is safe to read
     and to free. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_NULL) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* No chttpsvr_engine_wait() call here, for the reason in the same comment in
   * the sibling forced-reap test above: srv2 is a correctly RUNNING server at
   * this point, on a reactor the library has just created again, and nothing
   * asks it to stop. */

  /* The result goes into a local and srv2 is destroyed no matter what, before
     either REQUIRE_* below, for the reason in the same comment in the sibling
     forced-reap test above. */
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18823/hello", &status);

  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/*   chttpsvr_engine_stop() ASYNC-SIGNAL-SAFETY (real signal handler)         */
/*                                                                            */
/* chttpserver.h documents chttpsvr_engine_stop() as "async-signal-safe: safe */
/* to call from a signal handler". The whole point is that an application     */
/* installs it, or a thin wrapper around it, as a SIGTERM or SIGINT handler   */
/* for a graceful shutdown, so its real work must NOT run directly on         */
/* whatever thread called it. That work takes                                 */
/* _ccol_mutex_lock(srv_engine_bundler.mutex) and, when a reactor is live,    */
/* also calls _ccol_thread_create() through _spawn_reaper(), and POSIX        */
/* guarantees neither of them to be async-signal-safe. Running them there is  */
/* a self-deadlock that a real program can hit, not a theoretical one.        */
/* _engine_acquire() (called by chttpsvr_start()), _engine_release() (called  */
/* by chttpsvr_destroy()) and the chttpsvr_set_engine_*() setters all take    */
/* srv_engine_bundler.mutex too, and a signal can arrive on a thread that is  */
/* inside any of those calls. SIGTERM in the middle of chttpsvr_start() is    */
/* one example, and a realistic race during the shutdown or rolling restart   */
/* of a container. The handler's _ccol_mutex_lock() then deadlocks against    */
/* the lock that the same thread already holds, which hangs the whole process */
/* until a SIGKILL.                                                           */
/*                                                                            */
/* So every step that touches a mutex or _ccol_thread_create runs on a        */
/* dedicated watcher thread that is always running, g_engine_stop_watcher in  */
/* chttpserver.c. It is woken by sem_post(), the one synchronization          */
/* primitive that POSIX explicitly lists as async-signal-safe, so             */
/* chttpsvr_engine_stop() itself never calls _ccol_mutex_lock or              */
/* _ccol_thread_create, directly or indirectly, whatever the interrupted      */
/* thread is doing.                                                           */
/*                                                                            */
/* This test reproduces the exact hazard deterministically and relies on no   */
/* timing. A white-box hook locks srv_engine_bundler.mutex itself and then    */
/* calls raise() for SIGUSR1 while it still holds the lock. In a program with */
/* more than one thread raise() runs the signal handler synchronously on the  */
/* same thread before it returns, which models "a signal interrupts a thread  */
/* that already holds this mutex" exactly. The SIGUSR1 handler the test       */
/* installs calls chttpsvr_engine_stop(). Without the watcher thread this     */
/* sequence of calls deadlocks forever, and the bounded wait below turns that */
/* into a clean test failure instead of hanging the whole suite.              */
/* ========================================================================== */

static void _sigusr1_calls_engine_stop(int sig) {
  (void)sig;
  chttpsvr_engine_stop();
}

static _Atomic bool g_signal_test_done = false;

static void *_signal_test_thread(void *arg) {
  (void)arg;
  _chttpsvr_test_hold_engine_mutex_and_signal_self(SIGUSR1);
  atomic_store(&g_signal_test_done, true);
  return NULL;
}

TEST_F(engine_stop_fixture, engine_stop_from_signal_handler_does_not_deadlock) {
  /* A started server guarantees that the shared engine already exists and is
   * ready before the test provokes the race, and so is the signal-safe stop
   * watcher, which _engine_acquire() starts as the very first thing it does
   * under srv_engine_bundler.mutex. */
  /* On purpose this handle has no _ccol_destructor scope, unlike the other
     tests of this file. The REQUIRE_TRUE(done) a few lines below can
     legitimately fail on a real regression, and then the thread this test
     starts below has deadlocked with itself forever and still holds
     srv_engine_bundler.mutex. An automatic chttpsvr_destroy(srv) at the end of
     the scope would fire right then and block forever itself while trying to
     take the same mutex, through _engine_release() inside
     _quiesce_server_once(), turning a clean, reported test failure back into
     the whole-process hang this file works to avoid elsewhere. So srv is
     cleaned up explicitly, and only at the two earlier checkpoints below, where
     no such regression can be in progress yet because no other thread has
     touched srv_engine_bundler.mutex there. On the done==false path srv is
     deliberately left as it is, for the reasoning documented on that path. */
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

  /* A bounded wait instead of a join. A regression here makes the thread above
   * deadlock with itself forever inside chttpsvr_engine_stop(), which the
   * SIGUSR1 handler calls synchronously while it still holds
   * srv_engine_bundler.mutex, and a REQUIRE_TRUE(done) failure below is the
   * sign of that regression. The fixture teardown then tries one more bounded
   * join on th, limited to 30s, and if that fails it detaches th instead of
   * hanging the other tests of this binary. The comment of the
   * engine_stop_fixture teardown above explains why that's safe even though th
   * may hold srv_engine_bundler.mutex forever on this path. */
  bool done = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&g_signal_test_done)) {
      done = true;
      break;
    }
    struct timespec nap = {0, 50000000L}; /* 50ms */
    nanosleep(&nap, NULL);
  }
  sigaction(SIGUSR1, &old_sa, NULL);
  REQUIRE_TRUE(done);
  if (!done) return; /* a join would hang forever; nothing more to check */
  _fx_join(tau, th, NULL);

  /* Make sure chttpsvr_engine_stop() really ran, and didn't just return without
   * a deadlock, by checking that the engine really stops. */
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  /* Show that the engine comes back up cleanly afterwards. Its watcher thread
   * survives a stop and start cycle, because only the exit of the process tears
   * the watcher down. A scoped handle is safe from here on: getting here
   * already proves done==true, so the self-deadlock scenario, which is why srv
   * above has no scope, can't be in progress. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18805", 18805);
  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18805/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

/* ========================================================================== */
/* This test covers a deadlock between chttpsvr_start() and a concurrent      */
/* force-quiesce pass of chttpsvr_engine_stop(), for a server that is already */
/* registered and stopped by now.                                             */
/*                                                                            */
/* chttpsvr_start() holds its own _chttpsvr_resolve() pin                     */
/* (pending_resolve_count) for its whole call, including while it checks      */
/* whether a _quiesce_server_once pass for this exact server is already in    */
/* progress (quiesce_state == CHTTPSVR_QS_QUIESCING). _quiesce_server_once    */
/* itself first blocks until pending_resolve_count reaches 0, before any of   */
/* its real teardown work, which is the work that finally reaches             */
/* CHTTPSVR_QS_QUIESCED and unblocks a waiter. So chttpsvr_start() must not   */
/* wait on quiesce_done_cv while it STILL holds that exact pin, or the two    */
/* calls deadlock each other: chttpsvr_start() waits for a signal only        */
/* _quiesce_server_once can send, and _quiesce_server_once waits for a pin    */
/* only chttpsvr_start() can release. The race looks ordinary because it is:  */
/* a perfectly normal restart (a chttpsvr_stop() and then a chttpsvr_start()) */
/* against a concurrent chttpsvr_engine_stop(), not some contrived edge case. */
/*                                                                            */
/* So chttpsvr_start() releases its pin completely before it backs off and    */
/* resolves the handle again from the start, through the same                 */
/* _chttpsvr_resolve_unpin that every other exit path uses, and it never      */
/* blocks while still pinned.                                                 */
/*                                                                            */
/* The test reproduces the exact interleaving deterministically with a        */
/* dedicated white-box hook that pauses chttpsvr_start() right after it       */
/* resolves and takes the pin, before it does anything else. A fixed sleep    */
/* would only get two threads to the right instant by luck, while the hook    */
/* guarantees that the racing chttpsvr_start() call already holds its pin     */
/* before the reaper thread of chttpsvr_engine_stop() reaches the             */
/* pending_resolve_count wait of _quiesce_server_once for it.                 */
/* ========================================================================== */

/* Each call site below allocates this struct on the heap with calloc() instead
 * of using a shared file-scope global or a local on the caller's stack, for the
 * same reason as _stopping_race_ctx_t above and _destroy_hang_ctx_t in tests.c.
 * Both call sites below deliberately leave restart_th tracked but unjoined on
 * their own bail-out path, after their bounded poll, when that thread looks
 * stuck, instead of blocking on a thread that may really be deadlocked (that
 * deadlock IS the regression that
 * start_does_not_deadlock_against_concurrent_quiesce_pass is here to catch).
 * With a shared global for start_rv and returned, an abandoned thread of one
 * test, which may only be slow and not really hung, could corrupt the freshly
 * reset copy of the other test when it finally finishes. An arg on the stack
 * would be even worse, because the struct itself is gone once the bailing-out
 * test function returns. A heap allocation for each run, freed only after
 * _fx_join confirms that the owning thread stopped, makes both kinds of
 * corruption impossible by construction. The thread function below reads srv
 * and port exactly once, synchronously, at its very top (see its own comment),
 * so unlike start_rv and returned those two would be safe as plain fields
 * either way; they're in this struct anyway because the other two fields
 * already put it on the heap. */
typedef struct {
  chttpsvr *srv;
  int port;
  ccol_retval_t start_rv;
  /* The thread function below sets this flag as the very last thing it does,
     strictly after it has written start_rv in full, and every caller polls the
     flag, with a bound, before it reads start_rv and before it joins the
     thread. Without that poll each of those two is a real data race under the
     C11 memory model. */
  _Atomic bool returned;
} resolve_racing_start_arg_t;

/* The caller passes port and srv in instead of this code hardcoding them, so
   this restart always targets the exact port and the exact handle that the
   calling test's srv really started on (the two call sites below start srv on
   different ports of their own). This function reads both fields exactly once,
   here, before it does anything else the racing main thread can see, and the
   main thread goes past its own "hook entered" wait only after this call has
   resolved srv and paused. So neither field needs the heap allocation that
   start_rv and returned need for a safe late write, since those two are written
   at the very end, possibly much later on the path of the regression under
   test. See the comment of the struct above. */
static void *_resolve_racing_start_thread(void *arg) {
  resolve_racing_start_arg_t *a = (resolve_racing_start_arg_t *)arg;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = a->port;
  a->start_rv = chttpsvr_start(*a->srv, &cfg);
  atomic_store(&a->returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       start_does_not_deadlock_against_concurrent_quiesce_pass) {
  /* On purpose srv has no scoped destructor, unlike most other tests of this
     file. The whole premise of the test is that restart_th's chttpsvr_start()
     call on srv can get stuck for good under the exact regression the test
     guards against (a deadlock across two condition variables, against the
     reaper thread), and __chttpsvr_destroy() would then block forever too while
     waiting for srv's pending_resolve_count to drain. A scoped destructor at
     the end of this function would fire on the `!returned` bail-out path, which
     is there precisely to avoid a hang of the whole binary when this regression
     fires, so the destructor would hang the whole binary again and
     defeat the point of the bounded wait a few lines below. Three other places
     apply the same reasoning to their own "a background thread can hold my
     server's pin forever" scenario:
     engine_stop_from_signal_handler_does_not_deadlock,
     start_racing_engine_stop_and_destroy_does_not_free_raw_too_early, and both
     fork tests in this file. Every early-exit path below where nothing races
     srv yet is proven safe and destroys srv explicitly, while the `!returned`
     bail-out deliberately doesn't. */
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18807;
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* An ordinary stop: srv stays registered in servers_bundler and keeps its
     engine reference, because chttpsvr_stop() only closes the listener. That's
     exactly the state a real restart starts from, a chttpsvr_stop() first and a
     chttpsvr_start() after it. */
  chttpsvr_stop(srv);

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* srv is already registered here and still holds an engine reference. It's
       stopped, nothing destroyed it and nothing races it yet either, since
       nothing entered the race hook, so it's safe to destroy it explicitly
       here, unlike on the `!returned` bail-out further below (see the opening
       comment of this test). */
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  restart_arg->srv = &srv;
  restart_arg->port = 18807;
  pthread_t restart_th;
  if (pthread_create(&restart_th, NULL, _resolve_racing_start_thread,
                     restart_arg) != 0) {
    /* Nothing races srv yet, so destroying it directly here is still safe. The
       hook was armed above and nothing ever enters it.
       _start_resolve_race_hook_wait_if_armed checks its own `go` flag again
       only after it finds `armed` still true, so we set `go` here up front
       instead of leaving it unset, and a later, unrelated chttpsvr_start() call
       elsewhere in this binary that lands on this hook passes straight through.
       Without this, such a call would block forever on a release that never
       comes. */
    _chttpsvr_release_start_resolve_race_hook_for_tests();
    free(restart_arg); /* no thread was created */
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, restart_th);

  /* Deterministic: this returns only once restart_th's chttpsvr_start() call
     has resolved srv, taken its pin and paused at the hook without touching
     anything else, so no lucky timing is needed. */
  _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* The documentation says chttpsvr_engine_stop() doesn't block, it only starts
     a background reaper thread. */
  _chttpsvr_arm_quiesce_claimed_signal_for_tests();
  chttpsvr_engine_stop();

  /* Wait until the reaper thread reaches _quiesce_server_once for srv and
     claims CHTTPSVR_QS_QUIESCING. It then blocks on pending_resolve_count,
     which isn't zero because restart_th holds its pin. The released
     chttpsvr_start() call below takes raw->mutex after that claim, so it always
     goes into the CHTTPSVR_QS_QUIESCING branch this test is about.
     quiescing_claimed is only asserted after the cleanup: without the claim the
     restart wouldn't race the pass and the test would prove nothing. */
  bool quiescing_claimed =
      _chttpsvr_wait_quiesce_claimed_signal_entered_for_tests();

  /* The real moment under test. It releases restart_th's paused
     chttpsvr_start() call, which must not wait on quiesce_done_cv while it
     still holds the exact pin the reaper thread above is waiting for, because
     such a call deadlocks for good against that thread across two condition
     variables. It must release the pin and back off instead. */
  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* A bounded wait instead of a blind pthread_join. A regression here makes
     restart_th deadlock with itself forever, and with it the reactor's reaper
     thread and the chttpsvr_engine_wait() call below, and a
     REQUIRE_TRUE(returned) failure is the sign of that regression. The fixture
     teardown then tries one more bounded join on restart_th, limited to 30s,
     and if that fails it detaches restart_th instead of hanging the other tests
     of this binary, the same way as
     engine_stop_from_signal_handler_does_not_deadlock does elsewhere in this
     file. */
  bool returned = false;
  for (int i = 0; i < 100; i++) {
    if (atomic_load(&restart_arg->returned)) {
      returned = true;
      break;
    }
    struct timespec nap = {0, 50000000L}; /* 50ms */
    nanosleep(&nap, NULL);
  }
  REQUIRE_TRUE(returned);
  if (!returned)
    return; /* a join would hang for ever; nothing more to check. This
                code leaks restart_arg on purpose (see its type comment). */
  _fx_join(tau, restart_th, NULL);

  /* restart_th has provably returned at this point, so nothing races srv any
     more: srv is safe to destroy no matter what from here on, and restart_arg
     is safe to read and free. The result goes into a local and the cleanup
     comes before the asserts, because restart_th's chttpsvr_start() call is the
     real assertion of this test, and in any other order a real regression there
     could leave a live srv holding an engine reference behind for every later
     test of this binary to trip over. */
  ccol_retval_t restart_rv = restart_arg->start_rv;
  free(restart_arg);
  if (restart_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  }

  /* On purpose there's no chttpsvr_engine_wait() before the next check.
     restart_th's chttpsvr_start() call succeeded, so it must have taken a FRESH
     engine reference and a fresh reactor, because the chttpsvr_engine_stop()
     call above that this restart raced had already torn the original one down.
     Nothing has asked the new reactor to stop yet, so chttpsvr_engine_wait()
     would just block waiting for it, which says nothing about whether the
     deadlock under test happened. A successful restart of srv, plus a real
     request that it serves below, is itself the proof that
     chttpsvr_engine_stop() ran to completion, since its reaper thread had to
     finish releasing the old reactor before the _engine_acquire of this restart
     could create a new one. The real assertion is that srv restarted cleanly
     and really works, and not just that chttpsvr_start returned success while
     the reactor is still wedged. */
  int status2 = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18807/hello", &status2);

  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();
  REQUIRE_TRUE(quiescing_claimed);

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status2, 200);
}

/* ========================================================================== */
/* This test covers a use-after-free hazard in the CHTTPSVR_QS_QUIESCING      */
/* backoff branch of chttpsvr_start(). That branch must register itself in   */
/* quiesce_waiters BEFORE it releases its resolve pin, never after. Releasing */
/* the pin first can immediately unblock the pending_resolve_count wait of a  */
/* concurrent _quiesce_server_once() pass, here driven by the forced-quiesce  */
/* reaper of chttpsvr_engine_stop(). The pass can then run to completion and  */
/* find quiesce_waiters still at zero, because this call hasn't taken         */
/* raw->mutex again yet to increment it. A truly concurrent                   */
/* chttpsvr_destroy() on the very same handle then sees the pass finish and   */
/* frees raw, while chttpsvr_start() is still on its way back to raw->mutex   */
/* for the first time since it released its pin, and it then locks memory     */
/* that's already freed, a use-after-free. Incrementing quiesce_waiters in    */
/* the same critical section that sees CHTTPSVR_QS_QUIESCING, strictly before */
/* the pin release, is what makes that impossible. The doc comment of that    */
/* branch in chttpserver.c gives the details.                                 */
/*                                                                            */
/* The test drives the case deterministically with a dedicated white-box hook */
/* that pauses a real chttpsvr_start() call exactly in that window, where the */
/* call has registered itself in quiesce_waiters and released its resolve pin */
/* but hasn't taken raw->mutex again. Around that paused call the test then   */
/* drives two things. One is the reaper thread, which that pin release        */
/* unblocks and which runs its own real teardown work to completion with no   */
/* hook of its own (nearly instant here). The other is a truly concurrent     */
/* chttpsvr_destroy() call, which stays blocked for as long as this          */
/* call stays paused. It blocks on the reaper's servers_bundler_pins, which   */
/* are still outstanding because the reaper waits for this call's             */
/* quiesce_waiters registration to clear. That proves raw survives that exact */
/* window, which is stronger than asserting afterwards that nothing crashed.  */
/* Move the quiesce_waiters++ after the pin release                          */
/* and the relock and run it again under AddressSanitizer, and the test fails */
/* reliably with a heap-use-after-free on raw->mutex.                         */
/* ========================================================================== */

static chttpsvr g_start_uaf_race_srv = CHTTPSVR_NULL;
static _Atomic bool g_start_uaf_race_destroy_returned = false;

static void *_start_uaf_race_destroy_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_start_uaf_race_srv);
  atomic_store(&g_start_uaf_race_destroy_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       start_racing_engine_stop_and_destroy_does_not_free_raw_too_early) {
  /* The same discipline as every other test of this file that drives a hook:
     each intermediate outcome goes into a local instead of being asserted at
     once, and every armed hook is released and every created thread joined, no
     matter what, before any REQUIRE_* runs. So a failing assertion here can
     never leave a thread parked forever in one of the two hooks below, where it
     would wedge every later test of this binary that touches the same
     machinery. */
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18816;
  /* Unlike most other tests of this file, srv has no scoped destructor, because
     srv is about to alias g_start_uaf_race_srv, which
     _start_uaf_race_destroy_thread below destroys directly on a background
     thread. This codebase's sequential_double_destroy_is_fatal test shows that
     a second destroy of the same handle is fatal even when the two don't run at
     the same time, and a scoped destructor at the end of this scope, on top of
     that thread's destroy, would be exactly such a second destroy. This one
     early window has no such conflict, since g_start_uaf_race_srv isn't
     assigned yet and no background thread touches srv yet, so a start failure
     here is cleaned up explicitly. */
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }
  /* The same ordinary stop-then-restart starting state as in
     start_does_not_deadlock_against_concurrent_quiesce_pass above: srv stays
     registered and keeps its engine reference, just like a real restart, where
     a chttpsvr_stop() comes first and a chttpsvr_start() after it. */
  chttpsvr_stop(srv);
  g_start_uaf_race_srv = srv;

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* g_start_uaf_race_srv aliases srv, but no background thread has been
       started yet (that happens a few lines further down), so destroying srv
       directly and resetting the alias is safe here, unlike on the `!returned`
       bail-out further below, where a background thread may already be racing
       srv. See the opening comment of this test. */
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
    free(restart_arg); /* no thread was created */
  }
  if (restart_th_created)
    _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* Arm the hook for the vulnerable window before restart_th is released. After
     the release restart_th sees CHTTPSVR_QS_QUIESCING (set by the reaper
     below), registers itself in quiesce_waiters, releases its own resolve pin
     and pauses right there, before it takes raw->mutex for the first time since
     that pin release. */
  _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests();

  bool quiescing_claimed = false;
  if (restart_th_created) {
    _chttpsvr_arm_quiesce_claimed_signal_for_tests();
    chttpsvr_engine_stop(); /* the documentation says it doesn't block */
    /* Wait until the reaper thread reaches _quiesce_server_once for srv and
       claims CHTTPSVR_QS_QUIESCING. It then blocks on pending_resolve_count,
       which isn't zero because restart_th still holds its pin. */
    quiescing_claimed =
        _chttpsvr_wait_quiesce_claimed_signal_entered_for_tests();
  }

  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* restart_th_created alone doesn't prove that we reach this hook. It also
     needs the reaper started by chttpsvr_engine_stop() above to set
     CHTTPSVR_QS_QUIESCING before restart_th's paused chttpsvr_start() call
     checks quiesce_state again. quiescing_claimed above makes that order
     certain, because restart_th takes raw->mutex only after the release and the
     claim happened before it. The wait function has a bound inside, at 10s, and
     returns false instead of hanging this whole binary if a regression keeps
     the call away from the hook; see its doc comment in chttpserver.c. */
  bool hook_entered = false;
  if (restart_th_created && quiescing_claimed) {
    hook_entered =
        _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests();
  }
  bool paused_at_vulnerable_window = !restart_th_created || hook_entered;

  /* At this exact point restart_th has registered itself in quiesce_waiters and
     released its own resolve pin completely, and it is paused without having
     touched raw->mutex again. That pin release unblocks the reaper driven by
     engine_stop(), and we let that reaper run its own real teardown to
     completion, which is nearly instant here because srv has no live connection
     to drain and the reaper side deliberately has no hook. The reaper's final
     wait for quiesce_waiters == 0 must find restart_th's registration still
     there and correctly block, which keeps raw alive and keeps
     servers_bundler_pins above zero. */
  bool destroy_th_created = false;
  pthread_t destroy_th;
  if (paused_at_vulnerable_window) {
    atomic_store(&g_start_uaf_race_destroy_returned, false);
    destroy_th_created =
        pthread_create(&destroy_th, NULL, _start_uaf_race_destroy_thread,
                       NULL) == 0;
    if (destroy_th_created) _fx_track(tau, destroy_th);
  }

  /* A bounded negative check: chttpsvr_destroy() must not be able to return and
     free raw while restart_th is still parked in the exact window the guard
     protects, which is direct, positive proof that the guard works. Without the
     guard the concurrent reaper pass above would find quiesce_waiters still at
     zero, finish its own teardown and let this destroy call return and free raw
     well before restart_th gets back to raw->mutex. */
  bool destroy_returned_too_early = false;
  if (destroy_th_created) {
    for (int i = 0; i < 6; i++) {
      struct timespec nap = {0, 50000000L}; /* 50ms */
      nanosleep(&nap, NULL);
      if (atomic_load(&g_start_uaf_race_destroy_returned)) {
        destroy_returned_too_early = true;
        break;
      }
    }
  }

  /* Release restart_th. It then takes raw->mutex again (still safe, as the
     assertion just above shows), sees CHTTPSVR_QS_QUIESCED, decrements its own
     registration and resolves srv again, which fails because destroy_th has
     marked the slot as not in use by now. The call is safe and harmless even if
     restart_th never reached the hook. */
  _chttpsvr_release_start_quiescing_unpinned_race_hook_for_tests();

  bool restart_returned = false;
  ccol_retval_t restart_rv = ccol_success;
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
      /* We confirmed that restart_th was joined, so restart_arg is safe to read
         and free. On the !restart_returned path restart_arg is deliberately
         leaked and never freed, because restart_th may still be running and
         write into it later; see the comment on the struct's type above. */
      restart_rv = restart_arg->start_rv;
      free(restart_arg);
    }
  }
  /* restart_arg was already freed above when !restart_th_created, right after
     pthread_create failed, so there's nothing more to do for that case here. */

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
    /* destroy_th was never created, either because pthread_create failed or
       because hook_entered came back false (see this test's hook_entered
       comment above), so nothing else in the test releases the engine reference
       of srv, which restart_th restarted successfully since nothing destroyed
       it under that call. Without this destroy srv stays alive and registered,
       and the unconditional chttpsvr_engine_wait() below would block forever
       waiting for an engine reference that never goes away. We only get to this
       branch once restart_th has fully returned, which restart_returned records
       and the join above confirms, so this destroy can't race restart_th's
       chttpsvr_start() call while it's in flight. */
    chttpsvr_destroy(srv);
  }

  /* chttpsvr_engine_wait() has no timeout of its own (see its doc comment). It
     blocks until reactor_refs really reaches zero, so an unconditional call
     here would be a fresh hang risk in exactly the compound regression this
     test is here to catch. Take the case where destroy_th_created is true and
     destroy_th's chttpsvr_destroy() call never returns, a hypothetical
     regression that brings a deadlock back into the very teardown path under
     test. The `if (destroy_th_created)` branch above then never joins
     destroy_th and the code never falls through to the chttpsvr_destroy() call
     of the `else if` branch either, so nothing has released srv's engine
     reference, and an unconditional wait here would block forever and turn a
     clean, reported test failure into a hang of the whole binary. The same risk
     applies to restart_th's chttpsvr_start() call, which takes an engine
     reference of its own too and, on every real completion path, either
     correctly releases it or keeps it. So we call chttpsvr_engine_wait() only
     once both sides have confirmed they really returned, as the start_returned
     gate of stop_racing_a_concurrent_start_does_not_bind_over_open_listener
     below does as well. */
  bool engine_refs_should_settle = (!destroy_th_created || destroy_returned) &&
                                   (!restart_th_created || restart_returned);
  if (engine_refs_should_settle) chttpsvr_engine_wait();

  REQUIRE_TRUE(restart_th_created);
  REQUIRE_TRUE(quiescing_claimed);
  REQUIRE_TRUE(hook_entered);
  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(restart_returned);
  REQUIRE_TRUE(destroy_returned);
  if (!restart_returned || !destroy_returned)
    return; /* one side is stuck forever, nothing more to check safely */

  REQUIRE_FALSE(destroy_returned_too_early);
  /* restart_th has to have lost the race, because by the time it resolves srv
     again destroy_th has already marked the slot as not in use. */
  REQUIRE_EQ((int)restart_rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/* chttpsvr_stop() RACING A CONCURRENT chttpsvr_start() ON THE SAME HANDLE    */
/*                                                                            */
/* A plain chttpsvr_stop() call takes raw->lifecycle off CHTTPSVR_LC_RUNNING  */
/* BEFORE its own blocking ccol_event_loop_remove() and close() calls for the */
/* OLD listener registration return, not after them. chttpsvr_destroy() and   */
/* chttpsvr_engine_stop() are different, because both go through              */
/* _quiesce_server_once and its quiesce_state interlock, which the wait loop  */
/* of chttpsvr_start() already handles. Without a lifecycle state of its own  */
/* for that window, a chttpsvr_start() call racing an in-flight               */
/* chttpsvr_stop() call on the same handle from another thread would see      */
/* lifecycle == CHTTPSVR_LC_IDLE and quiesce_state ==                         */
/* CHTTPSVR_QS_NOT_QUIESCED at once and go straight into                      */
/* _make_listen_socket() and bind() for a NEW listener on the very same host  */
/* and port, while the OLD listener's fd is still open and bound. It's a      */
/* narrow race and it fails gracefully, with a spurious                       */
/* ccol_unexpected_failure from a concurrent EADDRINUSE. CHTTPSVR_LC_STOPPING */
/* closes it: a lifecycle state the server is in for the whole real teardown  */
/* work of _chttpsvr_stop_internal(), which the wait loop of chttpsvr_start() */
/* checks and which has exactly the same shape as quiesce_state. The test     */
/* uses a white-box hook to pause chttpsvr_stop() inside that exact window    */
/* deterministically, instead of relying on unbounded real timing to hit a    */
/* race window a few instructions wide by luck.                               */
/* ========================================================================== */

static chttpsvr g_stop_race_srv = CHTTPSVR_NULL;
static ccol_retval_t g_stop_race_start_rv = ccol_success;
static _Atomic bool g_stop_race_start_returned = false;
/* _stop_racing_server_thread sets this flag as its last statement, once
   chttpsvr_stop() itself has really returned, and the test below polls it, with
   a bound, before it ever calls chttpsvr_destroy() or chttpsvr_engine_wait() on
   g_stop_race_srv. If a hypothetical regression hangs chttpsvr_stop() itself,
   for example inside its own ccol_event_loop_remove() or close() call, the flag
   reports that as a clean, bounded test failure, whereas without it those two
   later calls would risk a hang of their own. The whole file follows the same
   discipline (make sure the racing thread really returned before touching
   shared state any further), and g_race_start_returned above is another such
   flag. */
static _Atomic bool g_stop_race_stop_returned = false;

static void *_stop_racing_server_thread(void *arg) {
  (void)arg;
  chttpsvr_stop(g_stop_race_srv);
  atomic_store(&g_stop_race_stop_returned, true);
  return NULL;
}

static void *_start_racing_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18808;
  g_stop_race_start_rv = chttpsvr_start(g_stop_race_srv, &cfg);
  atomic_store(&g_stop_race_start_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       stop_racing_a_concurrent_start_does_not_bind_over_open_listener) {
  /* Each intermediate outcome below goes into a local instead of being asserted
     at once with a REQUIRE_*, and every background thread is released and
     joined and g_stop_race_srv torn down, UNCONDITIONALLY, before any REQUIRE_*
     runs at all, as bounded_pool_full_returns_503 in tests.c does for the same
     reason. g_stop_race_srv is a plain global, which can't carry a scope-exit
     _ccol_destructor like a local handle can, because _start_racing_stop_thread
     writes g_stop_race_start_rv from a background thread;
     force_stop_racing_a_concurrent_start_is_safe above does the same. A
     REQUIRE_* that returned early from this function before the hook is
     released would leave stop_th parked forever inside the hook wait of
     _chttpsvr_stop_internal, which nothing else in this binary ever releases,
     so g_stop_race_srv's listener fd would never close. That leaves a thread
     blocked forever and a leaked server that stays registered and holds an
     engine reference for the rest of this binary's run, which is much worse
     than one failed assertion. */
  g_stop_race_srv = _start_server("18808", 18808);

  _chttpsvr_arm_stop_race_hook_for_tests();
  atomic_store(&g_stop_race_stop_returned, false);

  pthread_t stop_th;
  bool stop_th_created =
      pthread_create(&stop_th, NULL, _stop_racing_server_thread, NULL) == 0;
  if (stop_th_created) _fx_track(tau, stop_th);

  /* Deterministic once stop_th_created is true: it returns only after
     _chttpsvr_stop_internal() has entered CHTTPSVR_LC_STOPPING and paused right
     there, strictly before its own ccol_event_loop_remove() and close() calls,
     so the OLD listener's fd is still fully open and bound and no lucky timing
     is needed. _start_server() fails fast, with exit(1), when a chttpsvr_start
     call fails, so raw->lifecycle was already CHTTPSVR_LC_RUNNING by the time
     the code above assigned g_stop_race_srv. _chttpsvr_stop_internal therefore
     always reaches the hook once stop_th runs, and this wait can never hang for
     a hook that stop_th never gets to. */
  if (stop_th_created) _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_stop_race_start_rv = ccol_success;
  atomic_store(&g_stop_race_start_returned, false);
  /* The signal is armed before start_th is created, because chttpsvr_start()
     can get to its own CHTTPSVR_LC_STOPPING wait branch very quickly, and
     arming after creating the thread could miss the signal. */
  _chttpsvr_arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      stop_th_created &&
      pthread_create(&start_th, NULL, _start_racing_stop_thread, NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    /* Deterministic and not a fixed sleep: it returns only once
       chttpsvr_start() really reaches its own CHTTPSVR_LC_STOPPING wait loop
       and is about to enter it. The doc comment of that signal explains why it
       is a plain "the code got here" signal and not a park-until-release hook
       like g_stop_race_hook above. stop_th is still parked in g_stop_race_hook
       (nothing below has released it yet), so lifecycle can't have left
       CHTTPSVR_LC_STOPPING yet and still_blocked is guaranteed true here on
       every single run, and nobody has to guess whether a fixed sleep was long
       enough for the scheduler to even run start_th. On an unlucky, heavily
       loaded run such a sleep could read true for the wrong reason, because the
       scheduler hasn't run start_th yet, instead of the right reason, which is
       that the guard really blocks it. */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_stop_race_start_returned);
  }

  /* Release the hook (safe and harmless even if stop_th was never created to
     reach it), then join every thread that was really created, no matter what,
     before any REQUIRE_* below runs; the opening comment of this test explains
     why that order is load-bearing. The release lets chttpsvr_stop() finish:
     its ccol_event_loop_remove() and close() calls for the OLD listener really
     run and lifecycle leaves CHTTPSVR_LC_STOPPING, which lets the waiting
     chttpsvr_start() call, if there is one, go on. */
  _chttpsvr_release_stop_race_hook_for_tests();
  bool stop_returned = !stop_th_created;
  if (stop_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_stop_race_stop_returned)) {
        stop_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L}; /* 50ms */
      nanosleep(&nap, NULL);
    }
    if (stop_returned) _fx_join(tau, stop_th, NULL);
  }

  /* A bounded wait instead of a blind pthread_join, so when a regression turns
     this race into a hang this loop times out instead of hanging the whole test
     binary. The fixture teardown then tries one more bounded join on start_th,
     limited to 30s, and if that fails it detaches start_th instead of hanging
     the other tests of this binary. This file uses the same shape of bounded
     wait instead of a pthread_join elsewhere too. */
  bool start_returned = !start_th_created;
  if (start_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_stop_race_start_returned)) {
        start_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L}; /* 50ms */
      nanosleep(&nap, NULL);
    }
    if (start_returned) _fx_join(tau, start_th, NULL);
  }

  /* Try the real HTTP round trip only once we know the restart finished and
     succeeded, since calling out to a server that never started again, or whose
     own start thread is stuck forever, is just a second, redundant way to hang
     or fail. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_stop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18808/hello", &status);

  /* Tear g_stop_race_srv down before any REQUIRE_* below, but only once we know
     stop_th really returned. A concurrent chttpsvr_start() still in flight on
     another thread is exactly the case that this module's resolve and pin
     machinery makes memory-safe (other tests in this file drive that
     machinery), so a start_th that is stuck forever doesn't gate this teardown.
     A hypothetical regression that hangs chttpsvr_stop() ITSELF, past the point
     where the hook above released it, is a different and unproven risk: nothing
     shows that chttpsvr_destroy() and chttpsvr_engine_wait() are safe to call
     at the same time as a chttpsvr_stop() call on the same handle that is stuck
     somewhere in its own real teardown work, as opposed to a stop that is
     cleanly blocked on this test's hook. So in that case we call neither of the
     two, to avoid a second hang on top of the first, and deliberately leak
     g_stop_race_srv in that branch, which only a regression reaches. The
     start_th-stuck case below makes the same trade-off. */
  bool stop_teardown_safe = !stop_th_created || stop_returned;
  if (stop_teardown_safe) {
    chttpsvr_destroy(g_stop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(stop_th_created);
  REQUIRE_TRUE(stop_returned);
  if (!stop_returned)
    return; /* stop_th is stuck forever, nothing more to check safely */
  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is stuck forever, nothing more to check safely */
  /* The real assertion is that there's no spurious EADDRINUSE-type failure: the
     restart succeeded cleanly after the old listener was really gone. */
  REQUIRE_EQ((int)g_stop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/* chttpsvr_start() RACING chttpsvr_engine_stop()'S OWN _chttpsvr_stop_       */
/* INTERNAL() CALL THROUGH _quiesce_server_once, NOT A PLAIN chttpsvr_stop()  */
/*                                                                            */
/* The test just above,                                                       */
/* stop_racing_a_concurrent_start_does_not_bind_over_open_listener, drives    */
/* the CHTTPSVR_LC_STOPPING wait of chttpsvr_start() for the case where a     */
/* PLAIN chttpsvr_stop() call drives _chttpsvr_stop_internal(). There is a    */
/* second caller of _chttpsvr_stop_internal() with a different structure:     */
/* _quiesce_server_once(), the engine-wide path that both                     */
/* chttpsvr_engine_stop() and chttpsvr_destroy() go through. It reaches       */
/* CHTTPSVR_LC_STOPPING with quiesce_state already at CHTTPSVR_QS_QUIESCING,  */
/* which it sets before it ever calls _chttpsvr_stop_internal(), and it uses  */
/* a completely different set of pins, servers_bundler_pins, instead of the   */
/* pending_resolve_count that a plain chttpsvr_stop() call uses. The          */
/* CHTTPSVR_LC_STOPPING wait itself doesn't care which of the two drove it,   */
/* and this test is what drives that case on its own. Every other combination */
/* of hooks in this file either pauses a plain chttpsvr_stop() call inside    */
/* CHTTPSVR_LC_STOPPING (the test above), or pauses _quiesce_server_once()    */
/* BEFORE it ever reaches _chttpsvr_stop_internal, with quiesce_state ==      */
/* QUIESCING and pending_resolve_count already drained (see the fork-safety   */
/* tests further below), but none does both together. This one reuses the     */
/* SAME _stop_race_hook, since _chttpsvr_stop_internal() pauses at the same   */
/* point whichever caller got there, and drives that hook with                */
/* chttpsvr_engine_stop() instead of a plain chttpsvr_stop() call. A          */
/* concurrent chttpsvr_start() call on the same handle then sees              */
/* CHTTPSVR_LC_STOPPING with quiesce_state already at CHTTPSVR_QS_QUIESCING,  */
/* which is exactly the interleaving that the comment of the                  */
/* CHTTPSVR_LC_STOPPING branch reasons through (see the doc comment there in  */
/* chttpsvr_start()).                                                         */
/*                                                                            */
/* The condition variable wait of CHTTPSVR_LC_STOPPING isn't what keeps THIS  */
/* driver from a crash, a hang or starvation, unlike its two sibling          */
/* backoffs, CHTTPSVR_QS_QUIESCING and "currently stopping". A blind backoff  */
/* in that one branch (release the pin, nanosleep(1ms), resolve again and     */
/* retry) would pass this exact test too, still_blocked included, because     */
/* _chttpsvr_stop_internal() never waits on pending_resolve_count whichever   */
/* caller got there, so a blind re-pin from this branch can't starve it out   */
/* the way it can for the two siblings. What this test proves is that the     */
/* interleaving itself is safe: no hang, no crash, no spurious EADDRINUSE,    */
/* and the library rebuilds the whole shared engine cleanly once the quiesce  */
/* pass that drove it finishes. The condition variable wait has a separate    */
/* benefit of its own, which is that it avoids a busy poll thousands of times */
/* a second while a concurrent quiesce pass drains, and that drain can take   */
/* far longer than the comparatively quick listener teardown of a plain       */
/* chttpsvr_stop() call. That benefit is about CPU efficiency, which a single */
/* pass-or-fail test can't measure. The tests in tests.c that count           */
/* dispatches show the general pattern such a measurement would need here,    */
/* for example post_accept_alloc_failure_backs_off_instead_of_busy_looping.   */
/* ========================================================================== */
static chttpsvr g_qstop_race_srv = CHTTPSVR_NULL;
static ccol_retval_t g_qstop_race_start_rv = ccol_success;
/* _start_racing_quiesce_stop_thread sets this flag as its last statement, once
   chttpsvr_start() itself has really returned, and this test polls it, with a
   bound, before it ever joins that thread and before it reads
   g_qstop_race_start_rv, for the same reason as g_stop_race_start_returned
   above. */
static _Atomic bool g_qstop_race_start_returned = false;

static void *_start_racing_quiesce_stop_thread(void *arg) {
  (void)arg;
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18817;
  g_qstop_race_start_rv = chttpsvr_start(g_qstop_race_srv, &cfg);
  atomic_store(&g_qstop_race_start_returned, true);
  return NULL;
}

TEST_F(
    engine_stop_fixture,
    stop_race_hook_driven_by_quiesce_pass_racing_a_concurrent_start_is_safe) {
  /* Each intermediate outcome below goes into a local instead of being asserted
     on the spot with a REQUIRE_*, and every background thread is released and
     joined and g_qstop_race_srv torn down, no matter what, before any REQUIRE_*
     runs at all, just as
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener above does
     for the same reason. A REQUIRE_* that returned early here, before the hook
     is released, would leave the engine-stop reaper thread parked forever
     inside the hook wait of _chttpsvr_stop_internal, which nothing else in this
     binary ever releases, and every later test of this binary that touches the
     shared engine again would hang. */
  g_qstop_race_srv = _start_server("18817", 18817);

  _chttpsvr_arm_stop_race_hook_for_tests();

  /* chttpsvr_engine_stop() itself doesn't block: it only posts to the semaphore
     of the signal-safe watcher thread and returns at once (see its doc
     comment). The real reap it triggers runs on a separate reaper thread, which
     is what gets to _quiesce_server_once(), then _chttpsvr_stop_internal(),
     then the armed hook, so this test needs no background thread of its own
     just to make this call. */
  chttpsvr_engine_stop();

  /* Deterministic: it returns only after the reaper thread's
     _chttpsvr_stop_internal() call has entered CHTTPSVR_LC_STOPPING and paused
     right there, strictly before its own ccol_event_loop_remove() and close()
     calls, so the OLD listener's fd is still fully open and bound.
     quiesce_state is already CHTTPSVR_QS_QUIESCING at that point, because
     _quiesce_server_once() sets it before it ever calls
     _chttpsvr_stop_internal(), and that's exactly the interleaving this test is
     here to drive. */
  _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_qstop_race_start_rv = ccol_success;
  atomic_store(&g_qstop_race_start_returned, false);
  /* The signal is armed before start_th is created; the same use of this signal
     in stop_racing_a_concurrent_start_does_not_bind_over_open_listener above
     says why. */
  _chttpsvr_arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      pthread_create(&start_th, NULL, _start_racing_quiesce_stop_thread,
                     NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    /* Deterministic and not a fixed sleep. The reaper thread is still parked in
       g_stop_race_hook, since nothing below has released it yet, so lifecycle
       can't have left CHTTPSVR_LC_STOPPING yet, and still_blocked reads true
       here on every single run once chttpsvr_start() really reaches its own
       wait loop. See the doc comment of that signal. */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_qstop_race_start_returned);
  }

  /* Release the hook so the reaper's _chttpsvr_stop_internal() call can finish:
     its ccol_event_loop_remove() and close() calls for the OLD listener really
     run and lifecycle leaves CHTTPSVR_LC_STOPPING, which lets the waiting
     chttpsvr_start() call, if there is one, go on. That call then falls
     through, sees quiesce_state == CHTTPSVR_QS_QUIESCING and waits there
     instead (machinery that other tests already cover) until the reaper has
     finished the drain, the release and the teardown of the whole shared
     engine, so this restart can really rebuild that engine from scratch. */
  _chttpsvr_release_stop_race_hook_for_tests();

  /* A bounded poll instead of a blind pthread_join. A regression here makes
     start_th deadlock with itself forever, and the poll turns that otherwise
     unbounded hang into a clean REQUIRE_TRUE(start_returned) failure that names
     the test. The flag is also what really gives a happens-before relationship
     to g_qstop_race_start_rv below, a plain, non-atomic global that start_th
     wrote. */
  bool start_returned = !start_th_created;
  if (start_th_created) {
    for (int i = 0; i < 100; i++) {
      if (atomic_load(&g_qstop_race_start_returned)) {
        start_returned = true;
        break;
      }
      struct timespec nap = {0, 50000000L}; /* 50ms */
      nanosleep(&nap, NULL);
    }
    if (start_returned) _fx_join(tau, start_th, NULL);
  }

  /* Try the real HTTP round trip only once we know the restart finished and
     succeeded, with the same guard as the sibling test above. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_qstop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18817/hello", &status);

  /* g_qstop_race_srv is a global that _start_server writes before start_th ever
     runs, and start_th itself may restart it, so it can't carry a scope-exit
     _ccol_destructor. It's torn down only once we know start_th really
     returned, for the same reason as in the sibling test: don't risk a second
     hang on top of a start_th that is really stuck. */
  if (start_returned) {
    chttpsvr_destroy(g_qstop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is stuck forever, nothing more to check safely */
  /* The real assertion is that there's no spurious EADDRINUSE-type failure, and
     no hang or crash, when _quiesce_server_once() drove
     _chttpsvr_stop_internal() instead of a plain chttpsvr_stop() call: the
     restart succeeded cleanly after the library really tore down the old
     listener, and the whole shared engine after it, and then built both again
     from scratch. */
  REQUIRE_EQ((int)g_qstop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/* This test covers the lifetime of the bare struct chttpserver* that         */
/* _engine_force_stop_quiesce_all() reads straight out of                     */
/* servers_bundler.servers[0]. servers_bundler_pins exists exactly to protect */
/* the lifetime of that pointer (see the comment of that field on struct      */
/* chttpserver), and reusing pending_resolve_count and                        */
/* _chttpsvr_resolve_unpin for it instead would be a self-deadlock (the doc   */
/* comment of _engine_force_stop_quiesce_all explains why). The narrower      */
/* hazard that servers_bundler_pins closes is what this test is about. The    */
/* reaper thread reads a server out of servers_bundler.servers[] and is about */
/* to call _quiesce_server_once() on it. If no pin protected that bare        */
/* pointer, a concurrent chttpsvr_destroy(srv) on that exact server could run */
/* to completion and free srv while the reaper thread is still about to       */
/* dereference it, and the reaper thread's very next touch of srv would be a  */
/* use-after-free. The test uses a white-box hook to pause the reaper thread  */
/* deterministically right after it has pinned srv, that is after it          */
/* incremented servers_bundler_pins and released servers_bundler.mutex but    */
/* before it called _quiesce_server_once(). It then lands a concurrent        */
/* chttpsvr_destroy() on the same srv from a second thread. That destroy()    */
/* call must block and wait for servers_bundler_pins to reach 0, and stay    */
/* blocked until the hook is released and the reaper thread has really        */
/* finished with srv. It must not go ahead and free srv from under the reaper */
/* thread while that thread is still paused.                                  */
/* ========================================================================== */

static chttpsvr g_reaper_race_srv = CHTTPSVR_NULL;
static _Atomic bool g_reaper_race_destroy_returned = false;

static void *_destroy_racing_reaper_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_reaper_race_srv);
  atomic_store(&g_reaper_race_destroy_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       destroy_racing_the_reaper_does_not_free_srv_out_from_under_it) {
  /* The same discipline as
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener above: each
     intermediate outcome goes into a local, and the hook is released and every
     created thread joined, no matter what, before any REQUIRE_* runs. So a
     failing assertion here can never leave destroy_th parked forever in the
     reaper's hook wait, which nothing else in this binary would release, and
     can't leak g_reaper_race_srv's engine reference or its listener for the
     rest of this binary's run either. */
  g_reaper_race_srv = _start_server("18809", 18809);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* The documentation says chttpsvr_engine_stop() doesn't block: it only starts
     a real, separate reaper thread and returns at once, and that reaper thread
     is what pins g_reaper_race_srv and then blocks inside the armed hook. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_reaper_race_hook_entered_for_tests();

  atomic_store(&g_reaper_race_destroy_returned, false);
  pthread_t destroy_th;
  bool destroy_th_created =
      pthread_create(&destroy_th, NULL, _destroy_racing_reaper_thread, NULL) ==
      0;
  if (destroy_th_created) _fx_track(tau, destroy_th);

  bool still_blocked = false;
  if (destroy_th_created) {
    /* Gives chttpsvr_destroy() time to run, get to its own servers_bundler_pins
       wait and block inside it, the same 150ms this file uses elsewhere to let
       the other side get to the point where it blocks. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* chttpsvr_destroy() still needs to be blocked here waiting for
       servers_bundler_pins to reach 0, and must not have returned yet.
       Returning by now would mean it has already freed g_reaper_race_srv while
       the reaper thread is still paused and still holds a bare pointer to it.
 */
    still_blocked = !atomic_load(&g_reaper_race_destroy_returned);
  }

  /* Release the hook and then join destroy_th, both no matter what, before any
     REQUIRE_* below runs. The release is harmless even if the reaper thread
     never got to the hook, and since we know g_reaper_race_srv started, the
     real reaper thread of chttpsvr_engine_stop() always gets there, so the
     release can never hang on a hook the thread would never reach. The release
     lets the reaper thread finish with g_reaper_race_srv by calling
     _quiesce_server_once() on it. chttpsvr_destroy() has already won that race,
     because the destroy was held back until this point, so that call takes the
     loser path and returns at once. The reaper then decrements
     servers_bundler_pins, which lets chttpsvr_destroy()'s wait go on. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  if (destroy_th_created) _fx_join(tau, destroy_th, NULL);

  chttpsvr_engine_wait();

  /* Show that the engine came back up cleanly afterwards. A fresh server on the
     same port has to be able to start and serve a real request, because it
     isn't enough that nothing crashed above. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
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

/* ========================================================================== */
/* REPEATED chttpsvr_engine_stop() DURING TEARDOWN IS SAFE                    */
/*                                                                            */
/* _engine_reaper_fn sets srv_engine_bundler.reactor to NULL only near its    */
/* very end, well after _engine_force_stop_quiesce_all,                       */
/* _idle_sweep_stop_if_running and ccol_event_loop_destroy have all run, and  */
/* those can take real wall-clock time. srv_engine_bundler.stopping, on the   */
/* other hand, it sets to true at once, long before any of that finishes. So  */
/* _engine_force_stop_now, the function that the signal-safe watcher thread   */
/* runs for chttpsvr_engine_stop(), must not check only `if                   */
/* (srv_engine_bundler.reactor)` before starting a reaper thread. It needs    */
/* the same `&& !stopping` guard as the graceful _engine_release() path (see  */
/* that function's comment for why). Without that guard a second              */
/* chttpsvr_engine_stop() call can land while the reaper of a first call is   */
/* still in the middle of its teardown, which is completely ordinary for the  */
/* documented use from a signal handler: two SIGTERMs moments apart do it,    */
/* and so does a defensive double call in an application's shutdown code. It  */
/* then starts a SECOND reaper thread that grabs the very same, still live    */
/* ccol_event_loop handle, and both reapers call ccol_event_loop_destroy() on */
/* that handle independently. The documented contract of cthreadcomm.h makes  */
/* that unconditionally fatal, with _ccol_fatal_err() and SIGABRT, both for a */
/* double destroy where one call has already finished and for one where the   */
/* two overlap in time. A second engine_stop() call that is supposed to be    */
/* safe would then abort the process instead of doing nothing, as the         */
/* documentation says it does.                                                */
/* ========================================================================== */

TEST_F(engine_stop_fixture,
       second_engine_stop_call_while_first_still_tearing_down_is_safe) {
  (void)tau; /* this test creates no background thread of its own */
  /* The same discipline as the reaper-race test above: every intermediate step
     (releasing the hook and waiting for the engine to settle) runs no matter
     what, before any REQUIRE_* below. So a failing assertion here can never
     leave the reaper race hook armed forever, which would hang every later test
     of this binary that force-stops the engine, and can't leave the shared
     engine wedged halfway through its teardown for the rest of this binary's
     run. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18810", 18810);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* The first call. It starts a real reaper thread that pins srv through
     _engine_force_stop_quiesce_all and then blocks inside the hook armed just
     before, strictly before its own _quiesce_server_once(srv) call. At that
     point srv_engine_bundler.stopping is already true while
     srv_engine_bundler.reactor is still fully live, which is exactly the window
     the !stopping guard is there to recognise. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_reaper_race_hook_entered_for_tests();

  /* The second call, which lands right inside that window. Without the guard it
     would start a second reaper thread that goes on to call
     ccol_event_loop_destroy() on the very handle the first reaper is still
     going to use, and that's fatal. The !srv_engine_bundler.stopping guard of
     _engine_force_stop_now makes this call silently do nothing instead, as the
     documentation says: it dispatches nothing on the watcher thread and starts
     no second reaper. */
  chttpsvr_engine_stop();

  /* Before the first reaper is released, give a second reaper thread, if
     anything ever started one, a moment to get to its own
     ccol_event_loop_destroy() call. It's the same 150ms this binary uses
     elsewhere to let the other side get to the point where it blocks or
     crashes. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Release the reaper thread, and there really is only one, so it can finish
     the quiesce of srv and tear the reactor down normally. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  chttpsvr_engine_wait();

  /* A regression here would take the whole process down well before this point,
     either synchronously above or through a second reaper's delayed
     ccol_event_loop_destroy() call racing the first one's, so getting to this
     line at all is a meaningful part of the assertion. Then show that the
     engine came back up really clean, with no corrupt global state and no
     dangling, still registered server, by starting a fresh server on the same
     port and driving one real request through it, exactly like the other
     force-stop tests of this file. */
  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18810", 18810);
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18810/hello", &status);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/* This test covers the child-side fixup of quiesce_state and quiesce_waiters */
/* inside _chttpsvr_atfork_release_impl; the doc comment of that function in  */
/* chttpserver.c gives the details. fork() can land while a parent thread is  */
/* really in the middle of the teardown work of _quiesce_server_once for some */
/* server, with quiesce_state == CHTTPSVR_QS_QUIESCING at that moment, and    */
/* the child gets no copy of that thread. Without the fixup two things go     */
/* wrong. (1) The "loser" branch of _quiesce_server_once blocks forever: a    */
/* later chttpsvr_destroy() on the same handle in the child takes that branch */
/* and waits for a broadcast that only the vanished thread could send. (2)    */
/* The driver loop of _engine_force_stop_quiesce_all spins forever on this    */
/* exact server the next time it runs, because nothing removes the server     */
/* from servers_bundler.servers[] either. The test drives both halves. It     */
/* drives (2) directly, with a white-box accessor that reads                  */
/* servers_bundler.count itself, and not through a fresh                      */
/* chttpsvr_engine_stop() pass in the child, because chttpsvr_engine_stop()   */
/* alone isn't expected to do anything there anyway, whether or not the fixup */
/* corrected the server. It only wakes a watcher thread that is already       */
/* running, g_engine_stop_watcher, and this same atfork machinery             */
/* deliberately and correctly marks that watcher as not ready in a freshly    */
/* forked child (see the comment of that field), while a really fresh         */
/* chttpsvr_start() call arms it again. That's an unrelated part of this      */
/* module's fork story, already correct, and not what this test is about. The */
/* test drives the case deterministically with a dedicated white-box hook     */
/* that pauses a real reaper thread driven by chttpsvr_engine_stop() exactly  */
/* where quiesce_state == CHTTPSVR_QS_QUIESCING, in the parent, before the    */
/* fork. It lives in this binary and not in tests.c for the same reason as    */
/* every other test here: it drives the real process-wide                     */
/* chttpsvr_engine_stop(), which would pull the reactor out from under the    */
/* long-lived g_srv of tests.c.                                               */
/* ========================================================================== */
#if CCOL_FORK_SAFETY_REQUIRED
TEST_F(engine_stop_fixture, fork_mid_quiesce_teardown_does_not_hang_child) {
  (void)tau; /* This test creates no background thread of its own to track:
                chttpsvr_engine_stop() itself starts the internal reaper
                thread of the reactor, instead of this test creating it
                directly. */
  chttpsvr srv = _start_server("18811", 18811);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();

  /* The documentation says chttpsvr_engine_stop() doesn't block: it only starts
     a background reaper thread, which runs _quiesce_server_once against srv and
     then blocks inside the hook armed just before, right after quiesce_state
     becomes CHTTPSVR_QS_QUIESCING and strictly before any of its real teardown
     work starts. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv is now really stuck in this very process with quiesce_state ==
     CHTTPSVR_QS_QUIESCING, and we call fork() while that's still the case. */
  int pipefd[2];
  /* No REQUIRE_EQ here, because a failure of pipe() or fork() still has to
     release the armed hook and destroy srv before returning. Otherwise the real
     reaper thread this test paused above stays parked forever and wedges the
     process's shared engine for every later test of this binary that touches it
     through chttpsvr_start, chttpsvr_engine_stop or chttpsvr_engine_wait, which
     is much worse than one test failing on its own. */
  if (pipe(pipefd) != 0) {
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
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
    /* Bounds the lifetime of this child in case the hazard this test guards
       against still fires, where the chttpsvr_destroy() below would otherwise
       block forever. The child reports its outcome through the pipe and not
       through its exit code, because under make memtest valgrind replaces a
       forked child's real exit code with its own --error-exitcode the instant
       it finds ANY allocation of the child's inherited image still reachable at
       exit. Every child forked in the middle of a suite always has one, since
       the rest of the suite hasn't quiesced yet, so the exit code can't carry
       this result reliably. wait_from_within_own_task_does_not_hang in
       tests/cthreadpool and the fork_safety group in tests/clogger deal with
       the same valgrind behaviour. */
    alarm(30);
    char ok = 0;
    /* Half 1: srv is gone from servers_bundler.servers[] already, which
       shows that the atfork fixup ran the unregister step and not only the flip
       to CHTTPSVR_QS_QUIESCED. Without this check a regression here would show
       up only indirectly and much later, as the driver loop of
       _engine_force_stop_quiesce_all spinning forever the next time this
       process really drives a fresh reaper pass. */
    if (_chttpsvr_servers_bundler_count_for_tests() == 0) {
      /* Half 2: chttpsvr_destroy() on the very same handle must not block on
         the loser wait of _quiesce_server_once either. The handle still
         resolves here, since this path never touched chttpsvr_slot_table at
         all. */
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
  /* No REQUIRE_EQ here, because a spurious return from waitpid(), for example
     EINTR, should still release the hook and clean srv up before this function
     returns, just like the pipe() and fork() failure branches above. Otherwise
     the real, paused reaper thread stays parked forever and wedges the shared
     engine for every later test. */
  pid_t waited = waitpid(pid, &status, 0);

  /* The parent side: let the real, paused reaper thread finish its own teardown
     work normally, then clean srv up here too, whatever happened in the child.
 */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  /* We check WIFEXITED and not WEXITSTATUS (see this test's comment above).
     WIFEXITED alone still catches a real regression, because a hang becomes
     WIFSIGNALED through the alarm above, and a real crash shows up the same
     way. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* ========================================================================== */
/* This test covers the child-side reset of srv_engine_bundler.stopping       */
/* inside _chttpsvr_atfork_release_impl. The doc comment of that function in  */
/* chttpserver.c has the full story, which also covers the sibling resets of  */
/* reaper_joinable and idle_sweep_bundler.running that this test can't        */
/* isolate by themselves. The `while (stopping) _ccol_cond_var_wait(...)`     */
/* loop of _engine_acquire() runs strictly before its own                     */
/* _join_reaper_if_needed_locked() call, so a stopping flag that stays true   */
/* always blocks first and hides whatever the reaper_joinable reset would     */
/* otherwise show on its own. Those two resets are correct by the same        */
/* construction as this one, which the test confirms deterministically, and   */
/* the g_engine_stop_watcher.started precedent of this module has the same    */
/* shape, so neither needs a narrower test of its own. A real parent reaper   */
/* thread, paused halfway through its teardown at the moment of the fork,     */
/* leaves srv_engine_bundler.stopping true. The setup is the same as in the   */
/* previous test, and the child gets no copy of that thread. The previous     */
/* test never gets to code that reads that flag at all, because its           */
/* chttpsvr_destroy() takes the loser branch of _quiesce_server_once, which   */
/* never touches the shared engine's stopping bookkeeping. A restart of THAT  */
/* SAME srv would skip _engine_acquire() completely as well: the              */
/* `need_acquire = !raw->contributed_to_engine` test of chttpsvr_start() sees */
/* a stale value that is still true, inherited from before the whole stop     */
/* sequence started, which is a second, independent reason why srv itself     */
/* can't be the handle under test. A really fresh handle, never started and   */
/* with contributed_to_engine false from construction, is what forces         */
/* chttpsvr_start() to call _engine_acquire() for real and so drives that     */
/* call's stopping-wait loop. This test is not vacuous: without the           */
/* child-side reset the child's chttpsvr_start() call hangs forever. srv is   */
/* only needed to put the shared engine into the right state to fork out of,  */
/* stuck halfway through a teardown, and the start and stop of the fresh      */
/* handle have nothing else to do with srv.                                   */
/* ========================================================================== */
TEST_F(engine_stop_fixture,
       fork_mid_quiesce_teardown_does_not_hang_fresh_start_in_child) {
  (void)tau; /* This test creates no background thread of its own to track:
                chttpsvr_engine_stop() itself starts the internal reaper
                thread of the reactor, instead of this test creating it
                directly. */
  chttpsvr srv = _start_server("18812", 18812);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv_engine_bundler.stopping is true now, and a real reaper thread is alive
     and paused inside the hook, so reaper_joinable == true. We call fork()
     while both are still true. */
  int pipefd[2];
  /* No REQUIRE_EQ here. The same comment in the previous test explains why a
     failure of pipe() or fork() must still release the hook and destroy srv
     before returning; otherwise the real reaper thread stays parked forever and
     wedges the shared engine for every later test. */
  if (pipe(pipefd) != 0) {
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }

  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
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
    /* The same comment in the previous test explains why this child reports
       through the pipe and not through the exit code of the process. */
    alarm(30);
    /* On purpose a brand new handle and not srv itself; the doc comment of this
       test above explains why restarting srv wouldn't drive _engine_acquire()
       at all. */
    chttpsvr fresh = chttpsvr_create(CLOG_NULL, NULL);
    char ok = 0;
    if (fresh != CHTTPSVR_NULL) {
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18813;
      chttpsvr_start(fresh, &cfg); /* Nothing asserts on this result:
                                       whether the bind() of this fresh
                                       server succeeds does not matter, only
                                       whether the call returns at all is
                                       under test here. */
      ok = 1; /* we only get here if the call above returned */
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
  /* No REQUIRE_EQ here. The same comment in the previous test explains why a
     spurious return from waitpid() must still release the hook and clean srv
     up before this function returns. */
  pid_t waited = waitpid(pid, &status, 0);

  /* The parent side: let the real, paused reaper thread finish its own teardown
     work normally, then clean srv up here too, whatever happened in the child.
 */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* Sends a GET of /hello to 127.0.0.1:port and tells whether a 200 came back
   within timeout_ms. */
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

/* A child forked while the reaper of its parent's engine is still tearing the
   reactor down inherits a reactor handle with no poller and no threads. Its
   first server has to run on a reactor of its own and answer a request.
   A child that kept the inherited reactor would start its server on it, the
   listener would never be polled, and the request would get no answer within
   its 20 s bound. */
TEST_F(engine_stop_fixture, a_child_forked_during_a_teardown_serves_requests) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
  chttpsvr srv = _start_server("18850", 18850);
  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  int pipefd[2];
  if (pipe(pipefd) != 0) {
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
    chttpsvr_engine_wait();
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  pid_t pid = fork();
  if (pid == -1) {
    close(pipefd[0]);
    close(pipefd[1]);
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
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
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18851;
      if (chttpsvr_start(fresh, &cfg) == ccol_success &&
          _chttpsvr_engine_reactor_pid_for_tests() == getpid() &&
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
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);
  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* An engine needs its stop watcher, because without it chttpsvr_engine_stop()
   does nothing and a chttpsvr_engine_wait() that relies on that stop waits
   forever. When the watcher thread can't be created, chttpsvr_start() fails
   with ccol_unexpected_failure, and a later start that has threads to give
   brings the watcher up, so a stop and a wait end. A fork child has no watcher
   yet, which is what lets the test get to its creation. An engine that
   started without its watcher would make the first start succeed. */
TEST_F(engine_stop_fixture, a_start_fails_when_the_stop_watcher_cannot_start) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
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
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18852;
      _chttpsvr_force_watcher_thread_create_fail_for_tests(true);
      ccol_retval_t first = chttpsvr_start(srv, &cfg);
      _chttpsvr_force_watcher_thread_create_fail_for_tests(false);
      out[0] = first == ccol_unexpected_failure;
      if (first == ccol_success) chttpsvr_stop(srv);
      if (chttpsvr_start(srv, &cfg) == ccol_success) {
        chttpsvr_engine_stop();
        chttpsvr_engine_wait(); /* the alarm bounds a wait that never ends */
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

/* A server relies on the shared idle sweep thread for its idle timeout and
   for getting max_connections capacity back. When that thread can't be
   created, chttpsvr_start() fails with ccol_unexpected_failure and leaves
   the server stopped, and a later start that has a thread to give brings
   the sweep up and serves requests. A fork child has no sweep thread yet,
   which is what lets the test reach its creation. A start that carried on
   without the sweep would make the first start succeed. */
TEST_F(engine_stop_fixture, a_start_fails_when_the_idle_sweep_cannot_start) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
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
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 0;
      _chttpsvr_force_idle_sweep_thread_create_fail_for_tests(true);
      ccol_retval_t first = chttpsvr_start(srv, &cfg);
      _chttpsvr_force_idle_sweep_thread_create_fail_for_tests(false);
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

/* When the engine's reaper thread can't be created, the destroy of the last
   server runs the reaper inline. That reaper quiesces every server that is
   still registered, so it must not find the server whose own quiesce runs on
   the same thread, or it waits for itself forever. The child reports through a
   pipe whether the destroy and the engine wait returned, and the alarm ends a
   child that hangs, so a regression fails instead of blocking the suite. A
   reaper started before the server unregisters makes the destroy wait
   forever. */
TEST_F(engine_stop_fixture, a_destroy_whose_reaper_runs_inline_returns) {
  (void)tau;
  TEST_SKIP_FORK_IF_UNSUPPORTED();
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
    _chttpsvr_force_reaper_thread_create_fail_for_tests(true);
    chttpsvr_destroy(srv); /* the alarm bounds a destroy that never ends */
    _chttpsvr_force_reaper_thread_create_fail_for_tests(false);
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

/* ========================================================================== */
/*          NO UNJOINED REAPER THREAD AFTER THE LAST DESTROY                  */
/* ========================================================================== */

/* chttpsvr_destroy() on the last started server hands the shared reactor's
   teardown to a joinable reaper thread and returns right away, which is the
   documented asynchrony that chttpsvr_engine_wait() is there to wait on. That
   thread still needs to end up joined for an application that calls neither
   chttpsvr_engine_wait() nor a later chttpsvr_start(), because a joinable
   thread that has returned but was never joined keeps its stack mapping and its
   glibc bookkeeping allocated for the rest of the process's life.

   On purpose nothing but the white-box hook is called after chttpsvr_destroy():
   any chttpsvr_start()/chttpsvr_engine_wait() here would join the reaper itself
   and make the test vacuous, which is exactly the shape of the gap under test.
   The wait below is a bounded poll on real state and not a fixed sleep, so a
   slow machine (or valgrind) costs latency but never gives a wrong answer.

   Without the engine reaper's hand-off to the engine-stop watcher thread
   nothing joins that thread, and the hook stays true until the poll's 30-second
   bound runs out. */
TEST_F(engine_stop_fixture,
       destroying_the_last_server_leaves_no_unjoined_reaper_thread) {
  (void)tau; /* this test creates no background thread of its own */
  chttpsvr srv = _start_server("18824", 18824);
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18824/hello", &status);

  chttpsvr_destroy(srv);

  bool joined = false;
  for (int i = 0; i < 6000; i++) {
    if (!_chttpsvr_engine_reaper_unjoined_for_tests()) {
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

/* The reaper can run to its end before the call that spawned it records it. The
   delay hook holds that call for 3 s right after the thread starts, which is
   longer than a reaper needs (its longest step is joining the sweep thread,
   which sleeps 1 s between ticks). The reaper still has to end up joined. If
   the record were made after the reaper had already asked to be joined, the
   join request would find nothing and the thread would stay unjoined until
   the poll's own bound runs out. */
TEST_F(engine_stop_fixture,
       a_reaper_that_ends_before_it_is_recorded_is_still_joined) {
  (void)tau; /* this test creates no background thread of its own */
  chttpsvr srv = _start_server("18841", 18841);
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18841/hello", &status);

  _chttpsvr_set_spawn_reaper_delay_for_tests(3000);
  chttpsvr_destroy(srv);
  _chttpsvr_set_spawn_reaper_delay_for_tests(0);

  bool joined = false;
  for (int i = 0; i < 1000; i++) {
    if (!_chttpsvr_engine_reaper_unjoined_for_tests()) {
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

/* The engine reaper's hand-off must not turn chttpsvr_destroy() into a
   synchronous wait for that reaper. destroy stays asynchronous, and a caller
   that wants to wait calls chttpsvr_engine_wait(). A fresh server binding the
   very same port right after the previous one was destroyed is what shows that
   the shared engine still comes back cleanly on the ordinary path, with no
   engine wait anywhere in between. */
TEST_F(engine_stop_fixture,
       destroy_without_an_engine_wait_still_allows_an_immediate_rebind) {
  (void)tau; /* this test creates no background thread of its own */
  chttpsvr srv = _start_server("18825", 18825);
  int status = 0;
  ccol_retval_t rv_a = _get("http://127.0.0.1:18825/hello", &status);
  int status_a = status;
  chttpsvr_destroy(srv);

  chttpsvr srv2 = _start_server("18825", 18825);
  status = 0;
  ccol_retval_t rv_b = _get("http://127.0.0.1:18825/hello", &status);
  int status_b = status;
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)rv_a, (int)ccol_success);
  REQUIRE_EQ(status_a, 200);
  REQUIRE_EQ((int)rv_b, (int)ccol_success);
  REQUIRE_EQ(status_b, 200);
}

/* ========================================================================== */
/*          ENGINE STOP WITH CONNECTIONS THAT WAIT WITH NO THREAD             */
/* ========================================================================== */

static void _es_len_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)ctx;
  size_t n = 0;
  chttpsvr_req_body(req, &n);
  chttpsvr_resp_printf(resp, "%zu", n);
}

/* Reads the body until it fails or ends. */
static void _es_drain_stream_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                     void *ctx) {
  (void)ctx;
  char buf[256];
  while (chttpsvr_req_read(req, buf, sizeof(buf)) > 0) {
  }
  chttpsvr_resp_write_str(resp, "drained");
}

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

static bool _es_wait(chttpsvr srv, int what, size_t want) {
  for (int i = 0; i < 5000; i++) {
    size_t v = what == 0   ? _chttpsvr_parked_count_for_tests(srv, 1)
               : what == 1 ? _chttpsvr_mem_waiting_for_tests(srv)
               : what == 2 ? _chttpsvr_pool_active_for_tests(srv, true)
                           : _chttpsvr_stream_queue_len_for_tests(srv);
    if (v >= want) return true;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
  return false;
}

/* Reads until the end of the stream, for at most 5 s. Returns true on an end of
 * stream or a reset. */
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

static void *_es_stop_and_wait(void *arg) {
  (void)arg;
  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  return NULL;
}

TEST_F(engine_stop_fixture, force_stop_ends_parked_waiting_and_queued) {
  /* A parked body, a request waiting for body memory, a streaming handler
   * blocked on its client and a streaming request in the queue.
   * chttpsvr_engine_stop() must end every one of them in bounded time: the
   * waiters get closed and the queued request gets 503. valgrind and
   * ThreadSanitizer run this same test. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      chttpsvr_create(CLOG_NULL, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_NULL);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _es_len_handler, NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/drain",
                                      _es_drain_stream_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18830;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  /* The parked body holds its first buffer of 16384 bytes, so the second body
   * doesn't fit next to it and waits for memory. */
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
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
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

/* Counts how many times needle occurs in the file at path. */
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

/* Waits, for at most bound_ms on CLOCK_MONOTONIC, until no engine runs and
 * no reaper is left to join, then joins it through chttpsvr_engine_wait(),
 * which returns at once by then. Returns false when an engine is still up
 * at the bound: a server that an earlier test left running would otherwise
 * make the unbounded wait hang this test. */
static bool _engine_settle_bounded(int bound_ms) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  int64_t end = (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000 + bound_ms;
  for (;;) {
    bool idle = _chttpsvr_engine_reactor_pid_for_tests() == 0 &&
                !_chttpsvr_engine_reaper_unjoined_for_tests();
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

/* A logger installed with chttpsvr_set_engine_logger() serves every engine of
 * the process, not only the one that runs, or first starts, after the call.
 * Each of two full engine cycles logs its creation and its teardown through it.
 * The caller's logger is closed right after the call, and the engine doesn't
 * depend on it. An engine teardown that closed the installed logger would
 * send the second cycle to the silent fallback logger, and the file would
 * then hold one creation and one teardown.
 *
 * The logger stays installed until a later test replaces or releases it, and it
 * writes to a file that is unlinked here and whose descriptor stays open for
 * that reason.
 *
 * An earlier test may destroy its last server without waiting for the reap.
 * A running engine switches to a newly installed logger at once, so that
 * engine's teardown would add a third "destroyed" line here. The wait at the
 * top makes the test count only its own two cycles, in any order. */
TEST_F(engine_stop_fixture, engine_logger_serves_every_later_engine) {
  (void)tau; /* this test creates no background thread of its own */
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

/* A closed logger handle given to chttpsvr_set_engine_logger() is refused with
 * ccol_invalid_args, and the engine keeps the logger it had, so the next engine
 * cycle logs its creation through it.
 *
 * A setter that derived from the handle without checking it would end the
 * process with a fatal error on the closed handle. */
TEST_F(engine_stop_fixture, a_closed_engine_logger_is_refused) {
  (void)tau; /* this test creates no background thread of its own */
  char path[] = "chttpsvr_closed_engine_logger_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_TRUE(fd >= 0);
  clog lg = clog_create_from_fd(fd, CLOG_INFO, NULL);
  bool installed = lg && chttpsvr_set_engine_logger(lg) == ccol_success;
  if (lg) clog_close(lg);

  clog closed = clog_create_from_fd(2, CLOG_INFO, NULL);
  if (closed) clog_close(closed);
  ccol_retval_t closed_rv =
      closed ? chttpsvr_set_engine_logger(closed) : ccol_unexpected_failure;

  int status = 0;
  if (installed) {
    chttpsvr srv = _start_server("18840", 18840);
    (void)_get("http://127.0.0.1:18840/hello", &status);
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
  }
  ccol_retval_t release_rv = chttpsvr_set_engine_logger(CLOG_NULL);
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

/* ========================================================================== */
/*                 TEARDOWN OF THE SHARED ENGINE IS PROMPT                    */
/* ========================================================================== */

static int64_t _engine_test_mono_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* The engine's idle sweep thread ticks once a second, and the engine teardown
   joins it. The join must wake the thread instead of waiting out the rest of
   its tick, so the destroy of the last server and the chttpsvr_engine_wait()
   after it don't wait for a tick. The server starts and stops at once, so the
   sweep thread has just started its first wait. The test counts how each
   sweep thread stopped instead of timing the teardown, because a loaded
   runner under valgrind can take most of a second to tear an engine down
   anyway. A stop whose signal didn't end the wait leaves the thread to sleep
   out its tick, and that counts as stopped after a timeout, three times. The
   counters are process-wide, so the test first waits for any engine an
   earlier test left behind. Each round must still end within 10 s, a bound
   that only a hang crosses. */
TEST(engine_teardown, the_last_destroy_and_engine_wait_return_quickly) {
  char sock_path[64];
  snprintf(sock_path, sizeof(sock_path), "/tmp/chttpsvr_teardown_%d.sock",
           (int)getpid());
  unlink(sock_path);
  char host_buf[96];
  snprintf(host_buf, sizeof(host_buf), "unix://%s", sock_path);

  REQUIRE_TRUE(_engine_settle_bounded(10000));
  size_t woken0 = 0, timed_out0 = 0;
  _chttpsvr_idle_sweep_exits_for_tests(&woken0, &timed_out0);
  bool all_started = true;
  int64_t slowest_round_ms = 0;
  for (int round = 0; round < 3; round++) {
    int64_t round_start = _engine_test_mono_ms();
    chttpsvr srv = chttpsvr_create(CLOG_NULL, NULL);
    if (srv == CHTTPSVR_NULL) {
      all_started = false;
      break;
    }
    chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
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
  _chttpsvr_idle_sweep_exits_for_tests(&woken1, &timed_out1);

  REQUIRE_TRUE(all_started);
  REQUIRE_EQ(timed_out1 - timed_out0, (size_t)0);
  REQUIRE_EQ(woken1 - woken0, (size_t)3);
  REQUIRE_LT(slowest_round_ms, (int64_t)10000);
}

/* ========================================================================== */
/*          A STOP RACING A DESTROY, AND A STOP FOR NO RUNNING ENGINE         */
/* ========================================================================== */

static _Atomic bool g_s300_entered = false;
static _Atomic bool g_s300_done = false;

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

static void *_destroy_handle_thread(void *arg) {
  chttpsvr *h = (chttpsvr *)arg;
  chttpsvr_destroy(*h);
  return NULL;
}

/* Connects to port, or returns -1. */
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

/* Sends one keep-alive GET /hello on fd and reads until the body of the reply
   has arrived. Returns whether it did. */
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

/* Waits up to timeout_ms for the end of the stream on fd. */
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

/* chttpsvr_engine_stop() closes the idle keep-alive connections of every
   registered server at once, while the reap is still draining a request of
   another server that it quiesces first. The first server, registered first,
   holds a request in its handler, and the idle connection of the second has to
   see the end of its stream before that handler is released. Closing idle
   connections only when each server's turn comes would keep the second
   connection open until the first drain ends. */
TEST_F(engine_stop_fixture,
       an_engine_stop_closes_every_idle_connection_at_once) {
  _chttpsvr_set_wait_in_flight_bounds_for_tests(8000, 2000);
  char *err = NULL;
  chttpsvr a = chttpsvr_create(CLOG_NULL, &err);
  bool created = a != CHTTPSVR_NULL;
  if (!created) _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(created);
  chttpsvr_register_handler(a, CHTTP_GET, "/slow", _slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18871;
  ccol_retval_t start_rv = chttpsvr_start(a, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(a);
    chttpsvr_engine_wait();
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
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
  /* ctx is freed only once no thread can still write into it. */
  bool joined = req_started && _fx_join(tau, th, NULL);
  if (!req_started || joined) free(ctx);
  if (fd >= 0) close(fd);
  chttpsvr_engine_wait();
  chttpsvr_destroy(a);
  chttpsvr_destroy(b);
  chttpsvr_engine_wait();
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(idle_ok);
  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(closed_early);
}

/* A chttpsvr_engine_stop() that lands while chttpsvr_destroy() of the same
   server is draining a request waits for that drain, so
   chttpsvr_engine_wait() returns only after the handler has ended. The destroy
   then releases its engine reference before the engine is gone, so a server
   that starts after chttpsvr_engine_wait() keeps its new engine, its keep-alive
   connections and its listener.

   This test is non-vacuous in two ways. If the destroy left the registry of
   servers before its drain, the stop's reap would find nothing to wait for and
   chttpsvr_engine_wait() would return while the handler still sleeps
   (handler_done_at_wait is false). If on top of that releasing an engine
   reference weren't tied to the engine it was taken from, the destroy's late
   release would drop the new engine's reference and reap it under s2 (both s2
   checks fail). */
TEST_F(engine_stop_fixture, a_stop_racing_a_destroy_waits_for_its_drain) {
  _chttpsvr_set_wait_in_flight_bounds_for_tests(5000, 2000);
  char *err = NULL;
  chttpsvr srv = chttpsvr_create(CLOG_NULL, &err);
  bool created = srv != CHTTPSVR_NULL;
  if (!created) _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(created);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow300", _slow_300ms_handler,
                            NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18860;
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
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

  /* The destroy claims the quiesce of srv on its own thread and holds it at the
     hook, so the stop below is the loser on every run. */
  pthread_t destroy_th;
  bool destroy_started = false;
  if (entered) {
    _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
    destroy_started =
        pthread_create(&destroy_th, NULL, _destroy_handle_thread, &srv) == 0;
    if (destroy_started) {
      _fx_track(tau, destroy_th);
      _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();
    }
    _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  }
  /* A destroy that leaves the registry before its drain does so right after the
     hook, so we give it that moment and the stop meets the state it would meet.
     A destroy that stays registered keeps the count at 1. */
  for (int i = 0; destroy_started && i < 200 &&
                  _chttpsvr_servers_bundler_count_for_tests() != 0;
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
    /* A reap of the new engine started by the destroy's late release takes a
       moment, and it has run by now. */
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
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);

  REQUIRE_TRUE(entered);
  REQUIRE_TRUE(destroy_started);
  REQUIRE_TRUE(req_joined);
  REQUIRE_EQ(req_status, 200);
  REQUIRE_TRUE(handler_done_at_wait);
  REQUIRE_TRUE(first_ok);
  REQUIRE_TRUE(second_ok);
  REQUIRE_TRUE(fresh_ok);
}

/* A chttpsvr_engine_stop() call names the engine running at that moment and
   only that one. A call while no engine runs does nothing, and a stop request
   that the watcher thread handles after its engine is gone must not reach the
   engine that a later start brought up. The watcher delay holds the request
   between the call and the work it asks for, and a server starts in that
   window. If the request carried no engine, the watcher would stop the new
   engine and the request to it would fail. */
TEST_F(engine_stop_fixture, a_stop_never_reaches_an_engine_that_starts_later) {
  (void)tau;
  /* The watcher has to exist, which the first start of the process takes care
     of, and after the wait the engine itself is down. */
  chttpsvr warm = _start_server("18862", 18862);
  chttpsvr_destroy(warm);
  chttpsvr_engine_wait();

  _chttpsvr_set_engine_stop_watcher_delay_for_tests(300);
  /* No engine is running. */
  chttpsvr_engine_stop();
  chttpsvr s1 = _start_server("18862", 18862);
  struct timespec nap = {0, 600000000L};
  nanosleep(&nap, NULL);
  int status_idle_stop = 0;
  ccol_retval_t rv_idle_stop =
      _get("http://127.0.0.1:18862/hello", &status_idle_stop);

  /* An engine runs and the request names it. That engine then ends on its own
     and another one starts before the watcher acts. */
  chttpsvr_engine_stop();
  chttpsvr_destroy(s1);
  chttpsvr_engine_wait();
  chttpsvr s2 = _start_server("18863", 18863);
  nanosleep(&nap, NULL);
  int status_old_stop = 0;
  ccol_retval_t rv_old_stop =
      _get("http://127.0.0.1:18863/hello", &status_old_stop);
  _chttpsvr_set_engine_stop_watcher_delay_for_tests(0);

  /* Stopping the engine that runs still works. */
  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  chttpsvr_destroy(s2);

  REQUIRE_EQ((int)rv_idle_stop, (int)ccol_success);
  REQUIRE_EQ(status_idle_stop, 200);
  REQUIRE_EQ((int)rv_old_stop, (int)ccol_success);
  REQUIRE_EQ(status_old_stop, 200);
}

/* ========================================================================== */
/*                    RELEASE OF THE ENGINE LOGGER                            */
/* ========================================================================== */

static _Atomic bool g_dead_alloc_dead = false;
static _Atomic int g_dead_alloc_calls_after_death = 0;

static void _dead_alloc_check(void) {
  if (atomic_load(&g_dead_alloc_dead)) {
    atomic_fetch_add(&g_dead_alloc_calls_after_death, 1);
    /* Nothing may call this allocator once the application has destroyed it,
       including the exit of the process after this test returned. */
    fprintf(stderr, "FATAL: allocator called after it was destroyed\n");
    abort();
  }
}
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

/* chttpsvr_set_engine_logger(CLOG_NULL) closes the engine logger at once,
   so an application can close its own logger and destroy the allocator behind
   it before main returns. It's refused while an engine runs. The allocator
   aborts the process on any call after it has been marked destroyed, which
   covers the exit of the process too. This test is non-vacuous: without the
   release the call would be refused as invalid and the engine logger would be
   closed at exit through the destroyed allocator. */
TEST_F(engine_stop_fixture, releasing_the_engine_logger_frees_it_at_once) {
  (void)tau;
  static ccol_memmgmt_procs_t procs = {.malloc = _dead_malloc,
                                       .free = _dead_free,
                                       .calloc = _dead_calloc,
                                       .realloc = _dead_realloc};
  clog lg = clog_create_from_fd_mp(2, CLOG_FATAL, NULL, &procs);
  REQUIRE_TRUE(lg != CLOG_NULL);
  ccol_retval_t set_rv = chttpsvr_set_engine_logger(lg);

  chttpsvr srv = _start_server("18864", 18864);
  ccol_retval_t release_while_running = chttpsvr_set_engine_logger(CLOG_NULL);
  int status = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18864/hello", &status);
  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  ccol_retval_t release_rv = chttpsvr_set_engine_logger(CLOG_NULL);
  ccol_retval_t release_again_rv = chttpsvr_set_engine_logger(CLOG_NULL);
  clog_close(lg);
  atomic_store(&g_dead_alloc_dead, true);

  /* A later engine runs with its own fallback logger. */
  srv = _start_server("18864", 18864);
  int status2 = 0;
  ccol_retval_t get2_rv = _get("http://127.0.0.1:18864/hello", &status2);
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

/* A server stopped by chttpsvr_engine_stop() doesn't listen any more, and it
 * says so through chttpsvr_is_running() and chttpsvr_bound_port(). */
TEST_F(engine_stop_fixture, an_engine_stop_ends_the_running_state) {
  (void)tau; /* this test creates no background thread of its own */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) = _start_server("0", 0);
  bool before = false, after = true;
  uint16_t port = 0, port_after = 1;
  ccol_retval_t before_rv = chttpsvr_is_running(srv, &before);
  ccol_retval_t port_rv = chttpsvr_bound_port(srv, &port);
  chttpsvr_engine_stop();
  chttpsvr_engine_wait();
  ccol_retval_t after_rv = chttpsvr_is_running(srv, &after);
  ccol_retval_t port_after_rv = chttpsvr_bound_port(srv, &port_after);
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
