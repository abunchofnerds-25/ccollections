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

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <arpa/inet.h>
#include <chttpserver.h>
#include <cthreadcomm.h>
#include <cthreadpool.h>
#include <cvector.h>
#include <errno.h>
#include <fcntl.h>
#include <internal/catfork.h>
#include <internal/cdeadline.h>
#include <internal/chttp1_parser.h>
#include <internal/csock.h>
#include <internal/cstrutil.h>
#include <internal/ctls.h>
#include <internal/test_helpers/chttpserver.h>
#include <internal/test_helpers/common.h>
#include <internal/test_helpers/cthreadpool.h>
#include <limits.h>
#if defined(__linux__)
#include <linux/sockios.h> /* SIOCOUTQ */
#endif
#include <internal/cascii.h>
#include <internal/clogderive.h>
#include <internal/ctlsmodel.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

/* ========================================================================== */
/*                         INTERNAL TYPES                                     */
/* ========================================================================== */

typedef struct chttpsvr_mw_node chttpsvr_mw_node_t;

/** Linked list node for middleware entries.
 *
 * next is a plain pointer. Every read walks the list with the read-lock of
 * routes_lock held and every write appends under the write-lock, so the lock
 * already gives us the ordering and we don't need atomics here. */
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
  char **param_names; /* comes from the {name} segments */
  int param_count;
  chttpsvr_handler_fn fn;
  void *ctx;
  bool is_streaming;
} chttpsvr_route_t;

/** Sub-router (also used as the root router). */
struct chttpsvr_router {
  char *prefix; /* for example "/api/v1"; "" for the root */
  size_t prefix_len;
  /* How many '/'-delimited segments the prefix has. It's 0 for the root's own
   * "" placeholder and for the special "/" prefix (the doc comment of
   * chttpsvr_subrouter explains that "/" edge case). _create_router computes
   * it once, from a prefix that has no trailing slash and no consecutive
   * slashes. We use it to order the sub-routers for _find_route, which also
   * reads it to know where the group of routers that own a path ends, and to
   * keep max_prefix_seg_count of struct chttpserver up to date (see that
   * field's comment). _prefix_matches never reads it since it walks
   * router->prefix directly: prefixes are short and the operator controls
   * them, so caching that side buys nothing. The percent-decode of the request
   * path is the only thing worth sharing, because every router can repeat it,
   * and max_prefix_seg_count is what lets us share it. */
  int prefix_seg_count;
  chttpsvr_mw_node_t *mw_head;
  chttpsvr_mw_node_t *mw_tail;
  int mw_count;              /* the count of registered middleware; the cap is
                              * _CHTTPSVR_MAX_MW */
  chttpsvr_route_t **routes; /* pointer array; each element is a stable
                              * allocation */
  size_t route_count;
  size_t route_cap;
  /* The running maximum of route->seg_count over the routes of this router.
   * We only ever add routes and never remove one, so a running maximum is
   * always right and never needs recomputing. It bounds how many raw
   * segments of an incoming sub_path the per-router decode cache of
   * _find_route (_seg_cache_t) ever splits and decodes, even when the request
   * path, which the client controls, has many more segments. The doc comment
   * of _seg_cache_build explains why that bound matters. */
  int max_route_seg_count;
  struct chttpserver *srv;       /* Back-pointer. chttpsvr_router_on, _on_stream
                                  * and _use must not read it directly; the
                                  * comment of owner says why. */
  ccol_memmgmt_procs_t *m_procs; /* The server's allocator. It only serves the
                                  * CONTENTS this router owns: the prefix, the
                                  * mw list, the routes array and the route
                                  * data. The chttpsvr_router struct itself
                                  * (this shell) is on purpose not allocated
                                  * through it; see _create_router. */
  /* The chttpsvr handle that owns this router. There are only two cases where
   * it is CHTTPSVR_INVALID. One is a short window for the root router, since
   * _create_router runs before we mint a handle and chttpsvr_create_mp stores
   * the fresh handle here afterwards. The other is any router, the root
   * included, whose server is already destroyed (see _destroy_router). The
   * root router is never handed to a caller as a chttpsvr_router*, so
   * chttpsvr_router_on, _on_stream and _use never resolve this field for it.
   * We set it for the root anyway for a single reason: the exit-time
   * destructor of the shell registry below needs to tell a live root from a
   * destroyed one, the same way it does for every sub-router. The field is
   * _Atomic because _destroy_router writes it without holding any lock that a
   * concurrent reader also holds (there is no such lock, the comment below
   * says why), and with a plain field that would be a real data race.
   *
   * chttpsvr_router_on, _on_stream and _use resolve this handle before they
   * touch srv, routes or mw_head, like every other public mutator does with
   * its chttpsvr handle. But the resolve alone is NOT enough, and that's
   * why _destroy_router never frees the memory of this struct (the "shell").
   * The shell is this field plus prefix, srv, m_procs, routes, route_count,
   * route_cap, mw_head, mw_tail and mw_count, so everything BUT the routes,
   * the mw nodes and the prefix it points to. Every other per-request and
   * per-route allocation of this file does go back to the allocator. The
   * difference comes from the fact that reading this very field dereferences
   * `router` itself, and that happens BEFORE any resolve or pin can run. If
   * the shell's memory had already gone back to the allocator, the read would
   * be a use-after-free, no matter whether the free runs at the same
   * time, is half way through, or finished completely in an earlier destroy.
   * A lock taken AFTER the read can't prevent that, and valgrind reports it as
   * a real use-after-free. Resolving this field doesn't close the hole and an
   * rwlock around the free in _destroy_router doesn't either: the lock only
   * narrows the window and does nothing for a reader that starts after a
   * destroy has finished its free. Since the shell stays allocated for the
   * rest of the process (see the registry below), reading this field is
   * always memory-safe, and the atomic load sees one of two values: a live
   * handle we can resolve, or the CHTTPSVR_INVALID sentinel that
   * _destroy_router stores once the owning server is gone. From there on the
   * ordinary resolve and pin protect every field access, and _destroy_router
   * waits for pending_resolve_count before it frees the CONTENTS of this
   * router, which serializes correctly against them.
   *
   * Two kinds of shell do go back to the allocator, because no caller can hold
   * a pointer to them: the shell of a root router, together with the rest of
   * its server, and, once the registry's exit-time destructor has run, the
   * shell of every router of a server that gets destroyed after it. */
  _Atomic chttpsvr owner;
  /* Where this shell sits in chttpsvr_router_shell_registry, or SIZE_MAX when
   * the registry doesn't hold it. The registry mutex guards it. */
  size_t shell_idx;
};

/** One response header entry stored in the flat headers array. */
typedef struct {
  char *name;
  char *value;
} resp_header_t;

/** The response accumulator for each request.
 *
 * We keep the response headers in a flat dynamic array instead of a linked
 * list, so the duplicate scan of chttpsvr_resp_set_header stays cache-friendly
 * and a header doesn't cost a node allocation. */
struct chttpsvr_resp {
  int status_code;
  resp_header_t *headers; /* flat array; it grows to 2x+8 when it is full */
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

/* How many middleware steps one request may go through, global steps and
 * router steps together. A request that needs more gets a 500. */
#define _CHTTPSVR_MAX_MW 32

/* The Retry-After value, in seconds, of every 503 the server sends unasked:
 * a full worker pool, a full streaming queue, a streaming request that waited
 * past streaming_queue_timeout_ms and a request that waited past
 * body_memory_wait_timeout_ms. All of them mean the server is saturated. Five
 * seconds is several ticks of the sweep that admits and expires waiting
 * requests, which is long enough that a client honouring it doesn't walk
 * straight back into the same saturation and short enough to cost a
 * well-behaved client little. */
#define _CHTTPSVR_RETRY_AFTER_SECONDS "5"

/* Defaults of the slow-client settings of chttpsvr_config_t (each field's doc
 * comment in chttpserver.h has the details). 240 bytes a second after a
 * five-second grace is the same minimum data rate Kestrel and IIS use. */
#define _CHTTPSVR_DEFAULT_MIN_RATE_BPS 240u
#define _CHTTPSVR_DEFAULT_RATE_GRACE_MS 5000u
#define _CHTTPSVR_DEFAULT_PARTIAL_BODY_MEMORY ((size_t)256 * 1024 * 1024)
#define _CHTTPSVR_DEFAULT_MEMORY_WAIT_MS 30000u
#define _CHTTPSVR_DEFAULT_STREAM_QUEUE_TIMEOUT_MS 5000u

/* A hard cap, in ms, on the *total* wall-clock time of one of the small,
 * fixed-shape writes that the library makes itself. There are two of
 * them: the courtesy rejection response (see _conn_reject_and_close) and the
 * "100 Continue" interim line (see _write_interim_continue). The cap applies
 * whatever the operator set for conn->srv->max_response_write_duration_ms;
 * the comment of _response_write_deadline_ok shows how the two combine.
 * Neither write waits on the peer, because a full socket parks the connection
 * with the unsent rest and the reactor resumes it once the socket takes bytes
 * again. But a peer that takes one or two bytes at a time can keep such a
 * write alive as long as it wants, holding a descriptor and the parked state.
 * That's the "trickle forever" pattern which max_response_write_duration_ms
 * closes for a real handler response, except that the knob can be 0 (off),
 * and an operator who turns it off almost certainly means it for the bodies
 * of their own handlers, not for our tiny internal writes. A rejection has no
 * body and only a few header bytes, the interim continue is a fixed 25-byte
 * status line, so even under real load neither has a good reason to take
 * more than a couple of seconds. Without an unconditional bound there is a
 * real denial-of-service surface during normal operation, not only at
 * shutdown: an "Expect: 100-continue" request to any route that accepts a
 * body reaches the interim line, and clients do this all the time (curl does
 * it by default for large uploads). Both the sweep and the resumed write
 * judge the deadline, so a parked write ends at it. On a streaming route the
 * handler thread writes the interim line itself inside chttpsvr_req_read(),
 * where it waits for the client the same way it waits for the body, and this
 * bound is what limits that wait. 2 seconds is plenty for any real network
 * path, loopback or not, to deliver well under a kilobyte, and still short
 * enough to keep the worst case tight. */
#define _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS 2000

#ifdef RUNNING_UNIT_TESTS
/* Tests that look at what a parked rejection does, and not at how long it may
 * last, raise the ceiling with this, so a client reading a large rejection
 * slowly (under valgrind, through a small receive window) still gets all of
 * it. 0 goes back to _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. */
static _Atomic unsigned g_internal_write_ceiling_ms_for_tests;
void _chttpsvr_set_internal_write_ceiling_ms_for_tests(unsigned ms) {
  atomic_store(&g_internal_write_ceiling_ms_for_tests, ms);
}
static inline unsigned _internal_write_ceiling_ms(void) {
  unsigned ms = atomic_load(&g_internal_write_ceiling_ms_for_tests);
  return ms ? ms : _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS;
}
#else
#define _internal_write_ceiling_ms() _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS
#endif

/* reject_pool's thread count isn't a fixed number. chttpsvr_start computes it
 * once as the larger of _CHTTPSVR_REJECT_POOL_MIN_THREADS and half of
 * worker_pool's resolved thread count (the comment at that spot in the
 * function has more). Every rejection response goes through here, not just
 * the 503 for a full pool (see _conn_reject_via_pool), so we size it as a
 * small dedicated pool and not as one thread that is "good enough for an
 * overload corner case". A burst of requests for unmatched routes happens
 * every day, much more often than a worker_pool that stays full, and such a
 * burst needs its own concurrency to drain fast instead of queueing behind a
 * single thread. Following worker_pool's size lets reject_pool grow on a big
 * machine with many cores, where a constant tuned for the much smaller
 * default would stay too small. The floor keeps a server with one or a few
 * workers from falling back to a one-thread bottleneck. */
#define _CHTTPSVR_REJECT_POOL_MIN_THREADS 2

/* Capacity of reject_pool's task queue. We keep it bounded rather than
 * unbounded like CHTTPSVR_QUEUE_UNBOUNDED (0), since otherwise a flood of
 * rejected connections that never stops grows reject_pool's backlog without
 * limit, and that backlog is our own memory (one queued chttpsvr_conn_t per
 * waiting reject-close). When the queue is full ctpool_try_submit fails and
 * _conn_reject_via_pool falls back to closing synchronously on the calling
 * thread, the same fallback that runs when reject_pool isn't there at all,
 * during a shutdown for example. The size is generous because every
 * rejection path feeds this one queue and not only the 503 of a full pool. */
#define _CHTTPSVR_REJECT_POOL_QUEUE_CAP 1024

/* Snapshot entry for one middleware step. */
typedef struct {
  chttpsvr_middleware_fn fn;
  void *ctx;
} _mw_entry_t;

/** Middleware dispatch state. It lives inside chttpsvr_conn_t, which is on the
 * heap and stays alive for the whole keep-alive connection, so it's neither
 * on the stack nor allocated per request. We reset it for each request just
 * like the other per-request scratch fields of that struct. */
typedef struct dispatch_ctx {
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  _mw_entry_t
      mw_snap[_CHTTPSVR_MAX_MW]; /* fn and ctx pairs, copied under the lock */
  int mw_count;                  /* how many elements the snapshot holds */
  int mw_idx;                    /* the next element to dispatch */
} dispatch_ctx_t;

/* A small growable buffer. A buffered route collects the whole body in it,
 * and a streaming route keeps in it the body bytes that chttpsvr_req_read
 * hasn't taken yet. See the comment of _on_body. */
typedef struct {
  char *buf;
  size_t cap;
  size_t len; /* valid bytes in the buffer right now */
  size_t pos; /* streaming route only: how much of [0,len)
               * chttpsvr_req_read has already given to the caller */
} growbuf_t;

typedef enum {
  CONN_ST_TLS_HANDSHAKE,
  CONN_ST_READING_HEADERS,
  CONN_ST_DIVERTED, /* A worker thread owns the fd. If there is a reactor
                     * registration we pause it with ccol_event_loop_pause
                     * instead of removing it, so no events fire but the
                     * registration stays alive and ccol_event_loop_resume
                     * brings it back cheaply for the next request. We never
                     * rebuild it from scratch. */
  CONN_ST_CLOSING
} conn_state_t;

/** The per-connection object. It lives across all the keep-alive requests of
 * the connection, and _conn_reset_for_request() clears its per-request
 * scratch fields (method, path, headers, route, dispatch state, response and
 * body buffers) before each new request. It's on the heap and owned either by
 * the reactor's ccol_event_reg or by a worker's ctpool task, never by both at
 * once. The header-reading callback and _task_worker are the two places
 * where it changes hands. */
typedef struct chttpsvr_conn {
  int fd;
  struct chttpserver *srv;
  ccol_event_reg reg; /* current registration, CCOL_EVENT_REG_INVALID while
                       * the connection is diverted */
  ctls_conn_t *tls;
  conn_state_t state;
  chttp1_parser_t parser;

  /* The scratch fields for each request. */
  chttp_method_t method;
  /* Owned and RAW, so still percent-encoded, and route matching must work on
   * exactly this form. _match_route_cached and _prefix_matches split on a
   * literal '/' and decode every segment separately, which is the only way to
   * tell a real path separator from a %2F inside a {param} segment. Decoding
   * the whole path once, here, looks cheaper, but it's wrong: %2F would turn
   * into a real '/' and one param segment would silently become two. Such a
   * decode would also have to reject the whole request for any malformed %XX
   * anywhere in the path, while the tests of this module pin another
   * contract: a malformed encoding in one segment is an ordinary route
   * mismatch (404) and not a 400. _seg_cache_get further down already treats
   * a failed decode as "this segment does not match". */
  char *path;
  /* Owned and fully URL-decoded. _on_headers_complete() fills it once a route
   * matched, and the decode can't fail there because the match has already
   * shown that every segment decodes cleanly. chttpsvr_req_path() returns
   * this value. */
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
  /* allowed_methods of the route match, for the Allow header of a 405.
   * _on_headers_complete sets it along with reject_status 405, and the only
   * reader is _conn_reject_and_close, for that status only. */
  uint8_t allowed_methods;
  /* True for "OPTIONS *", the asterisk-form request of RFC 9112 SS3.2.4.
   * The server answers it directly, with no route, middleware or handler;
   * see _options_star_route. */
  bool options_star;
  bool expects_continue;
  /* Set once we have written the "HTTP/1.1 100 Continue\r\n\r\n" interim
   * response for this request. It's sent from two places, _task_worker for a
   * buffered route and chttpsvr_req_read for a streaming one, and the flag
   * keeps a streaming handler that calls chttpsvr_req_read() several times
   * from writing the interim line twice. */
  bool interim_continue_sent;
  /* Set when _write_interim_continue() wrote less than the full
   * "HTTP/1.1 100 Continue\r\n\r\n" line, because the write failed, timed out
   * or hit the max_response_write_duration_ms deadline. This is a real short
   * write and not a write of nothing: whatever bytes did go out stay on the
   * wire, so anything we write afterwards on this connection lands behind a
   * truncated status line and breaks the client's framing. And if the peer is
   * already gone such a write is useless anyway. _task_worker reads the flag,
   * skips the real final response and closes the connection, so we don't keep
   * adding bytes to a stream the client may already consider malformed. */
  bool interim_write_failed;
  growbuf_t body;         /* the whole body for a buffered route, or the
                           * bytes waiting for a streaming route */
  size_t body_bytes_seen; /* running total for max_body_size */
  bool body_too_large;
  /* Only _on_body sets this, and only when growing the body buffer fails,
   * i.e. a real _ccol_mem_realloc failure while body bytes come in, never a
   * max_body_size rejection. We keep it apart from body_too_large and
   * transfer_aborted because chttpsvr_req_stream_error() of a streaming route
   * must report a real allocation failure on the server as
   * ccol_not_enough_memory. ccol_http_transfer_aborted means "the connection
   * closed, or the framing is malformed", which is clearly not what happened.
   * Against transfer_aborted this flag gets the same priority body_too_large
   * already has, so chttpsvr_req_stream_error() checks it first. In practice
   * the two can't both be set, since _on_body returns 1 and aborts the parse
   * the first time it meets either condition and no later call of the same
   * request gets to set the other one, but the order would still matter if
   * that ever changed. */
  bool body_alloc_failed;
  /* Set when chttpsvr_req_read() of a streaming route meets a truncated body,
   * i.e. an EOF from the peer or a hard I/O error before the message has
   * finished its framing. It's also set for a chttp1_parser level
   * CHTTP1_USER or CHTTP1_ERROR that is neither a max_body_size rejection nor
   * a body_alloc_failed one, because those two have their own flags with a
   * higher priority (see the order in chttpsvr_req_stream_error()). Only
   * chttpsvr_req_stream_error() reads it. The buffered-route path
   * (_drain_body) doesn't need it, since it returns ccol_http_transfer_aborted
   * directly instead of leaving a flag in conn for the caller to read back. */
  bool transfer_aborted;
  /* True when the request BODY didn't arrive the way its own framing
   * promised, so we stopped reading it. Two things set it. One is a
   * CHTTP1_ERROR from chttp1_parser while it reads the body (a chunk size
   * that isn't hexadecimal, a chunk with no terminating CRLF, a malformed
   * trailer line). The other is an EOF before the message was complete (a
   * body shorter than its own Content-Length, or a chunked body that never
   * sends the zero-length chunk).
   *
   * Both are client errors and RFC 7231 SS6.5.1 gives them 400. Without the
   * flag they all reach _task_worker as the same ccol_http_transfer_aborted
   * that a hard I/O error produces, and the buffered-route path answers 500
   * Internal Server Error for every one of them. That tells the operator the
   * server broke when it was the client, and puts a client fault into the 5xx
   * rate.
   *
   * max_body_size and a failed allocation keep their own flags and statuses
   * (413 and 500). This flag never competes with them because _drain_body
   * only sets it after it has ruled both out.
   *
   * The streaming-route path ignores it. chttpsvr_req_stream_error() keeps
   * reporting ccol_http_transfer_aborted for all of these cases, which is its
   * documented value for "the connection closed, or the framing is
   * malformed". A streaming handler picks its own status, so we must not
   * change what that accessor answers. */
  bool body_malformed;

  /* Bytes left in the reactor's read buffer after the header parse, past
   * chttp1_parser_consumed(). We have to copy them out (see
   * _conn_start_diverted) because that buffer is on the reactor thread's
   * stack and dies with the callback that filled it. _task_worker frees them
   * once chttp1_stream_prepare() or chttp1_stream_prepare_tls() has copied
   * them into its own heap buffer. */
  char *_carry_over;
  size_t _carry_over_len;

  /* Bookkeeping for max_body_read_duration_ms. */
  struct timespec read_deadline;
  bool read_deadline_set;
  bool deadline_exceeded;

  /* Bookkeeping for max_response_write_duration_ms; see the conn and
   * is_reject parameters of _send_response, _response_write_deadline_ok and
   * _shrink_timeout_to_deadline. Three sends read these fields: the real
   * response of a matched route in _task_worker, the courtesy response of a
   * rejection in _conn_reject_and_close, and the "100 Continue" interim line
   * in _write_interim_continue. The last two also get
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS as a hard ceiling on top of whatever
   * max_response_write_duration_ms allows. */
  struct timespec write_deadline;
  bool write_deadline_set;

  /* Bookkeeping for the idle timeout sweep. srv->idle_mutex guards the list
   * fields. */
  struct timespec last_activity;
  struct chttpsvr_conn *idle_prev, *idle_next;
  bool in_idle_list;

  /* Bookkeeping for the registry of diverted connections (see the comment of
   * diverted_mutex, diverted_head and diverted_tail in struct chttpserver).
   * Every connection a worker thread owns (CONN_ST_DIVERTED) is linked here,
   * so the teardown can find the connection of a stuck worker and force a
   * shutdown(2) on it. srv->diverted_mutex guards the list fields, the same
   * way as for the idle list above. */
  struct chttpsvr_conn *diverted_prev, *diverted_next;
  bool in_diverted_list;

  /* This counter tells us when the struct can be freed. It follows the
   * refcount design of ccol_event_reg in cthreadcomm.c, which is 1 while the
   * object is registered plus 1 per callback in flight. It starts at 1, which
   * stands for "the application hasn't closed this connection yet" (see
   * _conn_create), and gets +1 for every conn->reg registration we create for
   * the connection, so for every successful ccol_event_loop_add call site
   * below. It gets -1 exactly once from _conn_close, the application's "done
   * with conn" decision, and -1 exactly once per on_removed that fires
   * (_conn_on_removed). cthreadcomm promises that on_removed fires exactly
   * once for each registration that went live, asynchronously, and only when
   * no dispatch of that registration can touch conn any more. _conn_free only
   * runs when the counter reaches 0.
   *
   * A single on_removed isn't enough because a connection can go through
   * more than one registration episode in its life. A failed pause (see
   * _conn_pause_reg), for example, removes the registration synchronously,
   * either from that registration's own dispatch or from the sweep, which
   * holds the connection's claim. conn survives, and later _conn_park_arm or
   * the keep-alive tail of _task_tail registers the descriptor again for the
   * next wait while the earlier registration's on_removed may still be
   * pending. Freeing on the first on_removed would free conn while a later
   * episode, or the application's close decision, is still open. Adding up
   * every contribution, in whatever order they come, gives exactly the
   * condition we want: the application asked for a close AND every
   * registration we created is proven safe to free. */
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs_t *m_procs;

  /* Bookkeeping for max_header_read_duration_ms. These two members are placed
   * after all the others on purpose: the read and dispatch path reads the
   * members in front of them, and appending keeps every one of those offsets
   * where it was. header_phase_start is when the reactor-owned phase of the
   * CURRENT request began, which is the first handshake step for a TLS
   * connection and the first byte of the request's header block otherwise.
   * header_phase_active tells whether the field holds a meaningful instant.
   * _conn_reset_for_request clears it, so the next request on a keep-alive
   * connection gets its budget from its own first byte; it doesn't inherit
   * the start of the previous request and isn't charged for the idle gap in
   * between.
   *
   * They are plain fields and not atomics because we only write them while
   * the connection is OUT of the idle list: a dispatch claims it out first
   * and _idle_list_add publishes it again once the writes are done. The idle
   * sweep is the only reader and it holds srv->idle_mutex for its whole walk,
   * so that mutex orders the two sides, the same edge last_activity above
   * relies on. */
  struct timespec header_phase_start;
  bool header_phase_active;

  /* Slow-client state. Everything below exists for one rule: for a buffered
   * route a worker thread never waits on a client socket. These members come
   * last so the offsets of everything the read and dispatch path reads stay
   * the same.
   *
   * _on_headers_complete writes divert_gate for every matched request and
   * _conn_start_diverted reads it. 0 means "divert as usual", SIZE_MAX means
   * "a streaming route that runs on the streaming pool", and anything else is
   * the Content-Length of a buffered body, which reserves body memory
   * unless the whole body already came with the headers. So in the ordinary
   * case a single compare with the carry-over length decides. */
  size_t divert_gate;

  /* Which wait the connection is in while no thread owns it, one of the
   * _CONN_PARK_* values. The thread that parks the connection writes it
   * before publishing the connection into the idle list, the memory-wait list
   * or the streaming queue, and the thread that claims it back out of that
   * list reads it, with the list's lock ordering the two. park_pending is the
   * same kind of value, which the body-read helper sets for the worker that
   * is about to park the connection. */
  uint8_t park;
  uint8_t park_pending;
  /* The direction (a ccol_select_dir value) reg was last added or modified
   * with, so a wait in the same direction skips the ccol_event_loop_modify
   * call; see _conn_reg_modify. Like reg, only the connection's owner touches
   * it. */
  uint8_t reg_dir;

  /* The rate floor of chttpsvr_config_t.min_transfer_rate_bps and the per-gap
   * limit of a parked connection. rate_start is where the current body read
   * or response write started, rate_paused_ms is how much of that time the
   * server itself held the request back, and rate_bytes counts the bytes
   * moved since rate_start. last_progress is the last time any bytes moved.
   * rate_queued is how many unread bytes the sweep last saw in the socket
   * while the receive low-water mark was above 1. All of them run on the
   * _slow_clock clock. */
  bool rate_active;
  struct timespec rate_start;
  long long rate_paused_ms;
  uint64_t rate_bytes;
  struct timespec last_progress;
  size_t rate_queued;
  /* The SO_RCVLOWAT the socket has right now (0 and 1 both mean the default
   * of 1). rcvlowat_cap is the highest mark the receive buffer can reach; we
   * read it on the first park and it stays 0 until then. */
  int rcvlowat;
  int rcvlowat_cap;

  /* Partial-body memory of chttpsvr_config_t.max_partial_body_memory. As
   * soon as the connection holds a charge or waits, srv->wait_mutex guards
   * every member of this group, and apart from that only the connection's
   * owner reads them. mem_charged is the reservation the connection holds
   * from before its body is read until _conn_drop_body frees the body.
   * mem_exempt says it went past the limit under the progress rule and may
   * reserve freely while its body comes in; it ends together with the
   * reservation. mem_need is what a waiter is asking for. */
  size_t mem_charged;
  size_t mem_need;
  bool mem_exempt;
  bool in_mem_wait;
  struct timespec mem_wait_start;
  struct chttpsvr_conn *mem_prev, *mem_next;

  /* When a parked connection left its wait and went to the worker pool, on
   * CLOCK_MONOTONIC and on _slow_clock. _task_resume judges the park limits
   * at this instant, and whatever time the connection then spends in the
   * pool's queue is charged to the server, not the client (see
   * _conn_uncharge_queue_wait). The submitting thread writes both, and the
   * submit orders them before the worker reads them. */
  struct timespec claim_mono;
  struct timespec claim_slow;

  /* The streaming queue. srv->wait_mutex guards the links and the flag. */
  bool in_stream_queue;
  bool sq_has_deadline;
  struct timespec sq_deadline;
  struct chttpsvr_conn *sq_prev, *sq_next;

  /* A response whose write stopped on a full socket. wp_head holds the unsent
   * part of the header block (the response builder keeps the block on its
   * stack), and wp_body_pos is how much of resp.body has gone out already. */
  bool wp_active;
  bool wp_keep_alive;
  bool wp_no_body;
  /* True when it was the TLS layer and not the socket that stopped the
   * write, since a TLS write may need to read first. The park then waits for
   * readability. */
  bool wp_wants_read;
  /* True when the parked write is a rejection's courtesy response, which ends
   * in a close or a lingering close and never in keep-alive; see
   * _conn_reject_and_close. */
  bool wp_reject;
  /* True when the parked write is the "100 Continue" interim line of a
   * buffered route, after which the body gets read (see _task_resume). The
   * carry-over then holds the body bytes that came along with the headers. */
  bool wp_interim;
  char *wp_head;
  size_t wp_head_len;
  size_t wp_head_pos;
  size_t wp_body_pos;
  /* The lowest number of bytes the kernel still had unsent or unacknowledged
   * in the send queue (SIOCOUTQ) of a parked response, as seen by the park
   * and the sweep, or SIZE_MAX when the kernel doesn't report it. A queue
   * that shrinks below this value counts as the reader making progress; see
   * _parked_expired. */
  size_t wp_outq;

  /* The server's parked list (see parked_head in struct chttpserver).
   * srv->idle_mutex guards these fields just like the idle list. */
  struct chttpsvr_conn *parked_prev, *parked_next;
  bool in_parked_list;

  /* A lingering close, see _conn_linger. linger_deadline is on
   * CLOCK_MONOTONIC. */
  struct timespec linger_deadline;
} chttpsvr_conn_t;

/* The waits a connection can be in while no thread owns it (see
 * chttpsvr_conn_t.park). _CONN_PARK_BODY, _CONN_PARK_WRITE and
 * _CONN_PARK_LINGER are on the parked list with the reactor registration
 * armed for the direction the socket has to reach. The two memory waits are
 * on the memory-wait list and the streaming wait is on the streaming queue,
 * and those three keep the registration paused. */
enum {
  _CONN_PARK_NONE = 0,
  _CONN_PARK_BODY = 1,      /* the body of a buffered route stopped arriving */
  _CONN_PARK_WRITE = 2,     /* the response stopped on a full socket */
  _CONN_PARK_MEM_START = 3, /* a Content-Length body waits to be admitted */
  _CONN_PARK_MEM_BODY = 4,  /* a chunked body waits for more memory */
  _CONN_PARK_STREAM = 5,    /* a streaming request waits for a thread */
  _CONN_PARK_LINGER = 6     /* a closing connection discards what arrives */
};

/** The per-request object a handler or a middleware gets: a thin view over a
 * chttpsvr_conn_t plus the worker's chttp1_stream_t. The stream can't live
 * on chttpsvr_conn_t itself because it only exists, and is only valid, while
 * one worker drives the body of one request. */
/* One value of chttpsvr_req_set_context(). A pair that holds a value never
 * has a NULL key. */
typedef struct {
  const void *key;
  void *value;
} _chttpsvr_ctx_pair_t;

/* The chttpsvr_req_set_context() values after the first one (the first lives
 * in the request itself). */
typedef struct {
  size_t count;
  size_t cap;
  _chttpsvr_ctx_pair_t pairs[];
} _chttpsvr_ctx_more_t;

struct chttpsvr_req {
  chttpsvr_conn_t *conn;
  chttp1_stream_t *stream;  /* the worker's own; NULL until the worker sets
                             * it up */
  const char **param_names; /* borrowed from the route (const aliases) */
  chttpsvr_qparams_t *_qparams;
  const char **_qresult;
  size_t _qresult_cap;
  ccol_memmgmt_procs_t *m_procs;
  /* The first chttpsvr_req_set_context() value, which costs no allocation;
   * _ctx_first.key stays NULL while the request holds none. Any further value
   * goes into _ctx_more, which _destroy_req_scratch frees. */
  _chttpsvr_ctx_pair_t _ctx_first;
  _chttpsvr_ctx_more_t *_ctx_more;
  bool _qparams_attempted;
  bool _qparams_parse_oom;
  bool _qresult_oom;
};

/* ========================================================================== */
/*                         CHTTPSVR HANDLE SLOT TABLE                         */
/* ========================================================================== */

/* chttpsvr is an opaque value handle whose top 32 bits are the slot index and
 * bottom 32 bits the generation (the doc comment on the typedef in
 * include/chttpserver.h has the details). We resolve the handle through this
 * table before touching the struct chttpserver* behind it, and that's what
 * lets __chttpsvr_destroy report a _ccol_fatal_err instead of a
 * use-after-free or a double free, both for a concurrent double destroy
 * (racing another destroy of the same live handle) and for a sequential one
 * (a stale handle from a destroy that already finished). A slot is marked
 * not-in-use the moment it's freed and its generation goes up on every reuse,
 * so a stale handle can never alias a later, unrelated server in the same
 * slot. The table works exactly like chttpcli_slot_table in chttpclient.c,
 * and the section comment there explains the whole design, the alternatives
 * it rules out and the resolve-then-use race it closes, so we don't repeat
 * it here. */
typedef struct {
  struct chttpserver *ptr; /* NULL when the slot is free */
  uint32_t generation;     /* fresh on every acquire and monotonic per slot
                              index; 0 before the first use, 1 after the
                              first acquire */
  bool in_use;
} chttpsvr_slot_t;

static struct {
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
  cvec slots; /* cvec of chttpsvr_slot_t; it only grows through
                 push_back, and an index stays valid for good once
                 it has been allocated */
  /* True when the exit-time destructor found a live server and left the
   * table's release to the destroy of the last one. */
  bool release_deferred;
  cvec free_indices; /* cvec of uint32_t, a LIFO free list for O(1)
                        reuse */
} chttpsvr_slot_table = {0};

/* Becomes true once a thread enters _chttpsvr_slot_table_init_globals, which
 * every path that can create or resolve a server enters first. The exit-time
 * destructor uses it to tell a process that never touched this module from
 * one that did. The init does more than its own work, since it also creates
 * the slot tables of clogger and cthreadpool and registers their fork
 * handlers, and at exit those modules may have already run their own
 * destructors before ours. */
static atomic_bool chttpsvr_slot_table_entered = false;

#if CCOL_FORK_SAFETY_REQUIRED
/* Forward declarations. The bodies come further down, after struct
 * chttpserver, srv_engine_bundler, servers_bundler and
 * chttpsvr_router_shell_registry are declared, since the handlers dereference
 * and lock all of them. _chttpsvr_slot_table_init_globals below registers the
 * handlers, and it's the first thing in the file that runs once, lazily, when
 * the module is used for the first time. It's the same
 * forward-declare-then-define-after-the-struct layout as in cthreadpool.c and
 * cthreadcomm.c.
 *
 * Defining CCOL_FORK_SAFETY_REQUIRED to 0 turns off this whole fork() safety
 * mechanism (the three handlers and their _ccol_at_fork() registration
 * below); see the doc comment of that macro in common.h. */
static void _chttpsvr_atfork_prepare(void);
static void _chttpsvr_atfork_release(void);
static void _chttpsvr_atfork_child_release(void);
#endif

static void _chttpsvr_slot_table_init_globals(void) {
  atomic_store(&chttpsvr_slot_table_entered, true);
  if (_ccol_mutex_init(chttpsvr_slot_table.mutex) != 0)
    _ccol_fatal_err("chttpsvr slot table: failed to initialize mutex");
  chttpsvr_slot_table.slots = cvector_create(sizeof(chttpsvr_slot_t), NULL);
  if (!chttpsvr_slot_table.slots)
    _ccol_fatal_err("chttpsvr slot table: failed to allocate slots vector");
  chttpsvr_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpsvr_slot_table.free_indices)
    _ccol_fatal_err(
        "chttpsvr slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  /* fork() only duplicates the calling thread (the doc comment of
   * _chttpsvr_atfork_prepare describes the whole hazard). Long story short,
   * the child can inherit a lock some other thread was holding, and since
   * that thread doesn't exist in the child the lock stays locked forever.
   *
   * These three calls make the _ccol_at_fork() triples of cthreadcomm (owner
   * of ccol_event_loop), cthreadpool and clogger register NOW, strictly
   * before this module's own _ccol_at_fork() call below. pthread_atfork runs
   * prepare handlers in REVERSE registration order, so with this order
   * _chttpsvr_atfork_prepare always runs FIRST at every later fork(). It
   * takes our own locks first (srv_engine_bundler.mutex,
   * servers_bundler.mutex, the mutex of each live server and so on), and only
   * after that do the prepare handlers of ccol_event_loop, ctpool or clogger
   * take theirs: ccol_event_loop_slot_table.mutex, ctpool_slot_table.mutex,
   * clog_slot_table.rwlock, and the internal locks of a live ccol_event_loop,
   * a live pool or a live clog_shared_t.
   *
   * Every ordinary call in this file already nests the locks in that order.
   * _engine_acquire holds srv_engine_bundler.mutex for its whole body, which
   * includes its nested call into ccol_event_loop_create_mp() and its own
   * direct clog_create_from_fd_mp() call when there is no engine-wide logger
   * yet. The _SRV_ENGINE_LOG macro holds the same mutex around every
   * clog_info, clog_warn and similar call it makes through an open logger.
   *
   * Without these calls the lock order at fork() time would depend on which
   * subsystem the embedding application happens to use FIRST, which is pure
   * chance and can come out reversed against this nesting. For
   * ccol_event_loop and ctpool that gives two real lock-order-inversion
   * cycles, srv_engine_bundler.mutex against
   * ccol_event_loop_slot_table.mutex and a three-way one that also contains
   * ctpool_slot_table.mutex. ThreadSanitizer reports both through the
   * test_engine_stop_tsan target of tests/chttpserver.
   *
   * clogger has the same shape of hazard, and you can hit it without
   * ThreadSanitizer. Every public chttpsvr_* function that resolves a handle
   * (chttpsvr_start, _stop, _use, _register_handler,
   * _register_streaming_handler and _subrouter) accepts an invalid handle and
   * reports it as a plain error rather than making the caller prove the
   * handle valid first. But each of them still runs this module's
   * _ccol_call_once through _chttpsvr_resolve before checking the handle. So
   * a process that makes such a call before using clog for anything else
   * would, without the forcing below, register our _ccol_at_fork() with no
   * clogger registration in front of it, and a later independent first use
   * of clog would register AFTER us, the opposite of the order we need. The
   * doc comment of queue_mutex_registry in cthreadcomm.c describes the same
   * kind of hazard for another pair of subsystems, with the same remedy:
   * force a fixed registration order instead of leaving it to chance.
   *
   * Calling all three every time this function runs is harmless since each
   * has its own _ccol_call_once guard. The caller may already have forced
   * some of them through its own unrelated use of ccol_event_loop, ctpool or
   * clog, in which case the call sees the handler registered and returns
   * at once. */
  _cthreadcomm_ensure_atfork_registered_before_caller();
  _ctpool_ensure_atfork_registered_before_caller();
  _clog_ensure_atfork_registered_before_caller();
  _ccol_at_fork(_chttpsvr_atfork_prepare, _chttpsvr_atfork_release,
                _chttpsvr_atfork_child_release);
#endif
}

#if defined(_CCOL_ATFORK_AT_LOAD)
/* See _CCOL_ATFORK_AT_LOAD in common.h. */
__attribute__((constructor)) static void _chttpsvr_atfork_at_load(void) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
}
#endif

/* A chttpsvr handle's lifecycle has two independent dimensions rather than
 * one linear state machine: start/stop, and a one-shot quiesce latch. A
 * caller can take the CLAIM of the latch right away, independently of
 * start/stop, and a separate mechanism (`pending_resolve_count`) defers the
 * real work. Each dimension gets its own enum, which keeps the claim
 * independent and gives each dimension its own `-Wswitch` coverage.
 * `-Wswitch` comes with this project's standing `-Wall` plus `-Werror`, so if
 * somebody adds an enumerator later and a call site doesn't handle it, the
 * build fails instead of silently compiling. Every enumerator has an explicit
 * value, the same rule as for `ccol_retval_t` in common.h and for the same
 * reason: we never want to depend on the auto-increment of declaration
 * order. */
typedef enum {
  CHTTPSVR_LC_IDLE = 0,     /* not listening; chttpsvr_start() may go ahead */
  CHTTPSVR_LC_STARTING = 1, /* chttpsvr_start() is doing its real work */
  CHTTPSVR_LC_RUNNING = 2,  /* listening and serving */
  CHTTPSVR_LC_STOPPING = 3, /* _chttpsvr_stop_internal() is doing its real
                             * work, on the way back to IDLE */
} chttpsvr_lifecycle_t;

typedef enum {
  CHTTPSVR_QS_NOT_QUIESCED = 0, /* _quiesce_server_once() may claim and
                                 * run */
  CHTTPSVR_QS_QUIESCING = 1,    /* its real teardown work is running */
  CHTTPSVR_QS_QUIESCED = 2,     /* the teardown is finished. It's an
                                 * idempotent latch and only a later,
                                 * legitimate chttpsvr_start() resets it to
                                 * NOT_QUIESCED. */
} chttpsvr_quiesce_state_t;

/** The main struct of the server. */
/* What a unix:// listener needs to remove its own socket file when it stops,
 * and nothing more. path is our own copy of the configured path. dir_fd is
 * the directory holding the socket, opened when the server bound it, and
 * base is the socket's name inside it. Every later lookup goes through
 * dir_fd, so an application that chdir()s after the start can't point the
 * cleanup at some other file. dev and ino identify the socket this server
 * bound, and the cleanup only unlinks the name while it still names that
 * socket, so whatever somebody else put at the path in the meantime (another
 * server's socket included) survives. */
typedef struct {
  char *path;
  const char *base;
  int dir_fd;
  dev_t dev;
  ino_t ino;
} _chttpsvr_unix_bind_t;

struct chttpserver {
  /* [0] is the root and [1..n] are the sub-routers, kept sorted by
   * prefix_seg_count from the largest to the smallest (see the sorted insert
   * in chttpsvr_subrouter and the walk order in _find_route). */
  chttpsvr_router **routers;
  /* routers[0], copied once at create time. The root router shell never
   * moves and never goes away while the server lives, so we write this field
   * once before the handle is published and read it without routes_lock.
   * Reading routers[0] itself does need the lock, because chttpsvr_subrouter
   * may realloc the routers array under the write lock. */
  chttpsvr_router *root_router;
  size_t router_count;
  size_t router_cap;
  /* The running maximum of prefix_seg_count over the non-root routers of
   * this server. Routers are only ever added, never removed, so the running
   * maximum is always right without a recompute. routes_lock guards it the
   * same way it guards router_count, router_cap and routers: when the new
   * value is higher, chttpsvr_subrouter updates it next to
   * raw->routers[raw->router_count++] = r, both writes in the same
   * write-locked critical section. With it _find_route can build ONE shared
   * decode cache, modelled on the per-router match cache of _seg_cache_t,
   * that remembers the leading segments of the request path. _find_route
   * reuses that cache in the _prefix_matches call of every non-root router;
   * without it each router would percent-decode the same request path
   * segments again from scratch (see the comment of _find_route at the call
   * site). The cache's split is bounded by this value, which the operator
   * controls, and not by the request path's real segment count, which the
   * client fully controls, for the same reason as max_route_seg_count. */
  int max_prefix_seg_count;
  ctpool worker_pool;
  /* A pool of dedicated threads with a bounded queue. It has the larger of
   * _CHTTPSVR_REJECT_POOL_MIN_THREADS and half of worker_pool's resolved
   * thread count (see the comment of that constant), and a queue of
   * _CHTTPSVR_REJECT_POOL_QUEUE_CAP. Every _conn_reject_and_close call runs
   * here, i.e. the courtesy write-then-close a rejected connection gets plus
   * whatever middleware may run before it. The write never waits on the peer
   * since a full socket parks the connection. Two cases end up in this pool:
   * a 404, 405 or 500 for an unmatched or malformed route, which the header
   * parse finds synchronously, and a 503 because worker_pool is already full.
   * It is on purpose NOT worker_pool itself, the 503 case included. We want
   * the middleware of a rejection off the reactor thread, but that work also
   * must not wait on, or compete with, the very pool that just turned the
   * request away for being at capacity. The queue is bounded because without
   * a bound a flood of rejections that never stops grows this pool's backlog
   * without limit. When the queue is full _conn_reject_via_pool falls back to
   * closing synchronously on the calling thread; the comment of that function
   * explains why the pool exists in the first place. */
  ctpool reject_pool;
  ctls_ctx_t *tls_ctx; /* not NULL when the caller configures TLS */
  _ccol_mutex_t mutex;
  _ccol_cond_var_t requests_done_cv;
  int in_flight_requests;
  /* _chttpsvr_resolve pins this counter with a lock-free atomic increment.
   * The pin is held while a caller has a struct chttpserver* it just resolved
   * and hasn't yet handed over to its own tier-specific protection. This
   * module's public API is short and synchronous (chttpsvr_start, _stop,
   * _register_handler, _register_streaming_handler, _use and _subrouter), and
   * each of those holds its pin for its whole body, so the pin lasts exactly
   * as long as that one call. _chttpsvr_resolve_unpin drops the pin under
   * `mutex` together with the broadcast that wakes a waiting destroy, and the
   * decrement itself must be under the lock too, not only the broadcast
   * (the comment of that function, and the identical _chttpcli_resolve_unpin
   * in chttpclient.c, say why). __chttpsvr_destroy blocks until the counter
   * is 0 and only then frees the object, which closes the real
   * resolve-then-use race a naive resolve (look the handle up, unlock, return
   * the pointer) would leave open. We keep it apart from in_flight_requests
   * above on purpose, with its own resolve_cv instead of requests_done_cv.
   * Every user of a chttpsvr handle holds this pin for its whole synchronous
   * call, so we never wait on the two counters together. That's
   * different from the pending_resolve_count and in_flight_count pair in
   * chttpclient.c, where chttpclient_do has requests that stay in flight for
   * a long time; nothing here hands protection over, half way through a call,
   * to some other mechanism that drains later. */
  _Atomic size_t pending_resolve_count;
  /* Pinned only by _engine_force_stop_quiesce_all, for a bare struct
   * chttpserver* it reads straight out of servers_bundler.servers[] (the doc
   * comment of that function explains why that pointer needs protecting at
   * all). This is on purpose a SEPARATE counter and not a second use of
   * pending_resolve_count. The winner path of _quiesce_server_once waits for
   * pending_resolve_count to drop to 0 before doing any real work, so a
   * caller holding a pending_resolve_count pin across its own
   * _quiesce_server_once call would wait for a pin only it can release, a
   * real self-deadlock. _quiesce_server_once never waits on this counter;
   * only __chttpsvr_destroy does, just before freeing raw, so there's no such
   * cycle here. It broadcasts resolve_cv, the same condition variable the
   * drain of pending_resolve_count uses, because both are conditions
   * __chttpsvr_destroy may wait on before raw is safe to free and a change in
   * one is a reason to check the other again. */
  _Atomic size_t servers_bundler_pins;
  /* _listener_on_readable pins this counter for one whole dispatch, i.e. its
   * own accept4() loop and also the pause of the listener registration when a
   * resource-exhaustion condition doesn't go away (see
   * _listener_pause_for_resource_pressure). The pin is dropped right before
   * the dispatch returns.
   *
   * What it protects is the listener fd, not srv's memory. srv's memory is
   * safe during the dispatch through another mechanism: every listener
   * registration holds one srv->lifetime_refs reference, and its on_removed
   * (_listener_on_removed) only releases it once no dispatch of that
   * registration can still run (see the comment of that field).
   * ccol_event_loop_remove() doesn't wait for a dispatch that has already
   * started, so without that reference a destroy would free srv under such a
   * dispatch.
   *
   * The fd needs the counter because _chttpsvr_stop_internal closes it right
   * after removing the listener, while a running dispatch may be just about
   * to call accept4() on it. _chttpsvr_stop_internal clears listen_fd under
   * srv->mutex before the removal, and the dispatch takes this pin under the
   * same mutex and then compares its own registration's fd with listen_fd. A
   * dispatch that pins first holds the close back until it returns, and one
   * that pins after the clear sees a different listen_fd and returns without
   * touching the fd or anything else of srv.
   *
   * It's a SEPARATE counter and not another use of pending_resolve_count for
   * the same self-deadlock reason that gives servers_bundler_pins its own:
   * chttpsvr_stop() resolves srv and pins pending_resolve_count for its whole
   * call, _chttpsvr_stop_internal included, so a wait on
   * pending_resolve_count anywhere inside that call would wait for a pin only
   * the same call can release. THIS counter, on the other hand, is safe to
   * wait on from inside _chttpsvr_stop_internal, _quiesce_server_once or
   * chttpsvr_stop() itself, and they do wait on it. Only the reactor thread
   * running _listener_on_readable holds the pin, and the thread calling
   * chttpsvr_stop(), _quiesce_server_once or __chttpsvr_destroy never does,
   * so the waiting thread is never the one that needs to run for the counter to
   * reach 0 and the wait can't self-deadlock like a wait on the other two.
   * _chttpsvr_stop_internal waits for it right after its own
   * ccol_event_loop_remove() of the listener and strictly before closing the
   * listener fd; the doc comment of that function describes the real fd-reuse
   * race this closes. Once ccol_event_loop_remove() has run no NEW dispatch
   * can start, so the wait is only ever for a dispatch that is live at that
   * moment, never a fresh one, and there is at most one of those because the
   * per-registration dispatch_lock of ccol_event_loop already makes sure a
   * registration's callback never runs concurrently with itself.
   * __chttpsvr_destroy also checks this counter, next to
   * servers_bundler_pins, right before freeing the contents of raw. By then
   * the check is provably redundant (see the comment at that wait) but we
   * keep it as a cheap second check. It broadcasts resolve_cv, like
   * servers_bundler_pins and pending_resolve_count, for the same reason. */
  _Atomic size_t listener_dispatch_pins;
  _ccol_cond_var_t resolve_cv;
  _ccol_rw_lock_t routes_lock;
  clog cl;

  /* These two are _Atomic, and not just written under srv->mutex, because
   * _listener_on_readable runs on the reactor thread, reads both on every
   * accept() and never takes srv->mutex. With a plain int and bool that is a
   * real data race against the locked writes of chttpsvr_start and
   * chttpsvr_stop, and ThreadSanitizer confirms it. chttpsvr_start also sets
   * both BEFORE registering the listener with ccol_event_loop_add, not after,
   * since the registration is live as soon as ccol_event_loop_add makes it
   * and a connection can arrive before the fields are written. Otherwise
   * that connection reaches _listener_on_readable while it still sees the
   * pre-start listen_fd of -1. */
  _Atomic int listen_fd; /* -1 while the server isn't started */
  _Atomic bool is_unix_socket;
  /* The socket file of a unix:// listener (path is NULL for TCP).
   * srv->mutex guards it. _listener_on_readable never reads it, only
   * chttpsvr_start, chttpsvr_stop and _quiesce_server_once touch it, so it
   * doesn't need to be _Atomic. */
  _chttpsvr_unix_bind_t unix_bind;
  /* Port and numeric local address that getsockname() returned for a TCP
   * listener at bind time, for chttpsvr_bound_port() and
   * chttpsvr_bound_address(). srv->mutex guards both, and they only describe
   * the listener while lifecycle is CHTTPSVR_LC_RUNNING. */
  uint16_t bound_port;
  char bound_host[INET6_ADDRSTRLEN];
  ccol_event_reg listen_reg;
  /* CHTTPSVR_LC_IDLE -> CHTTPSVR_LC_STARTING -> CHTTPSVR_LC_RUNNING ->
   * CHTTPSVR_LC_STOPPING -> CHTTPSVR_LC_IDLE. Every read and write of this
   * field is under raw->mutex. The top of chttpsvr_start()'s retry loop
   * checks it with an exhaustive switch that has no `default:` label on
   * purpose, so `-Wswitch` plus this project's standing `-Werror` force every
   * decision point that reads it to handle any value added to
   * chttpsvr_lifecycle_t later. Otherwise a new value would compile silently
   * and fall into whatever the nearest case does.
   *
   * CHTTPSVR_LC_STARTING spans a whole chttpsvr_start() call on this handle,
   * from the moment the call passes the IDLE check and the self-call check
   * until it returns, successfully or not. We check it next to
   * CHTTPSVR_LC_RUNNING in the same raw->mutex critical section, which a
   * second concurrent chttpsvr_start() on the same handle would otherwise
   * race past. Without this state two threads calling chttpsvr_start() on the
   * same IDLE handle at the same time both see IDLE and both go on in
   * parallel through creating the pools and through the unsynchronized read,
   * release and replace of raw->tls_ctx below. One thread's worker pool and
   * reject pool then disappear silently, along with the OS threads it has
   * already spawned, and the other thread's pair takes their place. Worse,
   * one thread can call ctls_ctx_release() on a TLS context the other thread
   * has already published to raw->tls_ctx while an accepted connection may be
   * running its handshake against that context through
   * ctls_conn_create_server, which is a real use-after-free and not just a
   * leak. chttpsvr_start() puts the field back to CHTTPSVR_LC_IDLE under
   * raw->mutex at every failure return, and moves it to CHTTPSVR_LC_RUNNING
   * on success.
   *
   * CHTTPSVR_LC_STOPPING spans the REAL teardown work of a
   * _chttpsvr_stop_internal() call on this handle. It begins as soon as that
   * call has confirmed the server was RUNNING, in the same critical section
   * that leaves CHTTPSVR_LC_RUNNING, and ends when the call has finished its
   * ccol_event_loop_remove(), close() and unlink() sequence. chttpsvr_start()'s
   * retry loop checks it.
   *
   * We need CHTTPSVR_LC_STOPPING because chttpsvr_stop() leaves
   * CHTTPSVR_LC_RUNNING BEFORE its blocking ccol_event_loop_remove() of the
   * OLD listener registration returns, not after. chttpsvr_destroy() and
   * chttpsvr_engine_stop() are different here since both go through
   * _quiesce_server_once and its quiesce_state interlock below. Without this
   * state a chttpsvr_start() could race a concurrent chttpsvr_stop() on the
   * same handle from another thread: the start sees lifecycle == IDLE and
   * quiesce_state == NOT_QUIESCED at the same time (chttpsvr_stop() never
   * touches quiesce_state) and goes straight into _make_listen_socket() and
   * bind() for a NEW listener on the same host and port while the OLD
   * listener's fd is still open, because the stopping thread hasn't run
   * ccol_event_loop_remove() and close() yet. The race is narrow and fails
   * gracefully, with a spurious ccol_unexpected_failure from a concurrent
   * EADDRINUSE, but it's real, and this state guards against it just like
   * the equivalent guard between chttpsvr_engine_stop() and chttpsvr_start().
   *
   * In a fresh child after a fork(), _chttpsvr_atfork_release_impl
   * unconditionally resets this field to CHTTPSVR_LC_IDLE for every live
   * server whose lifecycle was CHTTPSVR_LC_STARTING or CHTTPSVR_LC_STOPPING
   * when the fork() happened (the doc comment of that function says why
   * that's safe). quiesce_state below is different because its reset needs to
   * tell apart the state a server was in when the fork() caught it. A server
   * that was already CHTTPSVR_LC_RUNNING or CHTTPSVR_LC_IDLE at the fork()
   * stays as it is, since nothing interrupted it half way through a
   * transition. */
  chttpsvr_lifecycle_t lifecycle;
  bool contributed_to_engine;
  /* The generation of the engine that contributed_to_engine's reference
   * belongs to (see srv_engine_bundler.generation). srv->mutex guards it
   * together with contributed_to_engine. */
  uintptr_t engine_gen;
  /* CHTTPSVR_QS_NOT_QUIESCED -> CHTTPSVR_QS_QUIESCING ->
   * CHTTPSVR_QS_QUIESCED. It's the one-shot latch behind the winner/loser
   * claim of _quiesce_server_once() (see the doc comment of that function),
   * and only a later legitimate chttpsvr_start() that restarts this same
   * handle puts it back to CHTTPSVR_QS_NOT_QUIESCED. Every reader checks it
   * with an exhaustive switch without a `default:` label, for the same
   * `-Wswitch` reason the comment of lifecycle above gives. The readers are
   * chttpsvr_start()'s retry loop, the claim and the loser wait of
   * _quiesce_server_once(), and the child-side fixup of
   * _chttpsvr_atfork_release_impl.
   *
   * CHTTPSVR_QS_QUIESCING is set the moment _quiesce_server_once() claims
   * this server's teardown as the winner of a race between
   * __chttpsvr_destroy() and chttpsvr_engine_stop() (the doc comment of that
   * function explains why both can legitimately call it for the same srv at
   * the same time). It's set before any real teardown work runs, i.e. before
   * we stop listening, drain the in-flight requests, close the idle
   * connections, drop the engine reference and destroy the worker pools, and
   * CHTTPSVR_QS_QUIESCED is only set once all that is done. The loser must
   * BLOCK on quiesce_done_cv below until quiesce_state reaches
   * CHTTPSVR_QS_QUIESCED, not just CHTTPSVR_QS_QUIESCING, and must not return
   * immediately as a no-op. The reason is that __chttpsvr_destroy goes
   * straight from the return of _quiesce_server_once into
   * _ccol_mutex_destroy(raw->mutex) and then frees raw, while the winning
   * call may at that very moment still be using raw->mutex, raw->idle_mutex
   * and raw->requests_done_cv inside _drain_and_close_all_connections. That
   * would destroy a mutex still in use and free srv from under a teardown
   * that's still running, a real use-after-free. The top of chttpsvr_start()
   * resets the field to CHTTPSVR_QS_NOT_QUIESCED.
   *
   * In a fresh child after a fork(), _chttpsvr_atfork_release_impl also
   * forces the field straight to CHTTPSVR_QS_QUIESCED and resets
   * quiesce_waiters below to 0. It does that for exactly the servers it finds
   * half way through a teardown at the fork(), the ones with quiesce_state ==
   * CHTTPSVR_QS_QUIESCING (the doc comment of that function explains why
   * that's the safe choice). It releases both the loser wait of
   * _quiesce_server_once and the retry loop of chttpsvr_start() for such a
   * server in the child, whereas resetting to CHTTPSVR_QS_NOT_QUIESCED there
   * would not be safe. */
  chttpsvr_quiesce_state_t quiesce_state;
  /* Broadcast once quiesce_state reaches CHTTPSVR_QS_QUIESCED (see the
   * comment of that field). */
  _ccol_cond_var_t quiesce_done_cv;
  /* How many threads are blocked in a `while (quiesce_state !=
   * CHTTPSVR_QS_QUIESCED) _ccol_cond_var_wait(...)` loop, in practice 0, 1
   * or a few. The same raw->mutex and quiesce_done_cv pair guards it. Such a
   * thread is either the losing side of a __chttpsvr_destroy() vs
   * chttpsvr_engine_stop() race inside _quiesce_server_once itself, or a
   * concurrent chttpsvr_start() whose retry loop backed off after seeing
   * quiesce_state == CHTTPSVR_QS_QUIESCING (see the CHTTPSVR_QS_QUIESCING
   * branch of that function). A loser increments the count just before it
   * enters the wait, and right after it leaves the wait it decrements the
   * count and broadcasts quiesce_done_cv again, both with raw->mutex still
   * held. So the decrement and then the broadcast are the loser's very last
   * touch of raw->mutex and quiesce_done_cv before it unlocks and returns.
   *
   * The winner of _quiesce_server_once waits at its tail for this count to
   * reach 0 before returning to ITS OWN caller, right after it sets
   * CHTTPSVR_QS_QUIESCED and broadcasts. This matters because
   * __chttpsvr_destroy goes straight from the return of _quiesce_server_once
   * into _ccol_mutex_destroy(raw->mutex), then
   * _ccol_cond_var_destroy(raw->quiesce_done_cv) and then frees raw, with no
   * synchronization of its own. The doc comment of quiesce_state already says
   * the broadcast is what wakes a loser, but that doesn't mean the loser gets
   * the mutex back in time. It reacquires it inside its own
   * _ccol_cond_var_wait call, and that can still be in progress when the
   * winner's caller moves on, since pthread_cond_broadcast only makes the
   * waiters runnable and says nothing about when the OS schedules them
   * relative to the broadcasting thread. Take the case where
   * __chttpsvr_destroy is the WINNER, a direction the
   * concurrent_destroy_and_engine_stop_is_safe test doesn't exercise (that
   * test builds the reverse, safer order on purpose, see its own comment).
   * Then the engine's reaper thread may still be half way through its
   * reacquire, inside its own losing _ccol_cond_var_wait on
   * raw->quiesce_done_cv and raw->mutex, at the exact moment
   * __chttpsvr_destroy destroys both. POSIX calls that undefined behavior: no
   * code may destroy a mutex or a condition variable while another thread
   * may still reference it, e.g. through a pthread_cond_wait in progress, and
   * this is a real concern. The mutex's mutual exclusion is what makes the
   * counter-based wait correct, because the winner can only get the mutex
   * back and see zero strictly after the loser has decremented and unlocked,
   * the same happens-before relation pending_resolve_count relies on
   * elsewhere in this file.
   *
   * In a fresh child after a fork(), _chttpsvr_atfork_release_impl resets
   * this count to 0 next to quiesce_state for a server the fork() caught in
   * the middle of a teardown; see the field comment of quiesce_state and the
   * doc comment of that function. */
  size_t quiesce_waiters;
  /* True from the start of a teardown (chttpsvr_destroy, or the forced stop
   * of chttpsvr_engine_stop) until a later chttpsvr_start() restarts the
   * handle. While it's set a response carries Connection: close and ends its
   * connection, and a request that reaches headers-complete gets a 503
   * instead of a handler. So the wait for in-flight requests ends when the
   * handlers already running return, and clients that keep sending can't
   * stretch it. It's a plain bool accessed only through the __atomic
   * builtins, so the hot response path pays an ordinary load and the field
   * has no _Atomic qualifier. _srv_draining() reads it. */
  bool draining;

  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned max_response_write_duration_ms;
  _Atomic unsigned idle_timeout_ms; /* keep-alive idle timeout, 0 turns it
                                     * off */
  /* The total wall-clock ceiling on the reactor-owned phase of one request,
   * which is the TLS handshake plus reading the request headers; 0 turns it
   * off. The idle sweep enforces it against conn->header_phase_start. See
   * max_header_read_duration_ms in chttpsvr_config_t and
   * _conn_header_phase_expired. */
  _Atomic unsigned max_header_read_duration_ms;
  /* _Atomic, and not just written under srv->mutex, for the same reason as
   * stream_read_timeout_ms and its neighbours above. chttpsvr_start() writes
   * them again on every restart, in the small unlocked gap after it
   * publishes srv->worker_pool (see the _ccol_mutex_lock and
   * _ccol_mutex_unlock a few lines above the write site), a gap that ends
   * when the listener is registered again. A keep-alive connection can
   * survive the chttpsvr_stop() before the restart, because the stop only
   * tears down the listener and leaves the accepted connections alone, and
   * such a connection may get diverted to the pool that was just published.
   * Its worker thread can then read max_body_size (in _on_body) or
   * max_header_bytes (in _conn_reset_for_request) inside that gap, and a
   * request of the previous run that keeps running across the restart reads
   * them while the restart writes them. The writer and those readers share
   * no lock, so only atomic access makes each read see either the old or the
   * new value. */
  _Atomic size_t max_body_size;
  _Atomic size_t max_header_bytes; /* 0 picks chttp1_parser's built-in
                                    * default */
  _Atomic size_t max_connections;  /* 0 means no limit */
  /* Unlike the three fields above this is a plain bool. Its only reader is
   * _listener_on_readable, through _apply_accepted_socket_options, which only
   * runs while a listener registration is live. chttpsvr_stop()'s
   * ccol_event_loop_remove(listen_reg) blocks until any listener callback in
   * flight returns and makes sure none fires afterwards, and the new listener
   * is registered well after chttpsvr_start() writes this field, so a reader
   * and a writer of it never run at the same time. max_body_size and
   * max_header_bytes above are different because their reader is the worker
   * thread of an existing connection, which has nothing to do with this
   * server's listener lifecycle. */
  bool enable_keepalive;
  _Atomic size_t current_connections;

  /* _listener_on_readable_impl sets this just before it pauses this server's
   * listen_reg, for a resource-exhaustion errno from accept4() (EMFILE,
   * ENFILE, ENOBUFS or ENOMEM) and for an allocation failure after the accept,
   * in _conn_create or in ctls_conn_create_server.
   * _listener_resume_if_resource_pressure_cleared, on the idle-timeout sweep
   * thread, consumes it by reading and clearing it in one atomic_exchange.
   * It's the same pause/resume split as max_connections and
   * current_connections above, for a similar reason. Retrying accept4()
   * inline right after the backoff blocks the single shared reactor thread,
   * and if the condition doesn't go away every later dispatch blocks it
   * again, because level-triggered epoll keeps reporting the same non-empty
   * backlog. That goes on as long as the resource pressure lasts and starves
   * every other connection of every server on that reactor thread. Pausing
   * instead, with the sweep retrying once per tick, removes the block
   * completely, and the cost is a recovery latency of up to
   * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, the same as the max_connections resume
   * already has. It's _Atomic for the same cross-thread reason as
   * current_connections: the writer is always _listener_on_readable_impl,
   * which never runs concurrently with itself (see the note about
   * dispatch_lock serialization a few fields down), and the reader is
   * another thread. */
  _Atomic bool listener_paused_for_resource_pressure;

  /* These two rate-limit the diagnostic that _listener_on_readable logs for
   * an unexpected accept4() failure, meaning anything other than the routine
   * EWOULDBLOCK, EAGAIN and EINTR the loop handles silently. Without the
   * limit a resource-exhaustion condition that persists, like EMFILE or
   * ENFILE (the process or the system is out of file descriptors), logs the
   * same line on every reactor dispatch for as long as it lasts, and the
   * diagnostic floods the log it was meant to help with. Plain fields are
   * fine since the per-registration dispatch_lock of ccol_event_loop already
   * makes sure this listener's registration is never dispatched concurrently
   * with itself (see cthreadcomm.h). So one server's _listener_on_readable
   * never runs on two threads at once, however many reactor threads are
   * configured, and since it's the only function that touches these fields
   * its runs are simply serialized in time. */
  struct timespec last_accept_err_log;
  bool last_accept_err_log_set;

  /* Registry of idle connections for the idle-timeout sweep. It holds every
   * connection the reactor owns while it waits for the headers of its next
   * request, and never a diverted connection that a worker owns. */
  _ccol_mutex_t idle_mutex;
  chttpsvr_conn_t *idle_head, *idle_tail;
  /* Parked connections, the ones waiting with no thread for the rest of a
   * body or for room to write a response (conn->park is _CONN_PARK_BODY or
   * _CONN_PARK_WRITE). They're claimed exactly like idle connections, under
   * idle_mutex, by _idle_list_try_claim and _idle_list_remove, but they have
   * their own list so the sweep can look at them without walking every idle
   * keep-alive connection. */
  chttpsvr_conn_t *parked_head, *parked_tail;

  /* Registry of diverted connections, every connection a worker thread owns
   * (CONN_ST_DIVERTED; see _conn_start_diverted and _task_worker). Such a
   * connection can block forever in chttp1_stream_read or
   * chttp1_stream_write when stream_read_timeout_ms or
   * response_write_timeout_ms is 0, a documented and legitimate "wait
   * forever" setting, and the peer then stalls without closing.
   * _wait_in_flight_bounded uses this registry to force a shutdown(2) on such
   * a connection's fd once its own graceful wait runs out, so one stalled
   * peer can't hang chttpsvr_destroy(), a chttpsvr_stop() plus
   * chttpsvr_start() restart or chttpsvr_engine_wait() forever, which is what
   * an unconditional ctpool_shutdown_drain() with no timeout would do here.
   * That function's comment explains why. diverted_mutex
   * guards these fields the same way idle_mutex guards idle_head and
   * idle_tail above. */
  _ccol_mutex_t diverted_mutex;
  chttpsvr_conn_t *diverted_head, *diverted_tail;

  /* This server's index in chttpsvr_slot_table.slots, set once by
   * _chttpsvr_handle_slot_acquire. _chttpsvr_finish_destroy needs it when it
   * runs from _conn_on_removed (see the comment of that field), because that
   * path has only this raw pointer and no chttpsvr handle value to take the
   * index from. */
  uint32_t self_slot_idx;

  /* This counter tells us when the struct is safe to free. It works exactly
   * like lifetime_refs of chttpsvr_conn_t (see that field's comment for the
   * reasoning). It starts at 1, standing for "the application hasn't
   * destroyed this server yet" (see chttpsvr_create_mp). It gets +1 for every
   * conn->reg registration we create for any connection this server accepts,
   * i.e. every successful ccol_event_loop_add call site in _conn_pump and
   * _task_worker, and +1 for the listener registration each chttpsvr_start
   * creates. It gets -1 exactly once from __chttpsvr_destroy, the
   * application's "done with this server" decision, and -1 exactly once per
   * on_removed that fires (_conn_on_removed, _listener_on_removed). So a
   * listener dispatch that had already started when chttpsvr_stop removed
   * the listener keeps srv allocated until it returns, since the listener's
   * on_removed only fires after that. _chttpsvr_finish_destroy runs only once
   * the counter reaches 0, which any of those callers can cause, and does the
   * real _ccol_mutex_destroy, the free and the slot release. That's exactly
   * why it lives outside __chttpsvr_destroy: a connection's on_removed can
   * fire well after the caller of __chttpsvr_destroy has returned (the doc
   * comment of ccol_event_loop_remove() explains why that step can't be made
   * synchronous without risking a lock-ordering cycle). It covers one
   * specific case, a registration removed too close to the async teardown of
   * the shared reactor, which the engine's reaper drives, and which therefore
   * never reaches the poller's ordinary reclaim pass. Without the deferral
   * __chttpsvr_destroy would free raw synchronously at the wrong moment,
   * while the final unconditional sweep of _ccol_event_loop_teardown_raw
   * still has this exact connection's on_removed pending on the reaper
   * thread, an intermittent use-after-free that valgrind catches.
   * __chttpsvr_destroy itself must never block waiting for the counter to
   * reach 0, since that deadlocks against the tests where chttpsvr_start()
   * races a concurrent graceful reap: those tests need chttpsvr_destroy() and
   * _engine_release() to return promptly rather than block on the reap they
   * may have triggered. Deferring the free, instead of blocking for it, is
   * what closes the gap without that deadlock. */
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs_t *m_procs;

  /* Slow-client settings, resolved by chttpsvr_start. _Atomic for the same
   * reason as stream_read_timeout_ms above, a restart writes them while
   * connections that survived the stop may be reading them. min_rate_bps is
   * UINT_MAX when the floor is off, mem_cap is SIZE_MAX without a memory
   * limit, the two waits are UINT_MAX when they have no deadline,
   * stream_threads is -1 when streaming handlers share the worker pool, and
   * stream_qcap is SIZE_MAX when the streaming queue is unlimited. */
  _Atomic unsigned min_rate_bps;
  _Atomic unsigned rate_grace_ms;
  _Atomic size_t mem_cap;
  _Atomic unsigned mem_wait_ms;
  _Atomic int stream_threads;
  _Atomic size_t stream_qcap;
  _Atomic unsigned stream_qtimeout_ms;

  /* The pool that runs streaming handlers. srv->mutex guards it the same way
   * as worker_pool, since _conn_start_diverted and the detach of a stop or a
   * destroy read and write it under that lock. It's created by the first
   * streaming request, see _stream_pool_get. */
  ctpool stream_pool;
  /* Changes every time stream_pool goes out of service (a restart or a stop),
   * under wait_mutex and together with the sq_tokens reset. A
   * _stream_token_task task carries the value from when it was submitted,
   * and once that differs the task takes no more requests, so a retired pool
   * doesn't keep draining the queue instead of the current one. */
  _Atomic uint64_t stream_gen;

  /* Pools a restart took out of service while requests of the previous run
   * might still be running on them. Each one drains on its own thread (see
   * _retire_pools), srv->mutex guards the list, and a teardown waits for all
   * of them (see _retired_pools_reap). */
  struct _chttpsvr_retired_pools *retired_pools;

  /* wait_mutex guards the memory accounting, the memory-wait list and the
   * streaming queue. An ordinary request never goes near it: it's only taken
   * when a body reserves memory, or when a request enters or leaves
   * the streaming queue. We never hold it across I/O, a dispatch or another
   * lock of this server.
   *
   * mem_in_use is the sum of all mem_charged. mem_holders counts the
   * connections holding a charge, and mem_blocked the ones among them
   * waiting in the memory-wait list for more. mem_exempt_active means one
   * connection went past the limit (_mem_admit has the rule and the proof
   * that it makes progress). mem_admission_closed stops a teardown from
   * dispatching a waiter, and mem_admitting with mem_admit_again serialize
   * _mem_admit without recursion. */
  _ccol_mutex_t wait_mutex;
  size_t mem_in_use;
  size_t mem_holders;
  size_t mem_blocked;
  bool mem_exempt_active;
  bool mem_admission_closed;
  bool mem_admitting;
  bool mem_admit_again;
  chttpsvr_conn_t *mem_head, *mem_tail;
  size_t mem_waiting;

  /* The streaming queue, oldest at sq_head. sq_tokens counts the tasks that
   * the current run's streaming pool is running or holding to drain the
   * queue. It never goes above that pool's thread count and a new run starts
   * it from zero (see _stream_token_start). sq_unclaimed counts the ones
   * among them that haven't taken their first request yet, so we never start
   * a task for a request that a task already on its way will pick up. */
  chttpsvr_conn_t *sq_head, *sq_tail;
  size_t sq_len;
  size_t sq_tokens;
  size_t sq_unclaimed;
};

/* ========================================================================== */
/*                    CHTTPSVR HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* Whether srv is in the draining phase of a teardown (see the comment of the
 * draining field). */
static inline bool _srv_draining(const struct chttpserver *srv) {
  return __atomic_load_n(&srv->draining, __ATOMIC_RELAXED);
}

/* Puts srv into the draining phase (see the comment of the draining field). */
static void _srv_begin_draining(struct chttpserver *srv) {
  __atomic_store_n(&srv->draining, true, __ATOMIC_RELAXED);
}

/* Resolves h and pins the result against a concurrent destroy. Returns NULL
 * when h is 0, when it's garbage, and when it names a slot that is free right
 * now or has been reused with another generation. On success the caller MUST
 * call _chttpsvr_resolve_unpin(result) exactly once, right before every
 * return path of the short, synchronous public function that resolved it. No
 * user of the handle needs the pin for longer than its own function body;
 * the comment of pending_resolve_count in struct chttpserver says why. Same
 * as _chttpcli_resolve in chttpclient.c. */
static struct chttpserver *_chttpsvr_resolve(chttpsvr h) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  /* A table released at unload holds no server, so every handle resolves to
   * NULL there. */
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* Lock-free on purpose: we never take raw->mutex here, so nothing blocks
   * while chttpsvr_slot_table.mutex is held (_chttpcli_resolve in
   * chttpclient.c gives the contention argument). It's safe because raw is
   * still allocated at this point. The only thing that could make it unsafe
   * to touch is the slot-release step of __chttpsvr_destroy, and that step
   * needs chttpsvr_slot_table.mutex too, which we're still holding. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

static void _chttpsvr_resolve_unpin(struct chttpserver *raw) {
  /* The decrement itself MUST be under raw->mutex and not a bare atomic
   * outside the lock. _chttpcli_resolve_unpin in chttpclient.c explains the
   * real use-after-free a lock-free decrement opens, and the same applies
   * here. The resolve's increment stays lock-free, but the unpin's decrement
   * goes together with the broadcast inside raw->mutex, the usual pattern
   * for a condition variable. */
  _ccol_mutex_lock(raw->mutex);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  _ccol_cond_var_broadcast(raw->resolve_cv); /* wakes a destroy waiting on
                                              * this */
  _ccol_mutex_unlock(raw->mutex);
}

/* Takes a fresh slot for srv, or reuses a freed one, and returns the handle
 * built from it, or 0 when we run out of memory. chttpsvr_create_mp calls it
 * once, after the rest of the object is fully built. It mirrors
 * _chttpcli_handle_slot_acquire in chttpclient.c, including the order of the
 * generation mint (it has to happen before we build the handle from it) and
 * the skip on a generation wraparound, without which a live server whose
 * generation wrapped could get a handle equal to CHTTPSVR_INVALID. */
static chttpsvr _chttpsvr_handle_slot_acquire(struct chttpserver *srv) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  /* The table was released at unload, so we can't publish a handle any more
   * and the create that asked for one fails cleanly. */
  if (!chttpsvr_slot_table.slots) {
    _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
    return 0;
  }
  uint32_t idx;
  chttpsvr_slot_t *slot;
  if (cvector_elem_count(chttpsvr_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpsvr_slot_table.free_indices, &idx);
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  } else {
    chttpsvr_slot_t fresh = {0};
    if (cvector_push_back(chttpsvr_slot_table.slots, &fresh) != ccol_success) {
      _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
      return 0; /* an ordinary allocation failure, not fatal */
    }
    idx = (uint32_t)cvector_elem_count(chttpsvr_slot_table.slots) - 1;
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* skip the value that would collide with
                         * CHTTPSVR_INVALID; the same guard in
                         * chttpclient.c explains why */
  slot->ptr = srv;
  slot->in_use = true;
  srv->self_slot_idx = idx;
  chttpsvr h = ((chttpsvr)idx << 32) | (chttpsvr)slot->generation;
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return h;
}

/* Whether the exit-time destructor has already released the slot table. */
static bool _chttpsvr_slot_table_released(void) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  bool released = chttpsvr_slot_table.slots == NULL;
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return released;
}

/* ========================================================================== */
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* A process-wide thread-local key that records which struct chttpserver*, if
 * any, the calling thread runs worker_pool or reject_pool tasks for. It's set
 * once at the top of _task_worker and _reject_task, the only two functions we
 * ever submit to a server's worker_pool or reject_pool, and nothing sets or
 * clears it again for the rest of that thread's life. Same idea as
 * ctpool_worker_key_bundle and _ctpool_is_self_call in cthreadpool.c, and for
 * the same reason: a worker thread of srv->worker_pool or srv->reject_pool
 * belongs to one server privately and for its whole life, because ctpool
 * always spawns its own dedicated threads and never shares them between
 * pools, and a restart destroys a server's pools and builds them again with
 * new OS threads. So there's no per-task set-and-clear to manage, unlike the
 * key of a ccol_event_loop dispatch-pool worker in cthreadcomm.c, which does
 * need one.
 *
 * The key is there to catch a request handler, a middleware or the
 * handler's chttpsvr_next_fn continuation calling chttpsvr_destroy(), or
 * chttpsvr_stop() directly followed by chttpsvr_start(), on the very server
 * whose worker pool is running that code. Without it the call goes into
 * _wait_in_flight_bounded's bounded wait for in_flight_requests to reach 0,
 * about a minute once the current_connections poll of
 * _drain_and_close_all_connections is counted. in_flight_requests can never
 * reach 0 since this very call stack is the one that decrements it, so the
 * call then goes into ctpool_destroy() on the pool this thread is a worker
 * of, which ends in an unrelated _ccol_fatal_err() and abort() from
 * cthreadpool.c (see the comment of ctpool_worker_key_bundle there). The
 * symptoms are indirect and hard to trace back: a handler that calls
 * chttpsvr_destroy() on its own server hangs the whole process for about a
 * minute and then aborts it, with a crash pointing into cthreadpool.c rather
 * than this file. If a concurrent chttpsvr_engine_stop() for the same server
 * races in, it deadlocks forever with no abort at all, because the reaper
 * thread's ctpool_shutdown_drain isn't a self-call, has no timeout and waits
 * forever for the self-deadlocked handler to return. With the key we catch
 * the case right here, cheaply, with one thread-local read and no lock,
 * before any of that can happen, and a clear error at the call is much
 * easier to debug than a hang that ends in another module's abort. */
/*
 * Creating the key can fail when the process already holds PTHREAD_KEYS_MAX
 * keys. Then the key field doesn't hold a key of ours: the value 0 names
 * whatever key some other component created first, and setting it would
 * hand srv to that component's destructor. `ready` records that creation
 * succeeded, and nothing reads or writes `key` without it. chttpsvr_start
 * creates the key and refuses to start a server when it can't, so a worker
 * thread never exists without the key, and a later chttpsvr_start tries
 * again. The module destructor deletes the key once no server is left that a
 * worker could belong to; see _cleanup_chttpsvr_slot_table. */
static struct {
  _ccol_thread_ls_key_t key;
  _ccol_once_flag_t once;
  _ccol_mutex_t mutex; /* serializes creating and deleting key */
  _Atomic bool ready;
} chttpsvr_worker_key_bundle = {0};

static void _chttpsvr_worker_key_init_globals(void) {
  if (_ccol_mutex_init(chttpsvr_worker_key_bundle.mutex) != 0)
    _ccol_fatal_err("chttpsvr worker key: failed to initialize mutex");
}

/* Creates the key if it isn't there yet and returns whether it exists now.
 * Only chttpsvr_start calls this. */
static bool _chttpsvr_worker_key_ensure(void) {
  if (atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                           memory_order_acquire))
    return true;
  _ccol_call_once(chttpsvr_worker_key_bundle.once,
                  _chttpsvr_worker_key_init_globals);
  _ccol_mutex_lock(chttpsvr_worker_key_bundle.mutex);
  if (!atomic_load(&chttpsvr_worker_key_bundle.ready) &&
      _ccol_thread_ls_key_create(chttpsvr_worker_key_bundle.key, NULL) == 0)
    atomic_store_explicit(&chttpsvr_worker_key_bundle.ready, true,
                          memory_order_release);
  bool ready = atomic_load(&chttpsvr_worker_key_bundle.ready);
  _ccol_mutex_unlock(chttpsvr_worker_key_bundle.mutex);
  return ready;
}

/* Whether the key exists. A thread can only have marked itself if it does,
 * so every reader takes a missing key as "not a worker". */
static inline bool _chttpsvr_worker_key_ready(void) {
  return atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                              memory_order_acquire);
}

/* Called once at the top of _task_worker and _reject_task to mark the
 * calling thread as a permanent worker of srv's pools (see the comment of
 * chttpsvr_worker_key_bundle above). */
static void _chttpsvr_mark_worker_thread(struct chttpserver *srv) {
  if (!_chttpsvr_worker_key_ready()) return;
  _ccol_thread_ls_set(chttpsvr_worker_key_bundle.key, (void *)srv);
}

/* True when the calling thread is a worker of srv's worker_pool or
 * reject_pool, which is the case when the call comes, directly or further
 * down, from a request handler, a middleware or a reject_pool courtesy-close
 * task currently running for srv. */
static bool _chttpsvr_is_self_call(struct chttpserver *srv) {
  if (!_chttpsvr_worker_key_ready()) return false;
  return _ccol_thread_ls_get(chttpsvr_worker_key_bundle.key) == (void *)srv;
}

/* True when the calling thread is a worker of the worker_pool or reject_pool
 * of ANY chttpsvr, i.e. the call comes, directly or further down, from a
 * request handler, a middleware or a reject_pool courtesy-close task of some
 * server, no matter which. _chttpsvr_is_self_call looks at one specific
 * server, while this one exists for chttpsvr_engine_wait(), which is
 * engine-wide. _engine_force_stop_quiesce_all drains the worker pool of every
 * registered server in turn, inside _quiesce_server_once, with a plain
 * ctpool_shutdown_drain that has no timeout. So a handler of ANY server that
 * blocks in chttpsvr_engine_wait() runs into the same kind of self-deadlock
 * that _chttpsvr_is_self_call already protects chttpsvr_destroy() and
 * chttpsvr_start() from: the reaper thread can never finish draining that
 * handler's server pool, because the task can only return through this very
 * call stack. The reactor then never gets torn down and marked stopped and
 * the wait never wakes, a permanent deadlock of the whole engine and not
 * just one stuck server. */
static bool _chttpsvr_is_any_worker_call(void) {
  if (!_chttpsvr_worker_key_ready()) return false;
  return _ccol_thread_ls_get(chttpsvr_worker_key_bundle.key) != NULL;
}

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpserver's OWN engine)        */
/* ========================================================================== */

/*
 * One static ccol_event_loop reactor for the whole process, shared by every
 * chttpsvr instance in it. chttpclient has its own static ccol_event_loop
 * (see chttpclient.c), so there are two separate reactors and the server and
 * the client never share one. That's why this lifecycle wrapper needs no
 * coordination between the two modules, just a refcount over the chttpsvr
 * instances in a simple acquire, release and reaper-thread shape, and no
 * cross-module ordering for an atexit safety net either, since nothing else
 * in the process races to bring this reactor up first.
 */
static struct {
  /* Only written under srv_engine_bundler.mutex: _engine_acquire creates it
   * (its rollback branch for a failed allocation included) and
   * _engine_reaper_fn sets it to CCOL_EVENT_LOOP_INVALID. Dozens of places in
   * this file read it without a lock though, every connection site, every
   * reactor callback and every listener site. That is safe by construction
   * and doesn't rely on a uint64_t load or store happening to be atomic on
   * the CPU. An unlocked reader can only run while at least one chttpsvr
   * instance is started and holds a live engine reference, and that
   * invariant is exactly what keeps the value constant between the write at
   * creation and the write at the reap. Real synchronization also orders
   * both writes against every reader, either _ccol_thread_create and
   * _ccol_thread_join for the reactor threads themselves or the pin and
   * resolve mechanisms that guard every chttpsvr handle. If some future path
   * reads the field OUTSIDE that invariant, i.e. without holding a live
   * engine reference of its own first, the field must become _Atomic. */
  ccol_event_loop reactor;
  size_t reactor_refs;
  _ccol_mutex_t mutex;
  _ccol_cond_var_t stopped_cv;
  _ccol_once_flag_t once;
  bool stopping;
  _ccol_thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;
  /* 0 picks the default of one dedicated reactor thread
   * (num_reactor_threads == 1 inside), which is the faster choice for the
   * common case; the doc comment of chttpsvr_set_engine_num_reactor_threads
   * gives the comparison. A positive value pins the reactor to exactly
   * that many OS threads. The value is built into the reactor when it's
   * constructed, so the same restriction as for mprocs above applies: set it
   * before the first start or after a full stop. */
  size_t num_reactor_threads;
  /* The value, auto-detected or explicit, passed to
   * ccol_event_loop_create_mp the last time the reactor was created. It only
   * exists for test instrumentation, see
   * _chttpsvr_engine_num_reactor_threads_for_tests below. */
  size_t last_resolved_num_reactor_threads;
  /* Engine-wide diagnostics logger. The ccol_event_loop reactor has no log
   * of its own to forward, so this one captures the diagnostics of
   * chttpserver's own reactor thread (TLS handshake failures, listener bind
   * errors, idle-timeout closes) for every chttpsvr instance sharing the
   * process's one reactor. If the caller installed a logger with
   * chttpsvr_set_engine_logger before the engine first started we use that,
   * otherwise _engine_acquire installs a fallback logger at the CLOG_FATAL
   * level on fd 2 on the first engine start. So the field is never NULL
   * while the engine runs, and that matters beyond this file's internals:
   * the clog_info call in _engine_reaper_fn's teardown and every other log
   * call need SOME live logger, and since _clog_write has no guard for a
   * NULL handle, logging through NULL would crash instead of being skipped.
   * When allocating the fallback logger fails, _engine_acquire destroys the
   * reactor it just created, synchronously and before any server holds a
   * reference, so a reactor never exists with this field NULL and the reaper
   * never runs for one. srv_engine_bundler.mutex only protects the field
   * against a torn read or write of the pointer racing a concurrent
   * chttpsvr_set_engine_logger(); clog itself is already thread-safe for
   * concurrent log calls through one handle. */
  clog log;
  /* The logger chttpsvr_set_engine_logger() derived from the caller's
   * logger, or CLOG_INVALID. It outlives every engine: each engine start
   * takes it as log and each engine's reaper leaves it open, so a logger the
   * caller installed keeps serving every later engine of the process. It is
   * only closed by a later chttpsvr_set_engine_logger() call, by process
   * exit or by unloading this module. srv_engine_bundler.mutex guards it. */
  clog user_log;
  /* True when the exit-time destructor couldn't close user_log because an
   * engine was still running. That engine's reaper closes it then, since the
   * process is going away and no later engine will log through it. mutex
   * guards it. */
  bool close_user_log_at_stop;
  /* Generation of the reactor running right now, or 0 when none is.
   * _engine_acquire gives every new reactor the next gen_counter value and
   * _engine_reaper_fn puts it back to 0 when it clears reactor, all writes
   * under mutex. A server tags its engine reference with this value so that
   * a release arriving after its engine was replaced never touches the new
   * engine's refcount. chttpsvr_engine_stop() reads it with a single
   * lock-free atomic load, which is async-signal-safe, so a stop request
   * names the engine that was running when the request was made. uintptr_t
   * is lock-free on every target we support. */
  _Atomic uintptr_t generation;
  uintptr_t gen_counter;
  /* The process that created the current (or the last) reactor. A fork()
   * child inherits reactor, its references and its generation but none of
   * its threads; see _engine_forget_inherited_locked. mutex guards it. */
  pid_t reactor_pid;
} srv_engine_bundler = {0};

/* The idle-timeout sweep thread. There's one per process, shared by the idle
 * connection registries of all chttpsvr instances: every server has its own
 * srv->idle_head/srv->idle_tail list and the sweep walks every started
 * server. */
static struct {
  _ccol_thread_id_t thread;
  bool running;
  /* The process that started the sweep thread. A fork() child that finds
   * running set by another process has no such thread to join or rely on
   * (see _idle_sweep_forget_inherited_locked). servers_bundler.mutex guards
   * it together with running. */
  pid_t pid;
  /* _Atomic, and not just written under servers_bundler.mutex, because
   * _idle_sweep_fn runs on its own thread and reads it in its loop condition
   * without ever taking that mutex. With a plain bool this is a real data
   * race against the locked write in _idle_sweep_stop_if_running, and
   * ThreadSanitizer confirms it. Being a single byte, a torn read is
   * unlikely in practice, but the race is still undefined behavior and lets
   * the compiler cache the read across loop iterations so it never sees the
   * write, and then the _ccol_thread_join of _idle_sweep_stop_if_running
   * really hangs forever. */
  _Atomic bool stop_flag;
  /* The sweep thread waits for the next tick on wake_cv (CLOCK_MONOTONIC),
   * and _idle_sweep_stop_if_running sets stop_flag under wake_mu and signals
   * wake_cv, so a stop ends the wait immediately instead of sitting out the
   * rest of a tick before the join returns. wake_mu is a leaf lock, nobody
   * takes another lock while holding it. _engine_globals_init creates
   * both. */
  _ccol_mutex_t wake_mu;
  _ccol_cond_var_t wake_cv;
} idle_sweep_bundler = {0};

static struct {
  _ccol_mutex_t mutex;
  struct chttpserver **servers;
  size_t count;
  size_t capacity;
} servers_bundler = {0};

static void _idle_sweep_stop_if_running(void);
/* Defined much further down, next to the rest of the engine-stop watcher's
 * signal-safe machinery. It's declared here for _engine_reaper_fn, its only
 * caller, which comes first in the file. */
static void _engine_request_reaper_join(void);
static void _quiesce_server_once(struct chttpserver *srv);
static bool _engine_release_begin(uintptr_t gen);
static void _close_idle_connections(struct chttpserver *srv,
                                    bool include_parked);
static void _spawn_reaper(void);
static void _chttpsvr_stop_internal(struct chttpserver *raw);
static void _engine_globals_init(void);
#ifdef RUNNING_UNIT_TESTS
static void _reaper_race_hook_wait_if_armed(void);
static void _listener_dispatch_race_hook_wait_if_armed(bool at_entry);
static void _quiesce_teardown_race_hook_wait_if_armed(void);
#endif /* RUNNING_UNIT_TESTS */

/* Only the engine reaper (_engine_reaper_fn) calls this, right before it
 * tears down the shared reactor. It quiesces every chttpsvr instance still
 * registered in servers_bundler.servers: stop listening, drain the in-flight
 * requests, close the idle connections, shut down and destroy the worker
 * pool, then drop the engine reference. That applies whatever made the
 * reaper run. When chttpsvr_engine_stop() triggered it the servers may still
 * be fully live and started. On the graceful path, where the refcount hit
 * zero, the server that released the last reference unregistered itself
 * before starting the reaper, so what we find here is servers whose quiesce
 * is still running on another thread (we wait for each one as a loser) plus
 * whatever a concurrent chttpsvr_start() registered since, and usually the
 * list is empty. Without this function a forced engine stop would tear down
 * srv_engine_bundler.reactor and free servers_bundler.servers from under
 * servers that are still started, whose listen_reg and conn->reg
 * registrations point into that reactor, and the next chttpsvr_destroy() of
 * such a server would dereference freed state.
 *
 * The loop reads servers_bundler.servers[0] again on every pass instead of
 * snapshotting the list up front, because _quiesce_server_once calls
 * _servers_unregister, which removes the element just processed. If a
 * concurrent chttpsvr_destroy() on another thread is already quiescing that
 * same server, the element stays where it is until that call finishes.
 * Either way the loop ends up at servers_bundler.count == 0 once every
 * legitimate teardown in progress has completed.
 *
 * We pin srv for the rest of each iteration, the _quiesce_server_once call
 * included (see the atomic_fetch_add below), because a bare struct
 * chttpserver* read out of servers_bundler.servers[] has nothing else
 * protecting its lifetime from the moment servers_bundler.mutex is
 * unlocked. The real work of _quiesce_server_once, draining in-flight
 * requests and closing connections, can take real wall-clock time and not
 * just a few instructions. That window is real and reproducible: inside it
 * a completely independent chttpsvr_destroy(h) of this exact server can run
 * to completion and free it from under the loop. __chttpsvr_destroy has no
 * way of knowing this thread is about to use srv, since it never goes
 * through servers_bundler, only through the separate chttpsvr_slot_table.
 *
 * The pin is srv->servers_bundler_pins and NOT srv->pending_resolve_count.
 * pending_resolve_count looks tempting, since _chttpsvr_resolve uses it for
 * every other user of a chttpsvr handle and __chttpsvr_destroy has a
 * matching wait for it, but it self-deadlocks: the winner path of
 * _quiesce_server_once ALSO waits for pending_resolve_count to reach 0 as
 * its very first step, before any real work. A caller holding its own
 * pending_resolve_count pin across its own _quiesce_server_once call
 * therefore waits forever for a pin only it can release.
 * servers_bundler_pins is a separate counter _quiesce_server_once never
 * touches, so there's no such cycle (see the comment of that field). */
static void _engine_force_stop_quiesce_all(void) {
  /* Every real caller only runs after _engine_acquire's
   * _ccol_call_once(srv_engine_bundler.once, ...) has fired. But the project
   * rule is to put the guard in every function that touches the primitive
   * directly and never rely on call-graph reasoning (see the same guard in
   * _servers_register), so this function has its own guard and doesn't
   * assume that order. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  /* All registered servers stop taking new requests at once. The loop
   * below quiesces them one by one, and without this a server further down
   * the list would keep serving keep-alive clients for as long as the drains
   * in front of it take. */
  _ccol_mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++)
    _srv_begin_draining(servers_bundler.servers[i]);
  size_t idle_pos = servers_bundler.count;
  _ccol_mutex_unlock(servers_bundler.mutex);
  /* Every registered server also closes its idle keep-alive connections now,
   * before the first drain, and not when its turn comes in the loop below.
   * We walk the array from the end down because an unregister moves the last
   * element into the slot it frees, and a server moved that way has already
   * been visited, so none is skipped. Each server stays pinned while its
   * connections close, just like in the loop below. */
  for (;;) {
    _ccol_mutex_lock(servers_bundler.mutex);
    if (idle_pos > servers_bundler.count) idle_pos = servers_bundler.count;
    if (idle_pos == 0) {
      _ccol_mutex_unlock(servers_bundler.mutex);
      break;
    }
    struct chttpserver *srv = servers_bundler.servers[--idle_pos];
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    _ccol_mutex_unlock(servers_bundler.mutex);
    _close_idle_connections(srv, /*include_parked=*/false);
    _ccol_mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    _ccol_cond_var_broadcast(srv->resolve_cv);
    _ccol_mutex_unlock(srv->mutex);
  }
  struct chttpserver *prev_unremoved = NULL;
  for (;;) {
    _ccol_mutex_lock(servers_bundler.mutex);
    if (servers_bundler.count == 0) {
      _ccol_mutex_unlock(servers_bundler.mutex);
      return;
    }
    struct chttpserver *srv = servers_bundler.servers[0];
    /* The comment of servers_bundler_pins says why this is its own counter
     * and not pending_resolve_count. We increment while still holding
     * servers_bundler.mutex, otherwise this thread's reference to srv would
     * be unprotected for at least one instruction. */
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    _ccol_mutex_unlock(servers_bundler.mutex);

#ifdef RUNNING_UNIT_TESTS
    _reaper_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

    if (srv == prev_unremoved) {
      /* _quiesce_server_once never returns to a caller, winner or loser,
       * before _servers_unregister(srv) has run (see how quiesce_state is
       * ordered against the unregister in that function). So seeing the same
       * srv again as servers_bundler.servers[0] never means the previous
       * iteration's unregister is still pending. It means a concurrent
       * chttpsvr_start(srv) restart legitimately ran _servers_register(raw)
       * again. That register call comes before the start's _engine_acquire
       * precisely so that a force-stop pass like this one can see it (the
       * call site of _servers_register in chttpsvr_start explains it in
       * full), and the restart happened in the narrow window between the
       * return of this loop's previous _quiesce_server_once(srv) and this
       * iteration's read of servers_bundler.servers[0]. The racing
       * chttpsvr_start() holds its resolve pin for its whole duration, so
       * the _quiesce_server_once below blocks on that pin before quiescing
       * srv again. The short sleep only keeps us from busy-spinning against
       * that restart while it runs. */
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
    }
    prev_unremoved = srv;
    _quiesce_server_once(srv);
    /* Pairs with the pin above: srv is valid, unfreed memory up to and
     * including the _quiesce_server_once call just above.
     * __chttpsvr_destroy waits for servers_bundler_pins == 0 right before
     * freeing a single byte of srv, and that wait can't end while we still
     * hold our pin. We drop it under srv->mutex together with the broadcast
     * that wakes such a destroy, for the same reason _chttpsvr_resolve_unpin
     * does it that way for pending_resolve_count. */
    _ccol_mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    _ccol_cond_var_broadcast(srv->resolve_cv);
    _ccol_mutex_unlock(srv->mutex);
  }
}

/* Creates the condition variable the idle sweep thread waits on between
 * ticks, on CLOCK_MONOTONIC to match the deadline the thread computes. */
static int _idle_sweep_init_wake_cv(void) {
  _ccol_cond_var_attr_t cv_attr;
  if (_ccol_cond_var_attr_init(cv_attr) != 0) return 1;
  int rc = _ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
  if (rc == 0) rc = _ccol_cond_var_init_ca(idle_sweep_bundler.wake_cv, cv_attr);
  _ccol_cond_var_attr_destroy(cv_attr);
  return rc;
}

static void _engine_globals_init(void) {
  if (_ccol_mutex_init(srv_engine_bundler.mutex) != 0)
    _ccol_fatal_err("chttpserver engine: failed to initialize mutex");
  if (_ccol_cond_var_init(srv_engine_bundler.stopped_cv) != 0)
    _ccol_fatal_err(
        "chttpserver engine: failed to initialize condition variable");
  if (_ccol_mutex_init(servers_bundler.mutex) != 0)
    _ccol_fatal_err(
        "chttpserver engine: failed to initialize servers_bundler mutex");
  if (_ccol_mutex_init(idle_sweep_bundler.wake_mu) != 0)
    _ccol_fatal_err("chttpserver engine: failed to initialize the sweep mutex");
  if (_idle_sweep_init_wake_cv() != 0)
    _ccol_fatal_err(
        "chttpserver engine: failed to initialize the sweep condition "
        "variable");
  /* We leave SIGPIPE's disposition to the application. A client can close
   * its read side, or the whole connection, while a worker is half way
   * through writing the response, but none of our writes can raise the
   * signal for that. Every plaintext write goes through
   * chttp1_stream_write() or chttp1_stream_writev2(), which send with
   * MSG_NOSIGNAL, and every TLS write (a teardown's close_notify too) goes
   * through ctls's socket BIO, which sends with MSG_NOSIGNAL as well. */
}

/*
 * Logs through srv_engine_bundler.log, reading the pointer and making the
 * clog_info or clog_error call in ONE critical section rather than copying
 * the pointer out under the lock and using it later without the lock. Copy
 * now, use later is a use-after-free here: chttpsvr_set_engine_logger()
 * swaps in a new logger under srv_engine_bundler.mutex and only calls
 * clog_close() on the OLD one after unlocking, and clog_close() frees the
 * handle unconditionally, whatever the separate refcount of the shared
 * backing store says (see clogger.c). A caller that copied the old pointer
 * before the swap and uses it afterwards would call clog_info() or
 * clog_error() on memory that was just freed. It's a macro and not a wrapper
 * function so that the log call's __FILE__, __LINE__ and __func__ still name
 * the real call site instead of this helper.
 */
#define _SRV_ENGINE_LOG(level_call, ...)                            \
  do {                                                              \
    _ccol_call_once(srv_engine_bundler.once, _engine_globals_init); \
    _ccol_mutex_lock(srv_engine_bundler.mutex);                     \
    if (srv_engine_bundler.log)                                     \
      level_call(srv_engine_bundler.log, __VA_ARGS__);              \
    _ccol_mutex_unlock(srv_engine_bundler.mutex);                   \
  } while (0)

static void _join_reaper_if_needed_locked(void) {
  if (srv_engine_bundler.reaper_joinable) {
    _ccol_thread_join(srv_engine_bundler.reaper_thread);
    srv_engine_bundler.reaper_joinable = false;
  }
}

static void *_engine_reaper_fn(void *arg) {
  (void)arg;
  /* Every real caller spawns this thread only after _engine_acquire's
   * _ccol_call_once(srv_engine_bundler.once, ...) has fired (see the two
   * call sites of _spawn_reaper). The project rule is still to guard every
   * function that touches the primitive directly instead of relying on
   * call-graph reasoning, so we carry our own guard here. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_event_loop loop_to_destroy;
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  loop_to_destroy = srv_engine_bundler.reactor;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  /* Quiesce every server still registered BEFORE touching the idle sweep
   * thread or the reactor. chttpsvr_engine_stop() is the forced path and can
   * run this reaper while servers are still fully started, with live
   * listen_reg and conn->reg registrations into srv_engine_bundler.reactor.
   * Tearing the reactor down first would leave those registrations dangling
   * for the owning server's next chttpsvr_stop() or _conn_close(). On the
   * graceful path, where the refcount hit zero, the server that released the
   * last reference is already unregistered and the call only waits for
   * quiesces other threads are still running (see the doc comment of
   * _engine_force_stop_quiesce_all). */
  _engine_force_stop_quiesce_all();
  /* Stop the idle sweep thread before tearing down the reactor, since the
   * sweep calls into it through _conn_close and ccol_event_loop_remove (see
   * the doc comment of _idle_sweep_stop_if_running). */
  _idle_sweep_stop_if_running();
  if (loop_to_destroy) ccol_event_loop_destroy(loop_to_destroy);

  /* servers_bundler.servers is a plain realloc'ed buffer, not tied to the
   * lifetime of any one server. Every server that contributed a reference to
   * this engine is unregistered by now, either by the
   * _engine_force_stop_quiesce_all call above or, on the graceful path, by
   * the server that released the last reference before starting this
   * reaper. Another server's chttpsvr_start() can still register during the
   * teardown (it registers before it acquires the next engine), and that
   * registration belongs to the next engine. So we free the array only when
   * it's empty, which lets the next engine's sweep find the server and a
   * later engine stop quiesce it. Without the free an empty array would stay
   * reachable for the rest of the process. */
  _ccol_mutex_lock(servers_bundler.mutex);
  if (servers_bundler.count == 0) {
    free(servers_bundler.servers);
    servers_bundler.servers = NULL;
    servers_bundler.capacity = 0;
  }
  _ccol_mutex_unlock(servers_bundler.mutex);

  _ccol_mutex_lock(srv_engine_bundler.mutex);
  srv_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  atomic_store(&srv_engine_bundler.generation, (uintptr_t)0);
  srv_engine_bundler.stopping = false;
  /* The fallback logger _engine_acquire installed lives exactly as long as
   * this reactor, so we close it here. Otherwise a full engine stop would
   * leave it reachable for the rest of the process, and a later
   * chttpsvr_start() brings the reactor back with a fresh one anyway. The
   * chttpsvr_set_engine_logger logger is different, it stays open and the
   * next engine logs through it again (see the comment of user_log).
   *
   * log is never NULL here because _engine_acquire destroys the reactor
   * itself when the fallback logger can't be allocated, and no reaper runs
   * for that reactor, so the old_logger checks below are only a defence. We
   * copy the handle into a local instead of logging right here and defer the
   * clog_info call to after the _ccol_mutex_unlock below (see the comment at
   * that call). A logger an operator installs with
   * chttpsvr_set_engine_logger can be a synchronous sink of any slowness,
   * and this mutex also guards every other chttpsvr_start, chttpsvr_stop,
   * _engine_acquire and _engine_release in the process, not just this reaper
   * thread. old_logger is still good for a log call and a close after the
   * unlock because we reset srv_engine_bundler.log to CLOG_INVALID below
   * under the lock, so no other thread can see or touch this exact clog
   * handle once the mutex is unlocked. */
  clog old_logger = srv_engine_bundler.log;
  srv_engine_bundler.log = CLOG_INVALID;
  /* The chttpsvr_set_engine_logger() logger stays open for the next engine,
   * and a concurrent chttpsvr_set_engine_logger() may close it as soon as we
   * release this mutex. So its log call happens here under the mutex, and
   * only the fallback logger, which nothing else can reach any more, goes
   * the deferred way below. */
  if (old_logger && old_logger == srv_engine_bundler.user_log) {
    clog_info(old_logger, "The http server reactor engine has been destroyed");
    old_logger = CLOG_INVALID;
  }
  /* The exit-time destructor has already run and left the
   * chttpsvr_set_engine_logger() logger open for this engine; nothing logs
   * through it after this point. */
  clog closing_user_log = CLOG_INVALID;
  if (srv_engine_bundler.close_user_log_at_stop) {
    closing_user_log = srv_engine_bundler.user_log;
    srv_engine_bundler.user_log = CLOG_INVALID;
    srv_engine_bundler.close_user_log_at_stop = false;
  }
  _ccol_cond_var_broadcast(srv_engine_bundler.stopped_cv);
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (closing_user_log) clog_close(closing_user_log);

  if (old_logger) {
    clog_info(old_logger, "The http server reactor engine has been destroyed");
    clog_close(old_logger);
  }
  /* Hand this thread over to be joined. Otherwise, in the ordinary case of
   * an application that destroys its last server and does nothing else, it
   * is never joined: only _engine_acquire (run by a later chttpsvr_start)
   * and _engine_wait_until_stopped (run by chttpsvr_engine_wait) reach
   * _join_reaper_if_needed_locked, and such an application calls neither. A
   * joinable thread that has returned keeps its stack mapped until someone
   * joins it.
   *
   * The engine-stop watcher, which is always running, does the join instead
   * of whoever spawned this thread. That's what keeps chttpsvr_destroy()
   * asynchronous: destroy returns as soon as this thread is spawned, as the
   * documentation says, and chttpsvr_engine_wait() stays the way to wait for
   * the teardown.
   *
   * It must be the last statement and take no lock, because the
   * watcher may join this thread the instant the request is visible, and
   * joining a thread that still waits on srv_engine_bundler.mutex deadlocks
   * against whoever holds that mutex. The store also comes after the logger
   * work above on purpose, so the join only waits for this thread to return.
   * If the exit-time destructor has already stopped the watcher, the request
   * isn't serviced, and a later chttpsvr_engine_wait() or the backstop join
   * in _cleanup_engine_stop_watcher covers that case. */
  _engine_request_reaper_join();
  return NULL;
}

#ifdef RUNNING_UNIT_TESTS
/* A delay in ms between creating the reaper and recording its thread in
 * _spawn_reaper, so a test can let the reaper run to its end inside that
 * window. */
static _Atomic unsigned g_spawn_reaper_delay_ms_for_tests = 0;
void _chttpsvr_set_spawn_reaper_delay_for_tests(unsigned ms) {
  atomic_store(&g_spawn_reaper_delay_ms_for_tests, ms);
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* While this is set, creating the reaper thread fails as if the system had no
 * thread left to give, so _spawn_reaper runs the reaper inline. */
static _Atomic bool g_force_reaper_thread_create_fail_for_tests = false;
void _chttpsvr_force_reaper_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_reaper_thread_create_fail_for_tests, force);
}
#endif /* RUNNING_UNIT_TESTS */

/* If the reaper thread can't be created the reaper runs inline on the
 * calling thread. Its _engine_force_stop_quiesce_all then quiesces every
 * server still registered and waits for each quiesce running on another
 * thread, so a caller must never be in the middle of quiescing a registered
 * server when it calls this. That's why _quiesce_server_once only spawns the
 * reaper after it has unregistered its server (see _engine_release_begin). */
static void _spawn_reaper(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  /* We create and record the thread under srv_engine_bundler.mutex. The
   * reaper takes that mutex before it can finish, so until this call has
   * recorded it the reaper can't hand itself over for a join, and a later
   * start plus release can't spawn a second reaper. Recording after the
   * unlock would let both happen first: the join request would find nothing
   * to join and the second reaper's record would then be overwritten, so one
   * thread would never be joined. */
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  _ccol_thread_id_t reaper;
  bool reaper_create_fails = false;
#ifdef RUNNING_UNIT_TESTS
  reaper_create_fails =
      atomic_load(&g_force_reaper_thread_create_fail_for_tests);
#endif /* RUNNING_UNIT_TESTS */
  if (reaper_create_fails ||
      _ccol_thread_create(reaper, _engine_reaper_fn, NULL) != 0) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    /* For an out-of-memory kind of failure like this, running the reaper
     * inline is the safest fallback we have. Nothing needs joining afterwards
     * since the reaper ran to completion synchronously. */
    _engine_reaper_fn(NULL);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  unsigned delay_ms = atomic_load(&g_spawn_reaper_delay_ms_for_tests);
  if (delay_ms) {
    struct timespec nap = {(time_t)(delay_ms / 1000),
                           (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&nap, NULL);
  }
#endif /* RUNNING_UNIT_TESTS */
  srv_engine_bundler.reaper_thread = reaper;
  srv_engine_bundler.reaper_joinable = true;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
}

/* Defined further down with the rest of the engine-stop watcher's
 * signal-safe machinery. Declared here because _engine_acquire, its only
 * caller, comes first in the file. */
static bool _engine_stop_watcher_ensure_started_locked(void);
static void _engine_forget_inherited_locked(void);

/* Always sets out_currently_stopping, as the first thing after locking
 * srv_engine_bundler.mutex. It is true only when a reap is tearing the
 * shared reactor down right now, forced through chttpsvr_engine_stop() or
 * graceful because the last other server's chttpsvr_destroy() dropped
 * reactor_refs to 0, and then we return immediately: we do NOT wait for that
 * reap and never increment reactor_refs. On every other path, success or a
 * real failure, it's false, so the caller never needs to clear it first.
 *
 * Blocking here with something like `while (stopping)
 * _ccol_cond_var_wait(...)` would only be correct if the calling
 * chttpsvr_start() held nothing a concurrent reap could wait on, and it
 * holds exactly that for its whole duration: its own resolve pin
 * (pending_resolve_count). _quiesce_server_once()'s claim on this exact
 * server blocks on that pin before doing any real work, so a reap reaching
 * this server while its chttpsvr_start() waits here deadlocks forever. The
 * reap can't finish because it waits for a pin only this call can release,
 * and this call can't finish because it waits for `stopping`, which only the
 * very reap it's stuck behind can clear. What the caller does instead is
 * drop the pin and retry from the start rather than block here; see the
 * comment at the "currently stopping" branch of chttpsvr_start()'s retry
 * loop.
 *
 * The caller never reads the ccol_retval_t that comes back with
 * *out_currently_stopping == true, so it carries no meaning. It's
 * always ccol_unexpected_failure, just so the return statement has a value
 * of the right type. */
static ccol_retval_t _engine_acquire(bool *out_currently_stopping,
                                     uintptr_t *out_gen) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  /* First thing under the lock: make sure the signal-safe stop watcher is
   * live for the rest of this call, and so for every later call in the
   * process that locks srv_engine_bundler.mutex. That closes the
   * self-deadlock window described in the doc comment of
   * chttpsvr_engine_stop() and the comment of g_engine_stop_watcher. Before
   * that we forget an engine inherited by a fork() child.
   *
   * An engine without the watcher would ignore chttpsvr_engine_stop(), and a
   * chttpsvr_engine_wait() counting on that stop would wait forever, so we
   * fail the acquire instead and a later start tries to create the watcher
   * again. */
  _engine_forget_inherited_locked();
  *out_currently_stopping = false;
  if (!_engine_stop_watcher_ensure_started_locked()) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    return ccol_unexpected_failure;
  }
  if (srv_engine_bundler.stopping) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    *out_currently_stopping = true;
    return ccol_unexpected_failure;
  }
  _join_reaper_if_needed_locked();

  if (!srv_engine_bundler.reactor) {
    /* The default is 1 and not an auto-detected CPU count (the doc comment
     * of chttpsvr_set_engine_num_reactor_threads compares the two).
     * One dedicated poller thread that also runs every callback inline
     * measured faster than CPU-count dispatch threads, with more consistent
     * latency, for plain HTTP and for TLS with reused connections, which is
     * what a population of well-behaved clients looks like. Many dispatch
     * threads only win under a synthetic storm of TLS handshakes where every
     * request opens a new connection, and even there only by a moderate
     * margin. For most servers 1 is the setting to keep. */
    size_t nthreads = srv_engine_bundler.num_reactor_threads;
    if (nthreads == 0) nthreads = 1;
    srv_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    srv_engine_bundler.reactor = ccol_event_loop_create_mp(
        256, 4, nthreads, srv_engine_bundler.mprocs, &err);
    if (!srv_engine_bundler.reactor) {
      _ccol_mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }
    srv_engine_bundler.reactor_pid = getpid();

    if (!srv_engine_bundler.log && srv_engine_bundler.user_log)
      srv_engine_bundler.log = srv_engine_bundler.user_log;
    if (!srv_engine_bundler.log) {
      srv_engine_bundler.log = clog_create_from_fd_mp(
          2, CLOG_FATAL, NULL, srv_engine_bundler.mprocs);
      if (!srv_engine_bundler.log) {
        /* Roll back the reactor we just created. A reactor left running with
         * reactor_refs at 0 leaks for the life of the process, since nothing
         * would ever call _engine_release() to reap it; that only happens
         * after a server has successfully contributed a reference. Destroying
         * it synchronously here is safe even with srv_engine_bundler.mutex
         * held, unlike the similar code in chttpclient for its deadline-sweep
         * thread. Another thread CAN contend for this mutex right now, since
         * the async-signal-safe engine-stop watcher blocks on it like any
         * other locker if a signal fires chttpsvr_engine_stop() in exactly
         * this window. That's ordinary contention and no deadlock risk,
         * because nobody waiting on this mutex needs THIS thread to make
         * progress before getting the lock. What matters for the synchronous
         * join inside ccol_event_loop_destroy() below is that nothing can
         * call back INTO this mutex from under it. The idle-timeout sweep
         * thread is only started by chttpsvr_start(), strictly AFTER this
         * function has returned success, and the ccol_event_loop reactor
         * threads know nothing about srv_engine_bundler. So nothing can
         * re-enter _SRV_ENGINE_LOG or chttpsvr_set_engine_logger() (both lock
         * this mutex) from inside the join and deadlock against the lock this
         * thread holds. */
        ccol_event_loop_destroy(srv_engine_bundler.reactor);
        srv_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
        _ccol_mutex_unlock(srv_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(srv_engine_bundler.log, "component", "http-server-engine");
    }

    /* We log here, still under the lock, and on purpose do NOT defer it past
     * _ccol_mutex_unlock like _engine_reaper_fn defers its "engine destroyed"
     * log. The deferral is safe there for one specific reason: that function
     * resets srv_engine_bundler.log to CLOG_INVALID under the lock before
     * unlocking, so a concurrent chttpsvr_set_engine_logger() can only see and
     * close the NEW logger it installs, never the handle the reaper copied.
     * Here srv_engine_bundler.log still holds this exact logger after the
     * block, and it stays the live engine logger until something replaces it.
     * Deferring would leave a window where a concurrent
     * chttpsvr_set_engine_logger() swaps the logger out and clog_close()s it
     * before the deferred call runs, because that call only synchronizes
     * through this same mutex and doesn't depend on reactor_refs or any other
     * pin. That's a real use-after-free on the logger, not just a lost log
     * line. The call is rare anyway, it only fires when a fresh reactor is
     * created, once per full stop cycle and not on every chttpsvr_start(), so
     * the contention a slow custom logger can cause here for other users of
     * the mutex is minor. */
    clog_info(srv_engine_bundler.log,
              "New http server reactor engine has been created");
    /* 0 means "no engine running", so the counter skips it on wraparound. */
    if (++srv_engine_bundler.gen_counter == 0) srv_engine_bundler.gen_counter++;
    atomic_store(&srv_engine_bundler.generation,
                 srv_engine_bundler.gen_counter);
  }
  srv_engine_bundler.reactor_refs++;
  *out_gen = atomic_load(&srv_engine_bundler.generation);
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

/* Drops the engine reference of generation gen and tells whether it was the
 * last one, in which case the caller must call _spawn_reaper(). stopping is
 * already set by then, so no other release, stop or start can spawn a
 * second reaper in between, and chttpsvr_engine_wait() is already waiting
 * for the reap. */
static bool _engine_release_begin(uintptr_t gen) {
  /* Every real caller runs on a server that holds an engine reference it
   * contributed, and only an earlier _engine_acquire() of that same server
   * can take one, so that call's _ccol_call_once has already fired. The
   * project rule still says to guard every function that touches the
   * primitive directly and not rely on call-graph reasoning, so this one
   * has its own guard. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool should_reap = false;
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  /* A reference to an engine a force-stop has already reaped doesn't belong
   * to the engine running now. Taking it off a newer engine's refcount would
   * reap that engine from under the servers holding it. */
  if (gen == 0 || gen != atomic_load(&srv_engine_bundler.generation)) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    return false;
  }
  if (srv_engine_bundler.reactor_refs > 0) srv_engine_bundler.reactor_refs--;
  /* The !srv_engine_bundler.stopping guard is for the case
   * _engine_force_stop_quiesce_all creates. That function calls
   * _quiesce_server_once for every server it quiesces, which calls us, WHILE
   * a reaper spawned by chttpsvr_engine_stop() is already running. At that
   * point srv_engine_bundler.stopping is already true and
   * srv_engine_bundler.reactor isn't NULL yet because the reaper hasn't
   * destroyed it. Without the guard the release of the last reference in
   * that pass would look like the ordinary graceful case of the last server
   * dropping its reference, and we'd spawn a second, redundant reaper that
   * races the one already tearing this reactor down. */
  if (srv_engine_bundler.reactor_refs == 0 && srv_engine_bundler.reactor &&
      !srv_engine_bundler.stopping) {
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return should_reap;
}

static void _engine_release(uintptr_t gen) {
  if (_engine_release_begin(gen)) _spawn_reaper();
}

static void _engine_wait_until_stopped(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  _engine_forget_inherited_locked();
  while (srv_engine_bundler.reactor || srv_engine_bundler.stopping)
    _ccol_cond_var_wait(srv_engine_bundler.stopped_cv,
                        srv_engine_bundler.mutex);
  _join_reaper_if_needed_locked();
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
}

/* The actual force-stop logic. It takes srv_engine_bundler.mutex and, with a
 * live reactor, can go on to _ccol_thread_create() through _spawn_reaper().
 * Neither pthread_mutex_lock nor pthread_create is async-signal-safe in
 * POSIX, so this must only ever run on an ordinary thread, never in a signal
 * handler. The comment of g_engine_stop_watcher below describes the
 * signal-safe path chttpsvr_engine_stop() takes to get here. */
static void _engine_force_stop_now(uintptr_t gen) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool should_reap = false;
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  _engine_forget_inherited_locked();
  /* The same !srv_engine_bundler.stopping guard as in _engine_release (the
   * doc comment there says why). Without it a SECOND chttpsvr_engine_stop()
   * can arrive while the reaper of a FIRST one is half way through its
   * teardown, when srv_engine_bundler.stopping is already true but
   * srv_engine_bundler.reactor isn't NULL yet. The reaper only clears it near
   * its very end, well after _engine_force_stop_quiesce_all,
   * _idle_sweep_stop_if_running and ccol_event_loop_destroy have run, which
   * can take real wall-clock time. The second call would spawn a SECOND
   * reaper holding the same live ccol_event_loop handle, and both reapers
   * would call ccol_event_loop_destroy() on it. The contract in cthreadcomm.h
   * makes that fatal every time, with a _ccol_fatal_err() and a SIGABRT,
   * whether the two destroys are sequential or overlap.
   * chttpsvr_engine_stop() is documented as the thing to wire into a signal
   * handler, and a second SIGTERM or SIGINT can easily come before the first
   * teardown is done, as can a defensive double call in the application's
   * shutdown code; that's ordinary, not a rare corner case. This guard is
   * what turns a repeated or overlapping chttpsvr_engine_stop() into the
   * documented safe no-op instead of a process abort. */
  if (srv_engine_bundler.reactor && !srv_engine_bundler.stopping &&
      gen == atomic_load(&srv_engine_bundler.generation)) {
    srv_engine_bundler.reactor_refs = 0;
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) _spawn_reaper();
}

/* ========================================================================== */
/*                    SIGNAL-SAFE ENGINE-STOP WATCHER                         */
/* ========================================================================== */

/*
 * chttpsvr.h documents chttpsvr_engine_stop() as async-signal-safe, so an
 * application can install it, or a thin wrapper around it, as its SIGTERM or
 * SIGINT handler for a graceful shutdown. _engine_force_stop_now() above is
 * NOT async-signal-safe though: it calls _ccol_mutex_lock() on a plain,
 * non-recursive pthread mutex and _ccol_thread_create() through
 * _spawn_reaper(), and POSIX lists neither as async-signal-safe. Both are
 * real, reachable self-deadlocks. Plenty of other code takes
 * srv_engine_bundler.mutex: _engine_acquire(), which chttpsvr_start() calls
 * synchronously, _engine_release() and _engine_release_begin(), called by a
 * failed chttpsvr_start() and by _quiesce_server_once, and
 * chttpsvr_set_engine_logger, chttpsvr_set_engine_mem_mgmt_procs and
 * chttpsvr_set_engine_num_reactor_threads. Say a signal lands on the very
 * thread that is inside one of those, for instance a SIGTERM in the middle of
 * chttpsvr_start(), which is a realistic race when a container shuts down or
 * does a rolling restart. The handler's
 * _ccol_mutex_lock(srv_engine_bundler.mutex) then deadlocks against the lock
 * its own thread already holds, and the whole process hangs until a SIGKILL
 * ends it, exactly the opposite of what a graceful-shutdown handler is for.
 * An extra thread that sleeps nearly all the time is a small price for
 * avoiding that.
 *
 * So every step that touches a mutex or _ccol_thread_create moves off the
 * thread calling chttpsvr_engine_stop() onto this dedicated watcher thread,
 * which is always running. chttpsvr_engine_stop() itself does at most two
 * things, both async-signal-safe in POSIX: a lock-free atomic load to check
 * that the watcher is up, and sem_post(), the one synchronization primitive
 * POSIX explicitly lists as async-signal-safe (see _ccol_semaphore_post() in
 * common.h). The watcher blocks in _ccol_semaphore_wait(), an ordinary
 * execution context and not a signal context, so _ccol_mutex_lock() and
 * _ccol_thread_create() are fine there, and when it wakes up it does the same
 * work as _engine_force_stop_now().
 */
static struct {
  _ccol_semaphore_t sem;
  /* True once both sem and thread are fully live. It's the only field
   * chttpsvr_engine_stop() itself reads, with a lock-free atomic load that's
   * fine in a signal handler, where a mutex-guarded bool would not be. */
  _Atomic bool ready;
  _ccol_thread_id_t thread;
  /* Guards the attempts to start the watcher. Only
   * _engine_stop_watcher_ensure_started_locked() touches it, always with
   * srv_engine_bundler.mutex held, and that function is only ever called on
   * an ordinary thread (ultimately from _engine_acquire()), never in a signal
   * handler. We can't reuse `ready` for this: a start that fails
   * temporarily, like an out-of-memory error, must be retryable on a later
   * call, and without a separate flag `ready` would be stuck at false
   * forever. */
  bool started;
  _Atomic bool exit_requested;
  /* This thread does two kinds of work and each has its own request flag,
   * set before the matching _ccol_semaphore_post(). Having one flag per kind
   * is what makes an extra, already-consumed post harmless. The semaphore
   * counts posts but can't tell which request a post belongs to, and with a
   * single "something happened" wakeup the thread could treat a reaper-join
   * request posted by _engine_reaper_fn as a force-stop and tear a healthy
   * engine down. Since a flag is set before its own post, it's always seen by
   * the iteration that consumes that post or by an earlier one, so no request
   * gets lost either. */
  /* The engine generation a stop request names, or 0 when none is pending.
   * chttpsvr_engine_stop() stores the generation running at the time of the
   * call, and the watcher only stops the engine while that same generation
   * is still running, so a request never hits an engine started after it. */
  _Atomic uintptr_t stop_requested;
  /* Set by the engine reaper as its very last act, to ask this thread to
   * _ccol_thread_join() it. See the comment of _engine_reaper_fn at that
   * store and _engine_join_finished_reaper below. */
  _Atomic bool join_reaper_requested;
  /* The process that started the watcher thread (see
   * _engine_forget_inherited_locked). srv_engine_bundler.mutex guards it,
   * like started. */
  pid_t pid;
} g_engine_stop_watcher = {0};

/* Forgets the engine a fork() child inherited from its parent. The child gets
 * a copy of the reactor handle, its references, its generation, its fallback
 * logger and the reaper and stop-watcher flags, but none of the threads. A
 * reactor without a poller never serves a connection and waiting for a
 * reaper or watcher that doesn't exist never ends, so the child starts with
 * no engine and its first server creates a reactor of its own. It runs on
 * every path that acquires, stops or waits for the engine, under
 * srv_engine_bundler.mutex, and it decides by process ID, so it also works
 * with CCOL_FORK_SAFETY_REQUIRED set to 0. We leave the inherited reactor
 * itself alone since its threads belong to the parent. The references of
 * servers the parent started belong to an engine the child never runs, which
 * is why forking while a server runs is unsupported. */
static void _engine_forget_inherited_locked(void) {
  if (g_engine_stop_watcher.started && g_engine_stop_watcher.pid != getpid()) {
    g_engine_stop_watcher.started = false;
    atomic_store(&g_engine_stop_watcher.ready, false);
    atomic_store(&g_engine_stop_watcher.join_reaper_requested, false);
    atomic_store(&g_engine_stop_watcher.stop_requested, (uintptr_t)0);
  }
  if (!srv_engine_bundler.reactor && !srv_engine_bundler.stopping &&
      !srv_engine_bundler.reaper_joinable)
    return;
  if (srv_engine_bundler.reactor_pid == getpid()) return;
  clog inherited_log = srv_engine_bundler.log;
  srv_engine_bundler.log = CLOG_INVALID;
  if (inherited_log && inherited_log != srv_engine_bundler.user_log)
    clog_close(inherited_log);
  srv_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  srv_engine_bundler.reactor_refs = 0;
  srv_engine_bundler.stopping = false;
  srv_engine_bundler.reaper_joinable = false;
  atomic_store(&srv_engine_bundler.generation, (uintptr_t)0);
}

#ifdef RUNNING_UNIT_TESTS
/* The process that created the engine's reactor, for a test that checks a
 * fork() child runs a reactor of its own. */
pid_t _chttpsvr_engine_reactor_pid_for_tests(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  pid_t pid = srv_engine_bundler.reactor ? srv_engine_bundler.reactor_pid : 0;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return pid;
}
#endif

/* Asks the watcher thread to join the engine reaper. The request is made at
 * the very end of _engine_reaper_fn (that call explains why, and why it must
 * be the function's last statement). Unlike _engine_force_stop's flag,
 * which is only raised when a watcher exists to consume it, this one is
 * always raised, and that's what lets the backstop in
 * _cleanup_engine_stop_watcher notice a pending request no watcher was ever
 * around to service. */
static void _engine_request_reaper_join(void) {
  atomic_store(&g_engine_stop_watcher.join_reaper_requested, true);
  if (atomic_load(&g_engine_stop_watcher.ready))
    _ccol_semaphore_post(g_engine_stop_watcher.sem);
}

/* Joins the engine reaper thread once it's done. For an application that
 * destroys its last server and then neither starts another nor calls
 * chttpsvr_engine_wait(), this join is what keeps an unjoined thread from
 * being left behind. A joinable thread that returned keeps its stack mapping
 * and glibc bookkeeping until somebody joins it, real address space held for
 * the rest of the process, so "nobody happened to ask" isn't acceptable for
 * the engine's teardown thread.
 *
 * The !stopping test is what makes the _ccol_thread_join() below safe with
 * srv_engine_bundler.mutex held. The reaper clears stopping in its own last
 * locked section and takes no lock after that, so a reaper we see with
 * stopping already false can never block on this mutex. _engine_acquire and
 * _engine_wait_until_stopped rely on the same invariant for their own
 * _join_reaper_if_needed_locked() calls, since both only get there with
 * stopping false. Joining a reaper that may still be half way through its
 * teardown would deadlock us against it. If stopping is true a fresh reap
 * is already running, which by construction means the previous reaper was
 * joined before this one could be spawned, so skipping isn't a missed join
 * and the new reaper posts its own request when it finishes. */
static void _engine_join_finished_reaper(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  if (!srv_engine_bundler.stopping) _join_reaper_if_needed_locked();
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box hook for a regression test only. True while there is an engine
 * reaper thread we spawned that nobody has joined yet. It pins the property
 * that an application which destroys its last server and then neither starts
 * another one nor calls chttpsvr_engine_wait() still ends up with false here
 * without any further call of its own (see _engine_join_finished_reaper
 * above). Like every other white-box helper in this file it's compiled out of
 * a production build. */
bool _chttpsvr_engine_reaper_unjoined_for_tests(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  bool unjoined = srv_engine_bundler.reaper_joinable;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return unjoined;
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
/* A delay in ms the watcher waits after each wakeup before acting on a stop
 * request, so a test can start an engine between a chttpsvr_engine_stop()
 * call and the work it asks for. */
static _Atomic unsigned g_engine_stop_watcher_delay_ms_for_tests = 0;
void _chttpsvr_set_engine_stop_watcher_delay_for_tests(unsigned ms) {
  atomic_store(&g_engine_stop_watcher_delay_ms_for_tests, ms);
}
static void _engine_stop_watcher_delay_if_armed(void) {
  unsigned ms = atomic_load(&g_engine_stop_watcher_delay_ms_for_tests);
  if (!ms) return;
  struct timespec nap = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
  nanosleep(&nap, NULL);
}
#endif /* RUNNING_UNIT_TESTS */

static void *_engine_stop_watcher_fn(void *arg) {
  (void)arg;
  for (;;) {
    _ccol_semaphore_wait(g_engine_stop_watcher.sem);
    if (atomic_load(&g_engine_stop_watcher.exit_requested)) return NULL;
    if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
      _engine_join_finished_reaper();
#ifdef RUNNING_UNIT_TESTS
    _engine_stop_watcher_delay_if_armed();
#endif /* RUNNING_UNIT_TESTS */
    uintptr_t stop_gen =
        atomic_exchange(&g_engine_stop_watcher.stop_requested, (uintptr_t)0);
    if (stop_gen != 0) _engine_force_stop_now(stop_gen);
  }
}

/* Starts the watcher thread the first time we need it, and keeps retrying on
 * every call until it works, because an out-of-memory failure here is
 * temporary and must not cost chttpsvr_engine_stop() its ability to act for
 * good. The caller must already hold srv_engine_bundler.mutex and must be an
 * ordinary thread, not a signal handler. Its only caller, _engine_acquire,
 * calls it as the very first step after taking the lock, before any of its
 * own reactor-creation work, so the watcher is live for the rest of that call
 * and for every later call in the process that locks
 * srv_engine_bundler.mutex, which closes the self-deadlock window above. One
 * small gap is left, a signal arriving in the few instructions before the
 * first call of this function. Nobody holds srv_engine_bundler.mutex at that
 * point since this is the very first statement taking it, so the
 * self-deadlock can't happen there either. A stop request in that sliver has
 * nothing to act on yet, because nothing is running to stop, and it's a
 * documented no-op like any other chttpsvr_engine_stop() before the engine
 * has ever started. We don't quietly keep the request and replay it against
 * some later, unrelated chttpsvr_start(), since that would bring a
 * long-forgotten stop back against a server started much later in the same
 * process. */
#ifdef RUNNING_UNIT_TESTS
/* While set, creating the watcher thread fails as if the system had no thread
 * left to give. */
static _Atomic bool g_force_watcher_thread_create_fail_for_tests = false;
void _chttpsvr_force_watcher_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_watcher_thread_create_fail_for_tests, force);
}
#endif /* RUNNING_UNIT_TESTS */

/* Starts the watcher thread if it isn't running and tells whether it runs. */
static bool _engine_stop_watcher_ensure_started_locked(void) {
  if (g_engine_stop_watcher.started) return true;
  if (_ccol_semaphore_init(g_engine_stop_watcher.sem, 0) != 0) return false;
  bool created = false;
#ifdef RUNNING_UNIT_TESTS
  if (!atomic_load(&g_force_watcher_thread_create_fail_for_tests))
#endif
    created = _ccol_thread_create(g_engine_stop_watcher.thread,
                                  _engine_stop_watcher_fn, NULL) == 0;
  if (!created) {
    _ccol_semaphore_destroy(g_engine_stop_watcher.sem);
    return false;
  }
  g_engine_stop_watcher.started = true;
  g_engine_stop_watcher.pid = getpid();
  atomic_store(&g_engine_stop_watcher.ready, true);
  return true;
}

/* Joins the watcher thread at process exit, plus any engine reaper the
 * watcher didn't get to; otherwise the valgrind pass of "make memtest"
 * reports either thread's stack as "possibly lost". Reading `started` and
 * `thread` here without a mutex is safe whether or not an application-created
 * chttpsvr is still alive at exit, which is different from
 * _cleanup_chttpsvr_router_shells and _cleanup_chttpsvr_slot_table, which
 * check every handle for liveness before freeing anything exactly for that
 * reason. Both fields are written once for the life of the process, together,
 * the first time any chttpsvr acquires the shared engine (see
 * _engine_stop_watcher_ensure_started_locked, the only writer), and nothing
 * ever un-starts the watcher. A plain read racing that one write sees either
 * the pre-start state (false and unset) or the fully started one, never a
 * torn state or one that went backwards. */
__attribute__((destructor)) static void _cleanup_engine_stop_watcher(void) {
  if (g_engine_stop_watcher.started) {
    atomic_store(&g_engine_stop_watcher.exit_requested, true);
    _ccol_semaphore_post(g_engine_stop_watcher.sem);
    _ccol_thread_join(g_engine_stop_watcher.thread);
    /* We clear this flag before destroying the semaphore instead of leaving
     * it true. Being async-signal-safe means a signal handler may call
     * chttpsvr_engine_stop() at any point of process exit, including the
     * narrow window after this destructor and before the process is gone, and
     * all that call does (_engine_force_stop) is a bare
     * `if (ready) sem_post(sem)`. We don't control the order of destructors
     * and atexit handlers across the shared objects of a process (see the
     * standing caution about that in this file). With the flag left true such
     * a racing sem_post() would hit a semaphore this destructor already
     * destroyed, a real use-after-free class violation on a primitive and not
     * just a lost stop request. */
    atomic_store(&g_engine_stop_watcher.ready, false);
    _ccol_semaphore_destroy(g_engine_stop_watcher.sem);
  }
  /* Backstop for what the watcher can't cover. It runs whether or not a
   * watcher was ever started, and there are two cases. Either a reaper raised
   * its join request while no watcher existed to service it, which happens
   * when this destructor had already stopped the watcher and a later destroy
   * stopped the engine, or a reaper raised it in the sliver between the
   * watcher's last semaphore wait and the exit request just above. The
   * request flag gates the backstop instead of running it every time, so it
   * never touches a reaper still half way through its teardown, and the
   * !stopping test inside _engine_join_finished_reaper makes the same
   * promise a second time, since a fresh reap may be spawned after the flag
   * is raised. */
  if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
    _engine_join_finished_reaper();

  /* The chttpsvr_set_engine_logger() logger outlives every engine, so it
   * gets closed at process exit. All reapers are joined by now, so none of
   * them reads it any more, but an engine still running at exit keeps it
   * because its reactor threads may still log through it. */
  if (srv_engine_bundler.user_log) {
    _ccol_mutex_lock(srv_engine_bundler.mutex);
    clog user_log = CLOG_INVALID;
    if (!srv_engine_bundler.log) {
      user_log = srv_engine_bundler.user_log;
      srv_engine_bundler.user_log = CLOG_INVALID;
    } else {
      /* An engine is still running, e.g. one whose last server the
       * application destroys from its own destructor that runs after this
       * one. Its reaper closes the logger when the engine stops. */
      srv_engine_bundler.close_user_log_at_stop = true;
    }
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    if (user_log) clog_close(user_log);
  }
}

/* The real public entry point. The comment of g_engine_stop_watcher above
 * explains why it is on purpose just this one async-signal-safe operation,
 * with no _ccol_mutex_lock and no _ccol_thread_create of its own, directly or
 * indirectly. With `ready == false` it does nothing, since that means no
 * engine has ever started in this process or, for a very narrow window, one
 * is starting for the first time right now. That matches its documented
 * contract of being safe to call before the reactor ever starts; the comment
 * of _engine_stop_watcher_ensure_started_locked says why we don't keep the
 * request and replay it later. */
static void _engine_force_stop(void) {
  /* Generation 0 means no engine is running, so there's nothing for this
   * request to stop. */
  uintptr_t gen = atomic_load(&srv_engine_bundler.generation);
  if (gen != 0 && atomic_load(&g_engine_stop_watcher.ready)) {
    /* The flag is set inside the `ready` test and not before it. A flag
     * raised while no watcher exists to consume it would sit there until some
     * unrelated, much later post wakes the watcher, and that would bring a
     * long-forgotten stop request back against a server started well
     * afterwards. A lock-free atomic store is just as async-signal-safe as
     * the atomic load above it. */
    atomic_store(&g_engine_stop_watcher.stop_requested, gen);
    _ccol_semaphore_post(g_engine_stop_watcher.sem);
  }
}

#ifdef RUNNING_UNIT_TESTS
/* White-box hook for a regression test only. It locks
 * srv_engine_bundler.mutex, exactly the critical section the doc comment of
 * chttpsvr_engine_stop() says a signal handler must be able to interrupt
 * safely, and then delivers `sig` to the calling thread synchronously with
 * raise(3). In a multithreaded program POSIX defines raise(3) as
 * pthread_kill(pthread_self(), sig), so sig's installed handler, if any, runs
 * to completion on this same thread with the mutex still held before raise()
 * returns. That reproduces a signal landing on a thread that already holds
 * srv_engine_bundler.mutex, deterministically and without depending on
 * timing. The hook isn't vacuous: if the caller's handler for `sig` calls
 * chttpsvr_engine_stop(), this call deadlocks as soon as that ever reaches
 * _ccol_mutex_lock or _ccol_thread_create directly (g_engine_stop_watcher
 * above explains why it must not), which is exactly what a real SIGTERM
 * handler installed the documented way would do. It's compiled out of a
 * production build. */
void _chttpsvr_test_hold_engine_mutex_and_signal_self(int sig) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  raise(sig);
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* ========================================================================== */
/*                    IDLE-TIMEOUT SWEEP (module-local, own thread)           */
/* ========================================================================== */

static void _conn_close(chttpsvr_conn_t *conn);
static void _conn_linger(chttpsvr_conn_t *conn);
static void _conn_linger_step(chttpsvr_conn_t *conn);
static bool _conn_request_bytes_may_remain(chttpsvr_conn_t *conn);
static void _conn_reject_and_close(chttpsvr_conn_t *conn, bool run_middleware);
/* Defined much further down, near __chttpsvr_destroy. Declared here because
 * _conn_on_removed comes well before it and has to call it, possibly long
 * after the caller of __chttpsvr_destroy has returned. The comment of
 * lifetime_refs in struct chttpserver says why this is a separate step we can
 * defer. */
static void _chttpsvr_finish_destroy(struct chttpserver *raw);
static void _retired_pools_reap(struct chttpserver *srv, bool wait_all);

/* Slow-client machinery, see the "SLOW CLIENTS" section further down. */
static void _timespec_add_ms(struct timespec *ts, unsigned ms);
static void _slow_clock(struct timespec *ts);
static void _mem_release(chttpsvr_conn_t *conn);
static void _sweep_parked_waits(struct chttpserver *srv,
                                struct timespec now_slow);
static bool _parked_expired(struct chttpserver *srv, chttpsvr_conn_t *c,
                            struct timespec now, struct timespec now_slow);
static bool _parked_needs_kernel_query(struct chttpserver *srv,
                                       const chttpsvr_conn_t *c,
                                       struct timespec now,
                                       struct timespec now_slow);
#ifdef RUNNING_UNIT_TESTS
/* How many kernel queries judged a parked connection, and how many of them the
 * sweep made while holding idle_mutex (that one must stay 0). */
static _Atomic size_t g_parked_queries_for_tests = 0;
static _Atomic size_t g_parked_queries_locked_for_tests = 0;
static __thread bool g_sweep_holds_idle_mutex_for_tests = false;
static void _parked_query_note_for_tests(void) {
  atomic_fetch_add_explicit(&g_parked_queries_for_tests, 1,
                            memory_order_relaxed);
  if (g_sweep_holds_idle_mutex_for_tests)
    atomic_fetch_add_explicit(&g_parked_queries_locked_for_tests, 1,
                              memory_order_relaxed);
}
void _chttpsvr_parked_queries_for_tests(size_t *total, size_t *locked) {
  *total = atomic_load(&g_parked_queries_for_tests);
  *locked = atomic_load(&g_parked_queries_locked_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */
static void _conn_resume_parked(chttpsvr_conn_t *conn);
static bool _conn_divert_gate(chttpsvr_conn_t *conn);
static ssize_t _drain_body_socket_read(chttpsvr_conn_t *conn,
                                       chttp1_stream_t *stream, char *raw,
                                       size_t len);
static void _send_response_park(chttpsvr_conn_t *conn, chttp1_stream_t *stream,
                                const char *head, size_t head_len,
                                size_t body_pos, bool keep_alive, bool no_body,
                                bool internal);
static void _conn_park_write(chttpsvr_conn_t *conn, struct chttpserver *srv);
static bool _conn_park_arm(chttpsvr_conn_t *conn, uint8_t kind);
static void _conn_end_parked_write(chttpsvr_conn_t *conn);
static void _conn_dispatch_reject(chttpsvr_conn_t *conn);
static void _parked_unlink_locked(struct chttpserver *srv, chttpsvr_conn_t *c);
static void _task_park_body(chttpsvr_conn_t *conn, struct chttpserver *srv,
                            chttp1_stream_t *stream, chttpsvr_req *req);

/* Target capacity for a doubling pointer array backed by plain realloc: cap*2,
 * or `initial` the first time when cap is 0. It returns 0 when the doubling
 * itself would overflow size_t or the caller's coming multiplication by
 * elem_size would, and the caller reads 0 as "can't grow right now".
 * Every caller in this file already has a graceful, tested path for a realloc
 * that returns NULL on out-of-memory and treats a 0 from here the same way
 * (see the comments of _servers_register and
 * _chttpsvr_router_shell_register). All the doubling registries of this file
 * that use plain malloc and realloc, instead of this module's
 * ccol_memmgmt_procs_t helpers, share it. It's the project's usual guard
 * relative to SIZE_MAX, the same one _router_add_route, chttpsvr_subrouter,
 * chttpsvr_resp_set_header and _parse_qparams use with _ccol_mem_realloc
 * instead of plain realloc. It's pure on purpose and touches no global state,
 * so a white-box test can drive it with a fake cap instead of growing one of
 * the real process-wide registries to an extreme size (see
 * _chttpsvr_doubling_growth_cap_for_tests below). */
static size_t _doubling_growth_cap(size_t cap, size_t elem_size,
                                   size_t initial) {
  if (cap == 0) return initial;
  if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / elem_size) return 0;
  return cap * 2;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * White-box test helper exposing _doubling_growth_cap directly. It isn't
 * public API, and like every other white-box helper of this file it's
 * compiled out of a production libccollections.so.
 */
size_t _chttpsvr_doubling_growth_cap_for_tests(size_t cap, size_t elem_size,
                                               size_t initial) {
  return _doubling_growth_cap(cap, elem_size, initial);
}
#endif /* RUNNING_UNIT_TESTS */

/* Registers a server so the idle sweep thread walks its idle connections too.
 * chttpsvr_start calls it, and not just once per server but again on every
 * stop/start restart cycle, so it is idempotent and skips the add when
 * srv is already there. Without that check a srv that goes through N restart
 * cycles would sit in servers_bundler.servers N+1 times, while
 * _servers_unregister below removes only one of them, leaving N stale,
 * dangling pointers once __chttpsvr_destroy frees srv. The idle sweep thread
 * would read them on its very next pass, a real use-after-free that valgrind
 * catches through restart_races_live_keep_alive_connection_is_safe (it
 * restarts one srv 5 times before destroying it). */
#ifdef RUNNING_UNIT_TESTS
/* Makes the next _servers_register() call that needs to add a server fail,
 * like an allocation failure of its growth would. */
static _Atomic bool g_servers_register_fail_for_tests = false;
void _chttpsvr_fail_next_servers_register_for_tests(void) {
  atomic_store(&g_servers_register_fail_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

/* Returns false when srv isn't registered and can't be, because the array
 * can't grow. */
static bool _servers_register(struct chttpserver *srv) {
  /* servers_bundler.mutex is initialized by _engine_globals_init under
   * srv_engine_bundler.once. chttpsvr_start() calls us on purpose BEFORE its
   * own _engine_acquire() (see the comment at that call site), so this can be
   * the very first thing in the process to touch servers_bundler.mutex, e.g.
   * a plain construct, register_handler, start sequence that never called
   * chttpsvr_set_engine_logger, chttpsvr_set_engine_mem_mgmt_procs,
   * chttpsvr_set_engine_num_reactor_threads, chttpsvr_engine_wait or
   * chttpsvr_engine_stop before. Don't drop this guard on the grounds that
   * "some other guarded function always runs first"; see the _ccol_mutex_t
   * conventions in common.h. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      _ccol_mutex_unlock(servers_bundler.mutex);
      return true;
    }
  }
  bool room = servers_bundler.count < servers_bundler.capacity;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_servers_register_fail_for_tests, false)) {
    _ccol_mutex_unlock(servers_bundler.mutex);
    return false;
  }
#endif /* RUNNING_UNIT_TESTS */
  if (!room) {
    /* Overflow-safe growth like every other growable array in this file. If
     * it fails the registration fails, and chttpsvr_start() then fails and
     * undoes its setup, because a server running outside this array would
     * never be swept: none of its idle, header-phase or parked limits would
     * apply, and an engine force-stop would tear the reactor down under it
     * without quiescing it first. */
    size_t new_cap = _doubling_growth_cap(servers_bundler.capacity,
                                          sizeof(struct chttpserver *), 8);
    struct chttpserver **nn = new_cap > 0
                                  ? (struct chttpserver **)realloc(
                                        servers_bundler.servers,
                                        new_cap * sizeof(struct chttpserver *))
                                  : NULL;
    if (!nn) {
      _ccol_mutex_unlock(servers_bundler.mutex);
      return false;
    }
    servers_bundler.servers = nn;
    servers_bundler.capacity = new_cap;
  }
  servers_bundler.servers[servers_bundler.count++] = srv;
  _ccol_mutex_unlock(servers_bundler.mutex);
  return true;
}

/* The actual removal. It's a function of its own so the child-side fixup of
 * _chttpsvr_atfork_release_impl can use it without locking
 * servers_bundler.mutex a second time (see the doc comment of that
 * function), since that fixup already holds the mutex for its whole walk over
 * the slots, having inherited it locked from _chttpsvr_atfork_prepare. The
 * caller must already hold servers_bundler.mutex. Idempotent, and it does
 * nothing when srv isn't there. */
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
  /* Every real caller only gets here after _servers_register has already
   * fired this guard for this srv, but by the project rule every function
   * touching the primitive directly carries its own guard rather than
   * relying on the call graph. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(servers_bundler.mutex);
  _servers_unregister_locked(srv);
  _ccol_mutex_unlock(servers_bundler.mutex);
}

/* Undoes the _servers_register() and the engine reference of a
 * chttpsvr_start() that registered raw, may have acquired the shared engine,
 * and then failed further down, in the listen socket setup or in
 * ccol_event_loop_add for example. That way a failed start leaves raw exactly
 * as it was before the call. Without it the start would quietly keep the
 * shared reactor alive and raw registered with the idle-timeout sweep until
 * some later chttpsvr_destroy() reclaimed it, which the application may
 * never do. Picture an application that checks chttpsvr_start()'s return
 * value and on failure calls chttpsvr_engine_wait() before it ever calls
 * chttpsvr_destroy() on the failed handle, an order this module's header
 * doesn't warn against. It would block forever, since nothing would ever
 * drop this server's engine reference.
 *
 * It's fine to call this while this thread's chttpsvr_start() still holds
 * its resolve pin on raw, i.e. before the _chttpsvr_resolve_unpin(raw)
 * below. _servers_unregister is idempotent, and a concurrent
 * _quiesce_server_once for raw (from __chttpsvr_destroy or from
 * _engine_force_stop_quiesce_all) that might race to do the same cleanup
 * can't get past its wait on pending_resolve_count until we drop our pin,
 * always strictly after this function returns. So two callers never touch
 * raw->contributed_to_engine or servers_bundler.servers for the same raw at
 * the same time.
 *
 * acquired_here has to be exactly the `need_acquire` that chttpsvr_start()
 * computed earlier in the same call, telling whether this specific call is
 * the one that took raw from not registered with no engine reference to
 * registered with one. The other case is a RESTART of a server an earlier
 * successful chttpsvr_start() already registered and that already holds its
 * engine reference. chttpsvr_stop() never unregisters the server or drops
 * that reference, precisely so a later restart can skip a second acquire,
 * and only the call that took them may undo them. A failed restart that ran
 * this unconditionally would unregister and release a server that rightly
 * still holds a reference and is registered, and servers_bundler's
 * bookkeeping would go out of sync. If this server held the last reference
 * it could even tear down the shared reactor while raw's live idle and
 * keep-alive connections still have registrations in it, because
 * chttpsvr_stop() only closes the listener and never the existing
 * connections. That's exactly the ordering hazard the doc comment of
 * _quiesce_server_once describes and prevents. A failed restart must leave
 * raw precisely as it was before the call: registered, holding the engine
 * reference and not started, the same state a plain chttpsvr_stop() without
 * a restart leaves it in. */
static void _chttpsvr_undo_start_registration(struct chttpserver *raw,
                                              bool acquired_here) {
  if (!acquired_here) return;
  _servers_unregister(raw);
  bool release = false;
  uintptr_t gen = 0;
  _ccol_mutex_lock(raw->mutex);
  if (raw->contributed_to_engine) {
    raw->contributed_to_engine = false;
    release = true;
    gen = raw->engine_gen;
    raw->engine_gen = 0;
  }
  _ccol_mutex_unlock(raw->mutex);
  if (release) _engine_release(gen);
}

#define _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS 1000
/* Size of the stack scratch buffer into which a teardown pass
 * (_close_all_idle_connections, _mem_wait_flush, _stream_queue_flush) and
 * _mem_admit claim connections under a lock, so the close or the dispatch can
 * run outside it. Each of them wraps the batch in an outer loop to get
 * through its whole list. The sweep doesn't need a buffer at all, see
 * _sweep_server. */
#define _CHTTPSVR_IDLE_CLOSE_BATCH 64

/* Resumes srv's listener registration when _listener_on_readable paused it
 * because the server was at max_connections (see the comment of that
 * function). Does nothing unless a cap is configured and current_connections
 * has dropped back below it since the pause.
 *
 * _idle_sweep_fn's per-server loop below calls it unconditionally once per
 * sweep tick, with no gate on idle_timeout_ms, and _conn_free never calls it
 * synchronously when a connection closes. Calling ccol_event_loop_resume
 * straight from _conn_free right after its atomic_fetch_sub of
 * current_connections would be immediate and correct alone, but combined
 * with __chttpsvr_destroy's teardown it's a real use-after-free, and
 * ThreadSanitizer reports it. _drain_and_close_all_connections polls
 * current_connections down to zero (see its own comment), that poll is its
 * ONLY signal that no connection teardown needs srv any more, and then it
 * frees srv. Touching srv again after the very atomic operation that signal
 * depends on, with a resume call for instance, reopens the race the poll loop
 * closes. From the idle-timeout sweep thread we instead use the established,
 * TSan-clean way that thread touches a registered server's state, i.e.
 * walking servers_bundler.servers under servers_bundler.mutex. That's safe
 * because a server is only removed from that list under the same lock and
 * before any of its state is freed (see the _servers_unregister call site in
 * _quiesce_server_once, which runs before _drain_and_close_all_connections
 * even starts), and by then _chttpsvr_stop_internal has removed the listener
 * registration completely, so there's nothing left to resume. The price is
 * that a paused listener resumes within at most
 * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS (1 second) after a slot frees up instead of
 * almost immediately. That's fine for a documented coarse admission-control
 * knob whose doc comment (chttpsvr_config_t.max_connections) promises no
 * sub-second pickup, and it's far better than the 100 percent CPU busy loop
 * this whole mechanism exists to prevent. */
static void _listener_resume_if_capacity_freed(struct chttpserver *srv) {
  size_t cap = atomic_load(&srv->max_connections);
  if (!cap || atomic_load(&srv->current_connections) >= cap) return;
  _ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  _ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* Resumes srv's listener registration when _listener_on_readable_impl paused
 * it for resource exhaustion in accept4() or an allocation failure after the
 * accept (see the comment of listener_paused_for_resource_pressure). Does
 * nothing unless that flag is set. It's kept separate from
 * _listener_resume_if_capacity_freed above on purpose, not folded into one
 * check, because the two reasons for a pause are independent. A listener
 * paused at max_connections must stay paused as long as
 * current_connections says so, whatever this flag is, and this flag has to
 * keep the listener paused whatever current_connections says. With one flag
 * or one condition for both, a resume for one reason could fire, or be
 * suppressed, because of the other reason's state (the file's convention is
 * to verify that two flags' values really exclude each other before merging
 * them). atomic_exchange reads and clears the flag in one step. A fresh pause
 * from a new failure on the very next dispatch can race this resume and land
 * between the exchange and the ccol_event_loop_resume below, so we un-pause a
 * registration the reactor thread rightly paused a moment ago. That's
 * harmless and fixes itself: that same dispatch has set the flag to true
 * again, so the next sweep tick repeats the cycle, or more likely a new
 * dispatch from the still non-empty backlog repeats it sooner. Nothing gets
 * stuck unpaused and nothing gets stuck without a resume. */
static void _listener_resume_if_resource_pressure_cleared(
    struct chttpserver *srv) {
  if (!atomic_exchange(&srv->listener_paused_for_resource_pressure, false))
    return;
  _ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  _ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* The nanoseconds from earlier to later, negative when later comes first. */
static inline __attribute__((always_inline)) long long _ns_between(
    struct timespec later, struct timespec earlier) {
  return (long long)(later.tv_sec - earlier.tv_sec) * 1000000000LL +
         (long long)(later.tv_nsec - earlier.tv_nsec);
}

/* ns as whole milliseconds, rounded up (toward positive infinity). */
static inline __attribute__((always_inline)) long long _ms_ceil_of_ns(
    long long ns) {
  return ns > 0 ? (ns + 999999LL) / 1000000LL : ns / 1000000LL;
}

/* True when a connection counts as timed out for the idle timeout, judging
 * its last_activity against now and idle_ms. idle_ms is the resolved
 * idle_timeout_ms or read_timeout_ms, which the caller has already made sure
 * is non-zero. It's a small pure function on its own, following this file's
 * convention of a pure helper plus an accessor under RUNNING_UNIT_TESTS (like
 * _doubling_growth_cap, _accept_errno_is_transient and
 * _accept_errno_is_resource_exhaustion), so a test can drive it with a
 * synthetic, adversarial now and last_activity instead of having to win a
 * real scheduling race against a live sweep thread.
 *
 * long long and not long, for the same two separate reasons as in
 * _shrink_timeout_to_deadline a few functions up. One is overflow: idle_ms is
 * unsigned and now and last_activity can in principle be very far apart, so
 * the multiplication below must not overflow a 32-bit long on ILP32, which we
 * build and test in CI. The other is the sign. The caller, _idle_sweep_fn,
 * takes now once, before locking idle_mutex and before looking at any
 * connection, while _idle_list_add refreshes last_activity from ANOTHER thread
 * every time a connection lands back in the idle list, including the very
 * ordinary case of a keep-alive request finishing at the same time as this
 * sweep tick. So a connection can get fresh activity between the sweep's
 * `now` snapshot and the moment the sweep reaches it under idle_mutex. The
 * window is brief, but such a connection then has last_activity > now, which
 * is a legitimate and expected result of this design under normal concurrent
 * load and not some clock skew bug. That's why elapsed is signed and
 * checked for a negative value BEFORE it's compared with idle_ms. Without the
 * check we'd evict a connection that just went idle instead of computing zero
 * or negative idle time, because a bare `(unsigned long)elapsed_ms >= idle_ms`
 * silently wraps a small negative elapsed_ms into a huge unsigned value, which
 * is `>= idle_ms` for any realistic timeout. It's the same kind of defect
 * _shrink_timeout_to_deadline avoids. */
static bool _conn_idle_timed_out(struct timespec now,
                                 struct timespec last_activity,
                                 unsigned idle_ms) {
  /* Whole milliseconds rounded down, so the limit is hit exactly when idle_ms
   * have passed and never before. */
  long long elapsed_ms = _ns_between(now, last_activity) / 1000000LL;
  if (elapsed_ms < 0) return false;
  return (unsigned long long)elapsed_ms >= (unsigned long long)idle_ms;
}

/* Whether conn has spent more than max_ms in the reactor-owned phase of its
 * current request (TLS handshake plus reading the request headers), counted
 * from conn->header_phase_start.
 *
 * It's a TOTAL bound, and that's the whole point. last_activity, which
 * _conn_idle_timed_out measures against, is refreshed for every chunk of bytes
 * we feed to the parser and every return to the idle list, so it only
 * measures the GAP between one bit of activity and the next. A peer that sends
 * one byte of its header block every few seconds resets that gap forever and
 * never finishes the block, holding a file descriptor, an epoll registration
 * and a whole chttpsvr_conn_t (with the parser's 8 KiB line buffer) as long
 * as it likes for very little bandwidth, until the server hits
 * max_connections and the listener stops accepting. max_header_bytes bounds
 * how many bytes such a peer can send but not how long it can take, and the
 * peer never gets near that cap since it never completes the block. This is
 * the header-phase counterpart of max_body_read_duration_ms, which closes the
 * same trickle-forever loophole for the body.
 *
 * It goes through the same signed-elapsed helper as _conn_idle_timed_out for
 * the same reason: header_phase_start is taken on another thread than the
 * sweep's `now`, so elapsed can legitimately be negative and must never be
 * compared as unsigned. */
static bool _conn_header_phase_expired(struct timespec now,
                                       const chttpsvr_conn_t *conn,
                                       unsigned max_ms) {
  if (!conn->header_phase_active) return false;
  return _conn_idle_timed_out(now, conn->header_phase_start, max_ms);
}

/* Starts the reactor-owned phase clock for the request conn is reading now,
 * and leaves it alone if it's already running. Within one request it's
 * idempotent and the first call wins, so the budget counts from the first
 * handshake step or the first byte of the header block, whichever comes
 * first. Every later step of the same request measures against that instant
 * and none of them pushes the deadline out. */
static void _conn_header_phase_arm(chttpsvr_conn_t *conn,
                                   const struct timespec *now) {
  if (conn->header_phase_active) return;
  conn->header_phase_start = *now;
  conn->header_phase_active = true;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook exposing the pure idle-timeout decision. The real race
 * is a concurrent _idle_list_add refreshing last_activity between the sweep's
 * `now` snapshot and the moment the sweep looks at this connection, and
 * reproducing that deterministically from a test would mean winning a
 * scheduling race against a live sweep thread. With this hook a test just
 * builds the adversarial now and last_activity pair itself. Compiled out of a
 * production build like every other white-box helper here. */
bool _chttpsvr_conn_idle_timed_out_for_tests(struct timespec now,
                                             struct timespec last_activity,
                                             unsigned idle_ms) {
  return _conn_idle_timed_out(now, last_activity, idle_ms);
}
#endif /* RUNNING_UNIT_TESTS */

/* One sweep tick for one server; _idle_sweep_fn calls it for every registered
 * server with srv pinned. It enforces the idle and header-phase limits of the
 * connections the reactor owns, and the limits of the connections waiting
 * with no thread: a parked body or response, a request waiting for body
 * memory and a streaming request waiting for a thread. See _parked_expired
 * and _sweep_parked_waits. */
static size_t _sweep_server(struct chttpserver *srv, struct timespec now) {
  /* Unconditional, because a paused listener needs to be checked for every
   * registered server on every tick whether or not the idle timeout is on,
   * and this tick is the only thing that triggers it. */
  _listener_resume_if_capacity_freed(srv);
  _listener_resume_if_resource_pressure_cleared(srv);

  unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
  unsigned hdr_ms = atomic_load(&srv->max_header_read_duration_ms);
  struct timespec now_slow;
  _slow_clock(&now_slow);

  /* Connections no thread owns sit in two lists. The idle list has the ones
   * waiting for a request's headers, judged by the idle timeout and
   * max_header_read_duration_ms, and with both limits off we don't walk it at
   * all. The parked list has the ones waiting for the rest of a body or for
   * room to write a response, judged by _parked_expired; we walk it on every
   * tick, so its cost only grows with the number of parked connections. All
   * of those limits are enforced here, for two reasons: every unowned
   * connection waiting on its socket can be reached from exactly this place,
   * and unlinking a candidate under idle_mutex right here already is the
   * claim, which keeps a closer and a concurrent dispatch from both owning
   * the same connection (see _idle_list_try_claim).
   *
   * We collect the expired connections under idle_mutex and act on them
   * outside it, because _conn_close ends up in ccol_event_loop_remove and a
   * 408 goes through the reject pool, and neither may run while we hold a lock
   * a callback of that removal might also need. A parked body gets 408
   * Request Timeout since no response byte has gone out yet, while a parked
   * response is just cut off and closed.
   *
   * One tick takes EVERY expired connection of both lists, without a cap. A
   * per-tick cap looks like a safe way to bound one tick's work, but it
   * would turn the sweep into draining a fixed number of connections a
   * second, which a server whose clients leave keep-alive connections idle
   * outgrows, and expired connections would pile up faster than we close
   * them until the listener hits max_connections. We don't need a buffer to
   * collect them: an unlinked connection belongs to this tick alone, so its
   * link field from the list it left chains it into this tick's private
   * list. We read that link before acting on the connection, since a close
   * frees it and a 408 can park it again. */
  chttpsvr_conn_t *idle_close = NULL;   /* chained through idle_next */
  chttpsvr_conn_t *parked_close = NULL; /* chained through parked_next */
  chttpsvr_conn_t *parked_reject = NULL;
  /* Parked connections whose verdict needs a kernel query. We claim them
   * under the lock, judge them after it and put back the ones that didn't
   * expire. A dispatch that runs into one of them in between fails its
   * claim, and level-triggered readiness reports it again once the
   * connection is back. */
  chttpsvr_conn_t *parked_query = NULL;
  size_t expired_n = 0, lingers_n = 0;
  size_t visited = 0;
  _ccol_mutex_lock(srv->idle_mutex);
#ifdef RUNNING_UNIT_TESTS
  g_sweep_holds_idle_mutex_for_tests = true;
#endif /* RUNNING_UNIT_TESTS */
  chttpsvr_conn_t *c = (idle_ms || hdr_ms) ? srv->idle_head : NULL;
  while (c) {
    chttpsvr_conn_t *next = c->idle_next;
    visited++;
    if ((idle_ms && _conn_idle_timed_out(now, c->last_activity, idle_ms)) ||
        (hdr_ms && _conn_header_phase_expired(now, c, hdr_ms))) {
      if (c->idle_prev) c->idle_prev->idle_next = c->idle_next;
      if (c->idle_next) c->idle_next->idle_prev = c->idle_prev;
      if (srv->idle_head == c) srv->idle_head = c->idle_next;
      if (srv->idle_tail == c) srv->idle_tail = c->idle_prev;
      c->idle_prev = NULL;
      c->in_idle_list = false;
      c->idle_next = idle_close;
      idle_close = c;
      expired_n++;
    }
    c = next;
  }
  c = srv->parked_head;
  while (c) {
    chttpsvr_conn_t *next = c->parked_next;
    visited++;
    if (_parked_needs_kernel_query(srv, c, now, now_slow)) {
      _parked_unlink_locked(srv, c);
      c->parked_next = parked_query;
      parked_query = c;
    } else if (_parked_expired(srv, c, now, now_slow)) {
      _parked_unlink_locked(srv, c);
      if (c->park == _CONN_PARK_BODY) {
        c->parked_next = parked_reject;
        parked_reject = c;
      } else {
        c->parked_next = parked_close;
        parked_close = c;
      }
      if (c->park == _CONN_PARK_LINGER) lingers_n++;
      expired_n++;
    }
    c = next;
  }
#ifdef RUNNING_UNIT_TESTS
  g_sweep_holds_idle_mutex_for_tests = false;
#endif /* RUNNING_UNIT_TESTS */
  _ccol_mutex_unlock(srv->idle_mutex);
  if (parked_query) {
    chttpsvr_conn_t *keep = NULL;
    while (parked_query) {
      chttpsvr_conn_t *q = parked_query;
      parked_query = q->parked_next;
      if (_parked_expired(srv, q, now, now_slow)) {
        if (q->park == _CONN_PARK_BODY) {
          q->parked_next = parked_reject;
          parked_reject = q;
        } else {
          q->parked_next = parked_close;
          parked_close = q;
        }
        expired_n++;
      } else {
        q->parked_next = keep;
        keep = q;
      }
    }
    _ccol_mutex_lock(srv->idle_mutex);
    while (keep) {
      chttpsvr_conn_t *k = keep;
      keep = k->parked_next;
      k->parked_prev = srv->parked_tail;
      k->parked_next = NULL;
      if (srv->parked_tail) srv->parked_tail->parked_next = k;
      srv->parked_tail = k;
      if (!srv->parked_head) srv->parked_head = k;
      k->in_parked_list = true;
    }
    _ccol_mutex_unlock(srv->idle_mutex);
  }
  /* A lingering close running out is just how it normally ends, not a client
   * timeout, so we don't log it. */
  if (expired_n > lingers_n) {
    _SRV_ENGINE_LOG(clog_info, "timeout closing %zu connection(s)",
                    expired_n - lingers_n);
  }
  while (idle_close) {
    chttpsvr_conn_t *next = idle_close->idle_next;
    idle_close->idle_next = NULL;
    idle_close->park = _CONN_PARK_NONE;
    _conn_close(idle_close);
    idle_close = next;
  }
  while (parked_close) {
    chttpsvr_conn_t *next = parked_close->parked_next;
    parked_close->parked_next = NULL;
    parked_close->park = _CONN_PARK_NONE;
    _conn_close(parked_close);
    parked_close = next;
  }
  while (parked_reject) {
    chttpsvr_conn_t *r = parked_reject;
    parked_reject = r->parked_next;
    r->parked_next = NULL;
    r->park = _CONN_PARK_NONE;
    r->req_rejected = true;
    r->reject_status = CHTTP_STATUS_REQUEST_TIMEOUT;
    _conn_dispatch_reject(r);
  }

  _sweep_parked_waits(srv, now_slow);
  /* The closes above may have freed room under max_connections, and the
   * listener shouldn't have to wait for the next tick to use it. */
  if (expired_n) _listener_resume_if_capacity_freed(srv);
  /* A pool set retired by a restart whose drain has finished gets joined
   * here, so its thread doesn't hang around until the next restart or the
   * destroy. */
  _retired_pools_reap(srv, false);
  return visited;
}

static void *_idle_sweep_fn(void *arg) {
  (void)arg;
  /* Every function touching servers_bundler.mutex or srv_engine_bundler's
   * state directly has this guard itself, instead of relying on call-graph
   * reasoning like "this thread only starts after the caller of
   * _idle_sweep_start_if_needed has fired the guard" (that's the project's
   * standing rule on exactly this point). In practice it does nothing here,
   * since every real caller has run the init before this thread's first
   * iteration, but without it a future path that starts the thread earlier
   * would quietly bring back the kind of bug the rule is there to prevent. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
    /* Wait for one tick, or until _idle_sweep_stop_if_running signals a stop.
     * The stop sets the flag under wake_mu, so it either happens before this
     * check or wakes the wait, and we never miss one. */
    struct timespec tick_deadline;
    clock_gettime(CLOCK_MONOTONIC, &tick_deadline);
    _timespec_add_ms(&tick_deadline, _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS);
    _ccol_mutex_lock(idle_sweep_bundler.wake_mu);
    while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
      if (_ccol_cond_var_timedwait(idle_sweep_bundler.wake_cv,
                                   idle_sweep_bundler.wake_mu,
                                   tick_deadline) == ETIMEDOUT)
        break;
    }
    _ccol_mutex_unlock(idle_sweep_bundler.wake_mu);
    if (atomic_load(&idle_sweep_bundler.stop_flag)) break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Snapshot every registered server and pin each one while still holding
     * servers_bundler.mutex, with servers_bundler_pins in the same
     * pin-then-release pattern as _engine_force_stop_quiesce_all (the comment
     * of that field says why it needs its own counter and not
     * pending_resolve_count). Then we unlock before any of the real
     * per-server work below, which can be a real close() and TLS teardown for
     * every connection that expired this tick, slow compared to a global
     * lock. chttpsvr_start(), chttpsvr_destroy() and chttpsvr_engine_stop()
     * need servers_bundler.mutex for every OTHER registered server too, not
     * only the ones whose connections this tick closes, so holding it across
     * that work would queue those calls behind this tick's idle-connection
     * churn for nothing. The snapshot array uses the plain default allocator,
     * like servers_bundler.servers itself, so neither depends on any one
     * server's custom allocator. */
    _ccol_mutex_lock(servers_bundler.mutex);
    size_t n = servers_bundler.count;
    struct chttpserver **snapshot =
        n > 0 ? (struct chttpserver **)malloc(n * sizeof(struct chttpserver *))
              : NULL;
    if (n > 0 && !snapshot) {
      /* The allocation failed, so skip this tick's sweep completely and try
       * again on the next one a second later. */
      n = 0;
    } else {
      for (size_t i = 0; i < n; i++) {
        snapshot[i] = servers_bundler.servers[i];
        atomic_fetch_add(&snapshot[i]->servers_bundler_pins, 1);
      }
    }
    _ccol_mutex_unlock(servers_bundler.mutex);

    for (size_t i = 0; i < n; i++) {
      struct chttpserver *srv = snapshot[i];
      _sweep_server(srv, now);

      /* Pairs with the pin above, so srv is valid, unfreed memory up to and
       * including this point. Same unpin and broadcast as in
       * _engine_force_stop_quiesce_all. */
      _ccol_mutex_lock(srv->mutex);
      atomic_fetch_sub(&srv->servers_bundler_pins, 1);
      _ccol_cond_var_broadcast(srv->resolve_cv);
      _ccol_mutex_unlock(srv->mutex);
    }
    free(snapshot);
  }
  return NULL;
}

/* Returns true once the shared idle-timeout sweep thread is known to be
 * running, started either by an earlier call or successfully by this one, and
 * false only when _ccol_thread_create() itself failed this time. Two things
 * depend entirely on this thread: every registered server's idle_timeout_ms
 * rule and the max_connections capacity recovery (see
 * _listener_resume_if_capacity_freed, whose only caller is this sweep
 * thread). So chttpsvr_start() treats false as a real start failure instead
 * of carrying on silently degraded; the reasoning is at its call site. */
#ifdef RUNNING_UNIT_TESTS
/* Test-only white-box hook. A real pthread_create() failure from resource
 * exhaustion can't be reproduced deterministically from a test, and the idle
 * sweep thread is an engine-lifetime shared resource that only tries
 * _ccol_thread_create() once per window in which it isn't running (see the
 * idle_sweep_bundler.running gate right below), so this hook simulates the
 * failure directly. Compiled out of a production build like every other
 * white-box helper in this file. */
static _Atomic bool g_force_idle_sweep_thread_create_fail_for_tests = false;
void _chttpsvr_force_idle_sweep_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_idle_sweep_thread_create_fail_for_tests, force);
}
#endif /* RUNNING_UNIT_TESTS */
/* A fork() child that inherits running from its parent has no sweep thread,
 * so it starts its own instead of joining or relying on the parent's (see
 * _engine_forget_inherited_locked). The caller holds servers_bundler.mutex. */
static void _idle_sweep_forget_inherited_locked(void) {
  if (idle_sweep_bundler.running && idle_sweep_bundler.pid != getpid())
    idle_sweep_bundler.running = false;
}

static bool _idle_sweep_start_if_needed(void) {
  /* Same guard and reason as in _idle_sweep_fn. Like every other function
   * here that touches servers_bundler.mutex or srv_engine_bundler's state
   * directly, it doesn't rely on call-graph reasoning about what has already
   * run by the time a caller gets here. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(servers_bundler.mutex);
  _idle_sweep_forget_inherited_locked();
  if (!idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, false);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_force_idle_sweep_thread_create_fail_for_tests)) {
      /* A simulated failure. No real thread was created, so there's nothing
       * to join. */
    } else
#endif /* RUNNING_UNIT_TESTS */
      if (_ccol_thread_create(idle_sweep_bundler.thread, _idle_sweep_fn,
                              NULL) == 0) {
        idle_sweep_bundler.running = true;
        idle_sweep_bundler.pid = getpid();
      }
  }
  bool ok = idle_sweep_bundler.running;
  _ccol_mutex_unlock(servers_bundler.mutex);
  return ok;
}

/* Stops and joins the idle sweep thread if it's running. It's tied to the
 * shared engine's lifetime, so the engine reaper calls it next to
 * ccol_event_loop_destroy, and not to the stop or destroy of a single server,
 * since the sweep thread walks all registered servers. A sweep thread still
 * running and unjoined at process exit produces exactly the "possibly lost"
 * false positive for glibc's TLS allocation (allocate_dtv) that the other
 * reaper threads of this codebase document closing; see this module's reaper
 * thread above. */
static void _idle_sweep_stop_if_running(void) {
  /* See the identical guard and comment in _idle_sweep_fn. */
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool was_running = false;
  _ccol_thread_id_t t = {0};
  _ccol_mutex_lock(servers_bundler.mutex);
  _idle_sweep_forget_inherited_locked();
  if (idle_sweep_bundler.running) {
    _ccol_mutex_lock(idle_sweep_bundler.wake_mu);
    atomic_store(&idle_sweep_bundler.stop_flag, true);
    _ccol_cond_var_signal(idle_sweep_bundler.wake_cv);
    _ccol_mutex_unlock(idle_sweep_bundler.wake_mu);
    t = idle_sweep_bundler.thread;
    was_running = true;
    idle_sweep_bundler.running = false;
  }
  _ccol_mutex_unlock(servers_bundler.mutex);
  if (was_running) _ccol_thread_join(t);
}

/* Unlinks c from the parked list, which is what claims it. The caller holds
 * srv->idle_mutex. */
static void _parked_unlink_locked(struct chttpserver *srv, chttpsvr_conn_t *c) {
  if (c->parked_prev) c->parked_prev->parked_next = c->parked_next;
  if (c->parked_next) c->parked_next->parked_prev = c->parked_prev;
  if (srv->parked_head == c) srv->parked_head = c->parked_next;
  if (srv->parked_tail == c) srv->parked_tail = c->parked_prev;
  c->parked_prev = c->parked_next = NULL;
  c->in_parked_list = false;
}

/* Points conn->reg at dir, and skips the call when it already points there.
 * Every direction change of conn->reg goes through here and every
 * ccol_event_loop_add of conn->reg records its direction in reg_dir, so
 * reg_dir always tells what the registration is waiting for. */
#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_conn_reg_modify_calls_for_tests = 0;
size_t _chttpsvr_conn_reg_modify_calls_for_tests(void) {
  return atomic_load(&g_conn_reg_modify_calls_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */
static inline ccol_retval_t _conn_reg_modify(chttpsvr_conn_t *conn,
                                             ccol_select_dir dir) {
  if (conn->reg_dir == (uint8_t)dir) return ccol_success;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_conn_reg_modify_calls_for_tests, 1,
                            memory_order_relaxed);
#endif /* RUNNING_UNIT_TESTS */
  ccol_retval_t rv =
      ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg, dir);
  if (rv == ccol_success) conn->reg_dir = (uint8_t)dir;
  return rv;
}

/* Publishes a parked connection, the parked-list twin of _idle_list_add. */
static void _parked_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->idle_mutex);
  if (!conn->in_parked_list) {
    conn->parked_prev = srv->parked_tail;
    conn->parked_next = NULL;
    if (srv->parked_tail) srv->parked_tail->parked_next = conn;
    srv->parked_tail = conn;
    if (!srv->parked_head) srv->parked_head = conn;
    conn->in_parked_list = true;
  }
  _ccol_mutex_unlock(srv->idle_mutex);
}

static void _idle_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->idle_mutex);
  if (!conn->in_idle_list) {
    conn->idle_prev = srv->idle_tail;
    conn->idle_next = NULL;
    if (srv->idle_tail) srv->idle_tail->idle_next = conn;
    srv->idle_tail = conn;
    if (!srv->idle_head) srv->idle_head = conn;
    conn->in_idle_list = true;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  _ccol_mutex_unlock(srv->idle_mutex);
}

static void _idle_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
  } else if (conn->in_parked_list) {
    _parked_unlink_locked(srv, conn);
  }
  _ccol_mutex_unlock(srv->idle_mutex);
}

/* Adds conn to srv's registry of diverted connections. Idempotent like
 * _idle_list_add, so it does nothing when conn is already there. The
 * registry lets a teardown find this connection's fd and force a shutdown(2)
 * on it, which matters when the connection turns out to be stuck in a worker
 * thread's blocking I/O past the normal graceful drain window (see the
 * comment of diverted_mutex, diverted_head and diverted_tail in struct
 * chttpserver, and _wait_in_flight_bounded). There are two callers.
 * _conn_start_diverted calls it every time a request goes to a worker thread,
 * the first request as well as a later pipelined one on the same connection.
 * _conn_dispatch_reject calls it when it writes a courtesy rejection through
 * reject_pool or its synchronous fallback, and that write is exactly the kind
 * of blocking connection I/O this registry is there for (the comment of that
 * function describes the real hang this closes). */
static void _diverted_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->diverted_mutex);
  if (!conn->in_diverted_list) {
    conn->diverted_prev = srv->diverted_tail;
    conn->diverted_next = NULL;
    if (srv->diverted_tail) srv->diverted_tail->diverted_next = conn;
    srv->diverted_tail = conn;
    if (!srv->diverted_head) srv->diverted_head = conn;
    conn->in_diverted_list = true;
  }
  _ccol_mutex_unlock(srv->diverted_mutex);
}

/* Removes conn from srv's registry of diverted connections. Idempotent, and a
 * no-op when conn isn't there. Two paths call it: _conn_free, when a worker
 * is done with conn for good (every close and every reject path), and the
 * keep-alive tail of _task_worker, when conn goes back to the reactor. From
 * then on no worker owns conn, so it's no longer a candidate for the forced
 * shutdown this registry exists for. */
static void _diverted_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->diverted_mutex);
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
  _ccol_mutex_unlock(srv->diverted_mutex);
}

/* Forcibly interrupts every connection in srv's registry of diverted
 * connections with a shutdown(2) SHUT_RDWR on each raw fd. That unblocks the
 * chttp1_stream_read or chttp1_stream_write a stuck worker is sitting in with
 * an ordinary I/O error, the same path a real peer disconnect takes (see e.g.
 * client_disconnect_mid_body_does_not_hang_server), and the worker's existing
 * error handling then finishes and frees the request normally. It's
 * shutdown() and not close() because the worker thread owns the fd, never this
 * thread, and only that worker may close(2) it, through _conn_free. Closing it
 * here would risk the classic cross-thread close fd-reuse race, while
 * shutdown() has no such problem: it only affects current and future I/O on
 * this exact fd, and the fd number stays allocated until its owner closes it.
 * We only call this after unlocking srv->mutex (see _wait_in_flight_bounded)
 * and only once the normal, bounded, graceful wait for in-flight requests has
 * run out; the comment of that function explains why this exists at all. It's
 * best effort and we don't check shutdown()'s return value, since for an fd
 * that is already closed or otherwise invalid the call is a harmless no-op. */
static void _force_unblock_diverted_connections(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->diverted_mutex);
  for (chttpsvr_conn_t *c = srv->diverted_head; c; c = c->diverted_next)
    shutdown(c->fd, SHUT_RDWR);
  _ccol_mutex_unlock(srv->diverted_mutex);
}

/* Atomically tries to claim conn out of the idle list for exclusive work,
 * either dispatching a real I/O event for it or closing it. Returns true when
 * conn was idle and has just been removed, so the caller owns it alone and
 * may safely read and free it. Returns false when conn was NOT in the idle
 * list because someone else (another dispatch or a concurrent closer) already
 * claimed it, and then the caller must not touch conn at all.
 *
 * This is the single bit of synchronization that makes it safe for
 * _close_all_idle_connections and _idle_sweep_fn to free a connection they
 * find in the idle list from a non-reactor thread. Without it a closer could
 * find a connection idle just as a reactor thread dispatches the same
 * connection to _conn_pump because new data came in at that instant, and the
 * closer's _conn_free would run at the same time as _conn_pump's
 * ctls_conn_read on the very same SSL*. For TLS, _conn_free calls
 * ctls_conn_destroy, which calls SSL_free, so that's a use-after-free that
 * shows up as a segfault deep in libcrypto's BIO code. It's most visible on
 * the TLS path. The plaintext race has the same shape but is far less
 * immediately fatal, since there a read() and a close() race each other
 * instead of freeing live OpenSSL state.
 *
 * A connection only goes into the idle list AFTER the ccol_event_loop_add or
 * ccol_event_loop_modify call that makes it dispatchable has returned (see the
 * handshake-wait branch of _conn_pump, the EWOULDBLOCK branch of its main read
 * loop and the keep-alive re-arm of _task_worker). So in a narrow window a
 * dispatch can fire before the idle-list add, and the claim here fails.
 * That's harmless. This module always uses level-triggered epoll, so a claim
 * that fails with "not idle yet" just means the next epoll_wait reports the
 * same readiness again, and the add finishes long before that, a few
 * instructions later on the same thread with no I/O in between. No request is
 * ever dropped and none ever hangs. */
static bool _idle_list_try_claim(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  bool claimed = false;
  _ccol_mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
    claimed = true;
  } else if (conn->in_parked_list) {
    _parked_unlink_locked(srv, conn);
    claimed = true;
  }
  _ccol_mutex_unlock(srv->idle_mutex);
  return claimed;
}

/* Closes and frees every connection of srv that is still idle and owned by
 * the reactor, either waiting for its next pipelined request or an open
 * keep-alive connection nobody has reused yet. __chttpsvr_destroy calls it
 * after chttpsvr_stop() has closed the listener, so no new connection can
 * arrive. Without it, a keep-alive connection a test client never closed
 * explicitly outlives the server that accepted it, and that's the common
 * case, because chttpclient's idle pool keeps a connection open after a
 * response instead of closing it. That connection's chttpsvr_conn_t would then
 * leak for the rest of the process together with everything it owns (path,
 * headers, route match state and more), and valgrind reports it as a real
 * "definitely lost" block back to _conn_create and _listener_on_readable, not
 * a false positive.
 *
 * Each candidate is claimed at collection time under idle_mutex, with the
 * same list removal _idle_list_try_claim does, instead of just copying the
 * pointer and closing it afterwards, so a connection can never be found here
 * and dispatched to _conn_pump on a reactor thread at the same time. The doc
 * comment of _idle_list_try_claim describes the use-after-free this
 * closes. */
static void _close_idle_connections(struct chttpserver *srv,
                                    bool include_parked) {
  for (;;) {
    chttpsvr_conn_t *to_close[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t to_close_n = 0;
    _ccol_mutex_lock(srv->idle_mutex);
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
    while (include_parked && srv->parked_head &&
           to_close_n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      c = srv->parked_head;
      _parked_unlink_locked(srv, c);
      c->park = _CONN_PARK_NONE;
      to_close[to_close_n++] = c;
    }
    _ccol_mutex_unlock(srv->idle_mutex);
    if (to_close_n == 0) break;
    for (size_t i = 0; i < to_close_n; i++) _conn_close(to_close[i]);
  }
}

static void _close_all_idle_connections(struct chttpserver *srv) {
  _close_idle_connections(srv, /*include_parked=*/true);
}

/* ========================================================================== */
/*                         PERCENT-DECODE HELPERS                             */
/* ========================================================================== */

/* Plain RFC 3986 %XX decoding, where '+' either stays literal (its meaning in
 * a path) or becomes a space (its meaning in a query string). It's safe in
 * place with dst == src since the write index never gets ahead of the read
 * index. Returns the decoded length, or -1 for malformed percent-encoding: a
 * '%' not followed by two hex digits, or a %XX that decodes to a literal NUL
 * byte.
 *
 * Refusing a NUL matters beyond this function's own contract, because every
 * caller treats the output as a NUL-terminated C string.
 * chttpsvr_req_path() publicly promises a NUL-terminated string,
 * _seg_matches_literal strcmp()s against a registered route or prefix
 * segment, and chttpsvr_req_query and chttpsvr_req_query_one strcmp() the
 * query's keys and values. strcmp and friends stop at the first NUL, not at
 * the length we return, so a decoded NUL silently cuts the comparison short.
 * A path segment like "foo%00bar" decodes to "foo\0bar" and strcmp says it
 * equals the literal route segment "foo", letting an attacker hide a suffix
 * behind a route match and behind chttpsvr_req_path()'s return value, while a
 * caller reasonably expects both to reflect the whole segment. So a decoded
 * NUL counts as malformed and returns -1 just like a bad %XX escape, which
 * sends it down the existing, correct handling every caller already has: a
 * failed decode means this segment or path doesn't match. Without that the
 * NUL would silently corrupt a strcmp further down. */
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

/* Turns an absolute-form request-target into the origin-form slice inside it
 * and leaves every other form alone.
 *
 * RFC 7230 SS5.3.2: "a server MUST accept the absolute-form in requests, even
 * though HTTP/1.1 clients will only send them in requests to proxies." A
 * client talking to us through an explicit proxy setting sends one, and so
 * does a proxy that forwards the target it got unchanged. Without this,
 * "GET http://host/items HTTP/1.1" reaches the route search as the four
 * segments "http:", "", "host" and "items", matches nothing and answers 404
 * for a resource this very server serves at "/items".
 *
 * We recognise the two schemes an HTTP request can name, case-insensitively.
 * A target with any other scheme stays as it is, since this is an origin
 * server that serves no other scheme and the route search answers it with a
 * 404.
 *
 * SS5.4 says a server MUST ignore the received Host header when the target is
 * absolute-form. We route on the path alone and never on an authority, so we
 * ignore it anyway, and the authority skipped here is thrown away on purpose,
 * not lost by accident. chttp1_parser still applies SS5.4's Host COUNT rule to
 * such a request; see the blank-line branch of its header state.
 *
 * *out and *out_len describe a slice of target, never a new allocation. It
 * returns true when it really stripped a prefix, which the caller needs to
 * know because an absolute-form target with an empty path stands for "/",
 * while an origin-form target is never empty at all. */
static bool _strip_absolute_form(const char *target, size_t target_len,
                                 const char **out, size_t *out_len) {
  *out = target;
  *out_len = target_len;

  size_t scheme_len;
  if (target_len > 7 && ccol_ascii_ncasecmp(target, "http://", 7) == 0)
    scheme_len = 7;
  else if (target_len > 8 && ccol_ascii_ncasecmp(target, "https://", 8) == 0)
    scheme_len = 8;
  else
    return false;

  /* The authority ends at the first "/", "?" or "#". parse_request_line()
   * has already refused every whitespace and control byte in a
   * request-target, so nothing else can end it. */
  size_t i = scheme_len;
  while (i < target_len && target[i] != '/' && target[i] != '?' &&
         target[i] != '#')
    i++;

  *out = target + i;
  *out_len = target_len - i;
  return true;
}

/* Private sentinel _parse_method returns for a method string it doesn't
 * recognise. It isn't part of the public chttp_method_t enum and no caller
 * can use it as a route's method, so it can never match a registered
 * route. */
#define _CHTTP_METHOD_UNKNOWN ((chttp_method_t)(CHTTP_ANY + 1))

/* The largest request body "OPTIONS *" may carry while keeping the
 * connection alive (RFC 9110 SS9.3.7 reserves such a body for future use).
 * We read and discard up to this much and close the connection after the 200
 * when the body is bigger, the same bound and the same outcome as Go's
 * net/http server. */
#define _CHTTPSVR_OPTIONS_STAR_MAX_BODY ((size_t)4096)

/* The route "OPTIONS *" matches. It never comes from a registration and the
 * router tables never hold it. It's only there so the worker path, which
 * reads conn->matched_route, handles the request as a buffered route. Its
 * handler never runs because _task_worker answers the request itself (see the
 * options_star branch there). */
static void _options_star_unused_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                         void *ctx) {
  (void)req;
  (void)resp;
  (void)ctx;
}
static chttpsvr_route_t _options_star_route = {
    .method = CHTTP_OPTIONS, .fn = _options_star_unused_handler};

/* ========================================================================== */
/*                         FORWARD DECLARATIONS                               */
/* ========================================================================== */

static void _chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp);
static void _task_worker(void *arg);
#ifdef RUNNING_UNIT_TESTS
/* How many requests _conn_start_diverted has handed to the worker pool, so a
 * test can tell that a request is waiting in its queue. */
atomic_size_t _chttpsvr_worker_submits_for_tests;
#endif /* RUNNING_UNIT_TESTS */
static void _task_resume(void *arg);
static void _reject_task(void *arg);
static void _destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs_t *mp);
static void _conn_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                              ccol_selectable *sel, void *arg);
static void _conn_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                              ccol_selectable *sel, void *arg);
static void _conn_on_error(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg);
static void _listener_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg);

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

  char **segs =
      (char **)_ccol_mem_calloc(mp, (size_t)seg_count, sizeof(char *));
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
    segs[si] = (char *)_ccol_mem_alloc(mp, len + 1);
    if (!segs[si]) {
      for (int j = 0; j < si; j++) _ccol_mem_free(mp, segs[j]);
      _ccol_mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    memcpy(segs[si], s, len);
    segs[si][len] = '\0';
    if (segs[si][0] == '{') {
      if (len < 3 || segs[si][len - 1] != '}') {
        for (int j = 0; j <= si; j++) _ccol_mem_free(mp, segs[j]);
        _ccol_mem_free(mp, segs);
        return ccol_invalid_args;
      }
      for (size_t ci = 1; ci < len - 1; ci++) {
        char c = segs[si][ci];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
          for (int j = 0; j <= si; j++) _ccol_mem_free(mp, segs[j]);
          _ccol_mem_free(mp, segs);
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
    param_names =
        (char **)_ccol_mem_calloc(mp, (size_t)param_count, sizeof(char *));
    if (!param_names) {
      for (int j = 0; j < seg_count; j++) _ccol_mem_free(mp, segs[j]);
      _ccol_mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    int pi = 0;
    for (int i = 0; i < seg_count; i++) {
      if (segs[i][0] == '{') {
        size_t len = strlen(segs[i]);
        param_names[pi] = (char *)_ccol_mem_alloc(mp, len - 1);
        if (!param_names[pi]) {
          for (int j = 0; j < pi; j++) _ccol_mem_free(mp, param_names[j]);
          _ccol_mem_free(mp, param_names);
          for (int j = 0; j < seg_count; j++) _ccol_mem_free(mp, segs[j]);
          _ccol_mem_free(mp, segs);
          return ccol_not_enough_memory;
        }
        memcpy(param_names[pi], segs[i] + 1, len - 2);
        param_names[pi][len - 2] = '\0';
        /* A pattern can use the same {name} twice, like "/a/{id}/b/{id}".
         * Without this check it compiles fine and the value captured by the
         * second use is silently unreachable, because chttpsvr_req_param
         * always returns on the FIRST match of a name. We refuse it at
         * registration time instead of letting it show up later as a
         * confusing wrong value at request time. */
        for (int j = 0; j < pi; j++) {
          if (strcmp(param_names[j], param_names[pi]) == 0) {
            for (int k = 0; k <= pi; k++) _ccol_mem_free(mp, param_names[k]);
            _ccol_mem_free(mp, param_names);
            for (int k = 0; k < seg_count; k++) _ccol_mem_free(mp, segs[k]);
            _ccol_mem_free(mp, segs);
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
  for (int i = 0; i < r->seg_count; i++) _ccol_mem_free(mp, r->segs[i]);
  _ccol_mem_free(mp, r->segs);
  for (int i = 0; i < r->param_count; i++)
    _ccol_mem_free(mp, r->param_names[i]);
  _ccol_mem_free(mp, r->param_names);
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
  /* On ROUTE_MATCH_OK, the router of the matched route. On
   * ROUTE_MATCH_METHOD and ROUTE_MATCH_NONE, the sub-router that owns the
   * path and whose middleware runs for the rejection, or NULL when no mount
   * owns it and the root does the rejecting. */
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  char **param_values; /* on OK the caller must free it */
  route_match_t result;
  /* On ROUTE_MATCH_METHOD, one bit (1u << method) per method accepted by a
   * route matching the path, HEAD included wherever the HEAD-to-GET fallback
   * applies; the 405's Allow header is built from it. 0 for every other
   * result. */
  uint8_t allowed_methods;
} match_result_t;

/* One raw '/'-separated segment of a request sub_path plus its
 * percent-decoded value, which we compute lazily and remember. A router with
 * N routes tries all of them against the same sub_path, and the decoded value
 * of a raw segment, or the failure to decode it, is the same whichever route
 * asks, since a percent-decode only depends on the segment's bytes and never
 * on the route being tried. With this cache each position is decoded at most
 * once per router and request, instead of once per candidate route touching
 * it, so the cost goes from O(routes * segment length) down to one decode for
 * each of the cache->count segments. */
typedef struct {
  const char *raw; /* points into the sub_path the cache was built from;
                    * there's no NUL at raw_len */
  size_t raw_len;
  bool attempted; /* true once a decode was tried for this position */
  bool ok;        /* only meaningful once attempted is true; true exactly
                   * when decoded and decoded_len below are valid */
  char *decoded;  /* owned and NUL-terminated */
  size_t decoded_len;
} _seg_cache_entry_t;

typedef struct {
  _seg_cache_entry_t *entries; /* heap array, one element per raw segment
                                * found, up to and including this cache's
                                * cap */
  size_t count;                /* how many elements were filled. The doc
                                * comment of _seg_cache_build explains why,
                                * for every route whose own seg_count is
                                * strictly below this cache's cap, this value
                                * stands in for sub_path's true total segment
                                * count without ambiguity. */
  ccol_memmgmt_procs_t *mp;
} _seg_cache_t;

/* Splits sub_path on '/' into its raw segments, recording each one's pointer
 * and length inside sub_path itself, without decoding anything yet. It stops
 * after `cap` segments even when sub_path has more. The split matches a
 * route's segment-by-segment walk exactly: it strips the leading slash the
 * same way and keeps a zero-length segment instead of skipping it. Such a
 * segment comes from a request path with consecutive slashes and can't
 * satisfy any route either.
 *
 * `cap` MUST be strictly greater than the seg_count of every route in the
 * calling router, so the caller passes router->max_route_seg_count + 1. That's
 * what makes `cache->count == route->seg_count` an exact test, for every such
 * route, of "sub_path has exactly this many segments, no more, no fewer",
 * and the cache never needs sub_path's true total when that total is more than
 * any registered route could need. There are two cases. If cache->count < cap,
 * the split loop below only stopped because sub_path ran out, so cache->count
 * is sub_path's real total. If cache->count == cap, sub_path has AT LEAST cap
 * segments, strictly more than any route's seg_count since all of those are
 * below cap by construction, so cache->count can't accidentally equal a real
 * route's seg_count there either. The bound is `cap`, which route registration
 * fixes and the server's operator controls, and not sub_path's real segment
 * count, which the client fully controls, and that keeps this cache's memory
 * independent of how many "/" a request path holds. Returns 0 on success
 * (cache->count can legitimately be 0, e.g. for sub_path == "/") and -1 when
 * out of memory. */
static int _seg_cache_build(const char *sub_path, size_t cap,
                            _seg_cache_t *cache, ccol_memmgmt_procs_t *mp) {
  cache->entries = NULL;
  cache->count = 0;
  cache->mp = mp;
  if (cap == 0) return 0;

  const char *p = sub_path;
  if (*p == '/') p++;
  if (*p == '\0') return 0;

  /* On purpose the loop condition does NOT test `*s`, only `count < cap`. A
   * trailing '/', like in sub_path == "/items/99/", means one more, empty,
   * segment after the last real one. A route's segment-by-segment walk sees it
   * the same way: its last-segment check "if (next_sep != NULL) goto no_match"
   * rejects a route whose last segment is followed by any further separator,
   * a bare trailing one included. A loop testing `*s` would stop one segment
   * early, as soon as `s` lands on the NUL right after that trailing '/', and
   * silently count "items/99/" as the same 2 segments as "items/99", so a
   * route registered as "/items/{id}" would wrongly match a path with an
   * extra "/" at the end. The trailing_slash_not_matched test in tests.c pins
   * this. `!e` means no further separator was found at all, and that's what
   * correctly ends both loops below, the only thing that ends them. */
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

  _seg_cache_entry_t *entries = (_seg_cache_entry_t *)_ccol_mem_calloc(
      mp, count, sizeof(_seg_cache_entry_t));
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
    _ccol_mem_free(cache->mp, cache->entries[i].decoded);
  _ccol_mem_free(cache->mp, cache->entries);
  cache->entries = NULL;
  cache->count = 0;
}

/* Returns the percent-decoded value of cache's segment idx from the memo. It
 * gets decoded at most once, on the first call for that position, however
 * many candidate routes ask. NULL means the raw segment can never match any
 * route, which happens when it's empty (a path with consecutive slashes like
 * "a//b") or its percent-encoding is malformed. Both are a plain "no match"
 * for the asking route and neither is an error, in line with this module's
 * contract that a failed decode is a route mismatch and never a 400 (see the
 * doc comment of conn->path above). The cache keeps ownership of the returned
 * pointer, valid until _seg_cache_free, so a caller who needs the value
 * longer, like a matched route's captured param, needs to copy it. *oom_out
 * becomes true only on a real allocation failure, which the caller must treat
 * as fatal to the whole route search and not just to this candidate.
 *
 * WARNING for whoever changes this later: e->attempted is set below BEFORE
 * the allocation is even tried, so a temporary allocation failure gets
 * memoized as a permanent "can't decode" for the rest of this cache's life,
 * just like the two deterministic outcomes next to it (empty segment,
 * malformed encoding), and no later call for the same idx retries the
 * allocation. That's only harmless because _match_route_cached and
 * _find_route both treat *oom_out == true as fatal to the ENTIRE route search
 * the moment it happens: the search aborts and this cache is freed within
 * microseconds (see how "ms == -1" is handled in _find_route), so a second
 * call for this idx on the same cache never happens. If that policy ever
 * becomes skippable per route, so the search tries another candidate instead
 * of aborting, this memo needs revisiting first; otherwise a route that
 * would match once the memory pressure is gone stays stuck at "no match" for
 * the rest of this request's route search. We deliberately don't carry a
 * guard for it up front, since under the always-fatal policy it would have no
 * observable effect (nothing reaches a second call for the same idx either
 * way), no test could show it does anything, and the project's discipline
 * doesn't allow code like that. */
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

  char *decoded = (char *)_ccol_mem_alloc(cache->mp, e->raw_len + 1);
  if (!decoded) {
    *oom_out = true; /* see the WARNING above: memoized as permanent and
                      * never retried, harmless only because the caller
                      * treats it as fatal to the search */
    return NULL;
  }
  memcpy(decoded, e->raw, e->raw_len);
  decoded[e->raw_len] = '\0';
  ssize_t dlen = _decode_path_unsafe(decoded, decoded); /* safe in place */
  if (dlen < 0) {
    _ccol_mem_free(cache->mp, decoded);
    return NULL; /* malformed percent-encoding: never matches anything */
  }
  decoded[(size_t)dlen] = '\0';
  e->ok = true;
  e->decoded = decoded;
  e->decoded_len = (size_t)dlen;
  *len_out = e->decoded_len;
  return decoded;
}

/* Matches route's compiled segments against cache, which the caller builds
 * once per router from that router's sub_path and shares across every
 * candidate route it tries on the router (see the doc comment of
 * _seg_cache_t). A route whose own segment count differs from cache->count is
 * rejected before any segment is decoded.
 *
 * If pv_out isn't NULL it gets a fresh array on a match (return value 1)
 * holding an independently owned copy of each matched param value. We never
 * hand out the cache's decoded copies directly: a candidate can match every
 * segment up to some position and then fail on a later one, and the earlier
 * positions must stay intact for the next candidate to read, so values are
 * only ever copied out of the cache and ownership never moves. On any other
 * outcome *pv_out is left alone. Returns 1 for a match, 0 for no match and -1
 * when out of memory; a -1 is fatal to the whole route search and not just to
 * this candidate. */
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
          for (int j = 0; j < param_idx; j++) _ccol_mem_free(mp, pv[j]);
          _ccol_mem_free(mp, pv);
        }
        return -1;
      }
      goto no_match;
    }

    if (route->segs[i][0] == '{') {
      if (pv_out) {
        if (!pv) {
          pv = (char **)_ccol_mem_calloc(mp, (size_t)route->param_count,
                                         sizeof(char *));
          if (!pv) return -1;
        }
        char *val = (char *)_ccol_mem_alloc(mp, dlen + 1);
        if (!val) {
          for (int j = 0; j < param_idx; j++) _ccol_mem_free(mp, pv[j]);
          _ccol_mem_free(mp, pv);
          return -1;
        }
        memcpy(val, decoded, dlen + 1); /* +1 for the NUL terminator */
        pv[param_idx++] = val;
      }
      /* A NULL pv_out means we're only probing this route to choose between
       * ROUTE_MATCH_METHOD and ROUTE_MATCH_NONE (see _find_route). The check
       * above already showed the value decodes, since _seg_cache_get returned
       * a non-NULL `decoded`, and in this case we don't copy it anywhere. */
    } else {
      if (strcmp(decoded, route->segs[i]) != 0) goto no_match;
    }
  }
  if (pv_out) *pv_out = pv;
  return 1;

no_match:
  if (pv) {
    for (int j = 0; j < param_idx; j++) _ccol_mem_free(mp, pv[j]);
    _ccol_mem_free(mp, pv);
  }
  return 0;
}

/* Decides whether router->prefix owns the request path, whose leading
 * segments are already split as raw values with their decoded values
 * memoized in cache (see the doc comment of _seg_cache_t). _find_route builds
 * that cache once per request from the FULL request path and shares it
 * between all the non-root routers' calls into this function, so N
 * sub-routers never decode the same leading segments N times. The prefix
 * side is walked straight off router->prefix since it's a short,
 * operator-controlled string that only one router ever parses.
 *
 * A prefix owns a path when the DECODED path equals the prefix or starts with
 * the prefix plus a '/'. The decoded path is the decoded segments joined with
 * '/', exactly what chttpsvr_req_path() reports. We decide ownership on that
 * form and not on raw segments alone, because a guard in a sub-router's
 * middleware reasons about the path it can read. "/admin%2Fsecret" decodes to
 * "/admin/secret", so the "/admin" mount owns it even though its first raw
 * segment is the single segment "admin%2Fsecret". An empty segment
 * (consecutive slashes) or one with malformed percent-encoding never matches
 * a prefix segment, which is never empty and is compared as a plain literal.
 *
 * Returns:
 *   1  the prefix owns the path and ends on a boundary between two raw
 *      segments. *sub_path_out is the rest of the raw path, which this
 *      router's routes are matched against.
 *   2  the prefix owns the path but ends inside a raw segment whose decoded
 *      value contains a '/' ("/admin%2Fsecret" against the mount "/admin").
 *      No route of this router can match such a path, since routes match
 *      whole raw segments, so the router answers it with its own 404.
 *      *sub_path_out isn't set.
 *   0  the prefix doesn't own the path.
 *  -1  an allocation failed, which is fatal to the whole route search, just
 *      like in _seg_cache_get. */
static int _prefix_matches(_seg_cache_t *cache, chttpsvr_router *router,
                           const char **sub_path_out) {
  const char *pp = router->prefix + 1;

  if (*pp == '\0') {
    /* The special "/" prefix (see the doc comment of chttpsvr_subrouter),
     * which only owns the exact root path, a request path with no segments
     * of its own. */
    if (cache->count != 0) return 0;
    *sub_path_out = "/";
    return 1;
  }

  /* The part of the prefix the decoded segments still have to cover. The
   * prefix has no trailing '/' and no "//" (see _create_router and
   * chttpsvr_subrouter). */
  size_t prem = router->prefix_len - 1;
  size_t idx = 0;
  for (;;) {
    /* The request path ran out of segments before the prefix did. We check
     * this before indexing cache->entries since _seg_cache_get doesn't check
     * the bound. The cache's cap is one more than the largest prefix segment
     * count and every iteration eats at least one prefix segment, so a prefix
     * never needs more entries than the cache has. */
    if (idx >= cache->count) return 0;

    size_t dlen = 0;
    bool oom = false;
    const char *decoded = _seg_cache_get(cache, idx, &dlen, &oom);
    if (oom) return -1;
    if (!decoded) return 0;

    if (dlen < prem) {
      /* The segment covers part of the prefix, so a '/' of the prefix must
       * come right after it. */
      if (memcmp(decoded, pp, dlen) != 0 || pp[dlen] != '/') return 0;
      pp += dlen + 1;
      prem -= dlen + 1;
      idx++;
      continue;
    }
    if (memcmp(decoded, pp, prem) != 0) return 0;
    if (dlen == prem) {
      const char *rp_after_last_seg =
          cache->entries[idx].raw + cache->entries[idx].raw_len;
      *sub_path_out = (*rp_after_last_seg == '\0') ? "/" : rp_after_last_seg;
      return 1;
    }
    /* The segment goes past the end of the prefix, and the prefix only owns
     * the path if the decoded value continues with a '/' at that point. */
    return decoded[prem] == '/' ? 2 : 0;
  }
}

/* Defined just below; _find_route's HEAD-fallback bookkeeping frees a
 * candidate's param values through it. */
static void _free_param_values(char **pv, int count, ccol_memmgmt_procs_t *mp);

static match_result_t _find_route(struct chttpserver *srv, const char *path,
                                  chttp_method_t method) {
  /* One bit per method accepted by a route whose path matched, for a 405's
   * Allow header (RFC 9110 SS15.5.6). Non-zero exactly when at least one
   * route matched the path but not the method. */
  unsigned allowed_methods = 0;

  /* Every non-root router shares this cache in its own _prefix_matches call
   * below (the doc comment of that function says why), and it's built once
   * per request from the FULL request path. The cap is
   * srv->max_prefix_seg_count + 1, a bound the operator controls, rather than
   * the request path's real segment count, which the client fully controls;
   * max_route_seg_count follows the same reasoning for each router's
   * route-matching cache below. With no sub-router at all we leave the cache
   * empty, and _seg_cache_free handles an empty cache as a no-op. routers[0]
   * is always the root and every other entry a real sub-router with a
   * prefix, so with only the root the loop below never calls
   * _prefix_matches. */
  _seg_cache_t path_cache = {0};
  if (srv->router_count > 1) {
    if (_seg_cache_build(path, (size_t)srv->max_prefix_seg_count + 1,
                         &path_cache, srv->m_procs) != 0)
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
  }

  /* A router's precedence comes from how specific its mount prefix is, never
   * from registration order. srv->routers has the root at index 0 and all
   * sub-routers from index 1 on, in descending prefix segment count, which
   * chttpsvr_subrouter's sorted insert maintains. So this loop visits
   * 1..router_count-1 first and the root last.
   *
   * The root's prefix matches every path, so trying the root first would let
   * one root pattern answer for a whole class of paths. A two-segment root
   * pattern like "/{a}/{b}" would answer every two-segment path in the
   * process, including the ones a sub-router is mounted on. The effective
   * middleware chain _on_headers_complete builds would then only carry the
   * root's entries, and the sub-router's middleware, an authenticator for
   * instance, would be silently skipped for a request that clearly lands
   * inside its mount prefix. That's a security problem and not a question of
   * style, so registration order never decides.
   *
   * The most specific mount whose prefix owns the path (see _prefix_matches)
   * owns the request outright. Only routers mounted on that same prefix may
   * answer it; a router with a shorter prefix and the root never see it. When
   * none of their routes matches, the 404 or 405 comes from the owner, which
   * goes back in match_result_t.router so its middleware runs for that
   * rejection too. That way a guard in a mount's middleware covers every path
   * under the mount whatever routes other routers register. Two routers on
   * the identical prefix share ownership, and the one registered first
   * answers a rejection.
   *
   * The loop visits 1..router_count-1 in descending prefix segment count and
   * the root last. Once there's an owner, the first router with a smaller
   * segment count ends the owning group, because two different prefixes with
   * the same segment count can never both own one path. */
  chttpsvr_router *owner = NULL;
  for (size_t visit = 0; visit < srv->router_count; visit++) {
    size_t ri = (visit + 1 < srv->router_count) ? visit + 1 : 0;
    chttpsvr_router *router = srv->routers[ri];

    /* Every path that gets to the cache build below sets this. The
     * initializer is only there for a compiler that can't follow
     * _prefix_matches's return codes. */
    const char *sub_path = NULL;
    if (router->prefix_len == 0) {
      if (owner) break;
      sub_path = path;
    } else {
      if (owner && router->prefix_seg_count < owner->prefix_seg_count) break;
      int pm = _prefix_matches(&path_cache, router, &sub_path);
      if (pm < 0) {
        _seg_cache_free(&path_cache);
        return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
      }
      if (pm == 0) continue;
      if (!owner) owner = router;
      if (pm == 2) continue;
    }

    if (router->route_count == 0) continue;

    /* Built once per router and shared by every candidate route below; the
     * doc comment of _seg_cache_t says why a raw segment's decoded value is
     * the same whichever route asks. The split is capped at
     * router->max_route_seg_count + 1 and not at sub_path's real segment
     * count, which the client fully controls (the doc comment of
     * _seg_cache_build explains). That cap covers every route this router can
     * match and doesn't depend on how many "/" a request path holds. */
    _seg_cache_t cache;
    if (_seg_cache_build(sub_path, (size_t)router->max_route_seg_count + 1,
                         &cache, srv->m_procs) != 0) {
      _seg_cache_free(&path_cache);
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
    }

    match_result_t found = {NULL, NULL, NULL, ROUTE_MATCH_NONE, 0};
    bool have_result = false;
    /* The first CHTTP_GET route of this router that matched a CHTTP_HEAD
     * request's path. The scan keeps it and carries on, since an explicit
     * CHTTP_HEAD or CHTTP_ANY route may match later and has to win. See
     * head_ok below. */
    chttpsvr_route_t *head_fallback_route = NULL;
    char **head_fallback_pv = NULL;
    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route_t *route = router->routes[i];

      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      /* RFC 9110 SS9.3.2 makes HEAD identical to GET except that the server
       * must not send a body, so every path that answers GET answers HEAD
       * too. Otherwise a path registered for GET alone would give a HEAD
       * request 405, which breaks health checkers, uptime monitors, link
       * checkers and cache revalidation. The response side already handles
       * the rest: _send_response drops the body for a CHTTP_HEAD request but
       * still sends the content-length the body would have had. This flag is
       * only looked at once method_ok is already false, so a request whose
       * method matches outright pays for one test of an already loaded bool
       * and nothing more. */
      bool head_ok =
          !method_ok && method == CHTTP_HEAD && route->method == CHTTP_GET;
      int ms = _match_route_cached(
          &cache, route, (method_ok || head_ok) ? &pv : NULL, srv->m_procs);
      if (ms == -1) {
        found = (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
        have_result = true;
        break;
      }
      if (ms == 0) continue;

      if (!method_ok) {
        if (head_ok) {
          /* Keep this candidate instead of returning it, because an explicit
           * CHTTP_HEAD route registered later on the same router still has to
           * win. So the scan runs to the end and we only use this candidate
           * after that. */
          if (!head_fallback_route) {
            head_fallback_route = route;
            head_fallback_pv = pv;
          } else {
            _free_param_values(pv, route->param_count, srv->m_procs);
          }
          continue;
        }
        /* A CHTTP_ANY route never gets here since it accepts every method,
         * and a CHTTP_GET route accepts HEAD too, through the fallback
         * above. */
        allowed_methods |= 1u << route->method;
        if (route->method == CHTTP_GET) allowed_methods |= 1u << CHTTP_HEAD;
        continue;
      }
      found = (match_result_t){router, route, pv, ROUTE_MATCH_OK, 0};
      have_result = true;
      break;
    }
    if (!have_result && head_fallback_route) {
      found = (match_result_t){router, head_fallback_route, head_fallback_pv,
                               ROUTE_MATCH_OK, 0};
      have_result = true;
      head_fallback_pv = NULL; /* found owns these values from here on */
    }
    /* Usually a no-op. It only does real work when the scan ended on an exact
     * match or an out-of-memory error while we were holding a fallback
     * candidate's param values. */
    _free_param_values(
        head_fallback_pv,
        head_fallback_route ? head_fallback_route->param_count : 0,
        srv->m_procs);
    _seg_cache_free(&cache);
    if (have_result) {
      _seg_cache_free(&path_cache);
      return found;
    }
  }
  _seg_cache_free(&path_cache);
  if (allowed_methods)
    return (match_result_t){owner, NULL, NULL, ROUTE_MATCH_METHOD,
                            (uint8_t)allowed_methods};
  return (match_result_t){owner, NULL, NULL, ROUTE_MATCH_NONE, 0};
}

static void _free_param_values(char **pv, int count, ccol_memmgmt_procs_t *mp) {
  if (!pv) return;
  for (int i = 0; i < count; i++) _ccol_mem_free(mp, pv[i]);
  _ccol_mem_free(mp, pv);
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
    _ccol_mem_free(mp, resp->headers[i].name);
    _ccol_mem_free(mp, resp->headers[i].value);
  }
  _ccol_mem_free(mp, resp->headers);
  resp->headers = NULL;
  resp->header_count = 0;
  resp->header_cap = 0;
  _ccol_mem_free(mp, resp->body);
  resp->body = NULL;
  resp->body_len = 0;
  resp->body_cap = 0;
}

static void _destroy_req_scratch(chttpsvr_req *req) {
  ccol_memmgmt_procs_t *mp = req->m_procs;
  if (req->_qparams) {
    chttpsvr_qparams_t *qp = req->_qparams;
    for (size_t i = 0; i < qp->count; i++) {
      _ccol_mem_free(mp, qp->keys[i]);
      _ccol_mem_free(mp, qp->values[i]);
    }
    _ccol_mem_free(mp, qp->keys);
    _ccol_mem_free(mp, qp->values);
    _ccol_mem_free(mp, qp);
  }
  if (req->_qresult) _ccol_mem_free(mp, req->_qresult);
  if (req->_ctx_more) {
    _ccol_mem_free(mp, req->_ctx_more);
    req->_ctx_more = NULL;
  }
}

static const char *_status_reason(int status) {
  /* Reason phrases from the IANA HTTP Status Code Registry. Clients ignore
   * the phrase (RFC 9112 SS4), so a code not in this list just gets
   * "Unknown". */
  switch (status) {
    case 200:
      return "OK";
    case 201:
      return "Created";
    case 202:
      return "Accepted";
    case 203:
      return "Non-Authoritative Information";
    case 204:
      return "No Content";
    case 205:
      return "Reset Content";
    case 206:
      return "Partial Content";
    case 207:
      return "Multi-Status";
    case 208:
      return "Already Reported";
    case 226:
      return "IM Used";
    case 300:
      return "Multiple Choices";
    case 301:
      return "Moved Permanently";
    case 302:
      return "Found";
    case 303:
      return "See Other";
    case 304:
      return "Not Modified";
    case 305:
      return "Use Proxy";
    case 307:
      return "Temporary Redirect";
    case 308:
      return "Permanent Redirect";
    case 400:
      return "Bad Request";
    case 401:
      return "Unauthorized";
    case 402:
      return "Payment Required";
    case 403:
      return "Forbidden";
    case 404:
      return "Not Found";
    case 405:
      return "Method Not Allowed";
    case 406:
      return "Not Acceptable";
    case 407:
      return "Proxy Authentication Required";
    case 408:
      return "Request Timeout";
    case 409:
      return "Conflict";
    case 410:
      return "Gone";
    case 411:
      return "Length Required";
    case 412:
      return "Precondition Failed";
    case 413:
      return "Payload Too Large";
    case 414:
      return "URI Too Long";
    case 415:
      return "Unsupported Media Type";
    case 416:
      return "Range Not Satisfiable";
    case 417:
      return "Expectation Failed";
    case 421:
      return "Misdirected Request";
    case 422:
      return "Unprocessable Entity";
    case 423:
      return "Locked";
    case 424:
      return "Failed Dependency";
    case 425:
      return "Too Early";
    case 426:
      return "Upgrade Required";
    case 428:
      return "Precondition Required";
    case 429:
      return "Too Many Requests";
    case 431:
      return "Request Header Fields Too Large";
    case 451:
      return "Unavailable For Legal Reasons";
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
    case 505:
      return "HTTP Version Not Supported";
    case 506:
      return "Variant Also Negotiates";
    case 507:
      return "Insufficient Storage";
    case 508:
      return "Loop Detected";
    case 510:
      return "Not Extended";
    case 511:
      return "Network Authentication Required";
    default:
      return "Unknown";
  }
}

/* Converts a chttpsvr_config_t duration field from microseconds to the
 * milliseconds the server keeps. It rounds up, so a non-zero value never
 * turns into the 0 several fields read as "off" or "the default", and
 * anything past UINT_MAX - 1 milliseconds saturates there. UINT_MAX is kept
 * free because it's the internal form of CHTTPSVR_NO_DEADLINE, which only
 * _cfg_deadline_us_to_ms produces, so no finite configuration can alias it.
 * The arithmetic is uint64_t on every target. */
static unsigned _cfg_us_to_ms(uint64_t us) {
  uint64_t ms = ccol_us_to_ms_ceil(us);
  return ms >= (uint64_t)UINT_MAX ? UINT_MAX - 1u : (unsigned)ms;
}

/* _cfg_us_to_ms for the two fields that accept CHTTPSVR_NO_DEADLINE, which
 * maps to UINT_MAX, the internal "no deadline". */
static unsigned _cfg_deadline_us_to_ms(uint64_t us) {
  return us == CHTTPSVR_NO_DEADLINE ? UINT_MAX : _cfg_us_to_ms(us);
}

/* chttpsvr_config_t documents a 0 in stream_read_timeout_us or
 * response_write_timeout_us as "wait forever", while chttp1_stream_read and
 * chttp1_stream_write follow poll(2)'s convention: negative blocks forever, 0
 * is one non-blocking try with no wait at all, positive is a bound in ms.
 *
 * Passing a configured 0 straight through as their int timeout_ms would mean
 * the opposite of what was configured, not "forever" but "give up right away
 * on every call without even one try". Their deadline code shows why: for a
 * timeout_ms of 0 it calls clock_gettime() once and sets the deadline to
 * "now", then checks the elapsed time with a second clock_gettime() before it
 * ever calls poll(). On a monotonic clock the second reading can only be at
 * or after the first, so the "already expired" branch always fires and
 * poll() and the real read or write are never tried.
 *
 * Without this translation a configured stream_read_timeout_us of 0 makes
 * chttpsvr_req_read() return -1 before the client has sent a single body
 * byte, and even a bodyless GET gets no response at all, because
 * response_write_timeout_us defaults to "0 means use stream_read_timeout_us"
 * and so inherits the same 0. Every timeout value from this module's config
 * must go through this helper before it reaches chttp1_stream_read or
 * chttp1_stream_write, or "wait forever" quietly becomes "never wait".
 *
 * A configured value above INT_MAX is a separate problem. timeout_ms of
 * chttp1_stream_read and chttp1_stream_write is a plain int, like poll(2)'s,
 * so such a value silently wraps negative on the (int) cast below. On every
 * mainstream two's complement target we build for, a negative value is
 * itself poll(2)'s "block forever", so an unlikely but valid setting like a
 * stream_read_timeout_us a bit over 24.8 days would turn into "wait forever"
 * when the configuration asked for a finite, just very long, wait. So we clamp
 * to INT_MAX instead, the longest finite wait this API can express since
 * chttp1_stream_read and chttp1_stream_write have no wider type to carry more.
 * Unlike the wraparound, the clamp doesn't collapse into the other sentinel
 * this function already treats specially (the 0 that becomes -1 above). */
static int _to_stream_timeout_ms(unsigned configured_timeout_ms) {
  if (configured_timeout_ms == 0) return -1;
  if (configured_timeout_ms > (unsigned)INT_MAX) return INT_MAX;
  return (int)configured_timeout_ms;
}

/* A small growable buffer for building one response's header block. It
 * starts on the stack, which is enough for the usual handful of headers,
 * moves to the heap when a handler's headers push the block past the stack
 * reservation, and from there grows by doubling like chttpsvr_resp_write's
 * body buffer.
 *
 * Without it a handler that sets enough headers silently loses the whole
 * response, and a single large header value is enough for that (several
 * Set-Cookie headers, CORS and CSP headers, a long custom token). A
 * fixed-capacity buffer would be simpler code, but it fails the moment the
 * block doesn't fit and closes the connection with zero bytes on the wire
 * and nothing in the log, for a request that had already produced a
 * perfectly valid response. */
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
  char *nb = (char *)_ccol_mem_alloc(b->mp, new_cap);
  if (!nb) return false;
  memcpy(nb, b->buf, b->len);
  if (b->heap) _ccol_mem_free(b->mp, b->buf);
  b->buf = nb;
  b->cap = new_cap;
  b->heap = true;
  return true;
}

/* Appends fmt and its arguments to b, growing b as often as needed. Unlike a
 * fixed-capacity buffer it never fails because of size alone, only on a real
 * allocation failure or a vsnprintf encoding error.
 *
 * __attribute__((format(printf, 2, 3))) is what lets the inner
 * vsnprintf(b->buf + b->len, avail, fmt, ap) pass fmt and ap straight
 * through. Without it Clang's -Wformat-nonliteral complains that fmt isn't a
 * literal there; with it the real format-string and argument-type check moves
 * to every call site of this function, where fmt really is a literal. */
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
    /* Try the same append again now that b has room for it. */
  }
}

/* Appends n bytes of s to b without any formatting. */
static bool _head_builder_append_bytes(_head_builder_t *b, const char *s,
                                       size_t n) {
  if (n > b->cap - b->len && !_head_builder_grow(b, n)) return false;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  return true;
}

/* ========================================================================== */
/*                         THE DATE RESPONSE HEADER                           */
/* ========================================================================== */

/* Length of an RFC 9110 SS5.6.7 IMF-fixdate such as
 * "Sun, 06 Nov 1994 08:49:37 GMT". */
#define _CHTTPSVR_IMF_FIXDATE_LEN 29

/* The last Date value this thread formatted and the second it stands for.
 * A Date header only changes once a second but goes on every response, so
 * each response-writing thread formats it once a second and reuses it for
 * the rest of that second's responses. The cache belongs to one thread, so
 * nobody else can see it half written and it needs neither a lock nor an
 * atomic.
 *
 * Its TLS model is the same as the library's other thread-local fast paths:
 * initial-exec, unless CCOL_MEMPOOL_DYNAMIC_TLS picks the general model for a
 * library that dlopen() loads late (see the same switch in cmempool.c). */
typedef struct {
  int64_t sec;
  char text[_CHTTPSVR_IMF_FIXDATE_LEN + 1];
} _chttpsvr_date_cache_t;

#if defined(CCOL_MEMPOOL_DYNAMIC_TLS) && CCOL_MEMPOOL_DYNAMIC_TLS
static __thread _chttpsvr_date_cache_t _chttpsvr_date_cache = {.sec =
                                                                   INT64_MIN};
#else
static __thread _chttpsvr_date_cache_t _chttpsvr_date_cache
    __attribute__((tls_model("initial-exec"))) = {.sec = INT64_MIN};
#endif

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook. Any value other than INT64_MIN replaces the clock, so
 * a test can cross a second boundary without sleeping. */
static _Atomic int64_t g_date_override_for_tests = INT64_MIN;
void _chttpsvr_set_date_override_for_tests(int64_t sec) {
  atomic_store(&g_date_override_for_tests, sec);
}
#endif

static void _put2(char *out, int v) {
  out[0] = (char)('0' + v / 10);
  out[1] = (char)('0' + v % 10);
}

/* Formats sec (seconds since the epoch) as a UTC IMF-fixdate. Day and month
 * names come from fixed tables, never from the process locale strftime()
 * would look at, and gmtime_r() is the reentrant form that touches no shared
 * state. It only runs once a second per thread, so we keep it out of line and
 * out of the response writer's code. */
static __attribute__((noinline, cold)) void _format_imf_fixdate(int64_t sec,
                                                                char *out) {
  static const char days[7][3] = {
      {'S', 'u', 'n'}, {'M', 'o', 'n'}, {'T', 'u', 'e'}, {'W', 'e', 'd'},
      {'T', 'h', 'u'}, {'F', 'r', 'i'}, {'S', 'a', 't'}};
  static const char months[12][3] = {
      {'J', 'a', 'n'}, {'F', 'e', 'b'}, {'M', 'a', 'r'}, {'A', 'p', 'r'},
      {'M', 'a', 'y'}, {'J', 'u', 'n'}, {'J', 'u', 'l'}, {'A', 'u', 'g'},
      {'S', 'e', 'p'}, {'O', 'c', 't'}, {'N', 'o', 'v'}, {'D', 'e', 'c'}};
  struct tm tm;
  time_t t = (time_t)sec;
  if ((int64_t)t != sec || !gmtime_r(&t, &tm) || tm.tm_year + 1900 < 0 ||
      tm.tm_year + 1900 > 9999) {
    /* A second time_t can't hold, or one without a four-digit year. No real
     * clock gets here, and the epoch is a well-formed value to use. */
    memcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", _CHTTPSVR_IMF_FIXDATE_LEN);
    out[_CHTTPSVR_IMF_FIXDATE_LEN] = '\0';
    return;
  }
  int year = tm.tm_year + 1900;
  memcpy(out, days[tm.tm_wday], 3);
  out[3] = ',';
  out[4] = ' ';
  _put2(out + 5, tm.tm_mday);
  out[7] = ' ';
  memcpy(out + 8, months[tm.tm_mon], 3);
  out[11] = ' ';
  _put2(out + 12, year / 100);
  _put2(out + 14, year % 100);
  out[16] = ' ';
  _put2(out + 17, tm.tm_hour);
  out[19] = ':';
  _put2(out + 20, tm.tm_min);
  out[22] = ':';
  _put2(out + 23, tm.tm_sec);
  memcpy(out + 25, " GMT", 4);
  out[_CHTTPSVR_IMF_FIXDATE_LEN] = '\0';
}

/* The Date value for the current second. The coarse real-time clock is just a
 * read of a value the kernel keeps in the vDSO page, no system call, and its
 * few-millisecond resolution is far finer than the one second the header can
 * express. */
static const char *_chttpsvr_date_now(void) {
  struct timespec ts;
#ifdef CLOCK_REALTIME_COARSE
  clock_gettime(CLOCK_REALTIME_COARSE, &ts);
#else
  clock_gettime(CLOCK_REALTIME, &ts);
#endif
  int64_t sec = (int64_t)ts.tv_sec;
#ifdef RUNNING_UNIT_TESTS
  int64_t ov = atomic_load(&g_date_override_for_tests);
  if (ov != INT64_MIN) sec = ov;
#endif
  _chttpsvr_date_cache_t *c = &_chttpsvr_date_cache;
  if (__builtin_expect(c->sec != sec, 0)) {
    _format_imf_fixdate(sec, c->text);
    c->sec = sec;
  }
  return c->text;
}

static void _head_builder_release(_head_builder_t *b) {
  if (b->heap) _ccol_mem_free(b->mp, b->buf);
}

/* Serializes resp into a raw HTTP/1.1 response and writes it to stream, with
 * response_write_timeout_ms bounding the write. It sets Content-Length itself
 * on every response that sends a body, since this server never uses chunked
 * transfer-encoding for its own responses, and always writes a Connection
 * header that reflects keep_alive. It returns false on a write error or a
 * timeout, and the caller must then treat the connection as unusable and
 * close it. The header block has no size limit (see _head_builder_t above).
 *
 * Two handler headers are treated differently from all the others. A
 * "Connection" header set by the handler never reaches the wire: keep_alive is
 * the one source of truth for the Connection value we send, and the caller,
 * _task_worker, reads that same value right after this call to decide whether
 * it really keeps the connection open. On a response with a body,
 * resp->body_len is the one source of truth for Content-Length, being the
 * exact number of bytes we're about to write, so the handler's Content-Length
 * never reaches the wire there either.
 *
 * If either header went out as the handler set it, the two sides could
 * silently disagree: the response could say "keep-alive" while we close the
 * socket right after, or announce a body length different from what we
 * really send. That's a framing bug that splits or desynchronizes responses on
 * a kept-alive connection, and the caller can neither detect it nor recover.
 *
 * suppress_body is true for a HEAD request. RFC 7231 SS4.3.2 says a HEAD
 * response MUST NOT contain a body and SHOULD report the same header fields as
 * a GET, Content-Length included. So we still compute Content-Length from
 * resp->body_len when the handler wrote a body and only hold back the body
 * bytes. Otherwise those bytes would sit in the socket buffer, and a
 * standards-following client, which stops reading after a HEAD response's
 * headers, would take them for the start of the next pipelined response on a
 * keep-alive connection.
 *
 * 204 and 304 have the same hazard, independently of suppress_body and of
 * HEAD: RFC 9110 SS6.4.1 and SS15.4.5 say neither may ever carry a body,
 * whatever the method. Our own chttp1_parser treats both as bodyless without
 * exception on the response side (see its CHTTP1_ST_HEADERS handling), and any
 * client following the same rule, chttpclient for one, would misread a body we
 * wrote as the start of the next pipelined response. A 1xx status from a
 * handler goes out as 500 with its body, because a 1xx is interim and a client
 * that only gets that waits forever.
 *
 * A handler that sets one of those two statuses and also writes a body with
 * chttpsvr_resp_write is much more likely to have made a mistake than to want
 * a response that breaks the standard, so we always drop the body here rather
 * than leave it to the handler.
 *
 * A 204 also MUST NOT have a Content-Length header at all, and we write none.
 * A 304, and a HEAD response for which the handler wrote no body, may carry
 * the length a 200 to a GET would have, and only that (RFC 9110 SS8.6). Only
 * the handler knows it, so there we write the handler's Content-Length, or
 * none if it set none. Such a message ends at its header block whatever the
 * field says, so the handler's value can't desynchronize the framing, and
 * chttpsvr_resp_set_header and chttpsvr_resp_add_header only accept a
 * well-formed number for it. A rejection the server writes itself (is_reject)
 * has no handler, and its HEAD form reports the body length its GET form
 * carries. */

/* Shared countdown helper against the monotonic clock. On the first call it
 * sets *deadline max_dur ms ahead and latches that in *deadline_set. On every
 * call it then shrinks *timeout_ms_inout to the time really left until that
 * deadline, but only if that's less than the per-call timeout the caller
 * already computed; a 0 in *timeout_ms_inout means "no per-call bound yet",
 * so the remaining time always wins then. Returns false once the deadline
 * itself has passed.
 *
 * With max_dur 0 it's a no-op that always returns true and touches nothing, so
 * a caller with the matching total-duration cap turned off pays one branch
 * and that's it. It's shared by _check_read_deadline (for
 * max_body_read_duration_ms) and _send_response below (for
 * max_response_write_duration_ms), so both total-duration caps are computed
 * the same way. */
/* The core of _shrink_timeout_to_deadline_inl, at the instant now. */
static inline __attribute__((always_inline)) bool _shrink_timeout_at(
    struct timespec *deadline, bool *deadline_set, unsigned max_dur,
    struct timespec now, unsigned *timeout_ms_inout) {
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
  /* long long and not long, because C99 guarantees long long has at least 64
   * bits. max_dur is unsigned and can reach UINT_MAX ms (about 49.7 days), so
   * the remaining time can reach about 4.29e15 nanoseconds, which overflows a
   * 32-bit long on ILP32, a build we compile and test in CI. With long long no
   * value of max_dur can overflow.
   *
   * The deadline has only passed when not a single nanosecond is left, and
   * what's left rounds up to whole milliseconds, so a wait never ends and a
   * cap never expires before the deadline. */
  long long remaining_ns = _ns_between(*deadline, now);
  if (remaining_ns <= 0) return false;
  long long remaining_ms = _ms_ceil_of_ns(remaining_ns);
  unsigned remaining_ms_clamped =
      (remaining_ms > (long long)UINT_MAX) ? UINT_MAX : (unsigned)remaining_ms;
  if (!*timeout_ms_inout || remaining_ms_clamped < *timeout_ms_inout)
    *timeout_ms_inout = remaining_ms_clamped;
  return true;
}

static inline __attribute__((always_inline)) bool
_shrink_timeout_to_deadline_inl(struct timespec *deadline, bool *deadline_set,
                                unsigned max_dur, unsigned *timeout_ms_inout) {
  if (!max_dur) return true;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return _shrink_timeout_at(deadline, deadline_set, max_dur, now,
                            timeout_ms_inout);
}

#ifdef RUNNING_UNIT_TESTS
/* What _shrink_timeout_to_deadline_inl decides, and the timeout it shrinks
 * to, for an already set deadline at the instant now. */
bool _chttpsvr_shrink_timeout_at_for_tests(struct timespec deadline,
                                           struct timespec now,
                                           unsigned *timeout_ms_inout) {
  bool set = true;
  return _shrink_timeout_at(&deadline, &set, 1, now, timeout_ms_inout);
}
#endif

/* Out-of-line form of _shrink_timeout_to_deadline_inl for callers that aren't
 * on every request body's path. */
static bool _shrink_timeout_to_deadline(struct timespec *deadline,
                                        bool *deadline_set, unsigned max_dur,
                                        unsigned *timeout_ms_inout) {
  return _shrink_timeout_to_deadline_inl(deadline, deadline_set, max_dur,
                                         timeout_ms_inout);
}

#ifdef RUNNING_UNIT_TESTS
/* Purely a white-box test hook. It makes the very next conn-guarded
 * write-deadline check, in _send_response's write loop through
 * _response_write_deadline_ok below, report "already expired" regardless of
 * the real elapsed time and of conn->srv->max_response_write_duration_ms.
 *
 * It exists because a courtesy rejection's header block is small (a status
 * line plus a few fixed headers, always well under a kilobyte, no body), and
 * in practice one write(2) always finishes it, so socket buffering gives no
 * reliable way to get a real short write or a real expired deadline for it.
 * max_response_write_duration_exceeded_closes_connection can do that with a
 * handler's real, large response body. For the fixed-size interim line of
 * _write_interim_continue, g_force_short_interim_write_bytes_for_tests exists
 * for the same reason.
 *
 * It disarms itself after one use, so a test doesn't have to reset it. With a
 * NULL conn it does nothing, and neither does a real expired deadline, since
 * the deadline state this hook fakes is only read when conn isn't NULL. */
static _Atomic bool g_force_next_response_write_deadline_expired_for_tests =
    false;
void _chttpsvr_force_next_response_write_deadline_expired_for_tests(void) {
  atomic_store(&g_force_next_response_write_deadline_expired_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

/* Wraps _shrink_timeout_to_deadline for every internal write-retry loop that
 * tracks a write deadline for one connection. There are three of them,
 * _send_response's loop, _send_response_continue's loop for a parked response
 * and _write_interim_continue's loop, and they all share this logic,
 * including the RUNNING_UNIT_TESTS-only forced-expiry hook above. Every caller
 * has already checked that conn isn't NULL.
 *
 * apply_internal_ceiling picks the total-duration cap for this write. false
 * means a real response for a matched route (_send_response passes its own
 * is_reject straight through), and the cap is then exactly
 * conn->srv->max_response_write_duration_ms, unchanged; 0 there means "no
 * limit", the operator's explicit choice for a body the handler controls.
 *
 * true means one of the small, fixed-shape internal writes, a courtesy
 * rejection or the "100 Continue" interim line (the comment of
 * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS says why both get this). The cap is
 * then the tighter of the configured value and
 * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. It's never looser than what the
 * operator configured, so a smaller configured cap still applies exactly, and
 * never unbounded either, even with the knob left at its documented "off"
 * default. */
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
      (configured == 0 || configured > _internal_write_ceiling_ms()))
    effective_max_dur = _internal_write_ceiling_ms();
  return _shrink_timeout_to_deadline(&conn->write_deadline,
                                     &conn->write_deadline_set,
                                     effective_max_dur, call_timeout_ms);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook. The next handler response write sends only its first
 * n bytes and then acts as if the socket were full, so a test can end a
 * partial write exactly inside the header block or the body and watch the
 * parked rest resume. It disarms itself after one use. */
static _Atomic size_t g_force_short_response_write_for_tests = 0;
void _chttpsvr_force_short_response_write_for_tests(size_t n) {
  atomic_store(&g_force_short_response_write_for_tests, n);
}
#endif

/* About conn: both callers, _conn_reject_and_close for a courtesy rejection
 * and _task_worker for a real matched-route response, always pass their own
 * real, non-NULL conn, so conn->srv's max_response_write_duration_ms bounds
 * every _send_response write on top of timeout_ms's per-call bound. It's
 * applied through conn->write_deadline, conn->write_deadline_set and the
 * shared _shrink_timeout_to_deadline helper above. Both callers write with
 * stream->write_nonblocking set, so a full socket parks the rest of the
 * response instead of waiting (see _send_response_park).
 *
 * is_reject also switches on _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS, a hard
 * ceiling on the total duration that applies only to a rejection, whatever
 * the operator set max_response_write_duration_ms to; the comment of
 * _response_write_deadline_ok says why.
 *
 * The `if (conn && ...)` checks below just defend this internal helper's
 * signature, since no real caller passes NULL. */
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

  /* This is the final response, so a 1xx goes out as 500 like any
   * out-of-range code. A 1xx is interim (RFC 9110 SS15.2) and a client that
   * gets one keeps waiting for a final response that never comes. We write
   * the interim "100 Continue" ourselves on another path. */
  int status = (resp->status_code >= 200 && resp->status_code <= 999)
                   ? resp->status_code
                   : 500;
  /* 205 Reset Content carries no content either (RFC 9110 SS15.3.6). */
  bool no_body =
      suppress_body || status == 204 || status == 304 || status == 205;
  bool handler_date = false;
  const char *handler_length = NULL;

  if (!_head_builder_append(&hb, "HTTP/1.1 %d %s\r\n", status,
                            _status_reason(status))) {
    _head_builder_release(&hb);
    return false;
  }

  for (size_t i = 0; i < resp->header_count; i++) {
    /* We never write "connection" from resp->headers, whether a handler set
     * one or not, because keep_alive below is the single source of truth for
     * whether we really keep the connection open afterwards. _task_worker
     * computes its own keep_alive the same way and makes the real decision
     * after the response, and a handler's value going out as is could
     * silently contradict that. A handler might set "Connection: keep-alive"
     * after the server's framing and timeout logic already decided to close
     * right after, and the client would then believe the opposite of what
     * happens. This function is the one place that both decides and
     * announces connection reuse, so it must be the only writer of this
     * header.
     *
     * "content-length" is filtered for the same reason. resp->body_len is the
     * single source of truth for how many body bytes we write a few lines
     * below (chttpsvr_resp_write, _write_str, _printf and _write_json fill
     * it), and a handler's value can disagree with it, e.g. a stale or
     * copy-pasted header or one that just echoes request data. On a kept-alive
     * connection that's as real a framing hazard as a diverging Connection
     * header: the client reads the next pipelined response's bytes as the
     * tail of this body, or blocks waiting for bytes that never come. We
     * always compute the value from the one true byte count we're about to
     * send and never trust a caller's number that can drift from it, closing
     * the gap the same way as for Connection above. */
    if (ccol_ascii_casecmp(resp->headers[i].name, "connection") == 0) continue;
    if (ccol_ascii_casecmp(resp->headers[i].name, "content-length") == 0) {
      handler_length = resp->headers[i].value;
      continue;
    }
    /* A Date the handler set replaces the one the server adds (see the Date
     * block below). The response keeps one entry per each of these three
     * names, whichever call set it, so this loop writes at most one. */
    if (ccol_ascii_casecmp(resp->headers[i].name, "date") == 0)
      handler_date = true;
    /* No space after the colon: this module's test suite does raw-socket
     * checks on this exact wire format, like strstr(buf, "connection:close"),
     * so it has to come out byte for byte. */
    if (!_head_builder_append(&hb, "%s:%s\r\n", resp->headers[i].name,
                              resp->headers[i].value)) {
      _head_builder_release(&hb);
      return false;
    }
  }

  /* Every final response carries a Date, as RFC 9110 SS6.6.1 asks of an
   * origin server that has a clock, unless the handler set its own. Go's
   * net/http server follows the same rule. */
  if (!handler_date) {
    char date_line[5 + _CHTTPSVR_IMF_FIXDATE_LEN + 2];
    memcpy(date_line, "date:", 5);
    memcpy(date_line + 5, _chttpsvr_date_now(), _CHTTPSVR_IMF_FIXDATE_LEN);
    date_line[5 + _CHTTPSVR_IMF_FIXDATE_LEN] = '\r';
    date_line[6 + _CHTTPSVR_IMF_FIXDATE_LEN] = '\n';
    if (!_head_builder_append_bytes(&hb, date_line, sizeof(date_line))) {
      _head_builder_release(&hb);
      return false;
    }
  }

  /* Content-Length for a response that sends no body. A 204 has none at all.
   * A 304, and a HEAD response where the handler wrote nothing, describe a
   * representation that isn't in this message. Only the handler knows its
   * length, so the field goes out exactly when the handler set it, and a 0
   * made up by the server would tell a cache the stored representation is
   * empty (RFC 9110 SS8.6). Wherever the handler did write a body, that
   * body's length is the framing and beats any value the handler set, which
   * is also Go's net/http rule. A rejection the server writes itself has no
   * handler; the server authored the body a GET would get, so a HEAD
   * rejection reports that body's length. A 205 says it has no content with
   * "content-length:0" whatever the handler wrote or set, as RFC 9110
   * SS15.3.6 asks. */
  if (status != 204) {
    bool ok_len = true;
    bool handler_head = suppress_body && resp->body_len == 0 && !is_reject;
    if (__builtin_expect(status == 205, 0))
      ok_len = _head_builder_append(&hb, "content-length:0\r\n");
    else if (__builtin_expect(status != 304 && !handler_head, 1))
      ok_len =
          _head_builder_append(&hb, "content-length:%zu\r\n", resp->body_len);
    else if (handler_length)
      ok_len =
          _head_builder_append(&hb, "content-length:%s\r\n", handler_length);
    if (!ok_len) {
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

  /* Head and body go out together, as one writev(2) for plain text and as one
   * TLS record when they fit (see chttp1_stream_writev2). We try the write
   * first and only wait once the socket is full, so an ordinary response
   * costs a single system call. sent counts across the head and then the
   * body, and a short write can end inside either.
   *
   * The loop rereads conn's server-level cap on every write instead of
   * caching it. The cap can only shrink the per-call timeout further, never
   * widen it. */
  size_t blen = no_body ? 0 : resp->body_len;
  size_t total = hb.len + blen;
  size_t sent = 0;
  bool ok = true;
  while (sent < total) {
    unsigned call_timeout_ms = timeout_ms;
    if (conn &&
        !_response_write_deadline_ok(conn, is_reject, &call_timeout_ms)) {
      ok = false;
      break;
    }
    size_t alen = sent < hb.len ? hb.len - sent : 0;
    size_t boff = sent > hb.len ? sent - hb.len : 0;
#ifdef RUNNING_UNIT_TESTS
    /* See _chttpsvr_force_short_response_write_for_tests. */
    size_t forced =
        atomic_exchange(&g_force_short_response_write_for_tests, (size_t)0);
    if (forced && stream->write_nonblocking && conn) {
      size_t fa = alen < forced ? alen : forced;
      size_t brest = blen - boff;
      size_t fb = brest < forced - fa ? brest : forced - fa;
      ssize_t fn = chttp1_stream_writev2(stream, hb.buf + (hb.len - alen), fa,
                                         fb ? resp->body + boff : NULL, fb, 0);
      if (fn > 0) sent += (size_t)fn;
      alen = sent < hb.len ? hb.len - sent : 0;
      boff = sent > hb.len ? sent - hb.len : 0;
      _send_response_park(conn, stream, hb.buf + (hb.len - alen), alen, boff,
                          keep_alive, no_body, is_reject);
      ok = false;
      break;
    }
#endif
    ssize_t n = chttp1_stream_writev2(
        stream, hb.buf + (hb.len - alen), alen, blen ? resp->body + boff : NULL,
        blen - boff, _to_stream_timeout_ms(call_timeout_ms));
    if (n <= 0) {
      /* The socket is full and the caller won't let us wait, so the rest of
       * the response goes out later from a parked connection. The unsent part
       * of the header block lives on this stack, so it gets copied. */
      if (n < 0 && stream->write_nonblocking && conn &&
          chttp1_stream_timed_out(stream))
        _send_response_park(conn, stream, hb.buf + (hb.len - alen), alen, boff,
                            keep_alive, no_body, is_reject);
      ok = false;
      break;
    }
    sent += (size_t)n;
  }
  _head_builder_release(&hb);
  return ok;
}

/* Writes RFC 7231 SS5.1.1's "HTTP/1.1 100 Continue\r\n\r\n" interim response
 * for both Expect: 100-continue send sites, _task_worker for a buffered route
 * and chttpsvr_req_read for a streaming one. It copies _send_response's write
 * loop on purpose instead of doing a single write without retry, which
 * max_response_write_duration_ms would leave unbounded, and there are three
 * reasons for that.
 *
 *   - It loops on a short write instead of taking n <= 0 as the only sign of
 *     failure. chttp1_stream_write is documented to return less than asked
 *     on a short write, POSIX write(2) semantics, so a caller that needs the
 *     whole buffer on the wire needs to retry the rest, the same discipline
 *     _send_response follows for the real response. A single unretried write
 *     of this 25-byte line can leave a truncated status line like
 *     "HTTP/1.1 100 Con" on the wire that nothing notices or reports, on a
 *     connection with enough send-buffer pressure for even such a short
 *     write to block part way.
 *
 *   - Every single write attempt goes through conn->write_deadline and
 *     _response_write_deadline_ok just like _send_response's writes, so this
 *     interim write's time counts against the same per-request
 *     max_response_write_duration_ms budget that bounds the real response,
 *     instead of being extra cost outside it. Otherwise the write is fully
 *     unbounded in a common configuration that leaves
 *     response_write_timeout_ms at its default ("fall back to
 *     stream_read_timeout_ms") and sets stream_read_timeout_ms to 0, i.e.
 *     "wait forever", which suits slow but legitimate uploads. The operator
 *     then relies on max_response_write_duration_ms alone to limit how long
 *     a slow reader can pin a worker thread.
 *
 *     apply_internal_ceiling is always true here, unlike in _send_response,
 *     which only sets it for a rejection and never for a real response. The
 *     interim line is always this library's own small, fixed 25 bytes, so
 *     _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS always bounds it, even when the
 *     operator sets max_response_write_duration_ms to 0, which turns that
 *     knob off for their own handlers' response bodies. Without the ceiling
 *     a peer could hold a worker-pool thread forever just by sending an
 *     ordinary "Expect: 100-continue" request, standard behavior and curl's
 *     default for a large upload, and then trickling its reads of this
 *     reply. A handful of such connections would use up the whole pool.
 *
 *   - It resets conn->write_deadline_set to false before returning, on every
 *     exit path without exception. conn->write_deadline and
 *     conn->write_deadline_set belong to one REQUEST and only
 *     _conn_reset_for_request clears them, once, while this function runs
 *     strictly before the request body is read: before _drain_body for a
 *     buffered route, before chttpsvr_req_read's read loop for a streaming
 *     one.
 *
 *     Leaving write_deadline_set true after this call would make
 *     _send_response's later _shrink_timeout_to_deadline calls reuse the
 *     deadline THIS call set, so the max_response_write_duration_ms budget
 *     would silently pay for every second the body read takes in between.
 *     That phase has nothing to do with it, is often much longer, and
 *     already has its own bounds in stream_read_timeout_ms and
 *     max_body_read_duration_ms. Picture a request whose body legitimately
 *     takes a few real seconds to arrive, well inside its read budget (the
 *     "wait forever" setup for slow legitimate uploads is documented and
 *     supported). Without the reset, _send_response would find the deadline
 *     expired at its very first write and close the connection with zero
 *     response bytes sent, even though the real send took no measurable
 *     time.
 *
 *     With the reset _send_response always sets its own fresh deadline that
 *     only reflects its own send time, which matches the documented contract
 *     of max_response_write_duration_ms: it bounds the total wall-clock time
 *     a worker thread spends to SEND one response. This interim write's own
 *     cost is still charged to the budget while it happens, through the same
 *     _shrink_timeout_to_deadline calls below, though at about 25 bytes it's
 *     usually too small to measure. Only the leftover deadline, set but not
 *     used up, is dropped, and it's never carried into another phase of the
 *     request it was never meant to bound.
 *
 * Returns 1 only when the complete 25-byte line was written, and -1 when the
 * rest got parked (see the function's own comment). Two outcomes return 0: a
 * total write failure, where n <= 0 on the very first attempt and nothing hit
 * the wire, and a real short write, where the deadline expired or the stream
 * reported a hard error after one or more bytes of the line were out.
 *
 * For a caller the two are NOT the same. A total failure leaves the
 * connection exactly as it was, so any later read or response send can just
 * fail through its own error path, which the caller already handles. A short
 * write leaves a truncated status line on the wire that no client can parse
 * and nothing can take back, so the caller must not write anything more on
 * this stream: that would land right after the truncated line and break the
 * client's response framing, and it's pointless anyway once the peer gives up
 * on the connection.
 *
 * So both callers handle ANY 0 the same way, not just a short write: they
 * skip the real response and close the connection. There's no cheap way to
 * tell the two cases apart from outside, and no need to, since neither leaves
 * a connection worth any more bytes. */
#ifdef RUNNING_UNIT_TESTS
/* Purely a white-box test hook. A test needs direct control over how much of
 * the tiny 25-byte interim line reaches the wire, because that's the only way
 * to reproduce a real short write of it every time; coaxing the OS's socket
 * buffering into a real partial write isn't reliable.
 *
 * Armed with an n where 0 < n < 25, the very next _write_interim_continue()
 * writes exactly n bytes for real, with a real chttp1_stream_write() of a
 * truncated prefix that a test client really receives, and then returns false
 * immediately. That imitates a deadline expiring just after the partial write,
 * exactly what a real slow reader can trigger under
 * max_response_write_duration_ms.
 *
 * It disarms itself after one use by setting n back to 0, so a test doesn't
 * need to reset it, and the guard keeps the symbol out of a production build
 * completely, like every other white-box helper in this file. */
static _Atomic size_t g_force_short_interim_write_bytes_for_tests = 0;
void _chttpsvr_force_short_interim_write_for_tests(size_t n) {
  atomic_store(&g_force_short_interim_write_bytes_for_tests, n);
}
#endif /* RUNNING_UNIT_TESTS */
#ifdef RUNNING_UNIT_TESTS
/* White-box test hook. Armed with n, the next interim line a buffered route
 * writes without waiting sends its first n bytes (n may be 0) and then acts
 * as if the socket were full, so a test can watch the rest wait in a parked
 * connection and then go out. Disarms itself after one use; SIZE_MAX means
 * "not armed". */
static _Atomic size_t g_force_interim_park_after_for_tests = SIZE_MAX;
void _chttpsvr_force_interim_park_after_for_tests(size_t n) {
  atomic_store(&g_force_interim_park_after_for_tests, n);
}
#endif /* RUNNING_UNIT_TESTS */

/* The interim line and its length. */
static const char _interim_continue_line[] = "HTTP/1.1 100 Continue\r\n\r\n";
#define _INTERIM_CONTINUE_LEN (sizeof(_interim_continue_line) - 1)

/* Returns 1 once the whole line is out and 0 when the write failed or hit its
 * deadline. A stream whose writes don't wait (write_nonblocking) can also run
 * into a full socket. Then we save the unsent rest as a parked response of
 * conn (see _send_response_park), keep the write's deadline for that rest and
 * return -1, the caller lets go of the connection, and the reactor resumes it
 * once the socket takes bytes again (see _task_resume). If copying the rest
 * fails we return 0. */
static int _write_interim_continue(chttpsvr_conn_t *conn,
                                   chttp1_stream_t *stream,
                                   unsigned wtimeout_ms) {
  const char *cont = _interim_continue_line;
  const size_t clen = _INTERIM_CONTINUE_LEN;
  size_t sent = 0;
#ifdef RUNNING_UNIT_TESTS
  size_t forced_short =
      atomic_exchange(&g_force_short_interim_write_bytes_for_tests, 0);
  if (forced_short > 0 && forced_short < clen) {
    ssize_t n = chttp1_stream_write(stream, cont, forced_short,
                                    _to_stream_timeout_ms(wtimeout_ms));
    if (n > 0) sent = (size_t)n;
    conn->write_deadline_set = false;
    return sent == clen;
  }
  size_t forced_park = SIZE_MAX;
  if (stream->write_nonblocking)
    forced_park =
        atomic_exchange(&g_force_interim_park_after_for_tests, SIZE_MAX);
#endif /* RUNNING_UNIT_TESTS */
  while (sent < clen) {
    unsigned call_timeout_ms = wtimeout_ms;
    if (!_response_write_deadline_ok(conn, /*apply_internal_ceiling=*/true,
                                     &call_timeout_ms))
      break;
    size_t want = clen - sent;
    bool would_block = false;
#ifdef RUNNING_UNIT_TESTS
    if (forced_park != SIZE_MAX) {
      want = forced_park < clen ? forced_park : clen;
      forced_park = SIZE_MAX;
      would_block = true;
    }
#endif
    ssize_t n = 0;
    if (want)
      n = chttp1_stream_write(stream, cont + sent, want,
                              _to_stream_timeout_ms(call_timeout_ms));
    if (n > 0) sent += (size_t)n;
    if (!would_block && n > 0) continue;
    if (!would_block && n < 0 && stream->write_nonblocking &&
        chttp1_stream_timed_out(stream))
      would_block = true;
    if (would_block) {
      if (sent == clen) break;
      _send_response_park(conn, stream, cont + sent, clen - sent, 0,
                          /*keep_alive=*/true, /*no_body=*/true,
                          /*internal=*/true);
      if (conn->wp_active && conn->wp_keep_alive) {
        conn->wp_interim = true;
        return -1;
      }
      /* We couldn't save the rest of the line, or the body bytes that came
       * along with the headers. */
      conn->wp_active = false;
      _ccol_mem_free(conn->m_procs, conn->wp_head);
      conn->wp_head = NULL;
    }
    break;
  }
  conn->write_deadline_set = false;
  return sent == clen;
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
  /* A NULL route is the last step in a REJECTED request's chain (see
   * _conn_reject_and_close): either no handler matched, or the one that did
   * isn't allowed to run. So reaching the end of the chain means "send the
   * rejection response resp already holds". A middleware that never calls
   * next ends the chain with its own response instead, the same as for a
   * matched route. */
  if (ctx->route) ctx->route->fn(req, resp, ctx->route->ctx);
}

/* Which rejection statuses run the request's middleware chain before their
 * courtesy response goes out.
 *
 * ROUTING decides a 404 and a 405, and the request's declared Content-Length
 * against the configured limit decides a 413. All three are ordinary,
 * high-volume client traffic: a scanner probing paths, a client using the
 * wrong verb, another sending an oversized upload. Some middleware, a rate
 * limiter, a per-peer ban list or an access log, needs to see and count all
 * three. A middleware may also answer such a request itself by writing its own
 * response and not calling next.
 *
 * Three rejection statuses are left out on purpose. A 500 comes from an
 * allocation failure inside this module, where running application code that
 * allocates again is the wrong answer to memory pressure, and the middleware
 * snapshot may itself be what we failed to build. A 503 means the worker pool
 * is already full, so starting more application work is exactly what we must
 * not do. A 501 comes from the request line itself, decided before the
 * request target is parsed for a method token that isn't one of the seven
 * chttp_method_t names, so there is no request object for a middleware to
 * read. It belongs with the other request-line rejections, which this module
 * answers by closing the connection without running anything. */
static bool _reject_status_runs_middleware(int status) {
  return status == CHTTP_STATUS_NOT_FOUND ||
         status == CHTTP_STATUS_METHOD_NOT_ALLOWED ||
         status == CHTTP_STATUS_PAYLOAD_TOO_LARGE;
}

/* Snapshots the effective middleware chain into dispatch: the root router's
 * chain (the global middleware), then router's chain when router is a
 * sub-router. The caller holds srv->routes_lock, and that's what makes it
 * cheap, at most _CHTTPSVR_MAX_MW pointer pairs copied inside the lock section
 * the route search needed anyway.
 *
 * router is the matched route's router, or the sub-router that owns the path
 * of a request it answers with a 404 or 405, or NULL for a rejection from the
 * root. dispatch->route is left NULL and the caller sets it for a matched
 * route. For a rejection the NULL route is what tells _chttpsvr_next that the
 * end of the chain is the rejection response, and it's also what keeps a
 * rejected request away from any handler.
 *
 * Returns false when the two chains together have more than _CHTTPSVR_MAX_MW
 * entries, and the caller answers that with a 500, whatever the request would
 * have got otherwise. */
static bool _snapshot_chain(struct chttpserver *srv, chttpsvr_router *router,
                            dispatch_ctx_t *dispatch) {
  int mc = 0;
  chttpsvr_mw_node_t *n;
  for (n = srv->root_router->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
    dispatch->mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
  bool overflow = (n != NULL);
  if (!overflow && router && router != srv->root_router) {
    for (n = router->mw_head; n && mc < _CHTTPSVR_MAX_MW; n = n->next)
      dispatch->mw_snap[mc++] = (_mw_entry_t){n->fn, n->ctx};
    overflow = (n != NULL);
  }
  dispatch->srv = srv;
  dispatch->router = router ? router : srv->root_router;
  dispatch->route = NULL;
  dispatch->mw_idx = 0;
  dispatch->mw_count = mc;
  return !overflow;
}

/* ========================================================================== */
/*                    CONNECTION LIFECYCLE                                    */
/* ========================================================================== */

/* Frees the body of the request that just ended and gives back its body
 * memory reservation in the same step, since nothing reads the body once the
 * handler has returned or the request was refused or closed. The reservation
 * lives exactly as long as the buffer, so max_partial_body_memory bounds every
 * body byte a buffered route holds, including the body a running handler is
 * reading. A response that then waits for a slow reader, or a lingering
 * close, must not keep the body: such a wait holds no thread, so there's no
 * bound on how many connections are in it that the memory limit could cover.
 *
 * Every path where a request ends gets here: _task_finish,
 * _conn_drop_request_state, _conn_reset_for_request and _conn_close. A
 * reservation can exist before its buffer, because a Content-Length body
 * reserves its first buffer before its first byte is read, so the release
 * doesn't depend on the buffer being there. */
static inline __attribute__((always_inline)) void _conn_drop_body(
    chttpsvr_conn_t *conn) {
  if (conn->body.buf) {
    _ccol_mem_free(conn->m_procs, conn->body.buf);
    memset(&conn->body, 0, sizeof(conn->body));
  }
  if (__builtin_expect(conn->mem_charged != 0 || conn->mem_exempt, 0))
    _mem_release(conn);
}

/* Frees the rest of what a request holds that nobody reads once its response
 * is being written: the target, the query, the header fields and the path
 * parameter values. A connection calls it when it starts waiting for a slow
 * reader or lingering, for the reason given at _conn_drop_body. matched_route
 * stays since routes live as long as the server. */
static __attribute__((noinline, cold)) void _conn_drop_request_state(
    chttpsvr_conn_t *conn) {
  ccol_memmgmt_procs_t *mp = conn->m_procs;
  _conn_drop_body(conn);
  _ccol_mem_free(mp, conn->path);
  conn->path = NULL;
  _ccol_mem_free(mp, conn->decoded_path);
  conn->decoded_path = NULL;
  _ccol_mem_free(mp, conn->raw_query);
  conn->raw_query = NULL;
  for (size_t i = 0; i < conn->hdr_count; i++) {
    _ccol_mem_free(mp, conn->hdr_names[i]);
    _ccol_mem_free(mp, conn->hdr_values[i]);
  }
  _ccol_mem_free(mp, conn->hdr_names);
  _ccol_mem_free(mp, conn->hdr_values);
  conn->hdr_names = conn->hdr_values = NULL;
  conn->hdr_count = conn->hdr_cap = 0;
  _free_param_values(conn->matched_param_values,
                     conn->matched_route ? conn->matched_route->param_count : 0,
                     mp);
  conn->matched_param_values = NULL;
}

static void _conn_reset_for_request(chttpsvr_conn_t *conn) {
  ccol_memmgmt_procs_t *mp = conn->m_procs;
  _ccol_mem_free(mp, conn->path);
  conn->path = NULL;
  _ccol_mem_free(mp, conn->decoded_path);
  conn->decoded_path = NULL;
  _ccol_mem_free(mp, conn->raw_query);
  conn->raw_query = NULL;
  for (size_t i = 0; i < conn->hdr_count; i++) {
    _ccol_mem_free(mp, conn->hdr_names[i]);
    _ccol_mem_free(mp, conn->hdr_values[i]);
  }
  _ccol_mem_free(mp, conn->hdr_names);
  _ccol_mem_free(mp, conn->hdr_values);
  conn->hdr_names = conn->hdr_values = NULL;
  conn->hdr_count = conn->hdr_cap = 0;
  /* Free the finished request's matched param values before clearing the
   * pointer. _conn_free, the final teardown, already does this right for the
   * LAST request on a connection, but a keep-alive connection comes through
   * this reset between every two requests, so clearing the pointer without
   * the free would leak every param-value array but the last. valgrind would
   * report those as "definitely lost" blocks going back to
   * _match_route_cached's pv calloc, with each one's decoded strings as
   * "indirectly lost". */
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
  conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
  conn->options_star = false;
  conn->expects_continue = false;
  conn->interim_continue_sent = false;
  conn->interim_write_failed = false;
  _conn_drop_body(conn);
  conn->body_bytes_seen = 0;
  conn->body_too_large = false;
  conn->body_alloc_failed = false;
  conn->transfer_aborted = false;
  conn->body_malformed = false;
  conn->read_deadline_set = false;
  conn->deadline_exceeded = false;
  conn->write_deadline_set = false;
  /* The reactor-owned phase clock belongs to one request, not to the
   * connection. It gets armed again by whichever step runs first for the
   * NEXT request, the handshake step of _conn_pump or _conn_feed_bytes. A
   * clock left armed here would charge that request for the keep-alive gap in
   * front of it and close a healthy idle connection the moment its next
   * request shows up. */
  conn->header_phase_active = false;
  /* The rate floor measures one body read or one response write of one
   * request, so the next request starts its own. */
  conn->rate_active = false;

  /* We re-init conn->parser in place rather than building a fresh
   * chttp1_parser_t on the stack and assigning it over conn->parser, because
   * of its size: chttp1_parser_t has an 8192-byte line_buf that a struct
   * assignment would copy on every keep-alive request. The in-place init only
   * clears the state in front of that buffer and never the buffer itself (see
   * the comment of line_buf in chttp1_parser.h).
   *
   * conn->parser.settings is read into the call argument before
   * chttp1_parser_init_request clears the field, and since C evaluates a
   * call's arguments before entering the callee's body, we're not reading a
   * value the callee has already cleared. */
  const chttp1_settings_t *settings = conn->parser.settings;
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  size_t max_hdr_bytes = atomic_load(&conn->srv->max_header_bytes);
  if (max_hdr_bytes) {
    conn->parser.max_header_count_override = 0;
    conn->parser.max_total_header_bytes_override = max_hdr_bytes;
  }
  /* Caps one chunk's declared size at what the whole request body may be.
   * chttp1_parser checks it as soon as it parses a chunk-size line instead of
   * waiting for that many bytes to arrive. See the max_body_size check of
   * _on_body and the matching up-front Content-Length check in
   * _on_headers_complete below.
   *
   * Without it, one absurdly large declared chunk (anything up to UINT64_MAX
   * is a valid chunk-size token) ties up a worker thread when the peer never
   * sends those bytes: the thread waits for stream_read_timeout_ms or
   * max_body_read_duration_ms, because a limit that counts bytes can never
   * trigger when the bytes never come.
   *
   * We leave the field at its default 0 ("no cap") in one narrow corner, a
   * max_body_size configured to exactly 0. 0 is this field's "no cap"
   * sentinel too (see its doc comment) and can't also mean "cap at zero". In
   * that corner the reactive _on_body check still catches the very first body
   * byte the peer sends for any chunk, and the only case left uncapped is a
   * chunk that declares a non-zero size and then sends nothing. That's a
   * low-value target and not worth a second sentinel and the complexity that
   * comes with it. */
  conn->parser.max_chunk_size_override =
      (uint64_t)atomic_load(&conn->srv->max_body_size);
}

static chttpsvr_conn_t *_conn_create(struct chttpserver *srv, int fd,
                                     const chttp1_settings_t *settings) {
  chttpsvr_conn_t *conn =
      (chttpsvr_conn_t *)_ccol_mem_calloc(srv->m_procs, 1, sizeof(*conn));
  if (!conn) return NULL;
  conn->fd = fd;
  conn->srv = srv;
  conn->m_procs = srv->m_procs;
  atomic_init(&conn->lifetime_refs, 1);
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  /* _conn_reset_for_request() sets every per-request default this connection
   * needs (resp.status_code, resp.m_procs, reject_status, the header-size
   * override and more), and it has to run before the first request too. A
   * freshly calloc'ed conn has resp.status_code == 0, and without this call
   * _send_response() would take that 0 as an unset or invalid status and
   * quietly turn it into 500 for a connection's very first request. Later
   * keep-alive requests are fine since each of them goes through this reset
   * anyway. All the frees the reset does are no-ops here, because calloc left
   * those fields NULL or 0. */
  _conn_reset_for_request(conn);
  return conn;
}

static void _conn_free(chttpsvr_conn_t *conn) {
  if (!conn) return;
  /* Grab this before conn is freed below, since the deferred decrement at the
   * end of the function needs it and reading conn there isn't safe. */
  struct chttpserver *srv = conn->srv;
  ccol_memmgmt_procs_t *mp = conn->m_procs;
  _idle_list_remove(conn);
  _diverted_list_remove(conn);
  if (conn->tls) ctls_conn_destroy(conn->tls);
  if (conn->fd >= 0) close(conn->fd);
  _ccol_mem_free(mp, conn->path);
  _ccol_mem_free(mp, conn->decoded_path);
  _ccol_mem_free(mp, conn->raw_query);
  for (size_t i = 0; i < conn->hdr_count; i++) {
    _ccol_mem_free(mp, conn->hdr_names[i]);
    _ccol_mem_free(mp, conn->hdr_values[i]);
  }
  _ccol_mem_free(mp, conn->hdr_names);
  _ccol_mem_free(mp, conn->hdr_values);
  _free_param_values(conn->matched_param_values,
                     conn->matched_route ? conn->matched_route->param_count : 0,
                     mp);
  _destroy_resp(&conn->resp, mp);
  _ccol_mem_free(mp, conn->body.buf);
  /* Defence in depth, not a fix for a live leak. Every path that sets
   * conn->_carry_over (_conn_start_diverted and _task_worker's keep-alive
   * tail) frees it and clears it before conn can get here, so on every
   * existing path this free does nothing. It's there so that a future reject
   * or close path whose author doesn't know that rule fails safely with a
   * harmless free of NULL, instead of silently leaking the carry-over buffer
   * with nothing to catch it. */
  _ccol_mem_free(mp, conn->_carry_over);
  if (conn->wp_head) _ccol_mem_free(mp, conn->wp_head);
  _ccol_mem_free(mp, conn);

  /* This decrement comes LAST, after every access to conn and mp above.
   * _drain_and_close_all_connections's wait loop takes a current_connections
   * of 0 as proof that this connection is completely done, and lets the
   * server-level teardown go on to free the routers, m_procs and more.
   * Decrementing any earlier, say right after close(conn->fd), would let that
   * waiter see "done" while this function is still working on conn, mp and
   * srv. That's a real data race, because _conn_free can run asynchronously
   * from _conn_on_removed on cthreadcomm's reactor thread and not always on
   * the thread that called _conn_close. */
  atomic_fetch_sub(&srv->current_connections, 1);
}

/* The on_removed handler of every conn->reg registration created below. It
 * fires exactly once per registration, asynchronously, from cthreadcomm's
 * reclaim path, and only once neither a dispatch of that registration nor any
 * other ccol_event_loop call resolving it can touch conn any more. The doc
 * comment of ccol_event_loop_remove() explains why that call can't promise
 * the same synchronously: a synchronous version risks a lock-ordering cycle
 * against conn->srv's locks, which _conn_on_readable, _conn_on_writable and
 * _conn_on_error take.
 *
 * It decrements conn->lifetime_refs (that field's comment says why one firing
 * alone doesn't prove conn is safe to free) and frees conn when this was the
 * last outstanding contribution.
 *
 * srv is read before that possible free, since _conn_free frees conn and
 * never srv. srv->lifetime_refs is then decremented every time, whether this
 * firing freed conn or not. srv's count tracks every registration ever created
 * for ANY of its connections (see that field's comment), one contribution per
 * registration, completely independent of the per-connection count in
 * conn->lifetime_refs. That's what lets __chttpsvr_destroy leave the final
 * free of srv to whichever decrement turns out to be last, this one or its
 * own direct one, so __chttpsvr_destroy never blocks on it. */
static void _conn_on_removed(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) _conn_free(conn);
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(srv);
}

static void _conn_close(chttpsvr_conn_t *conn) {
  /* We unlink conn here, synchronously, rather than leaving it to _conn_free
   * below, so once this function returns neither the idle-timeout sweep nor a
   * bounded shutdown drain can find conn in either list again, even though the
   * real free may happen much later from _conn_on_removed when
   * conn->lifetime_refs allows it. Both helpers are idempotent, so these
   * calls are harmless when conn was never in either list. */
  _idle_list_remove(conn);
  _diverted_list_remove(conn);

  if (conn->reg) {
    ccol_event_reg reg = conn->reg;
    conn->reg = CCOL_EVENT_REG_INVALID;
    /* On purpose we do NOT free conn here. This may be the registration
     * removing itself with no dispatch in flight, where freeing would be
     * right for THIS registration alone, but conn->lifetime_refs can still
     * hold a contribution from an earlier registration episode whose
     * on_removed hasn't fired yet (see that field's comment). The on_removed
     * that ccol_event_loop_remove() delivers for this exact registration
     * accounts for the count correctly whatever the order, and it goes
     * through the shared decrement-and-maybe-free path just below. */
    ccol_event_loop_remove(srv_engine_bundler.reactor, reg);
  }

  conn->state = CONN_ST_CLOSING;
  /* A body that ends here, cut off or refused, is freed and its body memory
   * reservation goes back immediately, so a waiting request can go ahead; see
   * _conn_drop_body. */
  _conn_drop_body(conn);
  /* The application's "done with conn" vote, counted exactly once per
   * _conn_close call. This function is only ever called once per conn, and
   * the single _conn_free call here relies on that; see the comment of
   * conn->lifetime_refs. */
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) _conn_free(conn);
}

/* ========================================================================== */
/*                    PARSER CALLBACKS (reactor-thread side)                  */
/* ========================================================================== */

static int _on_request_line(chttp1_parser_t *p, const char *method,
                            size_t method_len, const char *target,
                            size_t target_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;

  /* This server speaks HTTP/1.x. A request line with any other major
   * version (HTTP/0.9, HTTP/2.0, HTTP/3.0) gets 505 HTTP Version Not
   * Supported (RFC 9110 SS15.6.6) and a close before anything looks at the
   * target or the headers, since we don't know how such a message frames its
   * body and can't trust a single byte after the request line. A higher
   * HTTP/1 minor version is served as HTTP/1.1, as RFC 9110 SS2.5 asks of a
   * recipient implementing the major version. The version grammar itself
   * (digit, dot, digit) is the parser's business, and anything outside it is
   * a 400. */
  if (p->http_major != 1) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
    return 1;
  }

  conn->method = _parse_method(method, method_len);

  /* CHTTP_ANY is a placeholder that only exists at registration time, meaning
   * "match any concrete method", and never the method of a real request.
   * Neither is _CHTTP_METHOD_UNKNOWN, the sentinel _parse_method returns for
   * a syntactically valid method token we don't recognise, like WebDAV's
   * PROPFIND, TRACE and CONNECT, or any custom verb.
   *
   * Without this check such a request would still reach the handler, since a
   * CHTTP_ANY route's method_ok test in _find_route is route->method ==
   * CHTTP_ANY, true whatever the real method is. chttpsvr_req_method() can
   * only report one of the seven named chttp_method_t constants, so it would
   * silently hand the handler a meaningless sentinel, while its own doc
   * comment in chttp.h promises the request's real method.
   *
   * So we reject it here, before parsing the path or the headers and before
   * any route match, because this server implements no method outside the
   * seven it knows. The status is 501, which RFC 7231 SS6.6.2 defines as "the
   * server does not support the functionality required to fulfill the
   * request", and it goes through the same reject_pool machinery as every
   * other rejection (the 404, 405 and 500 cases); see the CHTTP1_USER
   * handling of _conn_pump and _conn_dispatch_reject. */
  if (conn->method == _CHTTP_METHOD_UNKNOWN) {
    conn->req_rejected = true;
    /* The asterisk-form target "*" is a 400 with every method but OPTIONS,
     * methods we don't know included (see the asterisk-form branch below).
     * Go's net/http server answers the same way. */
    conn->reject_status = (target_len == 1 && target[0] == '*')
                              ? CHTTP_STATUS_BAD_REQUEST
                              : CHTTP_STATUS_NOT_IMPLEMENTED;
    return 1;
  }

  /* RFC 9112 SS3.2.4's asterisk-form "*". "OPTIONS *" asks about the server
   * as a whole and the server answers it without a handler, exactly like Go's
   * net/http: 200 with Content-Length 0, and no route, no middleware, no
   * handler (see _options_star_route). Any other method with this target is a
   * 400 and a close, again like Go. */
  if (target_len == 1 && target[0] == '*') {
    if (conn->method != CHTTP_OPTIONS) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
      return 1;
    }
    char *star = (char *)_ccol_mem_alloc(conn->m_procs, 2);
    if (!star) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    star[0] = '*';
    star[1] = '\0';
    conn->path = star;
    conn->options_star = true;
    return 0;
  }

  /* An absolute-form target turns into the origin-form slice inside it (see
   * the doc comment of _strip_absolute_form). was_absolute is what tells the
   * empty-path case below apart from an origin-form target, which can never
   * be empty. */
  const char *eff_target = target;
  size_t eff_target_len = target_len;
  bool was_absolute =
      _strip_absolute_form(target, target_len, &eff_target, &eff_target_len);

  const char *qmark = (const char *)memchr(eff_target, '?', eff_target_len);
  size_t path_raw_len = qmark ? (size_t)(qmark - eff_target) : eff_target_len;

  /* RFC 9112 SS3.2.1: an origin-form target is an absolute-path, and an
   * absolute-path starts with "/". An absolute-form target's path is either
   * empty, which stands for "/" below, or starts with "/" too. Any other
   * shape gets a 400 here, before the route search and before any middleware
   * can read the path.
   *
   * Without this a target like "admin/secret" would reach the "/admin/secret"
   * route, because the segment splitter only drops a leading "/" when there
   * is one, while chttpsvr_req_path() reports "admin/secret". A middleware
   * guarding a prefix such as "/admin" would then wave through a request the
   * guarded handler serves, and "?x" would reach the "/" route with an empty
   * path the same way.
   *
   * The asterisk-form "*" never gets this far; see the branch above. */
  if (path_raw_len > 0 ? eff_target[0] != '/' : !was_absolute) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
    return 1;
  }

  /* Stored RAW, so still percent-encoded; the doc comment of conn->path says
   * why we must not decode it here.
   *
   * An allocation failure here is reported like every other mid-request
   * out-of-memory error in this file: set req_rejected and reject_status and
   * the request goes through reject_pool for a graceful 500. A bare
   * `return 1` with neither field set still aborts the parse with
   * CHTTP1_USER, but _conn_feed_bytes only reads that as an already decided
   * rejection when req_rejected is set, so with the fields unset a temporary
   * allocation failure would silently drop the connection with no response
   * at all. _on_headers_complete handles the same failure gracefully a few
   * callbacks later. */
  /* RFC 7230 SS5.3.1: an empty path component stands for "/". Only an
   * absolute-form target can have one, like "http://host" or
   * "http://host?q=1". An origin-form target always starts with "/" and is
   * never empty here, so this substitution never changes how we read one. */
  bool empty_abs_path = (was_absolute && path_raw_len == 0);
  size_t path_alloc_len = empty_abs_path ? 1 : path_raw_len;
  char *path_raw = (char *)_ccol_mem_alloc(conn->m_procs, path_alloc_len + 1);
  if (!path_raw) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return 1;
  }
  if (empty_abs_path)
    path_raw[0] = '/';
  else
    memcpy(path_raw, eff_target, path_raw_len);
  path_raw[path_alloc_len] = '\0';
  conn->path = path_raw;

  if (qmark) {
    size_t qlen = eff_target_len - path_raw_len - 1;
    conn->raw_query = (char *)_ccol_mem_alloc(conn->m_procs, qlen + 1);
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

/* Fires only for the request's header block. chttp1_parser sends a chunked
 * body's trailer fields to settings->on_trailer instead, which
 * _init_parser_settings leaves NULL on purpose, so nothing this function
 * appends to conn->hdr_names and conn->hdr_values can come from after the
 * body.
 *
 * That's what keeps chttpsvr_req_header's answer stable. It scans the array
 * backwards so a repeated header name resolves to its last occurrence, and a
 * trailer appended here would beat the real header of the same name, letting
 * a client override a header an upstream proxy set on a request we already
 * routed. */
static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  /* Every out-of-memory return below sets req_rejected and reject_status
   * first, just like _on_request_line right above handles an allocation
   * failure (its comment has the reason). A bare `return 1` with neither
   * field set would silently drop the connection without a response, while
   * every other mid-request allocation failure in this file gives a graceful
   * 500. */
  if (conn->hdr_count >= conn->hdr_cap) {
    /* Overflow-safe growth through the shared helper that _servers_register
     * and _chttpsvr_router_shell_register also use; chttpsvr_subrouter,
     * _router_add_route and _parse_qparams write the same overflow-safe
     * pattern inline with their own cap formula instead. They all guard
     * against the same thing: a plain "cap * 2" can wrap on an extreme cap
     * and then silently allocate less than new_cap * sizeof(char *) below.
     *
     * Nothing actually gets there, since conn->hdr_count is bounded by
     * chttp1_parser's header-count cap, at most 100 by default, and
     * chttpsvr_config_t has no separate knob for it. We keep the file's usual
     * idiom anyway instead of relying on that bound never changing. */
    size_t new_cap = _doubling_growth_cap(conn->hdr_cap, sizeof(char *), 8);
    if (new_cap == 0) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    char **nn = (char **)_ccol_mem_realloc(conn->m_procs, conn->hdr_names,
                                           new_cap * sizeof(char *));
    if (!nn) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_names = nn;
    char **nv = (char **)_ccol_mem_realloc(conn->m_procs, conn->hdr_values,
                                           new_cap * sizeof(char *));
    if (!nv) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_values = nv;
    conn->hdr_cap = new_cap;
  }
  char *n = (char *)_ccol_mem_alloc(conn->m_procs, name_len + 1);
  char *v = (char *)_ccol_mem_alloc(conn->m_procs, value_len + 1);
  if (!n || !v) {
    _ccol_mem_free(conn->m_procs, n);
    _ccol_mem_free(conn->m_procs, v);
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

  /* The parser only decodes chunked framing and no other transfer coding, so
   * a request with "Transfer-Encoding: gzip, chunked" would hand the handler a
   * body that's still gzip-encoded with no way for it to find out. RFC 9112
   * SS6.1 has a server that doesn't understand a transfer coding answer 501,
   * and we refuse the request before any route, middleware or handler sees
   * it. The rejection closes the connection since the body never gets read. */
  if (chttp1_has_other_transfer_coding(p)) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_NOT_IMPLEMENTED;
    return -1;
  }

  struct chttpserver *srv = conn->srv;
  if (conn->options_star) {
    /* No route search and no middleware snapshot, dispatch stays empty. The
     * request still diverts to a worker like any other, so its body, its
     * keep-alive decision and a pipelined next request all follow the one
     * code path every request takes. */
    conn->matched_router = srv->root_router;
    conn->matched_route = &_options_star_route;
    conn->expects_continue = chttp1_expects_continue(p);
    conn->divert_gate = 0;
    return CHTTP1_HEADERS_DIVERT_BODY;
  }
  _ccol_rw_lock_rdlock(srv->routes_lock);
  match_result_t mr = _find_route(srv, conn->path, conn->method);
  if (mr.result == ROUTE_MATCH_NONE || mr.result == ROUTE_MATCH_METHOD) {
    /* The chain of the sub-router that owns the path runs for its 404 or 405
     * exactly as it would for one of its routes; see the ownership comment of
     * _find_route. */
    bool chain_ok = _snapshot_chain(srv, mr.router, &conn->dispatch);
    _ccol_rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    if (!chain_ok) {
      conn->dispatch.mw_count = 0;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return -1;
    }
    if (mr.result == ROUTE_MATCH_NONE) {
      conn->reject_status = CHTTP_STATUS_NOT_FOUND;
    } else {
      conn->reject_status = CHTTP_STATUS_METHOD_NOT_ALLOWED;
      conn->allowed_methods = mr.allowed_methods;
    }
    return -1;
  }
  if (mr.result == ROUTE_MATCH_OOM) {
    _ccol_rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }

  dispatch_ctx_t *dispatch = &conn->dispatch;
  if (!_snapshot_chain(srv, mr.router, dispatch)) {
    _ccol_rw_lock_unlock(srv->routes_lock);
    dispatch->mw_count = 0;
    _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }
  dispatch->route = mr.route;
  _ccol_rw_lock_unlock(srv->routes_lock);

  /* For a buffered route we reject before diverting to a worker
   * thread, when the declared Content-Length already exceeds max_body_size,
   * instead of waiting for that many bytes to actually stream in (see the
   * check in _on_body). Otherwise a peer that declares an oversized
   * Content-Length and never sends the body ties up a worker thread until a
   * read timeout fires, because a limit that counts bytes can't trigger when
   * the bytes never come. See the doc comment of
   * chttp1_declared_content_length.
   *
   * Streaming routes are left out on purpose. This module's contract for
   * them, in the doc comment of chttpsvr_config_t.max_body_size, says the
   * handler is ALWAYS called and decides its own response with
   * chttpsvr_req_read() and chttpsvr_req_stream_error(), so rejecting
   * automatically here would quietly break that contract.
   *
   * A chunked body gets the equivalent protection against an oversized CHUNK,
   * applied the same way to both kinds of route through
   * max_chunk_size_override (see _conn_reset_for_request). That one has no
   * "the handler always runs" contract to keep, since it comes out of
   * chttp1_parser_execute()'s ordinary error path, which _drain_body and
   * chttpsvr_req_read already handle like any other malformed body. */
  /* See the comment of the divert_gate field. A buffered Content-Length body
   * must reserve its first buffer before a worker reads it, and a
   * streaming route runs on the streaming pool. */
  size_t divert_gate = 0;
  if (mr.route->is_streaming) {
    if (atomic_load(&srv->stream_threads) >= 0) divert_gate = SIZE_MAX;
  } else if (chttp1_has_content_length(p)) {
    size_t limit = atomic_load(&srv->max_body_size);
    /* A limit of 0 is max_body_size's documented "no cap" sentinel (see that
     * field's doc comment in chttpserver.h), not "cap at zero". A bare
     * `declared > limit` would reject every request with any body at all
     * when the caller asked for no limit this way. max_connections and
     * max_header_bytes use the same convention in the same config struct. */
    uint64_t declared = chttp1_declared_content_length(p);
    if (limit && declared > (uint64_t)limit) {
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      /* The effective chain from the snapshot just above (global plus the
       * matched router's) still runs for this rejection, so a middleware that
       * authenticates or rate-limits the mount sees the request. We clear the
       * route itself because a rejected request must never reach a handler,
       * and that's also what makes the end of the chain the 413 response. */
      conn->dispatch.route = NULL;
      conn->dispatch.mw_idx = 0;
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_PAYLOAD_TOO_LARGE;
      return -1;
    }
    if (declared && atomic_load(&srv->mem_cap) != SIZE_MAX)
      divert_gate =
          declared < (uint64_t)(SIZE_MAX - 1) ? (size_t)declared : SIZE_MAX - 1;
  }

  /* A successful match has already shown that every segment of conn->path
   * (RAW, so still percent-encoded) decodes cleanly. _match_route_cached and
   * _prefix_matches both treat any decode failure as a non-match, so such a
   * request takes the ROUTE_MATCH_NONE or ROUTE_MATCH_METHOD branch above and
   * never gets here. Decoding the whole path here for the public
   * chttpsvr_req_path() accessor can therefore only fail on a real allocation
   * failure, never on malformed input.
   *
   * We reject that failure with a 500 here, before moving mr.param_values
   * into conn, like every other allocation-failure check in this function
   * (the route-match failure and the middleware-snapshot overflow above). A
   * request must not go to a worker with conn->decoded_path NULL, since
   * chttpsvr_req_path()'s doc comment promises a pointer valid for the
   * request's lifetime for any non-NULL req, with no NULL case other than a
   * NULL req. A handler that strlen()s that pointer without checking is being
   * perfectly reasonable, and it would crash. */
  size_t plen = strlen(conn->path);
  char *dp = (char *)_ccol_mem_alloc(srv->m_procs, plen + 1);
  if (dp) {
    ssize_t dlen = _decode_path_unsafe(dp, conn->path);
    if (dlen >= 0) {
      dp[(size_t)dlen] = '\0';
    } else {
      _ccol_mem_free(srv->m_procs, dp);
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
  conn->divert_gate = divert_gate;

  /* We always divert, handing every matched route to the worker pool whether
   * it's buffered or streaming and whatever the body size, as this module's
   * documented threading model says. chttp1_parser itself turns this into an
   * ordinary immediate completion when there's no body to divert (see the doc
   * comment of CHTTP1_HEADERS_DIVERT_BODY). Either way the header-read
   * callback below submits to the worker pool once execute() returns
   * CHTTP1_HEADERS_ONLY or CHTTP1_PAUSED. */
  return CHTTP1_HEADERS_DIVERT_BODY;
}

/* The initial buffer capacity of a Content-Length body: the 8 KiB that can
 * come with the headers plus one 8 KiB _drain_body read window. It's also what
 * such a body reserves before a worker reads it (see _body_first_reserve). */
#define _CHTTPSVR_BODY_FIRST_CAP ((size_t)16384)

/* The capacity conn's body buffer grows to from cap when it needs to hold
 * min_cap bytes. limit is the max_body_size that applies to the request (0
 * for none), and min_cap never goes above it.
 *
 * A buffered Content-Length body that arrived whole with its headers while
 * body memory is limited gets its declared length in one allocation; it holds
 * no reservation and is bounded by the read that brought it. Every other such
 * body doubles from _CHTTPSVR_BODY_FIRST_CAP as its bytes come in, never past
 * its declared length, and reserves each growth before the read that needs
 * it, so a client that declares a big body and sends little of it holds
 * little memory.
 *
 * Any other body doubles from 8 KiB, never past limit. A buffered body
 * reserves the result before the read that needs it; see
 * _drain_body_socket_read, which calls this with the same rule.
 *
 * Overflow-safe doubling like in _head_builder_grow: with limit close to
 * SIZE_MAX a plain "keep doubling" loop could wrap to 0 and never reach
 * min_cap. */
static size_t _body_grow_target(const chttpsvr_conn_t *conn, size_t cap,
                                size_t min_cap, size_t limit) {
  if (min_cap <= cap) return cap;
  size_t ceiling = limit;
  size_t start = 8192;
  if (!conn->matched_route->is_streaming && !conn->options_star &&
      chttp1_has_content_length(&conn->parser)) {
    /* The parser counts down the body bytes it hasn't handed over yet, so the
     * declared length is what the buffer holds plus that count. */
    uint64_t rest = chttp1_declared_content_length(&conn->parser);
    size_t held = conn->body.len;
    size_t d =
        rest < (uint64_t)(SIZE_MAX - held) ? held + (size_t)rest : SIZE_MAX;
    if (d >= min_cap) {
      if (conn->mem_charged == 0 && !conn->mem_exempt &&
          atomic_load(&conn->srv->mem_cap) != SIZE_MAX)
        return d;
      if (!ceiling || d < ceiling) ceiling = d;
      start = _CHTTPSVR_BODY_FIRST_CAP;
    }
  }
  size_t new_cap = cap ? cap : start;
  while (new_cap < min_cap) {
    if (new_cap > SIZE_MAX / 2) {
      new_cap = min_cap;
      break;
    }
    new_cap *= 2;
  }
  if (ceiling && new_cap > ceiling && min_cap <= ceiling) new_cap = ceiling;
  return new_cap;
}

static void _mem_trim(chttpsvr_conn_t *conn, size_t keep);
#ifdef RUNNING_UNIT_TESTS
static void _body_growth_note_for_tests(const chttpsvr_conn_t *conn,
                                        size_t old_cap, size_t new_cap);
#endif

/* One callback and one growbuf_t for two jobs: collecting a buffered route's
 * body, and holding a streaming route's bytes that chttpsvr_req_read hasn't
 * taken yet. We enforce max_body_size here the same way for both, since
 * chttp1_parser knows nothing about that limit and leaves it to the caller. */
static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  /* Overflow-safe accumulation, matching the guard on growbuf growth a few
   * lines below. On a 32-bit (ILP32) build a streaming route can receive way
   * over 4 GiB of body in one request, because its growbuf compacts back to
   * empty as chttpsvr_req_read drains it and the connection never holds more
   * than one batch. Without the guard body_bytes_seen (a size_t) would
   * silently wrap back near zero past that point, the max_body_size check
   * would start passing again, and the size cap would do nothing for the rest
   * of the request. We treat it as an ordinary over-limit body instead of
   * wrapping. */
  if (len > SIZE_MAX - conn->body_bytes_seen) {
    conn->body_too_large = true;
    return 1;
  }
  conn->body_bytes_seen += len;
  size_t body_limit = conn->options_star
                          ? _CHTTPSVR_OPTIONS_STAR_MAX_BODY
                          : atomic_load(&conn->srv->max_body_size);
  /* A body_limit of 0 is the documented "no cap" sentinel, the same special
   * case as in _on_headers_complete's Content-Length pre-check above (see
   * that comment). */
  if (body_limit && conn->body_bytes_seen > body_limit) {
    conn->body_too_large = true;
    return 1;
  }
  growbuf_t *b = &conn->body;
  /* A buffered route never drains through pos since chttpsvr_req_read isn't
   * used for one, so its pos stays 0 and this is never true for it; there's
   * nothing to compact then. */
  if (conn->matched_route->is_streaming && b->pos == b->len && b->pos > 0)
    b->pos = b->len = 0; /* compact, an earlier req_read drained it */
  /* We compute min_cap and check it for wraparound BEFORE comparing it with
   * b->cap. Comparing the unguarded sum "b->len + len > b->cap" first, with
   * the overflow check only INSIDE that branch, isn't the same thing: if the
   * unguarded sum wraps around SIZE_MAX the small wrapped result can satisfy
   * "<= b->cap", so we'd skip the growth branch and its overflow check
   * entirely and fall into the memcpy below with b->len still at its huge
   * pre-wrap value, a heap buffer overflow.
   *
   * It's the same correct construction as in _head_builder_grow: compute
   * min_cap, then catch the wraparound with "min_cap < b->len" before using
   * the value for anything else. To get here b->len must be within `len` of
   * SIZE_MAX, impossible on a 64-bit build where b->cap would already need
   * about 2^64 bytes, but a real if narrow concern on ILP32, where SIZE_MAX is
   * about 4.29 GiB. It takes a max_body_size configured close to SIZE_MAX for
   * a streaming route whose handler drains slower than the peer sends. */
  size_t min_cap = b->len + len;
  if (min_cap < b->len) {
    /* A size_t overflow: a body this big can't exist at all on this
     * platform. It's the same "too large to hold" outcome the
     * body_bytes_seen overflow guard above reports this way, not a temporary
     * allocation failure. */
    conn->body_too_large = true;
    return 1;
  }
  if (min_cap > b->cap) {
    size_t old_cap = b->cap;
    size_t new_cap = _body_grow_target(conn, old_cap, min_cap, body_limit);
    char *nb = (char *)_ccol_mem_realloc(conn->m_procs, b->buf, new_cap);
    if (!nb) {
      /* A real allocation failure, unlike every other reason this function
       * returns 1. The comment of the body_alloc_failed field says why it
       * must not be lumped in with the general transfer-aborted cases. */
      conn->body_alloc_failed = true;
      return 1;
    }
    b->buf = nb;
    b->cap = new_cap;
#ifdef RUNNING_UNIT_TESTS
    _body_growth_note_for_tests(conn, old_cap, new_cap);
#endif
    /* The old block is gone, so the body only keeps the reservation its new
     * buffer needs; see _drain_body_socket_read. */
    if (__builtin_expect(conn->mem_charged > new_cap, 0))
      _mem_trim(conn, new_cap);
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
  _ccol_once_flag_t once;
} srv_parser_bundler = {0};

static void _init_parser_settings(void) {
  chttp1_settings_init(&srv_parser_bundler.settings);
  srv_parser_bundler.settings.on_request_line = _on_request_line;
  srv_parser_bundler.settings.on_header = _on_header;
  /* settings.on_trailer stays NULL, as chttp1_settings_init zeroed it, since
   * this server has no trailer API. Leaving the callback unset is what makes
   * it structurally impossible for a trailer field to reach the request's
   * header array; see the comment of _on_header. */
  srv_parser_bundler.settings.on_headers_complete = _on_headers_complete;
  srv_parser_bundler.settings.on_body = _on_body;
  srv_parser_bundler.settings.on_message_complete = _on_message_complete;
}

/* ========================================================================== */
/*                    REACTOR-THREAD READ/HANDSHAKE CALLBACKS                 */
/* ========================================================================== */

/* The pools of a server, for _srv_submit. */
typedef enum {
  _SRV_POOL_WORKER = 0,
  _SRV_POOL_REJECT = 1,
  _SRV_POOL_STREAM = 2
} _srv_pool_kind_t;

static ctpool _stream_pool_get(struct chttpserver *srv);

/* Starts a new streaming pool run. The caller holds srv->mutex and has just
 * taken stream_pool out of service. The token task count starts from zero
 * again for the next pool, and stream_gen tells every task of the earlier run
 * to stop taking requests. Both change under wait_mutex, which every reader
 * of sq_tokens holds. */
static void _stream_new_run_locked(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->wait_mutex);
  srv->sq_tokens = 0;
  srv->sq_unclaimed = 0;
  atomic_fetch_add(&srv->stream_gen, 1);
  _ccol_mutex_unlock(srv->wait_mutex);
}

/* srv's current pool of that kind. The streaming pool is created on demand,
 * the same as for any streaming request. */
static ctpool _srv_current_pool(struct chttpserver *srv, _srv_pool_kind_t k) {
  if (k == _SRV_POOL_STREAM) return _stream_pool_get(srv);
  _ccol_mutex_lock(srv->mutex);
  ctpool p = k == _SRV_POOL_WORKER ? srv->worker_pool : srv->reject_pool;
  _ccol_mutex_unlock(srv->mutex);
  return p;
}

#ifdef RUNNING_UNIT_TESTS
/* Holds the nth worker submit of _conn_start_diverted from now, after it has
 * read the pool and before it submits, until the test lets it go, so a test
 * can restart the server inside that window. retried counts the submits
 * _srv_submit_retry moved to a newer pool. */
static struct {
  _Atomic int countdown;
  _Atomic bool entered;
  _Atomic bool go;
  _Atomic size_t retried;
} g_divert_submit_hook;
void _chttpsvr_arm_divert_submit_hook_for_tests(int nth) {
  atomic_store(&g_divert_submit_hook.entered, false);
  atomic_store(&g_divert_submit_hook.go, false);
  atomic_store(&g_divert_submit_hook.countdown, nth);
}
size_t _chttpsvr_submit_retried_for_tests(void) {
  return atomic_load(&g_divert_submit_hook.retried);
}
bool _chttpsvr_divert_submit_hook_entered_for_tests(void) {
  return atomic_load(&g_divert_submit_hook.entered);
}
void _chttpsvr_release_divert_submit_hook_for_tests(void) {
  atomic_store(&g_divert_submit_hook.go, true);
}
static void _divert_submit_hook_wait_if_armed(void) {
  int left = atomic_load(&g_divert_submit_hook.countdown);
  while (left > 0 && !atomic_compare_exchange_weak(
                         &g_divert_submit_hook.countdown, &left, left - 1)) {
  }
  if (left != 1) return;
  atomic_store(&g_divert_submit_hook.entered, true);
  for (int i = 0; i < 20000 && !atomic_load(&g_divert_submit_hook.go); i++) {
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
  }
}
#endif /* RUNNING_UNIT_TESTS */

/* The cold half of _srv_submit, for when the first pool refused the task. If
 * a restart replaced that pool after the caller read it, the refusal comes
 * from a retired, draining pool, and the task goes to srv's current pool, so
 * a request arriving during a restart is never refused because of the
 * restart. A refusal from the current pool (a full queue) is the final
 * answer. */
static __attribute__((noinline, cold)) bool _srv_submit_retry(
    struct chttpserver *srv, _srv_pool_kind_t k, ctpool pool,
    void (*fn)(void *), void *arg) {
  for (;;) {
    ctpool cur = _srv_current_pool(srv, k);
    if (!cur || cur == pool) return false;
    pool = cur;
    if (ctpool_try_submit(pool, fn, arg, NULL) == ccol_success) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_divert_submit_hook.retried, 1);
#endif /* RUNNING_UNIT_TESTS */
      return true;
    }
  }
}

/* Submits fn(arg) to pool, which the caller read from srv under srv->mutex,
 * and retries on the current pool of the same kind if a restart retired pool
 * in the meantime (see _srv_submit_retry). */
static inline __attribute__((always_inline)) bool _srv_submit(
    struct chttpserver *srv, _srv_pool_kind_t k, ctpool pool,
    void (*fn)(void *), void *arg) {
  if (__builtin_expect(
          pool && ctpool_try_submit(pool, fn, arg, NULL) == ccol_success, 1))
    return true;
  return _srv_submit_retry(srv, k, pool, fn, arg);
}

/* Decrements srv->in_flight_requests and, when it reaches zero, wakes every
 * thread waiting in _drain_and_close_all_connections. Every path that
 * increments in_flight_requests (that's _conn_start_diverted) must call this
 * exactly once.
 *
 * When matters: only once the connection is in a state
 * _drain_and_close_all_connections can observe, which is either fully closed
 * with _conn_close (a close through _conn_reject_and_close included) or
 * safely published back into the idle list with _idle_list_add.
 *
 * Calling it any earlier opens a real window, e.g. before a keep-alive
 * connection's registration is resumed or re-added and the connection
 * idle-listed, or before a rejected connection's courtesy response is written
 * and the connection closed. In that window the waiter can wake, see
 * in_flight_requests == 0 and let __chttpsvr_destroy free srv while another
 * thread is still finishing this connection, a real use-after-free that
 * ThreadSanitizer reports over tests_tls. */
static void _release_in_flight(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0)
    _ccol_cond_var_broadcast(srv->requests_done_cv);
  _ccol_mutex_unlock(srv->mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook. While it's armed every _conn_pause_reg pause takes the
 * failed-pause path, and the counter counts each removal that path makes. */
static _Atomic bool g_force_pause_fail_for_tests = false;
static _Atomic size_t g_pause_fallback_count_for_tests = 0;
void _chttpsvr_force_pause_fail_for_tests(bool force) {
  atomic_store(&g_force_pause_fail_for_tests, force);
}
size_t _chttpsvr_pause_fallback_count_for_tests(void) {
  return atomic_load(&g_pause_fallback_count_for_tests);
}
#endif

/* Pauses conn's registration while a thread owns the connection, so no
 * readiness fires in the meantime. The caller owns conn. If the pause fails
 * we fall back to removing the registration, and the connection's next wait
 * adds a registration of its own (see _conn_park_arm and _task_tail).
 * Removing a registration that's already gone is a documented no-op, and
 * removing one that's still live is what reclaims it. */
static void _conn_pause_reg(chttpsvr_conn_t *conn) {
  if (!conn->reg) return;
  bool paused;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_pause_fail_for_tests))
    paused = false;
  else
#endif
    paused = ccol_event_loop_pause(srv_engine_bundler.reactor, conn->reg) ==
             ccol_success;
  if (!paused) {
    ccol_event_loop_remove(srv_engine_bundler.reactor, conn->reg);
    conn->reg = CCOL_EVENT_REG_INVALID;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_pause_fallback_count_for_tests, 1);
#endif
  }
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hook. When armed, the next rejection skips reject_pool and
 * takes _conn_reject_via_pool's inline answer, just as when that pool is
 * full, on the rejecting thread (the reactor thread for an unmatched route).
 * Disarms itself after one use. */
static _Atomic bool g_force_reject_inline_for_tests = false;
void _chttpsvr_force_reject_inline_for_tests(bool force) {
  atomic_store(&g_force_reject_inline_for_tests, force);
}
#endif

/* Submits conn to reject_pool for its courtesy rejection response, after the
 * caller has set conn->reject_status, so the rejection's middleware never
 * runs on the calling thread. It falls back to answering inline on the
 * calling thread, without middleware, in two cases: reject_pool isn't
 * available because the server is tearing down, or that pool's bounded queue
 * (_CHTTPSVR_REJECT_POOL_QUEUE_CAP) is full.
 *
 * Both rejection paths in this file share it. One is the pool-full 503 in
 * _conn_start_diverted, which has already incremented in_flight_requests and
 * paused or removed conn->reg itself, because at that point it didn't know
 * yet whether to keep the connection alive or reject it. The other is the
 * reactor thread's synchronous 404, 405 and 500, through
 * _conn_dispatch_reject below.
 *
 * Every caller must already have incremented srv->in_flight_requests for this
 * connection. We only release that count with _release_in_flight once the
 * close has really completed, not as soon as the task is queued; the comment
 * of _release_in_flight explains why an earlier release is a real
 * use-after-free. */
static void _conn_reject_via_pool(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->mutex);
  ctpool reject_pool = srv->reject_pool;
  _ccol_mutex_unlock(srv->mutex);

  bool inline_only = false;
#ifdef RUNNING_UNIT_TESTS
  inline_only = atomic_exchange(&g_force_reject_inline_for_tests, false);
#endif
  if (!inline_only &&
      _srv_submit(srv, _SRV_POOL_REJECT, reject_pool, _reject_task, conn)) {
    return;
  }

  /* Last-resort fallback, for when reject_pool isn't available because the
   * server is tearing down, its bounded queue is full, or the submit failed
   * for some other reason like an allocation failure. We answer on the
   * calling thread without middleware. The write never waits on the peer,
   * same as at every other reject-and-close site, so it's safe on the reactor
   * thread and on the sweep. srv was read above, before
   * _conn_reject_and_close, which may free or park conn. */
  _conn_reject_and_close(conn, /*run_middleware=*/false);
  _release_in_flight(srv);
}

/* Routes a rejected connection through reject_pool, for a 404, 405 or 500
 * decided by the synchronous header parse on the reactor thread after
 * _on_headers_complete has set conn->req_rejected and conn->reject_status.
 * It's the same route the pool-full 503 below takes, so a slow-reading client
 * that we have to tell about a bad route can't stall the single reactor
 * thread either.
 *
 * The registration is paused, like in _conn_start_diverted, not removed. A
 * rejection never keeps the connection alive, but the connection can still
 * wait for the reactor: a courtesy response that hits a full socket parks,
 * and a lingering close waits for input. Both re-arm this same registration
 * with a modify and a resume, which allocate nothing and can't fail for lack
 * of memory, whereas after a removal that wait would have to allocate a new
 * registration and handle a failed add. A failed pause falls back to a
 * removal as in _conn_start_diverted, and the later wait adds its own
 * registration; a registration added for a descriptor after its earlier one
 * was removed is watched like any other (see ccol_event_loop_remove).
 *
 * conn->state becomes CONN_ST_DIVERTED and conn goes into srv->diverted_head
 * and srv->diverted_tail through _diverted_list_add, just like
 * _conn_start_diverted does for a matched-route request. The rejection's
 * courtesy response, written by _conn_reject_via_pool, runs on a reject_pool
 * thread, or on the calling thread itself in the fallback case, and never
 * waits on the peer. The connection stays in the registry while that thread
 * owns it, so a teardown can still find it like any other thread-owned
 * connection.
 *
 * Without the registration _force_unblock_diverted_connections couldn't see a
 * connection rejected here. _wait_in_flight_bounded calls that function, and a
 * chttpsvr_stop() plus chttpsvr_start() restart calls
 * _wait_in_flight_bounded, as do the teardowns of chttpsvr_destroy() and
 * chttpsvr_engine_stop(). A graceful shutdown or restart would then have no
 * way to interrupt an ordinary rejected request (404, 405, 413, 500 or 501)
 * parked on a slow reader. It could only sit out that write's duration bound
 * inside ctpool_shutdown_drain(reject_pool), which has no timeout of its own,
 * once for every rejection still in the pool's queue.
 *
 * How long each of those waits is depends on the operator through
 * max_response_write_duration_ms, which may be set to 0 to turn it off. So
 * the registration here doesn't depend on that setting, because the registry
 * is what the forced-unblock mechanism searches.
 *
 * _conn_free always calls _diverted_list_remove, and every close path,
 * _conn_reject_and_close included, gets to _conn_free through _conn_close,
 * so nothing more needs cleaning up here. */
static void _conn_dispatch_reject(chttpsvr_conn_t *conn) {
  _idle_list_remove(conn);
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
  _ccol_mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  _ccol_mutex_unlock(conn->srv->mutex);
  _conn_reject_via_pool(conn);
}

static void _conn_start_diverted(chttpsvr_conn_t *conn, const char *leftover,
                                 size_t leftover_len) {
  _idle_list_remove(conn);
  /* Pause the registration instead of removing it. Resuming with
   * ccol_event_loop_resume once the worker is done with this request and the
   * connection waits for the next one is cheap, so pausing saves a full
   * event_entry allocate and free on every keep-alive request cycle, plus the
   * churn in the fd registry chmap.
   *
   * A pause shouldn't fail in practice: nothing else touches this
   * connection's reg while the reactor still owns it, and every documented
   * ccol_event_loop_pause failure for a reg with a real fd comes down to
   * "another thread removed reg". A failed pause removes the registration
   * (see _conn_pause_reg) and the connection then carries on like one that
   * never had a live registration: its next wait, in _task_tail's keep-alive
   * tail or in _conn_park_arm, adds a fresh registration for the same
   * descriptor instead of resuming a stale reg. */
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  /* Idempotent, so it does nothing for a connection the registry already
   * holds from an earlier divert cycle that hasn't finished, which happens
   * when a further pipelined request gets diverted. The comment of
   * _diverted_list_remove says where the matching removal happens. */
  _diverted_list_add(conn);

  _ccol_mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ctpool pool = conn->srv->worker_pool;
  bool draining = _srv_draining(conn->srv);
  _ccol_mutex_unlock(conn->srv->mutex);

  if (__builtin_expect(draining, 0)) {
    /* The server is tearing down, so this request gets no handler. It's
     * answered with 503 and Connection: close, like a request that meets a
     * full worker pool, and then the connection closes. Only the headers were
     * read, so there's no body to keep. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_reject_via_pool(conn);
    return;
  }

  /* Copy leftover now, because it points into the reactor's stack read
   * buffer, which goes out of scope the moment this callback returns.
   * _task_worker derives nothing from chttp1_parser_consumed() on its own,
   * since that value only means something against the exact buffer and
   * length of the execute() call that produced it, i.e. the reactor's stack
   * buffer, gone by the time the worker runs. Handing over the copied carry
   * bytes directly avoids the lifetime problem altogether. */
  char *carry = NULL;
  bool carry_alloc_failed = false;
  if (leftover_len > 0) {
    carry = (char *)_ccol_mem_alloc(conn->m_procs, leftover_len);
    if (carry)
      memcpy(carry, leftover, leftover_len);
    else
      carry_alloc_failed = true;
  }
  /* carry and _carry_over_len are published BEFORE the submit, never after,
   * because ctpool_try_submit may hand the task to an idle worker that starts
   * _task_worker(conn) at once, in parallel with the rest of this
   * function. Writing these fields after the submit races that worker's read
   * of conn->_carry_over, which may see NULL, the field's steady value
   * between requests (_task_worker always frees and clears it after using
   * it). The worker would then silently drop this request's real leftover
   * bytes, and by the time we got to the assignment, too late, nothing would
   * ever free that orphaned buffer. That's a data-loss bug and a leak at the
   * same time, and valgrind reports the leak. If you rework this function,
   * keep these two writes in front of the submit.
   *
   * _carry_over_len must agree with whether _carry_over really is non-NULL.
   * Leaving it at leftover_len after the allocation above failed would hand
   * the worker a NULL pointer with a non-zero length, and
   * chttp1_stream_prepare and chttp1_stream_prepare_tls would memcpy()
   * leftover_len bytes FROM it without checking; a memcpy from NULL is not a
   * no-op. */
  conn->_carry_over = carry;
  conn->_carry_over_len = carry ? leftover_len : 0;

  if (carry_alloc_failed) {
    /* The leftover bytes are pipelined request-body bytes we already read
     * off the wire, so they're gone from the socket for good. We can't
     * quietly drop them, since the worker would then read the body from the
     * wrong offset and silently truncate it, and we can't ignore the NULL
     * pointer problem above either, so we reject with a 500 just like the
     * pool-full 503 below. in_flight_requests was already incremented and
     * conn->reg already paused or removed above. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    _conn_reject_via_pool(conn);
    return;
  }

  /* A streaming route goes to the streaming pool, and a buffered body that
   * didn't arrive together with its headers reserves body memory first (see
   * _conn_divert_gate). Every other request just pays one compare here. */
  if (__builtin_expect(conn->divert_gate > leftover_len, 0) &&
      _conn_divert_gate(conn))
    return;

#ifdef RUNNING_UNIT_TESTS
  _divert_submit_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
  if (!_srv_submit(conn->srv, _SRV_POOL_WORKER, pool, _task_worker, conn)) {
    _ccol_mem_free(conn->m_procs, carry);
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    /* The worker pool is full. ctpool_try_submit returns ccol_container_full
     * instead of blocking, in line with this module's documented "never block
     * the reactor thread" contract. It's a real server condition, if a
     * temporary one, and the client should hear about it as a synchronous
     * 503 rather than a bare connection reset. See
     * bounded_pool_full_returns_503 in tests.c. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;

    /* in_flight_requests was incremented and conn->reg paused or removed
     * above, before we knew whether this connection would divert or be
     * rejected. _conn_reject_via_pool takes it from here without submitting
     * to `pool` again, since that's the very pool that just turned the
     * request away for being full. The comment of that function covers the
     * rest. */
    _conn_reject_via_pool(conn);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&_chttpsvr_worker_submits_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
}

/* Fills conn->decoded_path for a request rejected before routing sets that
 * field, so a middleware running for the rejection can still read the target
 * through chttpsvr_req_path(). Both guards below are defensive. Every
 * rejection that runs a middleware chain is decided at headers-complete,
 * after the request line is parsed and before the matched-route path sets
 * decoded_path itself, so in practice conn->path is always set and
 * decoded_path never is.
 *
 * A target with malformed percent-encoding is reported exactly as it came
 * in, still encoded, because the malformed encoding is itself one of the
 * reasons no route could match and there's no decoded form to report. An
 * access log that leaves out the one request most worth recording is worse
 * than one that records exactly what the peer sent.
 *
 * An allocation failure leaves the field NULL, the same answer every other
 * chttpsvr_req_path() caller gets when the decode couldn't be allocated. */
static void _conn_set_rejected_decoded_path(chttpsvr_conn_t *conn) {
  if (conn->decoded_path || !conn->path) return;
  size_t plen = strlen(conn->path);
  char *dp = (char *)_ccol_mem_alloc(conn->m_procs, plen + 1);
  if (!dp) return;
  ssize_t dlen = _decode_path_unsafe(dp, conn->path);
  if (dlen >= 0)
    dp[(size_t)dlen] = '\0';
  else
    memcpy(dp, conn->path, plen + 1);
  conn->decoded_path = dp;
}

/* Writes the Allow header value for an allowed_methods mask into out, which
 * has room for at least 64 bytes. Methods come in the order GET, HEAD, POST,
 * PUT, DELETE, PATCH, OPTIONS, separated by ", ", and the longest value, with
 * all seven, is 44 bytes including the terminator. */
static void _format_allow_header(unsigned mask, char *out) {
  static const chttp_method_t order[] = {
      CHTTP_GET,    CHTTP_HEAD,  CHTTP_POST,   CHTTP_PUT,
      CHTTP_DELETE, CHTTP_PATCH, CHTTP_OPTIONS};
  size_t n = 0;
  for (size_t i = 0; i < sizeof(order) / sizeof(order[0]); i++) {
    if (!(mask & (1u << order[i]))) continue;
    const char *name = chttp_method_str(order[i]);
    size_t len = strlen(name);
    if (n) {
      out[n++] = ',';
      out[n++] = ' ';
    }
    memcpy(out + n, name, len);
    n += len;
  }
  out[n] = '\0';
}

/* run_middleware says whether this rejection may run application middleware
 * before its courtesy response goes out. It's only true from _reject_task,
 * i.e. on one of reject_pool's dedicated threads, and that's what bounds how
 * much application code a flood of rejected requests can start: reject_pool
 * has a fixed thread count and a bounded queue, just like the worker pool a
 * matched route's handler runs on.
 *
 * It's false on _conn_reject_via_pool's last-resort synchronous fallback,
 * which runs on whatever thread called it (the single reactor thread in the
 * default configuration) exactly because reject_pool is saturated or gone.
 * There we send the rejection bare rather than run application code on a
 * thread where it must not block.
 *
 * _reject_status_runs_middleware narrows it further to the statuses for which
 * a chain means anything at all. */
static void _conn_reject_and_close(chttpsvr_conn_t *conn, bool run_middleware) {
  /* conn->resp and not a fresh response on the stack, because no handler ran
   * for this request (a rejection never diverts to the worker pool), so
   * conn->resp is still in its initial state for this request. It's also what
   * a middleware writes into if one runs below, and _conn_free destroys
   * it. */
  chttpsvr_resp *resp = &conn->resp;
  resp->status_code = conn->reject_status;
  /* Every 503 we send means "the server is saturated right now", so it tells
   * the client when a retry makes sense. If allocating the header fails it's
   * just left out. */
  if (conn->reject_status == CHTTP_STATUS_SERVICE_UNAVAILABLE)
    (void)chttpsvr_resp_set_header(resp, "retry-after",
                                   _CHTTPSVR_RETRY_AFTER_SECONDS);
  /* A 405 lists the methods the target resource does support (RFC 9110
   * SS15.5.6). If allocating the header fails it's just left out. */
  if (conn->reject_status == CHTTP_STATUS_METHOD_NOT_ALLOWED &&
      conn->allowed_methods) {
    char allow[64];
    _format_allow_header(conn->allowed_methods, allow);
    (void)chttpsvr_resp_set_header(resp, "allow", allow);
  }
  bool wants_middleware = run_middleware && conn->dispatch.mw_count > 0 &&
                          _reject_status_runs_middleware(conn->reject_status);
  if (wants_middleware) _conn_set_rejected_decoded_path(conn);
  /* conn->decoded_path comes last as a memory-pressure check, not a policy
   * check. chttpsvr_req_path() is documented to hand a middleware a readable
   * path, and the decode above is the only thing that can fail to produce
   * one, in which case a middleware that logs or matches on the path would
   * dereference NULL. So a rejection whose path we couldn't decode goes out
   * bare, the same as when reject_pool is saturated. The matched-route path
   * answers the same failure with a 500 for the same reason. */
  if (wants_middleware && conn->decoded_path) {
    conn->dispatch.route = NULL;
    conn->dispatch.mw_idx = 0;
    chttpsvr_req req;
    memset(&req, 0, sizeof(req));
    req.conn = conn;
    /* No stream and no route here. We never read this request's body, since
     * the rejection is decided at headers-complete before a single body byte
     * is consumed, and there's no handler either. So chttpsvr_req_read()
     * reports -1 and chttpsvr_req_param() reports NULL, each through the
     * guard it already has for a req without a matched route. Method, path,
     * headers and query string all stay readable, which is what an access log
     * or a rate limiter needs. */
    req.stream = NULL;
    req.param_names = NULL;
    req.m_procs = conn->m_procs;
    _chttpsvr_next(&req, resp);
    _destroy_req_scratch(&req);
  }
  chttp1_stream_t stream;
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                              conn->m_procs);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  /* Every connection of this server is non-blocking (see accept4()). The
   * response never waits for a client that doesn't read: a full socket parks
   * the connection with the unsent rest, on no thread, and the reactor
   * resumes it once the socket takes bytes again (see _task_continue_write).
   * So whatever thread runs this, the reactor thread, the sweep or a pool
   * thread, it never waits on the peer.
   *
   * conn switches on the write's total-duration bound set by
   * max_response_write_duration_ms, and is_reject adds
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS on top as a hard ceiling. A caller
   * who set max_response_write_duration_ms to 0 (off) still gets a bounded
   * rejection, whether it goes out immediately or waits parked, since the
   * sweep and the resumed write judge the same deadline.
   * conn->write_deadline_set is always false when we get here, because
   * _conn_reset_for_request clears it before any header parse and so before
   * any rejection decision. */
  stream.fd_nonblocking = true;
  stream.write_nonblocking = true;
  bool sent =
      _send_response(&stream, resp, false, 0, conn->method == CHTTP_HEAD, conn,
                     /*is_reject=*/true);
  chttp1_stream_release(&stream);
  if (!sent && conn->wp_active) {
    /* The socket is full. Nothing of the request gets read again, so its body
     * memory goes back now rather than when the write finishes. */
    conn->wp_reject = true;
    _conn_drop_request_state(conn);
    _diverted_list_remove(conn);
    if (!_conn_park_arm(conn, _CONN_PARK_WRITE)) {
      conn->wp_reject = false;
      _conn_end_parked_write(conn);
      _conn_close(conn);
    }
    return;
  }
  /* The request is refused, so its body, if any, stays unread, and closing
   * with unread bytes makes the kernel send a reset that can wipe out the
   * response in the client before the client has read it. */
  if (sent && _conn_request_bytes_may_remain(conn))
    _conn_linger(conn);
  else
    _conn_close(conn);
}

#ifdef RUNNING_UNIT_TESTS
/* Purely white-box test instrumentation. It counts how many rejections really
 * ran on a reject_pool thread; _conn_reject_via_pool's synchronous fallback
 * never reaches _reject_task, so it adds nothing. The counter is process-wide
 * rather than per server, the convention
 * _chttpsvr_engine_num_reactor_threads_for_tests already set, so a test reads
 * the difference across its own window instead of an absolute value, since
 * other tests in the same process cause rejections too. The guard keeps the
 * symbol and the counter out of a production build completely. */
static _Atomic size_t g_reject_task_run_count_for_tests = 0;

/* Purely a white-box test hook. It makes _task_worker's first
 * chttp1_stream_prepare() or chttp1_stream_prepare_tls() call count as
 * failed, exercising the same body_unavailable fallback a real allocation
 * failure takes (see _task_worker's comment about body_unavailable).
 *
 * A test can't reproduce a real failure there reliably, because that call's
 * carry-over copy has the same size as the conn->_carry_over allocation
 * _conn_start_diverted makes before it from the same allocator, so a custom
 * allocator that fails at a given size fails that earlier allocation first
 * and never gets here; see
 * carry_over_alloc_failure_rejects_gracefully_instead_of_crashing in tests.c.
 * The guard keeps this symbol out of a production build completely. */
static _Atomic bool g_force_stream_prepare_fail_for_tests = false;
void _chttpsvr_force_stream_prepare_fail_for_tests(bool force) {
  atomic_store(&g_force_stream_prepare_fail_for_tests, force);
}

/* Purely a white-box test hook. Once armed, chttpsvr_start() blocks at one
 * fixed point, right after it has confirmed or acquired the shared engine
 * reference and registered itself with servers_bundler, and before any more
 * reactor work (_idle_sweep_start_if_needed, _make_listen_socket and the
 * listener's ccol_event_loop_add). It signals that it got there and then
 * waits to be released.
 *
 * That way a test can land a concurrent chttpsvr_engine_stop() every time
 * inside the exact window that this server's early _servers_register call
 * closes (see the comment at that call site), which _quiesce_server_once's
 * pending_resolve_count wait closes too. Without the hook a test could only
 * sleep for a fixed time and hope to hit a race window a few instructions
 * wide.
 *
 * It fires once per arm call, and the guard keeps all of it out of a
 * production build. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_race_hook = {0};

static void _start_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_start_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_start_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_race_hook_for_tests(void) {
  _ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  _ccol_mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.armed = true;
  g_start_race_hook.entered = false;
  g_start_race_hook.go = false;
  _ccol_mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_wait_start_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  _ccol_mutex_lock(g_start_race_hook.mutex);
  while (!g_start_race_hook.entered)
    _ccol_cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  _ccol_mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_release_start_race_hook_for_tests(void) {
  _ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  _ccol_mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.go = true;
  _ccol_cond_var_broadcast(g_start_race_hook.cv);
  _ccol_mutex_unlock(g_start_race_hook.mutex);
}

static void _start_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  _ccol_mutex_lock(g_start_race_hook.mutex);
  if (!g_start_race_hook.armed) {
    _ccol_mutex_unlock(g_start_race_hook.mutex);
    return;
  }
  g_start_race_hook.armed = false; /* one-shot */
  g_start_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_start_race_hook.cv);
  while (!g_start_race_hook.go)
    _ccol_cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  _ccol_mutex_unlock(g_start_race_hook.mutex);
}

/* Purely a white-box test hook. Once armed, chttpsvr_start() blocks at one
 * fixed point, right after setting its own contributed_to_engine to true (so
 * a concurrent _engine_force_stop_quiesce_all pass that finds raw registered
 * already sees a real engine contribution to release) and right before
 * chttpsvr_start()'s _engine_acquire() call.
 *
 * That's exactly the window where _engine_acquire() can see
 * srv_engine_bundler.stopping == true and return without blocking on it. The
 * doc comment of that function describes the deadlock this closes, and the
 * "currently stopping" branch of chttpsvr_start() has the caller-side retry
 * that this hook reproduces every time.
 *
 * It fires once per arm call, and the guard keeps all of it out of a
 * production build. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_engine_stopping_race_hook = {0};

static void _engine_stopping_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_engine_stopping_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_engine_stopping_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_engine_stopping_race_hook_for_tests(void) {
  _ccol_call_once(g_engine_stopping_race_hook.once,
                  _engine_stopping_race_hook_init_globals);
  _ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.armed = true;
  g_engine_stopping_race_hook.entered = false;
  g_engine_stopping_race_hook.go = false;
  _ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_engine_stopping_race_hook.once,
                  _engine_stopping_race_hook_init_globals);
  _ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  while (!g_engine_stopping_race_hook.entered)
    _ccol_cond_var_wait(g_engine_stopping_race_hook.cv,
                        g_engine_stopping_race_hook.mutex);
  _ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_release_engine_stopping_race_hook_for_tests(void) {
  _ccol_call_once(g_engine_stopping_race_hook.once,
                  _engine_stopping_race_hook_init_globals);
  _ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.go = true;
  _ccol_cond_var_broadcast(g_engine_stopping_race_hook.cv);
  _ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

static void _engine_stopping_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_engine_stopping_race_hook.once,
                  _engine_stopping_race_hook_init_globals);
  _ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  if (!g_engine_stopping_race_hook.armed) {
    _ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
    return;
  }
  g_engine_stopping_race_hook.armed = false; /* one-shot */
  g_engine_stopping_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_engine_stopping_race_hook.cv);
  while (!g_engine_stopping_race_hook.go)
    _ccol_cond_var_wait(g_engine_stopping_race_hook.cv,
                        g_engine_stopping_race_hook.mutex);
  _ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

/* Purely a white-box test hook. Once armed, chttpsvr_start() blocks at one
 * fixed point, right after _chttpsvr_resolve(h) has succeeded, so this
 * call's resolve pin (pending_resolve_count) is already live, and before it
 * does anything else, its own lifecycle and quiesce_state checks included. It
 * signals that it got there and then waits to be released.
 *
 * Unlike g_start_race_hook above, which pauses much later, after the
 * registration and the engine acquire, this one exists to place a
 * chttpsvr_start() call's resolve pin at a precise point every time: BEFORE a
 * concurrent _quiesce_server_once pass for the same server reaches its own
 * pending_resolve_count wait. That interleaving deadlocks the two, but only
 * if chttpsvr_start()'s wait blocks on quiesce_done_cv while still holding
 * this pin, which is why that wait doesn't block there (see its comment).
 *
 * It fires once per arm call, and the guard keeps all of it out of a
 * production build. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_resolve_race_hook = {0};

static void _start_resolve_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_start_resolve_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_start_resolve_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_resolve_race_hook_for_tests(void) {
  _ccol_call_once(g_start_resolve_race_hook.once,
                  _start_resolve_race_hook_init_globals);
  _ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.armed = true;
  g_start_resolve_race_hook.entered = false;
  g_start_resolve_race_hook.go = false;
  _ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_wait_start_resolve_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_start_resolve_race_hook.once,
                  _start_resolve_race_hook_init_globals);
  _ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  while (!g_start_resolve_race_hook.entered)
    _ccol_cond_var_wait(g_start_resolve_race_hook.cv,
                        g_start_resolve_race_hook.mutex);
  _ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_release_start_resolve_race_hook_for_tests(void) {
  _ccol_call_once(g_start_resolve_race_hook.once,
                  _start_resolve_race_hook_init_globals);
  _ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.go = true;
  _ccol_cond_var_broadcast(g_start_resolve_race_hook.cv);
  _ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

static void _start_resolve_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_start_resolve_race_hook.once,
                  _start_resolve_race_hook_init_globals);
  _ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  if (!g_start_resolve_race_hook.armed) {
    _ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
    return;
  }
  g_start_resolve_race_hook.armed = false; /* one-shot */
  g_start_resolve_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_start_resolve_race_hook.cv);
  while (!g_start_resolve_race_hook.go)
    _ccol_cond_var_wait(g_start_resolve_race_hook.cv,
                        g_start_resolve_race_hook.mutex);
  _ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

/* Purely a white-box test hook. Once armed, _chttpsvr_stop_internal() blocks
 * at one fixed point, right after it entered CHTTPSVR_LC_STOPPING and
 * unlocked raw->mutex and strictly before the blocking
 * ccol_event_loop_remove() and close() of the OLD listener registration. It
 * signals that it got there and then waits to be released.
 *
 * That way a test can land a concurrent chttpsvr_start() every time inside
 * the exact window CHTTPSVR_LC_STOPPING closes (see the comment of
 * chttpsvr_lifecycle_t on struct chttpserver). Without the hook a test would
 * depend on real, unbounded timing to hit a race window a handful of
 * instructions wide.
 *
 * It fires once per arm call, the guard keeps all of it out of a production
 * build, and it has exactly the shape of g_start_race_hook and
 * g_start_resolve_race_hook. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_stop_race_hook = {0};

static void _stop_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_stop_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_stop_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_stop_race_hook_for_tests(void) {
  _ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  _ccol_mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.armed = true;
  g_stop_race_hook.entered = false;
  g_stop_race_hook.go = false;
  _ccol_mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_wait_stop_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  _ccol_mutex_lock(g_stop_race_hook.mutex);
  while (!g_stop_race_hook.entered)
    _ccol_cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  _ccol_mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_release_stop_race_hook_for_tests(void) {
  _ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  _ccol_mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.go = true;
  _ccol_cond_var_broadcast(g_stop_race_hook.cv);
  _ccol_mutex_unlock(g_stop_race_hook.mutex);
}

static void _stop_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  _ccol_mutex_lock(g_stop_race_hook.mutex);
  if (!g_stop_race_hook.armed) {
    _ccol_mutex_unlock(g_stop_race_hook.mutex);
    return;
  }
  g_stop_race_hook.armed = false; /* one-shot */
  g_stop_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_stop_race_hook.cv);
  while (!g_stop_race_hook.go)
    _ccol_cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  _ccol_mutex_unlock(g_stop_race_hook.mutex);
}

/* Purely white-box test instrumentation: a plain "we got here" signal that
 * never blocks. It's for the CHTTPSVR_LC_STOPPING wait branch of
 * chttpsvr_start() (see the comment of that switch case), and it's shaped
 * differently from g_stop_race_hook and g_start_race_hook above on purpose,
 * since those also park the caller until released.
 *
 * A parking hook isn't safe here. Every parking hook in this file fires only
 * after its caller has unlocked whatever mutex the OTHER side of the race may
 * need next, but this call site still holds raw->mutex at this point, and
 * raw->mutex is exactly what _chttpsvr_stop_internal() needs to take to leave
 * CHTTPSVR_LC_STOPPING. A test that forgot to release such a park promptly,
 * or couldn't because the cross-thread choreography is trickier here, would
 * wedge BOTH sides of the very race the hook is there to test, not just one
 * call.
 *
 * So it fires a one-shot signal and never blocks: it sets a flag,
 * broadcasts and goes straight into the real _ccol_cond_var_wait loop below,
 * adding no blocking point of its own. A test can still confirm every time
 * that chttpsvr_start() really got to its real wait and is about to enter it,
 * which a fixed sleep can't do: a 150ms settling sleep may run out because
 * the OS never scheduled the other thread, not because that thread is really
 * blocked. The hook doesn't touch this function's locking discipline at
 * all. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
} g_start_stopping_wait_signal = {0};

static void _start_stopping_wait_signal_init_globals(void) {
  if (_ccol_mutex_init(g_start_stopping_wait_signal.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_start_stopping_wait_signal.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_stopping_wait_signal_for_tests(void) {
  _ccol_call_once(g_start_stopping_wait_signal.once,
                  _start_stopping_wait_signal_init_globals);
  _ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  g_start_stopping_wait_signal.armed = true;
  g_start_stopping_wait_signal.entered = false;
  _ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

void _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests(void) {
  _ccol_call_once(g_start_stopping_wait_signal.once,
                  _start_stopping_wait_signal_init_globals);
  _ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  while (!g_start_stopping_wait_signal.entered)
    _ccol_cond_var_wait(g_start_stopping_wait_signal.cv,
                        g_start_stopping_wait_signal.mutex);
  _ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

static void _start_stopping_wait_signal_fire_if_armed(void) {
  _ccol_call_once(g_start_stopping_wait_signal.once,
                  _start_stopping_wait_signal_init_globals);
  _ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  if (g_start_stopping_wait_signal.armed) {
    g_start_stopping_wait_signal.armed = false; /* one-shot */
    g_start_stopping_wait_signal.entered = true;
    _ccol_cond_var_broadcast(g_start_stopping_wait_signal.cv);
  }
  _ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

/* Purely a white-box test hook. Once armed, _engine_force_stop_quiesce_all()
 * blocks at one fixed point, right after it has pinned the server it read
 * from servers_bundler.servers[0] (with servers_bundler_pins, see that
 * field's comment) and unlocked servers_bundler.mutex, and strictly before
 * calling _quiesce_server_once(). It signals that it got there and then waits
 * to be released.
 *
 * That way a test can land a concurrent chttpsvr_destroy() on exactly that
 * server every time, inside the window servers_bundler_pins closes, where the
 * reaper thread holds a bare struct chttpserver* and only this pin keeps
 * another thread from freeing it. Without the hook a test would depend on
 * real, unbounded timing to hit a race window a handful of instructions
 * wide.
 *
 * It fires once per arm call and has exactly the shape of
 * g_stop_race_hook. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_reaper_race_hook = {0};

static void _reaper_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_reaper_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_reaper_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_reaper_race_hook_for_tests(void) {
  _ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  _ccol_mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.armed = true;
  g_reaper_race_hook.entered = false;
  g_reaper_race_hook.go = false;
  _ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_wait_reaper_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  _ccol_mutex_lock(g_reaper_race_hook.mutex);
  while (!g_reaper_race_hook.entered)
    _ccol_cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  _ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_release_reaper_race_hook_for_tests(void) {
  _ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  _ccol_mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.go = true;
  _ccol_cond_var_broadcast(g_reaper_race_hook.cv);
  _ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

static void _reaper_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  _ccol_mutex_lock(g_reaper_race_hook.mutex);
  if (!g_reaper_race_hook.armed) {
    _ccol_mutex_unlock(g_reaper_race_hook.mutex);
    return;
  }
  g_reaper_race_hook.armed = false; /* one-shot */
  g_reaper_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_reaper_race_hook.cv);
  while (!g_reaper_race_hook.go)
    _ccol_cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  _ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

/* Purely a white-box test hook. Once armed, _quiesce_server_once() blocks at
 * one fixed point, right after it has claimed the winning side
 * (srv->quiesce_state is CHTTPSVR_QS_QUIESCING, the pending_resolve_count
 * wait is satisfied and srv->mutex is unlocked) and strictly before any of
 * the real teardown work (_chttpsvr_stop_internal, _servers_unregister,
 * _drain_and_close_all_connections, _engine_release and
 * _destroy_detached_pools).
 *
 * That way a test can fork() every time while a thread is parked exactly
 * where a real, unbounded interleaving could park one, with the server in
 * quiesce_state == CHTTPSVR_QS_QUIESCING, precisely the state the child-side
 * fixup of _chttpsvr_atfork_release_impl deals with (see that function's doc
 * comment). Without the hook a test would depend on real, unbounded timing to
 * hit a race window that can be arbitrarily narrow.
 *
 * It fires once per arm call and has exactly the shape of
 * g_reaper_race_hook. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_quiesce_teardown_race_hook = {0};

static void _quiesce_teardown_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_quiesce_teardown_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_quiesce_teardown_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void) {
  _ccol_call_once(g_quiesce_teardown_race_hook.once,
                  _quiesce_teardown_race_hook_init_globals);
  _ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.armed = true;
  g_quiesce_teardown_race_hook.entered = false;
  g_quiesce_teardown_race_hook.go = false;
  _ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_quiesce_teardown_race_hook.once,
                  _quiesce_teardown_race_hook_init_globals);
  _ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  while (!g_quiesce_teardown_race_hook.entered)
    _ccol_cond_var_wait(g_quiesce_teardown_race_hook.cv,
                        g_quiesce_teardown_race_hook.mutex);
  _ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void) {
  _ccol_call_once(g_quiesce_teardown_race_hook.once,
                  _quiesce_teardown_race_hook_init_globals);
  _ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.go = true;
  _ccol_cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  _ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

static void _quiesce_teardown_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_quiesce_teardown_race_hook.once,
                  _quiesce_teardown_race_hook_init_globals);
  _ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  if (!g_quiesce_teardown_race_hook.armed) {
    _ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
    return;
  }
  g_quiesce_teardown_race_hook.armed = false; /* one-shot */
  g_quiesce_teardown_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  while (!g_quiesce_teardown_race_hook.go)
    _ccol_cond_var_wait(g_quiesce_teardown_race_hook.cv,
                        g_quiesce_teardown_race_hook.mutex);
  _ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

/* Forward declaration. It's defined further down where
 * _wait_in_flight_bounded and its neighbours use it, but the bounded wait in
 * _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests below
 * needs it here too. */
static void _timespec_add_ms(struct timespec *ts, unsigned ms);

/* Purely a white-box test hook. Once armed it pauses the
 * CHTTPSVR_QS_QUIESCING backoff branch of chttpsvr_start() right after that
 * branch has registered itself in quiesce_waiters and released its resolve
 * pin, and strictly before it takes raw->mutex again for the first time after
 * those two steps.
 *
 * There's a use-after-free in that window if quiesce_waiters++ happens AFTER
 * the pin release instead of before it: releasing the pin first can unblock
 * a concurrent _quiesce_server_once pass's pending_resolve_count wait, which
 * then runs to the end and frees raw before this call gets back to raw->mutex
 * to register as a protected waiter.
 *
 * With the hook a test pauses a real chttpsvr_start() exactly there and runs
 * a real _quiesce_server_once pass and a real concurrent chttpsvr_destroy()
 * to completion around it, which shows directly that raw survives either way.
 * Without it a test would depend on unbounded, real timing to land two
 * threads in a race window a few instructions wide.
 *
 * It fires once per arm call and has exactly the shape of
 * g_quiesce_teardown_race_hook. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_quiescing_unpinned_race_hook = {0};

static void _start_quiescing_unpinned_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_start_quiescing_unpinned_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  /* CLOCK_MONOTONIC, the same as chttpsvr_create_mp, which has its own
   * comment about it. The bounded wait in
   * _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests below
   * computes its deadline with clock_gettime(CLOCK_MONOTONIC, ...), while
   * _ccol_cond_var_init defaults to CLOCK_REALTIME, and comparing against
   * that deadline would give the wrong answer. */
  _ccol_cond_var_attr_t cv_attr;
  int cv_rc;
  if (_ccol_cond_var_attr_init(cv_attr) == 0) {
    _ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc = _ccol_cond_var_init_ca(g_start_quiescing_unpinned_race_hook.cv,
                                   cv_attr);
    _ccol_cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = _ccol_cond_var_init(g_start_quiescing_unpinned_race_hook.cv);
  }
  if (cv_rc != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests(void) {
  _ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                  _start_quiescing_unpinned_race_hook_init_globals);
  _ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.armed = true;
  g_start_quiescing_unpinned_race_hook.entered = false;
  g_start_quiescing_unpinned_race_hook.go = false;
  _ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* The only test using this hook,
 * start_racing_engine_stop_and_destroy_does_not_free_raw_too_early in
 * tests_engine_stop.c, only gets here when a concurrent reaper thread sets
 * CHTTPSVR_QS_QUIESCING before the paused chttpsvr_start() checks
 * quiesce_state again. The test makes that order certain with
 * _chttpsvr_wait_quiesce_claimed_signal_entered_for_tests, which only
 * returns once the reaper has claimed the server, and releases the paused
 * call after that.
 *
 * The wait is bounded to 10s, generous even under heavy instrumentation, so a
 * regression that keeps the call away from this hook becomes a clean test
 * failure the harness catches. Without the bound the call would hang, and
 * since it runs on the main test thread and not a background one it would
 * hang the whole binary, which no bounded-join fixture can rescue. Returns
 * false on a timeout. */
bool _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                  _start_quiescing_unpinned_race_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  _timespec_add_ms(&deadline, 10000);
  _ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  bool timed_out = false;
  while (!g_start_quiescing_unpinned_race_hook.entered && !timed_out) {
    timed_out =
        _ccol_cond_var_timedwait(g_start_quiescing_unpinned_race_hook.cv,
                                 g_start_quiescing_unpinned_race_hook.mutex,
                                 deadline) == ETIMEDOUT;
  }
  bool entered = g_start_quiescing_unpinned_race_hook.entered;
  _ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
  return entered;
}

void _chttpsvr_release_start_quiescing_unpinned_race_hook_for_tests(void) {
  _ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                  _start_quiescing_unpinned_race_hook_init_globals);
  _ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.go = true;
  _ccol_cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  _ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

static void _start_quiescing_unpinned_race_hook_wait_if_armed(void) {
  _ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                  _start_quiescing_unpinned_race_hook_init_globals);
  _ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  if (!g_start_quiescing_unpinned_race_hook.armed) {
    _ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
    return;
  }
  g_start_quiescing_unpinned_race_hook.armed = false; /* one-shot */
  g_start_quiescing_unpinned_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  while (!g_start_quiescing_unpinned_race_hook.go)
    _ccol_cond_var_wait(g_start_quiescing_unpinned_race_hook.cv,
                        g_start_quiescing_unpinned_race_hook.mutex);
  _ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* Purely a white-box test hook. Once armed, the next _quiesce_server_once()
 * pass that claims a server signals once, right after setting quiesce_state
 * to CHTTPSVR_QS_QUIESCING and before waiting for pending_resolve_count. It
 * only signals and never blocks, so the pass gets no new blocking point. It
 * fires while the pass holds srv->mutex, and the hook mutex is a leaf lock
 * nobody holds while taking any other lock.
 *
 * A test that pauses a chttpsvr_start() with its pin held and then calls
 * chttpsvr_engine_stop() uses it to know the reaper has claimed the server.
 * When the test then releases the paused call, that call takes raw->mutex
 * after the claim and so sees CHTTPSVR_QS_QUIESCING every time. The wait is
 * bounded to 10s and returns false on a timeout, so a missed claim fails the
 * test instead of hanging the binary. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool entered;
} g_quiesce_claimed_signal = {0};

static void _quiesce_claimed_signal_init_globals(void) {
  if (_ccol_mutex_init(g_quiesce_claimed_signal.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  /* The bounded wait below computes its deadline on CLOCK_MONOTONIC, so the
   * condition variable has to use that clock as well. */
  _ccol_cond_var_attr_t cv_attr;
  int cv_rc;
  if (_ccol_cond_var_attr_init(cv_attr) == 0) {
    _ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc = _ccol_cond_var_init_ca(g_quiesce_claimed_signal.cv, cv_attr);
    _ccol_cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = _ccol_cond_var_init(g_quiesce_claimed_signal.cv);
  }
  if (cv_rc != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_quiesce_claimed_signal_for_tests(void) {
  _ccol_call_once(g_quiesce_claimed_signal.once,
                  _quiesce_claimed_signal_init_globals);
  _ccol_mutex_lock(g_quiesce_claimed_signal.mutex);
  g_quiesce_claimed_signal.armed = true;
  g_quiesce_claimed_signal.entered = false;
  _ccol_mutex_unlock(g_quiesce_claimed_signal.mutex);
}

bool _chttpsvr_wait_quiesce_claimed_signal_entered_for_tests(void) {
  _ccol_call_once(g_quiesce_claimed_signal.once,
                  _quiesce_claimed_signal_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  _timespec_add_ms(&deadline, 10000);
  _ccol_mutex_lock(g_quiesce_claimed_signal.mutex);
  bool timed_out = false;
  while (!g_quiesce_claimed_signal.entered && !timed_out) {
    timed_out = _ccol_cond_var_timedwait(g_quiesce_claimed_signal.cv,
                                         g_quiesce_claimed_signal.mutex,
                                         deadline) == ETIMEDOUT;
  }
  bool entered = g_quiesce_claimed_signal.entered;
  /* A missed signal mustn't fire later in some unrelated pass. */
  g_quiesce_claimed_signal.armed = false;
  _ccol_mutex_unlock(g_quiesce_claimed_signal.mutex);
  return entered;
}

static void _quiesce_claimed_signal_fire_if_armed(void) {
  _ccol_call_once(g_quiesce_claimed_signal.once,
                  _quiesce_claimed_signal_init_globals);
  _ccol_mutex_lock(g_quiesce_claimed_signal.mutex);
  if (g_quiesce_claimed_signal.armed) {
    g_quiesce_claimed_signal.armed = false; /* one-shot */
    g_quiesce_claimed_signal.entered = true;
    _ccol_cond_var_broadcast(g_quiesce_claimed_signal.cv);
  }
  _ccol_mutex_unlock(g_quiesce_claimed_signal.mutex);
}

/* Purely a white-box test hook. Once armed, _listener_on_readable() blocks
 * at one of two fixed points, picked by the arm call. The default is right
 * after the call pins srv with listener_dispatch_pins (see that field's
 * comment) and strictly before it calls _listener_on_readable_impl(). The
 * entry point is the callback's very first statement, before the pin, where a
 * concurrent chttpsvr_stop and chttpsvr_destroy see no pin at all. It signals
 * that it got there and then waits to be released.
 *
 * That way a test can land a concurrent chttpsvr_destroy() every time while a
 * listener dispatch is really in flight holding this pin, and check that the
 * destroy blocks until the hook is released. Without it a test would have to
 * race a live accept4() backoff sleep on real, unbounded timing, a window
 * that's real but can't be reproduced on demand.
 *
 * It has exactly the shape of g_reaper_race_hook. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  bool armed;
  bool at_entry;
  bool entered;
  bool go;
} g_listener_dispatch_race_hook = {0};

static void _listener_dispatch_race_hook_init_globals(void) {
  if (_ccol_mutex_init(g_listener_dispatch_race_hook.mutex) != 0)
    _ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (_ccol_cond_var_init(g_listener_dispatch_race_hook.cv) != 0)
    _ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

static void _listener_dispatch_race_hook_arm(bool at_entry) {
  _ccol_call_once(g_listener_dispatch_race_hook.once,
                  _listener_dispatch_race_hook_init_globals);
  _ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.armed = true;
  g_listener_dispatch_race_hook.at_entry = at_entry;
  g_listener_dispatch_race_hook.entered = false;
  g_listener_dispatch_race_hook.go = false;
  _ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_arm_listener_dispatch_race_hook_for_tests(void) {
  _listener_dispatch_race_hook_arm(false);
}

void _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests(void) {
  _listener_dispatch_race_hook_arm(true);
}

void _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests(void) {
  _ccol_call_once(g_listener_dispatch_race_hook.once,
                  _listener_dispatch_race_hook_init_globals);
  _ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  while (!g_listener_dispatch_race_hook.entered)
    _ccol_cond_var_wait(g_listener_dispatch_race_hook.cv,
                        g_listener_dispatch_race_hook.mutex);
  _ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_release_listener_dispatch_race_hook_for_tests(void) {
  _ccol_call_once(g_listener_dispatch_race_hook.once,
                  _listener_dispatch_race_hook_init_globals);
  _ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.go = true;
  _ccol_cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  _ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

static void _listener_dispatch_race_hook_wait_if_armed(bool at_entry) {
  _ccol_call_once(g_listener_dispatch_race_hook.once,
                  _listener_dispatch_race_hook_init_globals);
  _ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  if (!g_listener_dispatch_race_hook.armed ||
      g_listener_dispatch_race_hook.at_entry != at_entry) {
    _ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
    return;
  }
  g_listener_dispatch_race_hook.armed = false; /* one-shot */
  g_listener_dispatch_race_hook.entered = true;
  _ccol_cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  while (!g_listener_dispatch_race_hook.go)
    _ccol_cond_var_wait(g_listener_dispatch_race_hook.cv,
                        g_listener_dispatch_race_hook.mutex);
  _ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* reject_pool's task function. It runs _conn_reject_and_close on one of
 * reject_pool's dedicated threads instead of the reactor thread (the comment
 * of that field on struct chttpserver and the comment of _conn_start_diverted
 * say why). Then, like _task_worker's decrement, it releases this request's
 * in_flight_requests slot only once the close has really completed, not as
 * soon as the task is queued; the comment of _release_in_flight explains why
 * that order matters. srv is read before _conn_reject_and_close because that
 * function frees conn internally. */
static void _reject_task(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _chttpsvr_mark_worker_thread(srv);
  _conn_reject_and_close(conn, /*run_middleware=*/true);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_reject_task_run_count_for_tests, 1);
#endif
  _release_in_flight(srv);
}

/* Feeds n fresh bytes into conn->parser and deals with whatever comes out,
 * exactly like _conn_pump's read loop. It's a function of its own so that
 * _task_worker's keep-alive tail (see that tail's comment about
 * chttp1_stream_take_leftover) can drive the same header-parse machinery
 * synchronously on the worker thread against the bytes of a pipelined next
 * request. Those bytes are already off the wire, so they can never come back
 * as a fresh socket-readable event for the reactor to handle.
 *
 * buf and n don't need to come from a live socket read at all. The caller
 * only owns buf for the duration of this call, since chttp1_parser_execute
 * keeps no pointer into it after returning, and _conn_start_diverted below
 * copies out whatever leftover it gets, just as it does for a real read on
 * the reactor thread.
 *
 * @return true when the caller should keep reading, i.e. the parser needs
 *         more bytes to complete the request line or the header block
 *         (CHTTP1_OK). false when this call has already settled conn's fate
 *         for now: diverted to a worker, rejected and routed to reject_pool,
 *         or closed outright. The caller must not touch conn again. */
static bool _conn_feed_bytes(chttpsvr_conn_t *conn, const char *buf, size_t n) {
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  /* Reuse the instant we just read for last_activity instead of taking
   * another one. The call is idempotent within a request, so the budget runs
   * from this request's FIRST byte however many chunks follow; refreshing it
   * per chunk is exactly what makes last_activity a gap timer rather than a
   * total one. */
  _conn_header_phase_arm(conn, &conn->last_activity);
  chttp1_errno_t r = chttp1_parser_execute(&conn->parser, buf, n);

  if (r == CHTTP1_OK) return true; /* need more header bytes, keep reading */

  if (r == CHTTP1_HEADERS_ONLY || r == CHTTP1_PAUSED) {
    size_t consumed = chttp1_parser_consumed(&conn->parser);
    const char *leftover = buf + consumed;
    size_t leftover_len = n - consumed;

    /* When conn->expects_continue is true, _task_worker sends the Expect:
     * 100-continue interim line on a worker thread once the request is really
     * diverted, not here; the comment of that function says why. */
    _conn_start_diverted(conn, leftover, leftover_len);
    return false;
  }

  /* CHTTP1_USER means an unmatched or rejected route, decided by our
   * _on_headers_complete, which has already set conn->req_rejected to a
   * specific status. We identified the route ourselves, so we send a graceful
   * error response, never read the body if there is one, and close the
   * connection instead of keeping it alive; otherwise the body still coming
   * from the client would be read as a pipelined request.
   *
   * CHTTP1_ERROR means a syntax-level problem: the parser itself rejected the
   * request before any routing ran. A malformed request line, a negative
   * Content-Length, a chunked entry that isn't last in a Transfer-Encoding
   * list, a header that's too long, a request that breaks RFC 7230 SS5.4's
   * Host rule, things like that.
   *
   * Each of those is a CLIENT error, and RFC 7231 SS6.5.1 has the status for
   * it, 400 Bad Request, for a request the server "will not process due to
   * something that is perceived to be a client error (e.g., malformed request
   * syntax ...)". We answer with it through the same bounded reject path our
   * 404, 405, 413, 500, 501 and 503 take, and then close, because the byte
   * stream is no longer framed and nothing after this point can be read as a
   * request.
   *
   * Just closing without a response would be wrong, because the peer would
   * have to guess between a network fault, a crash and a rejection, and an
   * operator would have no status code to count.
   *
   * The response goes out bare: _reject_status_runs_middleware() doesn't list
   * 400, so no application middleware runs for a request whose syntax never
   * parsed, and there'd be no decoded path or matched route to give one
   * anyway. */
  if (!conn->req_rejected) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
  }
  /* This goes through reject_pool, or its bounded synchronous fallback, just
   * like the pool-full 503, so a slow-reading client we need to tell about a
   * 400, 404, 405 or 500 can't stall the single reactor thread. See the
   * comments of _conn_dispatch_reject and _conn_reject_via_pool. */
  _conn_dispatch_reject(conn);
  return false;
}

/* Drives the reactor-owned part of a connection: the TLS handshake if there is
 * one, then the header parse, until the request is either rejected or
 * diverted to a worker. For a rejection the response goes out synchronously
 * and the connection is closed. */
static void _conn_pump(chttpsvr_conn_t *conn) {
  /* A parked connection is waiting for its socket in the middle of a request,
   * not for the next request's headers, and a worker carries it on. */
  if (__builtin_expect(conn->park != _CONN_PARK_NONE, 0)) {
    if (conn->park == _CONN_PARK_LINGER)
      _conn_linger_step(conn);
    else
      _conn_resume_parked(conn);
    return;
  }
  if (conn->state == CONN_ST_TLS_HANDSHAKE) {
    /* The handshake belongs to the reactor-owned phase and shares its budget,
     * because a peer can open a TLS connection and then drip handshake bytes
     * without ever reaching the header block, so a ceiling that only arms
     * once header bytes arrive would never fire for it. We only read the
     * clock on the step that arms the ceiling, this request's first step, not
     * on every step. */
    if (!conn->header_phase_active) {
      struct timespec hs_now;
      clock_gettime(CLOCK_MONOTONIC, &hs_now);
      _conn_header_phase_arm(conn, &hs_now);
    }
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_ERROR) {
      _SRV_ENGINE_LOG(clog_warn, "TLS handshake failed fd=%d", conn->fd);
      _conn_close(conn);
      return;
    }
    if (r == CTLS_HANDSHAKE_WANT_READ || r == CTLS_HANDSHAKE_WANT_WRITE) {
      ccol_select_dir want = (r == CTLS_HANDSHAKE_WANT_WRITE)
                                 ? ccol_select_write
                                 : ccol_select_read;
      if (conn->reg) {
        /* A modify shouldn't fail in practice, since nothing else touches
         * this connection's reg while the reactor owns it alone, and
         * _idle_list_try_claim makes sure there's a single owner. We still
         * treat a failure as fatal for the connection rather than ignore it,
         * because otherwise conn->reg's real epoll interest gets out of step
         * with `want` and the handshake gets stranded, with no further
         * readiness event coming in the direction it still needs. */
        if (_conn_reg_modify(conn, want) != ccol_success) {
          _conn_close(conn);
          return;
        }
      } else {
        char *err = NULL;
        conn->reg = ccol_event_loop_add(
            srv_engine_bundler.reactor, ccol_selectable_from_fd(conn->fd, want),
            (ccol_event_handlers_t){.on_readable = _conn_on_readable,
                                    .on_writable = _conn_on_writable,
                                    .on_error = _conn_on_error,
                                    .on_removed = _conn_on_removed},
            conn, &err);
        if (!conn->reg) {
          _conn_close(conn);
          return;
        }
        conn->reg_dir = (uint8_t)want;
        /* See the comment of conn->lifetime_refs. The on_removed this new
         * registration eventually fires is a fresh +1 contribution to that
         * counter, and we count it the moment the registration goes live, so
         * nothing can miss it. srv->lifetime_refs gets the same treatment at
         * the server level for the same reason (see its own comment). */
        atomic_fetch_add(&conn->lifetime_refs, 1);
        atomic_fetch_add(&conn->srv->lifetime_refs, 1);
      }
      /* Put conn back in the idle list before returning, since this thread
       * is done with it for now and waits for the next handshake step's
       * readiness, and a later dispatch must be able to claim it back with
       * _idle_list_try_claim. Without this a connection in the middle of a
       * handshake would never be in the idle list between steps, so neither
       * the idle-timeout sweep nor a server destroy's idle-connection cleanup
       * could see it, let alone close it. */
      _idle_list_add(conn);
      return;
    }
    /* CTLS_HANDSHAKE_DONE */
    conn->state = CONN_ST_READING_HEADERS;
    if (conn->reg) {
      if (_conn_reg_modify(conn, ccol_select_read) != ccol_success) {
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
      /* A signal landing on this reactor thread mid-read can interrupt the
       * read() or ctls_conn_read() above with EINTR while the connection is
       * perfectly healthy, e.g. the application's SIGTERM handler that calls
       * chttpsvr_engine_stop() as this module's header docs recommend,
       * installed without SA_RESTART. ctls_conn_read() documents itself as a
       * non-blocking read(2), errno included, so both branches above are
       * covered. We retry, just like _listener_on_readable's accept() loop
       * retries an EINTR, instead of tearing down an unrelated, healthy
       * connection because of one spurious signal interruption. */
      if (errno == EINTR) continue;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        /* On a plaintext connection an EWOULDBLOCK from a raw read(2) always
         * means "wait until readable", matching the read-direction
         * ccol_selectable_from_fd registration below. On a TLS connection it
         * does NOT, because OpenSSL may need to write before this very
         * ctls_conn_read() can make progress, to flush a session ticket it
         * deferred after the handshake for example, or for a TLS 1.2
         * renegotiation (see the doc comment of ctls_conn_wants_write).
         *
         * Registering read interest only in that case would leave the
         * connection without any further readiness event in the direction it
         * really needs, so it would stall until the configured read or idle
         * timeout fires rather than until the next real byte, and with the
         * documented "wait forever" setting of 0 it would stall for good. */
        ccol_select_dir want = ccol_select_read;
        if (conn->tls && ctls_conn_wants_write(conn->tls))
          want = ccol_select_write;
        if (!conn->reg) {
          char *err = NULL;
          conn->reg = ccol_event_loop_add(
              srv_engine_bundler.reactor,
              ccol_selectable_from_fd(conn->fd, want),
              (ccol_event_handlers_t){.on_readable = _conn_on_readable,
                                      .on_writable = _conn_on_writable,
                                      .on_error = _conn_on_error,
                                      .on_removed = _conn_on_removed},
              conn, &err);
          if (!conn->reg) {
            _conn_close(conn);
            return;
          }
          conn->reg_dir = (uint8_t)want;
          /* See the comments of conn->lifetime_refs and of
           * srv->lifetime_refs, which gets the same treatment at the server
           * level. */
          atomic_fetch_add(&conn->lifetime_refs, 1);
          atomic_fetch_add(&conn->srv->lifetime_refs, 1);
        } else {
          /* The connection already has a read-direction registration from an
           * earlier pass of this loop or from CTLS_HANDSHAKE_DONE above. Like
           * the handshake-step branch above, we compute the interest fresh on
           * every EWOULDBLOCK instead of assuming the old one still holds;
           * _conn_reg_modify skips the call when want hasn't changed. */
          if (_conn_reg_modify(conn, want) != ccol_success) {
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
    /* CHTTP1_OK, so the loop reads more header bytes. */
  }
}

/* Every entry point through which the reactor dispatches into a live
 * connection (one that already has a registration) must claim it out of
 * the idle list first. The doc comment of _idle_list_try_claim describes the
 * use-after-free this prevents, where a concurrent closer from the idle
 * timeout or a destroy frees the same connection, TLS state and all, while a
 * dispatch is using it.
 *
 * A failed claim isn't an error. Either a closer already has exclusive
 * ownership, or, harmlessly, this connection's _idle_list_add for the
 * registration that just fired hasn't run yet (the same doc comment explains
 * why level-triggered epoll heals that case by itself). Either way the right
 * thing is the same: touch nothing and return. */
static void _conn_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                              ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_pump(conn);
}

static void _conn_on_writable(ccol_event_loop loop, ccol_event_reg reg,
                              ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_pump(conn);
}

static void _conn_on_error(ccol_event_loop loop, ccol_event_reg reg,
                           ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  if (!_idle_list_try_claim(conn)) return;
  _conn_close(conn);
}

/* ========================================================================== */
/*                    SLOW CLIENTS                                            */
/* ========================================================================== */

/* For a buffered route a worker thread never waits on a client's socket, and
 * four mechanisms work together to make that true.
 *
 * The first is parking. A body read or a response write that finds nothing
 * to read, or no room to write, gives the connection up: it goes into the
 * idle list with park set, its registration armed for the direction it
 * needs, and no thread at all. The reactor claims it on readiness and hands
 * it to any free worker, which carries on where the previous one stopped.
 * The claim is _idle_list_try_claim's, so a closer and a dispatch never both
 * act on it.
 *
 * Then there's the rate floor. Once min_transfer_rate_grace_ms has passed,
 * the sweep closes a parked connection whose average rate since the start of
 * its body read or response write is below min_transfer_rate_bps, and a
 * streaming handler runs into the same floor inside chttpsvr_req_read.
 *
 * Body memory is the third. From the first byte read until the handler
 * returns, all the bodies buffered routes hold together stay within
 * max_partial_body_memory, and a request whose body doesn't fit waits,
 * without a thread, in the memory-wait list (see _mem_admit and
 * _conn_drop_body).
 *
 * Last, streaming handlers run on their own bounded pool fed by a bounded
 * queue whose entries hold no thread; see _stream_token_task.
 *
 * Every wait here is visible to the sweep, which enforces its limits, and to
 * the server's teardown, which closes or answers it. */

/* The largest receive low-water mark a parked body asks for, which bounds how
 * much the kernel buffers before waking the reactor. */
#define _CHTTPSVR_RCVLOWAT_MAX 16384

#ifdef RUNNING_UNIT_TESTS
/* White-box test instrumentation. The clock offset shifts the clock of every
 * slow-client limit and nothing else, so a test can expire a rate floor, a
 * memory wait or a queue deadline without sleeping. The counters are
 * process-wide like the other counters of this file. */
static _Atomic long long g_slow_clock_offset_ms_for_tests = 0;
static _Atomic size_t g_resume_dispatch_count_for_tests = 0;
static _Atomic size_t g_park_count_for_tests = 0;
/* The same count per kind of park, indexed by _CONN_PARK_*. */
static _Atomic size_t g_park_kind_count_for_tests[8];
size_t _chttpsvr_park_kind_count_for_tests(int kind) {
  return kind >= 0 && kind < 8 ? atomic_load(&g_park_kind_count_for_tests[kind])
                               : 0;
}
static _Atomic bool g_rcvlowat_disabled_for_tests = false;
void _chttpsvr_advance_slow_clock_for_tests(long long ms) {
  atomic_fetch_add(&g_slow_clock_offset_ms_for_tests, ms);
}
size_t _chttpsvr_resume_dispatch_count_for_tests(void) {
  return atomic_load(&g_resume_dispatch_count_for_tests);
}
size_t _chttpsvr_park_count_for_tests(void) {
  return atomic_load(&g_park_count_for_tests);
}
void _chttpsvr_set_rcvlowat_enabled_for_tests(bool enabled) {
  atomic_store(&g_rcvlowat_disabled_for_tests, !enabled);
}

/* Holds a streaming-pool task at one of two points around its claim, just
 * before it takes the next queued request or just after. A test that runs the
 * sweep while the task is held shows that exactly one of the two answers the
 * request. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_cond_var_t cv;
  _ccol_once_flag_t once;
  int armed_at; /* 0 = not armed, 1 = before the claim, 2 = after it */
  bool entered;
  /* Bumped by every release. A held task waits until it differs from what it
   * saw on entry, so a later arm, which the test makes after its release,
   * can never take the release back from a task that hasn't woken up
   * yet. */
  unsigned release_gen;
} g_stream_claim_hook = {0};
static void _stream_claim_hook_init_globals(void) {
  if (_ccol_mutex_init(g_stream_claim_hook.mutex) != 0 ||
      _ccol_cond_var_init(g_stream_claim_hook.cv) != 0)
    _ccol_fatal_err("chttpsvr: stream claim hook init failed");
}
void _chttpsvr_arm_stream_claim_hook_for_tests(int where) {
  _ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  _ccol_mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.armed_at = where;
  g_stream_claim_hook.entered = false;
  _ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
bool _chttpsvr_wait_stream_claim_hook_entered_for_tests(unsigned timeout_ms) {
  _ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  _timespec_add_ms(&deadline, timeout_ms);
  _ccol_mutex_lock(g_stream_claim_hook.mutex);
  while (!g_stream_claim_hook.entered)
    if (_ccol_cond_var_timedwait(g_stream_claim_hook.cv,
                                 g_stream_claim_hook.mutex,
                                 deadline) == ETIMEDOUT)
      break;
  bool entered = g_stream_claim_hook.entered;
  _ccol_mutex_unlock(g_stream_claim_hook.mutex);
  return entered;
}
void _chttpsvr_release_stream_claim_hook_for_tests(void) {
  _ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  _ccol_mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.release_gen++;
  _ccol_cond_var_broadcast(g_stream_claim_hook.cv);
  _ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
static void _stream_claim_hook_wait_if_armed(int where) {
  _ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  _ccol_mutex_lock(g_stream_claim_hook.mutex);
  if (g_stream_claim_hook.armed_at == where) {
    unsigned gen = g_stream_claim_hook.release_gen;
    g_stream_claim_hook.armed_at = 0;
    g_stream_claim_hook.entered = true;
    _ccol_cond_var_broadcast(g_stream_claim_hook.cv);
    while (g_stream_claim_hook.release_gen == gen)
      _ccol_cond_var_wait(g_stream_claim_hook.cv, g_stream_claim_hook.mutex);
  }
  _ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* The clock for every slow-client limit: CLOCK_MONOTONIC, plus the tests'
 * offset in a test build. */
static void _slow_clock(struct timespec *ts) {
  clock_gettime(CLOCK_MONOTONIC, ts);
#ifdef RUNNING_UNIT_TESTS
  long long off = atomic_load(&g_slow_clock_offset_ms_for_tests);
  if (off > 0) _timespec_add_ms(ts, (unsigned)off);
#endif
}

/* Whole milliseconds from then to now, rounded down and signed, so negative
 * when another thread stamped then after this thread read now. A limit of n
 * ms that compares this with >= n is reached exactly when n ms have passed,
 * never sooner. */
static long long _ms_since(struct timespec now, struct timespec then) {
  return _ns_between(now, then) / 1000000LL;
}

/* The same span rounded up. A wait the server takes off a client's clock and
 * the time left before a deadline both round up, so neither charges the
 * client for a fraction of a millisecond it didn't use. */
static long long _ms_since_ceil(struct timespec now, struct timespec then) {
  return _ms_ceil_of_ns(_ns_between(now, then));
}

#ifdef RUNNING_UNIT_TESTS
long long _chttpsvr_ms_since_for_tests(struct timespec now,
                                       struct timespec then, bool ceil) {
  return ceil ? _ms_since_ceil(now, then) : _ms_since(now, then);
}
#endif

static void _timespec_sub_ms(struct timespec *ts, long long ms) {
  ts->tv_sec -= (time_t)(ms / 1000);
  ts->tv_nsec -= (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec < 0) {
    ts->tv_nsec += 1000000000L;
    ts->tv_sec -= 1;
  }
}

static bool _timespec_reached(struct timespec now, struct timespec when) {
  return now.tv_sec > when.tv_sec ||
         (now.tv_sec == when.tv_sec && now.tv_nsec >= when.tv_nsec);
}

/* The receive low-water mark. */

/* Sets SO_RCVLOWAT on a connection that parks with an incomplete body, so the
 * reactor wakes once a useful amount of body is queued and not for every
 * segment a slow client sends. The mark is the number of body bytes the
 * framing still guarantees, capped at _CHTTPSVR_RCVLOWAT_MAX (see
 * chttp1_body_bytes_still_expected). A client that follows its own framing
 * sends at least that many bytes before it can expect an answer, so the mark
 * is always reached and the tail of the body always wakes the reader. For a
 * chunked body the guarantee is what the current chunk still needs, and
 * nothing on a chunk-size line or in the trailers, where the next byte may be
 * the last, so the mark is 1 there. An end of stream wakes the reader
 * whatever the mark.
 *
 * The plaintext count is safe for TLS too, since a record never takes fewer
 * bytes on the wire than the plaintext inside it. But that only holds when
 * OpenSSL doesn't already hold part of a record: a partial record it read
 * ahead needs fewer bytes from the socket than the plaintext it completes. So
 * the mark is 1 whenever ctls_conn_has_pending_input() is true.
 *
 * The mark must also be a count the socket can actually queue. A receive
 * buffer holds a bit less data than its size (OpenBSD gives 16384 bytes and
 * queues 16358), so a mark at the buffer size is never reached and the
 * connection sleeps until a timeout. That's why the mark is at most half of
 * the receive buffer, which the first park reads. If the buffer grows later
 * the mark just stays lower than it could be, costing a few extra wakeups.
 *
 * The sweep can't see bytes sitting in the socket below the mark, so it asks
 * the kernel (FIONREAD) before judging such a connection slow; see
 * _parked_expired. */
static void _conn_set_rcvlowat(chttpsvr_conn_t *conn) {
  int want = 1;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_rcvlowat_disabled_for_tests)) want = 0;
#endif
  if (want && !(conn->tls && ctls_conn_has_pending_input(conn->tls))) {
    uint64_t rem = chttp1_body_bytes_still_expected(&conn->parser);
    if (rem > 1)
      want = rem < (uint64_t)_CHTTPSVR_RCVLOWAT_MAX ? (int)rem
                                                    : _CHTTPSVR_RCVLOWAT_MAX;
  }
  if (want > 1) {
    if (conn->rcvlowat_cap == 0) {
      int rcvbuf = 0;
      socklen_t len = sizeof(rcvbuf);
      conn->rcvlowat_cap =
          getsockopt(conn->fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, &len) == 0 &&
                  rcvbuf / 2 > 1
              ? rcvbuf / 2
              : 1;
    }
    if (want > conn->rcvlowat_cap) want = conn->rcvlowat_cap;
  }
  if (want <= 1) {
    if (conn->rcvlowat > 1) {
      int one = 1;
      (void)setsockopt(conn->fd, SOL_SOCKET, SO_RCVLOWAT, &one, sizeof(one));
      conn->rcvlowat = 1;
    }
    return;
  }
  if (want != conn->rcvlowat &&
      setsockopt(conn->fd, SOL_SOCKET, SO_RCVLOWAT, &want, sizeof(want)) == 0)
    conn->rcvlowat = want;
}

/* Puts the mark back to 1 once the connection is done with its body, so the
 * next request, or a write park, wakes on its first byte. */
static void _conn_reset_rcvlowat(chttpsvr_conn_t *conn) {
  if (conn->rcvlowat > 1) {
    int one = 1;
    (void)setsockopt(conn->fd, SOL_SOCKET, SO_RCVLOWAT, &one, sizeof(one));
    conn->rcvlowat = 1;
  }
}

/* The rate floor. */

static void _rate_start(chttpsvr_conn_t *conn, uint64_t bytes_so_far) {
  _slow_clock(&conn->rate_start);
  conn->last_progress = conn->rate_start;
  conn->rate_bytes = bytes_so_far;
  conn->rate_paused_ms = 0;
  conn->rate_queued = 0;
  conn->rate_active = true;
}

/* How much of the current phase counts against the client: the time since
 * the phase started minus the time the server held it back. */
static long long _rate_elapsed_ms(const chttpsvr_conn_t *conn,
                                  struct timespec now) {
  return _ms_since(now, conn->rate_start) - conn->rate_paused_ms;
}

/* How long, in ms, the phase stays above the floor with the bytes it has
 * moved: the grace period or the time those bytes buy at the floor rate,
 * whichever is longer. */
static long long _rate_allowed_ms(unsigned rate, unsigned grace,
                                  uint64_t bytes) {
  uint64_t bought =
      bytes > UINT64_MAX / 1000u ? UINT64_MAX : bytes * 1000u / rate;
  if (bought > (uint64_t)LLONG_MAX) bought = (uint64_t)LLONG_MAX;
  return (long long)bought > (long long)grace ? (long long)bought
                                              : (long long)grace;
}

static bool _rate_below_floor(struct chttpserver *srv,
                              const chttpsvr_conn_t *conn, struct timespec now,
                              uint64_t extra_bytes) {
  unsigned rate = atomic_load(&srv->min_rate_bps);
  if (rate == UINT_MAX || !conn->rate_active) return false;
  unsigned grace = atomic_load(&srv->rate_grace_ms);
  return _rate_elapsed_ms(conn, now) >
         _rate_allowed_ms(rate, grace, conn->rate_bytes + extra_bytes);
}

/* The rate floor inside a streaming handler's blocking read. The first call
 * starts the phase. It returns false when the body is already below the
 * floor, and otherwise shrinks *timeout_ms_inout to the moment the floor
 * would be crossed if no more bytes came. A 0 there means "no per-call bound
 * yet", as in _shrink_timeout_to_deadline. */
static bool _rate_floor_shrink(chttpsvr_conn_t *conn,
                               unsigned *timeout_ms_inout) {
  struct chttpserver *srv = conn->srv;
  unsigned rate = atomic_load(&srv->min_rate_bps);
  if (rate == UINT_MAX) return true;
  if (!conn->rate_active) _rate_start(conn, 0);
  struct timespec now;
  _slow_clock(&now);
  long long left = _rate_allowed_ms(rate, atomic_load(&srv->rate_grace_ms),
                                    conn->rate_bytes) -
                   _rate_elapsed_ms(conn, now) + 1;
  if (left <= 0) return false;
  unsigned left_ms = left > (long long)UINT_MAX ? UINT_MAX : (unsigned)left;
  if (!*timeout_ms_inout || left_ms < *timeout_ms_inout)
    *timeout_ms_inout = left_ms;
  return true;
}

/* How many bytes conn's send queue still holds, not sent by the kernel or not
 * acknowledged by the peer yet (SIOCOUTQ), or SIZE_MAX when the kernel
 * doesn't report it. */
static size_t _conn_send_queue(const chttpsvr_conn_t *conn) {
#ifdef RUNNING_UNIT_TESTS
  _parked_query_note_for_tests();
#endif /* RUNNING_UNIT_TESTS */
  int outq = 0;
#if defined(SIOCOUTQ)
  if (ioctl(conn->fd, SIOCOUTQ, &outq) != 0 || outq < 0) return SIZE_MAX;
#elif defined(FIONWRITE)
  /* The BSD name of the same count. */
  if (ioctl(conn->fd, FIONWRITE, &outq) != 0 || outq < 0) return SIZE_MAX;
#elif defined(SO_NWRITE)
  /* macOS reports it as a socket option. */
  socklen_t len = sizeof(outq);
  if (getsockopt(conn->fd, SOL_SOCKET, SO_NWRITE, &outq, &len) != 0 || outq < 0)
    return SIZE_MAX;
#else
  (void)conn;
  return SIZE_MAX;
#endif
  return (size_t)outq;
}

/* Gap and rate floor for a parked response. The reactor only wakes such a
 * connection when the socket becomes writable again, which for TCP is when
 * about a third of a send buffer the kernel autotunes to several MB has
 * drained. A reader taking the response steadily, way above the floor, may
 * need longer than response_write_timeout_ms to drain that much, so the
 * wakeup alone doesn't measure its progress. The send queue does: every byte
 * that leaves it has reached the reader, and a queue that has shrunk since
 * the park or the last tick counts as progress for the gap. For TLS the
 * queue holds records, which is still what reached the reader.
 *
 * The rate floor also counts bytes that reached the reader and not bytes the
 * kernel accepted, because a response that fits in the send buffer is
 * accepted at once whatever the reader does. The bytes that reached it are
 * the accepted ones minus the ones still queued. That's true for TCP. For a
 * unix:// connection the kernel reports the memory the queued buffers take,
 * which is more than the bytes inside them, so there the floor sticks to the
 * accepted bytes and the queue only serves the gap.
 *
 * While the gap or the floor is on, the sweep reads every parked response's
 * queue on each tick, one ioctl per such connection, which is affordable for
 * a sweep that walks the parked list anyway. It makes that call after it has
 * claimed the connection and released idle_mutex (see
 * _parked_needs_kernel_query). */
static bool _parked_write_expired(struct chttpserver *srv, chttpsvr_conn_t *c,
                                  struct timespec now_slow, bool gap_hit) {
  unsigned rate = atomic_load(&srv->min_rate_bps);
  bool rate_on = rate != UINT_MAX && c->rate_active;
  if (!gap_hit && !rate_on) return false;
  size_t outq = _conn_send_queue(c);
  if (outq == SIZE_MAX)
    return gap_hit || _rate_below_floor(srv, c, now_slow, 0);
  if (outq < c->wp_outq) {
    c->wp_outq = outq;
    c->last_progress = now_slow;
    gap_hit = false;
  }
  if (!rate_on) return gap_hit;
  if (atomic_load(&srv->is_unix_socket))
    return gap_hit || _rate_below_floor(srv, c, now_slow, 0);
  uint64_t delivered =
      c->rate_bytes > (uint64_t)outq ? c->rate_bytes - (uint64_t)outq : 0;
  return gap_hit || _rate_elapsed_ms(c, now_slow) >
                        _rate_allowed_ms(rate, atomic_load(&srv->rate_grace_ms),
                                         delivered);
}

/* Whether _parked_expired would ask the kernel about c before deciding,
 * which it only does when the limits the connection carries itself don't
 * settle it: a parked response with the gap hit or the rate floor on, or a
 * parked body that looks stalled while bytes may be sitting below its
 * low-water mark. The sweep calls this with srv->idle_mutex held and it makes
 * no system call. A connection it returns true for is claimed first and
 * judged after the unlock, so a tick's kernel queries never run under the
 * lock every dispatch and every park takes. */
static bool _parked_needs_kernel_query(struct chttpserver *srv,
                                       const chttpsvr_conn_t *c,
                                       struct timespec now,
                                       struct timespec now_slow) {
  unsigned gap;
  if (c->park == _CONN_PARK_BODY) {
    if (c->read_deadline_set && _timespec_reached(now, c->read_deadline))
      return false;
    if (c->rcvlowat <= 1) return false;
    gap = atomic_load(&srv->stream_read_timeout_ms);
  } else if (c->park == _CONN_PARK_WRITE) {
    if (c->write_deadline_set && _timespec_reached(now, c->write_deadline))
      return false;
    gap = atomic_load(&srv->response_write_timeout_ms);
  } else {
    return false;
  }
  bool gap_hit = gap && _ms_since(now_slow, c->last_progress) >= (long long)gap;
  if (c->park == _CONN_PARK_WRITE)
    return gap_hit ||
           (atomic_load(&srv->min_rate_bps) != UINT_MAX && c->rate_active);
  return gap_hit || _rate_below_floor(srv, c, now_slow, c->rate_queued);
}

/* Whether a parked connection broke one of its limits. The caller owns c:
 * either it claimed c, or it's the sweep holding srv->idle_mutex for an
 * unowned connection in the parked list for which _parked_needs_kernel_query
 * is false. Either way it may read and update c's rate state.
 *
 * A parked body answers to stream_read_timeout_ms for the gap since its last
 * byte, to max_body_read_duration_ms for the whole read, and to the rate
 * floor. A parked response answers to response_write_timeout_ms,
 * max_response_write_duration_ms and the rate floor. The two total limits run
 * on CLOCK_MONOTONIC, the way the worker set them, and the gap and the floor
 * run on _slow_clock. */
static bool _parked_expired(struct chttpserver *srv, chttpsvr_conn_t *c,
                            struct timespec now, struct timespec now_slow) {
  unsigned gap;
  if (c->park == _CONN_PARK_BODY) {
    if (c->read_deadline_set && _timespec_reached(now, c->read_deadline))
      return true;
    gap = atomic_load(&srv->stream_read_timeout_ms);
  } else if (c->park == _CONN_PARK_WRITE) {
    if (c->write_deadline_set && _timespec_reached(now, c->write_deadline))
      return true;
    gap = atomic_load(&srv->response_write_timeout_ms);
  } else if (c->park == _CONN_PARK_LINGER) {
    return _timespec_reached(now, c->linger_deadline);
  } else {
    return false;
  }
  bool gap_hit = gap && _ms_since(now_slow, c->last_progress) >= (long long)gap;
  if (c->park == _CONN_PARK_WRITE)
    return _parked_write_expired(srv, c, now_slow, gap_hit);
  bool rate_hit = _rate_below_floor(srv, c, now_slow, c->rate_queued);
  if ((gap_hit || rate_hit) && c->rcvlowat > 1) {
    /* Bytes below the low-water mark wake nobody, but they're progress. */
#ifdef RUNNING_UNIT_TESTS
    _parked_query_note_for_tests();
#endif /* RUNNING_UNIT_TESTS */
    int queued = 0;
    if (ioctl(c->fd, FIONREAD, &queued) == 0 && queued > 0 &&
        (size_t)queued > c->rate_queued) {
      c->rate_queued = (size_t)queued;
      c->last_progress = now_slow;
      gap_hit = false;
      rate_hit = _rate_below_floor(srv, c, now_slow, c->rate_queued);
    }
  }
  return gap_hit || rate_hit;
}

/* Records when conn leaves a wait for the worker pool (see the comment of the
 * claim_mono field). */
static void _conn_stamp_claim(chttpsvr_conn_t *conn) {
  clock_gettime(CLOCK_MONOTONIC, &conn->claim_mono);
  _slow_clock(&conn->claim_slow);
}

/* Takes the time between conn's claim and now, which it spent in the worker
 * pool's queue behind other work, off every limit of the phase it resumes:
 * the rate floor, the gap since its last byte and the total read or write
 * duration. That way a client whose bytes have already arrived is never
 * charged for how long the server took to get to them. A body's memory wait
 * does the same for its own wait in _mem_dispatch. */
static void _conn_uncharge_queue_wait(chttpsvr_conn_t *conn,
                                      struct timespec now,
                                      struct timespec now_slow) {
  long long waited = _ms_since_ceil(now_slow, conn->claim_slow);
  if (waited > 0) {
    unsigned w = waited > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited;
    conn->rate_paused_ms += waited;
    _timespec_add_ms(&conn->last_progress, w);
  }
  long long waited_mono = _ms_since_ceil(now, conn->claim_mono);
  if (waited_mono > 0) {
    unsigned w =
        waited_mono > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited_mono;
    if (conn->read_deadline_set) _timespec_add_ms(&conn->read_deadline, w);
    if (conn->write_deadline_set) _timespec_add_ms(&conn->write_deadline, w);
  }
}

/* Body memory. */

/* Whether need more bytes fit under the limit right now. mem_in_use can be
 * above the limit while a connection runs past it under the progress rule,
 * hence the guarded subtraction. The caller holds srv->wait_mutex. */
static bool _mem_fits_locked(struct chttpserver *srv, size_t need) {
  size_t cap = atomic_load(&srv->mem_cap);
  return srv->mem_in_use <= cap && need <= cap - srv->mem_in_use;
}

#ifdef RUNNING_UNIT_TESTS
/* The highest mem_in_use any server has reached since the last reset, for the
 * test of the progress rule's bound. */
static _Atomic size_t g_mem_peak_for_tests = 0;
size_t _chttpsvr_mem_peak_for_tests(void) {
  return atomic_load(&g_mem_peak_for_tests);
}
void _chttpsvr_mem_peak_reset_for_tests(void) {
  atomic_store(&g_mem_peak_for_tests, 0);
}
#endif

#ifdef RUNNING_UNIT_TESTS
/* Since the last reset, the most that the body buffer of a request holding a
 * body memory reservation ever held beyond that reservation, counting the
 * old and new blocks of a growth together. */
static _Atomic size_t g_body_overshoot_for_tests = 0;
size_t _chttpsvr_body_overshoot_for_tests(void) {
  return atomic_load(&g_body_overshoot_for_tests);
}
void _chttpsvr_body_overshoot_reset_for_tests(void) {
  atomic_store(&g_body_overshoot_for_tests, 0);
}
static void _body_growth_note_for_tests(const chttpsvr_conn_t *conn,
                                        size_t old_cap, size_t new_cap) {
  if (conn->mem_charged == 0 || conn->mem_exempt) return;
  size_t held = old_cap + new_cap;
  if (held <= conn->mem_charged) return;
  size_t over = held - conn->mem_charged;
  size_t prev = atomic_load(&g_body_overshoot_for_tests);
  while (over > prev && !atomic_compare_exchange_weak(
                            &g_body_overshoot_for_tests, &prev, over)) {
  }
}
#endif

/* Charges need bytes to c. A charge above the whole limit counts as the
 * limit, since such a request can only ever run under the progress rule and
 * a charge of the full limit already keeps every other reservation out until
 * it ends. The sum saturates too, so mem_in_use never wraps whatever the
 * bodies declare (with max_body_size 0 a Content-Length can declare almost
 * SIZE_MAX bytes). */
static void _mem_grant_locked(struct chttpserver *srv, chttpsvr_conn_t *c,
                              size_t need) {
  size_t cap = atomic_load(&srv->mem_cap);
  if (need > cap) need = cap;
  if (need > SIZE_MAX - srv->mem_in_use) need = SIZE_MAX - srv->mem_in_use;
  if (need == 0) return;
  if (c->mem_charged == 0) srv->mem_holders++;
  c->mem_charged += need;
  srv->mem_in_use += need;
#ifdef RUNNING_UNIT_TESTS
  size_t peak = atomic_load(&g_mem_peak_for_tests);
  while (srv->mem_in_use > peak &&
         !atomic_compare_exchange_weak(&g_mem_peak_for_tests, &peak,
                                       srv->mem_in_use)) {
  }
#endif
}

/* Reserves need more bytes of body memory for conn (its owner holds it) if
 * they fit right now. A reservation never jumps ahead of a request that's
 * already waiting, so a stream of small requests can't starve a big one. A
 * connection running past the limit under the progress rule always gets its
 * reservation. */
static bool _mem_try_reserve(struct chttpserver *srv, chttpsvr_conn_t *conn,
                             size_t need) {
  _ccol_mutex_lock(srv->wait_mutex);
  bool ok = conn->mem_exempt || (!srv->mem_head && _mem_fits_locked(srv, need));
  if (ok) _mem_grant_locked(srv, conn, need);
  _ccol_mutex_unlock(srv->wait_mutex);
  return ok;
}

/* Puts conn (its owner holds it) at the tail of the memory-wait list. It's
 * the owner's last touch of conn, since from the unlock on an admission on
 * another thread may take it. */
static void _mem_wait_enqueue(struct chttpserver *srv, chttpsvr_conn_t *conn,
                              size_t need, uint8_t kind) {
  _slow_clock(&conn->mem_wait_start);
  conn->park = kind;
  conn->mem_need = need;
  _ccol_mutex_lock(srv->wait_mutex);
  conn->mem_prev = srv->mem_tail;
  conn->mem_next = NULL;
  if (srv->mem_tail)
    srv->mem_tail->mem_next = conn;
  else
    srv->mem_head = conn;
  srv->mem_tail = conn;
  conn->in_mem_wait = true;
  srv->mem_waiting++;
  if (conn->mem_charged) srv->mem_blocked++;
  _ccol_mutex_unlock(srv->wait_mutex);
}

/* Unlinks c from the memory-wait list, which is what claims it. The caller
 * holds srv->wait_mutex. */
static void _mem_wait_unlink_locked(struct chttpserver *srv,
                                    chttpsvr_conn_t *c) {
  if (c->mem_prev) c->mem_prev->mem_next = c->mem_next;
  if (c->mem_next) c->mem_next->mem_prev = c->mem_prev;
  if (srv->mem_head == c) srv->mem_head = c->mem_next;
  if (srv->mem_tail == c) srv->mem_tail = c->mem_prev;
  c->mem_prev = c->mem_next = NULL;
  c->in_mem_wait = false;
  srv->mem_waiting--;
  if (c->mem_charged) srv->mem_blocked--;
}

static void _mem_admit(struct chttpserver *srv);

/* Gives back conn's whole reservation and ends its progress-rule exemption.
 * _conn_drop_body calls it on conn's owner as it frees the body, because the
 * handler returned, the request was refused or the connection is closing.
 * Then it admits whatever the freed memory allows. */
static __attribute__((noinline, cold)) void _mem_release(
    chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->wait_mutex);
  if (conn->mem_charged) {
    srv->mem_in_use -= conn->mem_charged;
    srv->mem_holders--;
    conn->mem_charged = 0;
  }
  if (conn->mem_exempt) {
    conn->mem_exempt = false;
    srv->mem_exempt_active = false;
  }
  bool waiters = srv->mem_head != NULL;
  _ccol_mutex_unlock(srv->wait_mutex);
  if (waiters) _mem_admit(srv);
}

/* Gives back the part of conn's reservation above keep bytes, which a body
 * doesn't need once its buffer has grown and freed the old block, or while
 * it's parked. conn's owner calls it. */
static __attribute__((noinline, cold)) void _mem_trim(chttpsvr_conn_t *conn,
                                                      size_t keep) {
  struct chttpserver *srv = conn->srv;
  _ccol_mutex_lock(srv->wait_mutex);
  bool freed = false;
  if (conn->mem_charged > keep) {
    srv->mem_in_use -= conn->mem_charged - keep;
    if (keep == 0) srv->mem_holders--;
    conn->mem_charged = keep;
    freed = true;
  }
  bool waiters = freed && srv->mem_head != NULL;
  _ccol_mutex_unlock(srv->wait_mutex);
  if (waiters) _mem_admit(srv);
}

/* Once conn's body has stopped arriving, makes its reservation equal to what
 * the body buffer really holds. A body reserves its buffer's growth before
 * the read that needs it, and a read that brings fewer bytes than asked
 * leaves that growth reserved and unused. From here the reservation stays
 * exactly what the buffer holds until _task_finish frees the body after the
 * handler returns. A connection running past the limit under the progress
 * rule may hold more than its reservation, and keeps it. */
static inline __attribute__((always_inline)) void _mem_settle(
    chttpsvr_conn_t *conn) {
  if (__builtin_expect(conn->mem_charged > conn->body.cap, 0))
    _mem_trim(conn, conn->body.cap);
}

/* Hands an admitted waiter to the worker pool. A request that waited before
 * its body started goes to _task_worker as if it had just diverted, and a
 * body that waited for more memory goes to _task_resume. The wait is the
 * server's time, so neither the rate floor nor the body timers count it. A
 * full pool answers 503, since no response byte has gone out yet. */
static void _mem_dispatch(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  void (*fn)(void *) = _task_worker;
  if (conn->park == _CONN_PARK_MEM_BODY) {
    struct timespec now;
    _slow_clock(&now);
    long long waited = _ms_since_ceil(now, conn->mem_wait_start);
    if (waited < 0) waited = 0;
    conn->rate_paused_ms += waited;
    conn->last_progress = now;
    if (conn->read_deadline_set)
      _timespec_add_ms(&conn->read_deadline, waited > (long long)UINT_MAX
                                                 ? UINT_MAX
                                                 : (unsigned)waited);
    conn->park = _CONN_PARK_BODY;
    fn = _task_resume;
    _conn_stamp_claim(conn);
  } else {
    conn->park = _CONN_PARK_NONE;
  }
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
  _ccol_mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  ctpool pool = srv->worker_pool;
  _ccol_mutex_unlock(srv->mutex);
  if (!_srv_submit(srv, _SRV_POOL_WORKER, pool, fn, conn)) {
    conn->park = _CONN_PARK_NONE;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_reject_via_pool(conn);
  }
}

/* Admits waiters from the head of the memory-wait list, in the order they
 * started waiting, as long as the head fits under the limit.
 *
 * When the head doesn't fit and every connection that holds a reservation
 * is itself in this list, nothing holding memory can make progress and
 * nothing would ever free any. That's where the progress rule kicks in: the
 * head goes ahead past the limit and keeps reserving freely until its body
 * ends, and only one connection at a time may do that.
 *
 * Here's why nothing deadlocks. At any moment the list is non-empty, either
 * some holder is outside the list or none is. A holder outside the list is a
 * body being read, parked on its client, a request being refused, or a
 * handler running. A body that's arriving ends, by completing or by one of
 * the sweep's limits; a refusal ends within the internal-write ceiling; a
 * handler ends when it returns, which is the application's job, just like
 * for the worker-pool thread running it. Each of them then frees its body and
 * the release runs this function again. If no holder is outside the list,
 * the head is admitted right away, past the limit if need be, and becomes a
 * holder outside the list. A waiter not at the head moves up every time the
 * head leaves, by admission or by the sweep's deadline, so strict ordering
 * can't starve it either.
 *
 * A handler waiting for another request is the one dependency the server
 * can't see. If that other request's body doesn't fit next to the bodies
 * such handlers hold, it waits for memory until body_memory_wait_timeout
 * answers it with a 503, or, with no deadline, until one of those handlers
 * returns. The progress rule doesn't let it jump ahead, because as far as the
 * server can tell a holder whose handler is running is making progress, and
 * exempting it would let running handlers' bodies exceed the limit without
 * any bound.
 *
 * As for the cost, only one connection runs past the limit at a time, from
 * admission until its body is freed, and it can't reserve more than its own
 * body buffer plus the old buffer while a growth copies it. So the limit is
 * exceeded by less than twice max_body_size (or twice one declared
 * Content-Length when max_body_size is 0), and no other exemption starts
 * while it holds its body, since it's a holder outside the list.
 *
 * We never hold wait_mutex across a dispatch and never run two dispatch
 * loops at once: a call that finds another one in progress asks it to look
 * again and returns. A dispatch whose submit fails closes its connection,
 * and the release from that close arrives here as such a request. */
static void _mem_admit(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->wait_mutex);
  if (srv->mem_admitting) {
    srv->mem_admit_again = true;
    _ccol_mutex_unlock(srv->wait_mutex);
    return;
  }
  srv->mem_admitting = true;
  for (;;) {
    chttpsvr_conn_t *batch[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    srv->mem_admit_again = false;
    while (!srv->mem_admission_closed && srv->mem_head &&
           n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn_t *c = srv->mem_head;
      bool fits = _mem_fits_locked(srv, c->mem_need);
      bool ahead = !fits && !srv->mem_exempt_active &&
                   srv->mem_holders == srv->mem_blocked;
      if (!fits && !ahead) break;
      _mem_wait_unlink_locked(srv, c);
      _mem_grant_locked(srv, c, c->mem_need);
      if (ahead) {
        c->mem_exempt = true;
        srv->mem_exempt_active = true;
      }
      batch[n++] = c;
    }
    if (n == 0 && !srv->mem_admit_again) break;
    _ccol_mutex_unlock(srv->wait_mutex);
    for (size_t i = 0; i < n; i++) _mem_dispatch(batch[i]);
    _ccol_mutex_lock(srv->wait_mutex);
  }
  srv->mem_admitting = false;
  _ccol_mutex_unlock(srv->wait_mutex);
}

/* Stops _mem_admit from dispatching, for a server's teardown. */
static void _mem_admission_close(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->wait_mutex);
  srv->mem_admission_closed = true;
  _ccol_mutex_unlock(srv->wait_mutex);
}

/* Closes every waiter in the memory-wait list for a server's teardown,
 * claiming each one out of the list before closing it. */
static void _mem_wait_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *batch[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    _ccol_mutex_lock(srv->wait_mutex);
    while (srv->mem_head && n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn_t *c = srv->mem_head;
      _mem_wait_unlink_locked(srv, c);
      batch[n++] = c;
    }
    _ccol_mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
      batch[i]->park = _CONN_PARK_NONE;
      _conn_close(batch[i]);
    }
  }
}

/* The streaming pool. */

#ifdef RUNNING_UNIT_TESTS
/* Thread count of the streaming pool _stream_pool_get created last, or 0 if
 * none was created since the test last took the value. */
static _Atomic int g_stream_pool_threads_for_tests = 0;
/* Arming g_start_hold_armed_for_tests makes the next chttpsvr_start wait
 * right after it has swapped in the new run's pools and released srv->mutex,
 * until the test releases it, 10 s at most. */
static _Atomic bool g_start_hold_armed_for_tests = false;
static _Atomic bool g_start_hold_entered_for_tests = false;
static _Atomic bool g_start_hold_release_for_tests = false;

int _chttpsvr_take_stream_pool_threads_for_tests(void) {
  return atomic_exchange(&g_stream_pool_threads_for_tests, 0);
}

/* Arming this makes the next _stream_pool_get that builds a pool wait, after
 * it has read the size and before it creates the pool, until the test
 * releases it, 10 s at most. */
static _Atomic bool g_stream_create_hold_armed_for_tests = false;
static _Atomic bool g_stream_create_hold_entered_for_tests = false;
static _Atomic bool g_stream_create_hold_release_for_tests = false;

static void _stream_pool_create_hold_for_tests(void) {
  if (!atomic_exchange(&g_stream_create_hold_armed_for_tests, false)) return;
  atomic_store(&g_stream_create_hold_entered_for_tests, true);
  struct timespec ts = {0, 1000000L};
  for (int i = 0;
       i < 10000 && !atomic_load(&g_stream_create_hold_release_for_tests); i++)
    nanosleep(&ts, NULL);
}

void _chttpsvr_arm_stream_create_hold_for_tests(void) {
  atomic_store(&g_stream_create_hold_entered_for_tests, false);
  atomic_store(&g_stream_create_hold_release_for_tests, false);
  atomic_store(&g_stream_create_hold_armed_for_tests, true);
}

bool _chttpsvr_stream_create_hold_entered_for_tests(void) {
  return atomic_load(&g_stream_create_hold_entered_for_tests);
}

void _chttpsvr_release_stream_create_hold_for_tests(void) {
  atomic_store(&g_stream_create_hold_armed_for_tests, false);
  atomic_store(&g_stream_create_hold_release_for_tests, true);
}

void _chttpsvr_arm_start_hold_for_tests(void) {
  atomic_store(&g_start_hold_entered_for_tests, false);
  atomic_store(&g_start_hold_release_for_tests, false);
  atomic_store(&g_start_hold_armed_for_tests, true);
}

bool _chttpsvr_start_hold_entered_for_tests(void) {
  return atomic_load(&g_start_hold_entered_for_tests);
}

void _chttpsvr_release_start_hold_for_tests(void) {
  atomic_store(&g_start_hold_armed_for_tests, false);
  atomic_store(&g_start_hold_release_for_tests, true);
}

static void _start_hold_for_tests(void) {
  if (!atomic_exchange(&g_start_hold_armed_for_tests, false)) return;
  atomic_store(&g_start_hold_entered_for_tests, true);
  struct timespec ts = {0, 1000000};
  for (int i = 0; i < 10000 && !atomic_load(&g_start_hold_release_for_tests);
       i++)
    nanosleep(&ts, NULL);
}
#endif

/* Returns srv's streaming pool, creating it if this is the first streaming
 * request since the start. We create it on the first request rather than at
 * chttpsvr_start or at route registration, because its size comes from the
 * start's configuration and a server that registers streaming routes but
 * never gets a streaming request shouldn't start threads for them. Creation
 * happens outside srv->mutex, and a second creator that loses the race
 * destroys its own pool again. A server that isn't running, or that a
 * teardown has just detached, gets CTPOOL_INVALID. */
static ctpool _stream_pool_get(struct chttpserver *srv) {
  for (;;) {
    /* The size is read together with the run it belongs to. A start stores
     * the new streaming_thread_count before beginning a new run under
     * srv->mutex, so a pool built from this snapshot is only installed while
     * the run is unchanged, and a restart in between makes this call build a
     * pool for the new run instead. */
    _ccol_mutex_lock(srv->mutex);
    ctpool pool = srv->stream_pool;
    bool running = srv->worker_pool != CTPOOL_INVALID;
    uint64_t gen = atomic_load(&srv->stream_gen);
    int nthreads = atomic_load(&srv->stream_threads);
    _ccol_mutex_unlock(srv->mutex);
    if (pool || !running) return pool;
    if (nthreads <= 0) return CTPOOL_INVALID;

#ifdef RUNNING_UNIT_TESTS
    _stream_pool_create_hold_for_tests();
#endif
    char *err = NULL;
    ctpool fresh = ctpool_create_mp((size_t)nthreads, 0, srv->m_procs, &err);
    if (!fresh) return CTPOOL_INVALID;
    _ccol_mutex_lock(srv->mutex);
    bool same_run = atomic_load(&srv->stream_gen) == gen;
    if (same_run && !srv->stream_pool && srv->worker_pool != CTPOOL_INVALID) {
      srv->stream_pool = fresh;
      fresh = CTPOOL_INVALID;
    }
    pool = srv->stream_pool;
    _ccol_mutex_unlock(srv->mutex);
    if (fresh) {
      ctpool_shutdown_drain(fresh);
      ctpool_destroy(fresh);
      if (!same_run) continue;
    }
#ifdef RUNNING_UNIT_TESTS
    else if (pool)
      atomic_store(&g_stream_pool_threads_for_tests, nthreads);
#endif
    return pool;
  }
}

/* Answers 503 to a request no pool can take, a streaming request the
 * streaming pool refuses or a parked body the worker pool refuses. The
 * connection already holds its in_flight_requests slot and is in the
 * diverted list, just like a request the worker pool refuses at divert
 * time. */
static void _conn_reject_busy(chttpsvr_conn_t *conn) {
  conn->park = _CONN_PARK_NONE;
  conn->req_rejected = true;
  conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
  _conn_reject_via_pool(conn);
}

static void _stream_unlink_locked(struct chttpserver *srv, chttpsvr_conn_t *c) {
  if (c->sq_prev) c->sq_prev->sq_next = c->sq_next;
  if (c->sq_next) c->sq_next->sq_prev = c->sq_prev;
  if (srv->sq_head == c) srv->sq_head = c->sq_next;
  if (srv->sq_tail == c) srv->sq_tail = c->sq_prev;
  c->sq_prev = c->sq_next = NULL;
  c->in_stream_queue = false;
  srv->sq_len--;
}

static void _stream_token_task(void *arg);
static void _stream_queue_flush(struct chttpserver *srv);

/* Puts a streaming request into the streaming queue, stamped with its
 * deadline. The connection keeps its in_flight_requests slot, its place in
 * the diverted list and its paused registration, so every teardown sees it as
 * an in-flight request, but it holds no thread. A full queue answers 503
 * immediately, and the dispatch never blocks.
 *
 * The queue is drained by streaming-pool tasks that each take the oldest
 * request, run it and take the next, until the queue is empty (see
 * _stream_token_task). sq_tokens counts those tasks, and a new one is only
 * submitted while fewer are running than the pool has threads, so the pool's
 * internal queue never holds more tasks than threads, and only while the
 * queue holds more requests than sq_unclaimed counts tasks on their way to
 * it. */
/* What a _stream_token_task task receives: its server and the stream_gen
 * value from when the task was counted in sq_tokens. */
typedef struct {
  struct chttpserver *srv;
  uint64_t gen;
} _stream_token_arg_t;

/* Starts _stream_token_task tasks on the current run's streaming pool while
 * the queue holds more requests than the current run has tasks that haven't
 * taken one yet, and fewer such tasks are counted than its pool has threads,
 * so a queued request costs at most one new task. The pool is read after
 * stream_gen, so a task counted for one run never goes to an earlier run's
 * pool; a pool newer than the count just makes the task pass the queue on.
 * sq_tokens only counts the current run's tasks. A restart or a stop resets
 * it together with stream_gen, so an earlier run's task still busy with a long
 * request never keeps the current run's pool idle. When no pool takes the task
 * and no other task of the current run drains the queue, every request in it
 * gets a 503. */
static bool _stream_token_start_one(struct chttpserver *srv);

static void _stream_token_start(struct chttpserver *srv) {
  while (_stream_token_start_one(srv)) {
  }
}

/* One step of _stream_token_start. It starts one task and returns true when
 * the queue holds more requests than the current run has tasks that haven't
 * taken one yet and the run counts fewer tasks than its pool has threads,
 * and false otherwise or when the task can't start. */
#ifdef RUNNING_UNIT_TESTS
/* White-box counter of the streaming token tasks submitted by all servers of
 * the process. */
static _Atomic size_t g_stream_tokens_started_for_tests = 0;

size_t _chttpsvr_stream_tokens_started_for_tests(void) {
  return atomic_load(&g_stream_tokens_started_for_tests);
}
#endif

static bool _stream_token_start_one(struct chttpserver *srv) {
  size_t nthreads = (size_t)atomic_load(&srv->stream_threads);
  _ccol_mutex_lock(srv->wait_mutex);
  bool submit = srv->sq_head != NULL && srv->sq_unclaimed < srv->sq_len &&
                srv->sq_tokens < nthreads;
  uint64_t gen = atomic_load(&srv->stream_gen);
  if (submit) {
    srv->sq_tokens++;
    srv->sq_unclaimed++;
  }
  _ccol_mutex_unlock(srv->wait_mutex);
  if (!submit) return false;
  ctpool pool = _stream_pool_get(srv);
  _stream_token_arg_t *arg =
      pool ? (_stream_token_arg_t *)_ccol_mem_alloc(srv->m_procs, sizeof(*arg))
           : NULL;
  if (arg) {
    arg->srv = srv;
    arg->gen = gen;
    /* From here on a request may already be running on the streaming
     * pool. */
    if (_srv_submit(srv, _SRV_POOL_STREAM, pool, _stream_token_task, arg)) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_stream_tokens_started_for_tests, 1);
#endif
      return true;
    }
    _ccol_mem_free(srv->m_procs, arg);
  }
  /* No pool took the task, which only happens with a pool in shutdown, no
   * running server or a failed allocation. If no other task of this run
   * drains the queue, nothing would ever pick up what waits in it, so every
   * waiter gets answered now. After a restart or a stop the count belongs to
   * the next run, which starts its own task. */
  _ccol_mutex_lock(srv->wait_mutex);
  bool same_run = atomic_load(&srv->stream_gen) == gen;
  if (same_run) {
    srv->sq_tokens--;
    srv->sq_unclaimed--;
  }
  bool orphaned = same_run && srv->sq_tokens == 0;
  _ccol_mutex_unlock(srv->wait_mutex);
  if (orphaned)
    _stream_queue_flush(srv);
  else if (!same_run)
    _stream_token_start(srv);
  return false;
}

static void _stream_enqueue(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ctpool pool = _stream_pool_get(srv);
  if (!pool) {
    _conn_reject_busy(conn);
    return;
  }
  size_t qcap = atomic_load(&srv->stream_qcap);
  unsigned tmo = atomic_load(&srv->stream_qtimeout_ms);
  conn->park = _CONN_PARK_STREAM;
  conn->sq_has_deadline = tmo != UINT_MAX;
  if (conn->sq_has_deadline) {
    _slow_clock(&conn->sq_deadline);
    _timespec_add_ms(&conn->sq_deadline, tmo);
  }

  _ccol_mutex_lock(srv->wait_mutex);
  if (qcap != SIZE_MAX && srv->sq_len >= qcap) {
    _ccol_mutex_unlock(srv->wait_mutex);
    _conn_reject_busy(conn);
    return;
  }
  conn->sq_prev = srv->sq_tail;
  conn->sq_next = NULL;
  if (srv->sq_tail)
    srv->sq_tail->sq_next = conn;
  else
    srv->sq_head = conn;
  srv->sq_tail = conn;
  conn->in_stream_queue = true;
  srv->sq_len++;
  _ccol_mutex_unlock(srv->wait_mutex);
  /* From here on conn may already be running on the streaming pool. */
  _stream_token_start(srv);
}

/* One streaming-pool task. It takes the oldest queued request, runs it, and
 * takes the next, until the queue is empty. Taking a request is a claim under
 * srv->wait_mutex, the same way the sweep claims an expired one, so a request
 * is answered by one of the two and never by both. A request whose deadline
 * passed while it waited gets a 503 here, through the same rejection path,
 * whose write never waits on the peer.
 *
 * A streaming handler runs on a thread of this pool and nowhere else. It may
 * block as long as its client takes, within the configured read limits, and
 * meanwhile it holds one streaming thread and never a worker-pool thread. The
 * thread counts as a worker of its server, so the calls a handler mustn't
 * make on its own server (chttpsvr_destroy, a restart, chttpsvr_engine_wait)
 * are refused or fatal here exactly like on the worker pool. */
static void _stream_token_task(void *arg) {
  _stream_token_arg_t a = *(_stream_token_arg_t *)arg;
  struct chttpserver *srv = a.srv;
  _ccol_mem_free(srv->m_procs, arg);
  _chttpsvr_mark_worker_thread(srv);
  /* The task stays counted in sq_unclaimed until it takes its first
   * request. */
  bool first = true;
  for (;;) {
#ifdef RUNNING_UNIT_TESTS
    _stream_claim_hook_wait_if_armed(1);
#endif
    _ccol_mutex_lock(srv->wait_mutex);
    if (atomic_load(&srv->stream_gen) != a.gen) {
      /* This task's run is over (a restart or a stop) and the next run's
       * count doesn't include it, so it takes no more requests and the queue
       * goes to the current run's pool. That way the current run's thread
       * count applies and this pool can drain and go away. */
      _ccol_mutex_unlock(srv->wait_mutex);
      _stream_token_start(srv);
      return;
    }
    chttpsvr_conn_t *c = srv->sq_head;
    if (first) srv->sq_unclaimed--;
    first = false;
    if (!c) {
      srv->sq_tokens--;
      _ccol_mutex_unlock(srv->wait_mutex);
      return;
    }
    _stream_unlink_locked(srv, c);
    _ccol_mutex_unlock(srv->wait_mutex);
#ifdef RUNNING_UNIT_TESTS
    _stream_claim_hook_wait_if_armed(2);
#endif
    struct timespec now;
    _slow_clock(&now);
    if (c->sq_has_deadline && _timespec_reached(now, c->sq_deadline)) {
      c->park = _CONN_PARK_NONE;
      c->req_rejected = true;
      c->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
      _conn_reject_and_close(c, /*run_middleware=*/false);
      _release_in_flight(srv);
      continue;
    }
    c->park = _CONN_PARK_NONE;
    _task_worker(c);
  }
}

/* Answers every queued streaming request with 503. A teardown calls it, and
 * so does _stream_token_start_one when the queue has lost every task that
 * drains it. */
static void _stream_queue_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *batch[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    _ccol_mutex_lock(srv->wait_mutex);
    while (srv->sq_head && n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn_t *c = srv->sq_head;
      _stream_unlink_locked(srv, c);
      batch[n++] = c;
    }
    _ccol_mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) _conn_reject_busy(batch[i]);
  }
}

/* The part of a sweep tick no idle list reaches: requests waiting for body
 * memory and streaming requests waiting for a thread. Each expired one is
 * claimed out of its list under srv->wait_mutex and then answered with a 503
 * and a Retry-After header. A memory waiter has no in_flight_requests slot,
 * so it goes through _conn_dispatch_reject, which takes one; a queued
 * streaming request already has its slot. */
static void _sweep_parked_waits(struct chttpserver *srv,
                                struct timespec now_slow) {
  /* All expired waiters of both lists go in one tick, without a cap, each
   * chained through the link field of the list it left, the same as in
   * _sweep_server. */
  unsigned mem_ms = atomic_load(&srv->mem_wait_ms);
  chttpsvr_conn_t *expired = NULL;
  bool any = false;
  _ccol_mutex_lock(srv->wait_mutex);
  if (mem_ms != UINT_MAX) {
    chttpsvr_conn_t *c = srv->mem_head;
    while (c) {
      chttpsvr_conn_t *next = c->mem_next;
      if (_ms_since(now_slow, c->mem_wait_start) >= (long long)mem_ms) {
        _mem_wait_unlink_locked(srv, c);
        c->mem_next = expired;
        expired = c;
      }
      c = next;
    }
  }
  _ccol_mutex_unlock(srv->wait_mutex);
  while (expired) {
    chttpsvr_conn_t *c = expired;
    expired = c->mem_next;
    c->mem_next = NULL;
    any = true;
    c->park = _CONN_PARK_NONE;
    c->req_rejected = true;
    c->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_dispatch_reject(c);
  }
  /* A waiter that just left may have been what kept the progress rule from
   * applying (see _mem_admit). */
  if (any) _mem_admit(srv);

  _ccol_mutex_lock(srv->wait_mutex);
  chttpsvr_conn_t *q = srv->sq_head;
  while (q) {
    chttpsvr_conn_t *next = q->sq_next;
    if (q->sq_has_deadline && _timespec_reached(now_slow, q->sq_deadline)) {
      _stream_unlink_locked(srv, q);
      q->sq_next = expired;
      expired = q;
    }
    q = next;
  }
  _ccol_mutex_unlock(srv->wait_mutex);
  while (expired) {
    chttpsvr_conn_t *c = expired;
    expired = c->sq_next;
    c->sq_next = NULL;
    _conn_reject_busy(c);
  }
}

/* What a Content-Length body of declared bytes, carry of which came with the
 * headers, reserves before a worker reads it: the capacity its buffer
 * reaches, under _body_grow_target's rule, to hold the carry-over plus one
 * 8 KiB read window. Every later growth of the buffer is reserved as the
 * bytes come in (see _drain_body_socket_read). */
static size_t _body_first_reserve(size_t declared, size_t carry) {
  size_t need = carry > SIZE_MAX - 8192 ? SIZE_MAX : carry + 8192;
  if (need > declared) need = declared;
  size_t first = _CHTTPSVR_BODY_FIRST_CAP;
  while (first < need) {
    if (first > SIZE_MAX / 2) return need;
    first *= 2;
  }
  return first < declared ? first : declared;
}

/* The cold half of _conn_start_diverted, for a request whose divert_gate is
 * above the carry-over (see the comment of the divert_gate field). Returns
 * true when it took the connection over, false when the ordinary divert
 * carries on. The caller has already counted the request in
 * in_flight_requests, put the connection in the diverted list, paused its
 * registration and saved the carry-over. */
static __attribute__((noinline, cold)) bool _conn_divert_gate(
    chttpsvr_conn_t *conn) {
  size_t gate = conn->divert_gate;
  if (gate == SIZE_MAX) {
    _stream_enqueue(conn);
    return true;
  }
  struct chttpserver *srv = conn->srv;
  size_t first = _body_first_reserve(gate, conn->_carry_over_len);
  if (_mem_try_reserve(srv, conn, first)) return false;
  /* The first buffer doesn't fit. The request waits without a thread, and
   * nobody reads a byte of its body or answers "100 Continue" until memory
   * frees up (see _mem_admit). */
  _diverted_list_remove(conn);
  _mem_wait_enqueue(srv, conn, first, _CONN_PARK_MEM_START);
  _release_in_flight(srv);
  _mem_admit(srv);
  return true;
}

/* Parking and resuming. */

/* _drain_body's body-read half once the carry-over is empty. It reads the
 * socket without waiting. When the socket has nothing it sets park_pending to
 * _CONN_PARK_BODY and returns -1, and the caller gives the connection up
 * instead of waiting. A body first makes sure its body memory reservation
 * covers this read; if it can't, it sets park_pending to _CONN_PARK_MEM_BODY
 * and returns -1 without reading, so the connection waits for memory without
 * taking a single byte past its reservation. A Content-Length body reserved
 * its first buffer before the worker started (see _conn_divert_gate).
 *
 * A request's first call starts the phase the rate floor and the per-gap
 * limit measure. */
static __attribute__((noinline, cold)) ssize_t _drain_body_socket_read(
    chttpsvr_conn_t *conn, chttp1_stream_t *stream, char *raw, size_t len) {
  struct chttpserver *srv = conn->srv;
  if (!conn->rate_active) _rate_start(conn, conn->body_bytes_seen);
  size_t cap = conn->body.cap;
  /* The capacity this read could need. _on_body refuses every byte past
   * max_body_size before growing the buffer, so a read near the limit never
   * needs more than the limit. Without the clamp the target would double past
   * it, and a legal body would ask for memory it never uses and might wait
   * for it until its memory deadline. */
  size_t limit = atomic_load(&srv->max_body_size);
  size_t need = conn->body.len + len;
  if (need < conn->body.len) need = SIZE_MAX;
  if (limit && need > limit) need = limit < cap ? cap : limit;
  /* A Content-Length body never holds more than its declared length, since
   * the parser hands every byte after that to the next request, so the same
   * clamp applies. The parser counts down the bytes it hasn't handed over
   * yet. */
  if (chttp1_has_content_length(&conn->parser)) {
    uint64_t rest = chttp1_declared_content_length(&conn->parser);
    if (rest < (uint64_t)(need - conn->body.len)) {
      size_t d = conn->body.len + (size_t)rest;
      need = d < cap ? cap : d;
    }
  }
  if ((need > cap || conn->mem_charged < cap) && !conn->options_star &&
      atomic_load(&srv->mem_cap) != SIZE_MAX) {
    /* A body reserves the memory its buffer holds before the read that needs
     * it. The buffer may hold bytes that came with the headers, read before a
     * chunked body reserved anything, and this read may make it grow. A
     * growth holds the old and the new block together until the copy ends,
     * so the reservation covers both, and _on_body gives the old part back
     * once the copy is done. */
    size_t target = _body_grow_target(conn, cap, need, limit);
    size_t want = target <= cap             ? cap
                  : target > SIZE_MAX - cap ? SIZE_MAX
                                            : target + cap;
    if (conn->mem_charged < want) {
      size_t step = want - conn->mem_charged;
      if (!_mem_try_reserve(srv, conn, step)) {
        conn->mem_need = step;
        conn->park_pending = _CONN_PARK_MEM_BODY;
        return -1;
      }
    }
  }
  ssize_t n = chttp1_stream_read(stream, raw, len, 0);
  if (n > 0) {
    conn->rate_bytes += (uint64_t)n;
    conn->rate_queued = 0;
    _slow_clock(&conn->last_progress);
  } else if (n < 0 && chttp1_stream_timed_out(stream)) {
    conn->park_pending = _CONN_PARK_BODY;
  }
  return n;
}

/* Arms conn's registration for the direction the park needs and publishes
 * conn into the idle list with park set. The caller owns conn and this is its
 * last touch of it. The registration is paused while a worker owns the
 * connection, so usually this is a modify plus a resume; a connection whose
 * first request was settled before it ever had a registration gets a fresh
 * one, the same as in _task_tail's keep-alive tail. Returns false, leaving
 * conn out of every list, when the reactor refuses, and the caller then
 * closes it. */
static bool _conn_park_arm(chttpsvr_conn_t *conn, uint8_t kind) {
  ccol_select_dir dir;
  if (kind == _CONN_PARK_BODY) {
    dir = (conn->tls && ctls_conn_wants_write(conn->tls)) ? ccol_select_write
                                                          : ccol_select_read;
    _conn_set_rcvlowat(conn);
  } else if (kind == _CONN_PARK_LINGER) {
    dir = ccol_select_read;
    _conn_reset_rcvlowat(conn);
  } else {
    dir = conn->wp_wants_read ? ccol_select_read : ccol_select_write;
    _conn_reset_rcvlowat(conn);
    conn->wp_outq = _conn_send_queue(conn);
  }
  conn->park = kind;
  conn->state = CONN_ST_READING_HEADERS;
  if (conn->reg) {
    if (_conn_reg_modify(conn, dir) != ccol_success ||
        ccol_event_loop_resume(srv_engine_bundler.reactor, conn->reg) !=
            ccol_success) {
      conn->park = _CONN_PARK_NONE;
      return false;
    }
  } else {
    char *err = NULL;
    conn->reg = ccol_event_loop_add(
        srv_engine_bundler.reactor, ccol_selectable_from_fd(conn->fd, dir),
        (ccol_event_handlers_t){.on_readable = _conn_on_readable,
                                .on_writable = _conn_on_writable,
                                .on_error = _conn_on_error,
                                .on_removed = _conn_on_removed},
        conn, &err);
    if (!conn->reg) {
      conn->park = _CONN_PARK_NONE;
      return false;
    }
    conn->reg_dir = (uint8_t)dir;
    /* See the comments of conn->lifetime_refs and srv->lifetime_refs. */
    atomic_fetch_add(&conn->lifetime_refs, 1);
    atomic_fetch_add(&conn->srv->lifetime_refs, 1);
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_park_count_for_tests, 1);
  if (kind < 8) atomic_fetch_add(&g_park_kind_count_for_tests[kind], 1);
#endif
  _parked_list_add(conn);
  return true;
}

/* On the worker that was reading it, gives up a connection whose body
 * stopped arriving or that has to wait for memory. The stream holds nothing
 * worth keeping: the body reader only touches the socket once the carry-over
 * is empty, and a pipelined next request can only follow a complete body.
 * After this the connection holds no thread and no in_flight_requests slot,
 * and the reactor, or a body memory admission, hands it to a worker
 * again. */
static __attribute__((noinline, cold)) void _task_park_body(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req) {
  uint8_t kind = conn->park_pending;
  conn->park_pending = _CONN_PARK_NONE;
  _destroy_req_scratch(req);
  chttp1_stream_release(stream);
  _diverted_list_remove(conn);
  if (kind == _CONN_PARK_MEM_BODY) {
    _mem_wait_enqueue(srv, conn, conn->mem_need, _CONN_PARK_MEM_BODY);
    _release_in_flight(srv);
    _mem_admit(srv);
    return;
  }
  /* A parked body only holds what its buffer holds: the growth it reserved
   * for a read that brought nothing goes back, and the next read reserves it
   * again. A body without a buffer yet keeps its first reservation (see
   * _conn_divert_gate). */
  if (conn->mem_charged > conn->body.cap && conn->body.cap)
    _mem_trim(conn, conn->body.cap);
  if (!_conn_park_arm(conn, _CONN_PARK_BODY)) {
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }
  _release_in_flight(srv);
}

/* Hands a parked connection the reactor just claimed on readiness to any free
 * worker (see _task_resume). Like _conn_start_diverted it pauses the
 * registration first, so no more readiness fires while a worker owns the
 * connection. With a full pool a parked body gets a 503, since no response
 * byte has gone out yet, and a parked response, already partly sent, is
 * closed. */
static __attribute__((noinline, cold)) void _conn_resume_parked(
    chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _conn_stamp_claim(conn);
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
  _ccol_mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  ctpool pool = srv->worker_pool;
  _ccol_mutex_unlock(srv->mutex);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_resume_dispatch_count_for_tests, 1);
#endif
  if (_srv_submit(srv, _SRV_POOL_WORKER, pool, _task_resume, conn)) return;
  if (conn->park == _CONN_PARK_WRITE) {
    conn->park = _CONN_PARK_NONE;
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }
  _conn_reject_busy(conn);
}

/* The lingering close. When the server closes a connection while its client
 * is still sending, typically the body of a refused request, the kernel
 * answers those bytes with a reset. A reset can wipe out the response in the
 * client's receive buffer before the client reads it, and a client still busy
 * writing only ever sees the reset. So the server shuts down its writing
 * side, which delivers the end of the response followed by an end of stream,
 * and reads and throws away whatever the client keeps sending, for at most
 * _CHTTPSVR_LINGER_MS, before closing, just like nginx's lingering_close. The
 * bound is a time and not a byte count: a client that sends a refused
 * upload's whole body before reading the response, which many clients do,
 * only reads that response once the server has taken every byte of the body,
 * and a byte budget smaller than the body would close the connection under
 * that client and turn the response into a reset.
 *
 * A lingering connection holds no thread. It sits in the parked list with its
 * registration armed for reading, the reactor throws away what arrives, and
 * the sweep closes it at its deadline. Until it closes it still counts
 * against max_connections. One dispatch discards at most
 * _CHTTPSVR_LINGER_STEP_BYTES and leaves the rest for the next readiness
 * report, so a fast sender can't hold the reactor thread in one dispatch. */
#define _CHTTPSVR_LINGER_MS 2000u
#define _CHTTPSVR_LINGER_STEP_BYTES ((size_t)256 * 1024)

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_linger_count_for_tests = 0;
size_t _chttpsvr_linger_count_for_tests(void) {
  return atomic_load(&g_linger_count_for_tests);
}
#endif

/* Whether conn's client may still send bytes the server hasn't read: the rest
 * of a body, pipelined bytes already waiting in the carry-over (such a client
 * may still be sending), or anything already queued in the socket. */
static bool _conn_request_bytes_may_remain(chttpsvr_conn_t *conn) {
  if (chttp1_request_bytes_may_remain(&conn->parser)) return true;
  if (conn->_carry_over_len != 0) return true;
  int queued = 0;
  return ioctl(conn->fd, FIONREAD, &queued) == 0 && queued > 0;
}

/* Reads and throws away what the client sent, without waiting, up to
 * _CHTTPSVR_LINGER_STEP_BYTES per call. Returns true while the linger goes
 * on, i.e. the socket is empty or the step budget is used up, and false once
 * the client has closed or the socket failed. */
static bool _linger_discard(chttpsvr_conn_t *conn) {
  char buf[16384];
  size_t taken = 0;
  while (taken < _CHTTPSVR_LINGER_STEP_BYTES) {
    ssize_t n = read(conn->fd, buf, sizeof(buf));
    if (n > 0) {
      taken += (size_t)n;
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    return n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK);
  }
  return true;
}

/* Starts conn's lingering close instead of _conn_close. The caller owns conn,
 * has it in no list other than the diverted list, and doesn't touch it
 * again. A TLS connection sends its close_notify first and its TLS state goes
 * along with it; nothing else gets written, and whatever the client still
 * sends is thrown away unread. */
static __attribute__((noinline, cold)) void _conn_linger(
    chttpsvr_conn_t *conn) {
  /* Nobody reads the request or the response past this point, so both, and
   * the body memory reservation, go now rather than at the close, which may
   * be up to _CHTTPSVR_LINGER_MS away. */
  _conn_drop_request_state(conn);
  _destroy_resp(&conn->resp, conn->m_procs);
  if (conn->tls) {
    ctls_conn_destroy(conn->tls);
    conn->tls = NULL;
  }
  if (shutdown(conn->fd, SHUT_WR) != 0) {
    _conn_close(conn);
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->linger_deadline);
  _timespec_add_ms(&conn->linger_deadline, _CHTTPSVR_LINGER_MS);
  if (!_linger_discard(conn)) {
    _conn_close(conn);
    return;
  }
  _diverted_list_remove(conn);
  if (!_conn_park_arm(conn, _CONN_PARK_LINGER)) {
    _conn_close(conn);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_linger_count_for_tests, 1);
#endif
}

/* The reactor has claimed a lingering connection on readiness. Throw away
 * what arrived, then either park the connection again with its registration
 * still armed or close it. */
static __attribute__((noinline, cold)) void _conn_linger_step(
    chttpsvr_conn_t *conn) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (_linger_discard(conn) && !_timespec_reached(now, conn->linger_deadline)) {
    _parked_list_add(conn);
    return;
  }
  conn->park = _CONN_PARK_NONE;
  _conn_close(conn);
}

/* Saves the state of a response whose write ran into a full socket, for
 * _task_continue_write. head and head_len are the unsent part of the header
 * block, which lives on _send_response's stack and so gets copied; body_pos is
 * how much of the body has gone out already. It also moves any pipelined
 * bytes of a next request out of the stream onto the connection, since the
 * caller releases the stream. The write's rate phase starts where the write
 * started: with a total-duration bound on, the write's deadline records that
 * instant, and otherwise this call is the best estimate. internal marks one of
 * the library's own writes, a rejection or the interim line, whose bound is
 * capped by _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. If the copy fails
 * wp_active stays false and the caller closes the connection as for any
 * failed write. */
static __attribute__((noinline, cold)) void _send_response_park(
    chttpsvr_conn_t *conn, chttp1_stream_t *stream, const char *head,
    size_t head_len, size_t body_pos, bool keep_alive, bool no_body,
    bool internal) {
  char *copy = NULL;
  if (head_len) {
    copy = (char *)_ccol_mem_alloc(conn->m_procs, head_len);
    if (!copy) return;
    memcpy(copy, head, head_len);
  }
  conn->wp_head = copy;
  conn->wp_head_len = head_len;
  conn->wp_head_pos = 0;
  conn->wp_body_pos = body_pos;
  conn->wp_keep_alive = keep_alive;
  conn->wp_no_body = no_body;
  conn->wp_wants_read = stream->tls && chttp1_stream_timed_out(stream) &&
                        !ctls_conn_wants_write((ctls_conn_t *)stream->tls);
  conn->wp_active = true;
  size_t lo_len = 0;
  char *lo = chttp1_stream_take_leftover(stream, &lo_len);
  if (lo) {
    _ccol_mem_free(conn->m_procs, conn->_carry_over);
    conn->_carry_over = lo;
    conn->_carry_over_len = lo_len;
  } else if (lo_len) {
    /* The pipelined bytes of a next request couldn't be saved and are lost,
     * so the connection closes when the parked write ends rather than wait
     * for a request its client has already sent. */
    conn->wp_keep_alive = false;
  }
  _rate_start(conn, body_pos);
  /* The write's deadline is one total-duration bound after its start, and
   * one of the library's own writes has _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS
   * as a ceiling over that bound (see _response_write_deadline_ok). */
  unsigned max_dur = atomic_load(&conn->srv->max_response_write_duration_ms);
  if (internal && (max_dur == 0 || max_dur > _internal_write_ceiling_ms()))
    max_dur = _internal_write_ceiling_ms();
  if (conn->write_deadline_set && max_dur) {
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    long long run =
        (long long)max_dur - _ms_since_ceil(conn->write_deadline, now_mono);
    if (run > 0) _timespec_sub_ms(&conn->rate_start, run);
  }
}

/* Gives up a connection whose response ran into a full socket (see
 * _send_response_park). It waits in the idle list for room to write while the
 * sweep enforces the write limits and the rate floor on it. */
static __attribute__((noinline, cold)) void _conn_park_write(
    chttpsvr_conn_t *conn, struct chttpserver *srv) {
  _diverted_list_remove(conn);
  if (!_conn_park_arm(conn, _CONN_PARK_WRITE)) {
    conn->wp_active = false;
    _ccol_mem_free(conn->m_procs, conn->wp_head);
    conn->wp_head = NULL;
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }
  _release_in_flight(srv);
}

/* Drops a parked response's saved state once its write has ended. */
static void _conn_end_parked_write(chttpsvr_conn_t *conn) {
  conn->wp_active = false;
  _ccol_mem_free(conn->m_procs, conn->wp_head);
  conn->wp_head = NULL;
  conn->wp_head_len = conn->wp_head_pos = conn->wp_body_pos = 0;
}

/* Writes what's left of a parked response without waiting. Returns 1 once
 * the whole response is out, 0 when the write failed or a write limit
 * expired, and -1 when the socket is full again. */
static int _send_response_continue(chttp1_stream_t *stream,
                                   chttpsvr_conn_t *conn) {
  size_t blen = conn->wp_no_body ? 0 : conn->resp.body_len;
  for (;;) {
    size_t alen = conn->wp_head_len - conn->wp_head_pos;
    size_t brem = blen - conn->wp_body_pos;
    if (alen + brem == 0) return 1;
    unsigned call_timeout_ms = 0;
    /* A rejection and the interim line keep the ceiling of the library's
     * internal writes (see _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS). */
    if (!_response_write_deadline_ok(conn, conn->wp_reject || conn->wp_interim,
                                     &call_timeout_ms))
      return 0;
    ssize_t n = chttp1_stream_writev2(
        stream, conn->wp_head ? conn->wp_head + conn->wp_head_pos : NULL, alen,
        brem ? conn->resp.body + conn->wp_body_pos : NULL, brem, 0);
    if (n <= 0) {
      if (n < 0 && chttp1_stream_timed_out(stream)) {
        conn->wp_wants_read =
            stream->tls && !ctls_conn_wants_write((ctls_conn_t *)stream->tls);
        return -1;
      }
      return 0;
    }
    size_t moved = (size_t)n;
    size_t in_head = moved < alen ? moved : alen;
    conn->wp_head_pos += in_head;
    conn->wp_body_pos += moved - in_head;
    conn->rate_bytes += (uint64_t)n;
    _slow_clock(&conn->last_progress);
  }
}

/* ========================================================================== */
/*                    WORKER THREAD: BODY READ + HANDLER + RESPONSE           */
/* ========================================================================== */

/* What _drain_body returns when the body stopped arriving or needs to wait for
 * memory, and the caller must park the connection instead of waiting.
 * _drain_body never returns this value for any other reason. */
#define _CHTTPSVR_BODY_PARKED ccol_container_empty

static bool _rate_floor_shrink(chttpsvr_conn_t *conn,
                               unsigned *timeout_ms_inout);

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

/* _check_read_deadline for _drain_body, which runs for every buffered body,
 * so the whole check stays inline. */
static inline __attribute__((always_inline)) bool _check_read_deadline_inl(
    chttpsvr_conn_t *conn, unsigned *timeout_ms_inout) {
  unsigned max_dur = atomic_load(&conn->srv->max_body_read_duration_ms);
  if (!_shrink_timeout_to_deadline_inl(&conn->read_deadline,
                                       &conn->read_deadline_set, max_dur,
                                       timeout_ms_inout)) {
    conn->deadline_exceeded = true;
    return false;
  }
  return true;
}

/* Drains a buffered route's whole request body with chttp1_stream_read and
 * chttp1_parser_execute, both driven by the calling worker thread. It's only
 * called for buffered routes; _task_worker never calls it for a streaming
 * one.
 *
 * A streaming route's handler pulls its own body with chttpsvr_req_read()
 * instead, and body bytes the handler leaves unread never come here for a
 * separate drain. We just can't keep that connection alive (see
 * _task_worker's msg_fully_parsed and keep_alive computation), because
 * otherwise the unread bytes would be read as the start of the next
 * pipelined request.
 *
 * Returns ccol_success or a specific failure the caller maps to a status
 * code. */
static ccol_retval_t _drain_body(chttpsvr_conn_t *conn,
                                 chttp1_stream_t *stream) {
  for (;;) {
    if (chttp1_parser_message_complete(&conn->parser)) break;
    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!_check_read_deadline_inl(conn, &timeout_ms)) return ccol_timed_out;
    char raw[8192];
    /* The carry-over holds the body bytes that came with the headers, and
     * reading it never waits. Once it's empty, _drain_body_socket_read reads
     * the socket without waiting and asks the caller to park the connection
     * when there's nothing (see the comment of the park_pending field). */
    ssize_t n = chttp1_stream_has_carry(stream)
                    ? chttp1_stream_read(stream, raw, sizeof(raw),
                                         _to_stream_timeout_ms(timeout_ms))
                    : _drain_body_socket_read(conn, stream, raw, sizeof(raw));
    if (n < 0) {
      if (conn->park_pending) return _CHTTPSVR_BODY_PARKED;
      if (chttp1_stream_timed_out(stream)) return ccol_timed_out;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) {
      /* A truncated body: the peer stopped before its own message's framing
       * was complete. That's a malformed request, not a server fault; see the
       * comment of conn->body_malformed. */
      conn->body_malformed = true;
      return ccol_http_transfer_aborted;
    }
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_PAUSED) {
      /* This message is done, but raw may hold more than it needed: extra
       * bytes that came off the wire in the same read and belong to a
       * pipelined next request right behind this one.
       * chttp1_parser_consumed() says exactly where this message's framing
       * ended, and everything past that must go back onto stream's
       * carry-over. Otherwise the pipelined next request silently vanishes,
       * since those bytes are already gone from the kernel's socket receive
       * buffer for good, and the client waits forever for a response that
       * never comes. The other half of this is the doc comment of
       * chttp1_stream_take_leftover and _task_worker's call into it right
       * before releasing this stream. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(stream, raw + consumed,
                                            (size_t)n - consumed)) {
        /* The next request's bytes are lost, so the connection closes after
         * this response instead of waiting for a request its client has
         * already sent. */
        chttp1_parser_force_close(&conn->parser);
      }
      break;
    }
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* A chunk may declare a size above max_chunk_size_override (see
       * _conn_reset_for_request). We report that like a cumulative body over
       * the limit (conn->body_too_large and ccol_msg_too_large) rather than
       * in the general transfer-aborted group where every other malformed
       * chunk-framing error goes, because for the caller it's the same "body
       * too large" outcome, only caught before a single byte of that chunk
       * arrives. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      /* A CHTTP1_ERROR here is the parser refusing the body bytes themselves:
       * a chunk size that isn't hexadecimal, a chunk without its terminating
       * CRLF, a malformed trailer line. That's a client error, and
       * conn->body_malformed is what turns it into a 400 instead of a 500
       * further up. We only set it after ruling out the conditions that have
       * a status of their own, so it doesn't disturb the priority below. A
       * CHTTP1_USER is never a syntax fault here; it's one of this file's
       * callbacks reporting a condition that has already set its own flag. */
      if (r == CHTTP1_ERROR && !conn->body_too_large &&
          !conn->body_alloc_failed)
        conn->body_malformed = true;
      /* A real allocation failure in _on_body sets conn->body_alloc_failed,
       * which is also reported as its own outcome and never in the general
       * transfer-aborted group, with the same priority
       * chttpsvr_req_stream_error() uses on the streaming-route path (see
       * the comment of body_alloc_failed). */
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
  /* conn->matched_route is NULL for the req a middleware sees on a rejection
   * we answer ourselves (404, 405, or a 413 found at the headers), because
   * no route matched; such a request has no body to read, so nothing is
   * read. A req used beyond its documented lifetime, the duration of the
   * handler call, also fails here instead of crashing. Only a streaming
   * route reads its body through this call. */
  if (!conn || !conn->matched_route) return -1;
  if (!conn->matched_route->is_streaming) return -1;
  if (!req->stream) return -1;

  /* The lazy Expect: 100-continue interim send for a streaming route (the
   * comment of _task_worker says why it's deferred to here rather than sent
   * before the handler runs). It fires at most once, on whichever
   * chttpsvr_req_read() call comes first for this request.
   *
   * A handler may never call chttpsvr_req_read() and reject the request
   * outright instead, and then it rightly never triggers this send. The
   * client's Expect: 100-continue logic gets the final rejection directly
   * instead of a "100 Continue" telling it to upload a body the server was
   * never going to read.
   *
   * We also skip the send, even on this first call, when the message never
   * had a body, i.e. no Content-Length and no chunked Transfer-Encoding.
   * chttp1_parser already hit message-complete at header-parse time then,
   * since CHTTP1_HEADERS_DIVERT_BODY turns into an immediate completion when
   * there's nothing to divert, so there's no body left to invite. A "100
   * Continue" here would be the same mistake the reject-without-reading case
   * above avoids, just reached another way, with a bodyless request instead
   * of a rejecting handler.
   *
   * interim_continue_sent still gets set every time, so this check isn't
   * repeated on every later chttpsvr_req_read() call for the same
   * request. */
  if (conn->expects_continue && !conn->interim_continue_sent) {
    conn->interim_continue_sent = true;
    if (!chttp1_parser_message_complete(&conn->parser)) {
      unsigned wtimeout_ms = atomic_load(&conn->srv->response_write_timeout_ms);
      /* See the doc comment of _write_interim_continue. A TOTAL write failure
       * leaves the connection untouched, so the read below fails as well
       * and reports through this function's ordinary error path either way.
       * A real SHORT write is different: it leaves a truncated status line on
       * the wire that no client can parse, which a plain read failure below
       * could never notice or account for.
       *
       * So both outcomes are handled the same. We latch them onto conn, so
       * _task_worker's later _send_response is suppressed and no real
       * response ever follows a possibly corrupt interim one, and we report
       * it to this streaming handler as an ordinary transfer-aborted error
       * through the immediate return below, exactly how the handler already
       * treats any other failure that ends a stream. */
      if (_write_interim_continue(conn, req->stream, wtimeout_ms) != 1) {
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
    /* The rate floor bounds this wait too, so the read gives up at the moment
     * this body's average rate would drop below the floor. */
    if (!_rate_floor_shrink(conn, &timeout_ms)) {
      conn->deadline_exceeded = true;
      return -1;
    }
    char raw[8192];
    ssize_t n = chttp1_stream_read(req->stream, raw, sizeof(raw),
                                   _to_stream_timeout_ms(timeout_ms));
    if (n > 0) conn->rate_bytes += (uint64_t)n;
    if (n < 0) {
      if (chttp1_stream_timed_out(req->stream)) {
        /* One of two things happened. Either the per-call poll(2) of
         * stream_read_timeout_ms timed out, or _check_read_deadline above
         * folded max_body_read_duration_ms's deadline into this call's
         * timeout and that deadline expired inside the poll. That function's
         * up-front check on the NEXT call can't catch the second case,
         * because there is no next call: chttpsvr_req_read returns right
         * here.
         *
         * We report it the same way _drain_body does on the buffered-route
         * path, through the conn->deadline_exceeded flag
         * chttpsvr_req_stream_error() reads, so both caps give the same
         * ccol_timed_out. See
         * max_body_read_duration_exceeded_reports_ccol_timed_out and
         * stream_read_timeout_reports_ccol_timed_out in tests.c. */
        conn->deadline_exceeded = true;
      } else {
        /* A hard I/O error, not a timeout: the peer reset the connection, a
         * raw read() or ctls_conn_read() failed, or something like that. It
         * belongs to the same "connection closed or malformed framing" group
         * the two branches below report through conn->transfer_aborted. */
        conn->transfer_aborted = true;
      }
      return -1;
    }
    if (n == 0) {
      /* The peer closed its write side, or the whole connection, before the
       * declared or chunked framing said the body was done. That's a
       * truncated body and not the natural end of the message, which is the
       * case where chttp1_parser_message_complete() returns true above and
       * is handled separately.
       *
       * Without this flag chttpsvr_req_stream_error() couldn't report the
       * truncation: it would fall through to ccol_success and tell a
       * streaming handler a truncated upload was a clean read. For a POST
       * with Content-Length: 100 whose peer sends 20 bytes and half-closes,
       * the handler would see chttpsvr_req_read() return -1 while
       * chttpsvr_req_stream_error() says ccol_success. */
      conn->transfer_aborted = true;
      return -1;
    }
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* Malformed framing, like a bad chunk-size line. By now the
       * max_body_size check in _on_body may already have set body_too_large
       * for this same CHTTP1_USER or CHTTP1_ERROR, and a chunk whose declared
       * size exceeds max_chunk_size_override sets it here too, for the same
       * reason (see the same check in _conn_reset_for_request and
       * _drain_body). chttpsvr_req_stream_error() reads body_too_large first,
       * so setting both flags is harmless and keeps that priority. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      conn->transfer_aborted = true;
      return -1;
    }
    if (r == CHTTP1_PAUSED) {
      /* Same reasoning as the identical check in _drain_body: raw may hold
       * more than this message needed, and the extra bytes belong to a
       * pipelined next request already read off the wire. We push the
       * unconsumed tail back onto req->stream's carry-over, where
       * _task_worker's later chttp1_stream_take_leftover() picks it up.
       * Without that push those bytes would vanish, since the
       * message_complete check above returns 0 at once on the next pass
       * once conn->body is drained. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(req->stream, raw + consumed,
                                            (size_t)n - consumed)) {
        /* Same failure as in _drain_body. */
        chttp1_parser_force_close(&conn->parser);
      }
    }
    /* For CHTTP1_OK or CHTTP1_PAUSED the loop goes back and drains what
     * _on_body just appended to conn->body. */
  }
}

ccol_retval_t chttpsvr_req_stream_error(const chttpsvr_req *req) {
  if (!req || !req->conn) return ccol_invalid_args;
  chttpsvr_conn_t *conn = req->conn;
  if (conn->deadline_exceeded) return ccol_timed_out;
  if (conn->body_too_large) return ccol_msg_too_large;
  if (conn->body_alloc_failed) return ccol_not_enough_memory;
  if (conn->transfer_aborted) return ccol_http_transfer_aborted;
  return ccol_success;
}

/* What's left of a request after its response went out or was given up: the
 * pipelined next request gets reclaimed, and then the connection is either
 * closed or handed back to the reactor. _task_worker, _task_resume and the
 * continuation of a parked response all end here. It's always inlined so an
 * ordinary request's path compiles as one function. */
static inline __attribute__((always_inline)) void _task_tail(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req, bool prepared, bool keep_alive, bool response_out) {
  /* Reclaims the bytes of a further pipelined request on this connection,
   * already pulled off the wire, which chttp1_stream_release below would
   * otherwise always throw away. Bytes end up here in two situations.
   *
   * One is when this message completed before _drain_body or
   * chttpsvr_req_read had to touch stream at all, like a bodyless GET or HEAD
   * or an explicit Content-Length: 0. Then the whole original
   * conn->_carry_over the stream was prepared with is still sitting here
   * untouched.
   *
   * The other is when a read that drained the body swept up extra bytes past
   * this message's framing boundary. _drain_body's and chttpsvr_req_read's
   * CHTTP1_PAUSED handling pushes exactly that back onto stream's
   * carry-over, and this call picks it up the same way as in the first case.
   *
   * We reclaim whenever prepared is true, which is cheap and does no I/O, but
   * only keep the bytes when the connection stays alive and free them
   * otherwise, because a closing connection has nobody to hand pipelined
   * bytes to. That matches the documented "reject and close" precedent of
   * pipelined_bytes_after_rejected_route_not_misparsed. */
  size_t reclaimed_len = 0;
  char *reclaimed =
      prepared ? chttp1_stream_take_leftover(stream, &reclaimed_len) : NULL;

  if (prepared) chttp1_stream_release(stream);
  _destroy_req_scratch(req);

  if (!keep_alive) _ccol_mem_free(conn->m_procs, reclaimed);

  /* in_flight_requests is released, by the _release_in_flight at the bottom
   * of every exit path below, only once the connection is in a state
   * _drain_and_close_all_connections can observe: fully closed through
   * _conn_close, or safely published back into the idle list. Releasing it
   * here, before either, would open exactly the use-after-free window the
   * comment of _release_in_flight describes. Every _release_in_flight call in
   * this function uses srv, a local read earlier that stays valid even after
   * conn is freed below. */

  if (!keep_alive) {
    /* A response that stopped on a full socket isn't finished, so the
     * connection waits for room to write the rest (see _conn_park_write). */
    if (__builtin_expect(conn->wp_active, 0)) {
      _conn_drop_request_state(conn);
      _conn_park_write(conn, srv);
      return;
    }
    /* A complete response on a connection whose client may still be sending
     * (an unread body, pipelined bytes already read, or bytes waiting in the
     * socket) ends with a lingering close; see _conn_linger. */
    if (__builtin_expect(response_out && (reclaimed_len != 0 ||
                                          _conn_request_bytes_may_remain(conn)),
                         0))
      _conn_linger(conn);
    else
      _conn_close(conn);
    _release_in_flight(srv);
    return;
  }

  /* The keep-alive path: reset the per-request state and hand the connection
   * back to the reactor to read the next request's headers.
   *
   * conn->reg is only non-NULL here when this request went through a live
   * ccol_event_loop registration that _conn_start_diverted then paused (see
   * that function's comment). Resuming it is far cheaper than a fresh
   * ccol_event_loop_add, because the event_entry, the fd registry chmap entry
   * and the epoll_ctl(ADD) all stayed in place and only the combined epoll
   * interest mask needs recomputing.
   *
   * conn->reg is NULL here in two cases worth telling apart. One is a pause
   * that really failed and removed the registration (see _conn_pause_reg).
   * The other is much more common: _conn_pump's optimistic first read right
   * after the accept read this connection's headers synchronously, sometimes
   * several requests' worth of pipelined bytes, before the connection was
   * ever registered with the reactor, since _listener_on_readable calls
   * _conn_pump directly without a ccol_event_loop_add in between.
   *
   * This branch's "there never was a registration" case and
   * _conn_start_diverted's "pause failed" case both give the same NULL, and
   * nothing here can tell them apart by design, because both need exactly the
   * same fresh ccol_event_loop_add fallback below. So it's not a gap, just two
   * paths with one outcome. */
  /* This worker is done with conn as a worker-owned connection for now, so
   * it's no longer a candidate for _force_unblock_diverted_connections. The
   * pipelined-bytes handling below may divert a further request on the same
   * connection, in which case _conn_start_diverted's idempotent
   * _diverted_list_add puts conn back. Otherwise it stays out of the list
   * until some later divert cycle. */
  _diverted_list_remove(conn);
  _conn_reset_for_request(conn);
  conn->state = CONN_ST_READING_HEADERS;

  if (reclaimed) {
    /* chttp1_stream_take_leftover() allocated these bytes from conn->m_procs,
     * the allocator the stream was prepared with and the one
     * conn->_carry_over always comes from, so conn takes over the buffer as
     * it is, without a copy. A non-NULL result always has at least one
     * byte. */
    conn->_carry_over = reclaimed;
    conn->_carry_over_len = reclaimed_len;
  } else if (__builtin_expect(reclaimed_len != 0, 0)) {
    /* Copying the next request's pipelined bytes ran out of memory, so those
     * bytes are gone. A kept-alive connection would wait for a request its
     * client has already sent, and closing it tells the client to send it
     * again. */
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }

  if (conn->_carry_over_len > 0) {
    /* The next request's bytes are already in memory, not just available for
     * a later read but gone from the kernel's socket receive buffer for good.
     * Waiting for a fresh epoll readiness event here, which is what the
     * plain resume or add path below does, would wait forever for data that
     * never comes.
     *
     * So we feed them straight into the parser we just reset, right now on
     * this worker thread, just like _conn_pump does when real socket bytes
     * come in (see the doc comment of _conn_feed_bytes). That's what prevents
     * the pipelining data loss the doc comment of chttp1_stream_take_leftover
     * describes. Without this feed the bytes would be thrown away, and
     * without chttp1_stream_take_leftover at all chttp1_stream_release above
     * would free them with no way to get them back. */
    char *lo = conn->_carry_over;
    size_t lo_len = conn->_carry_over_len;
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    /* mp comes from srv, a local read earlier that stays valid after conn is
     * freed below, instead of reading conn->m_procs after the call. By the
     * time _conn_feed_bytes returns it may already have settled conn's fate:
     * diverted it to another worker thread that races us to the end and
     * frees it, rejected it through a synchronous fallback close, or closed
     * it outright on a raw parse error. Its own contract says the caller must
     * not touch conn again, and reading conn->m_procs is exactly such a
     * touch.
     *
     * conn->m_procs and srv->m_procs are always the same pointer (see
     * _conn_create), so reading from srv is safe and equivalent, while reading
     * conn->m_procs here would be a real use-after-free. It would happen
     * every time the further pipelined bytes are malformed, since
     * _conn_feed_bytes then closes and frees conn synchronously on this
     * thread before returning, and every time reject_pool's fallback fires
     * synchronously. In all other cases it's a race against another worker
     * thread finishing and freeing conn before we get to this free. */
    ccol_memmgmt_procs_t *mp = srv->m_procs;
    bool need_more = _conn_feed_bytes(conn, lo, lo_len);
    _ccol_mem_free(mp, lo);
    if (!need_more) {
      /* _conn_feed_bytes has already settled conn's fate: it diverted a
       * further pipelined request to a worker, rejected it, or closed the
       * connection. A divert can go to this very pool, but always through a
       * fresh ctpool_submit and never a direct recursive call. We only
       * release this call's in_flight_requests slot here, the one taken when
       * THIS request was first diverted; whatever _conn_feed_bytes just
       * started owns its own independent slot. */
      _release_in_flight(srv);
      return;
    }
    /* CHTTP1_OK means the parser now holds part of the next request's
     * headers, exactly the state a live socket read leaves, so we fall
     * through to the TLS drain and then the ordinary "wait for more"
     * registration logic below. */
  }

  /* A TLS connection may hold more of the next request inside the TLS layer
   * rather than in the socket. OpenSSL reads a whole record, up to 16 KB of
   * plaintext, a pipelining client packs several requests into one record,
   * and each read of this request only took what its buffer could hold.
   * epoll(7) reports nothing for those bytes, so a registration here would
   * wait for data that's already in memory, and the connection would stall
   * until the client sends something else or the idle timeout closes it.
   *
   * So we keep reading on this worker thread while the TLS layer holds input,
   * feeding each read into the parser like _conn_pump's read loop does. The
   * carry-over above goes first, since those bytes come before everything
   * the TLS layer still holds. The loop ends on EWOULDBLOCK, and only then
   * does the registration below wait for the socket. */
  ccol_select_dir tail_dir = ccol_select_read;
  while (conn->tls && ctls_conn_has_pending_input(conn->tls)) {
    char tls_buf[8192];
    ssize_t n = ctls_conn_read(conn->tls, tls_buf, sizeof(tls_buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* See the doc comment of ctls_conn_wants_write(). The read may need the
       * socket to become writable first, and then the registration below
       * waits in that direction. */
      if (ctls_conn_wants_write(conn->tls)) tail_dir = ccol_select_write;
      break;
    }
    if (n <= 0) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
    if (!_conn_feed_bytes(conn, tls_buf, (size_t)n)) {
      /* Same as the matching branch of the carry-over feed above. */
      _release_in_flight(srv);
      return;
    }
  }

  if (conn->reg) {
    /* Force the registration back to the read direction before resuming it.
     * This site only exists to wait for the next request's bytes, always the
     * read direction, but conn->reg's recorded direction (ccol_event_reg's
     * sel.dir) may still be write from an earlier cycle.
     *
     * That happens in one specific sequence: the last successful read on this
     * connection, the one that completed the request that just finished
     * above, came after a write-direction wait in _conn_pump's read loop
     * (driven by ctls_conn_wants_write), and the connection was diverted to
     * this worker on that same read. Nothing in between flips the
     * registration back to read, since that only happens on a LATER read
     * attempt that hits EWOULDBLOCK and wants the read direction.
     *
     * ccol_event_loop_modify is safe on a paused registration. It only
     * updates the registration's recorded direction and the read or write
     * slot of the underlying fd entry, and delivery stays suppressed until
     * ccol_event_loop_resume arms it again. So when we resume, conn->reg is
     * always in tail_dir's direction, which is read unless the TLS drain
     * above ended on a read that needs the socket writable first.
     * _conn_reg_modify skips the call entirely when the recorded direction is
     * already tail_dir, the common keep-alive case, so an ordinary request
     * only pays for the resume.
     *
     * Without it we'd get one extra, wasted write-readiness dispatch: a TCP
     * send buffer is almost always writable immediately, so a stale
     * write-direction resume fixes itself through a spurious re-dispatch that
     * arrives almost immediately, instead of hanging. But only if the
     * registration really has an on_writable handler; the else branch below
     * covers the real hang this pairs with.
     *
     * The modify isn't expected to fail since reg was known to be live a
     * moment ago. We check it anyway rather than assume, the same discipline
     * this function follows for every other ccol_event_loop call. */
    if (_conn_reg_modify(conn, tail_dir) != ccol_success) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
    if (ccol_event_loop_resume(srv_engine_bundler.reactor, conn->reg) !=
        ccol_success) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
  } else {
    char *err = NULL;
    /* .on_writable is needed here for the same reason _conn_pump's two
     * ccol_event_loop_add call sites have it. This connection has no live
     * registration yet, because its very first request was handled fully
     * synchronously through _listener_on_readable's optimistic first read and
     * parse, without ever waiting on the reactor. But a LATER keep-alive
     * request on the same connection may still need write-direction interest
     * mid-read, as ctls_conn_wants_write decides (see its doc comment).
     *
     * _conn_pump's read loop then calls ccol_event_loop_modify(conn->reg,
     * ccol_select_write) on this exact registration, and that succeeds since
     * a modify only changes the epoll interest and never the handlers. With
     * no on_writable handler, ccol_event_loop silently drops the later
     * write-readiness event, because its documented behaviour for a direction
     * without a handler pointer is a no-op, and the connection stalls for
     * good until some unrelated timeout steps in, one the operator may have
     * set to 0, i.e. off.
     *
     * That's a real, reproducible gap. Every registration this file creates
     * for a live connection's fd must have both handlers, because any of them
     * may later need either direction. */
    conn->reg = ccol_event_loop_add(
        srv_engine_bundler.reactor, ccol_selectable_from_fd(conn->fd, tail_dir),
        (ccol_event_handlers_t){.on_readable = _conn_on_readable,
                                .on_writable = _conn_on_writable,
                                .on_error = _conn_on_error,
                                .on_removed = _conn_on_removed},
        conn, &err);
    if (!conn->reg) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
    conn->reg_dir = (uint8_t)tail_dir;
    /* See the comments of conn->lifetime_refs and srv->lifetime_refs (the
     * latter gets the same treatment at the server level). */
    atomic_fetch_add(&conn->lifetime_refs, 1);
    atomic_fetch_add(&srv->lifetime_refs, 1);
  }
  _idle_list_add(conn);
  _release_in_flight(srv);
}

/* What's left of a request after its body is read: the response, then
 * _task_tail. */
static inline __attribute__((always_inline)) void _task_finish(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req, bool prepared, bool aborted, unsigned write_timeout_ms) {
  /* The handler has returned, or will never run, so nobody reads the body
   * again (see _conn_drop_body). */
  _conn_drop_body(conn);
  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large && !_srv_draining(srv);

  /* The !conn->interim_write_failed test matters. A real short write of the
   * "100 Continue" interim line has already left a truncated status line on
   * the wire (see the doc comment of _write_interim_continue), and writing a
   * real response on top would corrupt the client's framing further, while
   * against a peer that's already gone it's pointless anyway. So we close the
   * connection and never try the real send. */
  /* A handler's response never waits for a slow reader on this thread; a
   * full socket parks the connection instead (see _send_response_park). */
  stream->write_nonblocking = true;
  bool sent = prepared && !conn->interim_write_failed &&
              _send_response(stream, &conn->resp, keep_alive, write_timeout_ms,
                             conn->method == CHTTP_HEAD, conn,
                             /*is_reject=*/false);
  if (!sent) keep_alive = false;

  _task_tail(conn, srv, stream, req, prepared, keep_alive, sent);
}

static void _task_worker(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _chttpsvr_mark_worker_thread(srv);

  /* A request that was waiting in the worker pool's queue when a teardown
   * started doesn't get a handler either. It's answered just like one
   * _conn_start_diverted's dispatch finds draining, with 503 and
   * Connection: close, so the teardown's wait ends when the handlers already
   * running return, however deep the queue is. The request still counts in
   * in_flight_requests, which _conn_reject_via_pool releases once the close
   * completes; only its headers were read. */
  if (__builtin_expect(_srv_draining(srv), 0)) {
    _ccol_mem_free(conn->m_procs, conn->_carry_over);
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_reject_via_pool(conn);
    return;
  }

  chttp1_stream_t stream;
  bool prepared;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_stream_prepare_fail_for_tests)) {
    prepared = false;
  } else
#endif /* RUNNING_UNIT_TESTS */
    if (conn->tls)
      prepared = chttp1_stream_prepare_tls(
          &stream, conn->fd, conn->tls, conn->_carry_over,
          conn->_carry_over_len, conn->m_procs);
    else
      prepared = chttp1_stream_prepare(&stream, conn->fd, conn->_carry_over,
                                       conn->_carry_over_len, conn->m_procs);
  /* chttp1_stream_prepare() and chttp1_stream_prepare_tls() fail ONLY when
   * copying a non-empty carry-over hits an allocation failure, and a NULL and
   * 0 leftover can never fail (see their doc comments). So we retry without
   * the carry-over, and the connection still gets a real stream it can write
   * to.
   *
   * Leaving prepared false here would shut every response path below, since
   * each one checks prepared, and the client would see the connection drop
   * with zero response bytes instead of a graceful error. For a streaming
   * route the handler would still run (see how body_unavailable is used
   * below) against a stream it can never read from, with
   * chttpsvr_req_stream_error() having no way to say why.
   *
   * The dropped carry-over bytes are lost either way, and body_unavailable
   * below forces a clean abort and close for this request, so retrying
   * without them loses nothing more. */
  bool body_unavailable = !prepared;
  if (body_unavailable) {
    prepared =
        conn->tls
            ? chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                                        conn->m_procs)
            : chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  }
  /* Every connection of this server is non-blocking, so each read and write
   * on this stream tries the I/O first and only polls when the socket is
   * empty or full (see chttp1_stream_t.fd_nonblocking). */
  stream.fd_nonblocking = true;
  _ccol_mem_free(conn->m_procs, conn->_carry_over);
  conn->_carry_over = NULL;
  conn->_carry_over_len = 0;

  /* response_write_timeout_ms is loaded once up front because two places need
   * it, the buffered route's interim write below and the real response send
   * further down (see chttp1_should_keep_alive and _send_response).
   *
   * The interim write goes out here on the worker thread and not from
   * _conn_pump on the reactor thread, and that placement is what makes it
   * correct by construction without any extra guard. _task_worker only runs
   * for a request that already passed _on_headers_complete's route match,
   * while a rejected route never diverts and goes through
   * _conn_dispatch_reject and _conn_reject_via_pool instead, so we don't need
   * a !conn->req_rejected guard. A request that hits the pool-full 503 path
   * never reaches _task_worker either, so no spurious "100 Continue" can come
   * before a 503. Sending from _conn_pump as soon as the headers are done,
   * before ctpool_try_submit's capacity check, would break both of these.
   *
   * The write never waits on the client: a full socket parks the connection
   * with the rest of the line and the body is read once it's out (see
   * _write_interim_continue and _task_continue_interim). */
  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  /* Buffered routes only. A buffered route's handler never runs before the
   * whole body is read (see _drain_body below), so it never gets a chance to
   * reject the request before the body arrives, whether the interim response
   * goes out here or later, and sending it now instead of deferring costs
   * this kind of route nothing.
   *
   * A streaming route's handler is different. It can decide to reject a
   * request, for bad authentication or an unacceptable Content-Type say,
   * without ever calling chttpsvr_req_read(). That's exactly the case
   * Expect: 100-continue (RFC 7231 SS5.1.1) exists to make cheap for the
   * client: the server answers with a final status instead of "100 Continue"
   * and never receives the body at all.
   *
   * Sending here for every route before the handler runs would quietly
   * defeat that, telling the client to upload the body before a streaming
   * handler gets its chance to reject without reading. So for a streaming
   * route the interim response is sent lazily from chttpsvr_req_read()
   * itself, the first time the handler asks to read the body, if it ever
   * does.
   *
   * For a buffered route we also skip the send when the message never had a
   * body, in which case chttp1_parser_message_complete() is already true
   * here since there's no Content-Length and no chunked Transfer-Encoding
   * (see the same guard in chttpsvr_req_read() for why). There's
   * no body left to invite, and a "100 Continue" would ask the client to
   * upload one that was never coming. */
  /* See the options_star branch below. A declared body over the "OPTIONS *"
   * bound is never read, so the client is never invited to send it. */
  bool options_star_body_refused =
      conn->options_star && chttp1_has_content_length(&conn->parser) &&
      chttp1_declared_content_length(&conn->parser) >
          (uint64_t)_CHTTPSVR_OPTIONS_STAR_MAX_BODY;
  if (prepared && !body_unavailable && conn->expects_continue &&
      !conn->matched_route->is_streaming && !options_star_body_refused &&
      !chttp1_parser_message_complete(&conn->parser)) {
    /* The line never waits for a client that doesn't read. A full socket
     * parks the connection and the body is read once the line is out (see
     * _task_resume). */
    stream.write_nonblocking = true;
    int ic = _write_interim_continue(conn, &stream, write_timeout_ms);
    conn->interim_continue_sent = true;
    if (ic < 0) {
      chttp1_stream_release(&stream);
      _conn_park_write(conn, srv);
      return;
    }
    if (ic == 0) conn->interim_write_failed = true;
  }

  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = prepared ? &stream : NULL;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval_t body_err = ccol_success;
  /* The code above may set conn->interim_write_failed, meaning the interim
   * "100 Continue" write already left a possibly truncated line on the wire.
   * Then this connection is always closed below and never gets a response
   * (see the !conn->interim_write_failed check on `sent` further down),
   * whatever happens here. Draining a body whose outcome nobody sees would
   * just tie up this worker thread for nothing, and so would running the
   * handler for a request that can never be answered, so we skip both. */
  bool aborted = conn->interim_write_failed;
  if (!aborted && conn->options_star) {
    /* "OPTIONS *" is answered here with no middleware and no handler: a 200
     * with Content-Length 0, which conn->resp already holds, exactly Go's
     * net/http answer. A body of at most _CHTTPSVR_OPTIONS_STAR_MAX_BODY
     * bytes is read and thrown away and the connection then follows the
     * ordinary keep-alive rules, while a declared Content-Length above that
     * bound isn't read at all. Any failure to read the body, an over-bound
     * body included, still answers 200 but closes the connection afterwards,
     * since the next request no longer has a known starting point. */
    bool body_ok = prepared && !body_unavailable && !options_star_body_refused;
    if (body_ok) {
      ccol_retval_t star_err = _drain_body(conn, &stream);
      if (star_err == _CHTTPSVR_BODY_PARKED) {
        _task_park_body(conn, srv, &stream, &req);
        return;
      }
      body_ok = (star_err == ccol_success);
    }
    conn->resp.status_code = CHTTP_STATUS_OK;
    aborted = !body_ok;
  } else if (!aborted) {
    if (body_unavailable) {
      /* The allocation failure above lost this request's body, and the
       * handler must never be called with a stream that can't deliver it,
       * buffered route or streaming. The comment of body_unavailable above
       * says why this mustn't depend on the kind of route, unlike the
       * ordinary body_err-from-_drain_body case below. */
      body_err = ccol_not_enough_memory;
    } else if (prepared) {
      if (!conn->matched_route->is_streaming) {
        body_err = _drain_body(conn, &stream);
        if (body_err != _CHTTPSVR_BODY_PARKED) _mem_settle(conn);
      }
      /* A streaming route's handler pulls the body itself with
       * chttpsvr_req_read(), so there's nothing to drain ahead of it. */
    } else {
      /* Nothing gets here in practice, since the NULL and 0 retry above can
       * never fail (see the comment of body_unavailable), but we fail closed
       * anyway instead of assuming. */
      body_err = ccol_not_enough_memory;
    }

    if (body_err != ccol_success &&
        (body_unavailable || !conn->matched_route->is_streaming)) {
      /* The body stopped arriving or must wait for memory, so this thread
       * gives the connection up and a later worker carries it on. */
      if (body_err == _CHTTPSVR_BODY_PARKED) {
        _task_park_body(conn, srv, &stream, &req);
        return;
      }
      /* conn->body_malformed tells a client whose body doesn't match its own
       * framing apart from a genuine server-side failure, and only the latter
       * is a 500 (see that flag's comment). */
      conn->resp.status_code =
          (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
          : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
          : conn->body_malformed           ? CHTTP_STATUS_BAD_REQUEST
                                           : CHTTP_STATUS_INTERNAL_ERROR;
      aborted = true;
    } else {
      _chttpsvr_next(&req, &conn->resp);
      /* A streaming handler may stop reading before the body's natural end,
       * or run into its own error where chttpsvr_req_read returns -1, and
       * either way there's nothing more to drain. Whether the connection is
       * kept alive or closed below already takes a not fully drained body
       * into account, through chttp1_should_keep_alive's message-completion
       * check. */
    }
  }

  _task_finish(conn, srv, &stream, &req, prepared, aborted, write_timeout_ms);
}

/* Carries on a connection the reactor handed back after a park (see
 * _conn_resume_parked). A parked body continues with _drain_body where it
 * stopped, and the request then ends through _task_finish just like in
 * _task_worker. A parked response continues with _task_continue_write. The
 * stream starts empty, since a body reader only parks with an empty
 * carry-over and a parked response's pipelined bytes wait on the
 * connection.
 *
 * A parked connection's limits are judged again here before anything else,
 * because the sweep only looks once a second. A client that stalled past a
 * limit and then became ready gets the same answer the sweep would have
 * given it, a 408 for a body and a close for a response. */
static void _task_continue_write(chttpsvr_conn_t *conn, struct chttpserver *srv,
                                 chttp1_stream_t *stream, bool expired);
static void _task_continue_interim(chttpsvr_conn_t *conn,
                                   struct chttpserver *srv, bool expired);
static void _task_read_body_and_finish(chttpsvr_conn_t *conn,
                                       struct chttpserver *srv,
                                       chttp1_stream_t *stream, bool expired);

static void _task_resume(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  _chttpsvr_mark_worker_thread(srv);
  uint8_t kind = conn->park;
  struct timespec now, now_slow;
  clock_gettime(CLOCK_MONOTONIC, &now);
  _slow_clock(&now_slow);
  /* The limits are judged at the claim, when the connection became ready,
   * not after it waited in the pool's queue. */
  bool expired = _parked_expired(srv, conn, conn->claim_mono, conn->claim_slow);
  if (!expired) _conn_uncharge_queue_wait(conn, now, now_slow);
  conn->park = _CONN_PARK_NONE;
  conn->rate_queued = 0;

  chttp1_stream_t stream;
  if (kind == _CONN_PARK_WRITE && conn->wp_interim) {
    _task_continue_interim(conn, srv, expired);
    return;
  }
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                              conn->m_procs);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  stream.fd_nonblocking = true;

  if (kind == _CONN_PARK_WRITE) {
    _task_continue_write(conn, srv, &stream, expired);
    return;
  }
  _task_read_body_and_finish(conn, srv, &stream, expired);
}

/* Reads the rest of a buffered route's body after it parked or waited for
 * its "100 Continue" line to go out, runs the handler and ends the request
 * through _task_finish, the same as _task_worker. stream is prepared and
 * owned by the caller up to this call; expired means one of the park's
 * limits ran out, which ends the request with a 408. */
static void _task_read_body_and_finish(chttpsvr_conn_t *conn,
                                       struct chttpserver *srv,
                                       chttp1_stream_t *stream, bool expired) {
  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = stream;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval_t body_err = ccol_timed_out;
  if (!expired) body_err = _drain_body(conn, stream);
  if (body_err == _CHTTPSVR_BODY_PARKED) {
    _task_park_body(conn, srv, stream, &req);
    return;
  }
  /* The body isn't arriving any more, so the connection's next wait, for a
   * pipelined request's headers or for room to write, wakes on its first
   * byte. */
  _conn_reset_rcvlowat(conn);
  _mem_settle(conn);

  bool aborted = false;
  if (conn->options_star) {
    conn->resp.status_code = CHTTP_STATUS_OK;
    aborted = body_err != ccol_success;
  } else if (body_err != ccol_success) {
    conn->resp.status_code =
        (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
        : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
        : conn->body_malformed           ? CHTTP_STATUS_BAD_REQUEST
                                         : CHTTP_STATUS_INTERNAL_ERROR;
    aborted = true;
  } else {
    _chttpsvr_next(&req, &conn->resp);
  }
  _task_finish(conn, srv, stream, &req, true, aborted, write_timeout_ms);
}

/* Carries on a parked response (see _send_response_park). If the socket is
 * full again the connection parks again. A finished response ends through
 * _task_tail with the keep-alive decision the first write recorded, and a
 * next request's pipelined bytes wait in the connection's carry-over, where
 * _task_tail looks for them. */
static void _task_continue_write(chttpsvr_conn_t *conn, struct chttpserver *srv,
                                 chttp1_stream_t *stream, bool expired) {
  stream->write_nonblocking = true;
  int r = expired ? 0 : _send_response_continue(stream, conn);
  if (r < 0) {
    chttp1_stream_release(stream);
    _conn_park_write(conn, srv);
    return;
  }
  bool keep_alive = r == 1 && conn->wp_keep_alive && !_srv_draining(srv);
  _conn_end_parked_write(conn);
  if (conn->wp_reject) {
    /* A rejection's courtesy response is out, or given up, and ends the way
     * _conn_reject_and_close ends one that went out without waiting. */
    conn->wp_reject = false;
    chttp1_stream_release(stream);
    if (r == 1 && _conn_request_bytes_may_remain(conn))
      _conn_linger(conn);
    else
      _conn_close(conn);
    _release_in_flight(srv);
    return;
  }
  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  _task_tail(conn, srv, stream, &req, true, keep_alive, r == 1);
}

/* Carries on a buffered request whose "100 Continue" line ran into a full
 * socket (see _write_interim_continue). The body bytes that came with the
 * headers wait in the carry-over and go into the stream first. Once the line
 * is out the body is read and the request ends exactly like in _task_worker.
 * If the line can't be finished, because a write limit ran out or the write
 * failed, the connection closes without a response, since part of the line
 * may already be on the wire. */
static void _task_continue_interim(chttpsvr_conn_t *conn,
                                   struct chttpserver *srv, bool expired) {
  chttp1_stream_t stream;
  bool carried;
  if (conn->tls)
    carried = chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls,
                                        conn->_carry_over,
                                        conn->_carry_over_len, conn->m_procs);
  else
    carried = chttp1_stream_prepare(&stream, conn->fd, conn->_carry_over,
                                    conn->_carry_over_len, conn->m_procs);
  if (!carried) {
    /* Copying a non-empty carry-over failed. A stream without a carry-over
     * can't fail, and the request then ends below without its body. */
    if (conn->tls)
      chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                                conn->m_procs);
    else
      chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  }
  _ccol_mem_free(conn->m_procs, conn->_carry_over);
  conn->_carry_over = NULL;
  conn->_carry_over_len = 0;
  stream.fd_nonblocking = true;
  stream.write_nonblocking = true;
  int r = expired || !carried ? 0 : _send_response_continue(&stream, conn);
  if (r < 0) {
    /* The socket is full again, so the carry-over goes back onto the
     * connection for the next try. */
    size_t lo_len = 0;
    char *lo = chttp1_stream_take_leftover(&stream, &lo_len);
    chttp1_stream_release(&stream);
    if (lo_len && !lo) {
      _conn_end_parked_write(conn);
      conn->wp_interim = false;
      conn->interim_write_failed = true;
      chttpsvr_req req;
      memset(&req, 0, sizeof(req));
      _task_tail(conn, srv, &stream, &req, false, false, false);
      return;
    }
    conn->_carry_over = lo;
    conn->_carry_over_len = lo_len;
    _conn_park_write(conn, srv);
    return;
  }
  _conn_end_parked_write(conn);
  conn->wp_interim = false;
  /* The write's deadline and rate end with the line, and the body read
   * measures its own. */
  conn->write_deadline_set = false;
  conn->rate_active = false;
  if (r == 0) {
    conn->interim_write_failed = true;
    chttpsvr_req req;
    memset(&req, 0, sizeof(req));
    _task_tail(conn, srv, &stream, &req, true, false, false);
    return;
  }
  _task_read_body_and_finish(conn, srv, &stream, false);
}

/* ========================================================================== */
/*                    LISTENER: ACCEPT + SOCKET OPTIONS                       */
/* ========================================================================== */

/* Minimum gap in seconds between two consecutive accept-failure log lines for
 * the same server. The comment of last_accept_err_log on struct chttpserver
 * says why we need it at all. */
#define _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC 5
/* How many connections one listener dispatch accepts at most before
 * returning to the reactor (see _listener_on_readable_impl). */
#define _CHTTPSVR_ACCEPT_BATCH_MAX 64u
/* Two kinds of failure pause srv's listener registration and return at
 * once: an accept4() failure from resource exhaustion (EMFILE, ENFILE,
 * ENOBUFS or ENOMEM), and an allocation failure after accept4(), from
 * _conn_create's _ccol_mem_calloc or from inside ctls_conn_create_server. We
 * pause rather than sleep on the calling thread before retrying; see
 * _listener_pause_for_resource_pressure and its two call sites below.
 *
 * Such a condition can last. The process or the system can really run out of
 * file descriptors under a sustained connection flood with a modest
 * ulimit -n, and a custom, bounded ccol_memmgmt_procs_t installed through
 * chttpsvr_create_mp can run dry while the system still has memory. The
 * reactor's level-triggered epoll keeps seeing a non-empty listen backlog and
 * dispatches this handler again and again, and no progress is possible until
 * something outside frees the resource.
 *
 * Sleeping synchronously in that dispatch does bound its OWN CPU cost, but it
 * still blocks the process's single shared reactor thread for the whole
 * sleep (see the default of chttpsvr_set_engine_num_reactor_threads), on every
 * re-dispatch, for as long as the condition lasts, and starves every OTHER
 * connection on every OTHER server sharing that reactor thread. The
 * max_connections capacity pause a few lines up in the same loop already
 * avoids exactly this by pausing instead of blocking.
 *
 * The idle-timeout sweep thread resumes the listener, checking every server on
 * every tick (see _listener_resume_if_resource_pressure_cleared), so recovery
 * takes at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS. That's an acceptable price
 * for the same documented reason as max_connections's resume latency:
 * chttpsvr_config_t's doc comments never promise sub-second recovery from
 * resource exhaustion, and it's far better than blocking the shared reactor
 * thread for as long as the condition lasts. */

/* True for an accept4() failure about one specific pending connection, which
 * the kernel put into the listen socket's error state before this call ran,
 * and not about the listener itself. Retrying right away is both safe and the
 * usual way to handle this class, since the accept loop can try the next
 * pending connection in the backlog immediately instead of waiting for a
 * fresh epoll dispatch. The BUGS section of the accept(2) man page documents
 * that Linux passes a pending per-connection network error through accept()
 * like this and recommends treating it as temporary.
 *
 * ECONNABORTED is the one such error POSIX itself documents. EPROTO,
 * ENETDOWN, ENETUNREACH, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH and
 * EOPNOTSUPP are exactly the Linux-specific pending network error codes that
 * page's ERRORS section lists word for word for accept(2): "In the case of
 * TCP/IP, these are ENETDOWN, EPROTO, ENOPROTOOPT, EHOSTDOWN, ENONET,
 * EHOSTUNREACH, EOPNOTSUPP, and ENETUNREACH".
 *
 * EPERM means "Firewall rules forbid connection" according to the same
 * ERRORS section. It has the same shape as ECONNABORTED, one connection
 * rejected, just from a different source, a firewall rule instead of the
 * peer. That's a realistic condition and not an adversarial one: any server
 * behind rules that rate-limit connections hits it, and so does one behind
 * rules a fail2ban-style tool installs as REJECT rather than DROP.
 *
 * ETIMEDOUT, ENOSR, ESOCKTNOSUPPORT and EPROTONOSUPPORT complete the same man
 * page's sentence right after its EPERM and EPROTO entries, "in addition,
 * network errors for the new socket... may be returned; various Linux kernels
 * can return other errors such as...". They're in the same per-pending-
 * connection category and aren't a listener-level problem. */
static bool _accept_errno_is_transient(int e) {
  switch (e) {
    case ECONNABORTED:
    case EPROTO:
    case ENETDOWN:
    case ENETUNREACH:
    case ENOPROTOOPT:
    case EHOSTDOWN:
#ifdef ENONET /* Linux only */
    case ENONET:
#endif
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case EPERM:
    case ETIMEDOUT:
#ifdef ENOSR /* Linux only */
    case ENOSR:
#endif
    case ESOCKTNOSUPPORT:
    case EPROTONOSUPPORT:
      return true;
    default:
      return false;
  }
}

/* True for an accept4() failure caused by exhausting a shared resource that
 * belongs to the whole process or system, not by one specific pending
 * connection. An immediate retry will probably fail the same way until
 * something ELSE in the process or the system frees that resource.
 *
 * It's a pure classifier with its own name and tests, used directly by this
 * file's white-box test accessor and the tests that exercise it. It's there
 * for documentation and diagnostics and does NOT decide whether
 * _listener_on_readable_impl calls _listener_pause_for_resource_pressure:
 * that function pauses for every non-transient errno that reaches it,
 * because an explicit allow-list there would leave an unclassified but
 * persistent errno open to exactly the busy loop this mechanism prevents.
 * The comment at that call site explains why. */
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
/* White-box test hooks exposing the two pure errno classifiers above. To
 * really trigger an EMFILE, an ECONNABORTED or the like, a test would have to
 * play with the whole process's fd limits or with connection state inside
 * the kernel, which depends on the environment and is out of proportion for
 * classifying an integer that always gives the same answer. Like every other
 * white-box helper here, both are compiled out of a production build. */
bool _chttpsvr_accept_errno_is_transient_for_tests(int e) {
  return _accept_errno_is_transient(e);
}
bool _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(int e) {
  return _accept_errno_is_resource_exhaustion(e);
}
#endif /* RUNNING_UNIT_TESTS */

/* Returns true at most once every _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC
 * seconds per server and false otherwise, and on true it also records this
 * moment as the new "last logged" one.
 *
 * It's the shared rate-limit gate for this accept loop's diagnostics about a
 * persistent listener-level condition nobody can do anything about
 * immediately. There are two of those: an unexpected accept4() failure, and an
 * allocation failure for a connection or TLS object right after a
 * successful accept4(). Both can flood the log the same way, since each can
 * fire again on every accept4() attempt for as long as the condition lasts.
 *
 * The two fields read and written here need no synchronization of their own;
 * the comment of last_accept_err_log on struct chttpserver says why. */
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

/* err's text through the thread-safe strerror_r(3). glibc with _GNU_SOURCE
 * declares the GNU form, which returns the text and may not use buf at all;
 * every other C library declares the POSIX form, which fills buf and returns
 * an error number. */
static const char *_errno_text(int err, char *buf, size_t len) {
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  return strerror_r(err, buf, len);
#else
  if (strerror_r(err, buf, len) != 0) snprintf(buf, len, "error %d", err);
  return buf;
#endif
}

/* Logs an unexpected accept4() failure through the engine logger,
 * rate-limited by _listener_accept_err_log_gate. The line says whether
 * _accept_errno_is_resource_exhaustion recognises err as one of the
 * well-known EMFILE, ENFILE, ENOBUFS or ENOMEM cases.
 *
 * That's only a hint for whoever reads the log. An operator can act on
 * "resource exhaustion" by raising ulimits or freeing memory, while anything
 * else here is a listener-level condition this loop doesn't expect at all and
 * is worth investigating as a possible bug rather than routine overload. It
 * doesn't change how the loop behaves: the single call site below pauses for
 * every such errno, whatever this classifier says. */
static void _listener_log_accept_err_rate_limited(struct chttpserver *srv,
                                                  int err) {
  if (!_listener_accept_err_log_gate(srv)) return;
  /* strerror_r rather than strerror, because this file's reactor really is
   * multi-threaded (see chttpsvr_set_engine_num_reactor_threads) and plain
   * strerror() doesn't have to be thread-safe: POSIX allows it to return a
   * pointer into a shared static buffer that a concurrent strerror() on
   * another thread can overwrite while the first one is still reading it. Two
   * different servers' listeners can run this exact call on two reactor
   * threads at the same moment.
   *
   * _errno_text() wraps whichever strerror_r the C library declares. */
  char errbuf[128];
  _SRV_ENGINE_LOG(clog_warn, "accept() failed errno=%d (%s)%s", err,
                  _errno_text(err, errbuf, sizeof(errbuf)),
                  _accept_errno_is_resource_exhaustion(err)
                      ? " [resource exhaustion]"
                      : "");
}

#ifdef RUNNING_UNIT_TESTS
/* Purely white-box test instrumentation. It counts, process-wide, how often
 * this accept loop paused a server's listener after an allocation failure
 * following an accept4(), i.e. a NULL from _conn_create or
 * ctls_conn_create_server.
 *
 * A test reads the difference over its own window while a custom
 * ccol_memmgmt_procs_t makes every such allocation fail, confirming that the
 * pause path really runs, in the same style g_listener_dispatch_count_for_tests
 * uses for max_connections's pause and resume path. The guard keeps the
 * symbol out of a production build. */
static _Atomic size_t g_listener_alloc_failure_pause_count_for_tests = 0;
size_t _chttpsvr_listener_alloc_failure_pause_count_for_tests(void) {
  return atomic_load(&g_listener_alloc_failure_pause_count_for_tests);
}

/* Purely white-box test instrumentation. It counts, process-wide, how often
 * _listener_pause_for_resource_pressure really paused a listener and set
 * listener_paused_for_resource_pressure. The idle-timeout sweep clears that
 * flag about once a second, so a test polling the flag could miss a pause,
 * which is why a test reads this count's difference over its own window
 * instead. */
static _Atomic size_t g_listener_resource_pause_count_for_tests = 0;
size_t _chttpsvr_listener_resource_pause_count_for_tests(void) {
  return atomic_load(&g_listener_resource_pause_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Pauses srv's listener registration in response to a resource-exhaustion
 * errno from accept4() or an allocation failure right after one, then sets
 * listener_paused_for_resource_pressure (see that field's comment), which the
 * idle-timeout sweep thread reads in
 * _listener_resume_if_resource_pressure_cleared to know it should retry the
 * listener later.
 *
 * It does nothing in two cases: srv has no live listener registration to
 * pause (lreg is NULL), or ccol_event_loop_pause fails. That call's contract
 * says a failure can only mean another thread removed lreg, a concurrent
 * chttpsvr_stop() for example, and listener_dispatch_pins exists precisely to
 * prevent that while this dispatch runs, so nothing should get there; if
 * something ever does, there's no paused registration to track anyway.
 *
 * The flag is only set once the pause has succeeded, never before trying, so
 * the sweep never wastes a resume on a registration that was never
 * paused. */
static void _listener_pause_for_resource_pressure(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  _ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  if (ccol_event_loop_pause(srv_engine_bundler.reactor, lreg) == ccol_success) {
    atomic_store(&srv->listener_paused_for_resource_pressure, true);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_listener_resource_pause_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  }
}

/* Logs an allocation failure the loop runs into right after a successful
 * accept4(), a NULL from _conn_create's _ccol_mem_calloc or from inside
 * ctls_conn_create_server. The log line is rate-limited through the same gate
 * and fields as the accept4()-failure logger above. Then srv's listener is
 * paused, exactly as _listener_pause_for_resource_pressure describes, rather
 * than retrying accept4() at once, the same resource-exhaustion handling
 * this loop applies to accept4() itself (see the "LISTENER: ACCEPT + SOCKET
 * OPTIONS" section comment above).
 *
 * Without the pause a sustained allocation failure would spin this loop
 * forever. The most likely cause is a custom, bounded ccol_memmgmt_procs_t
 * installed through chttpsvr_create_mp, which can run dry while the system
 * still has memory; a real system-wide out-of-memory is rarer but possible.
 * With connections still arriving in the listen backlog, the loop would
 * accept, fail to allocate immediately, close and repeat, with every
 * epoll_wait dispatching it again and nothing making progress, an unbounded
 * busy loop under a persistent condition. This file already treats that same
 * shape as a real bug in accept4()'s EMFILE, ENFILE, ENOBUFS and ENOMEM
 * handling a few lines down in this loop. */
static void _listener_log_and_pause_for_alloc_failure(struct chttpserver *srv,
                                                      const char *what) {
  if (_listener_accept_err_log_gate(srv))
    _SRV_ENGINE_LOG(clog_warn, "%s failed after accept()", what);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_alloc_failure_pause_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  _listener_pause_for_resource_pressure(srv);
}

#ifdef RUNNING_UNIT_TESTS
/* Purely white-box test instrumentation: how many accepted connections, in
 * the whole process, had their socket buffers raised by
 * _apply_accepted_socket_options. */
static _Atomic size_t g_accepted_sockbuf_raise_count_for_tests = 0;
size_t _chttpsvr_accepted_sockbuf_raise_count_for_tests(void) {
  return atomic_load(&g_accepted_sockbuf_raise_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

static void _apply_accepted_socket_options(int fd, bool is_unix,
                                           bool enable_keepalive) {
  /* accept4()'s SOCK_NONBLOCK flag already makes the socket non-blocking
   * atomically, and the only call site producing fd passes it (see
   * _listener_on_readable), so a separate fcntl(F_SETFL) isn't needed here
   * and isn't wanted either. */
  int one = 1;
  if (!is_unix) {
    /* TCP_NODELAY is an IPPROTO_TCP level option, and SO_KEEPALIVE's usual
     * purpose doesn't apply to AF_UNIX.
     *
     * A TCP connection keeps the kernel's buffer sizes. An explicit
     * SO_SNDBUF or SO_RCVBUF on a TCP socket locks that direction at the
     * given size and turns Linux's buffer autotuning off for it (see tcp(7)).
     * Autotuning grows a buffer to the path's bandwidth-delay product, way
     * past any fixed size that suits a local link, and locking it caps the
     * throughput of every connection with a long round trip. A fixed size
     * looks like harmless tuning here, and it's the wrong choice for TCP. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (enable_keepalive)
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return;
  }
  /* An AF_UNIX stream socket has no autotuning, and some platforms and
   * containers give it small default buffers. So a unix:// connection gets
   * both buffers raised to at least 128 KiB, and doesn't end up with
   * noticeably smaller buffers than a TCP client of the same server. */
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_accepted_sockbuf_raise_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
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
/* Purely white-box test instrumentation: how many times _listener_on_readable
 * was dispatched, process-wide, since the counter started at zero.
 *
 * A test reads the difference over its own window, while it holds a server at
 * max_connections and nothing else in the process connects or closes. That
 * difference tells a correctly paused listener from one that busy-loops: a
 * paused listener stays at a small fixed handful of dispatches, while a
 * busy-looping one climbs into the thousands within a fraction of a second,
 * because level-triggered epoll re-reports a listener with a non-empty accept
 * backlog on every epoll_wait. It's a far more precise signal than measuring
 * wall-clock CPU time and depends much less on the environment.
 *
 * Like every other white-box helper in this file, the symbol and the counter
 * are compiled out of a production build. */
static _Atomic size_t g_listener_dispatch_count_for_tests = 0;
size_t _chttpsvr_listener_dispatch_count_for_tests(void) {
  return atomic_load(&g_listener_dispatch_count_for_tests);
}

/* The most connections a single accept-loop dispatch accepted, across all
 * servers, since the last reset. A test compares it with
 * _CHTTPSVR_ACCEPT_BATCH_MAX. */
static _Atomic size_t g_listener_max_accepts_per_dispatch_for_tests = 0;
size_t _chttpsvr_listener_max_accepts_per_dispatch_for_tests(void) {
  return atomic_load(&g_listener_max_accepts_per_dispatch_for_tests);
}
void _chttpsvr_listener_max_accepts_per_dispatch_reset_for_tests(void) {
  atomic_store(&g_listener_max_accepts_per_dispatch_for_tests, 0);
}
static void _listener_note_accepts_for_tests(size_t n) {
  size_t cur = atomic_load(&g_listener_max_accepts_per_dispatch_for_tests);
  while (n > cur &&
         !atomic_compare_exchange_weak(
             &g_listener_max_accepts_per_dispatch_for_tests, &cur, n)) {
  }
}

/* Purely white-box test instrumentation.
 * g_force_next_accept_errno_for_tests_srv names one server by raw pointer,
 * resolved once at arm time through _chttpsvr_resolve_for_tests. When it
 * matches the server about to dispatch, the result of THAT server's very next
 * accept4() in _listener_on_readable_impl's loop is thrown away: a real,
 * successful connection just gets closed unused, as if it had never been
 * accepted, and the result is replaced with a simulated failure carrying the
 * paired _val field's errno. Both fields are then reset, so only that one
 * call for that one server is ever affected.
 *
 * It's scoped to one specific server on purpose, rather than "the very next
 * accept4() anywhere in the process", because this reactor is shared by the
 * whole process and an unrelated dispatch for some other chttpsvr can run at
 * the same time and eat the forced errno before the test's own connection
 * gets there. The long-lived shared fixture server of this test binary is one
 * such server, and it can be servicing an unrelated connection at that very
 * moment, which really happens under valgrind's scheduling. The result would
 * be a false pass or failure that comes and goes with the environment's
 * timing and has nothing to do with what the test is about.
 *
 * The hook exists because a REAL accept4() failure is hard to produce for an
 * errno outside the named cases of _accept_errno_is_transient and
 * _accept_errno_is_resource_exhaustion, like EBADF, EINVAL, ENOTSOCK or
 * EFAULT: a test would need to play with the whole process's fd table or
 * kernel socket state, which depends on the environment and is out of
 * proportion. The comment of _chttpsvr_accept_errno_is_transient_for_tests
 * gives the same reasoning for EMFILE, ECONNABORTED and the rest.
 *
 * With it a test can check how the LOOP itself handles such an errno (it
 * pauses, doesn't busy-loop, and resumes later) without the errno having to
 * be real. The guard keeps the symbol and its setter out of a production
 * build. */
static _Atomic(struct chttpserver *) g_force_next_accept_errno_for_tests_srv =
    NULL;
static _Atomic int g_force_next_accept_errno_for_tests_val = 0;
void _chttpsvr_force_next_accept_errno_for_tests(chttpsvr h, int errno_val) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  atomic_store(&g_force_next_accept_errno_for_tests_val, errno_val);
  atomic_store(&g_force_next_accept_errno_for_tests_srv, raw);
}
#endif /* RUNNING_UNIT_TESTS */

/* The accept loop's real body. The thin wrapper below pins it for its whole
 * duration and passes the listener fd it checked against srv->listen_fd
 * under that pin (see the comment of the listener_dispatch_pins field), and
 * the pin keeps that fd open until this returns.
 *
 * It's a function of its own instead of having a pin/unpin pair wrapped
 * around it inline because it has several early returns (the capacity pause,
 * an EWOULDBLOCK, an unexpected accept4() failure and more), and a thin
 * caller funnels all of them back through a single unpin, which is simpler
 * and less error-prone than threading a matching unpin into each return. */
static void _listener_on_readable_impl(struct chttpserver *srv, int lfd) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_dispatch_count_for_tests, 1);
  size_t accepted_for_tests = 0;
#endif /* RUNNING_UNIT_TESTS */

  /* One dispatch accepts at most _CHTTPSVR_ACCEPT_BATCH_MAX connections and
   * then goes back to the reactor. The listener registration is
   * level-triggered, so a still non-empty backlog is dispatched again on the
   * next epoll_wait, after the events of every other ready connection in that
   * batch. Without the bound a flood of connects would keep the single reactor
   * thread stuck in this loop with every established connection waiting
   * behind it. Every iteration counts, retries after EINTR or a transient
   * per-connection error included. */
  for (unsigned batch = 0; batch < _CHTTPSVR_ACCEPT_BATCH_MAX; batch++) {
#ifdef RUNNING_UNIT_TESTS
    _listener_note_accepts_for_tests(accepted_for_tests);
#endif /* RUNNING_UNIT_TESTS */
    size_t cap = atomic_load(&srv->max_connections);
    if (cap && atomic_load(&srv->current_connections) >= cap) {
      /* At capacity, so pause the listener's reactor registration rather than
       * just returning without another accept4(). Level-triggered epoll
       * reports a listen socket with a non-empty accept backlog as ready on
       * every epoll_wait, not only once, so a plain return would make the
       * reactor dispatch this handler again at once, and again, while no
       * progress is possible until a slot frees up.
       *
       * The CPU ticks in /proc/<pid>/stat show it directly: without the pause,
       * a listener left at capacity keeps a whole CPU core at about 100
       * percent for as long as the server stays there, doing nothing but
       * re-dispatching this handler, a real and severe waste.
       *
       * The waste happens whether or not the same epoll_wait batch also
       * services other connections' events, which it usually does since
       * epoll_wait returns all ready fds together and not just the listener.
       * The pause is there to get rid of the wasted CPU time, not because
       * other connections would starve.
       *
       * The idle-timeout sweep thread resumes the listener, checking every
       * server on every tick (see _listener_resume_if_capacity_freed), so the
       * resume comes at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS after a slot
       * frees. */
      _ccol_mutex_lock(srv->mutex);
      ccol_event_reg lreg = srv->listen_reg;
      _ccol_mutex_unlock(srv->mutex);
      if (lreg && ccol_event_loop_pause(srv_engine_bundler.reactor, lreg) ==
                      ccol_success) {
        /* Check the capacity again AFTER the pause, not only before it, to
         * close a real time-of-check to time-of-use race against the sweep
         * thread's resume check.
         *
         * If a slot has already freed up, a sweep tick's
         * _listener_resume_if_capacity_freed can run concurrently with the
         * pause above, just ahead of it. That resume finds nothing paused
         * yet, so it's a harmless no-op at that moment, but then nothing else
         * un-pauses this listener until the NEXT sweep tick, up to another
         * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS away.
         *
         * This recheck runs strictly after the pause has taken effect and
         * resumes immediately if a slot has freed since, which brings the gap
         * down to zero instead of up to one more tick.
         *
         * Like every other ccol_event_loop_pause, _modify, _add and _resume
         * call site in this file, we check the pause's return value instead of
         * fire-and-forget. A non-success can only mean another thread removed
         * lreg, the only documented failure for a non-NULL reg, and
         * listener_dispatch_pins exists precisely to prevent that while this
         * dispatch runs, so nothing should get there. If something ever does,
         * there's no paused registration to resume, and skipping the recheck
         * and resume altogether is right, rather than calling
         * ccol_event_loop_resume on a reg nobody paused. */
        if (atomic_load(&srv->current_connections) < cap)
          ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
      }
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    /* accept4() with SOCK_NONBLOCK rather than accept() plus a separate
     * fcntl(F_SETFL), because the resulting fd goes straight to the single
     * reactor thread's read() and ctls_conn_read() calls (see _conn_pump),
     * which must never block. accept4() makes non-blocking mode part of the
     * same atomic kernel operation that creates the fd, instead of relying on
     * a second, separate and unchecked fcntl() succeeding first.
     *
     * SOCK_CLOEXEC is there for the same atomicity reason on the other axis.
     * Every accepted connection's fd is a live, open client socket, and
     * without SOCK_CLOEXEC a child would inherit it across any fork() plus
     * exec() the embedding application does elsewhere while the server runs,
     * a request handler that shells out or an unrelated subprocess spawn, so
     * the fd would leak into a child that has no business holding it. It's
     * the same convention this codebase follows for every fd it creates, the
     * O_CLOEXEC file opens in clogger.c and the EFD_CLOEXEC eventfd() calls
     * in cthreadcomm.c. */
    int cfd = ccol_accept_nb(lfd, (struct sockaddr *)&ss, &slen);
    int accept_errno = cfd < 0 ? errno : 0;
#ifdef RUNNING_UNIT_TESTS
    /* See the comment of g_force_next_accept_errno_for_tests_srv. The forced
     * errno is only consumed, and both fields reset, when this dispatch's srv
     * is exactly the one a test armed, so a concurrent dispatch of some
     * unrelated server, like this test binary's long-lived shared fixture
     * server, can never steal a forced errno meant for another server under
     * test. */
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
      /* A problem with one specific pending connection, not with the
       * listener. Retry immediately so the loop tries the next backlog entry,
       * if there is one, instead of waiting for a fresh epoll dispatch to
       * notice the backlog still isn't empty. */
      if (_accept_errno_is_transient(accept_errno)) continue;
      _listener_log_accept_err_rate_limited(srv, accept_errno);
      /* The "LISTENER: ACCEPT + SOCKET OPTIONS" section comment above
       * explains why we pause here instead of sleeping. Without the pause a
       * persistent exhaustion condition busy-loops the single shared reactor
       * thread: the listen backlog never empties because nothing gets
       * accepted, so the reactor's level-triggered epoll dispatches this
       * handler again immediately and nothing makes progress.
       *
       * The pause is on purpose NOT gated on
       * _accept_errno_is_resource_exhaustion. That helper's switch only names
       * the well-documented EMFILE, ENFILE, ENOBUFS and ENOMEM cases, while
       * accept(2) documents several more, EBADF, EINVAL, ENOTSOCK and EFAULT
       * among them, none of which this loop expects in practice on a listener
       * it owns and manages itself.
       *
       * Any errno that gets here is by construction neither EWOULDBLOCK,
       * EAGAIN nor EINTR, nor one of _accept_errno_is_transient's
       * per-connection cases, so if ANY such errno persists it busy-loops
       * this thread exactly like an unhandled resource-exhaustion errno would.
       * For no errno value is returning here without a pause the right thing.
       *
       * Pausing costs nothing even for a one-off condition that clears by
       * itself, since the very next sweep tick resumes the listener anyway.
       * So we pause for every errno that gets here, rather than matching
       * against an explicit allow-list. */
      _listener_pause_for_resource_pressure(srv);
      return;
    }
    atomic_fetch_add(&srv->current_connections, 1);
#ifdef RUNNING_UNIT_TESTS
    accepted_for_tests++;
#endif /* RUNNING_UNIT_TESTS */
    _apply_accepted_socket_options(cfd, atomic_load(&srv->is_unix_socket),
                                   srv->enable_keepalive);

    _ccol_call_once(srv_parser_bundler.once, _init_parser_settings);
    chttpsvr_conn_t *conn =
        _conn_create(srv, cfd, &srv_parser_bundler.settings);
    if (!conn) {
      close(cfd);
      atomic_fetch_sub(&srv->current_connections, 1);
      /* See the comment of _listener_log_and_pause_for_alloc_failure.
       * Without this pause a sustained allocation failure would spin this
       * loop without progress while connections keep coming, the same hazard
       * the accept4() failure path a few lines up already guards against. */
      _listener_log_and_pause_for_alloc_failure(srv, "connection allocation");
      return;
    }

    if (srv->tls_ctx) {
      conn->tls = ctls_conn_create_server(srv->tls_ctx, cfd, conn, NULL);
      if (!conn->tls) {
        /* Go through the shared _conn_free helper, which closes the fd,
         * decrements current_connections and frees everything conn owns,
         * rather than a narrower hand-written version here. It's safe to hand
         * conn to it at this point since nothing put conn in the idle or the
         * diverted list, and conn->tls is still NULL because the creation
         * that just failed is what would fill it.
         *
         * It also covers a future change that makes _conn_create or
         * _conn_reset_for_request allocate something per connection: the
         * helper frees it automatically, whereas a hand-written version here
         * would silently leak it through this one narrow, easy-to-miss
         * early-failure path. */
        _conn_free(conn);
        /* See the comment of _listener_log_and_pause_for_alloc_failure; same
         * reasoning as for the _conn_create failure above, applied to building
         * the TLS connection object. */
        _listener_log_and_pause_for_alloc_failure(srv, "TLS connection setup");
        return;
      }
      conn->state = CONN_ST_TLS_HANDSHAKE;
    } else {
      conn->state = CONN_ST_READING_HEADERS;
    }
    _conn_pump(conn);
  }
#ifdef RUNNING_UNIT_TESTS
  _listener_note_accepts_for_tests(accepted_for_tests);
#endif /* RUNNING_UNIT_TESTS */
}

/* Registered with ccol_event_loop_add as the listener's on_readable
 * callback. It pins srv with listener_dispatch_pins (see that counter's
 * comment) for the whole call into _listener_on_readable_impl, any listener
 * pause it does included, and drops the pin as its very last action once that
 * call has fully returned. srv's memory stays valid for the whole call, also
 * before the pin, through the srv->lifetime_refs reference the listener
 * registration holds.
 *
 * The pin is taken under srv->mutex, which orders the increment against the
 * clearing of listen_fd in _chttpsvr_stop_internal and against that
 * function's wait for this counter, both under the same mutex; a bare atomic
 * would race that check.
 *
 * A dispatch can start before a chttpsvr_stop and pin after it. Its
 * registration's fd is then no longer listen_fd (it's -1, or the fd of a
 * listener a later chttpsvr_start created), and such a dispatch returns at
 * once: it must not accept on a listener it wasn't registered for, nor pause
 * or log on behalf of a server that may already be in its destroy. */
static void _listener_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  struct chttpserver *srv = (struct chttpserver *)arg;

#ifdef RUNNING_UNIT_TESTS
  _listener_dispatch_race_hook_wait_if_armed(true);
#endif /* RUNNING_UNIT_TESTS */

  _ccol_mutex_lock(srv->mutex);
  atomic_fetch_add(&srv->listener_dispatch_pins, 1);
  int lfd = atomic_load(&srv->listen_fd);
  _ccol_mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _listener_dispatch_race_hook_wait_if_armed(false);
#endif /* RUNNING_UNIT_TESTS */

  if (lfd >= 0 && lfd == sel->fd) _listener_on_readable_impl(srv, lfd);

  _ccol_mutex_lock(srv->mutex);
  atomic_fetch_sub(&srv->listener_dispatch_pins, 1);
  _ccol_cond_var_broadcast(srv->resolve_cv);
  _ccol_mutex_unlock(srv->mutex);
}

/* The listener registration's on_removed handler. It fires exactly once per
 * registration and only when no dispatch of it can still run (see the doc
 * comment of ccol_event_loop_remove). It drops the srv->lifetime_refs
 * reference chttpsvr_start took for the registration, and runs srv's final
 * teardown when that was the last reference. */
static void _listener_on_removed(void *arg) {
  struct chttpserver *srv = (struct chttpserver *)arg;
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(srv);
}

/* ========================================================================== */
/*                    LISTEN SOCKET SETUP (TCP + Unix)                        */
/* ========================================================================== */

#define _CHTTPSVR_UNIX_PREFIX "unix://"

/* Removes a unix:// listener's socket file if the name still points at the
 * socket this server bound, and releases what ub holds. A file somebody else
 * put at the path in the meantime stays. The window between the check and
 * the unlink can't be avoided, POSIX has no unlink by inode. */
static void _unix_bind_cleanup(_chttpsvr_unix_bind_t *ub,
                               ccol_memmgmt_procs_t *mp) {
  if (!ub->path) return;
  if (ub->dir_fd >= 0) {
    struct stat st;
    if (fstatat(ub->dir_fd, ub->base, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(st.st_mode) && st.st_dev == ub->dev && st.st_ino == ub->ino)
      unlinkat(ub->dir_fd, ub->base, 0);
    close(ub->dir_fd);
  }
  _ccol_mem_free(mp, ub->path);
  ub->path = NULL;
  ub->base = NULL;
  ub->dir_fd = -1;
}

/* Before the bind, decides what to do with a file that already exists at a
 * unix:// listener's path. Returns true when the bind may go ahead: nothing
 * is there, or there was a stale socket nobody listens on any more and it's
 * now removed. Returns false with errno set when the start must fail and
 * leave the file alone, either because it isn't a socket (a start never
 * deletes a regular file, a directory or anything else a typo in the
 * configuration might name) or because a live listener answers on it
 * (EADDRINUSE). The probe is a non-blocking connect(2) from a scratch socket,
 * and ECONNREFUSED is the kernel's answer for a socket file with no listener
 * behind it. */
static bool _unix_path_prepare(const struct sockaddr_un *addr) {
  struct stat st;
  if (lstat(addr->sun_path, &st) != 0) return errno == ENOENT;
  if (!S_ISSOCK(st.st_mode)) {
    _SRV_ENGINE_LOG(clog_error,
                    "unix socket path %s exists and is not a socket; "
                    "leaving it in place",
                    addr->sun_path);
    errno = EEXIST;
    return false;
  }
  int probe = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
  if (probe < 0) return false;
  int rc = connect(probe, (const struct sockaddr *)addr, sizeof(*addr));
  int e = rc == 0 ? 0 : errno;
  close(probe);
  if (e == ECONNREFUSED) {
    /* Stale. Only remove it while the name still points at the same socket
     * file the probe saw. */
    struct stat again;
    if (lstat(addr->sun_path, &again) == 0 && S_ISSOCK(again.st_mode) &&
        again.st_dev == st.st_dev && again.st_ino == st.st_ino)
      unlink(addr->sun_path);
    return true;
  }
  if (e == ENOENT) return true; /* removed since the lstat */
  if (e == 0 || e == EAGAIN || e == EINPROGRESS) {
    _SRV_ENGINE_LOG(clog_error,
                    "unix socket path %s: another server already listens "
                    "on it",
                    addr->sun_path);
    e = EADDRINUSE;
  }
  errno = e;
  return false;
}

/* Open flags for a unix:// path's directory descriptor. It only names the
 * directory for the *at() calls, so O_PATH is enough where the system has it
 * (no read permission needed); macOS doesn't have it. */
#if defined(O_PATH)
#define _UNIX_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define _UNIX_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

/* Opens the directory holding path, for _chttpsvr_unix_bind_t.dir_fd, and
 * points *base at path's last component. */
static int _unix_open_dir(const char *path, const char **base) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    *base = path;
    return open(".", _UNIX_DIR_OPEN_FLAGS);
  }
  *base = slash + 1;
  if (!**base) {
    errno = EINVAL;
    return -1;
  }
  size_t dlen = slash == path ? 1 : (size_t)(slash - path);
  char dir[sizeof(((struct sockaddr_un *)0)->sun_path)];
  memcpy(dir, path, dlen);
  dir[dlen] = '\0';
  return open(dir, _UNIX_DIR_OPEN_FLAGS);
}

#ifdef RUNNING_UNIT_TESTS
/* The errno reported by the last failed listening socket setup. */
static _Atomic int g_last_listen_errno_for_tests = 0;
int _chttpsvr_last_listen_errno_for_tests(void) {
  return atomic_load(&g_last_listen_errno_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

static int _make_unix_listen_socket(const char *path, _chttpsvr_unix_bind_t *ub,
                                    ccol_memmgmt_procs_t *mp) {
  if (!*path) {
    errno = EINVAL;
    return -1;
  }
  if (strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path)) {
    errno = ENAMETOOLONG;
    return -1;
  }
  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);

  _chttpsvr_unix_bind_t b = {.path = NULL, .base = NULL, .dir_fd = -1};
  b.path = ccol_strdup(mp, path);
  if (!b.path) return -1;
  const char *base_in_cfg = NULL;
  b.dir_fd = _unix_open_dir(path, &base_in_cfg);
  if (b.dir_fd < 0) {
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  b.base = b.path + (base_in_cfg - path);

  /* SOCK_CLOEXEC keeps a child of some fork() plus exec() the embedding
   * process does elsewhere while this server runs from inheriting the
   * listening socket itself. Same reasoning as at the accept4() call site in
   * _listener_on_readable. */
  int fd = -1;
  if (_unix_path_prepare(&addr)) fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    close(b.dir_fd);
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  struct stat st;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    /* EADDRINUSE here means another process created the file after the
     * check above, and we leave it alone. */
    close(fd);
    close(b.dir_fd);
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  if (fstatat(b.dir_fd, b.base, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(st.st_mode)) {
    /* We can't identify the socket bind() just created, so nothing could
     * ever remove it safely. It stays as a stale socket file that the next
     * start at this path replaces. */
    close(fd);
    close(b.dir_fd);
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  b.dev = st.st_dev;
  b.ino = st.st_ino;
  if (listen(fd, SOMAXCONN) != 0) {
    close(fd);
    _unix_bind_cleanup(&b, mp);
    return -1;
  }
  *ub = b;
  return fd;
}

/* Creates, binds and listens on one TCP socket. v6only is -1 to keep the
 * system's IPV6_V6ONLY default, 0 or 1 to set it. Returns the fd, or -1 with
 * errno set. */
static int _bind_tcp(int family, const struct sockaddr *sa, socklen_t salen,
                     bool enable_reuseport, int v6only) {
  /* SOCK_CLOEXEC next to SOCK_NONBLOCK for the same reason as in the AF_UNIX
   * branch and at the accept4() call in _listener_on_readable. Without it
   * this listening socket would leak into any child a fork() plus exec()
   * elsewhere in the application produces. */
  int fd = ccol_socket_nb(family, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
#ifdef SO_REUSEPORT
  if (enable_reuseport)
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
#else
  (void)enable_reuseport;
#endif
  if (family == AF_INET6 && v6only >= 0 &&
      setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  if (bind(fd, sa, salen) != 0 || listen(fd, SOMAXCONN) != 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  return fd;
}

/* Whether the system lets an IPv6 socket accept IPv4 clients. OpenBSD refuses
 * to turn IPV6_V6ONLY off, so it has no dual-stack socket. If the socket
 * can't be created for some reason other than missing IPv6 (no descriptors
 * or memory left) we answer yes, so the caller reports its own bind's error
 * instead of falling back to an IPv4 listener. */
static bool _ipv6_dual_stack_supported(void) {
  int fd = ccol_socket_nb(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT;
  int off = 0;
  bool ok = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0;
  close(fd);
  return ok;
}

/* Opens the listener cfg->host names, following the rules of the Go net
 * package's listener:
 *
 *  - NULL or "": one dual-stack socket on [::] with IPV6_V6ONLY off, which
 *    also takes IPv4 clients through v4-mapped addresses (ipv6_only makes it
 *    IPv6-only). A host without IPv6, or a system without dual-stack sockets
 *    like OpenBSD, gets 0.0.0.0.
 *  - A literal IPv4 or IPv6 address: exactly that address. A bracketed
 *    address like "[::1]" is the address inside the brackets; a bracketed
 *    name is refused.
 *  - A host name: its first IPv4 address if it has one, else its first IPv6
 *    address; with ipv6_only only its first IPv6 address. So "localhost"
 *    listens on 127.0.0.1. Exactly one address is tried, so a failure, like
 *    a port another socket holds there, is the answer and keeps its errno;
 *    another address of the name never hides it.
 *  - "unix://path": a Unix domain socket at path.
 *
 * *is_unix says which kind was opened, and *ub holds a unix:// listener's
 * socket file. On failure errno has the reason; a name that doesn't resolve
 * is logged with the resolver's message and gives EADDRNOTAVAIL. */
static int _make_listen_socket(const char *host, uint16_t port,
                               bool enable_reuseport, bool ipv6_only,
                               bool *is_unix, _chttpsvr_unix_bind_t *ub,
                               ccol_memmgmt_procs_t *mp) {
  ub->path = NULL;
  ub->base = NULL;
  ub->dir_fd = -1;
  *is_unix = host && strncmp(host, _CHTTPSVR_UNIX_PREFIX,
                             strlen(_CHTTPSVR_UNIX_PREFIX)) == 0;
  if (*is_unix)
    return _make_unix_listen_socket(host + strlen(_CHTTPSVR_UNIX_PREFIX), ub,
                                    mp);

  int v6only_explicit = ipv6_only ? 1 : -1;
  if (!host || !*host) {
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons(port);
    a6.sin6_addr = in6addr_any;
    int fd = _bind_tcp(AF_INET6, (struct sockaddr *)&a6, sizeof(a6),
                       enable_reuseport, ipv6_only ? 1 : 0);
    if (fd >= 0 || ipv6_only) return fd;
    /* Without IPv6 on this host the socket or the bind of [::] fails with one
     * of these, and a system without dual-stack sockets refuses the option
     * instead. Any other failure, a port in use for example, is the
     * answer. */
    int e = errno;
    if (e != EAFNOSUPPORT && e != EADDRNOTAVAIL && e != EPROTONOSUPPORT &&
        _ipv6_dual_stack_supported()) {
      errno = e;
      return -1;
    }
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_port = htons(port);
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    return _bind_tcp(AF_INET, (struct sockaddr *)&a4, sizeof(a4),
                     enable_reuseport, -1);
  }

  /* A bracketed host, the form a URL gives an IP literal (RFC 3986
   * IP-literal), names the address inside the brackets, and only an address
   * is accepted there, never a name. */
  char unbracketed[256];
  size_t host_len = strlen(host);
  bool bracketed = host[0] == '[';
  if (bracketed) {
    if (host_len < 3 || host[host_len - 1] != ']' ||
        host_len - 2 >= sizeof(unbracketed)) {
      _SRV_ENGINE_LOG(clog_error, "listen host \"%s\" is malformed", host);
      errno = EINVAL;
      return -1;
    }
    memcpy(unbracketed, host + 1, host_len - 2);
    unbracketed[host_len - 2] = '\0';
    host = unbracketed;
  }

  struct addrinfo hints, *res = NULL;
  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  if (bracketed) hints.ai_flags = AI_NUMERICHOST;
  char port_str[8];
  snprintf(port_str, sizeof(port_str), "%u", port);
  int gai = getaddrinfo(host, port_str, &hints, &res);
  if (gai != 0 || !res) {
    int e = errno;
    if (gai == EAI_SYSTEM)
      _SRV_ENGINE_LOG(clog_error,
                      "listen host \"%s\" does not resolve: errno=%d", host, e);
    else
      _SRV_ENGINE_LOG(clog_error, "listen host \"%s\" does not resolve: %s",
                      host, gai != 0 ? gai_strerror(gai) : "no address");
    if (res) freeaddrinfo(res);
    errno = EADDRNOTAVAIL;
    return -1;
  }
  /* A literal resolves only to itself, while a name can resolve to several
   * addresses of both families in the resolver's order, which puts ::1 first
   * for "localhost" on most systems. We take the first IPv4 address, else the
   * first IPv6 one, the same choice Go's net.Listen makes. An IPv6-only
   * listener takes the first IPv6 address instead, and an IPv4 one only when
   * the name has no IPv6 address, in which case ipv6_only has no effect. */
  const int first_family = ipv6_only ? AF_INET6 : AF_INET;
  const int second_family = ipv6_only ? AF_INET : AF_INET6;
  struct addrinfo *pick = NULL;
  for (struct addrinfo *rp = res; rp && !pick; rp = rp->ai_next)
    if (rp->ai_family == first_family) pick = rp;
  for (struct addrinfo *rp = res; rp && !pick; rp = rp->ai_next)
    if (rp->ai_family == second_family) pick = rp;
  int fd = -1;
  if (pick) {
    fd = _bind_tcp(pick->ai_family, pick->ai_addr, pick->ai_addrlen,
                   enable_reuseport, v6only_explicit);
  } else {
    errno = EAFNOSUPPORT;
  }
  int e = errno;
  freeaddrinfo(res);
  errno = e;
  return fd;
}

/* Reads the port and numeric local address of the TCP listener fd, for
 * chttpsvr_bound_port() and chttpsvr_bound_address(). A configured port of 0
 * lets the system choose, so the socket itself is the only place to get the
 * port from. Returns false with errno set when getsockname() fails or gives
 * an address of some other family. */
static bool _listen_sockname(int fd, uint16_t *port_out, char *host,
                             size_t host_len) {
  struct sockaddr_storage ss;
  socklen_t len = sizeof(ss);
  memset(&ss, 0, sizeof(ss));
  if (getsockname(fd, (struct sockaddr *)&ss, &len) != 0) return false;
  const void *addr;
  if (ss.ss_family == AF_INET) {
    const struct sockaddr_in *a4 = (const struct sockaddr_in *)&ss;
    *port_out = ntohs(a4->sin_port);
    addr = &a4->sin_addr;
  } else if (ss.ss_family == AF_INET6) {
    const struct sockaddr_in6 *a6 = (const struct sockaddr_in6 *)&ss;
    *port_out = ntohs(a6->sin6_port);
    addr = &a6->sin6_addr;
  } else {
    errno = EAFNOSUPPORT;
    return false;
  }
  return inet_ntop(ss.ss_family, addr, host, (socklen_t)host_len) != NULL;
}

/* ========================================================================== */
/*                    ROUTER SHELL REGISTRY                                   */
/* ========================================================================== */

/* Every chttpsvr_router has a "shell": the struct itself, i.e. the owner, srv
 * and m_procs fields plus the routes, mw_head, mw_tail and mw_count
 * bookkeeping. The CONTENTS are what the shell points to, the prefix, the mw
 * list nodes, the routes array and each route's data. Every shell is
 * allocated here through the process's plain default allocator, never
 * through a server's possibly custom ccol_memmgmt_procs_t, and
 * _destroy_router never frees a shell at all.
 *
 * The reason is that a chttpsvr_router* a caller holds has no indirection of
 * its own, unlike a chttpsvr handle, whose generation lives in the handle
 * VALUE and never in memory that can be freed from under a reader.
 * chttpsvr_router_on, _on_stream and _use need to read router->owner to find
 * out whether the router's server is still alive, and that read is itself a
 * dereference of `router`.
 *
 * Resolving router->owner through the ordinary chttpsvr slot table protects
 * everything AFTER the resolve: srv, routes and mw_head can't be freed while
 * any resolve is pinned, which __chttpsvr_destroy's pending_resolve_count
 * wait makes sure of because it runs before a single byte of any router's
 * contents is freed. But nothing protects the read of router->owner itself,
 * which happens before any pin exists, and valgrind catches that as a real
 * use-after-free.
 *
 * Nothing narrower closes it, because none of the alternatives touch the
 * shell's lifetime. Resolving router->owner alone leaves the dereference that
 * has to come before that resolve unprotected, and a process-wide rwlock
 * around _destroy_router's free does nothing for a reader that only starts
 * after the free has completed, not just started: once the rwlock is
 * released again, nothing marks the memory as unsafe for whoever holds the
 * bare pointer.
 *
 * So the shell stays allocated for the rest of the process, and it's
 * registered here so an exit-time destructor, built like chttpsvr_slot_table's,
 * can still free every shell exactly once. Reading router->owner is then
 * always memory-safe: _destroy_router stores CHTTPSVR_INVALID into that field
 * and only frees the CONTENTS, so a reader's atomic load sees either a live
 * handle it can resolve or the sentinel, never freed memory. */
static struct {
  _ccol_mutex_t mutex;
  _ccol_once_flag_t once;
  chttpsvr_router **shells; /* plain realloc'd array of every shell still
                             * allocated in the process */
  size_t count;
  size_t capacity;
  /* True once the exit-time destructor has run. From then on a server's
   * destroy frees all its routers' shells at once, and the registry array
   * goes away with its last shell. */
  bool finalized;
} chttpsvr_router_shell_registry = {0};

static void _chttpsvr_router_shell_registry_init_globals(void) {
  if (_ccol_mutex_init(chttpsvr_router_shell_registry.mutex) != 0)
    _ccol_fatal_err(
        "chttpsvr router shell registry: failed to initialize mutex");
}

static void _chttpsvr_router_shell_register(chttpsvr_router *r) {
  _ccol_call_once(chttpsvr_router_shell_registry.once,
                  _chttpsvr_router_shell_registry_init_globals);
  _ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  if (chttpsvr_router_shell_registry.count ==
      chttpsvr_router_shell_registry.capacity) {
    /* Overflow-safe growth like every other growable array in this file
     * (_router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_header,
     * _parse_qparams and _servers_register). A plain "capacity * 2" can wrap
     * on an extreme capacity and then silently allocate less than
     * new_cap * sizeof(ptr) below. A failed or skipped growth degrades exactly
     * like a real allocation failure a few lines down: this one shell just
     * isn't tracked for the exit-time free, which is never a correctness
     * problem. */
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
  /* A failed growth here comes from an allocation failure in this bookkeeping
   * array alone and only means this one shell isn't tracked for the exit-time
   * free below. Everything else stays safe, since nothing touches a router
   * again once _destroy_router has invalidated it, so that rare case leaves a
   * "still reachable" leak and not a correctness problem. */
  r->shell_idx = SIZE_MAX;
  if (chttpsvr_router_shell_registry.count <
      chttpsvr_router_shell_registry.capacity) {
    r->shell_idx = chttpsvr_router_shell_registry.count;
    chttpsvr_router_shell_registry
        .shells[chttpsvr_router_shell_registry.count++] = r;
  }
  _ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* Takes r out of the registry by swapping it with the last shell, and frees
 * the array once the destructor has run and the registry is empty. The
 * caller holds the registry mutex and frees r itself. */
static void _chttpsvr_router_shell_unregister_locked(chttpsvr_router *r) {
  size_t i = r->shell_idx;
  if (i < chttpsvr_router_shell_registry.count &&
      chttpsvr_router_shell_registry.shells[i] == r) {
    chttpsvr_router *last = chttpsvr_router_shell_registry
                                .shells[--chttpsvr_router_shell_registry.count];
    chttpsvr_router_shell_registry.shells[i] = last;
    last->shell_idx = i;
  }
  r->shell_idx = SIZE_MAX;
  if (chttpsvr_router_shell_registry.finalized &&
      chttpsvr_router_shell_registry.count == 0) {
    free(chttpsvr_router_shell_registry.shells);
    chttpsvr_router_shell_registry.shells = NULL;
    chttpsvr_router_shell_registry.capacity = 0;
  }
}

/* Frees the shells of a server's routers that no caller can reach any more as
 * the server is destroyed: always the root router, since it's never handed to
 * a caller, and every router once the exit-time destructor has run, since the
 * process is ending and that destructor frees every destroyed router's shell
 * too. A sub-router of a server destroyed before that keeps its shell, so a
 * stale pointer to it stays safe to pass (see chttpsvr_subrouter(3)). All of
 * the server's routers have already gone through _destroy_router. */
static void _chttpsvr_router_shells_release(chttpsvr_router **routers,
                                            size_t count,
                                            chttpsvr_router *root) {
  _ccol_call_once(chttpsvr_router_shell_registry.once,
                  _chttpsvr_router_shell_registry_init_globals);
  _ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  bool all = chttpsvr_router_shell_registry.finalized;
  for (size_t i = 0; i < count; i++) {
    chttpsvr_router *r = routers[i];
    if (r != root && !all) continue;
    _chttpsvr_router_shell_unregister_locked(r);
    _ccol_default_mem_free(r);
  }
  _ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* Frees every router shell's memory at process exit; otherwise make
 * memtest's valgrind pass reports each one as still reachable. The registry's
 * comment explains why _destroy_router itself never frees this memory.
 *
 * It has exactly the shape of _cleanup_chttpsvr_slot_table, with the same
 * _ccol_call_once guard, the same reasoning and the same in_use and owner
 * liveness check, because a process can link this library without ever
 * creating a chttpsvr, and such a process mustn't lock a mutex here that
 * nobody ever initialized.
 *
 * A shell is only freed here when router->owner is CHTTPSVR_INVALID, which
 * means either _destroy_router already ran for it from a real, completed
 * chttpsvr_destroy() call, or it belongs to a router whose registration
 * failed half way, on chttpsvr_subrouter's second exit path.
 *
 * A shell whose owner is still a live, resolvable handle belongs to a
 * chttpsvr the application never destroyed before exiting, and freeing it
 * here would be a use-after-free: that server's reactor and worker threads
 * are still running, because a teardown from __attribute__((destructor)),
 * unlike an explicit chttpsvr_destroy(), neither stops nor joins them, and
 * the moment one of them dispatches the next request through _find_route(),
 * that function dereferences this exact struct.
 *
 * A shell left unfreed here is a "still reachable" leak, which
 * chttpsvr_slot_table's sibling destructor already tolerates for the same
 * reason. An application that lets a chttpsvr outlive the process exit
 * without destroying it never gets a clean valgrind report for that handle's
 * memory anyway, and a leak that dies with the process beats a crash. */
__attribute__((destructor)) static void _cleanup_chttpsvr_router_shells(void) {
  _ccol_call_once(chttpsvr_router_shell_registry.once,
                  _chttpsvr_router_shell_registry_init_globals);
  _ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  /* Live servers' shells stay, packed at the front of the array, and each
   * such server's destroy frees them later; the array goes with the last of
   * them. */
  size_t kept = 0;
  for (size_t i = 0; i < chttpsvr_router_shell_registry.count; i++) {
    chttpsvr_router *r = chttpsvr_router_shell_registry.shells[i];
    if (atomic_load(&r->owner) == CHTTPSVR_INVALID) {
      r->shell_idx = SIZE_MAX;
      _ccol_default_mem_free(r);
    } else {
      r->shell_idx = kept;
      chttpsvr_router_shell_registry.shells[kept++] = r;
    }
  }
  chttpsvr_router_shell_registry.count = kept;
  chttpsvr_router_shell_registry.finalized = true;
  if (kept == 0) {
    free(chttpsvr_router_shell_registry.shells);
    chttpsvr_router_shell_registry.shells = NULL;
    chttpsvr_router_shell_registry.capacity = 0;
  }
  _ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* ========================================================================== */
/*                    FORK SAFETY (pthread_atfork)                            */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() only duplicates the calling thread, so the child inherits any lock
 * some OTHER thread held at that moment in a permanently locked state, with no
 * thread left in the child that could ever unlock it.
 *
 * So this prepare() handler takes every lock of this module before fork() may
 * go ahead: the process-wide registries we own, plus the mutex, idle_mutex,
 * diverted_mutex, wait_mutex and routes_lock of every live server. The
 * registries are chttpsvr_slot_table, srv_engine_bundler together with
 * servers_bundler (they share one lazy-init guard, see _engine_globals_init),
 * and chttpsvr_router_shell_registry. parent() and child() then release them
 * again through one shared function.
 *
 * That shared release is well defined because every mutex in this module is
 * of the default "normal" pthread type, which on Linux glibc tracks neither an
 * owner nor a TID, so a plain pthread_mutex_unlock is fine even from a thread
 * other than the one that locked it. For anything the forking thread didn't
 * hold itself, the locking thread doesn't even exist in the child.
 *
 * routes_lock is a pthread_rwlock_t, not a plain mutex. We take it for write
 * so the handler excludes concurrent readers as well as writers. The doc
 * comment of _chttpsvr_atfork_release_impl explains why the child must
 * treat it differently from the plain mutexes: unlike those, a glibc rwlock's
 * write lock DOES track ownership by TID, so a plain unlock from the child,
 * whose thread has a different number, silently fails to release it.
 *
 * It's the same atfork handling as in cthreadpool.c, cthreadcomm.c and
 * clogger.c, for the same reason: no other module can reach into this one's
 * opaque globals to protect them from outside, so every module owning
 * process-wide state that any thread can lock must register its own
 * handlers.
 *
 * Some of our state needs no handler here: worker_pool and reject_pool
 * (ctpool handles), the shared reactor (a ccol_event_loop handle) and every
 * clog handle this module opens or derives (the engine-wide diagnostics
 * logger and each server's logger). cthreadpool.c's, cthreadcomm.c's and
 * clogger.c's own atfork registrations already protect all of them, so
 * repeating that here would add nothing.
 *
 * Each registry's lazy _ccol_call_once guard runs here first, like at every
 * ordinary lock site elsewhere in this file (_engine_wait_until_stopped for
 * example), so a fork() that lands before anything has touched this module's
 * engine or router machinery initializes it safely before going on, instead
 * of locking uninitialized memory. glibc's malloc arena locks rely on the
 * same pattern being safe inside an atfork handler.
 *
 * The walk only visits slots with in_use true, exactly the condition every
 * resolve function already trusts as the only sign that slot->ptr is safe to
 * dereference, and chttpsvr_create_mp's construction order makes sure every
 * lock of a server is fully initialized before its slot is marked in_use.
 *
 * With is_child set, the release also resets three flags to false:
 * srv_engine_bundler.reaper_joinable, srv_engine_bundler.stopping and
 * idle_sweep_bundler.running (the doc comment of
 * _chttpsvr_atfork_release_impl has the full reasoning). This module has two
 * background threads of its own, and either may be running or about to be
 * spawned when the fork() happens. Without these resets the child inherits a
 * stale thread identifier naming a thread that doesn't exist in this process,
 * and a later _ccol_thread_join() on it has no defined outcome under POSIX
 * and hangs in practice. That join is reachable from
 * _join_reaper_if_needed_locked() and _idle_sweep_stop_if_running(), and an
 * ordinary chttpsvr_engine_wait() or chttpsvr_engine_stop() in the child gets
 * to both.
 *
 * A stopping flag stuck at true makes it worse. It keeps every later
 * chttpsvr_engine_stop() in the child from spawning a real reaper of its own,
 * and every later chttpsvr_start() and _engine_acquire() from getting past
 * its wait for a broadcast, because whoever would have sent that broadcast
 * vanished with the fork.
 *
 * None of these resets tries to reclaim or join a vanished thread; each just
 * says there's nothing left to wait for. g_engine_stop_watcher.started gets
 * the same treatment just below, and ctpool's and ccol_event_loop's
 * foreign_since_fork machinery does the same for their own worker threads. */
static void _chttpsvr_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(_ccol_atfork_module_chttpserver);
#endif
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);

  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  /* servers_bundler.mutex is locked BEFORE srv_engine_bundler.mutex. No
   * ordinary path holds both at once: _idle_sweep_fn's per-tick snapshot
   * releases servers_bundler.mutex before its later, separate _SRV_ENGINE_LOG
   * call touches srv_engine_bundler.mutex (see that function's comment), so
   * with the code as it is either order would be equally safe.
   *
   * We still pick one order on purpose, because a change can easily end up
   * nesting the two, e.g. by holding servers_bundler.mutex across a
   * diagnostics call that logs, and then the REVERSE order is a real AB-BA
   * deadlock against this handler. With one canonical order every change that
   * nests these two locks has exactly one order to follow, and nobody needs to
   * work out again which one is safe. */
  _ccol_mutex_lock(servers_bundler.mutex);
  _ccol_mutex_lock(srv_engine_bundler.mutex);

  _ccol_call_once(chttpsvr_router_shell_registry.once,
                  _chttpsvr_router_shell_registry_init_globals);
  _ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  /* A leaf lock the idle sweep thread holds around its wait (see the comment
   * of idle_sweep_bundler). */
  _ccol_mutex_lock(idle_sweep_bundler.wake_mu);

  /* A table released at unload holds no server. */
  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    _ccol_mutex_lock(slot->ptr->mutex);
    _ccol_mutex_lock(slot->ptr->idle_mutex);
    _ccol_mutex_lock(slot->ptr->diverted_mutex);
    _ccol_mutex_lock(slot->ptr->wait_mutex);
    _ccol_rw_lock_wrlock(slot->ptr->routes_lock);
  }
}

/* Shared by parent() and child(). The doc comment of _chttpsvr_atfork_prepare
 * explains why a plain unlock is right in both branches for this module's
 * plain mutexes.
 *
 * Walking the same structure prepare() just walked and releasing every lock
 * symmetrically is safe, because nothing can have changed the slot table or
 * any live server's state in between: every lock such a change would need is
 * still held at this point.
 *
 * routes_lock is a pthread_rwlock_t and needs different handling in the
 * child. The hazard is real and reproduces directly, the same one
 * clog_slot_table.rwlock has in clogger.c, whose doc comment tells the whole
 * story of the hang this avoids.
 *
 * A plain pthread_rwlock_unlock() of the write lock from the child's only
 * thread is NOT a valid release. The parent's forking thread took the lock in
 * _chttpsvr_atfork_prepare(), glibc's rwlock write lock tracks ownership by
 * TID internally, and after the fork the child's thread has a different TID
 * than the parent's forking thread, so the unlock silently releases nothing.
 * Every later chttpsvr_use() or chttpsvr_register_handler() on that server in
 * the child then hangs forever, as does anything else reaching
 * _router_add_route or _router_add_mw. A plain _ccol_rw_lock_unlock() in both
 * branches hangs on every run of tests/chttpserver's
 * fork_does_not_inherit_a_locked_mutex stress test.
 *
 * The standard fix is to initialize the lock again in the child instead of
 * unlocking it, which is what glibc's malloc arena locks do for exactly this
 * case and what clogger.c does in this codebase. It's safe because the child
 * has exactly one thread, so nobody else can be waiting on the lock and a
 * fresh init has nobody to race.
 *
 * A plain default-type mutex doesn't behave like this, since the fast/normal
 * mutex type used throughout this codebase does no TID tracking (see
 * common.h's _ccol_mutex_init()). So only routes_lock needs this, not a
 * server's mutex, idle_mutex, diverted_mutex and wait_mutex, and not the four
 * process-wide registry mutexes.
 *
 * With is_child set, three counters of every live server are also reset to
 * 0: pending_resolve_count, servers_bundler_pins and listener_dispatch_pins,
 * each of which __chttpsvr_destroy waits on without a timeout before freeing
 * the server. A parent thread that vanished with the fork may have been half
 * way through a resolve, a quiesce scan or an accept dispatch at that moment,
 * so as far as this process is concerned one of these counters would stay
 * non-zero forever with no thread left to decrement it. ctpool's atfork
 * release applies the same fix to its own pending_resolve_count, against the
 * same kind of hazard.
 *
 * in_flight_requests is on purpose NOT reset. The wait on it, in
 * _wait_in_flight_bounded, is already bounded and escalates to a forced
 * shutdown, so a stale non-zero count there costs a bounded delay and not a
 * permanent hang, while resetting it here could let a destroy free a
 * connection that a still existing worker thread really has in flight.
 *
 * With is_child set, g_engine_stop_watcher.started and
 * g_engine_stop_watcher.ready are also reset to false. The watcher is a
 * dedicated OS thread of its own (see the section comment above
 * _engine_stop_watcher_fn) that fork() doesn't duplicate, any more than it
 * duplicates srv_engine_bundler.reaper_thread or idle_sweep_bundler.thread,
 * while its started guard arrives in the child as an exact copy of the
 * parent's.
 *
 * Without that reset chttpsvr_engine_stop() would never work again in this
 * child, not only against the inherited, inert reactor but against any FUTURE,
 * really fresh chttpsvr_start() or _engine_acquire() too, because
 * _engine_stop_watcher_ensure_started_locked()'s `if (started) return;` would
 * read the stale inherited true and never create a real watcher thread for
 * this child.
 *
 * The sem and the thread are left untouched, no explicit destroy or reinit,
 * and that's safe: once started is false again, the child's next
 * _engine_acquire() calls _ccol_semaphore_init() on the inherited sem_t
 * itself, which is well defined since nothing in this child is or ever was
 * blocked on it. It's the same reinit-without-destroy approach as for
 * routes_lock above.
 *
 * With is_child set, a live server's quiesce_state and quiesce_waiters get
 * fixed up as well, but NOT by simply resetting quiesce_state to
 * CHTTPSVR_QS_NOT_QUIESCED, which isn't enough. Unlike the pin counters
 * above, this field gates a multi-step teardown with real state,
 * _quiesce_server_once, and fork() can land while a thread that then vanished
 * was half way through it, having done an unknown subset of its steps: the
 * listener stopped or not, connections drained or not, the engine reference
 * released or not, the worker pool destroyed or not, with srv->mutex released
 * between most of those steps.
 *
 * Resetting to CHTTPSVR_QS_NOT_QUIESCED would let a later chttpsvr_start() or
 * _quiesce_server_once in this child retry or re-enter that sequence, even
 * though the server's real state can't be safely inferred from this one enum
 * value. That risks a double release or a double free, a second
 * _destroy_detached_pools on a pool the vanished thread already freed for
 * instance.
 *
 * Instead we handle exactly the servers the fork() caught in the middle of a
 * teardown, those with quiesce_state == CHTTPSVR_QS_QUIESCING, the ONLY value
 * a vanished thread can leave behind: a server that was never in a teardown
 * is CHTTPSVR_QS_NOT_QUIESCED, and one whose teardown fully finished is
 * CHTTPSVR_QS_QUIESCED.
 *
 * For such a server we mark the interrupted teardown as finished by setting
 * quiesce_state to CHTTPSVR_QS_QUIESCED and quiesce_waiters to 0, which is
 * what the vanished thread's tail would eventually have done (see the doc
 * comment of _quiesce_server_once). Then srv is removed from
 * servers_bundler.servers[] with _servers_unregister_locked. Calling that
 * directly is safe here while going through _servers_unregister isn't,
 * because this whole loop already holds servers_bundler.mutex, inherited
 * locked from _chttpsvr_atfork_prepare, and the ordinary entry point locks
 * that mutex itself, so the second, recursive lock would deadlock.
 *
 * That second step matters on its own. Normally it's _quiesce_server_once's
 * real work that unregisters srv, and we skip that real work here, so nothing
 * else would ever unregister it. Without this step srv would stay in
 * servers_bundler.servers[] forever, and _engine_force_stop_quiesce_all's
 * driver loop, which rereads servers_bundler.servers[0] and only gets past an
 * entry once _servers_unregister removes it, would spin forever on this
 * never-removed entry the next time chttpsvr_engine_stop() runs in this
 * child. That's a second permanent hang, reachable on its own, and forcing
 * CHTTPSVR_QS_QUIESCED without the unregister would leave it wide open.
 *
 * The fixup still leaves things unreleased in this process: the interrupted
 * server's worker pool, its TLS context and whatever connections it held at
 * the moment of the fork(). We make no attempt to replay or infer that
 * bookkeeping, for the same double-free risk as above, so a later
 * chttpsvr_destroy() or chttpsvr_start() on this handle in the child returns
 * promptly instead of hanging, at the price of leaking whatever the vanished
 * thread hadn't released yet. If the handle is ever destroyed or restarted,
 * cthreadpool.c's foreign_since_fork machinery, which exists for exactly this
 * "destroy a pool inherited across a fork" case, makes reclaiming the worker
 * pool safe.
 *
 * With is_child set, every live server's lifecycle is also put back to
 * CHTTPSVR_LC_IDLE whenever it's CHTTPSVR_LC_STOPPING or
 * CHTTPSVR_LC_STARTING.
 *
 * CHTTPSVR_LC_STOPPING is different from quiesce_state above:
 * _chttpsvr_stop_internal() leaves CHTTPSVR_LC_RUNNING durably and
 * atomically, and in the SAME critical section that enters
 * CHTTPSVR_LC_STOPPING it also resets listen_fd, listen_reg and unix_bind to
 * their "nothing left to clean up" sentinels, all before any of its real work
 * (ccol_event_loop_remove(), close() and unlink()) starts, which is where a
 * fork() can interrupt it. So a fork() anywhere in that work can never leave
 * those fields in a state chttpsvr_start() could misread, unlike the
 * multi-step _quiesce_server_once sequence above that releases srv->mutex
 * between steps.
 *
 * For CHTTPSVR_LC_STARTING, a fork() mid-call can leave bits of state half
 * set, and the next chttpsvr_start() on the same handle rebuilds every one of
 * them. _retire_pools and _destroy_detached_pools always drain and destroy
 * whatever worker_pool and reject_pool are published, which is safe even for
 * a possibly half-built pool inherited across a fork thanks to
 * cthreadpool.c's foreign_since_fork machinery, the same mechanism the
 * quiesce_state case above relies on. chttpsvr_start() also always releases
 * and rebuilds any published tls_ctx from scratch, and a tls_ctx is plain heap
 * memory, not a slot-table handle, so a ctls_ctx_release() on it after a fork
 * needs nothing special. _servers_register() is idempotent itself (see the
 * comment at its call site in chttpsvr_start()), so a server registered
 * before the fork isn't registered twice by the next start attempt.
 *
 * It's the same graceful-leak approach the quiesce_state fixup of this
 * function already takes, not a new kind of risk. The vanished thread may
 * already have created a listener fd and registration for THIS start attempt,
 * as opposed to an old one chttpsvr_stop() already tore down. A lifecycle of
 * CHTTPSVR_LC_IDLE never references that fd and the next successful start
 * just overwrites it; if no start ever succeeds it stays as an unreferenced,
 * harmless leak.
 *
 * Without these resets a lifecycle stuck at CHTTPSVR_LC_STOPPING or
 * CHTTPSVR_LC_STARTING disables the handle in the child for good. For
 * STOPPING, chttpsvr_start()'s retry loop polls forever with a plain 1ms
 * sleep on that state, not a condvar wait. For STARTING, chttpsvr_start()
 * refuses at once with ccol_not_permitted and never retries, treating the
 * state exactly like a second, really concurrent chttpsvr_start().
 * chttpsvr_stop() can't unstick either state, because in both cases
 * _chttpsvr_stop_internal's was_started check comes out false, a silent no-op
 * that never touches lifecycle.
 *
 * Resetting both states to CHTTPSVR_LC_IDLE is safe here, since there's no
 * multi-step state to misread, only this one enum value. It has one cost, in
 * line with the quiesce_state case above: nothing in this process explicitly
 * releases the OLD listener fd or the OLD unix_bind path and directory fd, if
 * any. That listener fd is still open in two cases, for CHTTPSVR_LC_STOPPING
 * when the vanished thread hadn't got to its close() yet, and for
 * CHTTPSVR_LC_STARTING when it had already got to its ccol_event_loop_add().
 * So a later chttpsvr_start() on the same host and port may see a spurious
 * EADDRINUSE-type failure from that orphaned fd instead of a clean rebind,
 * which is a graceful failure the code already handles and not a crash. */
static void _chttpsvr_atfork_release_impl(bool is_child) {
  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct chttpserver *srv = slot->ptr;

    if (is_child) {
      atomic_store(&srv->pending_resolve_count, (size_t)0);
      atomic_store(&srv->servers_bundler_pins, (size_t)0);
      atomic_store(&srv->listener_dispatch_pins, (size_t)0);
      /* A fresh init, not an unlock; the doc comment of this function
       * describes the hazard it avoids, glibc's rwlock write lock tracking
       * its owner's TID. */
      if (_ccol_rw_lock_reinit_in_child(srv->routes_lock) != 0)
        _ccol_fatal_err(
            "chttpsvr atfork release: failed to reinit routes_lock");

      /* This function's doc comment explains both fixups
       * below. Each is an exhaustive switch without a `default:` label rather
       * than a plain comparison, for the `-Wswitch` reason the field comments
       * of chttpsvr_lifecycle_t and chttpsvr_quiesce_state_t give. */
      switch (srv->quiesce_state) {
        case CHTTPSVR_QS_QUIESCING: {
          srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
          srv->quiesce_waiters = 0;
          /* The locked variant, since this whole loop already holds
           * servers_bundler.mutex, inherited locked from
           * _chttpsvr_atfork_prepare. */
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
      _ccol_rw_lock_unlock(srv->routes_lock);
    }
    _ccol_mutex_unlock(srv->wait_mutex);
    _ccol_mutex_unlock(srv->diverted_mutex);
    _ccol_mutex_unlock(srv->idle_mutex);
    _ccol_mutex_unlock(srv->mutex);
  }

  if (is_child) {
    /* The doc comment of this function says why the child has to reset this.
     * Otherwise _engine_stop_watcher_ensure_started_locked()'s `started`
     * guard would block every attempt to create a real watcher thread for
     * this child for the rest of the process. */
    g_engine_stop_watcher.started = false;
    atomic_store(&g_engine_stop_watcher.ready, false);
    /* Both request flags go too, because each names work for a watcher thread
     * that doesn't exist in this child. An inherited join request points at a
     * reaper that isn't here either, and an inherited stop request would run
     * again against the first engine this child starts, exactly the forgotten
     * stop coming back to life that the comment of
     * _engine_stop_watcher_ensure_started_locked rules out for the parent. */
    atomic_store(&g_engine_stop_watcher.join_reaper_requested, false);
    atomic_store(&g_engine_stop_watcher.stop_requested, (uintptr_t)0);

    /* See the doc comment of _chttpsvr_atfork_prepare for the full reasoning.
     * This module owns two background threads, the shared engine's reaper and
     * the idle-timeout sweep thread, and either may still be marked live from
     * the parent's point of view while neither exists in this child. A later
     * _ccol_thread_join() on such a stale identifier has no defined outcome
     * under POSIX and hangs in practice. That join comes from
     * _join_reaper_if_needed_locked() or _idle_sweep_stop_if_running(), and
     * an ordinary chttpsvr_engine_wait() or chttpsvr_engine_stop() reaches
     * both.
     *
     * srv_engine_bundler.mutex guards the first of these fields and
     * servers_bundler.mutex the second. This function already holds both, so
     * the two writes need no extra lock. */
    srv_engine_bundler.reaper_joinable = false;
    idle_sweep_bundler.running = false;
    /* The parent's sweep thread may have been waiting on wake_cv and doesn't
     * exist here, so the child starts with a new condition variable with no
     * waiters, on the same clock as the ordinary init. */
    if (_idle_sweep_init_wake_cv() != 0)
      _ccol_fatal_err(
          "chttpserver atfork child: failed to reinit the sweep condition "
          "variable");

    /* srv_engine_bundler.stopping can also get stuck at true,
     * whenever fork() lands while a real reaper pass on the parent side is
     * running or about to: _engine_release_begin() and _engine_force_stop_now()
     * set it true strictly before _spawn_reaper() runs, so it can be true even
     * in the narrow window where reaper_joinable above is still false.
     *
     * Stuck at true it would keep EVERY later chttpsvr_engine_stop() in the
     * child from spawning a real reaper of its own, because
     * _engine_force_stop_now()'s !stopping guard, meant to reject only a real,
     * redundant second call, would silently reject all future ones. It would
     * also block EVERY later chttpsvr_start() and _engine_acquire(), whose
     * `while (stopping) _ccol_cond_var_wait(...)` loop waits for a broadcast
     * only the vanished reaper thread's completion would ever send.
     *
     * Resetting it is safe. The child's inherited reactor has no poller here,
     * and the child's first call that acquires, stops or waits for the engine
     * forgets it (see _engine_forget_inherited_locked, which decides by
     * process ID and so also covers a build without these handlers). */
    srv_engine_bundler.stopping = false;
  }

  _ccol_mutex_unlock(idle_sweep_bundler.wake_mu);
  _ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
  _ccol_mutex_unlock(servers_bundler.mutex);
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

static void _chttpsvr_atfork_release(void) {
  _chttpsvr_atfork_release_impl(false);
}

/* The child-side counterpart of _chttpsvr_atfork_release. It releases the same
 * locks (see the comment of _chttpsvr_atfork_release_impl) and also resets
 * every live server's pin counters, the ones a wait could block on forever.
 *
 * It must run before any application code in this process can reach one of
 * these servers' shutdown or destroy path, and it does, since pthread_atfork's
 * child handler runs synchronously as part of fork()'s own return, strictly
 * before fork()'s return value reaches the caller. */
static void _chttpsvr_atfork_child_release(void) {
  _chttpsvr_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

/* Counts a router prefix's '/'-separated segments. The prefix has no trailing
 * slash and no consecutive slashes, which is always the shape of r->prefix by
 * the time we get here: chttpsvr_subrouter refuses a "//" in the caller's
 * prefix before _create_router ever runs, and the loop just above this
 * function's call site strips trailing slashes without adding new ones.
 *
 * Two prefixes have zero segments: "", the root's placeholder, and "/", the
 * special prefix that only matches the exact root. Every other valid prefix
 * has exactly one segment per '/' it contains, because exactly one of them
 * comes before each segment: there's no leading run since every prefix starts
 * with a single '/', no trailing one since it was stripped, and no run inside
 * since that was refused. */
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
  /* The shell itself does NOT use `mp`, the server's possibly custom
   * allocator, but _ccol_default_mem_calloc and _ccol_default_mem_free, the
   * plain default-allocator macros, since the _mem_* ones need a real
   * ccol_memmgmt_procs_t argument and not a bare NULL literal. The comment of
   * chttpsvr_router_shell_registry explains why this struct's memory must
   * stay valid for the rest of the process, well past srv's lifetime and so
   * well past mp's. */
  chttpsvr_router *r =
      (chttpsvr_router *)_ccol_default_mem_calloc(1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    _ccol_default_mem_free(r);
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
  /* Invalidate the router before freeing any of its contents.
   * _chttpsvr_finish_destroy only gets here after __chttpsvr_destroy's
   * pending_resolve_count wait has confirmed that no resolve of this router's
   * owner is pinned and none can start, so no reader can be inside
   * _router_add_route or _router_add_mw right now. CHTTPSVR_INVALID is stored
   * here and not after the frees so that a brand new reader arriving from now
   * on sees the sentinel and returns immediately; otherwise it would race the
   * frees below. */
  atomic_store(&r->owner, CHTTPSVR_INVALID);
  chttpsvr_mw_node_t *mw = r->mw_head;
  while (mw) {
    chttpsvr_mw_node_t *next = mw->next;
    _ccol_mem_free(mp, mw);
    mw = next;
  }
  r->mw_head = r->mw_tail = NULL;
  r->mw_count = 0;
  for (size_t i = 0; i < r->route_count; i++) {
    _free_route_data(r->routes[i], mp);
    _ccol_mem_free(mp, r->routes[i]);
  }
  _ccol_mem_free(mp, r->routes);
  r->routes = NULL;
  r->route_count = r->route_cap = 0;
  _ccol_mem_free(mp, r->prefix);
  r->prefix = NULL;
  r->srv = NULL;
  /* The shell itself (this struct) is on purpose NOT freed here; see
   * chttpsvr_router_shell_registry's comment above. */
}

static ccol_retval_t _router_add_route(chttpsvr_router *router,
                                       chttp_method_t method,
                                       const char *pattern,
                                       chttpsvr_handler_fn fn, void *ctx,
                                       bool is_streaming) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  /* A route's method ends up as a bit in a mask (see _find_route), so only
   * the seven concrete methods and CHTTP_ANY mean anything here. */
  if ((unsigned int)method > (unsigned int)CHTTP_ANY) return ccol_invalid_args;

  ccol_memmgmt_procs_t *mp = router->m_procs;

  chttpsvr_route_t *rt =
      (chttpsvr_route_t *)_ccol_mem_alloc(mp, sizeof(chttpsvr_route_t));
  if (!rt) return ccol_not_enough_memory;
  memset(rt, 0, sizeof(*rt));
  rt->method = method;
  rt->fn = fn;
  rt->ctx = ctx;
  rt->is_streaming = is_streaming;

  ccol_retval_t rv = _compile_pattern(pattern, &rt->segs, &rt->seg_count,
                                      &rt->param_names, &rt->param_count, mp);
  if (rv != ccol_success) {
    _ccol_mem_free(mp, rt);
    return rv;
  }

  _ccol_rw_lock_wrlock(router->srv->routes_lock);
  if (router->route_count >= router->route_cap) {
    if (router->route_cap > (SIZE_MAX - 4) / 2 ||
        router->route_cap * 2 + 4 > SIZE_MAX / sizeof(chttpsvr_route_t *)) {
      _ccol_rw_lock_unlock(router->srv->routes_lock);
      _free_route_data(rt, mp);
      _ccol_mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    size_t new_cap = router->route_cap * 2 + 4;
    chttpsvr_route_t **nr = (chttpsvr_route_t **)_ccol_mem_realloc(
        mp, router->routes, new_cap * sizeof(chttpsvr_route_t *));
    if (!nr) {
      _ccol_rw_lock_unlock(router->srv->routes_lock);
      _free_route_data(rt, mp);
      _ccol_mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    router->routes = nr;
    router->route_cap = new_cap;
  }
  router->routes[router->route_count++] = rt;
  if (rt->seg_count > router->max_route_seg_count)
    router->max_route_seg_count = rt->seg_count;
  _ccol_rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

static ccol_retval_t _router_add_mw(chttpsvr_router *router,
                                    chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  ccol_memmgmt_procs_t *mp = router->m_procs;
  chttpsvr_mw_node_t *node =
      (chttpsvr_mw_node_t *)_ccol_mem_alloc(mp, sizeof(chttpsvr_mw_node_t));
  if (!node) return ccol_not_enough_memory;
  node->fn = fn;
  node->ctx = ctx;
  node->next = NULL;

  _ccol_rw_lock_wrlock(router->srv->routes_lock);
  if (router->mw_count >= _CHTTPSVR_MAX_MW) {
    _ccol_rw_lock_unlock(router->srv->routes_lock);
    _ccol_mem_free(mp, node);
    return ccol_not_permitted;
  }
  if (!router->mw_tail) {
    router->mw_head = router->mw_tail = node;
  } else {
    router->mw_tail->next = node;
    router->mw_tail = node;
  }
  router->mw_count++;
  _ccol_rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

/* ========================================================================== */
/*                         QUERY PARAMETER PARSING                            */
/* ========================================================================== */

static ccol_retval_t _parse_qparams(const char *raw_query,
                                    chttpsvr_qparams_t **qp_out,
                                    ccol_memmgmt_procs_t *mp) {
  chttpsvr_qparams_t *qp =
      (chttpsvr_qparams_t *)_ccol_mem_calloc(mp, 1, sizeof(chttpsvr_qparams_t));
  if (!qp) return ccol_not_enough_memory;
  qp->m_procs = mp;

  const char *p = raw_query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
    /* Skip a really empty pair, with nothing at all between two '&', before a
     * leading one or after a trailing one, like in "a=1&&b=2" and "&a=1",
     * instead of adding a bogus entry with an empty key and empty value for
     * it.
     *
     * Two other forms look similar but mustn't be confused with this one, and
     * both are supported on purpose. "?=value" has an empty key and a real
     * value, and its pair_len is above 0 since "=value" isn't empty. "?key"
     * has a real key and an implicit empty value, and its pair_len is above 0
     * as well. This check leaves both alone and the code below parses each as
     * its own entry; both are already documented and tested. */
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
      key_sp = (char *)_ccol_mem_alloc(mp, key_raw_len + 1);
      kh = true;
      if (!key_sp) goto oom;
    }
    memcpy(key_sp, p, key_raw_len);
    key_sp[key_raw_len] = '\0';
    char *key_decoded = (char *)_ccol_mem_alloc(mp, key_raw_len + 1);
    if (!key_decoded) {
      if (kh) _ccol_mem_free(mp, key_sp);
      goto oom;
    }
    ssize_t kl = _decode_url_unsafe(key_decoded, key_sp);
    if (kh) _ccol_mem_free(mp, key_sp);
    if (kl < 0) {
      _ccol_mem_free(mp, key_decoded);
      goto next;
    }
    key_decoded[(size_t)kl] = '\0';

    char *val_decoded = NULL;
    if (val_raw) {
      char val_src[512];
      char *val_sp = val_src;
      bool vh = false;
      if (val_raw_len + 1 > sizeof(val_src)) {
        val_sp = (char *)_ccol_mem_alloc(mp, val_raw_len + 1);
        vh = true;
        if (!val_sp) {
          _ccol_mem_free(mp, key_decoded);
          goto oom;
        }
      }
      memcpy(val_sp, val_raw, val_raw_len);
      val_sp[val_raw_len] = '\0';
      val_decoded = (char *)_ccol_mem_alloc(mp, val_raw_len + 1);
      if (!val_decoded) {
        if (vh) _ccol_mem_free(mp, val_sp);
        _ccol_mem_free(mp, key_decoded);
        goto oom;
      }
      ssize_t vl = _decode_url_unsafe(val_decoded, val_sp);
      if (vh) _ccol_mem_free(mp, val_sp);
      if (vl < 0) {
        _ccol_mem_free(mp, val_decoded);
        _ccol_mem_free(mp, key_decoded);
        goto next;
      }
      val_decoded[(size_t)vl] = '\0';
    } else {
      val_decoded = ccol_strdup(mp, "");
      if (!val_decoded) {
        _ccol_mem_free(mp, key_decoded);
        goto oom;
      }
    }

    if (qp->count >= qp->cap) {
      /* Overflow-safe growth, like every other growable array in this file
       * (_router_add_route, chttpsvr_subrouter, chttpsvr_resp_set_header). A
       * plain "cap * 2 + 8" can wrap on an extreme qp->cap and then silently
       * allocate less than nc * sizeof(char *) below. */
      if (qp->cap > (SIZE_MAX - 8) / 2 ||
          qp->cap * 2 + 8 > SIZE_MAX / sizeof(char *)) {
        _ccol_mem_free(mp, key_decoded);
        _ccol_mem_free(mp, val_decoded);
        goto oom;
      }
      size_t nc = qp->cap * 2 + 8;
      char **nk = (char **)_ccol_mem_realloc(mp, qp->keys, nc * sizeof(char *));
      if (!nk) {
        _ccol_mem_free(mp, key_decoded);
        _ccol_mem_free(mp, val_decoded);
        goto oom;
      }
      qp->keys = nk;
      char **nv =
          (char **)_ccol_mem_realloc(mp, qp->values, nc * sizeof(char *));
      if (!nv) {
        _ccol_mem_free(mp, key_decoded);
        _ccol_mem_free(mp, val_decoded);
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
    _ccol_mem_free(mp, qp->keys[i]);
    _ccol_mem_free(mp, qp->values[i]);
  }
  _ccol_mem_free(mp, qp->keys);
  _ccol_mem_free(mp, qp->values);
  _ccol_mem_free(mp, qp);
  return ccol_not_enough_memory;
}

static chttpsvr_qparams_t *_ensure_qparams(chttpsvr_req *req) {
  chttpsvr_conn_t *conn = req->conn;
  if (req->_qparams) return req->_qparams;
  if (req->_qparams_attempted) return NULL;
  req->_qparams_attempted = true;

  if (!conn->raw_query || !*conn->raw_query) {
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_ccol_mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams_t));
    if (!qp) {
      /* Matches the _parse_qparams failure branch below. This branch runs for
       * a request without any query string, the common case, and without the
       * flag chttpsvr_req_query_oom() couldn't tell an allocation failure
       * here from a real "the key isn't there", which is exactly what that
       * function is documented to do. */
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
    chttpsvr_qparams_t *qp = (chttpsvr_qparams_t *)_ccol_mem_calloc(
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

chttpsvr chttpsvr_create_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                            char **err_str) {
  if (!_ccol_verify_memmgmt_procs(mprocs, err_str)) return CHTTPSVR_INVALID;

  /* This module's exit-time destructor releases the handle table once no
   * server is left. A create that comes after that, from one of the
   * application's destructors or atexit handlers, fails here, before it
   * allocates or registers anything nobody would ever free. */
  if (_chttpsvr_slot_table_released()) {
    if (err_str)
      *err_str = _CCOL_ERR_STR(
          "the library already released its server table at unload");
    return CHTTPSVR_INVALID;
  }

  struct chttpserver *srv = (struct chttpserver *)_ccol_mem_calloc(
      mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = _CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }
  atomic_init(&srv->lifetime_refs, 1);

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs_t *)_ccol_mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs_t));
    if (!srv->m_procs) {
      _ccol_mem_free(mprocs, srv);
      if (err_str) *err_str = _CCOL_ERR_STR("out of memory");
      return CHTTPSVR_INVALID;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  /* A closed or stale parent logger is a caller error we report through the
   * error string; clog_derive() would end the process instead. */
  ccol_retval_t derive_rv = ccol_success;
  clog logger = cl ? _clog_derive_checked(cl, &derive_rv)
                   : clog_create_from_fd_mp(2, CLOG_FATAL, NULL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str)
      *err_str = derive_rv == ccol_invalid_args
                     ? _CCOL_ERR_STR("the parent logger is not open")
                     : _CCOL_ERR_STR("failed to create server logger");
    return CHTTPSVR_INVALID;
  }

  if (_ccol_mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (_ccol_mutex_init(srv->idle_mutex) != 0) {
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (_ccol_mutex_init(srv->diverted_mutex) != 0) {
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (_ccol_mutex_init(srv->wait_mutex) != 0) {
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (_ccol_cond_var_init(srv->resolve_cv) != 0) {
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  {
    /* CLOCK_MONOTONIC, to match _wait_in_flight_bounded's deadline, which it
     * builds with clock_gettime(CLOCK_MONOTONIC, ...). _ccol_cond_var_init
     * defaults to CLOCK_REALTIME, which would make that comparison wrong by
     * comparing a monotonic timespec against a condvar that uses the wall
     * clock inside. */
    _ccol_cond_var_attr_t cv_attr;
    int cv_rc = 0;
    if (_ccol_cond_var_attr_init(cv_attr) == 0) {
      _ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
      cv_rc = _ccol_cond_var_init_ca(srv->requests_done_cv, cv_attr);
      _ccol_cond_var_attr_destroy(cv_attr);
    } else {
      cv_rc = 1;
    }
    if (cv_rc != 0) {
      _ccol_cond_var_destroy(srv->resolve_cv);
      _ccol_mutex_destroy(srv->idle_mutex);
      _ccol_mutex_destroy(srv->diverted_mutex);
      _ccol_mutex_destroy(srv->wait_mutex);
      _ccol_mutex_destroy(srv->mutex);
      clog_close(logger);
      _ccol_mem_free(mprocs, srv->m_procs);
      _ccol_mem_free(mprocs, srv);
      if (err_str) *err_str = _CCOL_ERR_STR("_ccol_cond_var_init failed");
      return CHTTPSVR_INVALID;
    }
  }

  if (_ccol_cond_var_init(srv->quiesce_done_cv) != 0) {
    _ccol_cond_var_destroy(srv->requests_done_cv);
    _ccol_cond_var_destroy(srv->resolve_cv);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  if (_ccol_rw_lock_init(srv->routes_lock) != 0) {
    _ccol_cond_var_destroy(srv->quiesce_done_cv);
    _ccol_cond_var_destroy(srv->requests_done_cv);
    _ccol_cond_var_destroy(srv->resolve_cv);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("_ccol_rw_lock_init failed");
    return CHTTPSVR_INVALID;
  }

  chttpsvr_router *root = _create_router(srv, "", srv->m_procs);
  if (!root) {
    _ccol_rw_lock_destroy(srv->routes_lock);
    _ccol_cond_var_destroy(srv->quiesce_done_cv);
    _ccol_cond_var_destroy(srv->requests_done_cv);
    _ccol_cond_var_destroy(srv->resolve_cv);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }

  srv->routers = (chttpsvr_router **)_ccol_mem_alloc(srv->m_procs,
                                                     sizeof(chttpsvr_router *));
  if (!srv->routers) {
    _destroy_router(root, srv->m_procs);
    _ccol_rw_lock_destroy(srv->routes_lock);
    _ccol_cond_var_destroy(srv->quiesce_done_cv);
    _ccol_cond_var_destroy(srv->requests_done_cv);
    _ccol_cond_var_destroy(srv->resolve_cv);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = _CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }
  srv->routers[0] = root;
  srv->root_router = root;
  srv->router_count = 1;
  srv->router_cap = 1;
  srv->cl = logger;
  atomic_store(&srv->listen_fd, -1);
  srv->lifecycle = CHTTPSVR_LC_IDLE;

  chttpsvr h = _chttpsvr_handle_slot_acquire(srv);
  if (h == 0) {
    if (err_str) *err_str = _CCOL_ERR_STR("failed to allocate server slot");
    _ccol_mem_free(srv->m_procs, srv->routers);
    _destroy_router(root, srv->m_procs);
    _ccol_rw_lock_destroy(srv->routes_lock);
    _ccol_cond_var_destroy(srv->quiesce_done_cv);
    _ccol_cond_var_destroy(srv->requests_done_cv);
    _ccol_cond_var_destroy(srv->resolve_cv);
    _ccol_mutex_destroy(srv->idle_mutex);
    _ccol_mutex_destroy(srv->diverted_mutex);
    _ccol_mutex_destroy(srv->wait_mutex);
    _ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    return CHTTPSVR_INVALID;
  }

  /* The root router is unlike every other router: it's never handed back to
   * a caller as a chttpsvr_router*, so chttpsvr_router_on's, _on_stream's and
   * _use's owner-resolve path never runs on it.
   *
   * root->owner is set here for a single reason: the exit-time destructor,
   * _cleanup_chttpsvr_router_shells, has to tell "root is still live" from
   * "_destroy_router already destroyed root", the same way it already can for
   * every sub-router.
   *
   * _create_router leaves the field at CHTTPSVR_INVALID, and a root left like
   * that looks exactly like an already destroyed one, so the destructor would
   * free root's shell from under a server that's still running, without
   * stopping or joining that server's reactor and worker threads first. */
  atomic_store(&root->owner, h);
  return h;
}

/* A server has up to three pools: worker_pool, reject_pool and stream_pool.
 * The first two are created together and the third on the first streaming
 * request, and all three are destroyed together (see chttpsvr_start and
 * _quiesce_server_once), so every detach point needs all three, not just
 * worker_pool. They come back in one small return type rather than through
 * separate detach functions, so no call site can detach one and forget the
 * other. */
typedef struct {
  ctpool worker;
  ctpool reject;
  ctpool stream;
} _detached_pools_t;

/* The extra grace period _wait_in_flight_bounded allows after forcing a
 * shutdown(2) on the fd of every still diverted connection. Each such
 * connection's worker thread needs that time to notice the I/O error and
 * unwind through its own error paths, the same ones a real peer disconnect
 * already exercises. That function's comment gives the details. */
#define _CHTTPSVR_FORCE_UNBLOCK_GRACE_MS 5000
/* The real, documented bound on _wait_in_flight_bounded's graceful wait for
 * in-flight requests before it even considers the forced-unblock path. It has
 * its own name so the RUNNING_UNIT_TESTS override below has one clear default
 * to fall back to. */
#define _CHTTPSVR_GRACEFUL_WAIT_MS 30000

/* Bound on _drain_and_close_all_connections's wait for all of a server's
 * connections to be freed. */
#define _CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS 30000

#ifdef RUNNING_UNIT_TESTS
/* White-box test override for _wait_in_flight_bounded's two timing bounds,
 * the graceful wait before escalating and the grace period after the forced
 * unblock. 0 in both, the default, means "use the real, documented values"
 * above.
 *
 * A test calls _chttpsvr_set_wait_in_flight_bounds_for_tests with two small
 * millisecond values to run the same escalation logic every time in well
 * under a second, instead of a real wait of tens of seconds. Like every other
 * RUNNING_UNIT_TESTS hook here the override is process-wide, not per server,
 * and the guard keeps the storage and its setter out of a production
 * build. */
static _Atomic unsigned g_wait_in_flight_graceful_ms_for_tests = 0;
static _Atomic unsigned g_wait_in_flight_grace_ms_for_tests = 0;
void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                   unsigned grace_ms) {
  atomic_store(&g_wait_in_flight_graceful_ms_for_tests, graceful_ms);
  atomic_store(&g_wait_in_flight_grace_ms_for_tests, grace_ms);
}

/* The same kind of override for how long _drain_and_close_all_connections
 * waits for current_connections to reach 0. 0, the default, means the real
 * bound, _CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS. */
static _Atomic unsigned g_drain_connections_wait_ms_for_tests = 0;
void _chttpsvr_set_drain_connections_wait_ms_for_tests(unsigned ms) {
  atomic_store(&g_drain_connections_wait_ms_for_tests, ms);
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

/* Waits for srv->in_flight_requests to reach 0, giving it up to 30s of
 * graceful waiting. If that isn't enough it forcibly interrupts every
 * connection still diverted to a worker thread, through
 * _force_unblock_diverted_connections, and then waits a further, separately
 * bounded grace period for those workers to unwind after their new I/O error
 * and each release its in_flight_requests slot.
 *
 * It exists because of what every caller does right after: each one calls
 * ctpool_shutdown_drain() on the worker pool through _destroy_detached_pools,
 * and that has NO timeout of its own; its doc comment in cthreadpool.h says it
 * blocks until every queued and active task has completed.
 *
 * Without the forced unblock below a single connection can defeat this wait,
 * because its worker thread can legitimately block forever inside
 * chttp1_stream_read or chttp1_stream_write. That's a real, reachable case: a
 * stream_read_timeout_us or response_write_timeout_us of 0, i.e. "wait
 * forever", a documented and legitimate chttpsvr_config_t setting for slow but
 * legitimate uploads, plus a peer that stalls and never closes.
 *
 * Then this function's careful 30s bound would mean nothing: the timeout runs
 * out and we return anyway, and the caller's very next call,
 * ctpool_shutdown_drain, hangs forever on the same stuck task, so there would
 * be no point in bounding this wait at all. The callers are
 * chttpsvr_destroy() and chttpsvr_engine_stop(), through
 * _drain_and_close_all_connections. A restart never gets here; it retires the
 * previous run's pools and lets their requests run to the end (see
 * _retire_pools).
 *
 * The caller must hold srv->mutex. We release it around the shutdown(2) pass
 * and take it back afterwards, so no I/O happens with the lock held, the same
 * convention this codebase follows elsewhere, e.g. chttpclient.c's idle-pool
 * liveness probe. It doesn't touch worker_pool or reject_pool, which the
 * caller detaches itself. */
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
    if (_ccol_cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
        ETIMEDOUT) {
      timed_out = true;
      break;
    }
  }
  if (!timed_out || srv->in_flight_requests <= 0) return;

  _ccol_mutex_unlock(srv->mutex);
  _force_unblock_diverted_connections(srv);
  _ccol_mutex_lock(srv->mutex);
  if (srv->in_flight_requests <= 0) return;

  struct timespec grace_deadline;
  clock_gettime(CLOCK_MONOTONIC, &grace_deadline);
  _timespec_add_ms(&grace_deadline, grace_ms);
  while (srv->in_flight_requests > 0) {
    if (_ccol_cond_var_timedwait(srv->requests_done_cv, srv->mutex,
                                 grace_deadline) == ETIMEDOUT)
      break;
  }
  /* in_flight_requests may STILL be non-zero here. Then whatever is stuck
   * isn't a blocked socket read or write, since we've already unblocked that
   * whole class, but something this library can neither see nor control, like
   * a handler blocked in unrelated work that has nothing to do with a socket.
   * ctpool_shutdown_drain() below waits for it, and there's nothing more this
   * function could safely force. */
}

static void _destroy_detached_pools(_detached_pools_t pools) {
  if (pools.stream) {
    ctpool_shutdown_drain(pools.stream);
    ctpool_destroy(pools.stream);
  }
  if (pools.worker) {
    ctpool_shutdown_drain(pools.worker);
    ctpool_destroy(pools.worker);
  }
  if (pools.reject) {
    ctpool_shutdown_drain(pools.reject);
    ctpool_destroy(pools.reject);
  }
}

/* Restarting a server takes its pools out of service without waiting for the
 * previous run's requests still running on them. A connection that stays
 * open across a stop keeps being served and a running handler runs to its
 * own end: the restart installs fresh pools right away, and the old ones
 * drain on a thread of their own, without a forced unblock. This record holds
 * one such set of old pools. Each of its tasks references srv, so every
 * teardown of srv (_quiesce_server_once) first waits for all records (see
 * _retired_pools_reap). Once detached, nothing reaches the old pools through
 * srv: every submit reads the current pool under srv->mutex, and a submit
 * that read an old pool just before the detach either lands on it before the
 * drain or is refused after it, which its caller already handles like a full
 * pool. */
typedef struct _chttpsvr_retired_pools {
  struct _chttpsvr_retired_pools *next;
  struct chttpserver *srv;
  _detached_pools_t pools;
  _ccol_thread_id_t thread;
  bool has_thread; /* a drain thread runs or ran for this record */
  bool done;       /* that thread is finished; set under srv->mutex */
} _chttpsvr_retired_pools_t;

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_retired_drained_for_tests = 0;
/* How many retired pool sets in the process have finished draining. */
size_t _chttpsvr_retired_drained_for_tests(void) {
  return atomic_load(&g_retired_drained_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_retired_drain_started_for_tests = 0;
/* How many retired pool sets in the process have started draining. */
size_t _chttpsvr_retired_drain_started_for_tests(void) {
  return atomic_load(&g_retired_drain_started_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

static void *_retired_pools_drain_fn(void *arg) {
  _chttpsvr_retired_pools_t *rec = (_chttpsvr_retired_pools_t *)arg;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_retired_drain_started_for_tests, 1);
#endif
  _destroy_detached_pools(rec->pools);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_retired_drained_for_tests, 1);
#endif
  /* Our last touch of srv. The thread that created us holds srv->mutex until
   * it has recorded this thread in rec, so a reaper that sees done can
   * always join it. */
  struct chttpserver *srv = rec->srv;
  _ccol_mutex_lock(srv->mutex);
  rec->done = true;
  _ccol_mutex_unlock(srv->mutex);
  return NULL;
}

/* Starts rec's drain thread. The caller holds srv->mutex, which the thread
 * takes as its last step, so rec->thread is recorded before the thread can
 * report itself done. */
static void _retired_pools_spawn_locked(_chttpsvr_retired_pools_t *rec) {
  _ccol_thread_id_t t;
  if (_ccol_thread_create(t, _retired_pools_drain_fn, rec) == 0) {
    rec->thread = t;
    rec->has_thread = true;
  }
}

/* Joins and frees every retired set whose drain has finished, and starts a
 * drain thread for any set without one (a failed thread creation leaves
 * those). With wait_all it waits for every set instead; the caller is then a
 * teardown that has already ended or forcibly unblocked every request, so
 * each drain ends, and a set without a thread is drained right here. */
static void _retired_pools_reap(struct chttpserver *srv, bool wait_all) {
  _ccol_mutex_lock(srv->mutex);
  _chttpsvr_retired_pools_t *keep = NULL, *finish = NULL;
  _chttpsvr_retired_pools_t *r = srv->retired_pools;
  srv->retired_pools = NULL;
  while (r) {
    _chttpsvr_retired_pools_t *next = r->next;
    if (!wait_all && !r->has_thread) _retired_pools_spawn_locked(r);
    if (wait_all || r->done) {
      r->next = finish;
      finish = r;
    } else {
      r->next = keep;
      keep = r;
    }
    r = next;
  }
  srv->retired_pools = keep;
  _ccol_mutex_unlock(srv->mutex);
  while (finish) {
    _chttpsvr_retired_pools_t *next = finish->next;
    if (finish->has_thread)
      _ccol_thread_join(finish->thread);
    else
      _destroy_detached_pools(finish->pools);
    _ccol_mem_free(srv->m_procs, finish);
    finish = next;
  }
}

/* Takes pools out of service (see _chttpsvr_retired_pools_t). A set without
 * any pool means nothing to do. If the record can't be allocated, the pools
 * drain here on the calling thread, still without a forced unblock. */
#ifdef RUNNING_UNIT_TESTS
/* The worker pool the process's latest _retire_pools call took out of
 * service. */
static _Atomic ctpool g_last_retired_worker_pool_for_tests = CTPOOL_INVALID;

/* Purely a white-box test helper. It tells whether the shutdown of the
 * worker pool the latest restart retired has started, the point after which
 * submits to that pool are refused. A pool that doesn't resolve was already
 * destroyed, so its shutdown has started too. The caller must hold a task of
 * that pool, a worker held in a hook for example, so the drain can't end and
 * the pool can't be destroyed while this helper reads it. */
bool _chttpsvr_retired_worker_shutdown_started_for_tests(void) {
  ctpool h = atomic_load(&g_last_retired_worker_pool_for_tests);
  if (h == CTPOOL_INVALID) return false;
  struct cthread_pool *pool = _ctpool_resolve_for_tests(h);
  if (!pool) return true;
  bool started = false;
  _ctpool_shutdown_state_for_tests(pool, &started, NULL, NULL);
  return started;
}
#endif /* RUNNING_UNIT_TESTS */

static void _retire_pools(struct chttpserver *srv, _detached_pools_t pools) {
  if (!pools.worker && !pools.reject && !pools.stream) return;
#ifdef RUNNING_UNIT_TESTS
  if (pools.worker)
    atomic_store(&g_last_retired_worker_pool_for_tests, pools.worker);
#endif /* RUNNING_UNIT_TESTS */
  _chttpsvr_retired_pools_t *rec =
      (_chttpsvr_retired_pools_t *)_ccol_mem_alloc(srv->m_procs, sizeof(*rec));
  if (!rec) {
    _destroy_detached_pools(pools);
    return;
  }
  rec->srv = srv;
  rec->pools = pools;
  rec->has_thread = false;
  rec->done = false;
  _ccol_mutex_lock(srv->mutex);
  _retired_pools_spawn_locked(rec);
  rec->next = srv->retired_pools;
  srv->retired_pools = rec;
  _ccol_mutex_unlock(srv->mutex);
}

/* Detaches srv's current pools and retires them. */
static void _retire_current_pools(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->mutex);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool,
                           srv->stream_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  srv->stream_pool = CTPOOL_INVALID;
  _stream_new_run_locked(srv);
  _ccol_mutex_unlock(srv->mutex);
  _retire_pools(srv, out);
  /* With no worker pool there's no run, so a streaming request still waiting
   * in the queue gets its 503 now. */
  _stream_token_start(srv);
}

/* The variant of the wait above that __chttpsvr_destroy uses. It also closes
 * every still idle connection, atomically against every place that
 * increments in_flight_requests and reads worker_pool and reject_pool
 * (_conn_start_diverted, _conn_dispatch_reject and _conn_reject_via_pool),
 * because it holds srv->mutex without a break from the moment it sees the
 * in-flight work drained all the way through the idle-close pass.
 *
 * Otherwise a keep-alive connection could be silently re-diverted. A
 * connection whose request finished and landed back in the idle list just as
 * the in-flight wait below completed could receive another pipelined request
 * before an idle-close pass with its own separate lock got to it. It would
 * escape the pass entirely and leak once this function goes on to release the
 * engine and the reactor from under it. A real, if rare, leak of one orphaned
 * chttpsvr_conn_t per full suite run, and valgrind reports it.
 *
 * A request can still arrive after a connection is closed here, or while we
 * hold srv->mutex, and then one of two things happens, neither a leak nor a
 * crash. Either it hits an already removed ccol_event_loop registration, a
 * safe no-op through the same dispatch-time liveness check this codebase
 * relies on elsewhere, or once it finally gets the lock it reads a NULL
 * worker_pool (and by the same reasoning a NULL reject_pool) and falls all
 * the way back to _conn_start_diverted's synchronous, last-resort inline
 * close. */
static _detached_pools_t _drain_and_close_all_connections(
    struct chttpserver *srv) {
  /* From here on no body-memory waiter is dispatched: a close below gives
   * memory back while this thread holds srv->mutex, and a dispatch needs that
   * lock, so every waiter gets closed instead. The streaming queue gets its
   * 503s before the wait, so the wait doesn't sit out the queue deadline of
   * every request in it. */
  _mem_admission_close(srv);
  _stream_queue_flush(srv);
  /* The server is draining (see the comment of the draining field), so a
   * keep-alive connection waiting for its next request has nothing more to
   * get and closes now instead of after the wait below. A parked connection
   * carries on until then, since it holds the body or the response of a
   * request already being served. */
  _close_idle_connections(srv, /*include_parked=*/false);
  _ccol_mutex_lock(srv->mutex);
  _wait_in_flight_bounded(srv);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool,
                           srv->stream_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  srv->stream_pool = CTPOOL_INVALID;
  _stream_new_run_locked(srv);
  _close_all_idle_connections(srv);
  _mem_wait_flush(srv);
  _ccol_mutex_unlock(srv->mutex);

  /* One _close_all_idle_connections pass above isn't enough, because two
   * kinds of connection are invisible to it. One is a connection accepted
   * moments ago that's still in the TLS handshake or the header read on the
   * reactor thread and hasn't been put in the idle list yet. The other is a
   * connection a dispatch has already claimed out of the idle list for
   * ordinary work, e.g. one that finds the peer closed and handles it
   * directly through _conn_pump into _conn_close without ever diverting to a
   * worker, so in_flight_requests never saw it.
   *
   * That's a real use-after-free, and ThreadSanitizer reports it over
   * tests_tls: the reactor thread may still be inside _conn_pump or _conn_free
   * for such a connection after this function returns and __chttpsvr_destroy
   * goes on to free srv.
   *
   * current_connections covers a connection's whole life whatever path
   * closes it, incremented at the accept and decremented in _conn_free.
   * in_flight_requests only covers requests diverted to a worker, and the idle
   * list only holds connections no dispatch currently claims. So waiting for
   * current_connections to reach zero closes the gap completely instead of
   * trusting one snapshot, and the loop repeats the idle-close pass as still
   * active connections finish and land back in the idle list.
   *
   * A poll is usually the wrong tool for this kind of wait, but here it's
   * the right one. It polls on purpose instead of using a condition variable
   * of its own, because a broadcast from every _conn_free would add a
   * permanent cost to the hot per-connection close path, which runs for every
   * connection the server ever serves and not only at shutdown, while the
   * window it closes only matters on this cold path that runs once in a
   * server's life. */
  unsigned drain_wait_ms = _CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS;
#ifdef RUNNING_UNIT_TESTS
  unsigned override_drain_wait_ms =
      atomic_load(&g_drain_connections_wait_ms_for_tests);
  if (override_drain_wait_ms) drain_wait_ms = override_drain_wait_ms;
#endif /* RUNNING_UNIT_TESTS */
  struct timespec poll_deadline;
  clock_gettime(CLOCK_MONOTONIC, &poll_deadline);
  _timespec_add_ms(&poll_deadline, drain_wait_ms);
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
    _mem_wait_flush(srv);
    _stream_queue_flush(srv);
  }
  return out;
}

/* The complete teardown __chttpsvr_destroy needs: stop listening and
 * unregister srv from servers_bundler.servers, drain in-flight requests and
 * close idle connections, shut down and destroy the worker pool, and last
 * drop this server's engine reference.
 *
 * It's a function of its own, guarded by srv->quiesce_state, so the engine can
 * also drive it, exactly once, from its _engine_force_stop_quiesce_all pass,
 * which chttpsvr_engine_stop() may run while srv is still fully started. That
 * pass may run at the same time as an independent chttpsvr_destroy(srv) from
 * the application, not just before it, because chttpsvr_engine_stop() is
 * documented as safe to call from a signal handler and nothing requires a
 * caller to serialize it against chttpsvr_destroy() with
 * chttpsvr_engine_wait() first.
 *
 * Whichever of the two gets to a given srv first does the real work, and the
 * OTHER one BLOCKS until that work has really finished, i.e. until
 * quiesce_state reaches CHTTPSVR_QS_QUIESCED, instead of returning at once
 * as a no-op. This matters because __chttpsvr_destroy goes straight from this
 * function's return into _ccol_mutex_destroy(raw->mutex) and then frees raw.
 * If a losing caller returned as soon as it saw quiesce_state !=
 * CHTTPSVR_QS_NOT_QUIESCED, __chttpsvr_destroy could destroy raw->mutex and
 * raw->idle_mutex and free raw while the winner's call is still using them
 * inside _drain_and_close_all_connections and _engine_release below. That's a
 * genuine use-after-free plus destroying a mutex still in use, not just a
 * theory. _engine_force_stop_quiesce_all's retry loop already handles the
 * reverse race, a concurrent chttpsvr_destroy().
 *
 * The order is stop, drain, release the engine reference, destroy the pools,
 * unregister, and it must run to completion before anything tears down the
 * shared reactor (the doc comment of _engine_force_stop_quiesce_all says
 * why). The unregister comes last, so a reap started by a concurrent
 * chttpsvr_engine_stop() finds this server registered and waits for this
 * quiesce as a loser. */
static void _quiesce_server_once(struct chttpserver *srv) {
  _ccol_mutex_lock(srv->mutex);
  /* An exhaustive switch without a `default:` label, for the same `-Wswitch`
   * reason the comment of chttpsvr_quiesce_state_t gives. A loser already has
   * quiesce_state CHTTPSVR_QS_QUIESCING or CHTTPSVR_QS_QUIESCED, and both
   * share one branch whose wait loop is a correct zero-iteration no-op when
   * quiesce_state is already CHTTPSVR_QS_QUIESCED. So sending both down the
   * same path is exactly the same as testing for CHTTPSVR_QS_QUIESCING alone,
   * not a new case with different handling. */
  switch (srv->quiesce_state) {
    case CHTTPSVR_QS_QUIESCING:
    case CHTTPSVR_QS_QUIESCED: {
      /* The comment of quiesce_waiters explains this increment/decrement
       * pair. Both happen with srv->mutex held, and the pair lets the
       * winner's tail below learn, through a real happens-before relation a
       * broadcast alone doesn't give, that this loser is completely done with
       * srv->mutex and srv->quiesce_done_cv; only then may the winner's
       * caller destroy either of them. */
      srv->quiesce_waiters++;
      while (srv->quiesce_state != CHTTPSVR_QS_QUIESCED)
        _ccol_cond_var_wait(srv->quiesce_done_cv, srv->mutex);
      srv->quiesce_waiters--;
      _ccol_cond_var_broadcast(srv->quiesce_done_cv);
      _ccol_mutex_unlock(srv->mutex);
      return;
    }
    case CHTTPSVR_QS_NOT_QUIESCED:
      srv->quiesce_state = CHTTPSVR_QS_QUIESCING;
      _srv_begin_draining(srv);
#ifdef RUNNING_UNIT_TESTS
      _quiesce_claimed_signal_fire_if_armed();
#endif /* RUNNING_UNIT_TESTS */
      break;
  }
  /* Wait for every in-flight call on srv that _chttpsvr_resolve protects
   * (chttpsvr_start, _stop, _register_handler, _use, _subrouter and the
   * others), which has to finish before we touch any of srv's mutable state
   * below.
   *
   * __chttpsvr_destroy already does exactly this wait itself before calling
   * us, but _engine_force_stop_quiesce_all, chttpsvr_engine_stop()'s forced
   * path, doesn't: it calls this function directly on every registered server
   * without waiting. Unlike destroy, that path can legitimately run at the
   * same time as a chttpsvr_start() that has already taken an engine
   * reference, so raw->contributed_to_engine is true, and that still uses
   * srv_engine_bundler.reactor further down, to ccol_event_loop_add the
   * listener for example.
   *
   * Without this wait we could decide below to drop this server's engine
   * reference and unregister and drain srv while chttpsvr_start, which still
   * thinks it holds that very reference, goes on to register a listener
   * against a reactor that may be torn down, or already is, by the time it
   * gets there. That's a ccol_event_loop_add on an already destroyed
   * ccol_event_loop handle.
   *
   * The wait uses the same srv->mutex and resolve_cv pair as
   * _chttpsvr_resolve_unpin and closes the window the same way for every
   * caller of this function, so no caller needs to remember it.
   *
   * ccol_event_reg is itself a generation-checked handle resolved through its
   * ccol_event_loop's slot table just like the ccol_event_loop handle, so a
   * stale conn->reg reaching a later ccol_event_loop_remove(), _modify(),
   * _pause() or _resume() is always safely rejected without any risk of a
   * use-after-free. That's not what this particular wait is about. */
  while (atomic_load(&srv->pending_resolve_count) > 0)
    _ccol_cond_var_wait(srv->resolve_cv, srv->mutex);
  _ccol_mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _quiesce_teardown_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  _chttpsvr_stop_internal(srv);

  /* Drain the in-flight work and close every remaining idle connection. This
   * must happen BEFORE this server's engine reference is dropped below,
   * because that _engine_release_begin() may be the one that takes the shared
   * reactor's refcount to zero, when srv is the last contributing server, and
   * the reaper we then start calls
   * ccol_event_loop_destroy(srv_engine_bundler.reactor). Our own
   * ccol_event_loop_remove() calls, made inside
   * _drain_and_close_all_connections through _conn_close, would race that
   * reaper if they ran after the release instead of before. */
  _detached_pools_t old_pools = _drain_and_close_all_connections(srv);
  /* Pools retired by restarts run srv's requests too. The drain above has
   * already waited for, or forcibly unblocked, every request of every run, so
   * each of their drains ends. */
  _retired_pools_reap(srv, true);

  bool should_release_engine = false;
  uintptr_t engine_gen = 0;
  _ccol_mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
    engine_gen = srv->engine_gen;
    srv->engine_gen = 0;
  }
  _ccol_mutex_unlock(srv->mutex);
  /* The last reference's reaper only starts after the unregister below. If
   * its thread can't be created it runs inline on this thread, and otherwise
   * its force-stop pass would find srv registered and wait, as a loser, for
   * the quiesce this very thread is running. */
  bool should_reap = should_release_engine && _engine_release_begin(engine_gen);

  _destroy_detached_pools(old_pools);
  /* Only now, after its drain and after dropping its engine reference, does
   * the server leave the registry. While it's still registered, an engine
   * reap started by a concurrent chttpsvr_engine_stop() finds it in
   * _engine_force_stop_quiesce_all and waits, as a loser of the claim above,
   * until this quiesce ends. Otherwise the reap would find an empty registry,
   * destroy the reactor under the connections this drain is still closing,
   * and let chttpsvr_engine_wait() return while one of this server's handlers
   * is still running. */
  _servers_unregister(srv);
  if (should_reap) _spawn_reaper();

  _ccol_mutex_lock(srv->mutex);
  srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
  _ccol_cond_var_broadcast(srv->quiesce_done_cv);
  /* Wait for every loser this broadcast just woke to get out of its
   * _ccol_cond_var_wait before returning to our caller, since a loser's
   * quiesce_waiters decrement is its last touch of srv->mutex and
   * srv->quiesce_done_cv. The comment of quiesce_waiters describes the real,
   * POSIX-undefined race this closes, which happens when the caller is
   * __chttpsvr_destroy and destroys both right afterwards.
   *
   * A broadcast alone only makes a woken thread runnable, it doesn't mean the
   * thread has got the mutex back. Reacquiring and checking in this loop is
   * what turns "runnable" into "provably finished", through the same
   * mutual-exclusion happens-before relation the pending_resolve_count wait
   * uses elsewhere in this file.
   *
   * Usually there are no losers and the wait does nothing. Only the OS
   * scheduler bounds it, since a loser the broadcast above just woke is free
   * to take the lock, decrement and broadcast again at once. */
  while (srv->quiesce_waiters > 0)
    _ccol_cond_var_wait(srv->quiesce_done_cv, srv->mutex);
  _ccol_mutex_unlock(srv->mutex);
}

void __chttpsvr_destroy(chttpsvr srv) {
  if (!srv) return;

  /* Resolve srv through the slot table and mark the slot not in use in the
   * same critical section as the lookup. That's what makes a second destroy
   * of the same handle value, concurrent or later, see a resolve failure
   * instead of racing this call's teardown (the slot table's file-level
   * comment and the comment of _chttpsvr_resolve have the whole design). A
   * stale or already destroyed handle getting here is exactly the misuse the
   * generation-checked handle design exists to catch, and it's fatal rather
   * than a silent use-after-free or double free. */
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(srv >> 32);
  uint32_t gen = (uint32_t)(srv & 0xFFFFFFFFu);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot_t *slot = NULL;
  struct chttpserver *raw = NULL;
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *s =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
    _ccol_fatal_err(
        "chttpsvr_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpsvr handle)");
  }
  /* The self-call case (see the comment of chttpsvr_worker_key_bundle
   * above). Unlike a stale handle, this is a live, valid server whose own
   * worker pool is destroying it right now, and there's no safe way forward:
   * draining in-flight requests can never converge, because this very call
   * stack is what would finish and decrement the count, and the
   * ctpool_destroy() that follows, on this thread's own pool, is an unrelated
   * fatal misuse of cthreadpool.c.
   *
   * __chttpsvr_destroy has no ccol_retval_t to report it through, since its
   * documented contract is a live handle in or a _ccol_fatal_err on misuse.
   * So a detected self-call is handled exactly like the stale-handle case
   * right above, with a loud, immediate _ccol_fatal_err() naming the real
   * problem, instead of a silent hang followed by an abort whose stack trace
   * points somewhere else entirely. Same as the self-destroy guards of
   * __ctpool_destroy and __ccol_event_loop_destroy. */
  if (_chttpsvr_is_self_call(raw)) {
    _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
    _ccol_fatal_err(
        "chttpsvr_destroy: called from within a request handler (or "
        "middleware) running on this very server's own worker pool; "
        "destroying it here would deadlock waiting for this exact "
        "in-flight request to finish, then abort when the pool's own "
        "self-destroy guard fires. Destroy the server from a different "
        "thread instead, or call chttpsvr_engine_stop() (documented "
        "safe to call from within a handler) if a full shutdown is the "
        "actual goal.");
  }
  slot->in_use = false; /* from now on ALL resolves of this handle fail,
                            a second concurrent destroy attempt
                            included */
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);

  _ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->pending_resolve_count) > 0)
    _ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  _ccol_mutex_unlock(raw->mutex);

  _quiesce_server_once(raw);

  /* Wait for a concurrent _engine_force_stop_quiesce_all pass that took a bare
   * pointer to raw straight out of servers_bundler.servers[] (see the doc
   * comment of that function) and has to be done with raw before we free a
   * single byte of it below.
   *
   * The wait comes after _quiesce_server_once returns on purpose, rather than
   * being folded into the pending_resolve_count wait above. Neither
   * _quiesce_server_once nor _chttpsvr_stop_internal ever touches
   * servers_bundler_pins (the comment of that field describes the
   * self-deadlock that waiting on it from inside that call would cause), so
   * no ordering rule makes it happen any earlier than "some time before raw
   * is freed".
   *
   * The listener_dispatch_pins check next to it waits out at most one
   * listener dispatch that started before the chttpsvr_stop inside
   * _quiesce_server_once above and only pinned after it. Such a dispatch finds
   * listen_fd cleared and returns immediately without touching anything freed
   * below. raw's memory doesn't depend on this wait, since the listener
   * registration holds a raw->lifetime_refs reference until its on_removed
   * fires. */
  _ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->servers_bundler_pins) > 0 ||
         atomic_load(&raw->listener_dispatch_pins) > 0)
    _ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  _ccol_mutex_unlock(raw->mutex);

  if (raw->tls_ctx) {
    ctls_ctx_release(raw->tls_ctx);
    raw->tls_ctx = NULL;
  }

  /* raw's real teardown, its mutexes, its own memory and its slot, only runs
   * once every connection this server ever accepted has had every one of its
   * registrations proven safe to release. The comment of raw->lifetime_refs
   * gives the full reason, and _chttpsvr_finish_destroy is the deferred
   * teardown itself.
   *
   * This decrement is the application's "done with raw" vote, and there's
   * exactly one per raw because __chttpsvr_destroy is only reached once per
   * handle: a second call, concurrent or later, has already hit a
   * _ccol_fatal_err above through the slot's in_use guard. While some
   * connection's registration is still waiting to be reclaimed this isn't
   * the last contribution, and _conn_on_removed does the final teardown when
   * it is. */
  if (atomic_fetch_sub(&raw->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(raw);
}

/* raw's real final teardown: destroy its mutexes, condition variables and
 * rwlock, close its logger, free its memory and release its slot.
 *
 * It's deferred out of __chttpsvr_destroy (see the comment of
 * raw->lifetime_refs) so it can run from either of two places, whichever
 * turns out to make the last contribution to that count: the tail of
 * __chttpsvr_destroy above, or _conn_on_removed, possibly long after
 * __chttpsvr_destroy's caller has returned.
 *
 * It touches raw's mutexes, raw->m_procs and raw->self_slot_idx, all of which
 * are still valid here whichever caller runs it, since by construction
 * nothing has freed raw yet; that's what this very function is about to do.
 * self_slot_idx is written once, when the handle is acquired, and never
 * again. */
#ifdef RUNNING_UNIT_TESTS
/* Purely white-box test instrumentation. A test names one server by its
 * handle and _chttpsvr_finish_destroy records when it runs for exactly that
 * server, so the test can tell whether that server's final free has happened,
 * whatever the process's other servers are doing at the time. */
static _Atomic(struct chttpserver *) g_finish_destroy_watch_for_tests = NULL;
/* Counts teardowns of a server's route data that found one of its
 * connections still allocated. Such a connection could still read its matched
 * route, so the count must stay 0. */
static _Atomic size_t g_route_teardown_with_live_connections_for_tests = 0;
size_t _chttpsvr_route_teardown_with_live_connections_for_tests(void) {
  return atomic_load(&g_route_teardown_with_live_connections_for_tests);
}
static _Atomic bool g_finish_destroy_watch_hit_for_tests = false;
void _chttpsvr_watch_finish_destroy_for_tests(chttpsvr h) {
  atomic_store(&g_finish_destroy_watch_hit_for_tests, false);
  atomic_store(&g_finish_destroy_watch_for_tests,
               _chttpsvr_resolve_for_tests(h));
}
bool _chttpsvr_watched_finish_destroy_ran_for_tests(void) {
  return atomic_load(&g_finish_destroy_watch_hit_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

static void _chttpsvr_release_slot_table_if_deferred_locked(void);

static void _chttpsvr_finish_destroy(struct chttpserver *raw) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_finish_destroy_watch_for_tests) == raw) {
    atomic_store(&g_finish_destroy_watch_for_tests, NULL);
    atomic_store(&g_finish_destroy_watch_hit_for_tests, true);
  }
#endif /* RUNNING_UNIT_TESTS */
  /* The routers with their routes and middleware go here and not in
   * __chttpsvr_destroy. A connection a slow handler still owns when the
   * destroy's bounded drain gives up keeps reading its matched route (the
   * handler, the middleware chain, the parameter names and count) until
   * _conn_free runs from _conn_on_removed. Every such connection holds a
   * raw->lifetime_refs reference, so this is the first point where no
   * connection can reach the route data any more.
   *
   * No chttpsvr_router_on, _on_stream or _use call can reach the routers
   * either. Each resolves the router's owner handle through the slot table,
   * and __chttpsvr_destroy cleared that slot's in_use flag and drained every
   * pinned resolve before giving up its reference. _destroy_router never
   * frees a router's shell, only its contents, after atomically invalidating
   * owner; see the comment of chttpsvr_router_shell_registry. */
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&raw->current_connections) > 0)
    atomic_fetch_add(&g_route_teardown_with_live_connections_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  for (size_t i = 0; i < raw->router_count; i++) {
    _destroy_router(raw->routers[i], raw->m_procs);
  }
  _chttpsvr_router_shells_release(raw->routers, raw->router_count,
                                  raw->root_router);
  raw->root_router = NULL;
  _ccol_mem_free(raw->m_procs, raw->routers);

  _ccol_mutex_destroy(raw->mutex);
  _ccol_mutex_destroy(raw->idle_mutex);
  _ccol_mutex_destroy(raw->diverted_mutex);
  _ccol_mutex_destroy(raw->wait_mutex);
  _ccol_cond_var_destroy(raw->requests_done_cv);
  _ccol_cond_var_destroy(raw->resolve_cv);
  _ccol_cond_var_destroy(raw->quiesce_done_cv);
  _ccol_rw_lock_destroy(raw->routes_lock);

  clog_close(raw->cl);
  raw->cl = CLOG_INVALID;

  uint32_t idx = raw->self_slot_idx;
  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _ccol_mem_free(mp, raw);
  _ccol_mem_free(mp, mp);

  /* Release the slot last, only once raw's teardown and free are complete,
   * because bumping the slot's generation and pushing back the free index is
   * what marks the handle as reusable, and no earlier step does that.
   *
   * The slot is fetched again by idx instead of keeping a slot pointer across
   * the gap since raw was last touched, because a concurrent
   * chttpsvr_create_mp may run its own _chttpsvr_handle_slot_acquire in
   * between and reallocate the slots' backing array through
   * cvector_push_back, which invalidates any pointer into it taken earlier.
   * idx itself is stable. */
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot_t *slot2 =
      (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* moves the slot's generation past the value the
      just-freed srv's handle carried, so that stale handle can never match
      a FUTURE acquire's generation for this index */
  cvector_push_back(chttpsvr_slot_table.free_indices, &idx);
  /* The process-exit destructor has already run and found this server
   * alive. The release it couldn't do belongs to whoever frees the last
   * slot, which may be this call. */
  _chttpsvr_release_slot_table_if_deferred_locked();
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

/* The destructor below frees the slot table's bookkeeping arrays at process
 * exit so the --show-leak-kinds=all of make memtest doesn't report them as
 * still reachable, but ONLY when no chttpsvr handle is in use.
 *
 * This module has no library-owned singleton server that gets destroyed
 * first, nothing like chttpclient.c's chttp_default_client, which
 * _cleanup_default_client destroys there. All that tells us is that this
 * destructor has no automatic handle to destroy on the application's behalf;
 * it doesn't make freeing the slot table safe while a handle the
 * application made is alive.
 *
 * We don't control the order of __attribute__((destructor)) functions and
 * atexit handlers across a process's shared objects. An application may let
 * process exit reclaim a chttpsvr it made itself, without calling
 * chttpsvr_destroy, and then a destructor or atexit handler of its own can
 * touch that live handle after this destructor has run, through
 * chttpsvr_start, _stop, _register_handler, _use, _subrouter or _destroy,
 * all of which resolve through this slot table.
 *
 * Freeing the shared slot table under a live handle is a use-after-free, so
 * the release is deferred instead and the destroy of the last server, from a
 * later destructor or atexit handler, releases the table. Only a server the
 * application never destroys keeps it, which is the leak we accept ("the
 * caller never destroyed their server"). chttpclient.c's
 * _cleanup_default_client guard deals with the same class of hazard.
 *
 * An __attribute__((destructor)) runs for the whole shared object, whatever
 * parts of it the process used, and in a process that links us but never
 * makes a chttpsvr the mutex here isn't initialized. So the destructor
 * returns at once when no thread entered the initialization, and calls
 * _ccol_call_once before locking otherwise. */
/* Tells whether this slot still belongs to a server, in which case the
 * table must not be released out from under it.
 *
 * We read slot->ptr and not slot->in_use, because in_use is cleared as the
 * FIRST step of __chttpsvr_destroy, so a second destroy or a new resolve is
 * refused as early as possible, while the rest of that teardown runs after
 * it: quiescing the server, draining the pins, destroying the routers, and
 * finally the locked release of this very slot in _chttpsvr_finish_destroy.
 * A scan trusting in_use alone would free the table under a destroy still
 * inside that window, whose last step then indexes the table and writes to
 * it.
 *
 * ptr is written only once a slot is fully acquired and cleared only in that
 * final locked step, so it's set for exactly as long as the table must not be
 * released. Same as _chttpcli_any_slot_live_locked in chttpclient.c. */
static bool _chttpsvr_slot_live(const chttpsvr_slot_t *slot) {
  return slot->ptr != NULL;
}

#ifdef RUNNING_UNIT_TESTS
/* White-box test hooks only, gated out of a production build like every
 * other white-box helper in this file. Together they let a test build the
 * mid-teardown window above, with in_use already cleared and ptr not, and ask
 * the exit-time destructor's predicate what it makes of that state, which is
 * otherwise only reachable by winning a race against a real teardown. */
bool _chttpsvr_slot_live_for_tests(chttpsvr h) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  bool live = false;
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots))
    live = _chttpsvr_slot_live(
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx));
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return live;
}

void _chttpsvr_slot_set_in_use_for_tests(chttpsvr h, bool in_use) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots))
    ((chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx))->in_use =
        in_use;
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* Tells whether any slot of the table names a server. The caller holds the
 * table's mutex and the table exists. */
static bool _chttpsvr_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++)
    if (_chttpsvr_slot_live(
            (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i)))
      return true;
  return false;
}

/* Frees the slot table's arrays and retires the worker key. The caller holds
 * the table's mutex, the table exists and no slot is live. */
static void _chttpsvr_release_slot_table_locked(void) {
  /* The pointers go NULL along with the free, and that's what every resolve,
   * every acquire and the fork handlers test, so a stale handle that a later
   * destructor or atexit handler passes in resolves to nothing and a create
   * fails cleanly, rather than reading the freed arrays. */
  __cvector_destroy(chttpsvr_slot_table.slots);
  __cvector_destroy(chttpsvr_slot_table.free_indices);
  chttpsvr_slot_table.slots = NULL;
  chttpsvr_slot_table.free_indices = NULL;
  chttpsvr_slot_table.release_deferred = false;
  /* With no server left there's no worker thread of any server left either,
   * since a server's final teardown releases its slot only after its pools
   * have joined their threads. So the worker key has no user and a dlclose()
   * of the library doesn't leak it. A later call finds `ready` false and
   * treats the key as absent. */
  _ccol_call_once(chttpsvr_worker_key_bundle.once,
                  _chttpsvr_worker_key_init_globals);
  _ccol_mutex_lock(chttpsvr_worker_key_bundle.mutex);
  if (atomic_exchange(&chttpsvr_worker_key_bundle.ready, false))
    _ccol_thread_ls_key_delete(chttpsvr_worker_key_bundle.key);
  _ccol_mutex_unlock(chttpsvr_worker_key_bundle.mutex);
}

/* The release the exit-time destructor deferred, done once the last server
 * is gone. Kept out of line so the destroy path keeps its shape. */
static __attribute__((noinline)) void
_chttpsvr_release_slot_table_if_deferred_locked(void) {
  if (chttpsvr_slot_table.release_deferred && chttpsvr_slot_table.slots &&
      !_chttpsvr_any_slot_live_locked())
    _chttpsvr_release_slot_table_locked();
}

__attribute__((destructor)) static void _cleanup_chttpsvr_slot_table(void) {
  /* A process that never touched this module has nothing to release here,
   * and if we ran the initialization from here it would build the slot
   * tables of clogger and cthreadpool after their own destructors may have
   * run, and nothing would ever free them. A thread that entered the
   * initialization set the flag first, so the _ccol_call_once below waits
   * for that initialization to finish. */
  if (!atomic_load(&chttpsvr_slot_table_entered)) return;
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots) {
    if (_chttpsvr_any_slot_live_locked())
      chttpsvr_slot_table.release_deferred = true;
    else
      _chttpsvr_release_slot_table_locked();
  }
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

ccol_retval_t chttpsvr_start(chttpsvr h, const chttpsvr_config_t *cfg) {
  /* Port 0 is fine: the system binds an ephemeral port, which
   * chttpsvr_bound_port() reports.
   *
   * A server certificate and its private key come as a pair, so having only
   * one of them is never a valid configuration. The TLS setup further down
   * only tries TLS when BOTH cert_path and key_path are non-NULL (that's what
   * ctls_ctx_cert_add needs), so quietly reading a lone cert_path or key_path
   * as "no TLS configured" would start this server as plain, unencrypted HTTP
   * while the caller thinks the port is HTTPS, and nothing would report an
   * error anywhere. Same guard as in chttpclient_set_tls and the comment
   * above it.
   *
   * ca_bundle_path alone, without a cert_path and key_path pair, has the same
   * hazard: the TLS setup below only looks at ca_bundle_path inside the
   * `cert_path && key_path` branch, so a caller setting only ca_bundle_path
   * would get the same silent downgrade to plain HTTP instead of an error.
   * On the client side that's the everyday way to verify against a custom CA
   * with chttpclient_set_tls, as curl's --cacert, Go's
   * TLSClientConfig.RootCAs and Python's
   * ssl.create_default_context(cafile=...) all do, with no client certificate
   * involved. But no reference server (Go's ListenAndServeTLS, Node's
   * https.createServer, nginx's ssl_certificate and ssl_certificate_key)
   * offers a TLS server with a trust store and no identity certificate of
   * its own, so we refuse that shape the same way.
   *
   * The same silent downgrade happens when cert_path and key_path are BOTH
   * missing, ca_bundle_path included, e.g. a cfg->tls pointing at
   * CHTTP_TLS_DEFAULT (chttp.h), which names no path at all. That macro is
   * documented as the shared defaults, verification on, for both chttpclient
   * and chttpsvr, so a caller can reasonably reach for it here and forget to
   * set cert_path and key_path afterwards, which is exactly the hazard this
   * check is here for. A start that fails loudly is far better than a server
   * that quietly speaks plain HTTP on a port its operator believes is HTTPS.
   *
   * We demand both non-NULL whenever cfg->tls is non-NULL, so one condition
   * covers all three refused shapes above (a lone cert_path, a lone key_path,
   * a ca_bundle_path with neither) instead of listing each combination. */
  if (cfg && cfg->tls && !(cfg->tls->cert_path && cfg->tls->key_path))
    return ccol_invalid_args;
  /* CHTTPSVR_STREAMING_POOL_OFF is the only negative value
   * streaming_thread_count knows. Any other negative value is a mistake, not
   * a quiet synonym for it. */
  if (cfg && cfg->streaming_thread_count < CHTTPSVR_STREAMING_POOL_OFF)
    return ccol_invalid_args;

  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;

  /* If the workers can't mark themselves, the server can't catch a destroy,
   * a restart or an engine wait from its own handlers (see the comment of
   * chttpsvr_worker_key_bundle), so it doesn't start at all. */
  if (!_chttpsvr_worker_key_ensure()) {
    _SRV_ENGINE_LOG(clog_error,
                    "chttpsvr_start: no thread-specific key is available "
                    "(the process holds PTHREAD_KEYS_MAX keys)");
    _chttpsvr_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }

#ifdef RUNNING_UNIT_TESTS
  _start_resolve_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* The self-call case (see the comment of chttpsvr_worker_key_bundle
   * above): a request handler or middleware running on raw's worker_pool or
   * reject_pool calls chttpsvr_start() to restart the very server it runs
   * on. The restart would retire the pool running this call, and a failed
   * restart would leave the calling handler on a server stopped under it, so
   * the documented contract is that a handler never restarts its own server;
   * a thread outside the handlers does that.
   *
   * Unlike __chttpsvr_destroy we do have a ccol_retval_t to report through,
   * and simply refusing the restart leaves the server perfectly safe: it
   * keeps running its old configuration or, if the handler called
   * chttpsvr_stop() first, it's just stopped. Nothing gets corrupted and a
   * later, legitimate call from another thread can still restart it, so we
   * report it gracefully instead of aborting the process.
   *
   * The check can't hurt on a server's very first chttpsvr_start(), since no
   * worker thread of this server can exist yet to be marked. */
  if (_chttpsvr_is_self_call(raw)) {
    _chttpsvr_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  chttpsvr_config_t default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  _ccol_mutex_lock(raw->mutex);
  for (;;) {
    /* Exhaustive switch (no `default:` label), for the same `-Wswitch`
     * reason chttpsvr_lifecycle_t's field comment gives.
     * CHTTPSVR_LC_STOPPING backs off and retries below; CHTTPSVR_LC_IDLE is
     * the only value that falls through this switch to the second one
     * below it. */
    switch (raw->lifecycle) {
      case CHTTPSVR_LC_STARTING:
      case CHTTPSVR_LC_RUNNING:
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 1 */
        return ccol_not_permitted;
      case CHTTPSVR_LC_STOPPING: {
        /* A concurrent _chttpsvr_stop_internal() on this same handle, from
         * another thread, may still be in flight. A single caller doing
         * stop() then start() never sees this, because chttpsvr_stop()
         * doesn't return until its own teardown, leaving this state
         * included, is done. A plain chttpsvr_stop() can be driving that
         * stop, and so can the first call of a _quiesce_server_once pass.
         *
         * The quiesce_state pass checked below is different: its own
         * SEPARATE, later teardown steps wait for pending_resolve_count to
         * reach 0 as a precondition, while _chttpsvr_stop_internal() itself
         * never waits on pending_resolve_count anywhere, only on
         * listener_dispatch_pins. The comment of chttpsvr_lifecycle_t has the
         * spurious EADDRINUSE-class race this state closes.
         *
         * So this call keeps its resolve pin on purpose, unlike the
         * quiesce_state wait below, which must drop the pin first or it
         * deadlocks for real across two condition variables against
         * _quiesce_server_once's pending_resolve_count wait. Holding the pin
         * here can't hold up _chttpsvr_stop_internal(), since nothing it does
         * waits on this counter, and the pin also keeps raw alive across the
         * wait so nothing needs to resolve it again afterwards.
         *
         * We use a real condition variable wait here, woken by
         * _chttpsvr_stop_internal()'s resolve_cv broadcast right after it sets
         * CHTTPSVR_LC_IDLE, rather than a blind nanosleep-and-repoll at a fixed
         * interval. This function does the structurally identical
         * quiesce_state and "currently stopping" backoffs too (see each
         * comment). A blind poll like that measurably busy-loops thousands of
         * times a second under load, and this branch, with the same shape of
         * drop the pin, sleep, resolve again, retry, carries the same risk.
         *
         * This wait can be reached while a _quiesce_server_once pass is the
         * one driving _chttpsvr_stop_internal. The wake then sees
         * CHTTPSVR_LC_IDLE and falls through to the quiesce_state switch below
         * on the next loop iteration, which drops this pin before waiting any
         * further, so we never hold the pin longer than that one extra,
         * bounded trip round the loop. */
#ifdef RUNNING_UNIT_TESTS
        _start_stopping_wait_signal_fire_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        while (raw->lifecycle == CHTTPSVR_LC_STOPPING)
          _ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
        continue;
      }
      case CHTTPSVR_LC_IDLE:
        break;
    }
    /* A _quiesce_server_once pass for this very server may already be
     * running, driven either by a concurrent force-stop from
     * chttpsvr_engine_stop() through _engine_force_stop_quiesce_all, which
     * reads raw straight out of servers_bundler.servers[] and calls
     * _quiesce_server_once directly, never going through _chttpsvr_resolve or
     * pending_resolve_count (see that function's doc comment), or by another
     * thread's chttpsvr_destroy() racing this one.
     *
     * _chttpsvr_stop_internal, which leaves CHTTPSVR_LC_RUNNING, is only the
     * very first step of that pass and runs long before its
     * _drain_and_close_all_connections and _engine_release work is done, which
     * can be slow; only then does quiesce_state reach CHTTPSVR_QS_QUIESCED.
     * The pass never pins raw, so the pending_resolve_count wait that
     * _quiesce_server_once does on ITS OWN entry can't see this call's pin
     * either way, and a raw->lifecycle of CHTTPSVR_LC_IDLE above proves
     * nothing about whether a teardown is still in flight.
     *
     * We wait here for any such pass in progress to fully finish rather than
     * blindly resetting quiesce_state below, and that wait is what closes the
     * gap. Without it this call can stomp that state out from under the
     * in-progress pass's single-winner serialization (see the doc comment of
     * _quiesce_server_once), and one of two things happens. Either a second,
     * concurrent quiesce of the same raw runs next to the first, which is a
     * use-after-free on a destroyed mutex, since a racing chttpsvr_destroy()
     * goes straight from that second pass's return into freeing raw, or a
     * later, unrelated rescan by _engine_force_stop_quiesce_all quietly
     * quiesces, and so kills, the very server this call is busy restarting.
     *
     * We must NOT wait on raw->quiesce_done_cv while still holding this call's
     * resolve pin, because that deadlocks: _quiesce_server_once's
     * pending_resolve_count wait must reach 0 before the pass does ANY of
     * its real work, including what would eventually reach
     * CHTTPSVR_QS_QUIESCED and broadcast this condition variable, and that
     * wait counts the pin we hold. A pinned wait here would leave
     * _quiesce_server_once waiting forever for a pin only WE can release, and
     * us waiting forever for a signal only IT can send. That's a genuine
     * circular deadlock across two condition variables and not a rare corner
     * case: a perfectly ordinary chttpsvr_stop() then chttpsvr_start()
     * restart racing a concurrent chttpsvr_engine_stop() gets there.
     *
     * So we drop the pin completely before backing off, with the same
     * _chttpsvr_resolve_unpin every other exit path uses, and resolve the
     * original handle again from scratch once the pass in progress has
     * plausibly finished.
     *
     * Resolving again, instead of touching the same raw pointer directly, is
     * what makes this safe, since raw can be freed concurrently while we hold
     * no pin at all, e.g. by a racing chttpsvr_destroy(), unblocked by then,
     * that finishes its teardown and free while we're backed off. The
     * generation-checked slot table then either hands back the same,
     * still-live raw and the loop runs again, or reports the handle as gone
     * with ccol_invalid_args, and never hands back a stale pointer.
     *
     * The switch below is exhaustive with no `default:` label, for the same
     * `-Wswitch` reason as above. */
    switch (raw->quiesce_state) {
      case CHTTPSVR_QS_QUIESCING:
        /* Join the same winner and loser protocol that the
         * CHTTPSVR_QS_QUIESCING and _QUIESCED branch of _quiesce_server_once
         * uses: increment quiesce_waiters before waiting, decrement it and
         * broadcast after waking, so the winner's tail wait for
         * quiesce_waiters == 0 counts this call too (see that function's doc
         * comment).
         *
         * No blind poll at a fixed interval. This function does the
         * structurally identical CHTTPSVR_LC_STOPPING and "currently
         * stopping" backoffs elsewhere, and a blind poll like that measurably
         * busy-loops thousands of times a second and also starves the real
         * teardown thread's condition variable wakeup, which then never wins
         * against a much cheaper second pin. The "currently stopping" branch
         * further below blocks on a condition variable for the same reason.
         *
         * quiesce_waiters MUST be incremented here, in the same critical
         * section that just saw CHTTPSVR_QS_QUIESCING and strictly before this
         * call's resolve pin is dropped below, never after taking raw->mutex
         * again. Incrementing after dropping the pin is a use-after-free:
         * dropping the pin first can unblock _quiesce_server_once's
         * pending_resolve_count wait at once, and the winner then runs its
         * whole real teardown (_chttpsvr_stop_internal,
         * _drain_and_close_all_connections and _engine_release) and reaches its
         * own "while quiesce_waiters > 0" tail check before we ever get back
         * to raw->mutex. It finds quiesce_waiters still zero and returns,
         * __chttpsvr_destroy goes straight on to
         * _ccol_mutex_destroy(raw->mutex) and frees raw, and nothing is left
         * protecting our _ccol_mutex_lock(raw->mutex) below, which then runs
         * on freed memory.
         *
         * Incrementing while still holding the lock that saw
         * CHTTPSVR_QS_QUIESCING closes that, because the winner can't move to
         * CHTTPSVR_QS_QUIESCED until the increment is visible to it, as that
         * move needs raw->mutex itself. So quiesce_waiters is provably nonzero
         * for as long as the winner takes to reach its tail check, whatever
         * the real schedule of the two threads, and that's what keeps raw
         * alive across the resolve_unpin and the wait loop below.
         *
         * That's exactly why waiting directly on raw is safe here, unlike the
         * process-wide "currently stopping" condition below, which has to
         * resolve again first. We still resolve from the handle afterwards
         * instead of using this raw pointer again, like every other backoff
         * branch in this function: the slot table, not raw itself, is what
         * proves whether srv is still a live, startable handle once this wait
         * is over. */
        raw->quiesce_waiters++;
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw);
#ifdef RUNNING_UNIT_TESTS
        _start_quiescing_unpinned_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        _ccol_mutex_lock(raw->mutex);
        while (raw->quiesce_state != CHTTPSVR_QS_QUIESCED)
          _ccol_cond_var_wait(raw->quiesce_done_cv, raw->mutex);
        raw->quiesce_waiters--;
        _ccol_cond_var_broadcast(raw->quiesce_done_cv);
        _ccol_mutex_unlock(raw->mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        _ccol_mutex_lock(raw->mutex);
        continue;
      case CHTTPSVR_QS_NOT_QUIESCED:
      case CHTTPSVR_QS_QUIESCED:
        break;
    }
    /* No `break;` here: this `for (;;)` loop's scope runs all the way to the
     * end of the function, which is what lets the "currently stopping" branch
     * far below, inside the `if (need_acquire) {...}` block, `continue;` back
     * to the top. That scope doesn't affect any `return` in the rest of the
     * function, since a `return` leaves the function whatever the loop
     * nesting. Control just falls through to the claim below once there's
     * nothing left to check. */
    /* Claim the state here, under the same lock and in the same critical
     * section as the checks above, so a second, concurrent chttpsvr_start()
     * on this handle can't slip past both checks before this one sets it.
     * The comment of chttpsvr_lifecycle_t says what that race would
     * otherwise break: a use-after-free of raw->tls_ctx, and dropped worker
     * pools. Every return point below resets this field to CHTTPSVR_LC_IDLE
     * under raw->mutex. */
    raw->lifecycle = CHTTPSVR_LC_STARTING;
    /* An earlier stop or quiesce cycle, a chttpsvr_stop() plus
     * chttpsvr_start() restart or a chttpsvr_engine_stop() force-stop that
     * this server survived without a chttpsvr_destroy(), may have left
     * quiesce_state at CHTTPSVR_QS_QUIESCED. The server is legitimately
     * starting fresh, so _quiesce_server_once has to be ready to do its real
     * teardown again the next time something really destroys it.
     *
     * Resetting unconditionally is safe, since the loop above guarantees no
     * pass with quiesce_state == CHTTPSVR_QS_QUIESCING is still in flight at
     * this point, under the same raw->mutex critical section that we've held
     * without a break. */
    raw->quiesce_state = CHTTPSVR_QS_NOT_QUIESCED;
    __atomic_store_n(&raw->draining, false, __ATOMIC_RELAXED);
    _ccol_mutex_unlock(raw->mutex);

    /* Retired pool sets of earlier restarts that finished draining get
     * joined here as well, not only by the sweep. */
    _retired_pools_reap(raw, false);

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
    ctpool new_pool =
        ctpool_create_mp((size_t)nthreads, queue_cap, raw->m_procs, &pool_err);
    if (!new_pool) {
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return ccol_not_enough_memory;
    }

    /* reject_pool's thread count scales with worker_pool's resolved size,
     * nthreads above, i.e. the value already worked out from the CPU count
     * when cfg->worker_thread_count is 0 or less, not the raw config field,
     * which can be 0 or less. It isn't a fixed constant, and
     * _CHTTPSVR_REJECT_POOL_MIN_THREADS is its floor, so a server with one or
     * a few workers still gets real concurrency for its rejection traffic
     * (that constant's comment says why).
     *
     * The queue is bounded at _CHTTPSVR_REJECT_POOL_QUEUE_CAP; the comments of
     * reject_pool and of that constant say why. If this create fails, new_pool
     * above must be torn down too rather than leaked. */
    int reject_nthreads = nthreads / 2;
    if (reject_nthreads < _CHTTPSVR_REJECT_POOL_MIN_THREADS)
      reject_nthreads = _CHTTPSVR_REJECT_POOL_MIN_THREADS;
    char *reject_pool_err = NULL;
    ctpool new_reject_pool = ctpool_create_mp((size_t)reject_nthreads,
                                              _CHTTPSVR_REJECT_POOL_QUEUE_CAP,
                                              raw->m_procs, &reject_pool_err);
    if (!new_reject_pool) {
      ctpool_shutdown_drain(new_pool);
      ctpool_destroy(new_pool);
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 3 */
      return ccol_not_enough_memory;
    }

    /* The public fields are microseconds; the server keeps milliseconds.
     * See _cfg_us_to_ms. */
    unsigned stream_read_ms = _cfg_us_to_ms(cfg->stream_read_timeout_us);
    atomic_store(&raw->stream_read_timeout_ms, stream_read_ms);
    atomic_store(&raw->max_body_read_duration_ms,
                 _cfg_us_to_ms(cfg->max_body_read_duration_us));
    atomic_store(&raw->response_write_timeout_ms,
                 cfg->response_write_timeout_us
                     ? _cfg_us_to_ms(cfg->response_write_timeout_us)
                     : stream_read_ms);
    atomic_store(&raw->max_response_write_duration_ms,
                 _cfg_us_to_ms(cfg->max_response_write_duration_us));
    atomic_store(&raw->max_header_read_duration_ms,
                 _cfg_us_to_ms(cfg->max_header_read_duration_us));
    atomic_store(&raw->idle_timeout_ms,
                 _cfg_us_to_ms(cfg->idle_timeout_us ? cfg->idle_timeout_us
                                                    : cfg->read_timeout_us));
    atomic_store(&raw->max_body_size, cfg->max_body_size);
    atomic_store(&raw->max_header_bytes, cfg->max_header_bytes);
    atomic_store(&raw->max_connections, cfg->max_connections);
    raw->enable_keepalive = cfg->enable_keepalive;
    {
      /* The slow-client settings read 0 as "the default" and only turn off
       * through their own named constants, so a configuration that never
       * mentions them keeps every defense. Those constants are their types'
       * maximum values and pass through unchanged as the resolved field's
       * "off" or "no limit" state. */
      atomic_store(&raw->min_rate_bps, cfg->min_transfer_rate_bps
                                           ? cfg->min_transfer_rate_bps
                                           : _CHTTPSVR_DEFAULT_MIN_RATE_BPS);
      atomic_store(&raw->rate_grace_ms,
                   cfg->min_transfer_rate_grace_us
                       ? _cfg_us_to_ms(cfg->min_transfer_rate_grace_us)
                       : _CHTTPSVR_DEFAULT_RATE_GRACE_MS);
      atomic_store(&raw->mem_cap, cfg->max_partial_body_memory
                                      ? cfg->max_partial_body_memory
                                      : _CHTTPSVR_DEFAULT_PARTIAL_BODY_MEMORY);
      uint64_t mem_wait_us = cfg->body_memory_wait_timeout_us;
      atomic_store(&raw->mem_wait_ms, mem_wait_us
                                          ? _cfg_deadline_us_to_ms(mem_wait_us)
                                          : _CHTTPSVR_DEFAULT_MEMORY_WAIT_MS);
      int st = cfg->streaming_thread_count;
      if (st == 0) st = nthreads;
      if (st < 0) st = -1;
      atomic_store(&raw->stream_threads, st);
      size_t qcap;
      if (cfg->streaming_queue_capacity == CHTTPSVR_QUEUE_UNBOUNDED)
        qcap = SIZE_MAX;
      else if (cfg->streaming_queue_capacity == 0)
        qcap = st > 0 ? (size_t)4 * (size_t)st : 0;
      else
        qcap = cfg->streaming_queue_capacity;
      atomic_store(&raw->stream_qcap, qcap);
      atomic_store(&raw->stream_qtimeout_ms,
                   cfg->streaming_queue_timeout_us
                       ? _cfg_deadline_us_to_ms(cfg->streaming_queue_timeout_us)
                       : _CHTTPSVR_DEFAULT_STREAM_QUEUE_TIMEOUT_MS);
      _ccol_mutex_lock(raw->wait_mutex);
      raw->mem_admission_closed = false;
      _ccol_mutex_unlock(raw->wait_mutex);
    }

    /* The fresh pools replace the previous run's, if any, in one critical
     * section, so a connection that stays open across the stop always finds
     * a pool. The old pools drain in the background (see _retire_pools), so
     * a restart neither waits for nor cuts off a request of the previous run
     * that's still going. This start's settings are all stored above, before
     * the swap, so a request that finds a new pool through srv->mutex sees
     * the new settings too, and a first streaming request builds its pool
     * with the new streaming_thread_count, never the old one. */
    _ccol_mutex_lock(raw->mutex);
    _detached_pools_t previous = {raw->worker_pool, raw->reject_pool,
                                  raw->stream_pool};
    raw->worker_pool = new_pool;
    raw->reject_pool = new_reject_pool;
    raw->stream_pool = CTPOOL_INVALID;
    _stream_new_run_locked(raw);
    _ccol_mutex_unlock(raw->mutex);
#ifdef RUNNING_UNIT_TESTS
    _start_hold_for_tests();
#endif
    _retire_pools(raw, previous);
    /* Streaming requests waiting in the queue get started on this run's
     * streaming pool right here, and never wait for an earlier run's
     * handler to end. */
    _stream_token_start(raw);

    if (raw->tls_ctx) {
      ctls_ctx_release(raw->tls_ctx);
      raw->tls_ctx = NULL;
    }
    if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
      ctls_ctx_t *tls_ctx = ctls_ctx_new_mp(raw->m_procs, NULL);
      if (!tls_ctx) {
        _retire_current_pools(raw);
        _ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 4 */
        return ccol_not_enough_memory;
      }
      /* Every TLS context failure below passes ctls's own code on:
       * ccol_http_tls_cert_load_failed for a certificate, key or CA bundle
       * that won't load, and ccol_not_enough_memory for a failed
       * allocation. */
      ccol_retval_t tls_rv = ctls_ctx_cert_add(
          tls_ctx, NULL, cfg->tls->cert_path, cfg->tls->key_path, NULL, NULL);
      if (tls_rv != ccol_success) {
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        _ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 5 */
        return tls_rv;
      }
      /* Set the mode before the trust store, so the single rebuild that
       * ctls_ctx_trust() runs already uses it. A strict context, the
       * default, needs no call at all. */
      if (cfg->tls->client_cert_optional &&
          (tls_rv = ctls_ctx_peer_cert_optional(tls_ctx, true)) !=
              ccol_success) {
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        _ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 6 */
        return tls_rv;
      }
      if (cfg->tls->ca_bundle_path &&
          (tls_rv = ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL)) !=
              ccol_success) {
        /* We mustn't quietly treat a CA bundle we can't read, or a malformed
         * one, as "no CA bundle configured". A successful ctls_ctx_trust()
         * is what turns SSL_VERIFY_PEER on for its context, i.e. client
         * certificate verification for mutual TLS, so dropping this failure
         * would start the server with TLS but without the client
         * certificate enforcement the caller explicitly asked for, and
         * nothing would report an error anywhere. Same handling as the
         * ctls_ctx_cert_add failure a few lines up. */
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        _ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 6 */
        return tls_rv;
      }
      raw->tls_ctx = tls_ctx;
    }

    /* Register raw here, BEFORE this call tries to acquire the engine, not
     * only once that succeeds. We already hold our resolve pin for the whole
     * call (raw->pending_resolve_count, from the _chttpsvr_resolve at the top
     * of this function).
     *
     * Registering now lets a concurrent _engine_force_stop_quiesce_all pass of
     * chttpsvr_engine_stop(), which only quiesces servers it can find in
     * servers_bundler.servers, find raw at once. That pass then blocks on
     * our pin (see _quiesce_server_once's pending_resolve_count wait) before
     * releasing the shared reactor and maybe tearing it down, even when the
     * race lands in the narrow window before the _engine_acquire() below.
     *
     * Registering only after _engine_acquire() succeeds leaves exactly that
     * window open: a concurrent force-stop landing there doesn't find raw in
     * servers_bundler.servers at all, finishes its scan believing every
     * server is quiesced, and may then tear the reactor down while we're
     * about to acquire and use it. That shows up as a spurious
     * ccol_unexpected_failure from this call, with ccol_event_loop_add
     * failing against a reactor that's already gone. Not a crash, but a real
     * race that we can avoid.
     *
     * _servers_register is idempotent, so calling it unconditionally here is
     * safe for every caller, whether or not we go on to need a fresh engine
     * reference, and also for a restart that registered raw on an earlier
     * chttpsvr_start().
     *
     * If _engine_acquire() below fails, the failure path undoes this
     * registration rather than leaving it dangling (see exit 7 below), since
     * unregistered and holding no engine reference is the right state for a
     * call that never finished its start.
     *
     * A failed registration, which only a failed array growth gives, fails
     * the start: a server outside the array is never swept and never
     * quiesced before an engine force-stop tears the reactor down. */
    if (!_servers_register(raw)) {
      _SRV_ENGINE_LOG(clog_error,
                      "chttpsvr_start: the server registry cannot grow");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 6b */
      return ccol_not_enough_memory;
    }

    bool need_acquire;
    _ccol_mutex_lock(raw->mutex);
    need_acquire = !raw->contributed_to_engine;
    if (need_acquire) raw->contributed_to_engine = true;
    _ccol_mutex_unlock(raw->mutex);

    if (need_acquire) {
#ifdef RUNNING_UNIT_TESTS
      _engine_stopping_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
      bool currently_stopping = false;
      uintptr_t engine_gen = 0;
      ccol_retval_t engine_rc =
          _engine_acquire(&currently_stopping, &engine_gen);
      if (engine_rc == ccol_success) {
        _ccol_mutex_lock(raw->mutex);
        raw->engine_gen = engine_gen;
        _ccol_mutex_unlock(raw->mutex);
      }
      if (currently_stopping) {
        /* A reap is tearing the shared reactor down right now, either forced
         * by chttpsvr_engine_stop() or triggered gracefully by some other
         * server's chttpsvr_destroy() dropping reactor_refs to 0.
         * _engine_acquire() returned at once instead of blocking for that
         * reap, and never incremented reactor_refs; its doc comment has the
         * deadlock this closes: blocking there while we still hold our
         * resolve pin is exactly what a _quiesce_server_once pass for this
         * server would wait on, and the reap itself is stuck behind that same
         * pass.
         *
         * We unwind exactly like the engine_rc != ccol_success path below, on
         * purpose with the same template as exit 7 and not with the
         * _chttpsvr_undo_start_registration() helper that exits 8 and 9 use.
         * Those exits run after _engine_acquire() succeeded and really
         * incremented reactor_refs, so undoing them means really releasing a
         * reference, whereas this branch never incremented it and has nothing
         * to release. */
        _ccol_mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        _servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        _retire_current_pools(raw);
        _chttpsvr_resolve_unpin(raw);
        /* A real condition variable wait, like the quiesce_state and
         * lifecycle checks above. A blind poll and retry ("nanosleep(1ms),
         * resolve again, continue") measurably busy-loops thousands of times
         * a second under load for this kind of backoff (see each of those
         * comments).
         *
         * A blind retry loop has a second problem here too: it can out-race
         * _quiesce_server_once's thread for this server, which needs a short
         * window to notice pending_resolve_count hit 0 and get going. In that
         * window our pin sits at 0 for a moment, right after the
         * _chttpsvr_resolve_unpin above, and an instant retry registers and
         * pins again before the reaper thread ever gets scheduled. That's a
         * genuine starvation bug, not a theoretical one.
         *
         * We wait on srv_engine_bundler.stopped_cv, a process-wide condition
         * variable, while the other two waits use resolve_cv and
         * quiesce_done_cv, which belong to raw. The difference is that raw
         * may already be freed by the time the reap we're waiting on is done,
         * so nothing belonging to raw is safe to wait on here. We've ALREADY
         * fully dropped our own pin above, the one and only thing the reap
         * could have been waiting on, so waiting on a process-wide condition
         * variable with no pin held is safe, with no risk of a second
         * deadlock on a pin only we could release.
         *
         * stopped_cv is broadcast after the whole reap completes, not after
         * this one server's quiesce, which is the condition this wait needs,
         * and it's safe to wait on precisely because we hold no pin across
         * it. */
        _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
        _ccol_mutex_lock(srv_engine_bundler.mutex);
        while (srv_engine_bundler.stopping)
          _ccol_cond_var_wait(srv_engine_bundler.stopped_cv,
                              srv_engine_bundler.mutex);
        _ccol_mutex_unlock(srv_engine_bundler.mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        _ccol_mutex_lock(raw->mutex);
        continue;
      }
      if (engine_rc != ccol_success) {
        clog_error(raw->cl,
                   engine_rc == ccol_not_enough_memory
                       ? "the shared reactor engine could not be created"
                       : "the engine-stop watcher thread could not be "
                         "started");
        _ccol_mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        _ccol_mutex_unlock(raw->mutex);
        /* Undo the speculative registration above: this call never got an
         * engine reference, so raw mustn't stay visible to
         * _engine_force_stop_quiesce_all as though it held one. We only get
         * here when need_acquire was true, i.e. raw held no reference and
         * nothing had registered it before this call began, so unregistering
         * can never undo a registration that an earlier, successful
         * chttpsvr_start() still depends on. */
        _servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        _retire_current_pools(raw);
        _chttpsvr_resolve_unpin(raw); /* exit 7 */
        return engine_rc;
      }
    }
#ifdef RUNNING_UNIT_TESTS
    _start_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
    if (!_idle_sweep_start_if_needed()) {
      /* Two features of every registered server rely entirely on this shared
       * thread: enforcing idle_timeout_ms and getting max_connections
       * capacity back. Carrying on as if the call had worked would quietly
       * leave both broken for good on this server, and in the
       * max_connections case also leave a listener paused forever, with
       * nothing able to resume it, the moment the server hits capacity,
       * while chttpsvr_start() itself reported success. So this branch fails
       * just like the _make_listen_socket failure a few lines below: it
       * undoes whatever this attempt set up and reports a real error. */
      _SRV_ENGINE_LOG(clog_error,
                      "idle-timeout sweep thread could not be started");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _chttpsvr_undo_start_registration(raw, need_acquire);
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 8 */
      return ccol_unexpected_failure;
    }

    bool is_unix = false;
    _chttpsvr_unix_bind_t unix_bind;
    int lfd =
        _make_listen_socket(cfg->host, cfg->port, cfg->enable_reuseport,
                            cfg->ipv6_only, &is_unix, &unix_bind, raw->m_procs);
    uint16_t bound_port = 0;
    char bound_host[INET6_ADDRSTRLEN] = "";
    if (lfd >= 0 && !is_unix &&
        !_listen_sockname(lfd, &bound_port, bound_host, sizeof(bound_host))) {
      int e = errno;
      close(lfd);
      lfd = -1;
      errno = e;
    }
    if (lfd < 0) {
      int listen_errno = errno;
#ifdef RUNNING_UNIT_TESTS
      atomic_store(&g_last_listen_errno_for_tests, listen_errno);
#endif /* RUNNING_UNIT_TESTS */
      _SRV_ENGINE_LOG(
          clog_error, "listen socket setup failed host=%s port=%u errno=%d",
          cfg->host ? cfg->host : "(any)", (unsigned)cfg->port, listen_errno);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _chttpsvr_undo_start_registration(raw, need_acquire);
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 9 */
      return ccol_unexpected_failure;
    }

    /* Publish these fields BEFORE the ccol_event_loop_add below, not after,
     * because that call makes the registration live at once and a connection
     * can come in between the registration and the write of these fields.
     * Otherwise such a connection is dispatched to _listener_on_readable
     * while it still sees listen_fd's pre-start default of -1 (see the
     * comment of listen_fd on struct chttpserver). listen_fd goes back to -1
     * below if ccol_event_loop_add itself fails, so a failed start never
     * leaves listen_fd pointing at an fd we're about to close. */
    atomic_store(&raw->listen_fd, lfd);
    atomic_store(&raw->is_unix_socket, is_unix);

    _ccol_call_once(srv_parser_bundler.once, _init_parser_settings);
    char *reg_err = NULL;
    /* The registration holds one raw->lifetime_refs reference, released by
     * _listener_on_removed (see that field's comment). We take it before the
     * add, since a registration's on_removed can fire as soon as the
     * registration exists (a reactor teardown). The application reference
     * this call holds through its resolve keeps the count above 0, so the
     * release on a failed add never reaches the final teardown. */
    atomic_fetch_add(&raw->lifetime_refs, 1);
    ccol_event_reg lreg = ccol_event_loop_add(
        srv_engine_bundler.reactor,
        ccol_selectable_from_fd(lfd, ccol_select_read),
        (ccol_event_handlers_t){.on_readable = _listener_on_readable,
                                .on_removed = _listener_on_removed},
        raw, &reg_err);
    if (!lreg) {
      atomic_fetch_sub(&raw->lifetime_refs, 1);
      atomic_store(&raw->listen_fd, -1);
      close(lfd);
      _unix_bind_cleanup(&unix_bind, raw->m_procs);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _chttpsvr_undo_start_registration(raw, need_acquire);
      _ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      _ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 10 */
      return ccol_unexpected_failure;
    }

    _ccol_mutex_lock(raw->mutex);
    raw->unix_bind = unix_bind;
    raw->bound_port = bound_port;
    memcpy(raw->bound_host, bound_host, sizeof(bound_host));
    raw->listen_reg = lreg;
    raw->lifecycle = CHTTPSVR_LC_RUNNING;
    _ccol_mutex_unlock(raw->mutex);

    /* raw is already registered above, right after the engine reference was
     * confirmed, so there's nothing left to do here. */
    _chttpsvr_resolve_unpin(raw); /* exit 11 (success) */
    return ccol_success;
  } /* end of the for (;;) retry loop opened near the top of this function */
}

/* The real work of chttpsvr_stop, on a struct chttpserver* that's already
 * been resolved.
 *
 * It lives in its own function so _quiesce_server_once can call it directly:
 * that function already holds a resolved raw pointer, which it got from the
 * servers_bundler registry and never from a chttpsvr handle, so it doesn't
 * go through the public chttpsvr_stop, which resolves a handle.
 *
 * The split matters because chttpsvr is a value handle, not a pointer:
 * passing a raw struct chttpserver* where a handle belongs fails to compile
 * or, worse, reads the pointer's bit pattern as a slot index and generation
 * pair. Same split as chttpclient.c has between chttp_do_internal, which
 * works on a struct chttpclient*, and the public chttpclient_do wrapper,
 * which resolves and then calls. */
static void _chttpsvr_stop_internal(struct chttpserver *raw) {
  _ccol_mutex_lock(raw->mutex);
  /* An exhaustive switch with no `default:` label rather than a plain
   * `raw->lifecycle == CHTTPSVR_LC_RUNNING` comparison, for the same
   * `-Wswitch` reason chttpsvr_lifecycle_t's comment gives. Every other
   * reachable state comes out as was_started == false here, including a
   * concurrent second chttpsvr_stop() that sees CHTTPSVR_LC_STOPPING and a
   * call racing an in-flight chttpsvr_start() that sees
   * CHTTPSVR_LC_STARTING. */
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
  ccol_event_reg lreg = raw->listen_reg;
  _chttpsvr_unix_bind_t unix_bind = raw->unix_bind;
  if (was_started) {
    /* Set in the same critical section that leaves CHTTPSVR_LC_RUNNING, so
     * chttpsvr_start()'s wait loop can never see lifecycle ==
     * CHTTPSVR_LC_IDLE without having seen CHTTPSVR_LC_STOPPING first, for as
     * long as this function's real teardown below is still in flight. The
     * comment of chttpsvr_lifecycle_t has the race this state closes. */
    raw->lifecycle = CHTTPSVR_LC_STOPPING;
    atomic_store(&raw->listen_fd, -1);
    raw->listen_reg = CCOL_EVENT_REG_INVALID;
    raw->unix_bind.path = NULL;
    raw->unix_bind.dir_fd = -1;
  }
  _ccol_mutex_unlock(raw->mutex);
  if (!was_started) return;

#ifdef RUNNING_UNIT_TESTS
  _stop_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* Once ccol_event_loop_remove returns, no NEW _listener_on_readable
   * dispatch can start for lreg. But it does NOT wait for a dispatch already
   * in progress when it's called, because it never takes the registration's
   * dispatch_lock (see the comment of _ccol_event_loop_run_callback in
   * cthreadcomm.c and the doc comment of ccol_event_loop_remove in
   * cthreadcomm.h). "teardown is deferred until any in-progress callback
   * returns" there is about ccol_event_loop's internal bookkeeping memory,
   * not a promise that the calling thread blocks for that callback.
   *
   * Without the wait below there's a real, narrow race of a few
   * instructions, which load srv->listen_fd into a register and then call
   * accept4() with it. A dispatch already past that load may still be about
   * to accept4() on the fd number we're about to close, and some unrelated,
   * concurrent open() in this process can then reuse that number.
   *
   * We close it by waiting for listener_dispatch_pins to reach 0. A dispatch
   * that pinned before listen_fd was cleared above holds this wait back
   * until it returns, and one that pins after the clear compares its
   * registration's fd with listen_fd, sees a mismatch and never calls
   * accept4(). raw's memory is a separate matter: the listener registration
   * holds a raw->lifetime_refs reference until its on_removed fires (see
   * _listener_on_removed).
   *
   * This wait is safe here in particular. _quiesce_server_once and
   * _chttpsvr_stop_internal must never wait on servers_bundler_pins or
   * pending_resolve_count (see each field's comment), but
   * listener_dispatch_pins is different: only the reactor thread running
   * _listener_on_readable holds it, never the thread calling
   * chttpsvr_stop(), _quiesce_server_once or __chttpsvr_destroy. The thread
   * waiting here is never the one that must run for the counter to reach
   * 0, so it can't deadlock on itself.
   *
   * ccol_event_loop_remove() has already run when the wait starts, so it can
   * only be waiting on a dispatch live at that very moment, never a fresh
   * one, and there's at most one, since ccol_event_loop's per-registration
   * dispatch_lock already keeps a registration's callback from running
   * concurrently with itself. */
  if (lreg) ccol_event_loop_remove(srv_engine_bundler.reactor, lreg);
  _ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->listener_dispatch_pins) > 0)
    _ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  _ccol_mutex_unlock(raw->mutex);
  close(lfd);
  _unix_bind_cleanup(&unix_bind, raw->m_procs);

  /* An unconditional write rather than a switch: we only get here when
   * was_started was true (an early `return` sits between here and the false
   * case), and chttpsvr_start()'s retry loop always backs off and retries
   * for as long as lifecycle == CHTTPSVR_LC_STOPPING, so nothing else can
   * move raw->lifecycle off CHTTPSVR_LC_STOPPING between the claim at the
   * top of this function and this exit. A fork() is different, as it can
   * land at a truly arbitrary point with no such guarantee (see the matching
   * fixup in _chttpsvr_atfork_release_impl), while this is one uninterrupted,
   * synchronous call sequence with a single writer throughout.
   *
   * We broadcast resolve_cv, the same condition variable this function's
   * listener_dispatch_pins wait above uses, so a concurrent chttpsvr_start()
   * blocked in its own CHTTPSVR_LC_STOPPING wait wakes promptly (see that
   * branch's comment) instead of only noticing on its next poll tick. */
  _ccol_mutex_lock(raw->mutex);
  raw->lifecycle = CHTTPSVR_LC_IDLE;
  _ccol_cond_var_broadcast(raw->resolve_cv);
  _ccol_mutex_unlock(raw->mutex);
}

void chttpsvr_stop(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return;
  _chttpsvr_stop_internal(raw);
  _chttpsvr_resolve_unpin(raw);
}

/* A unix:// path is shorter than sun_path, so its "unix://" form fits in
 * CHTTPSVR_BOUND_ADDRESS_MAX bytes, and every numeric address fits too. */
_Static_assert(sizeof(_CHTTPSVR_UNIX_PREFIX) - 1 +
                       sizeof(((struct sockaddr_un *)0)->sun_path) <=
                   CHTTPSVR_BOUND_ADDRESS_MAX,
               "CHTTPSVR_BOUND_ADDRESS_MAX must hold a unix:// address");
_Static_assert(INET6_ADDRSTRLEN <= CHTTPSVR_BOUND_ADDRESS_MAX,
               "CHTTPSVR_BOUND_ADDRESS_MAX must hold a numeric address");

ccol_retval_t chttpsvr_is_running(chttpsvr h, bool *running_out) {
  if (running_out) *running_out = false;
  if (!running_out) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  _ccol_mutex_lock(raw->mutex);
  *running_out = raw->lifecycle == CHTTPSVR_LC_RUNNING;
  _ccol_mutex_unlock(raw->mutex);
  _chttpsvr_resolve_unpin(raw);
  return ccol_success;
}

ccol_retval_t chttpsvr_bound_port(chttpsvr h, uint16_t *port_out) {
  if (port_out) *port_out = 0;
  if (!port_out) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = ccol_not_permitted;
  _ccol_mutex_lock(raw->mutex);
  if (raw->lifecycle == CHTTPSVR_LC_RUNNING &&
      !atomic_load(&raw->is_unix_socket)) {
    *port_out = raw->bound_port;
    rv = ccol_success;
  }
  _ccol_mutex_unlock(raw->mutex);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_bound_address(chttpsvr h, char *buf, size_t buflen) {
  if (buf && buflen) buf[0] = '\0';
  if (!buf || !buflen) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = ccol_not_permitted;
  _ccol_mutex_lock(raw->mutex);
  if (raw->lifecycle == CHTTPSVR_LC_RUNNING) {
    const char *prefix = "";
    const char *text = raw->bound_host;
    if (atomic_load(&raw->is_unix_socket)) {
      prefix = _CHTTPSVR_UNIX_PREFIX;
      text = raw->unix_bind.path;
    }
    size_t prefix_len = strlen(prefix);
    size_t text_len = strlen(text);
    if (prefix_len + text_len < buflen) {
      memcpy(buf, prefix, prefix_len);
      memcpy(buf + prefix_len, text, text_len + 1);
      rv = ccol_success;
    } else {
      rv = ccol_msg_too_large;
    }
  }
  _ccol_mutex_unlock(raw->mutex);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_set_engine_logger(clog cl) {
  if (!cl) {
    /* A release. The derived logger is closed here, through the allocator
     * of the logger it came from, so nothing calls that allocator later at
     * process exit. A running engine logs through it, so a release has to
     * wait for a full stop. */
    _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
    _ccol_mutex_lock(srv_engine_bundler.mutex);
    if (srv_engine_bundler.reactor || srv_engine_bundler.stopping) {
      _ccol_mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_permitted;
    }
    clog old_user = srv_engine_bundler.user_log;
    srv_engine_bundler.user_log = CLOG_INVALID;
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    if (old_user) clog_close(old_user);
    return ccol_success;
  }
  /* A closed or stale handle is a caller error we report, not a reason to
   * end the process. */
  ccol_retval_t derive_rv = ccol_success;
  clog derived = _clog_derive_checked(cl, &derive_rv);
  if (!derived) return derive_rv;
  clog_set_field(derived, "component", "http-server-engine");
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  clog old_user = srv_engine_bundler.user_log;
  clog old_fallback = CLOG_INVALID;
  srv_engine_bundler.user_log = derived;
  /* A running engine switches to the new logger at once. The fallback it ran
   * with belongs to nobody else, so we close it too. */
  if (srv_engine_bundler.log) {
    if (srv_engine_bundler.log != old_user)
      old_fallback = srv_engine_bundler.log;
    srv_engine_bundler.log = derived;
  }
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (old_user) clog_close(old_user);
  if (old_fallback) clog_close(old_fallback);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    srv_engine_bundler.mprocs_storage = *mp;
    srv_engine_bundler.mprocs = &srv_engine_bundler.mprocs_storage;
  } else {
    srv_engine_bundler.mprocs = NULL;
  }
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_num_reactor_threads(size_t num_threads) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    _ccol_mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  srv_engine_bundler.num_reactor_threads = num_threads;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

/* White-box test helper: the reactor thread count actually wired into the
 * reactor we created last. Not public API; a gate keeps the symbol out of a
 * production build of libccollections.so, the same convention chttpclient.c
 * follows for the test helpers that expose its engine's internal state. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_engine_num_reactor_threads_for_tests(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  size_t n = srv_engine_bundler.last_resolved_num_reactor_threads;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return n;
}
#endif /* RUNNING_UNIT_TESTS */

/* White-box test helper for g_reject_task_run_count_for_tests (see that
 * variable's comment): how many rejection responses actually ran on a
 * reject_pool thread, process-wide, since the counter started at zero. A test
 * reads it before and after its own window and asserts on the difference,
 * since other tests in the same process may cause rejections too. Not public
 * API; a gate keeps the symbol out of a production build of
 * libccollections.so, like _chttpsvr_engine_num_reactor_threads_for_tests
 * just above. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_reject_pool_task_count_for_tests(void) {
  return atomic_load(&g_reject_task_run_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Resolves h to its struct chttpserver* WITHOUT a pin: it doesn't touch
 * pending_resolve_count at all, it's just a slot table lookup. That's safe
 * for tests in particular, since the test code calling it runs synchronously
 * on one thread, so there's no concurrent destroy to race in the first place.
 *
 * Unlike _chttpsvr_resolve there's no matching unpin for a test to remember,
 * which is an easy thing to forget: a missed unpin leaves
 * pending_resolve_count nonzero on that server for good, and every later
 * chttpsvr_destroy of it then hangs silently.
 *
 * Returns NULL under exactly the same conditions as _chttpsvr_resolve. Same
 * as _chttpcli_resolve_for_tests in chttpclient.c. */
#ifdef RUNNING_UNIT_TESTS
struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  /* A table released at unload holds no server, so every handle resolves to
   * NULL there. */
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

/* How many slots the chttpsvr handle table holds right now, so a test can
 * check that a create/destroy loop reuses freed slots; without that the table
 * grows without bound. Same as _chttpcli_slot_table_capacity_for_tests in
 * chttpclient.c. */
size_t _chttpsvr_slot_table_capacity_for_tests(void) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return n;
}

/* How many servers are registered right now in servers_bundler.servers[],
 * the list _engine_force_stop_quiesce_all's driver loop walks, so a test can
 * confirm a server really got unregistered, e.g. by the child-side fixup of
 * _chttpsvr_atfork_release_impl, which runs for a server a fork() caught in
 * the middle of a teardown (see that function's doc comment), without needing
 * a fresh chttpsvr_engine_stop() pass to see it indirectly. */
size_t _chttpsvr_servers_bundler_count_for_tests(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(servers_bundler.mutex);
  size_t n = servers_bundler.count;
  _ccol_mutex_unlock(servers_bundler.mutex);
  return n;
}

/* White-box test helper: the resolved idle timeout, in milliseconds, that a
 * chttpsvr_start() actually stored. It's built from the chttpsvr_config_t
 * fields idle_timeout_us and read_timeout_us, both microsecond counts, and
 * converted into the internal `unsigned` millisecond field through
 * _cfg_us_to_ms.
 *
 * So a test can check the conversion directly: a configured value of
 * UINT_MAX - 1 milliseconds or more has to land on UINT_MAX - 1 and never
 * quietly wrap to 0, the field's "off" sentinel, or to some unrelated value,
 * without sitting through an astronomically long real idle timeout.
 *
 * Returns 0 for an invalid handle, the same answer as for a real server
 * whose idle timeout really is off, so a test using it is expected to
 * already know h is otherwise valid. */
unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  return atomic_load(&raw->idle_timeout_ms);
}

/* White-box test helper: every duration a chttpsvr_start() resolved from
 * chttpsvr_config_t, in the milliseconds the server keeps, in this order:
 * stream_read_timeout, max_body_read_duration, response_write_timeout,
 * max_response_write_duration, max_header_read_duration, the idle timeout,
 * min_transfer_rate_grace, body_memory_wait_timeout and
 * streaming_queue_timeout. That lets a test pin the conversion from
 * microseconds, each default a 0 picks, and the internal form of
 * CHTTPSVR_NO_DEADLINE without waiting for any of them. Returns false for an
 * invalid handle. */
bool _chttpsvr_resolved_ms_for_tests(chttpsvr h, unsigned out[9]) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return false;
  out[0] = atomic_load(&raw->stream_read_timeout_ms);
  out[1] = atomic_load(&raw->max_body_read_duration_ms);
  out[2] = atomic_load(&raw->response_write_timeout_ms);
  out[3] = atomic_load(&raw->max_response_write_duration_ms);
  out[4] = atomic_load(&raw->max_header_read_duration_ms);
  out[5] = atomic_load(&raw->idle_timeout_ms);
  out[6] = atomic_load(&raw->rate_grace_ms);
  out[7] = atomic_load(&raw->mem_wait_ms);
  out[8] = atomic_load(&raw->stream_qtimeout_ms);
  return true;
}

/* White-box test helper: the current in_flight_requests count (that field's
 * comment, _release_in_flight, _conn_start_diverted and _conn_dispatch_reject
 * say what it tracks). Everywhere else in this file the field is read and
 * written under raw->mutex, so we read it the same way here, not with an
 * unsynchronized bare load.
 *
 * A test can poll it for a real, bounded condition, "the request I just sent
 * really got dispatched to a worker thread", instead of a fixed sleep that
 * guesses how long accept, parse and dispatch take on whatever machine runs
 * it.
 *
 * Returns -1 for an invalid handle, which no real count can be, since a real
 * count is never negative. */
int _chttpsvr_in_flight_requests_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return -1;
  _ccol_mutex_lock(raw->mutex);
  int n = raw->in_flight_requests;
  _ccol_mutex_unlock(raw->mutex);
  return n;
}

/* White-box test hook only. It marks the calling thread as though it were a
 * worker thread of h right now, i.e. a thread of h's worker_pool or
 * reject_pool (see the comment of chttpsvr_worker_key_bundle above), without
 * dispatching a real task and without needing a running listener or reactor
 * at all, so a test can drive chttpsvr_destroy()'s self-call guard
 * deterministically and directly.
 *
 * It's more than a convenience: driving that guard with a real end-to-end
 * request needs chttpsvr_start() to succeed inside a forked child of a test
 * binary whose shared reactor already runs real OS threads, and since fork(2)
 * doesn't copy threads into the child, the child holds a hollow reactor
 * handle nothing services, with no way to accept the connection meant to
 * trigger the handler under test.
 *
 * The hook is there for exactly that forked-child use, where the process ends
 * in an abort or an exit (see destroy_from_within_own_handler_is_fatal in
 * tests.c). There's no counterpart that unmarks the thread, since a mark in a
 * forked child that never returns has nothing left to leak into.
 *
 * Does nothing when h doesn't resolve. A gate keeps both this function and
 * the visibility of chttpsvr_worker_key_bundle out of a production build. */
void _chttpsvr_mark_self_as_worker_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (raw && _chttpsvr_worker_key_ensure()) _chttpsvr_mark_worker_thread(raw);
}

/* White-box accessors of the slow-client state, for the tests. Each one
 * returns 0 or false for an invalid handle. */
size_t _chttpsvr_mem_in_use_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  _ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_in_use;
  _ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

/* How many connections hold a body memory reservation, and in *exempt_out
 * whether one of them is running past the limit under the progress rule. */
size_t _chttpsvr_mem_holders_for_tests(chttpsvr h, bool *exempt_out) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (exempt_out) *exempt_out = false;
  if (!raw) return 0;
  _ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_holders;
  if (exempt_out) *exempt_out = raw->mem_exempt_active;
  _ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

/* For the request req names, held by a buffered route's handler: its
 * connection's body memory reservation and its body buffer's capacity. */
void _chttpsvr_req_body_memory_for_tests(chttpsvr_req *req, size_t *charged,
                                         size_t *held) {
  chttpsvr_conn_t *conn = req->conn;
  _ccol_mutex_lock(conn->srv->wait_mutex);
  *charged = conn->mem_charged;
  _ccol_mutex_unlock(conn->srv->wait_mutex);
  *held = conn->body.cap;
}

size_t _chttpsvr_mem_waiting_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  _ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_waiting;
  _ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

size_t _chttpsvr_stream_queue_len_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  _ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->sq_len;
  _ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

bool _chttpsvr_stream_pool_created_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return false;
  _ccol_mutex_lock(raw->mutex);
  bool created = raw->stream_pool != CTPOOL_INVALID;
  _ccol_mutex_unlock(raw->mutex);
  return created;
}

/* How many tasks the worker pool, or the streaming pool, is running right
 * now. A parked connection holds none. */
size_t _chttpsvr_pool_active_for_tests(chttpsvr h, bool streaming) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  _ccol_mutex_lock(raw->mutex);
  ctpool pool = streaming ? raw->stream_pool : raw->worker_pool;
  _ccol_mutex_unlock(raw->mutex);
  return pool ? ctpool_active_count(pool) : 0;
}

/* How many connections in the idle list are parked on a body (kind 1) or on
 * a response (kind 2). */
size_t _chttpsvr_parked_count_for_tests(chttpsvr h, int kind) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  _ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->parked_head; c; c = c->parked_next)
    if (c->park == (uint8_t)kind) n++;
  _ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* What connections parked on a response (kind 2) or lingering (kind 6)
 * still hold of their request: the body buffer's capacity, plus one per
 * header field, plus one each for a target, a decoded target, a query and a
 * parameter value array. A lingering connection also counts what it holds of
 * its response: the response body's capacity plus one per response header
 * field. 0 when none of them keeps anything. */
size_t _chttpsvr_parked_request_state_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  _ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->parked_head; c; c = c->parked_next) {
    if (c->park != _CONN_PARK_WRITE && c->park != _CONN_PARK_LINGER) continue;
    n += c->body.cap + c->hdr_count + (c->path != NULL) +
         (c->decoded_path != NULL) + (c->raw_query != NULL) +
         (c->matched_param_values != NULL);
    if (c->park == _CONN_PARK_LINGER)
      n += c->resp.body_cap + c->resp.header_count;
  }
  _ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* How many connections are in h's idle list. */
size_t _chttpsvr_idle_count_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  _ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->idle_head; c; c = c->idle_next) n++;
  _ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* Runs one sweep tick for h immediately on the calling thread, with the clock
 * read as ahead_ms later than it really is, and returns how many connections
 * that tick looked at. So a test can set the idle limits to a value no slow
 * setup reaches and still expire them without sleeping. The sweep thread
 * keeps running alongside; the lists' claims make the two safe together. */
size_t _chttpsvr_sweep_ahead_for_tests(chttpsvr h, unsigned ahead_ms) {
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  _timespec_add_ms(&now, ahead_ms);
  size_t visited = _sweep_server(raw, now);
  _chttpsvr_resolve_unpin(raw);
  return visited;
}

/* Runs one sweep tick for h right away, on the calling thread. */
size_t _chttpsvr_sweep_now_for_tests(chttpsvr h) {
  return _chttpsvr_sweep_ahead_for_tests(h, 0);
}
#endif /* RUNNING_UNIT_TESTS */

void chttpsvr_engine_stop(void) { _engine_force_stop(); }

void chttpsvr_engine_wait(void) {
  /* The self-call guard (see the comment of _chttpsvr_is_any_worker_call).
   * Blocking here from a request handler or middleware of ANY currently
   * registered server can deadlock the whole engine, not just that server:
   * the ctpool_shutdown_drain inside _engine_force_stop_quiesce_all has no
   * timeout and can never finish draining that handler's pool, since this
   * very call stack is what would let the task return, so the reactor never
   * gets torn down and this wait never wakes.
   *
   * There's no ccol_retval_t to report the misuse through, since the
   * documented contract is just "blocks until the engine exits". So a
   * detected self-call is handled like the same guards in chttpsvr_destroy()
   * and chttpsvr_start(): a loud, immediate _ccol_fatal_err() naming the real
   * problem instead of a silent hang for ever. */
  if (_chttpsvr_is_any_worker_call()) {
    _ccol_fatal_err(
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
  /* root_router, not routers[0]: a concurrent chttpsvr_subrouter() may
   * realloc raw->routers, while root_router needs no lock to read. */
  ccol_retval_t rv =
      _router_add_route(raw->root_router, method, pattern, fn, ctx, false);
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
  ccol_retval_t rv =
      _router_add_route(raw->root_router, method, pattern, fn, ctx, true);
  _chttpsvr_resolve_unpin(raw);
  return rv;
}

ccol_retval_t chttpsvr_use(chttpsvr h, chttpsvr_middleware_fn fn, void *ctx) {
  if (!fn) return ccol_invalid_args;
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval_t rv = _router_add_mw(raw->root_router, fn, ctx);
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

  _ccol_rw_lock_wrlock(raw->routes_lock);
  if (raw->router_count >= raw->router_cap) {
    if (raw->router_cap > (SIZE_MAX - 2) / 2 ||
        raw->router_cap * 2 + 2 > SIZE_MAX / sizeof(chttpsvr_router *)) {
      _ccol_rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return NULL;
    }
    size_t nc = raw->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)_ccol_mem_realloc(
        raw->m_procs, raw->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      _ccol_rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 3 */
      return NULL;
    }
    raw->routers = nr;
    raw->router_cap = nc;
  }
  /* Insert the new router among the sub-routers, the entries from index 1 up
   * (index 0 stays the root), in descending order of prefix segment count,
   * which is the order _find_route walks them in (its comment says why the
   * most specific mount has to be tried first).
   *
   * The shift stops at index 1, so nothing ever displaces the root. Mounts
   * of equal specificity keep their registration order, the only tie left to
   * break: two mounts with the same segment count are either disjoint
   * literals, at most one of which can match a given path, or the same
   * prefix registered twice. */
  {
    size_t pos = raw->router_count;
    while (pos > 1 &&
           raw->routers[pos - 1]->prefix_seg_count < r->prefix_seg_count) {
      raw->routers[pos] = raw->routers[pos - 1];
      pos--;
    }
    raw->routers[pos] = r;
    raw->router_count++;
  }
  /* See the comment of max_prefix_seg_count on struct chttpserver. We keep it
   * up to date here, in the same write-locked critical section as the
   * router's own registration, just as _router_add_route maintains
   * router->max_route_seg_count under the same lock. */
  if (r->prefix_seg_count > raw->max_prefix_seg_count)
    raw->max_prefix_seg_count = r->prefix_seg_count;
  _ccol_rw_lock_unlock(raw->routes_lock);
  _chttpsvr_resolve_unpin(raw); /* exit 4 (success) */
  return r;
}

/* The shared way in for every chttpsvr_router_on, _on_stream and _use call.
 * It reads router->owner, which is always safe to read (the comment of
 * chttpsvr_router_shell_registry explains why the shell this field lives in
 * is never freed), then resolves and pins that handle through the normal
 * chttpsvr slot table.
 *
 * Returns NULL when the owning server has already been destroyed, and the
 * resolved, pinned server otherwise, in which case the caller MUST call
 * _chttpsvr_resolve_unpin(raw) exactly once when it's done. */
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

_Static_assert(CHTTPSVR_PEER_CERT_SHA256_LEN == CTLS_SHA256_LEN,
               "the public digest length must match the one of ctls");

bool chttpsvr_req_peer_cert_verified(const chttpsvr_req *req) {
  if (!req) return false;
  return req->conn->tls && ctls_conn_peer_cert_verified(req->conn->tls);
}

const void *chttpsvr_req_peer_cert_der(const chttpsvr_req *req,
                                       size_t *len_out) {
  if (len_out) *len_out = 0;
  if (!req || !req->conn->tls) return NULL;
  return ctls_conn_peer_cert_der(req->conn->tls, len_out);
}

ccol_retval_t chttpsvr_req_peer_cert_sha256(
    const chttpsvr_req *req, unsigned char out[CHTTPSVR_PEER_CERT_SHA256_LEN]) {
  if (out) memset(out, 0, CHTTPSVR_PEER_CERT_SHA256_LEN);
  if (!req || !out) return ccol_invalid_args;
  if (!req->conn->tls || !ctls_conn_peer_cert_verified(req->conn->tls))
    return ccol_key_not_found;
  return ctls_conn_peer_cert_sha256(req->conn->tls, out)
             ? ccol_success
             : ccol_not_enough_memory;
}

const char *chttpsvr_req_peer_cert_subject(const chttpsvr_req *req) {
  if (!req || !req->conn->tls) return NULL;
  return ctls_conn_peer_cert_subject(req->conn->tls);
}

const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn_t *conn = req->conn;
  /* A repeated header name keeps every occurrence, in arrival order, in
   * conn->hdr_names and conn->hdr_values, so we scan backwards and the LAST
   * one wins, which is this module's documented behavior for a header that
   * arrives more than once (see streaming_repeated_header in tests.c). */
  for (size_t i = conn->hdr_count; i-- > 0;) {
    if (ccol_ascii_casecmp(conn->hdr_names[i], name) == 0)
      return conn->hdr_values[i];
  }
  return NULL;
}

const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out) {
  if (!req) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  chttpsvr_conn_t *conn = req->conn;
  /* The same guard as in chttpsvr_req_read explains why conn and
   * conn->matched_route are never really NULL with documented usage and why
   * we check them here anyway as a defence, unlike chttpsvr_req_method,
   * _path, _header and _raw_query, which each dereference req->conn
   * unconditionally once they've ruled out !req, even though the same
   * "never really NULL in practice" argument holds for all of them.
   *
   * A buffered route's body is pulled into conn->body up front as one
   * contiguous, stable buffer, but a streaming route's body never is. For a
   * streaming route conn->body is a live cursor instead (growbuf_t.pos),
   * which chttpsvr_req_read() drains batch by batch as bytes really arrive
   * and which _on_body() compacts back to empty lazily, when the NEXT batch
   * arrives, not the moment the cursor is drained.
   *
   * Without this guard, a streaming handler that called chttpsvr_req_read()
   * at all and then this function would get a pointer and length pair that
   * quietly mixes two kinds of byte: those already delivered, before pos, and
   * those from pos to len, not delivered yet. Or the length just reflects the
   * internal accounting since the last compaction and is neither the real
   * total nor the real unread count. Nothing reports an error at all.
   *
   * This function is documented for buffered routes only (see the doc
   * comment of chttpsvr_req_body in chttpserver.h), mirroring how
   * chttpsvr_req_read refuses a buffered route, so a route mismatch always
   * fails safe with a clear NULL and 0 on either accessor, not just on one of
   * them. */
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
  /* The same guard as in chttpsvr_req_read explains why conn and
   * conn->matched_route are never really NULL with documented usage and why
   * we check them here anyway as a defence, unlike chttpsvr_req_method,
   * _path, _header and _raw_query, which each dereference req->conn
   * unconditionally once they've ruled out !req. */
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
    const char **nr = (const char **)_ccol_mem_realloc(
        req->m_procs, req->_qresult, (n + 1) * sizeof(char *));
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
    return ccol_key_already_present;
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

/* Starting capacity of chttpsvr_req_set_context()'s overflow block, which
 * doubles from there. */
#define _CHTTPSVR_CTX_MORE_FIRST_CAP 4u

ccol_retval_t chttpsvr_req_set_context(chttpsvr_req *req, const void *key,
                                       void *value) {
  if (!req || !key) return ccol_invalid_args;
  if (!req->_ctx_first.key || req->_ctx_first.key == key) {
    req->_ctx_first.key = key;
    req->_ctx_first.value = value;
    return ccol_success;
  }
  _chttpsvr_ctx_more_t *more = req->_ctx_more;
  size_t count = more ? more->count : 0;
  for (size_t i = 0; i < count; i++) {
    if (more->pairs[i].key == key) {
      more->pairs[i].value = value;
      return ccol_success;
    }
  }
  if (!more || count == more->cap) {
    size_t cap = more ? more->cap : 0;
    if (cap > (SIZE_MAX - sizeof(_chttpsvr_ctx_more_t)) /
                  (2 * sizeof(_chttpsvr_ctx_pair_t)))
      return ccol_not_enough_memory;
    size_t new_cap = cap ? cap * 2 : _CHTTPSVR_CTX_MORE_FIRST_CAP;
    _chttpsvr_ctx_more_t *grown = (_chttpsvr_ctx_more_t *)_ccol_mem_realloc(
        req->m_procs, more,
        sizeof(_chttpsvr_ctx_more_t) + new_cap * sizeof(_chttpsvr_ctx_pair_t));
    if (!grown) return ccol_not_enough_memory;
    if (!more) grown->count = 0;
    grown->cap = new_cap;
    req->_ctx_more = more = grown;
  }
  more->pairs[more->count].key = key;
  more->pairs[more->count].value = value;
  more->count++;
  return ccol_success;
}

void *chttpsvr_req_context(const chttpsvr_req *req, const void *key) {
  if (!req || !key) return NULL;
  if (req->_ctx_first.key == key) return req->_ctx_first.value;
  const _chttpsvr_ctx_more_t *more = req->_ctx_more;
  if (!more) return NULL;
  for (size_t i = 0; i < more->count; i++)
    if (more->pairs[i].key == key) return more->pairs[i].value;
  return NULL;
}

/* ========================================================================== */
/*                         RESPONSE API                                       */
/* ========================================================================== */

void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code) {
  if (resp) resp->status_code = status_code;
}

/* Tells whether value is a Content-Length a response may carry: one or more
 * ASCII digits (RFC 9110 SS8.6) whose number fits a signed 64-bit integer,
 * the widest length the parsers of Go, curl and this library accept. */
static bool _resp_content_length_valid(const char *value) {
  static const char max_cl[] = "9223372036854775807";
  size_t n = 0;
  for (const char *p = value; *p; p++, n++)
    if (*p < '0' || *p > '9') return false;
  if (n == 0) return false;
  while (n > 1 && *value == '0') {
    value++;
    n--;
  }
  return n < sizeof(max_cl) - 1 ||
         (n == sizeof(max_cl) - 1 && strcmp(value, max_cl) <= 0);
}

/* The checks every response header goes through, whichever call sets it. */
static ccol_retval_t _resp_header_validate(const char *name,
                                           const char *value) {
  /* A field name must be a non-empty token of tchar bytes only (RFC 7230
   * SS3.2.6); being non-empty with no CR or LF isn't enough. A byte outside
   * that set, like a space or a literal ':', can't inject a CR or LF by
   * itself, but it still makes a structurally malformed wire line that a
   * strict parser downstream reads as ambiguous: the name "X Foo: bar" puts
   * "X Foo:bar:baz\r\n" on the wire, which isn't a genuine split into two
   * fields.
   *
   * We use the same tchar classifier chttp1_parser.c applies to an incoming
   * request's header names and chttpclient.c's chttp_request_set_header
   * applies to an outgoing request's names, not a second character class
   * that could drift away from it. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  /* _send_response() puts name and value on the wire exactly as
   * "name:value\r\n", with no further escaping. When a caller reflects
   * request-controlled data into a response header (a query parameter, a
   * path parameter, an echoed request header), an embedded CR or LF lets
   * whoever controls that data inject any number of extra header lines or
   * split the response in two. That's classic HTTP response splitting, a.k.a.
   * CRLF injection.
   *
   * Every call that sets a response header comes through here, so we refuse
   * such a byte outright rather than leave each caller to clean its own
   * input. Every other control byte goes the same way: RFC 9110 SS5.5 allows
   * only VCHAR, SP, HTAB and obs-text in a field value, and a recipient that
   * follows it, this library's client included, refuses a response carrying
   * a NUL, a DEL or another control byte. */
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if ((*p < 0x20 && *p != '\t') || *p == 0x7F) return ccol_invalid_args;
  }
  /* Refuse "Transfer-Encoding" outright, just as chttpclient.c's
   * chttp_request_set_header does on the client side and for the same
   * reason: this server never transfer-codes a response body (see the
   * comment of _send_response: "this server never uses chunked
   * transfer-encoding for its own responses"), so there's no way to honor a
   * Transfer-Encoding header a caller sets.
   *
   * If it reached the wire it would also sit next to the automatically
   * computed Content-Length, which describes a body nothing transfer-coded,
   * and the framing would be ambiguous (RFC 7230 SS3.3.3): an intermediary
   * that prefers Transfer-Encoding over Content-Length gets the message
   * boundary wrong. That's exactly the response-splitting and desync hazard
   * that _send_response's handling of Connection and Content-Length keeps
   * out already. */
  if (ccol_ascii_casecmp(name, "transfer-encoding") == 0)
    return ccol_invalid_args;
  /* A Content-Length only reaches the wire on a response with no body (see
   * _send_response), and there it's the response's entire framing statement
   * for any cache or client reading it. A value that isn't a number would
   * make that statement malformed. */
  if (ccol_ascii_casecmp(name, "content-length") == 0 &&
      !_resp_content_length_valid(value))
    return ccol_invalid_args;
  return ccol_success;
}

/* Tells whether a response carries at most one field of this name, whichever
 * call sets it. _send_response decides the Connection field itself and reads
 * a handler's Content-Length and Date as single values. */
static bool _resp_header_is_singleton(const char *name) {
  return ccol_ascii_casecmp(name, "connection") == 0 ||
         ccol_ascii_casecmp(name, "content-length") == 0 ||
         ccol_ascii_casecmp(name, "date") == 0;
}

/* Appends one more field line. The caller has validated name and value. */
static ccol_retval_t _resp_header_append(chttpsvr_resp *resp, const char *name,
                                         const char *value) {
  ccol_memmgmt_procs_t *mp = resp->m_procs;
  if (resp->header_count >= resp->header_cap) {
    if (resp->header_cap > (SIZE_MAX - 8) / 2) return ccol_not_enough_memory;
    size_t new_cap = resp->header_cap * 2 + 8;
    if (new_cap > SIZE_MAX / sizeof(resp_header_t))
      return ccol_not_enough_memory;
    resp_header_t *nh = (resp_header_t *)_ccol_mem_realloc(
        mp, resp->headers, new_cap * sizeof(resp_header_t));
    if (!nh) return ccol_not_enough_memory;
    resp->headers = nh;
    resp->header_cap = new_cap;
  }

  char *n_name = ccol_strdup(mp, name);
  char *n_val = ccol_strdup(mp, value);
  if (!n_name || !n_val) {
    _ccol_mem_free(mp, n_name);
    _ccol_mem_free(mp, n_val);
    return ccol_not_enough_memory;
  }
  resp->headers[resp->header_count].name = n_name;
  resp->headers[resp->header_count].value = n_val;
  resp->header_count++;
  return ccol_success;
}

/* Makes value the single value of name. The first field of that name keeps
 * its place and the letter case of its name and every later field of that
 * name is dropped, so the other fields stay in the order of the calls that
 * set them. The caller has validated name and value. */
static ccol_retval_t _resp_header_replace(chttpsvr_resp *resp, const char *name,
                                          const char *value) {
  ccol_memmgmt_procs_t *mp = resp->m_procs;
  size_t first = resp->header_count;
  for (size_t i = 0; i < resp->header_count; i++) {
    if (ccol_ascii_casecmp(resp->headers[i].name, name) == 0) {
      first = i;
      break;
    }
  }
  if (first == resp->header_count)
    return _resp_header_append(resp, name, value);

  /* Copy first, so a failed allocation leaves every field untouched. */
  char *new_val = ccol_strdup(mp, value);
  if (!new_val) return ccol_not_enough_memory;
  _ccol_mem_free(mp, resp->headers[first].value);
  resp->headers[first].value = new_val;
  size_t kept = first + 1;
  for (size_t i = first + 1; i < resp->header_count; i++) {
    if (ccol_ascii_casecmp(resp->headers[i].name, name) == 0) {
      _ccol_mem_free(mp, resp->headers[i].name);
      _ccol_mem_free(mp, resp->headers[i].value);
      continue;
    }
    resp->headers[kept++] = resp->headers[i];
  }
  resp->header_count = kept;
  return ccol_success;
}

ccol_retval_t chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                       const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_retval_t rv = _resp_header_validate(name, value);
  if (rv != ccol_success) return rv;
  return _resp_header_replace(resp, name, value);
}

ccol_retval_t chttpsvr_resp_add_header(chttpsvr_resp *resp, const char *name,
                                       const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_retval_t rv = _resp_header_validate(name, value);
  if (rv != ccol_success) return rv;
  if (_resp_header_is_singleton(name))
    return _resp_header_replace(resp, name, value);
  return _resp_header_append(resp, name, value);
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
    char *nb = (char *)_ccol_mem_realloc(mp, resp->body, new_cap);
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

  char *heap_buf = (char *)_ccol_mem_alloc(mp, (size_t)n + 1);
  if (!heap_buf) {
    va_end(ap2);
    return ccol_not_enough_memory;
  }
  int n2 = vsnprintf(heap_buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  if (n2 < 0) {
    _ccol_mem_free(mp, heap_buf);
    return ccol_invalid_args;
  }
  ccol_retval_t rv = chttpsvr_resp_write(resp, heap_buf, (size_t)n2);
  _ccol_mem_free(mp, heap_buf);
  return rv;
}

ccol_retval_t chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                       size_t len) {
  if (!resp || !json || len == 0) return ccol_invalid_args;
  ccol_retval_t rv = chttpsvr_resp_write(resp, json, len);
  if (rv != ccol_success) return rv;
  return chttpsvr_resp_set_header(resp, "content-type", "application/json");
}

#ifdef RUNNING_UNIT_TESTS
/* Runs this module's exit-time destructors right now, in a test's forked
 * child, so the test can then destroy a server the way an application
 * destructor running after them would. */
void _chttpsvr_run_exit_cleanup_for_tests(void) {
  _cleanup_chttpsvr_router_shells();
  _cleanup_chttpsvr_slot_table();
  _cleanup_engine_stop_watcher();
}

/* How many router shells the registry holds, and whether its array is
 * allocated. */
void _chttpsvr_router_shell_registry_state_for_tests(size_t *count,
                                                     bool *array) {
  _ccol_call_once(chttpsvr_router_shell_registry.once,
                  _chttpsvr_router_shell_registry_init_globals);
  _ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  *count = chttpsvr_router_shell_registry.count;
  *array = chttpsvr_router_shell_registry.shells != NULL;
  _ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* True once the slot table's arrays have been released. */
bool _chttpsvr_slot_table_released_for_tests(void) {
  _ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  _ccol_mutex_lock(chttpsvr_slot_table.mutex);
  bool released = chttpsvr_slot_table.slots == NULL;
  _ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return released;
}

/* True while a logger of chttpsvr_set_engine_logger() is installed. */
bool _chttpsvr_engine_user_log_set_for_tests(void) {
  _ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  _ccol_mutex_lock(srv_engine_bundler.mutex);
  bool set = srv_engine_bundler.user_log != CLOG_INVALID;
  _ccol_mutex_unlock(srv_engine_bundler.mutex);
  return set;
}
#endif /* RUNNING_UNIT_TESTS */
