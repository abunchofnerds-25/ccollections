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
  /* Number of '/'-delimited segments in prefix (0 for the root's own ""
   * placeholder and for the special "/" prefix; see chttpsvr_subrouter's own
   * "/" prefix edge-case doc comment). Computed once, in _create_router, from
   * the already trailing-slash-stripped/consecutive-slash-free prefix string;
   * used only to maintain struct chttpserver's own max_prefix_seg_count
   * (see that field's own comment), never read back off this router directly
   * by _prefix_matches, which instead walks router->prefix itself segment by
   * segment exactly as before (prefix strings are short and operator-
   * controlled, so there is nothing worth caching on that side; only the
   * request path's own, potentially-repeated-per-router percent-decoding is
   * worth sharing, which is what max_prefix_seg_count enables). */
  int prefix_seg_count;
  chttpsvr_mw_node_t *mw_head;
  chttpsvr_mw_node_t *mw_tail;
  int mw_count; /* number of registered middleware; capped at _CHTTPSVR_MAX_MW
                 */
  chttpsvr_route_t **routes; /* pointer array; each entry is a stable alloc */
  size_t route_count;
  size_t route_cap;
  /* Running max of every route->seg_count ever registered on this router
   * (routes are only ever added, never removed, so a running max is always
   * correct without ever needing to be recomputed). Bounds how many raw
   * segments of an incoming sub_path _find_route's own per-router decode
   * cache (_seg_cache_t) ever needs to split/decode, regardless of how many
   * segments the actual (client-controlled) request path itself contains;
   * see _seg_cache_build's own doc comment for why this bound matters. */
  int max_route_seg_count;
  struct chttpserver *srv;       /* back-pointer; do not read directly from
                                  * chttpsvr_router_on/_on_stream/_use; see
                                  * owner's own comment. */
  ccol_memmgmt_procs_t *m_procs; /* server's own allocator, for this
                                  * router's OWNED CONTENTS (prefix, mw
                                  * list, routes array + route data) only;
                                  * the chttpsvr_router struct itself (this
                                  * shell) is deliberately NOT allocated
                                  * through this, see _create_router. */
  /* The chttpsvr handle that owns this router; CHTTPSVR_INVALID only
   * momentarily for the root router (between _create_router, before a
   * handle has even been minted, and create_chttpsvr_mp storing the freshly
   * minted handle here) and for any router (root included) whose owning
   * server has since been destroyed (see _destroy_router). The root router
   * is never exposed to a caller as a chttpsvr_router*, so
   * chttpsvr_router_on/_on_stream/_use never resolve this field for it; this
   * field is set for root anyway, purely so the exit-time shell-registry
   * destructor below can tell a still-live root apart from an already-
   * destroyed one, exactly as it already can for every sub-router. _Atomic:
   * written by _destroy_router without holding any lock a concurrent reader
   * is guaranteed to also hold (there is no such lock; see the comment below
   * on why), so a plain field here would
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
   * lock taken AFTER the read could ever prevent; confirmed as a real,
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
 * threads (every rejection (404/405/500 unmatched/malformed routes and the
 * pool-full 503 case alike) is routed there; see _conn_reject_via_pool)
 * or the last-resort synchronous fallback on the calling thread (reactor or
 * worker) when reject_pool itself is unavailable or its own (bounded) queue
 * is full. One shared constant, not a separate value per call site: none of
 * these should be allowed to tie up their thread for long on a slow-reading
 * peer, so there is no reason for any of them to be more generous than the
 * others. */
#define _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS 100

/* Unconditional cap, in ms, on the *total* wall-clock time any single
 * internally-generated, small, fixed-shape write may take (currently a
 * courtesy rejection response's send, see _conn_reject_and_close, or the
 * "100 Continue" interim line, see _write_interim_continue), applied
 * regardless of conn->srv->max_response_write_duration_ms's own configured
 * value (see _response_write_deadline_ok's own comment for exactly how the
 * two combine). _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS alone only ever bounds one
 * individual write(2)-equivalent call; a peer that reads a byte or two just
 * before each such call's own 100ms timeout expires can keep either write's
 * own retry loop going indefinitely, exactly the "trickle forever" pattern
 * max_response_write_duration_ms exists to close for a real, handler-
 * supplied response; but that knob defaults to 0 ("disabled"), and an
 * operator who left it disabled almost certainly meant that for their own
 * handler-controlled response bodies, not for this library's own small,
 * fixed-shape internal writes (no body, a handful of header bytes for a
 * rejection; a fixed 25-byte status line for the interim continue), which
 * have no legitimate reason to ever need more than a couple of seconds to
 * send even under real load. Bounding these unconditionally, on this
 * project's own single-reactor-thread-by-default design (num_reactor_threads
 * defaults to 1), closes a real denial-of-service surface during ordinary,
 * non-shutdown operation: a slow-reading peer holding open a worker-pool
 * thread (the interim-continue case, reachable via an ordinary
 * "Expect: 100-continue" request to any body-accepting route; standard
 * client behavior, e.g. curl's own default for large uploads) or landing on
 * reject_pool's own synchronous fallback path, which then runs on the sole
 * reactor thread in the default configuration, would otherwise be able to
 * exhaust the worker pool or stall every other connection's events for as
 * long as it kept trickling reads. 2 seconds is generous for any real
 * network path (loopback or otherwise) to deliver well under a kilobyte of
 * data, yet short enough to bound the worst case tightly. */
#define _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS 2000

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
 * rejected connections can't grow reject_pool's backlog (and therefore
 * this process's memory, one queued chttpsvr_conn_t per pending reject-close)
 * without limit; once full, ctpool_try_submit fails and
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

/** Middleware dispatch state. Embedded directly in chttpsvr_conn_t (itself
 * heap-allocated and persistent across an entire keep-alive connection's
 * lifetime, not stack-allocated or per-request); reset per-request the same
 * way the rest of that struct's own per-request scratch fields are. */
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
  event_reg reg; /* current registration; EVENT_REG_INVALID while diverted */
  ctls_conn_t *tls;
  conn_state_t state;
  chttp1_parser_t parser;

  /* Per-request scratch. */
  chttp_method_t method;
  /* owned, RAW (still percent-encoded): route matching must operate on this
   * exact form; _match_route_cached/_prefix_matches split on a literal
   * '/' and decode each segment individually, which is the only way to tell
   * an actual path separator from a %2F encoded one inside a {param}
   * segment. Decoding the whole path eagerly here (as an earlier version of
   * this function did) would turn %2F into a real '/' before segmentation,
   * silently splitting one param segment into two path segments, and would
   * also have to rejects the entire request on any malformed %XX anywhere
   * in the path; this module's own tests document that a malformed
   * encoding in one segment must fall out as an ordinary route mismatch
   * (404), not a 400, since _seg_cache_get already treats a decode failure
   * as "this segment doesn't match" further down. */
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
  /* Set when _write_interim_continue() wrote fewer than the full "HTTP/1.1
   * 100 Continue\r\n\r\n" line before failing, timing out, or hitting
   * max_response_write_duration_ms's own deadline (a genuine short write,
   * not a total one): the bytes it did manage to write are already
   * irrevocably on the wire, so any further write on this connection would
   * either land after a truncated status line (corrupting the client's own
   * framing) or be pointless if the peer is already gone. Consulted by
   * _task_worker to suppress the real final response send and force the
   * connection closed instead of layering more bytes onto a stream that may
   * already be malformed from the client's point of view. */
  bool interim_write_failed;
  growbuf_t body; /* buffered-route whole body, or streaming pending bytes */
  size_t body_bytes_seen; /* cumulative, for max_body_size enforcement */
  bool body_too_large;
  /* Set only by _on_body's own body-buffer growth failure (a genuine
   * _mem_realloc failure while accumulating body bytes, never a
   * max_body_size rejection): distinct from body_too_large/transfer_aborted
   * so a streaming route's chttpsvr_req_stream_error() can report a real
   * server-side allocation failure as ccol_not_enough_memory, not as
   * ccol_http_transfer_aborted's "connection closed or malformed framing",
   * which it plainly is not. Checked with the same priority body_too_large
   * already has relative to transfer_aborted (before it, in chttpsvr_req_
   * stream_error()); the two are mutually exclusive in practice (_on_body
   * returns 1, aborting the parse, the first time either condition is hit,
   * so no later call in the same request can set the other), but the
   * ordering is still meaningful in case that ever changes. */
  bool body_alloc_failed;
  /* Set when a streaming route's chttpsvr_req_read() hits a truncated body
   * (peer EOF or a hard I/O error before the message finished framing) or a
   * chttp1_parser-level CHTTP1_USER/CHTTP1_ERROR that isn't a max_body_size
   * rejection or a body_alloc_failed one (those are reported via their own,
   * higher-priority flags instead; see chttpsvr_req_stream_error()'s own
   * priority order). Read only by chttpsvr_req_stream_error(); the
   * buffered-route path (_drain_body) has no equivalent need for this,
   * since it reports ccol_http_transfer_aborted directly as its own return
   * value instead of through a flag a caller reads back out of conn
   * afterward. */
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

  /* max_response_write_duration_ms bookkeeping; see _send_response's own
   * conn/is_reject parameters and _response_write_deadline_ok/
   * _shrink_timeout_to_deadline. Consulted for the real, matched-route
   * response send in _task_worker, a rejection's own courtesy response send
   * in _conn_reject_and_close, and the "100 Continue" interim line in
   * _write_interim_continue; the latter two additionally have
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS applied as an unconditional
   * ceiling on top of whatever max_response_write_duration_ms itself
   * allows. */
  struct timespec write_deadline;
  bool write_deadline_set;

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

#if FORK_SAFETY_REQUIRED
/* Forward declarations: bodies defined further below, once struct
 * chttpserver, srv_engine_bundler, servers_bundler, and chttpsvr_router_
 * shell_registry are all declared (they dereference/lock all of them);
 * registered from _chttpsvr_slot_table_init_globals below, the first point
 * in the file that runs once, lazily, the first time this module is used at
 * all. Mirrors cthreadpool.c's/cthreadcomm.c's own identical forward-
 * declare-then-define-after-the-struct placement exactly.
 *
 * This entire fork()-safety mechanism (these three handlers and their
 * at_fork() registration below) is compiled out entirely when FORK_SAFETY_
 * REQUIRED is defined to 0; see that macro's own doc comment in common.h. */
static void _chttpsvr_atfork_prepare(void);
static void _chttpsvr_atfork_release(void);
static void _chttpsvr_atfork_child_release(void);

/* Not declared in cthreadcomm.h/cthreadpool.h/clogger.h (not part of any of
 * those modules' public API): narrow, deliberate escape hatches that let a
 * dependent module force cthreadcomm's/cthreadpool's/clogger's own
 * at_fork() registration to happen before its own; see
 * _chttpsvr_slot_table_init_globals's own call site below, and each
 * function's own doc comment in its home file, for the full reasoning and
 * the real AB-BA deadlock shape this closes. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void);
void _ctpool_ensure_atfork_registered_before_caller(void);
void _clog_ensure_atfork_registered_before_caller(void);
#endif

static void _chttpsvr_slot_table_init_globals(void) {
  if (mutex_init(chttpsvr_slot_table.mutex) != 0)
    fatal_err("chttpsvr slot table: failed to initialize mutex");
  chttpsvr_slot_table.slots = cvector_create(sizeof(chttpsvr_slot_t), NULL);
  if (!chttpsvr_slot_table.slots)
    fatal_err("chttpsvr slot table: failed to allocate slots vector");
  chttpsvr_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpsvr_slot_table.free_indices)
    fatal_err("chttpsvr slot table: failed to allocate free-index vector");
#if FORK_SAFETY_REQUIRED
  /* fork() duplicates only the calling thread; see _chttpsvr_atfork_
   * prepare's own doc comment for the full hazard this closes (a lock held
   * by some other, now-vanished thread being inherited by the child
   * already, and permanently, locked).
   *
   * Force cthreadcomm's (event_loop's), cthreadpool's, and clogger's own
   * at_fork() triples to be registered NOW, strictly before this module's
   * own at_fork() call below: pthread_atfork's prepare handlers run in
   * REVERSE registration order, so this guarantees _chttpsvr_atfork_prepare
   * always runs FIRST at every future fork(), locking this module's own
   * locks (srv_engine_bundler.mutex, servers_bundler.mutex, every live
   * server's own mutex, ...) before event_loop's, ctpool's, or clogger's own
   * prepare handlers ever get a chance to lock event_loop_slot_table.mutex/
   * ctpool_slot_table.mutex/clog_slot_table.rwlock or any live event_loop/
   * pool/clog_shared_t's own internal locks; this is the exact same order
   * every ordinary call in this file already uses (_engine_acquire holds
   * srv_engine_bundler.mutex for its entire body, including its own nested
   * call into event_loop_create_with_mprocs() and, when no engine-wide
   * logger exists yet, its own direct clog_open_fd_mp() call; _SRV_ENGINE_
   * LOG holds the same mutex around every log_info/log_warn/... call it
   * makes through an already-open logger). Without this, whichever of the
   * four subsystems happens to be used FIRST by the embedding application
   * determines the real fork()-time lock order purely by accident, which can
   * end up reversed relative to this nesting: reproduced directly via
   * ThreadSanitizer (tests/chttpserver's own test_engine_stop_tsan target)
   * for the event_loop/ctpool pair, which reported two real lock-order-
   * inversion cycles (srv_engine_bundler.mutex versus event_loop_slot_
   * table.mutex, and a second, three-way cycle additionally involving
   * ctpool_slot_table.mutex), both tracing to exactly this accidental-
   * registration-order hazard. The clogger pairing is the identical hazard
   * shape, reachable without ThreadSanitizer: every handle-resolving public
   * chttpsvr_* function (chttpsvr_start/_stop/_use/_register_handler/
   * _register_streaming_handler/_subrouter) tolerates an invalid handle and
   * reports it as a plain error rather than requiring the caller to
   * guarantee validity first, but still runs this module's own call_once
   * (via _chttpsvr_resolve) before ever checking the handle; a process that
   * makes such a call before ever using clog for anything else would
   * otherwise register this module's own at_fork() with no forced clogger
   * registration first, letting a later, independent first use of clog
   * register AFTER this module's own and invert the required order. This is
   * the identical bug class (and identical fix shape: force a fixed
   * registration order rather than leaving it to chance) already found and
   * fixed once before, for a different pair of subsystems; see
   * queue_mutex_registry's own doc comment in cthreadcomm.c for that full
   * account. Calling all three of these unconditionally, every time this
   * function runs, is harmless: each is itself guarded by its own
   * call_once, so a caller that already forced one (or more) of them via its
   * own, unrelated use of event_loop/ctpool/clog simply finds it already
   * registered and returns immediately. */
  _cthreadcomm_ensure_atfork_registered_before_caller();
  _ctpool_ensure_atfork_registered_before_caller();
  _clog_ensure_atfork_registered_before_caller();
  at_fork(_chttpsvr_atfork_prepare, _chttpsvr_atfork_release,
          _chttpsvr_atfork_child_release);
#endif
}

/* A chttpsvr handle's lifecycle is two independent dimensions, not one
 * linear state machine: a start/stop dimension (was three separate bools,
 * `started`/`starting`/`stop_in_progress`) and a one-shot quiesce latch
 * (was `teardown_started`/`teardown_done`) whose CLAIM can be set
 * instantly, independent of the start/stop dimension, with the real work
 * deferred behind a separate mechanism (`pending_resolve_count`). Two
 * enums, one per dimension, preserve today's independent-claim semantics
 * exactly while still delivering `-Wswitch` (part of this project's
 * standing `-Wall`, combined with `-Werror`) coverage for each: a future
 * addition to either enum that some call site fails to handle fails the
 * build, rather than silently compiling. Every enumerator below is given
 * an explicit value, matching common.h's own `ccol_retval_t` rule, for the
 * identical reason: never rely on declaration-order auto-increment. */
typedef enum {
  CHTTPSVR_LC_IDLE = 0,     /* not listening; chttpsvr_start() may proceed */
  CHTTPSVR_LC_STARTING = 1, /* chttpsvr_start()'s own real work in flight */
  CHTTPSVR_LC_RUNNING = 2,  /* listening, serving */
  CHTTPSVR_LC_STOPPING = 3, /* _chttpsvr_stop_internal()'s real work in
                             * flight, on its way back to IDLE */
} chttpsvr_lifecycle_t;

typedef enum {
  CHTTPSVR_QS_NOT_QUIESCED = 0, /* _quiesce_server_once() may claim + run */
  CHTTPSVR_QS_QUIESCING = 1,    /* its real teardown work is in flight */
  CHTTPSVR_QS_QUIESCED = 2,     /* teardown work finished (an idempotent
                                 * latch); reset to NOT_QUIESCED only by a
                                 * later, legitimate chttpsvr_start() */
} chttpsvr_quiesce_state_t;

/** Main server struct. */
struct chttpserver {
  chttpsvr_router **routers; /* [0] = root, [1..n] = sub-routers */
  size_t router_count;
  size_t router_cap;
  /* Running max of every non-root router's own prefix_seg_count ever
   * registered on this server (routers are only ever added, never removed,
   * so a running max is always correct without ever needing to be
   * recomputed); guarded by routes_lock, exactly like router_count/
   * router_cap/routers itself, updated (if higher) alongside
   * raw->routers[raw->router_count++] = r in chttpsvr_subrouter, under the
   * same write-lock critical section. Lets _find_route build ONE shared,
   * memoizing decode cache for the request path's own leading segments
   * (mirroring _seg_cache_t's existing per-router route-matching cache) and
   * reuse it across every non-root router's own _prefix_matches call,
   * instead of each router independently re-percent-decoding the same
   * request-path segments from scratch; see _find_route's own comment at its
   * call site. Bounding the cache's own split by this operator-controlled
   * value (not the request path's own, entirely client-controlled, actual
   * segment count) mirrors max_route_seg_count's own identical reasoning. */
  int max_prefix_seg_count;
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
  /* Pinned only by _engine_force_stop_quiesce_all, for the duration of one
   * bare struct chttpserver* it reads directly out of servers_bundler.
   * servers[] (see that function's own doc comment for why the pointer
   * needs protecting at all). Deliberately a SEPARATE counter from
   * pending_resolve_count above, not a second use of it: _quiesce_server_
   * once's own winner path waits for pending_resolve_count to reach 0
   * before doing any of its real work, so a caller that held a pin on
   * pending_resolve_count across its own call to _quiesce_server_once
   * would be waiting on a pin only it could release, a genuine self-
   * deadlock (found, reproduced, and fixed during this field's own
   * introduction). This counter is never waited on by _quiesce_server_once
   * itself, only by __chttpsvr_destroy (right before it frees raw), so no
   * such cycle exists here. Broadcasts resolve_cv, the same condvar
   * pending_resolve_count's own drain already uses, since both are
   * "something __chttpsvr_destroy may be waiting on before it's safe to
   * free raw" conditions and either one changing warrants re-checking the
   * other. */
  _Atomic size_t servers_bundler_pins;
  /* Pinned by _listener_on_readable for the entire duration of one dispatch
   * (its own accept4() loop, including pausing the listener registration in
   * response to a persistent resource-exhaustion condition; see
   * _listener_pause_for_resource_pressure), released right before that
   * dispatch returns. Exists because _listener_on_readable receives
   * srv as a bare void* arg via event_loop_add, entirely outside the
   * ordinary chttpsvr-handle resolve/pin mechanism every other entry point
   * into this server goes through, so nothing protected srv against a
   * concurrent chttpsvr_destroy() freeing it while this dispatch was still
   * using it. event_loop_remove()'s own documented "callers may free
   * whatever the registration's own arg points to immediately after this
   * call returns" promise does NOT cover a callback already in progress at
   * the moment of the call (see cthreadcomm.c's _event_loop_run_callback,
   * whose own comment explains event_loop_remove() never takes the
   * registration's dispatch_lock and therefore cannot wait for one); this
   * was a real, if previously narrow (a small handful of instructions,
   * matching the class of window this codebase already accepts elsewhere,
   * e.g. max_body_size's own field comment above), use-after-free window
   * that this exact backoff sleep widened into a reliably, valgrind-
   * reproducible one the moment a single dispatch could legitimately run
   * for tens of milliseconds instead of a handful of instructions.
   *
   * Deliberately a SEPARATE counter from pending_resolve_count, not a
   * second use of it, for the identical self-deadlock reason servers_
   * bundler_pins above is its own counter: chttpsvr_stop() resolves and
   * pins srv via pending_resolve_count for its own entire call, including
   * its call into _chttpsvr_stop_internal; a wait on pending_resolve_count
   * anywhere reachable from that call would wait on a pin only that same
   * call could release. Unlike pending_resolve_count/servers_bundler_pins,
   * though, THIS counter is safe to wait on from within _chttpsvr_stop_
   * internal/_quiesce_server_once/chttpsvr_stop() itself, and does so:
   * this pin is held only by the reactor thread running _listener_on_
   * readable, never by whatever thread calls chttpsvr_stop()/_quiesce_
   * server_once/__chttpsvr_destroy, so there is no scenario where the
   * waiting thread is also the one that would need to run for this counter
   * to ever reach 0; no self-deadlock is possible waiting on it here, the
   * way there would be for the other two. _chttpsvr_stop_internal waits for
   * it immediately after its own event_loop_remove() call for the listener,
   * strictly before closing the listener fd (see that function's own doc
   * comment for the real fd-reuse race this closes): once event_loop_
   * remove() has run, no NEW dispatch can ever be triggered again, so this
   * can only be waiting on a dispatch already live at that exact moment,
   * never a fresh one, and there is at most one at a time (event_loop's own
   * per-registration dispatch_lock already guarantees a registration's
   * callback is never invoked concurrently with itself). __chttpsvr_destroy
   * also checks it, alongside servers_bundler_pins, right before it frees
   * raw's contents, provably redundant by that point (see that wait's own
   * comment), kept as a cheap belt-and-suspenders check. Broadcasts
   * resolve_cv, the same condvar servers_bundler_pins/pending_resolve_count
   * already use, for the identical reason. */
  _Atomic size_t listener_dispatch_pins;
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
  event_reg listen_reg;
  /* CHTTPSVR_LC_IDLE -> CHTTPSVR_LC_STARTING -> CHTTPSVR_LC_RUNNING ->
   * CHTTPSVR_LC_STOPPING -> CHTTPSVR_LC_IDLE, set and read under raw->mutex
   * throughout. Checked, at the top of chttpsvr_start()'s own retry loop,
   * by an exhaustive switch (no `default:` label) precisely so `-Wswitch`
   * (combined with this project's standing `-Werror`) forces every future
   * addition to chttpsvr_lifecycle_t to be handled at every decision point
   * that reads it, rather than silently compiling with the new value
   * falling through to whatever the nearest case happens to do.
   *
   * CHTTPSVR_LC_STARTING covers the entire duration of an in-progress
   * chttpsvr_start() call on this handle, from the moment it passes the
   * IDLE/self-call checks until the instant it returns (success or
   * failure); checked alongside CHTTPSVR_LC_RUNNING under the same
   * raw->mutex critical section a second, concurrent chttpsvr_start() call
   * on the same handle would otherwise race past. Without this, two
   * threads calling chttpsvr_start() on the same IDLE handle at the same
   * time both observe IDLE and proceed in parallel through pool creation
   * and the unsynchronized raw->tls_ctx read/release/replace sequence
   * below: one thread's worker/reject pool pair (and its already-spawned
   * OS threads) is silently dropped in favor of the other's, and, more
   * seriously, one thread can ctls_ctx_release() a TLS context the other
   * thread has already published to raw->tls_ctx and that an
   * already-accepted connection may be actively handshaking against via
   * ctls_conn_create_server, a genuine use-after-free, not merely a leak.
   * Reset to CHTTPSVR_LC_IDLE under raw->mutex at every failure return
   * point in chttpsvr_start(), or advanced to CHTTPSVR_LC_RUNNING on
   * success.
   *
   * CHTTPSVR_LC_STOPPING covers the entire duration of an in-progress
   * _chttpsvr_stop_internal() call's REAL teardown work on this handle
   * (from the moment it confirms the server was actually RUNNING, under
   * the same critical section that leaves CHTTPSVR_LC_RUNNING, until its
   * own event_loop_remove()/close()/unlink() sequence has fully
   * completed); checked by chttpsvr_start()'s own retry loop.
   *
   * CHTTPSVR_LC_STOPPING exists because chttpsvr_stop() (unlike
   * chttpsvr_destroy()/chttpsvr_engine_stop(), both of which go through
   * _quiesce_server_once and its own quiesce_state interlock below) leaves
   * CHTTPSVR_LC_RUNNING BEFORE its blocking event_loop_remove() call for
   * the OLD listener registration returns, not after. Without this state,
   * a chttpsvr_start() call racing a concurrent chttpsvr_stop() call on
   * the same handle from a different thread could observe lifecycle ==
   * IDLE and quiesce_state == NOT_QUIESCED (chttpsvr_stop() never touches
   * quiesce_state) at once, and proceed straight into
   * _make_listen_socket()/bind() for a NEW listener on the same host:port
   * while the OLD listener's own fd was still open (event_loop_remove()/
   * close() not yet run on the stopping thread); a real, if narrow and
   * gracefully-failing (spurious ccol_unexpected_failure from a
   * concurrent EADDRINUSE), race with no protection at all before this
   * state was added, unlike the extensively guarded
   * chttpsvr_engine_stop()-vs-chttpsvr_start() equivalent.
   *
   * Reset unconditionally to CHTTPSVR_LC_IDLE by
   * _chttpsvr_atfork_release_impl in a freshly forked child, for every
   * live server whose lifecycle was CHTTPSVR_LC_STARTING or
   * CHTTPSVR_LC_STOPPING at the instant of fork(); see that function's own
   * doc comment for why this reset is safe here specifically (unlike
   * quiesce_state just below, whose own reset needs to distinguish which
   * state a caught-mid-fork server was actually in). A server already
   * CHTTPSVR_LC_RUNNING (or already CHTTPSVR_LC_IDLE) at the instant of
   * fork() is untouched: nothing was interrupted mid-transition for it. */
  chttpsvr_lifecycle_t lifecycle;
  bool contributed_to_engine;
  /* CHTTPSVR_QS_NOT_QUIESCED -> CHTTPSVR_QS_QUIESCING -> CHTTPSVR_QS_
   * QUIESCED, the one-shot latch _quiesce_server_once()'s winner/loser
   * claim (see that function's own doc comment) is built on; reset back to
   * CHTTPSVR_QS_NOT_QUIESCED only by a later, legitimate chttpsvr_start()
   * restarting this same handle. Checked by an exhaustive switch (no
   * `default:` label), for the identical `-Wswitch` reason lifecycle's own
   * comment above explains, everywhere this state is consulted:
   * chttpsvr_start()'s own retry loop, _quiesce_server_once()'s own
   * claim/loser-wait, and _chttpsvr_atfork_release_impl's own child-side
   * fixup.
   *
   * CHTTPSVR_QS_QUIESCING is set the instant _quiesce_server_once() claims
   * this server's teardown (the winner of a __chttpsvr_destroy()-vs-
   * chttpsvr_engine_stop() race; see that function's own doc comment for
   * why both can legitimately call _quiesce_server_once() for the same
   * srv concurrently), before any of its real teardown work (stop
   * listening, drain in-flight requests, close idle connections, release
   * the engine reference, destroy the worker pools) has actually run.
   * CHTTPSVR_QS_QUIESCED is set only once that real work has genuinely
   * finished. The loser of the race must BLOCK, under quiesce_done_cv
   * below, until quiesce_state reaches CHTTPSVR_QS_QUIESCED (not merely
   * CHTTPSVR_QS_QUIESCING) rather than returning immediately as a bare
   * no-op, since __chttpsvr_destroy proceeds straight from
   * _quiesce_server_once's return into mutex_destroy(raw->mutex)/freeing
   * raw itself; doing that while the winning call is still using
   * raw->mutex/raw->idle_mutex/raw->requests_done_cv inside
   * _drain_and_close_all_connections would destroy a mutex still in use
   * and free srv out from under the still-running teardown, a genuine
   * use-after-free. Reset to CHTTPSVR_QS_NOT_QUIESCED at the top of
   * chttpsvr_start().
   *
   * Also force-advanced straight to CHTTPSVR_QS_QUIESCED (alongside
   * quiesce_waiters below, reset to 0) by _chttpsvr_atfork_release_impl in
   * a freshly forked child, for exactly the servers it finds caught
   * mid-teardown (quiesce_state == CHTTPSVR_QS_QUIESCING) at the instant
   * of fork(); see that function's own doc comment for why this, rather
   * than resetting quiesce_state back to CHTTPSVR_QS_NOT_QUIESCED, is the
   * safe way to unstick both _quiesce_server_once's own loser wait and
   * chttpsvr_start()'s own retry loop for such a server in the child. */
  chttpsvr_quiesce_state_t quiesce_state;
  /* Broadcast once quiesce_state reaches CHTTPSVR_QS_QUIESCED; see that
   * field's own comment. */
  cond_var_t quiesce_done_cv;
  /* Number of threads (realistically 0, 1, or a small handful) currently
   * blocked inside a `while (quiesce_state != CHTTPSVR_QS_QUIESCED)
   * cond_var_wait(...)` loop, guarded by the same raw->mutex/
   * quiesce_done_cv pair: either the losing side of a __chttpsvr_destroy()-
   * vs-chttpsvr_engine_stop() race inside _quiesce_server_once itself, or
   * a concurrent chttpsvr_start() call whose own retry loop backed off
   * after observing quiesce_state == CHTTPSVR_QS_QUIESCING (see that
   * function's own CHTTPSVR_QS_QUIESCING branch).
   * Incremented right before a loser enters that wait, decremented (and
   * quiesce_done_cv broadcast again) right after it exits, both while
   * still holding raw->mutex, so the decrement-then-broadcast is itself
   * the loser's very last touch of raw->mutex/quiesce_done_cv before it
   * unlocks and returns.
   *
   * The winner of _quiesce_server_once waits for this to reach 0 (see its
   * own tail, right after setting CHTTPSVR_QS_QUIESCED and broadcasting)
   * before ever returning to ITS OWN caller. This matters because
   * __chttpsvr_destroy proceeds straight from _quiesce_server_once's
   * return into mutex_destroy(raw->mutex)/cond_var_destroy(raw->
   * quiesce_done_cv)/freeing raw itself, with no further synchronization
   * of its own: quiesce_state's own doc comment already establishes that
   * a broadcast alone is what wakes a loser, but a broadcast does not
   * mean a loser has actually finished reacquiring the mutex inside its
   * own cond_var_wait call by the time the winner's caller proceeds
   * (pthread_cond_broadcast only marks waiters runnable, with no
   * guarantee about when the OS actually schedules them relative to the
   * broadcasting thread's own subsequent progress). If __chttpsvr_destroy
   * happens to be the one that WINS this race (the direction the
   * pre-existing concurrent_destroy_and_engine_stop_is_safe test never
   * actually exercises: that test deliberately engineers the reverse,
   * safer ordering, per its own comment), the engine reaper thread could
   * still be mid-reacquire inside its own losing cond_var_wait call on
   * raw->quiesce_done_cv/raw->mutex at the exact moment __chttpsvr_destroy
   * destroys both, genuine undefined behavior per POSIX (a mutex/condvar
   * must not be destroyed while another thread may still be referencing
   * it via a pthread_cond_wait call in progress), not merely a
   * theoretical concern: a mutex's own mutual-exclusion guarantee is
   * exactly what makes this counter-based wait correct, since the
   * winner's own reacquire-and-see-zero can only happen strictly after
   * the loser's decrement-and-unlock, the same happens-before
   * relationship pending_resolve_count already relies on elsewhere in
   * this file.
   *
   * Reset to 0 by _chttpsvr_atfork_release_impl alongside quiesce_state,
   * in a freshly forked child, for a server caught mid-teardown at the
   * instant of fork(); see quiesce_state's own field comment and that
   * function's own doc comment. */
  size_t quiesce_waiters;

  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned max_response_write_duration_ms;
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

  /* Set by _listener_on_readable_impl right before it pauses this server's
   * listen_reg in response to an accept4() resource-exhaustion errno
   * (EMFILE/ENFILE/ENOBUFS/ENOMEM) or a post-accept allocation failure
   * (_conn_create/ctls_conn_create_server), and consumed (atomically read
   * and cleared together, via atomic_exchange) by the idle-timeout sweep
   * thread's own _listener_resume_if_resource_pressure_cleared, exactly
   * mirroring max_connections/current_connections's own pause/resume split
   * above for a structurally identical reason: retrying accept4() again
   * immediately, inline, right after backing off is what used to leave this
   * one dispatch (and, for a sustained condition, every subsequent
   * dispatch, since level-triggered epoll keeps re-reporting the same
   * non-empty backlog) blocking the sole shared reactor thread for as long
   * as the underlying resource pressure persisted, starving every other
   * connection on every server sharing that one reactor thread in the
   * meantime. Pausing instead, and letting the sweep retry once per tick,
   * removes that blocking entirely at the cost of the same, already-
   * accepted-elsewhere up to _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS recovery
   * latency max_connections's own resume already carries. _Atomic for the
   * same cross-thread-read reason as current_connections just above (the
   * writer is always _listener_on_readable_impl, never itself concurrent
   * with its own re-entry per this struct's own dispatch_lock-serialization
   * note a few fields down; the reader is a different thread entirely). */
  _Atomic bool listener_paused_for_resource_pressure;

  /* Rate-limits the diagnostic log _listener_on_readable emits for an
   * unexpected accept4() failure (anything beyond the routine EWOULDBLOCK/
   * EAGAIN/EINTR this loop already handles silently): without this, a
   * sustained resource-exhaustion condition (EMFILE/ENFILE, the process or
   * system genuinely out of file descriptors) would otherwise re-trigger
   * this same log line on every single reactor dispatch for as long as the
   * condition persists; itself a form of log-flooding self-inflicted by
   * the very diagnostic meant to help. Plain (non-atomic) fields, not
   * _Atomic: event_loop's own per-registration dispatch_lock already
   * guarantees this listener's own registration is never dispatched
   * concurrently with itself (see cthreadcomm.h), so _listener_on_readable
   * for one server is never running on two threads at once regardless of
   * how many reactor threads are configured; both fields are read and
   * written exclusively from within that one, temporally-serialized
   * function. */
  struct timespec last_accept_err_log;
  bool last_accept_err_log_set;

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
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* Process-wide thread-local key recording which struct chttpserver* (if
 * any) the calling thread is currently running a worker_pool/reject_pool
 * task for; set once, at the top of _task_worker/_reject_task (the only two
 * functions ever submitted to a server's own worker_pool/reject_pool), and
 * never reassigned or cleared for the rest of that thread's life. Mirrors
 * cthreadpool.c's own ctpool_worker_key_bundle/_ctpool_is_self_call exactly,
 * for the identical reason: a worker thread of srv->worker_pool/reject_pool
 * is always privately, permanently owned by that one server's own ctpool
 * instance for its entire life (ctpool always spawns its own dedicated
 * threads, never shared across pools, and a server's pools are destroyed and
 * rebuilt from scratch (brand new OS threads) on every restart), so
 * there is no per-task set/clear lifecycle to manage the way event_loop's
 * own dispatch-pool-worker key needs in cthreadcomm.c.
 *
 * Exists to detect a request handler (or middleware, or that handler's own
 * chttpsvr_next_fn continuation) calling chttpsvr_destroy() (or
 * chttpsvr_stop() immediately followed by chttpsvr_start()) on the very
 * server whose worker pool is currently running it. Without this, that call
 * proceeds into _wait_in_flight_bounded's own bounded wait (roughly a
 * minute once the current_connections poll in _drain_and_close_all_
 * connections is included) for in_flight_requests to reach 0; that can
 * never happen, since this exact call stack is the one that would
 * eventually decrement it; and then into ctpool_destroy() on the very
 * pool this thread is a worker of, which is cthreadpool.c's own, unrelated
 * fatal_err()/abort() (see ctpool_worker_key_bundle's own comment in
 * cthreadpool.c). Confirmed by direct reproduction before this guard
 * existed: a handler calling chttpsvr_destroy() on its own server hung the
 * whole process for roughly a minute and then aborted it, with a crash
 * signature pointing into cthreadpool.c rather than anywhere in this file;
 * racing a concurrent chttpsvr_engine_stop() for the same server produced a
 * genuine permanent deadlock instead (no abort at all, since the reaper
 * thread's own, non-self-call ctpool_shutdown_drain has no timeout of its
 * own and would then wait forever for the self-deadlocked handler to
 * return). Detected here, immediately and cheaply (a single thread-local
 * read, no lock), before any of that can unfold. */
static struct {
  thread_ls_key_t key;
  once_flag_t once;
} chttpsvr_worker_key_bundle = {0};

static void _chttpsvr_init_worker_key(void) {
  thread_ls_key_create(chttpsvr_worker_key_bundle.key, NULL);
}

/* Called once, at the top of _task_worker/_reject_task, to mark the calling
 * thread as a permanent worker of srv's own pools; see
 * chttpsvr_worker_key_bundle's own comment above. */
static void _chttpsvr_mark_worker_thread(struct chttpserver *srv) {
  call_once(chttpsvr_worker_key_bundle.once, _chttpsvr_init_worker_key);
  thread_ls_set(chttpsvr_worker_key_bundle.key, (void *)srv);
}

/* True iff the calling thread is a worker thread of srv's own worker_pool or
 * reject_pool, i.e. this call was made (directly or transitively) from
 * within a request handler, middleware, or reject_pool's own courtesy-close
 * task, currently executing for srv. */
static bool _chttpsvr_is_self_call(struct chttpserver *srv) {
  call_once(chttpsvr_worker_key_bundle.once, _chttpsvr_init_worker_key);
  return thread_ls_get(chttpsvr_worker_key_bundle.key) == (void *)srv;
}

/* True iff the calling thread is a worker thread of ANY chttpsvr's
 * worker_pool or reject_pool, i.e. this call was made (directly or
 * transitively) from within some server's request handler, middleware, or
 * reject_pool's own courtesy-close task, regardless of which server it
 * belongs to. Unlike _chttpsvr_is_self_call, which is scoped to one
 * specific server, this exists for chttpsvr_engine_wait(), which is
 * engine-wide rather than server-specific: _engine_force_stop_quiesce_all
 * drains every currently-registered server's own worker pool in turn (via
 * a plain, timeout-less ctpool_shutdown_drain inside _quiesce_server_once),
 * so a handler on ANY server that blocks in chttpsvr_engine_wait() risks
 * the identical self-deadlock class _chttpsvr_is_self_call already guards
 * chttpsvr_destroy()/chttpsvr_start() against: the reaper thread can never
 * finish draining that handler's own server's pool (this exact call stack
 * is what would eventually let that task return), so the reactor can never
 * be torn down and marked stopped, so this wait can never wake (a
 * permanent, engine-wide deadlock, not merely a single stuck server). */
static bool _chttpsvr_is_any_worker_call(void) {
  call_once(chttpsvr_worker_key_bundle.once, _chttpsvr_init_worker_key);
  return thread_ls_get(chttpsvr_worker_key_bundle.key) != NULL;
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
  /* Written only under srv_engine_bundler.mutex (created in _engine_acquire,
   * including its own OOM-rollback branch; nulled to EVENT_LOOP_INVALID in
   * _engine_reaper_fn), but read unlocked at dozens of call sites throughout
   * this file (every connection-handling/reactor-callback/listener site).
   * This is safe by construction, not merely by accident of ISA-level
   * atomicity of a uint64_t load/store: every unlocked reader can only be
   * running while at least one chttpsvr instance is actively started and
   * holding a live engine reference, which is exactly the invariant that
   * keeps this field's value constant between the create-write and the
   * eventual reap-write; and both of those two writes are themselves
   * ordered relative to every reader via genuine synchronization
   * (thread_create/thread_join for the reactor threads themselves, or the
   * pin/resolve mechanisms guarding every chttpsvr handle), not merely
   * "probably fine." A future call path that could read this field OUTSIDE
   * that invariant (i.e. without first holding a live engine reference of
   * its own) would need this field made _Atomic instead. */
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
   * _idle_sweep_stop_if_running's mutex-protected write, and (unlike a
   * torn read, which this specific field's single-byte size makes unlikely
   * in practice) is undefined behavior that gives the compiler license
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
static void _engine_globals_init(void);
#ifdef RUNNING_UNIT_TESTS
static void _reaper_race_hook_wait_if_armed(void);
static void _listener_dispatch_race_hook_wait_if_armed(void);
static void _quiesce_teardown_race_hook_wait_if_armed(void);
#endif /* RUNNING_UNIT_TESTS */

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
 * in-progress teardown completes.
 *
 * srv is pinned (see the atomic_fetch_add below) for the entire remainder of
 * each loop iteration, including the _quiesce_server_once call itself: a
 * bare struct chttpserver* read out of servers_bundler.servers[] has nothing
 * else protecting its lifetime the instant servers_bundler.mutex is
 * released, and _quiesce_server_once's own real work (draining in-flight
 * requests, closing connections) can take a genuinely meaningful amount of
 * wall-clock time, not merely a handful of instructions; a real,
 * reproducible window (not merely theoretical) for an entirely independent,
 * concurrent chttpsvr_destroy(h) call on this exact server to run to
 * completion and free it out from under this loop, since __chttpsvr_destroy
 * has no way to know this thread is about to use srv at all (it never goes
 * through servers_bundler, only through the separate chttpsvr_slot_table).
 *
 * The pin used here is srv->servers_bundler_pins, NOT srv->pending_resolve_
 * count: an earlier version of this function pinned pending_resolve_count
 * instead (the same mechanism _chttpsvr_resolve uses for every other caller
 * of a chttpsvr handle), reasoning that it already had a matching wait in
 * __chttpsvr_destroy for free. That reasoning missed that _quiesce_server_
 * once's own winner path ALSO waits for pending_resolve_count to reach 0,
 * as its very first step, before doing any of its real work, so a caller
 * that holds its own pin on pending_resolve_count across its own call to
 * _quiesce_server_once waits forever on a pin only it could release, a
 * genuine self-deadlock (reproduced reliably: every tests_engine_stop run
 * hung in this exact wait). servers_bundler_pins is a separate counter
 * _quiesce_server_once never touches, so no such cycle exists; see that
 * field's own comment. */
static void _engine_force_stop_quiesce_all(void) {
  /* Every real caller today only ever runs after _engine_acquire's own
   * call_once(srv_engine_bundler.once, ...) has already fired, but per this
   * project's own "insert in literally every function that directly
   * touches the primitive, never rely on call-graph reasoning" rule (see
   * _servers_register's own identical guard, added after that exact
   * reasoning was found to be wrong once already), this function carries
   * its own guard rather than assuming that ordering. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
  struct chttpserver *prev_unremoved = NULL;
  for (;;) {
    mutex_lock(servers_bundler.mutex);
    if (servers_bundler.count == 0) {
      mutex_unlock(servers_bundler.mutex);
      return;
    }
    struct chttpserver *srv = servers_bundler.servers[0];
    /* See servers_bundler_pins' own field comment for why this is a
     * dedicated counter rather than pending_resolve_count. Incremented
     * while servers_bundler.mutex is still held, i.e. before this
     * thread's own reference to srv could otherwise become unprotected for
     * even one instruction. */
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    mutex_unlock(servers_bundler.mutex);

#ifdef RUNNING_UNIT_TESTS
    _reaper_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

    if (srv == prev_unremoved) {
      /* _quiesce_server_once (below) never returns to any caller, winner or
       * loser, before _servers_unregister(srv) has already run (see that
       * function's own quiesce_state-vs-unregister ordering), so srv
       * reappearing here as servers_bundler.servers[0] is never a sign that
       * the previous iteration's own unregister is still pending. It means a
       * concurrent chttpsvr_start(srv) restart has legitimately re-run
       * _servers_register(raw) (deliberately called before that call's own
       * _engine_acquire, precisely so a concurrent force-stop pass like this
       * one can see it; see _servers_register's call site in chttpsvr_start
       * for the full reasoning) in the narrow window between this loop's
       * previous _quiesce_server_once(srv) return and this iteration's read
       * of servers_bundler.servers[0]. That racing chttpsvr_start() call
       * stays resolve-pinned for its own entire duration, so the
       * _quiesce_server_once call below will correctly block on this exact
       * pin before re-quiescing srv; this brief sleep only avoids
       * busy-spinning against that same, already-in-flight restart while it
       * runs. */
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
    }
    prev_unremoved = srv;
    _quiesce_server_once(srv);
    /* Matches the pin above: srv is guaranteed to remain valid, not-yet-
     * freed memory up to and including the _quiesce_server_once call just
     * above, since __chttpsvr_destroy's own "wait for servers_bundler_pins
     * == 0" check (run right before it frees a single byte of srv) cannot
     * be satisfied while this thread's own pin is still held. Released
     * under srv->mutex, together with the broadcast that wakes such a
     * waiting destroy, mirroring _chttpsvr_resolve_unpin's own reasoning
     * for pending_resolve_count. */
    mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    cond_var_broadcast(srv->resolve_cv);
    mutex_unlock(srv->mutex);
  }
}

static void _engine_globals_init(void) {
  if (mutex_init(srv_engine_bundler.mutex) != 0)
    fatal_err("chttpserver engine: failed to initialize mutex");
  if (cond_var_init(srv_engine_bundler.stopped_cv) != 0)
    fatal_err("chttpserver engine: failed to initialize condition variable");
  if (mutex_init(servers_bundler.mutex) != 0)
    fatal_err("chttpserver engine: failed to initialize servers_bundler mutex");
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
  /* Every real caller today only ever spawns this thread after _engine_
   * acquire's own call_once(srv_engine_bundler.once, ...) has already
   * fired (see _spawn_reaper's own two call sites), but per this project's
   * own "insert in literally every function that directly touches the
   * primitive, never rely on call-graph reasoning" rule, this function
   * carries its own guard rather than assuming that ordering. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
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
   * comment) with no logger ever having been installed. Captured into a
   * local rather than logged through directly here, and the actual
   * log_info call deferred to after mutex_unlock below (see that call's own
   * comment): an operator-installed logger (chttpsvr_set_engine_logger) can
   * be an arbitrarily slow synchronous sink, and this mutex also guards
   * every other chttpsvr_start/_stop/_engine_acquire/_release call in the
   * process, not just this reaper thread. old_logger stays valid to log
   * through and then close after unlocking: srv_engine_bundler.log is
   * already reset to CLOG_INVALID below, under the lock, so no other
   * thread can observe or touch this exact clog handle once unlocked. */
  clog old_logger = srv_engine_bundler.log;
  srv_engine_bundler.log = CLOG_INVALID;
  cond_var_broadcast(srv_engine_bundler.stopped_cv);
  mutex_unlock(srv_engine_bundler.mutex);

  if (old_logger) {
    log_info(old_logger, "The http server reactor engine has been destroyed");
    clog_close(old_logger);
  }
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

/* out_currently_stopping is set unconditionally, as this function's very
 * first action after acquiring srv_engine_bundler.mutex: true only when a
 * reap (forced via chttpsvr_engine_stop(), or graceful via the last other
 * server's own chttpsvr_destroy() dropping reactor_refs to 0) is currently
 * tearing the shared reactor down, in which case this function returns
 * immediately WITHOUT blocking for that reap to finish and without ever
 * incrementing reactor_refs; false on every other path (success or a real
 * failure), so a caller never has to pre-zero it itself.
 *
 * A blocking wait here (`while (stopping) cond_var_wait(...)`) is only
 * correct so long as the calling chttpsvr_start() holds no other resource
 * a concurrent reap could be waiting on; chttpsvr_start() holds exactly
 * such a resource for its entire duration, though: its own resolve pin
 * (pending_resolve_count), which _quiesce_server_once()'s own claim on
 * this exact server blocks on before doing any of its real work. A reap
 * that reaches this server while a chttpsvr_start() call for it is
 * blocked in a wait here would deadlock permanently: the reap can't
 * finish (waiting on the pin only this call could release) and this call
 * can't finish (waiting on `stopping`, cleared only once the reap, the
 * very reap it's stuck behind, finishes). See chttpsvr_start()'s own
 * retry-loop comment, at its "currently stopping" branch, for the
 * caller-side half of the fix this enables: releasing the pin and
 * retrying from scratch instead of ever blocking here.
 *
 * The ccol_retval_t returned alongside *out_currently_stopping == true is
 * never consulted by the caller and carries no meaning of its own; it is
 * always ccol_unexpected_failure purely so the return statement has a
 * well-typed value, not because that value means anything in this case. */
static ccol_retval_t _engine_acquire(bool *out_currently_stopping) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(srv_engine_bundler.mutex);
  /* First thing done under the lock, before anything else in this
   * function: guarantees the signal-safe stop watcher is already live for
   * the remainder of this call and for every later srv_engine_bundler.
   * mutex-locking call in the process, closing the self-deadlock window
   * chttpsvr_engine_stop()'s own doc comment (and g_engine_stop_watcher's)
   * describes. */
  _engine_stop_watcher_ensure_started_locked();
  *out_currently_stopping = false;
  if (srv_engine_bundler.stopping) {
    mutex_unlock(srv_engine_bundler.mutex);
    *out_currently_stopping = true;
    return ccol_unexpected_failure;
  }
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
          clog_open_fd_mp(2, CLOG_FATAL, NULL, srv_engine_bundler.mprocs);
      if (!srv_engine_bundler.log) {
        /* Roll back the reactor already created just above: leaving it
         * running with reactor_refs still 0 would leak it for the process
         * lifetime, since nothing would ever call _engine_release() to
         * reap it (that only happens once a server has successfully
         * contributed a reference). Safe to destroy synchronously, still
         * holding srv_engine_bundler.mutex, unlike chttpclient's own
         * analogous fix for its deadline-sweep thread: another thread CAN
         * be contending for this mutex right now (the async-signal-safe
         * engine-stop watcher, if a signal fires chttpsvr_engine_stop()
         * during this exact window, blocks on it like any other locker),
         * but that is ordinary contention, not a deadlock risk, since
         * nothing that could be waiting on this mutex ever needs THIS
         * thread to make further progress first in order to eventually
         * release it. What matters for the synchronous event_loop_destroy()
         * join below specifically is that nothing could call back INTO this
         * mutex from underneath it: the idle-timeout sweep thread is only
         * ever started by chttpsvr_start(), strictly AFTER this function has
         * already returned success, and event_loop's own reactor threads
         * have no knowledge of srv_engine_bundler at all, so there is
         * nothing that could re-enter _SRV_ENGINE_LOG/chttpsvr_set_
         * engine_logger() (both of which lock this same mutex) from inside
         * the join and deadlock against this thread's own already-held
         * lock. */
        event_loop_destroy(srv_engine_bundler.reactor);
        srv_engine_bundler.reactor = EVENT_LOOP_INVALID;
        mutex_unlock(srv_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(srv_engine_bundler.log, "component", "http-server-engine");
    }

    /* Logged here, still under the lock, deliberately NOT deferred to after
     * mutex_unlock the way _engine_reaper_fn's own "engine destroyed" log
     * is: that deferral is safe there specifically because
     * srv_engine_bundler.log is reset to CLOG_INVALID, under the lock,
     * before ever unlocking, so a concurrent chttpsvr_set_engine_logger()
     * can only ever observe/close the NEW logger it installs, never the
     * reaper's own already-captured handle. Here, srv_engine_bundler.log
     * still holds this exact logger after this block returns (it stays the
     * live engine logger until something replaces it), so deferring this
     * log call would leave a window where a concurrent chttpsvr_set_engine_
     * logger() call (which itself only synchronizes via this same mutex,
     * with no dependency on reactor_refs or any other pin) could swap it
     * out and clog_close() it before this deferred call ever ran - a real
     * use-after-free on the logger, not merely a missed log line. A prior
     * version of this function deferred this call the same way the reaper
     * does and reintroduced exactly that race; reverted back to logging
     * under the lock once that was found. This call is rare (fires only
     * the once-per-full-stop-cycle instant a fresh reactor is created, not
     * on every ordinary chttpsvr_start()), so the contention this mutex's
     * own other callers could suffer from a slow custom logger here is
     * comparatively minor. */
    log_info(srv_engine_bundler.log,
             "New http server reactor engine has been created");
  }
  srv_engine_bundler.reactor_refs++;
  mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

static void _engine_release(void) {
  /* Every real caller today only ever runs on a server that already holds
   * a contributed engine reference, which can only have been taken by this
   * same server's own prior _engine_acquire() call (whose own call_once
   * has therefore already fired), but per this project's own "insert in
   * literally every function that directly touches the primitive, never
   * rely on call-graph reasoning" rule, this function carries its own
   * guard rather than assuming that ordering. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
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
  /* !srv_engine_bundler.stopping guard, mirroring _engine_release's own
   * identical guard and its own doc comment on why it exists: without it,
   * a SECOND chttpsvr_engine_stop() call arriving while a reaper spawned by
   * a FIRST call is still mid-teardown (srv_engine_bundler.stopping already
   * true, srv_engine_bundler.reactor not yet nulled; that only happens
   * near the very end of _engine_reaper_fn, well after
   * _engine_force_stop_quiesce_all/_idle_sweep_stop_if_running/
   * event_loop_destroy have all run, which can take real, non-negligible
   * wall-clock time) would spawn a SECOND reaper capturing the identical,
   * still-live event_loop handle. Both reapers would then independently
   * call event_loop_destroy() on that same handle; per cthreadcomm.h's own
   * documented contract this is unconditionally fatal (fatal_err()/
   * SIGABRT) for both a sequential (one already completed) and a
   * temporally-overlapping double-destroy. Confirmed by direct trace, not
   * assumed: chttpsvr_engine_stop() is documented as the mechanism to wire
   * into a signal handler, and a second SIGTERM/SIGINT (or a defensive
   * double call from application shutdown code) arriving before the first
   * call's own teardown has finished is entirely ordinary, not a rare
   * corner case; this guard is what makes a repeated or overlapping
   * chttpsvr_engine_stop() call the documented safe no-op it is supposed to
   * be, rather than a process abort. */
  if (srv_engine_bundler.reactor && !srv_engine_bundler.stopping) {
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
 * safe to call from a signal handler"; the whole point being that an
 * application can install it (or a thin wrapper around it) as a SIGTERM/
 * SIGINT handler for graceful shutdown. _engine_force_stop_now() above is
 * NOT actually async-signal-safe, though: it calls mutex_lock() (a plain,
 * non-recursive pthread mutex) and, via _spawn_reaper(), thread_create().
 * Neither is on the POSIX list of async-signal-safe functions, and this is
 * not a theoretical concern; both are genuinely reachable self-deadlocks:
 * srv_engine_bundler.mutex is also taken by _engine_acquire() (called
 * synchronously from chttpsvr_start()), _engine_release() (called from
 * chttpsvr_destroy()/_quiesce_server_once), and chttpsvr_set_engine_logger/
 * _set_engine_mem_mgmt_procs/_set_engine_num_reactor_threads. A signal
 * arriving on the very thread that is currently inside any one of those
 * calls (e.g. SIGTERM delivered mid-chttpsvr_start(), a realistic race
 * during a container's shutdown/rolling-restart sequence) would have the
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
 * common.h). The watcher thread blocks in semaphore_wait() (an ordinary,
 * non-signal execution context, so mutex_lock()/thread_create() are both
 * fine there) and, once woken, performs the exact same work
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
   * locked(); itself only ever called from an ordinary thread (ultimately
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
 * ordinary (non-signal) thread; see _engine_acquire, its one call site.
 * Called as the very first thing _engine_acquire() does after taking the
 * lock, before any of that function's own reactor-creation work: this
 * guarantees the watcher is already live for the remainder of that call
 * and every subsequent srv_engine_bundler.mutex-locking call in the
 * process, closing the self-deadlock window described above. The only
 * residual gap is a signal arriving in the handful of instructions before
 * this function's own first call; nothing yet holds srv_engine_bundler.
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
 * does not report it as a "possibly lost" thread stack. A plain,
 * unsynchronized read of `started`/`thread` here (no mutex) is safe
 * regardless of whether any chttpsvr the application created is still live
 * at process exit (unlike _cleanup_chttpsvr_router_shells/_cleanup_
 * chttpsvr_slot_table, which each check per-handle liveness before freeing
 * anything for exactly that reason): both fields are write-once for the
 * life of the process, set together exactly once, the first time any
 * chttpsvr ever acquires the shared engine (see
 * _engine_stop_watcher_ensure_started_locked, this thread's only writer),
 * with no "un-start" operation anywhere; a plain read racing that one write
 * either observes the pre-start (false/unset) or fully-started state, never
 * a torn or reverted one. */
__attribute__((destructor)) static void _cleanup_engine_stop_watcher(void) {
  if (!g_engine_stop_watcher.started) return;
  atomic_store(&g_engine_stop_watcher.exit_requested, true);
  semaphore_post(g_engine_stop_watcher.sem);
  thread_join(g_engine_stop_watcher.thread);
  /* Cleared before the semaphore is actually destroyed, not left true: this
   * function's own async-signal-safe contract means a signal handler can
   * call chttpsvr_engine_stop() (whose entire implementation, _engine_
   * force_stop, is a bare `if (ready) sem_post(sem)`) at literally any
   * point during process exit, including in the narrow window after this
   * destructor has already run but before the process has actually gone
   * away (destructor/atexit ordering across a process's own shared objects
   * is not controlled by this library; see this file's own standing
   * caution about relying on it). Leaving `ready` true here would let that
   * racing sem_post() target a semaphore this same destructor has already
   * destroyed, a real use-after-free-style violation on a primitive, not
   * merely a missed stop request. */
  atomic_store(&g_engine_stop_watcher.ready, false);
  semaphore_destroy(g_engine_stop_watcher.sem);
}

/* The actual public entry point: see g_engine_stop_watcher's own comment
 * above for why this is deliberately reduced to only this one genuinely
 * async-signal-safe operation, with no mutex_lock/thread_create of its
 * own, direct or indirect. `ready == false` (no engine has ever been
 * started in this process, or (for a vanishingly narrow window) one is
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
/* White-box regression test hook only: locks srv_engine_bundler.mutex
 * (exactly the critical section chttpsvr_engine_stop()'s own doc comment
 * says a signal handler must be safe to interrupt) then synchronously
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

/* Computes the doubling-growth target capacity for a plain-realloc'd
 * pointer array: cap*2 (or `initial` the first time cap is 0), or 0 if
 * either the doubling itself or the subsequent *elem_size multiplication
 * the caller is about to perform would overflow size_t. A 0 return must be
 * treated as "cannot grow right now"; every caller in this file already has
 * a graceful, already-tested degradation path for an ordinary
 * realloc-returned-NULL OOM and treats a 0 return from this function
 * identically (see _servers_register/_chttpsvr_router_shell_register's own
 * comments). Shared by every doubling-growth registry in this file that
 * uses plain malloc/realloc rather than this module's own
 * ccol_memmgmt_procs_t-based helpers; mirrors this project's own
 * established SIZE_MAX-relative guard idiom, used identically (just via
 * _mem_realloc instead of plain realloc) by _router_add_route/
 * chttpsvr_subrouter/chttpsvr_resp_set_header/_parse_qparams. Deliberately
 * pure (touches no global state) so it can be exercised directly from a
 * white-box test with a fake cap, without needing to actually grow one of
 * this file's real process-wide registries to a pathological size; see
 * _chttpsvr_doubling_growth_cap_for_tests below. */
static size_t _doubling_growth_cap(size_t cap, size_t elem_size,
                                   size_t initial) {
  if (cap == 0) return initial;
  if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / elem_size) return 0;
  return cap * 2;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _doubling_growth_cap directly. Not part of
 * the public API; gated so this symbol does not leak into a production
 * build of libccollections.so, matching every other white-box helper in
 * this file.
 */
size_t _chttpsvr_doubling_growth_cap_for_tests(size_t cap, size_t elem_size,
                                               size_t initial) {
  return _doubling_growth_cap(cap, elem_size, initial);
}
#endif /* RUNNING_UNIT_TESTS */

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
  /* servers_bundler.mutex is initialized by _engine_globals_init, guarded
   * by srv_engine_bundler.once; chttpsvr_start() deliberately calls this
   * function BEFORE its own _engine_acquire() call (see that call site's
   * own comment), so this can be the very first thing in the whole
   * process to ever touch servers_bundler.mutex, on a plain construct/
   * register_handler/start call sequence that never happens to call
   * chttpsvr_set_engine_logger/_mem_mgmt_procs/_num_reactor_threads,
   * chttpsvr_engine_wait, or chttpsvr_engine_stop first. Must not rely on
   * call-graph reasoning ("some other, already-guarded function always
   * runs first") to skip this guard; see common.h's own mutex_t
   * conventions. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      mutex_unlock(servers_bundler.mutex);
      return;
    }
  }
  if (servers_bundler.count == servers_bundler.capacity) {
    /* Overflow-safe growth, matching every other growable array in this file
     * (_router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_header,
     * _parse_qparams): a plain "capacity * 2" can wrap on a pathological
     * capacity, silently under-allocating new_cap * sizeof(ptr) below. A
     * failed/skipped growth here (like a genuine OOM a few lines down) means
     * srv is simply never added to servers_bundler.servers[] for this
     * session: neither the idle-timeout sweep NOR a later
     * chttpsvr_engine_stop()'s own _engine_force_stop_quiesce_all pass (which
     * only ever quiesces servers it can find in this exact array) will ever
     * discover or act on it, so srv keeps running and serving traffic
     * normally but outside both of those mechanisms' reach for the rest of
     * its life; an explicit chttpsvr_stop()/chttpsvr_destroy() call on srv
     * itself is completely unaffected, since neither goes through this
     * array. Not treated as a hard chttpsvr_start() failure: refusing to
     * start an otherwise perfectly capable server over a transient capacity-
     * array allocation hiccup would trade a real, working server for a
     * degraded-but-still-correct one, and this call already runs before this
     * function's own engine-acquire step specifically to close a different,
     * unrelated race (see this call's own site comment), a delicate ordering
     * this narrow, rare degradation is not worth reworking. */
    size_t new_cap = _doubling_growth_cap(servers_bundler.capacity,
                                          sizeof(struct chttpserver *), 8);
    if (new_cap > 0) {
      struct chttpserver **nn = (struct chttpserver **)realloc(
          servers_bundler.servers, new_cap * sizeof(struct chttpserver *));
      if (nn) {
        servers_bundler.servers = nn;
        servers_bundler.capacity = new_cap;
      }
    }
  }
  if (servers_bundler.count < servers_bundler.capacity)
    servers_bundler.servers[servers_bundler.count++] = srv;
  mutex_unlock(servers_bundler.mutex);
}

/* The actual removal, factored out so _chttpsvr_atfork_release_impl's own
 * child-side fixup (see that function's own doc comment) can perform it
 * without recursively re-locking servers_bundler.mutex, which it already
 * holds for the whole duration of its own per-slot walk (inherited locked
 * from _chttpsvr_atfork_prepare). Caller must already hold servers_bundler.
 * mutex. Idempotent: a no-op if srv is not present. */
static void _servers_unregister_locked(struct chttpserver *srv) {
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      servers_bundler.servers[i] =
          servers_bundler.servers[servers_bundler.count - 1];
      servers_bundler.count--;
      break;
    }
  }
}

static void _servers_unregister(struct chttpserver *srv) {
  /* Every real caller today only ever runs after _servers_register has
   * already fired this same guard for this srv, but per this project's own
   * "insert in literally every function that directly touches the
   * primitive, never rely on call-graph reasoning" rule, this function
   * carries its own guard rather than assuming that ordering. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(servers_bundler.mutex);
  _servers_unregister_locked(srv);
  mutex_unlock(servers_bundler.mutex);
}

/* Undoes _servers_register()/a taken engine reference for a chttpsvr_start()
 * call that registered raw and (possibly) acquired the shared engine, but
 * then failed further down the same call (listen socket setup, or
 * event_loop_add, both failing after that point); so a failed start
 * leaves raw in exactly the state it was in before the call, rather than
 * silently keeping the shared reactor alive (and raw registered with the
 * idle-timeout sweep) until some later, possibly-never-made
 * chttpsvr_destroy() call reclaims it. Without this, an application that
 * checks chttpsvr_start()'s return value and, on failure, calls
 * chttpsvr_engine_wait() before ever calling chttpsvr_destroy() on the
 * failed handle (a reordering this module's own header does not warn
 * against) would block forever, since nothing would otherwise ever drop
 * this server's engine reference.
 *
 * Safe to call while raw is still resolve-pinned by this thread's own
 * chttpsvr_start() call (i.e. before _chttpsvr_resolve_unpin(raw) below):
 * _servers_unregister is idempotent, and a concurrent _quiesce_server_once
 * call for raw (from __chttpsvr_destroy or _engine_force_stop_quiesce_all,
 * either of which could otherwise be racing to do this exact same cleanup)
 * cannot itself proceed past its own pending_resolve_count wait until this
 * thread's pin is released, which always happens strictly after this
 * function returns; so there is no window in which two callers touch
 * raw->contributed_to_engine/servers_bundler.servers for the same raw at
 * the same time.
 *
 * acquired_here must be the exact `need_acquire` value chttpsvr_start()
 * itself computed earlier in the same call (whether this specific call was
 * the one that transitioned raw from not-yet-registered/no-engine-reference
 * to registered/referenced, as opposed to a RESTART of a server that was
 * already registered and already held its engine reference from an earlier,
 * successful chttpsvr_start() call; chttpsvr_stop() never unregisters or
 * releases either of those, precisely so a later restart can skip
 * re-acquiring them). Only that call is entitled to undo them: a failed
 * restart calling this function unconditionally (the original version of
 * this function) unregistered and released a still-legitimately-referenced,
 * still-registered server out from under itself, desynchronizing
 * servers_bundler's bookkeeping and, in the last-referencing-server case,
 * risking the shared reactor being torn down while raw's own still-live
 * idle/keep-alive connections (chttpsvr_stop() only closes the listener,
 * never existing connections) still held registrations into it; exactly
 * the ordering hazard _quiesce_server_once's own doc comment describes and
 * exists to prevent. A failed restart must instead leave raw in precisely
 * the state it was already in before this call began (registered, still
 * referencing the engine, not started), the same state a plain
 * chttpsvr_stop() with no restart attempted at all would leave it in. */
static void _chttpsvr_undo_start_registration(struct chttpserver *raw,
                                              bool acquired_here) {
  if (!acquired_here) return;
  _servers_unregister(raw);
  bool release = false;
  mutex_lock(raw->mutex);
  if (raw->contributed_to_engine) {
    raw->contributed_to_engine = false;
    release = true;
  }
  mutex_unlock(raw->mutex);
  if (release) _engine_release();
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

/* Resumes srv's listener registration if _listener_on_readable had paused it
 * for being at max_connections capacity (see that function's own comment):
 * a no-op unless both a cap is actually configured and current_connections
 * has genuinely dropped back below it since the pause.
 *
 * Called unconditionally (not idle_timeout_ms-gated) from _idle_sweep_fn's
 * own per-server loop below, once per sweep tick, rather than synchronously
 * from _conn_free the moment a connection closes. A first version of this
 * fix called event_loop_resume directly from _conn_free, right after its
 * atomic_fetch_sub of current_connections; immediate, and correct in
 * isolation, but a real, ThreadSanitizer-caught use-after-free once
 * combined with __chttpsvr_destroy's own teardown: _drain_and_close_all_
 * connections (see its own comment) polls current_connections down to zero
 * as its SOLE signal that no connection's teardown still needs srv, then
 * proceeds to free srv; adding a second, later access to srv (the resume
 * call) after the exact atomic operation that signal depends on
 * reintroduced the identical class of race that poll loop's own history
 * already documents having been fixed once before, for a different access.
 * Moving this off of _conn_free's own call stack entirely, onto the
 * idle-timeout sweep thread's already-established, already-TSan-clean
 * pattern for touching a registered server's state (walking servers_
 * bundler.servers under servers_bundler.mutex, safe because a server is
 * only ever removed from that list, under the same lock, before any of its
 * state is freed; see _servers_unregister's own call site in _quiesce_
 * server_once, which runs before _drain_and_close_all_connections even
 * starts, and by which point the listener's own registration has already
 * been fully removed via _chttpsvr_stop_internal, leaving nothing left to
 * resume anyway) closes the race completely, at the cost of resuming a
 * paused listener within at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS (1s) of a
 * slot freeing up rather than near-instantly; an acceptable trade for a
 * documented coarse admission-control knob (chttpsvr_config_t.max_
 * connections's own doc comment never promised sub-second pickup latency),
 * and vastly better than the 100% CPU busy-loop this whole mechanism exists
 * to prevent in the first place. */
static void _listener_resume_if_capacity_freed(struct chttpserver *srv) {
  size_t cap = atomic_load(&srv->max_connections);
  if (!cap || atomic_load(&srv->current_connections) >= cap) return;
  mutex_lock(srv->mutex);
  event_reg lreg = srv->listen_reg;
  mutex_unlock(srv->mutex);
  if (!lreg) return;
  event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* Resumes srv's listener registration if _listener_on_readable_impl had
 * paused it for accept4() resource exhaustion or a post-accept allocation
 * failure (see listener_paused_for_resource_pressure's own field comment);
 * a no-op unless that flag is currently set. Deliberately independent of
 * _listener_resume_if_capacity_freed just above rather than folded into one
 * combined check: the two pause reasons are orthogonal (a listener paused
 * for being at max_connections capacity must stay paused for as long as
 * current_connections says so, regardless of this flag's state, and vice
 * versa), so collapsing them into a single flag/condition would risk one
 * reason's resume incorrectly firing (or being suppressed) based on the
 * other's state; see this file's own "verify each flag's value space is
 * truly mutually exclusive before merging" convention. atomic_exchange
 * both reads and clears the flag in one step: if a fresh pause (a new
 * failure on the very next dispatch) races this resume and lands after the
 * exchange but before event_loop_resume actually runs below, the resume
 * below un-pauses a registration the reactor thread just legitimately
 * re-paused a moment ago; harmless and self-correcting, since that same
 * fresh dispatch already set the flag back to true, so the very next sweep
 * tick (or, more likely, an immediate re-dispatch from the still-non-empty
 * backlog) simply repeats the cycle rather than leaving anything
 * permanently stuck unpaused or unresumed. */
static void _listener_resume_if_resource_pressure_cleared(
    struct chttpserver *srv) {
  if (!atomic_exchange(&srv->listener_paused_for_resource_pressure, false))
    return;
  mutex_lock(srv->mutex);
  event_reg lreg = srv->listen_reg;
  mutex_unlock(srv->mutex);
  if (!lreg) return;
  event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* Returns true iff a connection whose most recent activity was at
 * last_activity should be considered idle-timed-out relative to now, given
 * idle_ms (idle_timeout_ms/read_timeout_ms's own resolved value; the caller
 * already guarantees this is nonzero). Extracted as its own small, pure
 * function (matching this file's own established "pure helper + RUNNING_
 * UNIT_TESTS accessor" convention already used for _doubling_growth_cap and
 * _accept_errno_is_transient/_is_resource_exhaustion) specifically so it
 * can be exercised directly with a synthetic, adversarial (now,
 * last_activity) pair, without needing to actually win a real scheduling
 * race against a live sweep thread from a test.
 *
 * long long, not long, mirroring _shrink_timeout_to_deadline's own
 * identical reasoning a few functions up in this file, for two independent
 * reasons. Overflow: idle_ms is unsigned and now/last_activity can in
 * principle be arbitrarily far apart, so the multiplication below must not
 * overflow a 32-bit long on an ILP32 build this project builds and
 * CI-tests. Sign: now is captured once by the caller (_idle_sweep_fn),
 * before it ever takes idle_mutex or examines any specific connection;
 * last_activity is refreshed by _idle_list_add from a DIFFERENT thread
 * every time a connection lands back in the idle list, including the
 * ordinary case of finishing a keep-alive request concurrently with this
 * exact sweep tick. A connection that gains fresh activity in the
 * (however brief) window between the sweep's own `now` snapshot and the
 * sweep actually reaching that connection under idle_mutex therefore has
 * last_activity > now (an entirely legitimate, expected outcome of this
 * design under ordinary concurrent load, not a clock-skew bug) so
 * elapsed must be computed as a genuinely signed quantity and checked for
 * a negative result BEFORE ever being compared against idle_ms, or such a
 * just-turned-idle connection is spuriously evicted instead of correctly
 * recognized as having zero (or negative) elapsed idle time: comparing a
 * bare `(unsigned long)elapsed_ms >= idle_ms` silently wraps a small
 * negative elapsed_ms to a huge unsigned value, unconditionally `>=
 * idle_ms` for any realistic timeout. Closes the identical defect class
 * _shrink_timeout_to_deadline was already written specifically to avoid. */
static bool _conn_idle_timed_out(struct timespec now,
                                 struct timespec last_activity,
                                 unsigned idle_ms) {
  long long elapsed_ms =
      (long long)(now.tv_sec - last_activity.tv_sec) * 1000LL +
      (long long)(now.tv_nsec - last_activity.tv_nsec) / 1000000LL;
  if (elapsed_ms < 0) return false;
  return (unsigned long long)elapsed_ms >= (unsigned long long)idle_ms;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook exposing the pure idle-timeout decision directly:
 * reproducing the real race (a concurrent _idle_list_add refreshing
 * last_activity between the sweep's own `now` snapshot and the moment it
 * examines this exact connection) deterministically from a test would
 * require actually winning a scheduling race against a live sweep thread;
 * this lets a test construct the adversarial (now, last_activity) pair
 * directly instead. Gated so this symbol does not exist in a production
 * build, matching every other white-box helper in this file. */
bool _chttpsvr_conn_idle_timed_out_for_tests(struct timespec now,
                                             struct timespec last_activity,
                                             unsigned idle_ms) {
  return _conn_idle_timed_out(now, last_activity, idle_ms);
}
#endif /* RUNNING_UNIT_TESTS */

static void *_idle_sweep_fn(void *arg) {
  (void)arg;
  /* Every function that directly touches servers_bundler.mutex/srv_engine_
   * bundler state re-derives this guard itself rather than relying on
   * call-graph reasoning ("this thread only ever starts after
   * _idle_sweep_start_if_needed's own caller has already fired it") - see
   * this project's own standing rule on exactly this point. Harmless
   * no-op here in practice (every real caller today already guarantees
   * this has already run before this thread's very first iteration), but
   * a future call path that starts this thread earlier would otherwise
   * silently reintroduce the same class of bug this rule exists to
   * prevent. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
  while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
    struct timespec ts = {
        .tv_sec = _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS / 1000,
        .tv_nsec = (_CHTTPSVR_IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    if (atomic_load(&idle_sweep_bundler.stop_flag)) break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Snapshot every currently-registered server and pin each one (via
     * servers_bundler_pins, mirroring _engine_force_stop_quiesce_all's own
     * identical pin-then-release pattern; see that field's own comment for
     * why a dedicated counter, not pending_resolve_count, is required here)
     * while still holding servers_bundler.mutex, then release the lock
     * before doing any of the real per-server work below. Per-server work
     * can mean a real close()/TLS teardown for up to _CHTTPSVR_IDLE_CLOSE_
     * BATCH connections, genuinely slow compared to holding a global lock;
     * servers_bundler.mutex is also needed by chttpsvr_start()/_destroy()/
     * chttpsvr_engine_stop() for every OTHER registered server in the
     * process, not just the one(s) this tick happens to be closing
     * connections for, so holding it across that work (as an earlier
     * version of this function did) needlessly serialized those calls
     * behind whatever idle-connection churn this tick happened to find. The
     * snapshot array itself uses the plain default allocator, like
     * servers_bundler.servers' own backing storage, decoupled from any one
     * server's own custom allocator. */
    mutex_lock(servers_bundler.mutex);
    size_t n = servers_bundler.count;
    struct chttpserver **snapshot =
        n > 0 ? (struct chttpserver **)malloc(n * sizeof(struct chttpserver *))
              : NULL;
    if (n > 0 && !snapshot) {
      /* OOM: skip this tick's sweep entirely and retry on the next one,
       * the same self-healing degradation _servers_register's own failed-
       * growth path already relies on. */
      n = 0;
    } else {
      for (size_t i = 0; i < n; i++) {
        snapshot[i] = servers_bundler.servers[i];
        atomic_fetch_add(&snapshot[i]->servers_bundler_pins, 1);
      }
    }
    mutex_unlock(servers_bundler.mutex);

    for (size_t i = 0; i < n; i++) {
      struct chttpserver *srv = snapshot[i];
      /* Unconditional (unlike the idle_timeout_ms-gated work below): a
       * paused listener must be checked for every registered server on
       * every tick regardless of whether idle-timeout enforcement itself is
       * even enabled (it is disabled by default; see CHTTPSVR_CONFIG_
       * DEFAULT), since this is this mechanism's only trigger. */
      _listener_resume_if_capacity_freed(srv);
      _listener_resume_if_resource_pressure_cleared(srv);

      unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
      if (idle_ms) {
        /* Collect timed-out connections under idle_mutex, claiming
         * (removing) each one from the list right here rather than merely
         * copying its pointer out, then close them outside the lock:
         * _conn_close ultimately calls event_loop_remove, which must not
         * run while holding a lock a callback dispatched from that same
         * removal could also need. Claiming at collection time (not just
         * after, in a separately-locked pass) is what stops a connection
         * from being simultaneously "found here" and dispatched to
         * _conn_pump on a reactor thread; see _idle_list_try_claim's own
         * doc comment for the real use-after-free (a freed SSL* read
         * concurrently by ctls_conn_read) this closes. */
        chttpsvr_conn_t *to_close[_CHTTPSVR_IDLE_CLOSE_BATCH];
        size_t to_close_n = 0;
        mutex_lock(srv->idle_mutex);
        chttpsvr_conn_t *c = srv->idle_head;
        while (c && to_close_n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
          chttpsvr_conn_t *next = c->idle_next;
          if (_conn_idle_timed_out(now, c->last_activity, idle_ms)) {
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

      /* Matches the pin above: srv is guaranteed to remain valid, not-yet-
       * freed memory up to and including this point, mirroring
       * _engine_force_stop_quiesce_all's own identical unpin/broadcast. */
      mutex_lock(srv->mutex);
      atomic_fetch_sub(&srv->servers_bundler_pins, 1);
      cond_var_broadcast(srv->resolve_cv);
      mutex_unlock(srv->mutex);
    }
    free(snapshot);
  }
  return NULL;
}

/* Returns true once the shared idle-timeout sweep thread is confirmed
 * running (already running from an earlier call, or just started
 * successfully by this one), false only if thread_create() itself failed on
 * this attempt. Every registered server's own idle_timeout_ms enforcement
 * and max_connections capacity-recovery (see _listener_resume_if_capacity_
 * freed, this sweep thread's own sole caller) depend entirely on this
 * thread actually running, so chttpsvr_start() treats a false return as a
 * real startup failure rather than proceeding silently degraded; see that
 * function's own call site for the full reasoning. */
#ifdef RUNNING_UNIT_TESTS
/* White-box test hook only: real pthread_create() failures (resource
 * exhaustion) are not deterministically reproducible from a test, and the
 * idle sweep thread is a shared, engine-lifetime resource that only ever
 * attempts its own thread_create() call once per "not currently running"
 * window in the first place (see idle_sweep_bundler.running's own gate
 * immediately below), so this simulates the failure directly instead.
 * Gated so this symbol does not exist at all in a production build,
 * matching every other white-box helper in this file. */
static _Atomic bool g_force_idle_sweep_thread_create_fail_for_tests = false;
void _chttpsvr_force_idle_sweep_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_idle_sweep_thread_create_fail_for_tests, force);
}
#endif /* RUNNING_UNIT_TESTS */
static bool _idle_sweep_start_if_needed(void) {
  /* See _idle_sweep_fn's own identical guard/comment: re-derived here too,
   * matching every other function in this file that directly touches
   * servers_bundler.mutex/srv_engine_bundler state, rather than relying on
   * call-graph reasoning about what has already run by the time a caller
   * reaches this function. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(servers_bundler.mutex);
  if (!idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, false);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_force_idle_sweep_thread_create_fail_for_tests)) {
      /* Simulated failure: no real thread was created, so nothing to join. */
    } else
#endif /* RUNNING_UNIT_TESTS */
      if (thread_create(idle_sweep_bundler.thread, _idle_sweep_fn, NULL) == 0)
        idle_sweep_bundler.running = true;
  }
  bool ok = idle_sweep_bundler.running;
  mutex_unlock(servers_bundler.mutex);
  return ok;
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
  /* See _idle_sweep_fn's own identical guard/comment. */
  call_once(srv_engine_bundler.once, _engine_globals_init);
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
 * pipelined one on the same connection) is handed to a worker thread; and
 * from _conn_dispatch_reject, since a courtesy rejection response's own
 * write (via reject_pool or its synchronous fallback) is exactly the same
 * kind of blocking I/O on a connection this registry exists to protect
 * against; see that function's own comment for the real hang this closes. */
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

/* One raw ('/'-delimited) segment of a request sub_path, together with its
 * lazily-computed, memoized percent-decoded value. A router with N
 * registered routes tries every one of them against the identical sub_path;
 * a given raw segment's decoded value (or its decode failure; percent-
 * decoding depends only on the segment's own bytes, never on which route is
 * being tested against it) is therefore the same answer no matter which
 * candidate route asks for it. Decoding each position at most once per
 * router per request (via this cache), rather than once per candidate route
 * that happens to touch it, turns what used to be an O(routes * segment
 * length) cost into O(cache->count segments), each decoded once. */
typedef struct {
  const char *raw; /* pointer into the sub_path this cache was built from;
                    * not itself NUL-terminated at raw_len */
  size_t raw_len;
  bool attempted; /* true once a decode has been tried for this position */
  bool ok;        /* meaningful only once attempted; true iff decoded/
                   * decoded_len below are valid */
  char *decoded;  /* owned, NUL-terminated */
  size_t decoded_len;
} _seg_cache_entry_t;

typedef struct {
  _seg_cache_entry_t *entries; /* heap array, one per raw segment found, up
                                * to (and including) the cap this cache was
                                * built with */
  size_t count;                /* number of entries actually populated; see
                                * _seg_cache_build's own doc comment for why this is always
                                * an unambiguous stand-in for sub_path's true total segment
                                * count, for any route whose own seg_count is strictly less
                                * than the cap this cache was built with */
  ccol_memmgmt_procs_t *mp;
} _seg_cache_t;

/* Splits sub_path into its raw ('/'-delimited) segments (recording each
 * one's (pointer, length) within sub_path itself, with no decoding yet)
 * stopping once `cap` segments have been found even if sub_path itself has
 * more. Mirrors the exact leading-slash-stripping and zero-length-segment-
 * preserving splitting a route's own segment-by-segment walk has always
 * performed (a zero-length raw segment, from a request path with
 * consecutive slashes, is recorded as such rather than skipped, since it
 * can never satisfy any route either).
 *
 * `cap` MUST be strictly greater than every route in the calling router's
 * own seg_count (see the caller, which passes router->max_route_seg_count +
 * 1): this is what makes `cache->count == route->seg_count` an unambiguous
 * "sub_path has exactly this many segments, no more and no fewer" test for
 * every such route, without this cache ever needing to know sub_path's true
 * total segment count when that total exceeds what any registered route
 * could possibly need. Reasoning: if cache->count < cap, the split loop
 * below only stopped because sub_path genuinely ran out of content, so
 * cache->count is sub_path's real total; if cache->count == cap, sub_path
 * has AT LEAST cap segments, i.e. strictly more than any route's own
 * seg_count (all of which are < cap by construction), so cache->count can
 * never spuriously equal a real route's seg_count in that case either.
 * Bounding by `cap` (a value fixed at route-registration time, controlled
 * by the server operator) rather than sub_path's own actual segment count
 * (controlled entirely by the client sending the request) is what keeps
 * this cache's own memory footprint independent of how many "/" characters
 * a request path happens to contain. Returns 0 on success (cache->count may
 * legitimately be 0, e.g. for sub_path == "/"), or -1 on OOM. */
static int _seg_cache_build(const char *sub_path, size_t cap,
                            _seg_cache_t *cache, ccol_memmgmt_procs_t *mp) {
  cache->entries = NULL;
  cache->count = 0;
  cache->mp = mp;
  if (cap == 0) return 0;

  const char *p = sub_path;
  if (*p == '/') p++;
  if (*p == '\0') return 0;

  /* Deliberately NOT gated on `*s` in the loop condition (only on `count <
   * cap`): a trailing '/' (e.g. sub_path == "/items/99/") means one more,
   * empty, final segment past the last real one, exactly as a route's own
   * former segment-by-segment walk already treated it (its own last-segment
   * check, "if (next_sep != NULL) goto no_match", rejects a route whose
   * last segment is followed by any further separator at all, including a
   * bare trailing one with nothing after it). A `*s`-gated loop would stop
   * one segment short the moment `s` lands on the terminating NUL right
   * after that trailing '/', silently miscounting "items/99/" as having the
   * same 2 segments as "items/99"; confirmed as a real regression this
   * exact way by trailing_slash_not_matched (tests.c) before this comment
   * was written: it let a route registered as "/items/{id}" incorrectly
   * match a request path ending in an extra "/". `!e` (no further
   * separator found at all) is what correctly, and solely, terminates
   * both loops below. */
  size_t count = 0;
  {
    const char *s = p;
    while (count < cap) {
      const char *e = strchr(s, '/');
      count++;
      if (!e) break;
      s = e + 1;
    }
  }

  _seg_cache_entry_t *entries =
      (_seg_cache_entry_t *)_mem_calloc(mp, count, sizeof(_seg_cache_entry_t));
  if (!entries) return -1;

  size_t idx = 0;
  const char *s = p;
  while (idx < count) {
    const char *e = strchr(s, '/');
    size_t len = e ? (size_t)(e - s) : strlen(s);
    entries[idx].raw = s;
    entries[idx].raw_len = len;
    idx++;
    if (!e) break;
    s = e + 1;
  }

  cache->entries = entries;
  cache->count = count;
  return 0;
}

static void _seg_cache_free(_seg_cache_t *cache) {
  if (!cache->entries) return;
  for (size_t i = 0; i < cache->count; i++)
    _mem_free(cache->mp, cache->entries[i].decoded);
  _mem_free(cache->mp, cache->entries);
  cache->entries = NULL;
  cache->count = 0;
}

/* Returns the percent-decoded value of cache's segment idx (memoized:
 * decoded at most once, on the first call for that position, regardless of
 * how many candidate routes end up asking for it), or NULL if that raw
 * segment can never match any route at all; either it is empty (a request
 * path with consecutive slashes, e.g. "a//b") or its percent-encoding is
 * malformed; both are a plain "no match" for whichever route asked, never
 * an error, exactly matching this module's own established "a decode
 * failure is a route mismatch, never a 400" contract (see conn->path's own
 * doc comment above). The cache retains ownership of the returned pointer
 * (valid until _seg_cache_free); a caller that needs to keep the value past
 * that point (a matched route's own captured param) must copy it.
 * *oom_out is set true only for a genuine allocation failure, which the
 * caller must treat as fatal to the whole route search, not merely this one
 * candidate route.
 *
 * WARNING for a future maintainer: e->attempted is set below BEFORE the
 * allocation is even attempted, so a transient OOM here is memoized as
 * permanently "not decodable" for the rest of this cache's lifetime, exactly
 * like the two genuinely deterministic outcomes (empty segment, malformed
 * percent-encoding) right next to it; no later call for this same idx
 * ever retries the allocation. This is harmless today only because
 * _match_route_cached/_find_route both treat *oom_out == true as fatal to
 * the ENTIRE route search the instant it happens (the search aborts and
 * this cache is freed within microseconds; see _find_route's own
 * "ms == -1" handling), so no second call for this idx against this same
 * cache instance can ever actually happen. If that OOM-handling policy is
 * ever changed to be skippable per-route (retry a different candidate
 * instead of aborting the whole search), this memoization must be revisited
 * first, or a route that would otherwise have matched once memory pressure
 * passes gets stuck reporting "no match" for the rest of this request's
 * route search. Deliberately left as-is rather than "fixed" preemptively:
 * the fix would have zero observable effect under the current, always-fatal
 * policy (nothing exercises a second call to this same idx either way), so
 * it could not ship with a regression test proving it does anything, per
 * this project's own standing discipline of never landing an unverifiable
 * change. */
static const char *_seg_cache_get(_seg_cache_t *cache, size_t idx,
                                  size_t *len_out, bool *oom_out) {
  *oom_out = false;
  _seg_cache_entry_t *e = &cache->entries[idx];
  if (e->attempted) {
    if (!e->ok) return NULL;
    *len_out = e->decoded_len;
    return e->decoded;
  }
  e->attempted = true;
  if (e->raw_len == 0) return NULL; /* empty segment: never matches anything */

  char *decoded = (char *)_mem_alloc(cache->mp, e->raw_len + 1);
  if (!decoded) {
    *oom_out = true; /* see this function's own WARNING above: memoized as
                      * permanent, not retried, currently harmless only
                      * because the caller treats this as search-fatal */
    return NULL;
  }
  memcpy(decoded, e->raw, e->raw_len);
  decoded[e->raw_len] = '\0';
  ssize_t dlen = _decode_path_unsafe(decoded, decoded); /* safe in place */
  if (dlen < 0) {
    _mem_free(cache->mp, decoded);
    return NULL; /* malformed percent-encoding: never matches anything */
  }
  decoded[(size_t)dlen] = '\0';
  e->ok = true;
  e->decoded = decoded;
  e->decoded_len = (size_t)dlen;
  *len_out = e->decoded_len;
  return decoded;
}

/* Matches route's own compiled segments against cache (built once per
 * router by the caller from that router's sub_path, and shared across every
 * candidate route tried against it; see _seg_cache_t's own doc comment). A
 * route whose own segment count differs from cache->count is rejected
 * immediately, before any segment is even decoded.
 *
 * pv_out, if non-NULL, receives a freshly allocated array of independently-
 * owned copies of the matched param values on a successful (1) match (the
 * cache's own decoded copies are never handed out directly: a candidate
 * that matches every segment tried so far but then fails on a later one
 * must leave every earlier position's cached value completely undisturbed
 * for the next candidate route to still consult, so ownership can never be
 * transferred out of the cache, only copied from it); left untouched
 * otherwise. Returns 1 (match), 0 (no match), or -1 (OOM, fatal to the
 * whole route search, not just this one candidate). */
static int _match_route_cached(_seg_cache_t *cache, chttpsvr_route_t *route,
                               char ***pv_out, ccol_memmgmt_procs_t *mp) {
  if ((size_t)route->seg_count != cache->count) return 0;
  if (route->seg_count == 0) {
    if (pv_out) *pv_out = NULL;
    return 1;
  }

  char **pv = NULL;
  int param_idx = 0;
  for (int i = 0; i < route->seg_count; i++) {
    size_t dlen = 0;
    bool oom = false;
    const char *decoded = _seg_cache_get(cache, (size_t)i, &dlen, &oom);
    if (!decoded) {
      if (oom) {
        if (pv) {
          for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
          _mem_free(mp, pv);
        }
        return -1;
      }
      goto no_match;
    }

    if (route->segs[i][0] == '{') {
      if (pv_out) {
        if (!pv) {
          pv = (char **)_mem_calloc(mp, (size_t)route->param_count,
                                    sizeof(char *));
          if (!pv) return -1;
        }
        char *val = (char *)_mem_alloc(mp, dlen + 1);
        if (!val) {
          for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
          _mem_free(mp, pv);
          return -1;
        }
        memcpy(val, decoded, dlen + 1); /* +1: include the NUL terminator */
        pv[param_idx++] = val;
      }
      /* pv_out == NULL: this route is only being probed to decide
       * ROUTE_MATCH_METHOD vs ROUTE_MATCH_NONE (see _find_route); the
       * decoded value has already been validated as decodable above (that
       * is what a non-NULL `decoded` from _seg_cache_get already proves),
       * and is simply not copied out anywhere in that case. */
    } else {
      if (strcmp(decoded, route->segs[i]) != 0) goto no_match;
    }
  }
  if (pv_out) *pv_out = pv;
  return 1;

no_match:
  if (pv) {
    for (int j = 0; j < param_idx; j++) _mem_free(mp, pv[j]);
    _mem_free(mp, pv);
  }
  return 0;
}

/* Matches router->prefix against the request path whose leading segments are
 * already split (raw) and memoized (decoded) in cache (see _seg_cache_t's own
 * doc comment; cache is built once per request by _find_route from the FULL
 * request path, shared across every non-root router's own call to this
 * function, exactly the way that same cache shape is already shared across
 * every candidate ROUTE tried against one router's own sub_path). Without
 * this sharing, each of N sub-routers independently re-percent-decoded the
 * same leading request-path segments from scratch; harmless for a handful of
 * routers, but reducible, client-request-independent redundant work all the
 * same, closed here the identical way _match_route_cached already closes it
 * for route segments.
 *
 * The prefix side is walked directly off router->prefix, unlike the path
 * side: a router's own prefix is a short, operator-controlled string with
 * nothing worth memoizing (it is never re-parsed for more than the one
 * router it belongs to), so only the client-controlled, potentially-shared
 * path segments go through cache.
 *
 * Returns 1 (match; *sub_path_out set), 0 (no match), or -1 (OOM, fatal to
 * the whole route search, matching cache's own _seg_cache_get contract). */
static int _prefix_matches(_seg_cache_t *cache, chttpsvr_router *router,
                           const char **sub_path_out) {
  const char *pp = router->prefix + 1;

  if (*pp == '\0') {
    /* The special "/" prefix (see chttpsvr_subrouter's own doc comment):
     * matches only the exact root path, i.e. a request path with zero
     * segments of its own. */
    if (cache->count != 0) return 0;
    *sub_path_out = "/";
    return 1;
  }

  size_t idx = 0;
  const char *rp_after_last_seg = NULL;
  while (*pp) {
    const char *pe = strchr(pp, '/');
    size_t plen = pe ? (size_t)(pe - pp) : strlen(pp);

    /* The request path ran out of segments before this prefix did: never a
     * match, mirroring the original rlen == 0 check this replaces. Checked
     * before indexing into cache->entries, which _seg_cache_get itself does
     * not bounds-check. */
    if (idx >= cache->count) return 0;

    size_t dlen = 0;
    bool oom = false;
    const char *decoded = _seg_cache_get(cache, idx, &dlen, &oom);
    if (oom) return -1;
    /* decoded == NULL: this path segment is either empty (consecutive
     * slashes in the request path) or its percent-encoding is malformed;
     * either way it can never match a real, non-empty literal prefix
     * segment, matching _match_route_cached's identical treatment of the
     * same two cases for a route (as opposed to a prefix) segment. */
    if (!decoded) return 0;

    bool lit = (dlen == plen && memcmp(decoded, pp, plen) == 0);
    if (!lit) return 0;

    rp_after_last_seg = cache->entries[idx].raw + cache->entries[idx].raw_len;
    idx++;
    pp = pe ? pe + 1 : pp + plen;
  }

  *sub_path_out = (*rp_after_last_seg == '\0') ? "/" : rp_after_last_seg;
  return 1;
}

static match_result_t _find_route(struct chttpserver *srv, const char *path,
                                  chttp_method_t method) {
  bool method_mismatch_seen = false;

  /* Shared by every non-root router's own _prefix_matches call below; see
   * that function's own doc comment for why. Built once per request, from
   * the FULL request path, capped at srv->max_prefix_seg_count + 1 (an
   * operator-controlled bound, not the request path's own, entirely
   * client-controlled, actual segment count; mirrors max_route_seg_count's
   * identical reasoning for the per-router route-matching cache below).
   * Left as an empty, always-safe-to-_seg_cache_free no-op cache when this
   * server has no sub-router at all (routers[0] is always root; every other
   * entry is a genuine, prefix-bearing sub-router), since the loop below
   * then never calls _prefix_matches to begin with. */
  _seg_cache_t path_cache = {0};
  if (srv->router_count > 1) {
    if (_seg_cache_build(path, (size_t)srv->max_prefix_seg_count + 1,
                         &path_cache, srv->m_procs) != 0)
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
  }

  for (size_t ri = 0; ri < srv->router_count; ri++) {
    chttpsvr_router *router = srv->routers[ri];

    const char *sub_path;
    if (router->prefix_len == 0) {
      sub_path = path;
    } else {
      int pm = _prefix_matches(&path_cache, router, &sub_path);
      if (pm < 0) {
        _seg_cache_free(&path_cache);
        return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
      }
      if (pm == 0) continue;
    }

    if (router->route_count == 0) continue;

    /* Built once per router, shared by every candidate route tried below:
     * see _seg_cache_t's own doc comment for why a raw segment's decoded
     * value is the same regardless of which route asks for it, and
     * _seg_cache_build's own doc comment for why capping the split at
     * router->max_route_seg_count + 1 (rather than sub_path's own, entirely
     * client-controlled, actual segment count) is both sufficient for every
     * route this router could possibly match and independent of how many
     * "/" characters a request path happens to contain. */
    _seg_cache_t cache;
    if (_seg_cache_build(sub_path, (size_t)router->max_route_seg_count + 1,
                         &cache, srv->m_procs) != 0) {
      _seg_cache_free(&path_cache);
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
    }

    match_result_t found = {NULL, NULL, NULL, ROUTE_MATCH_NONE};
    bool have_result = false;
    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route_t *route = router->routes[i];

      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      int ms = _match_route_cached(&cache, route, method_ok ? &pv : NULL,
                                   srv->m_procs);
      if (ms == -1) {
        found = (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM};
        have_result = true;
        break;
      }
      if (ms == 0) continue;

      if (!method_ok) {
        method_mismatch_seen = true;
        continue;
      }
      found = (match_result_t){router, route, pv, ROUTE_MATCH_OK};
      have_result = true;
      break;
    }
    _seg_cache_free(&cache);
    if (have_result) {
      _seg_cache_free(&path_cache);
      return found;
    }
  }
  _seg_cache_free(&path_cache);
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
 * silently wrap to a negative value on the (int) cast below; on every
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
 * genuine allocation failure or a vsnprintf encoding error.
 *
 * __attribute__((format(printf, 2, 3))) is what lets this function's own
 * internal vsnprintf(b->buf + b->len, avail, fmt, ap) call below pass fmt/ap
 * straight through without Clang's -Wformat-nonliteral complaining that fmt
 * is not a literal there; the real format-string/argument-type check
 * instead applies to every call site of this function itself, where fmt
 * genuinely is a literal. */
static bool __attribute__((format(printf, 2, 3))) _head_builder_append(
    _head_builder_t *b, const char *fmt, ...) {
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
 * Unlike every other header, a handler-supplied "Connection" or
 * "Content-Length" header set via chttpsvr_resp_set_header is never emitted
 * verbatim: keep_alive is always the one and only source of truth for the
 * wire Connection value, since it is also what the caller (_task_worker)
 * uses right after this call returns to decide whether the connection is
 * actually kept open, and resp->body_len (the exact byte count this
 * function is about to write) is always the one and only source of truth
 * for the wire Content-Length value. Letting either reach the wire
 * independently of what this function actually does would let the two
 * silently diverge (a wire response claiming "keep-alive" while the
 * socket is closed immediately after, or claiming a body length different
 * from what was actually sent) which is exactly the kind of framing bug
 * (response splitting/desync on a kept-alive connection) a caller has no
 * way to detect or recover from.
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
 * below is skipped for those two specifically. Any handler-supplied
 * Content-Length is filtered out unconditionally regardless of status (see
 * this function's own opening comment above); there is no status-specific
 * exception to that. */

/* Shared monotonic countdown-deadline helper: establishes *deadline (once,
 * latched via *deadline_set) max_dur ms from the first call, then on every
 * call shrinks *timeout_ms_inout to whatever time actually remains until
 * that deadline, if that is less than the caller's own already-computed
 * per-call timeout (0 in *timeout_ms_inout means "no per-call bound of its
 * own yet", so the remaining time always wins in that case); returns false
 * once the deadline itself has already passed. A no-op (always returns
 * true, touches nothing else) when max_dur is 0, so a caller with the
 * corresponding total-duration cap disabled pays nothing beyond the one
 * branch. Shared by _check_read_deadline (max_body_read_duration_ms) and
 * _send_response (max_response_write_duration_ms, below) so both total-
 * duration caps compute the identical way. */
static bool _shrink_timeout_to_deadline(struct timespec *deadline,
                                        bool *deadline_set, unsigned max_dur,
                                        unsigned *timeout_ms_inout) {
  if (!max_dur) return true;

  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (!*deadline_set) {
    *deadline = now;
    deadline->tv_sec += (time_t)(max_dur / 1000);
    deadline->tv_nsec += (long)(max_dur % 1000) * 1000000L;
    if (deadline->tv_nsec >= 1000000000L) {
      deadline->tv_nsec -= 1000000000L;
      deadline->tv_sec += 1;
    }
    *deadline_set = true;
  }
  /* long long (guaranteed >= 64 bits by C99), not long: max_dur is unsigned
   * and can be as large as UINT_MAX ms (~49.7 days), so deadline->tv_sec -
   * now.tv_sec can be as large as ~4.29e6 seconds; multiplying that by 1000
   * overflows a 32-bit long on an ILP32 build (this project builds and CI-
   * tests one) well before max_dur's own valid range is exhausted, silently
   * producing a spuriously negative (premature timeout) or wrapped-positive
   * (under-enforced timeout) result. long long has no such overflow risk
   * for any value max_dur can actually hold. */
  long long remaining_ms =
      (long long)(deadline->tv_sec - now.tv_sec) * 1000LL +
      (long long)(deadline->tv_nsec - now.tv_nsec) / 1000000LL;
  if (remaining_ms <= 0) return false;
  unsigned remaining_ms_clamped =
      (remaining_ms > (long long)UINT_MAX) ? UINT_MAX : (unsigned)remaining_ms;
  if (!*timeout_ms_inout || remaining_ms_clamped < *timeout_ms_inout)
    *timeout_ms_inout = remaining_ms_clamped;
  return true;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook only: forces the very next conn-guarded write-deadline
 * check inside _send_response's own header/body write loops (via
 * _response_write_deadline_ok below) to report "already expired", regardless
 * of the real elapsed time or conn->srv->max_response_write_duration_ms's
 * actual configured value. Exists because a courtesy rejection response's
 * header block (status line + a handful of fixed headers, always well under
 * a kilobyte, with no body at all) always completes in a single write(2)
 * call in practice, so there is no reliable way to coax a genuine short
 * write/expired-deadline outcome out of real socket buffering for it the way
 * max_response_write_duration_exceeded_closes_connection does for a real,
 * large, handler-supplied response body; mirrors
 * g_force_short_interim_write_bytes_for_tests's own identical rationale for
 * _write_interim_continue's fixed-size interim line. Disarmed (false) after
 * one use, so a test does not need to remember to reset it; has no effect at
 * all when conn is NULL, exactly like a real expired deadline would not,
 * since the deadline-tracking code this hook fakes the outcome of is itself
 * only ever consulted when conn is non-NULL. */
static _Atomic bool g_force_next_response_write_deadline_expired_for_tests =
    false;
void _chttpsvr_force_next_response_write_deadline_expired_for_tests(void) {
  atomic_store(&g_force_next_response_write_deadline_expired_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

/* Wraps _shrink_timeout_to_deadline for every internal write-retry loop that
 * needs conn-scoped, write-side deadline tracking (_send_response's own two
 * loops, header block and body, and _write_interim_continue's own single
 * loop) so all three consult the identical logic, including the
 * RUNNING_UNIT_TESTS-only forced-expiry hook above. Every caller already
 * guards on conn being non-NULL before calling this.
 *
 * apply_internal_ceiling selects which total-duration cap actually governs
 * this write: for a real, matched-route response (false; _send_response's
 * own is_reject parameter, passed straight through), the effective cap is
 * exactly conn->srv->max_response_write_duration_ms, unchanged (0 means "no
 * limit", the operator's own explicit choice for handler-controlled response
 * bodies). For an internally-generated, small, fixed-shape write (true; a
 * courtesy rejection response, or the "100 Continue" interim line; see
 * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS's own comment for why both share this
 * treatment), the effective cap is the tighter of that same configured value
 * and _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS: never looser than what the
 * operator configured (an operator who set a smaller cap still gets exactly
 * that), but never unbounded either, even when the operator left the knob at
 * its documented "disabled" default. */
static bool _response_write_deadline_ok(chttpsvr_conn_t *conn,
                                        bool apply_internal_ceiling,
                                        unsigned *call_timeout_ms) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_force_next_response_write_deadline_expired_for_tests,
                      false))
    return false;
#endif
  unsigned configured = atomic_load(&conn->srv->max_response_write_duration_ms);
  unsigned effective_max_dur = configured;
  if (apply_internal_ceiling &&
      (configured == 0 || configured > _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS))
    effective_max_dur = _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS;
  return _shrink_timeout_to_deadline(&conn->write_deadline,
                                     &conn->write_deadline_set,
                                     effective_max_dur, call_timeout_ms);
}

/* conn: both call sites (_conn_reject_and_close's courtesy rejection
 * response, and _task_worker's real matched-route response) now always pass
 * their own real, non-NULL conn, so every _send_response call's write is
 * bounded by max_response_write_duration_ms (conn->srv's own config) in
 * addition to timeout_ms's own per-call bound, via
 * conn->write_deadline/write_deadline_set and the shared
 * _shrink_timeout_to_deadline helper above; a rejection's own fixed, short
 * _CHTTPSVR_REJECT_WRITE_TIMEOUT_MS per-call timeout alone does NOT bound
 * its total send time. is_reject additionally activates
 * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS as an unconditional ceiling on that
 * total-duration bound for a rejection response specifically, regardless of
 * the operator's own max_response_write_duration_ms setting; see
 * _response_write_deadline_ok's own comment for why. The `if (conn && ...)`
 * checks below are retained purely as defensive belt-and-suspenders for this
 * internal helper's own signature, not because a real caller currently
 * passes NULL. */
static bool _send_response(chttp1_stream_t *stream, chttpsvr_resp *resp,
                           bool keep_alive, unsigned timeout_ms,
                           bool suppress_body, chttpsvr_conn_t *conn,
                           bool is_reject) {
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

  for (size_t i = 0; i < resp->header_count; i++) {
    /* "connection" is never re-emitted from resp->headers, regardless of
     * whether a handler set one: keep_alive below is the single source of
     * truth for whether this connection is actually kept open afterward
     * (see _task_worker's own identically-computed keep_alive, used for the
     * real post-response decision), and letting a handler-supplied value
     * reach the wire independently of that decision let the two silently
     * diverge; e.g. a handler setting "Connection: keep-alive" while the
     * server's own framing/timeout logic had already decided to close right
     * after, telling the client the opposite of what actually happens. This
     * function is the one place that both decides and announces connection
     * reuse, so it must be the only writer of this header.
     *
     * "content-length" is filtered for the identical reason: resp->body_len
     * (via chttpsvr_resp_write/_write_str/_printf/_write_json) is the single
     * source of truth for how many body bytes this function actually goes on
     * to write a few lines below; a handler-supplied value that happened to
     * disagree with it (a stale/copy-pasted header, or one merely reflecting
     * request-controlled data) would be just as real a framing hazard on a
     * kept-alive connection as a diverging Connection header; the client
     * would misframe the next pipelined response's bytes as the tail of this
     * one's body, or block waiting for bytes that will never arrive. Always
     * computing it from the one true byte count this function is about to
     * send, rather than trusting a caller-supplied number that could
     * silently drift from it, closes that gap the same way the Connection
     * handling above already does. */
    if (strcasecmp(resp->headers[i].name, "connection") == 0) continue;
    if (strcasecmp(resp->headers[i].name, "content-length") == 0) continue;
    /* No space after the colon: this test suite's raw-socket assertions
     * (e.g. strstr(buf, "connection:close")) assert on this exact wire
     * format directly, so the format must be produced byte for byte. */
    if (!_head_builder_append(&hb, "%s:%s\r\n", resp->headers[i].name,
                              resp->headers[i].value)) {
      _head_builder_release(&hb);
      return false;
    }
  }

  if (may_report_length) {
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

  /* conn's own srv-level cap, re-read once per write call (below) rather
   * than cached here, since it can only ever shrink the per-call timeout
   * further, never widen it, and a plain unsigned load is cheap enough not
   * to bother caching across what is, in the overwhelming majority of
   * responses, only one or two calls total anyway. */
  size_t sent = 0;
  bool head_sent = true;
  while (sent < hb.len) {
    unsigned call_timeout_ms = timeout_ms;
    if (conn &&
        !_response_write_deadline_ok(conn, is_reject, &call_timeout_ms)) {
      head_sent = false;
      break;
    }
    ssize_t n2 = chttp1_stream_write(stream, hb.buf + sent, hb.len - sent,
                                     _to_stream_timeout_ms(call_timeout_ms));
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
    unsigned call_timeout_ms = timeout_ms;
    if (conn && !_response_write_deadline_ok(conn, is_reject, &call_timeout_ms))
      return false;
    ssize_t n3 =
        chttp1_stream_write(stream, resp->body + sent, resp->body_len - sent,
                            _to_stream_timeout_ms(call_timeout_ms));
    if (n3 <= 0) return false;
    sent += (size_t)n3;
  }
  return true;
}

/* Writes the "HTTP/1.1 100 Continue\r\n\r\n" interim response (RFC 7231
 * SS5.1.1), shared by both the buffered-route (_task_worker) and
 * streaming-route (chttpsvr_req_read) Expect: 100-continue send sites.
 * Deliberately mirrors _send_response's own header/body write loops rather
 * than the single, unretried, unbounded-by-max_response_write_duration_ms
 * write both call sites used to perform directly:
 *   - Loops on a short write instead of treating any n <= 0 as the only
 *     failure signal: chttp1_stream_write is documented to return less than
 *     the requested length on a short write (POSIX write(2) semantics), and
 *     a caller needing the whole buffer sent must retry the remainder, the
 *     same discipline _send_response already follows for the real response.
 *     A single un-retried write of this 25-byte line risked leaving a
 *     truncated "HTTP/1.1 100 Con"-style status line on the wire with
 *     nothing to ever detect or report it, on a connection under enough
 *     send-buffer pressure for even this short a write to partially block.
 *   - Threads every individual write attempt through conn->write_deadline/
 *     _response_write_deadline_ok exactly like _send_response's own writes
 *     do, so this interim write's own duration counts against the same
 *     per-request max_response_write_duration_ms total-duration budget the
 *     real response's own send is bounded by, instead of being an
 *     unaccounted-for addition to it (previously reachable as a fully
 *     unbounded write whenever an operator left response_write_timeout_ms
 *     at its own default of "fall back to stream_read_timeout_ms", and set
 *     stream_read_timeout_ms itself to 0/"wait indefinitely" for slow
 *     legitimate uploads, while still relying on max_response_write_
 *     duration_ms alone to bound how long a slow-reading peer could pin a
 *     worker thread). apply_internal_ceiling is always true for this call
 *     (unlike _send_response, which only sets it for a rejection response,
 *     never a real one): the interim line is always this library's own
 *     small, fixed-shape, 25-byte content, so _CHTTPSVR_INTERNAL_WRITE_
 *     MAX_TOTAL_MS unconditionally bounds it even when the operator left
 *     max_response_write_duration_ms at its own default-disabled value of
 *     0 for their own handler-controlled response bodies; otherwise a
 *     peer sending an ordinary "Expect: 100-continue" request (standard
 *     client behavior, e.g. curl's own default for large uploads) and then
 *     trickling reads of this reply could hold a worker-pool thread
 *     hostage indefinitely, exhausting the whole pool with nothing more
 *     than a handful of such connections.
 *   - Unconditionally resets conn->write_deadline_set to false before
 *     returning, on every exit path: conn->write_deadline/write_deadline_set
 *     are per-REQUEST fields (only ever cleared once, in
 *     _conn_reset_for_request), and this function runs strictly before the
 *     request body is ever read (buffered: before _drain_body; streaming:
 *     before chttpsvr_req_read's own read loop). Leaving write_deadline_set
 *     true after this call would make _send_response's own later
 *     _shrink_timeout_to_deadline calls reuse the deadline THIS call
 *     established, silently charging max_response_write_duration_ms's
 *     budget for every second spent reading the body in between; an
 *     entirely unrelated, often much longer, and separately time-bounded
 *     (stream_read_timeout_ms/max_body_read_duration_ms) phase. A request
 *     whose body legitimately took a few real seconds to arrive (well within
 *     its own read-side budget, including the documented-supported "wait
 *     indefinitely" configuration for slow legitimate uploads) would
 *     otherwise find its deadline already expired the instant _send_response
 *     made its very first write attempt, closing the connection with zero
 *     response bytes sent despite the real send itself never having taken
 *     any measurable time at all. Resetting here makes _send_response always
 *     establish its own fresh deadline, reflecting only its own send
 *     duration, exactly matching max_response_write_duration_ms's documented
 *     contract ("bounds the total wall-clock time a worker thread will spend
 *     SENDING one response"). The (typically negligible, ~25-byte) cost of
 *     this interim write itself is still charged to the budget while it is
 *     actually happening, via the very same _shrink_timeout_to_deadline
 *     calls below; only the LEFTOVER, established-but-not-yet-consumed
 *     deadline is discarded afterward, never carried forward into a
 *     different phase of the request it was never meant to bound.
 * Returns true only if the complete 25-byte line was written. A total write
 * failure (n <= 0 on the very first attempt, nothing yet on the wire) and a
 * genuine short write (the deadline expires, or the stream reports a hard
 * error, after one or more bytes of this line have already been flushed)
 * both return false, but they are NOT equivalent for a caller: a total
 * failure leaves the connection exactly as untouched as it was before this
 * call, so any subsequent read/response-send attempt is free to fail
 * through its own, already-handled error path with nothing extra to worry
 * about; a short write leaves a truncated, unparseable status line
 * irrevocably on the wire, so the caller must not attempt any further write
 * on this same stream afterward (doing so would either land immediately
 * after the truncated line, corrupting the client's own response framing,
 * or be pointless if the peer already gave up on this connection); both
 * callers therefore treat ANY false return, not just a short one, the same
 * way: skip the real response send and close the connection, since a
 * caller has no cheap way to distinguish the two cases from outside (nor
 * any need to, since neither case leaves a connection worth sending more
 * bytes on). */
#ifdef RUNNING_UNIT_TESTS
/* White-box test hook only: reproducing a genuine short write of this
 * function's own tiny (25-byte) interim line deterministically, rather than
 * by trying to coax the OS's own socket buffering into a real partial
 * write, needs direct control over how much of it actually reaches the
 * wire. When armed with 0 < n < 25, the very next _write_interim_continue()
 * call writes exactly n bytes for real (a genuine chttp1_stream_write() of
 * a truncated prefix, so a test client genuinely receives those bytes) and
 * then returns false immediately, simulating the deadline having expired
 * right after that partial write; exactly the scenario a real, slow-
 * reading peer can trigger under max_response_write_duration_ms. Disarmed
 * (n = 0) after one use, so a test does not need to remember to reset it.
 * Gated so this symbol does not exist at all in a production build,
 * matching every other white-box helper in this file. */
static _Atomic size_t g_force_short_interim_write_bytes_for_tests = 0;
void _chttpsvr_force_short_interim_write_for_tests(size_t n) {
  atomic_store(&g_force_short_interim_write_bytes_for_tests, n);
}
#endif /* RUNNING_UNIT_TESTS */
static bool _write_interim_continue(chttpsvr_conn_t *conn,
                                    chttp1_stream_t *stream,
                                    unsigned wtimeout_ms) {
  static const char cont[] = "HTTP/1.1 100 Continue\r\n\r\n";
  size_t sent = 0;
#ifdef RUNNING_UNIT_TESTS
  size_t forced_short =
      atomic_exchange(&g_force_short_interim_write_bytes_for_tests, 0);
  if (forced_short > 0 && forced_short < sizeof(cont) - 1) {
    ssize_t n = chttp1_stream_write(stream, cont, forced_short,
                                    _to_stream_timeout_ms(wtimeout_ms));
    if (n > 0) sent = (size_t)n;
    conn->write_deadline_set = false;
    return sent == sizeof(cont) - 1;
  }
#endif /* RUNNING_UNIT_TESTS */
  while (sent < sizeof(cont) - 1) {
    unsigned call_timeout_ms = wtimeout_ms;
    if (!_response_write_deadline_ok(conn, /*apply_internal_ceiling=*/true,
                                     &call_timeout_ms))
      break;
    ssize_t n =
        chttp1_stream_write(stream, cont + sent, sizeof(cont) - 1 - sent,
                            _to_stream_timeout_ms(call_timeout_ms));
    if (n <= 0) break;
    sent += (size_t)n;
  }
  conn->write_deadline_set = false;
  return sent == sizeof(cont) - 1;
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
   * blocks traced to the route-matching pv calloc (now in
   * _match_route_cached), plus their "indirectly lost" decoded string
   * contents). */
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
  conn->interim_write_failed = false;
  _mem_free(mp, conn->body.buf);
  memset(&conn->body, 0, sizeof(conn->body));
  conn->body_bytes_seen = 0;
  conn->body_too_large = false;
  conn->body_alloc_failed = false;
  conn->transfer_aborted = false;
  conn->read_deadline_set = false;
  conn->deadline_exceeded = false;
  conn->write_deadline_set = false;

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
   * max_body_size itself is configured to exactly 0 (0 is this field's
   * own "no cap" sentinel (see its doc comment), so it cannot also express
   * "cap at zero") in which case the very first body byte the peer does
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
    conn->reg = EVENT_REG_INVALID;
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
    /* Overflow-safe growth via the shared helper (also used directly by
     * _servers_register/_chttpsvr_router_shell_register; chttpsvr_subrouter/
     * _router_add_route/_parse_qparams implement the identical overflow-safe
     * pattern inline with their own cap formula rather than calling this
     * function, but guard against the same hazard): a plain "cap * 2" can
     * wrap on a pathological cap, silently under-allocating
     * new_cap * sizeof(char *) below. Unreachable in practice today
     * (conn->hdr_count is bounded by chttp1_parser's own header-count cap,
     * at most 100 by default and not independently configurable via
     * chttpsvr_config_t), but kept consistent with this file's own
     * established idiom rather than relying on that bound never changing. */
    size_t new_cap = _doubling_growth_cap(conn->hdr_cap, sizeof(char *), 8);
    if (new_cap == 0) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
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
   * max_body_size; rather than only reactively, once that many bytes have
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
   * "handler always runs" contract to preserve; it surfaces through the
   * ordinary chttp1_parser_execute() error path _drain_body/
   * chttpsvr_req_read already handle, exactly like any other malformed body. */
  if (!mr.route->is_streaming && chttp1_has_content_length(p)) {
    size_t limit = atomic_load(&srv->max_body_size);
    /* limit == 0 is max_body_size's own documented "no cap" sentinel (see
     * that field's doc comment in chttpserver.h), not "cap at zero": a bare
     * `declared > limit` comparison would otherwise reject every request
     * that has any body at all whenever a caller explicitly configures "no
     * limit" this way, exactly the same convention max_connections/
     * max_header_bytes already use in this same config struct. */
    if (limit && chttp1_declared_content_length(p) > (uint64_t)limit) {
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_PAYLOAD_TOO_LARGE;
      return -1;
    }
  }

  /* A successful match already proved every segment of conn->path (RAW,
   * still percent-encoded) decodes cleanly; _match_route_cached/
   * _prefix_matches both treat any decode failure as a non-match, which
   * would have taken the ROUTE_MATCH_NONE/ROUTE_MATCH_METHOD branch above
   * instead of reaching here. Decoding the whole path now, for the public
   * chttpsvr_req_path() accessor, therefore cannot fail on malformed input;
   * only a real allocation failure can; rejected with 500 here, before
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
  /* Overflow-safe accumulation, matching the growbuf-growth guard a few
   * lines below in this same function: on a 32-bit (ILP32) build, a
   * streaming route (whose growbuf is periodically compacted back to empty
   * as chttpsvr_req_read drains it, so the connection never needs to hold
   * more than one batch resident at once) can receive well over 4 GiB of
   * cumulative body over one request. Without this guard, body_bytes_seen
   * (size_t) would silently wrap back near zero once that threshold is
   * crossed, and max_body_size enforcement would start passing again,
   * defeating the size cap for the remainder of the request. Treated the
   * same as an ordinary over-limit body: reject rather than wrap. */
  if (len > SIZE_MAX - conn->body_bytes_seen) {
    conn->body_too_large = true;
    return 1;
  }
  conn->body_bytes_seen += len;
  size_t body_limit = atomic_load(&conn->srv->max_body_size);
  /* body_limit == 0 is the documented "no cap" sentinel, matching the
   * identical special case in _on_headers_complete's own Content-Length
   * pre-check above; see that check's own comment. */
  if (body_limit && conn->body_bytes_seen > body_limit) {
    conn->body_too_large = true;
    return 1;
  }
  growbuf_t *b = &conn->body;
  /* Buffered routes never drain via pos (chttpsvr_req_read is not used for
   * them), so pos always stays 0 and this is never true for them; nothing
   * to compact in that case. */
  if (conn->matched_route->is_streaming && b->pos == b->len && b->pos > 0)
    b->pos = b->len = 0; /* compact: fully drained by a prior req_read call */
  /* min_cap is computed, and checked for wraparound, BEFORE it is ever
   * compared against b->cap; unlike an earlier version of this guard,
   * which compared the unguarded sum "b->len + len > b->cap" first and only
   * checked for overflow *inside* that branch. If the unguarded sum itself
   * wrapped around SIZE_MAX, the wrapped (small) result could satisfy
   * "<= b->cap", skipping the growth branch (and the overflow check nested
   * inside it) entirely and falling through to the memcpy below with b->len
   * still at its huge, pre-wrap value: a heap buffer overflow. Mirrors
   * _head_builder_grow's own correct construction (compute min_cap, then
   * detect wraparound via "min_cap < b->len" before using it for anything),
   * which this function's own comment already claimed to match but did not.
   * Reaching this in practice requires b->len within `len` of SIZE_MAX,
   * unreachable on a 64-bit build (b->cap would already have to hold on the
   * order of 2^64 bytes) but a real, if narrow, concern on an ILP32 build
   * (SIZE_MAX ~4.29 GiB) with max_body_size configured close to SIZE_MAX for
   * a streaming route whose handler drains slower than the peer sends. */
  size_t min_cap = b->len + len;
  if (min_cap < b->len) {
    /* size_t overflow: this body genuinely cannot be represented at all on
     * this platform, the same "too large to hold" outcome the cumulative
     * body_bytes_seen overflow guard above already reports this way, not a
     * transient allocation failure. */
    conn->body_too_large = true;
    return 1;
  }
  if (min_cap > b->cap) {
    /* Overflow-safe growth, matching _head_builder_grow's own pattern: a
     * plain "keep doubling" loop can wrap new_cap to 0 (an infinite loop,
     * since 0 can never reach min_cap by doubling) if max_body_size is
     * configured close to SIZE_MAX, letting min_cap approach it too. */
    size_t new_cap = b->cap ? b->cap : 8192;
    while (new_cap < min_cap) {
      if (new_cap > SIZE_MAX / 2) {
        new_cap = min_cap;
        break;
      }
      new_cap *= 2;
    }
    char *nb = (char *)_mem_realloc(conn->m_procs, b->buf, new_cap);
    if (!nb) {
      /* A genuine allocation failure, distinct from every other reason this
       * function returns 1: see body_alloc_failed's own field comment for
       * why this must not fall into the generic transfer-aborted bucket. */
      conn->body_alloc_failed = true;
      return 1;
    }
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
 * call this exactly once, and (critically) only once the connection has
 * reached a state _drain_and_close_all_connections can actually observe:
 * fully closed via _conn_close (including via _conn_reject_and_close), or
 * safely published back into the idle list via _idle_list_add. Calling this
 * any earlier (e.g. before a keep-alive connection's registration has
 * actually been resumed/re-added and idle-listed, or before a rejected
 * connection's courtesy response has actually been written and the
 * connection closed) opens a real window where the waiter can wake,
 * observe in_flight_requests == 0, and let __chttpsvr_destroy proceed to
 * free srv while this connection is still being finished on another
 * thread; a genuine use-after-free a ThreadSanitizer run over tests_tls
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
 * and paused/removed conn->reg itself, since at that point the outcome
 * (kept alive vs rejected) was not yet known) and the reactor thread's own
 * synchronous 404/405/500 rejection via _conn_dispatch_reject below. Every
 * caller must have already incremented srv->in_flight_requests for this
 * connection; this function releases it (via _release_in_flight) only once
 * the close has actually completed, not merely once queued; see that
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
 * either; previously only the 503 overload case avoided this. Unlike
 * _conn_start_diverted's own reg handling, this connection is never kept
 * alive after a rejection, so there is no reason to pause a live
 * registration (pausing exists purely to make a later event_loop_resume
 * cheap, which never happens here); it is removed outright instead.
 *
 * conn->state is set to CONN_ST_DIVERTED and conn is added to
 * srv->diverted_head/_tail (via _diverted_list_add), exactly like
 * _conn_start_diverted does for a matched-route request: a rejection's own
 * courtesy response write (below, via _conn_reject_via_pool) is real
 * blocking I/O on a reject_pool worker thread (or, in the fallback case, on
 * the calling thread itself) that can genuinely stall past this server's
 * own graceful-shutdown window against a slow-reading peer, exactly the
 * hazard the diverted-connection registry exists to bound. Without this, a
 * connection rejected here was invisible to _force_unblock_diverted_
 * connections (called from _wait_in_flight_bounded, itself called by
 * chttpsvr_stop()+chttpsvr_start() restarts and by chttpsvr_destroy()/
 * _engine_stop()'s own teardown), so a single ordinary rejected request
 * (404/405/413/500/501) stuck on a slow reader at this project's own
 * shipped default configuration (max_response_write_duration_ms == 0,
 * "disabled") could make a graceful shutdown/restart hang forever waiting
 * on ctpool_shutdown_drain(reject_pool), which has no timeout of its own;
 * _conn_free (reached via _conn_close, on every close path including
 * _conn_reject_and_close) already unconditionally calls
 * _diverted_list_remove, so no further cleanup is needed here. */
static void _conn_dispatch_reject(chttpsvr_conn_t *conn) {
  _idle_list_remove(conn);
  if (conn->reg) {
    event_loop_remove(srv_engine_bundler.reactor, conn->reg);
    conn->reg = EVENT_REG_INVALID;
  }
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
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
    conn->reg = EVENT_REG_INVALID;
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
     * takes it from here, specifically not resubmitting to `pool` itself;
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
  /* conn itself, not NULL, for _send_response's own conn parameter: a
   * single chttp1_stream_write() call only ever bounds ONE write(2) attempt
   * to timeout_ms (_CHTTPSVR_REJECT_WRITE_TIMEOUT_MS, 100ms) and returns the
   * instant even a single short write succeeds, so _send_response's own
   * outer while loop can call it again and again, each time with a brand
   * new 100ms allowance; a slow-read peer accepting only a byte or two
   * per write(2) attempt (the classic "slow read" HTTP DoS pattern this
   * codebase already treats as a first-class concern; see
   * max_response_write_duration_ms's own doc comment) can stretch a single
   * rejection response out far longer than the fixed per-call timeout
   * alone would suggest. Passing conn here activates the identical
   * conn->write_deadline/write_deadline_set cumulative-duration tracking
   * the matched-route response path (_task_worker) already gets, via the
   * exact same conn->srv->max_response_write_duration_ms config value;
   * conn->write_deadline_set is always false on entry here (reset for
   * every new request by _conn_reset_for_request, called strictly before
   * any header parsing, and therefore before any rejection decision,
   * for that same request could occur). is_reject == true additionally
   * activates _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS as an unconditional
   * ceiling on this same total-duration bound, so a caller who left
   * max_response_write_duration_ms at its own default-disabled value of 0
   * still gets a bounded rejection-response send; see that constant's own
   * comment for why a small, fixed-shape courtesy response has no
   * legitimate reason to need an unbounded budget even when the operator's
   * own handler-controlled responses are deliberately left uncapped. */
  _send_response(&stream, &resp, false, timeout_ms, conn->method == CHTTP_HEAD,
                 conn, /*is_reject=*/true);
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
 * fixed point (right after it has confirmed/acquired the shared engine
 * reference and registered itself with servers_bundler, before doing any
 * further reactor work (_idle_sweep_start_if_needed, _make_listen_socket,
 * event_loop_add for the listener)) signalling entry and then waiting for
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
  if (mutex_init(g_start_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_start_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
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

/* White-box test hook only: when armed, chttpsvr_start() blocks at a single
 * fixed point, immediately after contributed_to_engine has been set true
 * for this call (so a concurrent _engine_force_stop_quiesce_all pass, once
 * it finds raw registered, will already see a genuine engine contribution
 * to release) but immediately before its own _engine_acquire() call, the
 * exact window in which _engine_acquire() can observe srv_engine_bundler.
 * stopping == true and return without ever blocking on it (see that
 * function's own doc comment for the deadlock this closes and
 * chttpsvr_start()'s own "currently stopping" branch for the caller-side
 * retry this hook exists to deterministically reproduce). One-shot per arm
 * call. Gated so none of this exists in a production build. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_engine_stopping_race_hook = {0};

static void _engine_stopping_race_hook_init_globals(void) {
  if (mutex_init(g_engine_stopping_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_engine_stopping_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_engine_stopping_race_hook_for_tests(void) {
  call_once(g_engine_stopping_race_hook.once,
            _engine_stopping_race_hook_init_globals);
  mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.armed = true;
  g_engine_stopping_race_hook.entered = false;
  g_engine_stopping_race_hook.go = false;
  mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests(void) {
  call_once(g_engine_stopping_race_hook.once,
            _engine_stopping_race_hook_init_globals);
  mutex_lock(g_engine_stopping_race_hook.mutex);
  while (!g_engine_stopping_race_hook.entered)
    cond_var_wait(g_engine_stopping_race_hook.cv,
                  g_engine_stopping_race_hook.mutex);
  mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_release_engine_stopping_race_hook_for_tests(void) {
  call_once(g_engine_stopping_race_hook.once,
            _engine_stopping_race_hook_init_globals);
  mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.go = true;
  cond_var_broadcast(g_engine_stopping_race_hook.cv);
  mutex_unlock(g_engine_stopping_race_hook.mutex);
}

static void _engine_stopping_race_hook_wait_if_armed(void) {
  call_once(g_engine_stopping_race_hook.once,
            _engine_stopping_race_hook_init_globals);
  mutex_lock(g_engine_stopping_race_hook.mutex);
  if (!g_engine_stopping_race_hook.armed) {
    mutex_unlock(g_engine_stopping_race_hook.mutex);
    return;
  }
  g_engine_stopping_race_hook.armed = false; /* one-shot */
  g_engine_stopping_race_hook.entered = true;
  cond_var_broadcast(g_engine_stopping_race_hook.cv);
  while (!g_engine_stopping_race_hook.go)
    cond_var_wait(g_engine_stopping_race_hook.cv,
                  g_engine_stopping_race_hook.mutex);
  mutex_unlock(g_engine_stopping_race_hook.mutex);
}

/* White-box test hook only: when armed, chttpsvr_start() blocks at a single
 * fixed point, immediately after _chttpsvr_resolve(h) has succeeded (so
 * this call's own resolve pin, pending_resolve_count, is already live) but
 * before it does anything else at all, including its own lifecycle/
 * quiesce_state checks, signalling entry and then waiting for an explicit
 * release. Distinct from g_start_race_hook above (which pauses much later,
 * after registration/engine-acquire); this one exists specifically to
 * deterministically land a chttpsvr_start() call's resolve pin BEFORE a
 * concurrent _quiesce_server_once pass for the same server reaches its own
 * pending_resolve_count wait, reproducing the exact interleaving a real
 * deadlock between the two was found in (see chttpsvr_start()'s own
 * quiesce_state-wait comment for the full account: an earlier version of
 * that wait blocked on quiesce_done_cv while still holding this exact pin,
 * which deadlocks against _quiesce_server_once's own wait for this pin to
 * drop). One-shot per arm call. Gated so none of this exists in a production
 * build. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_resolve_race_hook = {0};

static void _start_resolve_race_hook_init_globals(void) {
  if (mutex_init(g_start_resolve_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_start_resolve_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_resolve_race_hook_for_tests(void) {
  call_once(g_start_resolve_race_hook.once,
            _start_resolve_race_hook_init_globals);
  mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.armed = true;
  g_start_resolve_race_hook.entered = false;
  g_start_resolve_race_hook.go = false;
  mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_wait_start_resolve_race_hook_entered_for_tests(void) {
  call_once(g_start_resolve_race_hook.once,
            _start_resolve_race_hook_init_globals);
  mutex_lock(g_start_resolve_race_hook.mutex);
  while (!g_start_resolve_race_hook.entered)
    cond_var_wait(g_start_resolve_race_hook.cv,
                  g_start_resolve_race_hook.mutex);
  mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_release_start_resolve_race_hook_for_tests(void) {
  call_once(g_start_resolve_race_hook.once,
            _start_resolve_race_hook_init_globals);
  mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.go = true;
  cond_var_broadcast(g_start_resolve_race_hook.cv);
  mutex_unlock(g_start_resolve_race_hook.mutex);
}

static void _start_resolve_race_hook_wait_if_armed(void) {
  call_once(g_start_resolve_race_hook.once,
            _start_resolve_race_hook_init_globals);
  mutex_lock(g_start_resolve_race_hook.mutex);
  if (!g_start_resolve_race_hook.armed) {
    mutex_unlock(g_start_resolve_race_hook.mutex);
    return;
  }
  g_start_resolve_race_hook.armed = false; /* one-shot */
  g_start_resolve_race_hook.entered = true;
  cond_var_broadcast(g_start_resolve_race_hook.cv);
  while (!g_start_resolve_race_hook.go)
    cond_var_wait(g_start_resolve_race_hook.cv,
                  g_start_resolve_race_hook.mutex);
  mutex_unlock(g_start_resolve_race_hook.mutex);
}

/* White-box test hook only: when armed, _chttpsvr_stop_internal() blocks at
 * a single fixed point, right after it has entered CHTTPSVR_LC_STOPPING
 * and released raw->mutex, but strictly before its own blocking
 * event_loop_remove()/close() call for the OLD listener registration,
 * signalling entry and then waiting for an explicit release. This lets a
 * test deterministically land a concurrent chttpsvr_start() call inside the
 * exact window CHTTPSVR_LC_STOPPING exists to close (see chttpsvr_
 * lifecycle_t's own comment on struct chttpserver), rather than relying on
 * real, unbounded timing to probabilistically hit a race window that would
 * otherwise be a handful of instructions wide. One-shot per arm call. Gated
 * so none of this exists in a production build, mirroring g_start_race_
 * hook/g_start_resolve_race_hook's own identical shape exactly. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_stop_race_hook = {0};

static void _stop_race_hook_init_globals(void) {
  if (mutex_init(g_stop_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_stop_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_stop_race_hook_for_tests(void) {
  call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.armed = true;
  g_stop_race_hook.entered = false;
  g_stop_race_hook.go = false;
  mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_wait_stop_race_hook_entered_for_tests(void) {
  call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  mutex_lock(g_stop_race_hook.mutex);
  while (!g_stop_race_hook.entered)
    cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_release_stop_race_hook_for_tests(void) {
  call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.go = true;
  cond_var_broadcast(g_stop_race_hook.cv);
  mutex_unlock(g_stop_race_hook.mutex);
}

static void _stop_race_hook_wait_if_armed(void) {
  call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  mutex_lock(g_stop_race_hook.mutex);
  if (!g_stop_race_hook.armed) {
    mutex_unlock(g_stop_race_hook.mutex);
    return;
  }
  g_stop_race_hook.armed = false; /* one-shot */
  g_stop_race_hook.entered = true;
  cond_var_broadcast(g_stop_race_hook.cv);
  while (!g_stop_race_hook.go)
    cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  mutex_unlock(g_stop_race_hook.mutex);
}

/* White-box test instrumentation only: a plain, non-blocking "have we
 * reached this point" signal for chttpsvr_start()'s own CHTTPSVR_LC_
 * STOPPING wait branch (see that switch case's own comment), deliberately
 * NOT shaped like g_stop_race_hook/g_start_race_hook above (which also
 * park the caller until an explicit release). A parking hook here would be
 * unsafe: unlike every existing park-style hook in this file (each fires
 * only after its own caller has already released whatever mutex the OTHER
 * side of the race might need next), this exact call site is still holding
 * raw->mutex when it reaches this point, which is precisely what _chttpsvr_
 * stop_internal() itself must eventually acquire to leave CHTTPSVR_LC_
 * STOPPING; a test forgetting (or being unable, given the trickier
 * multi-thread choreography) to release such a park promptly could wedge
 * BOTH sides of the very race this hook exists to test deterministically,
 * not just one call. Firing a one-shot, non-blocking signal instead (set a
 * flag, broadcast, keep going immediately into the real cond_var_wait loop
 * below) adds no new blocking point of its own, while still letting a test
 * confirm, deterministically, that chttpsvr_start() has genuinely reached
 * (and is about to enter) its own real wait - closing the "the 150ms
 * settling sleep elapsed before the other thread was even scheduled, not
 * because it is genuinely blocked" gap a fixed sleep alone cannot close,
 * without touching this function's own carefully-established locking
 * discipline at all. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
} g_start_stopping_wait_signal = {0};

static void _start_stopping_wait_signal_init_globals(void) {
  if (mutex_init(g_start_stopping_wait_signal.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_start_stopping_wait_signal.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_stopping_wait_signal_for_tests(void) {
  call_once(g_start_stopping_wait_signal.once,
            _start_stopping_wait_signal_init_globals);
  mutex_lock(g_start_stopping_wait_signal.mutex);
  g_start_stopping_wait_signal.armed = true;
  g_start_stopping_wait_signal.entered = false;
  mutex_unlock(g_start_stopping_wait_signal.mutex);
}

void _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests(void) {
  call_once(g_start_stopping_wait_signal.once,
            _start_stopping_wait_signal_init_globals);
  mutex_lock(g_start_stopping_wait_signal.mutex);
  while (!g_start_stopping_wait_signal.entered)
    cond_var_wait(g_start_stopping_wait_signal.cv,
                  g_start_stopping_wait_signal.mutex);
  mutex_unlock(g_start_stopping_wait_signal.mutex);
}

static void _start_stopping_wait_signal_fire_if_armed(void) {
  call_once(g_start_stopping_wait_signal.once,
            _start_stopping_wait_signal_init_globals);
  mutex_lock(g_start_stopping_wait_signal.mutex);
  if (g_start_stopping_wait_signal.armed) {
    g_start_stopping_wait_signal.armed = false; /* one-shot */
    g_start_stopping_wait_signal.entered = true;
    cond_var_broadcast(g_start_stopping_wait_signal.cv);
  }
  mutex_unlock(g_start_stopping_wait_signal.mutex);
}

/* White-box test hook only: when armed, _engine_force_stop_quiesce_all()
 * blocks at a single fixed point (right after it has pinned the server
 * it just read out of servers_bundler.servers[0] (servers_bundler_pins;
 * see that field's own comment) and released servers_bundler.mutex, but
 * strictly before its own call to _quiesce_server_once()) signalling
 * entry and then waiting for an explicit release. This lets a test
 * deterministically land a concurrent chttpsvr_destroy() call on that
 * exact server inside the window servers_bundler_pins exists to close
 * (a bare struct chttpserver* the reaper thread holds with nothing but
 * this pin protecting it from being freed out from under it), rather than
 * relying on real, unbounded timing to probabilistically hit a race window
 * that would otherwise be a handful of instructions wide. One-shot per arm
 * call. Mirrors g_stop_race_hook's own identical shape exactly. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_reaper_race_hook = {0};

static void _reaper_race_hook_init_globals(void) {
  if (mutex_init(g_reaper_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_reaper_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_reaper_race_hook_for_tests(void) {
  call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.armed = true;
  g_reaper_race_hook.entered = false;
  g_reaper_race_hook.go = false;
  mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_wait_reaper_race_hook_entered_for_tests(void) {
  call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  mutex_lock(g_reaper_race_hook.mutex);
  while (!g_reaper_race_hook.entered)
    cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_release_reaper_race_hook_for_tests(void) {
  call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.go = true;
  cond_var_broadcast(g_reaper_race_hook.cv);
  mutex_unlock(g_reaper_race_hook.mutex);
}

static void _reaper_race_hook_wait_if_armed(void) {
  call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  mutex_lock(g_reaper_race_hook.mutex);
  if (!g_reaper_race_hook.armed) {
    mutex_unlock(g_reaper_race_hook.mutex);
    return;
  }
  g_reaper_race_hook.armed = false; /* one-shot */
  g_reaper_race_hook.entered = true;
  cond_var_broadcast(g_reaper_race_hook.cv);
  while (!g_reaper_race_hook.go)
    cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  mutex_unlock(g_reaper_race_hook.mutex);
}

/* White-box test hook only: when armed, _quiesce_server_once() blocks at a
 * single fixed point: right after it has claimed the winning side (srv->
 * quiesce_state just set to CHTTPSVR_QS_QUIESCING, the pending_resolve_
 * count wait already satisfied, srv->mutex released) but strictly before
 * it does any of its own real teardown work (_chttpsvr_stop_internal/
 * _servers_unregister/_drain_and_close_all_connections/_engine_release/
 * _destroy_detached_pools). This lets a test deterministically fork() a
 * process while a thread is parked exactly where a real, unbounded
 * interleaving could also land one: with quiesce_state == CHTTPSVR_QS_
 * QUIESCING, reproducing the exact server state _chttpsvr_atfork_release_
 * impl's own child-side fixup (see that function's own doc comment) exists
 * to handle, rather than relying on real, unbounded timing to
 * probabilistically hit a race window that would otherwise be arbitrarily
 * narrow. One-shot per arm call. Mirrors g_reaper_race_hook's own
 * identical shape exactly. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_quiesce_teardown_race_hook = {0};

static void _quiesce_teardown_race_hook_init_globals(void) {
  if (mutex_init(g_quiesce_teardown_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_quiesce_teardown_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void) {
  call_once(g_quiesce_teardown_race_hook.once,
            _quiesce_teardown_race_hook_init_globals);
  mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.armed = true;
  g_quiesce_teardown_race_hook.entered = false;
  g_quiesce_teardown_race_hook.go = false;
  mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void) {
  call_once(g_quiesce_teardown_race_hook.once,
            _quiesce_teardown_race_hook_init_globals);
  mutex_lock(g_quiesce_teardown_race_hook.mutex);
  while (!g_quiesce_teardown_race_hook.entered)
    cond_var_wait(g_quiesce_teardown_race_hook.cv,
                  g_quiesce_teardown_race_hook.mutex);
  mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void) {
  call_once(g_quiesce_teardown_race_hook.once,
            _quiesce_teardown_race_hook_init_globals);
  mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.go = true;
  cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

static void _quiesce_teardown_race_hook_wait_if_armed(void) {
  call_once(g_quiesce_teardown_race_hook.once,
            _quiesce_teardown_race_hook_init_globals);
  mutex_lock(g_quiesce_teardown_race_hook.mutex);
  if (!g_quiesce_teardown_race_hook.armed) {
    mutex_unlock(g_quiesce_teardown_race_hook.mutex);
    return;
  }
  g_quiesce_teardown_race_hook.armed = false; /* one-shot */
  g_quiesce_teardown_race_hook.entered = true;
  cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  while (!g_quiesce_teardown_race_hook.go)
    cond_var_wait(g_quiesce_teardown_race_hook.cv,
                  g_quiesce_teardown_race_hook.mutex);
  mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

/* Forward declaration: defined later in this file (used by
 * _wait_in_flight_bounded and friends), needed here too for the bounded
 * wait in _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_
 * tests below. */
static void _timespec_add_ms(struct timespec *ts, unsigned ms);

/* White-box test hook only: when armed, pauses chttpsvr_start()'s own
 * CHTTPSVR_QS_QUIESCING backoff branch immediately after it has both
 * registered itself in quiesce_waiters AND released its resolve pin, but
 * strictly before it re-acquires raw->mutex for the very first time since
 * doing so; the exact window a real, reproducible use-after-free used to
 * live in, before quiesce_waiters++ was moved to happen before (rather than
 * after) the pin release: releasing the pin first could unblock a
 * concurrent _quiesce_server_once pass's own pending_resolve_count wait,
 * letting it run to completion and free raw before this call ever got back
 * to raw->mutex to register itself as a protected waiter. Lets a test pause
 * a real chttpsvr_start() call exactly there, then drive the rest of a
 * genuine _quiesce_server_once pass (and a genuinely concurrent
 * chttpsvr_destroy()) to completion around it, to directly prove raw
 * survives regardless; rather than relying on unbounded, real timing to
 * probabilistically land two threads in a race window a few instructions
 * wide. One-shot per arm call. Mirrors g_quiesce_teardown_race_hook's own
 * identical shape exactly. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_quiescing_unpinned_race_hook = {0};

static void _start_quiescing_unpinned_race_hook_init_globals(void) {
  if (mutex_init(g_start_quiescing_unpinned_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  /* CLOCK_MONOTONIC, matching the create_chttpsvr_mp/_wait_and_detach_pools
   * precedent commented there: _chttpsvr_wait_start_quiescing_unpinned_
   * race_hook_entered_for_tests below computes its own bounded-wait
   * deadline via clock_gettime(CLOCK_MONOTONIC, ...), which cond_var_init's
   * default clock (CLOCK_REALTIME) would compare against incorrectly. */
  cond_var_attr_t cv_attr;
  int cv_rc;
  if (cond_var_attr_init(cv_attr) == 0) {
    cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc = cond_var_init_ca(g_start_quiescing_unpinned_race_hook.cv, cv_attr);
    cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = cond_var_init(g_start_quiescing_unpinned_race_hook.cv);
  }
  if (cv_rc != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests(void) {
  call_once(g_start_quiescing_unpinned_race_hook.once,
            _start_quiescing_unpinned_race_hook_init_globals);
  mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.armed = true;
  g_start_quiescing_unpinned_race_hook.entered = false;
  g_start_quiescing_unpinned_race_hook.go = false;
  mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* Unlike every OTHER _chttpsvr_wait_*_race_hook_entered_for_tests function
 * in this file, reaching THIS hook is not provably deterministic from the
 * calling test's own prior steps alone: the sole test that uses it
 * (start_racing_engine_stop_and_destroy_does_not_free_raw_too_early, in
 * tests_engine_stop.c) additionally depends on a concurrent reaper thread
 * having already set CHTTPSVR_QS_QUIESCING before the paused
 * chttpsvr_start() call re-checks quiesce_state, an ordering that test
 * enforces only via a fixed nanosleep(150ms), not a genuine synchronization
 * primitive; environment load (valgrind, TSan, a busy CI runner) could in
 * principle stretch that window past 150ms, in which case this hook would
 * never be entered at all. Bounded here (10s, generous even under heavy
 * instrumentation) so a missed race degrades to a clean, caught test
 * failure instead of hanging this call; and, since it runs on the main
 * test thread rather than a background one, hanging the entire binary with
 * no bounded-join fixture able to rescue it. Returns false on timeout. */
bool _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests(void) {
  call_once(g_start_quiescing_unpinned_race_hook.once,
            _start_quiescing_unpinned_race_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  _timespec_add_ms(&deadline, 10000);
  mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  bool timed_out = false;
  while (!g_start_quiescing_unpinned_race_hook.entered && !timed_out) {
    timed_out = cond_var_timedwait(g_start_quiescing_unpinned_race_hook.cv,
                                   g_start_quiescing_unpinned_race_hook.mutex,
                                   deadline) == ETIMEDOUT;
  }
  bool entered = g_start_quiescing_unpinned_race_hook.entered;
  mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
  return entered;
}

void _chttpsvr_release_start_quiescing_unpinned_race_hook_for_tests(void) {
  call_once(g_start_quiescing_unpinned_race_hook.once,
            _start_quiescing_unpinned_race_hook_init_globals);
  mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.go = true;
  cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

static void _start_quiescing_unpinned_race_hook_wait_if_armed(void) {
  call_once(g_start_quiescing_unpinned_race_hook.once,
            _start_quiescing_unpinned_race_hook_init_globals);
  mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  if (!g_start_quiescing_unpinned_race_hook.armed) {
    mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
    return;
  }
  g_start_quiescing_unpinned_race_hook.armed = false; /* one-shot */
  g_start_quiescing_unpinned_race_hook.entered = true;
  cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  while (!g_start_quiescing_unpinned_race_hook.go)
    cond_var_wait(g_start_quiescing_unpinned_race_hook.cv,
                  g_start_quiescing_unpinned_race_hook.mutex);
  mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* White-box test hook only: when armed, _listener_on_readable() blocks at a
 * single fixed point (right after it has pinned srv (listener_dispatch_
 * pins; see that field's own comment) but strictly before its own call into
 * _listener_on_readable_impl()) signalling entry and then waiting for an
 * explicit release. This lets a test deterministically land a concurrent
 * chttpsvr_destroy() call while a listener dispatch is genuinely "in
 * flight" and holding this pin, and assert that destroy blocks until the
 * hook is released, rather than relying on real, unbounded timing (racing a
 * live accept4() backoff sleep) to probabilistically hit a window that is
 * real but was never reliably reproducible on demand. Mirrors g_reaper_
 * race_hook's own identical shape exactly. */
static struct {
  mutex_t mutex;
  cond_var_t cv;
  once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_listener_dispatch_race_hook = {0};

static void _listener_dispatch_race_hook_init_globals(void) {
  if (mutex_init(g_listener_dispatch_race_hook.mutex) != 0)
    fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (cond_var_init(g_listener_dispatch_race_hook.cv) != 0)
    fatal_err("chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_listener_dispatch_race_hook_for_tests(void) {
  call_once(g_listener_dispatch_race_hook.once,
            _listener_dispatch_race_hook_init_globals);
  mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.armed = true;
  g_listener_dispatch_race_hook.entered = false;
  g_listener_dispatch_race_hook.go = false;
  mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests(void) {
  call_once(g_listener_dispatch_race_hook.once,
            _listener_dispatch_race_hook_init_globals);
  mutex_lock(g_listener_dispatch_race_hook.mutex);
  while (!g_listener_dispatch_race_hook.entered)
    cond_var_wait(g_listener_dispatch_race_hook.cv,
                  g_listener_dispatch_race_hook.mutex);
  mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_release_listener_dispatch_race_hook_for_tests(void) {
  call_once(g_listener_dispatch_race_hook.once,
            _listener_dispatch_race_hook_init_globals);
  mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.go = true;
  cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

static void _listener_dispatch_race_hook_wait_if_armed(void) {
  call_once(g_listener_dispatch_race_hook.once,
            _listener_dispatch_race_hook_init_globals);
  mutex_lock(g_listener_dispatch_race_hook.mutex);
  if (!g_listener_dispatch_race_hook.armed) {
    mutex_unlock(g_listener_dispatch_race_hook.mutex);
    return;
  }
  g_listener_dispatch_race_hook.armed = false; /* one-shot */
  g_listener_dispatch_race_hook.entered = true;
  cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  while (!g_listener_dispatch_race_hook.go)
    cond_var_wait(g_listener_dispatch_race_hook.cv,
                  g_listener_dispatch_race_hook.mutex);
  mutex_unlock(g_listener_dispatch_race_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* reject_pool's task function: runs _conn_reject_and_close on reject_pool's
 * own dedicated thread instead of the reactor thread (see that field's own
 * comment on struct chttpserver and _conn_start_diverted's comment on why),
 * then releases this request's in_flight_requests slot (mirroring
 * _task_worker's own decrement) only once the close has actually
 * completed, not merely once this task was queued; see
 * _release_in_flight's own comment for why that ordering matters. srv is
 * captured before _conn_reject_and_close, which frees conn internally. */
static void _reject_task(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _chttpsvr_mark_worker_thread(srv);
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
        /* For a plaintext connection, EWOULDBLOCK from a raw read(2) always
         * means "wait for readable," matching selectable_from_fd's own
         * read-direction registration below. For a TLS connection, it does
         * NOT: OpenSSL can need to write before this exact ctls_conn_read()
         * call can make progress (e.g. flushing a deferred post-handshake
         * session ticket, or a TLS 1.2 renegotiation); see
         * ctls_conn_wants_write's own doc comment. Registering (or staying
         * registered) for read-only interest in that case would leave this
         * connection with no further readiness event ever arriving in the
         * direction it actually still needs, stalling it until whatever
         * read/idle timeout is configured (indefinitely, under the
         * documented 0 = "wait forever" setting) fires instead of the next
         * real byte. */
        ccol_select_dir want = ccol_select_read;
        if (conn->tls && ctls_conn_wants_write(conn->tls))
          want = ccol_select_write;
        if (!conn->reg) {
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
        } else {
          /* Already registered (read-direction, from a prior iteration of
           * this same loop, or from CTLS_HANDSHAKE_DONE above): recompute
           * the interest fresh on every EWOULDBLOCK rather than assuming
           * it's still correct, exactly like the handshake-stepping branch
           * above already does for the identical reason. A harmless no-op
           * when want hasn't actually changed. */
          if (event_loop_modify(srv_engine_bundler.reactor, conn->reg, want) !=
              ccol_success) {
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
  if (!_shrink_timeout_to_deadline(&conn->read_deadline,
                                   &conn->read_deadline_set, max_dur,
                                   timeout_ms_inout)) {
    conn->deadline_exceeded = true;
    return false;
  }
  return true;
}

/* Drains the whole body of a buffered route's request via chttp1_stream_read
 * + chttp1_parser_execute, both driven by the calling worker thread. Only
 * ever called for a buffered route (see _task_worker, which never calls this
 * for a streaming one); a streaming route's handler pulls its own body via
 * chttpsvr_req_read() instead, and any body bytes left unread once the
 * handler returns are never separately drained here; that connection
 * simply cannot be kept alive (see _task_worker's own msg_fully_parsed/
 * keep_alive computation), since the unread bytes would otherwise be
 * misparsed as the start of the next pipelined request. Returns
 * ccol_success, or a specific failure the caller maps to a status code. */
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
      /* A genuine _on_body allocation failure (conn->body_alloc_failed) is
       * likewise reported as its own distinct outcome rather than folded
       * into the generic transfer-aborted bucket, matching chttpsvr_req_
       * stream_error()'s identical priority for the streaming-route path;
       * see body_alloc_failed's own field comment. */
      return conn->body_too_large      ? ccol_msg_too_large
             : conn->body_alloc_failed ? ccol_not_enough_memory
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
  /* conn/conn->matched_route are never actually NULL for a req reachable
   * through the documented public API: a chttpsvr_req is only ever
   * constructed by _task_worker, which only ever runs once a route has
   * already matched (conn->matched_route is set in _on_headers_complete
   * before this request is diverted to a worker at all). Guarded anyway
   * (unlike chttpsvr_req_method/_path/_header/_raw_query, which dereference
   * req->conn unconditionally once !req has already been ruled out, since
   * the same "never actually NULL in practice" argument applies equally to
   * all of them), so a req outlived past its documented "valid only for the
   * duration of the handler call" lifetime, or any other future misuse
   * that reaches here with a not-fully-populated req, fails safe instead
   * of crashing. */
  if (!conn || !conn->matched_route) return -1;
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
      unsigned wtimeout_ms = atomic_load(&conn->srv->response_write_timeout_ms);
      /* See _write_interim_continue's own doc comment: a TOTAL write
       * failure leaves the connection untouched, so the read attempt below
       * would naturally fail on its own and report through this function's
       * ordinary error path regardless; but a genuine SHORT write leaves a
       * truncated, unparseable status line already on the wire, which a
       * plain read failure below can never detect or account for on its
       * own. Both are therefore treated identically here: latched onto conn
       * so _task_worker's own later _send_response call is suppressed
       * (never writing a real response after a possibly-corrupted interim
       * one), and surfaced to this streaming handler as an ordinary
       * transfer-aborted error via the immediate return below, exactly like
       * any other stream-ending failure it already has to handle. */
      if (!_write_interim_continue(conn, req->stream, wtimeout_ms)) {
        conn->interim_write_failed = true;
        conn->transfer_aborted = true;
      }
    }
  }
  if (conn->interim_write_failed) return -1;

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
  if (conn->body_alloc_failed) return ccol_not_enough_memory;
  if (conn->transfer_aborted) return ccol_http_transfer_aborted;
  return ccol_success;
}

static void _task_worker(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _chttpsvr_mark_worker_thread(srv);

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
   * with zero response bytes instead of a graceful error; and, for a
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
   * ever calling chttpsvr_req_read(); exactly the scenario Expect:
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
   * chttpsvr_req_read()'s identical guard for the full reasoning; there is
   * no body left to invite, so sending "100 Continue" here would tell the
   * client to upload one that was never coming. */
  if (prepared && !body_unavailable && conn->expects_continue &&
      !conn->matched_route->is_streaming &&
      !chttp1_parser_message_complete(&conn->parser)) {
    if (!_write_interim_continue(conn, &stream, write_timeout_ms))
      conn->interim_write_failed = true;
    conn->interim_continue_sent = true;
  }

  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = prepared ? &stream : NULL;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval_t body_err = ccol_success;
  /* conn->interim_write_failed (set just above, if at all): the interim
   * "100 Continue" write already left a possibly-truncated line on the
   * wire, and this connection is closed unconditionally below with no
   * response ever sent (see the !conn->interim_write_failed check on
   * `sent` further down) regardless of what happens here; draining a
   * body nobody will ever see the outcome of, or running the handler for
   * a request that can never be answered, would only tie up this worker
   * thread for no benefit, so both are skipped entirely in that case. */
  bool aborted = conn->interim_write_failed;
  if (!aborted) {
    if (body_unavailable) {
      /* The request's body was lost to the OOM above; never invoke the
       * handler with a stream that cannot actually deliver it, for either a
       * buffered or a streaming route; see body_unavailable's own comment
       * above for why this must not be conditioned on route kind the way
       * the ordinary body_err-from-_drain_body case below is. */
      body_err = ccol_not_enough_memory;
    } else if (prepared) {
      if (!conn->matched_route->is_streaming) {
        body_err = _drain_body(conn, &stream);
      }
      /* Streaming routes: the handler pulls the body itself via
       * chttpsvr_req_read(); nothing to pre-drain here. */
    } else {
      /* Unreachable in practice: the NULL/0 retry above can never fail (see
       * body_unavailable's own comment), but fail closed rather than
       * assume. */
      body_err = ccol_not_enough_memory;
    }

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
       * (keep-alive vs close) below already accounts for a not-fully-
       * drained body via chttp1_should_keep_alive's message-completion
       * check. */
    }
  }

  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large;

  /* !conn->interim_write_failed: a genuine short write of the "100
   * Continue" interim line (see _write_interim_continue's own doc comment)
   * already left a truncated status line on the wire; writing a real
   * response on top of it here would either corrupt the client's framing
   * further or be pointless against a peer that is already gone, so this
   * connection is closed instead of ever attempting the real send. */
  bool sent = prepared && !conn->interim_write_failed &&
              _send_response(&stream, &conn->resp, keep_alive, write_timeout_ms,
                             conn->method == CHTTP_HEAD, conn,
                             /*is_reject=*/false);
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
    /* Force the registration back to read-direction before resuming it:
     * this site's only purpose is "wait for the next request's bytes to
     * arrive," always read-direction, but conn->reg's own recorded
     * direction (event_reg's own sel.dir) may still be write-direction
     * left over from an earlier cycle. That happens when this exact
     * connection's most recent successful read (the one that completed the
     * request just finished above) was itself preceded by a
     * ctls_conn_wants_write-driven write-direction wait in _conn_pump's own
     * read loop, and the connection was diverted to this worker on that
     * very same successful read; with nothing in between to ever flip
     * the registration back to read, since that flip only happens on a
     * LATER read attempt that hits EWOULDBLOCK wanting read specifically.
     * event_loop_modify is safe to call on a currently-paused registration
     * (it only updates the registration's own recorded direction and the
     * underlying fd entry's read/write slot assignment; delivery itself
     * stays suppressed until event_loop_resume actually re-arms it), so
     * this always leaves conn->reg correctly read-direction by the time it
     * resumes, rather than relying on one extra, wasted write-readiness
     * dispatch (TCP send buffers are almost always immediately writable,
     * so a stale write-direction resume would otherwise self-correct via a
     * near-immediate spurious re-dispatch, not hang; but only once
     * _conn_on_writable's own registration actually has an on_writable
     * handler at all; see the else branch below for the real hang this
     * pairs with). Not itself expected to fail (reg was already known-live
     * a moment ago), but checked anyway rather than assumed, matching this
     * function's own established discipline for every other event_loop
     * call. */
    if (event_loop_modify(srv_engine_bundler.reactor, conn->reg,
                          ccol_select_read) != ccol_success) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
    if (event_loop_resume(srv_engine_bundler.reactor, conn->reg) !=
        ccol_success) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
  } else {
    char *err = NULL;
    /* .on_writable is required here for the identical reason _conn_pump's
     * own two event_loop_add call sites already carry it: this connection
     * has no live registration yet (its very first request was resolved
     * entirely synchronously, via _listener_on_readable's own optimistic
     * first read/parse, with no reactor wait needed at all), but a LATER
     * keep-alive request on this same connection can still legitimately
     * need write-direction interest mid-read (ctls_conn_wants_write, see
     * that function's own doc comment). _conn_pump's read loop would then
     * correctly call event_loop_modify(conn->reg, ccol_select_write) on
     * this exact registration (succeeding, since modify only changes
     * epoll interest, not handlers) but with no on_writable handler ever
     * registered for it, a subsequent write-readiness event would be
     * silently dropped by event_loop's own "no handler pointer registered
     * for this direction" no-op behavior, permanently stalling the
     * connection until an unrelated timeout (possibly configured to 0,
     * i.e. disabled) eventually intervened. A real, reproducible gap found
     * via independent review after ctls_conn_wants_write was first added;
     * every registration this file ever creates for a live connection's fd
     * must carry both handlers, since any of them can later need either
     * direction. */
    conn->reg =
        event_loop_add(srv_engine_bundler.reactor,
                       selectable_from_fd(conn->fd, ccol_select_read),
                       (event_handlers_t){.on_readable = _conn_on_readable,
                                          .on_writable = _conn_on_writable,
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

/* Minimum gap, in seconds, between two consecutive accept-failure log lines
 * for the same server; see struct chttpserver's own last_accept_err_log
 * field comment for why this exists at all. */
#define _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC 5
/* A resource-exhaustion class accept4() failure (EMFILE/ENFILE/ENOBUFS/
 * ENOMEM) or a post-accept4() allocation failure (_conn_create's own
 * _mem_calloc, or ctls_conn_create_server's own internal allocation) pauses
 * srv's listener registration and returns immediately, rather than sleeping
 * on the calling thread before retrying (see _listener_pause_for_resource_
 * pressure and its two call sites below). A *persistent* condition (the
 * process or system genuinely out of file descriptors under sustained
 * connection-flood load with a modest ulimit -n; a custom, bounded
 * ccol_memmgmt_procs_t installed via create_chttpsvr_mp exhausting
 * independently of overall system memory) would otherwise have the
 * reactor's own level-triggered epoll immediately re-observe the still-
 * nonempty listen backlog and re-dispatch this same handler again and
 * again, with zero progress possible until something external frees up the
 * resource in question; a synchronous sleep in that dispatch does bound its
 * OWN CPU cost, but still blocks the one process-wide shared reactor
 * thread (see chttpsvr_set_engine_num_reactor_threads's own default) for
 * that entire sleep, on every single re-dispatch, for as long as the
 * condition persists, starving every OTHER connection on every OTHER
 * server sharing that one reactor thread in the meantime; the identical
 * shape the max_connections capacity-pause case a few lines up in the same
 * loop already avoids by pausing rather than blocking. Resumed by the
 * idle-timeout sweep thread's own unconditional per-server check (see
 * _listener_resume_if_resource_pressure_cleared) within at most
 * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS; an acceptable trade for the same
 * documented reason max_connections's own resume latency already is
 * (chttpsvr_config_t's own doc comments never promise sub-second recovery
 * from a resource-exhaustion condition either), and vastly better than
 * blocking the shared reactor thread for as long as the condition lasts. */

/* True for an accept4() failure that reflects a problem with one specific
 * already-pending connection (already reported into the listen socket's own
 * error state before this call ever ran), not with the listener itself:
 * retrying immediately, to let the next pending connection in the backlog
 * be tried right away instead of waiting for a fresh epoll dispatch, is
 * both safe and the conventional handling for exactly this class (see e.g.
 * the accept(2) man page's own BUGS section, which documents Linux passing
 * a pending per-connection network error through accept() this way and
 * recommends treating it as transient). ECONNABORTED is the one such error
 * POSIX itself documents; EPROTO/ENETDOWN/ENETUNREACH/ENOPROTOOPT/
 * EHOSTDOWN/ENONET/EHOSTUNREACH/EOPNOTSUPP are the exact Linux-specific
 * pending-network-error codes that page's own ERRORS section names,
 * verbatim, for accept(2) ("In the case of TCP/IP, these are ENETDOWN,
 * EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH, EOPNOTSUPP, and
 * ENETUNREACH"), and EPERM ("Firewall rules forbid connection", per that
 * same page's own ERRORS section) is the identical per-connection-
 * rejection shape as ECONNABORTED, just sourced from a firewall rule
 * rather than the peer itself; a realistic, non-adversarial condition
 * for any server sitting behind connection-rate-limiting/fail2ban-style
 * REJECT (not DROP) rules. ETIMEDOUT/ENOSR/ESOCKTNOSUPPORT/EPROTONOSUPPORT
 * round out the same man page's own "in addition, network errors for the
 * new socket... may be returned; various Linux kernels can return other
 * errors such as..." sentence, immediately following its own EPERM/EPROTO
 * entries; the identical per-pending-connection category, not a
 * listener-level problem. */
static bool _accept_errno_is_transient(int e) {
  switch (e) {
    case ECONNABORTED:
    case EPROTO:
    case ENETDOWN:
    case ENETUNREACH:
    case ENOPROTOOPT:
    case EHOSTDOWN:
    case ENONET:
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case EPERM:
    case ETIMEDOUT:
    case ENOSR:
    case ESOCKTNOSUPPORT:
    case EPROTONOSUPPORT:
      return true;
    default:
      return false;
  }
}

/* True for an accept4() failure reflecting exhaustion of a shared,
 * process- or system-wide resource (as opposed to a problem with one
 * specific pending connection): a fresh retry right away is likely to fail
 * identically until something ELSE in the process (or system) frees up the
 * resource in question. Named and tested as its own pure classifier (used
 * directly by this file's own white-box test accessor and the tests that
 * exercise it) purely for documentation/diagnostic value; it does NOT gate
 * _listener_on_readable_impl's own decision to call _listener_pause_for_
 * resource_pressure, which pauses unconditionally for every non-transient
 * errno reaching that point (see that call site's own comment for why an
 * explicit allow-list there would leave an unclassified-but-persistent
 * errno unprotected against the exact busy-loop this mechanism exists to
 * prevent). */
static bool _accept_errno_is_resource_exhaustion(int e) {
  switch (e) {
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
      return true;
    default:
      return false;
  }
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hooks exposing the two pure errno-classification helpers
 * above directly: genuinely triggering EMFILE/ECONNABORTED/etc. from a test
 * would require manipulating the whole process's fd limits or kernel-level
 * connection state, environment-dependent and disproportionate for testing
 * what is, underneath, simple, deterministic integer classification.
 * Gated so neither symbol exists in a production build, matching every
 * other white-box helper in this file. */
bool _chttpsvr_accept_errno_is_transient_for_tests(int e) {
  return _accept_errno_is_transient(e);
}
bool _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(int e) {
  return _accept_errno_is_resource_exhaustion(e);
}
#endif /* RUNNING_UNIT_TESTS */

/* Returns true (and marks this instant as the new "last logged" one) at
 * most once every _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC seconds per server;
 * false otherwise. Shared rate-limit gate for every diagnostic this accept
 * loop logs about a persistent, not-immediately-actionable listener-level
 * condition (an unexpected accept4() failure, or an allocation failure
 * for a connection/TLS object discovered right after a successful
 * accept4()) since both are the identical "this could keep re-triggering
 * on every single accept4() attempt for as long as the underlying condition
 * persists" log-flooding hazard. See struct chttpserver's own
 * last_accept_err_log field comment for why the two fields this reads/
 * writes need no synchronization of their own. */
static bool _listener_accept_err_log_gate(struct chttpserver *srv) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (srv->last_accept_err_log_set &&
      now.tv_sec - srv->last_accept_err_log.tv_sec <
          _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC)
    return false;
  srv->last_accept_err_log = now;
  srv->last_accept_err_log_set = true;
  return true;
}

/* Logs an unexpected accept4() failure via the engine logger, rate-limited
 * by _listener_accept_err_log_gate. Notes whether _accept_errno_is_resource_
 * exhaustion recognizes err as one of the well-documented EMFILE/ENFILE/
 * ENOBUFS/ENOMEM cases: this is purely a diagnostic aid for whoever reads
 * the log (an operator can act on "resource exhaustion" by raising ulimits/
 * freeing memory; anything else here reflects a listener-level condition
 * this loop does not expect to see in practice at all, worth investigating
 * as a potential bug rather than routine overload) and has no bearing on
 * this loop's own handling, which (see the one call site below) pauses
 * unconditionally for every such errno regardless of this classification. */
static void _listener_log_accept_err_rate_limited(struct chttpserver *srv,
                                                  int err) {
  if (!_listener_accept_err_log_gate(srv)) return;
  /* strerror_r, not strerror: this file's own reactor is genuinely
   * multi-threaded (see chttpsvr_set_engine_num_reactor_threads), and
   * plain strerror() is not required to be thread-safe (POSIX permits an
   * implementation to return a pointer into a shared static buffer another
   * thread's own concurrent strerror() call could overwrite mid-use); two
   * different servers' listeners can dispatch this exact call on two
   * different reactor threads at the same moment. _GNU_SOURCE is already
   * defined at the top of this file, so this resolves to glibc's own
   * GNU-specific strerror_r, whose return value (not necessarily buf) is
   * always safe to use directly regardless of whether it filled buf. */
  char errbuf[128];
  _SRV_ENGINE_LOG(log_warn, "accept() failed errno=%d (%s)%s", err,
                  strerror_r(err, errbuf, sizeof(errbuf)),
                  _accept_errno_is_resource_exhaustion(err)
                      ? " [resource exhaustion]"
                      : "");
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test instrumentation only: counts how many times this accept
 * loop has paused srv's listener after a post-accept4() allocation failure
 * (_conn_create or ctls_conn_create_server returning NULL), process-wide. A
 * test reads the delta across its own window (while forcing every such
 * allocation to fail via a custom ccol_memmgmt_procs_t) to confirm the pause
 * path is genuinely exercised, the same style g_listener_dispatch_count_
 * for_tests already established for the max_connections busy-loop fix.
 * Gated so this symbol does not exist in a production build. */
static _Atomic size_t g_listener_alloc_failure_pause_count_for_tests = 0;
size_t _chttpsvr_listener_alloc_failure_pause_count_for_tests(void) {
  return atomic_load(&g_listener_alloc_failure_pause_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Pauses srv's listener registration in response to an accept4() resource-
 * exhaustion errno or a post-accept4() allocation failure, and marks
 * listener_paused_for_resource_pressure (see that field's own comment) so
 * the idle-timeout sweep thread's own _listener_resume_if_resource_
 * pressure_cleared knows to retry it later. A no-op if srv currently has no
 * live listener registration to pause (lreg NULL) or if event_loop_pause
 * itself fails (per its own documented contract, this can only mean lreg
 * was concurrently removed, e.g. by a racing chttpsvr_stop(); listener_
 * dispatch_pins is specifically designed to prevent that for as long as
 * this dispatch is running, so not expected to be reachable in practice,
 * but if it somehow were, there would be no genuinely paused registration
 * to track anyway). The flag is set only once the pause has actually
 * succeeded, never speculatively before attempting it, so the sweep never
 * wastes a resume call on a registration that in fact never got paused. */
static void _listener_pause_for_resource_pressure(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  event_reg lreg = srv->listen_reg;
  mutex_unlock(srv->mutex);
  if (!lreg) return;
  if (event_loop_pause(srv_engine_bundler.reactor, lreg) == ccol_success)
    atomic_store(&srv->listener_paused_for_resource_pressure, true);
}

/* Logs (rate-limited, sharing the same gate/fields as the accept4()-failure
 * logger above) an allocation failure discovered right after a successful
 * accept4() (_conn_create's own _mem_calloc, or ctls_conn_create_server's
 * own internal allocation), then pauses srv's listener exactly as
 * _listener_pause_for_resource_pressure describes, rather than retrying
 * accept4() again immediately: mirrors the identical resource-exhaustion
 * handling this same loop already applies to accept4() itself (see this
 * file's own "LISTENER: ACCEPT + SOCKET OPTIONS" section comment above).
 * Without this, a sustained allocation-failure condition (most plausibly a
 * custom, bounded ccol_memmgmt_procs_t installed via create_chttpsvr_mp,
 * which can exhaust independently of overall system memory; less commonly
 * genuine system-wide OOM) combined with connections continuing to arrive
 * in the listen backlog would have this loop spin accept()-then-
 * immediately-fail-to-allocate-then-close indefinitely, re-dispatched on
 * every single epoll_wait with zero progress; the identical unbounded-
 * busy-loop-under-a-persistent-condition shape this file already treats as
 * a real bug for accept4()'s own EMFILE/ENFILE/ENOBUFS/ENOMEM handling a
 * few lines down in this same loop. */
static void _listener_log_and_pause_for_alloc_failure(struct chttpserver *srv,
                                                      const char *what) {
  if (_listener_accept_err_log_gate(srv))
    _SRV_ENGINE_LOG(log_warn, "%s failed after accept()", what);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_alloc_failure_pause_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  _listener_pause_for_resource_pressure(srv);
}

static void _apply_accepted_socket_options(int fd, bool is_unix,
                                           bool enable_keepalive) {
  /* Non-blocking mode is already guaranteed atomically by accept4()'s own
   * SOCK_NONBLOCK flag at the one call site that produces fd (see
   * _listener_on_readable); no separate fcntl(F_SETFL) call is needed (or
   * wanted) here. */
  int one = 1;
  /* TCP_NODELAY (an IPPROTO_TCP-level option) and SO_KEEPALIVE's usual
   * purpose genuinely don't apply to AF_UNIX, so only these two are guarded
   * by is_unix; the SO_SNDBUF/SO_RCVBUF bump just below is an ordinary
   * SOL_SOCKET-level option that is meaningful and effective on AF_UNIX
   * stream sockets too, so it deliberately applies unconditionally to both:
   * unix:// is a documented, first-class listener mode (its own config
   * knob and test suite), and on a platform/container whose default
   * unix-socket buffer sizing is smaller than this bump (unlike stock
   * Linux, where it commonly already exceeds it), skipping this for a
   * unix-socket connection would leave it with meaningfully smaller
   * effective buffers than an equivalent TCP client on the same server for
   * no deliberate reason. */
  if (!is_unix) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (enable_keepalive)
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
  }
  int bufsz = 131072;
  int cur = 0;
  socklen_t cur_len = sizeof(cur);
  if (getsockopt(fd, SOL_SOCKET, SO_SNDBUF, &cur, &cur_len) != 0 || cur < bufsz)
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &bufsz, sizeof(bufsz));
  cur_len = sizeof(cur);
  if (getsockopt(fd, SOL_SOCKET, SO_RCVBUF, &cur, &cur_len) != 0 || cur < bufsz)
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &bufsz, sizeof(bufsz));
}

#ifdef RUNNING_UNIT_TESTS
/* Forward declaration: defined much later in this file (with the rest of
 * the chttpsvr handle slot table machinery), needed here early so the new
 * server-scoped accept-errno force hook below can resolve a chttpsvr handle
 * to its raw struct chttpserver* for comparison, without moving that whole
 * slot-table section up. */
struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h);

/* White-box test instrumentation only: counts how many times
 * _listener_on_readable has been dispatched at all, process-wide, since the
 * counter's own zero-initialization. A test reads the delta across its own
 * window (while deliberately holding a server at max_connections capacity,
 * with nothing else in the process actively connecting/closing) to
 * distinguish a correctly paused listener (delta stays at most a small,
 * fixed handful of dispatches) from a busy-looping one (delta grows into
 * the thousands within a fraction of a second, since level-triggered epoll
 * re-reports a listener whose accept backlog is still non-empty on every
 * single epoll_wait call); a far more precise and less environment-
 * sensitive signal than measuring wall-clock CPU time directly. Gated so
 * this symbol/counter does not exist at all in a production build, matching
 * every other white-box helper in this file. */
static _Atomic size_t g_listener_dispatch_count_for_tests = 0;
size_t _chttpsvr_listener_dispatch_count_for_tests(void) {
  return atomic_load(&g_listener_dispatch_count_for_tests);
}

/* White-box test instrumentation only: when g_force_next_accept_errno_for_
 * tests_srv matches the specific server about to dispatch (compared by raw
 * pointer, resolved once at arm time via _chttpsvr_resolve_for_tests), the
 * very next accept4() call in _listener_on_readable_impl's loop below for
 * THAT server has its result discarded (a real, successful accept4()
 * connection is simply closed unused, exactly as if it had never been
 * accepted) and replaced with a simulated failure whose errno is the
 * paired _val field, then both are reset so only that one call, for that
 * one server, is ever affected. Deliberately scoped to one specific server
 * rather than "the very next accept4() dispatch anywhere in the process":
 * this is a shared, process-wide reactor, so an unrelated dispatch for a
 * completely different, concurrently-running chttpsvr (e.g. this test
 * binary's own long-lived shared fixture server servicing an unrelated
 * connection at the same moment) could otherwise consume the forced errno
 * before the test's own intended connection ever reached it - confirmed to
 * happen in practice under valgrind's own scheduling, an intermittent,
 * environment-timing-dependent false pass/fail unrelated to the fix any
 * such test is actually about.
 *
 * Exists because a REAL accept4() failure with an errno outside
 * _accept_errno_is_transient/_accept_errno_is_resource_exhaustion's own
 * named cases (EBADF/EINVAL/ENOTSOCK/EFAULT and similar) would require
 * manipulating the whole process's fd table or kernel-level socket state to
 * trigger genuinely - environment-dependent and disproportionate, the same
 * reasoning already given for not testing EMFILE/ECONNABORTED/etc. this way
 * (see _chttpsvr_accept_errno_is_transient_for_tests's own comment). This
 * hook lets a test instead directly verify the *loop's* own handling of
 * such an errno (pauses rather than busy-loops, later resumes) without
 * needing the errno to be real. Gated so neither this symbol nor the
 * setter exists in a production build. */
static _Atomic(struct chttpserver *) g_force_next_accept_errno_for_tests_srv =
    NULL;
static _Atomic int g_force_next_accept_errno_for_tests_val = 0;
void _chttpsvr_force_next_accept_errno_for_tests(chttpsvr h, int errno_val) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  atomic_store(&g_force_next_accept_errno_for_tests_val, errno_val);
  atomic_store(&g_force_next_accept_errno_for_tests_srv, raw);
}
#endif /* RUNNING_UNIT_TESTS */

/* The real accept-loop body, pinned by the thin wrapper below (see
 * listener_dispatch_pins's own field comment) for its entire duration
 * before srv is touched at all. Split out rather than wrapping the
 * existing body in a pin/unpin pair inline: this function has several
 * early-return points (capacity pause, EWOULDBLOCK, an unexpected
 * accept4() failure, ...), and funneling every one of them back through a
 * single unpin site via a thin caller is simpler and less error-prone than
 * threading a matching unpin call into each one individually. */
static void _listener_on_readable_impl(struct chttpserver *srv) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_dispatch_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */

  for (;;) {
    size_t cap = atomic_load(&srv->max_connections);
    if (cap && atomic_load(&srv->current_connections) >= cap) {
      /* At capacity: pause the listener's own reactor registration rather
       * than merely returning without calling accept4() again. Level-
       * triggered epoll means a listen socket with a non-empty accept
       * backlog stays reported-ready on every single epoll_wait call, not
       * just once; returning here with no further action would have the
       * reactor immediately re-dispatch this exact handler again, and
       * again, with zero progress possible until a slot frees. Confirmed
       * (not assumed) via a standalone /proc/<pid>/stat CPU-tick
       * measurement: an unpatched listener left at capacity pins a full CPU
       * core at ~100% for as long as the server stays there, purely
       * re-dispatching this handler to do nothing over and over; a real,
       * severe resource-waste bug regardless of whether any other
       * connection's own events happen to still be serviced in the same
       * epoll_wait batch (they generally are, since epoll_wait naturally
       * returns every currently-ready fd together, not just the listener;
       * this pause exists to eliminate the wasted CPU, not because other
       * connections were found to be starved). Resumed by the idle-timeout
       * sweep thread's own unconditional per-server check (see
       * _listener_resume_if_capacity_freed) within at most
       * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS of a slot freeing up. */
      mutex_lock(srv->mutex);
      event_reg lreg = srv->listen_reg;
      mutex_unlock(srv->mutex);
      if (lreg &&
          event_loop_pause(srv_engine_bundler.reactor, lreg) == ccol_success) {
        /* Re-check after pausing, not merely before: closes a real TOCTOU
         * race against the sweep thread's own resume check racing this
         * exact pause. If capacity had already freed and a sweep tick's own
         * _listener_resume_if_capacity_freed call ran concurrently with
         * (and just ahead of) the pause above, its resume would have found
         * nothing paused yet to resume; a harmless no-op at the time, but
         * with nothing else left to ever un-pause this listener until the
         * NEXT sweep tick, up to a further _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS
         * away. Rechecking here, strictly after the pause has taken effect,
         * and resuming immediately if capacity has since freed, closes that
         * gap down to zero rather than merely bounding it by one more tick.
         *
         * The pause's own return value is checked (unlike a bare fire-and-
         * forget call) for consistency with every other event_loop_pause/
         * _modify/_add/_resume call site in this file, all of which check
         * it: a non-success return here can only mean lreg was concurrently
         * removed (its only documented failure mode for a non-NULL reg),
         * which listener_dispatch_pins is specifically designed to prevent
         * for as long as this dispatch is running; not expected to be
         * reachable in practice, but if it ever were, there would be no
         * genuinely paused registration left to resume, so skipping the
         * recheck/resume entirely (rather than calling event_loop_resume on
         * a reg that was never actually paused) is the correct response. */
        if (atomic_load(&srv->current_connections) < cap)
          event_loop_resume(srv_engine_bundler.reactor, lreg);
      }
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    /* accept4() with SOCK_NONBLOCK, not accept() + a separate fcntl(F_SETFL)
     * call: the fd this produces is handed straight to the sole reactor
     * thread's own read()/ctls_conn_read() calls (see _conn_pump), which
     * must never block. accept4() makes non-blocking mode part of the same
     * atomic kernel operation that creates the fd, rather than depending on
     * a second, separate, unchecked fcntl() call succeeding first.
     *
     * SOCK_CLOEXEC alongside it for the identical atomicity reason, on the
     * other axis: without it, every accepted connection's fd (a live,
     * currently-open client socket) is inherited across any fork()+exec()
     * the embedding application performs elsewhere in this process while
     * the server is running (a request handler shelling out, an unrelated
     * subprocess spawn), leaking it into a child process that has no
     * business holding it open. Matches this codebase's own established
     * convention for every other fd it creates (clogger.c's O_CLOEXEC file
     * opens, cthreadcomm.c's EFD_CLOEXEC eventfd()s). */
    int cfd = accept4(atomic_load(&srv->listen_fd), (struct sockaddr *)&ss,
                      &slen, SOCK_NONBLOCK | SOCK_CLOEXEC);
    int accept_errno = cfd < 0 ? errno : 0;
#ifdef RUNNING_UNIT_TESTS
    /* See g_force_next_accept_errno_for_tests_srv's own comment: only
     * consumed (both fields reset) when this dispatch's own srv is exactly
     * the one a test armed, so an unrelated server's own concurrent
     * dispatch (e.g. this test binary's long-lived shared fixture server)
     * can never steal a forced errno meant for a different, specific
     * server under test. */
    if (atomic_load(&g_force_next_accept_errno_for_tests_srv) == srv) {
      int forced_errno =
          atomic_exchange(&g_force_next_accept_errno_for_tests_val, 0);
      atomic_store(&g_force_next_accept_errno_for_tests_srv, NULL);
      if (forced_errno) {
        if (cfd >= 0) close(cfd);
        cfd = -1;
        accept_errno = forced_errno;
      }
    }
#endif /* RUNNING_UNIT_TESTS */
    if (cfd < 0) {
      if (accept_errno == EWOULDBLOCK || accept_errno == EAGAIN) return;
      if (accept_errno == EINTR) continue;
      /* A problem with one specific already-pending connection, not the
       * listener itself: retry immediately so the next backlog entry (if
       * any) is tried right away, rather than waiting for a fresh epoll
       * dispatch to notice the backlog is still non-empty. */
      if (_accept_errno_is_transient(accept_errno)) continue;
      _listener_log_accept_err_rate_limited(srv, accept_errno);
      /* See this file's own "LISTENER: ACCEPT + SOCKET OPTIONS" section
       * comment above for why this pauses rather than sleeps: without it, a
       * persistent exhaustion condition busy-loops the sole shared reactor
       * thread, since the listen backlog stays non-empty (nothing was ever
       * actually accepted) and so the reactor's own level-triggered epoll
       * immediately re-dispatches this exact handler again with no
       * progress made. Deliberately NOT gated on _accept_errno_is_resource_
       * exhaustion specifically: that helper's own switch only names the
       * well-documented EMFILE/ENFILE/ENOBUFS/ENOMEM cases, but any other
       * errno reaching this point (accept(2) documents several more:
       * EBADF/EINVAL/ENOTSOCK/EFAULT among them, none of which this loop
       * expects to see in practice against a listener it fully owns and
       * manages itself) is, by construction, neither EWOULDBLOCK/EAGAIN/
       * EINTR nor one of _accept_errno_is_transient's own per-connection
       * cases, so a persistent occurrence of ANY such errno busy-loops this
       * thread exactly the same way an unhandled resource-exhaustion errno
       * would; there is no errno value for which returning here without
       * pausing is actually the desired outcome. Pausing has no downside
       * even for a one-off, self-clearing occurrence (the very next sweep
       * tick resumes the listener regardless), so this pauses
       * unconditionally for every errno that reaches this point rather than
       * matching an explicit allow-list. */
      _listener_pause_for_resource_pressure(srv);
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
      /* See _listener_log_and_pause_for_alloc_failure's own comment: without
       * this pause, a sustained allocation-failure condition combined with
       * connections continuing to arrive would spin this loop with zero
       * progress, the identical hazard already fixed for accept4() itself a
       * few lines up. */
      _listener_log_and_pause_for_alloc_failure(srv, "connection allocation");
      return;
    }

    if (srv->tls_ctx) {
      conn->tls = ctls_conn_create_server(srv->tls_ctx, cfd, conn, NULL);
      if (!conn->tls) {
        /* Routed through the shared _conn_free helper (close(fd), decrement
         * current_connections, free every conn-owned allocation) rather than
         * a manually re-derived, narrower equivalent: conn is still fully
         * safe to hand to it here (never added to the idle/diverted lists,
         * conn->tls itself still NULL since creation is what just failed),
         * and this way a future change to _conn_create/_conn_reset_for_
         * request that starts allocating something unconditionally is
         * covered automatically instead of silently leaking through this
         * one narrow, easy-to-miss early-failure path. */
        _conn_free(conn);
        /* See _listener_log_and_pause_for_alloc_failure's own comment; same
         * reasoning as the _conn_create failure above, for TLS connection
         * object construction specifically. */
        _listener_log_and_pause_for_alloc_failure(srv, "TLS connection setup");
        return;
      }
      conn->state = CONN_ST_TLS_HANDSHAKE;
    } else {
      conn->state = CONN_ST_READING_HEADERS;
    }
    _conn_pump(conn);
  }
}

/* Registered with event_loop_add as the listener's own on_readable
 * callback. Pins srv (via listener_dispatch_pins; see that field's own
 * comment) for the entire duration of _listener_on_readable_impl's call,
 * including any listener pause it performs, before touching srv at
 * all; and, symmetrically, releases the pin as the very last thing this
 * function does, after the impl call has fully returned. Taken under
 * srv->mutex so the increment is properly serialized against __chttpsvr_
 * destroy's own mutex-protected wait for this same counter, rather than a
 * bare atomic op racing that check the way pending_resolve_count's own
 * lock-free increment safely does elsewhere (safe there only because a
 * DIFFERENT lock, the slot table's, already excludes the corresponding
 * free; no such lock protects a bare event_loop callback arg here). */
static void _listener_on_readable(event_loop loop, ccol_selectable *sel,
                                  void *arg) {
  (void)loop;
  (void)sel;
  struct chttpserver *srv = (struct chttpserver *)arg;

  mutex_lock(srv->mutex);
  atomic_fetch_add(&srv->listener_dispatch_pins, 1);
  mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _listener_dispatch_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  _listener_on_readable_impl(srv);

  mutex_lock(srv->mutex);
  atomic_fetch_sub(&srv->listener_dispatch_pins, 1);
  cond_var_broadcast(srv->resolve_cv);
  mutex_unlock(srv->mutex);
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
    /* SOCK_CLOEXEC so the listening socket itself is not inherited across a
     * fork()+exec() performed elsewhere in the embedding process while this
     * server is running; see the identical reasoning at the accept4() call
     * site in _listener_on_readable. */
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
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
    /* SOCK_CLOEXEC alongside SOCK_NONBLOCK for the identical reason as the
     * AF_UNIX branch above and the accept4() call in _listener_on_readable:
     * without it this listening socket leaks into any child process a
     * fork()+exec() elsewhere in this application produces. */
    fd = socket(rp->ai_family, rp->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC,
                rp->ai_protocol);
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
 * m_procs/the routes and mw_head/mw_tail/mw_count bookkeeping fields;
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
 * a single byte of any router's contents, guarantees that); but nothing
 * protects the read of router->owner itself, since that happens before any
 * pin exists. Confirmed as a real, valgrind-caught use-after-free by two
 * earlier fix attempts, neither of which touched the shell's own lifetime:
 * resolving router->owner alone, and then a process-wide rwlock taken around
 * _destroy_router's free (which does nothing for a reader that starts only
 * after that free has already completed (not merely started, completed)
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
  if (mutex_init(chttpsvr_router_shell_registry.mutex) != 0)
    fatal_err("chttpsvr router shell registry: failed to initialize mutex");
}

static void _chttpsvr_router_shell_register(chttpsvr_router *r) {
  call_once(chttpsvr_router_shell_registry.once,
            _chttpsvr_router_shell_registry_init_globals);
  mutex_lock(chttpsvr_router_shell_registry.mutex);
  if (chttpsvr_router_shell_registry.count ==
      chttpsvr_router_shell_registry.capacity) {
    /* Overflow-safe growth, matching every other growable array in this file
     * (_router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_header,
     * _parse_qparams, _servers_register): a plain "capacity * 2" can wrap
     * on a pathological capacity, silently under-allocating
     * new_cap * sizeof(ptr) below. A failed/skipped growth here degrades
     * exactly like a real OOM already does a few lines down (this one shell
     * simply isn't tracked for the process-exit free), never a correctness
     * issue. */
    size_t new_cap = _doubling_growth_cap(
        chttpsvr_router_shell_registry.capacity, sizeof(chttpsvr_router *), 8);
    if (new_cap > 0) {
      chttpsvr_router **nn =
          (chttpsvr_router **)realloc(chttpsvr_router_shell_registry.shells,
                                      new_cap * sizeof(chttpsvr_router *));
      if (nn) {
        chttpsvr_router_shell_registry.shells = nn;
        chttpsvr_router_shell_registry.capacity = new_cap;
      }
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
 * Mirrors _cleanup_chttpsvr_slot_table's own call_once-guarded shape,
 * rationale, and in_use/owner liveness check exactly (a process that links
 * this library but never creates a single chttpsvr must not lock a
 * never-initialised mutex here).
 *
 * A shell is only ever actually freed here if router->owner is
 * CHTTPSVR_INVALID, i.e. _destroy_router has already genuinely run for it
 * (a real, completed chttpsvr_destroy() call), or it belongs to a router
 * whose registration failed partway through (chttpsvr_subrouter's own
 * exit-2 path). A shell whose owner is still a live, resolvable handle
 * belongs to a chttpsvr the application never destroyed before process
 * exit; freeing it here regardless would be a use-after-free the instant
 * that server's own reactor/worker threads (which __attribute__((destructor))
 * teardown does not stop or join, unlike an explicit chttpsvr_destroy()
 * call) next dispatch a request through _find_route(), which dereferences
 * this exact struct. Left unfreed here, it is the same, already-accepted
 * kind of "still reachable" leak chttpsvr_slot_table's own sibling
 * destructor already tolerates for the identical reason: an application
 * that lets a chttpsvr outlive process exit without destroying it first was
 * never going to get a clean valgrind report for that handle's own memory
 * either way, so the die-with-the-process leak is preferable to a crash. */
__attribute__((destructor)) static void _cleanup_chttpsvr_router_shells(void) {
  call_once(chttpsvr_router_shell_registry.once,
            _chttpsvr_router_shell_registry_init_globals);
  mutex_lock(chttpsvr_router_shell_registry.mutex);
  for (size_t i = 0; i < chttpsvr_router_shell_registry.count; i++) {
    chttpsvr_router *r = chttpsvr_router_shell_registry.shells[i];
    if (atomic_load(&r->owner) == CHTTPSVR_INVALID) mem_free(r);
  }
  free(chttpsvr_router_shell_registry.shells);
  chttpsvr_router_shell_registry.shells = NULL;
  chttpsvr_router_shell_registry.count = 0;
  chttpsvr_router_shell_registry.capacity = 0;
  mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* ========================================================================== */
/*                    FORK SAFETY (pthread_atfork)                            */
/* ========================================================================== */

#if FORK_SAFETY_REQUIRED
/* fork() duplicates only the calling thread; any lock some OTHER thread held
 * at that instant is inherited by the child in a permanently locked state,
 * since no thread survives in the child that could ever unlock it. Every
 * process-wide registry this module owns (chttpsvr_slot_table; srv_engine_
 * bundler and servers_bundler, which share one lazy-init guard, see
 * _engine_globals_init; and chttpsvr_router_shell_registry), plus every
 * currently-live server's own mutex/idle_mutex/diverted_mutex/routes_lock,
 * are therefore all taken here, in prepare(), before fork() is allowed to
 * proceed, and released again in both parent() and child() via the same
 * shared function: every mutex in this module uses the default ("normal")
 * pthread mutex type, which does no owner/TID tracking on Linux glibc, so a
 * plain pthread_mutex_unlock is well-defined even when called by a thread
 * other than whichever one originally locked it (which, for anything the
 * forking thread itself did not hold, no longer exists in the child at
 * all). routes_lock (a pthread_rwlock_t, not a plain mutex) is taken for
 * write here specifically so a concurrent reader is excluded too, not just
 * a writer; see _chttpsvr_atfork_release_impl's own doc comment for why it
 * needs different treatment than the plain mutexes in the child specifically
 * (glibc's rwlock write-lock, unlike this module's plain mutexes, DOES track
 * ownership by TID, so a plain unlock from the child's own, differently-
 * numbered thread silently fails to release it).
 *
 * Mirrors cthreadpool.c's, cthreadcomm.c's, and clogger.c's own identical
 * atfork fixes, for the identical reason: no other module can reach into
 * this one's own opaque globals to protect them from the outside, so each
 * module that owns process-wide, arbitrarily-lockable state must register
 * its own handlers. This module's own worker_pool/reject_pool (ctpool
 * handles), shared reactor (an event_loop handle), and every clog handle it
 * opens or derives (the engine-wide diagnostics logger, and each server's
 * own per-instance logger) are already protected by cthreadpool.c's,
 * cthreadcomm.c's, and clogger.c's own atfork registrations respectively and
 * need no duplicate protection here.
 *
 * Every registry's own lazy call_once guard is run here first (mirroring
 * every ordinary lock site elsewhere in this file, e.g.
 * _engine_wait_until_stopped), so a fork() landing before this module's
 * engine/router machinery has ever been touched still safely initializes
 * (rather than locking uninitialized memory) before proceeding, the same
 * pattern glibc's own malloc arena locks rely on being safe to call from
 * within an atfork handler.
 *
 * Only ever walks slots with in_use == true, mirroring the exact condition
 * every resolve function already trusts as the sole indicator that
 * slot->ptr is safe to dereference; create_chttpsvr_mp's own construction
 * order guarantees every one of a server's locks is already fully
 * initialized before its slot is ever marked in_use.
 *
 * is_child also resets srv_engine_bundler.reaper_joinable, srv_engine_
 * bundler.stopping, and idle_sweep_bundler.running to false (see
 * _chttpsvr_atfork_release_impl's own doc comment for the full reasoning):
 * if either of this module's own two background threads was still running
 * (or about to be spawned) at the instant of fork(), the child would
 * otherwise inherit a stale thread identifier referring to a thread that
 * does not exist in this process, and a later thread_join() on it (from
 * _join_reaper_if_needed_locked()/_idle_sweep_stop_if_running(), both
 * reachable from an ordinary chttpsvr_engine_wait()/chttpsvr_engine_stop()
 * call in the child) has no well-defined outcome under POSIX; confirmed
 * directly, not merely predicted, to hang in practice. A stuck-true stopping
 * flag compounds this: it permanently blocks every later chttpsvr_engine_
 * stop() call in the child from spawning a genuine reaper of its own, and
 * every later chttpsvr_start()/_engine_acquire() call from ever proceeding
 * past its own wait for a broadcast that stale flag's own now-vanished
 * setter would eventually have sent. None of these three resets attempts to
 * actually reclaim or join a vanished thread; each simply says there is
 * nothing left to wait for, mirroring g_engine_stop_watcher.started's own
 * identical treatment just below, and ctpool's/event_loop's own foreign_
 * since_fork machinery for their own worker threads. */
static void _chttpsvr_atfork_prepare(void) {
  call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  mutex_lock(chttpsvr_slot_table.mutex);

  call_once(srv_engine_bundler.once, _engine_globals_init);
  /* servers_bundler.mutex locked BEFORE srv_engine_bundler.mutex: no
   * ordinary code path currently holds both at once (_idle_sweep_fn's own
   * per-tick snapshot releases servers_bundler.mutex before its later,
   * separate _SRV_ENGINE_LOG call ever touches srv_engine_bundler.mutex; see
   * that function's own comment), so either relative order would be equally
   * safe against today's code. A fixed order is still picked deliberately,
   * rather than left to whichever order this function happens to be written
   * in: a nesting relationship between these two locks has existed here
   * before (an earlier version of _idle_sweep_fn held servers_bundler.mutex
   * across its own diagnostics call, which made the REVERSE of this order a
   * genuine, TSan-confirmed AB-BA deadlock risk against this exact handler),
   * and picking one canonical order now means a future change that
   * reintroduces such nesting anywhere in this file only has one order to
   * match, rather than needing to rediscover which order is safe from
   * scratch. */
  mutex_lock(servers_bundler.mutex);
  mutex_lock(srv_engine_bundler.mutex);

  call_once(chttpsvr_router_shell_registry.once,
            _chttpsvr_router_shell_registry_init_globals);
  mutex_lock(chttpsvr_router_shell_registry.mutex);

  size_t n = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    mutex_lock(slot->ptr->mutex);
    mutex_lock(slot->ptr->idle_mutex);
    mutex_lock(slot->ptr->diverted_mutex);
    rw_lock_wrlock(slot->ptr->routes_lock);
  }
}

/* Shared by both parent() and child(); see _chttpsvr_atfork_prepare's own
 * doc comment for why a plain unlock is correct, in both branches, for
 * this module's plain mutexes. Safe to re-walk the identical structure
 * prepare() just walked and release every lock symmetrically: nothing could
 * have mutated the slot table or any live server's own state in between,
 * since every lock that would be needed to do so is still held at this
 * exact point.
 *
 * routes_lock (a pthread_rwlock_t) needs different treatment in the child
 * specifically: a real, empirically-confirmed hazard, identical to the one
 * already found and fixed for clog_slot_table.rwlock in clogger.c (see that
 * module's own doc comment for the full account, including a gdb-verified
 * reproduction of the exact hang this avoids). A plain pthread_rwlock_
 * unlock() on the write lock, called from the child's own sole thread, is
 * NOT a valid release of a lock the parent's forking thread acquired in
 * _chttpsvr_atfork_prepare(): glibc's rwlock write-lock tracks ownership by
 * TID internally, and the child's post-fork thread has a different TID than
 * the parent's forking thread did, so the unlock silently fails to release
 * it, permanently hanging every subsequent chttpsvr_use()/_register_
 * handler()/... call (anything reaching _router_add_route/_router_add_mw)
 * against that server in the child. Reproduced directly: a first version of
 * this fix used a plain rw_lock_unlock() in both branches and hung
 * deterministically, every trial, in tests/chttpserver's own fork_does_not_
 * inherit_a_locked_mutex stress test. The standard, well-established fix
 * (used by e.g. glibc's own malloc arena locks for this exact scenario, and
 * already established in this codebase by clogger.c) is to re-initialize
 * the lock in the child instead of unlocking it, safe specifically
 * because the child has exactly one thread and no one else can possibly be
 * waiting on it, so there is no other party for a fresh init to race.
 * Plain (default-type) mutexes do not exhibit this (no TID tracking for the
 * fast/normal mutex type this codebase uses throughout, per common.h's own
 * mutex_init()), so only routes_lock needs this fix, not mutex/idle_mutex/
 * diverted_mutex/any of the four process-wide registry mutexes.
 *
 * is_child additionally resets every still-live server's own pending_
 * resolve_count/servers_bundler_pins/listener_dispatch_pins to 0: each is a
 * "wait for this to reach 0, with no timeout" counter __chttpsvr_destroy
 * blocks on before ever freeing that server, and a now-vanished parent-side
 * thread may have been mid-resolve/mid-quiesce-scan/mid-accept-dispatch at
 * the instant of fork(), leaving one of them permanently nonzero from this
 * process's own point of view with no thread left that could ever decrement
 * it (the exact same hazard class, and exact same fix, ctpool's own
 * atfork release already applies to its analogous pending_resolve_count).
 * Deliberately NOT applied to in_flight_requests: that counter's own wait
 * (_wait_in_flight_bounded) is already bounded, with a forced-shutdown
 * escalation, so a stale nonzero count there degrades to a bounded delay
 * rather than a permanent hang, and resetting it here would risk letting a
 * destroy proceed to free a connection some still-extant worker thread
 * genuinely has in flight.
 *
 * is_child also resets g_engine_stop_watcher.started/ready to false: that
 * watcher is its own dedicated OS thread (see its own section-level comment
 * above _engine_stop_watcher_fn), not duplicated by fork() any more than
 * srv_engine_bundler.reaper_thread/idle_sweep_bundler.thread are, and its
 * own started guard, copied verbatim from the parent, would otherwise
 * silently stop chttpsvr_engine_stop() from ever working again in this
 * child, not just against the now-inert inherited reactor but against any
 * FUTURE, genuinely fresh chttpsvr_start()/_engine_acquire() call too:
 * _engine_stop_watcher_ensure_started_locked()'s own `if (started) return;`
 * would see the stale, inherited true and skip creating a real watcher
 * thread for this child at all. Safe to leave sem/thread untouched (no
 * explicit destroy/reinit): the next _engine_acquire() call in this child
 * calls semaphore_init() on the inherited sem_t itself once started is
 * false again, which is well-defined given nothing in this child is (or
 * ever was) blocked on it, mirroring routes_lock's own reinit-without-
 * destroy precedent above.
 *
 * is_child also fixes up a live server's own quiesce_state/quiesce_waiters,
 * but NOT by naively resetting quiesce_state back to CHTTPSVR_QS_NOT_
 * QUIESCED (a first design considered for this, then rejected): unlike the
 * pin counters above, this gates a genuinely multi-step, stateful teardown
 * sequence (_quiesce_server_once), and fork() can land while a now-vanished
 * thread was partway through it, having already completed an unknown subset
 * of its steps (listener stopped or not, connections drained or not, the
 * engine reference released or not, the worker pool destroyed or not) with
 * srv->mutex released between most of them. Resetting quiesce_state back to
 * CHTTPSVR_QS_NOT_QUIESCED would let a later chttpsvr_start()/_quiesce_
 * server_once call in this child re-attempt or re-enter that sequence
 * against a server whose actual state cannot be safely inferred from this
 * one enum value alone, risking a double-release or double-free (e.g.
 * _destroy_detached_pools called a second time on a pool the vanished
 * thread had already freed). Instead, for exactly the servers caught
 * mid-teardown at the instant of fork() (quiesce_state ==
 * CHTTPSVR_QS_QUIESCING; the ONLY value a vanished thread could have left
 * behind, since a server that was never mid-teardown is CHTTPSVR_QS_NOT_
 * QUIESCED, and one whose teardown had already fully finished is
 * CHTTPSVR_QS_QUIESCED), this marks the interrupted teardown as if it had
 * finished (quiesce_state = CHTTPSVR_QS_QUIESCED, quiesce_waiters = 0,
 * matching what the vanished thread's own tail, see _quiesce_server_once's
 * own doc comment, would eventually have done) and removes srv from
 * servers_bundler.servers[] via _servers_unregister_locked (safe to call
 * directly rather than through _servers_unregister: servers_bundler.mutex is
 * already held for this entire loop, inherited locked from _chttpsvr_atfork_
 * prepare, so a second, recursive lock attempt through the ordinary,
 * self-locking entry point would deadlock). That second step matters on its
 * own: it is normally _quiesce_server_once's own real work that unregisters
 * srv, and skipping the real work here means nothing else ever will; without
 * it, srv would stay in servers_bundler.servers[] forever, and _engine_
 * force_stop_quiesce_all's own driver loop (which repeatedly reads
 * servers_bundler.servers[0] and only ever advances past an entry once
 * _servers_unregister removes it) would spin on this exact, never-removed
 * entry indefinitely the next time chttpsvr_engine_stop() runs in this
 * child; a second, independently-reachable permanent hang a first, narrower
 * attempt at this exact fix (CHTTPSVR_QS_QUIESCED alone, with no unregister)
 * was found to still leave open. This still leaves the interrupted server's
 * own worker pool, TLS context, and any connections it held at the fork()
 * instant unreleased in this process (no attempt is made to replay or
 * infer that specific bookkeeping, for the same double-free
 * risk described above); a later chttpsvr_destroy()/chttpsvr_start() call on
 * this exact handle in the child returns promptly instead of hanging, at the
 * cost of leaking whatever the vanished thread had not yet released (the
 * worker pool's own eventual reclamation, if this handle is ever destroyed
 * or restarted, is made safe by cthreadpool.c's own foreign_since_fork
 * machinery, built for exactly this "destroy a fork-inherited pool" case).
 *
 * is_child also unconditionally resets every live server's own lifecycle
 * from CHTTPSVR_LC_STOPPING or CHTTPSVR_LC_STARTING (if it was in either of
 * those states) back to CHTTPSVR_LC_IDLE.
 *
 * CHTTPSVR_LC_STOPPING: unlike quiesce_state above, _chttpsvr_stop_internal()
 * durably and atomically leaves CHTTPSVR_LC_RUNNING and resets listen_fd/
 * listen_reg/unix_socket_path to their own "nothing left to clean up"
 * sentinel values, all in the SAME critical section that enters CHTTPSVR_LC_
 * STOPPING, before any of its own potentially-interruptible real work
 * (event_loop_remove()/close()/unlink()) ever begins; a fork() landing
 * anywhere in that real work therefore can never leave those fields in a
 * state chttpsvr_start() could misinterpret, unlike the multi-step,
 * srv->mutex-released _quiesce_server_once sequence above.
 *
 * CHTTPSVR_LC_STARTING: chttpsvr_start() itself unconditionally rebuilds
 * every piece of state a fork() landing mid-call could have left half-set,
 * the next time it runs against this same handle; _wait_and_detach_pools/
 * _destroy_detached_pools always drain and destroy whatever worker_pool/
 * reject_pool happen to be published (safe against a fork-inherited,
 * possibly-half-built pool via cthreadpool.c's own foreign_since_fork
 * machinery, the same mechanism already relied on for the quiesce_state
 * case above), and any already-published tls_ctx is unconditionally
 * released and rebuilt from scratch (plain heap memory, not a slot-table
 * handle, so a post-fork ctls_ctx_release() on it needs no special
 * handling). _servers_register() is itself idempotent (see its own call
 * site's comment in chttpsvr_start()), so a server already registered from
 * before the fork is not double-registered by the next start attempt
 * either. This mirrors the graceful-leak precedent this function's own
 * quiesce_state fixup above already establishes, not a new risk class: a
 * listener fd/registration the vanished thread had already created for
 * THIS in-flight start attempt (as opposed to an old, already-torn-down one
 * chttpsvr_stop() left behind) is simply never referenced by lifecycle ==
 * CHTTPSVR_LC_IDLE and is silently overwritten by the next successful
 * start, or left as an unreferenced, harmless leak if none ever succeeds.
 *
 * A lifecycle left stuck at CHTTPSVR_LC_STOPPING or CHTTPSVR_LC_STARTING
 * forever would otherwise permanently disable this handle in the child:
 * chttpsvr_start()'s own retry loop either polls forever (STOPPING; a plain
 * 1ms-sleep poll on this exact state, not a condvar wait) or refuses
 * outright with ccol_not_permitted, with no retry at all (STARTING; treated
 * identically to a second, genuinely concurrent chttpsvr_start() call), and
 * chttpsvr_stop() cannot unstick either state itself (both make
 * _chttpsvr_stop_internal's own was_started check resolve to false, a
 * silent no-op that never touches lifecycle). Resetting both unconditionally
 * to CHTTPSVR_LC_IDLE is safe precisely because there is no multi-step state
 * to misinterpret here, only this one enum value. The one cost, mirroring
 * the quiesce_state case above: the OLD listener fd (if the vanished thread
 * had not yet reached its own close() call, for CHTTPSVR_LC_STOPPING, or had
 * already reached its own event_loop_add() call, for CHTTPSVR_LC_STARTING)
 * and the OLD unix_socket_path string (if any) are never explicitly released
 * in this process, either; a subsequent chttpsvr_start() on the same
 * host:port may see a spurious EADDRINUSE-class failure from that
 * still-open, orphaned fd rather than a leak-free rebind, but that is a
 * graceful, already-handled failure mode, not a crash. */
static void _chttpsvr_atfork_release_impl(bool is_child) {
  size_t n = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct chttpserver *srv = slot->ptr;

    if (is_child) {
      atomic_store(&srv->pending_resolve_count, (size_t)0);
      atomic_store(&srv->servers_bundler_pins, (size_t)0);
      atomic_store(&srv->listener_dispatch_pins, (size_t)0);
      /* Re-init, not unlock: see this function's own doc comment for the
       * glibc rwlock-write-lock-tracks-TID hazard this avoids. */
      if (rw_lock_init(srv->routes_lock) != 0)
        fatal_err("chttpsvr atfork release: failed to reinit routes_lock");

      /* See this function's own doc comment for the full reasoning behind
       * both of the fixups below. Both are exhaustive switches (no
       * `default:` label), not plain comparisons, for the identical
       * `-Wswitch` reason chttpsvr_lifecycle_t's/chttpsvr_quiesce_state_t's
       * own field comments explain. */
      switch (srv->quiesce_state) {
        case CHTTPSVR_QS_QUIESCING: {
          srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
          srv->quiesce_waiters = 0;
          /* Locked variant: servers_bundler.mutex is already held for this
           * entire loop (inherited locked from _chttpsvr_atfork_prepare). */
          _servers_unregister_locked(srv);
          break;
        }
        case CHTTPSVR_QS_NOT_QUIESCED:
        case CHTTPSVR_QS_QUIESCED:
          break;
      }
      switch (srv->lifecycle) {
        case CHTTPSVR_LC_STOPPING:
        case CHTTPSVR_LC_STARTING:
          srv->lifecycle = CHTTPSVR_LC_IDLE;
          break;
        case CHTTPSVR_LC_IDLE:
        case CHTTPSVR_LC_RUNNING:
          break;
      }
    } else {
      rw_lock_unlock(srv->routes_lock);
    }
    mutex_unlock(srv->diverted_mutex);
    mutex_unlock(srv->idle_mutex);
    mutex_unlock(srv->mutex);
  }

  if (is_child) {
    /* See this function's own doc comment for why this must be reset in
     * the child: without it, _engine_stop_watcher_ensure_started_locked()'s
     * own `started` guard would never let a genuine watcher thread be
     * created for this child, for the rest of this process's lifetime. */
    g_engine_stop_watcher.started = false;
    atomic_store(&g_engine_stop_watcher.ready, false);

    /* See _chttpsvr_atfork_prepare's own doc comment for the full
     * reasoning: a background thread this module owns (the shared engine's
     * own reaper, or the idle-timeout sweep thread) still marked live from
     * the parent's own point of view does not exist in this child at all;
     * a later thread_join() attempt against its stale identifier (from
     * _join_reaper_if_needed_locked()/_idle_sweep_stop_if_running(), both
     * reachable from an ordinary chttpsvr_engine_wait()/_engine_stop()
     * call) has no well-defined outcome under POSIX and hangs in practice.
     * Both mutexes these two fields are otherwise guarded by (srv_engine_
     * bundler.mutex, servers_bundler.mutex respectively) are already held
     * throughout this entire function, so no separate locking is needed
     * here. */
    srv_engine_bundler.reaper_joinable = false;
    idle_sweep_bundler.running = false;

    /* srv_engine_bundler.stopping can independently be left stuck true too,
     * whenever fork() lands while a real, parent-side reaper pass is (or
     * was about to be) in flight (set true by _engine_release()/_engine_
     * force_stop_now() strictly before _spawn_reaper() itself runs, so this
     * can be true even in the narrow window where reaper_joinable above is
     * still false). Left stuck true, this permanently blocks EVERY later
     * chttpsvr_engine_stop() call in the child from ever spawning a new,
     * genuine reaper of its own (_engine_force_stop_now()'s own !stopping
     * guard, meant to reject only a real, redundant second call, silently
     * rejects every future one instead) and permanently blocks EVERY later
     * chttpsvr_start()/_engine_acquire() call too (its own `while (stopping)
     * cond_var_wait(...)` loop waits for a broadcast only the now-vanished
     * reaper thread's own eventual completion would ever send). Resetting it
     * unconditionally is safe: srv_engine_bundler.reactor itself is left
     * untouched (still a real, resolvable, if inert-for-serving, event_loop
     * handle; see event_loop's own foreign_since_fork machinery), so a
     * fresh chttpsvr_engine_stop() call in the child can still genuinely
     * spawn a real reaper and tear it down properly. */
    srv_engine_bundler.stopping = false;
  }

  mutex_unlock(chttpsvr_router_shell_registry.mutex);
  mutex_unlock(servers_bundler.mutex);
  mutex_unlock(srv_engine_bundler.mutex);
  mutex_unlock(chttpsvr_slot_table.mutex);
}

static void _chttpsvr_atfork_release(void) {
  _chttpsvr_atfork_release_impl(false);
}

/* Child-side counterpart to _chttpsvr_atfork_release: releases the identical
 * locks (see _chttpsvr_atfork_release_impl's own comment) and additionally
 * resets each live server's own indefinite-wait pin counters. Must run
 * before any application code in this process can possibly reach one of
 * these servers' own shutdown/destroy path: pthread_atfork's child handler
 * runs synchronously, as part of fork() itself returning, strictly before
 * fork()'s return value ever reaches the calling code. */
static void _chttpsvr_atfork_child_release(void) {
  _chttpsvr_atfork_release_impl(true);
}
#endif /* FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

/* Counts the '/'-delimited segments in an already trailing-slash-stripped,
 * consecutive-slash-free router prefix (the exact form r->prefix always
 * holds by the time this is called; chttpsvr_subrouter rejects "//" in the
 * caller-supplied prefix before _create_router ever runs, and the trailing-
 * slash strip loop just above this function's own call site never introduces
 * a new one). "" (root's own placeholder) and "/" (the special exact-root-
 * only prefix) both have zero segments; every other valid prefix has exactly
 * one segment per '/' character it contains, since each segment is preceded
 * by exactly one such character and there is neither a leading run of them
 * (every prefix starts with a single '/') nor a trailing one (already
 * stripped) nor an internal run (already rejected). */
static int _count_prefix_segments(const char *prefix) {
  size_t len = strlen(prefix);
  if (len <= 1) return 0;
  int count = 0;
  for (size_t i = 0; i < len; i++)
    if (prefix[i] == '/') count++;
  return count;
}

static chttpsvr_router *_create_router(struct chttpserver *srv,
                                       const char *prefix,
                                       ccol_memmgmt_procs_t *mp) {
  /* The shell itself deliberately does NOT use `mp` (the server's own,
   * possibly custom, allocator), via mem_calloc/mem_free (the plain
   * default-allocator macros, not the _mem_* ones; which require an
   * actual ccol_memmgmt_procs_t-typed argument, not a bare NULL literal);
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
  r->prefix_seg_count = _count_prefix_segments(r->prefix);
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
  if (rt->seg_count > router->max_route_seg_count)
    router->max_route_seg_count = rt->seg_count;
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
    /* A genuinely empty pair (nothing at all between two '&' characters, or
     * before a leading one / after a trailing one, e.g. "a=1&&b=2" or
     * "&a=1") is skipped outright rather than added as a spurious key=""/
     * value="" entry. This is distinct from (and must not be confused
     * with) the intentionally-supported "?=value" (empty key, real
     * value; pair_len > 0 since "=value" itself is non-empty) and "?key"
     * (real key, implicit empty value; pair_len > 0 too) forms, both of
     * which are unaffected by this check and continue to be parsed as
     * their own, already-documented and tested entries below. */
    if (pair_len == 0) {
      p = amp ? amp + 1 : NULL;
      continue;
    }
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
    if (!qp) {
      /* Matches the sibling _parse_qparams-failure branch below: without
       * this, an allocation failure here (a request with no query string at
       * all, the common case) is indistinguishable from a genuine
       * key-absent result to chttpsvr_req_query_oom(), contradicting its
       * own documented purpose of telling the two apart. */
      req->_qparams_parse_oom = true;
      return NULL;
    }
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

  clog logger =
      cl ? clog_derive(cl) : clog_open_fd_mp(2, CLOG_FATAL, NULL, mprocs);
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

  if (cond_var_init(srv->quiesce_done_cv) != 0) {
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
    cond_var_destroy(srv->quiesce_done_cv);
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
    cond_var_destroy(srv->quiesce_done_cv);
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
    cond_var_destroy(srv->quiesce_done_cv);
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
  srv->lifecycle = CHTTPSVR_LC_IDLE;

  chttpsvr h = _chttpsvr_handle_slot_acquire(srv);
  if (h == 0) {
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate server slot");
    _mem_free(srv->m_procs, srv->routers);
    _destroy_router(root, srv->m_procs);
    rw_lock_destroy(srv->routes_lock);
    cond_var_destroy(srv->quiesce_done_cv);
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

  /* Unlike every other router, the root router is never handed back to a
   * caller as a chttpsvr_router*, so chttpsvr_router_on/_on_stream/_use's
   * owner-resolve path never runs on it; root->owner is set here purely so
   * _cleanup_chttpsvr_router_shells (this process's exit-time destructor)
   * can tell "root is still live" apart from "root was already destroyed
   * via _destroy_router", exactly like it already can for every sub-router.
   * Left at CHTTPSVR_INVALID (its value straight out of _create_router)
   * this would be indistinguishable from an already-destroyed root, so the
   * destructor would free root's own shell out from under a still-running
   * server whose reactor/worker threads it does not stop or join first. */
  atomic_store(&root->owner, h);
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
 * blocked forever inside chttp1_stream_read/_write (a real, reachable
 * case whenever stream_read_timeout_ms and/or response_write_timeout_ms is
 * configured to 0 ("wait indefinitely", chttpsvr_config_t's own documented,
 * legitimate setting for slow legitimate uploads) and the peer stalls
 * without ever closing the connection) would make this function's own
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
   * so in_flight_requests never even saw it) is invisible to that one pass;
   * a real use-after-free a ThreadSanitizer run over tests_tls caught;
   * the reactor thread could still be inside _conn_pump/_conn_free for such
   * a connection after this function had already returned and
   * __chttpsvr_destroy went on to free srv. current_connections
   * (incremented at accept, decremented in _conn_free) spans a connection's
   * entire lifetime regardless of which path closes it, unlike
   * in_flight_requests (worker-diverted requests only) or the idle list
   * (only connections not currently claimed by some dispatch), so waiting
   * for it to reach zero (repeating the idle-close pass as still-active
   * connections finish and land back in the idle list) closes the gap
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
 * srv->quiesce_state so it can also be driven, exactly once, from the
 * engine's own _engine_force_stop_quiesce_all pass (chttpsvr_engine_stop()
 * may run that pass while srv is still fully started, concurrently with,
 * not just before, an application's own, independent chttpsvr_destroy(srv)
 * call, since chttpsvr_engine_stop() is documented safe to call from a
 * signal handler with no requirement that a caller serialize it against
 * chttpsvr_destroy() via chttpsvr_engine_wait() first); whichever of the
 * two callers reaches a given srv first does the real work, and the OTHER
 * caller BLOCKS until that work has actually finished (quiesce_state
 * reaches CHTTPSVR_QS_QUIESCED), rather than returning as an immediate
 * no-op. This matters because __chttpsvr_destroy proceeds straight from
 * this function's return into mutex_destroy(raw->mutex)/freeing raw
 * itself; if the losing caller returned the instant it observed
 * quiesce_state != CHTTPSVR_QS_NOT_QUIESCED (the previous behavior),
 * __chttpsvr_destroy could destroy raw->mutex/raw->idle_mutex and free raw
 * while the winning caller's own call was still using them inside
 * _drain_and_close_all_connections/_engine_release below, a genuine
 * use-after-free/destroyed-in-use-mutex race, not merely a theoretical
 * one, since _engine_force_stop_quiesce_all's own retry loop already
 * demonstrates the reverse race (a concurrent chttpsvr_destroy()) was
 * anticipated for the other direction. See _engine_force_stop_quiesce_
 * all's own doc comment for why this ordering (stop, unregister, drain,
 * release engine ref, destroy pool) must run to completion before the
 * shared reactor can be torn down. */
static void _quiesce_server_once(struct chttpserver *srv) {
  mutex_lock(srv->mutex);
  /* Exhaustive switch (no `default:` label), for the identical `-Wswitch`
   * reason chttpsvr_quiesce_state_t's own field comment explains. A loser
   * (quiesce_state already CHTTPSVR_QS_QUIESCING or already CHTTPSVR_QS_
   * QUIESCED) shares one branch: its own wait loop below degrades to a
   * correct, zero-iteration no-op when quiesce_state is already
   * CHTTPSVR_QS_QUIESCED, so routing both non-NOT_QUIESCED values through
   * the same path is exactly equivalent to only ever distinguishing
   * CHTTPSVR_QS_QUIESCING, not a new case being handled differently. */
  switch (srv->quiesce_state) {
    case CHTTPSVR_QS_QUIESCING:
    case CHTTPSVR_QS_QUIESCED: {
      /* See quiesce_waiters's own field comment for why this increment/
       * decrement pair (both done while still holding srv->mutex) exists:
       * it lets the winner's own tail below know, via a genuine
       * happens-before relationship (not merely "a broadcast was sent"),
       * that this loser has fully finished touching srv->mutex/
       * srv->quiesce_done_cv before the winner's caller is allowed to
       * destroy either. */
      srv->quiesce_waiters++;
      while (srv->quiesce_state != CHTTPSVR_QS_QUIESCED)
        cond_var_wait(srv->quiesce_done_cv, srv->mutex);
      srv->quiesce_waiters--;
      cond_var_broadcast(srv->quiesce_done_cv);
      mutex_unlock(srv->mutex);
      return;
    }
    case CHTTPSVR_QS_NOT_QUIESCED:
      srv->quiesce_state = CHTTPSVR_QS_QUIESCING;
      break;
  }
  /* Wait for every currently in-flight _chttpsvr_resolve-protected call on
   * srv (chttpsvr_start/_stop/_register_handler/_use/_subrouter/...) to
   * finish before touching any of srv's mutable state below. __chttpsvr_
   * destroy already does this exact wait itself before ever calling this
   * function, but _engine_force_stop_quiesce_all (chttpsvr_engine_stop()'s
   * forced path) calls this function directly against every registered
   * server with no such wait of its own; unlike destroy, that path can
   * legitimately run concurrently with a chttpsvr_start() that has already
   * taken an engine reference (raw->contributed_to_engine == true) and is
   * still using srv_engine_bundler.reactor further down its own function
   * body (e.g. about to call event_loop_add for the listener). Without this
   * wait, this function could concurrently decide "release this server's
   * engine reference" (below) and unregister/drain srv while chttpsvr_start
   * still believes it holds that exact reference and goes on to register a
   * listener against a reactor that may be torn down (or already gone) by
   * the time it gets there, calling event_loop_add against an
   * already-destroyed event_loop handle. Waiting here, under the same
   * srv->mutex/resolve_cv pair _chttpsvr_resolve_unpin already uses, closes
   * that window uniformly for every caller of this function rather than
   * relying on each caller to remember it. (event_reg itself is a
   * generation-checked handle, resolved against its own event_loop's slot
   * table exactly like the event_loop handle is, so a stale conn->reg
   * handed to a later event_loop_remove()/_modify()/_pause()/_resume() call
   * is always safely rejected rather than risking a use-after-free; that is
   * not what this particular wait is protecting against.) */
  while (atomic_load(&srv->pending_resolve_count) > 0)
    cond_var_wait(srv->resolve_cv, srv->mutex);
  mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _quiesce_teardown_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

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
  srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
  cond_var_broadcast(srv->quiesce_done_cv);
  /* Wait for every loser this broadcast just woke to actually finish
   * exiting its own cond_var_wait call (decrementing quiesce_waiters is
   * its last touch of srv->mutex/srv->quiesce_done_cv) before this
   * function returns to its own caller; see quiesce_waiters's own field
   * comment for the real, POSIX-undefined-behavior race this closes when
   * that caller is __chttpsvr_destroy, which destroys both immediately
   * afterward. A plain broadcast alone does not guarantee a woken thread
   * has actually reacquired the mutex yet, only that it has been marked
   * runnable; this loop's own reacquire-and-check is what turns "marked
   * runnable" into "provably finished," via the same mutual-exclusion
   * happens-before relationship pending_resolve_count's own wait already
   * relies on elsewhere in this file. A no-op, bounded-by-nothing-but-OS-
   * scheduling wait in the overwhelmingly common case (0 losers), since
   * nothing prevents a loser already woken by the broadcast just above from
   * promptly reacquiring, decrementing, and re-broadcasting on its own. */
  while (srv->quiesce_waiters > 0)
    cond_var_wait(srv->quiesce_done_cv, srv->mutex);
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
  /* Self-call (see chttpsvr_worker_key_bundle's own comment above): unlike
   * a stale handle, this is a live, valid server; just one whose own
   * worker pool is what is calling destroy on it right now. There is no
   * safe way to proceed: draining in-flight requests can never converge
   * (this exact call stack is what would eventually finish and decrement
   * it), and the eventual ctpool_destroy() call on this thread's own pool
   * is cthreadpool.c's own unrelated fatal misuse. __chttpsvr_destroy has
   * no ccol_retval_t of its own to report this through (its documented
   * contract is: a live handle in, or fatal_err on misuse), so a detected
   * self-call is treated exactly like the stale-handle case immediately
   * above: a loud, immediate fatal_err() naming the real problem, rather
   * than a silent hang followed by an abort whose stack trace points
   * somewhere else entirely. Mirrors __ctpool_destroy's and
   * __event_loop_destroy's own identical self-destroy guards. */
  if (_chttpsvr_is_self_call(raw)) {
    mutex_unlock(chttpsvr_slot_table.mutex);
    fatal_err(
        "chttpsvr_destroy: called from within a request handler (or "
        "middleware) running on this very server's own worker pool; "
        "destroying it here would deadlock waiting for this exact "
        "in-flight request to finish, then abort when the pool's own "
        "self-destroy guard fires. Destroy the server from a different "
        "thread instead, or call chttpsvr_engine_stop() (documented "
        "safe to call from within a handler) if a full shutdown is the "
        "actual goal.");
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

  /* Waits for any concurrent _engine_force_stop_quiesce_all pass that grabbed
   * a bare pointer to raw directly out of servers_bundler.servers[] (see
   * that function's own doc comment) to finish using raw before this
   * function frees a single byte of it below. Deliberately placed after
   * _quiesce_server_once returns, not folded into the pending_resolve_count
   * wait above it, since servers_bundler_pins is never touched by
   * _quiesce_server_once/_chttpsvr_stop_internal themselves (see that
   * field's own comment for the self-deadlock waiting on it from inside
   * that call would otherwise cause), so there is no ordering requirement
   * forcing this wait to happen any earlier than "sometime before raw is
   * freed".
   *
   * The listener_dispatch_pins check alongside it is, at this specific
   * point, always already true (0) by construction, not merely usually
   * true: _chttpsvr_stop_internal (run a moment ago, inside _quiesce_
   * server_once above) now waits for this exact counter to reach 0 itself,
   * strictly after its own event_loop_remove() call for the listener (so no
   * NEW dispatch can ever be triggered again) and strictly before it closes
   * the listener fd (see that function's own doc comment for the real
   * fd-reuse race this closes, found via a fresh deep-scan review); once
   * drained there it can never become nonzero again. Kept here anyway as a
   * cheap, harmless belt-and-suspenders check alongside servers_bundler_
   * pins, both under the identical one-critical-section/resolve_cv pattern,
   * rather than special-cased away now that it happens to be provably
   * redundant at this specific call site. */
  mutex_lock(raw->mutex);
  while (atomic_load(&raw->servers_bundler_pins) > 0 ||
         atomic_load(&raw->listener_dispatch_pins) > 0)
    cond_var_wait(raw->resolve_cv, raw->mutex);
  mutex_unlock(raw->mutex);

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
  cond_var_destroy(raw->quiesce_done_cv);
  rw_lock_destroy(raw->routes_lock);

  clog_close(raw->cl);
  raw->cl = CLOG_INVALID;

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
 * reachable, but ONLY if no chttpsvr handle is still in-use. Unlike
 * chttpclient.c's analogous _cleanup_default_client, there is no
 * library-owned singleton server to destroy first here (this module has no
 * equivalent of chttp_default_client); but that fact only explains why
 * there is no automatic handle for this destructor to destroy on an
 * application's behalf; it does not make freeing the slot table itself safe
 * while an application-created handle is still live. This library does not
 * control __attribute__((destructor)) ordering across a process's various
 * shared objects/atexit handlers, so an application that relies on process
 * exit to reclaim a chttpsvr it created directly (rather than calling
 * chttpsvr_destroy itself) may still have a live handle touched by a
 * destructor/atexit handler that happens to run after this one (any
 * chttpsvr_start/_stop/_register_handler/_use/_subrouter/_destroy call, all
 * of which resolve through this exact slot table). Freeing the shared slot
 * table out from under a still-live handle would turn that into a
 * use-after-free; skipping the free instead leaves exactly the same
 * already-accepted "caller never destroyed their server" leak, just now
 * covering the slot table's own bookkeeping arrays too, mirroring
 * chttpclient.c's own _cleanup_default_client fix for the identical bug
 * class exactly.
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
  bool any_slot_in_use = false;
  size_t slot_count = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (slot->in_use) {
      any_slot_in_use = true;
      break;
    }
  }
  if (!any_slot_in_use) {
    __cvector_destroy(chttpsvr_slot_table.slots);
    __cvector_destroy(chttpsvr_slot_table.free_indices);
  }
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
  /* A server certificate and its private key are a pair: providing exactly
   * one of the two is never valid configuration. The TLS setup further down
   * this function only ever attempts TLS when BOTH cert_path and key_path
   * are non-NULL (ctls_ctx_cert_add's own requirement); silently treating a
   * lone cert_path or key_path as "no TLS configured" would start this
   * server as plain, unencrypted HTTP on the port the caller believes is
   * HTTPS, with no error surfaced anywhere; mirrors chttpclient_set_tls's
   * own identical guard, and the comment above referencing it.
   *
   * The identical hazard also exists for ca_bundle_path alone, with no
   * cert_path/key_path pair: the TLS setup block further down only ever
   * looks at ca_bundle_path inside the `cert_path && key_path` branch, so a
   * caller who set only ca_bundle_path (the ordinary, everyday way to
   * configure custom-CA verification on the client side, via
   * chttpclient_set_tls; curl's own --cacert, Go's TLSClientConfig.
   * RootCAs, and Python's ssl.create_default_context(cafile=...) all work
   * this same way with no client certificate involved) gets the same
   * silent plain-HTTP downgrade rather than an error. No reference server
   * implementation (Go's ListenAndServeTLS, Node's https.createServer,
   * nginx's ssl_certificate/ssl_certificate_key) has any way to configure a
   * TLS server with a trust store but no server identity certificate, so
   * this is rejected the same way.
   *
   * The same silent downgrade also happens when cert_path and key_path are
   * BOTH absent, ca_bundle_path included: e.g. cfg->tls pointing at
   * CHTTP_TLS_DEFAULT (chttp.h), which is {NULL, NULL, NULL, true, true}.
   * That macro is documented as shared, verification-on defaults for both
   * chttpclient and chttpserver; a caller reasonably reaching for it here
   * and forgetting to also set cert_path/key_path afterward hits the exact
   * hazard this whole check exists to prevent. Requiring both to be
   * non-NULL whenever cfg->tls is non-NULL subsumes all three rejected
   * shapes above (lone cert_path, lone key_path, ca_bundle_path with
   * neither) in one condition, rather than enumerating each combination
   * separately. */
  if (cfg && cfg->tls && !(cfg->tls->cert_path && cfg->tls->key_path))
    return ccol_invalid_args;

  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;

#ifdef RUNNING_UNIT_TESTS
  _start_resolve_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* Self-call (see chttpsvr_worker_key_bundle's own comment above): a
   * request handler (or middleware) running on raw's own worker_pool/
   * reject_pool calling chttpsvr_start() to restart the very server it is
   * running on would otherwise proceed into _wait_and_detach_pools' own
   * bounded wait for this exact in-flight request to finish (which it can
   * never do), then into ctpool_destroy() on the pool this thread is a
   * worker of: cthreadpool.c's own fatal self-destroy misuse. Unlike
   * __chttpsvr_destroy, this function has a real ccol_retval_t to report
   * through, and refusing the restart outright leaves the server in a
   * perfectly safe state (still running its old configuration, or, if
   * the handler already called chttpsvr_stop() first, simply stopped;
   * either way nothing is corrupted, and a later, legitimate call from a
   * different thread can still restart it normally), so this is reported
   * gracefully rather than aborting the process. Harmless for a server's
   * very first chttpsvr_start() call ever: no worker thread of this server
   * can possibly exist yet to have been marked. */
  if (_chttpsvr_is_self_call(raw)) {
    _chttpsvr_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  chttpsvr_config_t default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  mutex_lock(raw->mutex);
  for (;;) {
    /* Exhaustive switch (no `default:` label), for the identical
     * `-Wswitch` reason chttpsvr_lifecycle_t's own field comment explains.
     * CHTTPSVR_LC_STOPPING backs off and retries below; CHTTPSVR_LC_IDLE
     * is the only value that falls through past this switch to the second
     * one below it. */
    switch (raw->lifecycle) {
      case CHTTPSVR_LC_STARTING:
      case CHTTPSVR_LC_RUNNING:
        mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 1 */
        return ccol_not_permitted;
      case CHTTPSVR_LC_STOPPING: {
        /* A concurrent _chttpsvr_stop_internal() call on this exact handle
         * (from a different thread; a single caller doing stop()-then-
         * start() serially never observes this, since chttpsvr_stop() does
         * not return until its own teardown work, including leaving this
         * state, has completed) may still be mid-flight, whether driven by a
         * plain chttpsvr_stop() call or by a _quiesce_server_once pass's own
         * initial call into it: unlike the quiesce_state pass checked below
         * (whose own, SEPARATE, later teardown steps wait on pending_
         * resolve_count reaching 0 as their own precondition),
         * _chttpsvr_stop_internal() itself never waits on pending_resolve_
         * count anywhere in its body, only on listener_dispatch_pins; see
         * chttpsvr_lifecycle_t's own field comment for the spurious
         * EADDRINUSE-class race this state closes.
         *
         * This call's own resolve pin is therefore deliberately kept held
         * (unlike the quiesce_state wait below, which must release it first
         * to avoid a real two-condvar deadlock against _quiesce_server_
         * once's own pending_resolve_count wait): holding it here cannot
         * block _chttpsvr_stop_internal()'s own forward progress (nothing it
         * does waits on this counter), and it keeps raw itself alive across
         * this wait with no need to re-resolve afterward, so a genuine
         * condvar wait (woken by _chttpsvr_stop_internal()'s own resolve_cv
         * broadcast right after it sets CHTTPSVR_LC_IDLE) replaces what used
         * to be a blind, fixed-interval nanosleep-and-repoll here: measured,
         * for the structurally identical quiesce_state/"currently stopping"
         * backoffs this function also performs (see each of their own
         * comments), to busy-loop thousands of times a second under load;
         * this branch shares the identical structural shape (release pin,
         * sleep, re-resolve, retry) and therefore the identical risk, now
         * closed here the same way. If this exact wait is ever reached while
         * a _quiesce_server_once pass is the one driving
         * _chttpsvr_stop_internal, waking up to observe CHTTPSVR_LC_IDLE
         * simply falls through to the quiesce_state switch below on the next
         * loop iteration, which releases this pin correctly there before
         * waiting further; nothing here holds the pin any longer than that
         * one extra, bounded loop-back. */
#ifdef RUNNING_UNIT_TESTS
        _start_stopping_wait_signal_fire_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        while (raw->lifecycle == CHTTPSVR_LC_STOPPING)
          cond_var_wait(raw->resolve_cv, raw->mutex);
        continue;
      }
      case CHTTPSVR_LC_IDLE:
        break;
    }
    /* A _quiesce_server_once pass for this exact server (driven either by
     * a concurrent chttpsvr_engine_stop() force-stop, via
     * _engine_force_stop_quiesce_all, which reads raw straight out of
     * servers_bundler.servers[] and calls _quiesce_server_once directly,
     * WITHOUT ever going through _chttpsvr_resolve/pending_resolve_count;
     * see that function's own doc comment, or by another thread's
     * chttpsvr_destroy() call racing this one) may already be actively
     * running: _chttpsvr_stop_internal (which leaves CHTTPSVR_LC_RUNNING)
     * is only the very first step of that pass, run long before the
     * pass's own, potentially slow _drain_and_close_all_connections/
     * _engine_release work finishes and quiesce_state finally reaches
     * CHTTPSVR_QS_QUIESCED. Since that pass never pins raw, the
     * pending_resolve_count wait _quiesce_server_once performs on ITS OWN
     * entry cannot see this call's own pin either way, so raw->lifecycle
     * reading CHTTPSVR_LC_IDLE above is not proof no teardown is still in
     * flight. Waiting here for any such in-progress pass to fully finish,
     * rather than proceeding to blindly reset quiesce_state below, is what
     * closes that gap: without it, this call could stomp that state out
     * from under the in-progress pass's own single-winner serialization
     * (see _quiesce_server_once's own doc comment), letting either a
     * second, concurrent quiesce of the same raw run alongside the first
     * (a destroyed-mutex-in-use use-after-free once a racing
     * chttpsvr_destroy() proceeds straight from that second pass's return
     * into freeing raw), or a later, unrelated _engine_force_stop_quiesce_
     * all re-scan silently re-quiescing (and thereby killing) the very
     * server this call is in the middle of restarting.
     *
     * Must NOT wait on raw->quiesce_done_cv while still holding this
     * call's own resolve pin (an earlier version of this fix did exactly
     * that, and deadlocked): _quiesce_server_once's own pending_resolve_
     * count wait (which must reach 0 before that pass can do ANY of its
     * real work, including the work that would eventually reach
     * CHTTPSVR_QS_QUIESCED and broadcast this condvar) counts this call's
     * own still-held pin, so a pinned wait here would make _quiesce_
     * server_once wait forever for a pin only WE could release, while we
     * wait forever for a signal only IT could send; a genuine, two-condvar
     * circular deadlock, not a rare corner case, reachable from an
     * entirely ordinary chttpsvr_stop()-then-chttpsvr_start() restart
     * racing a concurrent chttpsvr_engine_stop(). Fixed by fully releasing
     * the pin (the same _chttpsvr_resolve_unpin every other exit path
     * already uses) before backing off, then re-resolving the original
     * handle from scratch once the in-progress pass has plausibly
     * finished. Re-resolving (rather than continuing to touch the same
     * raw pointer directly) is what makes this safe even if raw itself is
     * concurrently freed while this call holds no pin at all (e.g. a
     * racing chttpsvr_destroy() completing its own, now-unblocked,
     * teardown and free while we are backed off here): the generation-
     * checked slot table either hands back the same, still-live raw (loop
     * again) or correctly reports the handle as gone (ccol_invalid_args),
     * never a stale pointer. Exhaustive switch (no `default:` label), for
     * the identical `-Wswitch` reason explained above. */
    switch (raw->quiesce_state) {
      case CHTTPSVR_QS_QUIESCING:
        /* Joins the exact same winner/loser protocol _quiesce_server_once's
         * own CHTTPSVR_QS_QUIESCING/_QUIESCED branch already uses
         * (quiesce_waiters incremented before waiting, decremented and
         * broadcast after waking, so the winner's own tail wait for
         * quiesce_waiters == 0 correctly accounts for this call too;
         * see that function's own doc comment) rather than a blind,
         * fixed-interval poll: a poll here was measured, for the
         * structurally identical CHTTPSVR_LC_STOPPING/"currently stopping"
         * backoffs elsewhere in this function, to busy-loop thousands of
         * times a second and starve the real teardown thread's own
         * condition-variable wakeup out of ever winning the race against a
         * much cheaper re-pin (see the "currently stopping" branch further
         * below for that history and fix).
         *
         * quiesce_waiters MUST be incremented here, in the same critical
         * section that just observed CHTTPSVR_QS_QUIESCING, strictly before
         * this call's own resolve pin is released below; not after
         * re-locking raw->mutex, which is what an earlier version of this
         * code did and which was a real, reproducible use-after-free:
         * releasing the pin first can immediately unblock
         * _quiesce_server_once's own pending_resolve_count wait, letting the
         * winner run its entire real teardown (_chttpsvr_stop_internal,
         * _drain_and_close_all_connections, _engine_release) and reach its
         * own tail "while quiesce_waiters > 0" check before this call ever
         * gets back to raw->mutex; finding quiesce_waiters still zero, the
         * winner returns and __chttpsvr_destroy proceeds straight to
         * mutex_destroy(raw->mutex)/free(raw), with nothing left protecting
         * this call's own subsequent mutex_lock(raw->mutex) below from
         * running on already-freed memory. Incrementing while still holding
         * the lock that observed CHTTPSVR_QS_QUIESCING closes this: the
         * winner cannot transition to CHTTPSVR_QS_QUIESCED (which itself
         * requires raw->mutex) until after this increment is already
         * visible to it, so quiesce_waiters is provably nonzero for as long
         * as it takes the winner to ever reach its own tail check,
         * regardless of how the two threads are actually scheduled; which
         * is what keeps raw alive across the resolve_unpin call and the
         * wait loop below. Safe to wait directly on raw (rather than
         * re-resolving first, the way the process-wide "currently stopping"
         * condition below has to) for exactly this reason. Re-resolving
         * from the handle afterward, rather than continuing to use this
         * same raw pointer directly, still matches every other backoff
         * branch in this function: the slot table (not raw itself) is what
         * proves whether srv is still a live, startable handle by the time
         * this wait finishes. */
        raw->quiesce_waiters++;
        mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw);
#ifdef RUNNING_UNIT_TESTS
        _start_quiescing_unpinned_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        mutex_lock(raw->mutex);
        while (raw->quiesce_state != CHTTPSVR_QS_QUIESCED)
          cond_var_wait(raw->quiesce_done_cv, raw->mutex);
        raw->quiesce_waiters--;
        cond_var_broadcast(raw->quiesce_done_cv);
        mutex_unlock(raw->mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        mutex_lock(raw->mutex);
        continue;
      case CHTTPSVR_QS_NOT_QUIESCED:
      case CHTTPSVR_QS_QUIESCED:
        break;
    }
    /* No `break;` here: this `for (;;)` loop's own scope now extends all
     * the way to the end of the function (see the "currently stopping"
     * branch far below, inside the `if (need_acquire) {...}` block, for
     * the one new `continue;` site this enables) rather than being closed
     * right here as it once was. Every `return` statement anywhere in the
     * rest of this function is unaffected by this: a `return` exits the
     * function regardless of loop nesting, so falling through to the claim
     * below (nothing left to check) works exactly as it did when this was
     * a `break;` out of a small, separately-scoped loop. */
    /* Claimed here, under the same lock and in the same critical section as
     * the checks above, so a second, concurrent chttpsvr_start() call
     * on this same handle cannot slip past both checks before this one sets
     * it; see chttpsvr_lifecycle_t's own field comment for what that race
     * would otherwise corrupt (raw->tls_ctx use-after-free, dropped worker
     * pools). Reset to CHTTPSVR_LC_IDLE under raw->mutex at every return
     * point below. */
    raw->lifecycle = CHTTPSVR_LC_STARTING;
    /* A prior stop/quiesce cycle (whether via chttpsvr_stop()+chttpsvr_start()
     * restart, or a chttpsvr_engine_stop() force-stop this server happened to
     * survive without being chttpsvr_destroy()'d) may have left quiesce_state
     * at CHTTPSVR_QS_QUIESCED; this server is legitimately starting fresh
     * again, so _quiesce_server_once must be willing to run its real teardown
     * work again the next time this server actually is destroyed. Safe to
     * reset unconditionally here: the loop above already guarantees no
     * quiesce_state == CHTTPSVR_QS_QUIESCING pass is still in flight at this
     * exact point, under the same, continuously-held raw->mutex critical
     * section. */
    raw->quiesce_state = CHTTPSVR_QS_NOT_QUIESCED;
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
      mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return ccol_not_enough_memory;
    }

    /* reject_pool's own thread count scales with worker_pool's own resolved
     * size (nthreads, above; already the CPU-count-resolved value when
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
      mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 3 */
      return ccol_not_enough_memory;
    }

    mutex_lock(raw->mutex);
    raw->worker_pool = new_pool;
    raw->reject_pool = new_reject_pool;
    mutex_unlock(raw->mutex);

    atomic_store(&raw->stream_read_timeout_ms, cfg->stream_read_timeout_ms);
    atomic_store(&raw->max_body_read_duration_ms,
                 cfg->max_body_read_duration_ms);
    atomic_store(&raw->response_write_timeout_ms,
                 cfg->response_write_timeout_ms ? cfg->response_write_timeout_ms
                                                : cfg->stream_read_timeout_ms);
    atomic_store(&raw->max_response_write_duration_ms,
                 cfg->max_response_write_duration_ms);
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
        mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 4 */
        return ccol_not_enough_memory;
      }
      if (ctls_ctx_cert_add(tls_ctx, NULL, cfg->tls->cert_path,
                            cfg->tls->key_path, NULL, NULL) != ccol_success) {
        ctls_ctx_release(tls_ctx);
        _destroy_detached_pools(_wait_and_detach_pools(raw));
        mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 5 */
        return ccol_unexpected_failure;
      }
      if (cfg->tls->ca_bundle_path &&
          ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL) !=
              ccol_success) {
        /* An unreadable/malformed CA bundle must not be silently treated as
         * "no CA bundle configured": ctls_ctx_trust() succeeding is what
         * enables SSL_VERIFY_PEER (mutual-TLS client-certificate
         * verification) on the resulting context; discarding this failure
         * would start the server serving TLS without the client-certificate
         * enforcement the caller explicitly asked for, with no error
         * surfaced anywhere. Matches the sibling ctls_ctx_cert_add failure
         * a few lines above exactly. */
        ctls_ctx_release(tls_ctx);
        _destroy_detached_pools(_wait_and_detach_pools(raw));
        mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 6 */
        return ccol_unexpected_failure;
      }
      raw->tls_ctx = tls_ctx;
    }

    /* Registered here, BEFORE this call's own engine-acquire attempt (if any)
     * even runs, rather than only once it succeeds: this call already stays
     * resolve-pinned (raw->pending_resolve_count, from _chttpsvr_resolve at
     * the top of this function) for its entire duration, so registering raw
     * now lets a concurrent chttpsvr_engine_stop()'s _engine_force_stop_
     * quiesce_all pass (which only quiesces servers it can find in
     * servers_bundler.servers) find raw immediately and correctly block on
     * this call's own pin (see _quiesce_server_once's own pending_resolve_
     * count wait) before releasing the shared reactor and potentially tearing
     * it down, even if that race lands in the narrow window before
     * _engine_acquire() below has actually run. Registering only once
     * _engine_acquire() had already succeeded (an earlier version of this
     * ordering) left exactly that window open: a concurrent force-stop
     * landing there found raw entirely absent from servers_bundler.servers,
     * completed its own scan believing every server was already quiesced,
     * and could proceed to tear the reactor down while this call was still
     * about to acquire and use it; surfacing as a spurious
     * ccol_unexpected_failure from this call (event_loop_add failing against
     * an already-gone reactor), not a crash, but a real, avoidable race none
     * the less. _servers_register is idempotent, so calling it here
     * unconditionally (whether or not this call goes on to actually need a
     * fresh engine reference) is safe for every caller, including a restart
     * that already registered raw on an earlier chttpsvr_start() call. If
     * _engine_acquire() below fails, this registration is undone in that
     * failure path rather than left dangling (see exit 7 below): an
     * unregistered server holding no engine reference is the correct state
     * for a call that never actually finished starting. */
    _servers_register(raw);

    bool need_acquire;
    mutex_lock(raw->mutex);
    need_acquire = !raw->contributed_to_engine;
    if (need_acquire) raw->contributed_to_engine = true;
    mutex_unlock(raw->mutex);

    if (need_acquire) {
#ifdef RUNNING_UNIT_TESTS
      _engine_stopping_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
      bool currently_stopping = false;
      ccol_retval_t engine_rc = _engine_acquire(&currently_stopping);
      if (currently_stopping) {
        /* A reap (forced via chttpsvr_engine_stop(), or graceful via some
         * other server's own chttpsvr_destroy() dropping reactor_refs to 0)
         * is currently tearing the shared reactor down; _engine_acquire()
         * returned immediately without blocking for it and without ever
         * incrementing reactor_refs; see that function's own doc comment
         * for the deadlock this closes (blocking there while still holding
         * this call's own resolve pin is what a _quiesce_server_once pass for
         * this exact server would be waiting on, and that pass is what the
         * reap itself is stuck behind). Unwind exactly as the engine_rc !=
         * ccol_success path below does (this is deliberately the same
         * template as exit 7, not the _chttpsvr_undo_start_registration()
         * helper used at exits 8/9: those exits run after _engine_acquire()
         * has already succeeded and genuinely incremented reactor_refs, so
         * undoing them means actually releasing a reference; this branch
         * never incremented it in the first place, so there is nothing to
         * release). */
        mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        mutex_unlock(raw->mutex);
        _servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        _destroy_detached_pools(_wait_and_detach_pools(raw));
        _chttpsvr_resolve_unpin(raw);
        /* A genuine condvar wait, matching the quiesce_state/lifecycle
         * checks above (see each of their own comments for why a blind
         * "nanosleep(1ms); re-resolve; continue" poll-and-retry was measured,
         * empirically (a background thread's own retry cycle observed via a
         * temporary debug build), to busy-loop thousands of times a second
         * under load for this exact class of backoff): a blind retry loop
         * here specifically could out-race _quiesce_server_once's own thread
         * for this exact server before it can ever win the brief window
         * (this call's own pin momentarily at 0, immediately after the
         * _chttpsvr_resolve_unpin above) it needs to notice pending_resolve_
         * count has reached 0 and make real progress; a genuine starvation
         * bug, not merely a theoretical one, since retrying instantly
         * re-registers and re-pins before the reaper's own thread is ever
         * scheduled. Waits on srv_engine_bundler.stopped_cv (a process-wide
         * condvar, unlike the other two waits' own raw-scoped
         * resolve_cv/quiesce_done_cv) specifically because raw itself may
         * already be freed by the time the reap this call is waiting on
         * actually finishes, so nothing scoped to raw could safely be waited
         * on here; this call has ALREADY fully released its own pin above
         * (the one and only thing the reap could have been waiting on)
         * before reaching this point, so waiting on a process-wide condvar
         * with no pin held carries no risk of re-deadlocking against a pin
         * only this call could release. stopped_cv is broadcast once the
         * whole reap (not just this one server's own quiesce) completes,
         * which _engine_acquire()'s own now-removed blocking wait already
         * used to wait on; the only thing that changed is that this call no
         * longer holds a pin while doing so. */
        call_once(srv_engine_bundler.once, _engine_globals_init);
        mutex_lock(srv_engine_bundler.mutex);
        while (srv_engine_bundler.stopping)
          cond_var_wait(srv_engine_bundler.stopped_cv,
                        srv_engine_bundler.mutex);
        mutex_unlock(srv_engine_bundler.mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        mutex_lock(raw->mutex);
        continue;
      }
      if (engine_rc != ccol_success) {
        mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        mutex_unlock(raw->mutex);
        /* Undo the speculative registration above: this call never actually
         * acquired an engine reference, so raw must not be left visible to
         * _engine_force_stop_quiesce_all as if it held one. Only reachable
         * when need_acquire was true, i.e. raw did not already hold a
         * reference (and therefore was not already registered) before this
         * call began, so unregistering here can never undo a registration
         * some earlier, already-successful chttpsvr_start() call is still
         * relying on. */
        _servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        _destroy_detached_pools(_wait_and_detach_pools(raw));
        _chttpsvr_resolve_unpin(raw); /* exit 7 */
        return engine_rc;
      }
    }
#ifdef RUNNING_UNIT_TESTS
    _start_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
    if (!_idle_sweep_start_if_needed()) {
      /* Every registered server's idle_timeout_ms enforcement and
       * max_connections capacity recovery depend entirely on this shared
       * thread; proceeding as if this call had succeeded would silently
       * leave both features permanently non-functional for this server
       * (and, for the max_connections case, leave a listener paused forever
       * with nothing left able to resume it, the instant capacity is ever
       * reached), with chttpsvr_start() itself reporting success. Failed
       * exactly like _make_listen_socket's own failure a few lines below:
       * undo whatever this attempt already set up and report a real error
       * instead. */
      _SRV_ENGINE_LOG(log_error,
                      "idle-timeout sweep thread could not be started");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _destroy_detached_pools(_wait_and_detach_pools(raw));
      _chttpsvr_undo_start_registration(raw, need_acquire);
      mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 8 */
      return ccol_unexpected_failure;
    }

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
      _chttpsvr_undo_start_registration(raw, need_acquire);
      mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 9 */
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
    event_reg lreg = event_loop_add(
        srv_engine_bundler.reactor, selectable_from_fd(lfd, ccol_select_read),
        (event_handlers_t){.on_readable = _listener_on_readable}, raw,
        &reg_err);
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
      _chttpsvr_undo_start_registration(raw, need_acquire);
      mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 10 */
      return ccol_unexpected_failure;
    }

    mutex_lock(raw->mutex);
    raw->unix_socket_path = unix_path;
    raw->listen_reg = lreg;
    raw->lifecycle = CHTTPSVR_LC_RUNNING;
    mutex_unlock(raw->mutex);

    /* raw was already registered above, right after the engine reference was
     * confirmed; nothing further to do here. */
    _chttpsvr_resolve_unpin(raw); /* exit 11 (success) */
    return ccol_success;
  } /* end of the for (;;) retry loop opened near the top of this function */
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
  /* Exhaustive switch (no `default:` label), for the identical `-Wswitch`
   * reason chttpsvr_lifecycle_t's own field comment explains, rather than
   * a plain `raw->lifecycle == CHTTPSVR_LC_RUNNING` comparison: verified to
   * preserve every existing reachable case exactly, including a
   * concurrent second chttpsvr_stop() call observing CHTTPSVR_LC_STOPPING,
   * or a call racing an in-flight chttpsvr_start() and observing
   * CHTTPSVR_LC_STARTING, both correctly resolving to was_started == false
   * here, same as before. */
  bool was_started = false;
  switch (raw->lifecycle) {
    case CHTTPSVR_LC_RUNNING:
      was_started = true;
      break;
    case CHTTPSVR_LC_IDLE:
    case CHTTPSVR_LC_STARTING:
    case CHTTPSVR_LC_STOPPING:
      was_started = false;
      break;
  }
  int lfd = atomic_load(&raw->listen_fd);
  event_reg lreg = raw->listen_reg;
  bool is_unix = atomic_load(&raw->is_unix_socket);
  char *unix_path = raw->unix_socket_path;
  if (was_started) {
    /* Set in the same critical section that leaves CHTTPSVR_LC_RUNNING, so
     * chttpsvr_start()'s own wait loop can never observe lifecycle ==
     * CHTTPSVR_LC_IDLE without having first observed CHTTPSVR_LC_STOPPING
     * for as long as this function's own real teardown work (below) is
     * still in flight; see chttpsvr_lifecycle_t's own field comment for
     * the race this state closes. */
    raw->lifecycle = CHTTPSVR_LC_STOPPING;
    atomic_store(&raw->listen_fd, -1);
    raw->listen_reg = EVENT_REG_INVALID;
    raw->unix_socket_path = NULL;
  }
  mutex_unlock(raw->mutex);
  if (!was_started) return;

#ifdef RUNNING_UNIT_TESTS
  _stop_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* event_loop_remove guarantees no NEW _listener_on_readable dispatch is
   * ever triggered for lreg once it returns, but it does NOT wait for one
   * already in progress at the moment of the call to finish: it never takes
   * the registration's own dispatch_lock; see cthreadcomm.c's
   * _event_loop_run_callback's own comment, and event_loop_remove's own
   * doc comment in cthreadcomm.h, whose "teardown is deferred until any
   * in-progress callback returns" describes event_loop's own internal
   * bookkeeping memory, not a promise that the calling thread here blocks
   * for it. A real, if narrow (a few instructions: loading srv->listen_fd
   * into a register, then issuing accept4() with it), race would otherwise
   * exist here: a dispatch already past that load could still be about to
   * call accept4() with the fd number this function is about to close and
   * potentially let some unrelated concurrent open() in this same process
   * reuse. Closed the same way __chttpsvr_destroy already protects raw's
   * own struct memory against this same in-flight dispatch: waiting for
   * listener_dispatch_pins to reach 0. Safe to do here specifically (unlike
   * servers_bundler_pins/pending_resolve_count, which _quiesce_server_once/
   * _chttpsvr_stop_internal must never wait on; see each field's own
   * comment) because listener_dispatch_pins is held only by the reactor
   * thread running _listener_on_readable, never by whatever thread calls
   * chttpsvr_stop()/_quiesce_server_once/__chttpsvr_destroy; there is no
   * scenario where the thread waiting here is also the one that would need
   * to run for this counter to ever reach 0, so no self-deadlock is
   * possible. event_loop_remove() having already run when this wait starts
   * guarantees it can only be waiting on a dispatch already live at that
   * exact moment, never a fresh one; there is at most one at a time
   * (event_loop's own per-registration dispatch_lock already guarantees a
   * registration's callback is never invoked concurrently with itself). */
  if (lreg) event_loop_remove(srv_engine_bundler.reactor, lreg);
  mutex_lock(raw->mutex);
  while (atomic_load(&raw->listener_dispatch_pins) > 0)
    cond_var_wait(raw->resolve_cv, raw->mutex);
  mutex_unlock(raw->mutex);
  close(lfd);
  if (is_unix && unix_path) unlink(unix_path);
  _mem_free(raw->m_procs, unix_path);

  /* Unconditional, not a switch: this point is only ever reached on the
   * path where was_started was already true (an early `return` sits
   * between it and here for the false case), and chttpsvr_start()'s own
   * retry loop always backs off and retries for the entire time lifecycle
   * == CHTTPSVR_LC_STOPPING, so nothing else can change raw->lifecycle
   * away from CHTTPSVR_LC_STOPPING between this function's own top-of-body
   * claim and this exit; unlike a fork() (which can land at a genuinely
   * arbitrary point with no such guarantee, see
   * _chttpsvr_atfork_release_impl's own analogous fixup), this is one
   * uninterrupted, single-writer synchronous call sequence throughout.
   *
   * Broadcasts resolve_cv (the same condvar this function's own
   * listener_dispatch_pins wait above already uses) so a concurrent
   * chttpsvr_start() call blocked in its own CHTTPSVR_LC_STOPPING wait (see
   * that branch's own comment) wakes promptly instead of discovering this
   * transition only on its next poll tick. */
  mutex_lock(raw->mutex);
  raw->lifecycle = CHTTPSVR_LC_IDLE;
  cond_var_broadcast(raw->resolve_cv);
  mutex_unlock(raw->mutex);
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

/* Reads how many servers are currently registered in servers_bundler.
 * servers[] (the list _engine_force_stop_quiesce_all's own driver loop
 * walks). Lets a test confirm a server was genuinely unregistered (e.g. by
 * _chttpsvr_atfork_release_impl's own child-side fixup for a server caught
 * mid-teardown at fork() time; see that function's own doc comment) without
 * needing to actually drive a fresh chttpsvr_engine_stop() pass to observe
 * it indirectly. */
size_t _chttpsvr_servers_bundler_count_for_tests(void) {
  call_once(srv_engine_bundler.once, _engine_globals_init);
  mutex_lock(servers_bundler.mutex);
  size_t n = servers_bundler.count;
  mutex_unlock(servers_bundler.mutex);
  return n;
}

/* White-box test helper exposing the resolved idle_timeout_ms value a
 * chttpsvr_start() call actually stored, after chttpsvr_config_t.idle_
 * timeout_ms/read_timeout_ms (both `long`) were combined and clamped into
 * the internal `unsigned` field (see that clamp's own comment in
 * chttpsvr_start). Lets a test assert the clamp directly (a configured
 * value >= 2^32 ms must land on UINT_MAX, never silently wrap to 0 (this
 * field's own "disabled" sentinel) or some other unrelated value) without
 * waiting out an astronomically long real idle-timeout window. Returns 0
 * for an invalid handle, indistinguishable from a genuinely-resolved
 * server whose idle timeout really is disabled; a test using this accessor
 * is expected to already know h is otherwise valid. */
unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  return atomic_load(&raw->idle_timeout_ms);
}

/* White-box test helper exposing the current in_flight_requests count (see
 * that field's own comment and _release_in_flight/_conn_start_diverted/
 * _conn_dispatch_reject for what it tracks): every read/write of this field
 * elsewhere in this file happens under raw->mutex, so this reads it the same
 * way rather than a bare unsynchronized load. Lets a test poll for "the
 * request this test just sent has actually been dispatched to a worker
 * thread" via a genuine, bounded condition instead of a fixed sleep guessing
 * how long accept/parse/dispatch takes on the machine the test happens to run
 * on. Returns -1 for an invalid handle, distinguishable from any real count
 * (which is never negative). */
int _chttpsvr_in_flight_requests_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return -1;
  mutex_lock(raw->mutex);
  int n = raw->in_flight_requests;
  mutex_unlock(raw->mutex);
  return n;
}

/* White-box test helper exposing listener_paused_for_resource_pressure (see
 * that field's own comment) directly: a test proving the listener was
 * genuinely paused (not merely quiet because its backlog happened to run
 * dry at the moment observed) needs a deterministic state check rather than
 * inferring pausedness from the ABSENCE of network activity over some
 * timing window, since the idle-timeout sweep thread's own real, ~1s timer
 * runs unsynchronized with any test's clock and can legitimately resume
 * (and, if the underlying condition still persists, immediately re-pause)
 * at any point, making a network-observed window an inherently flaky
 * signal for this specific property. Returns false for an invalid handle,
 * indistinguishable from a genuinely-resolved, not-currently-paused server;
 * a test using this accessor is expected to already know h is otherwise
 * valid. */
bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return false;
  return atomic_load(&raw->listener_paused_for_resource_pressure);
}

/* White-box test hook only: marks the calling thread as if it were
 * currently a worker thread of h's own worker_pool/reject_pool (see
 * chttpsvr_worker_key_bundle's own comment above), without actually
 * dispatching any real task or needing a running listener/reactor at all.
 * Lets a test deterministically exercise chttpsvr_destroy()'s self-call
 * guard directly. This matters beyond mere convenience: driving the guard
 * via a real end-to-end request would require chttpsvr_start() to succeed
 * inside a forked child of a test binary whose shared reactor is already
 * running with real OS threads; threads fork(2) does not duplicate into
 * the child, leaving that child with a hollow, un-serviced reactor handle
 * and no way to ever actually accept the connection meant to trigger the
 * handler under test. Intended for exactly this forked-child, process-
 * ending-in-abort-or-exit use (see destroy_from_within_own_handler_is_
 * fatal in tests.c); no unmark counterpart exists since a mark made inside
 * a forked child that never returns has nothing left to leak into. A no-op
 * if h does not resolve. Gated so neither this function nor
 * chttpsvr_worker_key_bundle's own visibility exists in a production
 * build. */
void _chttpsvr_mark_self_as_worker_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (raw) _chttpsvr_mark_worker_thread(raw);
}
#endif /* RUNNING_UNIT_TESTS */

void chttpsvr_engine_stop(void) { _engine_force_stop(); }

void chttpsvr_engine_wait(void) {
  /* Self-call guard (see _chttpsvr_is_any_worker_call's own comment): a
   * request handler/middleware on ANY currently-registered server that
   * blocks here can deadlock the whole engine, not just its own server;
   * _engine_force_stop_quiesce_all's own timeout-less ctpool_shutdown_drain
   * can never finish draining that handler's own pool while this exact
   * call stack is what would eventually let the task return, so the
   * reactor can never be torn down and this wait can never wake. This
   * function has no ccol_retval_t of its own to report the misuse through
   * (its documented contract is simply "blocks until engine exit"), so a
   * detected self-call is treated exactly like chttpsvr_destroy()'s and
   * chttpsvr_start()'s own identical guards: a loud, immediate fatal_err()
   * naming the real problem, rather than a silent, permanent hang. */
  if (_chttpsvr_is_any_worker_call()) {
    fatal_err(
        "chttpsvr_engine_wait: called from within a request handler (or "
        "middleware) running on some server's own worker pool; blocking "
        "here would deadlock the shared engine's shutdown, which can "
        "never finish draining this exact server's worker pool while "
        "this in-flight request is what would eventually let it return. "
        "Call chttpsvr_engine_wait() from a different thread instead.");
  }
  _engine_wait_until_stopped();
}

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
      _chttpsvr_resolve_unpin(raw); /* exit 3 */
      return NULL;
    }
    raw->routers = nr;
    raw->router_cap = nc;
  }
  raw->routers[raw->router_count++] = r;
  /* See max_prefix_seg_count's own field comment on struct chttpserver: kept
   * up to date here, under the same write-lock critical section as the
   * router registration itself, exactly mirroring how _router_add_route
   * maintains router->max_route_seg_count under the identical lock. */
  if (r->prefix_seg_count > raw->max_prefix_seg_count)
    raw->max_prefix_seg_count = r->prefix_seg_count;
  rw_lock_unlock(raw->routes_lock);
  _chttpsvr_resolve_unpin(raw); /* exit 4 (success) */
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
  chttpsvr_conn_t *conn = req->conn;
  /* See chttpsvr_req_read's identical guard for why conn/conn->matched_route
   * are never actually NULL through documented usage, and why this is
   * checked defensively here anyway (unlike chttpsvr_req_method/_path/
   * _header/_raw_query, which dereference req->conn unconditionally once
   * !req has already been ruled out, since the same "never actually NULL
   * in practice" argument applies equally to all of them).
   *
   * A streaming route's body is never pre-extracted into conn->body as one
   * contiguous, stable buffer the way a buffered route's is: conn->body is
   * instead a live cursor (growbuf_t.pos) chttpsvr_req_read() drains batch
   * by batch as bytes actually arrive, only lazily compacted back to empty
   * by _on_body() on the NEXT batch's arrival, not immediately once fully
   * drained. A streaming handler that calls chttpsvr_req_read() at all and
   * then calls this function would, without this guard, get back a pointer/
   * length pair that silently mixes already-delivered bytes (before pos)
   * with not-yet-delivered ones (from pos to len), or a length that merely
   * reflects internal accounting since the last compaction rather than
   * either the true total or the true remaining unread count; with no
   * error signal at all. This function is documented as buffered-route-only
   * (see chttpsvr_req_body's own doc comment in chttpserver.h); mirrors
   * chttpsvr_req_read's own symmetric rejection of a buffered route exactly,
   * so a route mismatch always fails safe (a clear NULL/0) on either
   * accessor rather than only one of them. */
  if (!conn || !conn->matched_route || conn->matched_route->is_streaming) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = conn->body.len;
  return conn->body.buf;
}

const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn_t *conn = req->conn;
  /* See chttpsvr_req_read's identical guard for why conn/conn->matched_route
   * are never actually NULL through documented usage, and why this is
   * checked defensively here anyway (unlike chttpsvr_req_method/_path/
   * _header/_raw_query, which dereference req->conn unconditionally once
   * !req has already been ruled out). */
  if (!conn || !conn->matched_route) return NULL;
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
  if (!req || !key) {
    if (val_out) *val_out = NULL;
    return ccol_invalid_args;
  }

  chttpsvr_qparams_t *qp = _ensure_qparams(req);
  if (!qp) {
    if (val_out) *val_out = NULL;
    return ccol_not_enough_memory;
  }
  if (req->_qparams_parse_oom) {
    if (val_out) *val_out = NULL;
    return ccol_not_enough_memory;
  }

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
  /* A field-name must be a non-empty tchar-only token (RFC 7230 SS3.2.6),
   * not merely non-empty and CRLF-free: a byte outside that set (e.g. a
   * space or a literal ':') is not itself a CRLF-injection vector, but
   * still produces a structurally malformed wire line ambiguous to a
   * strict downstream parser; "X Foo: bar" as a name puts
   * "X Foo:bar:baz\r\n" on the wire, not a genuine two-field split. Uses
   * the identical tchar classifier chttp1_parser.c itself already applies
   * to an incoming request's own header names (and chttpclient.c's
   * chttp_request_set_header applies to an outgoing request's), rather
   * than a second, potentially-divergent character class. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
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
  /* "Transfer-Encoding" is rejected outright, exactly like
   * chttp_request_set_header does on the client side (chttpclient.c) and for
   * the identical reason: this server never transfer-codes a response body
   * (see _send_response's own comment: "this server never uses chunked
   * transfer-encoding for its own responses"), so honoring a caller-set
   * Transfer-Encoding header is impossible, and letting one reach the wire
   * would pair it with this function's own auto-computed Content-Length
   * header over a body that was never actually transfer-coded; an
   * ambiguous framing (RFC 7230 SS3.3.3) that lets an intermediary honoring
   * Transfer-Encoding over Content-Length misparse the message boundary,
   * exactly the response-splitting/desync hazard the Connection/
   * Content-Length filtering below already exists to prevent. Checked before
   * the duplicate-detection scan below, so a handler cannot install one via
   * this function under any circumstance, including updating a
   * previously-set value. */
  if (strcasecmp(name, "transfer-encoding") == 0) return ccol_invalid_args;
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
