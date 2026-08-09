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

#define _GNU_SOURCE

#include <chttp1_parser.h>
#include <chttpserver.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <ctls.h>
#include <cvector.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
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
  struct chttpserver *srv;       /* back-pointer; do not read directly from
                                  * chttpsvr_router_on/_on_stream/_use -- see
                                  * owner's own comment. */
  ccol_memmgmt_procs_t *m_procs; /* server's own allocator, for this
                                  * router's OWNED CONTENTS (prefix, mw
                                  * list, routes array + route data) only;
                                  * the chttpsvr_router struct itself (this
                                  * shell) is deliberately NOT allocated
                                  * through this, see _create_router. */
  /* The chttpsvr handle that owns this router; CHTTPSVR_INVALID for the root
   * router (created internally by create_chttpsvr_mp before a handle has
   * even been minted, and never exposed to a caller as a chttpsvr_router*)
   * and for any router whose owning server has since been destroyed (see
   * _destroy_router). _Atomic: written by _destroy_router without holding
   * any lock a concurrent reader is guaranteed to also hold (there is no
   * such lock; see the comment below on why), so a plain field here would
   * be a genuine data race, not merely a theoretical one.
   *
   * chttpsvr_router_on/_on_stream/_use resolve this handle (exactly like
   * every other public mutator resolves its chttpsvr handle) before ever
   * touching srv/routes/mw_head. This alone is NOT sufficient, and is the
   * reason this struct's own memory (the "shell": this field plus prefix/
   * srv/m_procs/routes/route_count/route_cap/mw_head/mw_tail/mw_count, i.e.
   * everything BUT the routes/mw-nodes/prefix it points to) is never freed
   * by _destroy_router at all, unlike every other per-request/per-route
   * allocation in this file: reading this very field requires dereferencing
   * `router` itself, which happens BEFORE any resolve/pin can occur, so if
   * the shell's memory had already been returned to the allocator (whether
   * concurrently, mid-free, or because an earlier, fully-completed destroy
   * had already freed it), that read would itself be a use-after-free no
   * lock taken AFTER the read could ever prevent -- confirmed as a real,
   * valgrind-caught UAF by two earlier fix attempts that stopped at
   * resolving this field (an rwlock around _destroy_router's free reduces
   * but does not close the window: it does nothing for a reader that starts
   * after a destroy's free has already completed). Keeping the shell
   * allocated for the remainder of the process (see the registry below)
   * makes reading this field always memory-safe; the atomic load then
   * correctly observes either a live, resolvable handle or the
   * CHTTPSVR_INVALID sentinel _destroy_router stores once the owning server
   * is gone, with every subsequent field access already protected by the
   * ordinary resolve/pin (which _destroy_router's own pending_resolve_count
   * wait, run before it ever frees this router's CONTENTS, correctly
   * serializes against). */
  _Atomic chttpsvr owner;
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

/* Timeout for every courtesy reject-and-close response _conn_reject_and_close
 * writes, regardless of which thread runs it: reject_pool's own dedicated
 * threads (every rejection -- 404/405/500 unmatched/malformed routes and the
 * pool-full 503 case alike -- is routed there; see _conn_reject_via_pool)
 * or the last-resort synchronous fallback on the calling thread (reactor or
 * worker) when reject_pool itself is unavailable or its own (bounded) queue
 * is full. One shared constant, not a separate value per call site: none of
 * these should be allowed to tie up their thread for long on a slow-reading
 * peer, so there is no reason for any of them to be more generous than the
 * others. */
#define _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS 100

/* reject_pool's own worker thread count is not a fixed constant: it is
 * max(_CHTTPSVR_REJECT_POOL_MIN_THREADS, worker_pool's own resolved thread
 * count / 2), computed once in chttpsvr_start (see that function's own
 * comment at the computation site). Every rejection response (see
 * _conn_reject_via_pool) is routed here, not just the pool-full 503 case,
 * so this is sized like a small dedicated pool rather than a single
 * "good enough for an overload corner case" thread: a burst of unmatched-
 * route requests (a very common, everyday shape, unlike a sustained
 * worker_pool-full condition) now needs enough concurrency of its own to
 * drain promptly rather than serializing behind one thread. Scaling with
 * worker_pool's own size (rather than a fixed number) means a large,
 * many-core deployment's reject_pool grows with it instead of staying
 * pinned at whatever constant happened to be picked for a much smaller
 * default configuration; the floor keeps a single- or few-worker server
 * from regressing to the original one-thread bottleneck this pool was
 * built to remove. */
#define _CHTTPSVR_REJECT_POOL_MIN_THREADS 2

/* reject_pool's own task queue capacity. Bounded (not
 * CHTTPSVR_QUEUE_UNBOUNDED-style 0/unbounded) so a sustained flood of
 * rejected connections can't grow reject_pool's backlog -- and therefore
 * this process's memory, one queued chttpsvr_conn_t per pending reject-close
 * -- without limit; once full, ctpool_try_submit fails and
 * _conn_reject_via_pool falls back to a synchronous close on the calling
 * thread instead (the same fallback already used when reject_pool is
 * unavailable entirely, e.g. during shutdown). Sized larger than the
 * original 256 now that every rejection path (not just pool-full 503) feeds
 * this same queue. */
#define _CHTTPSVR_REJECT_POOL_QUEUE_CAP 1024

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
  CONN_ST_DIVERTED, /* worker thread owns fd; reactor registration (if any)
                     * is paused (event_loop_pause), not removed: no
                     * events fire, but the registration is kept alive and
                     * cheaply resumed (event_loop_resume) for the next
                     * request instead of being rebuilt from scratch */
  CONN_ST_CLOSING
} conn_state_t;

/** Per-connection object. Persists across every keep-alive request on this
 * connection; per-request scratch fields (method/path/headers/route/
 * dispatch/resp/body buffers) are reset by _conn_reset_for_request() before
 * each new request begins. Heap-allocated; owned by whichever of (the
 * reactor's event_reg, a worker's ctpool task) currently holds it; never
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
   * exact form; _match_segments/_seg_matches_literal split on a literal
   * '/' and decode each segment individually, which is the only way to tell
   * an actual path separator from a %2F encoded one inside a {param}
   * segment. Decoding the whole path eagerly here (as an earlier version of
   * this function did) would turn %2F into a real '/' before segmentation,
   * silently splitting one param segment into two path segments, and would
   * also have to rejects the entire request on any malformed %XX anywhere
   * in the path; this module's own tests document that a malformed
   * encoding in one segment must fall out as an ordinary route mismatch
   * (404), not a 400, since _decode_seg_alloc/_seg_matches_literal already
   * treat a decode failure as "this segment doesn't match" further down. */
  char *path;
  /* owned, fully URL-decoded; populated by _on_headers_complete() once a
   * route has actually matched (decoding is then guaranteed to succeed,
   * since a match already proved every segment decodes cleanly); this is
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
  /* Set once the "HTTP/1.1 100 Continue\r\n\r\n" interim response has
   * actually been written for this request (see the send sites: _task_worker
   * for a buffered route, chttpsvr_req_read for a streaming route); prevents
   * a streaming handler that calls chttpsvr_req_read() more than once from
   * writing the interim line twice. */
  bool interim_continue_sent;
  growbuf_t body; /* buffered-route whole body, or streaming pending bytes */
  size_t body_bytes_seen; /* cumulative, for max_body_size enforcement */
  bool body_too_large;
  /* Set when a streaming route's chttpsvr_req_read() hits a truncated body
   * (peer EOF or a hard I/O error before the message finished framing) or a
   * chttp1_parser-level CHTTP1_USER/CHTTP1_ERROR that isn't a max_body_size
   * rejection (that case is reported via body_too_large instead, checked
   * first by chttpsvr_req_stream_error() so its priority is unaffected).
   * Read only by chttpsvr_req_stream_error(); the buffered-route path
   * (_drain_body) has no equivalent need for this, since it reports
   * ccol_http_transfer_aborted directly as its own return value instead of
   * through a flag a caller reads back out of conn afterward. */
  bool transfer_aborted;

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

  /* Diverted-connection registry bookkeeping (see struct chttpserver's own
   * diverted_mutex/diverted_head/diverted_tail comment): every connection
   * currently owned by a worker thread (CONN_ST_DIVERTED) is linked in here
   * so a stuck worker's connection can be forcibly shutdown(2)'d during
   * teardown; list fields guarded by srv->diverted_mutex, same pattern as
   * the idle list above. */
  struct chttpsvr_conn *diverted_prev, *diverted_next;
  bool in_diverted_list;

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

/* ========================================================================== */
/*                         CHTTPSVR HANDLE SLOT TABLE                         */
/* ========================================================================== */

/* chttpsvr is an opaque value handle (top 32 bits = slot index, bottom 32
 * bits = generation; see include/chttpserver.h's own doc comment on the
 * typedef), resolved through this table before the underlying struct
 * chttpserver* is ever touched. This is what lets __chttpsvr_destroy detect
 * BOTH a concurrent double-destroy (racing another destroy on the same
 * still-live handle) AND a sequential one (a stale handle, from an earlier,
 * already-completed destroy) as a fatal_err rather than a
 * use-after-free/double-free: a slot is marked not-in-use the instant it is
 * released, and its generation is bumped on every reuse, so a stale handle
 * can never alias a later, unrelated server occupying the same slot index.
 * Mirrors chttpclient.c's own identical chttpcli_slot_table mechanism
 * exactly; see that file's own section-level comment for the full design
 * rationale (rejected alternatives, the resolve-then-use race and its fix,
 * etc.); not repeated here. */
typedef struct {
  struct chttpserver *ptr; /* NULL when slot is free */
  uint32_t generation;     /* minted fresh on every acquire; monotonic per
                               slot index, starts at 0 (pre-first-use),
                               becomes 1 on first acquire */
  bool in_use;
} chttpsvr_slot_t;

static struct {
  mutex_t mutex;
  once_flag_t once;
  cvec slots;        /* cvec of chttpsvr_slot_t; grows via push_back only,
                         indices permanent once allocated */
  cvec free_indices; /* cvec of uint32_t; LIFO free list, O(1) reuse */
} chttpsvr_slot_table = {0};

static void _chttpsvr_slot_table_init_globals(void) {
  mutex_init(chttpsvr_slot_table.mutex);
  chttpsvr_slot_table.slots = cvector_create(sizeof(chttpsvr_slot_t), NULL);
  if (!chttpsvr_slot_table.slots)
    fatal_err("chttpsvr slot table: failed to allocate slots vector");
  chttpsvr_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpsvr_slot_table.free_indices)
    fatal_err("chttpsvr slot table: failed to allocate free-index vector");
}

/** Main server struct. */
struct chttpserver {
  chttpsvr_router **routers; /* [0] = root, [1..n] = sub-routers */
  size_t router_count;
  size_t router_cap;
  ctpool worker_pool;
  /* max(_CHTTPSVR_REJECT_POOL_MIN_THREADS, worker_pool's own resolved
   * thread count / 2) dedicated threads (see that constant's own comment),
   * bounded queue (_CHTTPSVR_REJECT_POOL_QUEUE_CAP), used for every
   * _conn_reject_and_close call (the courtesy write-then-close a rejected
   * connection gets, itself
   * bounded to _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS on one of this pool's own
   * threads): an unmatched/malformed-route 404/405/500 discovered
   * synchronously during header parsing, and worker_pool being already full
   * (503) alike. Deliberately NOT worker_pool itself, even for the 503
   * case: the whole point is to keep this write off the reactor thread
   * without waiting on (or competing with) the exact pool that just
   * rejected the request for being at capacity. Bounded, not unbounded, so
   * a sustained flood of rejections can't grow this pool's own backlog
   * without limit; once full, _conn_reject_via_pool falls back to a
   * synchronous close on the calling thread instead. See that function's
   * own comment on why this exists. */
  ctpool reject_pool;
  ctls_ctx_t *tls_ctx; /* non-NULL when TLS configured */
  mutex_t mutex;
  cond_var_t requests_done_cv;
  int in_flight_requests;
  /* Pinned by _chttpsvr_resolve (lock-free atomic increment) for as long as
   * some caller holds a just-resolved struct chttpserver* it hasn't yet
   * handed off to its own tier-specific protection; or (for the short,
   * synchronous public API this module has: chttpsvr_start/_stop/
   * _register_handler/_register_streaming_handler/_use/_subrouter) for as
   * long as that one call is still running, since each of those holds its
   * pin across its own entire body. Released by _chttpsvr_resolve_unpin
   * (under `mutex`, together with the broadcast that wakes a waiting
   * destroy; see that function's own comment, and chttpclient.c's
   * identical _chttpcli_resolve_unpin, for why the decrement itself, not
   * just the broadcast, must happen under the lock). __chttpsvr_destroy
   * blocks until this reaches 0 before freeing the object, closing a real
   * resolve-then-use race a naive "look up, unlock, return the pointer"
   * resolve step would otherwise leave open. Deliberately a separate
   * counter from in_flight_requests above (and its own resolve_cv below,
   * not requests_done_cv): every consumer of a chttpsvr handle holds this
   * pin for its own entire, synchronous duration, so the two counters never
   * need to be waited on together the way chttpclient.c's analogous
   * pending_resolve_count/in_flight_count do (unlike chttpclient_do's
   * long-running in-flight *requests*, nothing here hands off protection to
   * a separate, later-draining mechanism mid-call). */
  _Atomic size_t pending_resolve_count;
  cond_var_t resolve_cv;
  rw_lock_t routes_lock;
  clog cl;

  /* _Atomic, not merely written under srv->mutex, since _listener_on_
   * readable (running on the reactor thread) reads both fields on every
   * accept() call without ever taking srv->mutex; plain int/bool fields
   * here are a genuine, TSan-confirmed data race against chttpsvr_start/
   * _stop's mutex-protected writes. chttpsvr_start also assigns both of
   * these BEFORE registering the listener with event_loop_add, not after:
   * event_loop_add makes the registration immediately live, so a
   * connection arriving in the window between registration and these
   * fields being set could otherwise be dispatched to _listener_on_
   * readable while it still observed listen_fd's pre-start default (-1). */
  _Atomic int listen_fd; /* -1 when not started */
  _Atomic bool is_unix_socket;
  char *unix_socket_path; /* owned; NULL for a TCP listener; guarded by
                           * srv->mutex; never read by _listener_on_readable,
                           * only by chttpsvr_stop/_quiesce_server_once, so
                           * it does not need the same _Atomic treatment. */
  event_reg *listen_reg;
  bool started;
  bool contributed_to_engine;
  /* Set once this server's listener/connections/worker pool/engine reference
   * have been torn down for good, whether that happened via chttpsvr_destroy()
   * or via the shared engine's own force-stop quiesce pass (see
   * _engine_force_stop_quiesce_all); makes that teardown idempotent regardless
   * of which of the two callers reaches it first. Reset to false at the top
   * of chttpsvr_start() so a server that is legitimately restarted after a
   * stop is not permanently treated as already torn down. */
  bool teardown_started;
  /* Set only once the REAL teardown work _quiesce_server_once performs
   * (stop listening, drain in-flight requests, close idle connections,
   * release the engine reference, destroy the worker pools) has actually
   * finished, not merely started; distinct from teardown_started, which
   * flips the instant the winning caller claims the work. __chttpsvr_destroy
   * and _engine_force_stop_quiesce_all can call _quiesce_server_once for the
   * same srv concurrently (chttpsvr_engine_stop() is documented safe to call
   * from a signal handler, so it can race an application's own, independent
   * chttpsvr_destroy() call with no chttpsvr_engine_wait() serializing the
   * two); whichever call loses that race must BLOCK until teardown_done is
   * set (see teardown_done_cv below) rather than returning immediately as a
   * bare no-op, since __chttpsvr_destroy proceeds straight from
   * _quiesce_server_once's return into mutex_destroy(raw->mutex)/freeing raw
   * itself -- doing that while the winning call is still using raw->mutex/
   * raw->idle_mutex/raw->requests_done_cv inside _drain_and_close_all_
   * connections would destroy a mutex still in use and free srv out from
   * under the still-running teardown, a genuine use-after-free. Reset to
   * false alongside teardown_started at the top of chttpsvr_start(). */
  bool teardown_done;
  /* Broadcast once teardown_done is set; see that field's own comment. */
  cond_var_t teardown_done_cv;

  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned idle_timeout_ms; /* keep-alive idle timeout; 0 = disabled */
  /* _Atomic, not merely written under srv->mutex, for the same reason as
   * stream_read_timeout_ms/etc. just above: chttpsvr_start() (re)writes
   * these on every restart in the small unlocked gap after srv->worker_pool
   * is published (see the mutex_lock/_unlock a few lines above the
   * corresponding write site) and before the listener is re-registered.
   * A pre-existing keep-alive connection that survived the preceding
   * chttpsvr_stop() (which only tears down the listener, not already-
   * accepted connections) can be diverted to that just-published pool and
   * have its worker thread read max_body_size (_on_body) or
   * max_header_bytes (_conn_reset_for_request) inside that same gap, with
   * no lock or atomic op common to both the writer and that reader to
   * establish a happens-before edge. A genuinely in-flight request is
   * already excluded from racing here (chttpsvr_start's own
   * _wait_and_detach_pools call, gated by srv->mutex the same way
   * _conn_start_diverted's in_flight_requests increment is, blocks the
   * restart until every such request has fully finished), but an idle
   * connection's very next request is not. Found by code review, not by
   * a failing test: the window is a handful of unlocked instructions per
   * restart, real but too narrow to reliably reproduce under TSan even
   * across tens of thousands of stress-test restart cycles. */
  _Atomic size_t max_body_size;
  _Atomic size_t
      max_header_bytes;           /* 0 = chttp1_parser's own built-in default */
  _Atomic size_t max_connections; /* 0 = unlimited */
  /* Plain bool, unlike the three fields above: its one read site
   * (_listener_on_readable, via _apply_accepted_socket_options) only ever
   * runs while a listener registration is live, and chttpsvr_stop()'s
   * event_loop_remove(listen_reg) blocks until any in-flight listener
   * callback has returned and guarantees none fires again afterward, while
   * the new listener is not registered until well after chttpsvr_start()
   * writes this field. So, unlike max_body_size/max_header_bytes above
   * (whose reader is a pre-existing connection's worker thread, entirely
   * decoupled from this server's own listener lifecycle), there is no
   * window in which a reader of this specific field can run concurrently
   * with a writer of it. */
  bool enable_keepalive;
  _Atomic size_t current_connections;

  /* Idle-connection registry for the idle-timeout sweep: every connection
   * currently owned by the reactor and waiting for its next request's
   * headers (never a diverted, worker-owned connection). */
  mutex_t idle_mutex;
  chttpsvr_conn_t *idle_head, *idle_tail;

  /* Diverted-connection registry: every connection currently owned by a
   * worker thread (CONN_ST_DIVERTED; see _conn_start_diverted/_task_worker),
   * i.e. one that could be blocked indefinitely inside chttp1_stream_read/
   * _write if stream_read_timeout_ms/response_write_timeout_ms is configured
   * to 0 ("wait indefinitely", a documented, legitimate setting) and its
   * peer stalls without closing. _wait_in_flight_bounded uses this to
   * forcibly shutdown(2) such a connection's fd once its own graceful wait
   * has been exhausted, so a single stalled peer cannot hang
   * chttpsvr_destroy()/a chttpsvr_stop()+_start() restart/chttpsvr_engine_
   * wait() forever the way an unconditional, timeout-less
   * ctpool_shutdown_drain() call otherwise would; see that function's own
   * comment for the full reasoning. Guarded by diverted_mutex, same pattern
   * as idle_mutex/idle_head/idle_tail above. */
  mutex_t diverted_mutex;
  chttpsvr_conn_t *diverted_head, *diverted_tail;

  ccol_memmgmt_procs_t *m_procs;
};

/* ========================================================================== */
/*                    CHTTPSVR HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* Resolves h and pins the result against concurrent destroy, or returns
 * NULL if h is 0, garbage, or references a currently-free or
 * already-reused (wrong-generation) slot. On success, the caller MUST call
 * _chttpsvr_resolve_unpin(result) exactly once, immediately before every
 * return path of the short, synchronous public function that resolved it
 * (see struct chttpserver's own pending_resolve_count field comment for why
 * no consumer of this handle needs to hold the pin any longer than its own
 * function body). Mirrors chttpclient.c's _chttpcli_resolve exactly. */
static struct chttpserver *_chttpsvr_resolve(chttpsvr h) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* Lock-free: no raw->mutex acquisition here at all, so nothing can ever
   * block while chttpsvr_slot_table.mutex is held; see
   * chttpclient.c's _chttpcli_resolve for the full contention rationale
   * this mirrors. Safe because raw is guaranteed still-allocated here
   * regardless: the only thing that could make it unsafe to touch,
   * __chttpsvr_destroy's slot-release step, also requires
   * chttpsvr_slot_table.mutex, which we still hold at this exact point. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

static void _chttpsvr_resolve_unpin(struct chttpserver *raw) {
  /* The decrement itself MUST happen under raw->mutex, not as a bare atomic
   * op outside it; see chttpclient.c's _chttpcli_resolve_unpin for the
   * full account of the real use-after-free an earlier, lock-free version
   * of that function's decrement was found to cause. The reasoning and fix
   * are identical here: the increment (resolve) stays lock-free, but the
   * decrement (unpin) happens together with the broadcast, both inside
   * raw->mutex, matching the standard condition-variable pattern. */
  mutex_lock(raw->mutex);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  cond_var_broadcast(raw->resolve_cv); /* wake a destroy waiting on this */
  mutex_unlock(raw->mutex);
}

/* Allocates a fresh slot (or reuses a freed one) for srv and returns the
 * resulting handle, or 0 on OOM. Called once, from create_chttpsvr_mp,
 * after the object is otherwise fully constructed. Mirrors chttpclient.c's
 * _chttpcli_handle_slot_acquire exactly, including the generation-mint
 * ordering (must happen before the handle is built from it) and the
 * generation-wraparound skip (so a wrapped-around live server's handle can
 * never collide with CHTTPSVR_INVALID). */
static chttpsvr _chttpsvr_handle_slot_acquire(struct chttpserver *srv) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  mutex_lock(chttpsvr_slot_table.mutex);
  uint32_t idx;
  chttpsvr_slot_t *slot;
  if (cvector_elem_count(chttpsvr_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpsvr_slot_table.free_indices, &idx);
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  } else {
    chttpsvr_slot_t fresh = {0};
    if (cvector_push_back(chttpsvr_slot_table.slots, &fresh) != ccol_success) {
      mutex_unlock(chttpsvr_slot_table.mutex);
      return 0; /* ordinary, non-fatal OOM */
    }
    idx = (uint32_t)cvector_elem_count(chttpsvr_slot_table.slots) - 1;
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* skip the value that
would collide with CHTTPSVR_INVALID; see chttpclient.c's identical
guard for the full reasoning */
  slot->ptr = srv;
  slot->in_use = true;
  chttpsvr h = ((chttpsvr)idx << 32) | (chttpsvr)slot->generation;
  mutex_unlock(chttpsvr_slot_table.mutex);
  return h;
}

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpserver's OWN engine)        */
/* ========================================================================== */

/*
 * One static, process-wide event_loop reactor shared by every chttpsvr
 * instance in the process; one of two separate, independent reactors
 * (the other belongs to chttpclient, its own analogous static event_loop;
 * see chttpclient.c). chttpserver and chttpclient do not share a single
 * process-wide reactor, so this lifecycle wrapper needs no cross-module
 * coordination at all, only ref-counting across chttpsvr instances (an
 * acquire/release/reaper-thread shape, simplified: no atexit safety net
 * cross-module ordering concern, since there is nothing else in the process
 * racing to bring this specific reactor up first).
 */
static struct {
  event_loop reactor;
  size_t reactor_refs;
  mutex_t mutex;
  cond_var_t stopped_cv;
  once_flag_t once;
  bool stopping;
  thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;
  /* 0 = default (single dedicated reactor thread, num_reactor_threads == 1
   * under the hood); benchmarked, not assumed, to be the better choice for
   * the common case; see chttpsvr_set_engine_num_reactor_threads's own
   * doc comment for the full comparison. A positive value pins the reactor
   * to exactly that many OS threads instead. Baked into the reactor at
   * construction time, same "before first start, or after a full stop"
   * restriction as mprocs above. */
  size_t num_reactor_threads;
  /* The value actually passed to event_loop_create_with_mprocs the last time
   * the reactor was created (auto-detected or explicit); for test
   * instrumentation only, see _chttpsvr_engine_num_reactor_threads_for_tests
   * below. */
  size_t last_resolved_num_reactor_threads;
  /* Repurposed "engine logger": The event_loop reactor has no internal logging
   * of its own to forward, so this captures chttpserver's OWN reactor-thread
   * diagnostics (TLS handshake failures, listener bind errors, idle-timeout
   * closes) across every chttpsvr instance sharing the one process-wide
   * reactor. A caller-installed logger (via chttpsvr_set_engine_logger) is
   * used if set before the engine first starts; otherwise _engine_acquire
   * installs a CLOG_FATAL-level fallback logger (fd 2) the first time the
   * engine starts, so this field is never actually NULL for the engine's
   * whole running lifetime; not merely an internal detail: _engine_reaper_
   * fn's own teardown log_info call, and every logging call site, both
   * rely on having SOME live logger to call through (_clog_write itself
   * has no NULL-handle guard, so calling through a genuinely NULL logger
   * would crash, not silently skip). Only the rare logger-allocation-itself-
   * fails-with-OOM path ever reaps the engine with this left NULL (see that
   * path's own comment in _engine_acquire). Guarded by srv_engine_bundler.
   * mutex purely against a torn pointer read/write racing a concurrent
   * chttpsvr_set_engine_logger() call (clog itself is already thread-safe for
   * concurrent logging calls through one handle). */
  clog log;
} srv_engine_bundler = {0};

/* Idle-timeout sweep thread: one per process, shared by every chttpsvr
 * instance's idle-connection registry (each server has its own
 * srv->idle_head/tail list; the sweep just walks every started server). */
static struct {
  thread_id_t thread;
  bool running;
  /* _Atomic, not merely written under servers_bundler.mutex, since
   * _idle_sweep_fn (running on its own dedicated thread) reads this in its
   * own loop condition without ever taking that mutex; a plain bool here
   * is a genuine, TSan-confirmed data race against
   * _idle_sweep_stop_if_running's mutex-protected write, and -- unlike a
   * torn read, which this specific field's single-byte size makes unlikely
   * in practice -- is undefined behavior that gives the compiler license
   * to cache the read across loop iterations and never observe the write
   * at all, which would turn _idle_sweep_stop_if_running's thread_join
   * into a real, indefinite hang. */
  _Atomic bool stop_flag;
} idle_sweep_bundler = {0};

static struct {
  mutex_t mutex;
  struct chttpserver **servers;
  size_t count;
  size_t capacity;
} servers_bundler = {0};

static void _idle_sweep_stop_if_running(void);
static void _quiesce_server_once(struct chttpserver *srv);
static void _chttpsvr_stop_internal(struct chttpserver *raw);

/* Called only from the engine reaper (_engine_reaper_fn), before the shared
 * reactor is actually torn down: quiesces (stops listening, drains in-flight
 * requests, closes idle connections, shuts down and destroys the worker
 * pool, releases the engine reference) every chttpsvr instance still
 * registered in servers_bundler.servers, regardless of whether this reaper
 * run was triggered by chttpsvr_engine_stop() (servers may still be fully live
 * and started) or by the graceful ref-count-reaches-zero path (every registered
 * server has, by construction, already quiesced and unregistered itself, so
 * this is an immediate no-op there). Without this, a forced engine stop
 * would tear down srv_engine_bundler.reactor and free
 * servers_bundler.servers out from under still-started servers whose own
 * listen_reg/conn->reg registrations point into it, leaving those servers' next
 * chttpsvr_destroy() call to dereference already-freed state.
 *
 * Repeatedly re-peeks servers_bundler.servers[0] rather than snapshotting
 * the whole list up front, since _quiesce_server_once -> _servers_unregister
 * removes the entry it just processed (or, if a concurrent chttpsvr_destroy()
 * on another thread is already quiescing that exact same server, leaves it in
 * place until that other call finishes); either way this loop always
 * converges on servers_bundler.count == 0 once every legitimately
 * in-progress teardown completes. */
static void _engine_force_stop_quiesce_all(void) {
  struct chttpserver *prev_unremoved = NULL;
  for (;;) {
    mutex_lock(servers_bundler.mutex);
    if (servers_bundler.count == 0) {
      mutex_unlock(servers_bundler.mutex);
      return;
    }
    struct chttpserver *srv = servers_bundler.servers[0];
    mutex_unlock(servers_bundler.mutex);

    if (srv == prev_unremoved) {
      /* A concurrent chttpsvr_destroy(srv) on another thread already won the
       * race to quiesce this exact server and hasn't unregistered it yet;
       * avoid a tight spin while it finishes. */
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
    }
    prev_unremoved = srv;
    _quiesce_server_once(srv);
  }
}

static void _engine_globals_init(void) {
  mutex_init(srv_engine_bundler.mutex);
  cond_var_init(srv_engine_bundler.stopped_cv);
  mutex_init(servers_bundler.mutex);
  /* SIGPIPE must be suppressed for all TCP servers, unconditionally: a
   * client can close its read side (or the whole connection) while a worker
   * is still mid-write on the response, and chttp1_stream_write's raw
   * write()/send() has no per-call SIGNONE suppression of its own. Without
   * this, that write raises SIGPIPE, whose default disposition kills the
   * whole process. */
  signal(SIGPIPE, SIG_IGN);
}

/*
 * Logs through srv_engine_bundler.log with the read of that pointer and the
 * actual log_info/log_error call combined into ONE critical section, rather
 * than copying the pointer out under the lock and using it afterward with no
 * lock held at all (this replaces the _engine_logger_get() shape every call
 * site here used to follow). That earlier shape was a real use-after-free:
 * chttpsvr_set_engine_logger() swaps in a new logger under srv_engine_
 * bundler.mutex but calls clog_close() on the OLD one only after releasing
 * it (clog_close() unconditionally frees the handle itself, regardless of
 * the shared backing store's own separate refcount; see clogger.c), so a
 * caller that had already copied the old pointer out before the swap, and
 * only used it afterward, could call log_info()/log_error() on memory that
 * had just been freed out from under it. Kept as a macro (rather than a
 * wrapping function) specifically so the log call's own __FILE__/__LINE__/
 * __func__ capture still reflects the real call site, not this helper's.
 */
#define _SRV_ENGINE_LOG(level_call, ...)                      \
  do {                                                        \
    call_once(srv_engine_bundler.once, _engine_globals_init); \
    mutex_lock(srv_engine_bundler.mutex);                     \
    if (srv_engine_bundler.log)                               \
      level_call(srv_engine_bundler.log, __VA_ARGS__);        \
    mutex_unlock(srv_engine_bundler.mutex);                   \
  } while (0)

static void _join_reaper_if_needed_locked(void) {
  if (srv_engine_bundler.reaper_joinable) {
    thread_join(srv_engine_bundler.reaper_thread);
    srv_engine_bundler.reaper_joinable = false;
  }
}

static void *_engine_reaper_fn(void *arg) {
  (void)arg;
  event_loop loop_to_destroy;
  mutex_lock(srv_engine_bundler.mutex);
  loop_to_destroy = srv_engine_bundler.reactor;
  mutex_unlock(srv_engine_bundler.mutex);
  /* Quiesce every still-registered server BEFORE touching the idle sweep
   * thread or the reactor itself: chttpsvr_engine_stop() (the forced path)
   * may run this reaper while one or more servers are still fully started,
   * with live listen_reg/conn->reg registrations into
   * srv_engine_bundler.reactor; tearing the reactor down first would leave
   * those registrations dangling the moment the owning server's own
   * chttpsvr_stop()/_conn_close() next tried to use them. The graceful
   * ref-count-reaches-zero path already guarantees every registered server has
   * quiesced and unregistered itself by the time it gets here, so this is a
   * fast no-op in that case. */
  _engine_force_stop_quiesce_all();
  /* Stop the idle sweep thread before tearing down the reactor it calls
   * into (_conn_close -> event_loop_remove); see
   * _idle_sweep_stop_if_running's own doc comment. */
  _idle_sweep_stop_if_running();
  if (loop_to_destroy) event_loop_destroy(loop_to_destroy);

  /* servers_bundler.servers' backing array is a plain realloc'd buffer, not
   * itself tied to any one server's lifetime; every server that ever
   * contributed a ref to this engine has already been unregistered (via
   * _engine_force_stop_quiesce_all above, or, in the graceful path, by its own
   * chttpsvr_destroy() call before the ref count could ever reach zero) by the
   * time we get here, so servers_bundler.count is always 0 at this
   * point; servers_bundler.count is reset explicitly anyway as a
   * defensive belt-and-braces measure, not merely assumed, given this exact
   * assumption previously went unenforced and caused a real use-after-free (see
   * _engine_force_stop_quiesce_all's own doc comment). Free the now-empty array
   * itself so it doesn't show up as a still-reachable allocation for the rest
   * of the process. */
  mutex_lock(servers_bundler.mutex);
  free(servers_bundler.servers);
  servers_bundler.servers = NULL;
  servers_bundler.capacity = 0;
  servers_bundler.count = 0;
  mutex_unlock(servers_bundler.mutex);

  mutex_lock(srv_engine_bundler.mutex);
  srv_engine_bundler.reactor = EVENT_LOOP_INVALID;
  srv_engine_bundler.stopping = false;
  /* The engine-wide diagnostics logger (whether caller-installed via
   * chttpsvr_set_engine_logger, or the fallback _engine_acquire installs on
   * its own) is scoped to this reactor's own lifetime, same as the reactor
   * itself: closed here so a full engine stop doesn't leave it dangling as a
   * still-reachable allocation for the rest of the process's life. A later
   * chttpsvr_start() brings the reactor back up with a fresh fallback
   * logger (or a fresh call to chttpsvr_set_engine_logger(), if a
   * non-default one is wanted again).
   *
   * log can legitimately still be NULL here: _engine_acquire's own
   * fallback-logger allocation can itself fail, in which case it reaps the
   * engine via this exact same reaper path (see that function's own
   * comment) with no logger ever having been installed; _clog_write has no
   * NULL-handle guard of its own, so logging unconditionally here would
   * crash instead of simply skipping diagnostics. */
  if (srv_engine_bundler.log)
    log_info(srv_engine_bundler.log,
             "The http server reactor engine has been destroyed");
  clog old_logger = srv_engine_bundler.log;
  srv_engine_bundler.log = NULL;
  cond_var_broadcast(srv_engine_bundler.stopped_cv);
  mutex_unlock(srv_engine_bundler.mutex);

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
  mutex_lock(srv_engine_bundler.mutex);
  srv_engine_bundler.reaper_thread = reaper;
  srv_engine_bundler.reaper_joinable = true;
  mutex_unlock(srv_engine_bundler.mutex);
}

/* Defined further below, alongside the rest of the signal-safe engine-stop
 * watcher machinery; forward-declared here since _engine_acquire (the one
 * call site) is defined first in the file. */
static void _engine_stop_watcher_ensure_started_locked(void);

static ccol_retval_t _engine_acquire(void) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  /* First thing done under the lock, before anything else in this
   * function: guarantees the signal-safe stop watcher is already live for
   * the remainder of this call and for every later srv_engine_bundler.
   * mutex-locking call in the process, closing the self-deadlock window
   * chttpsvr_engine_stop()'s own doc comment (and g_engine_stop_watcher's)
   * describes. */
  _engine_stop_watcher_ensure_started_locked();
  while (srv_engine_bundler.stopping)
    cond_var_wait(srv_engine_bundler.stopped_cv, srv_engine_bundler.mutex);
  _join_reaper_if_needed_locked();

  if (!srv_engine_bundler.reactor) {
    /* Default 1 (not an auto-detected CPU count): benchmarked, not assumed,
     * against a real HTTP workload; see chttpsvr_set_engine_num_reactor_
     * threads's own doc comment for the full comparison and reasoning. A
     * single dedicated poller thread that also runs every callback inline
     * measured faster and more latency-consistent than CPU-count dispatch
     * threads for both plain HTTP and TLS-with-connection-reuse traffic (the
     * common case for a well-behaved client population); multi-threaded
     * dispatch only pulled ahead under a synthetic TLS handshake-storm
     * workload (every request a brand-new connection, no reuse at all), and
     * even there by a moderate, not dramatic, margin. */
    size_t nthreads = srv_engine_bundler.num_reactor_threads;
    if (nthreads == 0) nthreads = 1;
    srv_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    srv_engine_bundler.reactor = event_loop_create_with_mprocs(
        256, 4, nthreads, srv_engine_bundler.mprocs, &err);
    if (!srv_engine_bundler.reactor) {
      mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    if (!srv_engine_bundler.log) {
      srv_engine_bundler.log =
          clog_open_fd_mp(2, CLOG_FATAL, srv_engine_bundler.mprocs);
      if (!srv_engine_bundler.log) {
        /* Roll back the reactor already created just above: leaving it
         * running with reactor_refs still 0 would leak it for the process
         * lifetime, since nothing would ever call _engine_release() to
         * reap it (that only happens once a server has successfully
         * contributed a reference). Safe to destroy synchronously, still
         * holding srv_engine_bundler.mutex, unlike chttpclient's own
         * analogous fix for its deadline-sweep thread: no other thread can
         * possibly be contending for this mutex at this point here, the
         * idle-timeout sweep thread is only ever started by
         * chttpsvr_start(), strictly AFTER this function has already
         * returned success, and event_loop's own reactor threads have no
         * knowledge of srv_engine_bundler at all, so there is nothing else
         * that could call back into _SRV_ENGINE_LOG/chttpsvr_set_
         * engine_logger() and deadlock a synchronous join here. */
        event_loop_destroy(srv_engine_bundler.reactor);
        srv_engine_bundler.reactor = EVENT_LOOP_INVALID;
        mutex_unlock(srv_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(srv_engine_bundler.log, "component", "http-server-engine");
    }

    log_info(srv_engine_bundler.log,
             "New http server reactor engine has been created");
  }
  srv_engine_bundler.reactor_refs++;
  mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

static void _engine_release(void) {
  bool should_reap = false;
  mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor_refs > 0) srv_engine_bundler.reactor_refs--;
  /* The !srv_engine_bundler.stopping guard matters for a case that did
   * not exist before _engine_force_stop_quiesce_all: that function calls
   * _quiesce_server_once, which calls this function, for every server it
   * quiesces, WHILE a reaper spawned by chttpsvr_engine_stop() is already
   * running with srv_engine_bundler.stopping already true and
   * srv_engine_bundler.reactor still non-NULL (it hasn't been destroyed yet at
   * that point). Without this guard, releasing the last ref during that pass
   * would look identical to the ordinary graceful "last server just released
   * its ref" case and spawn a second, redundant reaper thread racing the one
   * already tearing this same reactor down. */
  if (srv_engine_bundler.reactor_refs == 0 && srv_engine_bundler.reactor &&
      !srv_engine_bundler.stopping) {
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) _spawn_reaper();
}

static void _engine_wait_until_stopped(void) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  while (srv_engine_bundler.reactor || srv_engine_bundler.stopping)
    cond_var_wait(srv_engine_bundler.stopped_cv, srv_engine_bundler.mutex);
  _join_reaper_if_needed_locked();
  mutex_unlock(srv_engine_bundler.mutex);
}

/* The real force-stop logic (identical to what this function's body used to
 * be before the signal-safe watcher below was introduced): takes
 * srv_engine_bundler.mutex and, if a reactor is currently live, may go on
 * to call thread_create() via _spawn_reaper(). Neither pthread_mutex_lock
 * nor pthread_create is POSIX-guaranteed async-signal-safe, so this
 * function must only ever run on an ordinary thread; see
 * g_engine_stop_watcher's own comment below for why chttpsvr_engine_stop()
 * itself no longer calls this directly. */
static void _engine_force_stop_now(void) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  bool should_reap = false;
  mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    srv_engine_bundler.reactor_refs = 0;
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) _spawn_reaper();
}

/* ========================================================================== */
/*                    SIGNAL-SAFE ENGINE-STOP WATCHER                         */
/* ========================================================================== */

/*
 * chttpsvr_engine_stop() is documented (chttpsvr.h) as "async-signal-safe:
 * safe to call from a signal handler" -- the whole point being that an
 * application can install it (or a thin wrapper around it) as a SIGTERM/
 * SIGINT handler for graceful shutdown. _engine_force_stop_now() above is
 * NOT actually async-signal-safe, though: it calls mutex_lock() (a plain,
 * non-recursive pthread mutex) and, via _spawn_reaper(), thread_create().
 * Neither is on the POSIX list of async-signal-safe functions, and this is
 * not a theoretical concern -- both are genuinely reachable self-deadlocks:
 * srv_engine_bundler.mutex is also taken by _engine_acquire() (called
 * synchronously from chttpsvr_start()), _engine_release() (called from
 * chttpsvr_destroy()/_quiesce_server_once), and chttpsvr_set_engine_logger/
 * _set_engine_mem_mgmt_procs/_set_engine_num_reactor_threads. A signal
 * arriving on the very thread that is currently inside any one of those
 * calls -- e.g. SIGTERM delivered mid-chttpsvr_start(), a realistic race
 * during a container's shutdown/rolling-restart sequence -- would have the
 * handler's own mutex_lock(srv_engine_bundler.mutex) call self-deadlock
 * against the lock that same thread already holds, hanging the whole
 * process (surviving only a SIGKILL), the opposite of what installing a
 * graceful-shutdown handler is for.
 *
 * Fixed by moving every mutex/thread_create-touching step off of whichever
 * thread happens to call chttpsvr_engine_stop() and onto this dedicated,
 * always-running watcher thread instead. chttpsvr_engine_stop() itself is
 * reduced to exactly two possible operations, both genuinely POSIX-
 * guaranteed async-signal-safe: a lock-free atomic load (checking the
 * watcher is up) and sem_post() (the one synchronization primitive POSIX
 * explicitly lists as async-signal-safe; see semaphore_post() in
 * common.h). The watcher thread blocks in semaphore_wait() -- an ordinary,
 * non-signal execution context, so mutex_lock()/thread_create() are both
 * fine there -- and, once woken, performs the exact same work
 * _engine_force_stop_now() always has.
 */
static struct {
  semaphore_t sem;
  /* True once sem and thread are both fully live; the only field
   * chttpsvr_engine_stop() itself ever reads, via a lock-free atomic load
   * (safe from a signal handler; a mutex-guarded bool would not be). */
  _Atomic bool ready;
  thread_id_t thread;
  /* Guards start attempts; only ever touched while holding
   * srv_engine_bundler.mutex, from _engine_stop_watcher_ensure_started_
   * locked() -- itself only ever called from an ordinary thread (ultimately
   * from _engine_acquire()), never from a signal handler. Not `ready`
   * itself: a transient start failure (OOM-class) must be retriable on a
   * later call rather than permanently wedging `ready` at false forever. */
  bool started;
  _Atomic bool exit_requested;
} g_engine_stop_watcher = {0};

static void *_engine_stop_watcher_fn(void *arg) {
  (void)arg;
  for (;;) {
    semaphore_wait(g_engine_stop_watcher.sem);
    if (atomic_load(&g_engine_stop_watcher.exit_requested)) return NULL;
    _engine_force_stop_now();
  }
}

/* Starts the watcher thread the first time it's needed, retrying on every
 * call until it succeeds (a transient OOM here must not permanently
 * disable chttpsvr_engine_stop()'s ability to ever act again). Must be
 * called with srv_engine_bundler.mutex already held, and only from an
 * ordinary (non-signal) thread -- see _engine_acquire, its one call site.
 * Called as the very first thing _engine_acquire() does after taking the
 * lock, before any of that function's own reactor-creation work: this
 * guarantees the watcher is already live for the remainder of that call
 * and every subsequent srv_engine_bundler.mutex-locking call in the
 * process, closing the self-deadlock window described above. The only
 * residual gap is a signal arriving in the handful of instructions before
 * this function's own first call -- nothing yet holds srv_engine_bundler.
 * mutex at that point (this is the very first statement to take it), so
 * the original self-deadlock cannot occur there either; a stop request
 * arriving in that specific sliver is simply not yet actionable (there is
 * nothing running yet for it to stop) and is treated exactly like any
 * other call to chttpsvr_engine_stop() before the engine has ever started:
 * a documented no-op, not something silently deferred and replayed against
 * a later, unrelated chttpsvr_start() call (a deferred-replay design was
 * tried and rejected here for exactly that reason: it could otherwise
 * resurrect a long-forgotten stop request against a server started much
 * later in the same process). */
static void _engine_stop_watcher_ensure_started_locked(void) {
  if (g_engine_stop_watcher.started) return;
  if (semaphore_init(g_engine_stop_watcher.sem, 0) != 0) return;
  if (thread_create(g_engine_stop_watcher.thread, _engine_stop_watcher_fn,
                    NULL) != 0) {
    semaphore_destroy(g_engine_stop_watcher.sem);
    return;
  }
  g_engine_stop_watcher.started = true;
  atomic_store(&g_engine_stop_watcher.ready, true);
}

/* Joins the watcher thread at process exit so make memtest's valgrind pass
 * does not report it as a "possibly lost" thread stack; mirrors this
 * file's own _cleanup_chttpsvr_router_shells/_cleanup_chttpsvr_slot_table
 * destructor pattern. A plain, unsynchronized read of `started`/`thread`
 * here (no mutex) matches those two destructors' own established
 * precondition: an application is expected to have already quiesced its
 * own use of this library (destroyed every chttpsvr, stopped calling
 * chttpsvr_engine_stop()) before process exit, so no concurrent writer of
 * these two fields remains by the time destructors run. */
__attribute__((destructor)) static void _cleanup_engine_stop_watcher(void) {
  if (!g_engine_stop_watcher.started) return;
  atomic_store(&g_engine_stop_watcher.exit_requested, true);
  semaphore_post(g_engine_stop_watcher.sem);
  thread_join(g_engine_stop_watcher.thread);
  semaphore_destroy(g_engine_stop_watcher.sem);
}

/* The actual public entry point: see g_engine_stop_watcher's own comment
 * above for why this is deliberately reduced to only this one genuinely
 * async-signal-safe operation, with no mutex_lock/thread_create of its
 * own, direct or indirect. `ready == false` (no engine has ever been
 * started in this process, or -- for a vanishingly narrow window -- one is
 * only just now starting for the very first time) is a no-op, exactly
 * matching this function's own documented "safe to call before the
 * reactor has ever started" contract; see
 * _engine_stop_watcher_ensure_started_locked's own comment for why that
 * request is not deferred/replayed later instead. */
static void _engine_force_stop(void) {
  if (atomic_load(&g_engine_stop_watcher.ready))
    semaphore_post(g_engine_stop_watcher.sem);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box regression test hook only: locks srv_engine_bundler.mutex --
 * exactly the critical section chttpsvr_engine_stop()'s own doc comment
 * says a signal handler must be safe to interrupt -- then synchronously
 * delivers `sig` to the calling thread via raise(3), which in a
 * multi-threaded program is equivalent to pthread_kill(pthread_self(),
 * sig) per POSIX: sig's installed handler (if any) runs to completion,
 * still on this same thread, still with the mutex held, before raise()
 * returns. This deterministically reproduces "a signal arrives on a thread
 * that already holds srv_engine_bundler.mutex" without depending on real
 * timing. If the caller's handler for `sig` calls chttpsvr_engine_stop()
 * and the async-signal-safety fix (see g_engine_stop_watcher above) were
 * ever to regress back to calling mutex_lock/thread_create directly, this
 * call would deadlock, exactly as a real SIGTERM handler installed per
 * this module's own documented usage pattern would. Gated so neither this
 * function nor its behavior exists in a production build. */
void _chttpsvr_test_hold_engine_mutex_and_signal_self(int sig) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  raise(sig);
  mutex_unlock(srv_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP (module-local, own thread)           */
/* ========================================================================== */

static void _conn_close(chttpsvr_conn_t *conn);
static void _conn_reject_and_close(chttpsvr_conn_t *conn, unsigned timeout_ms);

/* Registers a server so the idle sweep thread walks its idle-connection
 * list too. Called from chttpsvr_start (which runs not just once per
 * server but again on every stop/start restart cycle) so this must be
 * idempotent: skip the add if srv is already present. Without this check,
 * a srv that goes through N restart cycles ends up in
 * servers_bundler.servers N+1 times, and _servers_unregister's
 * single-occurrence removal (see below) would leave N stale, dangling pointers
 * behind after __chttpsvr_destroy frees srv; a real use-after-free the idle
 * sweep thread would read on its very next pass, caught by valgrind via
 * restart_races_live_keep_alive_connection_is_safe (which restarts one srv
 * 5 times before destroying it). */
static void _servers_register(struct chttpserver *srv) {
  mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      mutex_unlock(servers_bundler.mutex);
      return;
    }
  }
  if (servers_bundler.count == servers_bundler.capacity) {
    size_t new_cap =
        servers_bundler.capacity ? servers_bundler.capacity * 2 : 8;
    struct chttpserver **nn = (struct chttpserver **)realloc(
        servers_bundler.servers, new_cap * sizeof(struct chttpserver *));
    if (nn) {
      servers_bundler.servers = nn;
      servers_bundler.capacity = new_cap;
    }
  }
  if (servers_bundler.count < servers_bundler.capacity)
    servers_bundler.servers[servers_bundler.count++] = srv;
  mutex_unlock(servers_bundler.mutex);
}

static void _servers_unregister(struct chttpserver *srv) {
  mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      servers_bundler.servers[i] =
          servers_bundler.servers[servers_bundler.count - 1];
      servers_bundler.count--;
      break;
    }
  }
  mutex_unlock(servers_bundler.mutex);
}

#define _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS 1000
/* Soft cap on how many idle connections one collection pass claims off of
 * srv->idle_head before releasing srv->idle_mutex; bounds the stack-
 * allocated to_close[] scratch buffer both _idle_sweep_fn and
 * _close_all_idle_connections below use to move closing (which must not
 * run under idle_mutex; see either function's own comment) outside the
 * locked section. Self-healing in _idle_sweep_fn (anything beyond this on
 * one tick is simply left in the list, oldest-first, and caught on a later
 * tick); _close_all_idle_connections instead wraps this same batch size in
 * an outer loop to fully drain the list regardless. */
#define _CHTTPSVR_IDLE_CLOSE_BATCH 64

static void *_idle_sweep_fn(void *arg) {
  (void)arg;
  while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
    struct timespec ts = {
        .tv_sec = _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS / 1000,
        .tv_nsec = (_CHTTPSVR_IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    if (atomic_load(&idle_sweep_bundler.stop_flag)) break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    mutex_lock(servers_bundler.mutex);
    for (size_t i = 0; i < servers_bundler.count; i++) {
      struct chttpserver *srv = servers_bundler.servers[i];
      unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
      if (!idle_ms) continue;

      /* Collect timed-out connections under idle_mutex, claiming (removing)
       * each one from the list right here rather than merely copying its
       * pointer out, then close them outside the lock: _conn_close
       * ultimately calls event_loop_remove, which must not run while
       * holding a lock a callback dispatched from that same removal could
       * also need. Claiming at collection time (not just after, in a
       * separately-locked pass) is what stops a connection from being
       * simultaneously "found here" and dispatched to _conn_pump on a
       * reactor thread; see _idle_list_try_claim's own doc comment for the
       * real use-after-free (a freed SSL* read concurrently by
       * ctls_conn_read) this closes. */
      chttpsvr_conn_t *to_close[_CHTTPSVR_IDLE_CLOSE_BATCH];
      size_t to_close_n = 0;
      mutex_lock(srv->idle_mutex);
      chttpsvr_conn_t *c = srv->idle_head;
      while (c && to_close_n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
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
        _SRV_ENGINE_LOG(log_info, "idle-timeout closing %zu connection(s)",
                        to_close_n);
      }
      for (size_t j = 0; j < to_close_n; j++) _conn_close(to_close[j]);
    }
    mutex_unlock(servers_bundler.mutex);
  }
  return NULL;
}

static void _idle_sweep_start_if_needed(void) {
  mutex_lock(servers_bundler.mutex);
  if (!idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, false);
    if (thread_create(idle_sweep_bundler.thread, _idle_sweep_fn, NULL) == 0)
      idle_sweep_bundler.running = true;
  }
  mutex_unlock(servers_bundler.mutex);
}

/* Stops and joins the idle sweep thread, if one is running. Tied to the
 * shared engine's own lifetime (called from the engine reaper, alongside
 * event_loop_destroy) rather than to any single server's stop/destroy,
 * since the sweep thread walks every registered server, not one; an
 * unjoined sweep thread still running at process exit is exactly the class
 * of "possibly lost" glibc TLS (allocate_dtv) false positive this
 * codebase's other reaper threads are already documented to close (see
 * this module's own reaper thread above). */
static void _idle_sweep_stop_if_running(void) {
  bool was_running = false;
  thread_id_t t = {0};
  mutex_lock(servers_bundler.mutex);
  if (idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, true);
    t = idle_sweep_bundler.thread;
    was_running = true;
    idle_sweep_bundler.running = false;
  }
  mutex_unlock(servers_bundler.mutex);
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

/* Adds conn to srv's diverted-connection registry (idempotent: a no-op if
 * already present, exactly like _idle_list_add's own idempotence), so a
 * teardown path can find and forcibly shutdown(2) its fd if this connection
 * turns out to be stuck in a worker thread's blocking I/O past the normal
 * graceful drain window; see struct chttpserver's own diverted_mutex/
 * diverted_head/diverted_tail comment and _wait_in_flight_bounded. Called
 * from _conn_start_diverted, every time a request (initial or a further
 * pipelined one on the same connection) is handed to a worker thread. */
static void _diverted_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  mutex_lock(srv->diverted_mutex);
  if (!conn->in_diverted_list) {
    conn->diverted_prev = srv->diverted_tail;
    conn->diverted_next = NULL;
    if (srv->diverted_tail) srv->diverted_tail->diverted_next = conn;
    srv->diverted_tail = conn;
    if (!srv->diverted_head) srv->diverted_head = conn;
    conn->in_diverted_list = true;
  }
  mutex_unlock(srv->diverted_mutex);
}

/* Removes conn from srv's diverted-connection registry (idempotent: a no-op
 * if not present). Called both when a worker thread is done with conn for
 * good (_conn_free, covering every close/reject path) and when it is handed
 * back to the reactor for keep-alive (_task_worker's keep-alive tail,
 * before conn is no longer worker-owned and therefore no longer a candidate
 * for the forced-shutdown mechanism this registry exists for). */
static void _diverted_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  mutex_lock(srv->diverted_mutex);
  if (conn->in_diverted_list) {
    if (conn->diverted_prev)
      conn->diverted_prev->diverted_next = conn->diverted_next;
    if (conn->diverted_next)
      conn->diverted_next->diverted_prev = conn->diverted_prev;
    if (srv->diverted_head == conn) srv->diverted_head = conn->diverted_next;
    if (srv->diverted_tail == conn) srv->diverted_tail = conn->diverted_prev;
    conn->diverted_prev = conn->diverted_next = NULL;
    conn->in_diverted_list = false;
  }
  mutex_unlock(srv->diverted_mutex);
}

/* Forcibly interrupts every connection currently in srv's diverted-
 * connection registry by shutdown(2)-ing its raw fd (SHUT_RDWR): this
 * unblocks whichever chttp1_stream_read/_write call a stuck worker thread
 * is inside with an ordinary I/O error, the exact same path a real peer
 * disconnect already takes (see e.g. client_disconnect_mid_body_does_not_
 * hang_server), letting that worker's own existing error handling finish
 * the request and release it normally. shutdown(), not close(): the worker
 * thread (never this one) still owns the fd and is the only one that may
 * actually close(2) it (via _conn_free), so closing it here would risk the
 * classic close-from-another-thread fd-reuse race; shutdown() has no such
 * hazard; it only affects in-progress and future I/O on this exact fd,
 * leaving the fd number itself allocated until the owning thread closes it.
 * Called only once srv->mutex has been released (see _wait_in_flight_bounded)
 * and only after the normal, bounded, graceful wait for in-flight requests
 * has already been exhausted; see that function's own comment for why this
 * exists at all. Best-effort: shutdown()'s return value is not checked (an
 * already-closed or otherwise invalid fd is simply a harmless no-op). */
static void _force_unblock_diverted_connections(struct chttpserver *srv) {
  mutex_lock(srv->diverted_mutex);
  for (chttpsvr_conn_t *c = srv->diverted_head; c; c = c->diverted_next)
    shutdown(c->fd, SHUT_RDWR);
  mutex_unlock(srv->diverted_mutex);
}

/* Attempts to atomically claim conn out of the idle list for exclusive
 * processing (either dispatching a real I/O event for it, or closing it).
 * Returns true if conn was idle and has now been removed; the caller has
 * sole ownership and may safely read/free conn. Returns false if conn was
 * NOT in the idle list; someone else (another dispatch, or a concurrent
 * closer) already claimed it, and the caller must not touch conn at all.
 *
 * This is the one piece of synchronization that makes it safe for
 * _close_all_idle_connections/_idle_sweep_fn to free a connection found in
 * the idle list from a thread that is not the reactor: without it, a
 * connection could be simultaneously "found idle" by a closer AND
 * dispatched to _conn_pump on a reactor thread (new data having arrived in
 * the same instant), and the closer's _conn_free (which for a TLS
 * connection calls ctls_conn_destroy -> SSL_free) could run concurrently
 * with _conn_pump's ctls_conn_read on the very same SSL*; a real
 * use-after-free valgrind caught (a segfault deep in libcrypto's BIO code,
 * reproduced only against tests_tls.c, since the plaintext path's
 * equivalent race is a same-shape but much less immediately fatal
 * "read()/close() lost a race" instead of freeing live OpenSSL state).
 *
 * A connection is only ever added to the idle list AFTER the corresponding
 * event_loop_add/_modify call that makes it dispatchable has already
 * returned (see _conn_pump's handshake-wait branch and the main read
 * loop's EWOULDBLOCK branch, and _task_worker's keep-alive re-arm); so a
 * dispatch can, in a narrow window, fire before the idle-list add has
 * happened yet and see try_claim fail here. That is harmless, not a bug:
 * this module always uses level-triggered epoll, so a spurious "not idle
 * yet" claim failure just means the same readiness is reported again on
 * the very next epoll_wait, by which point the add has long since
 * completed (a handful of instructions on the same thread, no I/O in
 * between); never a dropped or hung request. */
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
 * connection nothing has used again yet); called from __chttpsvr_destroy,
 * after chttpsvr_stop() has closed the listener so no new connection can
 * arrive. Without this, a keep-alive connection a test client never
 * explicitly closed (the common case: chttpclient's own idle pool keeps a
 * connection open after a response, not closed) outlives the server that
 * accepted it, leaking its chttpsvr_conn_t (and everything it owns: path,
 * headers, route match state, ...) for the rest of the process's life;
 * caught by valgrind as a real "definitely lost" block traced back to
 * _conn_create/_listener_on_readable, not a false positive.
 *
 * Each candidate is claimed (removed from the idle list) at collection
 * time, under idle_mutex, via _idle_list_try_claim (not merely copied
 * out and closed afterward) so a connection can never be simultaneously
 * "found here" and "dispatched to _conn_pump on a reactor thread"; see
 * _idle_list_try_claim's own doc comment for the use-after-free this
 * closes. */
static void _close_all_idle_connections(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *to_close[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t to_close_n = 0;
    mutex_lock(srv->idle_mutex);
    chttpsvr_conn_t *c = srv->idle_head;
    while (c && to_close_n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
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

/* Plain RFC 3986 %XX decoding, with '+' either left literal (path semantics)
 * or turned into a space (query-string semantics). Safe for in-place
 * decoding (dst == src): the write index never overtakes the read index.
 * Returns the decoded length, or -1 on malformed percent-encoding (a '%' not
 * followed by two hex digits, or a %XX sequence that decodes to a literal
 * NUL byte).
 *
 * The NUL rejection matters beyond this function's own contract: every
 * caller of this decoder treats its output as a NUL-terminated C string
 * (chttpsvr_req_path()'s public "NUL-terminated" contract; _seg_matches_
 * literal's strcmp against a registered route/prefix segment; the query
 * key/value strcmp in chttpsvr_req_query[_one]). strcmp and friends stop at
 * the first NUL byte they see, not at this function's own returned length,
 * so a decoded embedded NUL would silently truncate the comparison: a path
 * segment like "foo%00bar" would decode to "foo\0bar" and compare equal to
 * the literal route segment "foo" via strcmp, letting an attacker-controlled
 * suffix hide behind a route match (and behind chttpsvr_req_path()'s own
 * return value) that a caller reasonably expects to reflect the whole
 * segment. Treating a decoded NUL as malformed input (returning -1, exactly
 * like a bad %XX escape) routes it through the same, already-correct
 * "decode failure -> this segment/path does not match" handling every other
 * caller of this function already relies on, rather than leaving it to
 * silently corrupt a strcmp-based comparison further down. */
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
      char decoded = (char)((hi << 4) | lo);
      if (decoded == '\0') return -1;
      dst[di++] = decoded;
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
static void _reject_task(void *arg);
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
        /* A pattern reusing the same {name} more than once (e.g.
         * "/a/{id}/b/{id}") would otherwise compile successfully and then
         * silently make the second occurrence's captured value unreachable:
         * chttpsvr_req_param always returns on the FIRST name match. Reject
         * this at registration time instead of letting it surface only as a
         * confusing wrong value at request time. */
        for (int j = 0; j < pi; j++) {
          if (strcmp(param_names[j], param_names[pi]) == 0) {
            for (int k = 0; k <= pi; k++) _mem_free(mp, param_names[k]);
            _mem_free(mp, param_names);
            for (int k = 0; k < seg_count; k++) _mem_free(mp, segs[k]);
            _mem_free(mp, segs);
            return ccol_invalid_args;
          }
        }
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

/* chttpsvr_config_t.stream_read_timeout_ms/response_write_timeout_ms are
 * documented "0 = wait indefinitely", but chttp1_stream_read/_write follow
 * poll(2)'s own convention instead: negative = block forever, 0 = a single
 * non-blocking attempt (no waiting at all), positive = bounded in ms.
 * Passing a configured 0 straight through as their own int timeout_ms
 * therefore means the opposite of what was configured: not "forever", but
 * "give up immediately, every single call, without even trying" (chttp1_
 * stream_read/_write's own deadline computation calls clock_gettime() once
 * to set a deadline of "now" for timeout_ms==0, then immediately
 * re-checks elapsed time via a second clock_gettime() call before ever
 * calling poll(); on a monotonic clock that second reading can only be >=
 * the first, so the "already expired" branch fires unconditionally and
 * poll()/the real read or write is never even attempted). Confirmed via a
 * standalone repro against the built library: with stream_read_timeout_ms
 * == 0, chttpsvr_req_read() returned -1 before the client's body bytes
 * were ever sent, and even a bodyless GET never received a response at
 * all, since response_write_timeout_ms's own "0 = use stream_read_timeout_
 * ms's value" default silently inherited the same broken 0. Every timeout
 * value sourced from this module's own config must be translated through
 * this helper before reaching chttp1_stream_read/_write, or "wait
 * indefinitely" silently becomes "never wait at all".
 *
 * Separately, a configured value > INT_MAX (chttp1_stream_read/_write's
 * timeout_ms parameter, and poll(2)'s own, are both a plain int) would
 * silently wrap to a negative value on the (int) cast below -- on every
 * mainstream two's-complement target this codebase builds for, that
 * negative value is itself poll(2)'s own "block forever" convention, so an
 * implausible but not inherently invalid config value (e.g.
 * stream_read_timeout_ms just over 24.8 days in ms) would silently become
 * "wait indefinitely" instead of the finite, merely very long, wait that
 * was actually configured. Clamped to INT_MAX instead: still the longest
 * finite wait this API can express (chttp1_stream_read/_write have no wider
 * type to hand a longer one through even if this helper computed it), and
 * unlike the wraparound it does not collapse into the *other* documented
 * sentinel this same function already treats specially (0 -> -1 above). */
static int _to_stream_timeout_ms(unsigned configured_timeout_ms) {
  if (configured_timeout_ms == 0) return -1;
  if (configured_timeout_ms > (unsigned)INT_MAX) return INT_MAX;
  return (int)configured_timeout_ms;
}

/* Small growable buffer used to assemble one response's header block;
 * starts on the stack (the common case: a handful of ordinary headers) and
 * falls back to a heap allocation, grown via doubling exactly like
 * chttpsvr_resp_write's own body buffer, whenever a handler's headers push
 * the block past the stack reservation. Without this, a handler that sets
 * enough headers (or one large header value: several Set-Cookie headers,
 * CORS/CSP headers, a long custom token, ...) to cross a fixed-size stack
 * buffer would silently lose the entire response: the old fixed-size-buffer
 * version of this code failed outright the moment the block didn't fit,
 * closing the connection with zero bytes ever written and nothing logged,
 * for a request that had already produced a perfectly valid response. */
typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  bool heap;
  ccol_memmgmt_procs_t *mp;
} _head_builder_t;

static bool _head_builder_grow(_head_builder_t *b, size_t needed_extra) {
  size_t min_cap = b->len + needed_extra;
  if (min_cap < b->len) return false; /* size_t overflow */
  size_t new_cap = b->cap ? b->cap : 1;
  while (new_cap < min_cap) {
    if (new_cap > SIZE_MAX / 2) {
      new_cap = min_cap;
      break;
    }
    new_cap *= 2;
  }
  char *nb = (char *)_mem_alloc(b->mp, new_cap);
  if (!nb) return false;
  memcpy(nb, b->buf, b->len);
  if (b->heap) _mem_free(b->mp, b->buf);
  b->buf = nb;
  b->cap = new_cap;
  b->heap = true;
  return true;
}

/* Appends fmt/... to b, growing b as many times as needed (never failing
 * due to sheer size the way a fixed-capacity buffer would); fails only on a
 * genuine allocation failure or a vsnprintf encoding error. */
static bool _head_builder_append(_head_builder_t *b, const char *fmt, ...) {
  for (;;) {
    size_t avail = b->cap - b->len;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(b->buf + b->len, avail, fmt, ap);
    va_end(ap);
    if (n < 0) return false;
    if ((size_t)n < avail) {
      b->len += (size_t)n;
      return true;
    }
    if (!_head_builder_grow(b, (size_t)n + 1)) return false;
    /* Retry the same append now that b has enough room. */
  }
}

static void _head_builder_release(_head_builder_t *b) {
  if (b->heap) _mem_free(b->mp, b->buf);
}

/* Serializes resp into a raw HTTP/1.1 response and writes it to stream,
 * bounded by response_write_timeout_ms. Always sets Content-Length
 * explicitly (this server never uses chunked transfer-encoding for its own
 * responses) and a Connection header reflecting keep_alive. Returns false on
 * a write error/timeout (caller must then treat the connection as unusable
 * and close it). The header block itself has no size limit: see
 * _head_builder_t above.
 *
 * Unlike every other header (including Content-Length; see below), a
 * handler-supplied "Connection" header set via chttpsvr_resp_set_header is
 * never emitted: keep_alive is always the one and only source of truth for
 * the wire value, since it is also what the caller (_task_worker) uses right
 * after this call returns to decide whether the connection is actually kept
 * open. Letting a handler's own Connection header reach the wire independently
 * of that decision would let the two silently diverge -- a wire response
 * claiming "keep-alive" while the socket is closed immediately after, or vice
 * versa -- which is exactly the kind of framing bug a caller has no way to
 * detect or recover from.
 *
 * suppress_body: true for a HEAD request (RFC 7231 SS4.3.2: a HEAD response
 * MUST NOT include a message body, but SHOULD report the same header fields,
 * Content-Length included, a GET would have). Content-Length is still
 * computed from resp->body_len as usual; only the actual body bytes are
 * withheld from the wire. Without this, a spec-compliant client that
 * correctly stops reading after headers on a HEAD response would leave the
 * unwritten body bytes sitting in the socket buffer, where they would be
 * misparsed as the start of the next pipelined response on a keep-alive
 * connection.
 *
 * The exact same hazard exists independently of suppress_body/HEAD for a
 * 1xx, 204, or 304 status (RFC 9110 SS6.4.1/SS15.2.1/SS15.4.5: none of the
 * three may ever carry a body, regardless of method): this codebase's own
 * chttp1_parser response-parsing side already treats all three as bodyless
 * unconditionally (see its own CHTTP1_ST_HEADERS handling), so any client
 * honoring that same rule, including chttpclient, would misparse a body
 * this function actually wrote as the start of the next pipelined response.
 * A handler that sets one of these three statuses after (or instead of)
 * writing a body via chttpsvr_resp_write is far more likely to be an
 * oversight than an intentional non-compliant response, so this is
 * suppressed unconditionally here rather than left as the handler's own
 * responsibility. A 1xx or 204 additionally MUST NOT carry a Content-Length
 * header at all (unlike HEAD/304, where reporting one matching what an
 * equivalent GET would have sent is expected/permitted); the auto-injection
 * below is skipped for those two specifically, though an explicit
 * Content-Length the caller already set of their own accord is still never
 * stripped, matching this function's existing treatment of every other
 * caller-supplied header. */
static bool _send_response(chttp1_stream_t *stream, chttpsvr_resp *resp,
                           bool keep_alive, unsigned timeout_ms,
                           bool suppress_body) {
  char stack_buf[4096];
  _head_builder_t hb = {.buf = stack_buf,
                        .cap = sizeof(stack_buf),
                        .len = 0,
                        .heap = false,
                        .mp = resp->m_procs};

  int status = (resp->status_code >= 100 && resp->status_code <= 999)
                   ? resp->status_code
                   : 500;
  bool status_is_1xx = status >= 100 && status < 200;
  bool no_body =
      suppress_body || status_is_1xx || status == 204 || status == 304;
  bool may_report_length = !status_is_1xx && status != 204;

  if (!_head_builder_append(&hb, "HTTP/1.1 %d %s\r\n", status,
                            _status_reason(status))) {
    _head_builder_release(&hb);
    return false;
  }

  bool have_content_length = false;
  for (size_t i = 0; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, "content-length") == 0) {
      have_content_length = true;
      break;
    }
  }

  for (size_t i = 0; i < resp->header_count; i++) {
    /* "connection" is never re-emitted from resp->headers, regardless of
     * whether a handler set one: keep_alive below is the single source of
     * truth for whether this connection is actually kept open afterward
     * (see _task_worker's own identically-computed keep_alive, used for the
     * real post-response decision), and letting a handler-supplied value
     * reach the wire independently of that decision let the two silently
     * diverge -- e.g. a handler setting "Connection: keep-alive" while the
     * server's own framing/timeout logic had already decided to close right
     * after, telling the client the opposite of what actually happens. This
     * function is the one place that both decides and announces connection
     * reuse, so it must be the only writer of this header. */
    if (strcasecmp(resp->headers[i].name, "connection") == 0) continue;
    /* No space after the colon: this test suite's raw-socket assertions
     * (e.g. strstr(buf, "connection:close")) assert on this exact wire
     * format directly, so the format must be produced byte for byte. */
    if (!_head_builder_append(&hb, "%s:%s\r\n", resp->headers[i].name,
                              resp->headers[i].value)) {
      _head_builder_release(&hb);
      return false;
    }
  }

  if (!have_content_length && may_report_length) {
    if (!_head_builder_append(&hb, "content-length:%zu\r\n", resp->body_len)) {
      _head_builder_release(&hb);
      return false;
    }
  }
  {
    const char *cval = keep_alive ? "keep-alive" : "close";
    if (!_head_builder_append(&hb, "connection:%s\r\n", cval)) {
      _head_builder_release(&hb);
      return false;
    }
  }
  if (!_head_builder_append(&hb, "\r\n")) {
    _head_builder_release(&hb);
    return false;
  }

  int stream_timeout_ms = _to_stream_timeout_ms(timeout_ms);
  size_t sent = 0;
  bool head_sent = true;
  while (sent < hb.len) {
    ssize_t n2 = chttp1_stream_write(stream, hb.buf + sent, hb.len - sent,
                                     stream_timeout_ms);
    if (n2 <= 0) {
      head_sent = false;
      break;
    }
    sent += (size_t)n2;
  }
  _head_builder_release(&hb);
  if (!head_sent) return false;
  if (no_body) return true;

  sent = 0;
  while (sent < resp->body_len) {
    ssize_t n3 = chttp1_stream_write(stream, resp->body + sent,
                                     resp->body_len - sent, stream_timeout_ms);
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
   * the pointer; _conn_free (final teardown) already does this correctly
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
  conn->interim_continue_sent = false;
  _mem_free(mp, conn->body.buf);
  memset(&conn->body, 0, sizeof(conn->body));
  conn->body_bytes_seen = 0;
  conn->body_too_large = false;
  conn->transfer_aborted = false;
  conn->read_deadline_set = false;
  conn->deadline_exceeded = false;

  /* Re-initialised directly on conn->parser, in place, rather than building
   * a fresh chttp1_parser_t on the stack and struct-assigning it over
   * conn->parser afterward: chttp1_parser_t embeds an 8192-byte line_buf, so
   * the old two-step version paid for that buffer's size TWICE on every
   * single keep-alive request (once zeroing it on the stack inside
   * chttp1_parser_init_request, again copying it stack-to-heap via the
   * struct assignment) on what is this server's hottest per-request path.
   * Initialising in place still pays the first cost (chttp1_parser_init_
   * request's own memset) but not the second. conn->parser.settings is read
   * into the function-call argument before chttp1_parser_init_request's
   * internal memset(conn->parser, 0, ...) ever runs, since C evaluates a
   * call's arguments before entering the callee body, so this is not a
   * use-after-clear of the very value being passed in. */
  const chttp1_settings_t *settings = conn->parser.settings;
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  size_t max_hdr_bytes = atomic_load(&conn->srv->max_header_bytes);
  if (max_hdr_bytes) {
    conn->parser.max_header_count_override = 0;
    conn->parser.max_total_header_bytes_override = max_hdr_bytes;
  }
  /* Bounds a single chunk's declared size to no more than the whole request
   * body is allowed to be, checked by chttp1_parser as soon as a chunk-size
   * line is parsed rather than only reactively once that many bytes have
   * actually arrived (see _on_body's own max_body_size check, and the
   * analogous Content-Length upfront check in _on_headers_complete below).
   * A single, absurdly large declared chunk (any value up to UINT64_MAX is
   * a syntactically valid chunk-size token) that the peer then never
   * actually sends would otherwise tie up a worker thread until
   * stream_read_timeout_ms/max_body_read_duration_ms fired, since nothing
   * else ever crosses a byte-count-based limit if the bytes never arrive.
   * Left at its default (0, "no cap") only in the narrow corner where
   * max_body_size itself is configured to exactly 0 -- 0 is this field's
   * own "no cap" sentinel (see its doc comment), so it cannot also express
   * "cap at zero" -- in which case the very first body byte the peer does
   * send (for any chunk) is still caught immediately by the existing
   * reactive _on_body check; only a chunk that declares a nonzero size and
   * then sends literally none of it stays unbounded by this specific cap in
   * that one corner, a low-value target not worth the extra complexity of
   * a second sentinel to close. */
  conn->parser.max_chunk_size_override =
      (uint64_t)atomic_load(&conn->srv->max_body_size);
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
   * resp.m_procs, reject_status, the header-size override, ...); a freshly
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
  _diverted_list_remove(conn);
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
  /* Defense in depth, not a fix for a live leak: every call path that ever
   * sets conn->_carry_over (_conn_start_diverted, _task_worker's keep-alive
   * tail) already frees and NULLs it itself before conn can reach this
   * function, so this is a no-op on every path exercised today. It exists so
   * that a future rejection/close path added without perfect knowledge of
   * that discipline fails safe (a no-op free of a NULL pointer) rather than
   * silently leaking the carry-over buffer with nothing left to catch it. */
  _mem_free(mp, conn->_carry_over);
  _mem_free(mp, conn);
}

static void _conn_close(chttpsvr_conn_t *conn) {
  if (conn->reg) {
    event_loop_remove(srv_engine_bundler.reactor, conn->reg);
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

  /* CHTTP_ANY is a registration-time-only placeholder ("match any concrete
   * method"); it is never a real incoming request's method, and neither is
   * _CHTTP_METHOD_UNKNOWN (the sentinel _parse_method returns for a
   * syntactically valid but unrecognized method token, e.g. a WebDAV verb
   * like PROPFIND, TRACE, CONNECT, or a custom verb). Without this check, a
   * CHTTP_ANY-registered route's method_ok test in _find_route
   * (route->method == CHTTP_ANY) short-circuits to true regardless of the
   * actual method, so such a request would reach the handler anyway;
   * chttpsvr_req_method() has no way to report anything back to it
   * other than one of the seven named chttp_method_t constants, silently
   * handing the handler a meaningless sentinel value instead of "the actual
   * method of the incoming request" its own doc comment in chttp.h promises.
   * Rejected here, before path/header parsing or route matching ever runs
   * (this server does not implement any method outside the seven it
   * recognizes), as 501 (RFC 7231 SS6.6.2: "the server does not support the
   * functionality required to fulfill the request"), through the same
   * reject_pool machinery every other rejection (404/405/500) already uses;
   * see _conn_pump's CHTTP1_USER handling and _conn_dispatch_reject. */
  if (conn->method == _CHTTP_METHOD_UNKNOWN) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_NOT_IMPLEMENTED;
    return 1;
  }

  const char *qmark = (const char *)memchr(target, '?', target_len);
  size_t path_raw_len = qmark ? (size_t)(qmark - target) : target_len;

  /* Stored RAW (still percent-encoded); see conn->path's own doc comment
   * for why this must not be decoded here. An allocation failure here is
   * reported the same way every other mid-request OOM in this file is
   * (req_rejected + reject_status, routed through reject_pool for a
   * graceful 500): a bare `return 1` with neither field set still aborts
   * the parse with CHTTP1_USER, but _conn_feed_bytes only recognizes that
   * as an already-decided rejection when req_rejected is set, so leaving it
   * unset here silently dropped the connection with no response at all on
   * transient OOM, unlike the identical failure mode _on_headers_complete
   * already handles gracefully a few callbacks later. */
  char *path_raw = (char *)_mem_alloc(conn->m_procs, path_raw_len + 1);
  if (!path_raw) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return 1;
  }
  memcpy(path_raw, target, path_raw_len);
  path_raw[path_raw_len] = '\0';
  conn->path = path_raw;

  if (qmark) {
    size_t qlen = target_len - path_raw_len - 1;
    conn->raw_query = (char *)_mem_alloc(conn->m_procs, qlen + 1);
    if (!conn->raw_query) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    memcpy(conn->raw_query, qmark + 1, qlen);
    conn->raw_query[qlen] = '\0';
  }
  return 0;
}

static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  /* Every OOM return below sets req_rejected/reject_status before
   * returning, exactly like _on_request_line's identical allocation-failure
   * handling right above it in this file; see that function's own comment
   * for why (a bare `return 1` with neither field set silently drops the
   * connection with no response, instead of the graceful 500 every other
   * mid-request OOM in this file already produces). */
  if (conn->hdr_count >= conn->hdr_cap) {
    size_t new_cap = conn->hdr_cap ? conn->hdr_cap * 2 : 8;
    char **nn = (char **)_mem_realloc(conn->m_procs, conn->hdr_names,
                                      new_cap * sizeof(char *));
    if (!nn) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_names = nn;
    char **nv = (char **)_mem_realloc(conn->m_procs, conn->hdr_values,
                                      new_cap * sizeof(char *));
    if (!nv) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_values = nv;
    conn->hdr_cap = new_cap;
  }
  char *n = (char *)_mem_alloc(conn->m_procs, name_len + 1);
  char *v = (char *)_mem_alloc(conn->m_procs, value_len + 1);
  if (!n || !v) {
    _mem_free(conn->m_procs, n);
    _mem_free(conn->m_procs, v);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
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

  /* Reject a buffered route's request immediately, before ever diverting it
   * to a worker thread, when the declared Content-Length already exceeds
   * max_body_size -- rather than only reactively, once that many bytes have
   * actually streamed in (see _on_body's own check). Without this, a peer
   * that declares an oversized Content-Length and then simply never sends
   * the body ties up a worker thread until a read timeout fires, since
   * nothing ever crosses a byte-count-based limit if the bytes never
   * arrive; see chttp1_declared_content_length's own doc comment. Streaming
   * routes are deliberately excluded: this module's documented contract for
   * them (chttpsvr_config_t.max_body_size's own doc comment) is that the
   * handler is ALWAYS invoked and decides its own response via
   * chttpsvr_req_read()/chttpsvr_req_stream_error(); auto-rejecting here
   * would silently break that contract. A chunked body's oversized-CHUNK
   * analogue of this same protection is instead enforced uniformly for
   * both route kinds via max_chunk_size_override (see
   * _conn_reset_for_request), since that mechanism does not carry this same
   * "handler always runs" contract to preserve -- it surfaces through the
   * ordinary chttp1_parser_execute() error path _drain_body/
   * chttpsvr_req_read already handle, exactly like any other malformed body. */
  if (!mr.route->is_streaming && chttp1_has_content_length(p)) {
    size_t limit = atomic_load(&srv->max_body_size);
    if (chttp1_declared_content_length(p) > (uint64_t)limit) {
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_PAYLOAD_TOO_LARGE;
      return -1;
    }
  }

  /* A successful match already proved every segment of conn->path (RAW,
   * still percent-encoded) decodes cleanly; _match_segments/
   * _seg_matches_literal treat any decode failure as a non-match, which
   * would have taken the ROUTE_MATCH_NONE/ROUTE_MATCH_METHOD branch above
   * instead of reaching here. Decoding the whole path now, for the public
   * chttpsvr_req_path() accessor, therefore cannot fail on malformed input;
   * only a real allocation failure can -- rejected with 500 here, before
   * mr.param_values is transferred to conn, exactly like every other OOM
   * check in this function (route-match OOM, mw-snapshot overflow above),
   * rather than silently diverting a request to a worker with
   * conn->decoded_path left NULL: chttpsvr_req_path()'s own doc comment
   * promises a pointer "valid for the lifetime of the request" for any
   * non-NULL req, with no documented NULL case beyond req itself being
   * NULL, so a handler calling e.g. strlen() on it unconditionally (a
   * reasonable thing to do given that contract) would crash. */
  size_t plen = strlen(conn->path);
  char *dp = (char *)_mem_alloc(srv->m_procs, plen + 1);
  if (dp) {
    ssize_t dlen = _decode_path_unsafe(dp, conn->path);
    if (dlen >= 0) {
      dp[(size_t)dlen] = '\0';
    } else {
      _mem_free(srv->m_procs, dp);
      dp = NULL;
    }
  }
  if (!dp) {
    _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }

  conn->matched_router = mr.router;
  conn->matched_route = mr.route;
  conn->matched_param_values = mr.param_values;
  conn->expects_continue = chttp1_expects_continue(p);
  conn->decoded_path = dp;

  /* Always divert: every matched route (buffered or streaming) is handed to
   * the worker pool regardless of body size, exactly matching this
   * module's existing documented threading model. chttp1_parser itself
   * downgrades this to an ordinary immediate completion when there turns
   * out to be no body to divert (see CHTTP1_HEADERS_DIVERT_BODY's own doc
   * comment); either way, the header-read callback below submits to the
   * worker pool once execute() returns CHTTP1_HEADERS_ONLY or CHTTP1_PAUSED. */
  return CHTTP1_HEADERS_DIVERT_BODY;
}

/* Buffered-route body accumulation, and streaming-route "pending, not yet
 * delivered to chttpsvr_req_read" body accumulation, share this one
 * callback and one growbuf_t: max_body_size is enforced here, uniformly,
 * for both route kinds, since chttp1_parser has no built-in notion of it
 * and leaves that enforcement to the caller. */
static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  conn->body_bytes_seen += len;
  if (conn->body_bytes_seen > atomic_load(&conn->srv->max_body_size)) {
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
    /* Overflow-safe growth, matching _head_builder_grow's own pattern: a
     * plain "keep doubling" loop can wrap new_cap to 0 (an infinite loop,
     * since 0 can never reach min_cap by doubling) if max_body_size is
     * configured close to SIZE_MAX, letting b->len+len approach it too. */
    if (len > SIZE_MAX - b->len) return 1; /* size_t overflow */
    size_t min_cap = b->len + len;
    size_t new_cap = b->cap ? b->cap : 8192;
    while (new_cap < min_cap) {
      if (new_cap > SIZE_MAX / 2) {
        new_cap = min_cap;
        break;
      }
      new_cap *= 2;
    }
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

static struct {
  chttp1_settings_t settings;
  once_flag_t once;
} srv_parser_bundler = {0};

static void _init_parser_settings(void) {
  chttp1_settings_init(&srv_parser_bundler.settings);
  srv_parser_bundler.settings.on_request_line = _on_request_line;
  srv_parser_bundler.settings.on_header = _on_header;
  srv_parser_bundler.settings.on_headers_complete = _on_headers_complete;
  srv_parser_bundler.settings.on_body = _on_body;
  srv_parser_bundler.settings.on_message_complete = _on_message_complete;
}

/* ========================================================================== */
/*                    REACTOR-THREAD READ/HANDSHAKE CALLBACKS                 */
/* ========================================================================== */

/* Decrements srv->in_flight_requests and, if it reaches zero, wakes any
 * thread waiting in _drain_and_close_all_connections/_wait_and_detach_pools.
 * Every path that increments in_flight_requests (_conn_start_diverted) must
 * call this exactly once, and -- critically -- only once the connection has
 * reached a state _drain_and_close_all_connections can actually observe:
 * fully closed via _conn_close (including via _conn_reject_and_close), or
 * safely published back into the idle list via _idle_list_add. Calling this
 * any earlier (e.g. before a keep-alive connection's registration has
 * actually been resumed/re-added and idle-listed, or before a rejected
 * connection's courtesy response has actually been written and the
 * connection closed) opens a real window where the waiter can wake,
 * observe in_flight_requests == 0, and let __chttpsvr_destroy proceed to
 * free srv while this connection is still being finished on another
 * thread -- a genuine use-after-free a ThreadSanitizer run over tests_tls
 * caught. */
static void _release_in_flight(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0) cond_var_broadcast(srv->requests_done_cv);
  mutex_unlock(srv->mutex);
}

/* Submits conn (conn->reject_status already set) to reject_pool for its
 * courtesy rejection response, so the blocking write never runs on the
 * calling thread; falls back to a synchronous inline close on the calling
 * thread if reject_pool is unavailable (server tearing down) or its own
 * bounded queue (_CHTTPSVR_REJECT_POOL_QUEUE_CAP) is full. Shared by both
 * rejection paths in this file: the pool-full 503 case in
 * _conn_start_diverted (which has already incremented in_flight_requests
 * and paused/removed conn->reg itself, since at that point the outcome --
 * kept alive vs rejected -- was not yet known) and the reactor thread's own
 * synchronous 404/405/500 rejection via _conn_dispatch_reject below. Every
 * caller must have already incremented srv->in_flight_requests for this
 * connection; this function releases it (via _release_in_flight) only once
 * the close has actually completed, not merely once queued -- see that
 * function's own comment on why releasing any earlier would be a real
 * use-after-free. */
static void _conn_reject_via_pool(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  mutex_lock(srv->mutex);
  ctpool reject_pool = srv->reject_pool;
  mutex_unlock(srv->mutex);

  if (reject_pool && ctpool_try_submit(reject_pool, _reject_task, conn, NULL) ==
                         ccol_success) {
    return;
  }

  /* Last-resort fallback: reject_pool is unavailable (server tearing down),
   * its own bounded queue is full, or its submission otherwise failed (e.g.
   * OOM). Falls back to a synchronous inline close on the calling thread,
   * exactly as this codebase always did before reject_pool existed, bounded
   * by the same _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS as every other
   * reject-and-close call site. srv was captured above, before
   * _conn_reject_and_close, which frees conn internally (via _conn_close). */
  _conn_reject_and_close(conn, _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS);
  _release_in_flight(srv);
}

/* Routes a connection whose route was rejected during synchronous header
 * parsing on the reactor thread (404/405/500; conn->req_rejected/
 * reject_status already set by _on_headers_complete) through reject_pool,
 * exactly like the pool-full 503 case below, so a slow-reading client being
 * told about a bad route can no longer stall the sole reactor thread
 * either -- previously only the 503 overload case avoided this. Unlike
 * _conn_start_diverted's own reg handling, this connection is never kept
 * alive after a rejection, so there is no reason to pause a live
 * registration (pausing exists purely to make a later event_loop_resume
 * cheap, which never happens here); it is removed outright instead. */
static void _conn_dispatch_reject(chttpsvr_conn_t *conn) {
  _idle_list_remove(conn);
  if (conn->reg) {
    event_loop_remove(srv_engine_bundler.reactor, conn->reg);
    conn->reg = NULL;
  }
  mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  mutex_unlock(conn->srv->mutex);
  _conn_reject_via_pool(conn);
}

static void _conn_start_diverted(chttpsvr_conn_t *conn, const char *leftover,
                                 size_t leftover_len) {
  _idle_list_remove(conn);
  /* Pause rather than remove: the registration is cheaply resumed
   * (event_loop_resume) once the worker finishes this request and the
   * connection goes back to waiting for the next one, avoiding a full
   * event_entry allocation/free and fd-registry chmap churn on every
   * keep-alive request cycle. A failed pause (should not happen in
   * practice: nothing else touches this connection's reg while the
   * reactor still owns it; event_loop_pause's own documented failure modes
   * for a non-NULL fd reg all reduce to "reg was concurrently removed")
   * is treated the same as never having had a live registration, so
   * _task_worker's own keep-alive tail correctly falls back to closing the
   * connection instead of resuming a stale reg. event_loop_remove is called
   * defensively before dropping the pointer: if the failure really is
   * "already removed", this is a safe, documented no-op (ccol_invalid_args,
   * ignored); if pause somehow failed for any other reason while the
   * registration was still genuinely live, this is what actually reclaims
   * it instead of leaking the event_entry in the reactor's registry. */
  if (conn->reg &&
      event_loop_pause(srv_engine_bundler.reactor, conn->reg) != ccol_success) {
    event_loop_remove(srv_engine_bundler.reactor, conn->reg);
    conn->reg = NULL;
  }
  conn->state = CONN_ST_DIVERTED;
  /* Idempotent: a no-op if this is a further pipelined request being
   * diverted on a connection that is already in the registry from an
   * earlier, still-unfinished divert cycle (see _diverted_list_remove's own
   * comment on where the matching removal happens). */
  _diverted_list_add(conn);

  mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ctpool pool = conn->srv->worker_pool;
  mutex_unlock(conn->srv->mutex);

  /* Copy leftover now: it points into the reactor's own stack read buffer,
   * which is about to go out of scope the moment this callback returns.
   * _task_worker re-derives nothing from chttp1_parser_consumed() itself;
   * that value is only meaningful relative to the exact buffer/length pair
   * passed to the execute() call that produced it, which is the reactor's
   * own stack buffer, gone by the time the worker runs. Passing the
   * already-copied carry bytes directly avoids that lifetime hazard
   * entirely. */
  char *carry = NULL;
  bool carry_alloc_failed = false;
  if (leftover_len > 0) {
    carry = (char *)_mem_alloc(conn->m_procs, leftover_len);
    if (carry)
      memcpy(carry, leftover, leftover_len);
    else
      carry_alloc_failed = true;
  }
  /* Publish carry/_carry_over_len BEFORE submitting to the pool, not after:
   * ctpool_try_submit can hand this task to an already-idle worker thread
   * that starts running _task_worker(conn) immediately, concurrently with
   * the rest of this function. Setting these fields after the submit call
   * raced that worker thread reading conn->_carry_over; it would see
   * NULL (this field's steady-state value between requests, since
   * _task_worker always frees-and-nulls it once consumed), silently
   * dropping the real leftover bytes for this request, and by the time
   * this function got around to the (now too late) assignment, nothing
   * would ever free that already-orphaned buffer; both a data-loss bug
   * and the exact leak valgrind caught.
   *
   * _carry_over_len must stay in lockstep with whether _carry_over is
   * actually non-NULL: leaving it at leftover_len when the allocation above
   * failed would hand the worker thread a NULL/nonzero pair, and
   * chttp1_stream_prepare/_tls unconditionally memcpy()s leftover_len bytes
   * FROM that pointer, a NULL-source memcpy, not a no-op. */
  conn->_carry_over = carry;
  conn->_carry_over_len = carry ? leftover_len : 0;

  if (carry_alloc_failed) {
    /* The leftover bytes are pipelined request-body bytes already read off
     * the wire and gone from the socket for good; they cannot be silently
     * dropped (the worker would then read the body starting at the wrong
     * offset, silently truncating it) and cannot be hand-waved past the
     * NULL-pointer hazard above. Reject with 500, exactly like the
     * pool-full 503 case below: in_flight_requests was already incremented
     * and conn->reg already paused/removed above. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    _conn_reject_via_pool(conn);
    return;
  }

  if (!pool ||
      ctpool_try_submit(pool, _task_worker, conn, NULL) != ccol_success) {
    _mem_free(conn->m_procs, carry);
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    /* The worker pool is at capacity (ctpool_try_submit returns
     * ccol_container_full rather than blocking, matching this module's
     * documented "never block the reactor thread" contract); this is a
     * real, if transient, server condition the client should be told about
     * via a synchronous 503, not a bare connection reset (see
     * bounded_pool_full_returns_503 in tests.c). */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;

    /* in_flight_requests was already incremented, and conn->reg already
     * paused/removed, above (before it was known whether this connection
     * would be diverted successfully or rejected); _conn_reject_via_pool
     * takes it from here, specifically not resubmitting to `pool` itself --
     * that is the exact pool that just rejected this request for being
     * full. See that function's own comment for the rest. */
    _conn_reject_via_pool(conn);
    return;
  }
}

static void _conn_reject_and_close(chttpsvr_conn_t *conn, unsigned timeout_ms) {
  chttpsvr_resp resp;
  memset(&resp, 0, sizeof(resp));
  resp.status_code = conn->reject_status;
  resp.m_procs = conn->m_procs;
  chttp1_stream_t stream;
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0);
  _send_response(&stream, &resp, false, timeout_ms, conn->method == CHTTP_HEAD);
  chttp1_stream_release(&stream);
  _destroy_resp(&resp, conn->m_procs);
  _conn_close(conn);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test instrumentation only: counts how many rejections were
 * actually carried out on a reject_pool thread (as opposed to the
 * synchronous fallback in _conn_reject_via_pool, which never reaches
 * _reject_task at all). Process-wide, not per-server, matching the same
 * convention _chttpsvr_engine_num_reactor_threads_for_tests already
 * established; a test reads the delta across its own window rather than an
 * absolute value, since other tests in the same process may also exercise
 * rejections. Gated so this symbol/counter does not exist at all in a
 * production build. */
static _Atomic size_t g_reject_task_run_count_for_tests = 0;

/* White-box test hook only: forces _task_worker's own initial
 * chttp1_stream_prepare()/_tls() call to be treated as if it had failed,
 * exercising the same body_unavailable fallback path a real allocation
 * failure takes (see _task_worker's own comment on body_unavailable). A
 * real failure there is not deterministically reproducible from a test:
 * unlike every other allocation this module makes, that one call's
 * carry-over copy goes through plain malloc(), not this module's own
 * ccol_memmgmt_procs_t convention, so the existing fail-at-size custom
 * allocator technique (see carry_over_alloc_failure_rejects_gracefully_
 * instead_of_crashing in tests.c) cannot reach it. Gated so this symbol
 * does not exist at all in a production build. */
static _Atomic bool g_force_stream_prepare_fail_for_tests = false;
void _chttpsvr_force_stream_prepare_fail_for_tests(bool force) {
  atomic_store(&g_force_stream_prepare_fail_for_tests, force);
}

/* White-box test hook only: when armed, chttpsvr_start() blocks at a single
 * fixed point -- right after it has confirmed/acquired the shared engine
 * reference and registered itself with servers_bundler, before doing any
 * further reactor work (_idle_sweep_start_if_needed, _make_listen_socket,
 * event_loop_add for the listener) -- signalling entry and then waiting for
 * an explicit release. This lets a test deterministically land a concurrent
 * chttpsvr_engine_stop() call inside the exact window that this server's
 * early _servers_register call (see that call site's own comment) and
 * _quiesce_server_once's pending_resolve_count wait both exist to close,
 * rather than relying on a fixed sleep to probabilistically hit a race
 * window that is otherwise only a few instructions wide. One-shot per arm
 * call. Gated so none of this exists in a production build. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_race_hook = {0};

static void _start_race_hook_init_globals(void) {
  mutex_init(g_start_race_hook.mutex);
  cond_var_init(g_start_race_hook.cv);
}

void _chttpsvr_arm_start_race_hook_for_tests(void) {
  call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.armed = true;
  g_start_race_hook.entered = false;
  g_start_race_hook.go = false;
  mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_wait_start_race_hook_entered_for_tests(void) {
  call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  mutex_lock(g_start_race_hook.mutex);
  while (!g_start_race_hook.entered)
    cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_release_start_race_hook_for_tests(void) {
  call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.go = true;
  cond_var_broadcast(g_start_race_hook.cv);
  mutex_unlock(g_start_race_hook.mutex);
}

static void _start_race_hook_wait_if_armed(void) {
  call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  mutex_lock(g_start_race_hook.mutex);
  if (!g_start_race_hook.armed) {
    mutex_unlock(g_start_race_hook.mutex);
    return;
  }
  g_start_race_hook.armed = false; /* one-shot */
  g_start_race_hook.entered = true;
  cond_var_broadcast(g_start_race_hook.cv);
  while (!g_start_race_hook.go)
    cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  mutex_unlock(g_start_race_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* reject_pool's task function: runs _conn_reject_and_close on reject_pool's
 * own dedicated thread instead of the reactor thread (see that field's own
 * comment on struct chttpserver and _conn_start_diverted's comment on why),
 * then releases this request's in_flight_requests slot -- mirroring
 * _task_worker's own decrement -- only once the close has actually
 * completed, not merely once this task was queued; see
 * _release_in_flight's own comment for why that ordering matters. srv is
 * captured before _conn_reject_and_close, which frees conn internally. */
static void _reject_task(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _conn_reject_and_close(conn, _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_reject_task_run_count_for_tests, 1);
#endif
  _release_in_flight(srv);
}

/* Feeds n freshly-available bytes into conn->parser and resolves whatever
 * outcome results, exactly the way _conn_pump's own read loop always has;
 * factored out so _task_worker's keep-alive tail (see its own comment on
 * chttp1_stream_take_leftover) can drive the very same header-parsing
 * machinery synchronously, on the worker thread, against a pipelined next
 * request's bytes that have already been pulled off the wire and can never
 * arrive as a fresh socket-readable event for the reactor to react to.
 *
 * buf/n need not come from a live socket read at all; the caller owns
 * buf's lifetime for the duration of this call only (chttp1_parser_execute
 * never retains a pointer into it past return, and _conn_start_diverted
 * below copies out whatever leftover it is given, exactly as it already
 * does for a real reactor-thread read).
 *
 * @return true if the caller should keep reading (more bytes needed to
 *         complete the request line/header block: CHTTP1_OK); false if
 *         this call already fully resolved conn's fate for now (diverted
 *         to a worker, rejected and routed to reject_pool, or closed
 *         outright) and the caller must not touch conn again. */
static bool _conn_feed_bytes(chttpsvr_conn_t *conn, const char *buf, size_t n) {
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  chttp1_errno_t r = chttp1_parser_execute(&conn->parser, buf, n);

  if (r == CHTTP1_OK) return true; /* need more header bytes; keep reading */

  if (r == CHTTP1_HEADERS_ONLY || r == CHTTP1_PAUSED) {
    size_t consumed = chttp1_parser_consumed(&conn->parser);
    const char *leftover = buf + consumed;
    size_t leftover_len = n - consumed;

    /* The Expect: 100-continue interim write (if conn->expects_continue)
     * is sent by _task_worker on a worker thread instead of here, once
     * this request has actually been diverted; see that function's own
     * comment for why. */
    _conn_start_diverted(conn, leftover, leftover_len);
    return false;
  }

  /* CHTTP1_USER: an unmatched/rejected route, decided by our own
   * _on_headers_complete (conn->req_rejected already set to a specific
   * status). The route itself was identified, so send a graceful
   * error response (the body, if any, is never read; the connection is
   * then closed rather than kept alive, since the client's still-arriving
   * body would otherwise be misread as a pipelined request).
   *
   * CHTTP1_ERROR: a syntax-level problem the parser itself rejected before
   * routing ever ran (a malformed request line, a negative Content-Length,
   * chunked not last in a Transfer-Encoding list, a too-long header,
   * ...). This matches every other pre-routing parse error in this
   * parser: an outright connection close with NO response at all, not a
   * graceful error page; see negative_content_length_rejected and
   * chunked_not_last_in_transfer_encoding_list_rejected in tests.c, which
   * assert exactly that. */
  if (conn->req_rejected) {
    /* Routed through reject_pool (or its bounded synchronous fallback),
     * exactly like the pool-full 503 case, so a slow-reading client being
     * told 404/405/500 can no longer stall the sole reactor thread; see
     * _conn_dispatch_reject/_conn_reject_via_pool's own comments. */
    _conn_dispatch_reject(conn);
  } else {
    _conn_close(conn);
  }
  return false;
}

/* Drives the reactor-owned portion of one connection: TLS handshake (if
 * any), then header parsing until a request is either rejected (sent
 * synchronously and the connection closed) or diverted to a worker. */
static void _conn_pump(chttpsvr_conn_t *conn) {
  if (conn->state == CONN_ST_TLS_HANDSHAKE) {
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_ERROR) {
      _SRV_ENGINE_LOG(log_warn, "TLS handshake failed fd=%d", conn->fd);
      _conn_close(conn);
      return;
    }
    if (r == CTLS_HANDSHAKE_WANT_READ || r == CTLS_HANDSHAKE_WANT_WRITE) {
      ccol_select_dir want = (r == CTLS_HANDSHAKE_WANT_WRITE)
                                 ? ccol_select_write
                                 : ccol_select_read;
      if (conn->reg) {
        /* A failed modify (should not happen in practice: nothing else
         * touches this connection's reg while the reactor exclusively owns
         * it, per _idle_list_try_claim's own single-owner guarantee) is
         * treated as fatal for this connection rather than silently
         * ignored: leaving conn->reg's actual epoll interest out of sync
         * with `want` here would strand the handshake with no further
         * readiness event ever arriving in the direction it actually still
         * needs. */
        if (event_loop_modify(srv_engine_bundler.reactor, conn->reg, want) !=
            ccol_success) {
          _conn_close(conn);
          return;
        }
      } else {
        char *err = NULL;
        conn->reg = event_loop_add(
            srv_engine_bundler.reactor, selectable_from_fd(conn->fd, want),
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
    if (conn->reg) {
      if (event_loop_modify(srv_engine_bundler.reactor, conn->reg,
                            ccol_select_read) != ccol_success) {
        _conn_close(conn);
        return;
      }
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
      /* A signal delivered to this reactor thread mid-read (e.g. the
       * application's own SIGTERM handler calling chttpsvr_engine_stop(),
       * exactly as this module's own header docs recommend, installed
       * without SA_RESTART) can interrupt this read()/ctls_conn_read() call
       * with EINTR even though the connection itself is perfectly healthy;
       * ctls_conn_read() documents itself as behaving like a non-blocking
       * read(2), errno included, so this applies to both branches above.
       * Retried exactly like _listener_on_readable's own accept() loop
       * already retries EINTR, rather than tearing down an unrelated,
       * healthy connection over a spurious signal interruption. */
      if (errno == EINTR) continue;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        if (!conn->reg) {
          char *err = NULL;
          conn->reg = event_loop_add(
              srv_engine_bundler.reactor,
              selectable_from_fd(conn->fd, ccol_select_read),
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

    if (!_conn_feed_bytes(conn, buf, (size_t)n)) return;
    /* CHTTP1_OK: loop for more header bytes. */
  }
}

/* Every reactor-dispatched entry point into a live (already-registered)
 * connection must claim it out of the idle list first; see
 * _idle_list_try_claim's own doc comment for the use-after-free this
 * prevents (a concurrent idle-timeout/destroy-time closer freeing the same
 * connection, including its TLS state, while a dispatch is using it). A
 * failed claim is not an error: it means a closer already has exclusive
 * ownership (or, harmlessly, that this connection's own _idle_list_add
 * call for the registration that just fired hasn't executed yet; see
 * that same doc comment for why level-triggered epoll makes this
 * self-healing); either way, the correct action is to touch nothing and
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
    ssize_t n = chttp1_stream_read(stream, raw, sizeof(raw),
                                   _to_stream_timeout_ms(timeout_ms));
    if (n < 0) {
      if (chttp1_stream_timed_out(stream)) return ccol_timed_out;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) return ccol_http_transfer_aborted; /* truncated body */
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_PAUSED) {
      /* This message is done, but raw may hold more than it needed: bytes
       * already read off the wire that belong to a pipelined next request
       * sitting right behind it in the same read. chttp1_parser_consumed()
       * reports exactly where this message's own framing ended; anything
       * past that must be pushed back onto stream's own carry-over rather
       * than dropped, or a pipelined next request silently vanishes (the
       * client hangs waiting for a response that will never come, since
       * those bytes are already permanently gone from the kernel's own
       * socket receive buffer); see chttp1_stream_take_leftover's own
       * doc comment, and _task_worker's own call to it right before this
       * stream is released, for the other half of this fix. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed)
        chttp1_stream_push_back_leftover(stream, raw + consumed,
                                         (size_t)n - consumed);
      break;
    }
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* A chunk's declared size exceeding max_chunk_size_override (see
       * _conn_reset_for_request) is reported the same way an over-limit
       * cumulative body already is (conn->body_too_large /
       * ccol_msg_too_large), rather than the generic transfer-aborted
       * bucket every other malformed-chunk-framing error falls into: from
       * the caller's perspective it is the identical "body too large"
       * outcome, just caught before any of that chunk's bytes had to
       * actually arrive. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
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

  /* Lazy Expect: 100-continue interim send for a streaming route (see
   * _task_worker's own comment on why this is deferred here rather than
   * sent eagerly before the handler ever runs): fires at most once, on
   * whichever chttpsvr_req_read() call is the first one actually made for
   * this request. A handler that never calls chttpsvr_req_read() at all
   * (choosing instead to reject the request outright) correctly never
   * triggers this, letting the client's own Expect: 100-continue logic see
   * the final rejection response directly instead of a "100 Continue" that
   * would tell it to upload a body the server was never going to read.
   *
   * Also skipped, even on this first call, if the message never carried a
   * body in the first place (no Content-Length, no chunked
   * Transfer-Encoding): chttp1_parser already reached message-complete at
   * header-parse time in that case (CHTTP1_HEADERS_DIVERT_BODY downgrades to
   * immediate completion when there is nothing to divert), so there is no
   * body left to invite; sending "100 Continue" here would be the identical
   * "tell the client to upload a body the server was never going to read"
   * mistake the reject-without-reading case above already avoids, just
   * reached via a bodyless request instead of a rejecting handler.
   * interim_continue_sent is still set unconditionally so this check is not
   * repeated on every subsequent chttpsvr_req_read() call for the same
   * request. */
  if (conn->expects_continue && !conn->interim_continue_sent) {
    conn->interim_continue_sent = true;
    if (!chttp1_parser_message_complete(&conn->parser)) {
      const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
      unsigned wtimeout_ms = atomic_load(&conn->srv->response_write_timeout_ms);
      /* Best-effort: a failed/timed-out write here leaves the connection in
       * trouble either way, and the subsequent read attempt below will
       * naturally fail too, reported through this function's own ordinary
       * error path. */
      chttp1_stream_write(req->stream, cont, strlen(cont),
                          _to_stream_timeout_ms(wtimeout_ms));
    }
  }

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
    ssize_t n = chttp1_stream_read(req->stream, raw, sizeof(raw),
                                   _to_stream_timeout_ms(timeout_ms));
    if (n < 0) {
      if (chttp1_stream_timed_out(req->stream)) {
        /* Either stream_read_timeout_ms's own per-call poll(2) timed out, or
         * max_body_read_duration_ms's deadline was folded into this call's
         * timeout by _check_read_deadline above and expired mid-poll instead
         * of being caught by that function's own up-front check on the NEXT
         * call (which never happens, since chttpsvr_req_read returns here
         * immediately). Report it the same way _drain_body already does for
         * the buffered-route path, via the same conn->deadline_exceeded flag
         * chttpsvr_req_stream_error() reads, so both caps collapse to the
         * identical ccol_timed_out result (see max_body_read_duration_exceeded_
         * reports_ccol_timed_out / stream_read_timeout_reports_ccol_timed_out
         * in tests.c). */
        conn->deadline_exceeded = true;
      } else {
        /* A hard I/O error (not a timeout): the peer reset the connection,
         * a raw read()/ctls_conn_read() failure, or similar. Same
         * "connection closed or malformed framing" bucket the two branches
         * below report via conn->transfer_aborted. */
        conn->transfer_aborted = true;
      }
      return -1;
    }
    if (n == 0) {
      /* Peer closed its write side (or the whole connection) before the
       * declared/chunked framing said the body was actually done; a
       * genuinely truncated body, not the message's natural end (that case
       * is chttp1_parser_message_complete() returning true above, handled
       * separately). Without this flag chttpsvr_req_stream_error() had no
       * way to report this and silently fell through to ccol_success,
       * telling a streaming handler a truncated upload was a clean read;
       * confirmed via a standalone repro (POST Content-Length: 100, send 20
       * bytes, half-close): the handler observed chttpsvr_req_read()
       * return -1 but chttpsvr_req_stream_error() report ccol_success. */
      conn->transfer_aborted = true;
      return -1;
    }
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* Malformed framing (e.g. a bad chunk-size line) past the point
       * max_body_size enforcement in _on_body could have already set
       * body_too_large for this same CHTTP1_USER/CHTTP1_ERROR result; a
       * chunk's declared size exceeding max_chunk_size_override (see
       * _conn_reset_for_request/_drain_body's identical check) sets it here
       * too, for the same reason. chttpsvr_req_stream_error() checks
       * body_too_large first, so setting both here is harmless and
       * preserves that existing priority. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      conn->transfer_aborted = true;
      return -1;
    }
    if (r == CHTTP1_PAUSED) {
      /* Same reasoning as _drain_body's own identical check: raw may hold
       * more than this message needed (a pipelined next request's bytes,
       * already read off the wire); push the trailing, unconsumed part
       * back onto req->stream's own carry-over so _task_worker's later
       * chttp1_stream_take_leftover() call reclaims it, rather than
       * letting it vanish. The next loop iteration's message_complete
       * check above returns 0 immediately once conn->body is drained. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed)
        chttp1_stream_push_back_leftover(req->stream, raw + consumed,
                                         (size_t)n - consumed);
    }
    /* CHTTP1_OK or CHTTP1_PAUSED: loop back to drain whatever _on_body just
     * appended to conn->body. */
  }
}

ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req) {
  if (!req || !req->conn) return ccol_unexpected_failure;
  chttpsvr_conn_t *conn = req->conn;
  if (conn->deadline_exceeded) return ccol_timed_out;
  if (conn->body_too_large) return ccol_msg_too_large;
  if (conn->transfer_aborted) return ccol_http_transfer_aborted;
  return ccol_success;
}

static void _task_worker(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;

  chttp1_stream_t stream;
  bool prepared;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_stream_prepare_fail_for_tests)) {
    prepared = false;
  } else
#endif /* RUNNING_UNIT_TESTS */
    if (conn->tls)
      prepared =
          chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls,
                                    conn->_carry_over, conn->_carry_over_len);
    else
      prepared = chttp1_stream_prepare(&stream, conn->fd, conn->_carry_over,
                                       conn->_carry_over_len);
  /* chttp1_stream_prepare[_tls]() fails ONLY when copying a nonzero
   * carry-over hits an allocation failure (a NULL/0 leftover pair can never
   * fail; see its own doc comment). Retry with the carry-over dropped so
   * this connection still gets a real, writable stream: leaving prepared ==
   * false here would gate every response path below (all guarded on
   * prepared) shut, so the client would see the connection simply drop
   * with zero response bytes instead of a graceful error -- and, for a
   * streaming route, the handler would still run (see body_unavailable's
   * own use below) against a stream it can never read from, with
   * chttpsvr_req_stream_error() having no way to report why. The dropped
   * carry-over bytes are unrecoverable either way; body_unavailable below
   * forces this request to a clean abort-and-close, so nothing further is
   * lost by retrying without them. */
  bool body_unavailable = !prepared;
  if (body_unavailable) {
    prepared = conn->tls ? chttp1_stream_prepare_tls(&stream, conn->fd,
                                                     conn->tls, NULL, 0)
                         : chttp1_stream_prepare(&stream, conn->fd, NULL, 0);
  }
  _mem_free(conn->m_procs, conn->_carry_over);
  conn->_carry_over = NULL;
  conn->_carry_over_len = 0;

  /* response_write_timeout_ms computed once, up front, since both a
   * buffered route's interim write below and the real response send further
   * down (see chttp1_should_keep_alive/_send_response) need it. Sent here,
   * on the worker thread, rather than from _conn_pump on the reactor thread
   * where this write used to live: _task_worker is only ever reached for a
   * request that already cleared _on_headers_complete's route match (a
   * rejected route never diverts at all, going through
   * _conn_dispatch_reject/_conn_reject_via_pool instead), so the old
   * !conn->req_rejected guard is unconditionally true here and is dropped;
   * a request that instead hits the pool-full 503 path never reaches
   * _task_worker either, so no spurious "100 Continue" precedes a 503 the
   * way it used to (_conn_pump sent this unconditionally as soon as headers
   * finished, before ctpool_try_submit's own capacity check ever ran). This
   * also moves a real (if normally tiny and fast) blocking write off the sole
   * reactor thread and onto a worker thread, where blocking on I/O is the
   * expected, designed-for behavior. */
  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  /* Buffered routes only: a buffered route's handler never runs until the
   * whole body has already been read in full (see _drain_body below), so it
   * has no opportunity to reject the request before that body arrives
   * regardless of whether the interim response is sent eagerly here or any
   * later; sending it now, rather than deferring, costs nothing and keeps
   * the existing, well-tested wire timing for this route kind unchanged.
   *
   * A streaming route's handler, by contrast, can genuinely decide to
   * reject a request (bad auth, unacceptable Content-Type, ...) without
   * ever calling chttpsvr_req_read() -- exactly the scenario Expect:
   * 100-continue (RFC 7231 SS5.1.1) exists to make cheap for the client, by
   * letting the server answer with a final status instead of "100
   * Continue" and never receiving the body at all. Sending the interim
   * response unconditionally here, before the handler ever runs, silently
   * defeated that: the client would already have been told to go ahead and
   * upload the body by the time a streaming handler got a chance to reject
   * without reading it. The interim response for a streaming route is
   * therefore sent lazily instead, from chttpsvr_req_read() itself, the
   * first time (if ever) the handler actually asks to read the body.
   *
   * Also skipped, even for a buffered route, when the message never carried
   * a body in the first place (chttp1_parser_message_complete() already
   * true here: no Content-Length, no chunked Transfer-Encoding); see
   * chttpsvr_req_read()'s identical guard for the full reasoning -- there is
   * no body left to invite, so sending "100 Continue" here would tell the
   * client to upload one that was never coming. */
  if (prepared && !body_unavailable && conn->expects_continue &&
      !conn->matched_route->is_streaming &&
      !chttp1_parser_message_complete(&conn->parser)) {
    const char *cont = "HTTP/1.1 100 Continue\r\n\r\n";
    chttp1_stream_write(&stream, cont, strlen(cont),
                        _to_stream_timeout_ms(write_timeout_ms));
    conn->interim_continue_sent = true;
  }

  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = prepared ? &stream : NULL;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval_t body_err = ccol_success;
  if (body_unavailable) {
    /* The request's body was lost to the OOM above; never invoke the
     * handler with a stream that cannot actually deliver it, for either a
     * buffered or a streaming route -- see body_unavailable's own comment
     * above for why this must not be conditioned on route kind the way the
     * ordinary body_err-from-_drain_body case below is. */
    body_err = ccol_not_enough_memory;
  } else if (prepared) {
    if (!conn->matched_route->is_streaming) {
      body_err = _drain_body(conn, &stream);
    }
    /* Streaming routes: the handler pulls the body itself via
     * chttpsvr_req_read(); nothing to pre-drain here. */
  } else {
    /* Unreachable in practice: the NULL/0 retry above can never fail (see
     * body_unavailable's own comment), but fail closed rather than assume. */
    body_err = ccol_not_enough_memory;
  }

  bool aborted = false;
  if (body_err != ccol_success &&
      (body_unavailable || !conn->matched_route->is_streaming)) {
    conn->resp.status_code =
        (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
        : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
                                         : CHTTP_STATUS_INTERNAL_ERROR;
    aborted = true;
  } else {
    _chttpsvr_next(&req, &conn->resp);
    /* A streaming handler may have stopped reading before the body's own
     * natural end (or hit its own error via chttpsvr_req_read returning
     * -1); either way, nothing further to drain; the connection's fate
     * (keep-alive vs close) below already accounts for a not-fully-drained
     * body via chttp1_should_keep_alive's message-completion check. */
  }

  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large;

  bool sent =
      prepared && _send_response(&stream, &conn->resp, keep_alive,
                                 write_timeout_ms, conn->method == CHTTP_HEAD);
  if (!sent) keep_alive = false;

  /* Reclaim any bytes belonging to a further pipelined request on this
   * same connection that have already been pulled off the wire, before
   * chttp1_stream_release below discards them unconditionally: whether
   * this message completed before _drain_body/chttpsvr_req_read ever
   * needed to touch stream at all (e.g. a bodyless GET/HEAD, or an
   * explicit Content-Length: 0), in which case the whole of the original
   * conn->_carry_over this stream was prepared with is still sitting here
   * untouched; or a body-draining read swept up extra bytes past this
   * message's own framing boundary (see _drain_body's/chttpsvr_req_read's
   * own CHTTP1_PAUSED handling, which pushes exactly that case back onto
   * stream's carry-over for this call to pick up uniformly). Reclaimed
   * unconditionally whenever prepared (cheap; no I/O), but only actually
   * kept when this connection is staying alive; freed outright otherwise,
   * since a closing connection has nowhere to hand pipelined bytes to
   * anyway (matching pipelined_bytes_after_rejected_route_not_misparsed's
   * own documented "reject and close" precedent). */
  size_t reclaimed_len = 0;
  char *reclaimed =
      prepared ? chttp1_stream_take_leftover(&stream, &reclaimed_len) : NULL;

  if (prepared) chttp1_stream_release(&stream);
  _destroy_req_qparams(&req);

  if (!keep_alive) free(reclaimed);

  /* in_flight_requests is released (via _release_in_flight, at the bottom
   * of every exit path below) only once this connection has reached a
   * state _drain_and_close_all_connections can actually observe: fully
   * closed via _conn_close, or safely published back into the idle list.
   * Releasing it here, before that, would open the exact use-after-free
   * window described in _release_in_flight's own comment: srv (a
   * previously-captured local, still valid even after conn is freed below)
   * is what every _release_in_flight call in this function uses. */

  if (!keep_alive) {
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }

  /* Keep-alive: reset per-request state and hand the connection back to the
   * reactor to read the next request's headers.
   *
   * conn->reg is non-NULL here only if this request actually went through a
   * live event_loop registration that _conn_start_diverted then paused (see
   * that function's own comment); resuming it is far cheaper than a fresh
   * event_loop_add, since the underlying event_entry, fd-registry chmap
   * entry, and epoll_ctl(ADD) were never torn down in the first place; only
   * the combined epoll interest mask is recomputed.
   *
   * conn->reg is NULL here in two cases that must be told apart: a genuinely
   * failed pause (see _conn_start_diverted's comment; treated as fatal,
   * below), or, far more common, this connection's headers (and
   * sometimes several requests' worth of pipelined bytes) were read
   * synchronously by _conn_pump's own optimistic first read, right after
   * accept, before this connection was ever registered with the reactor at
   * all (_listener_on_readable calls _conn_pump directly, with no
   * event_loop_add in between). Both this branch's own "no registration
   * ever existed" case and _conn_start_diverted's "pause failed" case
   * collapse to the identical NULL value, and are indistinguishable from
   * here by design; both need the exact same fresh event_loop_add
   * fallback the original always-add design already used, so this is not a
   * gap, just two paths sharing one outcome. */
  /* This worker is done with conn as a worker-owned entity for now: no
   * longer a candidate for _force_unblock_diverted_connections. If the
   * pipelined-bytes handling below ends up diverting a further request on
   * this same connection, _conn_start_diverted's own _diverted_list_add
   * call re-adds it (idempotently); otherwise it stays out until some later
   * divert cycle. */
  _diverted_list_remove(conn);
  _conn_reset_for_request(conn);
  conn->state = CONN_ST_READING_HEADERS;

  if (reclaimed && reclaimed_len > 0) {
    /* Copy into this module's own allocator (conn->_carry_over's normal
     * convention) and free the plain-malloc'd source immediately; matches
     * _conn_start_diverted's own carry_alloc_failed handling if this copy
     * itself fails under OOM: the pipelined next request's bytes are
     * then unrecoverably lost, but nothing crashes or corrupts, and the
     * client simply discovers this via its own timeout and retries on a
     * fresh connection. */
    char *owned = (char *)_mem_alloc(conn->m_procs, reclaimed_len);
    if (owned) {
      memcpy(owned, reclaimed, reclaimed_len);
      conn->_carry_over = owned;
      conn->_carry_over_len = reclaimed_len;
    }
  }
  free(reclaimed);

  if (conn->_carry_over_len > 0) {
    /* This connection's next request's bytes are already sitting in
     * memory, not merely available to read later: they are permanently
     * gone from the kernel's own socket receive buffer, so waiting for a
     * fresh epoll readiness event here (the plain resume/add path below)
     * would wait forever for data that will never arrive. Feed them
     * straight into the just-reset parser now, on this worker thread,
     * exactly as _conn_pump would once real socket bytes came in; see
     * _conn_feed_bytes's own doc comment. This is what actually fixes the
     * pipelining data-loss bug chttp1_stream_take_leftover's own doc
     * comment describes: without it, these bytes would simply have been
     * discarded (or, before this whole mechanism existed, silently freed
     * by chttp1_stream_release above with no way to get them back at all). */
    char *lo = conn->_carry_over;
    size_t lo_len = conn->_carry_over_len;
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    /* mp is captured from srv (a previously-captured local, still valid
     * even after conn is freed below), not read as conn->m_procs after the
     * fact: _conn_feed_bytes may already have fully resolved conn's fate
     * (diverted it to another worker thread that races this one to
     * completion and frees it, rejected it via a synchronous fallback close,
     * or closed it outright on a raw parse error) by the time it returns,
     * per its own "the caller must not touch conn again" contract; conn->
     * m_procs is exactly such a touch, and conn->m_procs/srv->m_procs are
     * always the identical pointer (see _conn_create), so reading it from
     * srv instead is both safe and equivalent. A prior version of this code
     * read conn->m_procs here, a real use-after-free: deterministic
     * whenever the further pipelined bytes are malformed (_conn_feed_bytes
     * synchronously closes and frees conn on this same thread before
     * returning) or reject_pool's fallback fires synchronously, and racy
     * otherwise (another worker thread finishing and freeing conn before
     * this thread reaches this free). */
    ccol_memmgmt_procs_t *mp = srv->m_procs;
    bool need_more = _conn_feed_bytes(conn, lo, lo_len);
    _mem_free(mp, lo);
    if (!need_more) {
      /* _conn_feed_bytes already fully resolved conn's fate: diverted a
       * further pipelined request to a worker (possibly this very thread
       * pool, but via a fresh ctpool_submit, not a direct recursive call),
       * rejected it, or closed the connection outright. Only this call's
       * own in_flight_requests slot (incremented back
       * when THIS request was first diverted) is released here; whatever
       * _conn_feed_bytes just started owns its own, independent slot. */
      _release_in_flight(srv);
      return;
    }
    /* CHTTP1_OK: the parser now holds a partial next request's headers,
     * exactly as if a live socket read had produced them; fall through to
     * the ordinary "wait for more" registration logic below unchanged. */
  }

  if (conn->reg) {
    if (event_loop_resume(srv_engine_bundler.reactor, conn->reg) !=
        ccol_success) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
  } else {
    char *err = NULL;
    conn->reg =
        event_loop_add(srv_engine_bundler.reactor,
                       selectable_from_fd(conn->fd, ccol_select_read),
                       (event_handlers_t){.on_readable = _conn_on_readable,
                                          .on_error = _conn_on_error},
                       conn, &err);
    if (!conn->reg) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
  }
  _idle_list_add(conn);
  _release_in_flight(srv);
}

/* ========================================================================== */
/*                    LISTENER: ACCEPT + SOCKET OPTIONS                       */
/* ========================================================================== */

static void _apply_accepted_socket_options(int fd, bool is_unix,
                                           bool enable_keepalive) {
  /* Non-blocking mode is already guaranteed atomically by accept4()'s own
   * SOCK_NONBLOCK flag at the one call site that produces fd (see
   * _listener_on_readable); no separate fcntl(F_SETFL) call is needed (or
   * wanted) here. */
  int one = 1;
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
       * epoll_wait batch once a slot frees up; no extra bookkeeping
       * needed to "wake up" again. */
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    /* accept4() with SOCK_NONBLOCK, not accept() + a separate fcntl(F_SETFL)
     * call: the fd this produces is handed straight to the sole reactor
     * thread's own read()/ctls_conn_read() calls (see _conn_pump), which
     * must never block. accept4() makes non-blocking mode part of the same
     * atomic kernel operation that creates the fd, rather than depending on
     * a second, separate, unchecked fcntl() call succeeding first. */
    int cfd = accept4(atomic_load(&srv->listen_fd), (struct sockaddr *)&ss,
                      &slen, SOCK_NONBLOCK);
    if (cfd < 0) {
      if (errno == EWOULDBLOCK || errno == EAGAIN) return;
      if (errno == EINTR) continue;
      return;
    }
    atomic_fetch_add(&srv->current_connections, 1);
    _apply_accepted_socket_options(cfd, atomic_load(&srv->is_unix_socket),
                                   srv->enable_keepalive);

    call_once(srv_parser_bundler.once, _init_parser_settings);
    chttpsvr_conn_t *conn =
        _conn_create(srv, cfd, &srv_parser_bundler.settings);
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
                               bool *is_unix, char **unix_path_out,
                               ccol_memmgmt_procs_t *mp) {
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
    char *path_copy = ccol_strdup(mp, path);
    if (!path_copy) {
      /* OOM copying the path: fail the whole setup rather than returning a
       * live listening fd with *unix_path_out left NULL. A caller further up
       * (chttpsvr_start) that later hits its own failure and needs to
       * unlink/free the socket path has no NULL-safe way to do so from a
       * bare "no path" signal here, and a successfully started server would
       * otherwise never be able to clean up its own socket file on
       * chttpsvr_stop()/destroy. */
      close(fd);
      unlink(path);
      return -1;
    }
    *unix_path_out = path_copy;
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
/*                    ROUTER SHELL REGISTRY                                   */
/* ========================================================================== */

/* Every chttpsvr_router's own "shell" (this struct itself: owner/srv/
 * m_procs/the routes and mw_head/mw_tail/mw_count bookkeeping fields --
 * as opposed to the CONTENTS it points to: prefix, mw list nodes, the routes
 * array and each route's own data) is allocated here via the process's
 * plain default allocator, never through a server's own (possibly custom)
 * ccol_memmgmt_procs_t, and is never freed by _destroy_router at all.
 *
 * This exists because a caller-held chttpsvr_router* has no indirection of
 * its own, unlike a chttpsvr handle (whose generation lives in the handle
 * VALUE, never in memory that could be freed out from under a reader):
 * chttpsvr_router_on/_on_stream/_use must read router->owner to find out
 * whether the router's owning server is still alive, and that read is
 * itself a dereference of `router`. Resolving router->owner through the
 * ordinary chttpsvr slot table protects everything AFTER a successful
 * resolve (srv/routes/mw_head cannot be freed while any resolve is pinned;
 * __chttpsvr_destroy's own pending_resolve_count wait, run before it frees
 * a single byte of any router's contents, guarantees that) -- but nothing
 * protects the read of router->owner itself, since that happens before any
 * pin exists. Confirmed as a real, valgrind-caught use-after-free by two
 * earlier fix attempts, neither of which touched the shell's own lifetime:
 * resolving router->owner alone, and then a process-wide rwlock taken around
 * _destroy_router's free (which does nothing for a reader that starts only
 * after that free has already completed -- not merely started, completed --
 * since nothing marks the memory itself as unsafe to a holder of the bare
 * pointer once the rwlock has been released again).
 *
 * Keeping the shell allocated for the remainder of the process (registered
 * here so a process-exit destructor, mirroring chttpsvr_slot_table's own
 * identical pattern, can still free every shell exactly once) makes reading
 * router->owner always memory-safe: _destroy_router stores CHTTPSVR_INVALID
 * into it and frees the CONTENTS only, so a reader's atomic load correctly
 * observes either a live, resolvable handle or the sentinel, never freed
 * memory. */
static struct {
  mutex_t mutex;
  once_flag_t once;
  chttpsvr_router **shells; /* plain realloc'd array of every shell ever
                             * created, process-wide */
  size_t count;
  size_t capacity;
} chttpsvr_router_shell_registry = {0};

static void _chttpsvr_router_shell_registry_init_globals(void) {
  mutex_init(chttpsvr_router_shell_registry.mutex);
}

static void _chttpsvr_router_shell_register(chttpsvr_router *r) {
  call_once(chttpsvr_router_shell_registry.once,
            _chttpsvr_router_shell_registry_init_globals);
  mutex_lock(chttpsvr_router_shell_registry.mutex);
  if (chttpsvr_router_shell_registry.count ==
      chttpsvr_router_shell_registry.capacity) {
    size_t new_cap = chttpsvr_router_shell_registry.capacity
                         ? chttpsvr_router_shell_registry.capacity * 2
                         : 8;
    chttpsvr_router **nn =
        (chttpsvr_router **)realloc(chttpsvr_router_shell_registry.shells,
                                    new_cap * sizeof(chttpsvr_router *));
    if (nn) {
      chttpsvr_router_shell_registry.shells = nn;
      chttpsvr_router_shell_registry.capacity = new_cap;
    }
  }
  /* A failed growth here (OOM in this bookkeeping array specifically) just
   * means this one shell is not tracked for the process-exit free below;
   * still fully safe otherwise (a router already invalidated by
   * _destroy_router is never touched again), merely a "still reachable"
   * leak in that rare degenerate case rather than a correctness issue. */
  if (chttpsvr_router_shell_registry.count <
      chttpsvr_router_shell_registry.capacity)
    chttpsvr_router_shell_registry
        .shells[chttpsvr_router_shell_registry.count++] = r;
  mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* Frees every router shell's own memory at process exit, so make memtest's
 * valgrind pass does not report them as still-reachable; see the registry's
 * own comment for why _destroy_router itself never frees this memory.
 * Mirrors _cleanup_chttpsvr_slot_table's own call_once-guarded shape and
 * rationale exactly (a process that links this library but never creates a
 * single chttpsvr must not lock a never-initialised mutex here). */
__attribute__((destructor)) static void _cleanup_chttpsvr_router_shells(void) {
  call_once(chttpsvr_router_shell_registry.once,
            _chttpsvr_router_shell_registry_init_globals);
  mutex_lock(chttpsvr_router_shell_registry.mutex);
  for (size_t i = 0; i < chttpsvr_router_shell_registry.count; i++)
    mem_free(chttpsvr_router_shell_registry.shells[i]);
  free(chttpsvr_router_shell_registry.shells);
  chttpsvr_router_shell_registry.shells = NULL;
  chttpsvr_router_shell_registry.count = 0;
  chttpsvr_router_shell_registry.capacity = 0;
  mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

static chttpsvr_router *_create_router(struct chttpserver *srv,
                                       const char *prefix,
                                       ccol_memmgmt_procs_t *mp) {
  /* The shell itself deliberately does NOT use `mp` (the server's own,
   * possibly custom, allocator), via mem_calloc/mem_free (the plain
   * default-allocator macros, not the _mem_* ones -- which require an
   * actual ccol_memmgmt_procs_t-typed argument, not a bare NULL literal) --
   * see chttpsvr_router_shell_registry's own comment for why this struct's
   * memory must remain valid for the whole remainder of the process, well
   * past srv's (and therefore mp's) own lifetime. */
  chttpsvr_router *r =
      (chttpsvr_router *)mem_calloc(1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    mem_free(r);
    return NULL;
  }
  size_t plen = strlen(r->prefix);
  while (plen > 1 && r->prefix[plen - 1] == '/') r->prefix[--plen] = '\0';
  r->prefix_len = plen;
  r->srv = srv;
  r->m_procs = mp;
  atomic_store(&r->owner, CHTTPSVR_INVALID);
  _chttpsvr_router_shell_register(r);
  return r;
}

static void _destroy_router(chttpsvr_router *r, ccol_memmgmt_procs_t *mp) {
  /* Invalidate before freeing any contents: __chttpsvr_destroy only ever
   * reaches this call once its own pending_resolve_count wait has already
   * confirmed no resolve of this router's owner is currently pinned, so no
   * reader can be inside _router_add_route/_router_add_mw right now; storing
   * CHTTPSVR_INVALID here (rather than after) is what makes a brand new
   * reader arriving from this instant onward see the sentinel and return
   * immediately, instead of racing the frees below. */
  atomic_store(&r->owner, CHTTPSVR_INVALID);
  chttpsvr_mw_node_t *mw = r->mw_head;
  while (mw) {
    chttpsvr_mw_node_t *next = mw->next;
    _mem_free(mp, mw);
    mw = next;
  }
  r->mw_head = r->mw_tail = NULL;
  r->mw_count = 0;
  for (size_t i = 0; i < r->route_count; i++) {
    _free_route_data(r->routes[i], mp);
    _mem_free(mp, r->routes[i]);
  }
  _mem_free(mp, r->routes);
  r->routes = NULL;
  r->route_count = r->route_cap = 0;
  _mem_free(mp, r->prefix);
  r->prefix = NULL;
  r->srv = NULL;
  /* The shell itself (this struct) is deliberately NOT freed here; see
   * chttpsvr_router_shell_registry's own comment above. */
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
    if (router->route_cap > (SIZE_MAX - 4) / 2 ||
        router->route_cap * 2 + 4 > SIZE_MAX / sizeof(chttpsvr_route_t *)) {
      rw_lock_unlock(router->srv->routes_lock);
      _free_route_data(rt, mp);
      _mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
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
      /* Overflow-safe growth, matching every other growable array in this
       * file (_router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_
       * header): a plain "cap * 2 + 8" can wrap on a pathological qp->cap,
       * silently under-allocating nc * sizeof(char *) below. */
      if (qp->cap > (SIZE_MAX - 8) / 2 ||
          qp->cap * 2 + 8 > SIZE_MAX / sizeof(char *)) {
        _mem_free(mp, key_decoded);
        _mem_free(mp, val_decoded);
        goto oom;
      }
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
  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return CHTTPSVR_INVALID;

  struct chttpserver *srv =
      (struct chttpserver *)_mem_calloc(mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs_t *)_mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs_t));
    if (!srv->m_procs) {
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("out of memory");
      return CHTTPSVR_INVALID;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  clog logger = cl ? clog_derive(cl) : clog_open_fd_mp(2, CLOG_FATAL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("failed to create server logger");
    return CHTTPSVR_INVALID;
  }

  if (mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (mutex_init(srv->idle_mutex) != 0) {
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (mutex_init(srv->diverted_mutex) != 0) {
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (cond_var_init(srv->resolve_cv) != 0) {
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  {
    /* CLOCK_MONOTONIC to match _wait_and_detach_pools' own
     * clock_gettime(CLOCK_MONOTONIC, ...)-based deadline; cond_var_init's
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
      cond_var_destroy(srv->resolve_cv);
      mutex_destroy(srv->idle_mutex);
      mutex_destroy(srv->diverted_mutex);
      mutex_destroy(srv->mutex);
      clog_close(logger);
      _mem_free(mprocs, srv->m_procs);
      _mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("cond_var_init failed");
      return CHTTPSVR_INVALID;
    }
  }

  if (cond_var_init(srv->teardown_done_cv) != 0) {
    cond_var_destroy(srv->requests_done_cv);
    cond_var_destroy(srv->resolve_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  if (rw_lock_init(srv->routes_lock) != 0) {
    cond_var_destroy(srv->teardown_done_cv);
    cond_var_destroy(srv->requests_done_cv);
    cond_var_destroy(srv->resolve_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("rw_lock_init failed");
    return CHTTPSVR_INVALID;
  }

  chttpsvr_router *root = _create_router(srv, "", srv->m_procs);
  if (!root) {
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->teardown_done_cv);
    cond_var_destroy(srv->requests_done_cv);
    cond_var_destroy(srv->resolve_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }

  srv->routers =
      (chttpsvr_router **)_mem_alloc(srv->m_procs, sizeof(chttpsvr_router *));
  if (!srv->routers) {
    _destroy_router(root, srv->m_procs);
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->teardown_done_cv);
    cond_var_destroy(srv->requests_done_cv);
    cond_var_destroy(srv->resolve_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }
  srv->routers[0] = root;
  srv->router_count = 1;
  srv->router_cap = 1;
  srv->cl = logger;
  atomic_store(&srv->listen_fd, -1);
  srv->started = false;

  chttpsvr h = _chttpsvr_handle_slot_acquire(srv);
  if (h == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate server slot");
    _mem_free(srv->m_procs, srv->routers);
    _destroy_router(root, srv->m_procs);
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->teardown_done_cv);
    cond_var_destroy(srv->requests_done_cv);
    cond_var_destroy(srv->resolve_cv);
    mutex_destroy(srv->idle_mutex);
    mutex_destroy(srv->diverted_mutex);
    mutex_destroy(srv->mutex);
    clog_close(logger);
    _mem_free(mprocs, srv->m_procs);
    _mem_free(mprocs, srv);
    return CHTTPSVR_INVALID;
  }
  return h;
}

/* Both of a server's pools (worker_pool and reject_pool) are always
 * created/destroyed together (see chttpsvr_start/_quiesce_server_once), so
 * every detach point needs both, not just worker_pool; bundled into one
 * small return type rather than two separate detach functions so no call
 * site can accidentally detach one and forget the other. */
typedef struct {
  ctpool worker;
  ctpool reject;
} _detached_pools_t;

/* Further grace period _wait_in_flight_bounded allows, after forcibly
 * shutdown(2)-ing every still-diverted connection's fd, for those
 * connections' worker threads to actually notice the resulting I/O error
 * and unwind (their own existing error-handling paths, e.g. the same ones
 * a real peer disconnect already exercises); see that function's own
 * comment for the full reasoning. */
#define _CHTTPSVR_FORCE_UNBLOCK_GRACE_MS 5000
/* The real, documented graceful-wait bound _wait_in_flight_bounded gives
 * in-flight requests before ever considering the forced-unblock path at
 * all; pulled out to a named constant so the RUNNING_UNIT_TESTS override
 * below has an unambiguous "default" to fall back to. */
#define _CHTTPSVR_GRACEFUL_WAIT_MS 30000

#ifdef RUNNING_UNIT_TESTS
/* White-box test override for _wait_in_flight_bounded's two timing bounds
 * (the graceful wait before escalating, and the post-force-unblock grace
 * period): both 0 (the default) means "use the real, documented values"
 * above; a test that calls _chttpsvr_set_wait_in_flight_bounds_for_tests
 * with a pair of small millisecond values can exercise the exact same
 * escalation logic deterministically in well under a second instead of
 * actually waiting out tens of real seconds. Process-wide, not per-server,
 * matching every other RUNNING_UNIT_TESTS-gated hook already in this file.
 * Gated so neither this storage nor the setter exists in a production
 * build. */
static _Atomic unsigned g_wait_in_flight_graceful_ms_for_tests = 0;
static _Atomic unsigned g_wait_in_flight_grace_ms_for_tests = 0;
void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                   unsigned grace_ms) {
  atomic_store(&g_wait_in_flight_graceful_ms_for_tests, graceful_ms);
  atomic_store(&g_wait_in_flight_grace_ms_for_tests, grace_ms);
}
#endif /* RUNNING_UNIT_TESTS */

static void _timespec_add_ms(struct timespec *ts, unsigned ms) {
  ts->tv_sec += (time_t)(ms / 1000);
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec += 1;
  }
}

/* Waits for srv->in_flight_requests to reach 0, bounded to 30s of graceful
 * waiting exactly as before. If that is not enough, forcibly interrupts
 * every connection still diverted to a worker thread (via
 * _force_unblock_diverted_connections) and waits a further, separately
 * bounded grace period for those newly-erroring workers to actually unwind
 * and release their own in_flight_requests slot.
 *
 * This exists because every caller of this function goes on to call
 * ctpool_shutdown_drain() on the worker pool shortly afterward (via
 * _destroy_detached_pools), and that function has NO timeout of its own: it
 * "blocks until every queued and active task has completed" (see its own
 * doc comment in cthreadpool.h), unconditionally. Without the forced
 * unblock below, a single connection whose worker thread is legitimately
 * blocked forever inside chttp1_stream_read/_write -- a real, reachable
 * case whenever stream_read_timeout_ms and/or response_write_timeout_ms is
 * configured to 0 ("wait indefinitely", chttpsvr_config_t's own documented,
 * legitimate setting for slow legitimate uploads) and the peer stalls
 * without ever closing the connection -- would make this function's own
 * carefully bounded 30s wait meaningless: the timeout would elapse, this
 * function would return anyway, and the very next call the caller makes
 * (ctpool_shutdown_drain) would then hang forever waiting for that exact
 * same stuck task, defeating the whole point of bounding this wait at all.
 * Every caller (chttpsvr_stop()+chttpsvr_start() restart via
 * _wait_and_detach_pools, and chttpsvr_destroy()/chttpsvr_engine_stop() via
 * _drain_and_close_all_connections) shared this exposure identically.
 *
 * Must be called with srv->mutex held; temporarily releases and reacquires
 * it around the shutdown(2) pass (no I/O while holding the lock, matching
 * this codebase's own established convention elsewhere, e.g. the idle
 * pool's liveness probe in chttpclient.c). Does not touch worker_pool/
 * reject_pool; the caller detaches those itself, as before. */
static void _wait_in_flight_bounded(struct chttpserver *srv) {
  if (srv->in_flight_requests <= 0) return;

  unsigned graceful_ms = _CHTTPSVR_GRACEFUL_WAIT_MS;
  unsigned grace_ms = _CHTTPSVR_FORCE_UNBLOCK_GRACE_MS;
#ifdef RUNNING_UNIT_TESTS
  unsigned override_graceful_ms =
      atomic_load(&g_wait_in_flight_graceful_ms_for_tests);
  unsigned override_grace_ms =
      atomic_load(&g_wait_in_flight_grace_ms_for_tests);
  if (override_graceful_ms) graceful_ms = override_graceful_ms;
  if (override_grace_ms) grace_ms = override_grace_ms;
#endif /* RUNNING_UNIT_TESTS */

  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  _timespec_add_ms(&deadline, graceful_ms);
  bool timed_out = false;
  while (srv->in_flight_requests > 0) {
    if (cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
        ETIMEDOUT) {
      timed_out = true;
      break;
    }
  }
  if (!timed_out || srv->in_flight_requests <= 0) return;

  mutex_unlock(srv->mutex);
  _force_unblock_diverted_connections(srv);
  mutex_lock(srv->mutex);
  if (srv->in_flight_requests <= 0) return;

  struct timespec grace_deadline;
  clock_gettime(CLOCK_MONOTONIC, &grace_deadline);
  _timespec_add_ms(&grace_deadline, grace_ms);
  while (srv->in_flight_requests > 0) {
    if (cond_var_timedwait(srv->requests_done_cv, srv->mutex, grace_deadline) ==
        ETIMEDOUT)
      break;
  }
  /* If in_flight_requests is STILL nonzero here, whatever is stuck is not a
   * blocked socket read/write (that class is now unblocked) but something
   * this library has no visibility into or control over, e.g. a handler
   * itself blocked in unrelated, non-socket work; ctpool_shutdown_drain()
   * below is left to wait on it exactly as it always has, since there is no
   * further, safe forcible action this function could take. */
}

static _detached_pools_t _wait_and_detach_pools(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  _wait_in_flight_bounded(srv);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  mutex_unlock(srv->mutex);
  return out;
}

static void _destroy_detached_pools(_detached_pools_t pools) {
  if (pools.worker) {
    ctpool_shutdown_drain(pools.worker);
    ctpool_destroy(pools.worker);
  }
  if (pools.reject) {
    ctpool_shutdown_drain(pools.reject);
    ctpool_destroy(pools.reject);
  }
}

/* __chttpsvr_destroy's own variant of the wait above: additionally closes
 * every still-idle connection, atomically with respect to every place that
 * increments in_flight_requests and reads worker_pool/reject_pool
 * (_conn_start_diverted, _conn_dispatch_reject, _conn_reject_via_pool), by
 * holding srv->mutex continuously from the moment in-flight work is
 * observed to have drained all the way through the idle-close pass.
 * Without this, a keep-alive connection whose request
 * finished (landing back in the idle list) right as the in-flight wait
 * below completed, but which then received a further pipelined request
 * before an separately-locked idle-close pass got to it, could be silently
 * re-diverted; escaping that pass entirely and leaking once this
 * function goes on to release the engine/reactor out from under it (a
 * real, if rare, leak valgrind caught: one orphaned chttpsvr_conn_t after
 * a full 176-case run). Any request that still manages to arrive after
 * this function has already closed a connection, or while it holds
 * srv->mutex, either hits an already-removed event_loop registration (a
 * safe no-op, the same dispatch-time liveness check this codebase already
 * relies on elsewhere) or observes worker_pool == NULL (and, by the same
 * reasoning, reject_pool == NULL) once it finally acquires the lock and
 * falls all the way back to _conn_start_diverted's own synchronous,
 * last-resort inline close; never a leak or a crash. */
static _detached_pools_t _drain_and_close_all_connections(
    struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  _wait_in_flight_bounded(srv);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  _close_all_idle_connections(srv);
  mutex_unlock(srv->mutex);

  /* A single _close_all_idle_connections pass above is not sufficient: a
   * connection accepted moments ago (still mid TLS handshake or mid
   * header-read on the reactor thread, not yet added to the idle list) or a
   * connection some dispatch has already claimed out of the idle list for
   * ordinary processing (e.g. discovering the peer closed, handled directly
   * via _conn_pump -> _conn_close without ever being diverted to a worker,
   * so in_flight_requests never even saw it) is invisible to that one pass
   * -- a real use-after-free a ThreadSanitizer run over tests_tls caught:
   * the reactor thread could still be inside _conn_pump/_conn_free for such
   * a connection after this function had already returned and
   * __chttpsvr_destroy went on to free srv. current_connections
   * (incremented at accept, decremented in _conn_free) spans a connection's
   * entire lifetime regardless of which path closes it, unlike
   * in_flight_requests (worker-diverted requests only) or the idle list
   * (only connections not currently claimed by some dispatch), so waiting
   * for it to reach zero -- repeating the idle-close pass as still-active
   * connections finish and land back in the idle list -- closes the gap
   * completely instead of trusting a single snapshot.
   *
   * Polling rather than a dedicated condition variable, deliberately:
   * broadcasting one from every _conn_free call (the hot per-connection-
   * close path, for every connection ever served, not just during
   * shutdown) would add permanent overhead to close a window that only
   * matters on this cold, once-per-server-lifetime path. */
  struct timespec poll_deadline;
  clock_gettime(CLOCK_MONOTONIC, &poll_deadline);
  poll_deadline.tv_sec += 30;
  while (atomic_load(&srv->current_connections) > 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > poll_deadline.tv_sec ||
        (now.tv_sec == poll_deadline.tv_sec &&
         now.tv_nsec >= poll_deadline.tv_nsec))
      break;
    struct timespec nap = {0, 1000000L}; /* 1ms */
    nanosleep(&nap, NULL);
    _close_all_idle_connections(srv);
  }
  return out;
}

/* Stops listening, unregisters from servers_bundler.servers, drains
 * in-flight requests, closes idle connections, shuts down and destroys the
 * worker pool, and releases this server's engine reference; everything
 * __chttpsvr_destroy used to do inline. Factored out and guarded by
 * srv->teardown_started/teardown_done so it can also be driven, exactly
 * once, from the engine's own _engine_force_stop_quiesce_all pass
 * (chttpsvr_engine_stop() may run that pass while srv is still fully
 * started, concurrently with -- not just before -- an application's own,
 * independent chttpsvr_destroy(srv) call, since chttpsvr_engine_stop() is
 * documented safe to call from a signal handler with no requirement that a
 * caller serialize it against chttpsvr_destroy() via chttpsvr_engine_wait()
 * first); whichever of the two callers reaches a given srv first does the
 * real work, and the OTHER caller BLOCKS until that work has actually
 * finished (teardown_done), rather than returning as an immediate no-op.
 * This matters because __chttpsvr_destroy proceeds straight from this
 * function's return into mutex_destroy(raw->mutex)/freeing raw itself; if
 * the losing caller returned the instant it observed teardown_started
 * (the previous behavior), __chttpsvr_destroy could destroy raw->mutex/
 * raw->idle_mutex and free raw while the winning caller's own call was
 * still using them inside _drain_and_close_all_connections/_engine_release
 * below -- a genuine use-after-free/destroyed-in-use-mutex race, not merely
 * a theoretical one, since _engine_force_stop_quiesce_all's own retry loop
 * already demonstrates the reverse race (a concurrent chttpsvr_destroy())
 * was anticipated for the other direction. See
 * _engine_force_stop_quiesce_all's own doc comment for why this ordering
 * (stop -> unregister -> drain -> release engine ref -> destroy pool) must
 * run to completion before the shared reactor can be torn down. */
static void _quiesce_server_once(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  if (srv->teardown_started) {
    while (!srv->teardown_done)
      cond_var_wait(srv->teardown_done_cv, srv->mutex);
    mutex_unlock(srv->mutex);
    return;
  }
  srv->teardown_started = true;
  /* Wait for every currently in-flight _chttpsvr_resolve-protected call on
   * srv (chttpsvr_start/_stop/_register_handler/_use/_subrouter/...) to
   * finish before touching any of srv's mutable state below. __chttpsvr_
   * destroy already does this exact wait itself before ever calling this
   * function, but _engine_force_stop_quiesce_all (chttpsvr_engine_stop()'s
   * forced path) calls this function directly against every registered
   * server with no such wait of its own -- and unlike destroy, that path can
   * legitimately run concurrently with a chttpsvr_start() that has already
   * taken an engine reference (raw->contributed_to_engine == true) and is
   * still using srv_engine_bundler.reactor further down its own function
   * body (e.g. about to call event_loop_add for the listener). Without this
   * wait, this function could concurrently decide "release this server's
   * engine reference" (below) and unregister/drain srv while chttpsvr_start
   * still believes it holds that exact reference and goes on to register a
   * listener against a reactor that may be torn down (or already gone) by
   * the time it gets there -- and, since event_reg* (unlike the event_loop
   * handle itself) is a bare pointer with no generation check, a later
   * chttpsvr_stop()/_destroy() call on srv would then hand a dangling
   * event_reg* into event_loop_remove(), a genuine use-after-free. Waiting
   * here, under the same srv->mutex/resolve_cv pair _chttpsvr_resolve_unpin
   * already uses, closes that window uniformly for every caller of this
   * function rather than relying on each caller to remember it. */
  while (atomic_load(&srv->pending_resolve_count) > 0)
    cond_var_wait(srv->resolve_cv, srv->mutex);
  mutex_unlock(srv->mutex);

  _chttpsvr_stop_internal(srv);
  _servers_unregister(srv);

  /* Drain in-flight work and close every remaining idle connection BEFORE
   * releasing this server's engine reference below: _engine_release() can
   * be the one that drops the shared reactor's ref count to zero (if srv
   * happens to be the last contributing server), which hands
   * event_loop_destroy(srv_engine_bundler.reactor) off to an async reaper
   * thread; racing this function's own event_loop_remove() calls (inside
   * _drain_and_close_all_connections -> _conn_close) if they ran after
   * releasing instead of before. */
  _detached_pools_t old_pools = _drain_and_close_all_connections(srv);

  bool should_release_engine = false;
  mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
  }
  mutex_unlock(srv->mutex);
  if (should_release_engine) _engine_release();

  _destroy_detached_pools(old_pools);

  mutex_lock(srv->mutex);
  srv->teardown_done = true;
  cond_var_broadcast(srv->teardown_done_cv);
  mutex_unlock(srv->mutex);
}

void __chttpsvr_destroy(chttpsvr srv) {
  if (!srv) return;

  /* Resolve srv through the slot table, marking the slot not-in-use in the
   * same critical section as the lookup: this is what makes a second,
   * concurrent (or later, sequential) destroy call on the same handle value
   * see a resolve failure rather than racing this call's own teardown; see
   * the slot table's own file-level comment and _chttpsvr_resolve's
   * comment for the full design. A stale or already-destroyed handle
   * reaching here is exactly the misuse this redesign exists to catch: it
   * is fatal, not a silent use-after-free/double-free. */
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(srv >> 32);
  uint32_t gen = (uint32_t)(srv & 0xFFFFFFFFu);
  mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot_t *slot = NULL;
  struct chttpserver *raw = NULL;
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *s =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    mutex_unlock(chttpsvr_slot_table.mutex);
    fatal_err(
        "chttpsvr_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpsvr handle)");
  }
  slot->in_use = false; /* blocks ALL future resolves for this handle from
                            this instant, including a second concurrent
                            destroy attempt */
  mutex_unlock(chttpsvr_slot_table.mutex);

  mutex_lock(raw->mutex);
  while (atomic_load(&raw->pending_resolve_count) > 0)
    cond_var_wait(raw->resolve_cv, raw->mutex);
  mutex_unlock(raw->mutex);

  _quiesce_server_once(raw);

  if (raw->tls_ctx) {
    ctls_ctx_release(raw->tls_ctx);
    raw->tls_ctx = NULL;
  }

  /* pending_resolve_count has already drained to 0 above, so no
   * chttpsvr_router_on/_on_stream/_use call can currently be holding a pin
   * on raw; _destroy_router itself never frees a router's own shell (only
   * its contents, after atomically invalidating owner), so a reader that
   * arrives from this point on safely observes CHTTPSVR_INVALID instead of
   * dereferencing freed memory. See chttpsvr_router_shell_registry's own
   * comment for the full reasoning. */
  for (size_t i = 0; i < raw->router_count; i++) {
    _destroy_router(raw->routers[i], raw->m_procs);
  }
  _mem_free(raw->m_procs, raw->routers);
  mutex_destroy(raw->mutex);
  mutex_destroy(raw->idle_mutex);
  mutex_destroy(raw->diverted_mutex);
  cond_var_destroy(raw->requests_done_cv);
  cond_var_destroy(raw->resolve_cv);
  cond_var_destroy(raw->teardown_done_cv);
  rw_lock_destroy(raw->routes_lock);

  clog_close(raw->cl);
  raw->cl = NULL;

  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _mem_free(mp, raw);
  _mem_free(mp, mp);

  /* Release the slot last, only after raw is fully torn down and freed:
   * this is what makes the slot's generation bump (and the free-index
   * push-back) mark the handle as reusable, not any earlier step. Re-fetch
   * by idx rather than reusing `slot`: a concurrent create_chttpsvr_mp's
   * own _chttpsvr_handle_slot_acquire call in between may have reallocated
   * slots' backing array via cvector_push_back, invalidating any pointer
   * into it taken before this second lock acquisition; idx itself is
   * stable. */
  mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot_t *slot2 =
      (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed srv's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(chttpsvr_slot_table.free_indices, &idx);
  mutex_unlock(chttpsvr_slot_table.mutex);
}

/* Frees the slot table's own bookkeeping arrays at process exit, so
 * make memtest's --show-leak-kinds=all does not report them as still-
 * reachable. Unlike chttpclient.c's analogous _cleanup_default_client,
 * there is no library-owned singleton server to destroy first here (this
 * module has no equivalent of chttp_default_client), so this destructor
 * only ever has the slot table itself to release. This does not and cannot
 * retroactively fix an application that created a chttpsvr and never
 * destroyed it itself; that was already an uncaught leak before this
 * redesign and remains exactly as much of one after; not this change's
 * problem to solve. This is sound given every server the application
 * created was itself destroyed before process exit; the same
 * precondition this test suite already satisfies today for a clean
 * make memtest.
 *
 * MUST call_once here, exactly as chttpclient.c's own destructor does and
 * for the identical reason: __attribute__((destructor)) functions run
 * unconditionally for the whole shared object regardless of which parts of
 * it were actually used, so a process that links this library but never
 * creates a single chttpsvr would otherwise lock a never-pthread_mutex_
 * init'd mutex here. */
__attribute__((destructor)) static void _cleanup_chttpsvr_slot_table(void) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  mutex_lock(chttpsvr_slot_table.mutex);
  __cvector_destroy(chttpsvr_slot_table.slots);
  __cvector_destroy(chttpsvr_slot_table.free_indices);
  mutex_unlock(chttpsvr_slot_table.mutex);
}

ccol_retval_t chttpsvr_start(chttpsvr h, const chttpsvr_config_t *cfg) {
  /* Argument-only validation stays here, before any resolve, exactly like
   * chttpclient_set_tls's identical cert/key-pair check: it never touches
   * srv/raw, so it needs no pin/unpin of its own. */
  if (cfg && cfg->port == 0 &&
      !(cfg->host && strncmp(cfg->host, _CHTTPSVR_UNIX_PREFIX,
                             strlen(_CHTTPSVR_UNIX_PREFIX)) == 0))
    return ccol_invalid_args;

  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;

  chttpsvr_config_t default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  mutex_lock(raw->mutex);
  if (raw->started) {
    mutex_unlock(raw->mutex);
    _chttpsvr_resolve_unpin(raw); /* exit 1 */
    return ccol_not_permitted;
  }
  /* A prior stop/quiesce cycle (whether via chttpsvr_stop()+chttpsvr_start()
   * restart, or a chttpsvr_engine_stop() force-stop this server happened to
   * survive without being chttpsvr_destroy()'d) may have left
   * teardown_started/teardown_done set; this server is legitimately starting
   * fresh again, so _quiesce_server_once must be willing to run its real
   * teardown work again the next time this server actually is destroyed. */
  raw->teardown_started = false;
  raw->teardown_done = false;
  mutex_unlock(raw->mutex);

  _destroy_detached_pools(_wait_and_detach_pools(raw));

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
                                           raw->m_procs, &pool_err);
  if (!new_pool) {
    _chttpsvr_resolve_unpin(raw); /* exit 2 */
    return ccol_not_enough_memory;
  }

  /* reject_pool's own thread count scales with worker_pool's own resolved
   * size (nthreads, above -- already the CPU-count-resolved value when
   * cfg->worker_thread_count <= 0, not the raw, possibly-<=0 config field)
   * rather than a fixed constant, floored at _CHTTPSVR_REJECT_POOL_MIN_
   * THREADS so a single- or few-worker server still gets real concurrency
   * for its rejection traffic; see that constant's own comment for the
   * reasoning. Bounded queue (_CHTTPSVR_REJECT_POOL_QUEUE_CAP; see
   * reject_pool's own field comment and that constant's own comment for
   * why); if this fails, new_pool above must be torn down too rather than
   * leaked. */
  int reject_nthreads = nthreads / 2;
  if (reject_nthreads < _CHTTPSVR_REJECT_POOL_MIN_THREADS)
    reject_nthreads = _CHTTPSVR_REJECT_POOL_MIN_THREADS;
  char *reject_pool_err = NULL;
  ctpool new_reject_pool = create_cthread_pool_mp(
      (size_t)reject_nthreads, _CHTTPSVR_REJECT_POOL_QUEUE_CAP, raw->m_procs,
      &reject_pool_err);
  if (!new_reject_pool) {
    ctpool_shutdown_drain(new_pool);
    ctpool_destroy(new_pool);
    _chttpsvr_resolve_unpin(raw); /* exit 3 */
    return ccol_not_enough_memory;
  }

  mutex_lock(raw->mutex);
  raw->worker_pool = new_pool;
  raw->reject_pool = new_reject_pool;
  mutex_unlock(raw->mutex);

  atomic_store(&raw->stream_read_timeout_ms, cfg->stream_read_timeout_ms);
  atomic_store(&raw->max_body_read_duration_ms, cfg->max_body_read_duration_ms);
  atomic_store(&raw->response_write_timeout_ms,
               cfg->response_write_timeout_ms ? cfg->response_write_timeout_ms
                                              : cfg->stream_read_timeout_ms);
  {
    /* cfg->idle_timeout_ms/read_timeout_ms are `long` (64-bit on this
     * platform), but raw->idle_timeout_ms is `unsigned` (32-bit), matching
     * every sibling timeout field. A bare (unsigned) cast of a value >
     * UINT_MAX (~49.7 days in ms) would silently wrap: a value that is an
     * exact multiple of 2^32 wraps to 0, which is this field's own "idle
     * timeout disabled" sentinel, silently disabling the idle-timeout sweep
     * instead of applying the (very long, but intentional) timeout that was
     * actually configured; any other large value silently aliases to an
     * unrelated, much shorter timeout. Clamped to UINT_MAX instead, exactly
     * mirroring _to_stream_timeout_ms's own identical clamp-instead-of-wrap
     * fix further up in this file: UINT_MAX is still a real, expressible,
     * finite timeout, and does not collide with the 0 sentinel. */
    long eff_idle_ms = (cfg->idle_timeout_ms > 0) ? cfg->idle_timeout_ms
                                                  : cfg->read_timeout_ms;
    unsigned idle_ms_clamped = 0;
    if (eff_idle_ms > 0) {
      unsigned long eff_idle_ul = (unsigned long)eff_idle_ms;
      idle_ms_clamped = (eff_idle_ul > (unsigned long)UINT_MAX)
                            ? UINT_MAX
                            : (unsigned)eff_idle_ul;
    }
    atomic_store(&raw->idle_timeout_ms, idle_ms_clamped);
  }
  atomic_store(&raw->max_body_size, cfg->max_body_size);
  atomic_store(&raw->max_header_bytes, cfg->max_header_bytes);
  atomic_store(&raw->max_connections, cfg->max_connections);
  raw->enable_keepalive = cfg->enable_keepalive;

  if (raw->tls_ctx) {
    ctls_ctx_release(raw->tls_ctx);
    raw->tls_ctx = NULL;
  }
  if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
    ctls_ctx_t *tls_ctx = ctls_ctx_new_mp(raw->m_procs, NULL);
    if (!tls_ctx) {
      _destroy_detached_pools(_wait_and_detach_pools(raw));
      _chttpsvr_resolve_unpin(raw); /* exit 4 */
      return ccol_not_enough_memory;
    }
    if (ctls_ctx_cert_add(tls_ctx, NULL, cfg->tls->cert_path,
                          cfg->tls->key_path, NULL, NULL) != ccol_success) {
      ctls_ctx_release(tls_ctx);
      _destroy_detached_pools(_wait_and_detach_pools(raw));
      _chttpsvr_resolve_unpin(raw); /* exit 5 */
      return ccol_unexpected_failure;
    }
    if (cfg->tls->ca_bundle_path)
      ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL);
    raw->tls_ctx = tls_ctx;
  }

  bool need_acquire;
  mutex_lock(raw->mutex);
  need_acquire = !raw->contributed_to_engine;
  if (need_acquire) raw->contributed_to_engine = true;
  mutex_unlock(raw->mutex);

  if (need_acquire) {
    ccol_retval_t engine_rc = _engine_acquire();
    if (engine_rc != ccol_success) {
      mutex_lock(raw->mutex);
      raw->contributed_to_engine = false;
      mutex_unlock(raw->mutex);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _destroy_detached_pools(_wait_and_detach_pools(raw));
      _chttpsvr_resolve_unpin(raw); /* exit 6 */
      return engine_rc;
    }
  }
  /* Registered here, as soon as raw is known to hold a live engine reference
   * (whether just acquired above, or already held from an earlier start on
   * this same server), rather than at the very end of this function after
   * the listener is already live: _engine_force_stop_quiesce_all (chttpsvr_
   * engine_stop()'s forced path) only quiesces servers it can find in
   * servers_bundler.servers, and only a registered server is guaranteed to
   * be waited on (see _quiesce_server_once's own pending_resolve_count wait)
   * before that path releases its engine reference and, potentially, tears
   * the shared reactor down. Registering only after event_loop_add for the
   * listener (the previous order) left a real window where a concurrent
   * chttpsvr_engine_stop() could destroy the reactor out from under a
   * server that had already taken a reference and was still using it a few
   * lines below, with no mechanism serializing the two. _servers_register is
   * idempotent, so calling it here rather than at the end is safe for every
   * caller, including a restart that already registered raw on an earlier
   * chttpsvr_start() call. */
  _servers_register(raw);
#ifdef RUNNING_UNIT_TESTS
  _start_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
  _idle_sweep_start_if_needed();

  bool is_unix = false;
  char *unix_path = NULL;
  int lfd =
      _make_listen_socket(cfg->host, cfg->port, cfg->enable_reuseport,
                          cfg->ipv6_only, &is_unix, &unix_path, raw->m_procs);
  if (lfd < 0) {
    _SRV_ENGINE_LOG(log_error, "listen socket setup failed host=%s port=%u",
                    cfg->host ? cfg->host : "0.0.0.0", (unsigned)cfg->port);
    if (raw->tls_ctx) {
      ctls_ctx_release(raw->tls_ctx);
      raw->tls_ctx = NULL;
    }
    _destroy_detached_pools(_wait_and_detach_pools(raw));
    _chttpsvr_resolve_unpin(raw); /* exit 7 */
    return ccol_unexpected_failure;
  }

  /* Published BEFORE event_loop_add below, not after: event_loop_add makes
   * the registration immediately live, so a connection arriving in the
   * window between registration and these fields being set could
   * otherwise be dispatched to _listener_on_readable while it still
   * observed listen_fd's pre-start default (-1) (see listen_fd's own field
   * comment on struct chttpserver). Reset back to -1 below if event_loop_add
   * itself goes on to fail, so a failed start doesn't leave listen_fd
   * pointing at an fd this function is about to close. */
  atomic_store(&raw->listen_fd, lfd);
  atomic_store(&raw->is_unix_socket, is_unix);

  call_once(srv_parser_bundler.once, _init_parser_settings);
  char *reg_err = NULL;
  event_reg *lreg = event_loop_add(
      srv_engine_bundler.reactor, selectable_from_fd(lfd, ccol_select_read),
      (event_handlers_t){.on_readable = _listener_on_readable}, raw, &reg_err);
  if (!lreg) {
    atomic_store(&raw->listen_fd, -1);
    close(lfd);
    if (is_unix && unix_path) unlink(unix_path);
    _mem_free(raw->m_procs, unix_path);
    if (raw->tls_ctx) {
      ctls_ctx_release(raw->tls_ctx);
      raw->tls_ctx = NULL;
    }
    _destroy_detached_pools(_wait_and_detach_pools(raw));
    _chttpsvr_resolve_unpin(raw); /* exit 8 */
    return ccol_unexpected_failure;
  }

  mutex_lock(raw->mutex);
  raw->unix_socket_path = unix_path;
  raw->listen_reg = lreg;
  raw->started = true;
  mutex_unlock(raw->mutex);

  /* raw was already registered above, right after the engine reference was
   * confirmed; nothing further to do here. */
  _chttpsvr_resolve_unpin(raw); /* exit 9 (success) */
  return ccol_success;
}

/* The actual work of chttpsvr_stop, operating directly on an already-
 * resolved struct chttpserver*. Split out so _quiesce_server_once (which
 * already holds a resolved raw pointer, obtained via servers_bundler's own
 * registry, never via a chttpsvr handle at all) can call this directly
 * without going through the public, handle-resolving chttpsvr_stop;
 * critical now that chttpsvr is a value handle, not a pointer: passing a
 * raw struct chttpserver* where a handle is expected would either fail to
 * compile or, worse, misinterpret the pointer's bit pattern as a
 * {slot index, generation} pair. Mirrors chttpclient.c's own split between
 * chttp_do_internal (operates on struct chttpclient*) and the public,
 * resolve-then-call chttpclient_do wrapper. */
static void _chttpsvr_stop_internal(struct chttpserver *raw) {
  mutex_lock(raw->mutex);
  bool was_started = raw->started;
  int lfd = atomic_load(&raw->listen_fd);
  event_reg *lreg = raw->listen_reg;
  bool is_unix = atomic_load(&raw->is_unix_socket);
  char *unix_path = raw->unix_socket_path;
  if (was_started) {
    raw->started = false;
    atomic_store(&raw->listen_fd, -1);
    raw->listen_reg = NULL;
    raw->unix_socket_path = NULL;
  }
  mutex_unlock(raw->mutex);
  if (!was_started) return;

  /* event_loop_remove blocks until any in-progress _listener_on_readable
   * call for lreg has returned and guarantees none will fire again
   * afterward (see its own doc comment in cthreadcomm.h), so close(lfd)
   * below can never race a concurrent accept() on this fd. */
  if (lreg) event_loop_remove(srv_engine_bundler.reactor, lreg);
  close(lfd);
  if (is_unix && unix_path) unlink(unix_path);
  _mem_free(raw->m_procs, unix_path);
}

void chttpsvr_stop(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return;
  _chttpsvr_stop_internal(raw);
  _chttpsvr_resolve_unpin(raw);
}

ccol_retval_t chttpsvr_set_engine_logger(clog cl) {
  if (!cl) return ccol_invalid_args;
  clog derived = clog_derive(cl);
  if (!derived) return ccol_not_enough_memory;
  clog_set_field(derived, "component", "http-server-engine");
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  clog old = srv_engine_bundler.log;
  srv_engine_bundler.log = derived;
  mutex_unlock(srv_engine_bundler.mutex);
  if (old) clog_close(old);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    srv_engine_bundler.mprocs_storage = *mp;
    srv_engine_bundler.mprocs = &srv_engine_bundler.mprocs_storage;
  } else {
    srv_engine_bundler.mprocs = NULL;
  }
  mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_num_reactor_threads(size_t num_threads) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  srv_engine_bundler.num_reactor_threads = num_threads;
  mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

/* White-box test helper exposing the reactor thread count actually wired
 * into the last-created reactor. Not part of the public API; gated so this
 * symbol does not leak into a production build of libccollections.so,
 * matching the identical convention chttpclient.c already established for
 * its own engine-internal-state test helpers. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_engine_num_reactor_threads_for_tests(void) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  size_t n = srv_engine_bundler.last_resolved_num_reactor_threads;
  mutex_unlock(srv_engine_bundler.mutex);
  return n;
}
#endif /* RUNNING_UNIT_TESTS */

/* White-box test helper exposing g_reject_task_run_count_for_tests (see
 * that variable's own comment): how many rejection responses have actually
 * been carried out on a reject_pool thread, process-wide, since the
 * counter's own zero-initialization. A test reads this before and after its
 * own window and asserts on the delta, since other tests in the same
 * process may also exercise rejections. Not part of the public API; gated
 * so this symbol does not leak into a production build of
 * libccollections.so, matching the identical convention
 * _chttpsvr_engine_num_reactor_threads_for_tests already established just
 * above. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_reject_pool_task_count_for_tests(void) {
  return atomic_load(&g_reject_task_run_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Resolves h to its underlying struct chttpserver* WITHOUT pinning it (does
 * not touch pending_resolve_count at all): a bare slot-table lookup, safe
 * for tests specifically because test code calling this runs synchronously,
 * single-threaded, with no concurrent destroy to race in the first place;
 * unlike _chttpsvr_resolve, there is no matching unpin call a test needs to
 * remember, which would otherwise be an easy gap to leave (a forgotten
 * unpin would leave pending_resolve_count permanently nonzero on that
 * server, silently hanging every future chttpsvr_destroy call against it).
 * Returns NULL under the exact same conditions _chttpsvr_resolve does.
 * Mirrors chttpclient.c's identical _chttpcli_resolve_for_tests. */
#ifdef RUNNING_UNIT_TESTS
struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

/* Reads how many slots the chttpsvr handle table currently holds: lets a
 * test assert that a create/destroy churn loop reuses freed slots rather
 * than growing the table without bound. Mirrors chttpclient.c's identical
 * _chttpcli_slot_table_capacity_for_tests. */
size_t _chttpsvr_slot_table_capacity_for_tests(void) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  mutex_lock(chttpsvr_slot_table.mutex);
  size_t n = cvector_elem_count(chttpsvr_slot_table.slots);
  mutex_unlock(chttpsvr_slot_table.mutex);
  return n;
}

/* White-box test helper exposing the resolved idle_timeout_ms value a
 * chttpsvr_start() call actually stored, after chttpsvr_config_t.idle_
 * timeout_ms/read_timeout_ms (both `long`) were combined and clamped into
 * the internal `unsigned` field (see that clamp's own comment in
 * chttpsvr_start). Lets a test assert the clamp directly -- a configured
 * value >= 2^32 ms must land on UINT_MAX, never silently wrap to 0 (this
 * field's own "disabled" sentinel) or some other unrelated value -- without
 * waiting out an astronomically long real idle-timeout window. Returns 0
 * for an invalid handle, indistinguishable from a genuinely-resolved
 * server whose idle timeout really is disabled; a test using this accessor
 * is expected to already know h is otherwise valid. */
unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  return atomic_load(&raw->idle_timeout_ms);
}
#endif /* RUNNING_UNIT_TESTS */

void chttpsvr_engine_stop(void) { _engine_force_stop(); }

void chttpsvr_engine_wait(void) { _engine_wait_until_stopped(); }

ccol_retval_t chttpsvr_register_handler(chttpsvr h, chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  /* raw->routers can be reallocated (moved) by a concurrent
   * chttpsvr_subrouter() call; snapshot routers[0] under routes_lock
   * before releasing it, rather than dereferencing raw->routers directly,
   * to avoid racing that realloc. The root router itself is never freed
   * while raw is alive, so the snapshotted pointer stays valid after the
   * lock is released. */
  rw_lock_rdlock(raw->routes_lock);
  chttpsvr_router *root = raw->routers[0];
  rw_lock_unlock(raw->routes_lock);
  ccol_retval_t rv = _router_add_route(root, method, pattern, fn, ctx, false);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_register_streaming_handler(chttpsvr h,
                                                  chttp_method_t method,
                                                  const char *pattern,
                                                  chttpsvr_handler_fn fn,
                                                  void *ctx) {
  if (!pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  /* See chttpsvr_register_handler's comment above: snapshot routers[0]
   * under routes_lock to avoid racing chttpsvr_subrouter's realloc. */
  rw_lock_rdlock(raw->routes_lock);
  chttpsvr_router *root = raw->routers[0];
  rw_lock_unlock(raw->routes_lock);
  ccol_retval_t rv = _router_add_route(root, method, pattern, fn, ctx, true);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_use(chttpsvr h, chttpsvr_middleware_fn fn, void *ctx) {
  if (!fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  /* See chttpsvr_register_handler's comment above: snapshot routers[0]
   * under routes_lock to avoid racing chttpsvr_subrouter's realloc. */
  rw_lock_rdlock(raw->routes_lock);
  chttpsvr_router *root = raw->routers[0];
  rw_lock_unlock(raw->routes_lock);
  ccol_retval_t rv = _router_add_mw(root, fn, ctx);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

chttpsvr_router *chttpsvr_subrouter(chttpsvr h, const char *prefix) {
  if (!prefix || prefix[0] != '/') return NULL;
  if (strstr(prefix, "//")) return NULL;

  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return NULL;

  chttpsvr_router *r = _create_router(raw, prefix, raw->m_procs);
  if (!r) {
    _chttpsvr_resolve_unpin(raw); /* exit 1 */
    return NULL;
  }
  atomic_store(&r->owner, h);

  rw_lock_wrlock(raw->routes_lock);
  if (raw->router_count >= raw->router_cap) {
    if (raw->router_cap > (SIZE_MAX - 2) / 2 ||
        raw->router_cap * 2 + 2 > SIZE_MAX / sizeof(chttpsvr_router *)) {
      rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return NULL;
    }
    size_t nc = raw->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)_mem_realloc(
        raw->m_procs, raw->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return NULL;
    }
    raw->routers = nr;
    raw->router_cap = nc;
  }
  raw->routers[raw->router_count++] = r;
  rw_lock_unlock(raw->routes_lock);
  _chttpsvr_resolve_unpin(raw); /* exit 3 (success) */
  return r;
}

/* Shared entry sequence for every chttpsvr_router_on/_on_stream/_use call:
 * read router->owner (always memory-safe; see chttpsvr_router_shell_
 * registry's own comment for why the shell this field lives in is never
 * freed) and resolve/pin it through the ordinary chttpsvr slot table.
 * Returns NULL if the owning server has been destroyed; otherwise returns
 * the resolved, pinned server, and the caller MUST call
 * _chttpsvr_resolve_unpin(raw) exactly once when done. */
static struct chttpserver *_chttpsvr_router_reader_enter(
    chttpsvr_router *router) {
  chttpsvr owner = atomic_load(&router->owner);
  return _chttpsvr_resolve(owner);
}

ccol_retval_t chttpsvr_router_on(chttpsvr_router *router, chttp_method_t method,
                                 const char *pattern, chttpsvr_handler_fn fn,
                                 void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _router_add_route(router, method, pattern, fn, ctx, false);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_router_on_stream(chttpsvr_router *router,
                                        chttp_method_t method,
                                        const char *pattern,
                                        chttpsvr_handler_fn fn, void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _router_add_route(router, method, pattern, fn, ctx, true);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_router_use(chttpsvr_router *router,
                                  chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _router_add_mw(router, fn, ctx);
  _chttpsvr_resolve_unpin(raw);
  return rv;
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
   * conn->hdr_names/hdr_values; scan backward so the LAST occurrence wins,
   * matching this module's documented behavior for a duplicated header (see
   * streaming_repeated_header in tests.c). */
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
  /* _send_response() writes name/value verbatim onto the wire as
   * "name:value\r\n", with no further escaping; an embedded CR or LF byte
   * would let a caller that reflects any request-controlled data (a query
   * parameter, a path parameter, an echoed request header) into a response
   * header inject arbitrary extra header lines, or split the response into
   * two, on behalf of whoever controls that data (classic HTTP response
   * splitting / CRLF injection). Rejected outright here, at the one place
   * every response header is set, rather than left to every caller to
   * sanitize its own inputs. */
  if (strpbrk(name, "\r\n") || strpbrk(value, "\r\n")) return ccol_invalid_args;
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
