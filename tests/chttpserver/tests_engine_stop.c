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

/* pthread_timedjoin_np is a glibc extension. It is what makes the fixture
 * teardown of this file a bounded join, and not a blind pthread_join. See
 * engine_stop_fixture below. A real regression can leave a background thread
 * permanently stuck, for example with srv_engine_bundler.mutex held for
 * ever. Without the bounded join, the join itself then hangs for ever too.
 * That only moves the "the whole binary hangs instead of one test that fails
 * cleanly" problem. It moves it from "the chttpsvr_start() call of the next
 * unrelated test" to "the teardown of this exact test". It does not close
 * the problem. This macro must come before the first #include that can pull
 * in <pthread.h> indirectly. src/chttpserver.c and src/clogger.c put it in
 * the same place. */
#define _GNU_SOURCE

#include <arpa/inet.h>
#include <assert.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
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

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#include <test_sanitizer.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* The test clients of this binary write to the server with plain write(2),
 * and the server can close a connection while one of them writes. The
 * library leaves the disposition of SIGPIPE to the application, so this
 * binary ignores it itself. tests_sigpipe.c covers the library under the
 * default disposition. */
__attribute__((constructor)) static void _ignore_sigpipe_for_test_writes(void) {
  signal(SIGPIPE, SIG_IGN);
}

/* ========================================================================== */
/*                    BOUNDED-JOIN TEST FIXTURE                               */
/*                                                                            */
/* Every test below starts one or more background threads. Those threads     */
/* race the internal locks and the internal refcounts of chttpsvr. Several   */
/* tests deliberately leave a thread unjoined on their own path that finds a */
/* regression. The comment of                                                */
/* engine_stop_from_signal_handler_does_not_deadlock is one example.         */
/* To abandon a thread that way is harmless only for the very LAST test in a */
/* binary, where nothing runs after it. This file has 10 tests.              */
/* srv_engine_bundler.mutex is a single global lock for the whole process.   */
/* chttpsvr_start(), _engine_acquire() and _engine_release() all take it. A  */
/* thread that stays stuck with that lock held is a real regression, and not */
/* the ordinary case. It does not affect only the one test that found it.    */
/* Every LATER test in this binary that calls chttpsvr_start() hangs too,    */
/* the instant that it tries to take that same mutex. One clean, reported    */
/* failure then becomes a cascade. Only an external CI timeout notices that  */
/* cascade, and nothing in it says which test is really at fault.            */
/*                                                                            */
/* This fixture closes that gap where it can. It contains the gap where it   */
/* cannot. The tests below use TEST_F and TEST_F_TEARDOWN for it, and not a  */
/* bare TEST(). The fixture tracks every thread that a test body creates     */
/* with pthread_create(), through _fx_track(). Every join goes through       */
/* _fx_join(). That covers the join inside the body of a test, and the       */
/* automatic sweep of this teardown. _fx_join() is a generous but BOUNDED    */
/* join, built on pthread_timedjoin_np, and not a blind pthread_join. On a   */
/* real regression this turns "some unrelated later test hangs for no clear  */
/* reason" into "the teardown of THIS test names the thread that never       */
/* finished". The teardown then abandons that thread with pthread_detach(),  */
/* and it lets Tau continue to the next test. This is still not a full fix   */
/* for a deadlock with no bound. Nothing can force a thread to release a     */
/* lock that it never releases. It is a real improvement in two ways. The    */
/* failure is pinned to the test at fault, and not to a later victim. And it */
/* is resilient for the more common case, where a thread is only slower than */
/* the narrow internal bound of a test allows, and is not deadlocked.        */
#define ENGINE_STOP_FIXTURE_MAX_THREADS 4

/* The struct must carry its own tag, engine_stop_fixture. That tag must
 * match the typedef name exactly. TEST_F_SETUP, TEST_F_TEARDOWN and TEST_F
 * all expand FIXTURE into `struct FIXTURE`. A typedef alone, over an
 * anonymous struct, would make each such expansion declare a fresh, separate
 * and incomplete struct type. It would not refer back to this one. */
typedef struct engine_stop_fixture {
  pthread_t threads[ENGINE_STOP_FIXTURE_MAX_THREADS];
  bool active[ENGINE_STOP_FIXTURE_MAX_THREADS]; /* true = not yet joined */
  int count;
} engine_stop_fixture;

/* This function registers tid. The teardown sweep below then joins tid with
 * a bound, if the body of the test never calls _fx_join() for it. Call this
 * function exactly one time for each thread that you create successfully.
 * Call it right after pthread_create() returns 0. A test with more
 * concurrent threads than ENGINE_STOP_FIXTURE_MAX_THREADS needs a larger
 * fixture array. No test in this file needs more than 3 today. */
static void _fx_track(engine_stop_fixture *fx, pthread_t tid) {
  /* This catches a future test that tracks more threads than the
     fixed-size arrays of this fixture hold. It catches that test before it
     silently corrupts the memory past the end of those arrays. The doc
     comment of this function above gives the intended fix, which is a
     larger fixture array. Do not raise this bound casually. */
  assert(fx->count < ENGINE_STOP_FIXTURE_MAX_THREADS);
  fx->threads[fx->count] = tid;
  fx->active[fx->count] = true;
  fx->count++;
}

/* A generous but bounded join. 30 seconds is comfortably beyond every real,
 * legitimate wait in this file. The longest such wait is a bounded
 * cond_timedwait of 10s, plus a little slack. This call therefore never
 * fires for no reason against a thread that behaves correctly. It also
 * guarantees that the call itself cannot hang the caller for ever against a
 * thread that is truly stuck. It returns true only when tid really stopped
 * and this call joined it. */
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

/* This is the one join call that every test body below should use in place
 * of a bare pthread_join(). It is bounded; see _fx_timed_join above. It also
 * marks tid as handled, so that the teardown sweep does not try to join tid
 * a second time. A second join on a pthread_t that something already joined
 * is undefined behavior. It is safe to call this function on a tid that this
 * fixture never tracked. The search below then finds nothing, and the
 * function does nothing. That case is not expected in practice, because
 * _fx_track() should always track every thread that a test creates. This
 * code deliberately does not treat it as a fatal error in the test
 * infrastructure either. */
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
    /* See the opening comment of this section for why this must be
     * bounded, and not a blind pthread_join. A real regression here must
     * not cascade into a hang of every later test in this binary. */
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
/* chttpserver.h documents chttpsvr_engine_stop() as the mechanism that a    */
/* signal handler uses to start a shutdown of the engine for the whole       */
/* process. The pattern is exactly this:                                     */
/*   chttpsvr_start(srv, &cfg);                                              */
/*   chttpsvr_engine_wait();  // returns once a signal calls engine_stop()   */
/*   chttpsvr_destroy(srv);                                                  */
/* This state belongs to the whole process, and the stop is forced. It tears */
/* down the one shared ccol_event_loop reactor that every chttpsvr instance  */
/* in the process depends on. It therefore cannot share the long-lived g_srv,*/
/* g_srv2 and other handles of tests.c. The servers of that suite stay       */
/* started across its whole run. A force-stop of the shared engine in the    */
/* middle of that suite would pull the reactor out from under every one of   */
/* them, and it would fail unrelated tests. This file therefore has a binary */
/* of its own. That matches the precedent of tests_tls.c and                 */
/* tests_mem_mgmt.c in this directory, for a scenario that needs a process   */
/* largely to itself.                                                        */
/* ========================================================================== */

static void _hello_handler(chttpsvr_req *req, chttpsvr_resp *resp, void *ctx) {
  (void)req;
  (void)ctx;
  chttpsvr_resp_write_str(resp, "Hello, engine-stop!");
}

static ccol_retval_t _get(const char *url, int *status_out) {
  chttpcli cli = ccol_create_chttpclient(NULL);
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
  chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (!srv) {
    fprintf(stderr, "FATAL: ccol_create_chttpsvr failed: %s\n",
            err ? err : "?");
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

/* This test covers a call to chttpsvr_engine_stop() while srv is still
 * fully started. That is the forced path, which a signal handler uses.
 * Nothing calls chttpsvr_stop() or chttpsvr_destroy() on srv first. The
 * reaper that this call starts must reset g_servers_count whenever it frees
 * the backing array of g_servers. Without that, a later chttpsvr_destroy(srv)
 * calls _servers_unregister(srv), which dereferences a NULL g_servers[0].
 * That is a deterministic SIGSEGV, and not a rare race. A clean run of this
 * test also proves that nothing leaks and that nothing is freed two times
 * along the way. The memtest target, which runs valgrind, proves that best. */
TEST_F(engine_stop_fixture, force_stop_while_started_then_destroy_is_safe) {
  (void)tau; /* this test creates no background thread of its own */
  /* _ccol_destructor is a safety net for a REQUIRE_* failure between here
     and the explicit chttpsvr_destroy() calls below. This file has no
     _teardown() and no atexit() to catch a leaked handle at exit. A leaked
     handle leaves a server that is still registered and that still holds an
     engine reference. For srv2 it also leaves a listening socket that is
     still bound to this same port. Every later test in this binary can then
     trip over that state. This is safe here, because nothing else destroys
     srv or srv2, and nothing races their destruction. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);

  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);

  chttpsvr_engine_stop();
  chttpsvr_engine_wait();

  /* The crash that this test guards against happens inside
   * chttpsvr_destroy() itself. To reach the line after it is the real
   * assertion. */
  chttpsvr_destroy(srv);

  /* Prove that the engine came back up cleanly, and that the force-stop
   * left no global state corrupt. Start a fresh server on the same port,
   * and drive one real request through it. */
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);
  status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18796/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}

/* The quiesce pass of the reaper is _engine_force_stop_quiesce_all. It
 * walks every server that is still registered in g_servers, and not only
 * one. This test drives that loop with two servers that started
 * independently and that are both still live at the moment of the forced
 * stop. Each one holds a finished connection in its own idle list, and
 * each such connection is eligible for keep-alive. */
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

extern void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                          unsigned grace_ms);

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

/* chttpsvr_engine_stop() must still drain every request that is truly in
 * flight before it tears the reactor down. That is the documented contract
 * of _drain_and_close_all_connections. A graceful chttpsvr_destroy()
 * already does exactly this. The forced path must not abandon a request
 * that a worker thread already handles at the moment that the signal
 * fires, and it must not corrupt such a request. */
TEST_F(engine_stop_fixture,
       force_stop_drains_in_flight_request_before_reactor_teardown) {
  /* This shrinks the timing of _wait_in_flight_bounded. The test
   * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts in
   * tests.c uses this same hook in the same way. The REQUIRE_TRUE(
   * g_slow_entered) below can fail while a worker thread is truly still
   * blocked inside _slow_handler. The chttpsvr_destroy() of srv at the exit
   * of the scope, through _ccol_destructor, then forces that connection
   * unblocked in well under a second. Without the shrink, it falls back to
   * the real production sizes, which are a graceful wait of 30s plus a
   * grace period of 5s before the force-unblock. The shrink therefore turns
   * a slow stall on a test that already fails into a fast, clean report of
   * that failure. The stall is slow, but it is not infinite. This test puts
   * the real defaults back before it returns, on every path. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  char *err = NULL;
  /* _ccol_destructor: see the comment of
     force_stop_while_started_then_destroy_is_safe above. This is safe here,
     because the background client thread below only sends an HTTP request
     to srv. That thread never destroys srv itself. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (srv == CHTTPSVR_INVALID)
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
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
  /* This is on the heap, and not a local on the stack. A real regression
     below leaves th still running when the final, bounded _fx_join times
     out. That must not become a later write through a pointer that dangles
     into the stack frame of this function, which something else has reused
     by then. _stopping_race_ctx_t, a few tests below in this same file,
     uses the same reasoning. This code deliberately never frees ctx on a
     bail-out path where th may still run and may still write into it. */
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
    free(ctx); /* nothing created a thread, so nothing can still touch it */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(create_rv, 0);
  _fx_track(tau, th);

  /* Wait for proof that the request really reached the worker thread and
   * that it blocks inside _slow_handler. That is what makes it truly in
   * flight. Only then force-stop the engine. A fixed sleep here would be a
   * flaky proxy for that proof on a slow or loaded machine. Under valgrind,
   * for example, the accept, parse and dispatch path can easily take longer
   * than a delay that looks like plenty. This code therefore waits on the
   * entry signal of the handler instead. That wait has a generous bound,
   * and it never blocks for ever. */
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
    /* Without this, th leaks on this path. It is a joinable thread that
       nothing ever joins. Release the handler unconditionally. That does
       nothing when the handler truly never started. It unblocks the handler
       when the bounded wait above gave a false negative instead. Then join
       th before the failure, and do not leave it running for the rest of
       the life of this process. The release just above guarantees that the
       HTTP request of th can now finish. That is what makes it safe to
       block here on a join with no bound. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_TRUE(g_slow_entered);

  chttpsvr_engine_stop();

  /* Only now let the handler finish. The forced stop must drain this
   * in-flight request, and it must not abandon it. Without that drain, the
   * client thread below hangs past its own request timeout, and it never
   * sees a 200. */
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);

  /* This code checks the result. It is not a fire-and-forget call. th can
     somehow still run 30s later. That is a real regression, because th
     should unblock the instant that the code above sets g_slow_go. On that
     path nothing here may read ctx or free it, because th may still write
     into it. To leave ctx un-freed there is deliberate. See the comment on
     the declaration of ctx above. */
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
  /* 150ms gives the reaper thread that chttpsvr_engine_stop() starts enough
   * time to win the race to quiesce srv. It then blocks inside
   * _drain_and_close_all_connections, where it waits on the /slow-race
   * request that is still in flight below. Only after that does this code
   * let the handler finish, and therefore let that drain finish. */
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
  return NULL;
}

/* This test covers a race between two callers. The first is the background
 * reaper thread of chttpsvr_engine_stop(), which quiesces every server that
 * is still registered, and that includes srv. The second is an independent
 * chttpsvr_destroy(srv) call on this thread, at the same time. No
 * chttpsvr_engine_wait() call serializes the two. This is exactly the
 * pattern that the doc comment of chttpsvr_engine_stop() invites. That
 * comment describes the function as safe to call from a signal handler, and
 * it asks no caller to synchronize it against a concurrent
 * chttpsvr_destroy() first.
 *
 * One of the two callers loses the race to quiesce srv. That loser must
 * wait for the real teardown work of the winner to finish. It must not
 * return from _quiesce_server_once() at once, and do nothing, the instant
 * that it sees that the other caller already claimed the quiesce state of
 * srv. A loser that returns early goes straight into
 * ccol_mutex_destroy(raw->mutex), ccol_mutex_destroy(raw->idle_mutex) and
 * free(raw). The reaper thread still uses those exact objects at that
 * moment, inside _drain_and_close_all_connections. This test forces that
 * function to block for a real, measurable window, with the /slow-race
 * request that is still in flight. The result is a use-after-free and a
 * mutex that something destroys while it is in use, both at once. A clean
 * run of this test is what proves that the race stays closed. The memtest
 * target, which runs valgrind, and a build with -fsanitize=thread prove
 * that best. */
TEST_F(engine_stop_fixture, concurrent_destroy_and_engine_stop_is_safe) {
  /* This is the same safety net that
     force_stop_drains_in_flight_request_before_reactor_teardown builds with
     the same hook. A REQUIRE_* between here and chttpsvr_engine_stop() can
     fire while a worker thread is truly still blocked inside
     _slow_handler. Without the shrink, the chttpsvr_destroy() of srv at the
     exit of the scope below then falls back to the real, production-sized
     _wait_in_flight_bounded timers. This test puts the real defaults back
     before it returns, on every path. The shrink does not affect the timing
     of the main path of this test. _release_slow_handler_after_delay
     normally releases the handler after its own sleep of 150ms. That is
     well before the code reaches either the shrunk bounds or the real
     default ones. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  char *err = NULL;
  /* _ccol_destructor: see the comment of
     force_stop_while_started_then_destroy_is_safe above. This is safe here
     for one specific reason. Every REQUIRE_* that can return early sits
     strictly before the chttpsvr_engine_stop() call below. No REQUIRE_*
     sits between chttpsvr_engine_stop() and the explicit
     chttpsvr_destroy(srv) a few lines later. This destructor can therefore
     never fire at the same time as the destroy race that the test drives on
     purpose, between the reaper and this thread. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (srv == CHTTPSVR_INVALID)
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
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
  /* This is on the heap, and not a local on the stack. See the comment on
     the same ctx declaration in
     force_stop_drains_in_flight_request_before_reactor_teardown above for
     the reason. */
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
    free(ctx); /* nothing created a thread, so nothing can still touch it */
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
    /* Without this, req_th leaks on this path. Release the handler
       unconditionally. That does nothing, and is harmless, when the
       handler never started. Then join req_th before the failure. See the
       same fallback in
       force_stop_drains_in_flight_request_before_reactor_teardown for the
       full reasoning. */
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
    /* Nothing created _release_slow_handler_after_delay, so it cannot
       release the handler itself. Release the handler here instead.
       req_th is already live, and it can then still finish before this
       failure, and it does not leak. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(releaser_create_rv, 0);

  /* The documentation says that chttpsvr_engine_stop() does not block. It
   * only starts a background reaper thread. That reaper is a real
   * ccol_thread_create(). A fixed sleep here would only guess that the
   * reaper had enough time to win the race to quiesce srv before the
   * chttpsvr_destroy() below runs. On a loaded CI run, or under valgrind, a
   * guess that turns out too short silently lets the chttpsvr_destroy() of
   * this thread win the race instead. The test then drives the OTHER
   * interleaving, which is already safe, and it never fails. That is a
   * silent false pass, and it never retests the regression that this test
   * exists to catch.
   * _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests() proves
   * the same thing instead, deterministically, with no guessed timing at
   * all. It confirms that the reaper thread already claimed
   * quiesce_state == CHTTPSVR_QS_QUIESCING for srv; see the doc comment of
   * that hook. Only then does this thread call chttpsvr_destroy(). This
   * call is therefore the losing side on every single run. The code
   * releases the hook right after that, so that the reaper can go into its
   * own real teardown work. That work then blocks on the /slow-race request
   * that is still in flight, exactly as before, and the 150ms timer of
   * _release_slow_handler_after_delay releases it. This mirrors the same
   * use of this same hook in
   * fork_mid_quiesce_teardown_does_not_hang_child below. */
  extern void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void);
  extern void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void);
  extern void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void);
  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();

  chttpsvr_destroy(srv);

  _fx_join(tau, releaser_th, NULL);
  /* This code checks the result. It is not a fire-and-forget call. See the
     same reasoning in
     force_stop_drains_in_flight_request_before_reactor_teardown for why
     nothing may read ctx or free it when req_th somehow still runs 30s
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

  /* Prove that the engine came back up cleanly after this. */
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
/* This test covers a race between chttpsvr_start() and a concurrent         */
/* chttpsvr_engine_stop(). It differs from the races above, which all use a  */
/* server that is ALREADY started. A server must register itself with        */
/* servers_bundler.servers as soon as it confirms its engine reference.      */
/* That is the list that _engine_force_stop_quiesce_all walks. It must never */
/* register only at the very end of chttpsvr_start(). A forced               */
/* chttpsvr_engine_stop() can land in the window between the moment that the */
/* server takes a live engine reference and that registration. There it      */
/* cannot see the server at all. Its quiesce pass then skips the server      */
/* completely, and something can tear the reactor down while                 */
/* chttpsvr_start() still uses it a few lines below. For example,            */
/* chttpsvr_start() may be about to call ccol_event_loop_add for the         */
/* listener. That quiesce pass also never touches the started, listen_reg    */
/* and contributed_to_engine bookkeeping of such a server. A later           */
/* chttpsvr_stop() or chttpsvr_destroy() call on it therefore hands a stale  */
/* reg to ccol_event_loop_remove(). It makes that call against a reactor     */
/* that something may already have torn down, or that may be gone.           */
/* _quiesce_server_once() therefore waits for every in-flight call on this   */
/* exact server that _chttpsvr_resolve protects. The chttpsvr_start() call   */
/* of this test is one such call. Only after that wait does it touch any     */
/* state of the server. __chttpsvr_destroy makes the same wait, and the      */
/* forced path of chttpsvr_engine_stop() now makes it too. This test uses a  */
/* white-box hook to pause chttpsvr_start() inside that exact window,        */
/* deterministically. It does not rely on a fixed sleep to hit a race window */
/* a few instructions wide by chance.                                        */
/* ========================================================================== */
extern void _chttpsvr_arm_start_race_hook_for_tests(void);
extern void _chttpsvr_wait_start_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_start_race_hook_for_tests(void);

static chttpsvr g_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_race_start_rv = ccol_success;
/* The thread function below sets this flag as the very last thing that it
   does. It sets it strictly after it writes both g_race_srv and
   g_race_start_rv in full. The test below polls this flag, with a bound,
   before it reads either of those two plain globals, and before it joins
   this thread. Neither of those two globals is atomic. The atomic store and
   load pair here is therefore what really establishes a happens-before
   relationship for them. Without the poll before it, a bare pthread_join
   would be a real data race on both globals under the C11 memory model.
   That holds whether or not the join itself returns quickly in practice. */
static _Atomic bool g_race_start_returned = false;

static void *_start_racing_server_thread(void *arg) {
  (void)arg;
  char *err = NULL;
  g_race_srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (!g_race_srv) {
    fprintf(stderr, "FATAL: ccol_create_chttpsvr failed: %s\n",
            err ? err : "?");
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
  g_race_srv = CHTTPSVR_INVALID;
  g_race_start_rv = ccol_success;
  atomic_store(&g_race_start_returned, false);

  _chttpsvr_arm_start_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_racing_server_thread, NULL) != 0) {
    /* The code above already armed the hook, and now nothing ever enters
       it. _start_race_hook_wait_if_armed checks its own `go` flag again
       only after it finds that `armed` is still true. This code therefore
       sets `go` here in advance, instead of leaving it unset. A later,
       unrelated chttpsvr_start() call elsewhere in this binary can then
       land on this hook and pass through at once. Without this, such a
       call blocks for ever on a release that never comes. */
    _chttpsvr_release_start_race_hook_for_tests();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until chttpsvr_start() confirms its engine reference, registers
   * with servers_bundler, and pauses right there. That is the exact window
   * that this test drives a forced engine stop into. */
  _chttpsvr_wait_start_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* This gives the reaper thread of chttpsvr_engine_stop() time to start,
   * and to reach the wait inside _quiesce_server_once. That wait is for the
   * in-flight chttpsvr_start() call of this exact server to finish. With
   * this sleep in place, the reaper blocks there. The 150ms of
   * _release_slow_handler_after_delay above gives a racing reaper time to
   * win a similar race in the same way. This test is meaningful either way,
   * and it does not need the sleep. See the comment of that call for why
   * the block of the hook, and not this sleep, is what makes the race
   * deterministic. The sleep only makes the test land on the more
   * interesting of the two safe interleavings more often. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let chttpsvr_start() finish. A reaper that reached this server
   * blocks and waits for exactly this. Nothing can tear the shared reactor
   * down until chttpsvr_start() releases its own resolve pin. */
  _chttpsvr_release_start_race_hook_for_tests();

  /* This is a bounded poll, and not a blind pthread_join. A regression here
     makes start_th deadlock with itself for ever. This poll turns that hang
     into a clean REQUIRE_TRUE(race_returned) failure that names the test.
     Without it, the hang has no bound. This flag is also what really
     establishes a happens-before relationship for g_race_srv and
     g_race_start_rv below. start_th wrote both of them, and neither is
     atomic. To read them, or to join start_th, without a check of this flag
     first would be a real data race under the C11 memory model. That holds
     whether or not the join itself returns quickly in practice. */
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
    return; /* a join would hang for ever; nothing more to check safely */
  _fx_join(tau, start_th, NULL);
  /* g_race_srv is a global. The racing background thread writes it, so it
     cannot carry a scope-exit _ccol_destructor the way that a local handle
     can. This code therefore cleans it up explicitly before it asserts on
     race_rv here. A REQUIRE_* failure at this exact point then does not
     leak it. This file has no _teardown() and no atexit() to catch such a
     leak. A leaked server here stays registered, and it still holds an
     engine reference, for every later test in this binary. Every REQUIRE_*
     below this point already sits after the final chttpsvr_destroy() call
     for g_race_srv further down. None of them needs the same treatment. */
  ccol_retval_t race_rv = g_race_start_rv;
  if (race_rv != ccol_success) {
    if (g_race_srv != CHTTPSVR_INVALID) chttpsvr_destroy(g_race_srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)race_rv, (int)ccol_success);
  }

  chttpsvr_engine_wait();

  /* The real assertion is that neither of these crashes and neither hangs.
   * The early registration and the wait in the quiesce pass above are what
   * make that true. Without them, the listen_reg of g_race_srv can point
   * into a ccol_event_loop that something already destroyed by this point.
   * chttpsvr_destroy() is then a use-after-free. */
  chttpsvr_destroy(g_race_srv);

  /* Prove that the engine came back up cleanly after this. */
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
/* These two tests cover the same class of deadlock as the test just above.  */
/* They cover a narrower and deeper window. In that window a                  */
/* chttpsvr_start() call already registered, and it already confirmed        */
/* contributed_to_engine. It is still inside its own _engine_acquire() call, */
/* or about to enter it. It races a reap that already claimed the quiesce of */
/* this exact server, and that blocks and waits for the resolve pin of this  */
/* call to drop. _engine_acquire() must therefore not block on               */
/* srv_engine_bundler.stopping while it still holds that pin. The reap can   */
/* then never finish, because it needs that pin to drop first. That is a     */
/* permanent deadlock, and not only a slow race. Two independent triggers    */
/* reach it. The first is a forced chttpsvr_engine_stop() call. The second   */
/* is the graceful reap that runs when the chttpsvr_destroy() of some other  */
/* server drops the last reference to the shared engine. Both variants below */
/* use a dedicated white-box hook. That hook is                              */
/* _chttpsvr_arm_engine_stopping_race_hook_for_tests, with its wait sibling  */
/* and its release sibling. It pauses right before the _engine_acquire()     */
/* call of the racing chttpsvr_start() call. This lands in exactly that      */
/* window, deterministically. It does not rely on a fixed sleep to hit by    */
/* chance a race that is only a few instructions wide. Each test creates its */
/* own S1, and neither relies on an earlier test in this binary to leave a   */
/* reactor live. Both are therefore self-contained, and the order in which   */
/* they run does not matter.                                                 */
/* ========================================================================== */
extern void _chttpsvr_arm_engine_stopping_race_hook_for_tests(void);
extern void _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_engine_stopping_race_hook_for_tests(void);

/* This is private state for each run of the two races below. Those races
 * are a forced reap against a concurrent start, and a graceful reap against
 * a concurrent start. The tests are
 * start_racing_a_concurrent_forced_reap_retries_and_succeeds and
 * start_racing_a_concurrent_graceful_reap_retries_and_succeeds. The two must
 * not share one file-scope global triple of srv2, start_rv and
 * start_returned. _destroy_hang_ctx_t in tests.c documents the reason for
 * the same class of bug. The two tests run one after the other. The bounded
 * poll of each one can bail out; see the opening comment of
 * ENGINE_STOP_FIXTURE. That path deliberately leaves start_th tracked but
 * unjoined, and it does not block on a thread that looks stuck. Such a
 * thread may be only slow, and not truly hung. It can then write into a
 * shared global after the NEXT test already reset that global, while that
 * next test races its own fresh thread for the same globals. The later test
 * then sees a value that neither of its own two writers produced. A ctx on
 * the heap for each run of a test makes that structurally impossible. This
 * code frees a ctx only after _fx_join confirms that the thread that owns it
 * stopped. An abandoned thread from an earlier test can therefore only ever
 * write into its OWN ctx, which is still live. It can never write into the
 * ctx of a later test. */
typedef struct {
  uint16_t port;
  chttpsvr srv;
  ccol_retval_t start_rv;
  /* The thread function below sets this flag as the very last thing that it
     does. It sets it strictly after it writes both srv and start_rv in
     full. Every caller polls this flag, with a bound, before it reads either
     of those two plain fields, and before it joins this thread. Neither of
     those two fields is atomic. Without that poll, a read of them, or a
     join of the thread, is a real data race under the C11 memory model. */
  _Atomic bool returned;
} _stopping_race_ctx_t;

static void *_start_stopping_race_server_thread(void *arg) {
  _stopping_race_ctx_t *ctx = (_stopping_race_ctx_t *)arg;
  char *err = NULL;
  ctx->srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  if (!ctx->srv) {
    fprintf(stderr, "FATAL: ccol_create_chttpsvr failed: %s\n",
            err ? err : "?");
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

TEST_F(engine_stop_fixture,
       start_racing_a_concurrent_forced_reap_retries_and_succeeds) {
  chttpsvr srv1 = _start_server("18820", 18820);

  _stopping_race_ctx_t *ctx =
      (_stopping_race_ctx_t *)calloc(1, sizeof(_stopping_race_ctx_t));
  if (!ctx) {
    /* srv1 is already live at this point. It is registered, and it holds an
       engine reference. Destroy it before the failure, so that a temporary
       lack of memory here does not leak it for the rest of the run of this
       binary. This matches the cleanup for a pthread_create failure a few
       lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18821;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* The code above already armed the hook, and now nothing ever enters
       it. _engine_stopping_race_hook_wait_if_armed checks its own `go` flag
       again only after it finds that `armed` is still true. This code
       therefore sets `go` here in advance, instead of leaving it unset. A
       later, unrelated chttpsvr_start() call elsewhere in this binary can
       then land on this hook and pass through at once. Without this, such a
       call blocks for ever on a release that never comes. */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* nothing created a thread, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until the racing chttpsvr_start() call confirms its engine
   * contribution, registers with servers_bundler, and pauses right before
   * its own _engine_acquire() call. That is the exact window that this test
   * targets. */
  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* This gives the reaper thread of chttpsvr_engine_stop() time to start,
   * to quiesce srv1, and to reach the wait inside _quiesce_server_once.
   * srv1 is also registered, and this same call also force-stops it. That
   * wait is for the in-flight chttpsvr_start() call of srv2 to finish. With
   * this sleep in place, the reaper correctly does NOT block there. This
   * test is meaningful either way, and it does not need the sleep. The
   * sleep only makes the test land on the more interesting of the two safe
   * interleavings more often. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let the racing chttpsvr_start() call go into _engine_acquire(). It
   * sees that stopping == true. It returns at once, and it does not block.
   * The caller then unwinds and retries. It does not deadlock against the
   * reaper. */
  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* This is a bounded poll, and not a blind pthread_join. It establishes a
     real happens-before relationship for ctx->srv and ctx->start_rv below.
     start_th wrote both of those fields, and neither is atomic. This poll
     runs before this thread reads them, and before it joins start_th. The
     comment on ctx->returned above gives the same reasoning. The poll also
     turns a real regression, where start_th deadlocks with itself, into a
     clean REQUIRE_TRUE failure that names the test. Without it, the hang
     has no bound. This code deliberately does NOT free ctx on the bail-out
     branch below. start_th may still run, and it goes on to write into ctx;
     see the comment on the type of ctx. The bounded-join sweep in the
     teardown of the fixture is what finally joins start_th or abandons it.
     This function never does. */
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
    /* srv1 has nothing to do with the stuck start_th. It is therefore
       still safe to destroy it directly here. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(stopping_race_returned);
    return; /* a join would hang for ever; nothing more to check */
  }
  _fx_join(tau, start_th, NULL);

  /* The force-stop above already quiesced srv1. This code still destroys
   * srv1 explicitly, to release the handle. That matches the convention of
   * this file for a server that something force-stops while it is live. */
  chttpsvr_destroy(srv1);

  /* The real assertion is that chttpsvr_start() on srv2 retries and
   * succeeds, and that it does so transparently. It must not pass the
   * temporary race against the engine stop to its own caller as a hard
   * failure. Without that retry, nothing ever reaches this line, because
   * the racing thread deadlocks for ever. The code above confirmed that it
   * joined start_th. ctx is therefore safe to read and to free from here
   * on. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_INVALID) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* There is no chttpsvr_engine_wait() call here. The racing server of the
   * start-race test above never contributes an engine reference before the
   * reap catches it and tears it down with itself. This call is different.
   * It succeeds through a transparent retry AFTER the first reap fully
   * finishes. srv2 is therefore a correctly RUNNING server now, on a
   * reactor that the library just created again, and nothing asks it to
   * stop. A wait for the engine to stop fully at this point would wait for
   * ever for a stop that never comes. */

  /* Prove that srv2 really works. It is not enough that chttpsvr_start()
   * returned success. This code captures the result into a local, and it
   * destroys srv2 unconditionally, before either REQUIRE_* below. A real
   * regression in this exact retry-then-serve path is what this test exists
   * to catch. Without this order, such a regression can leave a live server
   * behind that still holds an engine reference. Every later test in this
   * binary could then trip over it. */
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
    /* srv1 is already live at this point. It is registered, and it holds an
       engine reference. Destroy it before the failure, so that a temporary
       lack of memory here does not leak it for the rest of the run of this
       binary. This matches the cleanup for a pthread_create failure a few
       lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18823;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* See the same comment in the sibling forced-reap test above. It says
       why this code releases a hook that nothing ever entered, instead of
       only disarming it. */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* nothing created a thread, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  /* srv1 is the only OTHER live server that references the shared engine
   * at this point. The racing chttpsvr_start() call of srv2 pauses before
   * it ever calls _engine_acquire(), so it has not incremented the shared
   * reactor_refs counter itself. A destroy of srv1 therefore drops
   * reactor_refs to 0. That starts the graceful reap path, which is
   * _engine_release(). The sibling test above drives the forced
   * chttpsvr_engine_stop() path instead. Both paths feed the same chain
   * from here on, which is _engine_reaper_fn ->
   * _engine_force_stop_quiesce_all() -> _quiesce_server_once(). The rest of
   * this test is therefore the same. */
  chttpsvr_destroy(srv1);

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* This is a bounded poll, and not a blind pthread_join. See the same
     comment in the sibling forced-reap test above for the reason. The code
     above already destroyed srv1, and that is what starts the graceful reap
     that this test drives. There is therefore nothing left to clean up on
     the bail-out path here. */
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
               code leaks ctx here on purpose; see its type comment. */
  _fx_join(tau, start_th, NULL);

  /* The code above confirmed that it joined start_th. ctx is therefore
     safe to read and to free. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_INVALID) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* There is no chttpsvr_engine_wait() call here. See the same comment in
   * the sibling forced-reap test above for the reason. srv2 is a correctly
   * RUNNING server now, on a reactor that the library just created again,
   * and nothing asks it to stop. */

  /* This code captures the result into a local, and it destroys srv2
     unconditionally, before either REQUIRE_* below. See the same comment in
     the sibling forced-reap test above for the reason. */
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
/* chttpserver.h documents chttpsvr_engine_stop() as "async-signal-safe:     */
/* safe to call from a signal handler". The whole point is that an           */
/* application installs it, or a thin wrapper around it, as a SIGTERM or     */
/* SIGINT handler for a graceful shutdown. Its real work must therefore NOT  */
/* run directly on whichever thread called it. It takes                      */
/* ccol_mutex_lock(srv_engine_bundler.mutex). When a reactor is live, it     */
/* also calls ccol_thread_create() through _spawn_reaper(). POSIX guarantees */
/* neither of those two to be async-signal-safe. To run them there is a      */
/* self-deadlock that a real program can reach, and not a theoretical one.   */
/* _engine_acquire(), which chttpsvr_start() calls, also takes               */
/* srv_engine_bundler.mutex. So do _engine_release(), which                  */
/* chttpsvr_destroy() calls, and the chttpsvr_set_engine_*() setters. A      */
/* signal can arrive on the thread that is inside any one of those calls.    */
/* SIGTERM in the middle of chttpsvr_start() is one example, and it is a     */
/* realistic race during the shutdown or the rolling restart of a container. */
/* The ccol_mutex_lock() of the handler then deadlocks against the lock that */
/* the same thread already holds. That hangs the whole process, and only     */
/* SIGKILL ends it.                                                          */
/*                                                                            */
/* Every step that touches a mutex or ccol_thread_create therefore runs on a */
/* dedicated watcher thread that always runs. That thread is                 */
/* g_engine_stop_watcher in chttpserver.c. sem_post() wakes it, and that is  */
/* the one synchronization primitive that POSIX explicitly lists as          */
/* async-signal-safe. chttpsvr_engine_stop() itself therefore never calls    */
/* ccol_mutex_lock or ccol_thread_create, directly or indirectly, whatever   */
/* the interrupted thread does.                                              */
/*                                                                            */
/* This test reproduces the exact hazard deterministically, and it relies on */
/* no timing. A white-box hook locks srv_engine_bundler.mutex itself. It     */
/* then calls raise() for SIGUSR1 while it still holds that lock. raise() in */
/* a program with more than one thread runs the handler of the signal        */
/* synchronously, on the same thread, before it returns. That models "a      */
/* signal interrupts a thread that already holds this mutex" exactly. The    */
/* SIGUSR1 handler that the test installs calls chttpsvr_engine_stop().      */
/* Without the watcher thread, this sequence of calls deadlocks for ever.    */
/* The bounded wait below turns that into a clean test failure, and it does  */
/* not hang the whole suite.                                                 */
/* ========================================================================== */
extern void _chttpsvr_test_hold_engine_mutex_and_signal_self(int sig);

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
  /* A server that started guarantees that the shared engine already exists
   * and is ready before this test provokes the race. It also guarantees the
   * same for the signal-safe stop watcher. _engine_acquire() starts that
   * watcher as the very first thing that it does under
   * srv_engine_bundler.mutex. */
  /* This handle deliberately carries no _ccol_destructor scope, unlike the
     other tests in this file. The REQUIRE_TRUE(done) a few lines below can
     legitimately fail on a real regression. In that case the thread that
     this test starts below deadlocked with itself for ever, and it still
     holds srv_engine_bundler.mutex. An automatic chttpsvr_destroy(srv) at
     the exit of the scope would then fire at that exact point. It would
     itself block for ever as it tries to take that same mutex, through
     _engine_release() inside _quiesce_server_once(). That turns a clean,
     reported test failure back into the whole-process hang that this file
     exists to avoid elsewhere. This code therefore cleans srv up
     explicitly, and only at the two earlier checkpoints below. No such
     regression can be in progress yet at those two points, because nothing
     has touched srv_engine_bundler.mutex on any other thread there. On the
     done==false path this code deliberately leaves srv un-cleaned-up. The
     documented reasoning of that path says why. */
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

  /* This is a bounded wait, and not a join. A regression here makes the
   * thread above deadlock with itself for ever, inside
   * chttpsvr_engine_stop(). The handler of SIGUSR1 calls that function
   * synchronously, while it still holds srv_engine_bundler.mutex. A
   * REQUIRE_TRUE(done) failure below is the signal of that regression. The
   * teardown of this fixture then tries one more bounded join on th, with a
   * limit of 30s. If that join fails, the teardown detaches th, and it does
   * not hang the other tests of this binary. See the comment of the
   * engine_stop_fixture teardown above for why that is safe, although th
   * may hold srv_engine_bundler.mutex for ever on this path. */
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
  if (!done) return; /* a join would hang for ever; nothing more to check */
  _fx_join(tau, th, NULL);

  /* chttpsvr_engine_stop() really ran. It did not only return without a
   * deadlock. Confirm that the engine really stops. */
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  /* Prove that the engine comes back up cleanly after this. Its watcher
   * thread survives a cycle of stop and start. Only the exit of the process
   * tears that watcher down. A scoped handle is safe from here on. To reach
   * this point already proves that done==true. The self-deadlock scenario,
   * which is why srv above carries no scope, therefore cannot be in
   * progress. */
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
/* This test covers a deadlock between chttpsvr_start() and a concurrent     */
/* force-quiesce pass of chttpsvr_engine_stop(). The server in that race is  */
/* already registered, and it is stopped now.                                */
/*                                                                            */
/* chttpsvr_start() holds its own _chttpsvr_resolve() pin, which is          */
/* pending_resolve_count, for its whole call. It holds that pin while it     */
/* checks whether a _quiesce_server_once pass for this exact server is       */
/* already in progress, which is quiesce_state == CHTTPSVR_QS_QUIESCING.     */
/* _quiesce_server_once itself first blocks until pending_resolve_count      */
/* reaches 0. It does that before any of its real teardown work, which is    */
/* the work that finally reaches CHTTPSVR_QS_QUIESCED and unblocks a waiter. */
/* chttpsvr_start() must therefore not wait on quiesce_done_cv while it      */
/* STILL holds that exact pin. The two calls then deadlock each other.       */
/* chttpsvr_start() waits for a signal that only _quiesce_server_once can    */
/* send, and _quiesce_server_once waits for a pin that only                  */
/* chttpsvr_start() can release. This race looks ordinary. It is a perfectly */
/* normal restart, which is a chttpsvr_stop() and then a chttpsvr_start(),   */
/* against a concurrent chttpsvr_engine_stop(). It is not a contrived edge   */
/* case.                                                                     */
/*                                                                            */
/* chttpsvr_start() therefore releases its pin in full before it backs off   */
/* and resolves the handle again from the start. It uses the same            */
/* _chttpsvr_resolve_unpin that every other exit path already uses. It does  */
/* not block while it is still pinned.                                       */
/*                                                                            */
/* This test reproduces the exact interleaving deterministically. It uses a  */
/* dedicated white-box hook. That hook pauses chttpsvr_start() right after   */
/* it resolves and takes the pin, and before it does anything else. A fixed  */
/* sleep would only land two threads at the right instant by chance. The     */
/* hook guarantees instead that the racing chttpsvr_start() call already     */
/* holds its pin before the reaper thread of chttpsvr_engine_stop() reaches  */
/* the pending_resolve_count wait of _quiesce_server_once for it.            */
/* ========================================================================== */
extern void _chttpsvr_arm_start_resolve_race_hook_for_tests(void);
extern void _chttpsvr_wait_start_resolve_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_start_resolve_race_hook_for_tests(void);

/* Each call site below allocates this struct on the heap with calloc().
 * It is not a shared file-scope global, and it is not a local on the stack
 * of a caller. _stopping_race_ctx_t above and _destroy_hang_ctx_t in
 * tests.c have the same reason. Both call sites below deliberately leave
 * restart_th tracked but unjoined on their own bail-out path, after their
 * bounded poll, when that thread looks stuck. They do not block on a thread
 * that may be truly deadlocked. That deadlock IS the regression that
 * start_does_not_deadlock_against_concurrent_quiesce_pass exists to catch.
 * With a shared global for start_rv and returned, an abandoned thread from
 * one test could corrupt the freshly reset copy of the other test once it
 * finally finishes. Such a thread may be only slow, and not truly hung. An
 * arg on the stack would be worse still, because the struct itself is gone
 * once the test function that bails out returns. A heap allocation for each
 * run, which this code frees only after _fx_join confirms that the thread
 * that owns it stopped, makes both classes of corruption structurally
 * impossible. The thread function below reads srv and port exactly one
 * time, synchronously, at its very top; see its own comment. Unlike
 * start_rv and returned, those two would therefore be safe as plain fields
 * either way. They stay in this struct anyway, because the other two fields
 * already put it on the heap. */
typedef struct {
  chttpsvr *srv;
  int port;
  ccol_retval_t start_rv;
  /* The thread function below sets this flag as the very last thing that it
     does. It sets it strictly after it writes start_rv in full. Every
     caller polls this flag, with a bound, before it reads start_rv, and
     before it joins this thread. Without that poll, each of those two is a
     real data race under the C11 memory model. */
  _Atomic bool returned;
} resolve_racing_start_arg_t;

/* The caller passes port and srv in, and this code does not hardcode them.
   This restart therefore always targets the exact port and the exact handle
   that the srv of the calling test really started on. The two call sites
   below start srv on different ports of their own. This function reads both
   fields exactly one time, here. It does that before it does anything else
   that the racing main thread can see. That main thread goes past its own
   "hook entered" wait only after this call resolves srv and pauses. Neither
   field therefore needs the heap allocation that start_rv and returned
   need for a safe late write. This code writes those two at the very end,
   and possibly much later on the path of the regression under test. See the
   comment of the struct above. */
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
  /* srv deliberately carries no scoped destructor, unlike most of the other
     tests in this file. The whole premise of this test is that the
     chttpsvr_start() call of restart_th on srv can become permanently
     stuck, under the exact regression that the test guards against. That is
     a deadlock across two condition variables, against the reaper thread.
     __chttpsvr_destroy() would then also block for ever, as it waits for
     the pending_resolve_count of srv to drain. A scoped destructor at the
     exit of this function would fire on the `!returned` bail-out path. That
     path exists exactly to avoid a hang of the whole binary when this
     regression fires. The destructor would hang the whole binary again at
     once, and it would defeat the whole point of the bounded wait a few
     lines below. Three other places apply the same reasoning to their own
     "a background thread can hold the pin of my server for ever" scenario.
     They are engine_stop_from_signal_handler_does_not_deadlock,
     start_racing_engine_stop_and_destroy_does_not_free_raw_too_early, and
     both fork tests in this same file. Every early-exit path below where
     nothing races srv yet is proven safe, and it destroys srv explicitly
     instead. The `!returned` bail-out deliberately does not. */
  char *err = NULL;
  chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18807;
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* This is an ordinary stop. srv stays registered in servers_bundler, and
     it keeps its engine reference, because chttpsvr_stop() only closes the
     listener. That is exactly the state that a real restart starts from,
     where a chttpsvr_stop() comes first and a chttpsvr_start() follows. */
  chttpsvr_stop(srv);

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* srv is already registered at this point, and it still holds an engine
       reference. It is stopped, and nothing destroyed it. Nothing races it
       yet either, because nothing entered the race hook. It is therefore
       safe to destroy it explicitly here, unlike on the `!returned`
       bail-out further below. See the opening comment of this test. */
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  restart_arg->srv = &srv;
  restart_arg->port = 18807;
  pthread_t restart_th;
  if (pthread_create(&restart_th, NULL, _resolve_racing_start_thread,
                     restart_arg) != 0) {
    /* Nothing races srv yet, so it is still safe to destroy it directly
       here. The code above already armed the hook, and now nothing ever
       enters it. _start_resolve_race_hook_wait_if_armed checks its own `go`
       flag again only after it finds that `armed` is still true. This code
       therefore sets `go` here in advance, instead of leaving it unset. A
       later, unrelated chttpsvr_start() call elsewhere in this binary can
       then land on this hook and pass through at once. Without this, such a
       call blocks for ever on a release that never comes. */
    _chttpsvr_release_start_resolve_race_hook_for_tests();
    free(restart_arg); /* nothing created a thread */
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, restart_th);

  /* This is deterministic. It returns only after the chttpsvr_start() call
     of restart_th resolves srv, takes its pin, and pauses at the hook. At
     that point it has touched nothing else. No luck with the timing is
     needed. */
  _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* The documentation says that chttpsvr_engine_stop() does not block. It
     only starts a background reaper thread. */
  chttpsvr_engine_stop();

  /* This gives the reaper thread time to run, to reach
     _quiesce_server_once for srv, to claim CHTTPSVR_QS_QUIESCING, and to
     block on pending_resolve_count. The hook above guarantees that
     pending_resolve_count is not zero at that exact moment, because
     restart_th holds its pin. This matches the 150ms precedent that this
     file uses elsewhere to let the other side reach its own point where it
     blocks. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* This is the real moment under test. It releases the paused
     chttpsvr_start() call of restart_th. That call must not wait on
     quiesce_done_cv while it still holds the exact pin that the reactor
     thread above waits for. Such a call deadlocks permanently against that
     thread, across two condition variables. It must release that pin and
     back off instead. */
  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* This is a bounded wait, and not a blind pthread_join. A regression here
     makes restart_th deadlock with itself for ever. The reaper thread of
     the reactor then does the same, and so does the chttpsvr_engine_wait()
     call below. A REQUIRE_TRUE(returned) failure is the signal of that
     regression. The teardown of this fixture then tries one more bounded
     join on restart_th, with a limit of 30s. If that join fails, the
     teardown detaches restart_th, and it does not hang the other tests of
     this binary. engine_stop_from_signal_handler_does_not_deadlock sets the
     same precedent elsewhere in this file. */
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
                code leaks restart_arg on purpose; see its type comment. */
  _fx_join(tau, restart_th, NULL);

  /* restart_th has now provably returned, so nothing races srv any more.
     srv is therefore safe to destroy unconditionally from here on, and
     restart_arg is safe to read and to free. This code captures the result
     into a local and cleans up before it asserts. A real regression in the
     chttpsvr_start() call of restart_th is the real assertion of this test.
     Without this order, such a regression can leave a live srv behind that
     still holds an engine reference. Every later test in this binary could
     then trip over it. */
  ccol_retval_t restart_rv = restart_arg->start_rv;
  free(restart_arg);
  if (restart_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  }

  /* There is deliberately no chttpsvr_engine_wait() before this next check.
     The chttpsvr_start() call of restart_th succeeded, so it necessarily
     took a FRESH engine reference and a fresh reactor. The
     chttpsvr_engine_stop() call above already tore the original one down,
     and this restart raced that call. Nothing has asked the new reactor to
     stop yet. chttpsvr_engine_wait() would simply block and wait for it.
     That says nothing about whether the deadlock under test happened. A
     successful restart of srv, plus a real request that it serves below, is
     itself the proof that chttpsvr_engine_stop() ran to completion. Its own
     reaper thread had to finish the release of the old reactor before the
     _engine_acquire of this restart could create a new one.
     The real assertion is that srv restarted cleanly and truly. It is not
     enough that chttpsvr_start returned success while the reactor is still
     wedged. */
  int status2 = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18807/hello", &status2);

  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status2, 200);
}

/* ========================================================================== */
/* This test covers a use-after-free hazard in the CHTTPSVR_QS_QUIESCING     */
/* backoff branch of chttpsvr_start(). That branch must register itself in   */
/* quiesce_waiters BEFORE it releases its resolve pin, and never after. A    */
/* release of the pin first can at once unblock the pending_resolve_count    */
/* wait of a concurrent _quiesce_server_once() pass. Here the forced-quiesce */
/* reaper of chttpsvr_engine_stop() drives that pass. The pass can then run  */
/* to completion and find that quiesce_waiters is still zero, because this   */
/* call has not taken raw->mutex again to increment it yet. A truly          */
/* concurrent chttpsvr_destroy() call on the very same handle then sees that */
/* pass finish, and it frees raw. chttpsvr_start() is still on its way back  */
/* to raw->mutex, for the first time since it released its pin. It then      */
/* locks memory that something already freed, which is a use-after-free. An  */
/* increment of quiesce_waiters in the same critical section that sees       */
/* CHTTPSVR_QS_QUIESCING, strictly before the release of the pin, is what    */
/* makes that impossible. See the doc comment of that branch in              */
/* chttpserver.c for the full account.                                       */
/*                                                                            */
/* This test drives that case deterministically, with a dedicated white-box  */
/* hook. That hook pauses a real chttpsvr_start() call exactly in that       */
/* window. At that point the call has registered itself in quiesce_waiters   */
/* and released its resolve pin, and it has not taken raw->mutex again. The  */
/* test then drives two things around that paused call. The first is the     */
/* reaper thread, which that pin release unblocks, and which runs its own    */
/* real teardown work to completion with no hook of its own. That work is    */
/* nearly instant here. The second is a truly concurrent chttpsvr_destroy()  */
/* call. That destroy call must stay blocked for as long as this call stays  */
/* paused. It blocks on the servers_bundler_pins of the reaper, which are    */
/* still outstanding. The wait of the reaper for the quiesce_waiters         */
/* registration of this call to clear is what keeps those pins alive. This   */
/* proves that raw survives that exact window. It is stronger than an        */
/* assertion that nothing crashed after the fact. This test is not vacuous.  */
/* Move the quiesce_waiters++ to after the pin release and the relock, and   */
/* run again under AddressSanitizer: the test then fails reliably, with a    */
/* heap-use-after-free on raw->mutex.                                        */
/* ========================================================================== */
extern void _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests(void);
/* This function is bounded inside, at 10s. It returns false instead of a
   hang with no bound when the paused chttpsvr_start() call of restart_th
   never reaches this hook at all. See the doc comment of this function in
   chttpserver.c for why that can happen when the machine is loaded. Every
   other hook in this file is different. */
extern bool _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests(
    void);
extern void _chttpsvr_release_start_quiescing_unpinned_race_hook_for_tests(
    void);

static chttpsvr g_start_uaf_race_srv = CHTTPSVR_INVALID;
static _Atomic bool g_start_uaf_race_destroy_returned = false;

static void *_start_uaf_race_destroy_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_start_uaf_race_srv);
  atomic_store(&g_start_uaf_race_destroy_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       start_racing_engine_stop_and_destroy_does_not_free_raw_too_early) {
  /* This test uses the same discipline as every other test in this file
     that drives a hook. It captures each intermediate outcome into a local,
     and it does not assert on that outcome at once. It also releases every
     armed hook, and joins every thread that it created, unconditionally,
     before any REQUIRE_* runs. An assertion that fails here can therefore
     never leave a thread parked for ever inside one of the two hooks below.
     Such a thread would wedge every later test in this binary that touches
     the same machinery. */
  char *err = NULL;
  chttpsvr srv = ccol_create_chttpsvr(CLOG_INVALID, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18816;
  /* srv carries no scoped destructor, unlike most of the other tests in
     this file. srv is about to alias g_start_uaf_race_srv.
     _start_uaf_race_destroy_thread below destroys that alias directly, on a
     background thread. The sequential_double_destroy_is_fatal test of this
     codebase proves that a second destroy of the same handle is fatal, even
     when the two do not run at the same time. A scoped destructor at the
     exit of this scope, on top of the destroy of that thread, would be
     exactly such a second destroy. This one early window has no such
     conflict, because nothing has assigned g_start_uaf_race_srv yet and no
     background thread touches srv yet. A start failure here is therefore
     cleaned up explicitly instead. */
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }
  /* This is the same ordinary stop-then-restart starting state as in
     start_does_not_deadlock_against_concurrent_quiesce_pass above. srv
     stays registered, and it keeps its engine reference. That matches a
     real restart, where a chttpsvr_stop() comes first and a
     chttpsvr_start() follows. */
  chttpsvr_stop(srv);
  g_start_uaf_race_srv = srv;

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* g_start_uaf_race_srv aliases srv. Nothing has started a background
       thread at this point, because that happens only a few lines below. To
       destroy srv directly and to reset the alias is therefore safe here.
       The `!returned` bail-out further below is different, because a
       background thread may already race srv there. See the opening comment
       of this test. */
    chttpsvr_destroy(srv);
    g_start_uaf_race_srv = CHTTPSVR_INVALID;
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
    free(restart_arg); /* nothing created a thread */
  }
  if (restart_th_created)
    _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* Arm the hook for the vulnerable window before this code releases
     restart_th. After that release, restart_th sees CHTTPSVR_QS_QUIESCING,
     which the reaper below sets. It then registers itself in
     quiesce_waiters, releases its own resolve pin, and pauses right there.
     It pauses before it takes raw->mutex for the first time since that pin
     release. */
  _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests();

  if (restart_th_created) {
    chttpsvr_engine_stop(); /* the documentation says it does not block */
    /* This gives the reaper thread time to run, to reach
       _quiesce_server_once for srv, to claim CHTTPSVR_QS_QUIESCING, and to
       block on pending_resolve_count. That count is not zero at that exact
       moment, because restart_th still holds its pin. This matches the
       150ms precedent that this file uses elsewhere to let the other side
       reach its own point where it blocks. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
  }

  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* The other tests in this file that drive a hook are different.
     restart_th_created alone does not prove that the code reaches this
     hook. It also depends on the reaper, which chttpsvr_engine_stop() above
     starts. That reaper must set CHTTPSVR_QS_QUIESCING before the paused
     chttpsvr_start() call of restart_th checks quiesce_state again. This
     test enforces that order only with the fixed nanosleep of 150ms above,
     and not with a real synchronization primitive. The wait function itself
     is bounded inside, at 10s. It returns false instead of a hang of this
     whole binary when a loaded machine makes the code miss that race. See
     its own doc comment in chttpserver.c. */
  bool hook_entered = false;
  if (restart_th_created) {
    hook_entered =
        _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests();
  }
  bool paused_at_vulnerable_window = !restart_th_created || hook_entered;

  /* At this exact point, restart_th has registered itself in
     quiesce_waiters and released its own resolve pin in full. It is paused,
     and it has not touched raw->mutex again. That pin release unblocks the
     reaper that engine_stop() drives. This code leaves that reaper to run
     its own real teardown to completion. That work is nearly instant here,
     because srv has no live connection to drain, and because the reaper
     side deliberately carries no hook. The tail wait of that reaper for
     quiesce_waiters == 0 must find the registration of restart_th still
     there, and it must correctly block. That keeps raw alive, and it keeps
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

  /* This is a bounded negative check. chttpsvr_destroy() must not be able
     to return, and to free raw, while restart_th is still parked in the
     exact window that the guard protects. This is direct, positive proof
     that the guard works. Without the guard, the concurrent reaper pass
     above finds that quiesce_waiters is still zero. It finishes its own
     teardown, and it lets this destroy call return and free raw. That
     happens well before restart_th gets back to raw->mutex. */
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

  /* Release restart_th. It then takes raw->mutex again, which is still
     safe, per the assertion just above. It sees CHTTPSVR_QS_QUIESCED, it
     decrements its own registration, and it resolves srv again. That
     resolve now fails, because destroy_th already marked the slot as not in
     use by this point. This call is safe and harmless even when restart_th
     never reached the hook. */
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
      /* This code confirmed that it joined restart_th. restart_arg is
         therefore safe to read and to free. On the !restart_returned path
         this code deliberately leaks restart_arg and never frees it,
         because restart_th may still run and go on to write into it. See
         the comment on the type of the struct above. */
      restart_rv = restart_arg->start_rv;
      free(restart_arg);
    }
  }
  /* The code above already freed restart_arg when !restart_th_created. It
     does that right after pthread_create fails. There is nothing more to do
     for that case here. */

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
    /* Nothing created destroy_th. Either pthread_create failed, or
       hook_entered came back false; see the hook_entered comment of this
       test above. Nothing else in this test then releases the engine
       reference of srv. restart_th restarted srv successfully, because
       nothing destroyed it under that call. Without this destroy, srv
       stays alive and registered. The unconditional chttpsvr_engine_wait()
       below would then block for ever, and it would wait for an engine
       reference that never goes away. The code reaches this branch only
       after restart_th fully returns, which restart_returned records and
       the join above confirms. This destroy therefore cannot race the
       chttpsvr_start() call of restart_th while that call is in flight. */
    chttpsvr_destroy(srv);
  }

  /* chttpsvr_engine_wait() has no timeout of its own; see its own doc
     comment. It blocks until reactor_refs really reaches zero. An
     unconditional call here would itself be a fresh hang risk, in exactly
     the compound regression that this test exists to catch. Take the case
     where destroy_th_created is true and the chttpsvr_destroy() call of
     destroy_th never returns. That is a hypothetical regression that brings
     a deadlock back into the very teardown path under test. The
     `if (destroy_th_created)` branch above then never joins destroy_th, and
     the code never falls through to the chttpsvr_destroy() call of the
     `else if` branch either. Nothing has released the engine reference of
     srv, so an unconditional wait here would block for ever. That turns a
     clean, reported test failure into a hang of the whole binary. The same
     risk applies to the chttpsvr_start() call of restart_th, which also
     takes an engine reference of its own. On every real completion path
     that call correctly releases that reference or keeps it. Call
     chttpsvr_engine_wait() only after both sides confirm that they really
     returned. The start_returned gate of
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener below
     does the same. */
  bool engine_refs_should_settle = (!destroy_th_created || destroy_returned) &&
                                   (!restart_th_created || restart_returned);
  if (engine_refs_should_settle) chttpsvr_engine_wait();

  REQUIRE_TRUE(restart_th_created);
  REQUIRE_TRUE(hook_entered);
  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(restart_returned);
  REQUIRE_TRUE(destroy_returned);
  if (!restart_returned || !destroy_returned)
    return; /* one side is stuck for ever; nothing more to check safely */

  REQUIRE_FALSE(destroy_returned_too_early);
  /* restart_th necessarily lost the race. By the time that it resolves srv
     again, destroy_th has already marked the slot as not in use. */
  REQUIRE_EQ((int)restart_rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*   chttpsvr_stop() RACING A CONCURRENT chttpsvr_start() ON THE SAME HANDLE  */
/*                                                                            */
/* A plain chttpsvr_stop() call leaves raw->lifecycle ==                     */
/* CHTTPSVR_LC_RUNNING BEFORE its own blocking ccol_event_loop_remove() and  */
/* close() calls for the OLD listener registration return. It does not do    */
/* that after those calls. chttpsvr_destroy() and chttpsvr_engine_stop() are */
/* different. Both go through _quiesce_server_once and its quiesce_state     */
/* interlock, which the wait loop of chttpsvr_start() already handles.       */
/* Without a lifecycle state of its own for that window, a chttpsvr_start()  */
/* call can race an in-flight chttpsvr_stop() call on the same handle from   */
/* another thread. It then sees lifecycle == CHTTPSVR_LC_IDLE and            */
/* quiesce_state == CHTTPSVR_QS_NOT_QUIESCED at once. It goes straight into  */
/* _make_listen_socket() and bind() for a NEW listener on the identical host */
/* and port, while the fd of the OLD listener is still open and still bound. */
/* That is a narrow race, and it fails gracefully, with a spurious           */
/* ccol_unexpected_failure from a concurrent EADDRINUSE. CHTTPSVR_LC_STOPPING*/
/* is what closes it. It is a lifecycle state that the server enters for the */
/* whole duration of the real teardown work of _chttpsvr_stop_internal().    */
/* The wait loop of chttpsvr_start() checks it, and it mirrors the exact     */
/* shape of quiesce_state. This test uses a white-box hook to pause          */
/* chttpsvr_stop() inside that exact window, deterministically. It does not  */
/* rely on real timing with no bound to hit by chance a race window a few    */
/* instructions wide.                                                        */
/* ========================================================================== */
extern void _chttpsvr_arm_stop_race_hook_for_tests(void);
extern void _chttpsvr_wait_stop_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_stop_race_hook_for_tests(void);

static chttpsvr g_stop_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_stop_race_start_rv = ccol_success;
static _Atomic bool g_stop_race_start_returned = false;
/* _stop_racing_server_thread sets this flag as its last statement, after
   chttpsvr_stop() itself really returns. The test below polls it, with a
   bound, before it ever calls chttpsvr_destroy() or chttpsvr_engine_wait()
   on g_stop_race_srv. A hypothetical regression can hang chttpsvr_stop()
   itself, for example inside its own ccol_event_loop_remove() or close()
   call. This flag reports that as a clean, bounded test failure. Without
   it, those two later calls risk a hang of their own. This file uses the
   same discipline throughout: confirm that the racing thread really
   returned before you touch shared state any further. g_race_start_returned
   above is one such flag. */
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
  /* This test captures each intermediate outcome below into a local. It
     does not assert on that outcome at once with a REQUIRE_*. It also
     releases and joins every background thread, and it tears
     g_stop_race_srv down, UNCONDITIONALLY, before any REQUIRE_* runs at
     all. bounded_pool_full_returns_503 in tests.c uses the same discipline
     for the same reason. g_stop_race_srv is a plain global. It cannot carry
     a scope-exit _ccol_destructor the way that a local handle can, because
     _start_racing_stop_thread writes g_stop_race_start_rv from a background
     thread. force_stop_racing_a_concurrent_start_is_safe above sets the
     same precedent. A REQUIRE_* that returns early from this function
     before the code releases the hook would leave stop_th parked for ever
     inside the hook wait of _chttpsvr_stop_internal. Nothing else in this
     binary ever releases it. The listener fd of g_stop_race_srv is then
     never closed. That leaves a leaked thread that blocks for ever, and a
     leaked server that stays registered and still holds an engine
     reference, for the rest of the run of this binary. It is much worse
     than one assertion that fails. */
  g_stop_race_srv = _start_server("18808", 18808);

  _chttpsvr_arm_stop_race_hook_for_tests();
  atomic_store(&g_stop_race_stop_returned, false);

  pthread_t stop_th;
  bool stop_th_created =
      pthread_create(&stop_th, NULL, _stop_racing_server_thread, NULL) == 0;
  if (stop_th_created) _fx_track(tau, stop_th);

  /* This is deterministic once stop_th_created is true. It returns only
     after _chttpsvr_stop_internal() enters CHTTPSVR_LC_STOPPING and pauses
     right there. It pauses strictly before its own ccol_event_loop_remove()
     and close() calls, so the fd of the OLD listener is still fully open
     and still bound. No luck with the timing is needed. _start_server()
     fails fast, with exit(1), when a chttpsvr_start call fails. That
     already guarantees that raw->lifecycle was CHTTPSVR_LC_RUNNING by the
     time that the code above assigned g_stop_race_srv.
     _chttpsvr_stop_internal therefore always reaches the hook once stop_th
     runs. This wait can never hang for a hook that stop_th never
     reaches. */
  if (stop_th_created) _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_stop_race_start_rv = ccol_success;
  atomic_store(&g_stop_race_start_returned, false);
  /* This code arms the signal before it creates start_th.
     chttpsvr_start() can reach its own CHTTPSVR_LC_STOPPING wait branch
     very quickly. To arm after the creation of the thread would risk a
     miss of the signal. */
  extern void _chttpsvr_arm_start_stopping_wait_signal_for_tests(void);
  extern void _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests(void);
  _chttpsvr_arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      stop_th_created &&
      pthread_create(&start_th, NULL, _start_racing_stop_thread, NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    /* This is deterministic, and it is not a fixed sleep. It returns only
       after chttpsvr_start() really reaches its own CHTTPSVR_LC_STOPPING
       wait loop, and is about to enter it. See the doc comment of that
       signal for why it is a plain signal that says "the code reached this
       point", and not a park-until-release hook like g_stop_race_hook
       above. stop_th is still parked in g_stop_race_hook, because nothing
       released it below yet. lifecycle therefore cannot have left
       CHTTPSVR_LC_STOPPING yet. still_blocked is guaranteed true here on
       every single run. Nothing has to guess whether a fixed sleep was long
       enough for the scheduler to even run start_th. On an unlucky, heavily
       loaded run such a sleep can read true for the wrong reason, where the
       scheduler has not run start_th yet. The right reason is that the
       guard really blocks it. */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_stop_race_start_returned);
  }

  /* Release the hook. That call is safe and harmless even when nothing
     created stop_th to reach it. Then join every thread that this code
     really created, unconditionally, before any REQUIRE_* below runs. See
     the opening comment of this test for why that order is load-bearing.
     The release lets chttpsvr_stop() finish. Its ccol_event_loop_remove()
     and close() calls for the OLD listener then really run, and lifecycle
     leaves CHTTPSVR_LC_STOPPING. That frees the chttpsvr_start() call that
     waits, if there is one, and it can go on. */
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

  /* This is a bounded wait, and not a blind pthread_join. A regression can
     turn this race into a hang. This loop then times out, and it does not
     hang the whole test binary. The teardown of this fixture tries one more
     bounded join on start_th after that, with a limit of 30s. If that join
     fails, the teardown detaches start_th, and it does not hang the other
     tests of this binary. This file uses the same shape of bounded wait in
     place of a pthread_join elsewhere. */
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

  /* Try the real HTTP round trip only after the code confirms that the
     restart finished and succeeded. A call out to a server that never
     started again, or whose own start thread is still stuck for ever, is
     only a second and redundant way to hang or to fail. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_stop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18808/hello", &status);

  /* Tear g_stop_race_srv down before any REQUIRE_* below. Do that only
     after the code confirms that stop_th really returned. A concurrent
     chttpsvr_start() that is still in flight on another thread is exactly
     the case that the resolve and pin machinery of this whole module makes
     memory-safe. Other tests in this same file drive that machinery. A
     start_th that is stuck for ever therefore does not gate this teardown.
     A hypothetical regression that hangs chttpsvr_stop() ITSELF, past the
     point where the hook above already released it, is a different and
     unproven risk. Nothing establishes that chttpsvr_destroy() and
     chttpsvr_engine_wait() are safe to call at the same time as a
     chttpsvr_stop() call on the identical handle that is stuck somewhere in
     its own real teardown work. A stop that is cleanly blocked on the hook
     of this test is a different matter. This code therefore calls neither
     of those two in that case, and it avoids a second hang on top of the
     first. It deliberately leaks g_stop_race_srv in that branch, which only
     a regression reaches. The start_th-stuck case below accepts the same
     trade-off. */
  bool stop_teardown_safe = !stop_th_created || stop_returned;
  if (stop_teardown_safe) {
    chttpsvr_destroy(g_stop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(stop_th_created);
  REQUIRE_TRUE(stop_returned);
  if (!stop_returned)
    return; /* stop_th is stuck for ever; nothing more to check safely */
  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is stuck for ever; nothing more to check safely */
  /* The real assertion is that there is no spurious failure of the
     EADDRINUSE class. The restart succeeded cleanly after the old listener
     was really gone. */
  REQUIRE_EQ((int)g_stop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/*  chttpsvr_start() RACING chttpsvr_engine_stop()'S OWN _chttpsvr_stop_      */
/*  INTERNAL() CALL THROUGH _quiesce_server_once, NOT A PLAIN chttpsvr_stop() */
/*                                                                            */
/* The test just above is                                                    */
/* stop_racing_a_concurrent_start_does_not_bind_over_open_listener. It       */
/* drives the CHTTPSVR_LC_STOPPING wait of chttpsvr_start() for the case     */
/* where a PLAIN chttpsvr_stop() call drives _chttpsvr_stop_internal(). A    */
/* second caller also reaches _chttpsvr_stop_internal(), and its structure   */
/* differs. That caller is _quiesce_server_once(), the engine-wide path that */
/* both chttpsvr_engine_stop() and chttpsvr_destroy() go through. It reaches */
/* CHTTPSVR_LC_STOPPING with quiesce_state already at                        */
/* CHTTPSVR_QS_QUIESCING, which it sets before it ever calls                 */
/* _chttpsvr_stop_internal(). It also uses a completely different set of     */
/* pins, which is servers_bundler_pins, and not the pending_resolve_count    */
/* that a plain chttpsvr_stop() call uses. The CHTTPSVR_LC_STOPPING wait     */
/* itself does not care which of the two drove it. This test is what drives  */
/* that independently. Every other combination of hooks in this file does    */
/* one of two things. It pauses a plain chttpsvr_stop() call inside          */
/* CHTTPSVR_LC_STOPPING, which is the test above. Or it pauses               */
/* _quiesce_server_once() BEFORE it ever reaches _chttpsvr_stop_internal,    */
/* with quiesce_state == QUIESCING and pending_resolve_count already         */
/* drained; see the fork-safety tests further below. No other test does both */
/* together. This one reuses the SAME _stop_race_hook, because               */
/* _chttpsvr_stop_internal() pauses at the identical point whichever caller  */
/* reached it. It drives that hook with chttpsvr_engine_stop() instead of a  */
/* plain chttpsvr_stop() call. A concurrent chttpsvr_start() call on the     */
/* same handle therefore sees CHTTPSVR_LC_STOPPING with quiesce_state        */
/* already at CHTTPSVR_QS_QUIESCING. That is exactly the interleaving that   */
/* the comment of the CHTTPSVR_LC_STOPPING branch reasons through; see the   */
/* doc comment there in chttpsvr_start().                                    */
/*                                                                            */
/* The condition variable wait of CHTTPSVR_LC_STOPPING is not what keeps     */
/* THIS driver from a crash, a hang or starvation. Its two sibling backoffs  */
/* are different; those are CHTTPSVR_QS_QUIESCING and "currently stopping".  */
/* A blind backoff in that one branch passes this exact test too, and        */
/* still_blocked included. Such a backoff releases the pin, calls            */
/* nanosleep(1ms), resolves again and retries. It passes because             */
/* _chttpsvr_stop_internal() never waits on pending_resolve_count, whichever */
/* caller reached it. A blind re-pin from this branch therefore cannot       */
/* starve it out, as it can for the two siblings. The value of this test is  */
/* therefore that it proves that the interleaving itself is safe. There is   */
/* no hang, no crash and no spurious EADDRINUSE, and the library rebuilds    */
/* the whole shared engine cleanly once the quiesce pass that drove it       */
/* finishes. The condition variable wait has its own separate benefit. It    */
/* avoids a busy poll thousands of times a second while a concurrent quiesce */
/* pass drains. That drain can take far longer than the comparatively fast   */
/* listener teardown of a plain chttpsvr_stop() call. That benefit is a      */
/* property of CPU efficiency, and this single pass-or-fail test cannot      */
/* measure it. The tests in tests.c that count dispatches show the general   */
/* pattern that such a measurement would need here. One of them is           */
/* post_accept_alloc_failure_backs_off_instead_of_busy_looping.              */
/* ========================================================================== */
static chttpsvr g_qstop_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_qstop_race_start_rv = ccol_success;
/* _start_racing_quiesce_stop_thread sets this flag as its last statement,
   after chttpsvr_start() itself really returns. This test polls it, with a
   bound, before it ever joins that thread and before it reads
   g_qstop_race_start_rv. g_stop_race_start_returned above has the same
   reasoning. */
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
  /* This test captures each intermediate outcome below into a local. It
     does not assert on that outcome at once with a REQUIRE_*. It also
     releases and joins every background thread, and it tears
     g_qstop_race_srv down, unconditionally, before any REQUIRE_* runs at
     all. stop_racing_a_concurrent_start_does_not_bind_over_open_listener
     just above uses the same discipline for the same reason. A REQUIRE_*
     that returns early here, before the code releases the hook, would leave
     the engine-stop reaper thread parked for ever inside the hook wait of
     _chttpsvr_stop_internal. Nothing else in this binary ever releases it.
     Every later test in this binary that touches the shared engine again
     then hangs. */
  g_qstop_race_srv = _start_server("18817", 18817);

  _chttpsvr_arm_stop_race_hook_for_tests();

  /* chttpsvr_engine_stop() itself does not block. It only posts to the
     semaphore of the signal-safe watcher thread, and it returns at once;
     see its own doc comment. The real reap that it triggers runs on a
     separate reaper thread. That thread is what reaches
     _quiesce_server_once(), then _chttpsvr_stop_internal(), then the armed
     hook. This test needs no background thread of its own only to make this
     call. */
  chttpsvr_engine_stop();

  /* This is deterministic. It returns only after the
     _chttpsvr_stop_internal() call of the reaper thread enters
     CHTTPSVR_LC_STOPPING and pauses right there. It pauses strictly before
     its own ccol_event_loop_remove() and close() calls, so the fd of the
     OLD listener is still fully open and still bound. quiesce_state is
     already CHTTPSVR_QS_QUIESCING at that point, because
     _quiesce_server_once() sets it before it ever calls
     _chttpsvr_stop_internal(). That is exactly the interleaving that this
     test exists to drive. */
  _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_qstop_race_start_rv = ccol_success;
  atomic_store(&g_qstop_race_start_returned, false);
  /* This code arms the signal before it creates start_th. See the same use
     of this signal in
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener above
     for the full reasoning. */
  extern void _chttpsvr_arm_start_stopping_wait_signal_for_tests(void);
  extern void _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests(void);
  _chttpsvr_arm_start_stopping_wait_signal_for_tests();
  pthread_t start_th;
  bool start_th_created =
      pthread_create(&start_th, NULL, _start_racing_quiesce_stop_thread,
                     NULL) == 0;
  if (start_th_created) _fx_track(tau, start_th);

  bool still_blocked = false;
  if (start_th_created) {
    /* This is deterministic, and it is not a fixed sleep. The reaper thread
       is still parked in g_stop_race_hook, because nothing released it
       below yet. lifecycle therefore cannot have left CHTTPSVR_LC_STOPPING
       yet. still_blocked therefore reads true here on every single run,
       once chttpsvr_start() really reaches its own wait loop. See the doc
       comment of that signal. */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_qstop_race_start_returned);
  }

  /* Release the hook, so that the _chttpsvr_stop_internal() call of the
     reaper can finish. Its ccol_event_loop_remove() and close() calls for
     the OLD listener then really run, and lifecycle leaves
     CHTTPSVR_LC_STOPPING. That frees the chttpsvr_start() call that waits,
     if there is one, and it can go on. It then falls through, sees
     quiesce_state == CHTTPSVR_QS_QUIESCING, and waits there instead. That
     machinery is established, and other tests already cover it. It waits
     until the reaper finishes the drain, the release and the teardown of
     the whole shared engine. This restart can then truly rebuild that
     engine from the start. */
  _chttpsvr_release_stop_race_hook_for_tests();

  /* This is a bounded poll, and not a blind pthread_join. A regression here
     makes start_th deadlock with itself for ever. This poll turns that hang
     into a clean REQUIRE_TRUE(start_returned) failure that names the test.
     Without it, the hang has no bound. This flag is also what really
     establishes a happens-before relationship for g_qstop_race_start_rv
     below. That is a plain global that start_th wrote, and it is not
     atomic. */
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

  /* Try the real HTTP round trip only after the code confirms that the
     restart finished and succeeded. The sibling test above uses the same
     guard. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_qstop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18817/hello", &status);

  /* g_qstop_race_srv is a global. _start_server writes it before start_th
     ever runs, and start_th itself may restart it. It therefore cannot
     carry a scope-exit _ccol_destructor. This code tears it down only after
     it confirms that start_th really returned. The sibling test uses the
     same reasoning: do not risk a second hang on top of a start_th that is
     truly stuck. */
  if (start_returned) {
    chttpsvr_destroy(g_qstop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is stuck for ever; nothing more to check safely */
  /* The real assertion is that there is no spurious failure of the
     EADDRINUSE class, and no hang and no crash, when _quiesce_server_once()
     drove _chttpsvr_stop_internal() instead of a plain chttpsvr_stop()
     call. The restart succeeded cleanly after the library really tore the
     old listener down, and the whole shared engine after it, and then built
     both again from the start. */
  REQUIRE_EQ((int)g_qstop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/* This test covers the lifetime of the bare struct chttpserver* that       */
/* _engine_force_stop_quiesce_all() reads directly out of                    */
/* servers_bundler.servers[0]. servers_bundler_pins exists exactly to        */
/* protect the lifetime of that pointer; see the comment of that field on    */
/* struct chttpserver. To reuse pending_resolve_count and                    */
/* _chttpsvr_resolve_unpin for that instead is a self-deadlock; see the doc  */
/* comment of _engine_force_stop_quiesce_all for why. The narrower hazard    */
/* that servers_bundler_pins closes is the target of this test. The reaper   */
/* thread reads a server out of servers_bundler.servers[], and it is about   */
/* to call _quiesce_server_once() on it. A concurrent chttpsvr_destroy(srv)  */
/* call on that exact server can run to completion and free srv, while the   */
/* reaper thread is still about to dereference it. That happens when no pin  */
/* protects that bare pointer. The very next touch of srv on the reaper      */
/* thread is then a use-after-free. This test uses a white-box hook to pause */
/* the reaper thread at the exact point right after it pins srv,             */
/* deterministically. At that point it has incremented                       */
/* servers_bundler_pins and released servers_bundler.mutex, and it has not   */
/* called _quiesce_server_once() yet. The test then lands a concurrent       */
/* chttpsvr_destroy() call on that same srv from a second thread. That       */
/* destroy() call must block, and wait for servers_bundler_pins to reach 0.  */
/* It must stay blocked until the code releases the hook and the reaper      */
/* thread really finishes with srv. It must not go on and free srv out from  */
/* under the reaper thread while that thread is still paused.                */
/* ========================================================================== */
extern void _chttpsvr_arm_reaper_race_hook_for_tests(void);
extern void _chttpsvr_wait_reaper_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_reaper_race_hook_for_tests(void);

static chttpsvr g_reaper_race_srv = CHTTPSVR_INVALID;
static _Atomic bool g_reaper_race_destroy_returned = false;

static void *_destroy_racing_reaper_thread(void *arg) {
  (void)arg;
  chttpsvr_destroy(g_reaper_race_srv);
  atomic_store(&g_reaper_race_destroy_returned, true);
  return NULL;
}

TEST_F(engine_stop_fixture,
       destroy_racing_the_reaper_does_not_free_srv_out_from_under_it) {
  /* This test uses the same discipline as
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener above.
     It captures each intermediate outcome into a local. It releases the
     hook, and it joins every thread that it created, unconditionally,
     before any REQUIRE_* runs. An assertion that fails here can therefore
     never leave destroy_th parked for ever inside the hook wait of the
     reaper, which nothing else in this binary would release. It also cannot
     leak the engine reference of g_reaper_race_srv, or its listener, for
     the rest of the run of this binary. */
  g_reaper_race_srv = _start_server("18809", 18809);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* The documentation says that chttpsvr_engine_stop() does not block. It
     only starts a real, separate reaper thread, and it returns at once.
     That reaper thread is what pins g_reaper_race_srv and then blocks
     inside the armed hook. */
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
    /* This gives chttpsvr_destroy() time to run, to reach its own
       servers_bundler_pins wait, and to block inside it. This matches the
       150ms precedent that this file uses elsewhere to let the other side
       reach its own point where it blocks. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* chttpsvr_destroy() must still be blocked here, and it must wait for
       servers_bundler_pins to reach 0. It must not have returned yet. A
       return by now means that it already freed g_reaper_race_srv, while
       the reaper thread is still paused and still holds a bare pointer to
       it. */
    still_blocked = !atomic_load(&g_reaper_race_destroy_returned);
  }

  /* Release the hook, and then join destroy_th. Do both unconditionally,
     before any REQUIRE_* below runs. The release is harmless even when the
     reaper thread never reached the hook. The real reaper thread of
     chttpsvr_engine_stop() always reaches it, because the code above
     confirmed that g_reaper_race_srv started. This release therefore can
     never hang for a hook that the thread would never reach. The release
     lets the reaper thread finish with g_reaper_race_srv. It calls
     _quiesce_server_once() on it. chttpsvr_destroy() already won that race,
     because this code held the destroy back until now, so that call takes
     the loser path and returns at once. The reaper then decrements
     servers_bundler_pins, and that frees the wait of chttpsvr_destroy() to
     go on. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  if (destroy_th_created) _fx_join(tau, destroy_th, NULL);

  chttpsvr_engine_wait();

  /* Prove that the engine came back up cleanly after this. A fresh server
     on the same port must be able to start, and to serve a real request. It
     is not enough that nothing crashed above. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  chttpsvr srv2 = CHTTPSVR_INVALID;
  if (destroy_th_created) {
    srv2 = _start_server("18809", 18809);
    get_rv = _get("http://127.0.0.1:18809/hello", &status);
  }
  if (srv2 != CHTTPSVR_INVALID) chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();

  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/*         REPEATED chttpsvr_engine_stop() DURING TEARDOWN IS SAFE            */
/*                                                                            */
/* _engine_reaper_fn sets srv_engine_bundler.reactor to NULL only near its   */
/* very end. That is well after _engine_force_stop_quiesce_all,              */
/* _idle_sweep_stop_if_running and ccol_event_loop_destroy have all run, and */
/* those can take real wall-clock time. But it sets                          */
/* srv_engine_bundler.stopping to true at once, long before any of that      */
/* finishes. _engine_force_stop_now is the function that the signal-safe     */
/* watcher thread runs for chttpsvr_engine_stop(). It must therefore not     */
/* check only `if (srv_engine_bundler.reactor)` before it starts a reaper    */
/* thread. It needs the same `&& !stopping` guard that the graceful          */
/* _engine_release() path has; see the comment of that function for why.     */
/* Without that guard, a second chttpsvr_engine_stop() call can land while   */
/* the reaper of a first call is still in the middle of its teardown. That   */
/* is entirely ordinary for the documented use from a signal handler. Two    */
/* SIGTERMs moments apart do it, and so does a defensive double call from    */
/* the shutdown code of an application. It then starts a SECOND reaper       */
/* thread, which captures the identical ccol_event_loop handle that is still */
/* live. Both reapers then call ccol_event_loop_destroy() on that same       */
/* handle, independently. The documented contract of cthreadcomm.h makes     */
/* that unconditionally fatal, with ccol_fatal_err() and SIGABRT. That holds */
/* for a double destroy where one call already finished, and for one where   */
/* the two overlap in time. A second engine_stop() call that is supposed to  */
/* be safe then aborts the process, instead of doing nothing as the          */
/* documentation says.                                                       */
/* ========================================================================== */

TEST_F(engine_stop_fixture,
       second_engine_stop_call_while_first_still_tearing_down_is_safe) {
  (void)tau; /* this test creates no background thread of its own */
  /* This test uses the same discipline as the reaper-race test above. Every
     intermediate step runs unconditionally, before any REQUIRE_* below.
     Those steps release the hook and wait for the engine to settle. An
     assertion that fails here can therefore never leave the reaper race
     hook armed for ever, which would hang every later test in this binary
     that force-stops the engine. It also cannot leave the shared engine
     wedged in the middle of its teardown for the rest of the run of this
     binary. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18810", 18810);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* This is the first call. It starts a real reaper thread. That thread
     pins srv through _engine_force_stop_quiesce_all, and it then blocks
     inside the hook that the code just armed. It blocks strictly before its
     own _quiesce_server_once(srv) call. srv_engine_bundler.stopping is
     already true at that point, and srv_engine_bundler.reactor is still
     fully live. That is exactly the window that the !stopping guard exists
     to recognize. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_reaper_race_hook_entered_for_tests();

  /* This is the second call, and it lands squarely inside that window.
     With no guard, it starts a second reaper thread. That thread goes on to
     call ccol_event_loop_destroy() on the identical handle that the first
     reaper is still going to use, which is fatal. The
     !srv_engine_bundler.stopping guard of _engine_force_stop_now makes this
     call do nothing instead, silently, as the documentation says. It
     dispatches nothing on the watcher thread, and it starts no second
     reaper. */
  chttpsvr_engine_stop();

  /* This gives a second reaper thread a moment to reach its own
     ccol_event_loop_destroy() call, if anything ever started one. It gives
     that moment before the code releases the first reaper. This matches the
     150ms precedent that this binary uses elsewhere to let the other side
     reach its own point where it blocks or crashes. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Release the reaper thread, of which there is truly only one. It can
     then finish the quiesce of srv, and tear the reactor down normally. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  chttpsvr_engine_wait();

  /* A regression here takes the whole process down well before this point.
     It does that synchronously above, or through the delayed
     ccol_event_loop_destroy() call of a second reaper that races the call
     of the first one. To reach this line at all is therefore a meaningful
     part of the assertion. Also prove that the engine came back up truly
     clean, with no global state corrupt and no server that stays registered
     and dangles. Start a fresh server on the same port, and drive one real
     request through it. The other force-stop tests in this file do exactly
     the same. */
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
/* This test covers the child-side fixup of quiesce_state and               */
/* quiesce_waiters inside _chttpsvr_atfork_release_impl. See the doc comment */
/* of that function in chttpserver.c for the full account. fork() can land   */
/* while a thread in the parent is truly in the middle of the real teardown  */
/* work of _quiesce_server_once for some server. quiesce_state ==            */
/* CHTTPSVR_QS_QUIESCING at that moment, and the child gets no copy of that  */
/* thread. Without the fixup, two things go wrong. (1) The "loser" branch of */
/* _quiesce_server_once blocks for ever. A later chttpsvr_destroy() on the   */
/* same handle in the child takes that branch, and it waits for a broadcast  */
/* that only the thread that vanished could send. (2) The driver loop of     */
/* _engine_force_stop_quiesce_all spins for ever on this exact server the    */
/* next time that it runs, because nothing removes that server from          */
/* servers_bundler.servers[] either. This test drives both halves. It drives */
/* (2) directly, with a white-box accessor that reads                        */
/* servers_bundler.count itself. It does not drive a fresh                   */
/* chttpsvr_engine_stop() pass in the child. chttpsvr_engine_stop() alone is */
/* not expected to do anything there in any case, whether or not the fixup   */
/* corrected the server. It only wakes a watcher thread that already runs,   */
/* which is g_engine_stop_watcher. This same atfork machinery deliberately   */
/* and correctly marks that watcher as not ready in a child that something   */
/* just forked; see the comment of that field. A truly fresh                 */
/* chttpsvr_start() call arms it again. That is an unrelated part of the     */
/* fork story of this module, it is already correct, and this test is not    */
/* about it. This test drives the case deterministically, with a dedicated   */
/* white-box hook. That hook pauses a real reaper thread that                */
/* chttpsvr_engine_stop() drives, at exactly the point where quiesce_state   */
/* == CHTTPSVR_QS_QUIESCING, in the parent, before the fork. This test lives */
/* in this binary, and not in tests.c, for the same reason as every other    */
/* test here. It drives the real chttpsvr_engine_stop() for the whole        */
/* process, which would otherwise pull the reactor out from under the        */
/* long-lived g_srv of tests.c.                                              */
/* ========================================================================== */
#if CCOL_FORK_SAFETY_REQUIRED
extern void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void);
extern void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void);
extern size_t _chttpsvr_servers_bundler_count_for_tests(void);

TEST_F(engine_stop_fixture, fork_mid_quiesce_teardown_does_not_hang_child) {
  (void)tau; /* This test creates no background thread of its own to track.
                chttpsvr_engine_stop() itself starts the internal reaper
                thread of the reactor, and this test does not create it
                directly. */
  chttpsvr srv = _start_server("18811", 18811);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();

  /* The documentation says that chttpsvr_engine_stop() does not block. It
     only starts a background reaper thread. That thread is what runs
     _quiesce_server_once against srv. It then blocks inside the hook that
     the code just armed. It blocks right after quiesce_state becomes
     CHTTPSVR_QS_QUIESCING, and strictly before any of its own real teardown
     work starts. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv is now truly stuck in this exact process, with quiesce_state ==
     CHTTPSVR_QS_QUIESCING. Call fork() while that is still true. */
  int pipefd[2];
  /* This code does not use REQUIRE_EQ directly. A failure of pipe() or of
     fork() here must still release the hook that the code armed, and it
     must destroy srv, before it returns. Without that, the real reaper
     thread that this test paused above stays parked for ever. It then
     wedges the shared engine of the whole process for every later test in
     this binary that touches it, through chttpsvr_start,
     chttpsvr_engine_stop or chttpsvr_engine_wait. That is much worse than
     one test that fails on its own. */
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
    /* This bounds the lifetime of this child, for the case where the
       hazard that this test guards against still fires. The
       chttpsvr_destroy() below would otherwise block for ever. The child
       reports its own outcome through the pipe, and not through its own
       exit code. Under make memtest, valgrind replaces the real exit code
       of a forked child with its own --error-exitcode. It does that the
       instant that it finds ANY allocation in the inherited image of that
       child that is still reachable at exit. Every child that something
       forks in the middle of a suite always has such an allocation, because
       the rest of the suite has not quiesced yet. The exit code therefore
       cannot carry this result reliably. See
       wait_from_within_own_task_does_not_hang in tests/cthreadpool, and the
       fork_safety group in tests/clogger, for the same valgrind
       behaviour. */
    alarm(3);
    char ok = 0;
    /* Half 1: srv must already be gone from servers_bundler.servers[]
       here. That proves that the atfork fixup ran the unregister step, and
       not only the flip to CHTTPSVR_QS_QUIESCED. Without this check, a
       regression here shows up only indirectly, and much later. It shows up
       as the driver loop of _engine_force_stop_quiesce_all that spins for
       ever, the next time that this process truly drives a fresh reaper
       pass. */
    if (_chttpsvr_servers_bundler_count_for_tests() == 0) {
      /* Half 2: chttpsvr_destroy() on the exact same handle must not block
         on the loser wait of _quiesce_server_once either. That handle is
         still resolvable here, because this path never touched
         chttpsvr_slot_table at all. */
      chttpsvr_destroy(srv);
      ok = 1;
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  /* This code does not use REQUIRE_EQ directly. A spurious return from
     waitpid() here, for example EINTR, must still release the hook and
     clean srv up before this function returns. The pipe() and fork()
     failure branches above do the same, for the same reason. Without that,
     the real, paused reaper thread stays parked for ever, and it wedges the
     shared engine for every later test. */
  pid_t waited = waitpid(pid, &status, 0);

  /* This is the parent side. Let the real, paused reaper thread finish its
     own teardown work normally. Then clean srv up here too, whatever the
     outcome of the child was. */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  /* This code checks WIFEXITED and not WEXITSTATUS; see the comment of this
     test above. WIFEXITED alone still catches a real regression. A
     regression that hangs becomes WIFSIGNALED through the alarm above, and
     a real crash shows up the same way. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* ========================================================================== */
/* This test covers the child-side reset of srv_engine_bundler.stopping     */
/* inside _chttpsvr_atfork_release_impl. See the doc comment of that         */
/* function in chttpserver.c for the full account. That account also covers  */
/* the sibling resets of reaper_joinable and idle_sweep_bundler.running,     */
/* which this same test cannot isolate on their own. The                     */
/* `while (stopping) ccol_cond_var_wait(...)` loop of _engine_acquire() runs */
/* strictly before its own _join_reaper_if_needed_locked() call. A stopping  */
/* flag that stays true therefore always blocks first, and it masks whatever */
/* the reset of reaper_joinable would otherwise prove on its own. Those two  */
/* resets are correct by the same construction as this one, which the test   */
/* confirms deterministically. The g_engine_stop_watcher.started precedent   */
/* of this module has the same shape. Neither needs a narrower test of its   */
/* own. A real reaper thread in the parent, paused in the middle of its      */
/* teardown at the instant of the fork, leaves srv_engine_bundler.stopping   */
/* true. The setup of this test is identical to that of the previous test,   */
/* and the child gets no copy of that thread. The previous test above never  */
/* reaches code that reads that flag at all. Its chttpsvr_destroy() takes    */
/* the loser branch of _quiesce_server_once, which never touches the         */
/* stopping bookkeeping of the shared engine. A restart of THAT SAME srv     */
/* would also skip _engine_acquire() completely. The                         */
/* `need_acquire = !raw->contributed_to_engine` test of chttpsvr_start()     */
/* sees a stale value there that is still true, inherited from before the    */
/* whole stop sequence began. That is a second, independent reason why srv   */
/* itself cannot be the handle under test here. A truly fresh handle is what */
/* forces chttpsvr_start() to call _engine_acquire() for real, and it        */
/* therefore drives the stopping-wait loop of that call. Such a handle was   */
/* never started, and contributed_to_engine is false from its construction.  */
/* This test is not vacuous. Without that child-side reset, the             */
/* chttpsvr_start() call of the child hangs for ever. srv itself is needed   */
/* only to put the shared engine into the right state, which is stuck in the */
/* middle of a teardown, to fork out of. The start and the stop of the fresh */
/* handle are otherwise completely independent of srv.                       */
/* ========================================================================== */
TEST_F(engine_stop_fixture,
       fork_mid_quiesce_teardown_does_not_hang_fresh_start_in_child) {
  (void)tau; /* This test creates no background thread of its own to track.
                chttpsvr_engine_stop() itself starts the internal reaper
                thread of the reactor, and this test does not create it
                directly. */
  chttpsvr srv = _start_server("18812", 18812);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv_engine_bundler.stopping is now true, and a real reaper thread is
     alive and paused inside the hook, so reaper_joinable == true. Call
     fork() while both of those are still true. */
  int pipefd[2];
  /* This code does not use REQUIRE_EQ directly. See the same comment in the
     previous test for why a failure of pipe() or of fork() here must still
     release the hook, and must destroy srv, before it returns. Without
     that, the real reaper thread stays parked for ever, and it wedges the
     shared engine for every later test. */
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
    /* See the same comment in the previous test for why this child reports
       through the pipe, and not through the exit code of the process. */
    alarm(3);
    /* This is deliberately a brand new handle, and not srv itself. See the
       doc comment of this test above for why a restart of srv would not
       drive _engine_acquire() at all. */
    chttpsvr fresh = ccol_create_chttpsvr(CLOG_INVALID, NULL);
    char ok = 0;
    if (fresh != CHTTPSVR_INVALID) {
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18813;
      chttpsvr_start(fresh, &cfg); /* Nothing asserts on this result.
                                       Whether the bind() of this fresh
                                       server succeeds does not matter. Only
                                       whether the call returns at all is
                                       under test here. */
      ok = 1; /* the code reaches this only when the call above returned */
      chttpsvr_destroy(fresh);
    }
    ssize_t written = write(pipefd[1], &ok, 1);
    (void)written;
    close(pipefd[1]);
    _exit(0);
  }
  close(pipefd[1]);

  char ok = 0;
  ssize_t n = read(pipefd[0], &ok, 1);
  close(pipefd[0]);

  int status = 0;
  /* This code does not use REQUIRE_EQ directly. See the same comment in the
     previous test for why a spurious return from waitpid() here must still
     release the hook, and must clean srv up, before this function
     returns. */
  pid_t waited = waitpid(pid, &status, 0);

  /* This is the parent side. Let the real, paused reaper thread finish its
     own teardown work normally. Then clean srv up here too, whatever the
     outcome of the child was. */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*          NO UNJOINED REAPER THREAD AFTER THE LAST DESTROY                  */
/* ========================================================================== */

/* True while an engine reaper thread has been spawned and not yet joined;
   see src/chttpserver.c's own doc comment on this hook. */
extern bool _chttpsvr_engine_reaper_unjoined_for_tests(void);

/* chttpsvr_destroy() on the last started server hands the shared reactor's
   teardown to a joinable reaper thread and returns immediately, which is the
   documented asynchrony chttpsvr_engine_wait() exists to wait on. That thread
   must still end up joined for an application that calls neither
   chttpsvr_engine_wait() nor a later chttpsvr_start(): a joinable thread that
   has returned but has not been joined keeps its stack mapping and its glibc
   bookkeeping allocated for the rest of the process's life.

   Deliberately calls nothing after chttpsvr_destroy() but the white-box hook:
   any chttpsvr_start()/chttpsvr_engine_wait() here would itself join the
   reaper and make the test vacuous, which is exactly the shape of the gap
   under test. The wait below is a bounded poll on real state rather than a
   fixed sleep, so a slow machine (or valgrind) costs latency, never a wrong
   answer.

   This test is non-vacuous: without the engine reaper's own hand-off to the
   engine-stop watcher thread, nothing joins that thread and the hook stays
   true until the poll's own 30-second bound expires. */
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

extern void _chttpsvr_set_spawn_reaper_delay_for_tests(unsigned ms);

/* The reaper can run to its end before the call that spawned it records
   it. The delay hook holds that call for 3 s right after the thread
   starts, which is longer than a reaper needs; the longest step of the
   reaper is the join of the sweep thread, which sleeps 1 s between its
   ticks. The reaper must still end up joined. This test is non-vacuous: a
   record made after the reaper already asked to be joined makes the join
   request find nothing, and the thread then stays unjoined until the poll's
   own bound expires. */
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
   synchronous wait for that reaper: destroy stays asynchronous and a caller
   that wants to wait calls chttpsvr_engine_wait(). A fresh server binding the
   very same port immediately after the previous one was destroyed is what
   proves the shared engine still comes back cleanly on the ordinary path,
   with no engine wait anywhere in between. */
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

extern size_t _chttpsvr_parked_count_for_tests(chttpsvr h, int kind);
extern size_t _chttpsvr_mem_waiting_for_tests(chttpsvr h);
extern size_t _chttpsvr_stream_queue_len_for_tests(chttpsvr h);
extern size_t _chttpsvr_pool_active_for_tests(chttpsvr h, bool streaming);
extern void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                          unsigned grace_ms);

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
  if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
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

/* Reads until the end of the stream, for 5 s at most; returns true on an
 * end of stream or a reset. */
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
  /* A parked body, a request that waits for body memory, a streaming
   * handler blocked on its client and a streaming request in the queue.
   * chttpsvr_engine_stop() must end every one of them in bounded time: the
   * waiters are closed and the queued request gets 503. valgrind and
   * ThreadSanitizer run this same test. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      ccol_create_chttpsvr(CLOG_INVALID, NULL);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_register_handler(srv, CHTTP_POST, "/len", _es_len_handler, NULL);
  chttpsvr_register_streaming_handler(srv, CHTTP_POST, "/drain",
                                      _es_drain_stream_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18830;
  cfg.worker_thread_count = 2;
  cfg.streaming_thread_count = 1;
  cfg.max_partial_body_memory = 100000;
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

/* Counts the occurrences of needle in the file at path. */
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

/* A logger that chttpsvr_set_engine_logger() installs serves every engine
 * of the process, and not only the one that runs, or first starts, after
 * the call. Each of two full engine cycles logs its creation and its
 * teardown through it. The logger of the caller is closed right after the
 * call, which the engine does not depend on. This test is non-vacuous: an
 * engine teardown that closes the installed logger sends the second cycle
 * to the silent fallback logger, and the file then holds one creation and
 * one teardown.
 *
 * The logger stays installed for the rest of this binary, writing to a file
 * that is unlinked here and whose descriptor stays open for that reason. */
TEST_F(engine_stop_fixture, engine_logger_serves_every_later_engine) {
  (void)tau; /* this test creates no background thread of its own */
  char path[] = "chttpsvr_engine_logger_XXXXXX";
  int fd = mkstemp(path);
  REQUIRE_TRUE(fd >= 0);
  clog lg = clog_open_fd(fd, CLOG_INFO, NULL);
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
