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

#include <cfio_engine.h>
#include <fio.h>
#include <pthread.h>
#include <stdlib.h>
#include <unistd.h>

/*
 * This is a direct, careful merge of what used to be two independent copies
 * of the exact same lazy-start/ref-counted-stop lifecycle pattern --
 * chttpserver.c's g_engine_* globals and chttpclient.c's g_client_engine_*
 * globals -- generalized to know nothing about either module's own
 * bookkeeping (chttpserver's per-instance g_server_count; chttpclient's DNS/
 * connect pool and deadline sweep, both of which stay in chttpclient.c and
 * are acquired/released around calls into this module, not folded into it).
 *
 * http_lib_constructor (declared in the vendored third_party/facio/http.h,
 * defined in http_internal.c) is called unconditionally as part of the
 * one-time global init below, regardless of which module's acquire call
 * triggers it first. It only registers facio's HTTP-layer
 * FIO_CALL_ON_INITIALIZE/FIO_CALL_AT_EXIT callbacks (pre-interning and
 * freeing a handful of FIOBJ header-name constants); it does nothing
 * expensive and is harmless even for a process that never calls
 * http_listen(). Skipping it when chttpclient happens to be the first
 * caller would leave chttpserver's later http_listen() calls relying on
 * HTTP-layer state that was never initialized -- so it is not conditional
 * on which module asks first.
 */
extern void http_lib_constructor(void);
extern void fio_lib_init(void);
extern void fio_lib_destroy(void);

static pthread_mutex_t g_cfio_engine_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cfio_engine_ready_cv = PTHREAD_COND_INITIALIZER;
/* Signalled once a reaper thread (see _cfio_engine_reaper_fn) finishes
 * tearing the shared reactor down (g_cfio_engine_stopping -> false). */
static pthread_cond_t g_cfio_engine_stopped_cv = PTHREAD_COND_INITIALIZER;
static bool g_cfio_engine_running = false;
/* True from the moment the last reference is released until the reaper
 * thread has fully finished tearing the reactor down (joined its thread).
 * While true, a concurrent acquire must wait rather than starting a fresh
 * reactor or touching the dying one's thread handle -- see
 * _cfio_engine_acquire's wait loop. */
static bool g_cfio_engine_stopping = false;
static bool g_cfio_engine_ready = false;
static bool g_cfio_engine_thread_started = false;
static pthread_t g_cfio_engine_thread;
static int16_t g_cfio_engine_threads = 1;
/* Number of outstanding acquire() callers (from either module) currently
 * relying on the reactor. Stops when this reaches 0. */
static int g_cfio_engine_ref_count = 0;
/* Handle of the most recently spawned reaper thread, and whether it still
 * needs joining. The reaper is intentionally NOT detached -- see
 * _cfio_engine_join_reaper_if_needed_locked's comment for why an explicit
 * join, not just its stopped_cv broadcast, is required for a caller to be
 * able to rely on the reactor thread's OS-level teardown having fully
 * completed. */
static pthread_t g_cfio_engine_reaper_thread;
static bool g_cfio_engine_reaper_joinable = false;

static void _cfio_engine_atexit_safety_net(void);

static pthread_once_t _cfio_fio_init_once = PTHREAD_ONCE_INIT;
static void _cfio_fio_global_init(void) {
  http_lib_constructor();
  fio_lib_init();
  atexit(fio_lib_destroy);
  atexit(_cfio_engine_atexit_safety_net);
}

/* Called by facil.io on FIO_CALL_ON_START, once the reactor has entered its
 * event loop. Signals _cfio_engine_acquire that the reactor is ready. */
static void _cfio_engine_ready_cb(void *arg) {
  (void)arg;
  pthread_mutex_lock(&g_cfio_engine_mutex);
  g_cfio_engine_ready = true;
  pthread_cond_broadcast(&g_cfio_engine_ready_cv);
  pthread_mutex_unlock(&g_cfio_engine_mutex);
}

static void *_cfio_fio_thread_fn(void *arg) {
  (void)arg;
  fio_start(.threads = g_cfio_engine_threads, .workers = 1);
  /* g_cfio_engine_running is cleared by _cfio_engine_release/reaper, not
   * here -- this thread must not touch g_cfio_engine_mutex, since the
   * reaper holds it across the pthread_join that waits for this function to
   * return. */
  return NULL;
}

/*
 * Runs on a freshly spawned thread (never on one of the reactor's own
 * worker threads -- see _cfio_engine_release's comment for why that
 * distinction is essential) to perform the actual blocking teardown: stop
 * the reactor and join its thread, then clear g_cfio_engine_stopping so a
 * waiting acquirer can proceed.
 *
 * This thread is itself joinable (see _cfio_engine_join_reaper_if_needed_
 * locked), not detached -- but it must never join *itself*, and nothing
 * above touches g_cfio_engine_mutex after the broadcast below, so a
 * concurrent joiner can never deadlock against this function.
 */
static void *_cfio_engine_reaper_fn(void *arg) {
  (void)arg;
  fio_stop();
  pthread_join(g_cfio_engine_thread, NULL);
  pthread_mutex_lock(&g_cfio_engine_mutex);
  g_cfio_engine_thread_started = false;
  g_cfio_engine_stopping = false;
  pthread_cond_broadcast(&g_cfio_engine_stopped_cv);
  pthread_mutex_unlock(&g_cfio_engine_mutex);
  /* fio_lib_destroy (registered via atexit in _cfio_fio_global_init) fires
   * after all other cleanup, at process exit. */
  return NULL;
}

/*
 * Joins the previous reaper thread if one is still outstanding. Must be
 * called with g_cfio_engine_mutex already held, and is safe to call (and
 * safe to block) while holding it: by the time g_cfio_engine_reaper_joinable
 * is observed true here, the reaper has already broadcast stopped_cv and is
 * on its way to returning -- it never touches g_cfio_engine_mutex again
 * after that broadcast, so this pthread_join cannot deadlock against it, and
 * it returns in practice almost immediately (the reaper's real work is
 * already done; only its own OS-level thread teardown remains).
 *
 * Holding the mutex across the join is what makes this safe under
 * concurrent callers too: pthread_join-ing the same target pthread_t from
 * more than one thread simultaneously is undefined behavior per POSIX, so
 * only the first caller to observe g_cfio_engine_reaper_joinable may
 * actually join -- every other concurrent caller simply blocks on the mutex
 * itself until that join (and the flag clear) is done, rather than either
 * racing to join the same handle or wrongly skipping the wait.
 *
 * Without this, a caller relying on _cfio_engine_wait_for_quiescence /
 * _cfio_engine_wait_until_stopped to mean "the reactor and everything it
 * touched are now fully gone at the OS level" could observe stopped_cv's
 * broadcast and proceed (e.g. straight into process exit, as a test's
 * atexit teardown does) microseconds before the now-detached-in-spirit
 * reaper thread had actually finished its own glibc-internal thread-exit
 * bookkeeping -- observed in practice as an intermittent valgrind
 * "possibly lost" report for that thread's TLS/stack allocation
 * (glibc's allocate_dtv), since valgrind's final leak check can run in that
 * narrow window. An explicit join is the only way to make "fully torn down"
 * an OS-verified fact rather than a best-effort race.
 */
static void _cfio_engine_join_reaper_if_needed_locked(void) {
  if (g_cfio_engine_reaper_joinable) {
    pthread_join(g_cfio_engine_reaper_thread, NULL);
    g_cfio_engine_reaper_joinable = false;
  }
}

/* Spawns the reaper thread (see _cfio_engine_reaper_fn's own comment).
 * Shared by _cfio_engine_release, _cfio_engine_force_stop, and
 * _cfio_engine_atexit_safety_net -- all three reach the same "something must
 * stop the reactor now" decision, just via different triggers. */
static void _cfio_engine_spawn_reaper(void) {
  pthread_t reaper;
  if (pthread_create(&reaper, NULL, _cfio_engine_reaper_fn, NULL) != 0) {
    /* Could not spawn the reaper (OOM-class failure). No safer fallback
     * exists than doing it inline; this reintroduces the self-join deadlock
     * risk documented on _cfio_engine_release only in this already-
     * degenerate case. Nothing to join afterward since it already ran to
     * completion synchronously. */
    _cfio_engine_reaper_fn(NULL);
    return;
  }
  pthread_mutex_lock(&g_cfio_engine_mutex);
  g_cfio_engine_reaper_thread = reaper;
  g_cfio_engine_reaper_joinable = true;
  pthread_mutex_unlock(&g_cfio_engine_mutex);
}

ccol_retval_t _cfio_engine_acquire(void) {
  pthread_mutex_lock(&g_cfio_engine_mutex);

  /* A previous engine instance may still be mid-teardown on the reaper
   * thread (see _cfio_engine_release). Reusing g_cfio_engine_thread while
   * that's in flight would be a use-after-free, so wait for it to fully
   * finish before deciding whether to start fresh. */
  while (g_cfio_engine_stopping) {
    pthread_cond_wait(&g_cfio_engine_stopped_cv, &g_cfio_engine_mutex);
  }
  _cfio_engine_join_reaper_if_needed_locked();

  if (!g_cfio_engine_running) {
    pthread_once(&_cfio_fio_init_once, _cfio_fio_global_init);

    long raw = sysconf(_SC_NPROCESSORS_ONLN);
    if (raw < 1) raw = 1;
    g_cfio_engine_threads = (raw > INT16_MAX) ? INT16_MAX : (int16_t)raw;

    g_cfio_engine_ready = false;
    fio_state_callback_add(FIO_CALL_ON_START, _cfio_engine_ready_cb, NULL);

    int rc =
        pthread_create(&g_cfio_engine_thread, NULL, _cfio_fio_thread_fn, NULL);
    if (rc != 0) {
      fio_state_callback_remove(FIO_CALL_ON_START, _cfio_engine_ready_cb, NULL);
      pthread_mutex_unlock(&g_cfio_engine_mutex);
      return ccol_unexpected_failure;
    }

    g_cfio_engine_thread_started = true;
    g_cfio_engine_running = true;

    /* Block until the reactor fires FIO_CALL_ON_START: it is now in its
     * event loop. */
    while (!g_cfio_engine_ready) {
      pthread_cond_wait(&g_cfio_engine_ready_cv, &g_cfio_engine_mutex);
    }
  }

  g_cfio_engine_ref_count++;
  pthread_mutex_unlock(&g_cfio_engine_mutex);
  return ccol_success;
}

/*
 * Releasing is essential to hand off to a reaper thread rather than stop
 * inline: this can be called from inside a facio protocol callback
 * (chttpclient's _async_on_close, in particular), i.e. from a thread facio
 * itself owns as part of the very reactor being stopped. Tearing the
 * reactor down inline would mean fio_stop() followed by
 * pthread_join(g_cfio_engine_thread, ...) -- but g_cfio_engine_thread is
 * blocked inside fio_start(), which (for g_cfio_engine_threads > 1, the
 * common case) itself blocks in fio_defer_thread_pool_join() waiting for
 * every one of its own worker threads -- including whichever one is
 * currently running this exact on_close callback -- to finish. A worker
 * thread can never "finish" while it is still inside this call, so joining
 * the reactor thread from here would deadlock permanently, and since this
 * is called under g_cfio_engine_mutex, that deadlock would also wedge every
 * subsequent acquire/release forever. The reaper thread is a genuinely
 * separate thread with no membership in facio's own worker pool, so it can
 * join it without any such cycle.
 *
 * g_cfio_engine_running is cleared here (synchronously, before spawning the
 * reaper) so no concurrent acquire can be satisfied by the now-dying
 * reactor; g_cfio_engine_stopping is set alongside it so a concurrent
 * acquire waits for the reaper to fully finish (join done) rather than
 * possibly touching it mid-teardown.
 */
void _cfio_engine_release(void) {
  bool should_reap = false;
  pthread_mutex_lock(&g_cfio_engine_mutex);
  if (g_cfio_engine_ref_count > 0) g_cfio_engine_ref_count--;
  if (g_cfio_engine_ref_count == 0 && g_cfio_engine_running) {
    g_cfio_engine_running = false;
    g_cfio_engine_stopping = true;
    should_reap = true;
  }
  pthread_mutex_unlock(&g_cfio_engine_mutex);

  if (should_reap) _cfio_engine_spawn_reaper();
}

void _cfio_engine_wait_for_quiescence(void) {
  pthread_mutex_lock(&g_cfio_engine_mutex);
  while (g_cfio_engine_stopping) {
    pthread_cond_wait(&g_cfio_engine_stopped_cv, &g_cfio_engine_mutex);
  }
  _cfio_engine_join_reaper_if_needed_locked();
  pthread_mutex_unlock(&g_cfio_engine_mutex);
}

/*
 * Unlike _cfio_engine_wait_for_quiescence, this also blocks while the
 * reactor is running normally (references held, no stop triggered yet) --
 * it waits for the *eventual* transition to fully-stopped, not just for an
 * already-in-flight one to finish. Safe to use the same stopped_cv: running
 * is always already false by the time stopping flips back to false and
 * stopped_cv is broadcast (see _cfio_engine_release/_cfio_engine_reaper_fn),
 * and running can never flip back to true without first observing
 * stopping == false in _cfio_engine_acquire -- so this loop's condition is
 * guaranteed to become false at the exact same broadcast
 * _cfio_engine_wait_for_quiescence already relies on.
 */
void _cfio_engine_wait_until_stopped(void) {
  pthread_mutex_lock(&g_cfio_engine_mutex);
  while (g_cfio_engine_running || g_cfio_engine_stopping) {
    pthread_cond_wait(&g_cfio_engine_stopped_cv, &g_cfio_engine_mutex);
  }
  _cfio_engine_join_reaper_if_needed_locked();
  pthread_mutex_unlock(&g_cfio_engine_mutex);
}

bool _cfio_engine_running(void) {
  pthread_mutex_lock(&g_cfio_engine_mutex);
  bool running = g_cfio_engine_running;
  pthread_mutex_unlock(&g_cfio_engine_mutex);
  return running;
}

void _cfio_engine_force_stop(void) {
  bool should_reap = false;
  pthread_mutex_lock(&g_cfio_engine_mutex);
  if (g_cfio_engine_running) {
    g_cfio_engine_ref_count = 0;
    g_cfio_engine_running = false;
    g_cfio_engine_stopping = true;
    should_reap = true;
  }
  pthread_mutex_unlock(&g_cfio_engine_mutex);

  if (should_reap) _cfio_engine_spawn_reaper();
}

/*
 * Process-exit safety net: registered via atexit() immediately after
 * atexit(fio_lib_destroy) inside _cfio_fio_global_init, so -- atexit runs
 * handlers in reverse registration order -- this always runs *before*
 * fio_lib_destroy.
 *
 * _cfio_engine_wait_for_quiescence alone only protects a caller who *knows*
 * to call it once no more work is outstanding. Nothing forces that
 * discipline: an application (or a module built on top of this one, such as
 * chttpclient's own atexit safety net for its DNS pool/deadline sweep) can
 * exit while references are still genuinely held -- ref count still
 * nonzero, no release ever triggered a reaper. Without this function,
 * fio_lib_destroy would then unmap fio_data's backing memory while a facio
 * worker thread from this reactor is potentially still running, a
 * use-after-free/segfault.
 *
 * If the reactor is still running at process-exit time, forces the ref
 * count to 0 (there is no meaningful outstanding caller left to wait for)
 * and triggers the same stop-and-reap path _cfio_engine_release uses, then
 * waits for it -- covering both "we just triggered a stop" and "a
 * concurrent ordinary release already triggered one that hadn't finished
 * yet".
 */
static void _cfio_engine_atexit_safety_net(void) {
  _cfio_engine_force_stop();
  _cfio_engine_wait_for_quiescence();
}
