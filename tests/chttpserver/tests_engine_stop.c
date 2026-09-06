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

/* pthread_timedjoin_np (a glibc extension) is what makes this file's own
 * fixture teardown (see engine_stop_fixture below) a bounded join instead of
 * a blind pthread_join: without it, a background thread genuinely,
 * permanently stuck by a real regression (e.g. still holding srv_engine_
 * bundler.mutex forever) would make the join itself hang forever too, just
 * moving the whole-binary-hangs-instead-of-one-test-failing-cleanly problem
 * from "the next unrelated test's own chttpsvr_start() call" to "this exact
 * test's own teardown" rather than actually closing it. Must be defined
 * before the first #include that could pull in <pthread.h> transitively;
 * mirrors src/chttpserver.c's and src/clogger.c's own identical placement. */
#define _GNU_SOURCE

#include <assert.h>
#include <chttpclient.h>
#include <chttpserver.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-but-set-variable"
#include <tau/tau.h>
#pragma GCC diagnostic pop

TAU_MAIN()

/* ========================================================================== */
/*                    BOUNDED-JOIN TEST FIXTURE                               */
/*                                                                            */
/* Every test below spawns one or more background threads to race chttpsvr's  */
/* own internal locking/reference-counting; several deliberately leave a      */
/* thread unjoined on their own regression-detection path (see e.g.           */
/* engine_stop_from_signal_handler_does_not_deadlock's own comment), on the   */
/* reasoning that "the process can still exit normally afterward even with    */
/* that one thread permanently stuck, since process exit does not wait for    */
/* other threads." That reasoning is only true for the very LAST test in the  */
/* binary; this file has 10. srv_engine_bundler.mutex is a single, global,    */
/* process-wide lock every one of chttpsvr_start()/_engine_acquire()/         */
/* _engine_release() must take, so a thread left permanently stuck holding    */
/* it (a genuine regression, not the ordinary case) does not merely affect    */
/* the one test that found it: every LATER test in this binary that calls     */
/* chttpsvr_start() would hang too, the instant it tries to acquire that same */
/* mutex, turning one clean, reported failure into a cascade that needs an    */
/* external CI timeout to even notice, with no indication of which test was   */
/* actually at fault.                                                        */
/*                                                                            */
/* This fixture, used via TEST_F/TEST_F_TEARDOWN below instead of a bare      */
/* TEST(), closes that gap where it can be closed and localizes it where it   */
/* cannot: every pthread_create()'d in a test body is tracked here via        */
/* _fx_track(); every join (whether the test's own existing mid-body join, or */
/* this fixture's own automatic teardown sweep) goes through _fx_join(), a    */
/* generous-but-BOUNDED join via pthread_timedjoin_np rather than a blind     */
/* pthread_join. On a genuine regression, this converts "some unrelated later */
/* test mysteriously hangs" into "THIS test's own teardown reports exactly    */
/* which thread never finished, then abandons (pthread_detach()s) it and lets */
/* Tau continue to the next test", still not a full fix for a truly           */
/* unbounded deadlock (nothing can force a thread to release a lock it will   */
/* never release), but a real improvement in both diagnosability (the failure */
/* is pinned to the actual offending test, not a downstream victim) and       */
/* resilience for the more common case of a thread that is merely slower than */
/* a test's own narrow internal bound expected, not genuinely deadlocked. */
#define ENGINE_STOP_FIXTURE_MAX_THREADS 4

/* The struct must carry its own tag (engine_stop_fixture), matching the
 * typedef name exactly: TEST_F_SETUP/_TEARDOWN/TEST_F all expand FIXTURE
 * into `struct FIXTURE`, and a typedef alone (over an anonymous struct)
 * would make each such expansion declare a fresh, distinct, incomplete
 * struct type instead of referring back to this one. */
typedef struct engine_stop_fixture {
  pthread_t threads[ENGINE_STOP_FIXTURE_MAX_THREADS];
  bool active[ENGINE_STOP_FIXTURE_MAX_THREADS]; /* true = not yet joined */
  int count;
} engine_stop_fixture;

/* Registers tid so the teardown sweep below will (bounded-)join it if the
 * test body itself never gets around to it via _fx_join(). Must be called
 * exactly once per successfully-created thread, immediately after
 * pthread_create() returns 0; a test with more than
 * ENGINE_STOP_FIXTURE_MAX_THREADS concurrent threads would need a larger
 * fixture array (none in this file needs more than 3 today). */
static void _fx_track(engine_stop_fixture *fx, pthread_t tid) {
  /* Catches a future test tracking more threads than this fixture's own
     fixed-size arrays hold, before it silently corrupts memory past their
     end; see this function's own doc comment above for the intended fix
     (a larger fixture array) rather than raising this bound casually. */
  assert(fx->count < ENGINE_STOP_FIXTURE_MAX_THREADS);
  fx->threads[fx->count] = tid;
  fx->active[fx->count] = true;
  fx->count++;
}

/* Generous-but-bounded join: 30 seconds is comfortably beyond every real,
 * legitimate wait anywhere in this file (the longest is a 10s bounded
 * cond_timedwait plus a little slack), so this never fires spuriously
 * against a correctly-behaving thread, while still guaranteeing this call
 * itself cannot hang the caller forever against a genuinely stuck one.
 * Returns true iff tid actually terminated and was joined. */
static bool _fx_timed_join(pthread_t tid, void **retval) {
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  deadline.tv_sec += 30;
  return pthread_timedjoin_np(tid, retval, &deadline) == 0;
}

/* The one join call every test body below should use in place of a bare
 * pthread_join(): bounded (see _fx_timed_join above), and marks tid as
 * already handled so the teardown sweep does not attempt to join it again
 * (a second join on an already-joined pthread_t is undefined behavior).
 * Safe to call on a tid this fixture never tracked (a no-op fallthrough on
 * the tracking search below), not expected in practice, since every
 * thread a test creates should always be tracked via _fx_track() first, but
 * deliberately not treated as a fatal test-infrastructure error either. */
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
    /* See this section's own opening comment for why this must be bounded
     * rather than a blind pthread_join: a genuine regression here must not
     * cascade into every later test in this binary hanging too. */
    if (!_fx_timed_join(tau->threads[i], NULL)) {
      fprintf(stderr,
              "[WARN] tests_engine_stop teardown: a background thread this "
              "test spawned was still running 30s after the test itself "
              "finished (a real regression is suspected); abandoning it "
              "rather than hanging the rest of this binary's tests.\n");
      pthread_detach(tau->threads[i]);
    }
  }
}

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
  chttpsvr srv = create_chttpsvr(CLOG_INVALID, &err);
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
 * NULL g_servers[0]; a guaranteed, deterministic SIGSEGV, not a rare race.
 * A clean run of this test (especially under valgrind's memtest target) is
 * the direct proof the crash is gone and nothing was leaked or
 * double-freed along the way. */
TEST_F(engine_stop_fixture, force_stop_while_started_then_destroy_is_safe) {
  (void)tau; /* no background thread of its own to track */
  /* _ccol_destructor: a safety net for a REQUIRE_* failure between here and
     the explicit chttpsvr_destroy() calls below. This file has no
     _teardown()/atexit(), so a leaked handle here would not hang the binary
     at exit the way tests.c's own equivalent leak once did; but it would
     leave a still-registered, still-engine-referencing server (and, for
     srv2, a still-bound listening socket on this same port) behind for
     every later test in this binary to potentially trip over. Safe here:
     nothing else destroys srv/srv2 or races their destruction. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);

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
  chttpsvr srv2 _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18796", 18796);
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
TEST_F(engine_stop_fixture, force_stop_quiesces_multiple_servers) {
  (void)tau; /* no background thread of its own to track */
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

/* chttpsvr_engine_stop() must still drain genuinely in-flight requests
 * (_drain_and_close_all_connections's own documented contract) before the
 * reactor is actually torn down, exactly like a graceful chttpsvr_destroy()
 * already does; the forced path is not supposed to abandon or corrupt a
 * request that is already being handled by a worker thread at the moment the
 * signal fires. */
TEST_F(engine_stop_fixture,
       force_stop_drains_in_flight_request_before_reactor_teardown) {
  /* Shrinks _wait_in_flight_bounded's own timing (see
   * destroy_does_not_hang_when_worker_blocked_with_disabled_timeouts's own
   * identical use of this hook in tests.c) so that IF the REQUIRE_TRUE(
   * g_slow_entered) below ever fails while a worker thread is genuinely
   * still blocked inside _slow_handler, srv's own scope-exit
   * chttpsvr_destroy() (via _ccol_destructor) forces that connection
   * unblocked in well under a second instead of falling back to the real,
   * production-sized 30s graceful wait + 5s force-unblock grace period,
   * turning a slow (though not infinite) stall on an already-failing test
   * into a fast, clean failure report. Restored to the real defaults before
   * this test returns on any path. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  char *err = NULL;
  /* _ccol_destructor: see force_stop_while_started_then_destroy_is_safe's
     own comment above. Safe here: the background client thread below only
     issues an HTTP request against srv, never destroys it itself. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(CLOG_INVALID, &err);
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
  /* Heap-allocated, not a stack local: a genuine regression below (th still
     running when the final, bounded _fx_join times out) must not turn into
     a later write through a dangling pointer into this function's own,
     by-then-reused stack frame. Mirrors _stopping_race_ctx_t's own identical
     reasoning a few tests down in this same file; deliberately never freed
     on a bail-out path where th might still be running and could still
     write into it. */
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
    free(ctx); /* no thread was ever created, so nothing can still touch it */
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(create_rv, 0);
  _fx_track(tau, th);

  /* Wait for confirmation the request actually reached the worker thread and
   * is blocking inside _slow_handler (i.e. genuinely in-flight) before we
   * force-stop the engine; a fixed sleep here would be a flaky proxy for
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
  if (!g_slow_entered) {
    /* th is otherwise leaked (a joinable thread nothing ever joins) on this
       path: release the handler unconditionally (a no-op if it genuinely
       never started; unblocks it if this was instead a false-negative on
       the bounded wait above) and join th before failing, rather than
       leaving it running for the rest of this process's life. Safe to block
       on an unbounded pthread_join here specifically because the release
       just above guarantees th's own HTTP request can now actually
       complete. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
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

  /* Checked, not a bare fire-and-forget call: if th is somehow still running
     30s later (a genuine regression, since it should have unblocked the
     instant g_slow_go was set above), ctx must not be read or freed here -
     th could still be writing into it. Leaving ctx un-freed on that path is
     deliberate; see its own declaration comment above. */
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
  /* 150ms: give the reaper thread chttpsvr_engine_stop() spawns time to
   * actually win the race to quiesce srv and start blocking inside
   * _drain_and_close_all_connections (waiting on the still-in-flight
   * /slow-race request below) before the handler (and therefore that
   * drain) is allowed to complete. */
  struct timespec nap = {0, 150000000L};
  nanosleep(&nap, NULL);
  pthread_mutex_lock(&g_slow_mtx);
  g_slow_go = true;
  pthread_cond_broadcast(&g_slow_cv);
  pthread_mutex_unlock(&g_slow_mtx);
  return NULL;
}

/* Regression test for a race between chttpsvr_engine_stop()'s background
 * reaper thread (which quiesces every still-registered server, including
 * srv) and a concurrent, independent chttpsvr_destroy(srv) call on this
 * thread, with no chttpsvr_engine_wait() serializing the two; exactly the
 * pattern chttpsvr_engine_stop()'s own doc comment invites by describing
 * itself as safe to call from a signal handler with no requirement that a
 * caller synchronize it against a concurrent chttpsvr_destroy() first.
 *
 * Before the fix, whichever of the two callers lost the race to quiesce srv
 * returned from _quiesce_server_once() immediately, as a bare no-op, the
 * instant it observed the other caller had already claimed srv's own
 * quiesce state, without waiting for that other caller's real teardown
 * work to actually finish. If the loser was this thread's own
 * chttpsvr_destroy(srv) call, it would proceed straight into
 * mutex_destroy(raw->mutex)/mutex_destroy(raw->idle_mutex)/free(raw) while
 * the reaper thread was still concurrently using those exact objects inside
 * _drain_and_close_all_connections (which this test forces to block for a
 * genuine, measurable window via the still-in-flight /slow-race request);
 * a real use-after-free / destroyed-in-use-mutex race. A clean run of this
 * test (especially under valgrind's memtest target or -fsanitize=thread) is
 * the direct proof that race is closed. */
TEST_F(engine_stop_fixture, concurrent_destroy_and_engine_stop_is_safe) {
  /* Same safety-net rationale as force_stop_drains_in_flight_request_
     before_reactor_teardown's own identical use of this hook: guards
     against srv's scope-exit chttpsvr_destroy() (below) falling back to the
     real, production-sized _wait_in_flight_bounded timers if a REQUIRE_*
     between here and chttpsvr_engine_stop() ever fires while a worker
     thread is genuinely still blocked inside _slow_handler. Restored to the
     real defaults before this test returns on any path. Does not affect
     this test's own main-path timing: the handler is normally released via
     _release_slow_handler_after_delay's own 150ms sleep, well before either
     of these shrunk (or the real, default) bounds would ever be reached. */
  _chttpsvr_set_wait_in_flight_bounds_for_tests(300, 2000);

  char *err = NULL;
  /* _ccol_destructor: see force_stop_while_started_then_destroy_is_safe's
     own comment above. Safe here specifically because every REQUIRE_* that
     could return early is positioned strictly before chttpsvr_engine_stop()
     is ever called below (no REQUIRE_* sits between chttpsvr_engine_stop()
     and the explicit chttpsvr_destroy(srv) a few lines later), so this
     destructor can never fire concurrently with the deliberate reaper-vs-
     this-thread destroy race the test itself exercises. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      create_chttpsvr(CLOG_INVALID, &err);
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
  /* Heap-allocated, not a stack local: see force_stop_drains_in_flight_
     request_before_reactor_teardown's own identical ctx declaration comment
     above for why. */
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
    free(ctx); /* no thread was ever created, so nothing can still touch it */
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
    /* req_th is otherwise leaked on this path; release the handler
       unconditionally (harmless no-op if it never actually started) and
       join req_th before failing; see force_stop_drains_in_flight_
       request_before_reactor_teardown's own identical fallback for the
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
    /* _release_slow_handler_after_delay never got created to do this
       itself; release the handler here instead so req_th (already live)
       can still finish before this fails, rather than leaking it. */
    pthread_mutex_lock(&g_slow_mtx);
    g_slow_go = true;
    pthread_cond_broadcast(&g_slow_cv);
    pthread_mutex_unlock(&g_slow_mtx);
    _fx_join(tau, req_th, NULL);
    _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  }
  REQUIRE_EQ(releaser_create_rv, 0);

  /* chttpsvr_engine_stop() is documented non-blocking: it only kicks off a
   * background reaper thread. A fixed sleep here (as an earlier version of
   * this test used) only guesses that the reaper thread (a real
   * thread_create()) has had enough time to win the race to quiesce srv
   * before chttpsvr_destroy() below runs; under a loaded CI/valgrind run,
   * a guess that turns out too short would silently let this thread's own
   * chttpsvr_destroy() win the race instead, exercising the OTHER,
   * already-safe interleaving without ever failing - a silent false pass
   * that never actually re-tests the regression this test exists to catch.
   * _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests() instead
   * confirms deterministically, with no guessed timing at all, that the
   * reaper thread has already claimed quiesce_state == CHTTPSVR_QS_
   * QUIESCING for srv (see that hook's own doc comment) before this thread
   * ever calls chttpsvr_destroy(), guaranteeing this call is the one
   * exercising the losing side every single run; released immediately
   * afterward so the reaper can proceed into its own real teardown work
   * (which then blocks on the still-in-flight /slow-race request exactly as
   * before, released by _release_slow_handler_after_delay's own 150ms
   * timer). Mirrors the identical, already-established use of this same
   * hook in fork_mid_quiesce_teardown_does_not_hang_child below. */
  extern void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void);
  extern void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void);
  extern void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void);
  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();

  chttpsvr_destroy(srv);

  _fx_join(tau, releaser_th, NULL);
  /* Checked, not a bare fire-and-forget call: see force_stop_drains_in_
     flight_request_before_reactor_teardown's own identical reasoning for
     why ctx must not be read or freed if req_th is somehow still running
     30s later. */
  bool req_th_joined = _fx_join(tau, req_th, NULL);
  _chttpsvr_set_wait_in_flight_bounds_for_tests(0, 0);
  REQUIRE_TRUE(req_th_joined);
  ccol_retval_t async_rv = ctx->rv;
  int async_status = ctx->status;
  free(ctx);
  REQUIRE_EQ((int)async_rv, (int)ccol_success);
  REQUIRE_EQ(async_status, 200);

  chttpsvr_engine_wait();

  /* Prove the engine came back up cleanly afterward. */
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
/* Regression test for a race between chttpsvr_start() and a concurrent      */
/* chttpsvr_engine_stop(), distinct from the races above (which all involve  */
/* an ALREADY-started server). Before the fix, a server registered itself    */
/* with servers_bundler.servers (the list _engine_force_stop_quiesce_all     */
/* walks) only at the very end of chttpsvr_start(), after it had already     */
/* taken a live engine reference and registered a live listener. A forced    */
/* chttpsvr_engine_stop() landing in that window could not see this server  */
/* at all, so its quiesce pass skipped it entirely and the reactor could be  */
/* torn down while chttpsvr_start() was still using it a few lines below     */
/* (e.g. about to call event_loop_add for the listener); and, since this     */
/* server's own started/listen_reg/contributed_to_engine bookkeeping was     */
/* never touched by that quiesce pass, a later chttpsvr_stop()/_destroy()    */
/* call on it would hand a stale reg to event_loop_remove(), calling it      */
/* against a reactor that may already be torn down or gone.                  */
/* Fixed with two changes in chttpsvr_start()/_quiesce_server_once(): (1)    */
/* the server registers with servers_bundler as soon as its engine reference */
/* is confirmed, not at the very end; (2) _quiesce_server_once() waits for   */
/* any in-flight _chttpsvr_resolve-protected call on this exact server (this */
/* one's own chttpsvr_start(), in particular) to finish before touching any  */
/* of its state, mirroring the wait __chttpsvr_destroy already did before   */
/* calling it, but now applied uniformly to chttpsvr_engine_stop()'s forced  */
/* path too. This test uses a white-box hook to deterministically pause     */
/* chttpsvr_start() at the exact point the fix targets, rather than relying  */
/* on a fixed sleep to probabilistically hit a race window a few             */
/* instructions wide. */
/* ========================================================================== */
extern void _chttpsvr_arm_start_race_hook_for_tests(void);
extern void _chttpsvr_wait_start_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_start_race_hook_for_tests(void);

static chttpsvr g_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_race_start_rv = ccol_success;
/* Set only as the very last thing this thread function does, strictly after
   g_race_srv/g_race_start_rv have both been fully written; the calling test
   below polls this flag (with a bound) before ever reading either of those
   two plain, non-atomic globals or joining this thread, so the atomic
   store/load pair here is what actually establishes a real happens-before
   relationship for them; a bare, unguarded pthread_join with no
   preceding poll would otherwise be a genuine data race on both globals
   under the C11 memory model, independent of whether the join itself
   happens to return quickly in practice. */
static _Atomic bool g_race_start_returned = false;

static void *_start_racing_server_thread(void *arg) {
  (void)arg;
  char *err = NULL;
  g_race_srv = create_chttpsvr(CLOG_INVALID, &err);
  if (!g_race_srv) {
    fprintf(stderr, "FATAL: create_chttpsvr failed: %s\n", err ? err : "?");
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
    /* The hook is already armed above with nothing now ever going to enter
       it; _start_race_hook_wait_if_armed only ever re-checks its own `go`
       flag once it finds `armed` still true, so pre-setting `go` here
       (rather than leaving it unset) makes any later, unrelated
       chttpsvr_start() call elsewhere in this binary that happens to land
       on this hook sail through immediately instead of blocking forever on
       a release that was never going to come. */
    _chttpsvr_release_start_race_hook_for_tests();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until chttpsvr_start() has confirmed its engine reference and
   * registered with servers_bundler, and is now paused right there; the
   * exact window the fix closes. */
  _chttpsvr_wait_start_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* Give chttpsvr_engine_stop()'s reaper thread time to actually spawn and
   * reach (and, with the fix in place, start blocking inside)
   * _quiesce_server_once's own wait for this exact server's in-flight
   * chttpsvr_start() call to finish, the same way _release_slow_handler_
   * after_delay's own 150ms above gives a racing reaper time to win a
   * similar race. Not required for this test to be meaningful either way
   * (see the fix's own comment for why the hook's blocking, not this sleep,
   * is what makes the race deterministic), only for it to land on the more
   * interesting of the two safe interleavings more often. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let chttpsvr_start() finish. With the fix, a reaper that reached
   * this server is blocked waiting for exactly this; the shared reactor
   * cannot be torn down until chttpsvr_start() has released its own resolve
   * pin. */
  _chttpsvr_release_start_race_hook_for_tests();

  /* Bounded poll, not a blind pthread_join: if the fix ever regresses,
     start_th self-deadlocks forever, and this is what turns that into a
     clean, attributable REQUIRE_TRUE(race_returned) failure instead of an
     unbounded hang. This flag is also what actually establishes a real
     happens-before relationship for g_race_srv/g_race_start_rv below, both
     plain, non-atomic globals start_th wrote: reading them, or joining
     start_th, without first confirming this flag would be a genuine data
     race under the C11 memory model, independent of whether the join
     itself happens to return quickly in practice. */
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
    return; /* would hang forever; nothing further to check safely */
  _fx_join(tau, start_th, NULL);
  /* g_race_srv is a global (written by the racing background thread, so it
     cannot carry a scope-exit _ccol_destructor the way a local handle
     would); clean it up explicitly before asserting on race_rv here so a
     REQUIRE_* failure at this specific point doesn't leak it; this file
     has no _teardown()/atexit() to catch it, and a leaked, still-registered,
     still-engine-referencing server here would be left behind for every
     later test in this binary. Every REQUIRE_* below this point already sits
     after g_race_srv's own final chttpsvr_destroy() call further down, so
     none of them need the same treatment. */
  ccol_retval_t race_rv = g_race_start_rv;
  if (race_rv != ccol_success) {
    if (g_race_srv != CHTTPSVR_INVALID) chttpsvr_destroy(g_race_srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)race_rv, (int)ccol_success);
  }

  chttpsvr_engine_wait();

  /* The real assertion: neither of these crashes or hangs. Before the fix,
   * g_race_srv's listen_reg could point into an already-destroyed event_loop
   * by this point, making chttpsvr_destroy() a genuine use-after-free. */
  chttpsvr_destroy(g_race_srv);

  /* Prove the engine came back up cleanly afterward. */
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
/* Two more regression tests for the identical deadlock class as the test    */
/* just above, but for a narrower, deeper window: a chttpsvr_start() call     */
/* that has already registered and already confirmed contributed_to_engine,  */
/* but is still inside (or about to enter) its own _engine_acquire() call,   */
/* racing a reap that has already claimed this exact server's quiesce and is */
/* blocked waiting for this call's own resolve pin to drop. Before the fix,  */
/* _engine_acquire() blocked on srv_engine_bundler.stopping while still      */
/* holding that pin; and the reap could never finish, since it needed the    */
/* pin to drop first: a genuine, permanent deadlock, not merely a slow race, */
/* reachable via either of two independent triggers: a forced                */
/* chttpsvr_engine_stop() call, or the graceful reap triggered when some     */
/* other server's own chttpsvr_destroy() drops the shared engine's last      */
/* reference. Both variants below use a dedicated white-box hook             */
/* (_chttpsvr_arm_engine_stopping_race_hook_for_tests and its wait/release   */
/* siblings, paused right before the racing chttpsvr_start() call's own      */
/* _engine_acquire() call) to deterministically land in exactly that window, */
/* rather than relying on a fixed sleep to probabilistically hit a race that */
/* would otherwise be only a few instructions wide. Each creates its own S1  */
/* (rather than relying on an earlier test in this binary having already left */
/* a reactor live) so both are self-contained and order-independent. */
/* ========================================================================== */
extern void _chttpsvr_arm_engine_stopping_race_hook_for_tests(void);
extern void _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_engine_stopping_race_hook_for_tests(void);

/* Private per-test-invocation state for the two force-stop/graceful-reap-vs-
 * concurrent-start races below (start_racing_a_concurrent_forced_reap_
 * retries_and_succeeds and start_racing_a_concurrent_graceful_reap_retries_
 * and_succeeds). A single, file-scope-global srv2/start_rv/start_returned
 * triple was used here originally, matching _destroy_hang_ctx_t's own
 * documented rationale (tests.c) for the identical bug class: both tests run
 * back-to-back, and each one's bounded-poll-then-bail path (see
 * ENGINE_STOP_FIXTURE's own opening comment) deliberately leaves start_th
 * tracked-but-unjoined rather than blocking on it when it looks stuck. If
 * that thread is merely slow (not genuinely hung) and later writes into a
 * shared global after the NEXT test has already reset it and is racing its
 * own freshly-created thread for the same globals, the later test can
 * observe a value neither of its own two writers actually produced. Heap-
 * allocating the whole ctx per test invocation, and only freeing it once
 * _fx_join has actually confirmed the thread that owns it has terminated,
 * makes this structurally impossible: an abandoned thread from an earlier
 * test can only ever write into its OWN, still-live ctx, never a later
 * test's. */
typedef struct {
  uint16_t port;
  chttpsvr srv;
  ccol_retval_t start_rv;
  /* Set only as the very last thing this thread function does, strictly
     after srv/start_rv have both been fully written; every caller polls this
     flag (with a bound) before ever reading either of those two plain,
     non-atomic fields or joining this thread. Without that poll, reading
     them or joining the thread is a genuine data race under the C11 memory
     model. */
  _Atomic bool returned;
} _stopping_race_ctx_t;

static void *_start_stopping_race_server_thread(void *arg) {
  _stopping_race_ctx_t *ctx = (_stopping_race_ctx_t *)arg;
  char *err = NULL;
  ctx->srv = create_chttpsvr(CLOG_INVALID, &err);
  if (!ctx->srv) {
    fprintf(stderr, "FATAL: create_chttpsvr failed: %s\n", err ? err : "?");
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
    /* srv1 is already live/registered/engine-referencing at this point;
       destroy it before failing so a transient OOM here doesn't leak it for
       the rest of this binary's run, matching the pthread_create-failure
       cleanup a few lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18821;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* The hook is already armed above with nothing now ever going to enter
       it; _engine_stopping_race_hook_wait_if_armed only ever re-checks its
       own `go` flag once it finds `armed` still true, so pre-setting `go`
       here (rather than leaving it unset) makes any later, unrelated
       chttpsvr_start() call elsewhere in this binary that happens to land on
       this hook sail through immediately instead of blocking forever on a
       release that was never going to come. */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* no thread was ever created, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  /* Block until the racing chttpsvr_start() call has confirmed its engine
   * contribution and registered with servers_bundler, and is now paused
   * right before its own _engine_acquire() call; the exact window the
   * fix targets. */
  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  chttpsvr_engine_stop();

  /* Give chttpsvr_engine_stop()'s reaper thread time to actually spawn,
   * quiesce srv1 (also registered, and also force-stopped by this same
   * call), and reach (and, with the fix in place, correctly NOT block
   * inside) _quiesce_server_once's own wait for srv2's in-flight
   * chttpsvr_start() call to finish. Not required for this test to be
   * meaningful either way, only for it to land on the more interesting of
   * the two safe interleavings more often. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Now let the racing chttpsvr_start() call proceed into
   * _engine_acquire(): with the fix, it observes stopping == true, returns
   * immediately without blocking, and the caller unwinds and retries rather
   * than deadlocking against the reaper. */
  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* Bounded poll, not a blind pthread_join: establishes a real happens-
     before relationship for ctx->srv/ctx->start_rv below (both plain,
     non-atomic fields start_th wrote) before this thread ever reads them or
     joins start_th, matching ctx->returned's own identical rationale above.
     Also what turns a genuine regression (start_th self-deadlocked) into a
     clean, attributable REQUIRE_TRUE failure instead of an unbounded hang.
     ctx is deliberately NOT freed on the bail-out branch below: start_th may
     still be running and will go on to write into it (see ctx's own type
     comment); the fixture's bounded-join teardown sweep is what eventually
     joins-or-abandons start_th, never this function. */
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
    /* srv1 is unrelated to the stuck start_th, so it's still safe to
       destroy directly here. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(stopping_race_returned);
    return; /* would hang forever; nothing further to check safely */
  }
  _fx_join(tau, start_th, NULL);

  /* srv1 was already quiesced by the force-stop above; still explicitly
   * destroy it to release the handle, matching this file's own convention
   * for a server force-stopped while still live. */
  chttpsvr_destroy(srv1);

  /* The real assertion: chttpsvr_start() on srv2 must transparently retry
   * and succeed, not surface the transient engine-stopping race as a hard
   * failure to its own caller (and, before the fix, this line is never
   * reached at all: the racing thread is deadlocked forever). start_th is
   * confirmed joined above, so ctx is safe to read and free from here on. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_INVALID) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* No chttpsvr_engine_wait() call here: unlike the pre-existing start-
   * race test above (whose racing server never actually contributes an
   * engine reference before being caught by the reap and torn down along
   * with it), this call succeeds by transparently retrying AFTER the first
   * reap has already fully finished, so srv2 is now a genuinely, correctly
   * RUNNING server on a freshly re-created reactor with nothing asking it
   * to stop; waiting for the engine to fully stop at this point would
   * wait forever for a stop that never comes. */

  /* Prove srv2 actually works, not merely that chttpsvr_start() returned
   * success. Captured into a local and srv2 destroyed unconditionally before
   * either REQUIRE_* below, so a genuine regression in this exact retry-
   * then-serve path (what this test exists to catch) cannot leave a live,
   * still-engine-referencing server behind for every later test in this
   * binary to potentially trip over. */
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
    /* srv1 is already live/registered/engine-referencing at this point;
       destroy it before failing so a transient OOM here doesn't leak it for
       the rest of this binary's run, matching the pthread_create-failure
       cleanup a few lines below. */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  ctx->port = 18823;
  _chttpsvr_arm_engine_stopping_race_hook_for_tests();

  pthread_t start_th;
  if (pthread_create(&start_th, NULL, _start_stopping_race_server_thread,
                     ctx) != 0) {
    /* See the sibling forced-reap test's identical comment above for why
       this releases (rather than merely disarms) a hook nothing ever
       actually entered. */
    _chttpsvr_release_engine_stopping_race_hook_for_tests();
    free(ctx); /* no thread was ever created, so nothing can still touch it */
    chttpsvr_destroy(srv1);
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, start_th);

  _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests();

  /* Destroying srv1 (the only OTHER live server referencing the shared
   * engine at this point, since srv2's own racing chttpsvr_start() call is
   * paused before ever calling _engine_acquire() and so has not yet
   * incremented the shared reactor_refs counter itself) drops
   * reactor_refs to 0, triggering the graceful reap path (_engine_release())
   * instead of the forced chttpsvr_engine_stop() path the sibling test
   * above exercises. Both paths feed the identical _engine_reaper_fn ->
   * _engine_force_stop_quiesce_all() -> _quiesce_server_once() chain from
   * here on, so the rest of this test is otherwise the same. */
  chttpsvr_destroy(srv1);

  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  _chttpsvr_release_engine_stopping_race_hook_for_tests();

  /* Bounded poll, not a blind pthread_join; see the sibling forced-reap
     test's identical comment above for why. srv1 was already destroyed
     above (that's what triggers the graceful reap this test exercises), so
     there is nothing left to clean up on the bailout path here. */
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
    return; /* would hang forever; nothing further to check safely (ctx is
               deliberately leaked here; see its own type comment) */
  _fx_join(tau, start_th, NULL);

  /* start_th is confirmed joined above, so ctx is safe to read and free. */
  chttpsvr srv2 = ctx->srv;
  ccol_retval_t start_rv = ctx->start_rv;
  free(ctx);
  if (start_rv != ccol_success) {
    if (srv2 != CHTTPSVR_INVALID) chttpsvr_destroy(srv2);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }

  /* No chttpsvr_engine_wait() call here; see the sibling forced-reap test's
   * identical comment above for why: srv2 is now genuinely, correctly
   * RUNNING on a freshly re-created reactor, with nothing asking it to
   * stop. */

  /* Captured into a local and srv2 destroyed unconditionally before either
     REQUIRE_* below; see the sibling forced-reap test's identical comment
     above for why. */
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
/* chttpsvr_engine_stop() is documented (chttpserver.h) as "async-signal-    */
/* safe: safe to call from a signal handler"; the whole point being that     */
/* an application installs it (or a thin wrapper) as a SIGTERM/SIGINT        */
/* handler for graceful shutdown. Its real work used to run directly on      */
/* whichever thread called it: mutex_lock(srv_engine_bundler.mutex), and,    */
/* when a reactor was live, thread_create() via _spawn_reaper(). Neither is  */
/* POSIX-guaranteed async-signal-safe, and this was a genuine, reachable     */
/* self-deadlock, not a theoretical one: srv_engine_bundler.mutex is also    */
/* taken by _engine_acquire() (chttpsvr_start()), _engine_release()          */
/* (chttpsvr_destroy()), and the chttpsvr_set_engine_*() setters, so a       */
/* signal arriving on the thread currently inside any one of those calls     */
/* (e.g. SIGTERM delivered mid-chttpsvr_start(), a realistic race during a   */
/* container's shutdown/rolling-restart sequence) would have the            */
/* handler's own mutex_lock() self-deadlock against the lock that same      */
/* thread already held, hanging the whole process (surviving only           */
/* SIGKILL).                                                                 */
/*                                                                           */
/* Fixed by moving every mutex/thread_create-touching step onto a           */
/* dedicated, always-running watcher thread (g_engine_stop_watcher in       */
/* chttpserver.c), woken via sem_post() (the one synchronization            */
/* primitive POSIX explicitly lists as async-signal-safe), so               */
/* chttpsvr_engine_stop() itself never calls mutex_lock/thread_create,      */
/* directly or indirectly, regardless of what the interrupted thread was    */
/* doing.                                                                   */
/*                                                                           */
/* This test reproduces the exact hazard deterministically, without relying */
/* on timing: a white-box hook locks srv_engine_bundler.mutex itself, then  */
/* raise()s SIGUSR1 while still holding it; raise() in a multi-threaded     */
/* program runs the signal's handler synchronously, on the same thread,     */
/* before returning, exactly modeling "a signal interrupts a thread that    */
/* already holds this mutex." The installed SIGUSR1 handler calls           */
/* chttpsvr_engine_stop(). Before the fix this call sequence deadlocks      */
/* forever; the bounded wait below turns that into a clean test failure     */
/* instead of hanging the whole suite.                                      */
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
  /* A started server guarantees the shared engine (and therefore the
   * signal-safe stop watcher, started as the very first thing
   * _engine_acquire() does under srv_engine_bundler.mutex) already
   * exists and is ready before this test provokes the race. */
  /* Deliberately NOT _ccol_destructor-scoped, unlike this file's other
     tests: the REQUIRE_TRUE(done) a few lines below can legitimately fail
     on a genuine regression (the thread spawned below self-deadlocked
     forever, still holding srv_engine_bundler.mutex); an automatic
     scope-exit chttpsvr_destroy(srv) firing at that exact point would itself
     block forever trying to acquire that same still-held mutex (via
     _engine_release() inside _quiesce_server_once()), turning today's clean,
     reported test failure back into the exact whole-process hang this fix
     exists to avoid elsewhere. srv is instead cleaned up explicitly, only at
     the two earlier checkpoints below where no such regression could
     possibly be in progress yet (nothing has touched srv_engine_bundler.mutex
     on any other thread at those two points), and deliberately left
     un-cleaned-up on the done==false path, matching that path's own existing,
     documented reasoning. */
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

  /* Bounded wait, not a join: if the fix ever regresses, the thread above
   * self-deadlocks forever inside chttpsvr_engine_stop() (called
   * synchronously from SIGUSR1's own handler while still holding
   * srv_engine_bundler.mutex). A REQUIRE_TRUE(done) failure below is the
   * regression signal; this fixture's own teardown attempts a further
   * bounded (30s) join on th afterward and, failing that, detaches it
   * rather than hanging the rest of this binary's tests; see the
   * engine_stop_fixture teardown's own comment above for why this is safe
   * even though th may hold srv_engine_bundler.mutex forever on this path. */
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
  if (!done) return; /* would hang forever; nothing further to check safely */
  _fx_join(tau, th, NULL);

  /* chttpsvr_engine_stop() genuinely ran (not just returned without
   * deadlocking): confirm the engine actually stops. */
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  /* Prove the engine (and its watcher thread, which survives a stop/start
   * cycle (it is never torn down except at process exit) comes back up
   * cleanly afterward. Safe to RAII-scope from here on: reaching this point
   * already proves done==true, so the self-deadlock scenario srv was
   * deliberately left unscoped for above cannot be in progress. */
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
/* Regression test for a real deadlock between chttpsvr_start() and a         */
/* concurrent chttpsvr_engine_stop() force-quiesce pass on an already-        */
/* registered, currently-stopped server.                                     */
/*                                                                            */
/* chttpsvr_start() holds its own _chttpsvr_resolve() pin (pending_resolve_   */
/* count) for its entire call, including while checking whether a            */
/* _quiesce_server_once pass for this exact server is already in progress    */
/* (quiesce_state == CHTTPSVR_QS_QUIESCING). _quiesce_server_once itself,     */
/* before doing any of its real teardown work (the work that would           */
/* eventually reach CHTTPSVR_QS_QUIESCED and unblock a waiter), first        */
/* blocks until pending_resolve_count reaches 0; so if chttpsvr_start()      */
/* were to wait on quiesce_done_cv while STILL holding that exact pin, the   */
/* two calls deadlock each other: chttpsvr_start() waits for a signal only   */
/* _quiesce_server_once can send, and _quiesce_server_once waits for a pin   */
/* only chttpsvr_start() can release. This is a genuine, ordinary-looking    */
/* race (an entirely normal chttpsvr_stop()-then-chttpsvr_start() restart    */
/* racing a concurrent chttpsvr_engine_stop()), not a contrived edge case.   */
/*                                                                            */
/* Fixed by having chttpsvr_start() fully release its pin (the same          */
/* _chttpsvr_resolve_unpin every other exit path already uses) before        */
/* backing off and re-resolving the handle from scratch, rather than         */
/* blocking while still pinned.                                              */
/*                                                                            */
/* This test reproduces the exact interleaving deterministically, via a      */
/* dedicated white-box hook that pauses chttpsvr_start() immediately after   */
/* it resolves (pin acquired) but before it does anything else; rather       */
/* than relying on a fixed sleep to probabilistically land two threads at    */
/* the right instant, the hook guarantees the racing chttpsvr_start() call's  */
/* pin is already held before chttpsvr_engine_stop()'s own reaper thread     */
/* ever reaches _quiesce_server_once's pending_resolve_count wait for it.    */
/* ========================================================================== */
extern void _chttpsvr_arm_start_resolve_race_hook_for_tests(void);
extern void _chttpsvr_wait_start_resolve_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_start_resolve_race_hook_for_tests(void);

/* Heap-allocated (calloc'd by each call site below) rather than a shared
 * file-scope global or a caller's own stack local, for the identical reason
 * _stopping_race_ctx_t (above) and _destroy_hang_ctx_t (tests.c) are: both
 * call sites below deliberately leave restart_th tracked-but-unjoined on
 * their own bounded-poll-then-bail path when it looks stuck, rather than
 * blocking on a possibly-genuinely-deadlocked thread (that deadlock IS the
 * regression start_does_not_deadlock_against_concurrent_quiesce_pass exists
 * to catch). A shared global for start_rv/returned would let a merely-slow
 * (not actually hung) abandoned thread from one test corrupt the other
 * test's freshly-reset copy once it eventually finishes; a stack-local arg
 * would be worse still (the struct itself gone once the bailing-out test
 * function returns). Heap-allocating per invocation and only freeing once
 * _fx_join has confirmed the owning thread has actually terminated makes
 * both classes of corruption structurally impossible. srv/port themselves
 * are only ever read once, synchronously, at the very top of the thread
 * function below (see its own comment), so unlike start_rv/returned they
 * would be safe as plain fields either way; kept in the same struct anyway
 * since it's already heap-allocated for the other two fields. */
typedef struct {
  chttpsvr *srv;
  int port;
  ccol_retval_t start_rv;
  /* Set only as the very last thing this thread function does, strictly
     after start_rv has been fully written; every caller polls this flag
     (with a bound) before ever reading start_rv or joining this thread.
     Without that poll, doing either is a genuine data race under the C11
     memory model. */
  _Atomic bool returned;
} resolve_racing_start_arg_t;

/* port/srv are passed in rather than hardcoded, so this restart always
   targets the exact port/handle the calling test's own srv was actually
   started on; both call sites below start srv on different ports of their
   own. Both are read exactly once, here, before this function does anything
   else observable to the racing main thread (which only proceeds past its
   own "hook entered" wait after this call has resolved srv and paused), so
   neither needs the same heap-allocated-for-safe-late-write treatment
   start_rv/returned (written at the very end, possibly much later on the
   regression path under test) need; see the struct's own comment above. */
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
  /* srv is deliberately NOT RAII-scoped, unlike most of this file's other
     tests: this test's entire premise is that, under the exact regression
     it guards against, restart_th's own chttpsvr_start() call on srv can
     become permanently stuck (a two-condvar deadlock against the reaper
     thread). __chttpsvr_destroy() itself would then also block forever
     waiting for srv's pending_resolve_count to drain; an RAII
     destructor firing at this function's own scope exit on the `!returned`
     bailout path (the exact path that exists specifically to avoid hanging
     the whole binary when this regression fires) would immediately hang the
     whole binary right back, defeating the entire point of the bounded wait
     a few lines below. This is the identical reasoning
     engine_stop_from_signal_handler_does_not_deadlock,
     start_racing_engine_stop_and_destroy_does_not_free_raw_too_early, and
     both fork tests in this same file already apply to their own analogous
     "background thread could hold my server's pin forever" scenario; this
     test was the one outlier still using RAII for it. Every early-exit path
     below that is proven safe (nothing yet racing srv) destroys srv
     explicitly instead; the `!returned` bailout deliberately does not. */
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(CLOG_INVALID, &err);
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

  /* An ordinary stop: srv stays registered in servers_bundler and keeps its
     engine reference (chttpsvr_stop() only closes the listener), exactly the
     state a real chttpsvr_stop()-then-chttpsvr_start() restart begins from. */
  chttpsvr_stop(srv);

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* srv is already registered/engine-referencing (stopped, not destroyed)
       at this point, and nothing races it yet (the race hook has not been
       entered by anyone), so it is safe to destroy explicitly here, unlike
       the `!returned` bailout further below; see this test's own opening
       comment. */
    chttpsvr_destroy(srv);
    REQUIRE_TRUE(false);
  }
  restart_arg->srv = &srv;
  restart_arg->port = 18807;
  pthread_t restart_th;
  if (pthread_create(&restart_th, NULL, _resolve_racing_start_thread,
                     restart_arg) != 0) {
    /* Nothing is racing srv yet, so it's still safe to destroy directly
       here. The hook is already armed above with nothing now ever going to
       enter it; _start_resolve_race_hook_wait_if_armed only ever re-checks
       its own `go` flag once it finds `armed` still true, so pre-setting
       `go` here (rather than leaving it unset) makes any later, unrelated
       chttpsvr_start() call elsewhere in this binary that happens to land on
       this hook sail through immediately instead of blocking forever on a
       release that was never going to come. */
    _chttpsvr_release_start_resolve_race_hook_for_tests();
    free(restart_arg); /* no thread was ever created */
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_TRUE(false);
  }
  _fx_track(tau, restart_th);

  /* Deterministic: this returns only once restart_th's own chttpsvr_start()
     call has resolved srv (pin acquired) and is now paused at the hook,
     before it has touched anything else; no timing luck needed. */
  _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* chttpsvr_engine_stop() is documented non-blocking: it only kicks off a
     background reaper thread. */
  chttpsvr_engine_stop();

  /* Give the reaper thread time to actually run, reach _quiesce_server_once
     for srv, claim CHTTPSVR_QS_QUIESCING, and block on pending_resolve_count,
     which, thanks to the hook above, is guaranteed non-zero at that exact
     moment (restart_th's own pin). Matches this file's own established
     150ms precedent for "let the other side reach its own blocking point"
     synchronization elsewhere in this file. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* The actual moment under test: release restart_th's paused
     chttpsvr_start() call. With the bug, this call now proceeds to wait on
     quiesce_done_cv while still holding the exact pin the reactor thread
     above is blocked waiting for; a permanent, two-condvar deadlock. With
     the fix, it releases that pin and backs off instead. */
  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* Bounded wait, not a blind pthread_join: if the fix ever regresses,
     restart_th self-deadlocks forever (and, transitively, so does the
     reactor's own reaper thread, and chttpsvr_engine_wait() below). A
     REQUIRE_TRUE(returned) failure is the regression signal; this fixture's
     own teardown attempts a further bounded (30s) join on restart_th
     afterward and, failing that, detaches it rather than hanging the rest
     of this binary's tests; matching engine_stop_from_signal_handler_
     does_not_deadlock's own identical precedent elsewhere in this file. */
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
    return; /* would hang forever; nothing further to check safely.
                restart_arg deliberately leaked; see its own type comment */
  _fx_join(tau, restart_th, NULL);

  /* restart_th has now provably returned, so srv is no longer being raced
     by anything: safe to destroy unconditionally from here on, and
     restart_arg is safe to read and free. Captured into a local and cleaned
     up before asserting, so a genuine regression in restart_th's own
     chttpsvr_start() call (this test's own real assertion) cannot leave a
     live, still-engine-referencing srv behind for every later test in this
     binary to potentially trip over. */
  ccol_retval_t restart_rv = restart_arg->start_rv;
  free(restart_arg);
  if (restart_rv != ccol_success) {
    chttpsvr_destroy(srv);
    chttpsvr_engine_wait();
    REQUIRE_EQ((int)restart_rv, (int)ccol_success);
  }

  /* Deliberately no chttpsvr_engine_wait() before this next check: restart_
     th's own successful chttpsvr_start() necessarily acquired a FRESH
     engine reference and reactor (the original one was already torn down by
     the chttpsvr_engine_stop() call above, which this restart raced), and
     nothing has asked that new reactor to stop yet; chttpsvr_engine_wait()
     would simply block waiting for it, unrelated to whether the deadlock
     under test actually occurred. srv having successfully restarted and
     served a real request below is itself the proof chttpsvr_engine_stop()
     ran to completion (its own reaper thread had to finish releasing the
     old reactor before this restart's own _engine_acquire could create a
     new one).
     The real assertion: srv is genuinely, cleanly restarted, not merely
     "chttpsvr_start returned success" while the reactor is still wedged. */
  int status2 = 0;
  ccol_retval_t get_rv = _get("http://127.0.0.1:18807/hello", &status2);

  chttpsvr_destroy(srv);
  chttpsvr_engine_wait();

  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status2, 200);
}

/* ========================================================================== */
/* Regression test for a real, reproducible use-after-free in                 */
/* chttpsvr_start()'s own CHTTPSVR_QS_QUIESCING backoff branch: it used to    */
/* release its resolve pin BEFORE registering itself in quiesce_waiters, not  */
/* after. Releasing the pin first can immediately unblock a concurrent        */
/* _quiesce_server_once() pass's own pending_resolve_count wait; if that pass */
/* (driven here by chttpsvr_engine_stop()'s forced-quiesce reaper) then ran   */
/* to completion and found quiesce_waiters still zero (this call hadn't       */
/* re-acquired raw->mutex to increment it yet), a genuinely concurrent        */
/* chttpsvr_destroy() call on the very same handle could see that pass finish */
/* and free raw while chttpsvr_start() was still on its way back to           */
/* raw->mutex for the very first time since releasing its pin; a real        */
/* lock-on-freed-memory use-after-free, not merely a theoretical one. Fixed   */
/* by incrementing quiesce_waiters in the same critical section that observes */
/* CHTTPSVR_QS_QUIESCING, strictly before the pin is released; see that       */
/* branch's own doc comment in chttpserver.c for the full account.            */
/*                                                                            */
/* Reproduced deterministically via a dedicated white-box hook that pauses a  */
/* real chttpsvr_start() call exactly in the window the fix closes: after it  */
/* has both registered itself in quiesce_waiters and released its resolve     */
/* pin, but strictly before it ever re-acquires raw->mutex again. With the    */
/* reaper thread (unblocked by that pin release, and left to run its own     */
/* real, here essentially instant, teardown work to completion with no hook   */
/* of its own) and a genuinely concurrent chttpsvr_destroy() call both then   */
/* driven around that paused call, the destroy call must stay blocked (on the */
/* reaper's own still-outstanding servers_bundler_pins, itself kept alive by  */
/* the reaper's own wait for this call's quiesce_waiters registration to      */
/* clear) for as long as this call remains paused; proving raw survives the  */
/* exact window the original bug would have freed it in, rather than merely   */
/* asserting "nothing crashed" after the fact. Confirmed, by temporarily      */
/* moving the quiesce_waiters++ back to after the pin release/relock (the     */
/* pre-fix shape) and rerunning under AddressSanitizer, that this exact test  */
/* reliably fails there (a heap-use-after-free on raw->mutex) before the fix  */
/* was reapplied.                                                             */
/* ========================================================================== */
extern void _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests(void);
/* Bounded (10s internally): returns false, rather than hanging forever, if
   restart_th's own paused chttpsvr_start() call never reaches this hook at
   all (see this function's own doc comment in chttpserver.c for why that
   can happen under environment load, unlike every other hook in this
   file). */
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
  /* Same discipline as every other hook-based test in this file: every
     intermediate outcome is captured into a local instead of asserted on
     immediately, and every armed hook / created thread is released and
     joined unconditionally before any REQUIRE_* runs, so a failing
     assertion here can never leave a thread permanently parked inside one
     of the two hooks below (wedging every later test in this binary that
     touches the same machinery). */
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(CLOG_INVALID, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_register_handler(srv, CHTTP_GET, "/hello", _hello_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18816;
  /* Not RAII-scoped, unlike most of this file's other tests: srv is about to
     alias g_start_uaf_race_srv, which _start_uaf_race_destroy_thread below
     destroys directly on a background thread, and this codebase's own
     sequential_double_destroy_is_fatal test proves a second destroy of the
     same handle is fatal even non-concurrently; an RAII destructor firing
     here at scope exit on top of that thread's own destroy would be exactly
     that. This one early window, before g_start_uaf_race_srv is ever
     assigned or any background thread starts touching srv, has no such
     conflict, so a start failure here is cleaned up explicitly instead. */
  ccol_retval_t start_rv = chttpsvr_start(srv, &cfg);
  if (start_rv != ccol_success) {
    chttpsvr_destroy(srv);
    REQUIRE_EQ((int)start_rv, (int)ccol_success);
  }
  /* Same "ordinary stop-then-restart" starting state as
     start_does_not_deadlock_against_concurrent_quiesce_pass above: srv stays
     registered and keeps its engine reference, matching a real
     chttpsvr_stop()-then-chttpsvr_start() restart. */
  chttpsvr_stop(srv);
  g_start_uaf_race_srv = srv;

  _chttpsvr_arm_start_resolve_race_hook_for_tests();

  resolve_racing_start_arg_t *restart_arg =
      (resolve_racing_start_arg_t *)calloc(1,
                                           sizeof(resolve_racing_start_arg_t));
  if (!restart_arg) {
    /* g_start_uaf_race_srv aliases srv but no background thread has been
       spawned yet at this point (that only happens a few lines below), so
       destroying srv directly and resetting the alias is safe here, unlike
       the `!returned` bailout further below where a background thread may
       already be racing it; see this test's own opening comment. */
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
    free(restart_arg); /* no thread was ever created */
  }
  if (restart_th_created)
    _chttpsvr_wait_start_resolve_race_hook_entered_for_tests();

  /* Arm the vulnerable-window hook before releasing restart_th: once
     released, restart_th observes CHTTPSVR_QS_QUIESCING (set by the reaper
     below), registers itself in quiesce_waiters, releases its own resolve
     pin, and pauses right there; before ever re-acquiring raw->mutex for
     the first time since releasing that pin. */
  _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests();

  if (restart_th_created) {
    chttpsvr_engine_stop(); /* documented non-blocking */
    /* Give the reaper thread time to actually run, reach
       _quiesce_server_once for srv, claim CHTTPSVR_QS_QUIESCING, and block
       on pending_resolve_count, guaranteed non-zero at that exact moment
       thanks to restart_th's own still-held pin. Matches this file's own
       established 150ms precedent for "let the other side reach its own
       blocking point" synchronization elsewhere in this file. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
  }

  _chttpsvr_release_start_resolve_race_hook_for_tests();

  /* Unlike this file's other hook-based tests, reaching this hook is not
     provably deterministic from restart_th_created alone: it additionally
     depends on the reaper (kicked off by chttpsvr_engine_stop() above)
     having already set CHTTPSVR_QS_QUIESCING before restart_th's own
     paused chttpsvr_start() call re-checks quiesce_state, an ordering this
     test enforces only via the fixed nanosleep(150ms) above, not a genuine
     synchronization primitive. The wait function itself is internally
     bounded (10s) and returns false rather than hanging this whole binary
     if that race is ever missed under environment load; see its own doc
     comment in chttpserver.c. */
  bool hook_entered = false;
  if (restart_th_created) {
    hook_entered =
        _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests();
  }
  bool paused_at_vulnerable_window = !restart_th_created || hook_entered;

  /* At this exact point: restart_th has registered itself in quiesce_waiters
     and fully released its own resolve pin, and is paused before ever
     touching raw->mutex again. The engine_stop()-driven reaper, unblocked by
     that pin release, is left to run its own real teardown to completion on
     its own (essentially instant here, since srv has no live connections
     to drain, and deliberately no hook on its side) but its own tail wait
     for quiesce_waiters == 0 must find restart_th's registration still
     there and correctly block, keeping raw alive and servers_bundler_pins
     nonzero. */
  bool destroy_th_created = false;
  pthread_t destroy_th;
  if (paused_at_vulnerable_window) {
    atomic_store(&g_start_uaf_race_destroy_returned, false);
    destroy_th_created =
        pthread_create(&destroy_th, NULL, _start_uaf_race_destroy_thread,
                       NULL) == 0;
    if (destroy_th_created) _fx_track(tau, destroy_th);
  }

  /* Bounded negative check: chttpsvr_destroy() must not be able to return
     (and free raw) while restart_th is still parked in the exact window the
     fix protects. This is the direct, positive proof the fix works: without
     it, the concurrent reaper pass above would have found quiesce_waiters
     still zero, finished its own teardown, and let this destroy call return
     (freeing raw) well before restart_th ever got back to raw->mutex. */
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

  /* Release restart_th: it re-acquires raw->mutex (still safe, per the
     assertion just above), observes CHTTPSVR_QS_QUIESCED, decrements its own
     registration, and re-resolves srv; which will now fail, since
     destroy_th has already marked the slot not-in-use by this point. Safe/
     harmless to call even if restart_th never reached it. */
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
      /* restart_th confirmed joined: restart_arg is safe to read and free.
         Left deliberately leaked (never freed) on the !restart_returned
         path, since restart_th may still be running and go on to write into
         it; see the struct's own type comment above. */
      restart_rv = restart_arg->start_rv;
      free(restart_arg);
    }
  }
  /* restart_arg is already freed above when !restart_th_created (right after
     pthread_create failed): nothing further to do for that case here. */

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
    /* destroy_th was never created (either pthread_create failed, or
       hook_entered came back false; see this test's own hook_entered
       comment above), so nothing else in this test releases srv's own
       engine reference: without this, srv (successfully restarted by
       restart_th, since nothing destroyed it out from under that call)
       stays alive and registered, and the unconditional
       chttpsvr_engine_wait() below would otherwise block forever waiting
       for an engine reference that is never going away. Only reached once
       restart_th has already fully returned (restart_returned, checked and
       joined above), so this cannot race its own still-in-flight
       chttpsvr_start() call. */
    chttpsvr_destroy(srv);
  }

  /* chttpsvr_engine_wait() has no timeout of its own (see its own doc
     comment): it blocks until reactor_refs genuinely reaches zero. Calling
     it unconditionally here would itself be a fresh, self-inflicted hang
     risk in exactly the compound regression scenario this test exists to
     catch: if destroy_th_created is true but destroy_th's own
     chttpsvr_destroy() call never actually returns (a hypothetical
     regression re-introducing a deadlock in the very teardown path under
     test), the `if (destroy_th_created)` branch above never joins it and
     never falls through to the `else if` branch's own chttpsvr_destroy()
     call either; nothing has released srv's engine reference, so an
     unconditional wait here would block forever, turning a clean, reported
     test failure into a hang of the whole binary. The identical risk
     applies to restart_th's own chttpsvr_start() call, which also acquires
     (and, on every real completion path, correctly releases or keeps) an
     engine reference of its own. Only call it once both sides are confirmed
     to have actually returned (mirroring
     stop_racing_a_concurrent_start_does_not_bind_over_open_listener's own
     start_returned gate below). */
  bool engine_refs_should_settle = (!destroy_th_created || destroy_returned) &&
                                   (!restart_th_created || restart_returned);
  if (engine_refs_should_settle) chttpsvr_engine_wait();

  REQUIRE_TRUE(restart_th_created);
  REQUIRE_TRUE(hook_entered);
  REQUIRE_TRUE(destroy_th_created);
  REQUIRE_TRUE(restart_returned);
  REQUIRE_TRUE(destroy_returned);
  if (!restart_returned || !destroy_returned)
    return; /* one side is permanently stuck; nothing further to check
                safely */

  REQUIRE_FALSE(destroy_returned_too_early);
  /* restart_th necessarily lost the race: by the time it re-resolves srv,
     destroy_th has already marked the slot not-in-use. */
  REQUIRE_EQ((int)restart_rv, (int)ccol_invalid_args);
}

/* ========================================================================== */
/*   chttpsvr_stop() RACING A CONCURRENT chttpsvr_start() ON THE SAME HANDLE  */
/*                                                                            */
/* A plain chttpsvr_stop() call leaves raw->lifecycle == CHTTPSVR_LC_RUNNING */
/* BEFORE its own blocking event_loop_remove()/close() call for the OLD      */
/* listener registration returns, not after; unlike chttpsvr_destroy()/      */
/* chttpsvr_engine_stop(), both of which funnel through _quiesce_server_once */
/* and its own quiesce_state interlock, which chttpsvr_start()'s own wait    */
/* loop already accounted for. A chttpsvr_start() call racing a concurrent,  */
/* in-flight chttpsvr_stop() call on the same handle from a different        */
/* thread could therefore observe lifecycle == CHTTPSVR_LC_IDLE and          */
/* quiesce_state == CHTTPSVR_QS_NOT_QUIESCED all at once, and proceed        */
/* straight into _make_listen_socket()/bind() for a NEW listener on the      */
/* identical host:port while the OLD listener's own fd was still open and    */
/* bound; a real, if narrow and gracefully-failing (a spurious               */
/* ccol_unexpected_failure from a concurrent EADDRINUSE), race with no       */
/* protection at all before this test's own fix. Closed with a new           */
/* CHTTPSVR_LC_STOPPING lifecycle state, entered for the duration of         */
/* _chttpsvr_stop_internal()'s own real teardown work and checked by         */
/* chttpsvr_start()'s own wait loop, mirroring quiesce_state's exact shape.  */
/* This test uses a white-box hook to deterministically pause                */
/* chttpsvr_stop() inside that exact window, rather than relying on real,    */
/* unbounded timing to probabilistically hit a race window a few             */
/* instructions wide.                                                        */
/* ========================================================================== */
extern void _chttpsvr_arm_stop_race_hook_for_tests(void);
extern void _chttpsvr_wait_stop_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_stop_race_hook_for_tests(void);

static chttpsvr g_stop_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_stop_race_start_rv = ccol_success;
static _Atomic bool g_stop_race_start_returned = false;
/* Set as the last statement of _stop_racing_server_thread, after chttpsvr_
   stop() itself has genuinely returned. Consulted (bounded-polled) before
   the test below ever calls chttpsvr_destroy()/chttpsvr_engine_wait() on
   g_stop_race_srv, so a hypothetical regression re-hanging chttpsvr_stop()
   itself (e.g. inside its own event_loop_remove()/close() call) is reported
   as a clean, bounded test failure instead of risking those two later calls
   hanging in turn; mirroring this file's own established "confirm the
   racing thread actually returned before touching shared state further"
   discipline used throughout this file (e.g. g_race_start_returned above). */
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
  /* Every intermediate outcome below is captured into a local instead of
     asserted on immediately with REQUIRE_*, and every background thread is
     released/joined and g_stop_race_srv torn down UNCONDITIONALLY before any
     REQUIRE_* runs at all; mirroring bounded_pool_full_returns_503's own
     established discipline in tests.c for the identical reason: g_stop_race_
     srv is a plain global (it cannot carry a scope-exit _ccol_destructor the
     way a local handle would, since _start_racing_stop_thread writes
     g_stop_race_start_rv from a background thread; see force_stop_racing_a_
     concurrent_start_is_safe's own identical precedent above). A REQUIRE_*
     returning early from this function before the hook is released would
     leave stop_th permanently parked inside _chttpsvr_stop_internal's own
     hook wait forever (nothing else in this binary ever releases it), which
     in turn means g_stop_race_srv's listener fd is never actually closed;
     a leaked, permanently blocked thread and a leaked, still-registered,
     still-engine-referencing server for the remainder of this binary's run,
     not merely a failed assertion. */
  g_stop_race_srv = _start_server("18808", 18808);

  _chttpsvr_arm_stop_race_hook_for_tests();
  atomic_store(&g_stop_race_stop_returned, false);

  pthread_t stop_th;
  bool stop_th_created =
      pthread_create(&stop_th, NULL, _stop_racing_server_thread, NULL) == 0;
  if (stop_th_created) _fx_track(tau, stop_th);

  /* Deterministic once stop_th_created: this returns only once
     _chttpsvr_stop_internal() has already entered CHTTPSVR_LC_STOPPING and
     is now paused right there; strictly before its own event_loop_remove()/
     close() call, i.e. with the OLD listener fd still fully open and bound.
     No timing luck needed: _start_server()'s own fail-fast (exit(1) on a
     failed chttpsvr_start) already guarantees raw->lifecycle was
     CHTTPSVR_LC_RUNNING by the time g_stop_race_srv was assigned above, so
     _chttpsvr_stop_internal is guaranteed to actually reach the hook once
     stop_th runs, and this can never hang waiting for a hook stop_th will
     never reach. */
  if (stop_th_created) _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_stop_race_start_rv = ccol_success;
  atomic_store(&g_stop_race_start_returned, false);
  /* Armed before start_th is even created: chttpsvr_start() can reach its
     own CHTTPSVR_LC_STOPPING wait branch very quickly, so arming after
     creating the thread would risk missing the signal. */
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
    /* Deterministic, not a fixed sleep: this returns only once chttpsvr_
       start() has genuinely reached (and is about to enter) its own
       CHTTPSVR_LC_STOPPING wait loop - see that signal's own doc comment
       for why it is a plain, non-blocking "reached this point" signal
       rather than a park-until-release hook like g_stop_race_hook above.
       Since stop_th is still parked in g_stop_race_hook (not yet released
       below), lifecycle cannot have left CHTTPSVR_LC_STOPPING yet, so
       still_blocked is guaranteed true here every single run - no more
       guessing whether a fixed sleep was long enough for start_th to even
       be scheduled, which a genuinely unlucky, heavily loaded run could
       previously make read true for the wrong reason (not yet scheduled)
       rather than the right one (genuinely blocked by the fix). */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_stop_race_start_returned);
  }

  /* Release the hook (a safe, harmless call even if stop_th was never
     created to reach it) and join every thread that was actually created,
     unconditionally, before any REQUIRE_* below runs; see this test's own
     opening comment for why this ordering is load-bearing. Releasing lets
     chttpsvr_stop() finish (event_loop_remove()/close() for the OLD listener
     actually run, lifecycle leaves CHTTPSVR_LC_STOPPING), freeing the
     waiting chttpsvr_start() call (if any) to proceed. */
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

  /* Bounded wait, not a blind pthread_join: if the fix ever regresses into a
     hang instead of a race, this loop times out instead of hanging the
     whole test binary; this fixture's own teardown attempts a further
     bounded (30s) join on start_th afterward and, failing that, detaches it
     rather than hanging the rest of this binary's tests, matching this
     file's own established precedent for exactly this shape of
     bounded-wait-instead-of-pthread_join assertion elsewhere in this
     file. */
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

  /* Only attempt the real HTTP round-trip once the restart is confirmed to
     have actually finished and succeeded; calling out to a server that
     never (re)started, or whose own start thread is still permanently
     stuck, would just be a second, redundant way to hang or fail. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_stop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18808/hello", &status);

  /* Tear g_stop_race_srv down before any REQUIRE_* below, but only once
     stop_th is confirmed to have actually returned: a concurrent, still
     in-flight chttpsvr_start() on another thread is exactly the case this
     whole module's resolve/pin machinery, exercised elsewhere in this same
     file, is already designed to make memory-safe, so start_th being
     permanently stuck does not gate this; a hypothetical regression
     re-hanging chttpsvr_stop() ITSELF past the point the hook above already
     released it is a genuinely different, unproven risk: nothing establishes
     that chttpsvr_destroy()/chttpsvr_engine_wait() are safe to call
     concurrently with a chttpsvr_stop() call on the identical handle that is
     stuck somewhere in its own real teardown work rather than cleanly
     blocked on this test's own hook, so risking a second, compounding hang
     on top of the first is avoided by simply not calling either in that
     case; g_stop_race_srv is deliberately left leaked in that regression-only
     branch, matching the same trade-off this file's own start_th-stuck case
     already accepted below. */
  bool stop_teardown_safe = !stop_th_created || stop_returned;
  if (stop_teardown_safe) {
    chttpsvr_destroy(g_stop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(stop_th_created);
  REQUIRE_TRUE(stop_returned);
  if (!stop_returned)
    return; /* stop_th is permanently stuck; nothing further to check safely */
  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is permanently stuck; nothing further to check safely */
  /* The real assertion: no spurious EADDRINUSE-class failure; the restart
     succeeded cleanly once the old listener was genuinely gone. */
  REQUIRE_EQ((int)g_stop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/*  chttpsvr_start() RACING chttpsvr_engine_stop()'S OWN _chttpsvr_stop_      */
/*  INTERNAL() CALL (VIA _quiesce_server_once), NOT A PLAIN chttpsvr_stop()   */
/*                                                                            */
/* stop_racing_a_concurrent_start_does_not_bind_over_open_listener just above */
/* already exercises chttpsvr_start()'s own CHTTPSVR_LC_STOPPING wait for the */
/* case where a PLAIN chttpsvr_stop() call is what is driving                 */
/* _chttpsvr_stop_internal(). _chttpsvr_stop_internal() is also reachable via */
/* a second, structurally different caller: _quiesce_server_once() (the       */
/* engine-wide path chttpsvr_engine_stop()/chttpsvr_destroy() both funnel     */
/* through), which reaches CHTTPSVR_LC_STOPPING with quiesce_state already at */
/* CHTTPSVR_QS_QUIESCING (set before _quiesce_server_once() ever calls        */
/* _chttpsvr_stop_internal()) and via an entirely different set of pins       */
/* (servers_bundler_pins, not pending_resolve_count) than a plain             */
/* chttpsvr_stop() call uses. The CHTTPSVR_LC_STOPPING wait itself is coded   */
/* to be agnostic to which of the two drove it, but that claim had never been */
/* independently exercised by a test: every existing hook combination either  */
/* paused a plain chttpsvr_stop() call inside CHTTPSVR_LC_STOPPING (the test  */
/* above) or paused _quiesce_server_once() BEFORE it ever reaches             */
/* _chttpsvr_stop_internal (quiesce_state == QUIESCING, pending_resolve_count */
/* already drained; see the fork-safety tests further below), never both      */
/* together. This test closes that gap: it reuses the SAME _stop_race_hook    */
/* (_chttpsvr_stop_internal() pauses at the identical point regardless of     */
/* which caller reached it) but drives it via chttpsvr_engine_stop() instead  */
/* of a plain chttpsvr_stop() call, so a concurrent chttpsvr_start() call on  */
/* the same handle observes CHTTPSVR_LC_STOPPING with quiesce_state already   */
/* CHTTPSVR_QS_QUIESCING, exactly the interleaving the CHTTPSVR_LC_STOPPING   */
/* branch's own comment (chttpsvr_start(), see its own doc comment there)     */
/* reasons through but had no dedicated test for.                            */
/*                                                                            */
/* Unlike its two sibling backoffs (CHTTPSVR_QS_QUIESCING and "currently      */
/* stopping"), CHTTPSVR_LC_STOPPING's own move from a blind poll-and-retry to */
/* a genuine condvar wait is not a crash/hang/starvation fix for THIS specific
 */
/* driver: confirmed directly by temporarily reverting just that one branch   */
/* back to its own former "release pin; nanosleep(1ms); re-resolve; retry"    */
/* shape and re-running this exact test unchanged, which still passed         */
/* (still_blocked included) every time, precisely because _chttpsvr_stop_     */
/* internal() never waits on pending_resolve_count regardless of which caller */
/* reached it, so a blind re-pin from this branch specifically was never able */
/* to starve it out the way it could for the two siblings. This test's own    */
/* value is therefore proving the interleaving itself is safe (no hang, no    */
/* crash, no spurious EADDRINUSE, a clean rebuild of the whole shared engine */
/* once the quiesce pass that drove it finishes) for BOTH the current condvar-
 */
/* based implementation and the historical poll-based one, not distinguishing */
/* between them; the condvar-based fix's own real, distinct benefit (no       */
/* thousands-of-times-a-second busy-poll while a concurrent quiesce pass is */
/* draining, which can take far longer than a plain chttpsvr_stop() call's own
 */
/* comparatively fast listener teardown) is a CPU-efficiency property this */
/* single-shot pass/fail test cannot itself measure, mirroring this file's own
 */
/* dispatch-count-based tests (see e.g. post_accept_alloc_failure_backs_off_ */
/* instead_of_busy_looping in tests.c) for the general pattern such a */
/* measurement would need if ever added here.                                */
/* ========================================================================== */
static chttpsvr g_qstop_race_srv = CHTTPSVR_INVALID;
static ccol_retval_t g_qstop_race_start_rv = ccol_success;
/* Set as the last statement of _start_racing_quiesce_stop_thread, after
   chttpsvr_start() itself has genuinely returned; consulted (bounded-polled)
   before this test ever joins that thread or reads g_qstop_race_start_rv,
   mirroring g_stop_race_start_returned's own identical rationale above. */
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
  /* Every intermediate outcome below is captured into a local instead of
     asserted on immediately with REQUIRE_*, and every background thread is
     released/joined and g_qstop_race_srv torn down unconditionally before any
     REQUIRE_* runs at all, mirroring stop_racing_a_concurrent_start_does_not_
     bind_over_open_listener's own identical discipline just above and for the
     identical reason: a REQUIRE_* returning early here before the hook is
     released would leave the engine-stop reaper thread permanently parked
     inside _chttpsvr_stop_internal's own hook wait forever (nothing else in
     this binary ever releases it), hanging every later test in this binary
     that ever touches the shared engine again. */
  g_qstop_race_srv = _start_server("18817", 18817);

  _chttpsvr_arm_stop_race_hook_for_tests();

  /* chttpsvr_engine_stop() is itself non-blocking (it only posts to the
     signal-safe watcher thread's own semaphore and returns immediately; see
     its own doc comment); the actual reap this triggers runs on a separately
     spawned reaper thread, which is what will reach _quiesce_server_once() ->
     _chttpsvr_stop_internal() -> the armed hook. No background thread of this
     test's own is needed just to call it. */
  chttpsvr_engine_stop();

  /* Deterministic: this returns only once the reaper thread's own
     _chttpsvr_stop_internal() call has already entered CHTTPSVR_LC_STOPPING
     and is now paused right there, strictly before its own event_loop_
     remove()/close() call, i.e. with the OLD listener fd still fully open and
     bound, and with quiesce_state already CHTTPSVR_QS_QUIESCING (set by
     _quiesce_server_once() before it ever calls _chttpsvr_stop_internal()):
     exactly the interleaving this test exists to exercise. */
  _chttpsvr_wait_stop_race_hook_entered_for_tests();

  g_qstop_race_start_rv = ccol_success;
  atomic_store(&g_qstop_race_start_returned, false);
  /* Armed before start_th is even created; see stop_racing_a_concurrent_
     start_does_not_bind_over_open_listener's own identical use of this
     signal above for the full reasoning. */
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
    /* Deterministic, not a fixed sleep: the reaper thread is still parked
       in g_stop_race_hook (not yet released below), so lifecycle cannot
       have left CHTTPSVR_LC_STOPPING yet, guaranteeing still_blocked reads
       true here every single run once chttpsvr_start() genuinely reaches
       its own wait loop - see that signal's own doc comment. */
    _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests();
    still_blocked = !atomic_load(&g_qstop_race_start_returned);
  }

  /* Release the hook so the reaper's own _chttpsvr_stop_internal() call can
     finish (event_loop_remove()/close() for the OLD listener actually runs,
     lifecycle leaves CHTTPSVR_LC_STOPPING), freeing the waiting
     chttpsvr_start() call (if any) to proceed; it then falls through to
     observe quiesce_state == CHTTPSVR_QS_QUIESCING and waits there instead
     (already-established, already-tested machinery), until the reaper
     finishes draining/releasing/tearing down the shared engine entirely and
     this restart can genuinely rebuild it from scratch. */
  _chttpsvr_release_stop_race_hook_for_tests();

  /* Bounded poll, not a blind pthread_join: if the fix ever regresses,
     start_th self-deadlocks forever, and this is what turns that into a
     clean, attributable REQUIRE_TRUE(start_returned) failure instead of an
     unbounded hang. This flag is also what actually establishes a real
     happens-before relationship for g_qstop_race_start_rv below, a plain,
     non-atomic global start_th wrote. */
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

  /* Only attempt the real HTTP round-trip once the restart is confirmed to
     have actually finished and succeeded, mirroring the sibling test's own
     identical guard above. */
  int status = 0;
  ccol_retval_t get_rv = ccol_unexpected_failure;
  if (start_returned && g_qstop_race_start_rv == ccol_success)
    get_rv = _get("http://127.0.0.1:18817/hello", &status);

  /* g_qstop_race_srv is a global (written by _start_server before start_th
     ever runs, and potentially restarted by start_th itself), so it cannot
     carry a scope-exit _ccol_destructor; only torn down once start_th is
     confirmed to have actually returned, mirroring the sibling test's own
     identical "do not risk a second, compounding hang on top of a genuinely
     stuck start_th" reasoning. */
  if (start_returned) {
    chttpsvr_destroy(g_qstop_race_srv);
    chttpsvr_engine_wait();
  }

  REQUIRE_TRUE(start_th_created);
  REQUIRE_TRUE(still_blocked);
  REQUIRE_TRUE(start_returned);
  if (!start_returned)
    return; /* start_th is permanently stuck; nothing further to check safely */
  /* The real assertion: no spurious EADDRINUSE-class failure, and no hang or
     crash, when _chttpsvr_stop_internal() was driven by _quiesce_server_once()
     rather than a plain chttpsvr_stop() call; the restart succeeded cleanly
     once the old listener (and, after it, the whole shared engine) was
     genuinely torn down and rebuilt from scratch. */
  REQUIRE_EQ((int)g_qstop_race_start_rv, (int)ccol_success);
  REQUIRE_EQ((int)get_rv, (int)ccol_success);
  REQUIRE_EQ(status, 200);
}

/* ========================================================================== */
/* Regression test for a real use-after-free in _engine_force_stop_quiesce_   */
/* all(): it reads a bare struct chttpserver* directly out of servers_        */
/* bundler.servers[0] (servers_bundler_pins exists specifically to protect    */
/* that pointer's lifetime; see that field's own comment on struct            */
/* chttpserver). An earlier version of that protection reused pending_        */
/* resolve_count/_chttpsvr_resolve_unpin for it, which self-deadlocked (see   */
/* _engine_force_stop_quiesce_all's own doc comment for that story). The      */
/* actual, narrower hazard servers_bundler_pins fixes is this test's own      */
/* target: a concurrent chttpsvr_destroy(srv) call on the exact server the    */
/* reaper thread just read out of servers_bundler.servers[] and is about to   */
/* call _quiesce_server_once() on could, without any pin protecting that bare */
/* pointer, run to completion and free srv while the reaper thread was still  */
/* about to dereference it; a genuine use-after-free, not merely a            */
/* theoretical one, on the reaper thread's very next touch of srv.            */
/* This test uses a white-box hook to deterministically pause the reaper      */
/* thread at the exact point right after it has pinned srv (servers_bundler_  */
/* pins incremented, servers_bundler.mutex released) but strictly before its  */
/* own call to _quiesce_server_once(), then lands a concurrent chttpsvr_      */
/* destroy() call on that same srv from a second thread. With the fix, that   */
/* destroy() call must block (waiting for servers_bundler_pins to reach 0)    */
/* until the hook is released and the reaper thread has genuinely finished    */
/* using srv, rather than proceeding to free it out from under the still-     */
/* paused reaper thread.                                                     */
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
  /* Same discipline as stop_racing_a_concurrent_start_does_not_bind_over_
     open_listener above: every intermediate outcome is captured into a
     local, and the hook is released / every created thread is joined
     unconditionally before any REQUIRE_* runs, so a failing assertion here
     can never leave destroy_th permanently parked inside the reaper's own
     hook wait (nothing else in this binary would ever release it) or leave
     g_reaper_race_srv's engine reference/listener leaked for the rest of
     this binary's run. */
  g_reaper_race_srv = _start_server("18809", 18809);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* chttpsvr_engine_stop() is documented non-blocking: it only kicks off a
     real, separate reaper thread and returns immediately. That reaper
     thread is what will pin g_reaper_race_srv and then block inside the
     armed hook. */
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
    /* Give chttpsvr_destroy() time to actually run and reach (and, with the
       fix in place, block inside) its own servers_bundler_pins wait. Matches
       this file's own established 150ms precedent for "let the other side
       reach its own blocking point" synchronization elsewhere in this file. */
    struct timespec settle = {0, 150000000L};
    nanosleep(&settle, NULL);
    /* With the fix, chttpsvr_destroy() must still be blocked here, waiting
       for servers_bundler_pins to reach 0, not returned already, which
       (without the fix) would mean it already freed g_reaper_race_srv while
       the reaper thread was still paused holding a bare pointer to it. */
    still_blocked = !atomic_load(&g_reaper_race_destroy_returned);
  }

  /* Release the hook (harmless even if the reaper thread never actually
     reached it; chttpsvr_engine_stop()'s own real reaper thread always
     does, given g_reaper_race_srv was confirmed started above, so this can
     never hang waiting for a hook that thread would never reach) and join
     destroy_th, unconditionally, before any REQUIRE_* below runs. Releasing
     lets the reaper thread finish using g_reaper_race_srv (call _quiesce_
     server_once() on it, which, since chttpsvr_destroy() already won that
     race as long as the fix held destroy() back until now, takes the
     loser path and returns immediately) and decrement servers_bundler_pins,
     freeing chttpsvr_destroy()'s own wait to proceed. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  if (destroy_th_created) _fx_join(tau, destroy_th, NULL);

  chttpsvr_engine_wait();

  /* Prove the engine came back up cleanly afterward: a fresh server on the
     same port must be able to start and serve a real request, not merely
     that nothing crashed above. */
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
/* Regression coverage for a real bug found via code review: srv_engine_     */
/* bundler.reactor is only nulled near the very end of _engine_reaper_fn,    */
/* well after _engine_force_stop_quiesce_all/_idle_sweep_stop_if_running/    */
/* event_loop_destroy have all run (which can take real, non-negligible     */
/* wall-clock time), yet srv_engine_bundler.stopping is set true immediately */
/* (long before any of that finishes). _engine_force_stop_now (the function  */
/* the signal-safe watcher thread runs on chttpsvr_engine_stop()'s behalf)   */
/* used to check only `if (srv_engine_bundler.reactor)` before spawning a    */
/* reaper thread, with no `&& !stopping` guard the way the graceful          */
/* _engine_release() path already has (see that function's own comment for  */
/* why). A second chttpsvr_engine_stop() call landing while a first call's   */
/* own reaper was still mid-teardown (an entirely ordinary occurrence for    */
/* the documented signal-handler use case: two SIGTERMs moments apart, or a  */
/* defensive double call from application shutdown code) therefore spawned  */
/* a SECOND reaper thread capturing the identical, still-live event_loop     */
/* handle. Both reapers then independently called event_loop_destroy() on    */
/* that same handle; per cthreadcomm.h's own documented contract this is     */
/* unconditionally fatal (fatal_err()/SIGABRT) for either a sequential       */
/* (one already completed) or a temporally-overlapping double-destroy;       */
/* turning a second, supposedly-safe engine_stop() call into a process       */
/* abort instead of the documented no-op.                                   */
/* ========================================================================== */

TEST_F(engine_stop_fixture,
       second_engine_stop_call_while_first_still_tearing_down_is_safe) {
  (void)tau; /* no background thread of its own to track */
  /* Same discipline as the reaper-race test above: every intermediate step
     runs unconditionally (release the hook, wait for the engine to settle)
     before any REQUIRE_* below, so a failing assertion here can never leave
     the reaper race hook permanently armed (hanging every later test in
     this binary that force-stops the engine) or the shared engine wedged
     mid-teardown for the rest of this binary's run. */
  chttpsvr srv _ccol_destructor(___chttpsvr_destroy) =
      _start_server("18810", 18810);

  _chttpsvr_arm_reaper_race_hook_for_tests();

  /* First call: spawns a real reaper thread that pins srv (via
     _engine_force_stop_quiesce_all) and then blocks inside the now-armed
     hook, strictly before its own _quiesce_server_once(srv) call; i.e.
     with srv_engine_bundler.stopping already true and srv_engine_bundler.
     reactor still fully live, exactly the window the fix's own !stopping
     guard exists to recognize. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_reaper_race_hook_entered_for_tests();

  /* Second call, landing squarely inside that window. Before the fix, this
     spawned a second reaper thread that went on to call event_loop_destroy()
     on the identical handle the first reaper's own (still-pending) call was
     going to use; fatal. With the fix, _engine_force_stop_now's own
     !srv_engine_bundler.stopping guard makes this call a silent, documented
     no-op instead: nothing dispatched on the watcher thread, no second
     reaper spawned. */
  chttpsvr_engine_stop();

  /* Give a (would-be, pre-fix) second reaper thread a moment to actually
     reach its own event_loop_destroy() call, in case one was ever spawned,
     before releasing the first reaper; matches this file's own
     established 150ms "let the other side reach its own blocking/crash
     point" precedent used elsewhere in this binary. */
  struct timespec settle = {0, 150000000L};
  nanosleep(&settle, NULL);

  /* Release the (genuinely single) reaper thread so it can finish quiescing
     srv and tearing the reactor down normally. */
  _chttpsvr_release_reaper_race_hook_for_tests();
  chttpsvr_engine_wait();

  /* The crash this test guards against, if the fix regressed, would have
     already taken down the whole process well before this point (either
     synchronously above, or via the second reaper's own delayed
     event_loop_destroy() call racing the first one's); reaching here at all
     is a meaningful part of the assertion. Also prove the engine came back
     up genuinely clean (no corrupted global state, no still-registered
     dangling server) by starting a fresh server on the same port and
     driving one real request through it, exactly like this file's other
     force-stop tests do. */
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
/* Regression coverage for _chttpsvr_atfork_release_impl's own child-side     */
/* fixup of quiesce_state/quiesce_waiters (see that function's own doc        */
/* comment in chttpserver.c for the full account). fork() can land while a    */
/* parent-side thread is genuinely mid-way through _quiesce_server_once's     */
/* own real teardown work for some server (quiesce_state ==                   */
/* CHTTPSVR_QS_QUIESCING), and that thread is not duplicated into the child.  */
/* Without the fixup: (1) _quiesce_server_once's own "loser" branch (a later  */
/* chttpsvr_destroy() on the same handle in the child) blocks forever         */
/* waiting for a broadcast only the now-vanished thread could ever send;      */
/* (2) _engine_force_stop_quiesce_all's own driver loop would spin forever    */
/* on this exact server the next time it ran, since it would never have      */
/* been removed from servers_bundler.servers[] either. Both halves are        */
/* exercised here: (2) directly, via a white-box accessor reading             */
/* servers_bundler.count itself, rather than by actually driving a fresh      */
/* chttpsvr_engine_stop() pass in the child; chttpsvr_engine_stop() alone is  */
/* not expected to do anything there yet in any case, fixed server or not:    */
/* it only wakes an already-running watcher thread (g_engine_stop_watcher),   */
/* which this same atfork machinery correctly, deliberately marks not-ready   */
/* in a freshly forked child (see that field's own comment) until a           */
/* genuinely fresh chttpsvr_start() call re-arms it; that is an unrelated,    */
/* already-correct part of this module's fork story, not something this      */
/* test is about. Reproduced deterministically via a dedicated white-box      */
/* hook that pauses a real chttpsvr_engine_stop()-driven reaper thread at     */
/* exactly the quiesce_state == CHTTPSVR_QS_QUIESCING point, in the parent,   */
/* before forking. Lives in this binary (not tests.c) for the same reason     */
/* every other test here does: it drives the real, process-wide              */
/* chttpsvr_engine_stop(), which would otherwise yank the reactor out from    */
/* under tests.c's own long-lived g_srv.                                     */
/* ========================================================================== */
#if FORK_SAFETY_REQUIRED
extern void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void);
extern void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void);
extern void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void);
extern size_t _chttpsvr_servers_bundler_count_for_tests(void);

TEST_F(engine_stop_fixture, fork_mid_quiesce_teardown_does_not_hang_child) {
  (void)tau; /* no background thread of its own to track (the reactor's own
                internal reaper thread, spawned by chttpsvr_engine_stop()
                itself, is not one this test creates directly) */
  chttpsvr srv = _start_server("18811", 18811);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();

  /* chttpsvr_engine_stop() is documented non-blocking: it only kicks off a
     background reaper thread, which is what actually runs
     _quiesce_server_once against srv and blocks inside the now-armed hook,
     right after quiesce_state is set to CHTTPSVR_QS_QUIESCING and strictly
     before any of its own real teardown work begins. */
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv is now, in this exact process, genuinely stuck with quiesce_state
     == CHTTPSVR_QS_QUIESCING; fork() while that is still the case. */
  int pipefd[2];
  /* Not REQUIRE_EQ directly: a pipe()/fork() failure here must still release
     the now-armed hook and destroy srv before returning, or the real reaper
     thread this test paused above stays parked forever, wedging the shared,
     process-wide engine for every later test in this binary that touches it
     (chttpsvr_start/_engine_stop/_engine_wait), not merely failing this one
     test in isolation. */
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
    /* Bounds this child's own lifetime in case the hazard this test guards
       against still fires (chttpsvr_destroy() below would otherwise block
       forever). The child reports its own outcome through the pipe rather
       than through its own process exit code: under make memtest, valgrind
       overrides a forked child's real exit code with its own
       --error-exitcode the instant it finds ANY "still reachable"
       allocation in that child's inherited process image at exit time
       (which every child forked mid-suite always has, since the rest of
       this suite has not quiesced yet), so the exit code cannot reliably
       carry this result; see tests/cthreadpool's own wait_from_within_own_
       task_does_not_hang and tests/clogger's own fork_safety group for the
       original, independently-confirmed account of this exact valgrind
       behaviour. */
    alarm(3);
    char ok = 0;
    /* Half 1: srv must already be gone from servers_bundler.servers[] here,
       proving the atfork fixup ran the unregister step and not merely the
       CHTTPSVR_QS_QUIESCED flip; a regression here would otherwise only
       surface, indirectly and much later, as _engine_force_stop_quiesce_all's
       own driver loop spinning forever the next time this process genuinely
       drove a fresh reaper pass. */
    if (_chttpsvr_servers_bundler_count_for_tests() == 0) {
      /* Half 2: chttpsvr_destroy() on the exact same handle (still
         resolvable here: this path never touched chttpsvr_slot_table at
         all) must not block on _quiesce_server_once's own loser wait
         either. */
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
  /* Not REQUIRE_EQ directly: a spurious waitpid() return here (e.g. EINTR)
     must still release the hook and clean srv up before this function
     returns, for the identical reason the pipe()/fork() failure branches
     above already do; otherwise the real, paused reaper thread stays
     parked forever, wedging the shared engine for every later test. */
  pid_t waited = waitpid(pid, &status, 0);

  /* Parent side: let the real, paused reaper thread finish its own genuine
     teardown work normally, then clean srv up here too, regardless of the
     child's own outcome. */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  /* Not WEXITSTATUS (see this test's own comment above); WIFEXITED alone
     still catches a real regression re-hanging (turns into WIFSIGNALED via
     the alarm above) or a genuine crash. */
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}

/* ========================================================================== */
/* Regression coverage for _chttpsvr_atfork_release_impl's own child-side     */
/* reset of srv_engine_bundler.stopping (see that function's own doc comment */
/* in chttpserver.c for the full account, including the sibling reaper_      */
/* joinable/idle_sweep_bundler.running resets this same test cannot isolate  */
/* on their own: _engine_acquire()'s own `while (stopping) cond_var_wait     */
/* (...)` loop runs strictly before its own _join_reaper_if_needed_locked()  */
/* call, so a stuck-true stopping flag always blocks first, masking whatever */
/* reaper_joinable's own reset would otherwise independently prove; those    */
/* two remain correct by the same construction as this deterministically-    */
/* confirmed one, and as this module's own pre-existing g_engine_stop_       */
/* watcher.started precedent, without a narrower, dedicated test of their    */
/* own). A real, parent-side reaper thread paused mid-teardown at the        */
/* instant of fork() (this test's own setup, identical to the previous       */
/* test's) leaves srv_engine_bundler.stopping true, and that thread is not   */
/* duplicated into the child. The previous test above never actually reaches */
/* code that reads it at all: chttpsvr_destroy() there takes _quiesce_       */
/* server_once's own loser branch (never touches the shared engine's own     */
/* stopping bookkeeping), and restarting THAT SAME srv would also skip       */
/* _engine_acquire() entirely, since chttpsvr_start()'s own `need_acquire =  */
/* !raw->contributed_to_engine` sees a stale, still-true value inherited     */
/* from before the whole stop sequence ever began (a second, independently   */
/* found reason the naive version of this test needed a rethink). A          */
/* genuinely fresh handle (never started, contributed_to_engine == false     */
/* from construction) is what actually forces chttpsvr_start() to call       */
/* _engine_acquire() for real, exercising its own stopping-wait loop;        */
/* without the fix this hangs forever, confirmed directly by temporarily     */
/* reverting just this one reset and observing this exact test fail. srv     */
/* itself is still needed only to get the shared engine into the right       */
/* "stuck mid-teardown" state to fork() out of; the fresh handle's own       */
/* start/stop is otherwise completely independent of it.                    */
/* ========================================================================== */
TEST_F(engine_stop_fixture,
       fork_mid_quiesce_teardown_does_not_hang_fresh_start_in_child) {
  (void)tau; /* no background thread of its own to track (the reactor's own
                internal reaper thread, spawned by chttpsvr_engine_stop()
                itself, is not one this test creates directly) */
  chttpsvr srv = _start_server("18812", 18812);

  _chttpsvr_arm_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_stop();
  _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests();

  /* srv_engine_bundler.stopping is now true and a real reaper thread is
     alive, paused inside the hook (reaper_joinable == true); fork() while
     both are still the case. */
  int pipefd[2];
  /* Not REQUIRE_EQ directly: see the previous test's own identical comment
     for why a pipe()/fork() failure here must still release the hook and
     destroy srv before returning, rather than leaving the real reaper thread
     parked forever and wedging the shared engine for every later test. */
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
    /* See the previous test's own identical comment for why this reports
       through the pipe rather than through the process exit code. */
    alarm(3);
    /* Deliberately a brand-new handle, not srv itself; see this test's own
       doc comment above for why restarting srv would not actually exercise
       _engine_acquire() at all. */
    chttpsvr fresh = create_chttpsvr(CLOG_INVALID, NULL);
    char ok = 0;
    if (fresh != CHTTPSVR_INVALID) {
      chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
      cfg.host = "127.0.0.1";
      cfg.port = 18813;
      chttpsvr_start(fresh, &cfg); /* not asserted on: whether this fresh
                                       server's own bind() succeeds is
                                       incidental; only whether the call
                                       returns at all is under test here */
      ok = 1; /* reached only if the call above actually returned */
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
  /* Not REQUIRE_EQ directly: see the previous test's own identical comment
     for why a spurious waitpid() return here must still release the hook
     and clean srv up before this function returns. */
  pid_t waited = waitpid(pid, &status, 0);

  /* Parent side: let the real, paused reaper thread finish its own genuine
     teardown work normally, then clean srv up here too, regardless of the
     child's own outcome. */
  _chttpsvr_release_quiesce_teardown_race_hook_for_tests();
  chttpsvr_engine_wait();
  chttpsvr_destroy(srv);

  REQUIRE_EQ(waited, pid);
  REQUIRE_TRUE(WIFEXITED(status));
  REQUIRE_EQ(n, (ssize_t)1);
  REQUIRE_EQ(ok, 1);
}
#endif /* FORK_SAFETY_REQUIRED */
