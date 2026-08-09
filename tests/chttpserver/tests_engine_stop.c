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
#include <signal.h>
#include <stdatomic.h>
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
 * NULL g_servers[0]; a guaranteed, deterministic SIGSEGV, not a rare race.
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
 * already does; the forced path is not supposed to abandon or corrupt a
 * request that is already being handled by a worker thread at the moment the
 * signal fires. */
TEST(engine_stop, force_stop_drains_in_flight_request_before_reactor_teardown) {
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
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

static void *_release_slow_handler_after_delay(void *arg) {
  (void)arg;
  /* 150ms: give the reaper thread chttpsvr_engine_stop() spawns time to
   * actually win the race to quiesce srv and start blocking inside
   * _drain_and_close_all_connections (waiting on the still-in-flight
   * /slow-race request below) before the handler -- and therefore that
   * drain -- is allowed to complete. */
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
 * thread, with no chttpsvr_engine_wait() serializing the two -- exactly the
 * pattern chttpsvr_engine_stop()'s own doc comment invites by describing
 * itself as safe to call from a signal handler with no requirement that a
 * caller synchronize it against a concurrent chttpsvr_destroy() first.
 *
 * Before the fix, whichever of the two callers lost the race to quiesce srv
 * returned from _quiesce_server_once() immediately, as a bare no-op, the
 * instant it observed the other caller had already claimed
 * srv->teardown_started -- without waiting for that other caller's real
 * teardown work to actually finish. If the loser was this thread's own
 * chttpsvr_destroy(srv) call, it would proceed straight into
 * mutex_destroy(raw->mutex)/mutex_destroy(raw->idle_mutex)/free(raw) while
 * the reaper thread was still concurrently using those exact objects inside
 * _drain_and_close_all_connections (which this test forces to block for a
 * genuine, measurable window via the still-in-flight /slow-race request) --
 * a real use-after-free / destroyed-in-use-mutex race. A clean run of this
 * test (especially under valgrind's memtest target or -fsanitize=thread) is
 * the direct proof that race is closed. */
TEST(engine_stop, concurrent_destroy_and_engine_stop_is_safe) {
  char *err = NULL;
  chttpsvr srv = create_chttpsvr(NULL, &err);
  REQUIRE_TRUE(srv != CHTTPSVR_INVALID);
  chttpsvr_register_handler(srv, CHTTP_GET, "/slow-race", _slow_handler, NULL);
  chttpsvr_config_t cfg = CHTTPSVR_CONFIG_DEFAULT;
  cfg.host = "127.0.0.1";
  cfg.port = 18800;
  REQUIRE_EQ((int)chttpsvr_start(srv, &cfg), (int)ccol_success);

  g_slow_go = false;
  g_slow_entered = false;
  _async_get_ctx ctx = {.url = "http://127.0.0.1:18800/slow-race",
                        .status = 0,
                        .rv = ccol_success};
  pthread_t req_th;
  REQUIRE_EQ(pthread_create(&req_th, NULL, _async_get_thread, &ctx), 0);

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

  pthread_t releaser_th;
  REQUIRE_EQ(pthread_create(&releaser_th, NULL,
                            _release_slow_handler_after_delay, NULL),
             0);

  /* chttpsvr_engine_stop() is documented non-blocking: it only kicks off a
   * background reaper thread. Sleeping briefly before calling
   * chttpsvr_destroy() gives that reaper thread (a real thread_create())
   * time to win the race to quiesce srv, so this thread's own
   * chttpsvr_destroy() call below is the one exercising the losing side of
   * the race this test targets. */
  chttpsvr_engine_stop();
  struct timespec settle = {0, 50000000L};
  nanosleep(&settle, NULL);

  chttpsvr_destroy(srv);

  pthread_join(releaser_th, NULL);
  pthread_join(req_th, NULL);
  REQUIRE_EQ((int)ctx.rv, (int)ccol_success);
  REQUIRE_EQ(ctx.status, 200);

  chttpsvr_engine_wait();

  /* Prove the engine came back up cleanly afterward. */
  chttpsvr srv2 = _start_server("18801", 18801);
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
/* (e.g. about to call event_loop_add for the listener) -- and, since this   */
/* server's own started/listen_reg/contributed_to_engine bookkeeping was     */
/* never touched by that quiesce pass, a later chttpsvr_stop()/_destroy()    */
/* call on it would hand a dangling event_reg* into event_loop_remove(), a   */
/* genuine use-after-free (event_loop itself is a safe, generation-checked   */
/* value handle, but event_reg* is a bare pointer with no such protection).  */
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

static void *_start_racing_server_thread(void *arg) {
  (void)arg;
  char *err = NULL;
  g_race_srv = create_chttpsvr(NULL, &err);
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
  return NULL;
}

TEST(engine_stop, force_stop_racing_a_concurrent_start_is_safe) {
  g_race_srv = CHTTPSVR_INVALID;
  g_race_start_rv = ccol_success;

  _chttpsvr_arm_start_race_hook_for_tests();

  pthread_t start_th;
  REQUIRE_EQ(pthread_create(&start_th, NULL, _start_racing_server_thread, NULL),
             0);

  /* Block until chttpsvr_start() has confirmed its engine reference and
   * registered with servers_bundler, and is now paused right there -- the
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

  pthread_join(start_th, NULL);
  REQUIRE_EQ((int)g_race_start_rv, (int)ccol_success);

  chttpsvr_engine_wait();

  /* The real assertion: neither of these crashes or hangs. Before the fix,
   * g_race_srv's listen_reg could point into an already-destroyed event_loop
   * by this point, making chttpsvr_destroy() a genuine use-after-free. */
  chttpsvr_destroy(g_race_srv);

  /* Prove the engine came back up cleanly afterward. */
  chttpsvr srv3 = _start_server("18803", 18803);
  int status2 = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18803/hello", &status2),
             (int)ccol_success);
  REQUIRE_EQ(status2, 200);
  chttpsvr_destroy(srv3);
  chttpsvr_engine_wait();
}

/* ========================================================================== */
/*   chttpsvr_engine_stop() ASYNC-SIGNAL-SAFETY (real signal handler)         */
/*                                                                            */
/* chttpsvr_engine_stop() is documented (chttpserver.h) as "async-signal-    */
/* safe: safe to call from a signal handler" -- the whole point being that   */
/* an application installs it (or a thin wrapper) as a SIGTERM/SIGINT        */
/* handler for graceful shutdown. Its real work used to run directly on      */
/* whichever thread called it: mutex_lock(srv_engine_bundler.mutex), and,    */
/* when a reactor was live, thread_create() via _spawn_reaper(). Neither is  */
/* POSIX-guaranteed async-signal-safe, and this was a genuine, reachable     */
/* self-deadlock, not a theoretical one: srv_engine_bundler.mutex is also    */
/* taken by _engine_acquire() (chttpsvr_start()), _engine_release()          */
/* (chttpsvr_destroy()), and the chttpsvr_set_engine_*() setters, so a       */
/* signal arriving on the thread currently inside any one of those calls --  */
/* e.g. SIGTERM delivered mid-chttpsvr_start(), a realistic race during a    */
/* container's shutdown/rolling-restart sequence -- would have the          */
/* handler's own mutex_lock() self-deadlock against the lock that same      */
/* thread already held, hanging the whole process (surviving only           */
/* SIGKILL).                                                                 */
/*                                                                           */
/* Fixed by moving every mutex/thread_create-touching step onto a           */
/* dedicated, always-running watcher thread (g_engine_stop_watcher in       */
/* chttpserver.c), woken via sem_post() -- the one synchronization          */
/* primitive POSIX explicitly lists as async-signal-safe -- so              */
/* chttpsvr_engine_stop() itself never calls mutex_lock/thread_create,      */
/* directly or indirectly, regardless of what the interrupted thread was    */
/* doing.                                                                   */
/*                                                                           */
/* This test reproduces the exact hazard deterministically, without relying */
/* on timing: a white-box hook locks srv_engine_bundler.mutex itself, then  */
/* raise()s SIGUSR1 while still holding it -- raise() in a multi-threaded   */
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

TEST(engine_stop, engine_stop_from_signal_handler_does_not_deadlock) {
  /* A started server guarantees the shared engine -- and therefore the
   * signal-safe stop watcher, started as the very first thing
   * _engine_acquire() does under srv_engine_bundler.mutex -- already
   * exists and is ready before this test provokes the race. */
  chttpsvr srv = _start_server("18804", 18804);

  struct sigaction sa, old_sa;
  memset(&sa, 0, sizeof(sa));
  sa.sa_handler = _sigusr1_calls_engine_stop;
  sigemptyset(&sa.sa_mask);
  sa.sa_flags = 0;
  REQUIRE_EQ(sigaction(SIGUSR1, &sa, &old_sa), 0);

  atomic_store(&g_signal_test_done, false);
  pthread_t th;
  REQUIRE_EQ(pthread_create(&th, NULL, _signal_test_thread, NULL), 0);

  /* Bounded wait, not a join: if the fix ever regresses, the thread above
   * self-deadlocks forever inside chttpsvr_engine_stop() (called
   * synchronously from SIGUSR1's own handler while still holding
   * srv_engine_bundler.mutex). A REQUIRE_TRUE(done) failure below is the
   * regression signal; the process can still exit normally afterward even
   * with that one thread permanently stuck, since process exit does not
   * wait for other threads. */
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
  pthread_join(th, NULL);

  /* chttpsvr_engine_stop() genuinely ran (not just returned without
   * deadlocking): confirm the engine actually stops. */
  chttpsvr_engine_wait();

  chttpsvr_destroy(srv);

  /* Prove the engine (and its watcher thread, which survives a stop/start
   * cycle -- it is never torn down except at process exit) comes back up
   * cleanly afterward. */
  chttpsvr srv2 = _start_server("18805", 18805);
  int status = 0;
  REQUIRE_EQ((int)_get("http://127.0.0.1:18805/hello", &status),
             (int)ccol_success);
  REQUIRE_EQ(status, 200);
  chttpsvr_destroy(srv2);
  chttpsvr_engine_wait();
}
