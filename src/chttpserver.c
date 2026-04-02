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

#include <chttp1_parser.h>
#include <chttpserver.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <ctls.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

typedef struct chttpsvr_mw_node chttpsvr_mw_node_t;

/** Linked list node for middleware entries.
 *
 * next is a plain pointer.  All reads traverse the list while holding the
 * routes_lock read-lock; all writes append under the write-lock.  The lock
 * provides the ordering guarantee, so no atomic operations are needed. */
struct chttpsvr_mw_node {
  chttpsvr_middleware_fn fn;
  void *ctx;
  chttpsvr_mw_node_t *next;
};

/** Compiled route entry. */
typedef struct chttpsvr_route {
  chttp_method_t method;
  char **segs; /* split pattern segments */
  int seg_count;
  char **param_names; /* extracted from {name} segments */
  int param_count;
  chttpsvr_handler_fn fn;
  void *ctx;
  bool is_streaming;
} chttpsvr_route_t;

/** Sub-router (also used as the root router). */
struct chttpsvr_router {
  char *prefix; /* e.g. "/api/v1"; "" for root */
  size_t prefix_len;
  chttpsvr_mw_node_t *mw_head;
  chttpsvr_mw_node_t *mw_tail;
  int mw_count; /* number of registered middleware; capped at _CHTTPSVR_MAX_MW
                 */
  chttpsvr_route_t **routes; /* pointer array; each entry is a stable alloc */
  size_t route_count;
  size_t route_cap;
  struct chttpserver *srv; /* back-pointer */
  ccol_memmgmt_procs_t *m_procs;
};

/** One response header entry stored in the flat headers array. */
typedef struct {
  char *name;
  char *value;
} resp_header_t;

/** Per-request response accumulator.
 *
 * Response headers are kept in a flat dynamic array (not a linked list) so
 * that the duplicate-detection scan in chttpsvr_resp_set_header is
 * cache-friendly and no per-header node allocation is required. */
struct chttpsvr_resp {
  int status_code;
  resp_header_t *headers; /* flat array; grows 2x+8 on overflow */
  size_t header_count;
  size_t header_cap;
  char *body;
  size_t body_len;
  size_t body_cap;
  ccol_memmgmt_procs_t *m_procs;
};

/** Lazy-parsed query parameters. */
typedef struct chttpsvr_qparams {
  char **keys;   /* URL-decoded, owned */
  char **values; /* URL-decoded, owned */
  size_t count;
  size_t cap;
  ccol_memmgmt_procs_t *m_procs;
} chttpsvr_qparams_t;

/* Maximum number of middleware steps (global + router combined) per request.
 * Exceeding this limit causes the server to respond with 500. */
#define _CHTTPSVR_MAX_MW 32

/* Snapshot entry for one middleware step. */
typedef struct {
  chttpsvr_middleware_fn fn;
  void *ctx;
} _mw_entry_t;

/** Middleware dispatch state (stack-allocated per request). */
typedef struct dispatch_ctx {
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  _mw_entry_t
      mw_snap[_CHTTPSVR_MAX_MW]; /* fn/ctx pairs, snapshotted under lock */
  int mw_count;                  /* total snapshotted entries */
  int mw_idx;                    /* next entry to dispatch */
} dispatch_ctx_t;

/* A small growable buffer used both for a buffered route's whole-body
 * accumulation and for a streaming route's "pending, not yet delivered to
 * chttpsvr_req_read" body bytes (see _on_body's own comment). */
typedef struct {
  char *buf;
  size_t cap;
  size_t len; /* valid bytes currently held */
  size_t pos; /* streaming only: how much of [0,len) chttpsvr_req_read has
               * already delivered to the caller */
} growbuf_t;

typedef enum {
  CONN_ST_TLS_HANDSHAKE,
  CONN_ST_READING_HEADERS,
  CONN_ST_DIVERTED, /* worker thread owns fd; no reactor registration */
  CONN_ST_CLOSING
} conn_state_t;

/** Per-connection object. Persists across every keep-alive request on this
 * connection; per-request scratch fields (method/path/headers/route/
 * dispatch/resp/body buffers) are reset by _conn_reset_for_request() before
 * each new request begins. Heap-allocated; owned by whichever of (the
 * reactor's event_reg, a worker's ctpool task) currently holds it -- never
 * both at once (see the header-read callback and _task_worker for the
 * handoff points). */
typedef struct chttpsvr_conn {
  int fd;
  struct chttpserver *srv;
  event_reg *reg; /* current registration; NULL while diverted */
  ctls_conn_t *tls;
  conn_state_t state;
  chttp1_parser_t parser;

  /* Per-request scratch. */
  chttp_method_t method;
  /* owned, RAW (still percent-encoded): route matching must operate on this
   * exact form -- _match_segments/_seg_matches_literal split on a literal
   * '/' and decode each segment individually, which is the only way to tell
   * an actual path separator from a %2F encoded one inside a {param}
   * segment. Decoding the whole path eagerly here (as an earlier version of
   * this function did) would turn %2F into a real '/' before segmentation,
   * silently splitting one param segment into two path segments, and would
   * also have to rejects the entire request on any malformed %XX anywhere
   * in the path -- this module's own tests document that a malformed
   * encoding in one segment must fall out as an ordinary route mismatch
   * (404), not a 400, since _decode_seg_alloc/_seg_matches_literal already
   * treat a decode failure as "this segment doesn't match" further down. */
  char *path;
  /* owned, fully URL-decoded; populated by _on_headers_complete() once a
   * route has actually matched (decoding is then guaranteed to succeed,
   * since a match already proved every segment decodes cleanly) -- this is
   * what chttpsvr_req_path() returns. */
  char *decoded_path;
  char *raw_query; /* owned, or NULL */
  char **hdr_names, **hdr_values;
  size_t hdr_count, hdr_cap;
  chttpsvr_router *matched_router;
  chttpsvr_route_t *matched_route;
  char **matched_param_values;
  dispatch_ctx_t dispatch;
  chttpsvr_resp resp;
  bool req_rejected;
  int reject_status;
  bool expects_continue;
  growbuf_t body; /* buffered-route whole body, or streaming pending bytes */
  size_t body_bytes_seen; /* cumulative, for max_body_size enforcement */
  bool body_too_large;

  /* Leftover bytes from the reactor's own header-parsing read buffer, past
   * chttp1_parser_consumed(), copied out (see _conn_start_diverted) since
   * the original buffer is the reactor thread's own stack memory and does
   * not survive past the callback that produced it. Freed by _task_worker
   * once chttp1_stream_prepare()/_tls() has copied it into its own
   * heap buffer. */
  char *_carry_over;
  size_t _carry_over_len;

  /* max_body_read_duration_ms bookkeeping. */
  struct timespec read_deadline;
  bool read_deadline_set;
  bool deadline_exceeded;

  /* Idle-timeout sweep bookkeeping; list fields guarded by srv->idle_mutex. */
  struct timespec last_activity;
  struct chttpsvr_conn *idle_prev, *idle_next;
  bool in_idle_list;

  ccol_memmgmt_procs_t *m_procs;
} chttpsvr_conn_t;

/** Per-request object handed to handlers/middleware. Thin view over a
 * chttpsvr_conn_t plus the worker's own chttp1_stream_t (the latter cannot
 * live on chttpsvr_conn_t itself: it is only valid, and only exists, for the
 * duration of one worker's turn driving one request's body). */
struct chttpsvr_req {
  chttpsvr_conn_t *conn;
  chttp1_stream_t *stream;  /* worker-local; NULL until the worker sets it up */
  const char **param_names; /* borrowed from route (const aliases) */
  chttpsvr_qparams_t *_qparams;
  bool _qparams_attempted;
  bool _qparams_parse_oom;
  const char **_qresult;
  size_t _qresult_cap;
  bool _qresult_oom;
  ccol_memmgmt_procs_t *m_procs;
};

/** Main server struct. */
struct chttpserver {
  chttpsvr_router **routers; /* [0] = root, [1..n] = sub-routers */
  size_t router_count;
  size_t router_cap;
  ctpool worker_pool;
  ctls_ctx_t *tls_ctx; /* non-NULL when TLS configured */
  mutex_t mutex;
  cond_var_t requests_done_cv;
  int in_flight_requests;
  rw_lock_t routes_lock;
  clog cl;

  int listen_fd; /* -1 when not started */
  bool is_unix_socket;
  char *unix_socket_path; /* owned; NULL for a TCP listener */
  event_reg *listen_reg;
  bool started;
  bool contributed_to_engine;

  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned idle_timeout_ms; /* keep-alive idle timeout; 0 = disabled */
  size_t max_body_size;
  size_t max_header_bytes;        /* 0 = chttp1_parser's own built-in default */
  _Atomic size_t max_connections; /* 0 = unlimited */
  bool enable_keepalive; /* SO_KEEPALIVE on every accepted TCP connection;
                          * set once at chttpsvr_start, read-only afterward,
                          * same treatment as max_body_size above */
  _Atomic size_t current_connections;

  /* Idle-connection registry for the idle-timeout sweep: every connection
   * currently owned by the reactor and waiting for its next request's
   * headers (never a diverted, worker-owned connection). */
  mutex_t idle_mutex;
  chttpsvr_conn_t *idle_head, *idle_tail;

  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpserver's OWN engine)        */
/* ========================================================================== */

/*
 * One static, process-wide event_loop reactor shared by every chttpsvr
 * instance in the process -- confirmed via AskUserQuestion as one of two
 * separate, independent reactors (the other belongs to chttpclient, its own
 * analogous static event_loop; see chttpclient.c). Unlike the old
 * facio-backed design (coordinated via the now-deleted cfio_engine.c),
 * chttpserver and chttpclient no longer share a single process-wide
 * reactor, so this lifecycle wrapper needs no cross-module coordination at
 * all, only ref-counting across chttpsvr instances (mirroring the
 * acquire/release/reaper-thread shape cfio_engine.c originally established,
 * simplified: no atexit safety net cross-module ordering concern, since
 * there is nothing else in the process racing to bring this specific
 * reactor up first).
 */
static event_loop g_reactor = NULL;
static size_t g_reactor_refs = 0;
static mutex_t g_engine_mutex;
static cond_var_t g_engine_stopped_cv;
static once_flag_t g_engine_once = ONCE_INIT;
static bool g_engine_stopping = false;
static thread_id_t g_reaper_thread;
static bool g_reaper_joinable = false;
static ccol_memmgmt_procs_t g_engine_mp_storage;
static ccol_memmgmt_procs_t *g_engine_mp = NULL;

/* Repurposed "engine logger" (confirmed via AskUserQuestion): the new
 * event_loop reactor has no internal logging of its own to forward, unlike
 * facio's own unconditional FIO_LOG_* calls -- so this now captures
 * chttpserver's OWN reactor-thread diagnostics (TLS handshake failures,
 * listener bind errors, idle-timeout closes) across every chttpsvr instance
 * sharing the one process-wide reactor. NULL (the default) means
 * diagnostics are simply skipped; there is no default logger installed any
 * more, since there is nothing generating log-worthy events until the
 * caller opts in via chttpsvr_set_engine_logger(). Guarded by
 * g_engine_mutex purely against a torn pointer read/write racing a
 * concurrent chttpsvr_set_engine_logger() call -- clog itself is already
 * thread-safe for concurrent logging calls through one handle. */
static clog g_engine_logger = NULL;

/* Idle-timeout sweep thread: one per process, shared by every chttpsvr
 * instance's idle-connection registry (each server has its own
 * srv->idle_head/tail list; the sweep just walks every started server). */
static thread_id_t g_idle_sweep_thread;
static bool g_idle_sweep_running = false;
static bool g_idle_sweep_stop_flag = false;
static mutex_t g_servers_mutex;
static struct chttpserver **g_servers = NULL;
static size_t g_servers_count = 0, g_servers_cap = 0;

static void _idle_sweep_stop_if_running(void);

static void _engine_globals_init(void) {
  mutex_init(g_engine_mutex);
  cond_var_init(g_engine_stopped_cv);
  mutex_init(g_servers_mutex);
  /* SIGPIPE must be suppressed for all TCP servers, unconditionally: a
   * client can close its read side (or the whole connection) while a worker
   * is still mid-write on the response, and chttp1_stream_write's raw
   * write()/send() has no per-call SIGNONE suppression of its own. Without
   * this, that write raises SIGPIPE, whose default disposition kills the
   * whole process -- exactly the behavior facio's own fio.c already
   * documents and installs this same global handler for. */
  signal(SIGPIPE, SIG_IGN);
}

static clog _engine_logger_get(void) {
  call_once(g_engine_once, _engine_globals_init);
  mutex_lock(g_engine_mutex);
  clog l = g_engine_logger;
  mutex_unlock(g_engine_mutex);
  return l;
}

static void _join_reaper_if_needed_locked(void) {
  if (g_reaper_joinable) {
    thread_join(g_reaper_thread);
    g_reaper_joinable = false;
  }
}

static void *_engine_reaper_fn(void *arg) {
  (void)arg;
  event_loop loop_to_destroy;
  mutex_lock(g_engine_mutex);
  loop_to_destroy = g_reactor;
  mutex_unlock(g_engine_mutex);
  /* Stop the idle sweep thread before tearing down the reactor it calls
   * into (_conn_close -> event_loop_remove) -- see
   * _idle_sweep_stop_if_running's own doc comment. */
  _idle_sweep_stop_if_running();
  if (loop_to_destroy) event_loop_destroy(loop_to_destroy);
  /* g_servers' backing array is a plain realloc'd buffer, not itself tied to
   * any one server's lifetime -- every server that ever contributed a ref
   * to this engine has already been unregistered (and destroyed) by the
   * time the ref count could reach zero and get here, so g_servers_count is
   * always 0 at this point; free the now-empty array itself so it doesn't
   * show up as a still-reachable allocation for the rest of the process. */
  mutex_lock(g_servers_mutex);
  free(g_servers);
  g_servers = NULL;
  g_servers_cap = 0;
  mutex_unlock(g_servers_mutex);
  mutex_lock(g_engine_mutex);
  g_reactor = NULL;
  g_engine_stopping = false;
  /* The engine-wide diagnostics logger (chttpsvr_set_engine_logger) is
   * scoped to this reactor's own lifetime, same as the reactor itself:
   * closed here so a full engine stop doesn't leave it dangling as a
   * still-reachable allocation for the rest of the process's life. A later
   * chttpsvr_start() can bring the reactor back up; if engine-level
   * logging is wanted after that, chttpsvr_set_engine_logger() must be
   * called again, exactly like the "no logger configured" default already
   * documented for a fresh process. */
  clog old_logger = g_engine_logger;
  g_engine_logger = NULL;
  cond_var_broadcast(g_engine_stopped_cv);
  mutex_unlock(g_engine_mutex);
  if (old_logger) clog_close(old_logger);
  return NULL;
}

static void _spawn_reaper(void) {
  thread_id_t reaper;
  if (thread_create(reaper, _engine_reaper_fn, NULL) != 0) {
    /* No safer fallback than running it inline (OOM-class failure); nothing
     * to join afterward since it already ran to completion synchronously. */
    _engine_reaper_fn(NULL);
    return;
  }
  mutex_lock(g_engine_mutex);
  g_reaper_thread = reaper;
  g_reaper_joinable = true;
  mutex_unlock(g_engine_mutex);
}

static ccol_retval_t _engine_acquire(void) {
  call_once(g_engine_once, _engine_globals_init);
  mutex_lock(g_engine_mutex);
  while (g_engine_stopping) cond_var_wait(g_engine_stopped_cv, g_engine_mutex);
  _join_reaper_if_needed_locked();

  if (!g_reactor) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    size_t nthreads = (cpus > 0) ? (size_t)cpus : 1;
    char *err = NULL;
    g_reactor =
        event_loop_create_with_mprocs(256, 4, nthreads, g_engine_mp, &err);
    if (!g_reactor) {
      mutex_unlock(g_engine_mutex);
      return ccol_not_enough_memory;
    }
  }
  g_reactor_refs++;
  mutex_unlock(g_engine_mutex);
  return ccol_success;
}

static void _engine_release(void) {
  bool should_reap = false;
  mutex_lock(g_engine_mutex);
  if (g_reactor_refs > 0) g_reactor_refs--;
  if (g_reactor_refs == 0 && g_reactor) {
    g_engine_stopping = true;
    should_reap = true;
  }
  mutex_unlock(g_engine_mutex);
  if (should_reap) _spawn_reaper();
}

static void _engine_wait_until_stopped(void) {
  call_once(g_engine_once, _engine_globals_init);
  mutex_lock(g_engine_mutex);
  while (g_reactor || g_engine_stopping)
    cond_var_wait(g_engine_stopped_cv, g_engine_mutex);
  _join_reaper_if_needed_locked();
  mutex_unlock(g_engine_mutex);
}

static void _engine_force_stop(void) {
  call_once(g_engine_once, _engine_globals_init);
  bool should_reap = false;
  mutex_lock(g_engine_mutex);
  if (g_reactor) {
    g_reactor_refs = 0;
    g_engine_stopping = true;
    should_reap = true;
  }
  mutex_unlock(g_engine_mutex);
  if (should_reap) _spawn_reaper();
}

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP (module-local, own thread)           */
/* ========================================================================== */

static void _conn_close(chttpsvr_conn_t *conn);
static void _conn_reject_and_close(chttpsvr_conn_t *conn);

/* Registers a server so the idle sweep thread walks its idle-connection
 * list too. Called from chttpsvr_start -- which runs not just once per
 * server but again on every stop/start restart cycle -- so this must be
 * idempotent: skip the add if srv is already present. Without this check,
 * a srv that goes through N restart cycles ends up in g_servers N+1 times,
 * and _servers_unregister's single-occurrence removal (see below) would
 * leave N stale, dangling pointers behind after __chttpsvr_destroy frees
 * srv -- a real use-after-free the idle sweep thread would read on its very
 * next pass, caught by valgrind via
 * restart_races_live_keep_alive_connection_is_safe (which restarts one srv
 * 5 times before destroying it). */
static void _servers_register(struct chttpserver *srv) {
  mutex_lock(g_servers_mutex);
  for (size_t i = 0; i < g_servers_count; i++) {
    if (g_servers[i] == srv) {
      mutex_unlock(g_servers_mutex);
      return;
    }
  }
  if (g_servers_count == g_servers_cap) {
    size_t new_cap = g_servers_cap ? g_servers_cap * 2 : 8;
    struct chttpserver **nn = (struct chttpserver **)realloc(
        g_servers, new_cap * sizeof(struct chttpserver *));
    if (nn) {
      g_servers = nn;
      g_servers_cap = new_cap;
    }
  }
  if (g_servers_count < g_servers_cap) g_servers[g_servers_count++] = srv;
  mutex_unlock(g_servers_mutex);
}

static void _servers_unregister(struct chttpserver *srv) {
  mutex_lock(g_servers_mutex);
  for (size_t i = 0; i < g_servers_count; i++) {
    if (g_servers[i] == srv) {
      g_servers[i] = g_servers[g_servers_count - 1];
      g_servers_count--;
      break;
    }
  }
  mutex_unlock(g_servers_mutex);
}

#define _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS 1000

static void *_idle_sweep_fn(void *arg) {
  (void)arg;
  while (!g_idle_sweep_stop_flag) {
    struct timespec ts = {
        .tv_sec = _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS / 1000,
        .tv_nsec = (_CHTTPSVR_IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    if (g_idle_sweep_stop_flag) break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    mutex_lock(g_servers_mutex);
    for (size_t i = 0; i < g_servers_count; i++) {
      struct chttpserver *srv = g_servers[i];
      unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
      if (!idle_ms) continue;

      /* Collect timed-out connections under idle_mutex, claiming (removing)
       * each one from the list right here rather than merely copying its
       * pointer out, then close them outside the lock: _conn_close
       * ultimately calls event_loop_remove, which must not run while
       * holding a lock a callback dispatched from that same removal could
       * also need. Claiming at collection time -- not just after, in a
       * separately-locked pass -- is what stops a connection from being
       * simultaneously "found here" and dispatched to _conn_pump on a
       * reactor thread; see _idle_list_try_claim's own doc comment for the
       * real use-after-free (a freed SSL* read concurrently by
       * ctls_conn_read) this closes. */
      chttpsvr_conn_t *to_close[64];
      size_t to_close_n = 0;
      mutex_lock(srv->idle_mutex);
      chttpsvr_conn_t *c = srv->idle_head;
      while (c && to_close_n < 64) {
        chttpsvr_conn_t *next = c->idle_next;
        long elapsed_ms = (long)(now.tv_sec - c->last_activity.tv_sec) * 1000 +
                          (now.tv_nsec - c->last_activity.tv_nsec) / 1000000L;
        if ((unsigned long)elapsed_ms >= idle_ms) {
          if (c->idle_prev) c->idle_prev->idle_next = c->idle_next;
          if (c->idle_next) c->idle_next->idle_prev = c->idle_prev;
          if (srv->idle_head == c) srv->idle_head = c->idle_next;
          if (srv->idle_tail == c) srv->idle_tail = c->idle_prev;
          c->idle_prev = c->idle_next = NULL;
          c->in_idle_list = false;
          to_close[to_close_n++] = c;
        }
        c = next;
      }
      mutex_unlock(srv->idle_mutex);
      if (to_close_n > 0) {
        clog el = _engine_logger_get();
        if (el)
          log_info(el, "idle-timeout closing %zu connection(s)", to_close_n);
      }
      for (size_t j = 0; j < to_close_n; j++) _conn_close(to_close[j]);
    }
    mutex_unlock(g_servers_mutex);
  }
  return NULL;
}

static void _idle_sweep_start_if_needed(void) {
  mutex_lock(g_servers_mutex);
  if (!g_idle_sweep_running) {
    g_idle_sweep_stop_flag = false;
    if (thread_create(g_idle_sweep_thread, _idle_sweep_fn, NULL) == 0)
      g_idle_sweep_running = true;
  }
  mutex_unlock(g_servers_mutex);
}

/* Stops and joins the idle sweep thread, if one is running. Tied to the
 * shared engine's own lifetime (called from the engine reaper, alongside
 * event_loop_destroy) rather than to any single server's stop/destroy,
 * since the sweep thread walks every registered server, not one -- an
 * unjoined sweep thread still running at process exit is exactly the class
 * of "possibly lost" glibc TLS (allocate_dtv) false positive this
 * codebase's other reaper threads are already documented to close (see
 * this module's own reaper thread above). */
static void _idle_sweep_stop_if_running(void) {
  bool was_running = false;
  thread_id_t t = {0};
  mutex_lock(g_servers_mutex);
  if (g_idle_sweep_running) {
    g_idle_sweep_stop_flag = true;
    t = g_idle_sweep_thread;
    was_running = true;
    g_idle_sweep_running = false;
  }
  mutex_unlock(g_servers_mutex);
  if (was_running) thread_join(t);
}

static void _idle_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  mutex_lock(srv->idle_mutex);
  if (!conn->in_idle_list) {
    conn->idle_prev = srv->idle_tail;
    conn->idle_next = NULL;
    if (srv->idle_tail) srv->idle_tail->idle_next = conn;
    srv->idle_tail = conn;
    if (!srv->idle_head) srv->idle_head = conn;
    conn->in_idle_list = true;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  mutex_unlock(srv->idle_mutex);
}

static void _idle_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
  }
  mutex_unlock(srv->idle_mutex);
}

/* Attempts to atomically claim conn out of the idle list for exclusive
 * processing (either dispatching a real I/O event for it, or closing it).
 * Returns true if conn was idle and has now been removed -- the caller has
 * sole ownership and may safely read/free conn. Returns false if conn was
 * NOT in the idle list -- someone else (another dispatch, or a concurrent
 * closer) already claimed it, and the caller must not touch conn at all.
 *
 * This is the one piece of synchronization that makes it safe for
 * _close_all_idle_connections/_idle_sweep_fn to free a connection found in
 * the idle list from a thread that is not the reactor: without it, a
 * connection could be simultaneously "found idle" by a closer AND
 * dispatched to _conn_pump on a reactor thread (new data having arrived in
 * the same instant), and the closer's _conn_free (which for a TLS
 * connection calls ctls_conn_destroy -> SSL_free) could run concurrently
 * with _conn_pump's ctls_conn_read on the very same SSL* -- a real
 * use-after-free valgrind caught (a segfault deep in libcrypto's BIO code,
 * reproduced only against tests_tls.c, since the plaintext path's
 * equivalent race is a same-shape but much less immediately fatal
 * "read()/close() lost a race" instead of freeing live OpenSSL state).
 *
 * A connection is only ever added to the idle list AFTER the corresponding
 * event_loop_add/_modify call that makes it dispatchable has already
 * returned (see _conn_pump's handshake-wait branch and the main read
 * loop's EWOULDBLOCK branch, and _task_worker's keep-alive re-arm) -- so a
 * dispatch can, in a narrow window, fire before the idle-list add has
 * happened yet and see try_claim fail here. That is harmless, not a bug:
 * this module always uses level-triggered epoll, so a spurious "not idle
 * yet" claim failure just means the same readiness is reported again on
 * the very next epoll_wait, by which point the add has long since
 * completed (a handful of instructions on the same thread, no I/O in
 * between) -- never a dropped or hung request. */
static bool _idle_list_try_claim(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  bool claimed = false;
  mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
    claimed = true;
  }
  mutex_unlock(srv->idle_mutex);
  return claimed;
}

/* Closes and frees every connection of srv's still sitting idle (reactor-
 * owned, awaiting its next pipelined request or simply an open keep-alive
 * connection nothing has used again yet) -- called from __chttpsvr_destroy,
 * after chttpsvr_stop() has closed the listener so no new connection can
 * arrive. Without this, a keep-alive connection a test client never
 * explicitly closed (the common case: chttpclient's own idle pool keeps a
 * connection open after a response, not closed) outlives the server that
 * accepted it, leaking its chttpsvr_conn_t (and everything it owns: path,
 * headers, route match state, ...) for the rest of the process's life --
 * caught by valgrind as a real "definitely lost" block traced back to
 * _conn_create/_listener_on_readable, not a false positive.
 *
 * Each candidate is claimed (removed from the idle list) at collection
 * time, under idle_mutex, via _idle_list_try_claim -- not merely copied
 * out and closed afterward -- so a connection can never be simultaneously
 * "found here" and "dispatched to _conn_pump on a reactor thread"; see
 * _idle_list_try_claim's own doc comment for the use-after-free this
 * closes. */
static void _close_all_idle_connections(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *to_close[64];
    size_t to_close_n = 0;
    mutex_lock(srv->idle_mutex);
    chttpsvr_conn_t *c = srv->idle_head;
    while (c && to_close_n < 64) {
      chttpsvr_conn_t *next = c->idle_next;
      if (c->idle_prev) c->idle_prev->idle_next = c->idle_next;
      if (c->idle_next) c->idle_next->idle_prev = c->idle_prev;
      if (srv->idle_head == c) srv->idle_head = c->idle_next;
      if (srv->idle_tail == c) srv->idle_tail = c->idle_prev;
      c->idle_prev = c->idle_next = NULL;
      c->in_idle_list = false;
      to_close[to_close_n++] = c;
      c = next;
    }
    mutex_unlock(srv->idle_mutex);
    if (to_close_n == 0) break;
    for (size_t i = 0; i < to_close_n; i++) _conn_close(to_close[i]);
  }
}

/* ========================================================================== */
/*                         PERCENT-DECODE HELPERS                             */
/* ========================================================================== */

/* Replaces facio's http_decode_path_unsafe()/http_decode_url_unsafe(): plain
 * RFC 3986 %XX decoding, with '+' either left literal (path semantics) or
 * turned into a space (query-string semantics). Safe for in-place decoding
 * (dst == src): the write index never overtakes the read index. Returns the
 * decoded length, or -1 on malformed percent-encoding (a '%' not followed by
 * two hex digits) -- matching both replaced functions' contracts exactly. */
static int _hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

static ssize_t _percent_decode(char *dst, const char *src, bool plus_as_space) {
  size_t si = 0, di = 0;
  while (src[si]) {
    char c = src[si];
    if (c == '%') {
      char c1 = src[si + 1];
      if (!c1) return -1;
      int hi = _hex_nibble(c1);
      char c2 = src[si + 2];
      int lo = _hex_nibble(c2);
      if (hi < 0 || lo < 0) return -1;
      dst[di++] = (char)((hi << 4) | lo);
      si += 3;
    } else if (c == '+' && plus_as_space) {
      dst[di++] = ' ';
      si++;
    } else {
      dst[di++] = c;
      si++;
    }
  }
  return (ssize_t)di;
}

static ssize_t _decode_path_unsafe(char *dst, const char *src) {
  return _percent_decode(dst, src, false);
}

static ssize_t _decode_url_unsafe(char *dst, const char *src) {
  return _percent_decode(dst, src, true);
}

/* Private sentinel returned by _parse_method for unrecognised method strings.
 * Not part of the public chttp_method_t enum; using it as a route method is
 * impossible, so it can never match any registered route. */
#define _CHTTP_METHOD_UNKNOWN ((chttp_method_t)(CHTTP_ANY + 1))

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

static void _chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp);
static void _task_worker(void *arg);
static void _destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs_t *mp);
static void _conn_on_readable(event_loop loop, ccol_selectable *sel, void *arg);
static void _conn_on_writable(event_loop loop, ccol_selectable *sel, void *arg);
static void _conn_on_error(event_loop loop, ccol_selectable *sel, void *arg);
static void _listener_on_readable(event_loop loop, ccol_selectable *sel,
                                  void *arg);

/* ========================================================================== */
/*                         PATTERN COMPILATION                                */
/* ========================================================================== */

static ccol_retval_t _compile_pattern(const char *pattern, char ***segs_out,
                                      int *seg_count_out,
                                      char ***param_names_out,
                                      int *param_count_out,
                                      ccol_memmgmt_procs_t *mp) {
  if (pattern[0] != '/') return ccol_invalid_args;

  const char *p = pattern;
  if (*p == '/') p++;

  {
    size_t plen = strlen(p);
    if (plen > 0 && p[plen - 1] == '/') return ccol_invalid_args;
  }

  int seg_count = 0;
  {
    const char *s = p;
    while (*s) {
      const char *e = strchr(s, '/');
      size_t len = e ? (size_t)(e - s) : strlen(s);
      if (len == 0 && e != NULL) return ccol_invalid_args;
      if (len > 0) seg_count++;
      if (!e) break;
      s = e + 1;
    }
  }

  if (seg_count == 0) {
    *segs_out = NULL;
    *seg_count_out = 0;
    *param_names_out = NULL;
    *param_count_out = 0;
    return ccol_success;
  }

  char **segs = (char **)_mem_calloc(mp, (size_t)seg_count, sizeof(char *));
  if (!segs) return ccol_not_enough_memory;

  int si = 0;
  int param_count = 0;
  const char *s = p;
  while (*s && si < seg_count) {
    const char *e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    if (len == 0) {
      if (!e) break;
      s = e + 1;
      continue;
    }
    segs[si] = (char *)_mem_alloc(mp, len + 1);
    if (!segs[si]) {
      for (int j = 0; j < si; j++) _mem_free(mp, segs[j]);
      _mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    memcpy(segs[si], s, len);
    segs[si][len] = '\0';
    if (segs[si][0] == '{') {
      if (len < 3 || segs[si][len - 1] != '}') {
        for (int j = 0; j <= si; j++) _mem_free(mp, segs[j]);
        _mem_free(mp, segs);
        return ccol_invalid_args;
      }
      for (size_t ci = 1; ci < len - 1; ci++) {
        char c = segs[si][ci];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
          for (int j = 0; j <= si; j++) _mem_free(mp, segs[j]);
          _mem_free(mp, segs);
          return ccol_invalid_args;
        }
      }
      param_count++;
    }
    si++;
    if (!e) break;
    s = e + 1;
  }

  char **param_names = NULL;
  if (param_count > 0) {
    param_names = (char **)_mem_calloc(mp, (size_t)param_count, sizeof(char *));
    if (!param_names) {
      for (int j = 0; j < seg_count; j++) _mem_free(mp, segs[j]);
      _mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    int pi = 0;
    for (int i = 0; i < seg_count; i++) {
      if (segs[i][0] == '{') {
        size_t len = strlen(segs[i]);
        param_names[pi] = (char *)_mem_alloc(mp, len - 1);
        if (!param_names[pi]) {
          for (int j = 0; j < pi; j++) _mem_free(mp, param_names[j]);
          _mem_free(mp, param_names);
          for (int j = 0; j < seg_count; j++) _mem_free(mp, segs[j]);
          _mem_free(mp, segs);
          return ccol_not_enough_memory;
        }
        memcpy(param_names[pi], segs[i] + 1, len - 2);
        param_names[pi][len - 2] = '\0';
        pi++;
      }
    }
  }

  *segs_out = segs;
  *seg_count_out = seg_count;
  *param_names_out = param_names;
  *param_count_out = param_count;
  return ccol_success;
}

static void _free_route_data(chttpsvr_route_t *r, ccol_memmgmt_procs_t *mp) {
  for (int i = 0; i < r->seg_count; i++) _mem_free(mp, r->segs[i]);
  _mem_free(mp, r->segs);
  for (int i = 0; i < r->param_count; i++) _mem_free(mp, r->param_names[i]);
  _mem_free(mp, r->param_names);
}

/* ========================================================================== */
/*                         URL DECODING HELPERS                               */
/* ========================================================================== */

static char *_decode_seg_alloc(const char *seg, size_t len,
                               ccol_memmgmt_procs_t *mp, bool *oom_out) {
  if (oom_out) *oom_out = false;

  char src_buf[512];
  char *src = src_buf;
  bool src_heap = false;
  if (len + 1 > sizeof(src_buf)) {
    src = (char *)_mem_alloc(mp, len + 1);
    if (!src) {
      if (oom_out) *oom_out = true;
      return NULL;
    }
    src_heap = true;
  }
  memcpy(src, seg, len);
  src[len] = '\0';

  char *dest = (char *)_mem_alloc(mp, len + 1);
  if (!dest) {
    if (src_heap) _mem_free(mp, src);
    if (oom_out) *oom_out = true;
    return NULL;
  }
  ssize_t dlen = _decode_path_unsafe(dest, src);
  if (src_heap) _mem_free(mp, src);
  if (dlen < 0) {
    _mem_free(mp, dest);
    return NULL;
  }
  dest[(size_t)dlen] = '\0';
  return dest;
}

static int _seg_matches_literal(const char *path_seg, size_t seg_len,
                                const char *pattern_seg,
                                ccol_memmgmt_procs_t *mp) {
  char src_buf[512];
  char decoded_buf[512];
  char *src = src_buf;
  char *decoded = decoded_buf;
  bool src_heap = false;
  bool dec_heap = false;

  if (seg_len + 1 > sizeof(src_buf)) {
    src = (char *)_mem_alloc(mp, seg_len + 1);
    if (!src) return -1;
    src_heap = true;
  }
  if (seg_len + 1 > sizeof(decoded_buf)) {
    decoded = (char *)_mem_alloc(mp, seg_len + 1);
    if (!decoded) {
      if (src_heap) _mem_free(mp, src);
      return -1;
    }
    dec_heap = true;
  }
  memcpy(src, path_seg, seg_len);
  src[seg_len] = '\0';
  ssize_t dlen = _decode_path_unsafe(decoded, src);
  if (dlen >= 0) decoded[(size_t)dlen] = '\0';
  int result = (dlen >= 0 && strcmp(decoded, pattern_seg) == 0) ? 1 : 0;
  if (src_heap) _mem_free(mp, src);
  if (dec_heap) _mem_free(mp, decoded);
  return result;
}

/* ========================================================================== */
/*                         ROUTE MATCHING                                     */
/* ========================================================================== */

typedef enum {
  ROUTE_MATCH_OK,
  ROUTE_MATCH_METHOD,
  ROUTE_MATCH_OOM,
  ROUTE_MATCH_NONE
} route_match_t;

typedef struct {
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  char **param_values; /* must be freed by caller on OK */
  route_match_t result;
} match_result_t;

static int _match_segments(const char *sub_path, chttpsvr_route_t *route,
                           char ***pv_out, ccol_memmgmt_procs_t *mp) {
  const char *p = sub_path;
  if (*p == '/') p++;

  if (route->seg_count == 0) {
    return (*p == '\0') ? 1 : 0;
  }

  char **pv = NULL;
  int param_idx = 0;
  for (int i = 0; i < route->seg_count; i++) {
    const char *next_sep = strchr(p, '/');
    size_t tok_len = next_sep ? (size_t)(next_sep - p) : strlen(p);

    if (tok_len == 0) goto no_match;

    if (route->segs[i][0] == '{') {
      if (pv_out) {
        if (!pv) {
          pv = (char **)_mem_calloc(mp, (size_t)route->param_count,
                                    sizeof(char *));
          if (!pv) return -1;
        }
        bool seg_oom = false;
        char *val = _decode_seg_alloc(p, tok_len, mp, &seg_oom);
        if (!val) {
          if (seg_oom) {
            for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
            _mem_free(mp, pv);
            if (pv_out) *pv_out = NULL;
            return -1;
          }
          goto no_match;
        }
        pv[param_idx++] = val;
      } else {
        char _vbuf[512];
        char *_vp = _vbuf;
        bool _vheap = false;
        if (tok_len + 1 > sizeof(_vbuf)) {
          _vp = (char *)_mem_alloc(mp, tok_len + 1);
          if (!_vp) return -1;
          _vheap = true;
        }
        memcpy(_vp, p, tok_len);
        _vp[tok_len] = '\0';
        ssize_t _vl = _decode_path_unsafe(_vp, _vp);
        if (_vheap) _mem_free(mp, _vp);
        if (_vl < 0) goto no_match;
      }
    } else {
      int lit = _seg_matches_literal(p, tok_len, route->segs[i], mp);
      if (lit < 0) {
        if (pv) {
          for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
          _mem_free(mp, pv);
          if (pv_out) *pv_out = NULL;
        }
        return -1;
      }
      if (!lit) goto no_match;
    }

    if (i == route->seg_count - 1) {
      if (next_sep != NULL) goto no_match;
      p += tok_len;
    } else {
      p = next_sep ? next_sep + 1 : p + tok_len;
    }
  }
  if (*p != '\0') goto no_match;
  if (pv_out) *pv_out = pv;
  return 1;

no_match:
  if (pv) {
    for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
    _mem_free(mp, pv);
    if (pv_out) *pv_out = NULL;
  }
  return 0;
}

static int _prefix_matches(const char *path, chttpsvr_router *router,
                           ccol_memmgmt_procs_t *mp,
                           const char **sub_path_out) {
  const char *pp = router->prefix + 1;
  const char *rp = path;
  if (*rp == '/') rp++;

  if (*pp == '\0') {
    if (*rp != '\0') return 0;
    *sub_path_out = "/";
    return 1;
  }

  const char *rp_after_last_seg = path;
  while (*pp) {
    const char *pe = strchr(pp, '/');
    size_t plen = pe ? (size_t)(pe - pp) : strlen(pp);

    const char *re = strchr(rp, '/');
    size_t rlen = re ? (size_t)(re - rp) : strlen(rp);
    if (rlen == 0) return 0;

    char pseg_buf[256];
    char *pseg = pseg_buf;
    bool pseg_heap = false;
    if (plen + 1 > sizeof(pseg_buf)) {
      pseg = (char *)_mem_alloc(mp, plen + 1);
      if (!pseg) return -1;
      pseg_heap = true;
    }
    memcpy(pseg, pp, plen);
    pseg[plen] = '\0';

    int lit = _seg_matches_literal(rp, rlen, pseg, mp);
    if (pseg_heap) _mem_free(mp, pseg);
    if (lit < 0) return -1;
    if (!lit) return 0;

    rp_after_last_seg = rp + rlen;
    rp = re ? re + 1 : rp + rlen;
    pp = pe ? pe + 1 : pp + plen;
  }

  *sub_path_out = (*rp_after_last_seg == '\0') ? "/" : rp_after_last_seg;
  return 1;
}

static match_result_t _find_route(struct chttpserver *srv, const char *path,
                                  chttp_method_t method) {
  bool method_mismatch_seen = false;

  for (size_t ri = 0; ri < srv->router_count; ri++) {
    chttpsvr_router *router = srv->routers[ri];

    const char *sub_path;
    if (router->prefix_len == 0) {
      sub_path = path;
    } else {
      int pm = _prefix_matches(path, router, srv->m_procs, &sub_path);
      if (pm < 0) return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
      if (pm == 0) continue;
    }

    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route_t *route = router->routes[i];

      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      int ms = _match_segments(sub_path, route, method_ok ? &pv : NULL,
                               srv->m_procs);
      if (ms == -1) return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
      if (ms == 0) continue;

      if (!method_ok) {
        method_mismatch_seen = true;
        continue;
      }
      return (match_result_t){router, route, pv, ROUTE_MATCH_OK};
    }
  }
  if (method_mismatch_seen)
    return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_METHOD};
  return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_NONE};
}

static void _free_param_values(char **pv, int count, ccol_memmgmt_procs_t *mp) {
  if (!pv) return;
  for (int i = 0; i < count; i++) _mem_free(mp, pv[i]);
  _mem_free(mp, pv);
}

/* ========================================================================== */
/*                         METHOD PARSING                                     */
/* ========================================================================== */

static chttp_method_t _parse_method(const char *s, size_t len) {
  if (len == 3 && memcmp(s, "GET", 3) == 0) return CHTTP_GET;
  if (len == 4 && memcmp(s, "POST", 4) == 0) return CHTTP_POST;
  if (len == 3 && memcmp(s, "PUT", 3) == 0) return CHTTP_PUT;
  if (len == 6 && memcmp(s, "DELETE", 6) == 0) return CHTTP_DELETE;
  if (len == 5 && memcmp(s, "PATCH", 5) == 0) return CHTTP_PATCH;
  if (len == 4 && memcmp(s, "HEAD", 4) == 0) return CHTTP_HEAD;
  if (len == 7 && memcmp(s, "OPTIONS", 7) == 0) return CHTTP_OPTIONS;
  return _CHTTP_METHOD_UNKNOWN;
}

/* ========================================================================== */
/*                         RESPONSE HELPERS                                   */
/* ========================================================================== */

static void _destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs_t *mp) {
  for (size_t i = 0; i < resp->header_count; i++) {
    _mem_free(mp, resp->headers[i].name);
    _mem_free(mp, resp->headers[i].value);
  }
  _mem_free(mp, resp->headers);
  resp->headers = NULL;
  resp->header_count = 0;
  resp->header_cap = 0;
  _mem_free(mp, resp->body);
  resp->body = NULL;
  resp->body_len = 0;
  resp->body_cap = 0;
}

static void _destroy_req_qparams(chttpsvr_req *req) {
  ccol_memmgmt_procs_t *mp = req->m_procs;
  if (req->_qparams) {
    chttpsvr_qparams_t *qp = req->_qparams;
    for (size_t i = 0; i < qp->count; i++) {
      _mem_free(mp, qp->keys[i]);
      _mem_free(mp, qp->values[i]);
    }
    _mem_free(mp, qp->keys);
    _mem_free(mp, qp->values);
    _mem_free(mp, qp);
  }
  if (req->_qresult) _mem_free(mp, req->_qresult);
}

static const char *_status_reason(int status) {
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 204:
      return "No Content";
    case 206:
      return "Partial Content";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 304:
      return "Not Modified";
    case 307:
      return "Temporary Redirect";
    case 308:
      return "Permanent Redirect";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 408:
      return "Request Timeout";
    case 409:
      return "Conflict";
    case 410:
      return "Gone";
    case 413:
      return "Payload Too Large";
    case 415:
      return "Unsupported Media Type";
    case 417:
      return "Expectation Failed";
    case 422:
      return "Unprocessable Entity";
    case 429:
      return "Too Many Requests";
    case 500:
      return "Internal Server Error";
    case 501:
      return "Not Implemented";
    case 502:
      return "Bad Gateway";
    case 503:
      return "Service Unavailable";
    case 504:
      return "Gateway Timeout";
    default:
      return "Unknown";
  }
}

/* Serializes resp into a raw HTTP/1.1 response and writes it to stream,
 * bounded by response_write_timeout_ms. Replaces facio's http_set_header2 +
 * http_send_body/http_finish. Always sets Content-Length explicitly (this
 * server never uses chunked transfer-encoding for its own responses) and a
 * Connection header reflecting keep_alive. Returns false on a write
 * error/timeout (caller must then treat the connection as unusable and
 * close it). */
static bool _send_response(chttp1_stream_t *stream, chttpsvr_resp *resp,
                           bool keep_alive, unsigned timeout_ms) {
  char head[4096];
  int status = (resp->status_code >= 100 && resp->status_code <= 999)
                   ? resp->status_code
                   : 500;
  int n = snprintf(head, sizeof(head), "HTTP/1.1 %d %s\r\n", status,
                   _status_reason(status));
  if (n < 0) return false;
  size_t hlen = (size_t)n;

  bool have_content_length = false, have_connection = false;
  for (size_t i = 0; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, "content-length") == 0)
      have_content_length = true;
    if (strcasecmp(resp->headers[i].name, "connection") == 0)
      have_connection = true;
  }

  for (size_t i = 0; i < resp->header_count; i++) {
    /* No space after the colon: matches facio's own write_header()
     * (third_party/facio/http1.c) byte for byte, which this test suite's
     * raw-socket assertions (e.g. strstr(buf, "connection:close")) were
     * originally written against. */
    int hn =
        snprintf(head + hlen, sizeof(head) > hlen ? sizeof(head) - hlen : 0,
                 "%s:%s\r\n", resp->headers[i].name, resp->headers[i].value);
    if (hn < 0) return false;
    if ((size_t)hn >= (hlen < sizeof(head) ? sizeof(head) - hlen : 0)) {
      /* Header block overflowed the fixed stack buffer (very large/many
       * response headers, an unusual case for a handler-authored response);
       * fail closed rather than truncate/corrupt the response. */
      return false;
    }
    hlen += (size_t)hn;
  }

  if (!have_content_length) {
    int cn = snprintf(head + hlen, sizeof(head) - hlen,
                      "content-length:%zu\r\n", resp->body_len);
    if (cn < 0 || (size_t)cn >= sizeof(head) - hlen) return false;
    hlen += (size_t)cn;
  }
  if (!have_connection) {
    const char *cval = keep_alive ? "keep-alive" : "close";
    int cn =
        snprintf(head + hlen, sizeof(head) - hlen, "connection:%s\r\n", cval);
    if (cn < 0 || (size_t)cn >= sizeof(head) - hlen) return false;
    hlen += (size_t)cn;
  }
  if (hlen + 2 >= sizeof(head)) return false;
  head[hlen++] = '\r';
  head[hlen++] = '\n';

  size_t sent = 0;
  while (sent < hlen) {
    ssize_t n2 =
        chttp1_stream_write(stream, head + sent, hlen - sent, (int)timeout_ms);
    if (n2 <= 0) return false;
    sent += (size_t)n2;
  }
  sent = 0;
  while (sent < resp->body_len) {
    ssize_t n3 = chttp1_stream_write(stream, resp->body + sent,
                                     resp->body_len - sent, (int)timeout_ms);
    if (n3 <= 0) return false;
    sent += (size_t)n3;
  }
  return true;
}

/* ========================================================================== */
/*                         MIDDLEWARE DISPATCH                                */
/* ========================================================================== */

static void _chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp) {
  dispatch_ctx_t *ctx = &req->conn->dispatch;
  if (ctx->mw_idx < ctx->mw_count) {
    _mw_entry_t entry = ctx->mw_snap[ctx->mw_idx++];
    entry.fn(req, resp, entry.ctx, _chttpsvr_next);
    return;
  }
  ctx->route->fn(req, resp, ctx->route->ctx);
}

/* ========================================================================== */
/*                    CONNECTION LIFECYCLE                                    */
/* ========================================================================== */

static void _conn_reset_for_request(chttpsvr_conn_t *conn) {
  ccol_memmgmt_procs_t *mp = conn->m_procs;
  _mem_free(mp, conn->path);
  conn->path = NULL;
  _mem_free(mp, conn->decoded_path);
  conn->decoded_path = NULL;
  _mem_free(mp, conn->raw_query);
  conn->raw_query = NULL;
  for (size_t i = 0; i < conn->hdr_count; i++) {
    _mem_free(mp, conn->hdr_names[i]);
    _mem_free(mp, conn->hdr_values[i]);
  }
  _mem_free(mp, conn->hdr_names);
  _mem_free(mp, conn->hdr_values);
  conn->hdr_names = conn->hdr_values = NULL;
  conn->hdr_count = conn->hdr_cap = 0;
  /* Free the just-finished request's matched param values before nulling
   * the pointer -- _conn_free (final teardown) already does this correctly
   * for the LAST request on a connection, but a keep-alive connection
   * reaches this reset function between every request, and previously
   * nulled the pointer here with no free at all, leaking every param-value
   * array except the final one (caught by valgrind as "definitely lost"
   * blocks traced to _match_segments' pv calloc, plus their "indirectly
   * lost" decoded string contents). */
  _free_param_values(conn->matched_param_values,
                     conn->matched_route ? conn->matched_route->param_count : 0,
                     mp);
  conn->matched_router = NULL;
  conn->matched_route = NULL;
  conn->matched_param_values = NULL;
  memset(&conn->dispatch, 0, sizeof(conn->dispatch));
  _destroy_resp(&conn->resp, mp);
  conn->resp.status_code = CHTTP_STATUS_OK;
  conn->resp.m_procs = mp;
  conn->req_rejected = false;
  conn->reject_status = 500;
  conn->expects_continue = false;
  _mem_free(mp, conn->body.buf);
  memset(&conn->body, 0, sizeof(conn->body));
  conn->body_bytes_seen = 0;
  conn->body_too_large = false;
  conn->read_deadline_set = false;
  conn->deadline_exceeded = false;

  chttp1_parser_t parser;
  chttp1_parser_init_request(&parser, conn->parser.settings);
  parser.data = conn;
  if (conn->srv->max_header_bytes) {
    parser.max_header_count_override = 0;
    parser.max_total_header_bytes_override = conn->srv->max_header_bytes;
  }
  conn->parser = parser;
}

static chttpsvr_conn_t *_conn_create(struct chttpserver *srv, int fd,
                                     const chttp1_settings_t *settings) {
  chttpsvr_conn_t *conn =
      (chttpsvr_conn_t *)_mem_calloc(srv->m_procs, 1, sizeof(*conn));
  if (!conn) return NULL;
  conn->fd = fd;
  conn->srv = srv;
  conn->m_procs = srv->m_procs;
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  /* _conn_reset_for_request() establishes every per-request default this
   * connection needs before its first request too (resp.status_code,
   * resp.m_procs, reject_status, the header-size override, ...) -- a freshly
   * calloc'd conn has resp.status_code == 0, which _send_response() would
   * otherwise treat as an unset/invalid status and silently map to 500 for
   * a connection's very first request (subsequent keep-alive requests never
   * showed this, since they already go through this same reset call). All
   * of the frees this performs are no-ops here (every freed field is still
   * NULL/0 from calloc). */
  _conn_reset_for_request(conn);
  return conn;
}

static void _conn_free(chttpsvr_conn_t *conn) {
  if (!conn) return;
  ccol_memmgmt_procs_t *mp = conn->m_procs;
  _idle_list_remove(conn);
  if (conn->tls) ctls_conn_destroy(conn->tls);
  if (conn->fd >= 0) close(conn->fd);
  atomic_fetch_sub(&conn->srv->current_connections, 1);
  _mem_free(mp, conn->path);
  _mem_free(mp, conn->decoded_path);
  _mem_free(mp, conn->raw_query);
  for (size_t i = 0; i < conn->hdr_count; i++) {
    _mem_free(mp, conn->hdr_names[i]);
    _mem_free(mp, conn->hdr_values[i]);
  }
  _mem_free(mp, conn->hdr_names);
  _mem_free(mp, conn->hdr_values);
  _free_param_values(conn->matched_param_values,
                     conn->matched_route ? conn->matched_route->param_count : 0,
                     mp);
  _destroy_resp(&conn->resp, mp);
  _mem_free(mp, conn->body.buf);
  _mem_free(mp, conn);
}

static void _conn_close(chttpsvr_conn_t *conn) {
  if (conn->reg) {
    event_loop_remove(g_reactor, conn->reg);
    conn->reg = NULL;
  }
  conn->state = CONN_ST_CLOSING;
  _conn_free(conn);
}

/* ========================================================================== */
/*                    PARSER CALLBACKS (reactor-thread side)                  */
/* ========================================================================== */

static int _on_request_line(chttp1_parser_t *p, const char *method,
                            size_t method_len, const char *target,
                            size_t target_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  conn->method = _parse_method(method, method_len);

  const char *qmark = (const char *)memchr(target, '?', target_len);
  size_t path_raw_len = qmark ? (size_t)(qmark - target) : target_len;

  /* Stored RAW (still percent-encoded) -- see conn->path's own doc comment
   * for why this must not be decoded here. */
  char *path_raw = (char *)_mem_alloc(conn->m_procs, path_raw_len + 1);
  if (!path_raw) return 1;
  memcpy(path_raw, target, path_raw_len);
  path_raw[path_raw_len] = '\0';
  conn->path = path_raw;

  if (qmark) {
    size_t qlen = target_len - path_raw_len - 1;
    conn->raw_query = (char *)_mem_alloc(conn->m_procs, qlen + 1);
    if (!conn->raw_query) return 1;
    memcpy(conn->raw_query, qmark + 1, qlen);
    conn->raw_query[qlen] = '\0';
  }
  return 0;
}

static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  if (conn->hdr_count >= conn->hdr_cap) {
    size_t new_cap = conn->hdr_cap ? conn->hdr_cap * 2 : 8;
    char **nn = (char **)_mem_realloc(conn->m_procs, conn->hdr_names,
                                      new_cap * sizeof(char *));
    if (!nn) return 1;
    conn->hdr_names = nn;
    char **nv = (char **)_mem_realloc(conn->m_procs, conn->hdr_values,
                                      new_cap * sizeof(char *));
    if (!nv) return 1;
    conn->hdr_values = nv;
    conn->hdr_cap = new_cap;
  }
  char *n = (char *)_mem_alloc(conn->m_procs, name_len + 1);
  char *v = (char *)_mem_alloc(conn->m_procs, value_len + 1);
  if (!n || !v) {
    _mem_free(conn->m_procs, n);
    _mem_free(conn->m_procs, v);
    return 1;
  }
  memcpy(n, name, name_len);
  n[name_len] = '\0';
  memcpy(v, value, value_len);
  v[value_len] = '\0';
  conn->hdr_names[conn->hdr_count] = n;
  conn->hdr_values[conn->hdr_count] = v;
  conn->hdr_count++;
  return 0;
}

static int _on_headers_complete(chttp1_parser_t *p) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  if (conn->req_rejected) return -1; /* already decided in _on_request_line */

  struct chttpserver *srv = conn->srv;
  rw_lock_rdlock(srv->routes_lock);
  match_result_t mr = _find_route(srv, conn->path, conn->method);
  if (mr.result == ROUTE_MATCH_NONE) {
    rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_NOT_FOUND;
    return -1;
  }
  if (mr.result == ROUTE_MATCH_METHOD) {
    rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_METHOD_NOT_ALLOWED;
    return -1;
  }
  if (mr.result == ROUTE_MATCH_OOM) {
    rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }

  dispatch_ctx_t *dispatch = &conn->dispatch;
  dispatch->srv = srv;
  dispatch->router = mr.router;
  dispatch->route = mr.route;
  dispatch->mw_idx = 0;
  {
    int mc = 0;
    chttpsvr_mw_node_t *n;
    for (n = srv->routers[0]->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
      dispatch->mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
    bool overflow = (n != NULL);
    if (!overflow && mr.router != srv->routers[0]) {
      for (n = mr.router->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
        dispatch->mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
      overflow = (n != NULL);
    }
    dispatch->mw_count = mc;
    if (overflow) {
      rw_lock_unlock(srv->routes_lock);
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return -1;
    }
  }
  rw_lock_unlock(srv->routes_lock);

  conn->matched_router = mr.router;
  conn->matched_route = mr.route;
  conn->matched_param_values = mr.param_values;
  conn->expects_continue = chttp1_expects_continue(p);

  /* A successful match already proved every segment of conn->path (RAW,
   * still percent-encoded) decodes cleanly -- _match_segments/
   * _seg_matches_literal treat any decode failure as a non-match, which
   * would have taken the ROUTE_MATCH_NONE/ROUTE_MATCH_METHOD branch above
   * instead of reaching here. Decoding the whole path now, for the public
   * chttpsvr_req_path() accessor, therefore cannot fail on malformed input;
   * only a real allocation failure can. */
  size_t plen = strlen(conn->path);
  char *dp = (char *)_mem_alloc(srv->m_procs, plen + 1);
  if (dp) {
    ssize_t dlen = _decode_path_unsafe(dp, conn->path);
    if (dlen >= 0) {
      dp[(size_t)dlen] = '\0';
      conn->decoded_path = dp;
    } else {
      _mem_free(srv->m_procs, dp);
    }
  }

  /* Always divert: every matched route (buffered or streaming) is handed to
   * the worker pool regardless of body size, exactly matching this
   * module's existing documented threading model. chttp1_parser itself
   * downgrades this to an ordinary immediate completion when there turns
   * out to be no body to divert (see CHTTP1_HEADERS_DIVERT_BODY's own doc
   * comment) -- either way, the header-read callback below submits to the
   * worker pool once execute() returns CHTTP1_HEADERS_ONLY or CHTTP1_PAUSED. */
  return CHTTP1_HEADERS_DIVERT_BODY;
}

/* Buffered-route body accumulation, and streaming-route "pending, not yet
 * delivered to chttpsvr_req_read" body accumulation, share this one
 * callback and one growbuf_t: max_body_size is enforced here, uniformly,
 * for both route kinds (facio enforced it inside its own body-chunk parser;
 * this parser has no built-in notion of it, so the caller -- here -- must). */
static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  conn->body_bytes_seen += len;
  if (conn->body_bytes_seen > conn->srv->max_body_size) {
    conn->body_too_large = true;
    return 1;
  }
  growbuf_t *b = &conn->body;
  if (!conn->matched_route->is_streaming && b->pos == b->len) {
    /* Buffered route: never drained via pos (chttpsvr_req_read is not used),
     * so pos always stays 0; nothing to compact. */
  } else if (conn->matched_route->is_streaming && b->pos == b->len &&
             b->pos > 0) {
    b->pos = b->len = 0; /* compact: fully drained by a prior req_read call */
  }
  if (b->len + len > b->cap) {
    size_t new_cap = b->cap ? b->cap * 2 : 8192;
    while (new_cap < b->len + len) new_cap *= 2;
    char *nb = (char *)_mem_realloc(conn->m_procs, b->buf, new_cap);
    if (!nb) return 1;
    b->buf = nb;
    b->cap = new_cap;
  }
  memcpy(b->buf + b->len, at, len);
  b->len += len;
  return 0;
}

static int _on_message_complete(chttp1_parser_t *p) {
  (void)p;
  return 0;
}

static chttp1_settings_t g_parser_settings;
static once_flag_t g_parser_settings_once = ONCE_INIT;

static void _init_parser_settings(void) {
  chttp1_settings_init(&g_parser_settings);
  g_parser_settings.on_request_line = _on_request_line;
  g_parser_settings.on_header = _on_header;
  g_parser_settings.on_headers_complete = _on_headers_complete;
  g_parser_settings.on_body = _on_body;
  g_parser_settings.on_message_complete = _on_message_complete;
}

/* ========================================================================== */
/*                    REACTOR-THREAD READ/HANDSHAKE CALLBACKS                 */
/* ========================================================================== */

static void _conn_start_diverted(chttpsvr_conn_t *conn, const char *leftover,
                                 size_t leftover_len) {
  _idle_list_remove(conn);
  if (conn->reg) {
    event_loop_remove(g_reactor, conn->reg);
    conn->reg = NULL;
  }
  conn->state = CONN_ST_DIVERTED;

  mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ctpool pool = conn->srv->worker_pool;
  mutex_unlock(conn->srv->mutex);

  /* Copy leftover now: it points into the reactor's own stack read buffer,
   * which is about to go out of scope the moment this callback returns.
   * _task_worker re-derives nothing from chttp1_parser_consumed() itself --
   * that value is only meaningful relative to the exact buffer/length pair
   * passed to the execute() call that produced it, which is the reactor's
   * own stack buffer, gone by the time the worker runs. Passing the
   * already-copied carry bytes directly avoids that lifetime hazard
   * entirely. */
  char *carry = NULL;
  if (leftover_len > 0) {
    carry = (char *)_mem_alloc(conn->m_procs, leftover_len);
    if (carry) memcpy(carry, leftover, leftover_len);
  }
  /* Publish carry/_carry_over_len BEFORE submitting to the pool, not after:
   * ctpool_try_submit can hand this task to an already-idle worker thread
   * that starts running _task_worker(conn) immediately, concurrently with
   * the rest of this function. Setting these fields after the submit call
   * raced that worker thread reading conn->_carry_over -- it would see
   * NULL (this field's steady-state value between requests, since
   * _task_worker always frees-and-nulls it once consumed), silently
   * dropping the real leftover bytes for this request, and by the time
   * this function got around to the (now too late) assignment, nothing
   * would ever free that already-orphaned buffer -- both a data-loss bug
   * and the exact leak valgrind caught. */
  conn->_carry_over = carry;
  conn->_carry_over_len = leftover_len;

  if (!pool ||
      ctpool_try_submit(pool, _task_worker, conn, NULL) != ccol_success) {
    _mem_free(conn->m_procs, carry);
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    mutex_lock(conn->srv->mutex);
    if (--conn->srv->in_flight_requests == 0)
      cond_var_broadcast(conn->srv->requests_done_cv);
    mutex_unlock(conn->srv->mutex);
    /* The worker pool is at capacity (ctpool_try_submit returns
     * ccol_container_full rather than blocking, matching this module's
     * documented "never block the reactor thread" contract) -- this is a
     * real, if transient, server condition the client should be told about
     * via a synchronous 503, not a bare connection reset (see
     * bounded_pool_full_returns_503 in tests.c). */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_reject_and_close(conn);
    return;
  }
}

static void _conn_reject_and_close(chttpsvr_conn_t *conn) {
  chttpsvr_resp resp;
  memset(&resp, 0, sizeof(resp));
  resp.status_code = conn->reject_status;
  resp.m_procs = conn->m_procs;
  chttp1_stream_t stream;
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0);
  _send_response(&stream, &resp, false, 2000);
  chttp1_stream_release(&stream);
  _destroy_resp(&resp, conn->m_procs);
  _conn_close(conn);
}

/* Drives the reactor-owned portion of one connection: TLS handshake (if
 * any), then header parsing until a request is either rejected (sent
 * synchronously and the connection closed) or diverted to a worker. */
static void _conn_pump(chttpsvr_conn_t *conn) {
  if (conn->state == CONN_ST_TLS_HANDSHAKE) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_ERROR) {
      clog el = _engine_logger_get();
      if (el) log_warn(el, "TLS handshake failed fd=%d", conn->fd);
      _conn_close(conn);
      return;
    }
    if (r == CTLS_HANDSHAKE_WANT_READ || r == CTLS_HANDSHAKE_WANT_WRITE) {
      ccol_select_dir want = (r == CTLS_HANDSHAKE_WANT_WRITE)
                                 ? ccol_select_write
                                 : ccol_select_read;
      if (conn->reg) {
        event_loop_modify(g_reactor, conn->reg, want);
      } else {
        char *err = NULL;
        conn->reg =
            event_loop_add(g_reactor, selectable_from_fd(conn->fd, want),
                           (event_handlers_t){.on_readable = _conn_on_readable,
                                              .on_writable = _conn_on_writable,
                                              .on_error = _conn_on_error},
                           conn, &err);
        if (!conn->reg) {
          _conn_close(conn);
          return;
        }
      }
      /* Re-add to the idle list before returning: this thread is done with
       * conn for now (waiting on the next handshake step's readiness), and
       * a future dispatch must be able to _idle_list_try_claim it back out
       * again. Without this, a mid-handshake connection was never in the
       * idle list at all between steps, invisible to (and therefore never
       * closed by) the idle-timeout sweep or a server destroy's own
       * idle-connection cleanup. */
      _idle_list_add(conn);
      return;
    }
    /* CTLS_HANDSHAKE_DONE */
    conn->state = CONN_ST_READING_HEADERS;
    if (conn->reg && conn->reg != NULL) {
      event_loop_modify(g_reactor, conn->reg, ccol_select_read);
    }
  }

  for (;;) {
    char buf[8192];
    ssize_t n;
    if (conn->tls)
      n = ctls_conn_read(conn->tls, buf, sizeof(buf));
    else
      n = read(conn->fd, buf, sizeof(buf));

    if (n < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        if (!conn->reg) {
          char *err = NULL;
          conn->reg = event_loop_add(
              g_reactor, selectable_from_fd(conn->fd, ccol_select_read),
              (event_handlers_t){.on_readable = _conn_on_readable,
                                 .on_error = _conn_on_error},
              conn, &err);
          if (!conn->reg) {
            _conn_close(conn);
            return;
          }
        }
        _idle_list_add(conn);
        return;
      }
      _conn_close(conn);
      return;
    }
    if (n == 0) {
      _conn_close(conn);
      return;
    }

    clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, buf, (size_t)n);

    if (r == CHTTP1_OK) continue; /* need more header bytes; loop for more */

    if (r == CHTTP1_HEADERS_ONLY || r == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      const char *leftover = buf + consumed;
      size_t leftover_len = (size_t)n - consumed;

      if (conn->expects_continue && !conn->req_rejected) {
        const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
        chttp1_stream_t s;
        if (conn->tls)
          chttp1_stream_prepare_tls(&s, conn->fd, conn->tls, NULL, 0);
        else
          chttp1_stream_prepare(&s, conn->fd, NULL, 0);
        chttp1_stream_write(&s, cont, strlen(cont), 2000);
        chttp1_stream_release(&s);
      }
      _conn_start_diverted(conn, leftover, leftover_len);
      return;
    }

    /* CHTTP1_USER: an unmatched/rejected route, decided by our own
     * _on_headers_complete (conn->req_rejected already set to a specific
     * status). The route itself was identified, so send a graceful
     * synchronous error response (the body, if any, is never read -- the
     * connection is then closed rather than kept alive, since the client's
     * still-arriving body would otherwise be misread as a pipelined
     * request).
     *
     * CHTTP1_ERROR: a syntax-level problem the parser itself rejected before
     * routing ever ran (a malformed request line, a negative Content-Length,
     * chunked not last in a Transfer-Encoding list, a too-long header,
     * ...). This matches every other pre-routing parse error in this
     * parser: an outright connection close with NO response at all, not a
     * graceful error page -- see negative_content_length_rejected and
     * chunked_not_last_in_transfer_encoding_list_rejected in tests.c, which
     * assert exactly that. */
    if (conn->req_rejected) {
      _conn_reject_and_close(conn);
    } else {
      _conn_close(conn);
    }
    return;
  }
}

/* Every reactor-dispatched entry point into a live (already-registered)
 * connection must claim it out of the idle list first -- see
 * _idle_list_try_claim's own doc comment for the use-after-free this
 * prevents (a concurrent idle-timeout/destroy-time closer freeing the same
 * connection, including its TLS state, while a dispatch is using it). A
 * failed claim is not an error: it means a closer already has exclusive
 * ownership (or, harmlessly, that this connection's own _idle_list_add
 * call for the registration that just fired hasn't executed yet -- see
 * that same doc comment for why level-triggered epoll makes this
 * self-healing) -- either way, the correct action is to touch nothing and
 * return. */
static void _conn_on_readable(event_loop loop, ccol_selectable *sel,
                              void *arg) {
  (void)loop;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_pump(conn);
}

static void _conn_on_writable(event_loop loop, ccol_selectable *sel,
                              void *arg) {
  (void)loop;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_pump(conn);
}

static void _conn_on_error(event_loop loop, ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_close(conn);
}

/* ========================================================================== */
/*                    WORKER THREAD: BODY READ + HANDLER + RESPONSE           */
/* ========================================================================== */

static bool _check_read_deadline(chttpsvr_conn_t *conn,
                                 unsigned *timeout_ms_inout) {
  unsigned max_dur = atomic_load(&conn->srv->max_body_read_duration_ms);
  if (!max_dur) return true;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!conn->read_deadline_set) {
    conn->read_deadline = now;
    conn->read_deadline.tv_sec += (time_t)(max_dur / 1000);
    conn->read_deadline.tv_nsec += (long)(max_dur % 1000) * 1000000L;
    if (conn->read_deadline.tv_nsec >= 1000000000L) {
      conn->read_deadline.tv_nsec -= 1000000000L;
      conn->read_deadline.tv_sec += 1;
    }
    conn->read_deadline_set = true;
  }
  long remaining_ms = (long)(conn->read_deadline.tv_sec - now.tv_sec) * 1000 +
                      (conn->read_deadline.tv_nsec - now.tv_nsec) / 1000000L;
  if (remaining_ms <= 0) {
    conn->deadline_exceeded = true;
    return false;
  }
  if (!*timeout_ms_inout || (unsigned)remaining_ms < *timeout_ms_inout)
    *timeout_ms_inout = (unsigned)remaining_ms;
  return true;
}

/* Drains body bytes (for a buffered route, all of them; for a streaming
 * route, the message-complete tail only, since chttpsvr_req_read already
 * pulled the rest) via chttp1_stream_read + chttp1_parser_execute, both
 * driven by the calling worker thread. Returns ccol_success, or a specific
 * failure the caller maps to a status code. */
static ccol_retval_t _drain_body(chttpsvr_conn_t *conn,
                                 chttp1_stream_t *stream) {
  for (;;) {
    if (chttp1_parser_message_complete(&conn->parser)) break;
    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!_check_read_deadline(conn, &timeout_ms)) return ccol_timed_out;
    char raw[8192];
    ssize_t n = chttp1_stream_read(stream, raw, sizeof(raw), (int)timeout_ms);
    if (n < 0) {
      if (chttp1_stream_timed_out(stream)) return ccol_timed_out;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) return ccol_http_transfer_aborted; /* truncated body */
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_PAUSED) break;
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      return conn->body_too_large ? ccol_msg_too_large
                                  : ccol_http_transfer_aborted;
    }
  }
  return ccol_success;
}

ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen) {
  if (!req) return -1;
  if (buflen == 0) return 0;
  if (!buf) return -1;
  chttpsvr_conn_t *conn = req->conn;
  if (!conn->matched_route->is_streaming) return -1;
  if (!req->stream) return -1;

  for (;;) {
    growbuf_t *b = &conn->body;
    if (b->pos < b->len) {
      size_t avail = b->len - b->pos;
      size_t take = avail < buflen ? avail : buflen;
      memcpy(buf, b->buf + b->pos, take);
      b->pos += take;
      return (ssize_t)take;
    }
    if (chttp1_parser_message_complete(&conn->parser)) return 0;

    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!_check_read_deadline(conn, &timeout_ms)) return -1;
    char raw[8192];
    ssize_t n =
        chttp1_stream_read(req->stream, raw, sizeof(raw), (int)timeout_ms);
    if (n < 0) {
      /* Either stream_read_timeout_ms's own per-call poll(2) timed out, or
       * max_body_read_duration_ms's deadline was folded into this call's
       * timeout by _check_read_deadline above and expired mid-poll instead
       * of being caught by that function's own up-front check on the NEXT
       * call (which never happens, since chttpsvr_req_read returns here
       * immediately). Either way this is a real timeout, not a hard I/O
       * error -- report it the same way _drain_body already does for the
       * buffered-route path, via the same conn->deadline_exceeded flag
       * chttpsvr_req_stream_error() reads, so both caps collapse to the
       * identical ccol_timed_out result (see max_body_read_duration_exceeded_
       * reports_ccol_timed_out / stream_read_timeout_reports_ccol_timed_out
       * in tests.c). */
      if (chttp1_stream_timed_out(req->stream)) conn->deadline_exceeded = true;
      return -1;
    }
    if (n == 0) return -1; /* truncated body: framing wasn't done yet */
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) return -1;
    /* CHTTP1_OK or CHTTP1_PAUSED: loop back to drain whatever _on_body just
     * appended to conn->body. */
  }
}

ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req) {
  if (!req || !req->conn) return ccol_unexpected_failure;
  chttpsvr_conn_t *conn = req->conn;
  if (conn->deadline_exceeded) return ccol_timed_out;
  if (conn->body_too_large) return ccol_msg_too_large;
  return ccol_success;
}

static void _task_worker(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;

  chttp1_stream_t stream;
  bool prepared;
  if (conn->tls)
    prepared = chttp1_stream_prepare_tls(
        &stream, conn->fd, conn->tls, conn->_carry_over, conn->_carry_over_len);
  else
    prepared = chttp1_stream_prepare(&stream, conn->fd, conn->_carry_over,
                                     conn->_carry_over_len);
  _mem_free(conn->m_procs, conn->_carry_over);
  conn->_carry_over = NULL;
  conn->_carry_over_len = 0;

  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = prepared ? &stream : NULL;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval_t body_err = ccol_success;
  if (prepared) {
    if (!conn->matched_route->is_streaming) {
      body_err = _drain_body(conn, &stream);
    }
    /* Streaming routes: the handler pulls the body itself via
     * chttpsvr_req_read(); nothing to pre-drain here. */
  } else {
    body_err = ccol_not_enough_memory;
  }

  bool aborted = false;
  if (body_err != ccol_success && !conn->matched_route->is_streaming) {
    conn->resp.status_code =
        (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
        : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
                                         : CHTTP_STATUS_INTERNAL_ERROR;
    aborted = true;
  } else {
    _chttpsvr_next(&req, &conn->resp);
    /* A streaming handler may have stopped reading before the body's own
     * natural end (or hit its own error via chttpsvr_req_read returning
     * -1); either way, nothing further to drain -- the connection's fate
     * (keep-alive vs close) below already accounts for a not-fully-drained
     * body via chttp1_should_keep_alive's message-completion check. */
  }

  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large;

  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  bool sent = prepared && _send_response(&stream, &conn->resp, keep_alive,
                                         write_timeout_ms);
  if (!sent) keep_alive = false;

  if (prepared) chttp1_stream_release(&stream);
  _destroy_req_qparams(&req);

  mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0) cond_var_broadcast(srv->requests_done_cv);
  mutex_unlock(srv->mutex);

  if (!keep_alive) {
    _conn_close(conn);
    return;
  }

  /* Keep-alive: reset per-request state and hand the connection back to the
   * reactor to read the next request's headers. */
  _conn_reset_for_request(conn);
  conn->state = CONN_ST_READING_HEADERS;
  char *err = NULL;
  conn->reg =
      event_loop_add(g_reactor, selectable_from_fd(conn->fd, ccol_select_read),
                     (event_handlers_t){.on_readable = _conn_on_readable,
                                        .on_error = _conn_on_error},
                     conn, &err);
  if (!conn->reg) {
    _conn_close(conn);
    return;
  }
  _idle_list_add(conn);
}

/* ========================================================================== */
/*                    LISTENER: ACCEPT + SOCKET OPTIONS                       */
/* ========================================================================== */

static void _apply_accepted_socket_options(int fd, bool is_unix,
                                           bool enable_keepalive) {
  int one = 1;
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
  if (is_unix) return;
  setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
  if (enable_keepalive)
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  int bufsz = 131072;
  int cur = 0;
  socklen_t cur_len = sizeof(cur);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cur, &cur_len) != 0 || cur < bufsz)
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  cur_len = sizeof(cur);
  if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cur, &cur_len) != 0 || cur < bufsz)
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
}

static void _listener_on_readable(event_loop loop, ccol_selectable *sel,
                                  void *arg) {
  (void)loop;
  (void)sel;
  struct chttpserver *srv = (struct chttpserver *)arg;

  for (;;) {
    size_t cap = atomic_load(&srv->max_connections);
    if (cap && atomic_load(&srv->current_connections) >= cap) {
      /* At capacity: leave the pending connection in the kernel backlog.
       * The listener fd remains level-triggered-ready as long as the
       * backlog is non-empty, so this is naturally retried on a later
       * epoll_wait batch once a slot frees up -- no extra bookkeeping
       * needed to "wake up" again. */
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int cfd = accept(srv->listen_fd, (struct sockaddr *)&ss, &slen);
    if (cfd < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) return;
      if (errno == EINTR) continue;
      return;
    }
    atomic_fetch_add(&srv->current_connections, 1);
    _apply_accepted_socket_options(cfd, srv->is_unix_socket,
                                   srv->enable_keepalive);

    call_once(g_parser_settings_once, _init_parser_settings);
    chttpsvr_conn_t *conn = _conn_create(srv, cfd, &g_parser_settings);
    if (!conn) {
      close(cfd);
      atomic_fetch_sub(&srv->current_connections, 1);
      continue;
    }

    if (srv->tls_ctx) {
      conn->tls = ctls_conn_create_server(srv->tls_ctx, cfd, conn, NULL);
      if (!conn->tls) {
        close(cfd);
        atomic_fetch_sub(&srv->current_connections, 1);
        _mem_free(srv->m_procs, conn);
        continue;
      }
      conn->state = CONN_ST_TLS_HANDSHAKE;
    } else {
      conn->state = CONN_ST_READING_HEADERS;
    }
    _conn_pump(conn);
  }
}

/* ========================================================================== */
/*                    LISTEN SOCKET SETUP (TCP + Unix)                        */
/* ========================================================================== */

#define _CHTTPSVR_UNIX_PREFIX "unix://"

static int _make_listen_socket(const char *host, uint16_t port,
                               bool enable_reuseport, bool ipv6_only,
                               bool *is_unix, char **unix_path_out) {
  *is_unix = host && strncmp(host, _CHTTPSVR_UNIX_PREFIX,
                             strlen(_CHTTPSVR_UNIX_PREFIX)) == 0;

  if (*is_unix) {
    const char *path = host + strlen(_CHTTPSVR_UNIX_PREFIX);
    if (!*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
      return -1;
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    unlink(path); /* best-effort: remove a stale socket file at this path */
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
      close(fd);
      return -1;
    }
    if (listen(fd, SOMAXCONN) != 0) {
      close(fd);
      unlink(path);
      return -1;
    }
    *unix_path_out = strdup(path);
    return fd;
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);
  const char *bind_host = (!host || strcmp(host, "0.0.0.0") == 0) ? NULL : host;
  if (getaddrinfo(bind_host, port_str, &hints, &res) != 0 || !res) return -1;

  int fd = -1;
  for (struct addrinfo *rp = res; rp; rp = rp->ai_next) {
    fd =
        socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK, rp->ai_protocol);
    if (fd < 0) continue;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
    if (enable_reuseport)
      setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#endif
#ifdef IPV6_V6ONLY
    if (ipv6_only && rp->ai_family == AF_INET6)
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &one, sizeof(one));
#endif
#ifdef TCP_FASTOPEN
    int qlen = 128;
    setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
#endif
    if (bind(fd, rp->ai_addr, rp->ai_addrlen) == 0) break;
    close(fd);
    fd = -1;
  }
  freeaddrinfo(res);
  if (fd < 0) return -1;
  if (listen(fd, SOMAXCONN) != 0) {
    close(fd);
    return -1;
  }
  *unix_path_out = NULL;
  return fd;
}

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

static chttpsvr_router *_create_router(struct chttpserver *srv,
                                       const char *prefix,
                                       ccol_memmgmt_procs_t *mp) {
  chttpsvr_router *r =
      (chttpsvr_router *)_mem_calloc(mp, 1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    _mem_free(mp, r);
    return NULL;
  }
  size_t plen = strlen(r->prefix);
  while (plen > 1 && r->prefix[plen - 1] == '/') r->prefix[--plen] = '\0';
  r->prefix_len = plen;
  r->srv = srv;
  r->m_procs = mp;
  return r;
}

static void _destroy_router(chttpsvr_router *r, ccol_memmgmt_procs_t *mp) {
  chttpsvr_mw_node_t *mw = r->mw_head;
  while (mw) {
    chttpsvr_mw_node_t *next = mw->next;
    _mem_free(mp, mw);
    mw = next;
  }
  for (size_t i = 0; i < r->route_count; i++) {
    _free_route_data(r->routes[i], mp);
    _mem_free(mp, r->routes[i]);
  }
  _mem_free(mp, r->routes);
  _mem_free(mp, r->prefix);
  _mem_free(mp, r);
}

static ccol_retval_t _router_add_route(chttpsvr_router *router,
                                       chttp_method_t method,
                                       const char *pattern,
                                       chttpsvr_handler_fn fn, void *ctx,
                                       bool is_streaming) {
  if (!router || !pattern || !fn) return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = router->m_procs;

  chttpsvr_route_t *rt =
      (chttpsvr_route_t *)_mem_alloc(mp, sizeof(chttpsvr_route_t));
  if (!rt) return ccol_not_enough_memory;
  memset(rt, 0, sizeof(*rt));
  rt->method = method;
  rt->fn = fn;
  rt->ctx = ctx;
  rt->is_streaming = is_streaming;

  ccol_retval_t rv = _compile_pattern(pattern, &rt->segs, &rt->seg_count,
                                      &rt->param_names, &rt->param_count, mp);
  if (rv != ccol_success) {
    _mem_free(mp, rt);
    return rv;
  }

  rw_lock_wrlock(router->srv->routes_lock);
  if (router->route_count >= router->route_cap) {
    size_t new_cap = router->route_cap * 2 + 4;
    chttpsvr_route_t **nr = (chttpsvr_route_t **)_mem_realloc(
        mp, router->routes, new_cap * sizeof(chttpsvr_route_t *));
    if (!nr) {
      rw_lock_unlock(router->srv->routes_lock);
      _free_route_data(rt, mp);
      _mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    router->routes = nr;
    router->route_cap = new_cap;
  }
  router->routes[router->route_count++] = rt;
  rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

static ccol_retval_t _router_add_mw(chttpsvr_router *router,
                                    chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  ccol_memmgmt_procs_t *mp = router->m_procs;
  chttpsvr_mw_node_t *node =
      (chttpsvr_mw_node_t *)_mem_alloc(mp, sizeof(chttpsvr_mw_node_t));
  if (!node) return ccol_not_enough_memory;
  node->fn = fn;
  node->ctx = ctx;
  node->next = NULL;

  rw_lock_wrlock(router->srv->routes_lock);
  if (router->mw_count >= _CHTTPSVR_MAX_MW) {
    rw_lock_unlock(router->srv->routes_lock);
    _mem_free(mp, node);
    return ccol_not_permitted;
  }
  if (!router->mw_tail) {
    router->mw_head = router->mw_tail = node;
  } else {
    router->mw_tail->next = node;
    router->mw_tail = node;
  }
  router->mw_count++;
  rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

/* ========================================================================== */
/*                         QUERY PARAMETER PARSING                            */
/* ========================================================================== */

static ccol_retval_t _parse_qparams(const char *raw_query,
                                    chttpsvr_qparams_t **qp_out,
                                    ccol_memmgmt_procs_t *mp) {
  chttpsvr_qparams_t *qp =
      (chttpsvr_qparams_t *)_mem_calloc(mp, 1, sizeof(chttpsvr_qparams_t));
  if (!qp) return ccol_not_enough_memory;
  qp->m_procs = mp;

  const char *p = raw_query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
    const char *eq = (const char *)memchr(p, '=', pair_len);

    size_t key_raw_len = eq ? (size_t)(eq - p) : pair_len;
    const char *val_raw = eq ? eq + 1 : NULL;
    size_t val_raw_len = eq ? pair_len - key_raw_len - 1 : 0;

    char key_src[512];
    char *key_sp = key_src;
    bool kh = false;
    if (key_raw_len + 1 > sizeof(key_src)) {
      key_sp = (char *)_mem_alloc(mp, key_raw_len + 1);
      kh = true;
      if (!key_sp) goto oom;
    }
    memcpy(key_sp, p, key_raw_len);
    key_sp[key_raw_len] = '\0';
    char *key_decoded = (char *)_mem_alloc(mp, key_raw_len + 1);
    if (!key_decoded) {
      if (kh) _mem_free(mp, key_sp);
      goto oom;
    }
    ssize_t kl = _decode_url_unsafe(key_decoded, key_sp);
    if (kh) _mem_free(mp, key_sp);
    if (kl < 0) {
      _mem_free(mp, key_decoded);
      goto next;
    }
    key_decoded[(size_t)kl] = '\0';

    char *val_decoded = NULL;
    if (val_raw) {
      char val_src[512];
      char *val_sp = val_src;
      bool vh = false;
      if (val_raw_len + 1 > sizeof(val_src)) {
        val_sp = (char *)_mem_alloc(mp, val_raw_len + 1);
        vh = true;
        if (!val_sp) {
          _mem_free(mp, key_decoded);
          goto oom;
        }
      }
      memcpy(val_sp, val_raw, val_raw_len);
      val_sp[val_raw_len] = '\0';
      val_decoded = (char *)_mem_alloc(mp, val_raw_len + 1);
      if (!val_decoded) {
        if (vh) _mem_free(mp, val_sp);
        _mem_free(mp, key_decoded);
        goto oom;
      }
      ssize_t vl = _decode_url_unsafe(val_decoded, val_sp);
      if (vh) _mem_free(mp, val_sp);
      if (vl < 0) {
        _mem_free(mp, val_decoded);
        _mem_free(mp, key_decoded);
        goto next;
      }
      val_decoded[(size_t)vl] = '\0';
    } else {
      val_decoded = ccol_strdup(mp, "");
      if (!val_decoded) {
        _mem_free(mp, key_decoded);
        goto oom;
      }
    }

    if (qp->count >= qp->cap) {
      size_t nc = qp->cap * 2 + 8;
      char **nk = (char **)_mem_realloc(mp, qp->keys, nc * sizeof(char *));
      if (!nk) {
        _mem_free(mp, key_decoded);
        _mem_free(mp, val_decoded);
        goto oom;
      }
      qp->keys = nk;
      char **nv = (char **)_mem_realloc(mp, qp->values, nc * sizeof(char *));
      if (!nv) {
        _mem_free(mp, key_decoded);
        _mem_free(mp, val_decoded);
        goto oom;
      }
      qp->values = nv;
      qp->cap = nc;
    }
    qp->keys[qp->count] = key_decoded;
    qp->values[qp->count] = val_decoded;
    qp->count++;

  next:
    p = amp ? amp + 1 : NULL;
  }

  *qp_out = qp;
  return ccol_success;

oom:
  for (size_t i = 0; i < qp->count; i++) {
    _mem_free(mp, qp->keys[i]);
    _mem_free(mp, qp->values[i]);
  }
  _mem_free(mp, qp->keys);
  _mem_free(mp, qp->values);
  _mem_free(mp, qp);
  return ccol_not_enough_memory;
}

static chttpsvr_qparams_t *_ensure_qparams(chttpsvr_req *req) {
  chttpsvr_conn_t *conn = req->conn;
  if (req->_qparams) return req->_qparams;
  if (req->_qparams_attempted) return NULL;
  req->_qparams_attempted = true;

  if (!conn->raw_query || !*conn->raw_query) {
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams_t));
    if (!qp) return NULL;
    qp->m_procs = req->m_procs;
    req->_qparams = qp;
    return qp;
  }
  if (_parse_qparams(conn->raw_query, &req->_qparams, req->m_procs) !=
      ccol_success) {
    req->_qparams_parse_oom = true;
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams_t));
    if (qp) {
      qp->m_procs = req->m_procs;
      req->_qparams = qp;
      return qp;
    }
    return NULL;
  }
  return req->_qparams;
}

/* ========================================================================== */
/*                         PUBLIC API IMPLEMENTATION                          */
/* ========================================================================== */

chttpsvr create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str) {
  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return NULL;

  struct chttpserver *srv =
      (struct chttpserver *)_mem_calloc(mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs_t *)_mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs_t));
    if (!srv->m_procs) {
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("out of memory");
      return NULL;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  clog logger = cl ? clog_derive(cl) : clog_open_fd_mp(2, CLOG_FATAL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("failed to create server logger");
    return NULL;
  }

  if (mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return NULL;
  }
  if (mutex_init(srv->idle_mutex) != 0) {
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return NULL;
  }

  {
    /* CLOCK_MONOTONIC to match _wait_and_detach_worker_pool's own
     * clock_gettime(CLOCK_MONOTONIC, ...)-based deadline -- cond_var_init's
     * default clock (CLOCK_REALTIME) would make that deadline comparison
     * wrong (comparing a monotonic-clock timespec against a condvar
     * internally using the wall clock). */
    cond_var_attr_t cv_attr;
    int cv_rc = 0;
    if (cond_var_attr_init(cv_attr) == 0) {
      cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
      cv_rc = cond_var_init_ca(srv->requests_done_cv, cv_attr);
      cond_var_attr_destroy(cv_attr);
    } else {
      cv_rc = 1;
    }
    if (cv_rc != 0) {
      mutex_destroy(srv->idle_mutex);
      mutex_destroy(srv->mutex);
      clog_close(logger);
      _mem_free(mprocs, srv->m_procs);
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("cond_var_init failed");
      return NULL;
    }
  }

  if (rw_lock_init(srv->routes_lock) != 0) {
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("rw_lock_init failed");
    return NULL;
  }

  chttpsvr_router *root = _create_router(srv, "", srv->m_procs);
  if (!root) {
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }

  srv->routers =
      (chttpsvr_router **)_mem_alloc(srv->m_procs, sizeof(chttpsvr_router *));
  if (!srv->routers) {
    _destroy_router(root, srv->m_procs);
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->requests_done_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return NULL;
  }
  srv->routers[0] = root;
  srv->router_count = 1;
  srv->router_cap = 1;
  srv->cl = logger;
  srv->listen_fd = -1;
  srv->started = false;
  return srv;
}

static ctpool _wait_and_detach_worker_pool(chttpsvr srv) {
  mutex_lock(srv->mutex);
  if (srv->in_flight_requests > 0) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 30;
    while (srv->in_flight_requests > 0) {
      if (cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
          ETIMEDOUT)
        break;
    }
  }
  ctpool old_pool = srv->worker_pool;
  srv->worker_pool = NULL;
  mutex_unlock(srv->mutex);
  return old_pool;
}

/* __chttpsvr_destroy's own variant of the wait above: additionally closes
 * every still-idle connection, atomically with respect to
 * _conn_start_diverted (the only place that increments in_flight_requests
 * and reads worker_pool), by holding srv->mutex continuously from the
 * moment in-flight work is observed to have drained all the way through
 * the idle-close pass. Without this, a keep-alive connection whose request
 * finished (landing back in the idle list) right as the in-flight wait
 * below completed, but which then received a further pipelined request
 * before an separately-locked idle-close pass got to it, could be silently
 * re-diverted -- escaping that pass entirely and leaking once this
 * function goes on to release the engine/reactor out from under it (a
 * real, if rare, leak valgrind caught: one orphaned chttpsvr_conn_t after
 * a full 176-case run). Any request that still manages to arrive after
 * this function has already closed a connection, or while it holds
 * srv->mutex, either hits an already-removed event_loop registration (a
 * safe no-op, the same dispatch-time liveness check this codebase already
 * relies on elsewhere) or observes worker_pool == NULL once it finally
 * acquires the lock and gets the ordinary graceful 503 _conn_start_diverted
 * already sends for "no pool available" -- never a leak or a crash. */
static ctpool _drain_and_close_all_connections(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  if (srv->in_flight_requests > 0) {
    struct timespec deadline;
    clock_gettime(CLOCK_MONOTONIC, &deadline);
    deadline.tv_sec += 30;
    while (srv->in_flight_requests > 0) {
      if (cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
          ETIMEDOUT)
        break;
    }
  }
  ctpool old_pool = srv->worker_pool;
  srv->worker_pool = NULL;
  _close_all_idle_connections(srv);
  mutex_unlock(srv->mutex);
  return old_pool;
}

void __chttpsvr_destroy(chttpsvr srv) {
  if (!srv) return;

  chttpsvr_stop(srv);
  _servers_unregister(srv);

  /* Drain in-flight work and close every remaining idle connection BEFORE
   * releasing this server's engine reference below: _engine_release() can
   * be the one that drops the shared reactor's ref count to zero (if srv
   * happens to be the last contributing server), which hands
   * event_loop_destroy(g_reactor) off to an async reaper thread -- racing
   * this function's own event_loop_remove() calls (inside
   * _drain_and_close_all_connections -> _conn_close) if they ran after
   * releasing instead of before. */
  ctpool old_pool = _drain_and_close_all_connections(srv);

  bool should_release_engine = false;
  mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
  }
  mutex_unlock(srv->mutex);
  if (should_release_engine) _engine_release();

  if (old_pool) {
    ctpool_shutdown_drain(old_pool);
    ctpool_destroy(old_pool);
  }

  if (srv->tls_ctx) {
    ctls_ctx_release(srv->tls_ctx);
    srv->tls_ctx = NULL;
  }

  for (size_t i = 0; i < srv->router_count; i++) {
    _destroy_router(srv->routers[i], srv->m_procs);
  }
  _mem_free(srv->m_procs, srv->routers);
  mutex_destroy(srv->mutex);
  mutex_destroy(srv->idle_mutex);
  cond_var_destroy(srv->requests_done_cv);
  rw_lock_destroy(srv->routes_lock);

  clog_close(srv->cl);
  srv->cl = NULL;

  ccol_memmgmt_procs_t *mp = srv->m_procs;
  _mem_free(mp, srv);
  _mem_free(mp, mp);
}

ccol_retval_t chttpsvr_start(chttpsvr srv, const chttpsvr_config_t *cfg) {
  if (!srv) return ccol_invalid_args;
  if (cfg && cfg->port == 0 &&
      !(cfg->host && strncmp(cfg->host, _CHTTPSVR_UNIX_PREFIX,
                             strlen(_CHTTPSVR_UNIX_PREFIX)) == 0))
    return ccol_invalid_args;

  chttpsvr_config_t default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  mutex_lock(srv->mutex);
  if (srv->started) {
    mutex_unlock(srv->mutex);
    return ccol_not_permitted;
  }
  mutex_unlock(srv->mutex);

  {
    ctpool leftover_pool = _wait_and_detach_worker_pool(srv);
    if (leftover_pool) {
      ctpool_shutdown_drain(leftover_pool);
      ctpool_destroy(leftover_pool);
    }
  }

  int nthreads = cfg->worker_thread_count;
  if (nthreads <= 0) {
    long cpus = sysconf(_SC_NPROCESSORS_ONLN);
    nthreads = (cpus > 0) ? (int)cpus : 1;
  }
  size_t queue_cap;
  if (cfg->worker_queue_capacity == CHTTPSVR_QUEUE_UNBOUNDED) {
    queue_cap = 0;
  } else if (cfg->worker_queue_capacity == 0) {
    queue_cap = (size_t)1024 * (size_t)nthreads;
  } else {
    queue_cap = cfg->worker_queue_capacity;
  }

  char *pool_err = NULL;
  ctpool new_pool = create_cthread_pool_mp((size_t)nthreads, queue_cap,
                                           srv->m_procs, &pool_err);
  if (!new_pool) return ccol_not_enough_memory;
  mutex_lock(srv->mutex);
  srv->worker_pool = new_pool;
  mutex_unlock(srv->mutex);

  atomic_store(&srv->stream_read_timeout_ms, cfg->stream_read_timeout_ms);
  atomic_store(&srv->max_body_read_duration_ms, cfg->max_body_read_duration_ms);
  atomic_store(&srv->response_write_timeout_ms,
               cfg->response_write_timeout_ms ? cfg->response_write_timeout_ms
                                              : cfg->stream_read_timeout_ms);
  {
    long eff_idle_ms = (cfg->idle_timeout_ms > 0) ? cfg->idle_timeout_ms
                                                  : cfg->read_timeout_ms;
    atomic_store(&srv->idle_timeout_ms,
                 (unsigned)(eff_idle_ms > 0 ? eff_idle_ms : 0));
  }
  srv->max_body_size = cfg->max_body_size;
  srv->max_header_bytes = cfg->max_header_bytes;
  atomic_store(&srv->max_connections, cfg->max_connections);
  srv->enable_keepalive = cfg->enable_keepalive;

  if (srv->tls_ctx) {
    ctls_ctx_release(srv->tls_ctx);
    srv->tls_ctx = NULL;
  }
  if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
    ctls_ctx_t *tls_ctx = ctls_ctx_new_mp(srv->m_procs, NULL);
    if (!tls_ctx) {
      ctpool failed_pool = _wait_and_detach_worker_pool(srv);
      if (failed_pool) {
        ctpool_shutdown_drain(failed_pool);
        ctpool_destroy(failed_pool);
      }
      return ccol_not_enough_memory;
    }
    if (ctls_ctx_cert_add(tls_ctx, NULL, cfg->tls->cert_path,
                          cfg->tls->key_path, NULL, NULL) != ccol_success) {
      ctls_ctx_release(tls_ctx);
      ctpool failed_pool = _wait_and_detach_worker_pool(srv);
      if (failed_pool) {
        ctpool_shutdown_drain(failed_pool);
        ctpool_destroy(failed_pool);
      }
      return ccol_unexpected_failure;
    }
    if (cfg->tls->ca_bundle_path)
      ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL);
    srv->tls_ctx = tls_ctx;
  }

  bool need_acquire;
  mutex_lock(srv->mutex);
  need_acquire = !srv->contributed_to_engine;
  if (need_acquire) srv->contributed_to_engine = true;
  mutex_unlock(srv->mutex);

  if (need_acquire) {
    ccol_retval_t engine_rc = _engine_acquire();
    if (engine_rc != ccol_success) {
      mutex_lock(srv->mutex);
      srv->contributed_to_engine = false;
      mutex_unlock(srv->mutex);
      if (srv->tls_ctx) {
        ctls_ctx_release(srv->tls_ctx);
        srv->tls_ctx = NULL;
      }
      ctpool failed_pool = _wait_and_detach_worker_pool(srv);
      if (failed_pool) {
        ctpool_shutdown_drain(failed_pool);
        ctpool_destroy(failed_pool);
      }
      return engine_rc;
    }
  }
  _idle_sweep_start_if_needed();

  bool is_unix = false;
  char *unix_path = NULL;
  int lfd = _make_listen_socket(cfg->host, cfg->port, cfg->enable_reuseport,
                                cfg->ipv6_only, &is_unix, &unix_path);
  if (lfd < 0) {
    clog el = _engine_logger_get();
    if (el)
      log_error(el, "listen socket setup failed host=%s port=%u",
                cfg->host ? cfg->host : "0.0.0.0", (unsigned)cfg->port);
    if (srv->tls_ctx) {
      ctls_ctx_release(srv->tls_ctx);
      srv->tls_ctx = NULL;
    }
    ctpool failed_pool = _wait_and_detach_worker_pool(srv);
    if (failed_pool) {
      ctpool_shutdown_drain(failed_pool);
      ctpool_destroy(failed_pool);
    }
    return ccol_unexpected_failure;
  }

  call_once(g_parser_settings_once, _init_parser_settings);
  char *reg_err = NULL;
  event_reg *lreg = event_loop_add(
      g_reactor, selectable_from_fd(lfd, ccol_select_read),
      (event_handlers_t){.on_readable = _listener_on_readable}, srv, &reg_err);
  if (!lreg) {
    close(lfd);
    if (is_unix) unlink(unix_path);
    free(unix_path);
    if (srv->tls_ctx) {
      ctls_ctx_release(srv->tls_ctx);
      srv->tls_ctx = NULL;
    }
    ctpool failed_pool = _wait_and_detach_worker_pool(srv);
    if (failed_pool) {
      ctpool_shutdown_drain(failed_pool);
      ctpool_destroy(failed_pool);
    }
    return ccol_unexpected_failure;
  }

  mutex_lock(srv->mutex);
  srv->listen_fd = lfd;
  srv->is_unix_socket = is_unix;
  srv->unix_socket_path = unix_path;
  srv->listen_reg = lreg;
  srv->started = true;
  mutex_unlock(srv->mutex);

  _servers_register(srv);
  return ccol_success;
}

void chttpsvr_stop(chttpsvr srv) {
  if (!srv) return;
  mutex_lock(srv->mutex);
  bool was_started = srv->started;
  int lfd = srv->listen_fd;
  event_reg *lreg = srv->listen_reg;
  bool is_unix = srv->is_unix_socket;
  char *unix_path = srv->unix_socket_path;
  if (was_started) {
    srv->started = false;
    srv->listen_fd = -1;
    srv->listen_reg = NULL;
    srv->unix_socket_path = NULL;
  }
  mutex_unlock(srv->mutex);
  if (!was_started) return;

  if (lreg) event_loop_remove(g_reactor, lreg);
  close(lfd);
  if (is_unix && unix_path) unlink(unix_path);
  free(unix_path);
}

ccol_retval_t chttpsvr_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-engine");
  call_once(g_engine_once, _engine_globals_init);
  mutex_lock(g_engine_mutex);
  clog old = g_engine_logger;
  g_engine_logger = derived;
  mutex_unlock(g_engine_mutex);
  if (old) clog_close(old);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  call_once(g_engine_once, _engine_globals_init);
  mutex_lock(g_engine_mutex);
  if (g_reactor) {
    mutex_unlock(g_engine_mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    g_engine_mp_storage = *mp;
    g_engine_mp = &g_engine_mp_storage;
  } else {
    g_engine_mp = NULL;
  }
  mutex_unlock(g_engine_mutex);
  return ccol_success;
}

void chttpsvr_engine_stop(void) { _engine_force_stop(); }

void chttpsvr_engine_wait(void) { _engine_wait_until_stopped(); }

ccol_retval_t chttpsvr_register_handler(chttpsvr srv, chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!srv || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(srv->routers[0], method, pattern, fn, ctx, false);
}

ccol_retval_t chttpsvr_register_streaming_handler(chttpsvr srv,
                                                  chttp_method_t method,
                                                  const char *pattern,
                                                  chttpsvr_handler_fn fn,
                                                  void *ctx) {
  if (!srv || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(srv->routers[0], method, pattern, fn, ctx, true);
}

ccol_retval_t chttpsvr_use(chttpsvr srv, chttpsvr_middleware_fn fn, void *ctx) {
  if (!srv || !fn) return ccol_invalid_args;
  return _router_add_mw(srv->routers[0], fn, ctx);
}

chttpsvr_router *chttpsvr_subrouter(chttpsvr srv, const char *prefix) {
  if (!srv || !prefix || prefix[0] != '/') return NULL;
  if (strstr(prefix, "//")) return NULL;

  chttpsvr_router *r = _create_router(srv, prefix, srv->m_procs);
  if (!r) return NULL;

  rw_lock_wrlock(srv->routes_lock);
  if (srv->router_count >= srv->router_cap) {
    size_t nc = srv->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)_mem_realloc(
        srv->m_procs, srv->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      rw_lock_unlock(srv->routes_lock);
      _destroy_router(r, srv->m_procs);
      return NULL;
    }
    srv->routers = nr;
    srv->router_cap = nc;
  }
  srv->routers[srv->router_count++] = r;
  rw_lock_unlock(srv->routes_lock);
  return r;
}

ccol_retval_t chttpsvr_router_on(chttpsvr_router *router, chttp_method_t method,
                                 const char *pattern, chttpsvr_handler_fn fn,
                                 void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(router, method, pattern, fn, ctx, false);
}

ccol_retval_t chttpsvr_router_on_stream(chttpsvr_router *router,
                                        chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  return _router_add_route(router, method, pattern, fn, ctx, true);
}

ccol_retval_t chttpsvr_router_use(chttpsvr_router *router,
                                  chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  return _router_add_mw(router, fn, ctx);
}

/* ========================================================================== */
/*                         REQUEST API                                        */
/* ========================================================================== */

chttp_method_t chttpsvr_req_method(const chttpsvr_req *req) {
  if (!req) return CHTTP_GET;
  return req->conn->method;
}

const char *chttpsvr_req_path(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->conn->decoded_path;
}

const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn_t *conn = req->conn;
  /* A repeated header name keeps every occurrence, in arrival order, in
   * conn->hdr_names/hdr_values -- scan backward so the LAST occurrence wins,
   * matching this module's pre-existing documented behavior for a
   * duplicated header (facio's own FIOBJ_T_ARRAY handling picked the last
   * value too; see streaming_repeated_header in tests.c). */
  for (size_t i = conn->hdr_count; i-- > 0;) {
    if (strcasecmp(conn->hdr_names[i], name) == 0) return conn->hdr_values[i];
  }
  return NULL;
}

const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out) {
  if (!req) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = req->conn->body.len;
  return req->conn->body.buf;
}

const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn_t *conn = req->conn;
  int count = conn->matched_route->param_count;
  for (int i = 0; i < count; i++) {
    if (req->param_names[i] && strcmp(req->param_names[i], name) == 0) {
      return conn->matched_param_values[i];
    }
  }
  return NULL;
}

const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out) {
  if (!req || !key) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  chttpsvr_qparams_t *qp = _ensure_qparams(req);
  if (!qp) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  size_t n = 0;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) n++;
  }
  if (n == 0) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  if (n + 1 > req->_qresult_cap) {
    const char **nr = (const char **)_mem_realloc(req->m_procs, req->_qresult,
                                                  (n + 1) * sizeof(char *));
    if (!nr) {
      req->_qresult_oom = true;
      if (count_out) *count_out = 0;
      return NULL;
    }
    req->_qresult = nr;
    req->_qresult_cap = n + 1;
  }
  size_t idx = 0;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) req->_qresult[idx++] = qp->values[i];
  }
  req->_qresult[idx] = NULL;
  if (count_out) *count_out = n;
  return req->_qresult;
}

ccol_retval_t chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                     const char **val_out) {
  if (!req || !key) return ccol_invalid_args;

  chttpsvr_qparams_t *qp = _ensure_qparams(req);
  if (!qp) return ccol_not_enough_memory;
  if (req->_qparams_parse_oom) return ccol_not_enough_memory;

  size_t n = 0;
  const char *found_val = NULL;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) {
      n++;
      found_val = qp->values[i];
    }
  }
  if (n == 0) {
    if (val_out) *val_out = NULL;
    return ccol_key_not_found;
  }
  if (n > 1) {
    if (val_out) *val_out = NULL;
    return ccol_not_permitted;
  }
  if (val_out) *val_out = found_val;
  return ccol_success;
}

const char *chttpsvr_req_raw_query(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->conn->raw_query;
}

bool chttpsvr_req_query_oom(const chttpsvr_req *req) {
  if (!req) return false;
  return req->_qresult_oom || req->_qparams_parse_oom;
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code) {
  if (resp) resp->status_code = status_code;
}

ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_memmgmt_procs_t *mp = resp->m_procs;

  for (size_t i = 0; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, name) == 0) {
      char *new_val = ccol_strdup(mp, value);
      if (!new_val) return ccol_not_enough_memory;
      _mem_free(mp, resp->headers[i].value);
      resp->headers[i].value = new_val;
      return ccol_success;
    }
  }

  if (resp->header_count >= resp->header_cap) {
    if (resp->header_cap > (SIZE_MAX - 8) / 2) return ccol_not_enough_memory;
    size_t new_cap = resp->header_cap * 2 + 8;
    if (new_cap > SIZE_MAX / sizeof(resp_header_t))
      return ccol_not_enough_memory;
    resp_header_t *nh = (resp_header_t *)_mem_realloc(
        mp, resp->headers, new_cap * sizeof(resp_header_t));
    if (!nh) return ccol_not_enough_memory;
    resp->headers = nh;
    resp->header_cap = new_cap;
  }

  char *n_name = ccol_strdup(mp, name);
  char *n_val = ccol_strdup(mp, value);
  if (!n_name || !n_val) {
    _mem_free(mp, n_name);
    _mem_free(mp, n_val);
    return ccol_not_enough_memory;
  }
  resp->headers[resp->header_count].name = n_name;
  resp->headers[resp->header_count].value = n_val;
  resp->header_count++;
  return ccol_success;
}

ccol_retval_t chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                  size_t len) {
  if (!resp || (!data && len > 0)) return ccol_invalid_args;
  if (len == 0) return ccol_success;

  ccol_memmgmt_procs_t *mp = resp->m_procs;

  if (len > resp->body_cap - resp->body_len) {
    if (len > SIZE_MAX - resp->body_len) return ccol_not_enough_memory;
    size_t min_cap = resp->body_len + len;
    size_t new_cap = min_cap;
    if (resp->body_cap <= (SIZE_MAX - 256) / 2) {
      size_t doubled = resp->body_cap * 2 + 256;
      if (doubled > new_cap) new_cap = doubled;
    }
    char *nb = (char *)_mem_realloc(mp, resp->body, new_cap);
    if (!nb) return ccol_not_enough_memory;
    resp->body = nb;
    resp->body_cap = new_cap;
  }
  memcpy(resp->body + resp->body_len, data, len);
  resp->body_len += len;
  return ccol_success;
}

ccol_retval_t chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str) {
  if (!resp || !str) return ccol_invalid_args;
  return chttpsvr_resp_write(resp, str, strlen(str));
}

ccol_retval_t chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...) {
  if (!resp || !fmt) return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = resp->m_procs;
  char stack_buf[256];
  va_list ap, ap2;
  va_start(ap, fmt);
  va_copy(ap2, ap);

  int n = vsnprintf(stack_buf, sizeof(stack_buf), fmt, ap);
  va_end(ap);
  if (n < 0) {
    va_end(ap2);
    return ccol_invalid_args;
  }

  if ((size_t)n < sizeof(stack_buf)) {
    va_end(ap2);
    return chttpsvr_resp_write(resp, stack_buf, (size_t)n);
  }

  char *heap_buf = (char *)_mem_alloc(mp, (size_t)n + 1);
  if (!heap_buf) {
    va_end(ap2);
    return ccol_not_enough_memory;
  }
  int n2 = vsnprintf(heap_buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  if (n2 < 0) {
    _mem_free(mp, heap_buf);
    return ccol_invalid_args;
  }
  ccol_retval_t rv = chttpsvr_resp_write(resp, heap_buf, (size_t)n2);
  _mem_free(mp, heap_buf);
  return rv;
}

ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len) {
  if (!resp || !json || len == 0) return ccol_invalid_args;
  ccol_retval_t rv = chttpsvr_resp_write(resp, json, len);
  if (rv != ccol_success) return rv;
  return chttpsvr_resp_set_header(resp, "content-type", "application/json");
}
