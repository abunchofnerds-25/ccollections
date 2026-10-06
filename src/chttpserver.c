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
#include <internal/cdeadline.h>
#include <internal/chttp1_parser.h>
#include <internal/csock.h>
#include <internal/cstrutil.h>
#include <internal/ctls.h>
#include <limits.h>
#if defined(__linux__)
#include <linux/sockios.h> /* SIOCOUTQ */
#endif
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
#include <strings.h>
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
 * routes_lock held. Every write appends under the write-lock. The lock gives
 * the ordering guarantee, so the code needs no atomic operations. */
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
  /* The number of '/'-delimited segments in prefix. It is 0 for the root's
   * own "" placeholder and for the special "/" prefix. See the doc comment
   * of chttpsvr_subrouter about that "/" prefix edge case. The
   * _create_router function computes this value one time. The prefix string
   * that it reads has no trailing slash and no consecutive slashes. It
   * orders the sub-routers for _find_route, which also reads it to end the
   * group of routers that own a path, and it maintains max_prefix_seg_count
   * of struct chttpserver (see the comment of that field). _prefix_matches
   * never reads it. That function walks router->prefix itself. Prefix
   * strings are short and the operator controls them, so a cache on that
   * side has no value. Only the percent-decode of the request path is worth
   * a share, because every router can repeat it. max_prefix_seg_count is
   * what makes that share possible. */
  int prefix_seg_count;
  chttpsvr_mw_node_t *mw_head;
  chttpsvr_mw_node_t *mw_tail;
  int mw_count;              /* the count of registered middleware; the cap is
                              * _CHTTPSVR_MAX_MW */
  chttpsvr_route_t **routes; /* pointer array; each element is a stable
                              * allocation */
  size_t route_count;
  size_t route_cap;
  /* The running maximum of every route->seg_count that this router holds.
   * The code only adds routes and never removes one, so this running maximum
   * stays correct and never needs a recompute. It bounds how many raw
   * segments of an incoming sub_path the per-router decode cache of
   * _find_route (_seg_cache_t) must split and decode. This bound holds even
   * when the request path, which the client controls, has many more
   * segments. See the doc comment of _seg_cache_build for why this bound is
   * important. */
  int max_route_seg_count;
  struct chttpserver *srv;       /* Back-pointer. Do not read it directly
                                  * from chttpsvr_router_on, _on_stream or
                                  * _use. See the comment of owner. */
  ccol_memmgmt_procs_t *m_procs; /* The allocator of the server. It serves
                                  * only the CONTENTS that this router owns:
                                  * the prefix, the mw list, the routes
                                  * array and the route data. The library
                                  * does not allocate the chttpsvr_router
                                  * struct itself (this shell) through this
                                  * allocator. This is deliberate. See
                                  * _create_router. */
  /* The chttpsvr handle that owns this router. It is CHTTPSVR_INVALID in
   * only two cases. The first is a short window for the root router.
   * _create_router runs before the library mints a handle.
   * ccol_create_chttpsvr_mp then stores the fresh handle here. The second
   * is any router, the root included, whose owning server is already
   * destroyed (see _destroy_router). The library never gives the root router
   * to a caller as a chttpsvr_router*. This is why chttpsvr_router_on,
   * _on_stream and _use never resolve this field for the root. The library
   * sets the field for the root anyway, for one reason only. The exit-time
   * destructor of the shell registry below must tell a live root apart from
   * a destroyed one. It must do this for the root exactly as it does it for
   * every sub-router. The field is _Atomic because _destroy_router writes it
   * without any lock that a concurrent reader also holds. No such lock
   * exists, and the comment below says why. A plain field here is a real
   * data race, not a theoretical one.
   *
   * chttpsvr_router_on, _on_stream and _use resolve this handle before they
   * touch srv, routes or mw_head. They do this in the same way as every
   * other public mutator resolves its chttpsvr handle. The resolve alone is
   * NOT enough. This is why _destroy_router never frees the memory of this
   * struct (the "shell"). The shell is this field plus prefix, srv, m_procs,
   * routes, route_count, route_cap, mw_head, mw_tail and mw_count. The shell
   * is everything BUT the routes, the mw nodes and the prefix that it points
   * to. Every other per-request and per-route allocation in this file does
   * go back to the allocator. The reason for the difference is this. A read
   * of this very field must dereference `router` itself. That dereference
   * happens BEFORE any resolve or pin can run. If the memory of the shell
   * already went back to the allocator, that read is itself a
   * use-after-free. It makes no difference whether the free runs
   * concurrently, is in progress, or completed in full in an earlier
   * destroy. No lock taken AFTER the read can prevent it, and valgrind
   * reports it as a real use-after-free. A resolve of this field does not
   * close the hole, and neither does an rwlock around the free in
   * _destroy_router. Such a lock only narrows the window. It does nothing
   * for a reader that starts after the free of a destroy is already
   * complete. The shell stays allocated for the rest of the process (see the
   * registry below), which makes a read of this field always memory-safe.
   * The atomic load then observes one of two values. The first is a live
   * handle that the library can resolve. The second is the CHTTPSVR_INVALID
   * sentinel, which _destroy_router stores after the owning server is gone.
   * The ordinary resolve and pin protect every field access after that. The
   * _destroy_router function waits for pending_resolve_count before it frees
   * the CONTENTS of this router. That wait serializes correctly against
   * them. */
  _Atomic chttpsvr owner;
};

/** One response header entry stored in the flat headers array. */
typedef struct {
  char *name;
  char *value;
} resp_header_t;

/** The response accumulator for each request.
 *
 * The library keeps the response headers in a flat dynamic array, not in a
 * linked list. This makes the scan for duplicates in
 * chttpsvr_resp_set_header cache-friendly. It also needs no node allocation
 * for each header. */
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

/* The maximum count of middleware steps for each request. The count is the
 * global steps plus the router steps. If a request goes past this limit, the
 * server responds with 500. */
#define _CHTTPSVR_MAX_MW 32

/* The Retry-After value, in seconds, of every 503 that the server sends
 * itself: a full worker pool, a full streaming queue, a streaming request
 * that waited past streaming_queue_timeout_ms, and a request that waited past
 * body_memory_wait_timeout_ms. Each of those means that the server is
 * saturated. Five seconds is several ticks of the sweep that admits waiting
 * requests and expires them, long enough that a client which honours it does
 * not come straight back into the same saturation, and short enough to cost
 * a well-behaved client little. */
#define _CHTTPSVR_RETRY_AFTER_SECONDS "5"

/* The defaults of the slow-client settings of chttpsvr_config_t; see the doc
 * comment of each field in chttpserver.h. 240 bytes a second after a grace
 * of five seconds is the minimum data rate of Kestrel and of IIS. */
#define _CHTTPSVR_DEFAULT_MIN_RATE_BPS 240u
#define _CHTTPSVR_DEFAULT_RATE_GRACE_MS 5000u
#define _CHTTPSVR_DEFAULT_PARTIAL_BODY_MEMORY ((size_t)256 * 1024 * 1024)
#define _CHTTPSVR_DEFAULT_MEMORY_WAIT_MS 30000u
#define _CHTTPSVR_DEFAULT_STREAM_QUEUE_TIMEOUT_MS 5000u

/* The unconditional cap, in ms, on the *total* wall-clock time of one small
 * internal write that has a fixed shape. The library generates these writes
 * itself. There are two of them: the send of a courtesy rejection response
 * (see _conn_reject_and_close), and the "100 Continue" interim line (see
 * _write_interim_continue). This cap applies whatever value the operator
 * configures for conn->srv->max_response_write_duration_ms. See the comment
 * of _response_write_deadline_ok for how the two values combine.
 * Neither write waits on the peer: a full socket parks the connection with
 * the unsent rest, and the reactor resumes it once the socket takes bytes
 * again. A peer that takes one or two bytes at a time can still keep such a
 * write going for as long as it likes, holding a descriptor and the parked
 * state. This is the "trickle forever" pattern that
 * max_response_write_duration_ms closes for a real response from a handler.
 * But that knob can be 0, which turns it off. An
 * operator who leaves it off almost certainly means that for the response
 * bodies of their own handlers. The operator does not mean it for the small
 * internal writes of this library, which have a fixed shape. A rejection has
 * no body and a few header bytes. The interim continue is a fixed 25-byte
 * status line. Neither has a legitimate reason to need more than a couple of
 * seconds to send, even under real load. An unconditional bound on these
 * writes closes a real denial-of-service surface during ordinary operation,
 * outside of shutdown. An "Expect: 100-continue" request to any route that
 * accepts a body reaches the interim line, and this is standard client
 * behavior; for example, curl does it by default for large uploads. The
 * sweep and the resumed write both judge the deadline, so a parked write
 * ends at it. The interim line of a streaming route is written by the
 * handler thread itself, inside chttpsvr_req_read(), which waits for the
 * client there as it waits for the body; the bound is what limits that
 * wait. 2 seconds is generous for any real network path, loopback or
 * otherwise, to deliver much less than a kilobyte of data. It is also short
 * enough to bound the worst case tightly. */
#define _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS 2000

#ifdef RUNNING_UNIT_TESTS
/* A test of what a parked rejection does, rather than of how long it may
 * last, raises the ceiling with this, so that a client that reads a large
 * rejection slowly (under valgrind, through a small receive window) still
 * gets all of it. 0 restores _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. */
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

/* The count of worker threads of reject_pool is not a fixed constant. It is
 * the larger of _CHTTPSVR_REJECT_POOL_MIN_THREADS and half of the resolved
 * thread count of worker_pool. chttpsvr_start computes it once. See the
 * comment of that function at the place of the computation. The library
 * routes every rejection response here, not only the 503 case for a full
 * pool (see _conn_reject_via_pool). This is why the size is that of a small
 * dedicated pool, and not one thread that is "good enough for an overload
 * corner case". A burst of requests for unmatched routes is a very common,
 * everyday shape, unlike a worker_pool that stays full. Such a burst needs
 * enough concurrency of its own to drain quickly, and must not serialize
 * behind one thread. The size follows the size of worker_pool instead of a
 * fixed number. This lets reject_pool grow with a large deployment that has
 * many cores. Without it, reject_pool stays at one constant that fits a much
 * smaller default configuration. The floor stops a server with one worker,
 * or a few workers, from a collapse back to a bottleneck of one thread. */
#define _CHTTPSVR_REJECT_POOL_MIN_THREADS 2

/* The capacity of the task queue of reject_pool. It is bounded, and not
 * unbounded in the style of CHTTPSVR_QUEUE_UNBOUNDED (0). Without the bound,
 * a flood of rejected connections that does not stop grows the backlog of
 * reject_pool without limit. That backlog is memory of this process: one
 * queued chttpsvr_conn_t for each reject-close that waits. Once the queue is
 * full, ctpool_try_submit fails. _conn_reject_via_pool then falls back to a
 * synchronous close on the calling thread. This is the same fallback that
 * runs when reject_pool is not available at all, for example during a
 * shutdown. The size is generous because every rejection path feeds this one
 * queue, not only the 503 case for a full pool. */
#define _CHTTPSVR_REJECT_POOL_QUEUE_CAP 1024

/* Snapshot entry for one middleware step. */
typedef struct {
  chttpsvr_middleware_fn fn;
  void *ctx;
} _mw_entry_t;

/** The dispatch state of the middleware. It sits inside chttpsvr_conn_t
 * itself. That struct is on the heap and stays alive for the whole life of a
 * keep-alive connection. It is not on the stack and it is not per request.
 * The library resets this state for each request, in the same way as it
 * resets the other per-request scratch fields of that struct. */
typedef struct dispatch_ctx {
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  _mw_entry_t
      mw_snap[_CHTTPSVR_MAX_MW]; /* fn and ctx pairs, from a snapshot that
                                  * the library takes under the lock */
  int mw_count;                  /* the count of elements in the snapshot */
  int mw_idx;                    /* the next element to dispatch */
} dispatch_ctx_t;

/* A small growable buffer. It has two uses. A buffered route uses it to
 * accumulate the whole body. A streaming route uses it to hold the body
 * bytes that wait, because chttpsvr_req_read did not yet take them. See the
 * comment of _on_body. */
typedef struct {
  char *buf;
  size_t cap;
  size_t len; /* the count of valid bytes that the buffer holds now */
  size_t pos; /* for a streaming route only: how much of [0,len)
               * chttpsvr_req_read already gave to the caller */
} growbuf_t;

typedef enum {
  CONN_ST_TLS_HANDSHAKE,
  CONN_ST_READING_HEADERS,
  CONN_ST_DIVERTED, /* A worker thread owns the fd. The reactor
                     * registration, if there is one, is paused with
                     * ccol_event_loop_pause. The library does not remove
                     * it. No events fire, but the registration stays alive.
                     * ccol_event_loop_resume brings it back cheaply for the
                     * next request. The library never builds it again from
                     * the start. */
  CONN_ST_CLOSING
} conn_state_t;

/** The object for each connection. It stays alive across every keep-alive
 * request on this connection. _conn_reset_for_request() resets the
 * per-request scratch fields before each new request starts. Those fields
 * are the method, the path, the headers, the route, the dispatch state, the
 * response and the body buffers. The object is on the heap. Either the
 * ccol_event_reg of the reactor or a ctpool task of a worker owns it, and
 * never both at the same time. See the callback that reads the headers and
 * _task_worker for the two handover points. */
typedef struct chttpsvr_conn {
  int fd;
  struct chttpserver *srv;
  ccol_event_reg reg; /* the current registration; it is CCOL_EVENT_REG_INVALID
                       * while the connection is diverted */
  ctls_conn_t *tls;
  conn_state_t state;
  chttp1_parser_t parser;

  /* The scratch fields for each request. */
  chttp_method_t method;
  /* This field is owned and RAW, so it is still percent-encoded. The match
   * of a route must work on this exact form. _match_route_cached and
   * _prefix_matches split on a literal '/' and decode each segment on its
   * own. That is the only way to tell a real path separator apart from a
   * %2F encoding inside a {param} segment. A decode of the whole path here,
   * before the split, turns %2F into a real '/'. That silently splits one
   * param segment into two path segments. Such a decode must also reject the
   * whole request when any %XX anywhere in the path is malformed. The tests
   * of this module document a different contract. A malformed encoding in
   * one segment must come out as an ordinary route mismatch (404), and not
   * as a 400. The _seg_cache_get function further down already treats a
   * failed decode as "this segment does not match". */
  char *path;
  /* This field is owned and fully URL-decoded. _on_headers_complete() fills
   * it once a route matches. The decode must succeed at that point, because
   * the match already proved that every segment decodes cleanly. This is the
   * value that chttpsvr_req_path() returns. */
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
  /* The allowed_methods of the route match, for the Allow header of a 405.
   * _on_headers_complete sets it together with reject_status 405, and only
   * _conn_reject_and_close reads it, for that status alone. */
  uint8_t allowed_methods;
  /* True for "OPTIONS *", the asterisk-form request of RFC 9112 SS3.2.4.
   * The server answers it itself, with no route, no middleware and no
   * handler; see _options_star_route. */
  bool options_star;
  bool expects_continue;
  /* The library sets this flag once it writes the "HTTP/1.1 100
   * Continue\r\n\r\n" interim response for this request. The two send sites
   * are _task_worker for a buffered route and chttpsvr_req_read for a
   * streaming route. The flag stops a streaming handler that calls
   * chttpsvr_req_read() more than once from a second write of the interim
   * line. */
  bool interim_continue_sent;
  /* The library sets this flag when _write_interim_continue() writes less
   * than the full "HTTP/1.1 100 Continue\r\n\r\n" line. This happens when
   * the write fails, when it times out, or when it reaches the deadline of
   * max_response_write_duration_ms. It is a real short write, not a write of
   * nothing. The bytes that the write did put on the wire are there forever.
   * Any further write on this connection therefore lands after a truncated
   * status line, which corrupts the framing of the client. If the peer is
   * already gone, such a write has no value either. _task_worker reads this
   * flag. It then suppresses the real final response send and closes the
   * connection. This stops the library from adding more bytes to a stream
   * that the client may already see as malformed. */
  bool interim_write_failed;
  growbuf_t body;         /* the whole body for a buffered route, or the
                           * bytes that wait for a streaming route */
  size_t body_bytes_seen; /* the running total, to enforce max_body_size */
  bool body_too_large;
  /* Only _on_body sets this flag, and only when the growth of the body
   * buffer fails. That is a real failure of _ccol_mem_realloc while the
   * library accumulates body bytes. It is never a rejection by
   * max_body_size. The flag is separate from body_too_large and
   * transfer_aborted for one reason. chttpsvr_req_stream_error() of a
   * streaming route must report a real allocation failure on the server as
   * ccol_not_enough_memory. It must not report it as
   * ccol_http_transfer_aborted, which means "the connection closed, or the
   * framing is malformed". That is plainly not what happened. The check for
   * this flag has the same priority against transfer_aborted that
   * body_too_large already has: it comes before it, in
   * chttpsvr_req_stream_error(). In practice the two flags exclude each
   * other. The _on_body function returns 1 and aborts the parse the first
   * time it meets either condition. No later call in the same request can
   * therefore set the other flag. The order still has meaning if that ever
   * changes. */
  bool body_alloc_failed;
  /* The library sets this flag when chttpsvr_req_read() of a streaming route
   * meets a truncated body. That is an EOF from the peer, or a hard I/O
   * error before the message completes its framing. The library also sets it
   * for a CHTTP1_USER or CHTTP1_ERROR at the level of chttp1_parser that is
   * neither a max_body_size rejection nor a body_alloc_failed one. Those two
   * have their own flags with a higher priority; see the order of priority
   * in chttpsvr_req_stream_error(). Only chttpsvr_req_stream_error() reads
   * this flag. The path of a buffered route (_drain_body) has no such need.
   * It returns ccol_http_transfer_aborted as its own return value. It does
   * not use a flag that a caller reads back out of conn after the call. */
  bool transfer_aborted;
  /* True when the request BODY did not arrive as its own framing promised,
   * and the library therefore stopped reading it. Two things set it. The
   * first is a CHTTP1_ERROR from chttp1_parser while it reads the body: a
   * chunk size that is not hexadecimal, a chunk that no CRLF terminates, or
   * a malformed trailer line. The second is an EOF before the message
   * completed, which is a body shorter than its own Content-Length, or a
   * chunked body with no terminating zero-length chunk.
   *
   * Both are client errors, and RFC 7231 SS6.5.1 gives them 400. Without
   * this flag every one of them arrives at _task_worker as the same
   * ccol_http_transfer_aborted that a hard I/O error produces, and the
   * buffered-route path answers 500 Internal Server Error for all of them.
   * That tells an operator that the server broke when the client did, and it
   * counts a client fault into a 5xx rate.
   *
   * max_body_size and a failed allocation keep their own flags and their own
   * statuses, which are 413 and 500. This flag never competes with them:
   * _drain_body sets it only once it has ruled both of them out.
   *
   * The streaming-route path does not read this flag.
   * chttpsvr_req_stream_error() keeps reporting ccol_http_transfer_aborted
   * for every one of these cases, which is its documented value for "the
   * connection closed, or the framing is malformed". A streaming handler
   * chooses its own status, so the library must not change what that
   * accessor answers. */
  bool body_malformed;

  /* The bytes that remain in the read buffer of the reactor after the
   * header parse, past chttp1_parser_consumed(). The library copies them out
   * (see _conn_start_diverted). It must copy them. The original buffer is
   * stack memory of the reactor thread, and it does not live past the
   * callback that produced it. The _task_worker function frees these bytes
   * after chttp1_stream_prepare() or chttp1_stream_prepare_tls() copies them
   * into its own heap buffer. */
  char *_carry_over;
  size_t _carry_over_len;

  /* The bookkeeping for max_body_read_duration_ms. */
  struct timespec read_deadline;
  bool read_deadline_set;
  bool deadline_exceeded;

  /* The bookkeeping for max_response_write_duration_ms. See the conn and
   * is_reject parameters of _send_response, and see
   * _response_write_deadline_ok and _shrink_timeout_to_deadline. Three sends
   * read these fields. The first is the real response send for a matched
   * route in _task_worker. The second is the courtesy response send of a
   * rejection in _conn_reject_and_close. The third is the "100 Continue"
   * interim line in _write_interim_continue. The last two also get
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS as an unconditional ceiling, on
   * top of whatever max_response_write_duration_ms itself permits. */
  struct timespec write_deadline;
  bool write_deadline_set;

  /* The bookkeeping for the sweep of the idle timeout. srv->idle_mutex
   * guards the list fields. */
  struct timespec last_activity;
  struct chttpsvr_conn *idle_prev, *idle_next;
  bool in_idle_list;

  /* The bookkeeping for the registry of diverted connections. See the
   * comment of diverted_mutex, diverted_head and diverted_tail in struct
   * chttpserver. Every connection that a worker thread owns
   * (CONN_ST_DIVERTED) is linked in here. This lets the teardown find the
   * connection of a stuck worker and force a shutdown(2) on it.
   * srv->diverted_mutex guards the list fields, in the same pattern as the
   * idle list above. */
  struct chttpsvr_conn *diverted_prev, *diverted_next;
  bool in_diverted_list;

  /* This counter says when it is safe to free this struct. It follows the
   * refcount design of ccol_event_reg in cthreadcomm.c: 1 while the object
   * is registered, plus 1 for each callback that is in flight. It starts at
   * 1, which stands for "the application did not yet close this connection"
   * (see _conn_create). It gets +1 for every conn->reg registration that the
   * library creates for the connection. That is every call site of
   * ccol_event_loop_add below, on success. It gets -1 exactly once from
   * _conn_close, which is the "done with conn" decision of the application.
   * It also gets -1 exactly once for each on_removed that fires
   * (_conn_on_removed). cthreadcomm guarantees that on_removed fires exactly
   * once for each registration that went live. It fires asynchronously, and
   * only once no dispatch of that registration can still touch conn.
   * _conn_free runs only when this counter reaches 0.
   *
   * One on_removed is not enough on its own. A connection can go through
   * more than one registration episode in its life. For example, a pause
   * that fails (see _conn_pause_reg) removes the registration synchronously,
   * from the dispatch of that same registration or from the sweep, which
   * holds the claim of the connection. conn survives, and _conn_park_arm or
   * the keep-alive tail of _task_tail registers its descriptor again later
   * for a next wait, while the on_removed of the earlier registration can
   * still be pending. A free on the first on_removed alone therefore frees conn
   * while a later registration episode, or the close decision of the
   * application, is still open. The sum of every contribution, in any order, is
   * what makes the condition exact. That condition is: the application asked
   * for a close, AND every registration that the library created is proven safe
   * to free. A sum is the exact condition, not an approximation of it. */
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs_t *m_procs;

  /* The bookkeeping for max_header_read_duration_ms. These two members come
   * after every other member on purpose. The read and dispatch path of this
   * struct reads the members that sit ahead of them. An append leaves the
   * offset of each of those members exactly where it is.
   * header_phase_start is the instant when the reactor-owned phase of the
   * CURRENT request started. For a connection with TLS that is the first
   * step of the handshake. For any other connection it is the first byte of
   * the header block of that request. header_phase_active says whether the
   * field holds a meaningful instant. _conn_reset_for_request clears it.
   * The next request of a keep-alive connection therefore gets a budget from
   * its own first byte. It does not inherit the start of the request before
   * it, and the library does not charge it for the idle gap between the two.
   *
   * These are plain fields, not atomics. The library writes both only while
   * this connection is OUT of the idle list. A dispatch claims it out first,
   * and _idle_list_add publishes it again once the writes are done. Only the
   * idle sweep reads them, and that sweep holds srv->idle_mutex across its
   * whole walk. That mutex is what orders the two sides. This is the same
   * edge that last_activity just above already uses. */
  struct timespec header_phase_start;
  bool header_phase_active;

  /* The slow-client state. Everything below serves one rule: a worker thread
   * never waits on a client socket for a buffered route. These members come
   * last so that the offsets of every member that the read and dispatch path
   * reads stay where they are.
   *
   * divert_gate is written by _on_headers_complete for every matched request
   * and read by _conn_start_diverted. 0 means "divert as usual". SIZE_MAX
   * means "a streaming route that runs on the streaming pool". Any other
   * value is the Content-Length of a buffered body, which must reserve body
   * memory unless the whole body already arrived with the headers. One
   * compare against the length of the carry-over therefore decides the
   * ordinary case. */
  size_t divert_gate;

  /* Which wait the connection sits in while no thread owns it; one of the
   * _CONN_PARK_* values. The thread that parks a connection writes it before
   * it publishes the connection into the idle list, the memory-wait list or
   * the streaming queue, and the thread that claims the connection back out
   * of that list reads it. The lock of the list orders the two. park_pending
   * is the same kind of value, set by the body-read helper for the worker
   * that is about to park the connection. */
  uint8_t park;
  uint8_t park_pending;

  /* The rate floor of chttpsvr_config_t.min_transfer_rate_bps, and the
   * per-gap limit of a parked connection. rate_start is the start of the
   * current body read or response write, rate_paused_ms the time inside it
   * that the server itself held the request back, and rate_bytes the bytes
   * that moved since rate_start. last_progress is the last time that bytes
   * moved. rate_queued is the count of unread bytes that the sweep last saw
   * in the socket while the receive low-water mark was above 1. All of these
   * use the clock of _slow_clock. */
  bool rate_active;
  struct timespec rate_start;
  long long rate_paused_ms;
  uint64_t rate_bytes;
  struct timespec last_progress;
  size_t rate_queued;
  /* The SO_RCVLOWAT that the socket carries now. 0 and 1 both mean the
   * default of 1. */
  int rcvlowat;

  /* The partial-body memory of chttpsvr_config_t.max_partial_body_memory.
   * srv->wait_mutex guards every member of this group once the connection
   * has a charge or waits, and the owner of the connection is the only
   * other thread that reads them. mem_charged is the reservation that the
   * connection holds, from before its body is read until _conn_drop_body
   * frees the body; mem_exempt says that it went ahead past the limit under
   * the progress rule and may reserve freely while its body arrives, and it
   * ends with the reservation; mem_need is what a waiter asks for. */
  size_t mem_charged;
  size_t mem_need;
  bool mem_exempt;
  bool in_mem_wait;
  struct timespec mem_wait_start;
  struct chttpsvr_conn *mem_prev, *mem_next;

  /* The instant at which a parked connection left its wait and was handed
   * to the worker pool, on CLOCK_MONOTONIC and on _slow_clock. _task_resume
   * judges the limits of the park at this instant, and the time that the
   * connection then spends in the queue of the pool is the server's; see
   * _conn_uncharge_queue_wait. The thread that submits the task writes both,
   * and the submit orders them before the read of the worker. */
  struct timespec claim_mono;
  struct timespec claim_slow;

  /* The streaming queue. srv->wait_mutex guards the links and the flag. */
  bool in_stream_queue;
  bool sq_has_deadline;
  struct timespec sq_deadline;
  struct chttpsvr_conn *sq_prev, *sq_next;

  /* A response whose write stopped on a full socket. wp_head holds the
   * unsent part of the header block, which the response builder keeps on
   * its stack; wp_body_pos is how much of resp.body is already out. */
  bool wp_active;
  bool wp_keep_alive;
  bool wp_no_body;
  /* True when the TLS layer, and not the socket, stopped the write: a TLS
   * write can need to read first. The park then waits for readability. */
  bool wp_wants_read;
  /* True when the parked write is the courtesy response of a rejection,
   * which ends in a close or a lingering close and never in keep-alive; see
   * _conn_reject_and_close. */
  bool wp_reject;
  /* True when the parked write is the "100 Continue" interim line of a
   * buffered route, after which the body is read; see _task_resume. The
   * carry-over then holds body bytes that came with the headers. */
  bool wp_interim;
  char *wp_head;
  size_t wp_head_len;
  size_t wp_head_pos;
  size_t wp_body_pos;
  /* The smallest count of bytes that the kernel still held unsent or
   * unacknowledged in the send queue (SIOCOUTQ) of a parked response, as the
   * park and the sweep saw it, or SIZE_MAX when the kernel does not report
   * one. A queue that shrinks below it is progress of the reader; see
   * _parked_expired. */
  size_t wp_outq;

  /* The parked list of the server; see parked_head in struct chttpserver.
   * srv->idle_mutex guards these fields, as it guards the idle list. */
  struct chttpsvr_conn *parked_prev, *parked_next;
  bool in_parked_list;

  /* A lingering close; see _conn_linger. linger_deadline is on
   * CLOCK_MONOTONIC. */
  struct timespec linger_deadline;
} chttpsvr_conn_t;

/* The waits of a connection that no thread owns; see chttpsvr_conn_t.park.
 * _CONN_PARK_BODY, _CONN_PARK_WRITE and _CONN_PARK_LINGER sit in the parked
 * list, with the reactor registration armed for the direction that the socket
 * must reach. The two
 * memory waits sit in the memory-wait list and the streaming wait sits in the
 * streaming queue; all three keep the registration paused. */
enum {
  _CONN_PARK_NONE = 0,
  _CONN_PARK_BODY = 1,      /* the body of a buffered route stopped arriving */
  _CONN_PARK_WRITE = 2,     /* the response stopped on a full socket */
  _CONN_PARK_MEM_START = 3, /* a Content-Length body waits to be admitted */
  _CONN_PARK_MEM_BODY = 4,  /* a chunked body waits for more memory */
  _CONN_PARK_STREAM = 5,    /* a streaming request waits for a thread */
  _CONN_PARK_LINGER = 6     /* a closing connection discards what arrives */
};

/** The object for each request that the library gives to a handler or to a
 * middleware. It is a thin view over a chttpsvr_conn_t plus the
 * chttp1_stream_t of the worker. That stream cannot live on chttpsvr_conn_t
 * itself. It exists, and is valid, only while one worker drives the body of
 * one request. */
struct chttpsvr_req {
  chttpsvr_conn_t *conn;
  chttp1_stream_t *stream;  /* local to the worker; NULL until the worker
                             * sets it up */
  const char **param_names; /* borrowed from the route (const aliases) */
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

/* chttpsvr is an opaque value handle. The top 32 bits are the slot index and
 * the bottom 32 bits are the generation. See the doc comment on the typedef
 * in include/chttpserver.h. The library resolves the handle through this
 * table before it touches the struct chttpserver* behind it. This is what
 * lets __chttpsvr_destroy report a ccol_fatal_err for two cases, instead of
 * a use-after-free or a double free. The first case is a concurrent double
 * destroy, which races another destroy on the same live handle. The second
 * case is a sequential one, which is a stale handle from an earlier destroy
 * that already finished. The library marks a slot not-in-use the instant it
 * frees it, and it raises the generation on every reuse. A stale handle can
 * therefore never alias a later, unrelated server in the same slot index.
 * This table follows the chttpcli_slot_table mechanism of chttpclient.c
 * exactly. See the section-level comment of that file for the full design
 * rationale. It covers the alternatives that the design rules out and the
 * resolve-then-use race that it closes. This comment does not repeat it. */
typedef struct {
  struct chttpserver *ptr; /* NULL when the slot is free */
  uint32_t generation;     /* fresh on every acquire; monotonic for each
                              slot index; starts at 0 before the first use
                              and becomes 1 on the first acquire */
  bool in_use;
} chttpsvr_slot_t;

static struct {
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
  cvec slots;        /* a cvec of chttpsvr_slot_t; it grows only through
                        push_back, and an index is permanent once the
                        library allocates it */
  cvec free_indices; /* a cvec of uint32_t; a LIFO free list that gives
                        O(1) reuse */
} chttpsvr_slot_table = {0};

/* True after a thread enters _chttpsvr_slot_table_init_globals. Each path
 * that can create or resolve a server enters it first. The exit-time
 * destructor reads this flag. It tells a process that never touched this
 * module from a process that did. The initialization does more than its
 * own work: it also makes the slot tables of clogger and cthreadpool and
 * registers their fork handlers. At exit, those modules can have run their
 * own destructors before this one. */
static atomic_bool chttpsvr_slot_table_entered = false;

#if CCOL_FORK_SAFETY_REQUIRED
/* Forward declarations. The bodies come further below, once the file
 * declares struct chttpserver, srv_engine_bundler, servers_bundler and
 * chttpsvr_router_shell_registry. The handlers dereference and lock all of
 * them. _chttpsvr_slot_table_init_globals below registers the handlers. That
 * function is the first point in the file that runs once, lazily, the first
 * time anything uses this module. This placement follows the same
 * forward-declare-then-define-after-the-struct pattern as cthreadpool.c and
 * cthreadcomm.c.
 *
 * A build turns off this whole fork() safety mechanism when it defines
 * CCOL_FORK_SAFETY_REQUIRED to 0. The mechanism is these three handlers and
 * their ccol_at_fork() registration below. See the doc comment of that macro
 * in common.h. */
static void _chttpsvr_atfork_prepare(void);
static void _chttpsvr_atfork_release(void);
static void _chttpsvr_atfork_child_release(void);

/* cthreadcomm.h, cthreadpool.h and clogger.h do not declare these three
 * functions, because they are not part of the public API of those modules.
 * They are narrow, deliberate escape hatches. They let a module that depends
 * on cthreadcomm, cthreadpool or clogger force the ccol_at_fork()
 * registration of that module to happen before its own. See the call site in
 * _chttpsvr_slot_table_init_globals below. See also the doc comment of each
 * function in its home file. That comment gives the full reason and the real
 * AB-BA deadlock shape that this closes. */
void _cthreadcomm_ensure_atfork_registered_before_caller(void);
void _ctpool_ensure_atfork_registered_before_caller(void);
void _clog_ensure_atfork_registered_before_caller(void);
#endif

static void _chttpsvr_slot_table_init_globals(void) {
  atomic_store(&chttpsvr_slot_table_entered, true);
  if (ccol_mutex_init(chttpsvr_slot_table.mutex) != 0)
    ccol_fatal_err("chttpsvr slot table: failed to initialize mutex");
  chttpsvr_slot_table.slots = cvector_create(sizeof(chttpsvr_slot_t), NULL);
  if (!chttpsvr_slot_table.slots)
    ccol_fatal_err("chttpsvr slot table: failed to allocate slots vector");
  chttpsvr_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpsvr_slot_table.free_indices)
    ccol_fatal_err("chttpsvr slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  /* fork() duplicates only the calling thread. See the doc comment of
   * _chttpsvr_atfork_prepare for the full hazard that this closes. In short,
   * the child can inherit a lock that some other thread held. That thread is
   * gone in the child, so the lock stays locked forever.
   *
   * These three calls force the ccol_at_fork() triples of cthreadcomm (which
   * owns ccol_event_loop), cthreadpool and clogger to register NOW. They
   * must register strictly before the ccol_at_fork() call of this module
   * below. The prepare handlers of pthread_atfork run in REVERSE order of
   * registration. This order therefore guarantees that
   * _chttpsvr_atfork_prepare always runs FIRST at every future fork(). It
   * locks the locks of this module first: srv_engine_bundler.mutex,
   * servers_bundler.mutex, the mutex of every live server, and so on. Only
   * then can the prepare handlers of ccol_event_loop, ctpool or clogger take
   * their own locks. Those locks are ccol_event_loop_slot_table.mutex,
   * ctpool_slot_table.mutex, clog_slot_table.rwlock, and an internal lock of
   * a live ccol_event_loop, a live pool or a live clog_shared_t.
   *
   * This is the same order that every ordinary call in this file already
   * uses. _engine_acquire holds srv_engine_bundler.mutex for its whole body.
   * That body includes its nested call into
   * ccol_event_loop_create_with_mprocs(). It also includes its own direct
   * call to clog_open_fd_mp() when no engine-wide logger exists yet. The
   * _SRV_ENGINE_LOG macro holds the same mutex around every ccol_log_info,
   * ccol_log_warn and other such call. It makes those calls through a logger
   * that is already open.
   *
   * Without these three calls, the real lock order at fork() time comes from
   * the subsystem that the embedding application uses FIRST. That is pure
   * accident, and it can end up reversed against this nesting. For the pair
   * of ccol_event_loop and ctpool it gives two real lock-order-inversion
   * cycles. The first is srv_engine_bundler.mutex against
   * ccol_event_loop_slot_table.mutex. The second is a three-way cycle that
   * also has ctpool_slot_table.mutex in it. ThreadSanitizer reports both,
   * through the test_engine_stop_tsan target of tests/chttpserver.
   *
   * The pair with clogger has the same hazard shape, and it is reachable
   * without ThreadSanitizer. Every public chttpsvr_* function that resolves
   * a handle tolerates an invalid handle. Those functions are
   * chttpsvr_start, _stop, _use, _register_handler,
   * _register_streaming_handler and _subrouter. They report an invalid
   * handle as a plain error. They do not demand that the caller prove first
   * that the handle is valid. But each one still runs the ccol_call_once of
   * this module, through _chttpsvr_resolve, before it checks the handle.
   * Take a process that makes such a call before it uses clog for anything
   * else. Without the force below, that process registers the ccol_at_fork()
   * of this module with no clogger registration ahead of it. A later,
   * independent first use of clog then registers AFTER this module, which
   * inverts the order that the design needs. The doc comment of
   * queue_mutex_registry in cthreadcomm.c describes the same class of hazard
   * for a different pair of subsystems. It also describes the same remedy:
   * force a fixed order of registration instead of a choice by chance.
   *
   * A call to all three of these, every time this function runs, does no
   * harm. Each one has its own ccol_call_once guard. A caller can already
   * have forced one or more of them, through its own unrelated use of
   * ccol_event_loop, ctpool or clog. Such a call finds the handler
   * registered and returns at once. */
  _cthreadcomm_ensure_atfork_registered_before_caller();
  _ctpool_ensure_atfork_registered_before_caller();
  _clog_ensure_atfork_registered_before_caller();
  ccol_at_fork(_chttpsvr_atfork_prepare, _chttpsvr_atfork_release,
               _chttpsvr_atfork_child_release);
#endif
}

/* The lifecycle of a chttpsvr handle has two independent dimensions. It is
 * not one linear state machine. The first dimension is start and stop. The
 * second is a one-shot quiesce latch. A caller can set the CLAIM of that
 * latch at once, and that claim is independent of the start and stop
 * dimension. A separate mechanism (`pending_resolve_count`) then defers the
 * real work. There is one enum for each dimension. Two enums keep the
 * semantics of an independent claim. They also give `-Wswitch` coverage for
 * each dimension. `-Wswitch` is part of the standing `-Wall` of this
 * project, together with `-Werror`. A future addition to either enum that a
 * call site does not handle therefore fails the build instead of a silent
 * compile. Every enumerator below has an explicit value. This matches the
 * rule for `ccol_retval_t` in common.h, for the same reason: never depend on
 * the auto-increment of the declaration order. */
typedef enum {
  CHTTPSVR_LC_IDLE = 0,     /* not listening; chttpsvr_start() may go on */
  CHTTPSVR_LC_STARTING = 1, /* the real work of chttpsvr_start() is in
                             * flight */
  CHTTPSVR_LC_RUNNING = 2,  /* it listens and serves */
  CHTTPSVR_LC_STOPPING = 3, /* the real work of _chttpsvr_stop_internal() is
                             * in flight, on its way back to IDLE */
} chttpsvr_lifecycle_t;

typedef enum {
  CHTTPSVR_QS_NOT_QUIESCED = 0, /* _quiesce_server_once() may claim and
                                 * run */
  CHTTPSVR_QS_QUIESCING = 1,    /* its real teardown work is in flight */
  CHTTPSVR_QS_QUIESCED = 2,     /* the teardown work is complete; this is an
                                 * idempotent latch. Only a later,
                                 * legitimate chttpsvr_start() resets it to
                                 * NOT_QUIESCED. */
} chttpsvr_quiesce_state_t;

/** The main struct of the server. */
/* What a unix:// listener needs to remove its own socket file, and nothing
 * else, when it stops. path is the owned copy of the path from the
 * configuration. dir_fd is the directory that holds the socket, opened when
 * the server bound it, and base the name of the socket inside it: every
 * later lookup goes through dir_fd, so a chdir() of the application after
 * the start cannot point the cleanup at another file. dev and ino identify
 * the socket that this server bound; the cleanup unlinks the name only while
 * it still names that socket, so a file that somebody else put at the path
 * since, including the socket of another server, survives. */
typedef struct {
  char *path;
  const char *base;
  int dir_fd;
  dev_t dev;
  ino_t ino;
} _chttpsvr_unix_bind_t;

struct chttpserver {
  /* Element [0] is the root. Elements [1..n] are the sub-routers. The
   * library keeps them sorted by prefix_seg_count, from the largest to the
   * smallest. See the sorted insert in chttpsvr_subrouter and the walk order
   * in _find_route. */
  chttpsvr_router **routers;
  /* routers[0], copied once at create time. The root router shell never
   * moves and never goes away while the server exists, so this field is
   * written once before the handle is published and read without
   * routes_lock. A read of routers[0] itself needs the lock, because
   * chttpsvr_subrouter can realloc the routers array under the write
   * lock. */
  chttpsvr_router *root_router;
  size_t router_count;
  size_t router_cap;
  /* The running maximum of the prefix_seg_count of every non-root router on
   * this server. The library only adds routers and never removes one, so
   * this running maximum stays correct and never needs a recompute.
   * routes_lock guards it, exactly as it guards router_count, router_cap and
   * routers itself. chttpsvr_subrouter updates it, when the new value is
   * higher, beside raw->routers[raw->router_count++] = r. Both writes sit in
   * the same critical section of the write-lock. This field lets _find_route
   * build ONE shared decode cache with a memo for the leading segments of
   * the request path. That cache follows the per-router match cache of
   * _seg_cache_t. _find_route reuses it across the _prefix_matches call of
   * every non-root router. Without it, each router percent-decodes the same
   * segments of the request path again from the start. See the comment of
   * _find_route at its call site. The split of the cache is bounded by this
   * value, which the operator controls. It is not bounded by the real
   * segment count of the request path, which the client fully controls. The
   * reasoning here is the same as for max_route_seg_count. */
  int max_prefix_seg_count;
  ctpool worker_pool;
  /* A pool of dedicated threads with a bounded queue. The thread count is
   * the larger of _CHTTPSVR_REJECT_POOL_MIN_THREADS and half of the resolved
   * thread count of worker_pool. See the comment of that constant. The queue
   * capacity is _CHTTPSVR_REJECT_POOL_QUEUE_CAP. Every _conn_reject_and_close
   * call runs here. That call is the courtesy write-then-close that a
   * rejected connection gets, and the middleware that may run before it.
   * The write never waits on the peer; a full socket parks the connection.
   * Two cases reach this pool. The
   * first is a 404, 405 or 500 for an unmatched or malformed route, which
   * the header parse finds synchronously. The second is a 503, because
   * worker_pool is already full. This pool is deliberately NOT worker_pool
   * itself, and that holds for the 503 case too. The purpose is to keep the
   * middleware of a rejection off the reactor thread. That work must also not
   * wait on, or compete with, the same pool that just rejected the request
   * because it was at capacity. The queue is
   * bounded and not unbounded. Without the bound, a flood of rejections that
   * does not stop grows the backlog of this pool without limit. Once the
   * queue is full, _conn_reject_via_pool falls back to a synchronous close
   * on the calling thread. See the comment of that function for why this
   * pool exists. */
  ctpool reject_pool;
  ctls_ctx_t *tls_ctx; /* not NULL when the caller configures TLS */
  ccol_mutex_t mutex;
  ccol_cond_var_t requests_done_cv;
  int in_flight_requests;
  /* _chttpsvr_resolve pins this counter with a lock-free atomic increment.
   * The pin lasts while a caller holds a struct chttpserver* that it just
   * resolved and did not yet hand off to its own tier-specific protection.
   * The public API of this module is short and synchronous:
   * chttpsvr_start, _stop, _register_handler, _register_streaming_handler,
   * _use and _subrouter. For those, the pin lasts while that one call runs,
   * because each of them holds its pin across its whole body.
   * _chttpsvr_resolve_unpin frees the pin. It does so under `mutex`,
   * together with the broadcast that wakes a destroy that waits. The
   * decrement itself must happen under the lock, and not only the broadcast.
   * See the comment of that function, and the identical
   * _chttpcli_resolve_unpin in chttpclient.c, for the reason. The
   * __chttpsvr_destroy function blocks until this counter reaches 0. Only
   * then does it free the object. This closes a real
   * resolve-then-use race. A naive resolve step that looks the handle up,
   * unlocks, and returns the pointer leaves that race open. This counter is
   * deliberately separate from in_flight_requests above, and it has its own
   * resolve_cv below instead of requests_done_cv. Every consumer of a
   * chttpsvr handle holds this pin for its whole synchronous duration. The
   * two counters therefore never need a wait together, unlike the
   * pending_resolve_count and in_flight_count pair in chttpclient.c.
   * chttpclient_do has requests that stay in flight for a long time. Nothing
   * here hands protection off to a separate mechanism that drains later, in
   * the middle of a call. */
  _Atomic size_t pending_resolve_count;
  /* Only _engine_force_stop_quiesce_all pins this counter. The pin lasts for
   * one bare struct chttpserver* that the function reads directly out of
   * servers_bundler.servers[]. See the doc comment of that function for why
   * the pointer needs protection at all. This counter is deliberately
   * SEPARATE from pending_resolve_count above, and it is not a second use of
   * it. The winner path of _quiesce_server_once waits for
   * pending_resolve_count to reach 0 before it does any of its real work. A
   * caller that held a pin on pending_resolve_count across its own call to
   * _quiesce_server_once would then wait on a pin that only it can free.
   * That is a real self-deadlock. _quiesce_server_once never waits on this
   * counter. Only __chttpsvr_destroy waits on it, right before it frees raw,
   * so no such cycle exists here. This counter broadcasts resolve_cv, the
   * same condition variable that the drain of pending_resolve_count already
   * uses. Both are conditions that __chttpsvr_destroy may wait on before it
   * is safe to free raw. A change in either one is a reason to check the
   * other again. */
  _Atomic size_t servers_bundler_pins;
  /* _listener_on_readable pins this counter for the whole duration of one
   * dispatch. That dispatch is its own accept4() loop. It also covers the
   * pause of the listener registration when a resource-exhaustion condition
   * does not go away; see _listener_pause_for_resource_pressure. The
   * function frees the pin right before that dispatch returns.
   *
   * The counter protects the listener fd, and not the memory of srv. The
   * memory of srv is safe for the whole dispatch through a different
   * mechanism: every listener registration holds one srv->lifetime_refs
   * reference, and its on_removed (_listener_on_removed) releases that
   * reference only once no dispatch of the registration can still run. See
   * the comment of that field. ccol_event_loop_remove() does not wait for a
   * dispatch that is already in progress, so without that reference a
   * destroy frees srv under such a dispatch.
   *
   * The fd needs this counter because _chttpsvr_stop_internal closes it
   * right after its removal of the listener, and a dispatch in progress can
   * still be about to call accept4() on it. _chttpsvr_stop_internal clears
   * listen_fd under srv->mutex before that removal, and the dispatch takes
   * this pin under the same mutex and then compares the fd of its own
   * registration against listen_fd. A dispatch that pins first holds the
   * close back until it returns. A dispatch that pins after the clear sees
   * a different listen_fd and returns without touching the fd or anything
   * else of srv.
   *
   * This counter is deliberately SEPARATE from pending_resolve_count, and it
   * is not a second use of it. The self-deadlock reason is the same one that
   * makes servers_bundler_pins above its own counter. chttpsvr_stop()
   * resolves srv and pins pending_resolve_count for its whole call,
   * including its call into _chttpsvr_stop_internal. A wait on
   * pending_resolve_count anywhere that call reaches would wait on a pin
   * that only that same call can free. But THIS counter is different from
   * pending_resolve_count and servers_bundler_pins. It is safe to wait on
   * from inside _chttpsvr_stop_internal, _quiesce_server_once or
   * chttpsvr_stop() itself, and those functions do wait on it. Only the
   * reactor thread that runs _listener_on_readable holds this pin. The
   * thread that calls chttpsvr_stop(), _quiesce_server_once or
   * __chttpsvr_destroy never holds it. There is therefore no case where the
   * thread that waits is also the thread that must run for this counter to
   * reach 0. A wait here cannot self-deadlock, the way a wait on the other
   * two can. The _chttpsvr_stop_internal function waits for this counter
   * immediately after its own ccol_event_loop_remove() call for the
   * listener. It waits strictly before it closes the listener fd. See the
   * doc comment of that function for the real fd-reuse race that this
   * closes. After ccol_event_loop_remove() runs, no NEW dispatch can start
   * again. The wait can therefore only be for a dispatch that is live at
   * that exact moment, never for a fresh one. There is at most one such
   * dispatch at a time. The per-registration dispatch_lock of
   * ccol_event_loop already guarantees that the callback of a registration
   * never runs concurrently with itself. The __chttpsvr_destroy function
   * also checks this counter, beside servers_bundler_pins, right before it
   * frees the contents of raw. That
   * check is provably redundant by that point; see the comment of that wait.
   * It stays as a cheap second check. This counter broadcasts resolve_cv,
   * the same condition variable that servers_bundler_pins and
   * pending_resolve_count already use, for the same reason. */
  _Atomic size_t listener_dispatch_pins;
  ccol_cond_var_t resolve_cv;
  ccol_rw_lock_t routes_lock;
  clog cl;

  /* These two fields are _Atomic, and not merely written under srv->mutex.
   * _listener_on_readable runs on the reactor thread. It reads both fields
   * on every accept() call, and it never takes srv->mutex. Plain int and
   * bool fields here are a real data race against the mutex-protected writes
   * of chttpsvr_start and chttpsvr_stop. ThreadSanitizer confirms that race.
   * chttpsvr_start also assigns both fields BEFORE it registers the listener
   * with ccol_event_loop_add, and not after. ccol_event_loop_add makes the
   * registration live at once. A connection can arrive in the window between
   * the registration and the write of these fields. Without the earlier
   * assignment, the library dispatches that connection to
   * _listener_on_readable while it still sees the default value of listen_fd
   * before the start, which is -1. */
  _Atomic int listen_fd; /* -1 when the server did not start */
  _Atomic bool is_unix_socket;
  /* The socket file of a unix:// listener; path is NULL for a TCP listener.
   * srv->mutex guards it. _listener_on_readable never reads it. Only
   * chttpsvr_start, chttpsvr_stop and _quiesce_server_once touch it, so it
   * does not need to be _Atomic. */
  _chttpsvr_unix_bind_t unix_bind;
  ccol_event_reg listen_reg;
  /* The order is CHTTPSVR_LC_IDLE -> CHTTPSVR_LC_STARTING ->
   * CHTTPSVR_LC_RUNNING -> CHTTPSVR_LC_STOPPING -> CHTTPSVR_LC_IDLE. Every
   * write and every read of this field happens under raw->mutex. The top of
   * the retry loop of chttpsvr_start() checks it with an exhaustive switch
   * that has no `default:` label. This is deliberate. `-Wswitch` works
   * together with the standing `-Werror` of this project. The pair then
   * forces the code to handle every future addition to chttpsvr_lifecycle_t
   * at every decision point that reads it. Without that, the new value
   * compiles silently and falls through to whatever the nearest case does.
   *
   * CHTTPSVR_LC_STARTING covers the whole duration of a chttpsvr_start()
   * call on this handle. It starts the moment the call passes the IDLE check
   * and the self-call check. It ends the instant the call returns, on
   * success or on failure. The library checks it beside
   * CHTTPSVR_LC_RUNNING. It does so in the same critical section of
   * raw->mutex. A second, concurrent chttpsvr_start() call on the same
   * handle would otherwise race past that section.
   * Without this state, two threads that call chttpsvr_start() on the same
   * IDLE handle at the same time both see IDLE. Both then go on in parallel
   * through the creation of the pools and through the unsynchronized read,
   * release and replace sequence on raw->tls_ctx below. The worker pool and
   * reject pool pair of one thread then go away silently. The OS threads
   * that this thread already spawned go away with them. The pair of the
   * other thread takes their place. There is a more serious result too. One
   * thread can call ctls_ctx_release() on a TLS context that the other
   * thread already published to raw->tls_ctx. A connection that the server
   * already accepted may run its handshake against that context, through
   * ctls_conn_create_server. That is a real use-after-free, and not merely a
   * leak. chttpsvr_start() resets the field to CHTTPSVR_LC_IDLE under
   * raw->mutex at every failure return point. On success it advances the
   * field to CHTTPSVR_LC_RUNNING.
   *
   * CHTTPSVR_LC_STOPPING covers the whole duration of the REAL teardown work
   * of a _chttpsvr_stop_internal() call on this handle. It starts the moment
   * that call confirms that the server was RUNNING, in the same critical
   * section that leaves CHTTPSVR_LC_RUNNING. It ends once the
   * ccol_event_loop_remove(), close() and unlink() sequence of that call is
   * complete. The retry loop of chttpsvr_start() checks it.
   *
   * CHTTPSVR_LC_STOPPING exists because chttpsvr_stop() leaves
   * CHTTPSVR_LC_RUNNING BEFORE its blocking ccol_event_loop_remove() call
   * for the OLD listener registration returns, and not after.
   * chttpsvr_destroy() and chttpsvr_engine_stop() differ here, because both
   * go through _quiesce_server_once and its quiesce_state interlock below.
   * Without this state, a chttpsvr_start() call can race a concurrent
   * chttpsvr_stop() call on the same handle from another thread. That start
   * call can see lifecycle == IDLE and quiesce_state == NOT_QUIESCED at the
   * same time, because chttpsvr_stop() never touches quiesce_state. It then
   * goes straight into _make_listen_socket() and bind() for a NEW listener
   * on the same host and port. The fd of the OLD listener is still open at
   * that point, because the stopping thread did not yet run
   * ccol_event_loop_remove() and close(). That is a real race. It is narrow
   * and it fails gracefully, with a spurious ccol_unexpected_failure from a
   * concurrent EADDRINUSE. This state is the guard against it, and it
   * matches the equivalent guard between chttpsvr_engine_stop() and
   * chttpsvr_start().
   *
   * _chttpsvr_atfork_release_impl resets this field to CHTTPSVR_LC_IDLE
   * unconditionally in a fresh child after a fork(). It does so for every
   * live server whose lifecycle was CHTTPSVR_LC_STARTING or
   * CHTTPSVR_LC_STOPPING at the instant of the fork(). See the doc comment
   * of that function for why the reset is safe here. quiesce_state just
   * below is different, because its own reset must tell apart the state that
   * a server was in when the fork() caught it. A server that was already
   * CHTTPSVR_LC_RUNNING, or already CHTTPSVR_LC_IDLE, at the instant of the
   * fork() stays as it is. Nothing interrupted it in the middle of a
   * transition. */
  chttpsvr_lifecycle_t lifecycle;
  bool contributed_to_engine;
  /* The order is CHTTPSVR_QS_NOT_QUIESCED -> CHTTPSVR_QS_QUIESCING ->
   * CHTTPSVR_QS_QUIESCED. This is the one-shot latch under the winner and
   * loser claim of _quiesce_server_once(); see the doc comment of that
   * function. Only a later, legitimate chttpsvr_start() that restarts this
   * same handle resets the field to CHTTPSVR_QS_NOT_QUIESCED. Every place
   * that reads this state checks it with an exhaustive switch that has no
   * `default:` label. The `-Wswitch` reason is the same one that the comment
   * of lifecycle above explains. Those places are the retry loop of
   * chttpsvr_start(), the claim and the loser wait of _quiesce_server_once(),
   * and the child-side fixup of _chttpsvr_atfork_release_impl.
   *
   * The library sets CHTTPSVR_QS_QUIESCING the instant _quiesce_server_once()
   * claims the teardown of this server. That claim is the winner of a race
   * between __chttpsvr_destroy() and chttpsvr_engine_stop(). See the doc
   * comment of that function for why both can legitimately call
   * _quiesce_server_once() for the same srv at the same time. The library
   * sets the state before any of the real teardown work runs. That work is
   * to stop the listen and drain the requests that are in flight. It also
   * closes the idle connections, frees the engine reference, and destroys
   * the worker pools. The library sets CHTTPSVR_QS_QUIESCED only after that
   * real work is complete. The loser of the race must BLOCK under
   * quiesce_done_cv below.
   * It must block until quiesce_state reaches CHTTPSVR_QS_QUIESCED, and not
   * merely CHTTPSVR_QS_QUIESCING. It must not return at once as a plain
   * no-op. The reason is that __chttpsvr_destroy goes straight from the
   * return of _quiesce_server_once into ccol_mutex_destroy(raw->mutex) and
   * then frees raw itself. The winning call may still use raw->mutex,
   * raw->idle_mutex and raw->requests_done_cv inside
   * _drain_and_close_all_connections at that moment. A destroy there
   * destroys a mutex that is still in use, and frees srv out from under a
   * teardown that still runs. That is a real use-after-free. The top of
   * chttpsvr_start() resets the field to CHTTPSVR_QS_NOT_QUIESCED.
   *
   * _chttpsvr_atfork_release_impl also forces the field straight to
   * CHTTPSVR_QS_QUIESCED in a fresh child after a fork(), and it resets
   * quiesce_waiters below to 0 at the same time. It does this for exactly
   * the servers that it finds in the middle of a teardown at the instant of
   * the fork(). Those servers have quiesce_state == CHTTPSVR_QS_QUIESCING.
   * See the doc comment of that function for why this is the safe way here.
   * It frees both the loser wait of _quiesce_server_once and the retry loop
   * of chttpsvr_start() for such a server in the child. A reset back to
   * CHTTPSVR_QS_NOT_QUIESCED is not safe there. */
  chttpsvr_quiesce_state_t quiesce_state;
  /* The library broadcasts this condition variable once quiesce_state
   * reaches CHTTPSVR_QS_QUIESCED. See the comment of that field. */
  ccol_cond_var_t quiesce_done_cv;
  /* The count of threads that block inside a `while (quiesce_state !=
   * CHTTPSVR_QS_QUIESCED) ccol_cond_var_wait(...)` loop. In practice that
   * count is 0, 1 or a few. The same pair of raw->mutex and quiesce_done_cv
   * guards it. Such a thread is one of two things. It is the losing side of
   * a race between __chttpsvr_destroy() and chttpsvr_engine_stop() inside
   * _quiesce_server_once itself. Or it is a concurrent chttpsvr_start() call
   * whose own retry loop backed off after it saw quiesce_state ==
   * CHTTPSVR_QS_QUIESCING; see the CHTTPSVR_QS_QUIESCING branch of that
   * function. A loser increments this count right before it enters that
   * wait. It decrements the count, and broadcasts quiesce_done_cv again,
   * right after it leaves the wait. It does both while it still holds
   * raw->mutex. The decrement and then the broadcast are therefore the very
   * last touch of raw->mutex and quiesce_done_cv by the loser, before it
   * unlocks and returns.
   *
   * The winner of _quiesce_server_once waits for this count to reach 0
   * before it returns to ITS OWN caller. That wait is at its tail, right
   * after it sets CHTTPSVR_QS_QUIESCED and broadcasts. This is important
   * because __chttpsvr_destroy goes straight from the return of
   * _quiesce_server_once into ccol_mutex_destroy(raw->mutex), then
   * ccol_cond_var_destroy(raw->quiesce_done_cv), and then frees raw itself.
   * It adds no synchronization of its own. The doc comment of quiesce_state
   * already says that a broadcast is what wakes a loser. But a broadcast
   * does not mean that the loser completes its reacquire of the mutex in
   * time. That reacquire happens inside the ccol_cond_var_wait call of the
   * loser. It can still be in progress when the caller of the winner goes
   * on. pthread_cond_broadcast only makes the waiters runnable. It
   * gives no guarantee about when the OS schedules them against the further
   * progress of the thread that broadcasts. Take the case where
   * __chttpsvr_destroy is the WINNER of this race. The
   * concurrent_destroy_and_engine_stop_is_safe test does not exercise that
   * direction. That test deliberately builds the reverse, safer order; see
   * its own comment. In that case the reaper thread of the engine can still
   * be in the middle of its reacquire. That reacquire is inside its own
   * losing ccol_cond_var_wait call on raw->quiesce_done_cv and raw->mutex.
   * It can be there at the exact moment when __chttpsvr_destroy destroys
   * both. POSIX says that is undefined behavior. No code may destroy a mutex
   * or a condition variable while another thread may still reference it.
   * Such a thread references it through a pthread_cond_wait call that is in
   * progress. This is a real concern, not a theoretical one.
   * The mutual exclusion of the mutex is what makes this counter-based wait
   * correct. The winner can only reacquire the mutex and see zero strictly
   * after the loser decrements and unlocks. This is the same happens-before
   * relation that pending_resolve_count uses elsewhere in this file.
   *
   * _chttpsvr_atfork_release_impl resets this count to 0, beside
   * quiesce_state, in a fresh child after a fork(). It does so for a server
   * that the fork() caught in the middle of a teardown. See the field
   * comment of quiesce_state and the doc comment of that function. */
  size_t quiesce_waiters;

  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned max_response_write_duration_ms;
  _Atomic unsigned idle_timeout_ms; /* the idle timeout for keep-alive; 0
                                     * turns it off */
  /* The total wall-clock ceiling on the reactor-owned phase of one request.
   * That phase is the TLS handshake plus the read of the request headers. A
   * value of 0 turns the ceiling off. The idle sweep enforces it against
   * conn->header_phase_start. See max_header_read_duration_ms in
   * chttpsvr_config_t, and see _conn_header_phase_expired. */
  _Atomic unsigned max_header_read_duration_ms;
  /* These fields are _Atomic, and not merely written under srv->mutex. The
   * reason is the same one as for stream_read_timeout_ms and the other
   * fields just above. chttpsvr_start() writes them again on every restart,
   * in the small unlocked gap after it publishes srv->worker_pool. See the
   * ccol_mutex_lock and ccol_mutex_unlock a few lines above the matching
   * write site. That gap ends when the library registers the listener again.
   * A keep-alive connection can survive the chttpsvr_stop() before the
   * restart. That stop only tears the listener down. It leaves the
   * connections that the server already accepted. The library can divert
   * such a connection to the pool that it just published. The worker thread
   * of that connection can then read max_body_size (in _on_body) or
   * max_header_bytes (in _conn_reset_for_request) inside that same gap, and
   * a request of the previous run that still runs across the restart reads
   * them while the restart writes them. There is no lock common to the
   * writer and those readers, so only an atomic access makes each read see
   * either the old or the new value. */
  _Atomic size_t max_body_size;
  _Atomic size_t max_header_bytes; /* 0 selects the built-in default of
                                    * chttp1_parser */
  _Atomic size_t max_connections;  /* 0 means no limit */
  /* This is a plain bool, unlike the three fields above. It has one read
   * site: _listener_on_readable, through _apply_accepted_socket_options.
   * That site runs only while a listener registration is live. The
   * ccol_event_loop_remove(listen_reg) call of chttpsvr_stop() blocks until
   * any listener callback in flight returns, and it guarantees that none
   * fires after that. The library registers the new listener well after
   * chttpsvr_start() writes this field. There is therefore no window where a
   * reader of this field runs at the same time as a writer of it.
   * max_body_size and max_header_bytes above are different. Their reader is
   * the worker thread of a connection that already exists. That thread is
   * fully decoupled from the listener lifecycle of this server. */
  bool enable_keepalive;
  _Atomic size_t current_connections;

  /* _listener_on_readable_impl sets this flag right before it pauses the
   * listen_reg of this server. It does so for a resource-exhaustion errno
   * from accept4(), which is EMFILE, ENFILE, ENOBUFS or ENOMEM. It also does
   * so for an allocation failure after the accept, in _conn_create or in
   * ctls_conn_create_server. The
   * _listener_resume_if_resource_pressure_cleared function, on the thread of
   * the idle timeout sweep, consumes the flag. It reads and clears it
   * together, with
   * atomic_exchange. This follows the pause and resume split of
   * max_connections and current_connections above, for a reason of the same
   * shape. An immediate inline retry of accept4() right after the backoff
   * blocks the one shared reactor thread. For a condition that does not go
   * away, every later dispatch blocks it too. Level-triggered epoll keeps
   * reporting the same backlog that is not empty. That blocks for as
   * long as the resource pressure lasts. It starves every other connection
   * on every server that shares that one reactor thread. A pause instead,
   * with one retry by the sweep on each tick, removes that block entirely.
   * The price is a recovery latency of up to
   * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, which the resume of max_connections
   * already carries. The field is _Atomic for the same cross-thread read
   * reason as current_connections just above. The writer is always
   * _listener_on_readable_impl, which never runs at the same time as itself.
   * See the note about the serialization by dispatch_lock a few fields down.
   * The reader is a different thread. */
  _Atomic bool listener_paused_for_resource_pressure;

  /* These two fields rate-limit the diagnostic log that
   * _listener_on_readable writes for an unexpected accept4() failure. That
   * means any failure beyond the routine EWOULDBLOCK, EAGAIN and EINTR that
   * the loop already handles in silence. Without the rate limit, a
   * resource-exhaustion condition that does not go away triggers this same
   * log line on every reactor dispatch. It does so for as long as the
   * condition lasts. EMFILE and ENFILE are such conditions. The process, or
   * the system, is out of file descriptors. The diagnostic that is meant to
   * help then floods the log itself. These are plain fields and not _Atomic.
   * The per-registration dispatch_lock of ccol_event_loop already guarantees
   * that the library never dispatches the registration of this listener
   * concurrently with itself (see cthreadcomm.h). The _listener_on_readable
   * function of one server therefore never runs on two threads at once. This
   * holds whatever the count of reactor threads in the configuration is.
   * Only that one function reads and writes both fields, and time serializes
   * its runs. */
  struct timespec last_accept_err_log;
  bool last_accept_err_log_set;

  /* The registry of idle connections, for the sweep of the idle timeout. It
   * holds every connection that the reactor owns and that waits for the
   * headers of its next request. It never holds a diverted connection that a
   * worker owns. */
  ccol_mutex_t idle_mutex;
  chttpsvr_conn_t *idle_head, *idle_tail;
  /* The parked connections: those that wait with no thread for the rest of
   * a body or for room to write a response (conn->park is _CONN_PARK_BODY or
   * _CONN_PARK_WRITE). They are claimed exactly as idle connections are,
   * under idle_mutex, by _idle_list_try_claim and _idle_list_remove, but
   * they live in a list of their own, so that the sweep examines them
   * without walking every idle keep-alive connection. */
  chttpsvr_conn_t *parked_head, *parked_tail;

  /* The registry of diverted connections. It holds every connection that a
   * worker thread owns (CONN_ST_DIVERTED; see _conn_start_diverted and
   * _task_worker). Such a connection can block forever inside
   * chttp1_stream_read or chttp1_stream_write. That happens when the
   * configuration sets stream_read_timeout_ms or response_write_timeout_ms
   * to 0. A value of 0 means "wait forever", and it is a documented,
   * legitimate setting. The peer then stalls without a close. The
   * _wait_in_flight_bounded function uses this registry to force a
   * shutdown(2) on the fd of such a connection. It does so after its own
   * graceful wait runs out. One stalled peer can therefore not hang
   * chttpsvr_destroy(), a restart of
   * chttpsvr_stop() plus chttpsvr_start(), or chttpsvr_engine_wait()
   * forever. An unconditional ctpool_shutdown_drain() call with no timeout
   * does hang in that case. See the comment of that function for the full
   * reasoning. diverted_mutex guards these fields, in the same pattern as
   * idle_mutex, idle_head and idle_tail above. */
  ccol_mutex_t diverted_mutex;
  chttpsvr_conn_t *diverted_head, *diverted_tail;

  /* The index of this server into chttpsvr_slot_table.slots.
   * _chttpsvr_handle_slot_acquire sets it once. _chttpsvr_finish_destroy
   * needs it when it runs from _conn_on_removed; see the comment of that
   * field. That path has no chttpsvr handle value to derive the index from,
   * only this raw pointer. */
  uint32_t self_slot_idx;

  /* This counter says when it is safe to free this struct. It follows the
   * lifetime_refs design of chttpsvr_conn_t exactly; see the comment of that
   * field for the full reasoning. It starts at 1, which stands for "the
   * application did not yet destroy this server" (see
   * ccol_create_chttpsvr_mp). It gets +1 for every conn->reg registration
   * that the library creates for any connection that this server accepts.
   * That is every call site of ccol_event_loop_add in _conn_pump and
   * _task_worker, on success. It also gets +1 for the listener registration
   * that each chttpsvr_start creates. It gets -1 exactly once from
   * __chttpsvr_destroy, which is the "done with this server" decision of the
   * application. It also gets -1 exactly once for each on_removed that fires
   * (_conn_on_removed, _listener_on_removed). A listener dispatch that
   * already started when chttpsvr_stop removes the listener therefore keeps
   * srv allocated until it returns, because the on_removed of the listener
   * fires only after that. The _chttpsvr_finish_destroy function runs only
   * when the counter reaches 0. Any of those callers can bring it there. That
   * function does the real ccol_mutex_destroy, the free and the release of
   * the slot. It sits outside __chttpsvr_destroy for exactly this reason.
   * The on_removed of a connection can fire well after the caller of
   * __chttpsvr_destroy returns. See the doc comment of
   * ccol_event_loop_remove() for why it cannot make that step synchronous
   * without a risk of a lock-ordering cycle. This closes one specific case.
   * That case is a registration that the library removes too close to the
   * async teardown of the shared reactor. The reaper of the engine drives
   * that teardown. Such a registration never reaches the ordinary reclaim
   * pass of the poller. Without the deferral, __chttpsvr_destroy frees raw
   * synchronously at the wrong time. The final, unconditional sweep of
   * _ccol_event_loop_teardown_raw still has the on_removed of this exact
   * connection pending on the thread of the reaper. That is an intermittent
   * use-after-free, and valgrind catches it. __chttpsvr_destroy itself must
   * never block and wait for this counter to reach 0. Such a block
   * deadlocks against the tests of this project where chttpsvr_start()
   * races a concurrent graceful reap. The design of those tests needs
   * chttpsvr_destroy() and chttpsvr_engine_release() to return promptly, and
   * not to block on the reap that they may have triggered. A deferral of the
   * free itself, instead of a block for it, is what closes the gap without
   * that deadlock. */
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs_t *m_procs;

  /* The slow-client settings, resolved by chttpsvr_start. They are _Atomic
   * for the same reason as stream_read_timeout_ms above: a restart writes
   * them while connections that survived the stop can read them.
   * min_rate_bps is UINT_MAX when the floor is off, mem_cap is SIZE_MAX when
   * there is no memory limit, the two waits are UINT_MAX when they have no
   * deadline, stream_threads is -1 when streaming handlers share the worker
   * pool, and stream_qcap is SIZE_MAX when the streaming queue has no
   * limit. */
  _Atomic unsigned min_rate_bps;
  _Atomic unsigned rate_grace_ms;
  _Atomic size_t mem_cap;
  _Atomic unsigned mem_wait_ms;
  _Atomic int stream_threads;
  _Atomic size_t stream_qcap;
  _Atomic unsigned stream_qtimeout_ms;

  /* The pool that runs streaming handlers. srv->mutex guards it, exactly as
   * it guards worker_pool: _conn_start_diverted and the detach of a stop or
   * a destroy read and write it under that lock. The first streaming request
   * creates it; see _stream_pool_get. */
  ctpool stream_pool;

  /* The pools that a restart took out of service while requests of the
   * previous run could still be running on them. Each one drains on a
   * thread of its own; see _retire_pools. srv->mutex guards the list. A
   * teardown waits for every one of them; see _retired_pools_reap. */
  struct _chttpsvr_retired_pools *retired_pools;

  /* wait_mutex guards the memory accounting, the memory-wait list and the
   * streaming queue. None of those is on the path of an ordinary request:
   * a request takes it only when its body must reserve memory, or when it
   * enters or leaves the streaming queue. It is never held across I/O, a
   * dispatch or another lock of this server.
   *
   * mem_in_use is the sum of every mem_charged. mem_holders counts the
   * connections that hold a charge, and mem_blocked those of them that wait
   * in the memory-wait list for more. mem_exempt_active says that one
   * connection went ahead past the limit; see _mem_admit for the rule and
   * its proof of progress. mem_admission_closed stops a teardown from
   * dispatching a waiter. mem_admitting and mem_admit_again serialize
   * _mem_admit without recursion. */
  ccol_mutex_t wait_mutex;
  size_t mem_in_use;
  size_t mem_holders;
  size_t mem_blocked;
  bool mem_exempt_active;
  bool mem_admission_closed;
  bool mem_admitting;
  bool mem_admit_again;
  chttpsvr_conn_t *mem_head, *mem_tail;
  size_t mem_waiting;

  /* The streaming queue: oldest at sq_head. sq_tokens counts the tasks that
   * the streaming pool runs or holds to drain the queue; it never exceeds
   * the thread count of that pool. See _stream_token_task. */
  chttpsvr_conn_t *sq_head, *sq_tail;
  size_t sq_len;
  size_t sq_tokens;
};

/* ========================================================================== */
/*                    CHTTPSVR HANDLE RESOLVE / UNPIN                         */
/* ========================================================================== */

/* Resolves h and pins the result against a concurrent destroy. It returns
 * NULL when h is 0 and when h is garbage. It also returns NULL when h names
 * a slot that is free now, or that the library already reused with a
 * different generation. On success, the caller MUST call
 * _chttpsvr_resolve_unpin(result) exactly once. That call goes immediately
 * before every return path of the short, synchronous public function that
 * resolved the handle. No consumer of this handle needs to hold the pin
 * longer than its own function body. See the field comment of
 * pending_resolve_count in struct chttpserver for the reason.
 * This follows _chttpcli_resolve in chttpclient.c exactly. */
static struct chttpserver *_chttpsvr_resolve(chttpsvr h) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  /* This step is lock-free. It never takes raw->mutex, so nothing can block
   * while the library holds chttpsvr_slot_table.mutex. See _chttpcli_resolve
   * in chttpclient.c for the full rationale about contention that this
   * follows. The step is safe because raw is still allocated here. The one
   * thing that can make it unsafe to touch is the slot-release step of
   * __chttpsvr_destroy. That step also needs chttpsvr_slot_table.mutex,
   * which the library still holds at this exact point. */
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

static void _chttpsvr_resolve_unpin(struct chttpserver *raw) {
  /* The decrement itself MUST happen under raw->mutex. It must not be a
   * bare atomic operation outside the lock. See _chttpcli_resolve_unpin in
   * chttpclient.c for the full account of the real use-after-free that a
   * lock-free decrement opens. The reasoning is the same here. The increment
   * in the resolve stays lock-free. The decrement in the unpin happens
   * together with the broadcast, and both sit inside raw->mutex. This
   * matches the standard pattern for a condition variable. */
  ccol_mutex_lock(raw->mutex);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  ccol_cond_var_broadcast(raw->resolve_cv); /* wake a destroy that waits on
                                             * this */
  ccol_mutex_unlock(raw->mutex);
}

/* Allocates a fresh slot for srv, or reuses a slot that the library freed.
 * It returns the handle that follows from the slot, or 0 when it runs out of
 * memory. ccol_create_chttpsvr_mp calls it once, after the rest of the
 * object is fully built. This follows _chttpcli_handle_slot_acquire in
 * chttpclient.c exactly. That includes the order of the generation mint,
 * which must happen before the library builds the handle from it. It also
 * includes the skip for a wraparound of the generation. Without that skip,
 * the handle of a live server whose generation wrapped can collide with
 * CHTTPSVR_INVALID. */
static chttpsvr _chttpsvr_handle_slot_acquire(struct chttpserver *srv) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  uint32_t idx;
  chttpsvr_slot_t *slot;
  if (cvector_elem_count(chttpsvr_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpsvr_slot_table.free_indices, &idx);
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  } else {
    chttpsvr_slot_t fresh = {0};
    if (cvector_push_back(chttpsvr_slot_table.slots, &fresh) != ccol_success) {
      ccol_mutex_unlock(chttpsvr_slot_table.mutex);
      return 0; /* an ordinary failure to allocate; it is not fatal */
    }
    idx = (uint32_t)cvector_elem_count(chttpsvr_slot_table.slots) - 1;
    slot = (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0)
    slot->generation++; /* skip the value that collides with
                         * CHTTPSVR_INVALID; see the identical guard in
                         * chttpclient.c for the full reasoning */
  slot->ptr = srv;
  slot->in_use = true;
  srv->self_slot_idx = idx;
  chttpsvr h = ((chttpsvr)idx << 32) | (chttpsvr)slot->generation;
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return h;
}

/* ========================================================================== */
/*                    WORKER SELF-CALL DETECTION                              */
/* ========================================================================== */

/* A process-wide thread-local key. It records which struct chttpserver*, if
 * any, the calling thread runs a worker_pool or reject_pool task for. The
 * library sets it once, at the top of _task_worker and _reject_task. Those
 * are the only two functions that the library submits to the worker_pool or
 * the reject_pool of a server. Nothing assigns the key again, and nothing
 * clears it, for the rest of the life of that thread. This follows
 * ctpool_worker_key_bundle and _ctpool_is_self_call in cthreadpool.c
 * exactly, for the same reason. One server owns a worker thread of its
 * srv->worker_pool or srv->reject_pool privately and permanently, for the
 * whole life of that thread. ctpool always spawns its own dedicated threads
 * and never shares them across pools. The library also destroys the pools of
 * a server and builds them again, with new OS threads, on every restart.
 * There is therefore no set-and-clear lifecycle for each task to manage. The
 * key for a dispatch-pool worker of ccol_event_loop in cthreadcomm.c does
 * need one.
 *
 * This key exists to detect a call to chttpsvr_destroy() from a request
 * handler, from a middleware, or from the chttpsvr_next_fn continuation of
 * that handler. It detects a call to chttpsvr_stop() followed at once by
 * chttpsvr_start() in the same way. The detection is for the very server
 * whose worker pool runs that code. Without the key, that call goes into the
 * bounded wait of _wait_in_flight_bounded for in_flight_requests to reach 0.
 * That wait is about a minute, once the poll of current_connections in
 * _drain_and_close_all_connections is included. in_flight_requests can never
 * reach 0, because this exact call stack is the one that decrements it. The
 * call then goes into ctpool_destroy() on the very pool that this thread is
 * a worker of. That is an unrelated ccol_fatal_err() and abort() from
 * cthreadpool.c; see the comment of ctpool_worker_key_bundle there. Without
 * this guard the symptoms are indirect and hard to attribute. A handler that
 * calls chttpsvr_destroy() on its own server hangs the whole process for
 * about a minute and then aborts it. The crash signature points into
 * cthreadpool.c and not into this file. A race with a concurrent
 * chttpsvr_engine_stop() for the same server deadlocks forever instead, with
 * no abort at all. The ctpool_shutdown_drain of the reaper thread is not a
 * self-call, and it has no timeout of its own. It waits forever for the
 * self-deadlocked handler to return. The key detects the case here, at once
 * and cheaply, with one thread-local read and no lock, before any of that
 * can unfold. */
/*
 * The creation of the key can fail, when the process already holds
 * PTHREAD_KEYS_MAX keys. The key field then holds no key of this module: the
 * value 0 names whatever key some other component created first, and a set
 * through it hands srv to the destructor of that component. `ready` records
 * that the creation succeeded, and nothing reads or writes `key` without it.
 * chttpsvr_start creates the key, and it refuses to start a server when it
 * cannot, so no worker thread ever exists without the key. A later
 * chttpsvr_start tries the creation again. The module destructor deletes the
 * key once no server is left that a worker could belong to; see
 * _cleanup_chttpsvr_slot_table. */
static struct {
  ccol_thread_ls_key_t key;
  ccol_once_flag_t once;
  ccol_mutex_t mutex; /* serializes the creation and the deletion of key */
  _Atomic bool ready;
} chttpsvr_worker_key_bundle = {0};

static void _chttpsvr_worker_key_init_globals(void) {
  if (ccol_mutex_init(chttpsvr_worker_key_bundle.mutex) != 0)
    ccol_fatal_err("chttpsvr worker key: failed to initialize mutex");
}

/* Creates the key unless it exists. Returns whether it exists. Only
 * chttpsvr_start calls this. */
static bool _chttpsvr_worker_key_ensure(void) {
  if (atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                           memory_order_acquire))
    return true;
  ccol_call_once(chttpsvr_worker_key_bundle.once,
                 _chttpsvr_worker_key_init_globals);
  ccol_mutex_lock(chttpsvr_worker_key_bundle.mutex);
  if (!atomic_load(&chttpsvr_worker_key_bundle.ready) &&
      ccol_thread_ls_key_create(chttpsvr_worker_key_bundle.key, NULL) == 0)
    atomic_store_explicit(&chttpsvr_worker_key_bundle.ready, true,
                          memory_order_release);
  bool ready = atomic_load(&chttpsvr_worker_key_bundle.ready);
  ccol_mutex_unlock(chttpsvr_worker_key_bundle.mutex);
  return ready;
}

/* Says whether the key exists. A thread can only have marked itself when it
 * does, so every reader treats a missing key as "not a worker". */
static inline bool _chttpsvr_worker_key_ready(void) {
  return atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                              memory_order_acquire);
}

/* The library calls this once, at the top of _task_worker and _reject_task.
 * It marks the calling thread as a permanent worker of the pools of srv. See
 * the comment of chttpsvr_worker_key_bundle above. */
static void _chttpsvr_mark_worker_thread(struct chttpserver *srv) {
  if (!_chttpsvr_worker_key_ready()) return;
  ccol_thread_ls_set(chttpsvr_worker_key_bundle.key, (void *)srv);
}

/* Returns true when the calling thread is a worker thread of the
 * worker_pool or the reject_pool of srv, and false otherwise. That is true
 * when the call comes from a request handler, a middleware, or the
 * courtesy-close task of reject_pool that runs for srv now. The call can be
 * direct, or it can come through other calls. */
static bool _chttpsvr_is_self_call(struct chttpserver *srv) {
  if (!_chttpsvr_worker_key_ready()) return false;
  return ccol_thread_ls_get(chttpsvr_worker_key_bundle.key) == (void *)srv;
}

/* Returns true when the calling thread is a worker thread of the worker_pool
 * or the reject_pool of ANY chttpsvr. That is true when the call comes from
 * a request handler, a middleware, or the courtesy-close task of reject_pool
 * of some server. The call can be direct, or it can come through other
 * calls. The server does not matter. _chttpsvr_is_self_call has the scope of
 * one specific server. This function exists for chttpsvr_engine_wait(), and
 * that function is engine-wide and not server-specific. The
 * _engine_force_stop_quiesce_all function drains the worker pool of every
 * registered server in turn. It does so with a plain ctpool_shutdown_drain
 * that has no timeout, inside _quiesce_server_once. A handler on ANY server
 * that blocks in chttpsvr_engine_wait() therefore risks the same class of
 * self-deadlock that _chttpsvr_is_self_call already guards
 * chttpsvr_destroy() and chttpsvr_start() against. The reaper thread can
 * never finish the drain of the pool of the server of that handler. This
 * exact call stack is what lets that task return. The library can therefore
 * never tear the reactor down and mark it stopped, and this wait can never
 * wake. That is a
 * permanent deadlock of the whole engine, not one stuck server. */
static bool _chttpsvr_is_any_worker_call(void) {
  if (!_chttpsvr_worker_key_ready()) return false;
  return ccol_thread_ls_get(chttpsvr_worker_key_bundle.key) != NULL;
}

/* ========================================================================== */
/*                    SHARED STATIC REACTOR (chttpserver's OWN engine)        */
/* ========================================================================== */

/*
 * One static ccol_event_loop reactor for the whole process. Every chttpsvr
 * instance in the process shares it. There are two separate, independent
 * reactors. The other one belongs to chttpclient, which has its own static
 * ccol_event_loop; see chttpclient.c. chttpserver and chttpclient do not
 * share one reactor for the process. This lifecycle wrapper therefore needs
 * no coordination across the two modules. It needs only a refcount across
 * the chttpsvr instances. That is an acquire, release and reaper-thread
 * shape, in a simple form. There is no ordering concern across modules for
 * an atexit safety net, because nothing else in the process races to bring
 * this reactor up first.
 */
static struct {
  /* The library writes this field only under srv_engine_bundler.mutex. The
   * _engine_acquire function creates it, including in its own rollback
   * branch for a failed allocation. _engine_reaper_fn sets it to
   * CCOL_EVENT_LOOP_INVALID. But dozens of call sites in this file read it
   * with no lock. Those are every site that handles a connection, every
   * reactor callback and every listener site. This is safe by construction.
   * It is not safe by accident, through the atomicity of a uint64_t load or
   * store at the level of the instruction set. Every unlocked reader can
   * only run while at least one chttpsvr instance is started and holds a
   * live engine reference. That invariant is exactly what keeps the value of
   * this field constant between the write at creation and the later write at
   * the reap. Real synchronization also orders both of those writes against
   * every reader. That synchronization is ccol_thread_create and
   * ccol_thread_join for the reactor threads themselves, or the pin and
   * resolve mechanisms that guard every chttpsvr handle. It is not merely
   * "probably fine". A future call path that reads this field OUTSIDE that
   * invariant needs the field to be _Atomic instead. Such a path is one that
   * does not first hold a live engine reference of its own. */
  ccol_event_loop reactor;
  size_t reactor_refs;
  ccol_mutex_t mutex;
  ccol_cond_var_t stopped_cv;
  ccol_once_flag_t once;
  bool stopping;
  ccol_thread_id_t reaper_thread;
  bool reaper_joinable;
  ccol_memmgmt_procs_t mprocs_storage;
  ccol_memmgmt_procs_t *mprocs;
  /* A value of 0 selects the default, which is one dedicated reactor thread
   * (num_reactor_threads == 1 inside). A benchmark shows that this is the
   * better choice for the common case; nobody assumed it. See the doc
   * comment of chttpsvr_set_engine_num_reactor_threads for the full
   * comparison. A positive value pins the reactor to exactly that count of
   * OS threads instead. The library builds this value into the reactor at
   * the time of construction. The same restriction as for mprocs above
   * applies: set it before the first start, or after a full stop. */
  size_t num_reactor_threads;
  /* The value that the library passed to ccol_event_loop_create_with_mprocs
   * the last time it created the reactor. That value is either
   * auto-detected or explicit. It serves only the instrumentation of the
   * tests; see _chttpsvr_engine_num_reactor_threads_for_tests below. */
  size_t last_resolved_num_reactor_threads;
  /* The engine-wide logger for diagnostics. The ccol_event_loop reactor has
   * no internal log of its own to forward. This logger therefore captures
   * the diagnostics of the reactor thread of chttpserver itself, across
   * every chttpsvr instance that shares the one reactor of the process.
   * Those diagnostics are TLS handshake failures, listener bind errors and
   * closes from the idle timeout. The library uses a logger that the caller
   * installs with chttpsvr_set_engine_logger, when the caller sets it before
   * the engine first starts. If not, _engine_acquire installs a fallback
   * logger at the CLOG_FATAL level on fd 2, the first time the engine
   * starts. This field is therefore never NULL while the engine runs. That
   * is not merely an internal detail. The ccol_log_info call in the teardown
   * of _engine_reaper_fn, and every other log call site, both need SOME live
   * logger to call through. The _clog_write function has no guard for a
   * NULL handle. A call through a NULL logger therefore crashes instead of a
   * silent skip. Only one rare path reaps the engine with this field NULL.
   * That is the path where the allocation of the logger itself runs out of
   * memory. See the comment of that path in _engine_acquire.
   * srv_engine_bundler.mutex guards this field
   * only against a torn read or write of the pointer that races a concurrent
   * chttpsvr_set_engine_logger() call. clog itself is already thread-safe
   * for concurrent log calls through one handle. */
  clog log;
  /* The logger that chttpsvr_set_engine_logger() derived from the logger of
   * the caller, or CLOG_INVALID. It outlives every engine: each start of an
   * engine takes it as log, and the reaper of an engine leaves it open, so a
   * logger that the caller installs keeps serving every later engine of the
   * process. Only a later chttpsvr_set_engine_logger() call, or the exit of the
   * process or the unload of this module, closes it. srv_engine_bundler.mutex
   * guards it. */
  clog user_log;
} srv_engine_bundler = {0};

/* The sweep thread for the idle timeout. There is one for each process, and
 * the registry of idle connections of every chttpsvr instance shares it.
 * Each server has its own srv->idle_head and srv->idle_tail list. The sweep
 * walks every server that started. */
static struct {
  ccol_thread_id_t thread;
  bool running;
  /* This field is _Atomic, and not merely written under
   * servers_bundler.mutex. _idle_sweep_fn runs on its own dedicated thread
   * and reads this field in its loop condition. It never takes that mutex. A
   * plain bool here is a real data race against the mutex-protected write of
   * _idle_sweep_stop_if_running, and ThreadSanitizer confirms it. The single
   * byte of this field makes a torn read unlikely in practice. But the race
   * is still undefined behavior. That permits the compiler to cache the read
   * across the iterations of the loop. The compiler then never sees the
   * write at all. The ccol_thread_join of _idle_sweep_stop_if_running then
   * hangs forever, for real. */
  _Atomic bool stop_flag;
} idle_sweep_bundler = {0};

static struct {
  ccol_mutex_t mutex;
  struct chttpserver **servers;
  size_t count;
  size_t capacity;
} servers_bundler = {0};

static void _idle_sweep_stop_if_running(void);
/* The definition is far below, beside the rest of the signal-safe machinery
 * of the engine-stop watcher. The declaration is here for _engine_reaper_fn,
 * its one call site, which this file defines first. */
static void _engine_request_reaper_join(void);
static void _quiesce_server_once(struct chttpserver *srv);
static void _chttpsvr_stop_internal(struct chttpserver *raw);
static void _engine_globals_init(void);
#ifdef RUNNING_UNIT_TESTS
static void _reaper_race_hook_wait_if_armed(void);
static void _listener_dispatch_race_hook_wait_if_armed(bool at_entry);
static void _quiesce_teardown_race_hook_wait_if_armed(void);
#endif /* RUNNING_UNIT_TESTS */

/* Only the engine reaper (_engine_reaper_fn) calls this, before it tears the
 * shared reactor down. It quiesces every chttpsvr instance that is still
 * registered in servers_bundler.servers. To quiesce is to stop the listen
 * and drain the requests that are in flight. It also closes the idle
 * connections, shuts the worker pool down and destroys it. It then frees the
 * engine reference. This holds whatever triggered the run of the reaper.
 * For a run that
 * chttpsvr_engine_stop() triggers, the servers may still be fully live and
 * started. For the graceful path where the refcount reaches zero, every
 * registered server already quiesced and unregistered itself by
 * construction, so this function does nothing there. Without this function,
 * a forced engine stop tears srv_engine_bundler.reactor down and frees
 * servers_bundler.servers out from under servers that are still started. The
 * listen_reg and conn->reg registrations of those servers point into that
 * reactor. The next chttpsvr_destroy() call of such a server then
 * dereferences state that the library already freed.
 *
 * The loop reads servers_bundler.servers[0] again on every pass. It does not
 * take a snapshot of the whole list at the start. The reason is that
 * _quiesce_server_once calls _servers_unregister, which removes the element
 * that it just processed. If a concurrent chttpsvr_destroy() on another
 * thread already quiesces that same server, the element stays in place until
 * that other call finishes. Either way this loop converges on
 * servers_bundler.count == 0, once every legitimate teardown that is in
 * progress completes.
 *
 * The loop pins srv for the whole rest of each iteration, including the
 * _quiesce_server_once call itself. See the atomic_fetch_add below. A bare
 * struct chttpserver* that the loop reads out of servers_bundler.servers[]
 * has nothing else to protect its lifetime the instant the library unlocks
 * servers_bundler.mutex. The real work of _quiesce_server_once is a drain of
 * the requests in flight and a close of the connections. That work can take
 * a meaningful amount of wall-clock time, and not merely a few
 * instructions. This is a real and reproducible window, not a theoretical
 * one. Inside it, a fully independent, concurrent chttpsvr_destroy(h) call
 * on this exact server can run to completion. That call frees the server out
 * from under this loop. The __chttpsvr_destroy function has no way to know
 * that this thread is about to use srv. It never goes through
 * servers_bundler, only through the separate
 * chttpsvr_slot_table.
 *
 * The pin here is srv->servers_bundler_pins, and NOT
 * srv->pending_resolve_count. A pin on pending_resolve_count looks
 * attractive. The _chttpsvr_resolve function uses that mechanism for every
 * other caller of a chttpsvr handle. The __chttpsvr_destroy function also
 * has a matching wait for it. But it self-deadlocks. The winner path of
 * _quiesce_server_once ALSO waits for pending_resolve_count to reach 0, as
 * its very first step, before any of its real work. Take a caller that holds
 * its own pin on pending_resolve_count across its own call to
 * _quiesce_server_once. That caller waits forever on a pin that only it can
 * free. servers_bundler_pins is a separate counter, and
 * _quiesce_server_once never touches it, so no such cycle exists. See the
 * comment of that field. */
static void _engine_force_stop_quiesce_all(void) {
  /* Every real caller runs only after the
   * ccol_call_once(srv_engine_bundler.once, ...) of _engine_acquire fires.
   * But this project has a rule. Put the guard in every function that
   * touches the primitive directly. Never depend on reasoning about the call
   * graph. See the identical guard in _servers_register. This function
   * therefore carries its own guard and does not assume that order. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  struct chttpserver *prev_unremoved = NULL;
  for (;;) {
    ccol_mutex_lock(servers_bundler.mutex);
    if (servers_bundler.count == 0) {
      ccol_mutex_unlock(servers_bundler.mutex);
      return;
    }
    struct chttpserver *srv = servers_bundler.servers[0];
    /* See the field comment of servers_bundler_pins for why this is a
     * dedicated counter and not pending_resolve_count. The increment happens
     * while the library still holds servers_bundler.mutex. Without that, the
     * reference of this thread to srv is unprotected for one instruction or
     * more. */
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    ccol_mutex_unlock(servers_bundler.mutex);

#ifdef RUNNING_UNIT_TESTS
    _reaper_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

    if (srv == prev_unremoved) {
      /* _quiesce_server_once below never returns to any caller, winner or
       * loser, before _servers_unregister(srv) already ran. See the order of
       * quiesce_state against the unregister in that function. A srv that
       * appears here again as servers_bundler.servers[0] is therefore never
       * a sign that the unregister of the previous iteration is still open.
       * It means that a concurrent chttpsvr_start(srv) restart legitimately
       * ran _servers_register(raw) again. That register call deliberately
       * comes before the _engine_acquire of the start call, exactly so that
       * a force-stop pass like this one can see it. See the call site of
       * _servers_register in chttpsvr_start for the full reasoning. The
       * restart ran in the narrow window between the return of the previous
       * _quiesce_server_once(srv) of this loop and the read of
       * servers_bundler.servers[0] in this iteration. That racing
       * chttpsvr_start() call keeps its resolve pin for its whole duration.
       * The _quiesce_server_once call below therefore blocks on that exact
       * pin before it quiesces srv again. This short sleep only avoids a
       * busy spin against that same restart while the restart runs. */
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
    }
    prev_unremoved = srv;
    _quiesce_server_once(srv);
    /* This matches the pin above. srv stays valid memory that the library
     * did not free, up to and including the _quiesce_server_once call just
     * above. The __chttpsvr_destroy function waits for
     * servers_bundler_pins == 0. It does that wait right before it frees one
     * byte of srv. That wait cannot end while this
     * thread still holds its pin. The library frees the pin under
     * srv->mutex, together with the broadcast that wakes such a destroy.
     * This follows the reasoning of _chttpsvr_resolve_unpin for
     * pending_resolve_count. */
    ccol_mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    ccol_cond_var_broadcast(srv->resolve_cv);
    ccol_mutex_unlock(srv->mutex);
  }
}

static void _engine_globals_init(void) {
  if (ccol_mutex_init(srv_engine_bundler.mutex) != 0)
    ccol_fatal_err("chttpserver engine: failed to initialize mutex");
  if (ccol_cond_var_init(srv_engine_bundler.stopped_cv) != 0)
    ccol_fatal_err(
        "chttpserver engine: failed to initialize condition variable");
  if (ccol_mutex_init(servers_bundler.mutex) != 0)
    ccol_fatal_err(
        "chttpserver engine: failed to initialize servers_bundler mutex");
  /* The engine leaves the disposition of SIGPIPE to the application. A
   * client can close its read side, or the whole connection, while a worker
   * is in the middle of a write of the response. No write of this module
   * can raise the signal for that: every plaintext write goes out through
   * chttp1_stream_write() or chttp1_stream_writev2(), which send with
   * MSG_NOSIGNAL, and every TLS write, the close_notify of a teardown
   * included, goes through the socket BIO of ctls, which also sends with
   * MSG_NOSIGNAL. */
}

/*
 * Logs through srv_engine_bundler.log. It puts the read of that pointer and
 * the ccol_log_info or ccol_log_error call itself into ONE critical section.
 * It does not copy the pointer out under the lock and then use it with no
 * lock held. A copy of the pointer and a use of it later is a
 * use-after-free. chttpsvr_set_engine_logger() swaps a new logger in under
 * srv_engine_bundler.mutex. It calls clog_close() on the OLD logger only
 * after it unlocks that mutex. clog_close() frees the handle itself
 * unconditionally, whatever the separate refcount of the shared backing
 * store says; see clogger.c. Take a caller that copies the old pointer out
 * before the swap, and uses it only afterward. That caller then calls
 * ccol_log_info() or ccol_log_error() on memory that the library just freed.
 * This stays a macro and is not a wrapper function. That way the __FILE__,
 * __LINE__ and __func__ capture of the log call still names the real call
 * site, and not this helper.
 */
#define _SRV_ENGINE_LOG(level_call, ...)                           \
  do {                                                             \
    ccol_call_once(srv_engine_bundler.once, _engine_globals_init); \
    ccol_mutex_lock(srv_engine_bundler.mutex);                     \
    if (srv_engine_bundler.log)                                    \
      level_call(srv_engine_bundler.log, __VA_ARGS__);             \
    ccol_mutex_unlock(srv_engine_bundler.mutex);                   \
  } while (0)

static void _join_reaper_if_needed_locked(void) {
  if (srv_engine_bundler.reaper_joinable) {
    ccol_thread_join(srv_engine_bundler.reaper_thread);
    srv_engine_bundler.reaper_joinable = false;
  }
}

static void *_engine_reaper_fn(void *arg) {
  (void)arg;
  /* Every real caller spawns this thread only after the
   * ccol_call_once(srv_engine_bundler.once, ...) of _engine_acquire fires;
   * see the two call sites of _spawn_reaper. But this project has a rule.
   * Put the guard in every function that touches the primitive directly.
   * Never depend on reasoning about the call graph. This function therefore
   * carries its own guard and does not assume that order. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_event_loop loop_to_destroy;
  ccol_mutex_lock(srv_engine_bundler.mutex);
  loop_to_destroy = srv_engine_bundler.reactor;
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  /* Quiesce every server that is still registered BEFORE this function
   * touches the idle sweep thread or the reactor itself. chttpsvr_engine_stop()
   * is the forced path. It can run this reaper while one or more servers are
   * still fully started, with live listen_reg and conn->reg registrations
   * into srv_engine_bundler.reactor. A teardown of the reactor first leaves
   * those registrations dangling. The next chttpsvr_stop() or _conn_close()
   * of the owning server then uses them. The graceful path, where the
   * refcount reaches zero, already guarantees that every registered server
   * quiesced and unregistered itself before this point. The call is a fast
   * no-op in that case. */
  _engine_force_stop_quiesce_all();
  /* Stop the idle sweep thread before the teardown of the reactor that the
   * sweep calls into through _conn_close and ccol_event_loop_remove. See the
   * doc comment of _idle_sweep_stop_if_running. */
  _idle_sweep_stop_if_running();
  if (loop_to_destroy) ccol_event_loop_destroy(loop_to_destroy);

  /* The backing array of servers_bundler.servers is a plain buffer from
   * realloc. It is not tied to the lifetime of any one server. Every server
   * that contributed a reference to this engine is already unregistered at
   * this point. The _engine_force_stop_quiesce_all call above did that. On
   * the graceful path, the chttpsvr_destroy() call of the server did it
   * before the refcount could reach zero. servers_bundler.count is therefore
   * always 0 here. The code resets servers_bundler.count explicitly anyway,
   * as a defensive measure, instead of an assumption. A count that nothing
   * enforces is a use-after-free; see the doc comment of
   * _engine_force_stop_quiesce_all. The code also frees the empty array
   * itself. Without that free, the array stays a reachable allocation for
   * the rest of the process. */
  ccol_mutex_lock(servers_bundler.mutex);
  free(servers_bundler.servers);
  servers_bundler.servers = NULL;
  servers_bundler.capacity = 0;
  servers_bundler.count = 0;
  ccol_mutex_unlock(servers_bundler.mutex);

  ccol_mutex_lock(srv_engine_bundler.mutex);
  srv_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
  srv_engine_bundler.stopping = false;
  /* The fallback logger that _engine_acquire installed has the same scope as
   * the lifetime of this reactor, and the code closes it here. Without that
   * close, a full engine stop leaves it as a reachable allocation for the
   * rest of the life of the process. A later chttpsvr_start() brings the
   * reactor back up with a fresh one. The logger of
   * chttpsvr_set_engine_logger is different: it stays open, and the next
   * engine logs through it again; see the field comment of user_log.
   *
   * log can legitimately still be NULL here. The allocation of the fallback
   * logger inside _engine_acquire can fail. That function then reaps the
   * engine through this same reaper path; see the comment of that function.
   * No logger is ever installed in that case. The code copies the handle
   * into a local instead of a log call right here. It also defers the
   * ccol_log_info call to after the ccol_mutex_unlock below; see the comment
   * of that call. A logger that an operator installs with
   * chttpsvr_set_engine_logger can be a synchronous sink of any slowness.
   * This mutex also guards every other chttpsvr_start, chttpsvr_stop,
   * _engine_acquire and _engine_release call in the process, not only this
   * reaper thread. old_logger stays valid for a log call and then a close
   * after the unlock. The code resets srv_engine_bundler.log to CLOG_INVALID
   * below, under the lock. No other thread can then see or touch this exact
   * clog handle after the mutex is unlocked. */
  clog old_logger = srv_engine_bundler.log;
  srv_engine_bundler.log = CLOG_INVALID;
  /* The logger of chttpsvr_set_engine_logger() stays open for the next
   * engine, and a concurrent chttpsvr_set_engine_logger() may close it the
   * moment this mutex is released. Its log call therefore runs here, under
   * the mutex, and only the fallback logger, which nothing else can reach
   * any more, takes the deferred path below. */
  if (old_logger && old_logger == srv_engine_bundler.user_log) {
    ccol_log_info(old_logger,
                  "The http server reactor engine has been destroyed");
    old_logger = CLOG_INVALID;
  }
  ccol_cond_var_broadcast(srv_engine_bundler.stopped_cv);
  ccol_mutex_unlock(srv_engine_bundler.mutex);

  if (old_logger) {
    ccol_log_info(old_logger,
                  "The http server reactor engine has been destroyed");
    clog_close(old_logger);
  }
  /* Hand this thread over for a join. Without this, the ordinary sequence
   * where an application destroys the last server and does nothing else
   * never joins it. Only two paths reach _join_reaper_if_needed_locked:
   * _engine_acquire, which a later chttpsvr_start runs, and
   * _engine_wait_until_stopped, which chttpsvr_engine_wait runs. Such an
   * application calls neither. A joinable thread that returned keeps its
   * stack mapping allocated until something joins it.
   *
   * The engine-stop watcher, which always runs, does the join. The spawner
   * of this thread does not. This is what keeps chttpsvr_destroy()
   * asynchronous. Destroy still returns the moment the library spawns this
   * thread, exactly as the documentation says. chttpsvr_engine_wait() stays
   * the way to wait for the teardown.
   *
   * This must be the last statement, and it must take no lock. From the
   * instant the request becomes visible, the watcher may join this thread. A
   * join of a thread that still waits on srv_engine_bundler.mutex deadlocks
   * against whoever holds that mutex. The store also comes after the trailing
   * work on the logger above, on purpose. The join then blocks only for the
   * return of this thread. A watcher that never started leaves the request
   * unserviced. That case is a ccol_thread_create failure in the class of an
   * out-of-memory error. The backstop join in _cleanup_engine_stop_watcher
   * covers it. */
  _engine_request_reaper_join();
  return NULL;
}

#ifdef RUNNING_UNIT_TESTS
/* A delay, in ms, between the creation of the reaper and the record of its
 * thread in _spawn_reaper, so that a test can let the reaper run to its end
 * inside that window. */
static _Atomic unsigned g_spawn_reaper_delay_ms_for_tests = 0;
void _chttpsvr_set_spawn_reaper_delay_for_tests(unsigned ms) {
  atomic_store(&g_spawn_reaper_delay_ms_for_tests, ms);
}
#endif /* RUNNING_UNIT_TESTS */

static void _spawn_reaper(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  /* The thread is created, and recorded, under srv_engine_bundler.mutex. The
   * reaper takes that mutex before it can finish, so it cannot hand itself
   * over for a join, and a later start and release cannot spawn a second
   * reaper, before this call recorded it. A record after the unlock lets
   * both happen first: the join request finds nothing to join, and the
   * record of a second reaper is then overwritten, so one thread is never
   * joined. */
  ccol_mutex_lock(srv_engine_bundler.mutex);
  ccol_thread_id_t reaper;
  if (ccol_thread_create(reaper, _engine_reaper_fn, NULL) != 0) {
    ccol_mutex_unlock(srv_engine_bundler.mutex);
    /* There is no safer fallback than a run of the reaper inline. This is an
     * out-of-memory class failure. There is nothing to join after the call,
     * because the reaper ran to completion synchronously. */
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
  ccol_mutex_unlock(srv_engine_bundler.mutex);
}

/* The definition is further below, beside the rest of the signal-safe
 * machinery of the engine-stop watcher. The declaration is here because this
 * file defines _engine_acquire, the one call site, first. */
static void _engine_stop_watcher_ensure_started_locked(void);

/* This function sets out_currently_stopping unconditionally. That is its
 * very first action after it locks srv_engine_bundler.mutex. The value is
 * true only when a reap tears the shared reactor down at that moment. A reap
 * is forced through chttpsvr_engine_stop(), or graceful when the
 * chttpsvr_destroy() of the last other server drops reactor_refs to 0. In
 * that case this function returns at once. It does NOT block for that reap
 * to finish, and it never increments reactor_refs. The value is false on
 * every other path, on success and on a real failure. A caller therefore
 * never has to set it to false first.
 *
 * A blocking wait here, such as `while (stopping) ccol_cond_var_wait(...)`,
 * is correct only while the calling chttpsvr_start() holds no other resource
 * that a concurrent reap can wait on. chttpsvr_start() does hold exactly
 * such a resource for its whole duration: its own resolve pin
 * (pending_resolve_count). The claim of _quiesce_server_once() on this exact
 * server blocks on that pin before any of its real work. A reap that reaches
 * this server while a chttpsvr_start() call for it blocks in a wait here
 * deadlocks forever. The reap cannot finish, because it waits on the pin
 * that only this call can free. This call cannot finish, because it waits on
 * `stopping`, which only the very reap that it is stuck behind can clear.
 * The caller-side half of what this permits is to free the pin and retry
 * from the start, instead of a block here. See the comment of the retry loop
 * of chttpsvr_start(), at its "currently stopping" branch.
 *
 * The caller never reads the ccol_retval_t that comes back together with
 * *out_currently_stopping == true. That value has no meaning of its own. It
 * is always ccol_unexpected_failure, only so that the return statement has a
 * value of the right type. */
static ccol_retval_t _engine_acquire(bool *out_currently_stopping) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  /* This is the first step under the lock, before anything else in this
   * function. It guarantees that the signal-safe stop watcher is live for
   * the rest of this call. It is then also live for every later call in the
   * process that locks srv_engine_bundler.mutex. This closes the
   * self-deadlock window that the doc comment of chttpsvr_engine_stop(), and
   * the comment of g_engine_stop_watcher, describe. */
  _engine_stop_watcher_ensure_started_locked();
  *out_currently_stopping = false;
  if (srv_engine_bundler.stopping) {
    ccol_mutex_unlock(srv_engine_bundler.mutex);
    *out_currently_stopping = true;
    return ccol_unexpected_failure;
  }
  _join_reaper_if_needed_locked();

  if (!srv_engine_bundler.reactor) {
    /* The default is 1, and not an auto-detected CPU count. A benchmark
     * against a real HTTP workload chose that value; nobody assumed it. See
     * the doc comment of chttpsvr_set_engine_num_reactor_threads for the
     * full comparison and reasoning. One dedicated poller thread that also
     * runs every callback inline measures faster than dispatch threads at
     * the CPU count. Its latency is also more consistent. That holds for
     * plain HTTP traffic and for TLS traffic that reuses connections, which
     * is the common case for a population of well-behaved clients. Dispatch
     * on many threads only wins under a synthetic storm of TLS handshakes,
     * where every request opens a new connection and nothing is reused. Even
     * there the margin is moderate, not dramatic. */
    size_t nthreads = srv_engine_bundler.num_reactor_threads;
    if (nthreads == 0) nthreads = 1;
    srv_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    srv_engine_bundler.reactor = ccol_event_loop_create_with_mprocs(
        256, 4, nthreads, srv_engine_bundler.mprocs, &err);
    if (!srv_engine_bundler.reactor) {
      ccol_mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }

    if (!srv_engine_bundler.log && srv_engine_bundler.user_log)
      srv_engine_bundler.log = srv_engine_bundler.user_log;
    if (!srv_engine_bundler.log) {
      srv_engine_bundler.log =
          clog_open_fd_mp(2, CLOG_FATAL, NULL, srv_engine_bundler.mprocs);
      if (!srv_engine_bundler.log) {
        /* Roll the reactor that the code created just above back. A reactor
         * that keeps running with reactor_refs still at 0 leaks for the
         * lifetime of the process. Nothing ever calls _engine_release() to
         * reap it, because that happens only once a server contributes a
         * reference with success. A synchronous destroy here is safe, even
         * while the code still holds srv_engine_bundler.mutex. The analogous
         * fix in chttpclient for its deadline-sweep thread differs. Another
         * thread CAN contend for this mutex right now. The
         * async-signal-safe engine-stop watcher blocks on it like any other
         * locker, if a signal fires chttpsvr_engine_stop() in this exact
         * window. But that is ordinary contention and not a deadlock risk.
         * Nothing that waits on this mutex needs THIS thread to make further
         * progress before it can get the lock. What matters for the
         * synchronous join inside ccol_event_loop_destroy() below is that
         * nothing can call back INTO this mutex from under it. Only
         * chttpsvr_start() starts the sweep thread for the idle timeout, and
         * it does so strictly AFTER this function returns success. The
         * reactor threads of ccol_event_loop know nothing about
         * srv_engine_bundler. Nothing can therefore re-enter
         * _SRV_ENGINE_LOG or chttpsvr_set_engine_logger() from inside the
         * join and deadlock against the lock that this thread holds. Both of
         * those lock this same mutex. */
        ccol_event_loop_destroy(srv_engine_bundler.reactor);
        srv_engine_bundler.reactor = CCOL_EVENT_LOOP_INVALID;
        ccol_mutex_unlock(srv_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(srv_engine_bundler.log, "component", "http-server-engine");
    }

    /* This log call runs here, still under the lock. The code deliberately
     * does NOT defer it to after ccol_mutex_unlock, the way
     * _engine_reaper_fn defers its own "engine destroyed" log. That deferral
     * is safe there for one specific reason: that function resets
     * srv_engine_bundler.log to CLOG_INVALID under the lock, before it
     * unlocks. A concurrent chttpsvr_set_engine_logger() can then only see
     * and close the NEW logger that it installs, never the handle that the
     * reaper already copied. Here, srv_engine_bundler.log still holds this
     * exact logger after this block returns. It stays the live engine logger
     * until something replaces it. A deferral of this log call therefore
     * leaves a window. In that window a concurrent
     * chttpsvr_set_engine_logger() call can swap the logger out and
     * clog_close() it, before the deferred call runs. That call only
     * synchronizes through this same mutex, with no dependency on
     * reactor_refs or any other pin. The result is a real use-after-free on
     * the logger, not merely a lost log line. A deferral in the style of the
     * teardown log of the reaper opens exactly that race. This call is rare.
     * It fires only at the instant a fresh reactor is created, which is once
     * for each full stop cycle, and not on every ordinary chttpsvr_start().
     * The contention that a slow custom logger here can cause for the other
     * callers of this mutex is therefore minor. */
    ccol_log_info(srv_engine_bundler.log,
                  "New http server reactor engine has been created");
  }
  srv_engine_bundler.reactor_refs++;
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

static void _engine_release(void) {
  /* Every real caller runs on a server that already holds an engine
   * reference that it contributed. Only an earlier _engine_acquire() call of
   * that same server can take such a reference, so the ccol_call_once of
   * that call already fired. But this project has a rule. Put the guard in
   * every function that touches the primitive directly. Never depend on
   * reasoning about the call graph. This function therefore carries its own
   * guard and does not assume that order. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool should_reap = false;
  ccol_mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor_refs > 0) srv_engine_bundler.reactor_refs--;
  /* The !srv_engine_bundler.stopping guard matters for the case that
   * _engine_force_stop_quiesce_all creates. That function calls
   * _quiesce_server_once for every server that it quiesces, and that in turn
   * calls this function. It does so WHILE a reaper that
   * chttpsvr_engine_stop() spawned already runs. At that point
   * srv_engine_bundler.stopping is already true and
   * srv_engine_bundler.reactor is not yet NULL, because the reaper did not
   * destroy it yet. Without this guard, the release of the last reference
   * during that pass looks the same as the ordinary graceful case. In that
   * case the last server frees its reference. The code then spawns a second,
   * redundant reaper thread that races the one that already tears this same
   * reactor down. */
  if (srv_engine_bundler.reactor_refs == 0 && srv_engine_bundler.reactor &&
      !srv_engine_bundler.stopping) {
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) _spawn_reaper();
}

static void _engine_wait_until_stopped(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  while (srv_engine_bundler.reactor || srv_engine_bundler.stopping)
    ccol_cond_var_wait(srv_engine_bundler.stopped_cv, srv_engine_bundler.mutex);
  _join_reaper_if_needed_locked();
  ccol_mutex_unlock(srv_engine_bundler.mutex);
}

/* The real logic of the force-stop. It takes srv_engine_bundler.mutex. If a
 * reactor is live, it can go on and call ccol_thread_create() through
 * _spawn_reaper(). POSIX guarantees that neither pthread_mutex_lock nor
 * pthread_create is async-signal-safe. This function must therefore run only
 * on an ordinary thread, and never from a signal handler. See the comment of
 * g_engine_stop_watcher below for the signal-safe path that
 * chttpsvr_engine_stop() takes to reach this function. */
static void _engine_force_stop_now(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool should_reap = false;
  ccol_mutex_lock(srv_engine_bundler.mutex);
  /* This is the !srv_engine_bundler.stopping guard. It follows the identical
   * guard in _engine_release, and the doc comment there says why it exists.
   * Without it, a SECOND chttpsvr_engine_stop() call can arrive while a
   * reaper from a FIRST call is still in the middle of its teardown. In that
   * window srv_engine_bundler.stopping is already true and
   * srv_engine_bundler.reactor is not yet NULL. The reaper nulls it near its
   * very end, well after _engine_force_stop_quiesce_all,
   * _idle_sweep_stop_if_running and ccol_event_loop_destroy all run, which
   * can take a real amount of wall-clock time. Without the guard, that
   * second call spawns a SECOND reaper that captures the same live
   * ccol_event_loop handle. Both reapers then call
   * ccol_event_loop_destroy() on that one handle, each on its own. The
   * documented contract in cthreadcomm.h makes that unconditionally fatal,
   * with a ccol_fatal_err() and a SIGABRT. That holds both for a sequential
   * double destroy, where one already finished, and for two that overlap in
   * time. chttpsvr_engine_stop() is documented as the mechanism to wire into
   * a signal handler. A second SIGTERM or SIGINT can easily arrive before
   * the teardown of the first call finishes. So can a defensive double call
   * from the shutdown code of the application. That is ordinary, not a rare
   * corner case. This guard is what makes a repeated or overlapping
   * chttpsvr_engine_stop() call the documented safe no-op, instead of an
   * abort of the process. */
  if (srv_engine_bundler.reactor && !srv_engine_bundler.stopping) {
    srv_engine_bundler.reactor_refs = 0;
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) _spawn_reaper();
}

/* ========================================================================== */
/*                    SIGNAL-SAFE ENGINE-STOP WATCHER                         */
/* ========================================================================== */

/*
 * chttpsvr.h documents chttpsvr_engine_stop() as async-signal-safe, which
 * means it is safe to call from a signal handler. An application can
 * therefore install it, or a thin wrapper around it, as a handler for
 * SIGTERM or SIGINT. That gives a graceful shutdown. But
 * _engine_force_stop_now()
 * above is NOT async-signal-safe. It calls ccol_mutex_lock(), which uses a
 * plain, non-recursive pthread mutex. It also calls ccol_thread_create()
 * through _spawn_reaper(). POSIX lists neither as async-signal-safe, and
 * this is not a theoretical concern. Both are real, reachable
 * self-deadlocks. Other code also takes srv_engine_bundler.mutex:
 * _engine_acquire(), which chttpsvr_start() calls synchronously,
 * _engine_release(), which chttpsvr_destroy() and _quiesce_server_once call,
 * and chttpsvr_set_engine_logger, chttpsvr_set_engine_mem_mgmt_procs and
 * chttpsvr_set_engine_num_reactor_threads. Take a signal that arrives on the
 * very thread that is inside one of those calls. One example is a SIGTERM
 * that arrives in the middle of chttpsvr_start(). That is a realistic race
 * during the shutdown or rolling restart of a container. The
 * ccol_mutex_lock(srv_engine_bundler.mutex) call of the handler then
 * self-deadlocks against the lock that the same thread already holds. That
 * hangs the whole process, and only a SIGKILL ends it. That is the opposite
 * of the purpose of a handler for a graceful shutdown.
 *
 * The remedy moves every step that touches a mutex or ccol_thread_create off
 * the thread that calls chttpsvr_engine_stop(). Those steps run on this
 * dedicated watcher thread, which always runs. chttpsvr_engine_stop() itself
 * then does at most two operations, and POSIX guarantees that both are
 * async-signal-safe. The first is a lock-free atomic load, which checks that
 * the watcher is up. The second is sem_post(), the one synchronization
 * primitive that POSIX explicitly lists as async-signal-safe; see
 * ccol_semaphore_post() in common.h. The watcher thread blocks in
 * ccol_semaphore_wait(). That is an ordinary execution context and not a
 * signal context, so ccol_mutex_lock() and ccol_thread_create() are both
 * fine there. Once it wakes, it does the same work that
 * _engine_force_stop_now() does.
 */
static struct {
  ccol_semaphore_t sem;
  /* This flag is true once sem and thread are both fully live. It is the one
   * field that chttpsvr_engine_stop() itself reads. It reads it with a
   * lock-free atomic load, which is safe from a signal handler. A bool that
   * a mutex guards is not safe there. */
  _Atomic bool ready;
  ccol_thread_id_t thread;
  /* This flag guards the attempts to start the watcher. Only
   * _engine_stop_watcher_ensure_started_locked() touches it, and only while
   * it holds srv_engine_bundler.mutex. Only an ordinary thread calls that
   * function, in the end from _engine_acquire(), and never a signal handler.
   * The code does not use `ready` for this. A start that fails for a moment,
   * in the class of an out-of-memory error, must be open to a retry on a
   * later call. Without a separate flag, `ready` stays wedged at false
   * forever. */
  bool started;
  _Atomic bool exit_requested;
  /* This thread does two kinds of work, and each kind has its own request
   * flag. The code sets the flag before the matching ccol_semaphore_post().
   * One flag for each kind is what makes an extra post that something
   * already consumed harmless. The semaphore counts posts, but it cannot say
   * which request each post belongs to. With one "something happened" wakeup
   * instead, the thread can service a request to join the reaper, which
   * _engine_reaper_fn posts, as a force-stop. That tears a healthy engine
   * down. A flag that the code sets before its own post is always visible to
   * the iteration that consumes that post, or to an earlier one. No request
   * is lost either. */
  _Atomic bool stop_requested;
  /* The engine reaper sets this flag as its very last act. It asks this
   * thread to call ccol_thread_join() on it. See the comment of
   * _engine_reaper_fn at that store, and see _engine_join_finished_reaper
   * below. */
  _Atomic bool join_reaper_requested;
} g_engine_stop_watcher = {0};

/* Asks the watcher thread to join the engine reaper. The code raises the
 * request at the very end of _engine_reaper_fn. See that call for the
 * reason, and for why it must be the last statement of that function. This
 * function
 * raises the flag unconditionally. The flag of _engine_force_stop differs,
 * because the code raises that one only when a watcher exists to consume it.
 * The unconditional raise is what lets the backstop in
 * _cleanup_engine_stop_watcher see an open request that no watcher was ever
 * available to service. */
static void _engine_request_reaper_join(void) {
  atomic_store(&g_engine_stop_watcher.join_reaper_requested, true);
  if (atomic_load(&g_engine_stop_watcher.ready))
    ccol_semaphore_post(g_engine_stop_watcher.sem);
}

/* Joins the engine reaper thread once it finishes. Take an application that
 * destroys its last server and then neither starts another one nor calls
 * chttpsvr_engine_wait(). This join is what leaves no unjoined thread behind
 * for such an application. A joinable thread that returned keeps its stack
 * mapping and its glibc bookkeeping allocated until something joins it. That
 * is real address space, held for the rest of the life of the process. "No
 * caller happens to ask" is therefore not an acceptable outcome for the
 * teardown thread of the engine.
 *
 * The !stopping test is what makes the ccol_thread_join() below safe while
 * the code holds srv_engine_bundler.mutex. The reaper clears stopping in its
 * own final locked section, and it takes no lock at all after that. A reaper
 * that the code sees with stopping already false can therefore never block
 * on this mutex. The _engine_acquire and _engine_wait_until_stopped
 * functions already depend on that same invariant. They depend on it for
 * their own _join_reaper_if_needed_locked() calls, because both reach it
 * only after stopping is false. A join of a reaper that can still be in the
 * middle of its teardown deadlocks this thread against it. When stopping is
 * true, a fresh reap is already in flight. By construction that means
 * something joined the previous reaper before the code could spawn this one.
 * A skip is therefore not a missed join. The new reaper posts its own
 * request when it finishes. */
static void _engine_join_finished_reaper(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  if (!srv_engine_bundler.stopping) _join_reaper_if_needed_locked();
  ccol_mutex_unlock(srv_engine_bundler.mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for a regression test only. It is true while an engine
 * reaper thread exists that the library spawned and nothing joined yet. It
 * pins this property. Take an application that destroys its last server, and
 * then neither starts another one nor calls chttpsvr_engine_wait(). That
 * application still ends up with a false value here, and it makes no further
 * call of its own. See
 * _engine_join_finished_reaper above. A gate keeps this function and its
 * behavior out of a production build, in the same way as every other
 * white-box helper in this file. */
bool _chttpsvr_engine_reaper_unjoined_for_tests(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  bool unjoined = srv_engine_bundler.reaper_joinable;
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  return unjoined;
}
#endif /* RUNNING_UNIT_TESTS */

static void *_engine_stop_watcher_fn(void *arg) {
  (void)arg;
  for (;;) {
    ccol_semaphore_wait(g_engine_stop_watcher.sem);
    if (atomic_load(&g_engine_stop_watcher.exit_requested)) return NULL;
    if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
      _engine_join_finished_reaper();
    if (atomic_exchange(&g_engine_stop_watcher.stop_requested, false))
      _engine_force_stop_now();
  }
}

/* Starts the watcher thread the first time the library needs it. It retries
 * on every call until it succeeds. An out-of-memory failure here is
 * temporary, and it must not take away the ability of
 * chttpsvr_engine_stop() to act again forever. The caller must already hold
 * srv_engine_bundler.mutex, and it must be an ordinary thread and not a
 * signal handler. See _engine_acquire, its one call site. That function
 * calls it as the very first step after it takes the lock. The call comes
 * before any of its own work to create the reactor. This guarantees that the
 * watcher is live for the rest of that call. It is then also live for every
 * later call in the process that locks srv_engine_bundler.mutex. That closes
 * the self-deadlock window above. One small gap remains. It is a signal that
 * arrives in the few instructions before the first call of this function.
 * Nothing holds srv_engine_bundler.mutex at that point, because this is the
 * very first statement that takes it. The self-deadlock above therefore
 * cannot happen there either. A stop request that arrives in that sliver is
 * not yet actionable, because nothing runs yet for it to stop. The library
 * treats it as a documented no-op, exactly like any other call to
 * chttpsvr_engine_stop() before the engine ever starts. It does not defer
 * the request in silence
 * and replay it against a later, unrelated chttpsvr_start() call. Such a
 * replay would bring a long-forgotten stop request back against a server
 * that starts much later in the same process. */
static void _engine_stop_watcher_ensure_started_locked(void) {
  if (g_engine_stop_watcher.started) return;
  if (ccol_semaphore_init(g_engine_stop_watcher.sem, 0) != 0) return;
  if (ccol_thread_create(g_engine_stop_watcher.thread, _engine_stop_watcher_fn,
                         NULL) != 0) {
    ccol_semaphore_destroy(g_engine_stop_watcher.sem);
    return;
  }
  g_engine_stop_watcher.started = true;
  atomic_store(&g_engine_stop_watcher.ready, true);
}

/* Joins the watcher thread at the exit of the process. It also joins any
 * engine reaper that the watcher did not reach. Without this, the valgrind
 * pass of "make memtest" reports either thread stack as "possibly lost". The
 * plain read of `started` and `thread` here, with no mutex, is safe. It is
 * safe whether or not a chttpsvr that the application created is still live
 * at the exit of the process. The _cleanup_chttpsvr_router_shells and
 * _cleanup_chttpsvr_slot_table functions differ. Each one checks the
 * liveness of every handle before it frees anything, for exactly that
 * reason. Here, the code
 * writes both fields once for the life of the process. It sets them together
 * exactly once, the first time any chttpsvr acquires the shared engine. See
 * _engine_stop_watcher_ensure_started_locked, the only writer of this
 * thread. There is no operation anywhere that un-starts it. A plain read
 * that races that one write sees either the state before the start, which is
 * false and unset, or the fully started state. It never sees a torn state or
 * a state that went back. */
__attribute__((destructor)) static void _cleanup_engine_stop_watcher(void) {
  if (g_engine_stop_watcher.started) {
    atomic_store(&g_engine_stop_watcher.exit_requested, true);
    ccol_semaphore_post(g_engine_stop_watcher.sem);
    ccol_thread_join(g_engine_stop_watcher.thread);
    /* The code clears this flag before it destroys the semaphore. It does
     * not leave the flag true. The async-signal-safe contract means that a
     * signal handler can call chttpsvr_engine_stop() at any point during the
     * exit of the process. The whole implementation of that call,
     * _engine_force_stop, is a bare `if (ready) sem_post(sem)`. That includes
     * the narrow window after this destructor runs and before the process
     * goes away. This library does not control the order of destructors and
     * atexit handlers across the shared objects of a process. See the
     * standing caution about that in this file. A flag left true lets such a
     * racing sem_post() target a semaphore that this same destructor already
     * destroyed. That is a real violation in the class of a use-after-free,
     * on a primitive, and not merely a lost stop request. */
    atomic_store(&g_engine_stop_watcher.ready, false);
    ccol_semaphore_destroy(g_engine_stop_watcher.sem);
  }
  /* This is the backstop for the cases that the watcher cannot cover. It
   * runs whether or not the library ever started a watcher. There are two
   * such cases. In the first, a reaper raised its join request while no
   * watcher existed to service it. That happens when the
   * ccol_thread_create() of the watcher fails with an out-of-memory class
   * error, which does not stop _engine_acquire from bringing an engine up.
   * In the second, a reaper raised the request in the sliver between the
   * last semaphore wait of the watcher and the exit request just above. The
   * request flag gates this backstop, and it does not run unconditionally.
   * It therefore never touches a reaper that is still in the middle of its
   * teardown. The !stopping test inside _engine_join_finished_reaper makes
   * the same guarantee a second time. The library may spawn a fresh reap
   * after the code raises the flag. */
  if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
    _engine_join_finished_reaper();

  /* The logger of chttpsvr_set_engine_logger() outlives every engine, so the
   * exit of the process is where it closes. Every reaper is joined above, so
   * none of them still reads it. An engine that still runs at the exit keeps
   * it, because its reactor threads can still log through it. */
  if (srv_engine_bundler.user_log) {
    ccol_mutex_lock(srv_engine_bundler.mutex);
    clog user_log = CLOG_INVALID;
    if (!srv_engine_bundler.log) {
      user_log = srv_engine_bundler.user_log;
      srv_engine_bundler.user_log = CLOG_INVALID;
    }
    ccol_mutex_unlock(srv_engine_bundler.mutex);
    if (user_log) clog_close(user_log);
  }
}

/* The real public entry point. See the comment of g_engine_stop_watcher
 * above for why this function is deliberately only this one
 * async-signal-safe operation. It has no ccol_mutex_lock and no
 * ccol_thread_create of its own, direct or indirect. When `ready == false`,
 * this function does nothing. That state means that no engine ever started
 * in this process. For a very narrow window it can also mean that one starts
 * for the first time right now. This matches the documented contract of this
 * function, which is safe to call before the reactor ever starts. See the
 * comment of _engine_stop_watcher_ensure_started_locked for why the library
 * does not defer that request and replay it later. */
static void _engine_force_stop(void) {
  if (atomic_load(&g_engine_stop_watcher.ready)) {
    /* The code sets this flag inside the `ready` test, and not before it. A
     * flag that it raises while no watcher exists to consume it sits there
     * until some unrelated, much later post wakes the watcher. That brings a
     * long-forgotten stop request back against a server that starts well
     * afterwards. A lock-free atomic store is as async-signal-safe as the
     * atomic load above it. */
    atomic_store(&g_engine_stop_watcher.stop_requested, true);
    ccol_semaphore_post(g_engine_stop_watcher.sem);
  }
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for a regression test only. It locks
 * srv_engine_bundler.mutex, which is exactly the critical section that the
 * doc comment of chttpsvr_engine_stop() says a signal handler must be safe
 * to interrupt. It then delivers `sig` to the calling thread synchronously,
 * with raise(3). POSIX says that raise(3) in a program with many threads is
 * equivalent to pthread_kill(pthread_self(), sig). The installed handler of
 * sig, if there is one, therefore runs to completion on this same thread,
 * with the mutex still held, before raise() returns. This reproduces a
 * signal that arrives on a thread that already holds
 * srv_engine_bundler.mutex, deterministically and with no dependency on real
 * timing. This hook is not vacuous. Take a handler for `sig` from the caller
 * that calls chttpsvr_engine_stop(). If that call ever reached
 * ccol_mutex_lock or ccol_thread_create directly, this call deadlocks. See
 * g_engine_stop_watcher above for why it must not reach them. That deadlock
 * is exactly what a real SIGTERM handler does when an application installs
 * it in the documented way of this module. A gate keeps this function and
 * its behavior out of a production build. */
void _chttpsvr_test_hold_engine_mutex_and_signal_self(int sig) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  raise(sig);
  ccol_mutex_unlock(srv_engine_bundler.mutex);
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
/* The definition is near __chttpsvr_destroy, far below. The declaration is
 * here because this file defines _conn_on_removed well before it. That
 * function must call this one, and it can do so long after the caller of
 * __chttpsvr_destroy returns. See the field comment of
 * lifetime_refs in struct chttpserver for why this is a separate step that
 * the library can defer. */
static void _chttpsvr_finish_destroy(struct chttpserver *raw);
static void _retired_pools_reap(struct chttpserver *srv, bool wait_all);

/* The slow-client machinery; see the section "SLOW CLIENTS" further down. */
static void _timespec_add_ms(struct timespec *ts, unsigned ms);
static void _slow_clock(struct timespec *ts);
static void _mem_release(chttpsvr_conn_t *conn);
static void _sweep_parked_waits(struct chttpserver *srv,
                                struct timespec now_slow);
static bool _parked_expired(struct chttpserver *srv, chttpsvr_conn_t *c,
                            struct timespec now, struct timespec now_slow);
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

/* Computes the target capacity for a pointer array that grows by a doubling
 * and that plain realloc backs. The result is cap*2, or `initial` the first
 * time, when cap is 0. The result is 0 when the doubling itself overflows
 * size_t, or when the multiplication by elem_size that the caller is about
 * to do overflows it. The caller must treat a 0 result as "no growth is
 * possible now". Every caller in this file already has a graceful,
 * already-tested path for an ordinary realloc that returns NULL when it runs
 * out of memory. Each one treats a 0 result from this function in the same
 * way; see the comments of _servers_register and
 * _chttpsvr_router_shell_register. Every registry in this file that grows by
 * a doubling and uses plain malloc and realloc shares this helper. Those
 * registries do not use the helpers of this module that take a
 * ccol_memmgmt_procs_t. This follows the established guard idiom of this
 * project, which is relative to SIZE_MAX. The _router_add_route,
 * chttpsvr_subrouter, chttpsvr_resp_set_header and _parse_qparams functions
 * use the same idiom. They use _ccol_mem_realloc in place of plain realloc.
 * This function is deliberately pure and touches no global state. A
 * white-box test can therefore drive it directly with a fake cap. Such a
 * test does not
 * need to grow one of the real process-wide registries of this file to an
 * extreme size; see _chttpsvr_doubling_growth_cap_for_tests below. */
static size_t _doubling_growth_cap(size_t cap, size_t elem_size,
                                   size_t initial) {
  if (cap == 0) return initial;
  if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / elem_size) return 0;
  return cap * 2;
}

#ifdef RUNNING_UNIT_TESTS
/*
 * A white-box helper for the tests. It gives _doubling_growth_cap directly.
 * It is not part of the public API. A gate keeps this symbol out of a
 * production build of libccollections.so, in the same way as every other
 * white-box helper in this file.
 */
size_t _chttpsvr_doubling_growth_cap_for_tests(size_t cap, size_t elem_size,
                                               size_t initial) {
  return _doubling_growth_cap(cap, elem_size, initial);
}
#endif /* RUNNING_UNIT_TESTS */

/* Registers a server so that the idle sweep thread also walks its list of
 * idle connections. chttpsvr_start calls this function. That function does
 * not run only once for each server: it runs again on every restart cycle of
 * a stop and a start. This function must therefore be idempotent, so it
 * skips the add when srv is already present. Without that check, a srv that
 * goes through N restart cycles appears in servers_bundler.servers N+1
 * times. _servers_unregister below removes one occurrence only. It therefore
 * leaves N stale, dangling pointers behind once __chttpsvr_destroy frees
 * srv. That is a real use-after-free, and the idle sweep thread reads it on
 * its very next pass. valgrind catches it through
 * restart_races_live_keep_alive_connection_is_safe, which restarts one srv 5
 * times before it destroys it. */
#ifdef RUNNING_UNIT_TESTS
/* Makes the next _servers_register() call that has to add a server fail, as
 * an allocation failure of its growth does. */
static _Atomic bool g_servers_register_fail_for_tests = false;
void _chttpsvr_fail_next_servers_register_for_tests(void) {
  atomic_store(&g_servers_register_fail_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

/* Returns false when srv is not registered and cannot be, because the array
 * cannot grow. */
static bool _servers_register(struct chttpserver *srv) {
  /* _engine_globals_init initializes servers_bundler.mutex, and
   * srv_engine_bundler.once guards that init. chttpsvr_start() deliberately
   * calls this function BEFORE its own _engine_acquire() call; see the
   * comment at that call site. This function can therefore be the very first
   * thing in the whole process to touch servers_bundler.mutex. That happens
   * on a plain sequence of a construct, a register_handler and a start, that
   * never calls chttpsvr_set_engine_logger,
   * chttpsvr_set_engine_mem_mgmt_procs,
   * chttpsvr_set_engine_num_reactor_threads, chttpsvr_engine_wait or
   * chttpsvr_engine_stop first. The code must not skip this guard because of
   * reasoning about the call graph. One such reason is "some other function
   * that already has a guard always runs first". See the conventions for
   * ccol_mutex_t in common.h. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      ccol_mutex_unlock(servers_bundler.mutex);
      return true;
    }
  }
  bool room = servers_bundler.count < servers_bundler.capacity;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_servers_register_fail_for_tests, false)) {
    ccol_mutex_unlock(servers_bundler.mutex);
    return false;
  }
#endif /* RUNNING_UNIT_TESTS */
  if (!room) {
    /* The growth is safe against an overflow, like every other growable
     * array in this file. A failed growth fails the registration, and
     * chttpsvr_start() then fails and undoes what it set up. A server that
     * ran outside this array would never be swept, so none of its idle,
     * header-phase or parked limits would apply, and an engine force-stop
     * would tear the reactor down under it without quiescing it first. */
    size_t new_cap = _doubling_growth_cap(servers_bundler.capacity,
                                          sizeof(struct chttpserver *), 8);
    struct chttpserver **nn = new_cap > 0
                                  ? (struct chttpserver **)realloc(
                                        servers_bundler.servers,
                                        new_cap * sizeof(struct chttpserver *))
                                  : NULL;
    if (!nn) {
      ccol_mutex_unlock(servers_bundler.mutex);
      return false;
    }
    servers_bundler.servers = nn;
    servers_bundler.capacity = new_cap;
  }
  servers_bundler.servers[servers_bundler.count++] = srv;
  ccol_mutex_unlock(servers_bundler.mutex);
  return true;
}

/* The real removal. It sits in its own function for one reason. The
 * child-side fixup of _chttpsvr_atfork_release_impl can then do it without a
 * second lock of servers_bundler.mutex. See the doc comment of that
 * function. That fixup
 * already holds the mutex for the whole duration of its walk over the slots,
 * and it inherits it locked from _chttpsvr_atfork_prepare. The caller must
 * already hold servers_bundler.mutex. This function is idempotent, and it
 * does nothing when srv is not present. */
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
  /* Every real caller runs only after _servers_register already fired this
   * same guard for this srv. But this project has a rule. Put the guard in
   * every function that touches the primitive directly. Never depend on
   * reasoning about the call graph. This function therefore carries its own
   * guard and does not assume that order. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(servers_bundler.mutex);
  _servers_unregister_locked(srv);
  ccol_mutex_unlock(servers_bundler.mutex);
}

/* Undoes a _servers_register() call and an engine reference that the library
 * took. It runs for a chttpsvr_start() call that registered raw and that may
 * have acquired the shared engine. Such a call then failed further down in
 * the same call. The setup of the listen socket can fail there, and so can
 * ccol_event_loop_add. A failed start therefore leaves raw in exactly the
 * state that it was in before the call. Without this function, the start
 * silently keeps the shared reactor alive. It also keeps raw registered with
 * the sweep of the idle timeout, until some later chttpsvr_destroy() call
 * reclaims it. The application may never make that call. Take an application
 * that checks the return value of chttpsvr_start() and, on a failure, calls
 * chttpsvr_engine_wait() before it ever calls chttpsvr_destroy() on the
 * failed handle. The header of this module does not warn against that order.
 * Without this function, such an application blocks forever, because nothing
 * ever drops the engine reference of this server.
 *
 * It is safe to call this function while the chttpsvr_start() call of this
 * thread still holds its resolve pin on raw. That means before the
 * _chttpsvr_resolve_unpin(raw) below. _servers_unregister is idempotent. A
 * concurrent _quiesce_server_once call for raw, from __chttpsvr_destroy or
 * from _engine_force_stop_quiesce_all, could otherwise race to do this same
 * cleanup. But it cannot get past its own wait on pending_resolve_count
 * until this thread frees its pin, which always happens strictly after this
 * function returns. There is therefore no window where two callers touch
 * raw->contributed_to_engine or servers_bundler.servers for the same raw at
 * the same time.
 *
 * acquired_here must be the exact `need_acquire` value that chttpsvr_start()
 * itself computed earlier in the same call. That value says whether this
 * specific call is the one that moved raw from not-yet-registered with no
 * engine reference, to registered with a reference. The other case is a
 * RESTART of a server that an earlier, successful chttpsvr_start() call
 * already registered and that already holds its engine reference.
 * chttpsvr_stop() never unregisters the server and never frees that
 * reference, exactly so that a later restart can skip a second acquire. Only
 * the call that took them may undo them. A failed restart that ran this
 * function unconditionally unregisters and frees a server that legitimately
 * still holds a reference and is still registered. That desynchronizes the
 * bookkeeping of servers_bundler. Take the case where this server holds the
 * last reference. It then also risks a teardown of the shared reactor while
 * the live idle and keep-alive connections of raw still hold registrations
 * into it. chttpsvr_stop() only closes the listener and never the
 * connections that exist. This is exactly the ordering hazard that the doc
 * comment of _quiesce_server_once describes and that it prevents. A failed
 * restart must instead leave raw in precisely the state that it was in
 * before this call began. That state is registered, still holding the engine
 * reference, and not started. That is the same state that a plain
 * chttpsvr_stop() with no restart leaves it in. */
static void _chttpsvr_undo_start_registration(struct chttpserver *raw,
                                              bool acquired_here) {
  if (!acquired_here) return;
  _servers_unregister(raw);
  bool release = false;
  ccol_mutex_lock(raw->mutex);
  if (raw->contributed_to_engine) {
    raw->contributed_to_engine = false;
    release = true;
  }
  ccol_mutex_unlock(raw->mutex);
  if (release) _engine_release();
}

#define _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS 1000
/* The size of the scratch buffer on the stack into which a teardown pass
 * (_close_all_idle_connections, _mem_wait_flush, _stream_queue_flush) and
 * _mem_admit claim connections under a lock, so that the close or the
 * dispatch runs outside that lock. Each of them wraps the batch in an outer
 * loop, so it handles its whole list. The sweep needs no buffer; see
 * _sweep_server. */
#define _CHTTPSVR_IDLE_CLOSE_BATCH 64

/* Resumes the listener registration of srv when _listener_on_readable paused
 * it because the server was at the capacity of max_connections; see the
 * comment of that function. It does nothing unless a cap is configured and
 * current_connections dropped back below it after the pause.
 *
 * The per-server loop of _idle_sweep_fn below calls this unconditionally,
 * once for each sweep tick. idle_timeout_ms does not gate it. _conn_free
 * never calls it synchronously the moment a connection closes. A direct call
 * to ccol_event_loop_resume from _conn_free, right after its
 * atomic_fetch_sub of current_connections, is immediate and correct on its
 * own. But together with the teardown of __chttpsvr_destroy it is a real
 * use-after-free, and ThreadSanitizer reports it as one. The
 * _drain_and_close_all_connections function polls current_connections down
 * to zero; see its own comment. That poll is its ONLY signal that no
 * teardown of a connection still needs srv. It then frees srv. A second,
 * later access to srv, such as a resume call, reopens the class of race that
 * the poll loop closes. That is any access after the exact atomic operation
 * that the signal depends on. A call from the sweep thread of the idle
 * timeout instead uses the established, TSan-clean pattern of that thread.
 * That pattern is how the sweep thread touches the state of a registered
 * server. It walks servers_bundler.servers under servers_bundler.mutex. This
 * is safe because the library only removes a server from that list under the
 * same lock. It does so before it frees any of the state of that server. See
 * the call site of _servers_unregister
 * in _quiesce_server_once. That call runs before
 * _drain_and_close_all_connections even starts. By that point
 * _chttpsvr_stop_internal already removed the registration of the listener
 * in full, so there is nothing left to resume. The cost is that a paused
 * listener resumes within at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS, which is
 * 1 second, after a slot becomes free, instead of almost at once. That is an
 * acceptable trade for a documented coarse knob for admission control. The
 * doc comment of chttpsvr_config_t.max_connections promises no pickup
 * latency below a second. It is also far better than the busy loop at 100
 * percent CPU that this whole mechanism prevents. */
static void _listener_resume_if_capacity_freed(struct chttpserver *srv) {
  size_t cap = atomic_load(&srv->max_connections);
  if (!cap || atomic_load(&srv->current_connections) >= cap) return;
  ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* Resumes the listener registration of srv when _listener_on_readable_impl
 * paused it for a resource exhaustion in accept4(), or for an allocation
 * failure after the accept. See the field comment of
 * listener_paused_for_resource_pressure. It does nothing unless that flag is
 * set. This function is deliberately independent of
 * _listener_resume_if_capacity_freed just above, and the two are not one
 * combined check. The two reasons for a pause are orthogonal. A listener
 * that is paused at the capacity of max_connections must stay paused for as
 * long as current_connections says so. This holds whatever the state of this
 * flag is. The reverse also holds. This flag must keep the listener paused
 * whatever current_connections says. One flag or one condition for both
 * risks a wrong resume. A resume for one reason can then fire, or the code
 * can suppress it, because of the state of the other reason. See the
 * convention of this file: verify
 * that the value space of each flag truly excludes the other before a merge.
 * atomic_exchange reads and clears the flag in one step. A fresh pause, from
 * a new failure on the very next dispatch, can race this resume. It can land
 * after the exchange and before ccol_event_loop_resume runs below. The
 * resume below then un-pauses a registration that the reactor thread
 * legitimately paused a moment ago. That is harmless and it corrects itself.
 * That same fresh dispatch already set the flag back to true. The very next
 * sweep tick repeats the cycle. More likely, an immediate new dispatch from
 * the backlog, which is not empty, repeats it sooner. Nothing stays stuck
 * unpaused, and nothing stays stuck without a resume. */
static void _listener_resume_if_resource_pressure_cleared(
    struct chttpserver *srv) {
  if (!atomic_exchange(&srv->listener_paused_for_resource_pressure, false))
    return;
  ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
}

/* Returns true when a connection counts as timed out for the idle timeout,
 * and false otherwise. It judges the last activity of that connection, at
 * last_activity, against now and idle_ms. idle_ms is the resolved value of
 * idle_timeout_ms or read_timeout_ms. The caller already guarantees that it
 * is not zero.
 * This is a small, pure function of its own. That matches the established
 * convention of this file: a pure helper plus an accessor under
 * RUNNING_UNIT_TESTS. _doubling_growth_cap, _accept_errno_is_transient and
 * _accept_errno_is_resource_exhaustion already use it. The purpose is that a
 * test can drive it directly with a synthetic, adversarial pair of now and
 * last_activity. Such a test does not need to win a real scheduling race
 * against a live sweep thread.
 *
 * The type is long long and not long. This follows the same reasoning as
 * _shrink_timeout_to_deadline a few functions up in this file, and there are
 * two independent reasons. The first is overflow. idle_ms is unsigned, and
 * now and last_activity can in principle be very far apart. The
 * multiplication below must therefore not overflow a 32-bit long on an ILP32
 * build, which this project builds and tests in CI. The second is the sign.
 * The caller, _idle_sweep_fn, captures now one time. It does so before it
 * takes idle_mutex and before it looks at any specific connection. The
 * _idle_list_add function refreshes last_activity from a DIFFERENT thread
 * every time a connection lands back in the idle list. That includes the
 * ordinary case of a keep-alive request that finishes concurrently with this
 * exact sweep tick. A connection can gain fresh activity inside one window.
 * That window runs from the `now` snapshot of the sweep to the moment the
 * sweep reaches that connection under idle_mutex. That
 * window is brief, but such a connection then has last_activity > now. That
 * is a legitimate and expected outcome of this design under ordinary
 * concurrent load, and not a bug from a skewed clock. The elapsed value must
 * therefore be a signed quantity, and the code must check it for a negative
 * result BEFORE it compares it against idle_ms. Without that check, the
 * library evicts a connection that just became idle, instead of a correct
 * result of zero or negative elapsed idle time. A bare `(unsigned
 * long)elapsed_ms >= idle_ms` silently wraps a small negative elapsed_ms to
 * a huge unsigned value. That value is `>= idle_ms` for any realistic
 * timeout. This closes the same class of defect that
 * _shrink_timeout_to_deadline avoids. */
static bool _conn_idle_timed_out(struct timespec now,
                                 struct timespec last_activity,
                                 unsigned idle_ms) {
  long long elapsed_ms =
      (long long)(now.tv_sec - last_activity.tv_sec) * 1000LL +
      (long long)(now.tv_nsec - last_activity.tv_nsec) / 1000000LL;
  if (elapsed_ms < 0) return false;
  return (unsigned long long)elapsed_ms >= (unsigned long long)idle_ms;
}

/* Says whether conn spent more than max_ms in the reactor-owned phase of its
 * current request. That phase is the TLS handshake plus the read of the
 * request headers. The measure starts at conn->header_phase_start.
 *
 * This is a TOTAL bound, and that is the whole purpose of it. The library
 * refreshes last_activity, which _conn_idle_timed_out measures against, for
 * every chunk of bytes that it feeds to the parser. It also refreshes it for
 * every return to the idle list. last_activity therefore only measures the
 * GAP between one piece of activity and the next. A peer that sends one byte
 * of its header block every few seconds resets that gap forever and never
 * finishes the block. It holds a file descriptor, an epoll registration and
 * a whole chttpsvr_conn_t for as long as it likes, at a very small cost in
 * bandwidth. That struct holds the 8 KiB line buffer of the parser. This
 * goes on until the server reaches max_connections and the listener stops
 * every accept. max_header_bytes bounds the bytes that such a peer may send,
 * but it does not bound the time that the peer may take. The peer never
 * comes near that cap, because it never completes the block. This function
 * is the header-phase counterpart of max_body_read_duration_ms, which closes
 * the same trickle-forever loophole for the body phase.
 *
 * It uses the same helper with a signed elapsed value that
 * _conn_idle_timed_out uses, for the same reason. The library takes
 * header_phase_start on a different thread from the `now` of the sweep. The
 * elapsed value can therefore legitimately be negative, and the code must
 * never compare it as unsigned. */
static bool _conn_header_phase_expired(struct timespec now,
                                       const chttpsvr_conn_t *conn,
                                       unsigned max_ms) {
  if (!conn->header_phase_active) return false;
  return _conn_idle_timed_out(now, conn->header_phase_start, max_ms);
}

/* Starts the clock of the reactor-owned phase for the request that conn
 * reads now. If the clock already runs, it leaves it alone. The function is
 * idempotent inside one request, and the first call wins. The budget
 * therefore runs from the first step of the handshake, or from the first
 * byte of the header block. It starts at whichever comes first. Every
 * later step of the same request measures against that same instant, and no
 * step pushes the deadline out. */
static void _conn_header_phase_arm(chttpsvr_conn_t *conn,
                                   const struct timespec *now) {
  if (conn->header_phase_active) return;
  conn->header_phase_start = *now;
  conn->header_phase_active = true;
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for the tests. It gives the pure decision of the idle
 * timeout directly. The real race is a concurrent _idle_list_add that
 * refreshes last_activity inside one window. That window runs from the `now`
 * snapshot of the sweep to the moment the sweep looks at this exact
 * connection. A deterministic
 * reproduction of that race from a test needs a win in a scheduling race
 * against a live sweep thread. This hook lets a test build the adversarial
 * pair of now and last_activity directly instead. A gate keeps this symbol
 * out of a production build, in the same way as every other white-box helper
 * in this file. */
bool _chttpsvr_conn_idle_timed_out_for_tests(struct timespec now,
                                             struct timespec last_activity,
                                             unsigned idle_ms) {
  return _conn_idle_timed_out(now, last_activity, idle_ms);
}
#endif /* RUNNING_UNIT_TESTS */

/* One tick of the sweep for one server. _idle_sweep_fn calls it for every
 * registered server, with srv pinned. It enforces the idle and header-phase
 * limits of the connections that the reactor owns, and the limits of the
 * connections that wait with no thread: a parked body or response, a request
 * that waits for body memory, and a streaming request that waits for a
 * thread. See _parked_expired and _sweep_parked_waits. */
static size_t _sweep_server(struct chttpserver *srv, struct timespec now) {
  /* These two calls are unconditional. The code must check a paused
   * listener for every registered server on every tick, whether or not the
   * idle timeout itself is turned on. This tick is the only trigger of this
   * mechanism. */
  _listener_resume_if_capacity_freed(srv);
  _listener_resume_if_resource_pressure_cleared(srv);

  unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
  unsigned hdr_ms = atomic_load(&srv->max_header_read_duration_ms);
  struct timespec now_slow;
  _slow_clock(&now_slow);

  /* Two lists hold the connections that no thread owns. The idle list holds
   * the ones that wait for the headers of a request, which the idle timeout
   * and max_header_read_duration_ms judge; with both limits off the sweep
   * does not walk it at all. The parked list holds the ones that wait for
   * the rest of a body or for room to write a response, which
   * _parked_expired judges; the sweep walks it on every tick, so its cost is
   * proportional to the parked connections alone. This sweep is where the
   * library enforces every one of those limits, for two reasons. First, every
   * connection that no thread owns and that waits for its socket is reachable
   * from exactly here. Second, the unlink of a candidate under idle_mutex right
   * here is already the claim. That claim stops a closer and a concurrent
   * dispatch from both owning the same connection; see _idle_list_try_claim.
   *
   * The code collects the expired connections under idle_mutex and acts on
   * them outside the lock. _conn_close calls ccol_event_loop_remove in the
   * end, and a 408 goes through the reject pool; neither may run while the
   * code holds a lock that a callback of that removal can also need. A
   * parked body gets 408 Request Timeout, because no response byte went out
   * yet. A parked response is cut off and closed.
   *
   * One tick takes EVERY expired connection of both lists, with no cap. A
   * cap per tick turns the sweep into a drain of a fixed number of
   * connections a second, which a server whose clients leave keep-alive
   * connections idle outgrows: expired connections then pile up faster than
   * the sweep closes them, until the listener reaches max_connections. The
   * collection needs no buffer: an unlinked connection belongs to this tick
   * alone, so its own link field of the list that it left chains it into a
   * private list of this tick. That link is read before the connection is
   * acted on, because a close frees it and a 408 can park it again. */
  chttpsvr_conn_t *idle_close = NULL;   /* chained through idle_next */
  chttpsvr_conn_t *parked_close = NULL; /* chained through parked_next */
  chttpsvr_conn_t *parked_reject = NULL;
  size_t expired_n = 0, lingers_n = 0;
  size_t visited = 0;
  ccol_mutex_lock(srv->idle_mutex);
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
    if (_parked_expired(srv, c, now, now_slow)) {
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
  ccol_mutex_unlock(srv->idle_mutex);
  /* The end of a lingering close is its ordinary end, and not a timeout
   * of the client, so it is not logged. */
  if (expired_n > lingers_n) {
    _SRV_ENGINE_LOG(ccol_log_info, "timeout closing %zu connection(s)",
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
  /* The closes above can have freed room under max_connections; the
   * listener does not wait for the next tick to take it. */
  if (expired_n) _listener_resume_if_capacity_freed(srv);
  /* A pool set that a restart retired and whose drain finished is joined
   * here, so that its thread does not wait for the next restart or the
   * destroy. */
  _retired_pools_reap(srv, false);
  return visited;
}

static void *_idle_sweep_fn(void *arg) {
  (void)arg;
  /* Every function that directly touches servers_bundler.mutex or the state
   * of srv_engine_bundler carries this guard itself. It does not depend on
   * reasoning about the call graph, such as "this thread starts only after
   * the caller of _idle_sweep_start_if_needed already fired the guard". See
   * the standing rule of this project on exactly this point. In practice the
   * guard does nothing here, because every real caller already guarantees
   * that the init ran before the first iteration of this thread. But a
   * future call path that starts this thread earlier otherwise brings back
   * the same class of bug that the rule prevents, in silence. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
    struct timespec ts = {
        .tv_sec = _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS / 1000,
        .tv_nsec = (_CHTTPSVR_IDLE_SWEEP_INTERVAL_MS % 1000) * 1000000L};
    nanosleep(&ts, NULL);
    if (atomic_load(&idle_sweep_bundler.stop_flag)) break;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    /* Take a snapshot of every registered server and pin each one, while the
     * code still holds servers_bundler.mutex. The pin uses
     * servers_bundler_pins, in the same pin-then-release pattern as
     * _engine_force_stop_quiesce_all. See the comment of that field for why
     * this needs a dedicated counter and not pending_resolve_count. The code
     * then unlocks the mutex, before any of the real per-server work below.
     * That work can mean a real close() and a TLS teardown for every
     * connection that expired on this tick. That is slow next to the cost
     * of a global lock. chttpsvr_start(), chttpsvr_destroy() and
     * chttpsvr_engine_stop() also need servers_bundler.mutex for every OTHER
     * registered server in the process, not only the ones whose connections
     * this tick closes. A lock held across that work therefore serializes
     * those calls behind whatever idle-connection churn this tick finds, for
     * no reason. The snapshot array itself uses the plain default allocator.
     * The backing storage of servers_bundler.servers does the same. Both are
     * decoupled from the custom allocator of any one server. */
    ccol_mutex_lock(servers_bundler.mutex);
    size_t n = servers_bundler.count;
    struct chttpserver **snapshot =
        n > 0 ? (struct chttpserver **)malloc(n * sizeof(struct chttpserver *))
              : NULL;
    if (n > 0 && !snapshot) {
      /* The allocation failed. Skip the sweep of this tick in full and retry
       * on the next one, a second later. */
      n = 0;
    } else {
      for (size_t i = 0; i < n; i++) {
        snapshot[i] = servers_bundler.servers[i];
        atomic_fetch_add(&snapshot[i]->servers_bundler_pins, 1);
      }
    }
    ccol_mutex_unlock(servers_bundler.mutex);

    for (size_t i = 0; i < n; i++) {
      struct chttpserver *srv = snapshot[i];
      _sweep_server(srv, now);

      /* This matches the pin above. srv stays valid memory that the library
       * did not free, up to and including this point. It follows the same
       * unpin and broadcast as _engine_force_stop_quiesce_all. */
      ccol_mutex_lock(srv->mutex);
      atomic_fetch_sub(&srv->servers_bundler_pins, 1);
      ccol_cond_var_broadcast(srv->resolve_cv);
      ccol_mutex_unlock(srv->mutex);
    }
    free(snapshot);
  }
  return NULL;
}

/* Returns true once the shared sweep thread of the idle timeout is confirmed
 * to run. That covers a thread that an earlier call already started, and a
 * thread that this call started with success. It returns false only when
 * ccol_thread_create() itself failed on this attempt. Two mechanisms depend
 * fully on this thread: the idle_timeout_ms rule of every registered server,
 * and the recovery of capacity for max_connections. See
 * _listener_resume_if_capacity_freed, whose only caller is this sweep thread.
 * chttpsvr_start() therefore treats a false result as a real failure to
 * start. It does not go on in a silently degraded state. See the call site of
 * that function for the full reasoning. */
#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for the tests only. A real failure of pthread_create(),
 * from resource exhaustion, is not reproducible from a test in a
 * deterministic way. The idle sweep thread is also a shared resource with
 * the lifetime of the engine. It attempts its own ccol_thread_create() call
 * only once for each window where it does not run; see the gate on
 * idle_sweep_bundler.running immediately below. This hook therefore
 * simulates the failure directly. A gate keeps this symbol out of a
 * production build, in the same way as every other white-box helper in this
 * file. */
static _Atomic bool g_force_idle_sweep_thread_create_fail_for_tests = false;
void _chttpsvr_force_idle_sweep_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_idle_sweep_thread_create_fail_for_tests, force);
}
#endif /* RUNNING_UNIT_TESTS */
static bool _idle_sweep_start_if_needed(void) {
  /* See the identical guard and comment in _idle_sweep_fn. The guard is here
   * too. That matches every other function in this file that directly
   * touches servers_bundler.mutex or the state of srv_engine_bundler. The
   * code does not depend on reasoning about the call graph, about what
   * already ran by the time a caller reaches this function. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(servers_bundler.mutex);
  if (!idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, false);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_force_idle_sweep_thread_create_fail_for_tests)) {
      /* This is a simulated failure. The code created no real thread, so
       * there is nothing to join. */
    } else
#endif /* RUNNING_UNIT_TESTS */
      if (ccol_thread_create(idle_sweep_bundler.thread, _idle_sweep_fn, NULL) ==
          0)
        idle_sweep_bundler.running = true;
  }
  bool ok = idle_sweep_bundler.running;
  ccol_mutex_unlock(servers_bundler.mutex);
  return ok;
}

/* Stops the idle sweep thread and joins it, when one runs. This is tied to
 * the lifetime of the shared engine. The engine reaper calls it, beside
 * ccol_event_loop_destroy. It is not tied to the stop or the destroy of any
 * single server, because the sweep thread walks every registered server and
 * not one. Take a sweep thread that still runs at the exit of the process,
 * and that nothing joined. That is exactly the class of "possibly lost"
 * false positive for the glibc TLS allocation (allocate_dtv). The other
 * reaper threads of
 * this codebase already document that they close it; see the reaper thread
 * of this module above. */
static void _idle_sweep_stop_if_running(void) {
  /* See the identical guard and comment in _idle_sweep_fn. */
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  bool was_running = false;
  ccol_thread_id_t t = {0};
  ccol_mutex_lock(servers_bundler.mutex);
  if (idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, true);
    t = idle_sweep_bundler.thread;
    was_running = true;
    idle_sweep_bundler.running = false;
  }
  ccol_mutex_unlock(servers_bundler.mutex);
  if (was_running) ccol_thread_join(t);
}

/* Unlinks c from the parked list, which claims it. The caller holds
 * srv->idle_mutex. */
static void _parked_unlink_locked(struct chttpserver *srv, chttpsvr_conn_t *c) {
  if (c->parked_prev) c->parked_prev->parked_next = c->parked_next;
  if (c->parked_next) c->parked_next->parked_prev = c->parked_prev;
  if (srv->parked_head == c) srv->parked_head = c->parked_next;
  if (srv->parked_tail == c) srv->parked_tail = c->parked_prev;
  c->parked_prev = c->parked_next = NULL;
  c->in_parked_list = false;
}

/* Publishes a parked connection; the counterpart of _idle_list_add. */
static void _parked_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->idle_mutex);
  if (!conn->in_parked_list) {
    conn->parked_prev = srv->parked_tail;
    conn->parked_next = NULL;
    if (srv->parked_tail) srv->parked_tail->parked_next = conn;
    srv->parked_tail = conn;
    if (!srv->parked_head) srv->parked_head = conn;
    conn->in_parked_list = true;
  }
  ccol_mutex_unlock(srv->idle_mutex);
}

static void _idle_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->idle_mutex);
  if (!conn->in_idle_list) {
    conn->idle_prev = srv->idle_tail;
    conn->idle_next = NULL;
    if (srv->idle_tail) srv->idle_tail->idle_next = conn;
    srv->idle_tail = conn;
    if (!srv->idle_head) srv->idle_head = conn;
    conn->in_idle_list = true;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  ccol_mutex_unlock(srv->idle_mutex);
}

static void _idle_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->idle_mutex);
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
  ccol_mutex_unlock(srv->idle_mutex);
}

/* Adds conn to the registry of diverted connections of srv. It is
 * idempotent, and it does nothing when conn is already present, exactly like
 * _idle_list_add. The registry lets a teardown path find the fd of this
 * connection and force a shutdown(2) on it. That matters when the connection
 * turns out to be stuck in the blocking I/O of a worker thread, past the
 * normal graceful drain window. See the comment of diverted_mutex,
 * diverted_head and diverted_tail in struct chttpserver, and see
 * _wait_in_flight_bounded. Two functions call this one. The first is
 * _conn_start_diverted. It calls this one every time the library hands a
 * request to a worker thread. That covers the first request and a further
 * pipelined one on the same connection. The second is _conn_dispatch_reject.
 * That function writes a courtesy rejection response, through reject_pool or
 * through its synchronous fallback. Such a write is exactly the kind of
 * blocking I/O on a connection that this registry protects against. See the
 * comment of that function for the real hang that this closes. */
static void _diverted_list_add(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->diverted_mutex);
  if (!conn->in_diverted_list) {
    conn->diverted_prev = srv->diverted_tail;
    conn->diverted_next = NULL;
    if (srv->diverted_tail) srv->diverted_tail->diverted_next = conn;
    srv->diverted_tail = conn;
    if (!srv->diverted_head) srv->diverted_head = conn;
    conn->in_diverted_list = true;
  }
  ccol_mutex_unlock(srv->diverted_mutex);
}

/* Removes conn from the registry of diverted connections of srv. It is
 * idempotent, and it does nothing when conn is not present. Two paths call
 * it. The first is _conn_free, when a worker thread is done with conn for
 * good; that covers every close path and every reject path. The second is
 * the keep-alive tail of _task_worker, when the library hands conn back to
 * the reactor. After that point a worker no longer owns conn, so conn is no
 * longer a candidate for the forced-shutdown mechanism that this registry
 * serves. */
static void _diverted_list_remove(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->diverted_mutex);
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
  ccol_mutex_unlock(srv->diverted_mutex);
}

/* Interrupts every connection in the registry of diverted connections of srv
 * by force. It calls shutdown(2) with SHUT_RDWR on the raw fd of each one.
 * That unblocks the chttp1_stream_read or chttp1_stream_write call that a
 * stuck worker thread sits in, with an ordinary I/O error. This is the same
 * path that a real disconnect by the peer takes; see for example
 * client_disconnect_mid_body_does_not_hang_server. The existing error
 * handling of that worker then finishes the request and frees it normally.
 * The call is shutdown() and not close(). The worker thread, and never this
 * thread, still owns the fd. Only that worker may call close(2) on it,
 * through _conn_free. A close here therefore risks the classic fd-reuse race
 * of a close from another thread. shutdown() has no such hazard. It only
 * affects the I/O that is in progress and the I/O to come on this exact fd.
 * The fd number itself stays allocated until the owning thread closes it.
 * The library calls this function only after it unlocks srv->mutex; see
 * _wait_in_flight_bounded. It also calls it only after the normal, bounded,
 * graceful wait for the requests in flight runs out. See the comment of that
 * function for why this function exists at all. It is a best effort. The
 * code does not check the return value of shutdown(). An fd that is already
 * closed, or that is invalid for another reason, makes the call a harmless
 * no-op. */
static void _force_unblock_diverted_connections(struct chttpserver *srv) {
  ccol_mutex_lock(srv->diverted_mutex);
  for (chttpsvr_conn_t *c = srv->diverted_head; c; c = c->diverted_next)
    shutdown(c->fd, SHUT_RDWR);
  ccol_mutex_unlock(srv->diverted_mutex);
}

/* Tries to claim conn out of the idle list atomically, for exclusive work.
 * That work is either the dispatch of a real I/O event for conn, or a close
 * of conn. It returns true when conn was idle and the library removed it
 * now. The caller then owns conn alone, and it may safely read conn and free
 * it. It returns false when conn was NOT in the idle list. Somebody else
 * already claimed it, either another dispatch or a concurrent closer, and
 * the caller must not touch conn at all.
 *
 * This is the one piece of synchronization that makes one thing safe. The
 * _close_all_idle_connections and _idle_sweep_fn functions can free a
 * connection that they find in the idle list. They do so from a thread that
 * is not the reactor. Without
 * it, a closer can find a connection idle while a reactor thread dispatches
 * that same connection to _conn_pump. That happens when new data arrives in
 * the same instant. The _conn_free of the closer then runs at the same time
 * as the ctls_conn_read of _conn_pump, on the very same SSL*. For a TLS
 * connection _conn_free calls ctls_conn_destroy, which calls SSL_free. That
 * is a use-after-free, and it appears as a segfault deep in the BIO code of
 * libcrypto. It is most visible on the TLS path. The equivalent race on the
 * plaintext path has the same shape. But it is far less immediately fatal.
 * There a read() and a close() lose a race, instead of a free of live
 * OpenSSL state.
 *
 * The library adds a connection to the idle list only AFTER the matching
 * ccol_event_loop_add or ccol_event_loop_modify call returns. That call is
 * what makes the connection ready for a dispatch. See the handshake-wait
 * branch of _conn_pump, the EWOULDBLOCK branch of its main read loop, and
 * the keep-alive re-arm of _task_worker. In a narrow window, a dispatch can
 * therefore fire before the add to the idle list happens, and the claim here
 * fails. That is harmless and not a bug. This module always uses
 * level-triggered epoll. A claim that fails with "not idle yet" only means
 * that the next epoll_wait reports the same readiness again. The add
 * completes long before that. It is a few instructions on the same thread,
 * with no I/O in between. No request is ever dropped, and none ever
 * hangs. */
static bool _idle_list_try_claim(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  bool claimed = false;
  ccol_mutex_lock(srv->idle_mutex);
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
  ccol_mutex_unlock(srv->idle_mutex);
  return claimed;
}

/* Closes and frees every connection of srv that still sits idle. The reactor
 * owns such a connection. It either waits for its next pipelined request, or
 * it is an open keep-alive connection that nothing used again yet. The
 * __chttpsvr_destroy function calls this one, after chttpsvr_stop() closes
 * the listener. No new connection can arrive then. Without this function, a
 * keep-alive connection that a test client never closed explicitly outlives
 * the server that accepted it. That is the common case, because the idle
 * pool of chttpclient keeps a connection open after a response instead of a
 * close. The chttpsvr_conn_t of that connection then leaks for the rest of
 * the life of the process. Everything that it owns leaks with it: the path,
 * the headers, the state of the route match and more. valgrind reports that
 * as a real "definitely lost" block, back to _conn_create and
 * _listener_on_readable. It is not a false positive.
 *
 * This function claims each candidate at collection time, under idle_mutex,
 * with the same removal from the list that _idle_list_try_claim does. It
 * does not merely copy the pointer out and close it afterward. A connection
 * can therefore never be found here and dispatched to _conn_pump on a
 * reactor thread at the same time. See the doc comment of
 * _idle_list_try_claim for the use-after-free that this closes. */
static void _close_all_idle_connections(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *to_close[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t to_close_n = 0;
    ccol_mutex_lock(srv->idle_mutex);
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
    while (srv->parked_head && to_close_n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      c = srv->parked_head;
      _parked_unlink_locked(srv, c);
      c->park = _CONN_PARK_NONE;
      to_close[to_close_n++] = c;
    }
    ccol_mutex_unlock(srv->idle_mutex);
    if (to_close_n == 0) break;
    for (size_t i = 0; i < to_close_n; i++) _conn_close(to_close[i]);
  }
}

/* ========================================================================== */
/*                         PERCENT-DECODE HELPERS                             */
/* ========================================================================== */

/* Plain %XX decoding, as RFC 3986 defines it. The '+' character either stays
 * literal, which is the meaning in a path, or becomes a space, which is the
 * meaning in a query string. The decode is safe in place, with dst == src,
 * because the write index never passes the read index. The function returns
 * the decoded length. It returns -1 when the percent-encoding is malformed.
 * That means a '%' with no two hex digits after it, or a %XX sequence that
 * decodes to a literal NUL byte.
 *
 * The rejection of a NUL matters beyond the contract of this function. Every
 * caller of this decoder treats its output as a C string that ends with a
 * NUL. chttpsvr_req_path() promises a NUL-terminated string in public.
 * _seg_matches_literal runs strcmp against a registered route segment or
 * prefix segment. chttpsvr_req_query and chttpsvr_req_query_one run strcmp
 * on the keys and values of the query. strcmp and the functions like it stop
 * at the first NUL byte that they see, and not at the length that this
 * function returns. A decoded NUL inside the output therefore truncates the
 * comparison in silence. A path segment such as "foo%00bar" decodes to
 * "foo\0bar", and strcmp reports it as equal to the literal route segment
 * "foo". An attacker then hides a suffix behind a route match, and behind
 * the return value of chttpsvr_req_path(). A caller reasonably expects both
 * to reflect the whole segment. A decoded NUL therefore counts as malformed
 * input, and the function returns -1 for it, exactly as it does for a bad
 * %XX escape. That routes it through the existing, correct handling that
 * every other caller already uses. A failed decode means that this segment
 * or path does not match. Without it, the NUL silently corrupts a comparison
 * that strcmp makes further down. */
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

/* Turns an absolute-form request-target into the origin-form slice inside
 * it, and leaves every other form untouched.
 *
 * RFC 7230 SS5.3.2: "a server MUST accept the absolute-form in requests, even
 * though HTTP/1.1 clients will only send them in requests to proxies." A
 * client that talks to this server through an explicit proxy setting sends
 * one, and so does a proxy that forwards the target it received unchanged.
 * Without this, "GET http://host/items HTTP/1.1" reaches the route search as
 * the four segments "http:", "", "host" and "items", matches nothing, and
 * answers 404 for a resource that the very same server serves at "/items".
 *
 * The function recognises the two schemes that an HTTP request can name,
 * without regard to case. A target with any other scheme keeps its current
 * treatment: this is an origin server, it serves no other scheme, and the
 * route search answers it with a 404 as before.
 *
 * SS5.4 of the same section says that a server MUST ignore the received Host
 * header field when the target is in absolute-form. This server routes on
 * the path alone and never on an authority, so it already ignores it. The
 * authority that this function skips is therefore discarded on purpose, and
 * not lost by accident. chttp1_parser still applies the Host COUNT rule of
 * SS5.4 to such a request; read the blank-line branch of its header state.
 *
 * *out and *out_len describe a slice of target and never a new allocation.
 * The function returns true when it really stripped a prefix. The caller
 * needs that answer, because an absolute-form target whose path component is
 * empty stands for "/", and an origin-form target is never empty at all. */
static bool _strip_absolute_form(const char *target, size_t target_len,
                                 const char **out, size_t *out_len) {
  *out = target;
  *out_len = target_len;

  size_t scheme_len;
  if (target_len > 7 && strncasecmp(target, "http://", 7) == 0)
    scheme_len = 7;
  else if (target_len > 8 && strncasecmp(target, "https://", 8) == 0)
    scheme_len = 8;
  else
    return false;

  /* The authority ends at the first "/", "?" or "#". parse_request_line()
   * already refused every whitespace byte and every control byte in a
   * request-target, so nothing else can end it. */
  size_t i = scheme_len;
  while (i < target_len && target[i] != '/' && target[i] != '?' &&
         target[i] != '#')
    i++;

  *out = target + i;
  *out_len = target_len - i;
  return true;
}

/* A private sentinel that _parse_method returns for a method string that it
 * does not recognise. It is not part of the public chttp_method_t enum. No
 * caller can use it as the method of a route, so it can never match a
 * registered route. */
#define _CHTTP_METHOD_UNKNOWN ((chttp_method_t)(CHTTP_ANY + 1))

/* The most request body that "OPTIONS *" may carry and still leave the
 * connection alive. RFC 9110 SS9.3.7 reserves such a body for future use.
 * The server reads and discards up to this much, and it closes the
 * connection after the 200 when the body is larger. Go's net/http server
 * uses the same bound, with the same outcome. */
#define _CHTTPSVR_OPTIONS_STAR_MAX_BODY ((size_t)4096)

/* The route that "OPTIONS *" matches. It never comes from a registration,
 * and the router tables never hold it. It exists so that the worker path,
 * which reads conn->matched_route, treats the request as a buffered route.
 * Its handler never runs: _task_worker answers the request itself; see the
 * options_star branch there. */
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
        /* A pattern can use the same {name} more than once, for example
         * "/a/{id}/b/{id}". Without this check, such a pattern compiles with
         * success, and the captured value of the second use is then out of
         * reach in silence. chttpsvr_req_param always returns on the FIRST
         * match of a name. The library rejects this pattern at the time of
         * the registration. Without that, it surfaces only as a confusing
         * wrong value at the time of the request. */
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
   * path, whose middleware runs for the rejection, or NULL when no mount
   * owns it and the rejection comes from the root. */
  chttpsvr_router *router;
  chttpsvr_route_t *route;
  char **param_values; /* the caller must free this on OK */
  route_match_t result;
  /* On ROUTE_MATCH_METHOD, one bit (1u << method) for each method that a
   * route matching the path accepts, HEAD included wherever the HEAD-to-GET
   * fallback applies. The Allow header of the 405 comes from it. 0 on every
   * other result. */
  uint8_t allowed_methods;
} match_result_t;

/* One raw segment of a request sub_path, split on '/', together with its
 * percent-decoded value. The library computes that value lazily and keeps it
 * in a memo. A router with N registered routes tries every one of them
 * against the same sub_path. The decoded value of one raw segment, or the
 * failure of its decode, is therefore the same answer whichever candidate
 * route asks for it. A percent-decode depends only on the bytes of the
 * segment, and never on the route under test. This cache decodes each
 * position at most once for each router and each request. Without it, the
 * library decodes a position once for each candidate route that touches it.
 * The cost drops from O(routes * segment length) to one decode for each of
 * the cache->count segments. */
typedef struct {
  const char *raw; /* a pointer into the sub_path that this cache came from;
                    * there is no NUL at raw_len */
  size_t raw_len;
  bool attempted; /* true once the library tried a decode for this
                   * position */
  bool ok;        /* it has meaning only once attempted is true; it is true
                   * exactly when decoded and decoded_len below are valid */
  char *decoded;  /* owned, and it ends with a NUL */
  size_t decoded_len;
} _seg_cache_entry_t;

typedef struct {
  _seg_cache_entry_t *entries; /* an array on the heap, one element for each
                                * raw segment that the library found, up to
                                * and including the cap of this cache */
  size_t count;                /* the count of elements that the library
                                * filled. See the doc comment of
                                * _seg_cache_build for why this value always
                                * stands in for the true total segment count
                                * of sub_path without ambiguity. That holds
                                * for every route whose own seg_count is
                                * strictly less than the cap of this
                                * cache. */
  ccol_memmgmt_procs_t *mp;
} _seg_cache_t;

/* Splits sub_path into its raw segments on '/'. It records the pointer and
 * the length of each one inside sub_path itself, and it decodes nothing yet.
 * It stops once it finds `cap` segments, even when sub_path has more. The
 * split matches the segment-by-segment walk of a route exactly. It strips
 * the leading slash in the same way, and it keeps a zero-length segment
 * instead of a skip. Such a segment comes from a request path with
 * consecutive slashes, and it can never satisfy any route either.
 *
 * `cap` MUST be strictly greater than the seg_count of every route in the
 * calling router. The caller passes router->max_route_seg_count + 1. This is
 * what makes `cache->count == route->seg_count` an exact test for every such
 * route. That test means "sub_path has exactly this many segments, no more
 * and no fewer". The cache therefore never needs the true total segment
 * count of sub_path when that total is more than any registered route can
 * need. The reasoning has two cases. If cache->count < cap, the split loop
 * below stopped only because sub_path ran out of content, so cache->count is
 * the real total of sub_path. If cache->count == cap, sub_path has AT LEAST
 * cap segments. That is strictly more than the seg_count of any route,
 * because every one of those is below cap by construction. cache->count can
 * therefore never equal the seg_count of a real route by accident in that
 * case either. The bound is `cap`, a value that route registration fixes and
 * that the operator of the server controls. It is not the real segment count
 * of sub_path, which the client that sends the request fully controls. That
 * is what keeps the memory of this cache independent of how many "/"
 * characters a request path holds. The function returns 0 on success, where
 * cache->count may legitimately be 0, for example for sub_path == "/". It
 * returns -1 when it runs out of memory. */
static int _seg_cache_build(const char *sub_path, size_t cap,
                            _seg_cache_t *cache, ccol_memmgmt_procs_t *mp) {
  cache->entries = NULL;
  cache->count = 0;
  cache->mp = mp;
  if (cap == 0) return 0;

  const char *p = sub_path;
  if (*p == '/') p++;
  if (*p == '\0') return 0;

  /* The loop condition deliberately does NOT test `*s`. It tests only
   * `count < cap`. A trailing '/', for example in sub_path == "/items/99/",
   * means one more final segment past the last real one, and that segment is
   * empty. The segment-by-segment walk of a route treats it the same way.
   * Its own check of the last segment, "if (next_sep != NULL) goto
   * no_match", rejects a route whose last segment has any further separator
   * after it. That includes a bare trailing one with nothing after it. A
   * loop that tests `*s` stops one segment short, the moment `s` lands on
   * the terminating NUL right after that trailing '/'. It then counts
   * "items/99/" as the same 2 segments as "items/99", in silence. A route
   * that the operator registered as "/items/{id}" then wrongly matches a
   * request path that ends with an extra "/". The test
   * trailing_slash_not_matched in tests.c pins this. `!e` means that the
   * code found no further separator at all. That is what correctly ends both
   * loops below, and it is the only thing that ends them. */
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

/* Returns the percent-decoded value of segment idx of cache. The value comes
 * from a memo. The library decodes it at most one time, on the first call
 * for that position. That holds whatever count of candidate routes asks for
 * it. It returns
 * NULL when that raw segment can never match any route at all. There are two
 * such cases. The segment is empty, which comes from a request path with
 * consecutive slashes such as "a//b". Or its percent-encoding is malformed.
 * Both are a plain "no match" for the route that asked, and neither is an
 * error. This matches the established contract of this module: a failed
 * decode is a route mismatch and never a 400. See the doc comment of
 * conn->path above. The cache keeps ownership of the returned pointer, and
 * that pointer is valid until _seg_cache_free. A caller that needs the value
 * past that point must copy it. The captured param of a matched route is
 * such a case. The function sets *oom_out to true only for a real allocation
 * failure. The caller must treat that as fatal to the whole route search,
 * and not only to this one candidate route.
 *
 * WARNING for a future maintainer: the code sets e->attempted below BEFORE
 * it even tries the allocation. A temporary allocation failure here
 * therefore enters the memo as a permanent "cannot decode", for the rest of
 * the life of this cache. That is the same treatment as the two
 * deterministic outcomes next to it, the empty segment and the malformed
 * percent-encoding. No later call for this same idx retries the allocation.
 * This is harmless only because _match_route_cached and _find_route both
 * treat *oom_out == true as fatal to the ENTIRE route search the instant it
 * happens. The search aborts and the library frees this cache within
 * microseconds; see the handling of "ms == -1" in _find_route. No second
 * call for this idx against this same cache instance can therefore happen.
 * A future change can make that policy skippable for each route, so that the
 * search tries a different candidate instead of an abort. Such a change must
 * revisit this memo first. Without that, a route stays stuck at "no match"
 * for the rest of the route search of this request. That route would match
 * after the memory pressure passes. The code deliberately carries no guard
 * against this in advance. Under the current policy, which is always fatal,
 * such a guard has no observable effect. Nothing reaches a second call to
 * this same idx either way. No test could prove that the guard does
 * anything, and the discipline of this project does not allow that. */
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
    *oom_out = true; /* see this function's own WARNING above: memoized as
                      * permanent, not retried, currently harmless only
                      * because the caller treats this as search-fatal */
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

/* Matches the compiled segments of route against cache. The caller builds
 * that cache once for each router, from the sub_path of that router. The
 * caller then shares it across every candidate route that it tries against
 * the router. See the doc comment of _seg_cache_t. This function rejects a
 * route whose own segment count differs from cache->count at once, before it
 * decodes any segment.
 *
 * If pv_out is not NULL, it receives a fresh array on a match (a return
 * value of 1). That array holds one independently owned copy of each matched
 * param value. The library never hands the decoded copies of the cache out
 * directly. A candidate can match every segment up to some position and then
 * fail on a later one. Each earlier position of the cache must stay
 * undisturbed for the next candidate route to read. This is why the library
 * only copies a value out of the cache, and never moves ownership out of it.
 * On any other outcome this function leaves *pv_out untouched. It returns 1
 * for a match, 0 for no match, and -1 when it runs out of memory. A -1 is
 * fatal to the whole route search, and not only to this one candidate. */
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
        memcpy(val, decoded, dlen + 1); /* +1: include the NUL terminator */
        pv[param_idx++] = val;
      }
      /* A NULL pv_out means that the library only probes this route. The
       * probe decides between ROUTE_MATCH_METHOD and ROUTE_MATCH_NONE; see
       * _find_route. The check above already proved that the value decodes,
       * because _seg_cache_get gave back a `decoded` that is not NULL. The
       * library copies that value nowhere in this case. */
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

/* Decides whether router->prefix owns the request path. The leading
 * segments of that path are already split as raw values, and their decoded
 * values sit in a memo inside cache. See the doc comment of _seg_cache_t.
 * _find_route builds that cache once for each request, from the FULL request
 * path, and shares it across the call of every non-root router into this
 * function, so that N sub-routers never decode the same leading segments N
 * times. The prefix side is walked directly off router->prefix: it is a
 * short string that the operator controls, and one router alone ever parses
 * it.
 *
 * A prefix owns a path when the DECODED path equals the prefix, or starts
 * with the prefix followed by a '/'. The decoded path is the decoded
 * segments joined by '/', which is exactly what chttpsvr_req_path() reports.
 * Ownership is decided on that form, and not on the raw segments alone,
 * because a guard in the middleware of a sub-router reasons about the path
 * that it can read. "/admin%2Fsecret" decodes to "/admin/secret", so the
 * mount "/admin" owns it, although its first raw segment is the one segment
 * "admin%2Fsecret". A segment that is empty (consecutive slashes) or whose
 * percent-encoding is malformed never matches a prefix segment, which is
 * never empty and is compared as a plain literal.
 *
 * The function returns:
 *   1  the prefix owns the path, and the prefix ends on a boundary between
 *      two raw segments. *sub_path_out is the rest of the raw path, which
 *      the routes of this router match against.
 *   2  the prefix owns the path, but the prefix ends inside one raw segment
 *      whose decoded value carries a '/' ("/admin%2Fsecret" against the mount
 *      "/admin"). No route of this router can match such a path, because a
 *      route matches whole raw segments, so the router answers it with a
 *      404 of its own. *sub_path_out is not set.
 *   0  the prefix does not own the path.
 *  -1  an allocation failed. That is fatal to the whole route search, which
 *      matches the contract of _seg_cache_get. */
static int _prefix_matches(_seg_cache_t *cache, chttpsvr_router *router,
                           const char **sub_path_out) {
  const char *pp = router->prefix + 1;

  if (*pp == '\0') {
    /* This is the special "/" prefix; see the doc comment of
     * chttpsvr_subrouter. It owns only the exact root path. That is a
     * request path with zero segments of its own. */
    if (cache->count != 0) return 0;
    *sub_path_out = "/";
    return 1;
  }

  /* The part of the prefix that the decoded segments still have to cover.
   * The prefix carries no trailing '/' and no "//"; see _create_router and
   * chttpsvr_subrouter. */
  size_t prem = router->prefix_len - 1;
  size_t idx = 0;
  for (;;) {
    /* The request path ran out of segments before the prefix did. The code
     * checks this before it indexes into cache->entries, because
     * _seg_cache_get does not check the bound. The cap of the cache is one
     * more than the largest prefix segment count, and every loop iteration
     * consumes at least one prefix segment, so a prefix never needs more
     * entries than the cache holds. */
    if (idx >= cache->count) return 0;

    size_t dlen = 0;
    bool oom = false;
    const char *decoded = _seg_cache_get(cache, idx, &dlen, &oom);
    if (oom) return -1;
    if (!decoded) return 0;

    if (dlen < prem) {
      /* The segment covers part of the prefix, and a '/' of the prefix must
       * follow it. */
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
    /* The segment runs past the end of the prefix. The prefix owns the path
     * only when the decoded value continues with a '/' there. */
    return decoded[prem] == '/' ? 2 : 0;
  }
}

/* The definition comes just below. The HEAD-fallback bookkeeping of
 * _find_route frees the param values of a candidate through it. */
static void _free_param_values(char **pv, int count, ccol_memmgmt_procs_t *mp);

static match_result_t _find_route(struct chttpserver *srv, const char *path,
                                  chttp_method_t method) {
  /* One bit for each method that a route whose path matched accepts, for the
   * Allow header of a 405 (RFC 9110 SS15.5.6). Nonzero exactly when at least
   * one route matched the path and not the method. */
  unsigned allowed_methods = 0;

  /* Every non-root router shares this cache in its own _prefix_matches call
   * below. See the doc comment of that function for the reason. The library
   * builds it once for each request, from the FULL request path. The cap is
   * srv->max_prefix_seg_count + 1. That is a bound which the operator
   * controls. It is not the real segment count of the request path, which
   * the client controls in full. max_route_seg_count uses the same reasoning
   * for the route-matching cache of each router below. The code leaves this
   * cache empty when this server has no sub-router at all. An empty cache is
   * a no-op that _seg_cache_free always handles safely. routers[0] is always
   * the root, and every other entry is a real sub-router with a prefix. With
   * only the root, the loop below never calls _prefix_matches. */
  _seg_cache_t path_cache = {0};
  if (srv->router_count > 1) {
    if (_seg_cache_build(path, (size_t)srv->max_prefix_seg_count + 1,
                         &path_cache, srv->m_procs) != 0)
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
  }

  /* The precedence of a router comes from how specific its mount prefix is.
   * It never comes from the order of registration. srv->routers holds the
   * root at index 0. It holds every sub-router from index 1 onward, in
   * descending order of prefix segment count. The sorted insert of
   * chttpsvr_subrouter keeps that order. This loop therefore visits
   * 1..router_count-1 first and the root last.
   *
   * The prefix of the root matches every path. A try of the root first
   * therefore lets one root pattern answer for a whole class of paths. Take
   * a two-segment root pattern such as "/{a}/{b}". It answers for every
   * two-segment path in the process, and that includes the paths that a
   * sub-router is mounted on. The effective middleware chain that
   * _on_headers_complete builds then carries only the entries of the root.
   * The library silently skips the middleware of the sub-router, such as an
   * authenticator, for a request that plainly lands inside its mount prefix.
   *
   * The most specific mount whose prefix owns the path (see
   * _prefix_matches) owns the request outright. Only the routers mounted on
   * that same prefix may answer it; a router with a shorter prefix, and the
   * root, never see it. When none of their routes matches, the 404 or the
   * 405 comes from the owner, and the owner carries it back in
   * match_result_t.router so that its middleware runs for that rejection
   * too. A guard in the middleware of a mount therefore covers every path
   * under the mount, whatever routes other routers register. Two routers on
   * the identical prefix share the ownership; the one registered first
   * answers a rejection.
   *
   * The loop visits 1..router_count-1 in descending prefix segment count and
   * the root last. Once an owner is found, the first router with a smaller
   * segment count ends the owning group, because two different prefixes
   * with the same segment count can never both own one path. */
  chttpsvr_router *owner = NULL;
  for (size_t visit = 0; visit < srv->router_count; visit++) {
    size_t ri = (visit + 1 < srv->router_count) ? visit + 1 : 0;
    chttpsvr_router *router = srv->routers[ri];

    /* Every path that reaches the cache build below sets this; the
     * initializer states that for a compiler that cannot follow the
     * return codes of _prefix_matches. */
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

    /* The library builds this cache once for each router. Every candidate
     * route below shares it. The doc comment of _seg_cache_t says why the
     * decoded value of a raw segment is the same whichever route asks for
     * it. The cap on the split is router->max_route_seg_count + 1, and not
     * the real segment count of sub_path, which the client controls in full.
     * The doc comment of _seg_cache_build says why. That cap is enough for
     * every route that this router can match. It also stays independent of
     * how many "/" characters a request path holds. */
    _seg_cache_t cache;
    if (_seg_cache_build(sub_path, (size_t)router->max_route_seg_count + 1,
                         &cache, srv->m_procs) != 0) {
      _seg_cache_free(&path_cache);
      return (match_result_t){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
    }

    match_result_t found = {NULL, NULL, NULL, ROUTE_MATCH_NONE, 0};
    bool have_result = false;
    /* This holds the first CHTTP_GET route of this router that matched the
     * path of a CHTTP_HEAD request. The scan keeps it and goes on. An
     * explicit CHTTP_HEAD route, or a CHTTP_ANY one, can match later and
     * must win instead. See head_ok below. */
    chttpsvr_route_t *head_fallback_route = NULL;
    char **head_fallback_pv = NULL;
    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route_t *route = router->routes[i];

      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      /* RFC 9110 SS9.3.2 says that HEAD is identical to GET, with one
       * difference: the server must not send a body. Every path that answers
       * GET therefore answers HEAD. Without this, a path that the operator
       * registered for GET alone reports 405 to a HEAD request. That breaks
       * health checkers, uptime monitors, link checkers and cache
       * revalidation. The response side already carries the rest of the
       * semantics. _send_response suppresses the body for a CHTTP_HEAD
       * request. It still emits the content-length that the body would have
       * had. The code reads this flag only after method_ok already came out
       * false. A request whose method matches outright therefore pays one
       * test of a bool that is already loaded, and nothing else. */
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
          /* The code holds this candidate and does not return it. An
           * explicit CHTTP_HEAD route that the operator registered later on
           * this same router must still win. The scan therefore runs to its
           * end, and the code uses this candidate only after that. */
          if (!head_fallback_route) {
            head_fallback_route = route;
            head_fallback_pv = pv;
          } else {
            _free_param_values(pv, route->param_count, srv->m_procs);
          }
          continue;
        }
        /* A CHTTP_ANY route never reaches this point, because it accepts
         * every method. A CHTTP_GET route also accepts HEAD, through the
         * fallback above. */
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
      head_fallback_pv = NULL; /* found now owns these values */
    }
    /* This is a no-op in most cases. It does real work only in one case.
     * That is when the scan ended on an exact match, or on an out-of-memory
     * error. The code then still held the param values of a fallback
     * candidate. */
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

static void _destroy_req_qparams(chttpsvr_req *req) {
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
    case CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED:
      return "HTTP Version Not Supported";
    default:
      return "Unknown";
  }
}

/* Converts a duration field of chttpsvr_config_t, a count of microseconds,
 * to the milliseconds that the server keeps. It rounds up, so a value above 0
 * never becomes the 0 that several fields read as "off" or as "the default".
 * A value past UINT_MAX - 1 milliseconds saturates there. UINT_MAX stays
 * free: it is the internal form of CHTTPSVR_NO_DEADLINE, which only
 * _cfg_deadline_us_to_ms produces, so no finite configuration can alias it.
 * The arithmetic runs in uint64_t on every target. */
static unsigned _cfg_us_to_ms(uint64_t us) {
  uint64_t ms = ccol_us_to_ms_ceil(us);
  return ms >= (uint64_t)UINT_MAX ? UINT_MAX - 1u : (unsigned)ms;
}

/* _cfg_us_to_ms for the two fields that accept CHTTPSVR_NO_DEADLINE, which
 * becomes UINT_MAX, the internal "no deadline". */
static unsigned _cfg_deadline_us_to_ms(uint64_t us) {
  return us == CHTTPSVR_NO_DEADLINE ? UINT_MAX : _cfg_us_to_ms(us);
}

/* The documentation of chttpsvr_config_t says that a 0 in
 * stream_read_timeout_us or response_write_timeout_us means "wait forever".
 * chttp1_stream_read and chttp1_stream_write follow the convention of
 * poll(2) instead. A negative value blocks forever. A 0 makes one
 * non-blocking try with no wait at all. A positive value is a bound in ms.
 *
 * A configured 0 that goes straight through as their own int timeout_ms
 * therefore means the opposite of the configuration. It does not mean
 * "forever". It means "give up at once, on every call, without one try".
 * The deadline code of chttp1_stream_read and chttp1_stream_write shows why.
 * For a timeout_ms of 0 it calls clock_gettime() once and sets a deadline of
 * "now". It then checks the elapsed time again with a second clock_gettime()
 * call, before it ever calls poll(). On a monotonic clock that second
 * reading can only be at or after the first. The "already expired" branch
 * therefore always fires, and the library never tries poll() or the real
 * read or write.
 *
 * Without this translation, a configured stream_read_timeout_us of 0 makes
 * chttpsvr_req_read() return -1 before the client sends one body byte. It
 * also leaves even a bodyless GET with no response at all. The reason is
 * that response_write_timeout_us has its own default of "0 means use the
 * value of stream_read_timeout_us", so it inherits that same 0. Every
 * timeout value that comes from the config of this module must go through
 * this helper before it reaches chttp1_stream_read or chttp1_stream_write.
 * Without that, "wait forever" silently becomes "never wait at all".
 *
 * There is a separate problem with a configured value above INT_MAX. The
 * timeout_ms parameter of chttp1_stream_read and chttp1_stream_write is a
 * plain int, and so is the one of poll(2). Such a value wraps silently to a
 * negative value on the (int) cast below. On every mainstream two's
 * complement target that this codebase builds for, that negative value is
 * itself the "block forever" convention of poll(2). An unlikely but valid
 * config value, such as a stream_read_timeout_us a little over 24.8 days,
 * therefore becomes "wait forever". The real configuration asked for a
 * finite wait that is merely very long. This helper clamps to INT_MAX
 * instead. That is still the longest finite wait that this API can express,
 * because chttp1_stream_read and chttp1_stream_write have no wider type to
 * carry a longer one. Unlike the wraparound, the clamp does not collapse
 * into the other sentinel that this same function already treats specially
 * (the 0 that becomes -1 above). */
static int _to_stream_timeout_ms(unsigned configured_timeout_ms) {
  if (configured_timeout_ms == 0) return -1;
  if (configured_timeout_ms > (unsigned)INT_MAX) return INT_MAX;
  return (int)configured_timeout_ms;
}

/* A small growable buffer that builds the header block of one response. It
 * starts on the stack, which covers the common case of a handful of ordinary
 * headers. It falls back to a heap allocation when the headers of a handler
 * push the block past the stack reservation. It then grows by doubling, in
 * the same way as the body buffer of chttpsvr_resp_write.
 *
 * Without this, a handler that sets enough headers loses the whole response
 * in silence. One large header value is enough on its own: several
 * Set-Cookie headers, CORS and CSP headers, or a long custom token. A buffer
 * with a fixed capacity fails outright the moment the block does not fit. It
 * closes the connection with zero bytes on the wire and nothing in the log,
 * for a request that already produced a fully valid response. */
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

/* Appends fmt and its arguments to b. It grows b as many times as it needs
 * to. It never fails because of size alone, the way a buffer with a fixed
 * capacity does. It fails only on a real allocation failure, or on an
 * encoding error from vsnprintf.
 *
 * The __attribute__((format(printf, 2, 3))) is what lets the internal
 * vsnprintf(b->buf + b->len, avail, fmt, ap) call below pass fmt and ap
 * straight through. Without it, the -Wformat-nonliteral warning of Clang
 * reports that fmt is not a literal there. The real check of the format
 * string and the argument types applies to every call site of this function
 * instead. fmt is a real literal at each one. */
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

/* Appends n bytes of s to b, with no formatting pass. */
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

/* The length of an IMF-fixdate of RFC 9110 SS5.6.7, such as
 * "Sun, 06 Nov 1994 08:49:37 GMT". */
#define _CHTTPSVR_IMF_FIXDATE_LEN 29

/* The last Date value that this thread formatted, and the second that it
 * names. The value of a Date header changes once a second, and a server
 * writes it on every response, so the formatting runs once a second on each
 * thread that writes responses, and never on the other responses of that
 * second. The cache belongs to one thread, so nothing else can read it half
 * written, and it needs no lock and no atomic.
 *
 * The TLS model follows the other thread-local fast paths of the library:
 * initial-exec, unless CCOL_MEMPOOL_DYNAMIC_TLS selects the general model for
 * a library that dlopen() loads late. See the same switch in cmempool.c. */
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
/* A white-box test hook. A value other than INT64_MIN replaces the clock, so
 * that a test can cross a second boundary without a sleep. */
static _Atomic int64_t g_date_override_for_tests = INT64_MIN;
void _chttpsvr_set_date_override_for_tests(int64_t sec) {
  atomic_store(&g_date_override_for_tests, sec);
}
#endif

static void _put2(char *out, int v) {
  out[0] = (char)('0' + v / 10);
  out[1] = (char)('0' + v % 10);
}

/* Formats sec, in seconds since the epoch, as an IMF-fixdate in UTC. The day
 * and month names come from fixed tables and never from the locale of the
 * process, which strftime() would consult. gmtime_r() is the reentrant form
 * and touches no shared state. It runs once a second on each thread, so it
 * stays out of line and out of the code of the response writer. */
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
    /* A second that time_t cannot hold, or that has no four-digit year. No
     * real clock reaches this. The epoch is a well-formed value for it. */
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

/* Returns the Date value for the current second. The coarse real-time clock
 * is a read of a value that the kernel keeps in the vDSO page, with no system
 * call; its resolution of a few milliseconds is far finer than the one second
 * that the header can express. */
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

/* Serializes resp into a raw HTTP/1.1 response and writes it to stream.
 * response_write_timeout_ms bounds the write. The function sets the
 * Content-Length of every response that sends a body itself, because this
 * server never uses chunked transfer-encoding for its own responses. It
 * also always writes a
 * Connection header that reflects keep_alive. It returns false on a write
 * error or a timeout. The caller must then treat the connection as unusable
 * and close it. The header block itself has no size limit; see
 * _head_builder_t above.
 *
 * Two headers of a handler get a different treatment from every other one. A
 * "Connection" header that the handler sets never reaches the wire. keep_alive
 * is the one and only source of truth for the Connection value on the wire.
 * That same value is what the caller, _task_worker, reads right after this
 * call to decide whether it really keeps the connection open. On a response
 * that sends a body, resp->body_len is the one and only source of truth for
 * the Content-Length value on the wire. It is the exact byte count that this
 * function is about to write, and a Content-Length of the handler never
 * reaches the wire there.
 *
 * If either header reached the wire on its own, the two sides could diverge
 * in silence. A response could claim "keep-alive" while the library closes
 * the socket right after it. Or it could claim a body length that differs
 * from what the library really sent. That is a framing bug: it splits or
 * desynchronizes the responses on a kept-alive connection. A caller has no
 * way to detect such a bug and no way to recover from it.
 *
 * suppress_body is true for a HEAD request. RFC 7231 SS4.3.2 says that a
 * HEAD response MUST NOT hold a message body. It also says that the response
 * SHOULD report the same header fields as a GET, Content-Length included.
 * The function therefore still computes Content-Length from resp->body_len
 * whenever the handler wrote a body. It holds back only the body bytes
 * themselves. Without this, the unwritten
 * body bytes stay in the socket buffer. A client that obeys the standard
 * stops its read after the headers of a HEAD response. It then reads those
 * bytes as the start of the next pipelined response on a keep-alive
 * connection.
 *
 * The same hazard exists for a 1xx, 204 or 304 status, and it does not
 * depend on suppress_body or on HEAD. RFC 9110 SS6.4.1, SS15.2.1 and
 * SS15.4.5 say that none of the three may ever carry a body, whatever the
 * method is. The response-parse side of chttp1_parser in this codebase
 * already treats all three as bodyless, without exception; see its own
 * CHTTP1_ST_HEADERS handling. Any client that obeys the same rule misreads a
 * body that this function writes. It reads it as the start of the next
 * pipelined response. chttpclient is such a client.
 *
 * Take a handler that sets one of these three statuses and also writes a
 * body with chttpsvr_resp_write. Such a handler is far more likely to make a
 * mistake than to want a response that breaks the standard. This is why the
 * function suppresses the body here without exception, instead of leaving
 * the decision to the handler.
 *
 * A 1xx or a 204 also MUST NOT carry a Content-Length header at all, and the
 * function writes none there. A 304, and a HEAD response for which the
 * handler wrote no body, may carry the length that a 200 to a GET would
 * carry, and only that length (RFC 9110 SS8.6). Only the handler knows it,
 * so the function writes the Content-Length that the handler set there, and
 * none when the handler set none. Such a message ends at its header block
 * whatever the field says, so the value of the handler cannot desynchronize
 * the framing. chttpsvr_resp_set_header and chttpsvr_resp_add_header accept
 * only a well-formed number for it. A rejection that the server writes
 * itself (is_reject) has no handler, and its HEAD form reports the length
 * of the body that its GET form carries. */

/* A shared helper for a countdown against the monotonic clock. On the first
 * call it sets *deadline to max_dur ms ahead, and it latches that with
 * *deadline_set. On every call it then shrinks *timeout_ms_inout to the time
 * that really remains until that deadline. It does so only when that
 * remaining time is less than the per-call timeout that the caller already
 * computed. A 0 in *timeout_ms_inout means "no per-call bound of its own
 * yet", so the remaining time always wins in that case. The function returns
 * false once the deadline itself passes.
 *
 * The function is a no-op when max_dur is 0. It then always returns true and
 * touches nothing else. A caller that turns the matching total-duration cap
 * off therefore pays one branch and nothing more. Two functions share this
 * helper. They are _check_read_deadline, for max_body_read_duration_ms, and
 * _send_response below, for max_response_write_duration_ms. Both
 * total-duration caps therefore compute their value in the same way. */
static inline __attribute__((always_inline)) bool
_shrink_timeout_to_deadline_inl(struct timespec *deadline, bool *deadline_set,
                                unsigned max_dur, unsigned *timeout_ms_inout) {
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
  /* The type here is long long, and not long. C99 guarantees that long long
   * holds at least 64 bits. max_dur is unsigned and can reach UINT_MAX ms,
   * which is about 49.7 days. deadline->tv_sec - now.tv_sec can therefore
   * reach about 4.29e6 seconds. A multiply of that by 1000 overflows a
   * 32-bit long on an ILP32 build, and this project builds and CI-tests one.
   * It does so well before max_dur reaches the end of its own valid range.
   * The result is silently wrong. It is either negative, which times out too
   * early, or it wraps to a positive value, which under-enforces the
   * timeout. long long has no such overflow risk for any value that max_dur
   * can hold. */
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

/* The out-of-line form of _shrink_timeout_to_deadline_inl, for the callers
 * that are not on the path of every request body. */
static bool _shrink_timeout_to_deadline(struct timespec *deadline,
                                        bool *deadline_set, unsigned max_dur,
                                        unsigned *timeout_ms_inout) {
  return _shrink_timeout_to_deadline_inl(deadline, deadline_set, max_dur,
                                         timeout_ms_inout);
}

#ifdef RUNNING_UNIT_TESTS
/* This is a white-box test hook and nothing else. It forces the very next
 * conn-guarded write-deadline check to report "already expired". That check
 * lives in the write loop of _send_response, and it runs
 * through _response_write_deadline_ok below. The hook ignores the real
 * elapsed time and the configured value of
 * conn->srv->max_response_write_duration_ms.
 *
 * The hook exists because the header block of a courtesy rejection response
 * is small. It is a status line plus a handful of fixed headers, always well
 * under a kilobyte, with no body at all. In practice one write(2) call
 * always completes it. There is therefore no reliable way to get a real
 * short write, or a real expired deadline, out of the socket buffering for
 * it. max_response_write_duration_exceeded_closes_connection can do that for
 * a real, large response body of a handler.
 * g_force_short_interim_write_bytes_for_tests exists for the same reason,
 * for the fixed-size interim line of _write_interim_continue.
 *
 * The hook disarms itself after one use, so a test does not need to reset
 * it. It has no effect at all when conn is NULL. A real expired deadline has
 * no effect there either. The library reads the deadline state that this
 * hook fakes only when conn is not NULL. */
static _Atomic bool g_force_next_response_write_deadline_expired_for_tests =
    false;
void _chttpsvr_force_next_response_write_deadline_expired_for_tests(void) {
  atomic_store(&g_force_next_response_write_deadline_expired_for_tests, true);
}
#endif /* RUNNING_UNIT_TESTS */

/* Wraps _shrink_timeout_to_deadline for every internal write-retry loop that
 * needs write-side deadline tracking scoped to one connection. There are
 * three such loops: the loop of _send_response, the loop of
 * _send_response_continue for a parked response, and the loop of
 * _write_interim_continue. All three therefore read the same logic. That
 * includes the forced-expiry hook above, which exists only under
 * RUNNING_UNIT_TESTS. Every caller already checks that conn is not NULL before
 * it calls this function.
 *
 * apply_internal_ceiling selects which total-duration cap governs this
 * write. A false value marks a real response for a matched route.
 * _send_response passes its own is_reject parameter straight through. The
 * effective cap is then exactly conn->srv->max_response_write_duration_ms,
 * with no change. A 0 there means "no limit", which is the explicit choice
 * of the operator for a response body that a handler controls.
 *
 * A true value marks a small internal write with a fixed shape. That is a
 * courtesy rejection response, or the "100 Continue" interim line. See the
 * comment of _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS for why both get this
 * same treatment. The effective cap is then the tighter of that same
 * configured value and _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. It is never
 * looser than what the operator configured, so an operator who set a smaller
 * cap still gets exactly that. It is never unbounded either, even when the
 * operator leaves the knob at its documented "off" default. */
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
/* A white-box hook for the tests. It makes the next response write of a
 * handler send only its first n bytes and then behave as if the socket were
 * full, so that a test can end a partial write exactly inside the header
 * block or inside the body, and watch the parked rest resume. It disarms
 * itself after one use. */
static _Atomic size_t g_force_short_response_write_for_tests = 0;
void _chttpsvr_force_short_response_write_for_tests(size_t n) {
  atomic_store(&g_force_short_response_write_for_tests, n);
}
#endif

/* About conn: both call sites always pass their own real conn, which is
 * never NULL. Those sites are the courtesy rejection response of
 * _conn_reject_and_close and the real matched-route response of
 * _task_worker. max_response_write_duration_ms, from the config of
 * conn->srv, therefore bounds the write of every _send_response call. That
 * bound comes on top of the per-call bound of timeout_ms. The library
 * applies it through conn->write_deadline, conn->write_deadline_set and the
 * shared _shrink_timeout_to_deadline helper above. Both callers write with
 * stream->write_nonblocking set, so a full socket parks the rest of the
 * response instead of waiting; see _send_response_park.
 *
 * is_reject also turns _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS on. That value
 * is an unconditional ceiling on the total-duration bound, and it applies to
 * a rejection response alone. It applies whatever the operator sets
 * max_response_write_duration_ms to. See the comment of
 * _response_write_deadline_ok for the reason.
 *
 * The `if (conn && ...)` checks below are a defensive measure for the
 * signature of this internal helper. No real caller passes NULL. */
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
  bool handler_date = false;
  const char *handler_length = NULL;

  if (!_head_builder_append(&hb, "HTTP/1.1 %d %s\r\n", status,
                            _status_reason(status))) {
    _head_builder_release(&hb);
    return false;
  }

  for (size_t i = 0; i < resp->header_count; i++) {
    /* The library never writes "connection" out of resp->headers, whether a
     * handler set one or not. keep_alive below is the single source of truth
     * for whether the library really keeps this connection open afterward.
     * The _task_worker function computes its own keep_alive in the same way.
     * It makes the real decision after the response. A value from a handler
     * that reached the wire on its own could diverge from that decision in
     * silence. For example, a handler can set "Connection: keep-alive" after
     * the framing and timeout logic of the server already decided to close
     * right after. The client then reads the opposite of what happens. This
     * function is the one place that both decides and announces the reuse of
     * a connection. It must therefore be the only writer of this header.
     *
     * The library filters "content-length" for the same reason. resp->body_len
     * is the single source of truth for how many body bytes this function
     * writes a few lines below. chttpsvr_resp_write, _write_str, _printf and
     * _write_json are what fill it. A value from a handler can disagree with
     * it. That happens with a stale or copy-pasted header, or with one that
     * merely reflects data from the request. Such a value is as real a
     * framing hazard on a kept-alive connection as a Connection header that
     * diverges. The client then reads the bytes of the next pipelined
     * response as the tail of the body of this one. Or it blocks and waits
     * for bytes that never arrive. The function always computes the value
     * from the one true byte count that it is about to send. It never trusts
     * a number from the caller that can drift from that count. This closes
     * the gap in the same way as the Connection handling above. */
    if (strcasecmp(resp->headers[i].name, "connection") == 0) continue;
    if (strcasecmp(resp->headers[i].name, "content-length") == 0) {
      handler_length = resp->headers[i].value;
      continue;
    }
    /* A Date that the handler set replaces the one that the server adds; see
     * the Date block below. The response keeps one entry for each of these
     * three names, whichever call set it, so this loop writes at most one. */
    if (strcasecmp(resp->headers[i].name, "date") == 0) handler_date = true;
    /* There is no space after the colon. The raw-socket checks of the test
     * suite of this module read this exact wire format, for example with
     * strstr(buf, "connection:close"). The code must therefore produce the
     * format byte for byte. */
    if (!_head_builder_append(&hb, "%s:%s\r\n", resp->headers[i].name,
                              resp->headers[i].value)) {
      _head_builder_release(&hb);
      return false;
    }
  }

  /* Every final response carries a Date, as RFC 9110 SS6.6.1 asks of an
   * origin server with a clock, unless the handler set its own. A 1xx
   * response carries none. This is the rule of Go's net/http server. */
  if (!status_is_1xx && !handler_date) {
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

  /* The Content-Length of a response that sends no body. A 1xx and a 204
   * carry none at all. A 304, and a HEAD response for which the handler
   * wrote nothing, describe a representation that is not in this message:
   * only the handler knows its length, so the field goes out exactly when
   * the handler set it, and a 0 that the server made up would tell a cache
   * that the stored representation is empty (RFC 9110 SS8.6). Wherever the
   * handler wrote a body, the length of that body is the framing and wins
   * over any value that the handler set. This is the rule of Go's net/http
   * server. A rejection that the server writes itself has no handler: the
   * server is the author of the body that a GET would get, so a HEAD
   * rejection reports the length of that body. */
  if (!status_is_1xx && status != 204) {
    bool ok_len = true;
    bool handler_head = suppress_body && resp->body_len == 0 && !is_reject;
    if (__builtin_expect(status != 304 && !handler_head, 1))
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

  /* The head and the body go out together: one writev(2) for plain text,
   * and one TLS record when they fit in one; see chttp1_stream_writev2. The
   * write is tried first and waits only once the socket is full, so an
   * ordinary response costs one system call. sent counts across the head
   * and then the body, and a short write can end inside either one.
   *
   * The loop reads the server-level cap of conn again on every write call,
   * and does not cache it. The cap can only shrink the per-call timeout
   * further, and it can never widen it. */
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
      /* A socket that is full while the caller forbids a wait: the rest of
       * the response goes out later, from a parked connection. The unsent
       * part of the header block lives on this stack, so it is copied. */
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

/* Writes the "HTTP/1.1 100 Continue\r\n\r\n" interim response of RFC 7231
 * SS5.1.1. Both Expect: 100-continue send sites share it. Those are
 * _task_worker for a buffered route and chttpsvr_req_read for a streaming
 * route. The function follows the write loop of _send_response on
 * purpose. It does not do one write with no retry, which
 * max_response_write_duration_ms would leave unbounded. There are three
 * reasons.
 *
 *   - It loops on a short write. It does not treat n <= 0 as the only signal
 *     of a failure. The documentation of chttp1_stream_write says that the
 *     call can return less than the requested length on a short write. Those
 *     are the semantics of write(2) in POSIX. A caller that needs the whole
 *     buffer on the wire must retry the rest. _send_response already follows
 *     that same discipline for the real response. One write of this 25-byte
 *     line with no retry risks a truncated status line on the wire. One
 *     example is "HTTP/1.1 100 Con". Nothing detects it and nothing reports
 *     it. That happens on a connection under enough send-buffer pressure for
 *     even a write this short to block part way.
 *
 *   - It threads every single write attempt through conn->write_deadline and
 *     _response_write_deadline_ok, exactly as the writes of _send_response
 *     do. The duration of this interim write therefore counts against the
 *     same max_response_write_duration_ms budget for the request that bounds
 *     the send of the real response. It is not an extra cost outside that
 *     budget. Without this, the write is fully unbounded in a common
 *     configuration. That configuration leaves response_write_timeout_ms at
 *     its default, which is "fall back to stream_read_timeout_ms". It then
 *     sets stream_read_timeout_ms itself to 0, which means "wait forever".
 *     That suits slow but legitimate uploads. The operator then depends on
 *     max_response_write_duration_ms alone to bound how long a peer that
 *     reads slowly can pin a worker thread.
 *
 *     apply_internal_ceiling is always true for this call. _send_response
 *     differs, because it sets that flag only for a rejection response and
 *     never for a real one. The interim line is always the small, fixed
 *     25-byte content of this library. _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS
 *     therefore bounds it without exception. That holds even where the
 *     operator sets max_response_write_duration_ms to 0, which turns the
 *     knob off for the response bodies of their own handlers. Without that
 *     ceiling, a peer can hold a thread of the worker pool forever. It only
 *     needs to send an ordinary "Expect: 100-continue" request and then
 *     trickle its reads of this reply. Such a request is standard client
 *     behavior, and it is the default of curl for a large upload. A handful
 *     of such connections then exhausts the whole pool.
 *
 *   - It resets conn->write_deadline_set to false before it returns, on
 *     every exit path and without exception. conn->write_deadline and
 *     conn->write_deadline_set belong to one REQUEST. Only
 *     _conn_reset_for_request clears them, and it does so once. This
 *     function runs strictly before the library reads the request body. For
 *     a buffered route that is before _drain_body. For a streaming route it
 *     is before the read loop of chttpsvr_req_read.
 *
 *     A write_deadline_set left true after this call makes the later
 *     _shrink_timeout_to_deadline calls of _send_response reuse the deadline
 *     that THIS call set. The budget of max_response_write_duration_ms then
 *     silently pays for every second that the body read takes in between.
 *     That phase is unrelated, it is often much longer, and it already has
 *     its own bounds in stream_read_timeout_ms and max_body_read_duration_ms.
 *     Take a request whose body legitimately needs a few real seconds to
 *     arrive. That is well inside its own read-side budget, and the "wait
 *     forever" configuration for slow legitimate uploads is documented and
 *     supported. Without the reset, _send_response finds the deadline
 *     already expired at its very first write attempt. It then closes the
 *     connection with zero response bytes sent, although the real send took
 *     no measurable time at all.
 *
 *     The reset here makes _send_response always set its own fresh deadline.
 *     That deadline then reflects only its own send duration. This matches
 *     the documented contract of max_response_write_duration_ms, which
 *     bounds the total wall-clock time that a worker thread spends to SEND
 *     one response. The cost of this interim write itself is still charged
 *     to the budget while the write happens, through the same
 *     _shrink_timeout_to_deadline calls below. That cost is about 25 bytes
 *     and is usually too small to measure. The function discards only the
 *     leftover deadline, which it set but did not consume. It never carries
 *     that deadline into a different phase of the request, which it was
 *     never meant to bound.
 *
 * The function returns 1 only when it writes the complete 25-byte line, and
 * -1 when it parked the rest; see the comment of the function itself. Two
 * outcomes return 0. The first is a total write failure, where n <=
 * 0 on the very first attempt and nothing reached the wire. The second is a
 * real short write. There the deadline expires, or the stream reports a hard
 * error, after one or more bytes of this line already went out.
 *
 * The two are NOT equivalent for a caller. A total failure leaves the
 * connection exactly as untouched as it was before this call. Any later read
 * or response send can therefore fail through its own error path, which the
 * caller already handles, with nothing more to consider. A short write
 * leaves a truncated status line on the wire that no client can parse, and
 * nothing can take it back. The caller must therefore not try any further
 * write on this same stream. Such a write lands right after the truncated
 * line and corrupts the response framing of the client. It is pointless in
 * any case once the peer gives up on this connection.
 *
 * Both callers therefore treat ANY 0 return in the same way, and not
 * only a short one. They skip the real response send and close the
 * connection. A caller has no cheap way to tell the two cases apart from
 * outside. It also
 * has no need to, because neither case leaves a connection that is worth
 * more bytes. */
#ifdef RUNNING_UNIT_TESTS
/* This is a white-box test hook and nothing else. A test needs direct
 * control over how much of the tiny 25-byte interim line reaches the wire.
 * That is the only way to reproduce a real short write of it every time. The
 * alternative is to coax the socket buffering of the OS into a real partial
 * write, which is not reliable.
 *
 * When a test arms the hook with an n where 0 < n < 25, the very next
 * _write_interim_continue() call writes exactly n bytes for real. It does so
 * with a real chttp1_stream_write() of a truncated prefix, so a test client
 * really receives those bytes. The call then returns false at once. This
 * imitates a deadline that expires right after that partial write. It is
 * exactly the scenario that a real peer which reads slowly can trigger under
 * max_response_write_duration_ms.
 *
 * The hook disarms itself after one use, by setting n back to 0, so a test
 * does not need to reset it. The guard keeps this symbol out of a production
 * build entirely, in the same way as every other white-box helper in this
 * file. */
static _Atomic size_t g_force_short_interim_write_bytes_for_tests = 0;
void _chttpsvr_force_short_interim_write_for_tests(size_t n) {
  atomic_store(&g_force_short_interim_write_bytes_for_tests, n);
}
#endif /* RUNNING_UNIT_TESTS */
#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for the tests. Armed with n, the next interim line that a
 * buffered route writes without waiting sends its first n bytes (n may be 0)
 * and then behaves as if the socket were full, so that a test can watch the
 * rest wait in a parked connection and then go out. It disarms itself after
 * one use. The value SIZE_MAX means "not armed". */
static _Atomic size_t g_force_interim_park_after_for_tests = SIZE_MAX;
void _chttpsvr_force_interim_park_after_for_tests(size_t n) {
  atomic_store(&g_force_interim_park_after_for_tests, n);
}
#endif /* RUNNING_UNIT_TESTS */

/* The interim line and its length. */
static const char _interim_continue_line[] = "HTTP/1.1 100 Continue\r\n\r\n";
#define _INTERIM_CONTINUE_LEN (sizeof(_interim_continue_line) - 1)

/* It returns 1 once the whole line is out and 0 when the write failed or its
 * deadline expired. A stream whose writes do not wait (write_nonblocking)
 * can also meet a full socket: the function then saves the unsent rest as a
 * parked response of conn (see _send_response_park), keeps the deadline of
 * the write for the parked rest, and returns -1. The caller then gives the
 * connection up, and the reactor resumes it once the socket takes bytes
 * again; see _task_resume. A failed copy of that rest returns 0. */
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
      /* The rest of the line, or the body bytes that came with the headers,
       * could not be saved. */
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
  /* A NULL route is the last step of the chain of a REJECTED request; see
   * _conn_reject_and_close. Either no handler matched, or the one that
   * matched may not run. The end of the chain therefore means "send the
   * rejection response that resp already holds". A middleware that never
   * calls next stops the chain with its own response instead. That is
   * exactly what it does for a matched route. */
  if (ctx->route) ctx->route->fn(req, resp, ctx->route->ctx);
}

/* Which rejection statuses run the request's middleware chain before their
 * courtesy response is sent.
 *
 * ROUTING decides a 404 and a 405. The declared Content-Length of the
 * request against the configured limit decides a 413. All three are ordinary
 * client traffic, and they arrive in high volume. A scanner probes paths, a
 * client uses the wrong verb, and another client sends an oversized upload.
 * Some middleware must be able to see all three and count them. A rate
 * limiter, a ban list for each peer and an access log are such middleware. A
 * middleware may
 * also answer such a request itself. It does so when it writes its own
 * response and does not call next.
 *
 * This library excludes three rejection statuses on purpose. A 500 comes
 * from an allocation failure inside this module. Application code that
 * allocates again is the wrong answer to memory pressure. The middleware
 * snapshot can also be the very thing that the library could not build. A
 * 503 means that the worker pool is already full, so more application work
 * is precisely what must not start. A 501 comes from the request line
 * itself. The library decides it before it parses the request target, and it
 * decides it for a method token outside the seven chttp_method_t names.
 * There is therefore no request object for a middleware to read. A 501 sits
 * with the other rejections at the level of the request line. This module
 * answers those by a close of the connection, and it runs nothing. */
static bool _reject_status_runs_middleware(int status) {
  return status == CHTTP_STATUS_NOT_FOUND ||
         status == CHTTP_STATUS_METHOD_NOT_ALLOWED ||
         status == CHTTP_STATUS_PAYLOAD_TOO_LARGE;
}

/* Takes a snapshot of the effective middleware chain into dispatch: the
 * chain of the root router, which is the global middleware, then the chain
 * of router when router is a sub-router. The caller holds srv->routes_lock,
 * and that is what makes this cheap. The snapshot is at most
 * _CHTTPSVR_MAX_MW pointer pairs, and the code takes it inside the lock
 * section that the route search already needed.
 *
 * router is the router of the matched route, or the sub-router that owns
 * the path of a request that it answers with a 404 or a 405, or NULL for a
 * rejection that comes from the root. The code leaves dispatch->route NULL.
 * The caller sets it for a matched route; for a rejection, a NULL route is
 * what tells _chttpsvr_next that the end of the chain is the rejection
 * response, and it is also what keeps a rejected request away from a
 * handler.
 *
 * It returns false when the two chains together hold more than
 * _CHTTPSVR_MAX_MW entries. The caller answers that with a 500, whatever
 * the request would otherwise have got. */
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

/* Frees the body of the request that just ended, and gives its reservation
 * of body memory back in the same step. Nothing reads the body once the
 * handler returned, or once the request was refused or closed. The
 * reservation lasts exactly as long as the buffer, so that
 * max_partial_body_memory bounds every byte of body that a buffered route
 * holds, including the body that a running handler reads. A response that
 * then waits for a slow reader, or a lingering close, must not keep the body:
 * such a wait holds no thread, so the number of connections in it has no
 * bound that the memory limit could cover.
 *
 * Every path on which a request ends reaches this function: _task_finish,
 * _conn_drop_request_state, _conn_reset_for_request and _conn_close. A
 * reservation can exist before its buffer does, because a body that a
 * Content-Length frames reserves its whole length before its first byte is
 * read, so the release does not depend on the buffer. */
static inline __attribute__((always_inline)) void _conn_drop_body(
    chttpsvr_conn_t *conn) {
  if (conn->body.buf) {
    _ccol_mem_free(conn->m_procs, conn->body.buf);
    memset(&conn->body, 0, sizeof(conn->body));
  }
  if (__builtin_expect(conn->mem_charged != 0 || conn->mem_exempt, 0))
    _mem_release(conn);
}

/* Frees the rest of what a request holds and nothing reads once its response
 * is being written: the target, the query and the header fields, and the
 * values of the path parameters. A connection calls it as it starts to wait
 * for a slow reader or lingers, for the reason given at _conn_drop_body.
 * matched_route stays, because the routes live as long as the server. */
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
  /* Free the matched param values of the request that just ended, before the
   * code sets the pointer to NULL. The _conn_free function is the final
   * teardown. It already does this correctly for the LAST request on a
   * connection. But a
   * keep-alive connection reaches this reset function between every request.
   * A set to NULL here with no free first leaks every param-value array
   * except the final one. valgrind then reports those as "definitely lost"
   * blocks that lead back to the pv calloc of _match_route_cached. It also
   * reports the decoded string contents of each one as "indirectly lost". */
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
  /* The clock of the reactor-owned phase belongs to one request, and not to
   * one connection. The library arms it again from whichever step runs first
   * for the NEXT request: the handshake step of _conn_pump, or
   * _conn_feed_bytes. An armed clock left here charges that request for the
   * keep-alive gap in front of it. It then closes a healthy idle connection
   * the instant its next request arrives. */
  conn->header_phase_active = false;
  /* The rate floor measures one body read or one response write of one
   * request, so the next request starts its own. */
  conn->rate_active = false;

  /* The code initializes conn->parser again in place. It does not build a
   * fresh chttp1_parser_t on the stack and then assign that struct over
   * conn->parser. The reason is size. chttp1_parser_t holds an 8192-byte
   * line_buf. A version with two steps pays for the size of that buffer
   * TWICE on every keep-alive request. It pays once to zero the buffer on
   * the stack inside chttp1_parser_init_request. It pays again to copy the
   * buffer from the stack to the heap in the struct assignment. This is the
   * hottest per-request path of this server. An init in place pays only the
   * first cost, which is the memset of chttp1_parser_init_request. It never
   * pays the second.
   *
   * The code reads conn->parser.settings into the call argument before the
   * internal memset(conn->parser, 0, ...) of chttp1_parser_init_request
   * runs. C evaluates the arguments of a call before it enters the body of
   * the callee. This is therefore not a read of a value that the callee
   * already cleared. */
  const chttp1_settings_t *settings = conn->parser.settings;
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  size_t max_hdr_bytes = atomic_load(&conn->srv->max_header_bytes);
  if (max_hdr_bytes) {
    conn->parser.max_header_count_override = 0;
    conn->parser.max_total_header_bytes_override = max_hdr_bytes;
  }
  /* This bounds the declared size of one chunk. That size may not exceed
   * what the whole request body may be. chttp1_parser checks it the moment
   * it parses a chunk-size line. It does not wait until that many bytes
   * arrive. See the max_body_size check of _on_body, and the matching
   * up-front Content-Length check in _on_headers_complete below.
   *
   * Without this cap, one absurdly large declared chunk ties up a worker
   * thread. Any value up to UINT64_MAX is a valid chunk-size token, and the
   * peer then never sends those bytes. The thread waits until
   * stream_read_timeout_ms or max_body_read_duration_ms fires, because no
   * limit that counts bytes can ever trigger when the bytes never arrive.
   *
   * The code leaves the field at its default of 0, which means "no cap", in
   * one narrow corner. That corner is a max_body_size configured to exactly
   * 0. A 0 is the "no cap" sentinel of this field too; see its doc comment.
   * It therefore cannot also express "cap at zero". In that corner the
   * reactive _on_body check still catches the very first body byte that the
   * peer sends, for any chunk. Only one case stays outside this cap there: a
   * chunk that declares a nonzero size and then sends none of it. That is a
   * low-value target, and a second sentinel to close it is not worth the
   * extra complexity. */
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
  /* _conn_reset_for_request() sets every per-request default that this
   * connection needs. It must run before the first request too. Those
   * defaults are resp.status_code, resp.m_procs, reject_status, the
   * header-size override and more. A conn straight out of calloc has
   * resp.status_code == 0. Without this call, _send_response() reads that 0
   * as an unset or invalid status and silently maps it to 500, for the very
   * first request of a connection. Every later keep-alive request is safe,
   * because each one already goes through this same reset call. Every free
   * that the reset does is a no-op here, because calloc left each of those
   * fields NULL or 0. */
  _conn_reset_for_request(conn);
  return conn;
}

static void _conn_free(chttpsvr_conn_t *conn) {
  if (!conn) return;
  /* The code captures this before it frees conn below. The deferred
   * decrement at the end of this function needs it, and a read of conn is
   * not safe at that point. */
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
  /* This is a defense in depth, and not an answer to a live leak. Every call
   * path that sets conn->_carry_over frees it and sets it to NULL itself,
   * before conn can reach this function. Those paths are _conn_start_diverted
   * and the keep-alive tail of _task_worker. This free is therefore a no-op
   * on every path that runs today. It exists so that a future rejection or
   * close path fails safely, with a no-op free of a NULL pointer. The author
   * of such a path may not know this discipline. Without this line, that
   * path silently leaks the carry-over buffer, and nothing catches it. */
  _ccol_mem_free(mp, conn->_carry_over);
  if (conn->wp_head) _ccol_mem_free(mp, conn->wp_head);
  _ccol_mem_free(mp, conn);

  /* This decrement comes LAST. Every access to conn and mp above must finish
   * first. The wait loop of _drain_and_close_all_connections reads a
   * current_connections of 0 as proof that this connection is entirely done.
   * It then lets the server-level teardown go on, and that teardown frees
   * the routers, m_procs and more. A decrement any earlier, right after
   * close(conn->fd) for example, lets that waiter see "done" while this
   * function still runs against conn, mp and srv. That is a real data race.
   * _conn_free can run asynchronously, from _conn_on_removed on the reactor
   * thread of cthreadcomm. It does not always run on the thread that called
   * _conn_close. */
  atomic_fetch_sub(&srv->current_connections, 1);
}

/* This is the on_removed handler for every conn->reg registration that the
 * code creates below. It fires exactly once for each registration. It fires
 * asynchronously, from the reclaim path of cthreadcomm. It fires only once
 * no dispatch of that registration can still touch conn. The same holds for
 * any other ccol_event_loop call that resolves it. See the doc comment of
 * ccol_event_loop_remove() for why that call cannot give the same guarantee
 * synchronously. A synchronous version risks a lock-ordering cycle against
 * the locks of conn->srv, which _conn_on_readable, _conn_on_writable and
 * _conn_on_error take.
 *
 * The handler decrements conn->lifetime_refs. See the comment of that field
 * for why one firing alone does not prove that conn is safe to free. The
 * handler frees conn when this was the last outstanding contribution.
 *
 * The code captures srv before that possible free. _conn_free frees conn
 * itself, and never srv. It then decrements srv->lifetime_refs without
 * exception, whether this firing also freed conn or not. The count of srv
 * tracks every registration that the library ever created for ANY of its
 * connections; see the comment of that field. Each registration contributes
 * exactly one. That count is fully independent of the per-connection count
 * in conn->lifetime_refs. This is what lets __chttpsvr_destroy defer the
 * final free of srv. That free goes to whichever firing turns out to be the
 * last one. That is either this one or the direct decrement of
 * __chttpsvr_destroy. The __chttpsvr_destroy function never blocks on it. */
static void _conn_on_removed(void *arg) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)arg;
  struct chttpserver *srv = conn->srv;
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) _conn_free(conn);
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(srv);
}

static void _conn_close(chttpsvr_conn_t *conn) {
  /* The code unlinks conn here, synchronously. It does not leave that to
   * _conn_free below. Once this function returns, neither the sweep of the
   * idle timeout nor a bounded shutdown drain may find conn in either list
   * again. The real free can still run much later. It runs from
   * _conn_on_removed, and only when conn->lifetime_refs permits it. Both
   * helpers are idempotent, so these two calls are harmless when conn was
   * never in either list. */
  _idle_list_remove(conn);
  _diverted_list_remove(conn);

  if (conn->reg) {
    ccol_event_reg reg = conn->reg;
    conn->reg = CCOL_EVENT_REG_INVALID;
    /* The code does NOT free conn here, on purpose. This can be the
     * self-removal of the registration, with no dispatch in flight. A free
     * would be correct for THIS registration alone. But conn->lifetime_refs
     * can still carry an outstanding contribution from an earlier
     * registration episode. The on_removed of that episode may not have
     * fired yet; see the comment of that field. The on_removed callback that
     * ccol_event_loop_remove() delivers for this exact registration is what
     * accounts for the count correctly, in any order. It reaches the shared
     * decrement-and-maybe-free path just below. */
    ccol_event_loop_remove(srv_engine_bundler.reactor, reg);
  }

  conn->state = CONN_ST_CLOSING;
  /* A body that ends here, cut off or refused, is freed and gives its
   * reservation of body memory back at once, so that a waiting request can
   * go ahead; see _conn_drop_body. */
  _conn_drop_body(conn);
  /* This is the "done with conn" vote of the application. The code counts it
   * exactly once for each _conn_close call. The library calls this function
   * only once for each conn. The single _conn_free call here depends on that
   * same invariant. See the field comment of conn->lifetime_refs. */
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) _conn_free(conn);
}

/* ========================================================================== */
/*                    PARSER CALLBACKS (reactor-thread side)                  */
/* ========================================================================== */

static int _on_request_line(chttp1_parser_t *p, const char *method,
                            size_t method_len, const char *target,
                            size_t target_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;

  /* This server speaks HTTP/1.x. A request line with another major version
   * (HTTP/0.9, HTTP/2.0, HTTP/3.0) is refused with 505 HTTP Version Not
   * Supported (RFC 9110 SS15.6.6) and a close, before anything reads the
   * target or the headers: this server does not know how such a message
   * frames its body, so no byte after the request line can be trusted. A
   * higher minor version of HTTP/1 is served as HTTP/1.1, which is what RFC
   * 9110 SS2.5 asks of a recipient that implements the major version. The
   * grammar of the version itself (one digit, a dot, one digit) is the
   * parser's; a line outside it is a 400. */
  if (p->http_major != 1) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
    return 1;
  }

  conn->method = _parse_method(method, method_len);

  /* CHTTP_ANY is a placeholder that exists only at registration time. It
   * means "match any concrete method". It is never the method of a real
   * incoming request. _CHTTP_METHOD_UNKNOWN is never one either. That is the
   * sentinel that _parse_method gives back for a method token that is valid
   * in syntax but that this server does not recognize. Examples are the
   * WebDAV verb PROPFIND, the verbs TRACE and CONNECT, and any custom verb.
   *
   * Without this check, such a request reaches the handler anyway. The
   * method_ok test of a CHTTP_ANY route in _find_route is
   * route->method == CHTTP_ANY, and that is true whatever the real method
   * is. chttpsvr_req_method() can then report only one of the seven named
   * chttp_method_t constants. It silently hands the handler a sentinel value
   * with no meaning. Its own doc comment in chttp.h promises the real method
   * of the incoming request instead.
   *
   * The code therefore rejects the request here, before any parse of the
   * path or the headers, and before any route match. This server implements
   * no method outside the seven that it recognizes. The status is 501. RFC
   * 7231 SS6.6.2 defines that as "the server does not support the
   * functionality required to fulfill the request". The rejection goes
   * through the same reject_pool machinery as every other rejection, which
   * are the 404, 405 and 500 cases. See the CHTTP1_USER handling of
   * _conn_pump and see _conn_dispatch_reject. */
  if (conn->method == _CHTTP_METHOD_UNKNOWN) {
    conn->req_rejected = true;
    /* The asterisk-form target "*" is a 400 with every method except
     * OPTIONS, a method that this server does not know included; see the
     * asterisk-form branch below. Go's net/http server answers the same. */
    conn->reject_status = (target_len == 1 && target[0] == '*')
                              ? CHTTP_STATUS_BAD_REQUEST
                              : CHTTP_STATUS_NOT_IMPLEMENTED;
    return 1;
  }

  /* The asterisk-form "*" of RFC 9112 SS3.2.4. "OPTIONS *" asks about the
   * server as a whole, and the server answers it itself, exactly as the
   * net/http server of Go does: 200 with a Content-Length of 0, and no route,
   * no middleware and no handler. See _options_star_route. Any other method
   * with this target is a 400 and a close, again as in Go. */
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

  /* An absolute-form target becomes the origin-form slice inside it. Read
   * the doc comment of _strip_absolute_form. was_absolute is what tells the
   * empty-path case below apart from an origin-form target, which can never
   * be empty. */
  const char *eff_target = target;
  size_t eff_target_len = target_len;
  bool was_absolute =
      _strip_absolute_form(target, target_len, &eff_target, &eff_target_len);

  const char *qmark = (const char *)memchr(eff_target, '?', eff_target_len);
  size_t path_raw_len = qmark ? (size_t)(qmark - eff_target) : eff_target_len;

  /* RFC 9112 SS3.2.1: an origin-form target is an absolute-path, and an
   * absolute-path begins with "/". The path of an absolute-form target is
   * either empty, which stands for "/" below, or begins with "/" as well.
   * Every other shape is refused with a 400 here, before the route search,
   * and before any middleware can read the path.
   *
   * Without this, a target such as "admin/secret" reaches the route of
   * "/admin/secret", because the segment splitter drops a leading "/" only
   * when there is one, while chttpsvr_req_path() reports "admin/secret". A
   * middleware that guards a prefix such as "/admin" then passes a request
   * that the guarded handler serves. "?x" reaches the route of "/" with an
   * empty path in the same way.
   *
   * The asterisk-form "*" never reaches this point; see the branch above. */
  if (path_raw_len > 0 ? eff_target[0] != '/' : !was_absolute) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
    return 1;
  }

  /* The code stores this value RAW, so it is still percent-encoded. See the
   * doc comment of conn->path for why it must not decode it here.
   *
   * An allocation failure here reports itself in the same way as every other
   * out-of-memory error in the middle of a request in this file. It sets
   * req_rejected and reject_status, and the library routes the request
   * through reject_pool for a graceful 500. A bare `return 1` with neither
   * field set still aborts the parse with CHTTP1_USER. But _conn_feed_bytes
   * reads that as an already-decided rejection only when req_rejected is
   * set. A field left unset here therefore drops the connection in silence
   * on a temporary allocation failure, with no response at all. The
   * _on_headers_complete function handles the same failure gracefully a few
   * callbacks later. */
  /* RFC 7230 SS5.3.1: an empty path component stands for "/". Only an
   * absolute-form target can have one, as in "http://host" or
   * "http://host?q=1". An origin-form target always starts with "/" and can
   * never be empty here, so this substitution never changes how the server
   * reads one. */
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

/* This callback fires only for the header block of the request.
 * chttp1_parser sends the trailer fields of a chunked body to
 * settings->on_trailer instead, and _init_parser_settings leaves that NULL
 * on purpose. Nothing that this function appends to conn->hdr_names and
 * conn->hdr_values can therefore come from after the body.
 *
 * That is what keeps the answer of chttpsvr_req_header stable. That function
 * scans the array backward, so a repeated header name resolves to its last
 * occurrence. A trailer appended here would outrank the real header of the
 * same name. A client could then displace a header that an upstream proxy
 * set on a request that this server already routed. */
static int _on_header(chttp1_parser_t *p, const char *name, size_t name_len,
                      const char *value, size_t value_len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  /* Every out-of-memory return below sets req_rejected and reject_status
   * before it returns. The _on_request_line function, right above this in
   * the file, handles an allocation failure in exactly the same way. See the
   * comment of that function for the reason. A bare `return 1` with neither
   * field set drops the connection in silence, with no response. Every other
   * allocation failure in the middle of a request in this file produces a
   * graceful 500 instead. */
  if (conn->hdr_count >= conn->hdr_cap) {
    /* This growth is safe against an overflow, and it uses the shared
     * helper. _servers_register and _chttpsvr_router_shell_register call
     * that same helper. chttpsvr_subrouter, _router_add_route and
     * _parse_qparams write the same overflow-safe pattern inline, with their
     * own cap formula, instead of a call to this function. All of them guard
     * against the same hazard: a plain "cap * 2" can wrap on an extreme cap.
     * It then silently allocates less than new_cap * sizeof(char *) below.
     *
     * Nothing reaches that case today. conn->hdr_count has the header-count
     * cap of chttp1_parser above it, which is at most 100 by default, and
     * chttpsvr_config_t offers no separate knob for it. The code keeps the
     * established idiom of this file anyway, instead of a dependence on that
     * bound never changing. */
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

  /* The parser decodes the chunked framing of a body and no other transfer
   * coding. A request such as "Transfer-Encoding: gzip, chunked" would hand
   * the handler a body that is still gzip-encoded, while the handler has no
   * way to learn that it is. RFC 9112 SS6.1 has a server that does not
   * understand a transfer coding answer 501, and the request is refused
   * before any route, middleware or handler sees it. The rejection closes
   * the connection, because the body is never read. */
  if (chttp1_has_other_transfer_coding(p)) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_NOT_IMPLEMENTED;
    return -1;
  }

  struct chttpserver *srv = conn->srv;
  if (conn->options_star) {
    /* No route search and no middleware snapshot: dispatch stays empty. The
     * request still diverts to a worker like any other, so that its body,
     * its keep-alive decision and a pipelined next request follow the one
     * code path that every request takes. */
    conn->matched_router = srv->root_router;
    conn->matched_route = &_options_star_route;
    conn->expects_continue = chttp1_expects_continue(p);
    conn->divert_gate = 0;
    return CHTTP1_HEADERS_DIVERT_BODY;
  }
  ccol_rw_lock_rdlock(srv->routes_lock);
  match_result_t mr = _find_route(srv, conn->path, conn->method);
  if (mr.result == ROUTE_MATCH_NONE || mr.result == ROUTE_MATCH_METHOD) {
    /* The chain of a sub-router that owns the path runs for its 404 or its
     * 405, exactly as it runs for a route of that sub-router. See the
     * ownership comment of _find_route. */
    bool chain_ok = _snapshot_chain(srv, mr.router, &conn->dispatch);
    ccol_rw_lock_unlock(srv->routes_lock);
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
    ccol_rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }

  dispatch_ctx_t *dispatch = &conn->dispatch;
  if (!_snapshot_chain(srv, mr.router, dispatch)) {
    ccol_rw_lock_unlock(srv->routes_lock);
    dispatch->mw_count = 0;
    _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }
  dispatch->route = mr.route;
  ccol_rw_lock_unlock(srv->routes_lock);

  /* The code rejects the request of a buffered route at once, before it
   * diverts that request to a worker thread. It does so when the declared
   * Content-Length already exceeds max_body_size. It does not wait until
   * that many bytes really stream in; see the check of _on_body. Without
   * this, a peer that declares an oversized Content-Length and then never
   * sends the body ties up a worker thread. The thread waits until a read
   * timeout fires, because no limit that counts bytes can trigger when the
   * bytes never arrive. See the doc comment of
   * chttp1_declared_content_length.
   *
   * The code excludes streaming routes on purpose. The documented contract
   * of this module for them lives in the doc comment of
   * chttpsvr_config_t.max_body_size. It says that the library ALWAYS calls
   * the handler, and that the handler decides its own response with
   * chttpsvr_req_read() and chttpsvr_req_stream_error(). An automatic
   * rejection here would break that contract in silence.
   *
   * A chunked body has an equivalent protection against an oversized CHUNK.
   * The library applies that one to both kinds of route in the same way,
   * through max_chunk_size_override; see _conn_reset_for_request. That
   * mechanism carries no "the handler always runs" contract to keep. It
   * surfaces through the ordinary error path of chttp1_parser_execute(),
   * which _drain_body and chttpsvr_req_read already handle. That is exactly
   * how they handle any other malformed body. */
  /* See the field comment of divert_gate. A buffered body that a
   * Content-Length frames must reserve body memory before a worker reads it,
   * and a streaming route runs on the streaming pool. */
  size_t divert_gate = 0;
  if (mr.route->is_streaming) {
    if (atomic_load(&srv->stream_threads) >= 0) divert_gate = SIZE_MAX;
  } else if (chttp1_has_content_length(p)) {
    size_t limit = atomic_load(&srv->max_body_size);
    /* A limit of 0 is the documented "no cap" sentinel of max_body_size; see
     * the doc comment of that field in chttpserver.h. It does not mean "cap
     * at zero". A bare `declared > limit` comparison rejects every request
     * with any body at all when a caller sets "no limit" in this way.
     * max_connections and max_header_bytes use the same convention in this
     * same config struct. */
    uint64_t declared = chttp1_declared_content_length(p);
    if (limit && declared > (uint64_t)limit) {
      _free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
      /* The effective chain from the snapshot just above still runs for this
       * rejection. That chain is the global one plus the chain of the
       * matched router. A middleware that authenticates the mount, or that
       * rate-limits it, therefore sees this request. The code clears the
       * route itself, because a rejected request must never reach a handler.
       * That clear is also what makes the end of the chain the 413 response. */
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

  /* A match that succeeds already proved that every segment of conn->path
   * decodes cleanly. That field is RAW, so it is still percent-encoded.
   * _match_route_cached and _prefix_matches both treat any decode failure as
   * a non-match. Such a request takes the ROUTE_MATCH_NONE or the
   * ROUTE_MATCH_METHOD branch above, and never reaches this point. The
   * decode of the whole path here, for the public chttpsvr_req_path()
   * accessor, therefore cannot fail on malformed input. Only a real
   * allocation failure can.
   *
   * The code rejects such a failure with a 500 here. It does so before it
   * moves mr.param_values into conn. Every other allocation-failure check in
   * this function does the same, which are the route-match failure and the
   * middleware-snapshot overflow above. The code must not divert a request
   * to a worker with conn->decoded_path left NULL. The doc comment of
   * chttpsvr_req_path() promises a pointer that is valid for the lifetime of
   * the request, for any req that is not NULL. It documents no NULL case
   * beyond a NULL req. A handler that calls strlen() on that pointer without
   * a check is therefore reasonable, and it would crash. */
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

  /* The library always diverts. It hands every matched route to the worker
   * pool, whether that route is buffered or streaming, and whatever the body
   * size is. That matches the documented threading model of this module.
   * chttp1_parser itself downgrades this to an ordinary immediate completion
   * when there is no body to divert; see the doc comment of
   * CHTTP1_HEADERS_DIVERT_BODY. Either way, the header-read callback below
   * submits to the worker pool once execute() returns CHTTP1_HEADERS_ONLY or
   * CHTTP1_PAUSED. */
  return CHTTP1_HEADERS_DIVERT_BODY;
}

/* The capacity that the body buffer of conn grows to, from cap, when it must
 * hold min_cap bytes. limit is the max_body_size that applies to the
 * request (0 for none), and min_cap never exceeds it.
 *
 * A buffered body that a Content-Length frames gets its declared length in
 * one allocation when body memory is limited and the request reserved that
 * whole length before its first byte was read, or holds no reservation
 * because the whole body came with its headers. The buffer then never holds
 * more than the reservation and is never copied. Otherwise nothing vouches
 * for the declared length before the bytes arrive (no limit on body memory,
 * or a request that runs past the limit with a reservation clamped to it),
 * so the buffer doubles as the bytes arrive, never past that length.
 *
 * Every other body doubles from 8 KiB, never past limit. A chunked body of a
 * buffered route reserves the result before the read that needs it; see
 * _drain_body_socket_read, which calls this function with the same rule.
 *
 * The doubling is safe against an overflow, and it follows the pattern of
 * _head_builder_grow. A plain "keep doubling" loop can wrap to 0, which can
 * never reach min_cap. That happens when limit sits close to SIZE_MAX. */
static size_t _body_grow_target(const chttpsvr_conn_t *conn, size_t cap,
                                size_t min_cap, size_t limit) {
  if (min_cap <= cap) return cap;
  size_t ceiling = limit;
  if (!conn->matched_route->is_streaming && !conn->options_star &&
      chttp1_has_content_length(&conn->parser)) {
    uint64_t declared = chttp1_declared_content_length(&conn->parser);
    size_t d = declared < (uint64_t)SIZE_MAX ? (size_t)declared : SIZE_MAX;
    if (d >= min_cap) {
      if (atomic_load(&conn->srv->mem_cap) != SIZE_MAX &&
          (conn->mem_charged >= d ||
           (conn->mem_charged == 0 && !conn->mem_exempt)))
        return d;
      if (!ceiling || d < ceiling) ceiling = d;
    }
  }
  size_t new_cap = cap ? cap : 8192;
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

/* This one callback and one growbuf_t serve two jobs. They accumulate the
 * body of a buffered route. They also accumulate the bytes of a streaming
 * route that wait, because chttpsvr_req_read did not take them yet. The code
 * enforces max_body_size here, in the same way for both kinds of route.
 * chttp1_parser has no notion of that limit and leaves it to the caller. */
static int _on_body(chttp1_parser_t *p, const char *at, size_t len) {
  chttpsvr_conn_t *conn = (chttpsvr_conn_t *)p->data;
  /* This accumulation is safe against an overflow. It matches the guard on
   * the growth of the growbuf a few lines below in this same function. On a
   * 32-bit (ILP32) build, a streaming route can receive well over 4 GiB of
   * body in one request. The growbuf of such a route compacts back to empty
   * as chttpsvr_req_read drains it, so the connection never holds more than
   * one batch at once. Without this guard, body_bytes_seen, which is a
   * size_t, silently wraps back near zero once the total passes that
   * threshold. The max_body_size check then starts to pass again, and the
   * size cap has no effect for the rest of the request. The code treats this
   * as an ordinary over-limit body: it rejects instead of a wrap. */
  if (len > SIZE_MAX - conn->body_bytes_seen) {
    conn->body_too_large = true;
    return 1;
  }
  conn->body_bytes_seen += len;
  size_t body_limit = conn->options_star
                          ? _CHTTPSVR_OPTIONS_STAR_MAX_BODY
                          : atomic_load(&conn->srv->max_body_size);
  /* A body_limit of 0 is the documented "no cap" sentinel. The
   * Content-Length pre-check of _on_headers_complete above has the same
   * special case. See the comment of that check. */
  if (body_limit && conn->body_bytes_seen > body_limit) {
    conn->body_too_large = true;
    return 1;
  }
  growbuf_t *b = &conn->body;
  /* A buffered route never drains through pos, because the library does not
   * use chttpsvr_req_read for one. Its pos therefore stays 0, and this test
   * is never true for it. There is nothing to compact in that case. */
  if (conn->matched_route->is_streaming && b->pos == b->len && b->pos > 0)
    b->pos = b->len = 0; /* compact: an earlier req_read call drained it */
  /* The code computes min_cap, and checks it for a wraparound, BEFORE it
   * compares it against b->cap. A comparison of the unguarded sum
   * "b->len + len > b->cap" first, with the overflow check only INSIDE that
   * branch, is not equivalent. If the unguarded sum itself wraps around
   * SIZE_MAX, the small wrapped result can satisfy "<= b->cap". The code
   * then skips the growth branch, and the overflow check inside it, in full.
   * It falls through to the memcpy below with b->len still at its huge value
   * from before the wrap. That is a heap buffer overflow.
   *
   * This follows the correct construction of _head_builder_grow: compute
   * min_cap, then detect the wraparound with "min_cap < b->len", before any
   * other use of the value. To reach this case, b->len must be within `len`
   * of SIZE_MAX. That is out of reach on a 64-bit build, where b->cap would
   * already have to hold about 2^64 bytes. It is a real, if narrow, concern
   * on an ILP32 build, where SIZE_MAX is about 4.29 GiB. It needs a
   * max_body_size configured close to SIZE_MAX, for a streaming route whose
   * handler drains slower than the peer sends. */
  size_t min_cap = b->len + len;
  if (min_cap < b->len) {
    /* This is a size_t overflow. A body this size cannot exist at all on
     * this platform. That is the same "too large to hold" outcome that the
     * body_bytes_seen overflow guard above already reports in this way. It
     * is not a temporary allocation failure. */
    conn->body_too_large = true;
    return 1;
  }
  if (min_cap > b->cap) {
    size_t old_cap = b->cap;
    size_t new_cap = _body_grow_target(conn, old_cap, min_cap, body_limit);
    char *nb = (char *)_ccol_mem_realloc(conn->m_procs, b->buf, new_cap);
    if (!nb) {
      /* This is a real allocation failure. It differs from every other
       * reason that this function returns 1. See the field comment of
       * body_alloc_failed for why it must not join the general
       * transfer-aborted group. */
      conn->body_alloc_failed = true;
      return 1;
    }
    b->buf = nb;
    b->cap = new_cap;
#ifdef RUNNING_UNIT_TESTS
    _body_growth_note_for_tests(conn, old_cap, new_cap);
#endif
    /* The old block is gone, so a chunked body keeps only the reservation
     * that its new buffer needs; see _drain_body_socket_read. */
    if (__builtin_expect(conn->mem_charged > new_cap, 0) &&
        !chttp1_has_content_length(&conn->parser))
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
  ccol_once_flag_t once;
} srv_parser_bundler = {0};

static void _init_parser_settings(void) {
  chttp1_settings_init(&srv_parser_bundler.settings);
  srv_parser_bundler.settings.on_request_line = _on_request_line;
  srv_parser_bundler.settings.on_header = _on_header;
  /* settings.on_trailer stays NULL, because chttp1_settings_init zeroed it.
   * This server offers no trailer API. A callback left unset is what makes a
   * trailer field structurally unable to reach the header array of the
   * request. See the comment of _on_header. */
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

/* The pool of that kind that srv runs with now; the streaming pool is
 * created on demand, as for any streaming request. */
static ctpool _srv_current_pool(struct chttpserver *srv, _srv_pool_kind_t k) {
  if (k == _SRV_POOL_STREAM) return _stream_pool_get(srv);
  ccol_mutex_lock(srv->mutex);
  ctpool p = k == _SRV_POOL_WORKER ? srv->worker_pool : srv->reject_pool;
  ccol_mutex_unlock(srv->mutex);
  return p;
}

#ifdef RUNNING_UNIT_TESTS
/* Holds the nth worker submit of _conn_start_diverted from now, after it
 * read the pool and before it submits, until the test releases it, so that
 * a test can restart the server inside that window. retried counts the
 * submits that _srv_submit_retry moved to a newer pool. */
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

/* The cold half of _srv_submit: the first pool refused the task. When a
 * restart replaced that pool after the caller read it, the refusal comes
 * from a retired pool that is draining, and the task goes to the pool that
 * srv runs with now. A request that arrives during a restart is therefore
 * never refused because of the restart. A refusal by the current pool (a
 * full queue) is the answer. */
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
 * and retries on the current pool of the same kind when a restart retired
 * pool in between; see _srv_submit_retry. */
static inline __attribute__((always_inline)) bool _srv_submit(
    struct chttpserver *srv, _srv_pool_kind_t k, ctpool pool,
    void (*fn)(void *), void *arg) {
  if (__builtin_expect(
          pool && ctpool_try_submit(pool, fn, arg, NULL) == ccol_success, 1))
    return true;
  return _srv_submit_retry(srv, k, pool, fn, arg);
}

/* Decrements srv->in_flight_requests. When the counter reaches zero, it
 * wakes every thread that waits in _drain_and_close_all_connections. Every
 * path that increments in_flight_requests,
 * which is _conn_start_diverted, must call this exactly once.
 *
 * The timing matters. The call must come only once the connection reaches a
 * state that _drain_and_close_all_connections can observe. There are two
 * such states. The library closed the connection in full with _conn_close,
 * and that includes a close through _conn_reject_and_close. Or the library
 * published the connection safely back into the idle list with
 * _idle_list_add.
 *
 * A call any earlier opens a real window. Here are two examples. The first
 * is a call before the library resumes or re-adds the registration of a
 * keep-alive connection and idle-lists it. The second is a call before the
 * library writes the courtesy response of a rejected connection and closes
 * it. In that window the waiter can
 * wake, read in_flight_requests == 0, and let __chttpsvr_destroy free srv.
 * Another thread is still finishing this connection at that moment. That is
 * a real use-after-free, and ThreadSanitizer reports it over tests_tls. */
static void _release_in_flight(struct chttpserver *srv) {
  ccol_mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0)
    ccol_cond_var_broadcast(srv->requests_done_cv);
  ccol_mutex_unlock(srv->mutex);
}

#ifdef RUNNING_UNIT_TESTS
/* A white-box hook for the tests. While it is armed, every pause of
 * _conn_pause_reg takes the path of a pause that fails. The counter counts
 * every removal that path made. */
static _Atomic bool g_force_pause_fail_for_tests = false;
static _Atomic size_t g_pause_fallback_count_for_tests = 0;
void _chttpsvr_force_pause_fail_for_tests(bool force) {
  atomic_store(&g_force_pause_fail_for_tests, force);
}
size_t _chttpsvr_pause_fallback_count_for_tests(void) {
  return atomic_load(&g_pause_fallback_count_for_tests);
}
#endif

/* Pauses the registration of conn while a thread owns the connection, so
 * that no readiness fires meanwhile. The caller owns conn. A pause that
 * fails falls back to a removal, and the next wait of the connection then
 * adds a registration of its own; see _conn_park_arm and _task_tail. A
 * removal of a registration that is already gone is a documented no-op, and
 * one that is still live is what reclaims it. */
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
/* A white-box hook for the tests. Armed, the next rejection skips reject_pool
 * and takes the inline answer of _conn_reject_via_pool, exactly as when that
 * pool is full, on the thread that rejects it, which is the reactor thread
 * for a route that does not match. It disarms itself after one use. */
static _Atomic bool g_force_reject_inline_for_tests = false;
void _chttpsvr_force_reject_inline_for_tests(bool force) {
  atomic_store(&g_force_reject_inline_for_tests, force);
}
#endif

/* Submits conn to reject_pool for its courtesy rejection response. The
 * caller already set conn->reject_status. The middleware of the rejection
 * therefore never runs on the calling thread. The function falls back to an
 * inline answer on the calling thread, with no middleware, in two cases. In
 * the first, reject_pool is not available because the server tears down. In
 * the second, the bounded queue of that pool (_CHTTPSVR_REJECT_POOL_QUEUE_CAP)
 * is full.
 *
 * Both rejection paths in this file share this function. The first is the
 * pool-full 503 case in _conn_start_diverted. That path already incremented
 * in_flight_requests, and it already paused or removed conn->reg itself,
 * because the outcome was not yet known there. The outcome is either "keep
 * this connection alive" or "reject it". The second path is the synchronous
 * 404, 405 and 500 rejection of the reactor thread, through
 * _conn_dispatch_reject below.
 *
 * Every caller must already have incremented srv->in_flight_requests for
 * this connection. This function releases that count with _release_in_flight
 * only once the close really completes. It does not release it once the task
 * is merely queued. See the comment of _release_in_flight for why a release
 * any earlier is a real use-after-free. */
static void _conn_reject_via_pool(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->mutex);
  ctpool reject_pool = srv->reject_pool;
  ccol_mutex_unlock(srv->mutex);

  bool inline_only = false;
#ifdef RUNNING_UNIT_TESTS
  inline_only = atomic_exchange(&g_force_reject_inline_for_tests, false);
#endif
  if (!inline_only &&
      _srv_submit(srv, _SRV_POOL_REJECT, reject_pool, _reject_task, conn)) {
    return;
  }

  /* This is the last-resort fallback. It runs when reject_pool is not
   * available, because the server tears down. It also runs when the bounded
   * queue of that pool is full, or when the submit fails for another reason
   * such as an allocation failure. The code then answers on the calling
   * thread, with no middleware. The write never waits on the peer, exactly
   * as at every other reject-and-close call site, so this is safe on the
   * reactor thread and on the sweep. The code captured srv above, before
   * _conn_reject_and_close, which can free conn or park it. */
  _conn_reject_and_close(conn, /*run_middleware=*/false);
  _release_in_flight(srv);
}

/* Routes a rejected connection through reject_pool. The rejection came from
 * the synchronous header parse on the reactor thread, and it is a 404, a 405
 * or a 500. _on_headers_complete already set conn->req_rejected and
 * conn->reject_status. This is the same route that the pool-full 503 case
 * below takes. A client that reads slowly, and that the server must tell
 * about a bad route, therefore cannot stall the sole reactor thread either.
 *
 * The registration is paused, exactly as in _conn_start_diverted, and not
 * removed. A rejection never keeps the connection alive, but the connection
 * can still wait for the reactor: a courtesy response that meets a full
 * socket parks, and a lingering close waits for input. Both arm this same
 * registration again with a modify and a resume, which allocate nothing and
 * cannot fail for lack of memory, where a removal would make that wait
 * allocate a new registration and handle a failed add. A pause that fails
 * falls back to a removal, as in _conn_start_diverted, and the later wait
 * then adds a registration of its own; a registration added for a
 * descriptor after the removal of its earlier one is watched like any other
 * (see ccol_event_loop_remove).
 *
 * The code sets conn->state to CONN_ST_DIVERTED, and it adds conn to
 * srv->diverted_head and srv->diverted_tail with _diverted_list_add. That is
 * exactly what _conn_start_diverted does for a matched-route request. The
 * courtesy response of a rejection, which _conn_reject_via_pool writes, runs
 * on a thread of reject_pool, or on the calling thread itself in the
 * fallback case, and never waits on the peer. The connection stays in the
 * registry while that thread owns it, so that a teardown can still find it,
 * as it finds every connection that a thread owns.
 *
 * Without this registration, _force_unblock_diverted_connections cannot see
 * a connection that the library rejected here. _wait_in_flight_bounded calls
 * that function. A restart with chttpsvr_stop() plus chttpsvr_start() calls
 * _wait_in_flight_bounded, and so do the teardowns of chttpsvr_destroy() and
 * chttpsvr_engine_stop(). A graceful shutdown or restart then has no way to
 * interrupt an ordinary rejected request that is parked on a peer that reads
 * slowly. Such a request is a 404, 405, 413, 500 or 501. It can only wait
 * out the duration
 * bound of that write inside ctpool_shutdown_drain(reject_pool), which has
 * no timeout of its own. It must do that once for every rejection that the
 * pool still holds in its queue.
 *
 * How long each of those waits is depends on the operator, through
 * max_response_write_duration_ms. An operator may set that to 0, which turns
 * it off. The registration here is therefore unconditional, and it does not
 * depend on that setting. The registry is what the forced-unblock mechanism
 * searches.
 *
 * _conn_free already calls _diverted_list_remove without exception. Every
 * close path reaches _conn_free through _conn_close, and that includes
 * _conn_reject_and_close. No further cleanup is needed here. */
static void _conn_dispatch_reject(chttpsvr_conn_t *conn) {
  _idle_list_remove(conn);
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
  ccol_mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ccol_mutex_unlock(conn->srv->mutex);
  _conn_reject_via_pool(conn);
}

static void _conn_start_diverted(chttpsvr_conn_t *conn, const char *leftover,
                                 size_t leftover_len) {
  _idle_list_remove(conn);
  /* The code pauses the registration instead of a remove. A resume through
   * ccol_event_loop_resume is cheap. It runs once the worker finishes this
   * request and the connection waits for the next one. A pause therefore
   * avoids a full allocate and free of an event_entry on every keep-alive
   * request cycle. It also avoids churn in the fd registry chmap.
   *
   * A pause should not fail in practice. Nothing else touches the reg of
   * this connection while the reactor still owns it. Every documented
   * failure mode of ccol_event_loop_pause for a reg with a real fd reduces
   * to "another thread removed reg". A failed pause removes the
   * registration (see _conn_pause_reg), and the connection then goes on
   * exactly like one that never had a live registration: its next wait, in
   * the keep-alive tail of _task_tail or in _conn_park_arm, adds a fresh
   * registration for the same descriptor instead of a resume of a stale
   * reg. */
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  /* This call is idempotent. It is a no-op for a connection that the
   * registry already holds, from an earlier divert cycle that did not finish
   * yet. That happens when the library diverts a further pipelined request.
   * See the comment of _diverted_list_remove for where the matching removal
   * happens. */
  _diverted_list_add(conn);

  ccol_mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ctpool pool = conn->srv->worker_pool;
  ccol_mutex_unlock(conn->srv->mutex);

  /* The code copies leftover now. That pointer goes into the stack read
   * buffer of the reactor. That buffer leaves its scope the moment this
   * callback returns. The _task_worker function derives nothing from
   * chttp1_parser_consumed() itself. That value has meaning only against the
   * exact buffer and length pair of the execute() call that produced it.
   * That pair is the stack buffer of the reactor, which is gone by the time
   * the worker runs. A direct pass of the copied carry bytes avoids the
   * lifetime hazard entirely. */
  char *carry = NULL;
  bool carry_alloc_failed = false;
  if (leftover_len > 0) {
    carry = (char *)_ccol_mem_alloc(conn->m_procs, leftover_len);
    if (carry)
      memcpy(carry, leftover, leftover_len);
    else
      carry_alloc_failed = true;
  }
  /* The code publishes carry and _carry_over_len BEFORE it submits to the
   * pool, and never after. ctpool_try_submit can hand this task to a worker
   * thread that is already idle. That thread then starts _task_worker(conn)
   * at once, in parallel with the rest of this function. A write of these
   * fields after the submit call races the read of conn->_carry_over on that
   * worker thread. The worker sees NULL, which is the steady-state value of
   * this field between requests. The _task_worker function always frees it
   * and sets it to NULL after it consumes it. The worker therefore drops the
   * real leftover bytes of this request in silence. By the time this
   * function reaches the assignment, which is now too late, nothing will
   * ever free
   * that orphaned buffer. That is both a data-loss bug and a leak, and
   * valgrind reports the leak.
   *
   * _carry_over_len must stay in step with whether _carry_over is really not
   * NULL. A value left at leftover_len after the allocation above failed
   * hands the worker thread a NULL pointer with a nonzero length.
   * chttp1_stream_prepare and chttp1_stream_prepare_tls then memcpy()
   * leftover_len bytes FROM that pointer, without a check. A memcpy with a
   * NULL source is not a no-op. */
  conn->_carry_over = carry;
  conn->_carry_over_len = carry ? leftover_len : 0;

  if (carry_alloc_failed) {
    /* The leftover bytes are pipelined request-body bytes. The library
     * already read them off the wire, and they are gone from the socket for
     * good. The code cannot drop them in silence, because the worker would
     * then read the body from the wrong offset and truncate it in silence.
     * It also cannot ignore the NULL-pointer hazard above. The code
     * therefore rejects with a 500. That is exactly what the pool-full 503
     * case below does. The code already incremented in_flight_requests, and
     * it already paused or removed conn->reg, above. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    _conn_reject_via_pool(conn);
    return;
  }

  /* A streaming route goes to the streaming pool, and a buffered body that
   * did not arrive together with its headers reserves body memory first; see
   * _conn_divert_gate. Every other request pays one compare here. */
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
    /* The worker pool is at its capacity. ctpool_try_submit returns
     * ccol_container_full instead of a block, which matches the documented
     * "never block the reactor thread" contract of this module. This is a
     * real server condition, even if a temporary one. The client should hear
     * about it through a synchronous 503, and not through a bare connection
     * reset. See bounded_pool_full_returns_503 in tests.c. */
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;

    /* The code already incremented in_flight_requests above, and it already
     * paused or removed conn->reg there. It did both before it knew whether
     * this connection would divert successfully or be rejected.
     * _conn_reject_via_pool takes over from here. It does not submit to
     * `pool` again. That is the exact pool that just rejected this request
     * because it is full. See the comment of that function for the rest. */
    _conn_reject_via_pool(conn);
    return;
  }
}

/* Fills conn->decoded_path for a request that the library rejects before the
 * routing sets that field. A middleware that runs for that rejection can
 * then still read the target through chttpsvr_req_path(). Both guards below
 * are defensive. The library decides every rejection that runs a middleware
 * chain at headers-complete. That is after the parse of the request line,
 * and before the matched-route path sets decoded_path itself. In practice
 * conn->path is therefore always set, and decoded_path never is.
 *
 * The code reports a target whose percent-encoding is malformed exactly as
 * it arrived, in its encoded form. That malformed encoding is itself one of
 * the reasons why no route could match it, and there is no decoded form to
 * report. An access log that leaves out the one request most worth a record
 * is worse than one that records exactly what the peer sent.
 *
 * An allocation failure leaves the field NULL. That is the same answer that
 * every other caller of chttpsvr_req_path() gets when the library could not
 * allocate the decode. */
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

/* Writes the value of an Allow header for a mask of allowed_methods into out,
 * which holds at least 64 bytes. The methods go in the order GET, HEAD, POST,
 * PUT, DELETE, PATCH, OPTIONS, separated by ", ". The longest value, with all
 * seven, is 44 bytes with its terminator. */
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
 * before its courtesy response goes out. It is true only from _reject_task.
 * That means it is true only on one of the dedicated threads of
 * reject_pool. That restriction is what bounds how much application code a
 * flood of rejected requests can start. reject_pool has a fixed thread count
 * and a bounded queue, exactly like the worker pool that the handler of a
 * matched route runs on.
 *
 * The flag is false on the last-resort synchronous fallback of
 * _conn_reject_via_pool. That fallback runs on whichever thread called it,
 * which is the sole reactor thread in the default configuration. It runs
 * there precisely because reject_pool is saturated or gone. The library
 * sends that rejection bare. It does not run application code on a thread
 * where that code must not block.
 *
 * _reject_status_runs_middleware narrows the flag further. It limits it to
 * the statuses for which a chain has any meaning at all. */
static void _conn_reject_and_close(chttpsvr_conn_t *conn, bool run_middleware) {
  /* The code uses conn->resp, and not a fresh response on the stack. No
   * handler ran for this request, because a rejection never diverts to the
   * worker pool. conn->resp is therefore already in its first state for this
   * request. It is also what a middleware writes into when one runs below.
   * _conn_free destroys it. */
  chttpsvr_resp *resp = &conn->resp;
  resp->status_code = conn->reject_status;
  /* Every 503 of this library means "the server is saturated right now", so
   * it tells the client when a retry is reasonable. A failed allocation of
   * the header only leaves it out. */
  if (conn->reject_status == CHTTP_STATUS_SERVICE_UNAVAILABLE)
    (void)chttpsvr_resp_set_header(resp, "retry-after",
                                   _CHTTPSVR_RETRY_AFTER_SECONDS);
  /* A 405 names the methods that the target resource does support (RFC 9110
   * SS15.5.6). A failed allocation of the header only leaves it out. */
  if (conn->reject_status == CHTTP_STATUS_METHOD_NOT_ALLOWED &&
      conn->allowed_methods) {
    char allow[64];
    _format_allow_header(conn->allowed_methods, allow);
    (void)chttpsvr_resp_set_header(resp, "allow", allow);
  }
  bool wants_middleware = run_middleware && conn->dispatch.mw_count > 0 &&
                          _reject_status_runs_middleware(conn->reject_status);
  if (wants_middleware) _conn_set_rejected_decoded_path(conn);
  /* conn->decoded_path is the last condition. It is a check against memory
   * pressure, and not a policy check. The documentation of
   * chttpsvr_req_path() says that it hands a middleware a readable path. The
   * decode above is the only thing that can fail to produce one. A
   * middleware that logs the path, or matches on it, would dereference NULL.
   * A rejection whose path the library could not decode therefore goes out
   * bare. That is exactly what one does when reject_pool is saturated. The
   * matched-route path answers the same failure with a 500, for the same
   * reason. */
  if (wants_middleware && conn->decoded_path) {
    conn->dispatch.route = NULL;
    conn->dispatch.mw_idx = 0;
    chttpsvr_req req;
    memset(&req, 0, sizeof(req));
    req.conn = conn;
    /* There is no stream and no route here. The library never reads the body
     * of this request, because it decides a rejection at headers-complete,
     * before it consumes one body byte. No handler exists either.
     * chttpsvr_req_read() therefore reports -1, and chttpsvr_req_param()
     * reports NULL. Each does so through the guard that it already carries
     * for a req with no matched route. The method, the path, the headers and
     * the query string all stay readable. That is what an access log or a
     * rate limiter needs. */
    req.stream = NULL;
    req.param_names = NULL;
    req.m_procs = conn->m_procs;
    _chttpsvr_next(&req, resp);
    _destroy_req_qparams(&req);
  }
  chttp1_stream_t stream;
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                              conn->m_procs);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  /* Every connection of this server is non-blocking; see accept4(). The
   * response never waits for a client that does not read: a full socket
   * parks the connection with the unsent rest, on no thread, and the reactor
   * resumes it once the socket takes bytes again; see _task_continue_write.
   * The thread that runs this function, which can be the reactor thread, the
   * sweep or a thread of a pool, therefore never waits on the peer.
   *
   * conn turns on the total-duration bound of the write, which
   * max_response_write_duration_ms sets, and is_reject turns on
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS over it as an unconditional
   * ceiling. A caller who sets max_response_write_duration_ms to 0, which
   * turns it off, therefore still gets a bounded rejection, whether it goes
   * out at once or waits parked: the sweep and the resumed write both judge
   * the same deadline. conn->write_deadline_set is always false on entry
   * here, because _conn_reset_for_request clears it before any header
   * parse, and so before any rejection decision. */
  stream.fd_nonblocking = true;
  stream.write_nonblocking = true;
  bool sent =
      _send_response(&stream, resp, false, 0, conn->method == CHTTP_HEAD, conn,
                     /*is_reject=*/true);
  chttp1_stream_release(&stream);
  if (!sent && conn->wp_active) {
    /* The socket is full. Nothing of the request is read again, so its body
     * memory goes back now and not when the write ends. */
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
  /* The request is refused, so its body, if it has one, stays unread. A
   * close with unread bytes makes the kernel send a reset, which can
   * destroy the response in the client before the client reads it. */
  if (sent && _conn_request_bytes_may_remain(conn))
    _conn_linger(conn);
  else
    _conn_close(conn);
}

#ifdef RUNNING_UNIT_TESTS
/* This is white-box test instrumentation and nothing else. It counts how
 * many rejections really ran on a thread of reject_pool. The synchronous
 * fallback in _conn_reject_via_pool never reaches _reject_task at all, so it
 * adds nothing to this count. The counter covers the whole process, and not
 * one server. _chttpsvr_engine_num_reactor_threads_for_tests already
 * established that convention. A test therefore reads the difference across
 * its own window, and not an absolute value. Other tests in the same
 * process can also cause rejections. The guard keeps this symbol and this
 * counter out of a production build entirely. */
static _Atomic size_t g_reject_task_run_count_for_tests = 0;

/* This is a white-box test hook and nothing else. It makes the library treat
 * the first chttp1_stream_prepare() or chttp1_stream_prepare_tls() call of
 * _task_worker as a failure. That exercises the same body_unavailable
 * fallback path that a real allocation failure takes; see the comment of
 * _task_worker about body_unavailable.
 *
 * A test cannot reproduce a real failure there every time. The carry-over
 * copy of that one call has the same size as the conn->_carry_over
 * allocation that _conn_start_diverted makes before it, from the same
 * allocator. A custom allocator that fails at a given size therefore fails
 * that earlier allocation first, and never reaches this one; see
 * carry_over_alloc_failure_rejects_gracefully_instead_of_crashing in
 * tests.c. The guard keeps this symbol out of a production build
 * entirely. */
static _Atomic bool g_force_stream_prepare_fail_for_tests = false;
void _chttpsvr_force_stream_prepare_fail_for_tests(bool force) {
  atomic_store(&g_force_stream_prepare_fail_for_tests, force);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * chttpsvr_start() blocks at one fixed point. That point is right after the
 * call confirms or acquires the shared engine reference, and registers
 * itself with servers_bundler. It is before any further reactor work, which
 * is _idle_sweep_start_if_needed, _make_listen_socket and the
 * ccol_event_loop_add for the listener. The hook signals that it entered,
 * and it then waits for an explicit release.
 *
 * This lets a test land a concurrent chttpsvr_engine_stop() call inside an
 * exact window, every time. That window is the one that the early
 * _servers_register call of this server closes; see the comment of that call
 * site. The pending_resolve_count wait of _quiesce_server_once closes it
 * too. Without the hook, a test can only use a fixed sleep and hope to hit a
 * race window that is a few instructions wide.
 *
 * The hook fires once for each arm call. The guard keeps all of it out of a
 * production build. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_race_hook = {0};

static void _start_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_start_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_start_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_race_hook_for_tests(void) {
  ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  ccol_mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.armed = true;
  g_start_race_hook.entered = false;
  g_start_race_hook.go = false;
  ccol_mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_wait_start_race_hook_entered_for_tests(void) {
  ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  ccol_mutex_lock(g_start_race_hook.mutex);
  while (!g_start_race_hook.entered)
    ccol_cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  ccol_mutex_unlock(g_start_race_hook.mutex);
}

void _chttpsvr_release_start_race_hook_for_tests(void) {
  ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  ccol_mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.go = true;
  ccol_cond_var_broadcast(g_start_race_hook.cv);
  ccol_mutex_unlock(g_start_race_hook.mutex);
}

static void _start_race_hook_wait_if_armed(void) {
  ccol_call_once(g_start_race_hook.once, _start_race_hook_init_globals);
  ccol_mutex_lock(g_start_race_hook.mutex);
  if (!g_start_race_hook.armed) {
    ccol_mutex_unlock(g_start_race_hook.mutex);
    return;
  }
  g_start_race_hook.armed = false; /* one-shot */
  g_start_race_hook.entered = true;
  ccol_cond_var_broadcast(g_start_race_hook.cv);
  while (!g_start_race_hook.go)
    ccol_cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  ccol_mutex_unlock(g_start_race_hook.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * chttpsvr_start() blocks at one fixed point. That point is right after the
 * call sets contributed_to_engine to true for itself. A concurrent
 * _engine_force_stop_quiesce_all pass that finds raw registered therefore
 * already sees a real engine contribution to release. The point is also
 * right before the _engine_acquire() call of chttpsvr_start().
 *
 * That is the exact window in which _engine_acquire() can see
 * srv_engine_bundler.stopping == true and return without a block on it. See
 * the doc comment of that function for the deadlock that this closes. See
 * the "currently stopping" branch of chttpsvr_start() for the caller-side
 * retry that this hook reproduces every time.
 *
 * The hook fires once for each arm call. The guard keeps all of it out of a
 * production build. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_engine_stopping_race_hook = {0};

static void _engine_stopping_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_engine_stopping_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_engine_stopping_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_engine_stopping_race_hook_for_tests(void) {
  ccol_call_once(g_engine_stopping_race_hook.once,
                 _engine_stopping_race_hook_init_globals);
  ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.armed = true;
  g_engine_stopping_race_hook.entered = false;
  g_engine_stopping_race_hook.go = false;
  ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_wait_engine_stopping_race_hook_entered_for_tests(void) {
  ccol_call_once(g_engine_stopping_race_hook.once,
                 _engine_stopping_race_hook_init_globals);
  ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  while (!g_engine_stopping_race_hook.entered)
    ccol_cond_var_wait(g_engine_stopping_race_hook.cv,
                       g_engine_stopping_race_hook.mutex);
  ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void _chttpsvr_release_engine_stopping_race_hook_for_tests(void) {
  ccol_call_once(g_engine_stopping_race_hook.once,
                 _engine_stopping_race_hook_init_globals);
  ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.go = true;
  ccol_cond_var_broadcast(g_engine_stopping_race_hook.cv);
  ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

static void _engine_stopping_race_hook_wait_if_armed(void) {
  ccol_call_once(g_engine_stopping_race_hook.once,
                 _engine_stopping_race_hook_init_globals);
  ccol_mutex_lock(g_engine_stopping_race_hook.mutex);
  if (!g_engine_stopping_race_hook.armed) {
    ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
    return;
  }
  g_engine_stopping_race_hook.armed = false; /* one-shot */
  g_engine_stopping_race_hook.entered = true;
  ccol_cond_var_broadcast(g_engine_stopping_race_hook.cv);
  while (!g_engine_stopping_race_hook.go)
    ccol_cond_var_wait(g_engine_stopping_race_hook.cv,
                       g_engine_stopping_race_hook.mutex);
  ccol_mutex_unlock(g_engine_stopping_race_hook.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * chttpsvr_start() blocks at one fixed point. That point is right after
 * _chttpsvr_resolve(h) succeeds, so the resolve pin of this call, which is
 * pending_resolve_count, is already live. It is also before the call does
 * anything else at all, and that includes its own lifecycle and
 * quiesce_state checks. The hook signals that it entered, and it then waits
 * for an explicit release.
 *
 * This hook differs from g_start_race_hook above, which pauses much later,
 * after the registration and the engine acquire. This one exists to land the
 * resolve pin of a chttpsvr_start() call at a precise point. That point is
 * BEFORE a concurrent _quiesce_server_once pass for the same server reaches
 * its own pending_resolve_count wait. It does so every time. That
 * interleaving deadlocks the two against each other, but only if the wait
 * of chttpsvr_start() blocks on quiesce_done_cv while it still holds this
 * pin.
 * See the comment of that wait, which is why it does not block there.
 *
 * The hook fires once for each arm call. The guard keeps all of it out of a
 * production build. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_resolve_race_hook = {0};

static void _start_resolve_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_start_resolve_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_start_resolve_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_resolve_race_hook_for_tests(void) {
  ccol_call_once(g_start_resolve_race_hook.once,
                 _start_resolve_race_hook_init_globals);
  ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.armed = true;
  g_start_resolve_race_hook.entered = false;
  g_start_resolve_race_hook.go = false;
  ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_wait_start_resolve_race_hook_entered_for_tests(void) {
  ccol_call_once(g_start_resolve_race_hook.once,
                 _start_resolve_race_hook_init_globals);
  ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  while (!g_start_resolve_race_hook.entered)
    ccol_cond_var_wait(g_start_resolve_race_hook.cv,
                       g_start_resolve_race_hook.mutex);
  ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

void _chttpsvr_release_start_resolve_race_hook_for_tests(void) {
  ccol_call_once(g_start_resolve_race_hook.once,
                 _start_resolve_race_hook_init_globals);
  ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.go = true;
  ccol_cond_var_broadcast(g_start_resolve_race_hook.cv);
  ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

static void _start_resolve_race_hook_wait_if_armed(void) {
  ccol_call_once(g_start_resolve_race_hook.once,
                 _start_resolve_race_hook_init_globals);
  ccol_mutex_lock(g_start_resolve_race_hook.mutex);
  if (!g_start_resolve_race_hook.armed) {
    ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
    return;
  }
  g_start_resolve_race_hook.armed = false; /* one-shot */
  g_start_resolve_race_hook.entered = true;
  ccol_cond_var_broadcast(g_start_resolve_race_hook.cv);
  while (!g_start_resolve_race_hook.go)
    ccol_cond_var_wait(g_start_resolve_race_hook.cv,
                       g_start_resolve_race_hook.mutex);
  ccol_mutex_unlock(g_start_resolve_race_hook.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * _chttpsvr_stop_internal() blocks at one fixed point. That point is right
 * after the call enters CHTTPSVR_LC_STOPPING and unlocks raw->mutex. It is
 * strictly before the blocking ccol_event_loop_remove() and close() calls
 * for the OLD listener registration. The hook signals that it entered, and
 * it then waits for an explicit release.
 *
 * This lets a test land a concurrent chttpsvr_start() call inside the exact
 * window that CHTTPSVR_LC_STOPPING closes, every time. See the comment of
 * chttpsvr_lifecycle_t on struct chttpserver. Without the hook, a test
 * depends on real, unbounded timing to hit a race window that is a handful
 * of instructions wide.
 *
 * The hook fires once for each arm call. The guard keeps all of it out of a
 * production build. It has exactly the shape of g_start_race_hook and
 * g_start_resolve_race_hook. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_stop_race_hook = {0};

static void _stop_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_stop_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_stop_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_stop_race_hook_for_tests(void) {
  ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  ccol_mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.armed = true;
  g_stop_race_hook.entered = false;
  g_stop_race_hook.go = false;
  ccol_mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_wait_stop_race_hook_entered_for_tests(void) {
  ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  ccol_mutex_lock(g_stop_race_hook.mutex);
  while (!g_stop_race_hook.entered)
    ccol_cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  ccol_mutex_unlock(g_stop_race_hook.mutex);
}

void _chttpsvr_release_stop_race_hook_for_tests(void) {
  ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  ccol_mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.go = true;
  ccol_cond_var_broadcast(g_stop_race_hook.cv);
  ccol_mutex_unlock(g_stop_race_hook.mutex);
}

static void _stop_race_hook_wait_if_armed(void) {
  ccol_call_once(g_stop_race_hook.once, _stop_race_hook_init_globals);
  ccol_mutex_lock(g_stop_race_hook.mutex);
  if (!g_stop_race_hook.armed) {
    ccol_mutex_unlock(g_stop_race_hook.mutex);
    return;
  }
  g_stop_race_hook.armed = false; /* one-shot */
  g_stop_race_hook.entered = true;
  ccol_cond_var_broadcast(g_stop_race_hook.cv);
  while (!g_stop_race_hook.go)
    ccol_cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  ccol_mutex_unlock(g_stop_race_hook.mutex);
}

/* This is white-box test instrumentation and nothing else. It is a plain
 * signal that says "we reached this point", and it never blocks. It serves
 * the CHTTPSVR_LC_STOPPING wait branch of chttpsvr_start(); see the comment
 * of that switch case. Its shape differs from g_stop_race_hook and
 * g_start_race_hook above on purpose. Those two also park the caller until
 * an explicit release.
 *
 * A park hook here is not safe. Every park-style hook in this file fires
 * only after its own caller unlocks whatever mutex the OTHER side of the
 * race may need next. This exact call site still holds raw->mutex when it
 * reaches this point. raw->mutex is precisely what _chttpsvr_stop_internal()
 * must acquire to leave CHTTPSVR_LC_STOPPING. A test may forget to release
 * such a park promptly, or may not be able to, because the choreography
 * across the threads is harder here. That wedges BOTH sides of the very race
 * that this hook exists to test, and not one call alone.
 *
 * The hook fires a one-shot signal instead, and never blocks. It sets a
 * flag, it broadcasts, and it goes straight on into the real
 * ccol_cond_var_wait loop below. It therefore adds no new blocking point of
 * its own. A test can still confirm every time that chttpsvr_start() really
 * reached its own real wait, and is about to enter it. That closes a gap
 * which a fixed sleep alone cannot close. A 150ms settling sleep can elapse
 * because the OS never scheduled the other thread, and not because that
 * thread is really blocked. The hook touches none of the established locking
 * discipline of this function. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
} g_start_stopping_wait_signal = {0};

static void _start_stopping_wait_signal_init_globals(void) {
  if (ccol_mutex_init(g_start_stopping_wait_signal.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_start_stopping_wait_signal.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_stopping_wait_signal_for_tests(void) {
  ccol_call_once(g_start_stopping_wait_signal.once,
                 _start_stopping_wait_signal_init_globals);
  ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  g_start_stopping_wait_signal.armed = true;
  g_start_stopping_wait_signal.entered = false;
  ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

void _chttpsvr_wait_start_stopping_wait_signal_entered_for_tests(void) {
  ccol_call_once(g_start_stopping_wait_signal.once,
                 _start_stopping_wait_signal_init_globals);
  ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  while (!g_start_stopping_wait_signal.entered)
    ccol_cond_var_wait(g_start_stopping_wait_signal.cv,
                       g_start_stopping_wait_signal.mutex);
  ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

static void _start_stopping_wait_signal_fire_if_armed(void) {
  ccol_call_once(g_start_stopping_wait_signal.once,
                 _start_stopping_wait_signal_init_globals);
  ccol_mutex_lock(g_start_stopping_wait_signal.mutex);
  if (g_start_stopping_wait_signal.armed) {
    g_start_stopping_wait_signal.armed = false; /* one-shot */
    g_start_stopping_wait_signal.entered = true;
    ccol_cond_var_broadcast(g_start_stopping_wait_signal.cv);
  }
  ccol_mutex_unlock(g_start_stopping_wait_signal.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * _engine_force_stop_quiesce_all() blocks at one fixed point. That point is
 * right after the call pins the server that it read out of
 * servers_bundler.servers[0], and unlocks servers_bundler.mutex. The pin is
 * servers_bundler_pins; see the comment of that field. The point is strictly
 * before the call into _quiesce_server_once(). The hook signals that it
 * entered, and it then waits for an explicit release.
 *
 * This lets a test land a concurrent chttpsvr_destroy() call on that exact
 * server inside the window that servers_bundler_pins closes, every time. In
 * that window the reaper thread holds a bare struct chttpserver*, and
 * nothing but this pin stops another thread from freeing it. Without the
 * hook, a test depends on real, unbounded timing to hit a race window that
 * is a handful of instructions wide.
 *
 * The hook fires once for each arm call. It has exactly the shape of
 * g_stop_race_hook. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_reaper_race_hook = {0};

static void _reaper_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_reaper_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_reaper_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_reaper_race_hook_for_tests(void) {
  ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  ccol_mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.armed = true;
  g_reaper_race_hook.entered = false;
  g_reaper_race_hook.go = false;
  ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_wait_reaper_race_hook_entered_for_tests(void) {
  ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  ccol_mutex_lock(g_reaper_race_hook.mutex);
  while (!g_reaper_race_hook.entered)
    ccol_cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

void _chttpsvr_release_reaper_race_hook_for_tests(void) {
  ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  ccol_mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.go = true;
  ccol_cond_var_broadcast(g_reaper_race_hook.cv);
  ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

static void _reaper_race_hook_wait_if_armed(void) {
  ccol_call_once(g_reaper_race_hook.once, _reaper_race_hook_init_globals);
  ccol_mutex_lock(g_reaper_race_hook.mutex);
  if (!g_reaper_race_hook.armed) {
    ccol_mutex_unlock(g_reaper_race_hook.mutex);
    return;
  }
  g_reaper_race_hook.armed = false; /* one-shot */
  g_reaper_race_hook.entered = true;
  ccol_cond_var_broadcast(g_reaper_race_hook.cv);
  while (!g_reaper_race_hook.go)
    ccol_cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  ccol_mutex_unlock(g_reaper_race_hook.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * _quiesce_server_once() blocks at one fixed point. That point is right
 * after the call claims the winning side. At that moment srv->quiesce_state
 * is CHTTPSVR_QS_QUIESCING, the pending_resolve_count wait is already
 * satisfied, and srv->mutex is unlocked. The point is strictly before any of
 * the real teardown work, which is _chttpsvr_stop_internal,
 * _servers_unregister, _drain_and_close_all_connections, _engine_release and
 * _destroy_detached_pools.
 *
 * This lets a test fork() a process every time while a thread parks exactly
 * where a real, unbounded interleaving can also park one. The server then
 * has quiesce_state == CHTTPSVR_QS_QUIESCING, which is the exact state that
 * the child-side fixup of _chttpsvr_atfork_release_impl handles; see the doc
 * comment of that function. Without the hook, a test depends on real,
 * unbounded timing to hit a race window that can be arbitrarily narrow.
 *
 * The hook fires once for each arm call. It has exactly the shape of
 * g_reaper_race_hook. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_quiesce_teardown_race_hook = {0};

static void _quiesce_teardown_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_quiesce_teardown_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_quiesce_teardown_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_quiesce_teardown_race_hook_for_tests(void) {
  ccol_call_once(g_quiesce_teardown_race_hook.once,
                 _quiesce_teardown_race_hook_init_globals);
  ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.armed = true;
  g_quiesce_teardown_race_hook.entered = false;
  g_quiesce_teardown_race_hook.go = false;
  ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_wait_quiesce_teardown_race_hook_entered_for_tests(void) {
  ccol_call_once(g_quiesce_teardown_race_hook.once,
                 _quiesce_teardown_race_hook_init_globals);
  ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  while (!g_quiesce_teardown_race_hook.entered)
    ccol_cond_var_wait(g_quiesce_teardown_race_hook.cv,
                       g_quiesce_teardown_race_hook.mutex);
  ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void _chttpsvr_release_quiesce_teardown_race_hook_for_tests(void) {
  ccol_call_once(g_quiesce_teardown_race_hook.once,
                 _quiesce_teardown_race_hook_init_globals);
  ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.go = true;
  ccol_cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

static void _quiesce_teardown_race_hook_wait_if_armed(void) {
  ccol_call_once(g_quiesce_teardown_race_hook.once,
                 _quiesce_teardown_race_hook_init_globals);
  ccol_mutex_lock(g_quiesce_teardown_race_hook.mutex);
  if (!g_quiesce_teardown_race_hook.armed) {
    ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
    return;
  }
  g_quiesce_teardown_race_hook.armed = false; /* one-shot */
  g_quiesce_teardown_race_hook.entered = true;
  ccol_cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  while (!g_quiesce_teardown_race_hook.go)
    ccol_cond_var_wait(g_quiesce_teardown_race_hook.cv,
                       g_quiesce_teardown_race_hook.mutex);
  ccol_mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

/* This is a forward declaration. The definition comes later in this file,
 * where _wait_in_flight_bounded and its neighbours use it. The bounded wait
 * in _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests
 * below needs it here too. */
static void _timespec_add_ms(struct timespec *ts, unsigned ms);

/* This is a white-box test hook and nothing else. When a test arms it, it
 * pauses the CHTTPSVR_QS_QUIESCING backoff branch of chttpsvr_start(). The
 * pause comes right after that branch does two things: it registers itself
 * in quiesce_waiters, and it releases its resolve pin. The pause is strictly
 * before the branch takes raw->mutex again for the first time after those
 * two steps.
 *
 * A use-after-free lives in that window if quiesce_waiters++ happens AFTER
 * the pin release instead of before it. A pin released first can unblock the
 * pending_resolve_count wait of a concurrent _quiesce_server_once pass. That
 * pass then runs to its end and frees raw, before this call gets back to
 * raw->mutex to register itself as a protected waiter.
 *
 * The hook lets a test pause a real chttpsvr_start() call exactly there. The
 * test can then drive a real _quiesce_server_once pass, and a real
 * concurrent chttpsvr_destroy(), to completion around it. That proves
 * directly that raw survives either way. Without the hook, a test depends on
 * unbounded, real timing to land two threads in a race window a few
 * instructions wide.
 *
 * The hook fires once for each arm call. It has exactly the shape of
 * g_quiesce_teardown_race_hook. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool entered;
  bool go;
} g_start_quiescing_unpinned_race_hook = {0};

static void _start_quiescing_unpinned_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_start_quiescing_unpinned_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  /* The clock is CLOCK_MONOTONIC. This matches the precedent of
   * ccol_create_chttpsvr_mp, which carries a comment of its own.
   * _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests
   * below computes its own bounded-wait deadline with
   * clock_gettime(CLOCK_MONOTONIC, ...). The default clock of
   * ccol_cond_var_init is CLOCK_REALTIME, and a comparison against that
   * deadline gives the wrong answer. */
  ccol_cond_var_attr_t cv_attr;
  int cv_rc;
  if (ccol_cond_var_attr_init(cv_attr) == 0) {
    ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc =
        ccol_cond_var_init_ca(g_start_quiescing_unpinned_race_hook.cv, cv_attr);
    ccol_cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = ccol_cond_var_init(g_start_quiescing_unpinned_race_hook.cv);
  }
  if (cv_rc != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

void _chttpsvr_arm_start_quiescing_unpinned_race_hook_for_tests(void) {
  ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                 _start_quiescing_unpinned_race_hook_init_globals);
  ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.armed = true;
  g_start_quiescing_unpinned_race_hook.entered = false;
  g_start_quiescing_unpinned_race_hook.go = false;
  ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* Every OTHER _chttpsvr_wait_*_race_hook_entered_for_tests function in this
 * file reaches its hook every time, and the earlier steps of the calling
 * test prove that. THIS one does not. The only test that uses it is
 * start_racing_engine_stop_and_destroy_does_not_free_raw_too_early, in
 * tests_engine_stop.c. That test also depends on a concurrent reaper thread
 * setting CHTTPSVR_QS_QUIESCING before the paused chttpsvr_start() call
 * checks quiesce_state again. It enforces that order with a fixed
 * nanosleep(150ms) alone, and not with a real synchronization primitive.
 * Load in the environment can stretch that window past 150ms. valgrind,
 * ThreadSanitizer and a busy CI runner all do that. Nothing then enters this
 * hook at all.
 *
 * The wait here is bounded to 10s, which is generous even under heavy
 * instrumentation. A missed race therefore becomes a clean test failure that
 * the harness catches. Without the bound, this call hangs. It runs on the
 * main test thread, and not on a background one. It therefore hangs the
 * whole binary, and no bounded-join fixture can rescue it. The function
 * returns false on a timeout. */
bool _chttpsvr_wait_start_quiescing_unpinned_race_hook_entered_for_tests(void) {
  ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                 _start_quiescing_unpinned_race_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  _timespec_add_ms(&deadline, 10000);
  ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  bool timed_out = false;
  while (!g_start_quiescing_unpinned_race_hook.entered && !timed_out) {
    timed_out =
        ccol_cond_var_timedwait(g_start_quiescing_unpinned_race_hook.cv,
                                g_start_quiescing_unpinned_race_hook.mutex,
                                deadline) == ETIMEDOUT;
  }
  bool entered = g_start_quiescing_unpinned_race_hook.entered;
  ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
  return entered;
}

void _chttpsvr_release_start_quiescing_unpinned_race_hook_for_tests(void) {
  ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                 _start_quiescing_unpinned_race_hook_init_globals);
  ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.go = true;
  ccol_cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

static void _start_quiescing_unpinned_race_hook_wait_if_armed(void) {
  ccol_call_once(g_start_quiescing_unpinned_race_hook.once,
                 _start_quiescing_unpinned_race_hook_init_globals);
  ccol_mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  if (!g_start_quiescing_unpinned_race_hook.armed) {
    ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
    return;
  }
  g_start_quiescing_unpinned_race_hook.armed = false; /* one-shot */
  g_start_quiescing_unpinned_race_hook.entered = true;
  ccol_cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  while (!g_start_quiescing_unpinned_race_hook.go)
    ccol_cond_var_wait(g_start_quiescing_unpinned_race_hook.cv,
                       g_start_quiescing_unpinned_race_hook.mutex);
  ccol_mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

/* This is a white-box test hook and nothing else. When a test arms it,
 * _listener_on_readable() blocks at one of two fixed points, and the arm
 * call chooses which. The default point is right after the call pins srv
 * with listener_dispatch_pins; see the comment of that field. It is strictly
 * before the call into _listener_on_readable_impl(). The entry point is the
 * very first statement of the callback, before the pin, where a concurrent
 * chttpsvr_stop and chttpsvr_destroy see no pin at all. The hook signals
 * that it entered, and it then waits for an explicit release.
 *
 * This lets a test land a concurrent chttpsvr_destroy() call every time
 * while a listener dispatch is really in flight and holds this pin. The test
 * can then assert that the destroy blocks until it releases the hook.
 * Without the hook, a test must race a live accept4() backoff sleep on real,
 * unbounded timing. That window is real, but it is not reproducible on
 * demand.
 *
 * The hook has exactly the shape of g_reaper_race_hook. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  bool armed;
  bool at_entry;
  bool entered;
  bool go;
} g_listener_dispatch_race_hook = {0};

static void _listener_dispatch_race_hook_init_globals(void) {
  if (ccol_mutex_init(g_listener_dispatch_race_hook.mutex) != 0)
    ccol_fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol_cond_var_init(g_listener_dispatch_race_hook.cv) != 0)
    ccol_fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

static void _listener_dispatch_race_hook_arm(bool at_entry) {
  ccol_call_once(g_listener_dispatch_race_hook.once,
                 _listener_dispatch_race_hook_init_globals);
  ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.armed = true;
  g_listener_dispatch_race_hook.at_entry = at_entry;
  g_listener_dispatch_race_hook.entered = false;
  g_listener_dispatch_race_hook.go = false;
  ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_arm_listener_dispatch_race_hook_for_tests(void) {
  _listener_dispatch_race_hook_arm(false);
}

void _chttpsvr_arm_listener_dispatch_entry_race_hook_for_tests(void) {
  _listener_dispatch_race_hook_arm(true);
}

void _chttpsvr_wait_listener_dispatch_race_hook_entered_for_tests(void) {
  ccol_call_once(g_listener_dispatch_race_hook.once,
                 _listener_dispatch_race_hook_init_globals);
  ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  while (!g_listener_dispatch_race_hook.entered)
    ccol_cond_var_wait(g_listener_dispatch_race_hook.cv,
                       g_listener_dispatch_race_hook.mutex);
  ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void _chttpsvr_release_listener_dispatch_race_hook_for_tests(void) {
  ccol_call_once(g_listener_dispatch_race_hook.once,
                 _listener_dispatch_race_hook_init_globals);
  ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.go = true;
  ccol_cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

static void _listener_dispatch_race_hook_wait_if_armed(bool at_entry) {
  ccol_call_once(g_listener_dispatch_race_hook.once,
                 _listener_dispatch_race_hook_init_globals);
  ccol_mutex_lock(g_listener_dispatch_race_hook.mutex);
  if (!g_listener_dispatch_race_hook.armed ||
      g_listener_dispatch_race_hook.at_entry != at_entry) {
    ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
    return;
  }
  g_listener_dispatch_race_hook.armed = false; /* one-shot */
  g_listener_dispatch_race_hook.entered = true;
  ccol_cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  while (!g_listener_dispatch_race_hook.go)
    ccol_cond_var_wait(g_listener_dispatch_race_hook.cv,
                       g_listener_dispatch_race_hook.mutex);
  ccol_mutex_unlock(g_listener_dispatch_race_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* This is the task function of reject_pool. It runs _conn_reject_and_close
 * on a dedicated thread of reject_pool, and not on the reactor thread. See
 * the comment of that field on struct chttpserver, and see the comment of
 * _conn_start_diverted, for the reason. It then releases the
 * in_flight_requests slot of this request, in the same way as the decrement
 * of _task_worker. It does so only once the close really completes, and not
 * once this task merely enters the queue. See the comment of
 * _release_in_flight for why that order matters. The code captures srv
 * before _conn_reject_and_close, because that function frees conn
 * internally. */
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

/* Feeds n fresh bytes into conn->parser and resolves whatever outcome comes
 * out of that. It does this exactly as the read loop of _conn_pump does. It
 * sits in its own function so that the keep-alive tail of _task_worker can
 * drive the same header-parse machinery. See the comment of that tail about
 * chttp1_stream_take_leftover. That tail drives it synchronously, on the
 * worker thread, against the bytes of a pipelined next request. The library
 * already pulled those bytes off the wire, so they can never arrive as a
 * fresh socket-readable event for the reactor to answer.
 *
 * buf and n need not come from a live socket read at all. The caller owns
 * the lifetime of buf for the duration of this call only.
 * chttp1_parser_execute keeps no pointer into it past its return. The
 * _conn_start_diverted function below copies out whatever leftover it gets.
 * It does this exactly as it already does for a real read on the reactor
 * thread.
 *
 * @return true when the caller should keep reading. That means the parser
 *         needs more bytes to complete the request line or the header block,
 *         which is CHTTP1_OK. It returns false when this call already
 *         resolved the fate of conn for now. In that case the library either
 *         diverted conn to a worker, rejected it and routed it to
 *         reject_pool, or closed it outright. The caller must not touch conn
 *         again. */
static bool _conn_feed_bytes(chttpsvr_conn_t *conn, const char *buf, size_t n) {
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  /* This reuses the instant that the code just read for last_activity. It
   * does not take a second one. The call is idempotent within one request.
   * The budget therefore runs from the FIRST byte of this request, whatever
   * count of further chunks follows. A refresh for each chunk is exactly
   * what makes last_activity a gap timer instead of a total one. */
  _conn_header_phase_arm(conn, &conn->last_activity);
  chttp1_errno_t r = chttp1_parser_execute(&conn->parser, buf, n);

  if (r == CHTTP1_OK) return true; /* need more header bytes; keep reading */

  if (r == CHTTP1_HEADERS_ONLY || r == CHTTP1_PAUSED) {
    size_t consumed = chttp1_parser_consumed(&conn->parser);
    const char *leftover = buf + consumed;
    size_t leftover_len = n - consumed;

    /* _task_worker sends the Expect: 100-continue interim write, when
     * conn->expects_continue is true. It sends it on a worker thread, and
     * not here, once the library really diverted this request. See the
     * comment of that function for the reason. */
    _conn_start_diverted(conn, leftover, leftover_len);
    return false;
  }

  /* A CHTTP1_USER means an unmatched or rejected route. The
   * _on_headers_complete of this library decided it, and it already set
   * conn->req_rejected to a specific status. The library identified the
   * route itself, so it sends a graceful error response. It never reads the
   * body, if there is one. It then closes the connection instead of a keep
   * alive. Without that close, the library reads the body that still arrives
   * from the client as a pipelined request.
   *
   * A CHTTP1_ERROR means a problem at the level of syntax. The parser itself
   * rejected the request before any routing ran. One example is a malformed
   * request line. Another is a negative Content-Length. A chunked entry that
   * is not last in a Transfer-Encoding list is a third, a header that is too
   * long is a fourth, and a request that breaks the Host rule of RFC 7230
   * SS5.4 is a fifth.
   *
   * Every one of those is a CLIENT error, and RFC 7231 SS6.5.1 names the
   * status for it: 400 Bad Request, for a request that the server "will not
   * process due to something that is perceived to be a client error (e.g.,
   * malformed request syntax ...)". The library answers with that status,
   * through the same bounded reject path that its 404, 405, 413, 500, 501
   * and 503 answers take. It then closes the connection, because the byte
   * stream is no longer framed and nothing after this point can be read as a
   * request.
   *
   * A bare close with no response is the wrong answer here. It leaves the
   * peer to guess between a network fault, a crash and a rejection, and it
   * leaves an operator with no status code to count.
   *
   * The response goes out bare. _reject_status_runs_middleware() does not
   * list 400, so no application middleware runs for a request whose syntax
   * never parsed. There is no decoded path and no matched route to hand one
   * in any case. */
  if (!conn->req_rejected) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
  }
  /* The library routes this through reject_pool, or through the bounded
   * synchronous fallback of that pool. That is exactly what the pool-full
   * 503 case does. A client that reads slowly, and that the server must tell
   * about a 400, 404, 405 or 500, therefore cannot stall the sole reactor
   * thread. See the comments of _conn_dispatch_reject and
   * _conn_reject_via_pool. */
  _conn_dispatch_reject(conn);
  return false;
}

/* Drives the reactor-owned part of one connection. That part is the TLS
 * handshake, when the connection has one, and then the header parse. The
 * parse runs until the library either rejects the request or diverts it to a
 * worker. For a rejection, the library sends the response synchronously and
 * closes the connection. */
static void _conn_pump(chttpsvr_conn_t *conn) {
  /* A parked connection waits for its socket in the middle of a request, and
   * not for the headers of the next one. A worker continues it. */
  if (__builtin_expect(conn->park != _CONN_PARK_NONE, 0)) {
    if (conn->park == _CONN_PARK_LINGER)
      _conn_linger_step(conn);
    else
      _conn_resume_parked(conn);
    return;
  }
  if (conn->state == CONN_ST_TLS_HANDSHAKE) {
    /* The handshake is part of the reactor-owned phase, and it shares the
     * budget of that phase. A peer can open a TLS connection and then drip
     * handshake bytes. Such a peer never reaches the header block at all. A
     * ceiling that arms only once header bytes arrive would therefore never
     * fire for it. The code reads the clock only on the step that arms the
     * ceiling, which is the first step of this request. It does not read it
     * on every step. */
    if (!conn->header_phase_active) {
      struct timespec hs_now;
      clock_gettime(CLOCK_MONOTONIC, &hs_now);
      _conn_header_phase_arm(conn, &hs_now);
    }
    ctls_handshake_result_t r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_ERROR) {
      _SRV_ENGINE_LOG(ccol_log_warn, "TLS handshake failed fd=%d", conn->fd);
      _conn_close(conn);
      return;
    }
    if (r == CTLS_HANDSHAKE_WANT_READ || r == CTLS_HANDSHAKE_WANT_WRITE) {
      ccol_select_dir want = (r == CTLS_HANDSHAKE_WANT_WRITE)
                                 ? ccol_select_write
                                 : ccol_select_read;
      if (conn->reg) {
        /* A modify should not fail in practice. Nothing else touches the reg
         * of this connection while the reactor owns it alone, and
         * _idle_list_try_claim guarantees that single owner. The code still
         * treats a failure as fatal for this connection, and never ignores
         * it. Without that, the real epoll interest of conn->reg falls out
         * of step with `want`. The handshake then strands, because no
         * further readiness event arrives in the direction that it still
         * needs. */
        if (ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg,
                                   want) != ccol_success) {
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
        /* See the field comment of conn->lifetime_refs. The on_removed that
         * this new registration eventually fires is a fresh +1 contribution
         * against that counter. The code counts it the instant the
         * registration becomes live, so nothing can miss it.
         * srv->lifetime_refs gets the same treatment, for the same reason,
         * at the server level; see its own field comment. */
        atomic_fetch_add(&conn->lifetime_refs, 1);
        atomic_fetch_add(&conn->srv->lifetime_refs, 1);
      }
      /* The code adds conn back to the idle list before it returns. This
       * thread is done with conn for now, and it waits for the readiness of
       * the next handshake step. A future dispatch must be able to claim
       * conn back out with _idle_list_try_claim. Without this add, a
       * connection in the middle of a handshake is never in the idle list
       * between steps. The sweep of the idle timeout cannot see it, and
       * neither can the idle-connection cleanup of a server destroy. Neither
       * one can therefore close it. */
      _idle_list_add(conn);
      return;
    }
    /* CTLS_HANDSHAKE_DONE */
    conn->state = CONN_ST_READING_HEADERS;
    if (conn->reg) {
      if (ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg,
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
      /* A signal that arrives on this reactor thread in the middle of a read
       * can interrupt the read() or ctls_conn_read() call above with EINTR.
       * The connection itself is healthy at that moment. One example is the
       * SIGTERM handler of the application. Such a handler calls
       * chttpsvr_engine_stop() exactly as the header docs of this module
       * recommend, and the application installed it without SA_RESTART.
       * ctls_conn_read() documents itself as a non-blocking read(2), errno
       * included, so this applies to both branches above. The code retries,
       * exactly as the accept() loop of _listener_on_readable already
       * retries an EINTR. It does not tear an unrelated, healthy connection
       * down over one spurious interruption by a signal. */
      if (errno == EINTR) continue;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        /* For a plaintext connection, an EWOULDBLOCK from a raw read(2)
         * always means "wait until readable". That matches the
         * read-direction registration of ccol_selectable_from_fd below. For
         * a TLS connection it does NOT mean that. OpenSSL can need to write
         * before this exact ctls_conn_read() call can make progress. Two
         * examples are a flush of a session ticket that it deferred after
         * the handshake, and a renegotiation under TLS 1.2. See the doc
         * comment of ctls_conn_wants_write.
         *
         * A registration for read interest alone in that case leaves this
         * connection with no further readiness event in the direction that
         * it really needs. The connection then stalls until the configured
         * read or idle timeout fires, instead of until the next real byte.
         * Under the documented setting of 0, which means "wait forever", it
         * stalls with no end. */
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
          /* See the field comment of conn->lifetime_refs. See also the field
           * comment of srv->lifetime_refs, which gets the same treatment at
           * the server level. */
          atomic_fetch_add(&conn->lifetime_refs, 1);
          atomic_fetch_add(&conn->srv->lifetime_refs, 1);
        } else {
          /* The connection already has a registration in the read direction.
           * It came from an earlier pass of this same loop, or from
           * CTLS_HANDSHAKE_DONE above. The code computes the interest fresh
           * on every EWOULDBLOCK. It never assumes that the old interest is
           * still correct. The handshake-step branch above does the same,
           * for the same reason. This is a harmless no-op when want did not
           * change. */
          if (ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg,
                                     want) != ccol_success) {
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
    /* A CHTTP1_OK means the loop must read more header bytes. */
  }
}

/* Every entry point that the reactor dispatches into a live connection must
 * claim that connection out of the idle list first. A live connection is one
 * that already has a registration. See the doc comment of
 * _idle_list_try_claim for the use-after-free that the claim prevents. In
 * it, a concurrent closer frees the same connection, and its TLS state with
 * it, while a dispatch uses it. That closer comes from the idle timeout or
 * from a destroy.
 *
 * A failed claim is not an error. It means that a closer already holds
 * exclusive ownership. It can also mean, harmlessly, that the
 * _idle_list_add call of this connection for the registration that just
 * fired did not run yet. See that same doc comment for why level-triggered
 * epoll makes that case heal itself. In both cases the correct action is the
 * same: touch nothing and return. */
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

/* A worker thread never waits on the socket of a client for a buffered route.
 * Four mechanisms work together for that.
 *
 * Parking. A body read or a response write that meets a socket with nothing
 * to give, or no room to take, gives the connection up. The connection goes
 * into the idle list with park set, its registration armed for the direction
 * that it needs, and no thread. The reactor claims it on readiness and hands
 * it to any free worker, which continues where the last one stopped. The
 * claim is the one of _idle_list_try_claim, so a closer and a dispatch never
 * both act on it.
 *
 * The rate floor. The sweep closes a parked connection whose average rate
 * since the start of its body read, or of its response write, is below
 * min_transfer_rate_bps once min_transfer_rate_grace_ms has passed. A
 * streaming handler meets the same floor inside chttpsvr_req_read.
 *
 * Body memory. The bodies that buffered routes hold, from the first byte read
 * until the handler returned, hold at most max_partial_body_memory together;
 * a request whose body does not fit waits with no thread in the memory-wait
 * list. See _mem_admit and _conn_drop_body.
 *
 * The streaming pool. Streaming handlers run on their own bounded pool, fed
 * by a bounded queue whose entries hold no thread. See _stream_token_task.
 *
 * Every wait here is visible to the sweep, which enforces its limits, and to
 * the teardown of the server, which closes or answers it. */

/* The largest receive low-water mark that a parked body asks for. It bounds
 * how much the kernel buffers before it wakes the reactor. */
#define _CHTTPSVR_RCVLOWAT_MAX 16384

#ifdef RUNNING_UNIT_TESTS
/* White-box instrumentation for the tests. The clock offset moves the clock
 * of every slow-client limit, and of nothing else, so that a test can expire
 * a rate floor, a memory wait or a queue deadline without a sleep. The
 * counters cover the whole process, as the other counters of this file do. */
static _Atomic long long g_slow_clock_offset_ms_for_tests = 0;
static _Atomic size_t g_resume_dispatch_count_for_tests = 0;
static _Atomic size_t g_park_count_for_tests = 0;
/* The same count for each kind of park, indexed by _CONN_PARK_*. */
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

/* A hook that holds a streaming-pool task at one of two points of its claim:
 * just before it takes the next queued request, or just after. A test that
 * runs the sweep while the task is held proves that exactly one of the two
 * answers the request. */
static struct {
  ccol_mutex_t mutex;
  ccol_cond_var_t cv;
  ccol_once_flag_t once;
  int armed_at; /* 0 = not armed, 1 = before the claim, 2 = after it */
  bool entered;
  bool released;
} g_stream_claim_hook = {0};
static void _stream_claim_hook_init_globals(void) {
  if (ccol_mutex_init(g_stream_claim_hook.mutex) != 0 ||
      ccol_cond_var_init(g_stream_claim_hook.cv) != 0)
    ccol_fatal_err("chttpsvr: stream claim hook init failed");
}
void _chttpsvr_arm_stream_claim_hook_for_tests(int where) {
  ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  ccol_mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.armed_at = where;
  g_stream_claim_hook.entered = false;
  g_stream_claim_hook.released = false;
  ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
bool _chttpsvr_wait_stream_claim_hook_entered_for_tests(unsigned timeout_ms) {
  ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  _timespec_add_ms(&deadline, timeout_ms);
  ccol_mutex_lock(g_stream_claim_hook.mutex);
  while (!g_stream_claim_hook.entered)
    if (ccol_cond_var_timedwait(g_stream_claim_hook.cv,
                                g_stream_claim_hook.mutex,
                                deadline) == ETIMEDOUT)
      break;
  bool entered = g_stream_claim_hook.entered;
  ccol_mutex_unlock(g_stream_claim_hook.mutex);
  return entered;
}
void _chttpsvr_release_stream_claim_hook_for_tests(void) {
  ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  ccol_mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.released = true;
  ccol_cond_var_broadcast(g_stream_claim_hook.cv);
  ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
static void _stream_claim_hook_wait_if_armed(int where) {
  ccol_call_once(g_stream_claim_hook.once, _stream_claim_hook_init_globals);
  ccol_mutex_lock(g_stream_claim_hook.mutex);
  if (g_stream_claim_hook.armed_at == where) {
    g_stream_claim_hook.armed_at = 0;
    g_stream_claim_hook.entered = true;
    ccol_cond_var_broadcast(g_stream_claim_hook.cv);
    while (!g_stream_claim_hook.released)
      ccol_cond_var_wait(g_stream_claim_hook.cv, g_stream_claim_hook.mutex);
  }
  ccol_mutex_unlock(g_stream_claim_hook.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

/* The clock of every slow-client limit. It is CLOCK_MONOTONIC, plus the
 * offset of the tests in a test build. */
static void _slow_clock(struct timespec *ts) {
  clock_gettime(CLOCK_MONOTONIC, ts);
#ifdef RUNNING_UNIT_TESTS
  long long off = atomic_load(&g_slow_clock_offset_ms_for_tests);
  if (off > 0) _timespec_add_ms(ts, (unsigned)off);
#endif
}

/* Milliseconds from then to now, as a signed value; negative when another
 * thread stamped then after this thread read now. */
static long long _ms_since(struct timespec now, struct timespec then) {
  return (long long)(now.tv_sec - then.tv_sec) * 1000LL +
         (long long)(now.tv_nsec - then.tv_nsec) / 1000000LL;
}

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

/* Sets SO_RCVLOWAT on a connection that parks with its body incomplete, so
 * that the reactor wakes once a useful amount of the body is queued, and not
 * for every segment that a slow client sends. The mark is the number of body
 * bytes that the framing still guarantees, capped at _CHTTPSVR_RCVLOWAT_MAX;
 * see chttp1_body_bytes_still_expected. A client that follows its own
 * framing sends at least that many bytes before it can expect an answer, so
 * the mark is always reached and the tail of the body always wakes the
 * reader. For a chunked body the guarantee is what the current chunk still
 * needs, and nothing on a chunk-size line or in the trailers, where the next
 * byte can be the last one; the mark is then 1. An end of stream wakes the
 * reader whatever the mark.
 *
 * For TLS the plaintext count is safe too, because a record never carries
 * fewer bytes on the wire than the plaintext inside it. That holds only when
 * OpenSSL holds no part of a record already: a partial record that it read
 * ahead needs fewer bytes from the socket than the plaintext it completes.
 * The mark is therefore 1 whenever ctls_conn_has_pending_input() is true.
 *
 * The sweep cannot see bytes that sit in the socket below the mark, so it
 * asks the kernel for them (FIONREAD) before it judges such a connection
 * slow; see _parked_expired. */
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

/* Puts the mark back to 1 once the connection leaves its body, so that the
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

/* The time of the current phase that counts against the client: the time
 * since the phase started, less the time that the server held it back. */
static long long _rate_elapsed_ms(const chttpsvr_conn_t *conn,
                                  struct timespec now) {
  return _ms_since(now, conn->rate_start) - conn->rate_paused_ms;
}

/* The elapsed time, in ms, up to which the phase stays above the floor with
 * the bytes that it moved: the grace period, or the time that those bytes
 * buy at the floor rate, whichever is longer. */
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

/* The rate floor inside the blocking read of a streaming handler. It starts
 * the phase on the first call. It returns false when the body is already
 * below the floor, and otherwise shrinks *timeout_ms_inout to the instant
 * when the floor would be crossed with no further byte. A 0 there means "no
 * per-call bound yet", as in _shrink_timeout_to_deadline. */
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

/* The count of bytes that the send queue of conn still holds, which the
 * kernel has not sent or that the peer has not acknowledged yet (SIOCOUTQ),
 * or SIZE_MAX when the kernel does not report it. */
static size_t _conn_send_queue(const chttpsvr_conn_t *conn) {
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

/* The gap and the rate floor of a parked response. The reactor wakes such a
 * connection only once the socket is writable again, and for TCP that is
 * when about a third of a send buffer that the kernel autotunes to several
 * MB has drained. A reader that takes the response steadily, far above the
 * floor, can need longer than response_write_timeout_ms to drain that much,
 * so the wake-up alone is not the measure of its progress. The measure is
 * the send queue: every byte that leaves it reached the reader. A queue
 * that shrank since the park or the last tick is progress for the gap. For
 * TLS the queue holds records, which is still what reached the reader.
 *
 * The rate floor counts the bytes that reached the reader, too, and not the
 * bytes that the kernel accepted: a response that fits in the send buffer is
 * accepted at once, whatever the reader does. The bytes that reached it are
 * those accepted less those still queued. That holds for TCP. For a
 * unix:// connection the kernel reports the memory that the queued buffers
 * take, which is more than the bytes in them, so there the floor keeps to
 * the bytes accepted and the queue serves the gap alone.
 *
 * The sweep reads the queue of every parked response on each tick while the
 * gap or the floor is on: one ioctl for each such connection, which a sweep
 * that already walks the parked list affords. */
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

/* Says whether a parked connection broke one of its limits. The sweep calls
 * it with srv->idle_mutex held, for a connection in the idle list; no thread
 * owns such a connection, so the sweep may read and update its rate state.
 *
 * A parked body answers to stream_read_timeout_ms for the gap since its last
 * byte, to max_body_read_duration_ms for the whole read, and to the rate
 * floor. A parked response answers to response_write_timeout_ms, to
 * max_response_write_duration_ms and to the rate floor. The two total limits
 * run on CLOCK_MONOTONIC, as the worker set them; the gap and the floor run
 * on _slow_clock. */
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
    /* Bytes below the low-water mark wake nobody, yet they are progress. */
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

/* Records the instant at which conn leaves a wait for the worker pool; see
 * the field comment of claim_mono. */
static void _conn_stamp_claim(chttpsvr_conn_t *conn) {
  clock_gettime(CLOCK_MONOTONIC, &conn->claim_mono);
  _slow_clock(&conn->claim_slow);
}

/* Takes the time between the claim of conn and now, which conn spent in the
 * queue of the worker pool behind other work, off every limit of the phase
 * that it resumes: the rate floor, the gap since its last byte, and the total
 * read or write duration. A client whose bytes already arrived is then never
 * charged for how long the server took to get to them. The memory wait of a
 * body does the same for its own wait in _mem_dispatch. */
static void _conn_uncharge_queue_wait(chttpsvr_conn_t *conn,
                                      struct timespec now,
                                      struct timespec now_slow) {
  long long waited = _ms_since(now_slow, conn->claim_slow);
  if (waited > 0) {
    unsigned w = waited > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited;
    conn->rate_paused_ms += waited;
    _timespec_add_ms(&conn->last_progress, w);
  }
  long long waited_mono = _ms_since(now, conn->claim_mono);
  if (waited_mono > 0) {
    unsigned w =
        waited_mono > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited_mono;
    if (conn->read_deadline_set) _timespec_add_ms(&conn->read_deadline, w);
    if (conn->write_deadline_set) _timespec_add_ms(&conn->write_deadline, w);
  }
}

/* Body memory. */

/* Says whether need more bytes fit under the limit now. mem_in_use can sit
 * above the limit while a connection runs past it under the progress rule,
 * so the subtraction is guarded. The caller holds srv->wait_mutex. */
static bool _mem_fits_locked(struct chttpserver *srv, size_t need) {
  size_t cap = atomic_load(&srv->mem_cap);
  return srv->mem_in_use <= cap && need <= cap - srv->mem_in_use;
}

#ifdef RUNNING_UNIT_TESTS
/* The highest mem_in_use that any server reached since the last reset, for
 * the test of the bound of the progress rule. */
static _Atomic size_t g_mem_peak_for_tests = 0;
size_t _chttpsvr_mem_peak_for_tests(void) {
  return atomic_load(&g_mem_peak_for_tests);
}
void _chttpsvr_mem_peak_reset_for_tests(void) {
  atomic_store(&g_mem_peak_for_tests, 0);
}
#endif

#ifdef RUNNING_UNIT_TESTS
/* The largest amount by which the body buffer of a request that holds a
 * reservation of body memory ever held more memory than that reservation,
 * counting the old and the new block of a growth together, since the last
 * reset. */
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

/* Charges need bytes to c. A charge above the whole limit is charged as the
 * limit: such a request can only ever run under the progress rule, and a
 * charge of the limit already keeps every other reservation out until it
 * ends. The sum also saturates, so that mem_in_use never wraps, whatever the
 * declared lengths of the bodies (max_body_size 0 lets a Content-Length
 * declare almost SIZE_MAX bytes). */
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

/* Reserves need more bytes of body memory for conn, which its owner holds,
 * when they fit now. A reservation never goes ahead of a request that already
 * waits, so that a stream of small requests cannot starve a large one. A
 * connection that runs past the limit under the progress rule always gets
 * its reservation. */
static bool _mem_try_reserve(struct chttpserver *srv, chttpsvr_conn_t *conn,
                             size_t need) {
  ccol_mutex_lock(srv->wait_mutex);
  bool ok = conn->mem_exempt || (!srv->mem_head && _mem_fits_locked(srv, need));
  if (ok) _mem_grant_locked(srv, conn, need);
  ccol_mutex_unlock(srv->wait_mutex);
  return ok;
}

/* Puts conn, which its owner holds, at the tail of the memory-wait list. This
 * is the last touch of conn by its owner: from the unlock on, an admission on
 * another thread can take it. */
static void _mem_wait_enqueue(struct chttpserver *srv, chttpsvr_conn_t *conn,
                              size_t need, uint8_t kind) {
  _slow_clock(&conn->mem_wait_start);
  conn->park = kind;
  conn->mem_need = need;
  ccol_mutex_lock(srv->wait_mutex);
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
  ccol_mutex_unlock(srv->wait_mutex);
}

/* Unlinks c from the memory-wait list, which claims it. The caller holds
 * srv->wait_mutex. */
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

/* Gives the whole reservation of conn back, and ends its exemption under the
 * progress rule. _conn_drop_body calls it, on the owner of conn, as it frees
 * the body: the handler returned, the request was refused, or the
 * connection closes. It then admits whatever the freed memory allows. */
static __attribute__((noinline, cold)) void _mem_release(
    chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->wait_mutex);
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
  ccol_mutex_unlock(srv->wait_mutex);
  if (waiters) _mem_admit(srv);
}

/* Gives back the part of the reservation of conn above keep bytes, which a
 * chunked body does not need once its buffer grew and freed the old block.
 * The owner of conn calls it. */
static __attribute__((noinline, cold)) void _mem_trim(chttpsvr_conn_t *conn,
                                                      size_t keep) {
  struct chttpserver *srv = conn->srv;
  ccol_mutex_lock(srv->wait_mutex);
  bool freed = false;
  if (conn->mem_charged > keep) {
    srv->mem_in_use -= conn->mem_charged - keep;
    if (keep == 0) srv->mem_holders--;
    conn->mem_charged = keep;
    freed = true;
  }
  bool waiters = freed && srv->mem_head != NULL;
  ccol_mutex_unlock(srv->wait_mutex);
  if (waiters) _mem_admit(srv);
}

/* Makes the reservation of conn, whose body no longer arrives, equal to the
 * memory that its body buffer holds. A chunked body reserves the growth of
 * its buffer before the read that needs it, and a read that brings fewer
 * bytes than it asked for leaves that growth reserved and unused. The
 * reservation then stays exactly what the buffer holds until _task_finish
 * frees the body, once the handler returned. A connection that runs past the
 * limit under the progress rule can hold more than its reservation, and
 * keeps it. */
static inline __attribute__((always_inline)) void _mem_settle(
    chttpsvr_conn_t *conn) {
  if (__builtin_expect(conn->mem_charged > conn->body.cap, 0))
    _mem_trim(conn, conn->body.cap);
}

/* Hands an admitted waiter to the worker pool. A request that waited before
 * its body started goes to _task_worker, exactly as if it had just
 * diverted; a chunked body that waited for more memory goes to _task_resume.
 * The time of the wait is the server's, so neither the rate floor nor the
 * body timers count it. A full pool answers 503, since no response byte went
 * out yet. */
static void _mem_dispatch(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  void (*fn)(void *) = _task_worker;
  if (conn->park == _CONN_PARK_MEM_BODY) {
    struct timespec now;
    _slow_clock(&now);
    long long waited = _ms_since(now, conn->mem_wait_start);
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
  ccol_mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  ctpool pool = srv->worker_pool;
  ccol_mutex_unlock(srv->mutex);
  if (!_srv_submit(srv, _SRV_POOL_WORKER, pool, fn, conn)) {
    conn->park = _CONN_PARK_NONE;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    _conn_reject_via_pool(conn);
  }
}

/* Admits waiters from the head of the memory-wait list, in the order in
 * which they began to wait, while the head fits under the limit.
 *
 * The progress rule. When the head does not fit, and every connection that
 * holds a reservation is itself in this list, nothing that holds memory can
 * make progress, and nothing would ever free memory: the head then goes
 * ahead past the limit, and keeps reserving freely until its body ends. Only
 * one connection at a time may do so.
 *
 * Why nothing deadlocks. Take any moment at which the list is not empty.
 * Either some holder is outside the list, or none is. A holder outside the
 * list is a body that is being read, that is parked on its client, whose
 * request is being refused, or whose handler runs. A body that arrives ends,
 * by completing or by one of the limits of the sweep; a refusal ends within
 * the ceiling of the internal writes; a handler ends when it returns, which
 * is the obligation of the application, exactly as for the thread of the
 * worker pool that runs it. Each of them then frees its body, and the
 * release runs this function again. If no holder is outside the list, the
 * head is admitted at once, past the limit if need be, and it is then a
 * holder outside the list. A waiter that is not at the head moves up each
 * time the head leaves, whether by admission or by the deadline of the
 * sweep, so strict order cannot starve it either.
 *
 * A handler that waits for another request is the one dependency that the
 * server cannot see. When the body of that other request does not fit
 * beside the bodies that such handlers hold, it waits for memory until
 * body_memory_wait_timeout answers it with 503, or, with no deadline, until
 * one of those handlers returns. The progress rule does not let it go ahead:
 * a holder whose handler runs is making progress as far as the server can
 * tell, and an exemption for it would let the bodies of running handlers
 * exceed the limit without any bound.
 *
 * What it costs. Only one connection runs past the limit at a time, from its
 * admission until its body is freed, and it can reserve no more than its own
 * body buffer, plus the old buffer while a growth copies it, so the limit is
 * exceeded by less than twice max_body_size (or twice one declared
 * Content-Length where max_body_size is 0). No other exemption starts while
 * it holds its body, because it is a holder outside the list.
 *
 * The function never holds wait_mutex across a dispatch, and it never runs
 * two dispatch loops at once: a call that finds another call in progress
 * asks that call to look again, and returns. A dispatch whose submit fails
 * closes its connection, and the release of that close lands here as such a
 * request. */
static void _mem_admit(struct chttpserver *srv) {
  ccol_mutex_lock(srv->wait_mutex);
  if (srv->mem_admitting) {
    srv->mem_admit_again = true;
    ccol_mutex_unlock(srv->wait_mutex);
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
    ccol_mutex_unlock(srv->wait_mutex);
    for (size_t i = 0; i < n; i++) _mem_dispatch(batch[i]);
    ccol_mutex_lock(srv->wait_mutex);
  }
  srv->mem_admitting = false;
  ccol_mutex_unlock(srv->wait_mutex);
}

/* Stops _mem_admit from dispatching, for the teardown of a server. */
static void _mem_admission_close(struct chttpserver *srv) {
  ccol_mutex_lock(srv->wait_mutex);
  srv->mem_admission_closed = true;
  ccol_mutex_unlock(srv->wait_mutex);
}

/* Closes every waiter of the memory-wait list, for the teardown of a
 * server. Each one is claimed out of the list before it is closed. */
static void _mem_wait_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *batch[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    ccol_mutex_lock(srv->wait_mutex);
    while (srv->mem_head && n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn_t *c = srv->mem_head;
      _mem_wait_unlink_locked(srv, c);
      batch[n++] = c;
    }
    ccol_mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
      batch[i]->park = _CONN_PARK_NONE;
      _conn_close(batch[i]);
    }
  }
}

/* The streaming pool. */

#ifdef RUNNING_UNIT_TESTS
/* The thread count of the streaming pool that _stream_pool_get created last,
 * or 0 when no pool was created since the test last took the value. */
static _Atomic int g_stream_pool_threads_for_tests = 0;
/* A test arms g_start_hold_armed_for_tests to make the next chttpsvr_start
 * wait right after it swapped in the pools of the new run and released
 * srv->mutex, until the test releases it, and for 10 s at most. */
static _Atomic bool g_start_hold_armed_for_tests = false;
static _Atomic bool g_start_hold_entered_for_tests = false;
static _Atomic bool g_start_hold_release_for_tests = false;

int _chttpsvr_take_stream_pool_threads_for_tests(void) {
  return atomic_exchange(&g_stream_pool_threads_for_tests, 0);
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

/* Returns the streaming pool of srv, and creates it when this is the first
 * streaming request since the start. The pool is created at the first
 * request, and not at chttpsvr_start or at the registration of a route: the
 * size comes from the configuration of the start, and a server that
 * registers streaming routes but never receives a streaming request starts no
 * thread for them. The creation happens outside srv->mutex, and a second
 * creator that loses the race destroys its own pool again. A server that is
 * not running, or that a teardown just detached, gets CTPOOL_INVALID. */
static ctpool _stream_pool_get(struct chttpserver *srv) {
  ccol_mutex_lock(srv->mutex);
  ctpool pool = srv->stream_pool;
  bool running = srv->worker_pool != CTPOOL_INVALID;
  ccol_mutex_unlock(srv->mutex);
  if (pool || !running) return pool;

  int nthreads = atomic_load(&srv->stream_threads);
  if (nthreads <= 0) return CTPOOL_INVALID;
  char *err = NULL;
  ctpool fresh =
      ccol_create_cthread_pool_mp((size_t)nthreads, 0, srv->m_procs, &err);
  if (!fresh) return CTPOOL_INVALID;
  ccol_mutex_lock(srv->mutex);
  if (!srv->stream_pool && srv->worker_pool != CTPOOL_INVALID) {
    srv->stream_pool = fresh;
    fresh = CTPOOL_INVALID;
  }
  pool = srv->stream_pool;
  ccol_mutex_unlock(srv->mutex);
  if (fresh) {
    ctpool_shutdown_drain(fresh);
    ctpool_destroy(fresh);
  }
#ifdef RUNNING_UNIT_TESTS
  else if (pool)
    atomic_store(&g_stream_pool_threads_for_tests, nthreads);
#endif
  return pool;
}

/* Answers with 503 a request that no pool can take: a streaming request that
 * the streaming pool refuses, or a parked body that the worker pool refuses.
 * The connection already holds its in_flight_requests slot and sits in the
 * diverted list, exactly as a request that the worker pool refuses at its
 * divert. */
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
 * the diverted list and its paused registration, so that every teardown sees
 * it as a request in flight; it holds no thread. A full queue answers 503 at
 * once. The dispatch never blocks.
 *
 * The queue drains through tasks of the streaming pool, each of which takes
 * the oldest request, runs it, and takes the next one, until the queue is
 * empty; see _stream_token_task. sq_tokens counts those tasks, and a new one
 * is submitted only while fewer run than the pool has threads, so the
 * internal queue of the pool never holds more tasks than it has threads. */
static void _stream_enqueue(chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  ctpool pool = _stream_pool_get(srv);
  if (!pool) {
    _conn_reject_busy(conn);
    return;
  }
  size_t qcap = atomic_load(&srv->stream_qcap);
  unsigned tmo = atomic_load(&srv->stream_qtimeout_ms);
  size_t nthreads = (size_t)atomic_load(&srv->stream_threads);
  conn->park = _CONN_PARK_STREAM;
  conn->sq_has_deadline = tmo != UINT_MAX;
  if (conn->sq_has_deadline) {
    _slow_clock(&conn->sq_deadline);
    _timespec_add_ms(&conn->sq_deadline, tmo);
  }

  ccol_mutex_lock(srv->wait_mutex);
  if (qcap != SIZE_MAX && srv->sq_len >= qcap) {
    ccol_mutex_unlock(srv->wait_mutex);
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
  bool submit = srv->sq_tokens < nthreads;
  if (submit) srv->sq_tokens++;
  ccol_mutex_unlock(srv->wait_mutex);
  /* conn may already run on the streaming pool from here on. */
  if (!submit ||
      _srv_submit(srv, _SRV_POOL_STREAM, pool, _stream_token_task, srv))
    return;

  /* The pool refused the task, which only a pool in shutdown or a failed
   * allocation does. When no other task drains the queue, nothing would
   * ever take what waits in it, so every waiter is answered now. */
  ccol_mutex_lock(srv->wait_mutex);
  srv->sq_tokens--;
  bool orphaned = srv->sq_tokens == 0;
  ccol_mutex_unlock(srv->wait_mutex);
  if (orphaned) _stream_queue_flush(srv);
}

/* One task of the streaming pool. It takes the oldest queued request, runs
 * it, and takes the next one, until the queue is empty. Taking a request is a
 * claim: under srv->wait_mutex, exactly as the sweep claims an expired one,
 * so a request is answered by one of the two and never by both. A request
 * whose deadline passed while it waited gets 503 here, through the same
 * rejection path, whose write never waits on the peer.
 *
 * A streaming handler runs on a thread of this pool and nowhere else. It may
 * block for as long as its client takes, bounded by the read limits of the
 * configuration; while it does, it holds one streaming thread and never a
 * thread of the worker pool. The thread counts as a worker of its server, so
 * the calls that a handler must not make on its own server (chttpsvr_destroy,
 * a restart, chttpsvr_engine_wait) are refused or fatal there exactly as they
 * are on the worker pool. */
static void _stream_token_task(void *arg) {
  struct chttpserver *srv = (struct chttpserver *)arg;
  _chttpsvr_mark_worker_thread(srv);
  for (;;) {
#ifdef RUNNING_UNIT_TESTS
    _stream_claim_hook_wait_if_armed(1);
#endif
    ccol_mutex_lock(srv->wait_mutex);
    chttpsvr_conn_t *c = srv->sq_head;
    if (!c) {
      srv->sq_tokens--;
      ccol_mutex_unlock(srv->wait_mutex);
      return;
    }
    _stream_unlink_locked(srv, c);
    ccol_mutex_unlock(srv->wait_mutex);
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
 * so does _stream_enqueue when the queue lost every task that drains it. */
static void _stream_queue_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn_t *batch[_CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    ccol_mutex_lock(srv->wait_mutex);
    while (srv->sq_head && n < _CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn_t *c = srv->sq_head;
      _stream_unlink_locked(srv, c);
      batch[n++] = c;
    }
    ccol_mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) _conn_reject_busy(batch[i]);
  }
}

/* The part of a sweep tick that no idle list reaches: the requests that wait
 * for body memory and the streaming requests that wait for a thread. Each
 * expired one is claimed out of its list under srv->wait_mutex, and then
 * answered with 503 and a Retry-After header. A memory waiter holds no
 * in_flight_requests slot, so it goes through _conn_dispatch_reject, which
 * takes one; a queued streaming request already holds its slot. */
static void _sweep_parked_waits(struct chttpserver *srv,
                                struct timespec now_slow) {
  /* Every expired waiter of both lists goes in one tick, with no cap, and
   * each one chains through the link field of the list that it left, exactly
   * as in _sweep_server. */
  unsigned mem_ms = atomic_load(&srv->mem_wait_ms);
  chttpsvr_conn_t *expired = NULL;
  bool any = false;
  ccol_mutex_lock(srv->wait_mutex);
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
  ccol_mutex_unlock(srv->wait_mutex);
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
  /* A waiter that left can be the one that kept the progress rule from
   * applying; see _mem_admit. */
  if (any) _mem_admit(srv);

  ccol_mutex_lock(srv->wait_mutex);
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
  ccol_mutex_unlock(srv->wait_mutex);
  while (expired) {
    chttpsvr_conn_t *c = expired;
    expired = c->sq_next;
    c->sq_next = NULL;
    _conn_reject_busy(c);
  }
}

/* The cold half of _conn_start_diverted, for a request whose divert_gate is
 * above the carry-over; see the field comment of divert_gate. It returns true
 * when it took the connection over, and false when the ordinary divert goes
 * on. The caller already counted the request in in_flight_requests, put the
 * connection in the diverted list, paused its registration and saved the
 * carry-over. */
static __attribute__((noinline, cold)) bool _conn_divert_gate(
    chttpsvr_conn_t *conn) {
  size_t gate = conn->divert_gate;
  if (gate == SIZE_MAX) {
    _stream_enqueue(conn);
    return true;
  }
  struct chttpserver *srv = conn->srv;
  if (_mem_try_reserve(srv, conn, gate)) return false;
  /* The body does not fit. The request waits with no thread, and nothing
   * reads a byte of its body, nor answers "100 Continue", until memory
   * frees; see _mem_admit. */
  _diverted_list_remove(conn);
  _mem_wait_enqueue(srv, conn, gate, _CONN_PARK_MEM_START);
  _release_in_flight(srv);
  _mem_admit(srv);
  return true;
}

/* Parking and resuming. */

/* The body-read half of _drain_body once the carry-over is empty. It reads
 * the socket without waiting. When the socket has nothing, it sets
 * park_pending to _CONN_PARK_BODY and returns -1, and the caller gives the
 * connection up instead of waiting. A chunked body first makes sure that its
 * reservation of body memory covers this read; when it cannot, it sets
 * park_pending to _CONN_PARK_MEM_BODY and returns -1 without reading, so the
 * connection waits for memory with no byte taken past its reservation. A
 * body that a Content-Length frames reserved all of its memory before the
 * worker started; see _conn_divert_gate.
 *
 * The first call of a request starts the phase that the rate floor and the
 * per-gap limit measure. */
static __attribute__((noinline, cold)) ssize_t _drain_body_socket_read(
    chttpsvr_conn_t *conn, chttp1_stream_t *stream, char *raw, size_t len) {
  struct chttpserver *srv = conn->srv;
  if (!conn->rate_active) _rate_start(conn, conn->body_bytes_seen);
  size_t cap = conn->body.cap;
  if ((conn->body.len + len > cap || conn->mem_charged < cap) &&
      !conn->options_star && !chttp1_has_content_length(&conn->parser) &&
      atomic_load(&srv->mem_cap) != SIZE_MAX) {
    /* A chunked body reserves the memory that its buffer holds, before the
     * read that needs it. The buffer can hold bytes that came with the
     * headers, read before any reservation, and this read can make it grow.
     * A growth holds the old block and the new one together until the copy
     * ends, so the reservation covers both; _on_body gives the old part back
     * once the copy is done. */
    size_t limit = atomic_load(&srv->max_body_size);
    size_t target = _body_grow_target(conn, cap, conn->body.len + len, limit);
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

/* Arms the registration of conn for the direction that the park needs, and
 * publishes conn into the idle list with park set. The caller owns conn and
 * this is its last touch of it. The registration is paused while a worker
 * owns the connection, so the common case is a modify and a resume; a
 * connection whose first request resolved before it ever had a registration
 * gets a fresh one, exactly as in the keep-alive tail of _task_tail. It
 * returns false, with conn untouched by any list, when the reactor refuses,
 * and the caller then closes it. */
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
    if (ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg, dir) !=
            ccol_success ||
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
    /* See the field comments of conn->lifetime_refs and srv->lifetime_refs. */
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

/* Gives up a connection whose body stopped arriving, or must wait for
 * memory, on the worker that read it. The stream holds nothing to keep: the
 * body reader touches the socket only once the carry-over is empty, and a
 * pipelined next request can follow only a body that is complete. The
 * connection then holds no thread and no in_flight_requests slot; the
 * reactor, or an admission of body memory, hands it to a worker again. */
static __attribute__((noinline, cold)) void _task_park_body(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req) {
  uint8_t kind = conn->park_pending;
  conn->park_pending = _CONN_PARK_NONE;
  _destroy_req_qparams(req);
  chttp1_stream_release(stream);
  _diverted_list_remove(conn);
  if (kind == _CONN_PARK_MEM_BODY) {
    _mem_wait_enqueue(srv, conn, conn->mem_need, _CONN_PARK_MEM_BODY);
    _release_in_flight(srv);
    _mem_admit(srv);
    return;
  }
  if (!_conn_park_arm(conn, _CONN_PARK_BODY)) {
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }
  _release_in_flight(srv);
}

/* Hands a parked connection, which the reactor just claimed on readiness, to
 * any free worker; see _task_resume. It pauses the registration first, as
 * _conn_start_diverted does, so that no further readiness fires while a
 * worker owns the connection. A full pool answers a parked body with 503, as
 * no response byte went out yet, and closes a parked response, whose bytes
 * are already partly out. */
static __attribute__((noinline, cold)) void _conn_resume_parked(
    chttpsvr_conn_t *conn) {
  struct chttpserver *srv = conn->srv;
  _conn_stamp_claim(conn);
  _conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  _diverted_list_add(conn);
  ccol_mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  ctpool pool = srv->worker_pool;
  ccol_mutex_unlock(srv->mutex);
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

/* The lingering close. A connection that the server closes while its client
 * still sends, typically the body of a request that the server refused,
 * makes the kernel answer those bytes with a reset. A reset can destroy the
 * response in the receive buffer of the client before the client reads it,
 * and a client that is still busy writing sees only the reset. The server
 * therefore shuts down its side for writing, which delivers the end of the
 * response and then an end of stream, and reads and discards what the
 * client still sends, for at most _CHTTPSVR_LINGER_MS, before it closes, as
 * the lingering_close of nginx does. The bound is a time and not a byte
 * count: a client that sends the whole body of a refused upload before it
 * reads the response, as many clients do, reads that response only once
 * the server took every byte of that body. A byte budget smaller than the
 * body closes the connection under that client and turns the response into
 * a reset.
 *
 * A lingering connection holds no thread: it sits in the parked list with its
 * registration armed for reading, the reactor discards what arrives, and the
 * sweep closes it at its deadline. It still counts against max_connections
 * until it closes. One dispatch discards at most _CHTTPSVR_LINGER_STEP_BYTES
 * and leaves the rest to the next readiness report, so that a fast sender
 * cannot hold the reactor thread in one dispatch. */
#define _CHTTPSVR_LINGER_MS 2000u
#define _CHTTPSVR_LINGER_STEP_BYTES ((size_t)256 * 1024)

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_linger_count_for_tests = 0;
size_t _chttpsvr_linger_count_for_tests(void) {
  return atomic_load(&g_linger_count_for_tests);
}
#endif

/* Says whether the client of conn can still send bytes that the server did
 * not read: the rest of a body, or anything already queued in the socket. */
static bool _conn_request_bytes_may_remain(chttpsvr_conn_t *conn) {
  if (chttp1_request_bytes_may_remain(&conn->parser)) return true;
  int queued = 0;
  return ioctl(conn->fd, FIONREAD, &queued) == 0 && queued > 0;
}

/* Reads and discards what the client sent, without waiting, up to
 * _CHTTPSVR_LINGER_STEP_BYTES in one call. It returns true while the linger
 * goes on, which is when the socket is empty or the step budget is spent, and
 * false once the client closed or the socket failed. */
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

/* Starts the lingering close of conn in place of _conn_close. The caller
 * owns conn, holds no list of it except the diverted list, and does not
 * touch it again. A TLS connection sends its close_notify first, and the
 * TLS state goes with it: nothing else is written, and what the client
 * still sends is discarded unread. */
static __attribute__((noinline, cold)) void _conn_linger(
    chttpsvr_conn_t *conn) {
  /* Nothing reads the body of the request past this point, so it and its
   * reservation of body memory go now and not at the close, which can be up
   * to _CHTTPSVR_LINGER_MS away. */
  _conn_drop_request_state(conn);
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

/* The reactor claimed a lingering connection on readiness. It discards what
 * arrived, and either parks the connection again, with its registration
 * still armed, or closes it. */
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

/* Saves the state of a response whose write met a full socket, for
 * _task_continue_write. head and head_len are the unsent part of the header
 * block, which lives on the stack of _send_response and is therefore copied;
 * body_pos is how much of the body already went out. It also moves any
 * pipelined bytes of a next request out of the stream and onto the
 * connection, because the caller releases the stream. The rate phase of the
 * write starts where the write started: the deadline of the write records
 * that instant when a total-duration bound is on, and this call is the best
 * estimate otherwise. internal marks a write of the library itself, a
 * rejection or the interim line, whose bound has the ceiling of
 * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. A failed copy leaves wp_active
 * false, and the caller closes the connection as for any failed write. */
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
    /* The pipelined bytes of a next request could not be saved and are
     * lost. The connection closes once the parked write ends, instead of
     * waiting for a request that its client already sent. */
    conn->wp_keep_alive = false;
  }
  _rate_start(conn, body_pos);
  /* The deadline of the write lies one total-duration bound after its
   * start. A write of the library itself has the ceiling of
   * _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS over that bound; see
   * _response_write_deadline_ok. */
  unsigned max_dur = atomic_load(&conn->srv->max_response_write_duration_ms);
  if (internal && (max_dur == 0 || max_dur > _internal_write_ceiling_ms()))
    max_dur = _internal_write_ceiling_ms();
  if (conn->write_deadline_set && max_dur) {
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    long long run =
        (long long)max_dur - _ms_since(conn->write_deadline, now_mono);
    if (run > 0) _timespec_sub_ms(&conn->rate_start, run);
  }
}

/* Gives up a connection whose response met a full socket; see
 * _send_response_park. It waits in the idle list for room to write, and the
 * sweep enforces the write limits and the rate floor against it. */
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

/* Forgets the saved state of a parked response once its write ended. */
static void _conn_end_parked_write(chttpsvr_conn_t *conn) {
  conn->wp_active = false;
  _ccol_mem_free(conn->m_procs, conn->wp_head);
  conn->wp_head = NULL;
  conn->wp_head_len = conn->wp_head_pos = conn->wp_body_pos = 0;
}

/* Writes what is left of a parked response without waiting. It returns 1
 * once the whole response is out, 0 when the write failed or a write limit
 * expired, and -1 when the socket is full again. */
static int _send_response_continue(chttp1_stream_t *stream,
                                   chttpsvr_conn_t *conn) {
  size_t blen = conn->wp_no_body ? 0 : conn->resp.body_len;
  for (;;) {
    size_t alen = conn->wp_head_len - conn->wp_head_pos;
    size_t brem = blen - conn->wp_body_pos;
    if (alen + brem == 0) return 1;
    unsigned call_timeout_ms = 0;
    /* A rejection and the interim line keep the ceiling of the internal
     * writes of the library; see _CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS. */
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

/* What _drain_body returns when the body stopped arriving, or must wait for
 * memory, and the caller must park the connection instead of waiting. The
 * value is one that _drain_body never returns for any other reason. */
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

/* _check_read_deadline for _drain_body, which runs for every buffered body
 * and keeps the whole check inline. */
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

/* Drains the whole body of the request of a buffered route. It does so with
 * chttp1_stream_read and chttp1_parser_execute, and the calling worker
 * thread drives both. The library calls this function for a buffered route
 * only; see _task_worker, which never calls it for a streaming one.
 *
 * The handler of a streaming route pulls its own body with
 * chttpsvr_req_read() instead. Body bytes that the handler leaves unread
 * never come here for a separate drain. The library simply cannot keep that
 * connection alive; see the msg_fully_parsed and keep_alive computation of
 * _task_worker. Without that close, the library reads the unread bytes as
 * the start of the next pipelined request.
 *
 * The function returns ccol_success, or a specific failure that the caller
 * maps to a status code. */
static ccol_retval_t _drain_body(chttpsvr_conn_t *conn,
                                 chttp1_stream_t *stream) {
  for (;;) {
    if (chttp1_parser_message_complete(&conn->parser)) break;
    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!_check_read_deadline_inl(conn, &timeout_ms)) return ccol_timed_out;
    char raw[8192];
    /* The carry-over holds the body bytes that arrived with the headers, and
     * reading it never waits. Once it is empty, _drain_body_socket_read reads
     * the socket without waiting, and asks the caller to park the connection
     * when nothing is there; see the field comment of park_pending. */
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
      /* A truncated body: the peer stopped before the framing of its own
       * message was complete. That is a malformed request and not a server
       * fault; see the field comment of conn->body_malformed. */
      conn->body_malformed = true;
      return ccol_http_transfer_aborted;
    }
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_PAUSED) {
      /* This message is done, but raw can hold more than it needed. The
       * extra bytes come off the wire in the same read, and they belong to a
       * pipelined next request right behind this one.
       * chttp1_parser_consumed() reports exactly where the framing of this
       * message ended. The code must push everything past that point back
       * onto the carry-over of stream. Without that push, the pipelined next
       * request vanishes in silence. Those bytes are already gone from the
       * socket receive buffer of the kernel for good. The client then waits
       * forever for a response that never comes. See the doc comment of
       * chttp1_stream_take_leftover. See also the call of _task_worker into
       * it, right before the library releases this stream, for the other
       * half of this. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(stream, raw + consumed,
                                            (size_t)n - consumed)) {
        /* The bytes of the next request are lost. The connection closes
         * after this response instead of waiting for a request that its
         * client already sent. */
        chttp1_parser_force_close(&conn->parser);
      }
      break;
    }
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* A chunk can declare a size above max_chunk_size_override; see
       * _conn_reset_for_request. The code reports that in the same way as a
       * cumulative body over the limit, which is conn->body_too_large and
       * ccol_msg_too_large. It does not report it in the general
       * transfer-aborted group, where every other malformed chunk-framing
       * error goes. For the caller it is the same "body too large" outcome.
       * The library only catches it before one byte of that chunk has to
       * arrive. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      /* A CHTTP1_ERROR here is the parser refusing the bytes of the body
       * itself: a chunk size that is not hexadecimal, a chunk that no CRLF
       * terminates, or a malformed trailer line. That is a client error, and
       * conn->body_malformed is what turns it into a 400 instead of a 500
       * further up. The code sets it only after it has ruled out the
       * condition that owns a status of its own, so the priority below stays
       * exactly as it was. A CHTTP1_USER is never a syntax fault here: it is
       * one of the callbacks of this file reporting a condition that already
       * set its own flag. */
      if (r == CHTTP1_ERROR && !conn->body_too_large &&
          !conn->body_alloc_failed)
        conn->body_malformed = true;
      /* A real allocation failure inside _on_body sets
       * conn->body_alloc_failed. The code also reports that as its own
       * distinct outcome, and never inside the general transfer-aborted
       * group. chttpsvr_req_stream_error() uses the same priority for the
       * streaming-route path. See the field comment of body_alloc_failed. */
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
  /* conn and conn->matched_route are never NULL for a req that a caller can
   * reach through the documented public API. Only _task_worker builds a
   * chttpsvr_req, and that function runs only after a route already matched.
   * The _on_headers_complete function sets conn->matched_route before the
   * library diverts this request to a worker at all.
   *
   * The code guards both fields anyway. chttpsvr_req_method, _path, _header
   * and _raw_query differ: each dereferences req->conn without a check once
   * it rules out a NULL req. The same "never NULL in practice" argument
   * applies to all of them. The guard here means that two kinds of misuse
   * fail safely instead of a crash. The first is a req used past its
   * documented lifetime, which is the duration of the handler call alone.
   * The second is any future misuse that reaches here with a req that the
   * library did not fill in completely. */
  if (!conn || !conn->matched_route) return -1;
  if (!conn->matched_route->is_streaming) return -1;
  if (!req->stream) return -1;

  /* This is the lazy Expect: 100-continue interim send for a streaming
   * route. See the comment of _task_worker for why the library defers it to
   * here, instead of a send before the handler runs. It fires at most once,
   * on whichever chttpsvr_req_read() call is the first one for this request.
   *
   * A handler may never call chttpsvr_req_read() at all, and reject the
   * request outright instead. Such a handler correctly never triggers this
   * send. The Expect: 100-continue logic of the client then sees the final
   * rejection response directly. It does not see a "100 Continue" that tells
   * it to upload a body which the server was never going to read.
   *
   * The code also skips the send, even on this first call, when the message
   * never carried a body. That is a message with no Content-Length and no
   * chunked Transfer-Encoding. chttp1_parser already reached
   * message-complete at header-parse time in that case, because
   * CHTTP1_HEADERS_DIVERT_BODY downgrades to an immediate completion when
   * there is nothing to divert. There is therefore no body left to invite. A
   * "100 Continue" here is the same mistake that the reject-without-reading
   * case above avoids. Only the route to it differs: a bodyless request
   * instead of a handler that rejects.
   *
   * The code still sets interim_continue_sent without exception. This check
   * therefore does not repeat on every later chttpsvr_req_read() call for
   * the same request. */
  if (conn->expects_continue && !conn->interim_continue_sent) {
    conn->interim_continue_sent = true;
    if (!chttp1_parser_message_complete(&conn->parser)) {
      unsigned wtimeout_ms = atomic_load(&conn->srv->response_write_timeout_ms);
      /* See the doc comment of _write_interim_continue. A TOTAL write
       * failure leaves the connection untouched. The read attempt below
       * therefore fails on its own, and it reports through the ordinary
       * error path of this function either way. A real SHORT write is
       * different: it leaves a truncated status line on the wire that no
       * client can parse. A plain read failure below can never detect that,
       * and it can never account for it.
       *
       * The code therefore treats both outcomes in the same way. It latches
       * them onto conn, so the later _send_response call of _task_worker is
       * suppressed. The library then never writes a real response after an
       * interim one that may be corrupt. The code also surfaces the outcome
       * to this streaming handler as an ordinary transfer-aborted error,
       * through the immediate return below. That is exactly how the handler
       * already has to treat any other failure that ends a stream. */
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
    /* The rate floor bounds this wait too: the read gives up at the instant
     * when the average rate of this body would fall below the floor. */
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
        /* One of two things happened. The per-call poll(2) of
         * stream_read_timeout_ms timed out. Or _check_read_deadline above
         * folded the deadline of max_body_read_duration_ms into the timeout
         * of this call, and that deadline expired inside the poll. The
         * up-front check of that function on the NEXT call cannot catch the
         * second case. There is no next call, because chttpsvr_req_read
         * returns here at once.
         *
         * The code reports this in the same way as _drain_body does for the
         * buffered-route path. Both use the conn->deadline_exceeded flag
         * that chttpsvr_req_stream_error() reads. Both caps therefore give
         * the same ccol_timed_out result. See
         * max_body_read_duration_exceeded_reports_ccol_timed_out and
         * stream_read_timeout_reports_ccol_timed_out in tests.c. */
        conn->deadline_exceeded = true;
      } else {
        /* This is a hard I/O error, and not a timeout. The peer reset the
         * connection, or a raw read() or ctls_conn_read() failed, or
         * something similar happened. It belongs to the same "connection
         * closed or malformed framing" group that the two branches below
         * report through conn->transfer_aborted. */
        conn->transfer_aborted = true;
      }
      return -1;
    }
    if (n == 0) {
      /* The peer closed its write side, or the whole connection. It did so
       * before the declared or chunked framing said that the body was done.
       * This is a truncated body, and not the natural end of the message.
       * The natural end is the case where chttp1_parser_message_complete()
       * returns true above, and the code handles that separately.
       *
       * Without this flag, chttpsvr_req_stream_error() has no way to report
       * the truncation. It falls through to ccol_success and tells a
       * streaming handler that a truncated upload was a clean read. Take a
       * POST that declares Content-Length: 100 whose peer sends 20 bytes and
       * then half-closes. The handler sees chttpsvr_req_read() return -1
       * while chttpsvr_req_stream_error() reports ccol_success. */
      conn->transfer_aborted = true;
      return -1;
    }
    chttp1_errno_t r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      /* This is malformed framing, such as a bad chunk-size line. It sits
       * past the point where the max_body_size check in _on_body can already
       * have set body_too_large for this same CHTTP1_USER or CHTTP1_ERROR
       * result. A chunk whose declared size exceeds
       * max_chunk_size_override sets it here too, for the same reason; see
       * the same check in _conn_reset_for_request and _drain_body.
       * chttpsvr_req_stream_error() reads body_too_large first, so a set of
       * both flags here is harmless and keeps that priority. */
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      conn->transfer_aborted = true;
      return -1;
    }
    if (r == CHTTP1_PAUSED) {
      /* The reasoning is the same as for the identical check in
       * _drain_body. raw can hold more than this message needed. Those extra
       * bytes belong to a pipelined next request, and the library already
       * read them off the wire. The code pushes the trailing, unconsumed
       * part back onto the carry-over of req->stream. The later
       * chttp1_stream_take_leftover() call of _task_worker then reclaims it.
       * Without that push, those bytes vanish. The message_complete check
       * above returns 0 at once on the next pass of the loop, once the
       * library drains conn->body. */
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(req->stream, raw + consumed,
                                            (size_t)n - consumed)) {
        /* See the same failure in _drain_body. */
        chttp1_parser_force_close(&conn->parser);
      }
    }
    /* For a CHTTP1_OK or a CHTTP1_PAUSED, the loop goes back and drains what
     * _on_body just appended to conn->body. */
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

/* The part of a request after its response is sent, or given up: the
 * reclaim of a pipelined next request, and then either a close or the hand
 * back to the reactor. _task_worker, _task_resume and the continuation of a
 * parked response all end here. It is always inlined, so that the path of an
 * ordinary request compiles as one function. */
static inline __attribute__((always_inline)) void _task_tail(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req, bool prepared, bool keep_alive, bool response_out) {
  /* This reclaims the bytes of a further pipelined request on this same
   * connection. The library already pulled those bytes off the wire, and
   * chttp1_stream_release below discards them without exception. Two
   * situations put bytes here.
   *
   * In the first, this message completed before _drain_body or
   * chttpsvr_req_read had to touch stream at all. A bodyless GET or HEAD
   * does that, and so does an explicit Content-Length: 0. The whole original
   * conn->_carry_over that the library prepared this stream with then still
   * sits here untouched.
   *
   * In the second, a read that drained the body swept up extra bytes past
   * the framing boundary of this message. The CHTTP1_PAUSED handling of
   * _drain_body and chttpsvr_req_read pushes exactly that case back onto the
   * carry-over of stream. This call then picks it up in the same way as the
   * first case.
   *
   * The code reclaims whenever prepared is true, without exception. That is
   * cheap and does no I/O. It only keeps the bytes when this connection
   * stays alive. It frees them outright otherwise, because a connection that
   * closes has nowhere to hand pipelined bytes to. That matches the
   * documented "reject and close" precedent of
   * pipelined_bytes_after_rejected_route_not_misparsed. */
  size_t reclaimed_len = 0;
  char *reclaimed =
      prepared ? chttp1_stream_take_leftover(stream, &reclaimed_len) : NULL;

  if (prepared) chttp1_stream_release(stream);
  _destroy_req_qparams(req);

  if (!keep_alive) _ccol_mem_free(conn->m_procs, reclaimed);

  /* The code releases in_flight_requests only once this connection reaches a
   * state that _drain_and_close_all_connections can observe. It does so with
   * _release_in_flight, at the bottom of every exit path below. The two
   * observable states are a full close through _conn_close, and a safe
   * publish back into the idle list. A release here, before either, opens
   * the exact use-after-free window that the comment of _release_in_flight
   * describes. Every _release_in_flight call in this function uses srv. srv
   * is a local that the code captured earlier. It stays valid even after the
   * library frees conn below. */

  if (!keep_alive) {
    /* A response that stopped on a full socket is not finished; the
     * connection waits for room to write the rest; see _conn_park_write. */
    if (__builtin_expect(conn->wp_active, 0)) {
      _conn_drop_request_state(conn);
      _conn_park_write(conn, srv);
      return;
    }
    /* A complete response on a connection whose client can still be
     * sending (an unread body, or pipelined bytes) closes with a lingering
     * close; see _conn_linger. */
    if (__builtin_expect(
            response_out && (reclaimed_len != 0 ||
                             chttp1_request_bytes_may_remain(&conn->parser)),
            0))
      _conn_linger(conn);
    else
      _conn_close(conn);
    _release_in_flight(srv);
    return;
  }

  /* This is the keep-alive path. It resets the per-request state and hands
   * the connection back to the reactor, to read the headers of the next
   * request.
   *
   * conn->reg is not NULL here only when this request went through a live
   * ccol_event_loop registration that _conn_start_diverted then paused; see
   * the comment of that function. A resume of that registration is far
   * cheaper than a fresh ccol_event_loop_add. The event_entry, the entry in
   * the fd registry chmap, and the epoll_ctl(ADD) all stayed in place. Only
   * the combined epoll interest mask needs a recompute.
   *
   * conn->reg is NULL here in two cases, and they are worth telling apart.
   * The first is a pause that really failed and removed the registration;
   * see _conn_pause_reg. The second is
   * far more common: the optimistic first read of _conn_pump, right after
   * the accept, read the headers of this connection synchronously. It
   * sometimes reads several requests worth of pipelined bytes that way. It
   * does so before the library registers this connection with the reactor at
   * all, because _listener_on_readable calls _conn_pump directly, with no
   * ccol_event_loop_add in between.
   *
   * The "no registration ever existed" case of this branch and the "pause
   * failed" case of _conn_start_diverted both give the same NULL value.
   * Nothing here can tell them apart, and that is by design. Both need the
   * exact same fresh ccol_event_loop_add fallback below. This is therefore
   * not a gap. It is two paths that share one outcome. */
  /* This worker is done with conn as a worker-owned entity for now. conn is
   * no longer a candidate for _force_unblock_diverted_connections. The
   * handling of the pipelined bytes below can divert a further request on
   * this same connection. The _diverted_list_add call of
   * _conn_start_diverted then adds conn again, and that call is idempotent.
   * Otherwise conn stays out of the list until some later divert cycle. */
  _diverted_list_remove(conn);
  _conn_reset_for_request(conn);
  conn->state = CONN_ST_READING_HEADERS;

  if (reclaimed) {
    /* chttp1_stream_take_leftover() allocated these bytes from conn->m_procs,
     * which is the allocator that the stream was prepared with and the one
     * that conn->_carry_over always comes from. conn therefore adopts the
     * buffer as it is, with no copy. A non-NULL result always carries at
     * least one byte. */
    conn->_carry_over = reclaimed;
    conn->_carry_over_len = reclaimed_len;
  } else if (__builtin_expect(reclaimed_len != 0, 0)) {
    /* The copy of the pipelined bytes of the next request ran out of
     * memory, so those bytes are gone. A connection kept alive would wait
     * for a request that its client already sent; closing it tells the
     * client to send it again. */
    _conn_close(conn);
    _release_in_flight(srv);
    return;
  }

  if (conn->_carry_over_len > 0) {
    /* The bytes of the next request on this connection already sit in
     * memory. They are not merely available for a later read. They are gone
     * from the socket receive buffer of the kernel for good. A wait for a
     * fresh epoll readiness event here therefore waits forever for data that
     * never arrives. That wait is the plain resume or add path below.
     *
     * The code feeds those bytes straight into the parser that it just
     * reset, now, on this worker thread. That is exactly what _conn_pump
     * does once real socket bytes come in; see the doc comment of
     * _conn_feed_bytes. This is what prevents the pipelining data loss that
     * the doc comment of chttp1_stream_take_leftover describes. Without this
     * feed, the library discards these bytes. Without
     * chttp1_stream_take_leftover at all, chttp1_stream_release above frees
     * them, and nothing can get them back. */
    char *lo = conn->_carry_over;
    size_t lo_len = conn->_carry_over_len;
    conn->_carry_over = NULL;
    conn->_carry_over_len = 0;
    /* The code captures mp from srv, which is a local that it captured
     * earlier and which stays valid even after the library frees conn below.
     * It does not read conn->m_procs after the call. _conn_feed_bytes can
     * already have resolved the fate of conn by the time it returns. It can
     * have diverted conn to another worker thread that races this one to the
     * end and frees it. It can have rejected conn through a synchronous
     * fallback close. It can have closed conn outright on a raw parse error.
     * Its own contract says that the caller must not touch conn again, and a
     * read of conn->m_procs is exactly such a touch.
     *
     * conn->m_procs and srv->m_procs are always the same pointer; see
     * _conn_create. A read from srv is therefore both safe and equivalent. A
     * read of conn->m_procs here is a real use-after-free. It happens every
     * time when the further pipelined bytes are malformed. The
     * _conn_feed_bytes function then closes and frees conn synchronously on
     * this same thread before it returns. It also happens every time when
     * the fallback of reject_pool fires synchronously. In every other case
     * it is a race,
     * where another worker thread finishes and frees conn before this thread
     * reaches this free. */
    ccol_memmgmt_procs_t *mp = srv->m_procs;
    bool need_more = _conn_feed_bytes(conn, lo, lo_len);
    _ccol_mem_free(mp, lo);
    if (!need_more) {
      /* _conn_feed_bytes already resolved the fate of conn. It diverted a
       * further pipelined request to a worker, rejected that request, or
       * closed the connection outright. A divert can go to this very thread
       * pool, but it always goes through a fresh ctpool_submit, and never
       * through a direct recursive call. The code releases only the
       * in_flight_requests slot of this call here. The library incremented
       * that slot when it first diverted THIS request. Whatever
       * _conn_feed_bytes just started owns its own independent slot. */
      _release_in_flight(srv);
      return;
    }
    /* A CHTTP1_OK means that the parser now holds part of the headers of the
     * next request. That is exactly the state a live socket read leaves. The
     * code falls through to the TLS drain and then to the ordinary "wait for
     * more" registration logic below. */
  }

  /* A TLS connection can hold more of the next request inside the TLS layer,
   * and not in the socket. OpenSSL reads a whole record, up to 16 KB of
   * plaintext, and a pipelining client packs several requests into one
   * record. Every read of this request took only as much as its buffer held.
   * epoll(7) reports nothing for those bytes, so a registration here waits
   * for data that is already in memory. The connection then stalls until the
   * client sends something else, or until the idle timeout closes it.
   *
   * The code therefore keeps reading, on this worker thread, while the TLS
   * layer holds input. It feeds each read into the parser exactly as the read
   * loop of _conn_pump does. The carry-over above comes first, because those
   * bytes precede everything that the TLS layer still holds. The loop ends on
   * EWOULDBLOCK, and only then does the registration below wait for the
   * socket. */
  ccol_select_dir tail_dir = ccol_select_read;
  while (conn->tls && ctls_conn_has_pending_input(conn->tls)) {
    char tls_buf[8192];
    ssize_t n = ctls_conn_read(conn->tls, tls_buf, sizeof(tls_buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      /* See the doc comment of ctls_conn_wants_write(). The read can need
       * the socket to become writable first, and the registration below
       * then waits in that direction. */
      if (ctls_conn_wants_write(conn->tls)) tail_dir = ccol_select_write;
      break;
    }
    if (n <= 0) {
      _conn_close(conn);
      _release_in_flight(srv);
      return;
    }
    if (!_conn_feed_bytes(conn, tls_buf, (size_t)n)) {
      /* See the same branch of the carry-over feed above. */
      _release_in_flight(srv);
      return;
    }
  }

  if (conn->reg) {
    /* The code forces the registration back to the read direction before it
     * resumes it. The only purpose of this site is to wait for the bytes of
     * the next request, which is always the read direction. But the recorded
     * direction of conn->reg, which is the sel.dir of ccol_event_reg, can
     * still be the write direction from an earlier cycle.
     *
     * That happens in one specific sequence. One read of this exact
     * connection completed the request that just finished above. That most
     * recent successful read came after a write-direction wait in the read
     * loop of _conn_pump. ctls_conn_wants_write drove that wait. The library
     * then diverted the connection to this worker on that same successful
     * read. Nothing in between flips the registration back to read. That
     * flip happens only on a LATER read attempt that meets an EWOULDBLOCK
     * and wants the read direction.
     *
     * ccol_event_loop_modify is safe to call on a registration that is
     * paused. It only updates the recorded direction of that registration
     * and the read or write slot assignment of the underlying fd entry. The
     * library keeps delivery suppressed until ccol_event_loop_resume arms it
     * again. This call therefore always leaves conn->reg in the direction of
     * tail_dir by the time it resumes. That is the read direction, unless the
     * TLS drain above ended on a read that needs the socket to become
     * writable first.
     *
     * The alternative is one extra, wasted write-readiness dispatch. A TCP
     * send buffer is almost always writable at once, so a stale
     * write-direction resume corrects itself through a spurious re-dispatch
     * that arrives almost immediately. It does not hang. But that is only
     * true when the registration really carries an on_writable handler. See
     * the else branch below for the real hang that this pairs with.
     *
     * This modify is not expected to fail, because the library knew reg was
     * live a moment ago. The code checks it anyway, instead of an
     * assumption. That matches the established discipline of this function
     * for every other ccol_event_loop call. */
    if (ccol_event_loop_modify(srv_engine_bundler.reactor, conn->reg,
                               tail_dir) != ccol_success) {
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
    /* .on_writable is needed here for the same reason that the two
     * ccol_event_loop_add call sites of _conn_pump already carry it. This
     * connection has no live registration yet. Its very first request
     * resolved fully synchronously, through the optimistic first read and
     * parse of _listener_on_readable, with no wait on the reactor at all.
     * But a LATER keep-alive request on this same connection can still need
     * write-direction interest in the middle of a read. ctls_conn_wants_write
     * decides that; see its own doc comment.
     *
     * The read loop of _conn_pump then calls
     * ccol_event_loop_modify(conn->reg, ccol_select_write) on this exact
     * registration. That call succeeds, because a modify changes only the
     * epoll interest and never the handlers. With no on_writable handler on
     * that registration, ccol_event_loop drops a later write-readiness event
     * in silence. Its documented behavior for a direction with no handler
     * pointer is a no-op. The connection then stalls for good, until an
     * unrelated timeout steps in. The operator may have configured that
     * timeout to 0, which turns it off.
     *
     * This is a real, reproducible gap, and not a theoretical one. Every
     * registration that this file creates for the fd of a live connection
     * must carry both handlers. Any of them can later need either
     * direction. */
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
    /* See the field comment of conn->lifetime_refs. See also the field
     * comment of srv->lifetime_refs, which gets the same treatment at the
     * server level. */
    atomic_fetch_add(&conn->lifetime_refs, 1);
    atomic_fetch_add(&srv->lifetime_refs, 1);
  }
  _idle_list_add(conn);
  _release_in_flight(srv);
}

/* The part of a request after its body is read: the response and then
 * _task_tail. */
static inline __attribute__((always_inline)) void _task_finish(
    chttpsvr_conn_t *conn, struct chttpserver *srv, chttp1_stream_t *stream,
    chttpsvr_req *req, bool prepared, bool aborted, unsigned write_timeout_ms) {
  /* The handler has returned, or will never run, so nothing reads the body
   * again; see _conn_drop_body. */
  _conn_drop_body(conn);
  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large;

  /* The !conn->interim_write_failed test matters here. A real short write of
   * the "100 Continue" interim line already left a truncated status line on
   * the wire; see the doc comment of _write_interim_continue. A real
   * response written on top of that corrupts the framing of the client
   * further. Against a peer that is already gone it is pointless. The
   * library therefore closes this connection, and it never tries the real
   * send. */
  /* The response of a handler never waits for a slow reader on this thread:
   * a full socket parks the connection instead; see _send_response_park. */
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
   * the copy of a nonzero carry-over meets an allocation failure. A leftover
   * pair of NULL and 0 can never fail; see their own doc comment. The code
   * retries with the carry-over dropped, so this connection still gets a
   * real stream that it can write to.
   *
   * A prepared left false here gates every response path below shut, because
   * each one checks prepared. The client then sees the connection drop with
   * zero response bytes, instead of a graceful error. For a streaming route
   * the handler still runs; see the use of body_unavailable below. It runs
   * against a stream that it can never read from, and
   * chttpsvr_req_stream_error() has no way to report why.
   *
   * The dropped carry-over bytes are lost either way. body_unavailable below
   * forces this request to a clean abort and close, so a retry without those
   * bytes loses nothing more. */
  bool body_unavailable = !prepared;
  if (body_unavailable) {
    prepared =
        conn->tls
            ? chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                                        conn->m_procs)
            : chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  }
  /* Every connection of this server is non-blocking, so each read and write
   * of this stream tries the I/O first and polls only when the socket is
   * empty or full; see chttp1_stream_t.fd_nonblocking. */
  stream.fd_nonblocking = true;
  _ccol_mem_free(conn->m_procs, conn->_carry_over);
  conn->_carry_over = NULL;
  conn->_carry_over_len = 0;

  /* The code loads response_write_timeout_ms once, up front. Two places need
   * it: the interim write of a buffered route below, and the real response
   * send further down; see chttp1_should_keep_alive and _send_response.
   *
   * The interim write goes out here, on the worker thread. It does not go
   * out from _conn_pump on the reactor thread. That placement is what makes
   * it correct by construction, with no extra guard. The _task_worker
   * function runs only for a request that already passed the route match of
   * _on_headers_complete. A rejected route never diverts at all, and goes
   * through _conn_dispatch_reject and _conn_reject_via_pool instead. No
   * !conn->req_rejected guard is therefore needed. A request that meets the
   * pool-full 503 path never reaches _task_worker either, so no spurious
   * "100 Continue" can come before a 503. A send from _conn_pump, as soon as
   * the headers finished and before the capacity check of ctpool_try_submit
   * ran, breaks both of those properties.
   *
   * The write never waits on the client: a full socket parks the
   * connection with the rest of the line, and the body is read once it is
   * out; see _write_interim_continue and _task_continue_interim. */
  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  /* This send is for buffered routes only. The handler of a buffered route
   * never runs until the library reads the whole body; see _drain_body
   * below. Such a handler therefore has no chance to reject the request
   * before that body arrives. That is true whether the library sends the
   * interim response here or later. A send now, instead of a deferral,
   * therefore costs this kind of route nothing.
   *
   * The handler of a streaming route is different. It can decide to reject a
   * request, over bad authentication or an unacceptable Content-Type for
   * example, without one call to chttpsvr_req_read(). That is exactly the
   * scenario that Expect: 100-continue, in RFC 7231 SS5.1.1, exists to make
   * cheap for the client. The server answers with a final status instead of
   * "100 Continue", and it never receives the body at all.
   *
   * A send here for every route, before the handler runs, defeats that in
   * silence. The library then tells the client to upload the body before a
   * streaming handler gets its chance to reject without a read. The library
   * therefore sends the interim response for a streaming
   * route lazily, from chttpsvr_req_read() itself. It sends it the first
   * time, if ever, that the handler asks to read the body.
   *
   * The code also skips the send for a buffered route when the message never
   * carried a body. In that case chttp1_parser_message_complete() is already
   * true here, because there is no Content-Length and no chunked
   * Transfer-Encoding. See the same guard in chttpsvr_req_read() for the
   * full reasoning. There is no body left to invite, so a "100 Continue"
   * here tells the client to upload one that was never coming. */
  /* See the options_star branch below. A declared body above the bound for
   * "OPTIONS *" is never read, so the client is never invited to send it. */
  bool options_star_body_refused =
      conn->options_star && chttp1_has_content_length(&conn->parser) &&
      chttp1_declared_content_length(&conn->parser) >
          (uint64_t)_CHTTPSVR_OPTIONS_STAR_MAX_BODY;
  if (prepared && !body_unavailable && conn->expects_continue &&
      !conn->matched_route->is_streaming && !options_star_body_refused &&
      !chttp1_parser_message_complete(&conn->parser)) {
    /* The line never waits for a client that does not read: a full socket
     * parks the connection, and the body is read once the line is out; see
     * _task_resume. */
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
  /* The code above can set conn->interim_write_failed. That means the
   * interim "100 Continue" write already left a line on the wire that may be
   * truncated. The library then closes this connection below, without
   * exception, and it never sends a response; see the
   * !conn->interim_write_failed check on `sent` further down. That happens
   * whatever the code does here. A drain of a body whose outcome nobody sees
   * would only tie this worker thread up for no gain. A run of the handler
   * for a request that can never be answered does the same. The code
   * therefore skips both in that case. */
  bool aborted = conn->interim_write_failed;
  if (!aborted && conn->options_star) {
    /* "OPTIONS *" is answered here, with no middleware and no handler: a 200
     * with a Content-Length of 0, which is what conn->resp already holds.
     * That is exactly the answer of Go's net/http server. A body of at most
     * _CHTTPSVR_OPTIONS_STAR_MAX_BODY bytes is read and discarded, and the
     * connection then follows the ordinary keep-alive rules. A declared
     * Content-Length above that bound is not read at all. Any failure to
     * read the body, a body above the bound included, still answers 200 but
     * closes the connection afterwards, because the next request no longer
     * has a known starting point. */
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
      /* The allocation failure above lost the body of this request. The
       * library must never call the handler with a stream that cannot
       * deliver that body. This holds for a buffered route and for a
       * streaming one. See the comment of body_unavailable above for why
       * this must not depend on the kind of route. The ordinary
       * body_err-from-_drain_body case below does depend on it. */
      body_err = ccol_not_enough_memory;
    } else if (prepared) {
      if (!conn->matched_route->is_streaming) {
        body_err = _drain_body(conn, &stream);
        if (body_err != _CHTTPSVR_BODY_PARKED) _mem_settle(conn);
      }
      /* For a streaming route, the handler pulls the body itself with
       * chttpsvr_req_read(). There is nothing to drain ahead of it here. */
    } else {
      /* Nothing reaches this branch in practice. The retry with NULL and 0
       * above can never fail; see the comment of body_unavailable. The code
       * fails closed here anyway, instead of an assumption. */
      body_err = ccol_not_enough_memory;
    }

    if (body_err != ccol_success &&
        (body_unavailable || !conn->matched_route->is_streaming)) {
      /* The body stopped arriving, or must wait for memory: this thread
       * gives the connection up and a later worker continues it. */
      if (body_err == _CHTTPSVR_BODY_PARKED) {
        _task_park_body(conn, srv, &stream, &req);
        return;
      }
      /* conn->body_malformed separates a client that sent a body which does
       * not match its own framing from a genuine server-side failure. Only
       * the second one is a 500. See the field comment of that flag. */
      conn->resp.status_code =
          (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
          : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
          : conn->body_malformed           ? CHTTP_STATUS_BAD_REQUEST
                                           : CHTTP_STATUS_INTERNAL_ERROR;
      aborted = true;
    } else {
      _chttpsvr_next(&req, &conn->resp);
      /* A streaming handler can stop its read before the natural end of the
       * body. It can also meet its own error, where chttpsvr_req_read
       * returns -1. Either way there is nothing more to drain. The fate of
       * the connection below, which is keep alive or close, already accounts
       * for a body that the library did not fully drain. It does so through
       * the message-completion check of chttp1_should_keep_alive. */
    }
  }

  _task_finish(conn, srv, &stream, &req, prepared, aborted, write_timeout_ms);
}

/* Continues a connection that the reactor handed back after a park; see
 * _conn_resume_parked. A parked body goes on with _drain_body from where it
 * stopped, and the request then ends exactly as in _task_worker, through
 * _task_finish. A parked response goes on with _task_continue_write. The
 * stream starts empty: a body reader parks only with its carry-over empty,
 * and the pipelined bytes of a parked response wait on the connection.
 *
 * The limits of a parked connection are judged again here before anything
 * else, because the sweep looks only once a second: a client that stalled
 * past a limit and then became ready meets the same answer that the sweep
 * would have given it, a 408 for a body and a close for a response. */
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
  /* The limits are judged at the claim, which is when the connection became
   * ready, and not after its wait in the queue of the pool. */
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

/* Reads the rest of the body of a buffered route, which parked or which
 * waited for its "100 Continue" line to go out, runs the handler, and ends
 * the request through _task_finish, exactly as _task_worker does. stream is
 * prepared and owned by the caller until this call; expired says that a
 * limit of the park ran out, which ends the request with 408. */
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
  /* The body no longer arrives: the next wait of this connection, for the
   * headers of a pipelined request or for room to write, wakes on its first
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

/* Continues a parked response; see _send_response_park. A socket that is
 * full again parks the connection again. A finished response ends through
 * _task_tail with the keep-alive decision that the first write recorded, and
 * the pipelined bytes of a next request wait in the carry-over of the
 * connection, where _task_tail looks for them. */
static void _task_continue_write(chttpsvr_conn_t *conn, struct chttpserver *srv,
                                 chttp1_stream_t *stream, bool expired) {
  stream->write_nonblocking = true;
  int r = expired ? 0 : _send_response_continue(stream, conn);
  if (r < 0) {
    chttp1_stream_release(stream);
    _conn_park_write(conn, srv);
    return;
  }
  bool keep_alive = r == 1 && conn->wp_keep_alive;
  _conn_end_parked_write(conn);
  if (conn->wp_reject) {
    /* The courtesy response of a rejection is out, or given up. It ends as
     * _conn_reject_and_close ends one that went out at once. */
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

/* Continues a buffered request whose "100 Continue" line met a full socket;
 * see _write_interim_continue. The body bytes that came with the headers wait
 * in the carry-over and go into the stream first. Once the line is out, the
 * body is read and the request ends exactly as in _task_worker. A line that
 * cannot be finished, because a limit of the write ran out or the write
 * failed, closes the connection without a response: some of it may already
 * be on the wire. */
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
    /* A copy of a carry-over that is not empty failed. A stream with no
     * carry-over cannot fail, and the request then ends below without its
     * body. */
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
    /* The socket is full again. The carry-over goes back on the
     * connection for the next attempt. */
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
  /* The deadline and the rate of the write end with the line, and the body
   * read measures its own. */
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

/* The minimum gap, in seconds, between two accept-failure log lines that
 * follow each other for the same server. See the field comment of
 * last_accept_err_log on struct chttpserver for why this exists at all. */
#define _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC 5
/* The most connections that one dispatch of the listener accepts before it
 * returns to the reactor; see _listener_on_readable_impl. */
#define _CHTTPSVR_ACCEPT_BATCH_MAX 64u
/* Two classes of failure pause the listener registration of srv and return
 * at once. The first is an accept4() failure from resource exhaustion, which
 * is EMFILE, ENFILE, ENOBUFS or ENOMEM. The second is an allocation failure
 * after accept4(), from the _ccol_mem_calloc of _conn_create or from an
 * internal allocation of ctls_conn_create_server. The library pauses instead
 * of a sleep on the calling thread before a retry; see
 * _listener_pause_for_resource_pressure and its two call sites below.
 *
 * Such a condition can persist. The process or the system can genuinely run
 * out of file descriptors under a sustained flood of connections with a
 * modest ulimit -n. A custom, bounded ccol_memmgmt_procs_t that
 * ccol_create_chttpsvr_mp installed can exhaust itself even while the system
 * still has memory. The level-triggered epoll of the reactor then observes
 * the listen backlog, which is still not empty, and dispatches this same
 * handler again and again. No progress is possible until something outside
 * frees the resource.
 *
 * A synchronous sleep in that dispatch does bound its OWN CPU cost. But it
 * still blocks the one shared reactor thread of the process for the whole
 * sleep; see the default of chttpsvr_set_engine_num_reactor_threads. It does
 * that on every re-dispatch, for as long as the condition lasts. It starves
 * every OTHER connection on every OTHER server that shares that one reactor
 * thread. The capacity-pause case of max_connections, a few lines up in the
 * same loop, already avoids exactly this shape. It does so with a pause
 * instead of a block.
 *
 * The sweep thread of the idle timeout resumes the listener. It checks every
 * server on every tick, without exception; see
 * _listener_resume_if_resource_pressure_cleared. The recovery therefore
 * takes at most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS. That is an acceptable
 * trade, for the same documented reason as the resume latency of
 * max_connections. The doc comments of chttpsvr_config_t never promise
 * recovery from a resource-exhaustion condition in under a second. It is
 * also far better than a block of the shared reactor thread for as long as
 * the condition lasts. */

/* Returns true for an accept4() failure that reflects a problem with one
 * specific connection that was already pending. The kernel reported that
 * problem into the error state of the listen socket before this call ran. It
 * is not a problem with the listener itself. An immediate retry is both safe
 * and the conventional handling for this class. It lets the accept loop try
 * the next pending connection in the backlog at once, instead of a wait for
 * a fresh epoll dispatch. See the BUGS section of the accept(2) man page. It
 * documents that Linux passes a pending per-connection network error through
 * accept() this way, and it recommends a caller treat that as temporary.
 *
 * ECONNABORTED is the one such error that POSIX itself documents. EPROTO,
 * ENETDOWN, ENETUNREACH, ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH and
 * EOPNOTSUPP are the exact Linux-specific pending-network-error codes that
 * the ERRORS section of that page names for accept(2). It names them word
 * for word: "In the case of TCP/IP, these are ENETDOWN, EPROTO,
 * ENOPROTOOPT, EHOSTDOWN, ENONET, EHOSTUNREACH, EOPNOTSUPP, and
 * ENETUNREACH".
 *
 * EPERM means "Firewall rules forbid connection", per that same ERRORS
 * section. It has the same shape as ECONNABORTED: a rejection of one
 * connection. Only its source differs, because a firewall rule causes it
 * instead of the peer. That is a realistic condition, and not an adversarial
 * one. It reaches any server behind rules that rate-limit connections. It
 * also reaches a server behind rules that a fail2ban-style tool installs as
 * REJECT rather than DROP.
 *
 * ETIMEDOUT, ENOSR, ESOCKTNOSUPPORT and EPROTONOSUPPORT complete the
 * sentence of that same man page which reads "in addition, network errors
 * for the new socket... may be returned; various Linux kernels can return
 * other errors such as...". That sentence comes right after its EPERM and
 * EPROTO entries. Those codes belong to the same per-pending-connection
 * category, and they are not a problem at the level of the listener. */
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

/* Returns true for an accept4() failure that reflects the exhaustion of a
 * shared resource. That resource belongs to the whole process or to the
 * whole system. Such a failure is not a problem with one specific pending
 * connection. An immediate retry is likely to fail in the same way, until
 * something ELSE in the process or the system frees that resource.
 *
 * This is a pure classifier with its own name and its own tests. The
 * white-box test accessor of this file uses it directly, and so do the tests
 * that exercise it. It exists for its documentation and diagnostic value. It
 * does NOT gate the decision of _listener_on_readable_impl to call
 * _listener_pause_for_resource_pressure. That function pauses for every
 * errno that is not transient and that reaches it, without exception. An
 * explicit allow-list there leaves an errno that nothing classifies, but
 * that persists, open to the exact busy loop that this mechanism prevents.
 * See the comment of that call site for the reason. */
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
/* These are white-box test hooks. They expose the two pure errno classifiers
 * above directly. Take a test that really triggers an EMFILE, an
 * ECONNABORTED or another such error. It must manipulate the fd limits of
 * the whole process, or connection state inside the kernel. That depends on
 * the environment. It is also out of proportion for a simple classification
 * of an integer that always gives the same answer. The guard keeps both
 * symbols out of a production build, in the same way as every other
 * white-box helper in this file. */
bool _chttpsvr_accept_errno_is_transient_for_tests(int e) {
  return _accept_errno_is_transient(e);
}
bool _chttpsvr_accept_errno_is_resource_exhaustion_for_tests(int e) {
  return _accept_errno_is_resource_exhaustion(e);
}
#endif /* RUNNING_UNIT_TESTS */

/* Returns true at most once every _CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC
 * seconds for each server, and false otherwise. On a true it also marks this
 * instant as the new "last logged" one.
 *
 * This is the shared rate-limit gate for every diagnostic of this accept
 * loop. Those diagnostics are about a listener-level condition that persists
 * and that nobody can act on at once. There are two of them. The first is an
 * unexpected accept4() failure. The second is an allocation failure for a
 * connection object or a TLS object that the loop finds right after a
 * successful accept4(). Both carry the same log-flooding hazard. Each can
 * trigger again on every accept4() attempt, for as long as the underlying
 * condition lasts.
 *
 * The two fields that this function reads and writes need no synchronization
 * of their own. See the field comment of last_accept_err_log on struct
 * chttpserver for the reason. */
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

/* The text of err, through the thread-safe strerror_r(3). glibc with
 * _GNU_SOURCE declares the GNU form, which returns the text and may not use
 * buf; every other C library declares the POSIX form, which fills buf and
 * returns an error number. */
static const char *_errno_text(int err, char *buf, size_t len) {
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  return strerror_r(err, buf, len);
#else
  if (strerror_r(err, buf, len) != 0) snprintf(buf, len, "error %d", err);
  return buf;
#endif
}

/* Logs an unexpected accept4() failure through the engine logger.
 * _listener_accept_err_log_gate rate-limits it. The line notes whether
 * _accept_errno_is_resource_exhaustion recognizes err as one of the
 * well-documented EMFILE, ENFILE, ENOBUFS or ENOMEM cases.
 *
 * That note is a diagnostic aid for whoever reads the log, and nothing more.
 * An operator can act on "resource exhaustion" by a raise of the ulimits or
 * by a free of memory. Anything else here reflects a listener-level
 * condition that this loop does not expect to see at all. Such a condition
 * is worth an investigation as a possible bug, and not as routine overload.
 * The note has no effect on the handling of this loop. The one call site
 * below pauses for every such errno, without exception, whatever this
 * classifier says. */
static void _listener_log_accept_err_rate_limited(struct chttpserver *srv,
                                                  int err) {
  if (!_listener_accept_err_log_gate(srv)) return;
  /* The code calls strerror_r, and not strerror. The reactor of this file is
   * genuinely multi-threaded; see
   * chttpsvr_set_engine_num_reactor_threads. Plain strerror() need not be
   * thread-safe. POSIX permits an implementation to return a pointer into a
   * shared static buffer. A concurrent strerror() call on another thread can
   * then overwrite that buffer while the first thread still reads it. The
   * listeners of two different servers can dispatch this exact call on two
   * different reactor threads at the same moment.
   *
   * _errno_text() wraps whichever strerror_r the C library declares. */
  char errbuf[128];
  _SRV_ENGINE_LOG(ccol_log_warn, "accept() failed errno=%d (%s)%s", err,
                  _errno_text(err, errbuf, sizeof(errbuf)),
                  _accept_errno_is_resource_exhaustion(err)
                      ? " [resource exhaustion]"
                      : "");
}

#ifdef RUNNING_UNIT_TESTS
/* This is white-box test instrumentation and nothing else. It counts how
 * many times this accept loop paused the listener of a server after an
 * allocation failure that followed an accept4(). Such a failure is a NULL
 * from _conn_create or from ctls_conn_create_server. The counter covers the
 * whole process.
 *
 * A test reads the difference across its own window, while a custom
 * ccol_memmgmt_procs_t forces every such allocation to fail. That confirms
 * that the pause path really runs. g_listener_dispatch_count_for_tests
 * already established this style for the pause and resume path of
 * max_connections. The guard keeps this symbol out of a production build. */
static _Atomic size_t g_listener_alloc_failure_pause_count_for_tests = 0;
size_t _chttpsvr_listener_alloc_failure_pause_count_for_tests(void) {
  return atomic_load(&g_listener_alloc_failure_pause_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Pauses the listener registration of srv. It does so in answer to an
 * accept4() errno from resource exhaustion, or to an allocation failure that
 * followed an accept4(). It then sets
 * listener_paused_for_resource_pressure; see the comment of that field. The
 * sweep thread of the idle timeout reads that flag in
 * _listener_resume_if_resource_pressure_cleared, and it knows from the flag
 * to retry the listener later.
 *
 * The function is a no-op in two cases. The first is a srv with no live
 * listener registration to pause, where lreg is NULL. The second is a
 * ccol_event_loop_pause that fails. Its documented contract says that a
 * failure can only mean that another thread removed lreg, for example a
 * concurrent chttpsvr_stop(). listener_dispatch_pins exists to prevent
 * exactly that for as long as this dispatch runs, so nothing should reach
 * that case. If something ever does, there is no paused registration to
 * track either way.
 *
 * The code sets the flag only once the pause succeeds. It never sets it
 * ahead of the attempt. The sweep therefore never wastes a resume call on a
 * registration that never got paused. */
static void _listener_pause_for_resource_pressure(struct chttpserver *srv) {
  ccol_mutex_lock(srv->mutex);
  ccol_event_reg lreg = srv->listen_reg;
  ccol_mutex_unlock(srv->mutex);
  if (!lreg) return;
  if (ccol_event_loop_pause(srv_engine_bundler.reactor, lreg) == ccol_success)
    atomic_store(&srv->listener_paused_for_resource_pressure, true);
}

/* Logs an allocation failure that the loop finds right after a successful
 * accept4(). That failure is a NULL from the _ccol_mem_calloc of
 * _conn_create, or from an internal allocation of ctls_conn_create_server.
 * The log line is rate-limited, and it shares the gate and the fields of the
 * accept4()-failure logger above. The function then pauses the listener of
 * srv, exactly as _listener_pause_for_resource_pressure describes. It does
 * not retry accept4() at once. This follows the resource-exhaustion handling
 * that this same loop already applies to accept4() itself; see the section
 * comment "LISTENER: ACCEPT + SOCKET OPTIONS" above.
 *
 * Without this pause, a sustained allocation-failure condition spins this
 * loop forever. The most likely such condition is a custom, bounded
 * ccol_memmgmt_procs_t that ccol_create_chttpsvr_mp installed, because it
 * can exhaust itself while the system still has memory. A genuine
 * system-wide out-of-memory condition is less common but possible. Combine
 * that with connections that keep arriving in the listen backlog. The loop
 * then accepts, fails to allocate at once, closes, and repeats. Every
 * epoll_wait dispatches it again, and nothing makes progress. That is an
 * unbounded busy loop under a condition that persists. This file already
 * treats the same shape as a real bug. That is the EMFILE, ENFILE, ENOBUFS
 * and ENOMEM handling of accept4(), a few lines down in this same loop. */
static void _listener_log_and_pause_for_alloc_failure(struct chttpserver *srv,
                                                      const char *what) {
  if (_listener_accept_err_log_gate(srv))
    _SRV_ENGINE_LOG(ccol_log_warn, "%s failed after accept()", what);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_alloc_failure_pause_count_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  _listener_pause_for_resource_pressure(srv);
}

#ifdef RUNNING_UNIT_TESTS
/* This is white-box test instrumentation and nothing else. It counts the
 * accepted connections whose socket buffers _apply_accepted_socket_options
 * raised, across the whole process. */
static _Atomic size_t g_accepted_sockbuf_raise_count_for_tests = 0;
size_t _chttpsvr_accepted_sockbuf_raise_count_for_tests(void) {
  return atomic_load(&g_accepted_sockbuf_raise_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

static void _apply_accepted_socket_options(int fd, bool is_unix,
                                           bool enable_keepalive) {
  /* The SOCK_NONBLOCK flag of accept4() already guarantees non-blocking
   * mode, and it does so atomically. The one call site that produces fd
   * passes that flag; see _listener_on_readable. No separate fcntl(F_SETFL)
   * call is needed here, and none is wanted. */
  int one = 1;
  if (!is_unix) {
    /* TCP_NODELAY is an option at the IPPROTO_TCP level, and the usual
     * purpose of SO_KEEPALIVE does not apply to AF_UNIX.
     *
     * A TCP connection keeps the buffer sizes of the kernel. An explicit
     * SO_SNDBUF or SO_RCVBUF on a TCP socket locks that direction at the
     * given size and turns off the buffer autotuning of Linux for it (see
     * tcp(7)). Autotuning grows a buffer to the bandwidth-delay product of
     * the path, far past any fixed size that suits a local link, and a lock
     * caps the throughput of every connection with a long round trip. */
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (enable_keepalive)
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return;
  }
  /* An AF_UNIX stream socket has no autotuning, and some platforms and
   * containers size its default buffers small. A unix:// connection
   * therefore gets both buffers raised to at least 128 KiB, so that it does
   * not end up with meaningfully smaller buffers than a TCP client of the
   * same server. */
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
/* This is a forward declaration. The definition comes much later in this
 * file, with the rest of the slot table machinery for a chttpsvr handle. The
 * declaration is needed here for the server-scoped accept-errno force hook
 * below. That hook resolves a chttpsvr handle to its raw struct
 * chttpserver* for a comparison. The declaration saves a move of that whole
 * slot-table section up. */
struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h);

/* This is white-box test instrumentation and nothing else. It counts how
 * many times the library dispatched _listener_on_readable, across the whole
 * process, since the counter started at zero.
 *
 * A test reads the difference across its own window. During that window the
 * test holds a server at the capacity of max_connections, and nothing else
 * in the process connects or closes. The difference then tells a correctly
 * paused listener apart from one that busy-loops. A paused listener stays at
 * a small, fixed handful of dispatches. A listener that busy-loops grows
 * into the thousands inside a fraction of a second. Level-triggered epoll
 * re-reports a listener whose accept backlog is not empty on every
 * epoll_wait call. This signal is far more precise than a measurement of
 * wall-clock CPU time, and it depends far less on the environment.
 *
 * The guard keeps this symbol and this counter out of a production build.
 * That matches every other white-box helper in this file. */
static _Atomic size_t g_listener_dispatch_count_for_tests = 0;
size_t _chttpsvr_listener_dispatch_count_for_tests(void) {
  return atomic_load(&g_listener_dispatch_count_for_tests);
}

/* The largest number of connections that one dispatch of the accept loop
 * accepted, across every server, since the last reset. A test compares it
 * against _CHTTPSVR_ACCEPT_BATCH_MAX. */
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

/* This is white-box test instrumentation and nothing else.
 * g_force_next_accept_errno_for_tests_srv names one server. The code
 * compares it by raw pointer, which _chttpsvr_resolve_for_tests resolved
 * once at arm time. It can match the server that is about to dispatch. The
 * very next accept4() call in the loop of _listener_on_readable_impl for
 * THAT server then has its result discarded. A real, successful accept4()
 * connection then simply closes unused, exactly as if the loop had never
 * accepted it. The code replaces the result with a simulated failure whose
 * errno is the paired _val field. It then resets both fields, so only that
 * one call, for that one server, is ever affected.
 *
 * The scope is one specific server on purpose. The alternative is "the very
 * next accept4() dispatch anywhere in the process". This reactor is shared
 * across the whole process. An unrelated dispatch for a different chttpsvr
 * can run at the same time. It can therefore consume the forced errno before
 * the intended connection of the test ever reaches it. One such server is
 * the long-lived shared fixture server of this test binary, which can
 * service an unrelated connection at that same moment. That happens in
 * practice under the scheduling of valgrind. It produces a false pass or a
 * false failure that comes and goes with the timing of the environment. That
 * outcome has nothing to do with the behavior any such test is about.
 *
 * The hook exists because a REAL accept4() failure is hard to produce for an
 * errno outside the named cases of _accept_errno_is_transient and
 * _accept_errno_is_resource_exhaustion. Those other errno values are EBADF,
 * EINVAL, ENOTSOCK, EFAULT and similar. A test that triggers one of them
 * must manipulate the fd table of the whole process, or socket state inside
 * the kernel. That depends on the environment and is out of proportion. See
 * the comment of _chttpsvr_accept_errno_is_transient_for_tests, which gives
 * the same reasoning for EMFILE, ECONNABORTED and the rest.
 *
 * This hook lets a test verify the handling of such an errno by the LOOP
 * itself. The loop pauses, it does not busy-loop, and it resumes later. The
 * errno does not have to be real for that. The guard keeps this symbol and
 * its setter out of a production build. */
static _Atomic(struct chttpserver *) g_force_next_accept_errno_for_tests_srv =
    NULL;
static _Atomic int g_force_next_accept_errno_for_tests_val = 0;
void _chttpsvr_force_next_accept_errno_for_tests(chttpsvr h, int errno_val) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  atomic_store(&g_force_next_accept_errno_for_tests_val, errno_val);
  atomic_store(&g_force_next_accept_errno_for_tests_srv, raw);
}
#endif /* RUNNING_UNIT_TESTS */

/* This is the real body of the accept loop. The thin wrapper below pins it
 * for its whole duration, and it passes the listener fd that it checked
 * against srv->listen_fd under that pin; see the field comment of
 * listener_dispatch_pins. The pin keeps that fd open until this returns.
 *
 * The body sits in its own function, instead of a pin and unpin pair wrapped
 * around it inline. This function has several early return points: the
 * capacity pause, an EWOULDBLOCK, an unexpected accept4() failure and more.
 * A thin caller funnels every one of them back through one unpin site. That
 * is simpler, and it makes an error less likely, than a matching unpin call
 * threaded into each return point. */
static void _listener_on_readable_impl(struct chttpserver *srv, int lfd) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_dispatch_count_for_tests, 1);
  size_t accepted_for_tests = 0;
#endif /* RUNNING_UNIT_TESTS */

  /* One dispatch accepts at most _CHTTPSVR_ACCEPT_BATCH_MAX connections and
   * then returns to the reactor. The listener registration is
   * level-triggered, so a backlog that is still not empty is dispatched
   * again on the next epoll_wait, after the events of every other ready
   * connection in that batch. Without the bound, a flood of connects keeps
   * the one reactor thread inside this loop, and every established
   * connection waits behind it. Each iteration counts, including a retry
   * after EINTR or a transient per-connection error. */
  for (unsigned batch = 0; batch < _CHTTPSVR_ACCEPT_BATCH_MAX; batch++) {
#ifdef RUNNING_UNIT_TESTS
    _listener_note_accepts_for_tests(accepted_for_tests);
#endif /* RUNNING_UNIT_TESTS */
    size_t cap = atomic_load(&srv->max_connections);
    if (cap && atomic_load(&srv->current_connections) >= cap) {
      /* The server is at capacity. The code pauses the reactor registration
       * of the listener. It does not merely return without another accept4()
       * call. Level-triggered epoll reports a listen socket whose accept
       * backlog is not empty as ready on every epoll_wait call, and not only
       * once. A plain return here therefore makes the reactor dispatch this
       * exact handler again at once, and again after that. No progress is
       * possible until a slot frees.
       *
       * The CPU ticks in /proc/<pid>/stat measure this directly. Without the
       * pause, a listener left at capacity pins a full CPU core at about 100
       * percent for as long as the server stays there. It does nothing but
       * re-dispatch this handler over and over. That is a real and severe
       * waste of resources.
       *
       * That waste happens whether or not the same epoll_wait batch also
       * services the events of other connections. It generally does service
       * them, because epoll_wait returns every fd that is ready together,
       * and not the listener alone. This pause exists to remove the wasted
       * CPU time, and not because other connections starve.
       *
       * The sweep thread of the idle timeout resumes the listener. It checks
       * every server on every tick, without exception; see
       * _listener_resume_if_capacity_freed. The resume therefore comes at
       * most _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS after a slot frees. */
      ccol_mutex_lock(srv->mutex);
      ccol_event_reg lreg = srv->listen_reg;
      ccol_mutex_unlock(srv->mutex);
      if (lreg && ccol_event_loop_pause(srv_engine_bundler.reactor, lreg) ==
                      ccol_success) {
        /* The code checks the capacity again AFTER the pause, and not only
         * before it. This closes a real time-of-check to time-of-use race
         * against the resume check of the sweep thread.
         *
         * Take a capacity that already freed. The
         * _listener_resume_if_capacity_freed call of a sweep tick then runs
         * at the same time as the pause above, and just ahead of it. That
         * resume finds nothing paused yet to resume. It is a harmless no-op
         * at that moment. But nothing else is left to un-pause this
         * listener until the NEXT sweep tick, which is up to another
         * _CHTTPSVR_IDLE_SWEEP_INTERVAL_MS away.
         *
         * The recheck here runs strictly after the pause takes effect, and
         * it resumes at once when the capacity freed since. That closes the
         * gap to zero, instead of a bound of one more tick.
         *
         * The code checks the return value of the pause, and does not fire
         * and forget. Every other ccol_event_loop_pause, _modify, _add and
         * _resume call site in this file checks it too. A return that is not
         * a success can only mean that another thread removed lreg. That is
         * the only documented failure mode for a reg that is not NULL.
         * listener_dispatch_pins exists to prevent exactly that for as long
         * as this dispatch runs. Nothing should reach that case. If
         * something ever does, no paused registration is left to resume. A
         * skip of the whole recheck and resume is then the correct answer,
         * rather than a ccol_event_loop_resume call on a reg that nothing
         * ever paused. */
        if (atomic_load(&srv->current_connections) < cap)
          ccol_event_loop_resume(srv_engine_bundler.reactor, lreg);
      }
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    /* The code calls accept4() with SOCK_NONBLOCK. It does not call accept()
     * and then a separate fcntl(F_SETFL). The fd that this produces goes
     * straight to the read() and ctls_conn_read() calls of the sole reactor
     * thread; see _conn_pump. Those calls must never block. accept4() makes
     * non-blocking mode part of the same atomic kernel operation that
     * creates the fd. It does not depend on a second, separate and unchecked
     * fcntl() call succeeding first.
     *
     * SOCK_CLOEXEC sits beside it for the same reason of atomicity, on the
     * other axis. The fd of every accepted connection is a live, open client
     * socket. Without SOCK_CLOEXEC, a child inherits that fd across any
     * fork() plus exec() that the embedding application does elsewhere in
     * this process while the server runs. A request handler that shells out
     * does that, and so does an unrelated spawn of a subprocess. The fd
     * leaks into a child process that has no business holding it open. This
     * matches the established convention of this codebase for every other fd
     * that it creates. Those are the O_CLOEXEC file opens of clogger.c, and
     * the EFD_CLOEXEC eventfd() calls of cthreadcomm.c. */
    int cfd = ccol_accept_nb(lfd, (struct sockaddr *)&ss, &slen);
    int accept_errno = cfd < 0 ? errno : 0;
#ifdef RUNNING_UNIT_TESTS
    /* See the comment of g_force_next_accept_errno_for_tests_srv. The code
     * consumes the forced errno, and resets both fields, only when the srv
     * of this dispatch is exactly the one that a test armed. A concurrent
     * dispatch of an unrelated server can therefore never steal a forced
     * errno that belongs to a different, specific server under test. The
     * long-lived shared fixture server of this test binary is one such
     * unrelated server. */
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
      /* This is a problem with one specific connection that was already
       * pending. It is not a problem with the listener itself. The code
       * retries at once, so that the loop tries the next backlog entry right
       * away, when there is one. It does not wait for a fresh epoll dispatch
       * to notice that the backlog is still not empty. */
      if (_accept_errno_is_transient(accept_errno)) continue;
      _listener_log_accept_err_rate_limited(srv, accept_errno);
      /* See the section comment "LISTENER: ACCEPT + SOCKET OPTIONS" above
       * for why the code pauses here instead of a sleep. Without the pause,
       * an exhaustion condition that persists busy-loops the sole shared
       * reactor thread. The listen backlog stays not empty, because nothing
       * was ever accepted. The level-triggered epoll of the reactor
       * therefore dispatches this exact handler again at once, and nothing
       * makes progress.
       *
       * The pause is NOT gated on _accept_errno_is_resource_exhaustion, on
       * purpose. The switch of that helper names only the well-documented
       * EMFILE, ENFILE, ENOBUFS and ENOMEM cases. accept(2) documents
       * several more, among them EBADF, EINVAL, ENOTSOCK and EFAULT. This
       * loop expects none of those in practice against a listener that it
       * owns and manages itself.
       *
       * Any other errno that reaches this point is, by construction, neither
       * EWOULDBLOCK, EAGAIN nor EINTR. It is also not one of the
       * per-connection cases of _accept_errno_is_transient. A persistent
       * occurrence of ANY such errno therefore busy-loops this thread in
       * exactly the same way as an unhandled resource-exhaustion errno. No
       * errno value makes a return here without a pause the right outcome.
       *
       * A pause has no downside even for a one-off condition that clears
       * itself, because the very next sweep tick resumes the listener either
       * way. The code therefore pauses for every errno that reaches this
       * point, without exception, instead of a match against an explicit
       * allow-list. */
      _listener_pause_for_resource_pressure(srv);
      return;
    }
    atomic_fetch_add(&srv->current_connections, 1);
#ifdef RUNNING_UNIT_TESTS
    accepted_for_tests++;
#endif /* RUNNING_UNIT_TESTS */
    _apply_accepted_socket_options(cfd, atomic_load(&srv->is_unix_socket),
                                   srv->enable_keepalive);

    ccol_call_once(srv_parser_bundler.once, _init_parser_settings);
    chttpsvr_conn_t *conn =
        _conn_create(srv, cfd, &srv_parser_bundler.settings);
    if (!conn) {
      close(cfd);
      atomic_fetch_sub(&srv->current_connections, 1);
      /* See the comment of _listener_log_and_pause_for_alloc_failure.
       * Without this pause, a sustained allocation-failure condition spins
       * this loop with no progress, while connections keep arriving. That is
       * the same hazard that the failure path of accept4(), a few lines up,
       * already guards against. */
      _listener_log_and_pause_for_alloc_failure(srv, "connection allocation");
      return;
    }

    if (srv->tls_ctx) {
      conn->tls = ctls_conn_create_server(srv->tls_ctx, cfd, conn, NULL);
      if (!conn->tls) {
        /* The code routes this through the shared _conn_free helper. That
         * helper closes the fd, decrements current_connections, and frees
         * every allocation that conn owns. The code does not write a
         * narrower equivalent of it by hand here. conn is safe to hand to
         * that helper at this point. Nothing added it to the idle list or
         * the diverted list. conn->tls is still NULL, because the creation
         * that just failed is what fills it.
         *
         * This also covers a future change. Such a change can make
         * _conn_create or _conn_reset_for_request allocate something for
         * every connection. The helper then frees it automatically. A
         * hand-written equivalent here leaks it in silence, through this one
         * narrow early-failure path that is easy to miss. */
        _conn_free(conn);
        /* See the comment of _listener_log_and_pause_for_alloc_failure. The
         * reasoning is the same as for the _conn_create failure above. It
         * applies here to the construction of the TLS connection object. */
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

/* ccol_event_loop_add registers this function as the on_readable callback of
 * the listener. It pins srv with listener_dispatch_pins; see the field
 * comment of that counter. The pin covers the whole call into
 * _listener_on_readable_impl, and it includes any listener pause that the
 * call performs. It releases the pin as the very last thing that this
 * function does, after that call fully returns. The memory of srv itself
 * stays valid for the whole call, before the pin as well, through the
 * srv->lifetime_refs reference that the listener registration holds.
 *
 * The code takes the pin under srv->mutex. That serializes the increment
 * against the clear of listen_fd in _chttpsvr_stop_internal and against the
 * wait of that function for this counter, which both run under the same
 * mutex. A bare atomic operation would race that check.
 *
 * A dispatch can start before a chttpsvr_stop and pin after it. The fd of
 * its registration is then no longer listen_fd: it is -1, or the fd of a
 * listener that a later chttpsvr_start created. Such a dispatch returns at
 * once. It must not accept on a listener that it was not registered for,
 * and it must not pause or log on behalf of a server that may already be in
 * its destroy. */
static void _listener_on_readable(ccol_event_loop loop, ccol_event_reg reg,
                                  ccol_selectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  struct chttpserver *srv = (struct chttpserver *)arg;

#ifdef RUNNING_UNIT_TESTS
  _listener_dispatch_race_hook_wait_if_armed(true);
#endif /* RUNNING_UNIT_TESTS */

  ccol_mutex_lock(srv->mutex);
  atomic_fetch_add(&srv->listener_dispatch_pins, 1);
  int lfd = atomic_load(&srv->listen_fd);
  ccol_mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _listener_dispatch_race_hook_wait_if_armed(false);
#endif /* RUNNING_UNIT_TESTS */

  if (lfd >= 0 && lfd == sel->fd) _listener_on_readable_impl(srv, lfd);

  ccol_mutex_lock(srv->mutex);
  atomic_fetch_sub(&srv->listener_dispatch_pins, 1);
  ccol_cond_var_broadcast(srv->resolve_cv);
  ccol_mutex_unlock(srv->mutex);
}

/* This is the on_removed handler of the listener registration. It fires
 * exactly once for each registration, and only once no dispatch of that
 * registration can still run; see the doc comment of ccol_event_loop_remove.
 * It releases the srv->lifetime_refs reference that chttpsvr_start took for
 * the registration, and it runs the final teardown of srv when that was the
 * last reference. */
static void _listener_on_removed(void *arg) {
  struct chttpserver *srv = (struct chttpserver *)arg;
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(srv);
}

/* ========================================================================== */
/*                    LISTEN SOCKET SETUP (TCP + Unix)                        */
/* ========================================================================== */

#define _CHTTPSVR_UNIX_PREFIX "unix://"

/* Removes the socket file of a unix:// listener, when the name still names
 * the socket that this server bound, and releases what ub holds. A file
 * that somebody else put at the path since stays. The window between the
 * check and the unlink is inherent: POSIX has no unlink by inode. */
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

/* Decides what to do with a file that already exists at the path of a
 * unix:// listener, before the bind. It returns true when the bind may go
 * on: nothing is there, or a stale socket that no process listens on any
 * more was there and is now removed. It returns false, with errno set, when
 * the start must fail and leave the file alone: the file is not a socket
 * (a start never deletes a regular file, a directory or anything else that
 * a typing mistake in the configuration can name), or a live listener
 * answers on it (EADDRINUSE). The probe is a non-blocking connect(2) from a
 * scratch socket: ECONNREFUSED is the answer of the kernel for a socket file
 * with no listener behind it. */
static bool _unix_path_prepare(const struct sockaddr_un *addr) {
  struct stat st;
  if (lstat(addr->sun_path, &st) != 0) return errno == ENOENT;
  if (!S_ISSOCK(st.st_mode)) {
    _SRV_ENGINE_LOG(ccol_log_error,
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
    /* Stale. It is removed only while the name still names the same
     * socket file that the probe saw. */
    struct stat again;
    if (lstat(addr->sun_path, &again) == 0 && S_ISSOCK(again.st_mode) &&
        again.st_dev == st.st_dev && again.st_ino == st.st_ino)
      unlink(addr->sun_path);
    return true;
  }
  if (e == ENOENT) return true; /* removed since the lstat */
  if (e == 0 || e == EAGAIN || e == EINPROGRESS) {
    _SRV_ENGINE_LOG(ccol_log_error,
                    "unix socket path %s: another server already listens "
                    "on it",
                    addr->sun_path);
    e = EADDRINUSE;
  }
  errno = e;
  return false;
}

/* The flags of the directory descriptor of a unix:// path. It only names the
 * directory for the *at() calls, so O_PATH is enough where the system has
 * it, and it needs no read permission; macOS has none. */
#if defined(O_PATH)
#define _UNIX_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define _UNIX_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

/* Opens the directory that holds path, for _chttpsvr_unix_bind_t.dir_fd,
 * and sets *base to the last component of path. */
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

static int _make_unix_listen_socket(const char *path, _chttpsvr_unix_bind_t *ub,
                                    ccol_memmgmt_procs_t *mp) {
  if (!*path || strlen(path) >= sizeof(((struct sockaddr_un *)0)->sun_path))
    return -1;
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

  /* SOCK_CLOEXEC stops a child from inheriting the listening socket itself.
   * Such a child comes from a fork() plus exec() that the embedding process
   * performs elsewhere while this server runs. See the same reasoning at
   * the accept4() call site in _listener_on_readable. */
  int fd = -1;
  if (_unix_path_prepare(&addr)) fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    close(b.dir_fd);
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  struct stat st;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    /* EADDRINUSE here means that another process created the file after
     * the check above. It is left alone. */
    close(fd);
    close(b.dir_fd);
    _ccol_mem_free(mp, b.path);
    return -1;
  }
  if (fstatat(b.dir_fd, b.base, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(st.st_mode)) {
    /* The socket that bind() just created cannot be identified, so nothing
     * could ever remove it safely. It is left as a stale socket file, which
     * the next start at this path replaces. */
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
 * IPV6_V6ONLY default of the system, and 0 or 1 to set it. It returns the fd,
 * or -1 with errno set. */
static int _bind_tcp(int family, const struct sockaddr *sa, socklen_t salen,
                     bool enable_reuseport, int v6only) {
  /* SOCK_CLOEXEC sits beside SOCK_NONBLOCK for the same reason as in the
   * AF_UNIX branch and at the accept4() call in _listener_on_readable.
   * Without it, this listening socket leaks into any child process that a
   * fork() plus exec() elsewhere in this application produces. */
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
#ifdef TCP_FASTOPEN
  int qlen = 128;
  setsockopt(fd, IPPROTO_TCP, TCP_FASTOPEN, &qlen, sizeof(qlen));
#endif
  if (bind(fd, sa, salen) != 0 || listen(fd, SOMAXCONN) != 0) {
    int e = errno;
    close(fd);
    errno = e;
    return -1;
  }
  return fd;
}

/* Opens the listener that cfg->host names. The rules follow the listener of
 * the Go net package:
 *
 *  - NULL or "": one dual-stack socket on [::] with IPV6_V6ONLY off, which
 *    also takes IPv4 clients through v4-mapped addresses (ipv6_only turns
 *    it into an IPv6-only listener). A host with no IPv6 gets 0.0.0.0.
 *  - A literal IPv4 or IPv6 address: exactly that address. An address in
 *    brackets, as in "[::1]", is the address inside them; a name in
 *    brackets is refused.
 *  - A host name: its first IPv4 address when it has one, else its first
 *    IPv6 address; with ipv6_only, its first IPv6 address alone.
 *    "localhost" therefore listens on 127.0.0.1. Exactly one address is
 *    tried, so a failure, such as a port that another socket holds on it,
 *    is the answer and its errno is kept; another address of the name never
 *    hides it.
 *  - "unix://path": a Unix domain socket at path.
 *
 * *is_unix reports which kind it opened, and *ub holds the socket file of a
 * unix:// listener. On a failure errno holds the reason; a name that does
 * not resolve is logged with the message of the resolver and gives
 * EADDRNOTAVAIL. */
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
    /* No IPv6 on this host: the socket or the bind of [::] fails with one
     * of these. Any other failure, such as a port in use, is the answer. */
    if (errno != EAFNOSUPPORT && errno != EADDRNOTAVAIL &&
        errno != EPROTONOSUPPORT)
      return -1;
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_port = htons(port);
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    return _bind_tcp(AF_INET, (struct sockaddr *)&a4, sizeof(a4),
                     enable_reuseport, -1);
  }

  /* A bracketed host, the form that a URL gives an IP literal (RFC 3986
   * IP-literal), names the address inside the brackets, and only an address
   * is accepted there, never a name. */
  char unbracketed[256];
  size_t host_len = strlen(host);
  bool bracketed = host[0] == '[';
  if (bracketed) {
    if (host_len < 3 || host[host_len - 1] != ']' ||
        host_len - 2 >= sizeof(unbracketed)) {
      _SRV_ENGINE_LOG(ccol_log_error, "listen host \"%s\" is malformed", host);
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
      _SRV_ENGINE_LOG(ccol_log_error,
                      "listen host \"%s\" does not resolve: errno=%d", host, e);
    else
      _SRV_ENGINE_LOG(ccol_log_error, "listen host \"%s\" does not resolve: %s",
                      host, gai != 0 ? gai_strerror(gai) : "no address");
    if (res) freeaddrinfo(res);
    errno = EADDRNOTAVAIL;
    return -1;
  }
  /* A literal resolves to itself alone. A name can resolve to several
   * addresses of both families, in the order of the resolver, which puts
   * ::1 first for "localhost" on most systems. The first IPv4 address is
   * the one, else the first IPv6 address, as net.Listen of Go chooses. An
   * IPv6-only listener takes the first IPv6 address instead, and an IPv4
   * address only when the name has no IPv6 one, where ipv6_only has no
   * effect. */
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

/* ========================================================================== */
/*                    ROUTER SHELL REGISTRY                                   */
/* ========================================================================== */

/* Every chttpsvr_router has a "shell". The shell is the struct itself: the
 * owner, srv and m_procs fields, plus the routes, mw_head, mw_tail and
 * mw_count bookkeeping fields. The CONTENTS are what the shell points to:
 * the prefix, the mw list nodes, the routes array and the data of each
 * route. The code allocates every shell here, through the plain default
 * allocator of the process. It never allocates one through the possibly
 * custom ccol_memmgmt_procs_t of a server. _destroy_router never frees a
 * shell at all.
 *
 * This exists because a chttpsvr_router* that a caller holds has no
 * indirection of its own. A chttpsvr handle does have one. Its generation
 * lives in the handle VALUE, and never in memory that something can free out
 * from under a reader. chttpsvr_router_on, _on_stream and _use must read
 * router->owner to learn whether the owning server of that router is still
 * alive. That read is itself a dereference of `router`.
 *
 * A resolve of router->owner through the ordinary chttpsvr slot table
 * protects everything AFTER the resolve succeeds. srv, routes and mw_head
 * cannot be freed while any resolve is pinned. The pending_resolve_count
 * wait of __chttpsvr_destroy guarantees that, because it runs before that
 * function frees one byte of the contents of any router. But nothing
 * protects the read of router->owner itself. That read happens before any
 * pin exists, and valgrind catches it as a real use-after-free.
 *
 * No narrower remedy closes it, because none of them touches the lifetime of
 * the shell. A resolve of router->owner alone leaves the dereference that
 * must come before that resolve unprotected. Take a reader that starts only
 * after the free is already complete, and not merely started. A process-wide
 * rwlock around the free of _destroy_router does nothing for it. Once the
 * code releases that rwlock again, nothing marks the memory itself as unsafe
 * to whoever holds the bare pointer.
 *
 * The shell therefore stays allocated for the rest of the process. The code
 * registers it here so that a destructor at the exit of the process can
 * still free every shell exactly once. That destructor follows the same
 * pattern as the one of chttpsvr_slot_table. A read of router->owner is then
 * always safe in memory. _destroy_router stores CHTTPSVR_INVALID into that
 * field and frees the CONTENTS only. The atomic load of a reader therefore
 * observes either a live handle that it can resolve, or the sentinel. It
 * never observes freed memory. */
static struct {
  ccol_mutex_t mutex;
  ccol_once_flag_t once;
  chttpsvr_router **shells; /* plain realloc'd array of every shell ever
                             * created, process-wide */
  size_t count;
  size_t capacity;
} chttpsvr_router_shell_registry = {0};

static void _chttpsvr_router_shell_registry_init_globals(void) {
  if (ccol_mutex_init(chttpsvr_router_shell_registry.mutex) != 0)
    ccol_fatal_err(
        "chttpsvr router shell registry: failed to initialize mutex");
}

static void _chttpsvr_router_shell_register(chttpsvr_router *r) {
  ccol_call_once(chttpsvr_router_shell_registry.once,
                 _chttpsvr_router_shell_registry_init_globals);
  ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  if (chttpsvr_router_shell_registry.count ==
      chttpsvr_router_shell_registry.capacity) {
    /* This growth is safe against an overflow. It matches every other
     * growable array in this file: _router_add_route, chttpsvr_subrouter,
     * chttpsvr_resp_set_header, _parse_qparams and _servers_register. A plain
     * "capacity * 2" can wrap on an extreme capacity. It then silently
     * allocates less than new_cap * sizeof(ptr) below. A growth that fails,
     * or that the code skips, degrades exactly as a real allocation failure
     * already does a few lines down. The library then does not track this
     * one shell for the free at the exit of the process. It is never a
     * correctness problem. */
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
  /* A growth that fails here comes from an allocation failure in this
   * bookkeeping array alone. It means only that the code does not track this
   * one shell for the free at the exit of the process below. Everything else
   * stays safe, because nothing ever touches a router again once
   * _destroy_router invalidates it. That rare case leaves a "still
   * reachable" leak, and not a correctness problem. */
  if (chttpsvr_router_shell_registry.count <
      chttpsvr_router_shell_registry.capacity)
    chttpsvr_router_shell_registry
        .shells[chttpsvr_router_shell_registry.count++] = r;
  ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* Frees the memory of every router shell at the exit of the process. Without
 * this, the valgrind pass of make memtest reports each one as still
 * reachable. See the comment of the registry for why _destroy_router itself
 * never frees this memory.
 *
 * This function has exactly the shape of _cleanup_chttpsvr_slot_table, with
 * the same ccol_call_once guard, the same reasoning, and the same in_use and
 * owner liveness check. A process can link this library and never create one
 * chttpsvr. Such a process must not lock a mutex here that nothing ever
 * initialized.
 *
 * The code frees a shell here only when router->owner is CHTTPSVR_INVALID.
 * That means one of two things. _destroy_router already ran for it, from a
 * real, completed chttpsvr_destroy() call. Or the shell belongs to a router
 * whose registration failed part way through, on the second exit path of
 * chttpsvr_subrouter.
 *
 * Take a shell whose owner is still a live handle that the library can
 * resolve. It belongs to a chttpsvr that the application never destroyed
 * before the process exits. A free of such a shell here is a use-after-free.
 * The reactor and worker threads of that server are still running. A
 * teardown from __attribute__((destructor)) neither stops nor joins them,
 * unlike an explicit chttpsvr_destroy() call. The moment one of them
 * dispatches the next request through _find_route(), that function
 * dereferences this exact struct.
 *
 * A shell left unfreed here is a "still reachable" leak. The sibling
 * destructor of chttpsvr_slot_table already tolerates the same kind of leak,
 * for the same reason. An application can let a chttpsvr outlive the exit of
 * the process, with no destroy first. Such an application never gets a clean
 * valgrind report for the memory of that handle either way. A leak that dies
 * with the process is better than a crash. */
__attribute__((destructor)) static void _cleanup_chttpsvr_router_shells(void) {
  ccol_call_once(chttpsvr_router_shell_registry.once,
                 _chttpsvr_router_shell_registry_init_globals);
  ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);
  for (size_t i = 0; i < chttpsvr_router_shell_registry.count; i++) {
    chttpsvr_router *r = chttpsvr_router_shell_registry.shells[i];
    if (atomic_load(&r->owner) == CHTTPSVR_INVALID) ccol_mem_free(r);
  }
  free(chttpsvr_router_shell_registry.shells);
  chttpsvr_router_shell_registry.shells = NULL;
  chttpsvr_router_shell_registry.count = 0;
  chttpsvr_router_shell_registry.capacity = 0;
  ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

/* ========================================================================== */
/*                    FORK SAFETY (pthread_atfork)                            */
/* ========================================================================== */

#if CCOL_FORK_SAFETY_REQUIRED
/* fork() duplicates only the calling thread. The child inherits any lock
 * that some OTHER thread held at that instant, and it inherits it in a
 * permanently locked state. No thread survives in the child that could ever
 * unlock it.
 *
 * This prepare() handler therefore takes every lock of this module before
 * fork() may proceed. Those are the process-wide registries that this module
 * owns, and the mutex, idle_mutex, diverted_mutex, wait_mutex and
 * routes_lock of every live server. The registries are chttpsvr_slot_table,
 * srv_engine_bundler with servers_bundler, which share one lazy-init guard (see
 * _engine_globals_init), and chttpsvr_router_shell_registry. Both parent()
 * and child() then release them again, through one shared function.
 *
 * That shared release is well defined. Every mutex in this module uses the
 * default "normal" pthread mutex type. On Linux glibc that type tracks
 * neither an owner nor a TID. A plain pthread_mutex_unlock is therefore well
 * defined even when a thread other than the one that locked it makes the
 * call. For anything that the forking thread did not hold itself, the
 * locking thread does not exist in the child at all.
 *
 * routes_lock is a pthread_rwlock_t, and not a plain mutex. The code takes
 * it for write here, so that the handler excludes a concurrent reader too,
 * and not a writer alone. See the doc comment of
 * _chttpsvr_atfork_release_impl for why it needs a different treatment from
 * the plain mutexes in the child. The write lock of a glibc rwlock DOES
 * track ownership by TID, unlike the plain mutexes of this module. A plain
 * unlock from the child, whose own thread carries a different number, then
 * fails to release it, and it fails in silence.
 *
 * This follows the same atfork handling as cthreadpool.c, cthreadcomm.c and
 * clogger.c, for the same reason. No other module can reach into the opaque
 * globals of this one to protect them from outside. Every module that owns
 * process-wide state which any thread can lock must therefore register its
 * own handlers.
 *
 * Some state of this module needs no handler here. The worker_pool and
 * reject_pool are ctpool handles. The shared reactor is a ccol_event_loop
 * handle. Every clog handle that this module opens or derives is a logger:
 * the engine-wide diagnostics logger, and the logger of each server. The
 * atfork registrations of cthreadpool.c, cthreadcomm.c and clogger.c already
 * protect all of them. A duplicate here would add nothing.
 *
 * The code runs the lazy ccol_call_once guard of every registry here first.
 * Every ordinary lock site elsewhere in this file does the same, for example
 * _engine_wait_until_stopped. A fork() that lands before anything ever
 * touched the engine or router machinery of this module therefore
 * initializes that machinery safely before it proceeds. It does not lock
 * uninitialized memory. The malloc arena locks of glibc rely on this same
 * pattern being safe to call from inside an atfork handler.
 *
 * The walk visits only slots whose in_use is true. That is the exact
 * condition which every resolve function already trusts as the sole
 * indicator that slot->ptr is safe to dereference. The construction order of
 * ccol_create_chttpsvr_mp guarantees that every lock of a server is fully
 * initialized before anything marks its slot in_use.
 *
 * With is_child set, the release also resets three flags to false:
 * srv_engine_bundler.reaper_joinable, srv_engine_bundler.stopping and
 * idle_sweep_bundler.running. See the doc comment of
 * _chttpsvr_atfork_release_impl for the full reasoning. This module has two
 * background threads of its own. Either can be running at the instant of the
 * fork(), or about to be spawned. Without these resets the child inherits a
 * stale thread identifier that names a thread which does not exist in this
 * process. A later ccol_thread_join() on that identifier has no defined
 * outcome under POSIX, and it hangs in practice. That join is reachable from
 * _join_reaper_if_needed_locked() and _idle_sweep_stop_if_running(), and an
 * ordinary chttpsvr_engine_wait() or chttpsvr_engine_stop() call in the
 * child reaches both.
 *
 * A stopping flag stuck at true makes this worse. It blocks every later
 * chttpsvr_engine_stop() call in the child from spawning a real reaper of
 * its own. It also blocks every later chttpsvr_start() and _engine_acquire()
 * call from getting past its own wait for a broadcast. The setter that would
 * have sent that broadcast vanished with the fork.
 *
 * None of these three resets tries to reclaim or join a thread that
 * vanished. Each one says only that there is nothing left to wait for.
 * g_engine_stop_watcher.started gets the same treatment just below, and so
 * does the foreign_since_fork machinery of ctpool and ccol_event_loop for
 * their own worker threads. */
static void _chttpsvr_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  _ccol_atfork_order_record(ccol_atfork_module_chttpserver);
#endif
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);

  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  /* The code locks servers_bundler.mutex BEFORE srv_engine_bundler.mutex.
   * No ordinary code path holds the two locks at the same time. The
   * per-tick snapshot of _idle_sweep_fn releases servers_bundler.mutex
   * before its later, separate _SRV_ENGINE_LOG call touches
   * srv_engine_bundler.mutex (see the comment of that function). Therefore,
   * each relative order is equally safe against the current code.
   *
   * But the code selects one order on purpose. The order does not depend on
   * how this function is written. A change can easily nest these two
   * locks. An example is a hold of servers_bundler.mutex across a
   * diagnostics call that logs. Then the REVERSE of this order is a real
   * AB-BA deadlock against this handler. With one canonical order, each
   * change that nests the two locks in this file has one order to match.
   * Nobody has to find again which order is safe. */
  ccol_mutex_lock(servers_bundler.mutex);
  ccol_mutex_lock(srv_engine_bundler.mutex);

  ccol_call_once(chttpsvr_router_shell_registry.once,
                 _chttpsvr_router_shell_registry_init_globals);
  ccol_mutex_lock(chttpsvr_router_shell_registry.mutex);

  size_t n = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    ccol_mutex_lock(slot->ptr->mutex);
    ccol_mutex_lock(slot->ptr->idle_mutex);
    ccol_mutex_lock(slot->ptr->diverted_mutex);
    ccol_mutex_lock(slot->ptr->wait_mutex);
    ccol_rw_lock_wrlock(slot->ptr->routes_lock);
  }
}

/* Both parent() and child() share this function. See the doc comment of
 * _chttpsvr_atfork_prepare for why a plain unlock is correct in both
 * branches for the plain mutexes of this module.
 *
 * It is safe to walk the same structure that prepare() just walked, and to
 * release every lock symmetrically. Nothing can have changed the slot table,
 * or the state of any live server, in between. Every lock that such a change
 * would need is still held at this exact point.
 *
 * routes_lock is a pthread_rwlock_t, and it needs a different treatment in
 * the child. The hazard there is real and reproduces directly. It is the
 * same one that clog_slot_table.rwlock faces in clogger.c. See the doc
 * comment of that module for the full account of the hang that this
 * avoids.
 *
 * A plain pthread_rwlock_unlock() on the write lock, called from the sole
 * thread of the child, is NOT a valid release. The forking thread of the
 * parent acquired that lock in _chttpsvr_atfork_prepare(). The rwlock write
 * lock of glibc tracks ownership by TID internally. The thread of the child
 * after the fork carries a different TID from the forking thread of the
 * parent. The unlock therefore fails to release it, and it fails in
 * silence. Every later chttpsvr_use() or chttpsvr_register_handler() call
 * against that server in the child then hangs forever. So does anything else
 * that reaches _router_add_route or _router_add_mw. A plain
 * ccol_rw_lock_unlock() in both branches hangs on every trial of the
 * fork_does_not_inherit_a_locked_mutex stress test in tests/chttpserver.
 *
 * The standard remedy is to initialize the lock again in the child, instead
 * of an unlock. The malloc arena locks of glibc use it for this exact
 * scenario, and so does clogger.c in this codebase. It is safe because the
 * child has exactly one thread. Nobody else can be waiting on that lock, so
 * a fresh init has no other party to race.
 *
 * A plain mutex of the default type does not show this behavior. The
 * fast or normal mutex type that this codebase uses throughout does no TID
 * tracking; see the ccol_mutex_init() of common.h. Only routes_lock
 * therefore needs this treatment. The mutex, idle_mutex, diverted_mutex and
 * wait_mutex of a server do not, and neither do the four process-wide
 * registry mutexes.
 *
 * With is_child set, the code also resets three counters of every live
 * server to 0: pending_resolve_count, servers_bundler_pins and
 * listener_dispatch_pins. Each one is a counter that __chttpsvr_destroy
 * waits on, with no timeout, before it frees that server. A thread of the
 * parent vanishes with the fork. At that instant it can be in the middle of
 * a resolve, a quiesce scan or an accept dispatch. One of
 * these counters is then nonzero forever from the point of view of this
 * process, and no thread is left that could decrement it. The atfork release
 * of ctpool applies the same fix to its own pending_resolve_count, against
 * the same class of hazard.
 *
 * The code does NOT apply that reset to in_flight_requests, on purpose. The
 * wait on that counter, in _wait_in_flight_bounded, is already bounded, and
 * it escalates to a forced shutdown. A stale nonzero count there therefore
 * costs a bounded delay, and not a permanent hang. A reset here would also
 * risk letting a destroy free a connection that a worker thread which still
 * exists genuinely has in flight.
 *
 * With is_child set, the code also resets g_engine_stop_watcher.started and
 * g_engine_stop_watcher.ready to false. That watcher is a dedicated OS
 * thread of its own; see the section comment above _engine_stop_watcher_fn.
 * fork() does not duplicate it, any more than it duplicates
 * srv_engine_bundler.reaper_thread or idle_sweep_bundler.thread. Its started
 * guard arrives in the child as an exact copy from the parent.
 *
 * Without that reset, chttpsvr_engine_stop() never works again in this
 * child. That is true not only against the inherited reactor, which is now
 * inert, but also against any FUTURE, genuinely fresh chttpsvr_start() or
 * _engine_acquire() call. The `if (started) return;` inside
 * _engine_stop_watcher_ensure_started_locked() reads the stale inherited
 * true and skips the creation of a real watcher thread for this child
 * entirely.
 *
 * The code leaves the sem and the thread untouched, with no explicit destroy
 * and no explicit reinit. That is safe. Once started is false again, the
 * next _engine_acquire() call in this child calls ccol_semaphore_init() on
 * the inherited sem_t itself. That is well defined, because nothing in this
 * child is blocked on it, and nothing ever was. This follows the same
 * reinit-without-destroy precedent as routes_lock above.
 *
 * With is_child set, the code also fixes up the quiesce_state and the
 * quiesce_waiters of a live server. It does NOT do that by a plain reset of
 * quiesce_state back to CHTTPSVR_QS_NOT_QUIESCED. Such a reset is not
 * enough. This field differs from the pin counters above, because it gates a
 * teardown sequence with several steps and real state, which is
 * _quiesce_server_once. fork() can land while a thread that then vanished
 * was part way through that sequence. That thread can have completed an
 * unknown subset of its steps. The listener may be stopped or not. The
 * connections may be drained or not. The engine reference may be released or
 * not. The worker pool may be destroyed or not. srv->mutex is released
 * between most of those steps.
 *
 * A reset back to CHTTPSVR_QS_NOT_QUIESCED lets a later chttpsvr_start() or
 * _quiesce_server_once call in this child try that sequence again, or enter
 * it again. The real state of that server cannot be inferred safely from
 * this one enum value alone. The result risks a double release or a double
 * free. One example is a second _destroy_detached_pools call on a pool that
 * the vanished thread already freed.
 *
 * The code instead handles exactly the servers that the fork() caught in the
 * middle of a teardown. Those have quiesce_state == CHTTPSVR_QS_QUIESCING.
 * That is the ONLY value a vanished thread can leave behind. A server that
 * was never in a teardown is CHTTPSVR_QS_NOT_QUIESCED, and one whose
 * teardown already finished in full is CHTTPSVR_QS_QUIESCED.
 *
 * For such a server, the code marks the interrupted teardown as finished. It
 * sets quiesce_state to CHTTPSVR_QS_QUIESCED and quiesce_waiters to 0. That
 * matches what the tail of the vanished thread would eventually have done;
 * see the doc comment of _quiesce_server_once. The code then removes srv
 * from servers_bundler.servers[] with _servers_unregister_locked. A direct
 * call of that function is safe here, and a call through _servers_unregister
 * is not. This whole loop already holds servers_bundler.mutex, which it
 * inherited locked from _chttpsvr_atfork_prepare. The ordinary entry point
 * locks that mutex itself, and a second, recursive lock deadlocks.
 *
 * That second step matters on its own. The real work of
 * _quiesce_server_once is normally what unregisters srv. The code skips that
 * real work here, so nothing else ever unregisters it. Without this step,
 * srv stays in servers_bundler.servers[] forever. The driver loop of
 * _engine_force_stop_quiesce_all reads servers_bundler.servers[0] again and
 * again, and it only advances past an entry once _servers_unregister removes
 * that entry. It would therefore spin on this exact, never-removed entry
 * forever, the next time chttpsvr_engine_stop() runs in this child. That is
 * a second permanent hang, reachable on its own, and a forced
 * CHTTPSVR_QS_QUIESCED with no unregister leaves it wide open.
 *
 * This fixup still leaves things unreleased in this process. Those are the
 * worker pool of the interrupted server, its TLS context, and any
 * connections that it held at the instant of the fork(). The code makes no
 * attempt to replay or infer that bookkeeping, for the same double-free risk
 * as above. A later chttpsvr_destroy() or chttpsvr_start() call on this
 * exact handle in the child therefore returns promptly instead of a hang. It
 * costs a leak of whatever the vanished thread had not yet released. If this
 * handle is ever destroyed or restarted, the foreign_since_fork machinery of
 * cthreadpool.c makes the eventual reclamation of the worker pool safe. That
 * machinery exists for exactly this "destroy a pool inherited from a fork"
 * case.
 *
 * With is_child set, the code also resets the lifecycle of every live server
 * back to CHTTPSVR_LC_IDLE, without exception. It does this when that
 * lifecycle is CHTTPSVR_LC_STOPPING or CHTTPSVR_LC_STARTING.
 *
 * Take CHTTPSVR_LC_STOPPING first. It differs from quiesce_state above.
 * _chttpsvr_stop_internal() leaves CHTTPSVR_LC_RUNNING durably and
 * atomically. In the SAME critical section that enters CHTTPSVR_LC_STOPPING,
 * it also resets listen_fd, listen_reg and unix_bind to their own
 * "nothing left to clean up" sentinel values. All of that happens before any
 * of its real work begins. That real work is ccol_event_loop_remove(),
 * close() and unlink(). A fork() can interrupt it there. A fork() anywhere
 * in that work therefore can never leave those
 * fields in a state that chttpsvr_start() could misread. The
 * _quiesce_server_once sequence above differs, because it has several steps
 * and releases srv->mutex between them.
 *
 * Take CHTTPSVR_LC_STARTING next. A fork() in the middle of a call can leave
 * pieces of state half set. The next chttpsvr_start() against this same
 * handle rebuilds every one of them, without exception. The
 * _retire_pools and _destroy_detached_pools functions always drain and
 * destroy whatever worker_pool and reject_pool are published. That is
 * safe against a pool inherited from a fork, which can be half built,
 * because of
 * the foreign_since_fork machinery of cthreadpool.c. The quiesce_state case
 * above already relies on that same mechanism. chttpsvr_start() also
 * releases and rebuilds any published tls_ctx from the start, without
 * exception. A tls_ctx is plain heap memory, and not a slot-table handle. A
 * ctls_ctx_release() on it after a fork therefore needs no special handling.
 * _servers_register() is itself idempotent; see the comment of its call site
 * in chttpsvr_start(). A server that was already registered before the fork
 * is therefore not registered twice by the next start attempt.
 *
 * This follows the same graceful-leak precedent that the quiesce_state fixup
 * of this function already establishes. It is not a new class of risk. The
 * vanished thread can already have created a listener fd and registration
 * for THIS start attempt. That is different from an old one that
 * chttpsvr_stop() already tore down. A lifecycle of CHTTPSVR_LC_IDLE never
 * references that fd. The next successful start silently overwrites it. If
 * no start
 * ever succeeds, it stays as an unreferenced, harmless leak.
 *
 * Without these resets, a lifecycle stuck at CHTTPSVR_LC_STOPPING or
 * CHTTPSVR_LC_STARTING disables this handle in the child forever. For
 * STOPPING, the retry loop of chttpsvr_start() polls forever. That poll is a
 * plain 1ms sleep on this exact state, and not a condvar wait. For STARTING,
 * chttpsvr_start() refuses outright with ccol_not_permitted, and it never
 * retries; it treats that state exactly as a second, genuinely concurrent
 * chttpsvr_start() call. chttpsvr_stop() cannot unstick either state either.
 * Both make the was_started check of _chttpsvr_stop_internal resolve to
 * false, which is a silent no-op that never touches lifecycle.
 *
 * A reset of both states to CHTTPSVR_LC_IDLE, without exception, is safe
 * here. There is no multi-step state to misread, only this one enum value.
 * It has one cost, and that cost follows the quiesce_state case above.
 * Nothing in this process explicitly releases the OLD listener fd or the OLD
 * unix_bind path and directory fd, if there are any. That listener fd is
 * still open in two cases. The first is CHTTPSVR_LC_STOPPING, where the
 * vanished thread had not yet reached its own close() call. The second is
 * CHTTPSVR_LC_STARTING, where it had already reached its own
 * ccol_event_loop_add() call. A later chttpsvr_start() on the same host and
 * port can therefore see a spurious failure of the EADDRINUSE class from that
 * orphaned fd. It then does not get a clean rebind. That is a graceful failure
 * mode which the code already handles, and not a crash. */
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
      /* This is an init again, and not an unlock. See the doc comment of
       * this function for the hazard that it avoids: the rwlock write lock
       * of glibc tracks the TID of its owner. */
      if (ccol_rw_lock_init(srv->routes_lock) != 0)
        ccol_fatal_err("chttpsvr atfork release: failed to reinit routes_lock");

      /* See the doc comment of this function for the full reasoning behind
       * both fixups below. Each one is an exhaustive switch with no
       * `default:` label, and not a plain comparison. The `-Wswitch` reason
       * is the one that the field comments of chttpsvr_lifecycle_t and
       * chttpsvr_quiesce_state_t explain. */
      switch (srv->quiesce_state) {
        case CHTTPSVR_QS_QUIESCING: {
          srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
          srv->quiesce_waiters = 0;
          /* This is the locked variant. This whole loop already holds
           * servers_bundler.mutex, which it inherited locked from
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
      ccol_rw_lock_unlock(srv->routes_lock);
    }
    ccol_mutex_unlock(srv->wait_mutex);
    ccol_mutex_unlock(srv->diverted_mutex);
    ccol_mutex_unlock(srv->idle_mutex);
    ccol_mutex_unlock(srv->mutex);
  }

  if (is_child) {
    /* See the doc comment of this function for why the child must reset
     * this. Without the reset, the `started` guard of
     * _engine_stop_watcher_ensure_started_locked() blocks every attempt to
     * create a real watcher thread for this child. That holds for the rest
     * of the life of this process. */
    g_engine_stop_watcher.started = false;
    atomic_store(&g_engine_stop_watcher.ready, false);
    /* Both request flags go with it. Each one names work for a watcher
     * thread that does not exist in this child. An inherited join request
     * points at a reaper that is not here either. An inherited stop request
     * runs again against the first engine that this child starts. That is
     * exactly the outcome of a forgotten stop coming back to life, which the
     * comment of _engine_stop_watcher_ensure_started_locked rules out for
     * the parent. */
    atomic_store(&g_engine_stop_watcher.join_reaper_requested, false);
    atomic_store(&g_engine_stop_watcher.stop_requested, false);

    /* See the doc comment of _chttpsvr_atfork_prepare for the full
     * reasoning. This module owns two background threads: the reaper of the
     * shared engine, and the sweep thread of the idle timeout. Either can
     * still be marked live from the point of view of the parent, and neither
     * exists in this child at all. A later ccol_thread_join() against such a
     * stale identifier has no defined outcome under POSIX, and it hangs in
     * practice. That join comes from _join_reaper_if_needed_locked() or from
     * _idle_sweep_stop_if_running(), and an ordinary chttpsvr_engine_wait()
     * or chttpsvr_engine_stop() call reaches both.
     *
     * srv_engine_bundler.mutex guards the first of these two fields, and
     * servers_bundler.mutex guards the second. This whole function already
     * holds both, so these two writes need no separate lock. */
    srv_engine_bundler.reaper_joinable = false;
    idle_sweep_bundler.running = false;

    /* srv_engine_bundler.stopping can also be left stuck at true, on its
     * own. That happens whenever fork() lands while a real reaper pass on
     * the parent side is in flight, or is about to be. The
     * _engine_release() and _engine_force_stop_now() functions set it true
     * strictly before _spawn_reaper() itself runs. It can therefore be true
     * even in the narrow window where
     * reaper_joinable above is still false.
     *
     * Left stuck at true, it blocks EVERY later chttpsvr_engine_stop() call
     * in the child from spawning a new, real reaper of its own. The
     * !stopping guard of _engine_force_stop_now() exists to reject only a
     * real, redundant second call, and it silently rejects every future one
     * instead. It also blocks EVERY later chttpsvr_start() and
     * _engine_acquire() call. The `while (stopping) ccol_cond_var_wait(...)`
     * loop of those calls waits for a broadcast that only the completion of
     * the now-vanished reaper thread would ever send.
     *
     * A reset of this flag, without exception, is safe. The code leaves
     * srv_engine_bundler.reactor itself untouched. That is still a real
     * ccol_event_loop handle that the library can resolve, even though it is
     * inert for serving; see the foreign_since_fork machinery of
     * ccol_event_loop. A fresh chttpsvr_engine_stop() call in the child can
     * therefore still spawn a real reaper and tear the reactor down
     * properly. */
    srv_engine_bundler.stopping = false;
  }

  ccol_mutex_unlock(chttpsvr_router_shell_registry.mutex);
  ccol_mutex_unlock(servers_bundler.mutex);
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

static void _chttpsvr_atfork_release(void) {
  _chttpsvr_atfork_release_impl(false);
}

/* This is the child-side counterpart of _chttpsvr_atfork_release. It
 * releases the same locks; see the comment of
 * _chttpsvr_atfork_release_impl. It also resets the pin counters of every
 * live server, which are the ones that a wait can block on forever.
 *
 * It must run before any application code in this process can reach the
 * shutdown or destroy path of one of these servers. It does. The child
 * handler of pthread_atfork runs synchronously, as part of the return of
 * fork() itself. It runs strictly before the return value of fork() reaches
 * the calling code. */
static void _chttpsvr_atfork_child_release(void) {
  _chttpsvr_atfork_release_impl(true);
}
#endif /* CCOL_FORK_SAFETY_REQUIRED */

/* ========================================================================== */
/*                         ROUTER INTERNAL HELPERS                            */
/* ========================================================================== */

/* Counts the segments of a router prefix, which a '/' separates. The prefix
 * has no trailing slash and no consecutive slashes. r->prefix always holds
 * exactly that form by the time a caller reaches this function.
 * chttpsvr_subrouter rejects a "//" in the prefix that the caller gives,
 * before _create_router ever runs. The loop just above the call site of this
 * function strips the trailing slashes, and it never introduces a new one.
 *
 * Two prefixes have zero segments. The first is "", which is the placeholder
 * of the root. The second is "/", which is the special prefix that matches
 * the exact root only. Every other valid prefix has exactly one segment for
 * each '/' character that it holds. Exactly one such character precedes each
 * segment. There is no leading run of them, because every prefix starts with
 * a single '/'. There is no trailing one, because the code already stripped
 * it. And there is no run inside, because the code already rejected it. */
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
  /* The shell itself does NOT use `mp`, which is the allocator of the
   * server and may be a custom one. It uses ccol_mem_calloc and
   * ccol_mem_free instead. Those are the plain default-allocator macros. The
   * _mem_* ones need a real argument of type ccol_memmgmt_procs_t, and not a
   * bare NULL literal. See the comment of chttpsvr_router_shell_registry for
   * why the memory of this struct must stay valid for the whole rest of the
   * process. That is well past the lifetime of srv, and therefore well past
   * the lifetime of mp. */
  chttpsvr_router *r =
      (chttpsvr_router *)ccol_mem_calloc(1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    ccol_mem_free(r);
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
  /* The code invalidates the router before it frees any contents. The
   * _chttpsvr_finish_destroy function reaches this call only after the
   * pending_resolve_count wait of __chttpsvr_destroy confirmed one thing. No
   * resolve of the owner of this router is pinned, and none can start. No
   * reader can therefore be inside
   * _router_add_route or _router_add_mw right now. The code stores
   * CHTTPSVR_INVALID here, and not after the frees. A brand new reader that
   * arrives from this instant onward then sees the sentinel and returns at
   * once. Without it, such a reader races the frees below. */
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

  ccol_rw_lock_wrlock(router->srv->routes_lock);
  if (router->route_count >= router->route_cap) {
    if (router->route_cap > (SIZE_MAX - 4) / 2 ||
        router->route_cap * 2 + 4 > SIZE_MAX / sizeof(chttpsvr_route_t *)) {
      ccol_rw_lock_unlock(router->srv->routes_lock);
      _free_route_data(rt, mp);
      _ccol_mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    size_t new_cap = router->route_cap * 2 + 4;
    chttpsvr_route_t **nr = (chttpsvr_route_t **)_ccol_mem_realloc(
        mp, router->routes, new_cap * sizeof(chttpsvr_route_t *));
    if (!nr) {
      ccol_rw_lock_unlock(router->srv->routes_lock);
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
  ccol_rw_lock_unlock(router->srv->routes_lock);
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

  ccol_rw_lock_wrlock(router->srv->routes_lock);
  if (router->mw_count >= _CHTTPSVR_MAX_MW) {
    ccol_rw_lock_unlock(router->srv->routes_lock);
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
  ccol_rw_lock_unlock(router->srv->routes_lock);
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
    /* The code skips a pair that is truly empty. Such a pair holds nothing
     * at all between two '&' characters, before a leading one, or after a
     * trailing one. Examples are "a=1&&b=2" and "&a=1". The code does not add
     * a spurious entry with an empty key and an empty value for it.
     *
     * Two other forms look similar and must not be confused with this one.
     * The library supports both on purpose. "?=value" has an empty key and a
     * real value. Its pair_len is above 0, because "=value" is not empty.
     * "?key" has a real key and an implicit empty value, and its pair_len is
     * above 0 too. This check leaves both untouched. The code below parses
     * each as its own entry. Both are already documented and tested. */
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
      /* This growth is safe against an overflow. It matches every other
       * growable array in this file: _router_add_route, chttpsvr_subrouter
       * and chttpsvr_resp_set_header. A plain "cap * 2 + 8" can wrap on an
       * extreme qp->cap. It then silently allocates less than
       * nc * sizeof(char *) below. */
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
      /* This matches the sibling _parse_qparams failure branch below. This
       * branch runs for a request with no query string at all, which is the
       * common case. Without this flag, chttpsvr_req_query_oom() cannot tell
       * an allocation failure here from a real "the key is absent" result.
       * The documented purpose of that function is to tell the two apart. */
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

chttpsvr ccol_create_chttpsvr_mp(ccol_memmgmt_procs_t *mprocs, clog cl,
                                 char **err_str) {
  if (!ccol_verify_memmgmt_procs(mprocs, err_str)) return CHTTPSVR_INVALID;

  struct chttpserver *srv = (struct chttpserver *)_ccol_mem_calloc(
      mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }
  atomic_init(&srv->lifetime_refs, 1);

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs_t *)_ccol_mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs_t));
    if (!srv->m_procs) {
      _ccol_mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("out of memory");
      return CHTTPSVR_INVALID;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs_t));
  }

  clog logger =
      cl ? clog_derive(cl) : clog_open_fd_mp(2, CLOG_FATAL, NULL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("failed to create server logger");
    return CHTTPSVR_INVALID;
  }

  if (ccol_mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (ccol_mutex_init(srv->idle_mutex) != 0) {
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (ccol_mutex_init(srv->diverted_mutex) != 0) {
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (ccol_mutex_init(srv->wait_mutex) != 0) {
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_mutex_init failed");
    return CHTTPSVR_INVALID;
  }
  if (ccol_cond_var_init(srv->resolve_cv) != 0) {
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  {
    /* The clock is CLOCK_MONOTONIC, to match the deadline of
     * _wait_in_flight_bounded. That function builds its deadline with
     * clock_gettime(CLOCK_MONOTONIC, ...). The default clock of
     * ccol_cond_var_init is CLOCK_REALTIME, and it makes that comparison
     * wrong. It compares a timespec from the monotonic clock against a
     * condvar that uses the wall clock inside. */
    ccol_cond_var_attr_t cv_attr;
    int cv_rc = 0;
    if (ccol_cond_var_attr_init(cv_attr) == 0) {
      ccol_cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
      cv_rc = ccol_cond_var_init_ca(srv->requests_done_cv, cv_attr);
      ccol_cond_var_attr_destroy(cv_attr);
    } else {
      cv_rc = 1;
    }
    if (cv_rc != 0) {
      ccol_cond_var_destroy(srv->resolve_cv);
      ccol_mutex_destroy(srv->idle_mutex);
      ccol_mutex_destroy(srv->diverted_mutex);
      ccol_mutex_destroy(srv->wait_mutex);
      ccol_mutex_destroy(srv->mutex);
      clog_close(logger);
      _ccol_mem_free(mprocs, srv->m_procs);
      _ccol_mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL_ERR_STR("ccol_cond_var_init failed");
      return CHTTPSVR_INVALID;
    }
  }

  if (ccol_cond_var_init(srv->quiesce_done_cv) != 0) {
    ccol_cond_var_destroy(srv->requests_done_cv);
    ccol_cond_var_destroy(srv->resolve_cv);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_cond_var_init failed");
    return CHTTPSVR_INVALID;
  }

  if (ccol_rw_lock_init(srv->routes_lock) != 0) {
    ccol_cond_var_destroy(srv->quiesce_done_cv);
    ccol_cond_var_destroy(srv->requests_done_cv);
    ccol_cond_var_destroy(srv->resolve_cv);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("ccol_rw_lock_init failed");
    return CHTTPSVR_INVALID;
  }

  chttpsvr_router *root = _create_router(srv, "", srv->m_procs);
  if (!root) {
    ccol_rw_lock_destroy(srv->routes_lock);
    ccol_cond_var_destroy(srv->quiesce_done_cv);
    ccol_cond_var_destroy(srv->requests_done_cv);
    ccol_cond_var_destroy(srv->resolve_cv);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
    return CHTTPSVR_INVALID;
  }

  srv->routers = (chttpsvr_router **)_ccol_mem_alloc(srv->m_procs,
                                                     sizeof(chttpsvr_router *));
  if (!srv->routers) {
    _destroy_router(root, srv->m_procs);
    ccol_rw_lock_destroy(srv->routes_lock);
    ccol_cond_var_destroy(srv->quiesce_done_cv);
    ccol_cond_var_destroy(srv->requests_done_cv);
    ccol_cond_var_destroy(srv->resolve_cv);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL_ERR_STR("out of memory");
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
    if (err_str) *err_str = CCOL_ERR_STR("failed to allocate server slot");
    _ccol_mem_free(srv->m_procs, srv->routers);
    _destroy_router(root, srv->m_procs);
    ccol_rw_lock_destroy(srv->routes_lock);
    ccol_cond_var_destroy(srv->quiesce_done_cv);
    ccol_cond_var_destroy(srv->requests_done_cv);
    ccol_cond_var_destroy(srv->resolve_cv);
    ccol_mutex_destroy(srv->idle_mutex);
    ccol_mutex_destroy(srv->diverted_mutex);
    ccol_mutex_destroy(srv->wait_mutex);
    ccol_mutex_destroy(srv->mutex);
    clog_close(logger);
    _ccol_mem_free(mprocs, srv->m_procs);
    _ccol_mem_free(mprocs, srv);
    return CHTTPSVR_INVALID;
  }

  /* The root router differs from every other router. The library never hands
   * it back to a caller as a chttpsvr_router*. The owner-resolve path of
   * chttpsvr_router_on, _on_stream and _use therefore never runs on it.
   *
   * The code sets root->owner here for one reason only. The
   * _cleanup_chttpsvr_router_shells function is the destructor that runs at
   * the exit of this process. It must tell "root is still live" apart from
   * "_destroy_router already destroyed root". It can already do that for
   * every sub-router.
   *
   * _create_router leaves this field at CHTTPSVR_INVALID. A root left at
   * that value looks exactly like a root that something already destroyed.
   * The destructor then frees the shell of root out from under a server that
   * is still running. It neither stops nor joins the reactor and worker
   * threads of that server first. */
  atomic_store(&root->owner, h);
  return h;
}

/* A server has up to three pools: worker_pool, reject_pool and stream_pool.
 * The library creates the first two together and the third at the first
 * streaming request, and it destroys all three together; see chttpsvr_start
 * and _quiesce_server_once. Every detach point therefore needs all three, and
 * not worker_pool alone. They come back in one small return type, instead of
 * through two separate detach functions. That way no call site can detach
 * one and forget the other. */
typedef struct {
  ctpool worker;
  ctpool reject;
  ctpool stream;
} _detached_pools_t;

/* The extra grace period that _wait_in_flight_bounded gives after it forces
 * a shutdown(2) on the fd of every connection that is still diverted. The
 * worker thread of each such connection needs that time to notice the I/O
 * error and unwind. Each one unwinds through its own error-handling paths,
 * which are the same ones that a real disconnect by a peer already
 * exercises. See the comment of that function for the full reasoning. */
#define _CHTTPSVR_FORCE_UNBLOCK_GRACE_MS 5000
/* The real, documented bound on the graceful wait that
 * _wait_in_flight_bounded gives to the requests in flight, before it
 * considers the forced-unblock path at all. It has its own name so that the
 * RUNNING_UNIT_TESTS override below has one clear default to fall back
 * to. */
#define _CHTTPSVR_GRACEFUL_WAIT_MS 30000

/* The bound of the wait of _drain_and_close_all_connections for every
 * connection of a server to be freed. */
#define _CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS 30000

#ifdef RUNNING_UNIT_TESTS
/* This is a white-box test override for the two timing bounds of
 * _wait_in_flight_bounded. Those are the graceful wait before the
 * escalation, and the grace period after the forced unblock. A 0 in both,
 * which is the default, means "use the real, documented values" above.
 *
 * A test calls _chttpsvr_set_wait_in_flight_bounds_for_tests with a pair of
 * small millisecond values. It then exercises the same escalation logic
 * every time, in well under a second, instead of a real wait of tens of
 * seconds. The override covers the whole process, and not one server, in the
 * same way as every other RUNNING_UNIT_TESTS hook in this file. The guard
 * keeps this storage and its setter out of a production build. */
static _Atomic unsigned g_wait_in_flight_graceful_ms_for_tests = 0;
static _Atomic unsigned g_wait_in_flight_grace_ms_for_tests = 0;
void _chttpsvr_set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                   unsigned grace_ms) {
  atomic_store(&g_wait_in_flight_graceful_ms_for_tests, graceful_ms);
  atomic_store(&g_wait_in_flight_grace_ms_for_tests, grace_ms);
}

/* The same kind of override for the bound of the wait of
 * _drain_and_close_all_connections for current_connections to reach 0. A 0,
 * the default, means the real bound of _CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS. */
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

/* Waits for srv->in_flight_requests to reach 0, with a bound of 30s of
 * graceful waiting. When that is not enough, the function forcibly
 * interrupts every connection that is still diverted to a worker thread. It
 * does so through _force_unblock_diverted_connections. It then waits a
 * further grace period, with its own separate bound. In that period those
 * workers unwind after their new I/O error. Each one then releases its own
 * in_flight_requests slot.
 *
 * This exists because of what every caller does shortly afterward. Each one
 * calls ctpool_shutdown_drain() on the worker pool, through
 * _destroy_detached_pools. That function has NO timeout of its own. Its doc
 * comment in cthreadpool.h says that it blocks until every queued and active
 * task completes, without exception.
 *
 * One connection is enough to defeat this wait without the forced unblock
 * below. The worker thread of that connection can legitimately block forever
 * inside chttp1_stream_read or chttp1_stream_write. That is a real,
 * reachable case. It needs a stream_read_timeout_us or a
 * response_write_timeout_us configured to 0. A value of 0 means "wait
 * forever". It is a documented, legitimate setting of chttpsvr_config_t for
 * slow but legitimate uploads. It also needs a peer that stalls and never
 * closes the connection.
 *
 * The careful 30s bound of this function then means nothing. The timeout
 * elapses and this function returns anyway. The very next call of the
 * caller, which is ctpool_shutdown_drain, then hangs forever on that same
 * stuck task. Bounding this wait at all would have no point. The callers
 * are chttpsvr_destroy() and chttpsvr_engine_stop(), through
 * _drain_and_close_all_connections. A restart never comes here: it retires
 * the pools of the previous run and lets their requests run to their end;
 * see _retire_pools.
 *
 * The caller must hold srv->mutex. This function releases that mutex around
 * the shutdown(2) pass and takes it again afterward. It does no I/O while it
 * holds the lock. That matches the established convention of this codebase
 * elsewhere, such as the liveness probe of the idle pool in chttpclient.c.
 * It touches neither worker_pool nor reject_pool. The caller detaches those
 * itself. */
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
    if (ccol_cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
        ETIMEDOUT) {
      timed_out = true;
      break;
    }
  }
  if (!timed_out || srv->in_flight_requests <= 0) return;

  ccol_mutex_unlock(srv->mutex);
  _force_unblock_diverted_connections(srv);
  ccol_mutex_lock(srv->mutex);
  if (srv->in_flight_requests <= 0) return;

  struct timespec grace_deadline;
  clock_gettime(CLOCK_MONOTONIC, &grace_deadline);
  _timespec_add_ms(&grace_deadline, grace_ms);
  while (srv->in_flight_requests > 0) {
    if (ccol_cond_var_timedwait(srv->requests_done_cv, srv->mutex,
                                grace_deadline) == ETIMEDOUT)
      break;
  }
  /* in_flight_requests can STILL be nonzero here. Whatever is stuck is then
   * not a blocked socket read or write, because the code already unblocked
   * that whole class. It is something that this library can neither see nor
   * control. One example is a handler blocked in unrelated work that has
   * nothing to do with a socket. ctpool_shutdown_drain() below waits on it.
   * There is no
   * further forcible action that this function could safely take. */
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

/* A restart of a server takes its pools out of service without waiting for
 * the requests of the previous run that still run on them. A connection that
 * stays open across a stop keeps being served, and a handler that runs keeps
 * running to its own end: the restart installs fresh pools at once, and the
 * old ones drain on a thread of their own, with no forced unblock. This
 * record holds one such set of old pools. Each of its tasks references srv,
 * so every teardown of srv (_quiesce_server_once) waits for every record
 * first; see _retired_pools_reap. Nothing reaches the old pools through srv
 * once they are detached: every submit reads the current pool under
 * srv->mutex, and a submit that read an old pool just before the detach
 * lands on it before the drain, or is refused after it, which its caller
 * already handles as a full pool. */
typedef struct _chttpsvr_retired_pools {
  struct _chttpsvr_retired_pools *next;
  struct chttpserver *srv;
  _detached_pools_t pools;
  ccol_thread_id_t thread;
  bool has_thread; /* a drain thread runs or ran for this record */
  bool done;       /* that thread finished; set under srv->mutex */
} _chttpsvr_retired_pools_t;

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_retired_drained_for_tests = 0;
/* The number of retired pool sets whose drain finished, in the process. */
size_t _chttpsvr_retired_drained_for_tests(void) {
  return atomic_load(&g_retired_drained_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_retired_drain_started_for_tests = 0;
/* The number of retired pool sets whose drain started, in the process. */
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
  /* The last touch of srv. The thread that created this one holds
   * srv->mutex until it recorded the thread in rec, so a reaper that sees
   * done can always join it. */
  struct chttpserver *srv = rec->srv;
  ccol_mutex_lock(srv->mutex);
  rec->done = true;
  ccol_mutex_unlock(srv->mutex);
  return NULL;
}

/* Starts the drain thread of rec. The caller holds srv->mutex, which the
 * thread takes as its last step, so rec->thread is recorded before the
 * thread can report itself done. */
static void _retired_pools_spawn_locked(_chttpsvr_retired_pools_t *rec) {
  ccol_thread_id_t t;
  if (ccol_thread_create(t, _retired_pools_drain_fn, rec) == 0) {
    rec->thread = t;
    rec->has_thread = true;
  }
}

/* Joins and frees every retired set whose drain finished, and starts a
 * drain thread for a set that has none, which a failed thread creation
 * leaves. With wait_all it waits for every set instead: the caller is a
 * teardown, which already ended or forcibly unblocked every request, so
 * each drain ends. A set with no thread is then drained on this thread. */
static void _retired_pools_reap(struct chttpserver *srv, bool wait_all) {
  ccol_mutex_lock(srv->mutex);
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
  ccol_mutex_unlock(srv->mutex);
  while (finish) {
    _chttpsvr_retired_pools_t *next = finish->next;
    if (finish->has_thread)
      ccol_thread_join(finish->thread);
    else
      _destroy_detached_pools(finish->pools);
    _ccol_mem_free(srv->m_procs, finish);
    finish = next;
  }
}

/* Takes pools out of service; see _chttpsvr_retired_pools_t. A set with no
 * pool is nothing to do. When the record cannot be allocated, the pools
 * drain here, on the calling thread, still with no forced unblock. */
static void _retire_pools(struct chttpserver *srv, _detached_pools_t pools) {
  if (!pools.worker && !pools.reject && !pools.stream) return;
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
  ccol_mutex_lock(srv->mutex);
  _retired_pools_spawn_locked(rec);
  rec->next = srv->retired_pools;
  srv->retired_pools = rec;
  ccol_mutex_unlock(srv->mutex);
}

/* Detaches the pools that srv runs with now and retires them. */
static void _retire_current_pools(struct chttpserver *srv) {
  ccol_mutex_lock(srv->mutex);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool,
                           srv->stream_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  srv->stream_pool = CTPOOL_INVALID;
  ccol_mutex_unlock(srv->mutex);
  _retire_pools(srv, out);
}

/* This is the variant of the wait above that __chttpsvr_destroy uses. It
 * also closes every connection that is still idle. It does so atomically
 * against every place that increments in_flight_requests and reads
 * worker_pool and reject_pool. Those places are _conn_start_diverted,
 * _conn_dispatch_reject and _conn_reject_via_pool. The function holds
 * srv->mutex without a break, from the moment it sees the in-flight work
 * drained, all the way through the idle-close pass.
 *
 * Without that, the library can re-divert a keep-alive connection in
 * silence. Take a connection whose request finished, and which therefore
 * landed back in the idle list, exactly as the in-flight wait below
 * completed. It then receives a further pipelined request before an
 * idle-close pass with its own separate lock reaches it. That connection
 * escapes the pass entirely, and it leaks once this function goes on to
 * release the engine and the reactor out from under it. That is a real leak,
 * even if a rare one: one orphaned chttpsvr_conn_t for each full run of the
 * suite, and valgrind reports it.
 *
 * A request can still arrive after this function closes a connection, or
 * while this function holds srv->mutex. Such a request meets one of two
 * outcomes, and neither is a leak or a crash. It hits a ccol_event_loop
 * registration that the library already removed. That is a safe no-op
 * through the same dispatch-time liveness check that this codebase already
 * relies on elsewhere. Or it reads a worker_pool of NULL after it finally
 * acquires the lock, and a reject_pool of NULL by the same reasoning. It
 * then falls all the way back to the synchronous, last-resort inline close
 * of _conn_start_diverted. */
static _detached_pools_t _drain_and_close_all_connections(
    struct chttpserver *srv) {
  /* No waiter for body memory is dispatched from here on: a close below
   * gives memory back while this thread holds srv->mutex, and a dispatch
   * needs that lock. Every waiter is closed instead. The streaming queue is
   * answered with 503 before the wait, so that the wait does not sit out the
   * queue deadline of every request in it. */
  _mem_admission_close(srv);
  _stream_queue_flush(srv);
  ccol_mutex_lock(srv->mutex);
  _wait_in_flight_bounded(srv);
  _detached_pools_t out = {srv->worker_pool, srv->reject_pool,
                           srv->stream_pool};
  srv->worker_pool = CTPOOL_INVALID;
  srv->reject_pool = CTPOOL_INVALID;
  srv->stream_pool = CTPOOL_INVALID;
  _close_all_idle_connections(srv);
  _mem_wait_flush(srv);
  ccol_mutex_unlock(srv->mutex);

  /* One _close_all_idle_connections pass above is not enough. Two kinds of
   * connection are invisible to that one pass. The first is a connection
   * that the library accepted moments ago. It is still in the middle of the
   * TLS handshake or the header read on the reactor thread. Nothing added it
   * to the idle list yet. The second is a connection that a dispatch already
   * claimed out of the idle list for ordinary work. One example is a
   * dispatch that finds the peer closed and handles that directly. It goes
   * through _conn_pump into _conn_close, with no divert to a worker at all.
   * in_flight_requests never even saw that one.
   *
   * This is a real use-after-free, and ThreadSanitizer reports it over
   * tests_tls. The reactor thread can still be inside _conn_pump or
   * _conn_free for such a connection after this function returns and
   * __chttpsvr_destroy goes on to free srv.
   *
   * current_connections covers the whole life of a connection, whichever
   * path closes it. The library increments it at the accept and decrements
   * it in _conn_free. in_flight_requests covers only the requests that the
   * library diverted to a worker. The idle list holds only the connections
   * that no dispatch currently claims. A wait for current_connections to
   * reach zero therefore closes the gap in full, instead of trust in one
   * snapshot. The loop repeats the idle-close pass as connections that are
   * still active finish and land back in the idle list.
   *
   * The loop polls, and it does not use a condition variable of its own.
   * That is deliberate. A broadcast from every _conn_free call would add
   * permanent cost to the hot per-connection close path. That path runs for
   * every connection the server ever serves, and not only during a shutdown.
   * The window it closes matters only on this cold path, which runs once in
   * the life of a server. */
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

/* Does the complete teardown that __chttpsvr_destroy needs. It stops the
 * listen and unregisters srv from servers_bundler.servers. It then drains
 * the requests that are in flight and closes the idle connections. It shuts
 * the worker pool down and destroys it. Last, it releases the engine
 * reference of this server.
 *
 * This work sits in its own function, and srv->quiesce_state guards it. The
 * engine can therefore drive it too, exactly one time, from its own
 * _engine_force_stop_quiesce_all pass. chttpsvr_engine_stop() may run that
 * pass while srv is still fully started. That pass can run at the same time
 * as an independent chttpsvr_destroy(srv) call of the application, and not
 * only before it. chttpsvr_engine_stop() is documented as safe to call from
 * a signal handler. Nothing demands that a caller serialize it against
 * chttpsvr_destroy() with chttpsvr_engine_wait() first.
 *
 * Whichever of the two callers reaches a given srv first does the real work.
 * The OTHER caller BLOCKS until that work really finishes. That means until
 * quiesce_state reaches CHTTPSVR_QS_QUIESCED. It does not return as an
 * immediate no-op. This matters because of what __chttpsvr_destroy does
 * next. It goes straight from the return of this function into
 * ccol_mutex_destroy(raw->mutex), and then it frees raw itself. Take a
 * losing caller that returns the instant it observes quiesce_state !=
 * CHTTPSVR_QS_NOT_QUIESCED. The __chttpsvr_destroy function can then destroy
 * raw->mutex and raw->idle_mutex, and free raw. The call of the winning
 * caller still uses them. That use is inside
 * _drain_and_close_all_connections and _engine_release below. It is a
 * genuine use-after-free, and a destroy of a
 * mutex that is still in use. It is not merely theoretical. The retry loop
 * of _engine_force_stop_quiesce_all already handles the reverse race, which
 * is a concurrent chttpsvr_destroy().
 *
 * The order here is stop, unregister, drain, release the engine reference,
 * and destroy the pool. That order must run to completion before anything
 * tears the shared reactor down. See the doc comment of
 * _engine_force_stop_quiesce_all for the reason. */
static void _quiesce_server_once(struct chttpserver *srv) {
  ccol_mutex_lock(srv->mutex);
  /* This switch is exhaustive and has no `default:` label. The `-Wswitch`
   * reason is the identical one that the field comment of
   * chttpsvr_quiesce_state_t explains. A loser already has a quiesce_state
   * of CHTTPSVR_QS_QUIESCING or of CHTTPSVR_QS_QUIESCED. Both values share
   * one branch. The wait loop of that branch degrades to a correct,
   * zero-iteration no-op when quiesce_state is already CHTTPSVR_QS_QUIESCED.
   * To route both values through the same path is therefore exactly
   * equivalent to a test for CHTTPSVR_QS_QUIESCING alone. It is not a new
   * case with different handling. */
  switch (srv->quiesce_state) {
    case CHTTPSVR_QS_QUIESCING:
    case CHTTPSVR_QS_QUIESCED: {
      /* See the field comment of quiesce_waiters for why this increment and
       * decrement pair exists. Both run while the code still holds
       * srv->mutex. The pair lets the tail of the winner below learn one
       * thing through a genuine happens-before relationship. That thing is
       * that this loser fully finished with srv->mutex and
       * srv->quiesce_done_cv. A broadcast alone does not show it. Only after
       * that may the caller of the winner destroy either one. */
      srv->quiesce_waiters++;
      while (srv->quiesce_state != CHTTPSVR_QS_QUIESCED)
        ccol_cond_var_wait(srv->quiesce_done_cv, srv->mutex);
      srv->quiesce_waiters--;
      ccol_cond_var_broadcast(srv->quiesce_done_cv);
      ccol_mutex_unlock(srv->mutex);
      return;
    }
    case CHTTPSVR_QS_NOT_QUIESCED:
      srv->quiesce_state = CHTTPSVR_QS_QUIESCING;
      break;
  }
  /* Wait for every call on srv that _chttpsvr_resolve protects and that is
   * in flight now. Those calls are chttpsvr_start, _stop, _register_handler,
   * _use, _subrouter and the others. They must finish before this function
   * touches any mutable state of srv below.
   *
   * The __chttpsvr_destroy function already does this exact wait itself,
   * before it ever calls this function. The _engine_force_stop_quiesce_all
   * function does not. That function is the forced path of
   * chttpsvr_engine_stop(). It calls this function directly against every
   * registered server, with no wait of its own. Unlike destroy, that path
   * can legitimately run at the same time as a chttpsvr_start(). Such a
   * start already took an engine reference, so raw->contributed_to_engine is
   * true. It also still uses srv_engine_bundler.reactor further down its own
   * function body, for example to call ccol_event_loop_add for the listener.
   *
   * Without this wait, this function can decide to release the engine
   * reference of this server below. It can unregister and drain srv at the
   * same time. chttpsvr_start still believes that it holds that exact
   * reference. It goes on to register a listener against a reactor.
   * Something may tear that reactor down, or it may already be gone, by the
   * time it gets there. That is a ccol_event_loop_add call against a
   * ccol_event_loop handle that something already destroyed.
   *
   * The wait here uses the same srv->mutex and resolve_cv pair that
   * _chttpsvr_resolve_unpin already uses. It closes that window uniformly
   * for every caller of this function. No caller has to remember it.
   *
   * Note that ccol_event_reg is itself a generation-checked handle. The slot
   * table of its own ccol_event_loop resolves it, exactly as it resolves the
   * ccol_event_loop handle. A stale conn->reg that reaches a later
   * ccol_event_loop_remove(), _modify(), _pause() or _resume() call is
   * therefore always rejected safely, with no risk of a use-after-free. That
   * is not what this particular wait protects against. */
  while (atomic_load(&srv->pending_resolve_count) > 0)
    ccol_cond_var_wait(srv->resolve_cv, srv->mutex);
  ccol_mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  _quiesce_teardown_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  _chttpsvr_stop_internal(srv);
  _servers_unregister(srv);

  /* Drain the work that is in flight and close every idle connection that
   * remains. This must happen BEFORE the release of the engine reference of
   * this server below. The _engine_release() call can be the one that drops
   * the refcount of the shared reactor to zero. That happens when srv is the
   * last contributing server. It then hands
   * ccol_event_loop_destroy(srv_engine_bundler.reactor) to an asynchronous
   * reaper thread. This function makes its own ccol_event_loop_remove()
   * calls, inside _drain_and_close_all_connections through _conn_close.
   * Those calls race that reaper if they run after the release instead of
   * before it. */
  _detached_pools_t old_pools = _drain_and_close_all_connections(srv);
  /* The pools that restarts retired run requests of srv as well. The drain
   * above already waited for, or forcibly unblocked, every request of every
   * run, so each of their drains ends. */
  _retired_pools_reap(srv, true);

  bool should_release_engine = false;
  ccol_mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
  }
  ccol_mutex_unlock(srv->mutex);
  if (should_release_engine) _engine_release();

  _destroy_detached_pools(old_pools);

  ccol_mutex_lock(srv->mutex);
  srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
  ccol_cond_var_broadcast(srv->quiesce_done_cv);
  /* Wait for every loser that this broadcast just woke. Each one must leave
   * its own ccol_cond_var_wait call before this function returns to its own
   * caller. The decrement of quiesce_waiters is the last touch that a loser
   * makes to srv->mutex and srv->quiesce_done_cv. See the field comment of
   * quiesce_waiters for the real race that this closes. POSIX leaves that
   * race undefined, and it happens when the caller is __chttpsvr_destroy,
   * which destroys both immediately afterward.
   *
   * A plain broadcast alone guarantees only that a woken thread is marked
   * runnable. It does not guarantee that the thread took the mutex again.
   * The reacquire and check of this loop is what turns "marked runnable"
   * into "provably finished". It does so through the same mutual-exclusion
   * happens-before relationship that the wait of pending_resolve_count
   * already uses elsewhere in this file.
   *
   * In the common case there are 0 losers, and this wait is a no-op. Nothing
   * bounds it but the scheduling of the OS. A loser that the broadcast just
   * above woke is free to take the lock again, decrement, and broadcast
   * again, at once. */
  while (srv->quiesce_waiters > 0)
    ccol_cond_var_wait(srv->quiesce_done_cv, srv->mutex);
  ccol_mutex_unlock(srv->mutex);
}

void __chttpsvr_destroy(chttpsvr srv) {
  if (!srv) return;

  /* Resolve srv through the slot table. The code marks the slot as not in
   * use in the same critical section as the lookup. That is what makes a
   * second destroy call on the same handle value see a resolve failure. Such
   * a call can be concurrent, or later and sequential. It never races the
   * teardown of this call. See the file-level comment of the slot table, and
   * the comment of _chttpsvr_resolve, for the full design. A stale or
   * already-destroyed handle that reaches here is exactly the misuse that
   * the generation-checked handle design exists to catch. It is fatal. It is
   * not a silent use-after-free or double free. */
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(srv >> 32);
  uint32_t gen = (uint32_t)(srv & 0xFFFFFFFFu);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
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
    ccol_mutex_unlock(chttpsvr_slot_table.mutex);
    ccol_fatal_err(
        "chttpsvr_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpsvr handle)");
  }
  /* This is the self-call case. See the comment of
   * chttpsvr_worker_key_bundle above. Unlike a stale handle, this is a live,
   * valid server. Its own worker pool is what calls destroy on it right now.
   * There is no safe way to go on. A drain of the requests that are in
   * flight can never converge, because this exact call stack is what would
   * finish and decrement the count. The ctpool_destroy() call that follows,
   * on the pool of this very thread, is an unrelated fatal misuse of
   * cthreadpool.c.
   *
   * The __chttpsvr_destroy function has no ccol_retval_t of its own to
   * report this through. Its documented contract is a live handle in, or a
   * ccol_fatal_err on misuse. The code therefore treats a detected self-call
   * exactly like the stale-handle case immediately above. It makes a loud,
   * immediate ccol_fatal_err() call that names the real problem. The
   * alternative is a silent hang and then an abort whose stack trace points
   * somewhere else entirely. This mirrors the identical self-destroy guards
   * of __ctpool_destroy and __ccol_event_loop_destroy. */
  if (_chttpsvr_is_self_call(raw)) {
    ccol_mutex_unlock(chttpsvr_slot_table.mutex);
    ccol_fatal_err(
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
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);

  ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->pending_resolve_count) > 0)
    ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol_mutex_unlock(raw->mutex);

  _quiesce_server_once(raw);

  /* This waits for a concurrent _engine_force_stop_quiesce_all pass. Such a
   * pass took a bare pointer to raw directly out of
   * servers_bundler.servers[]; see the doc comment of that function. It must
   * finish with raw before this function frees one byte of raw below.
   *
   * The wait sits after the return of _quiesce_server_once on purpose. The
   * code does not fold it into the pending_resolve_count wait above. Neither
   * _quiesce_server_once nor _chttpsvr_stop_internal ever touches
   * servers_bundler_pins. See the comment of that field for the
   * self-deadlock that a wait on it from inside that call would cause. No
   * ordering rule therefore forces this wait to happen any earlier than
   * "sometime before the library frees raw".
   *
   * The listener_dispatch_pins check beside it waits out at most one
   * listener dispatch that started before the chttpsvr_stop inside
   * _quiesce_server_once above and pinned only after it. Such a dispatch
   * finds listen_fd cleared and returns at once, without touching anything
   * that this function frees below. The memory of raw does not depend on
   * this wait: the listener registration holds a raw->lifetime_refs
   * reference until its on_removed fires. */
  ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->servers_bundler_pins) > 0 ||
         atomic_load(&raw->listener_dispatch_pins) > 0)
    ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol_mutex_unlock(raw->mutex);

  if (raw->tls_ctx) {
    ctls_ctx_release(raw->tls_ctx);
    raw->tls_ctx = NULL;
  }

  /* The real teardown of raw covers its mutexes, its own memory and its
   * slot. It runs only after one condition holds. Every connection that this
   * server ever accepted must have every one of its own registrations proven
   * safe to release. See the field comment of raw->lifetime_refs for the
   * full reason, and see _chttpsvr_finish_destroy for the deferred teardown
   * itself.
   *
   * This decrement is the "done with raw" vote of the application. There is
   * exactly one of them for each raw. The code reaches __chttpsvr_destroy
   * only one time for each handle. A second call, concurrent or later,
   * already got a ccol_fatal_err above, through the in_use guard of the
   * slot. The registration of some connection can still be pending reclaim.
   * This is then not the last contribution, and _conn_on_removed does the
   * real final teardown when it becomes the last one. */
  if (atomic_fetch_sub(&raw->lifetime_refs, 1) == 1)
    _chttpsvr_finish_destroy(raw);
}

/* This is the real final teardown of raw. It destroys the mutexes, the
 * condition variables and the rwlock of raw. It closes the logger of raw. It
 * frees the memory of raw and releases its slot.
 *
 * The code defers it out of __chttpsvr_destroy itself; see the field comment
 * of raw->lifetime_refs. It can therefore run from either of two places.
 * Whichever one turns out to be the last contributor to that count runs it.
 * The first place is the tail of __chttpsvr_destroy above. The second is
 * _conn_on_removed, possibly well after the caller of __chttpsvr_destroy
 * already returned.
 *
 * This function touches the mutexes of raw, raw->m_procs and
 * raw->self_slot_idx. Every one of those fields is still valid at the point
 * where this runs, from either caller. Nothing freed raw yet, by
 * construction, because that is what this very function is about to do. The
 * code writes self_slot_idx one time, when it acquires the handle, and never
 * again. */
#ifdef RUNNING_UNIT_TESTS
/* This is white-box test instrumentation and nothing else. A test names one
 * server through its handle, and _chttpsvr_finish_destroy records when it
 * runs for exactly that server. The test can then tell whether the final
 * free of that server already happened, whatever other servers of the
 * process do at the same time. */
static _Atomic(struct chttpserver *) g_finish_destroy_watch_for_tests = NULL;
/* Counts the teardowns of the route data of a server that found a connection
 * of that server still allocated. Every such connection can still read its
 * matched route, so the count must stay 0. */
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

static void _chttpsvr_finish_destroy(struct chttpserver *raw) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_finish_destroy_watch_for_tests) == raw) {
    atomic_store(&g_finish_destroy_watch_for_tests, NULL);
    atomic_store(&g_finish_destroy_watch_hit_for_tests, true);
  }
#endif /* RUNNING_UNIT_TESTS */
  /* The routers and their routes and middleware go here, and not in
   * __chttpsvr_destroy. A connection that a slow handler still owns when the
   * bounded drain of the destroy gives up keeps reading its matched route
   * (the handler, the middleware chain, the parameter names and count) until
   * _conn_free runs from _conn_on_removed. Every such connection holds a
   * raw->lifetime_refs reference, so this is the first point at which no
   * connection can reach the route data any more.
   *
   * No chttpsvr_router_on, _on_stream or _use call can reach the routers
   * either. Each one resolves the owner handle of the router through the
   * slot table, and __chttpsvr_destroy cleared the in_use flag of that slot
   * and drained every pinned resolve before it gave up its reference. The
   * _destroy_router function never frees the shell of a router. It frees
   * only the contents, after it atomically invalidates owner; see the
   * comment of chttpsvr_router_shell_registry. */
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&raw->current_connections) > 0)
    atomic_fetch_add(&g_route_teardown_with_live_connections_for_tests, 1);
#endif /* RUNNING_UNIT_TESTS */
  for (size_t i = 0; i < raw->router_count; i++) {
    _destroy_router(raw->routers[i], raw->m_procs);
  }
  _ccol_mem_free(raw->m_procs, raw->routers);

  ccol_mutex_destroy(raw->mutex);
  ccol_mutex_destroy(raw->idle_mutex);
  ccol_mutex_destroy(raw->diverted_mutex);
  ccol_mutex_destroy(raw->wait_mutex);
  ccol_cond_var_destroy(raw->requests_done_cv);
  ccol_cond_var_destroy(raw->resolve_cv);
  ccol_cond_var_destroy(raw->quiesce_done_cv);
  ccol_rw_lock_destroy(raw->routes_lock);

  clog_close(raw->cl);
  raw->cl = CLOG_INVALID;

  uint32_t idx = raw->self_slot_idx;
  ccol_memmgmt_procs_t *mp = raw->m_procs;
  _ccol_mem_free(mp, raw);
  _ccol_mem_free(mp, mp);

  /* Release the slot last, only after the teardown and the free of raw are
   * complete. The bump of the generation of the slot, and the push back of
   * the free index, are what mark the handle as reusable. No earlier step
   * does that.
   *
   * The code fetches the slot again by idx. It does not cache a slot pointer
   * across the gap since it last touched raw. A concurrent
   * ccol_create_chttpsvr_mp call can run its own
   * _chttpsvr_handle_slot_acquire in between. That call can allocate the
   * backing array of the slots again, with cvector_push_back. Any pointer
   * into that array that the code took earlier is then invalid. idx itself
   * is stable. */
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot_t *slot2 =
      (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++; /* bumps this slot's generation past whatever value
      the just-freed srv's handle carried, so that stale handle can never
      again match a FUTURE acquire's generation for this same index */
  cvector_push_back(chttpsvr_slot_table.free_indices, &idx);
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

/* The destructor below frees the bookkeeping arrays of the slot table at
 * the exit of the process. Therefore, the --show-leak-kinds=all of make memtest
 * does not report them as still reachable. The destructor does this ONLY
 * when no chttpsvr handle is in use.
 *
 * This module has no singleton server that the library owns and destroys
 * first. It has no equivalent of the chttp_default_client of chttpclient.c,
 * which _cleanup_default_client destroys there. That fact means only one
 * thing: this destructor has no automatic handle to destroy for an
 * application. It does not make it safe to free the slot table while a
 * handle that an application made is live.
 *
 * This library does not control the order of __attribute__((destructor))
 * functions and atexit handlers across the shared objects of a process. An
 * application can let the exit of the process reclaim a chttpsvr that it
 * made directly, and not call chttpsvr_destroy. Such an application can
 * have a live handle that a destructor or an atexit handler touches after
 * this destructor runs. Each chttpsvr_start, _stop, _register_handler,
 * _use, _subrouter or _destroy call does that, because each one resolves
 * through this slot table.
 *
 * If the destructor frees the shared slot table under a live handle, the
 * result is a use-after-free. If the destructor does not free it, the
 * result is the same leak that this library accepts: "the caller never
 * destroyed their server". That leak then also includes the bookkeeping
 * arrays of the slot table. The _cleanup_default_client guard of
 * chttpclient.c does the same for the same class of hazard.
 *
 * An __attribute__((destructor)) function runs for the full shared object,
 * whatever parts of it the process used. In a process that links this
 * library and never makes a chttpsvr, the mutex here is not initialized.
 * Therefore, the destructor returns immediately when no thread entered the
 * initialization. Otherwise, it calls ccol_call_once before it locks. */
/* Says whether this slot still belongs to a server. The library must not
 * release the table out from under such a server.
 *
 * The test reads slot->ptr, and not slot->in_use. The code clears in_use as
 * the FIRST step of __chttpsvr_destroy. A second destroy, or a new resolve,
 * is then rejected as early as possible. The whole rest of that teardown
 * runs afterward. That rest quiesces the server, drains the pins, destroys
 * the routers, and finally reaches the locked release of this very slot in
 * _chttpsvr_finish_destroy. A scan that trusted in_use alone frees this
 * table out from under a destroy that is still inside that window. The last
 * step of that destroy then indexes the table and writes to it.
 *
 * The code writes ptr only after it fully acquires a slot. It clears ptr
 * only in that final locked step. ptr is therefore true for exactly as long
 * as the library must not release the table. This mirrors
 * _chttpcli_any_slot_live_locked of chttpclient.c exactly. */
static bool _chttpsvr_slot_live(const chttpsvr_slot_t *slot) {
  return slot->ptr != NULL;
}

#ifdef RUNNING_UNIT_TESTS
/* These are white-box test hooks only. A gate keeps both symbols out of a
 * production build, in the same way as every other white-box helper in this
 * file. Together they let a test build the mid-teardown window above, where
 * in_use is already cleared and ptr is not. The test can then ask the
 * predicate of the exit-time destructor what it makes of that state. Nothing
 * else reaches that state without a win in a race against a real teardown in
 * progress. */
bool _chttpsvr_slot_live_for_tests(chttpsvr h) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  bool live = false;
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots))
    live = _chttpsvr_slot_live(
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx));
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return live;
}

void _chttpsvr_slot_set_in_use_for_tests(chttpsvr h, bool in_use) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots))
    ((chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx))->in_use =
        in_use;
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}
#endif /* RUNNING_UNIT_TESTS */

__attribute__((destructor)) static void _cleanup_chttpsvr_slot_table(void) {
  /* A process that never touched this module has nothing to release here.
   * If this destructor ran the initialization, it would make the slot
   * tables of clogger and cthreadpool after their own destructors can have
   * run. Nothing would free those tables. A thread that entered the
   * initialization set the flag first. Therefore, the ccol_call_once below
   * waits until that initialization is complete. */
  if (!atomic_load(&chttpsvr_slot_table_entered)) return;
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  bool any_slot_live = false;
  size_t slot_count = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++) {
    if (_chttpsvr_slot_live(
            (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, i))) {
      any_slot_live = true;
      break;
    }
  }
  if (!any_slot_live) {
    __cvector_destroy(chttpsvr_slot_table.slots);
    __cvector_destroy(chttpsvr_slot_table.free_indices);
    /* No server is left, so no worker thread of any server is left either:
     * the final teardown of a server releases its slot only after its pools
     * joined their threads. The worker key therefore has no user, and a
     * dlclose() of the library does not leak it. A later call finds `ready`
     * false and treats the key as absent. */
    ccol_call_once(chttpsvr_worker_key_bundle.once,
                   _chttpsvr_worker_key_init_globals);
    ccol_mutex_lock(chttpsvr_worker_key_bundle.mutex);
    if (atomic_exchange(&chttpsvr_worker_key_bundle.ready, false))
      ccol_thread_ls_key_delete(chttpsvr_worker_key_bundle.key);
    ccol_mutex_unlock(chttpsvr_worker_key_bundle.mutex);
  }
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
}

ccol_retval_t chttpsvr_start(chttpsvr h, const chttpsvr_config_t *cfg) {
  /* This validation of the arguments alone stays here, before any resolve.
   * The identical certificate and key pair check of chttpclient_set_tls sits
   * in the same place. It never touches srv or raw, so it needs no pin and
   * unpin of its own. */
  if (cfg && cfg->port == 0 &&
      !(cfg->host && strncmp(cfg->host, _CHTTPSVR_UNIX_PREFIX,
                             strlen(_CHTTPSVR_UNIX_PREFIX)) == 0))
    return ccol_invalid_args;
  /* A server certificate and its private key are a pair. Exactly one of the
   * two is never a valid configuration. The TLS setup further down this
   * function tries TLS only when BOTH cert_path and key_path are not NULL.
   * That is the requirement of ctls_ctx_cert_add. A silent reading of a lone
   * cert_path or key_path as "no TLS configured" starts this server as
   * plain, unencrypted HTTP. The caller believes that port is HTTPS, and
   * nothing reports an error anywhere. This mirrors the identical guard of
   * chttpclient_set_tls, and the comment above that refers to it.
   *
   * The identical hazard exists for ca_bundle_path alone, with no cert_path
   * and key_path pair. The TLS setup block further down looks at
   * ca_bundle_path only inside the `cert_path && key_path` branch. A caller
   * that sets only ca_bundle_path therefore gets the same silent downgrade
   * to plain HTTP, and not an error. That is the ordinary, everyday way to
   * configure verification against a custom CA on the client side, with
   * chttpclient_set_tls. The --cacert of curl, the TLSClientConfig.RootCAs
   * of Go, and the ssl.create_default_context(cafile=...) of Python all work
   * this same way, with no client certificate involved. No reference server
   * implementation offers a way to configure a TLS server with a trust store
   * and no identity certificate of its own. The ListenAndServeTLS of Go, the
   * https.createServer of Node, and the ssl_certificate and
   * ssl_certificate_key of nginx are such implementations. The library
   * therefore rejects that shape in the same way.
   *
   * The same silent downgrade also happens when cert_path and key_path are
   * BOTH absent, ca_bundle_path included. One example is a cfg->tls that
   * points at CHTTP_TLS_DEFAULT (chttp.h), which names no path at all. That
   * macro is documented as the shared defaults, with verification on, for
   * both chttpclient and chttpsvr. A caller can
   * reasonably reach for it here and forget to set cert_path and key_path
   * afterward. That caller hits the exact hazard that this whole check
   * exists to prevent.
   *
   * The check demands that both are not NULL whenever cfg->tls is not NULL.
   * One condition therefore covers all three rejected shapes above. Those
   * are a lone cert_path, a lone key_path, and a ca_bundle_path with
   * neither. The code does not list each combination separately. */
  if (cfg && cfg->tls && !(cfg->tls->cert_path && cfg->tls->key_path))
    return ccol_invalid_args;
  /* CHTTPSVR_STREAMING_POOL_OFF is the one negative value that
   * streaming_thread_count names. Any other negative value is a mistake, and
   * not a quiet synonym of it. */
  if (cfg && cfg->streaming_thread_count < CHTTPSVR_STREAMING_POOL_OFF)
    return ccol_invalid_args;

  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;

  /* A server whose workers cannot mark themselves cannot detect a destroy,
   * a restart or an engine wait from its own handlers; see the comment of
   * chttpsvr_worker_key_bundle. It does not start at all. */
  if (!_chttpsvr_worker_key_ensure()) {
    _SRV_ENGINE_LOG(ccol_log_error,
                    "chttpsvr_start: no thread-specific key is available "
                    "(the process holds PTHREAD_KEYS_MAX keys)");
    _chttpsvr_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }

#ifdef RUNNING_UNIT_TESTS
  _start_resolve_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* This is the self-call case. See the comment of
   * chttpsvr_worker_key_bundle above. Take a request handler, or a
   * middleware, that runs on the worker_pool or the reject_pool of raw. It
   * calls chttpsvr_start() to restart the very server that it runs on. The
   * restart would retire the pool that runs this very call, and a failure
   * of the restart would leave the calling handler on a server that is
   * stopped under it. The documented contract is therefore that a handler
   * never restarts its own server, and a thread outside the handlers does.
   *
   * Unlike __chttpsvr_destroy, this function has a real ccol_retval_t to
   * report through. A plain refusal of the restart leaves the server in a
   * perfectly safe state. It still runs its old configuration. If the
   * handler already called chttpsvr_stop() first, it is simply stopped.
   * Either way nothing is corrupted. A later, legitimate call from a
   * different thread can still restart it normally. The library therefore
   * reports this gracefully, instead of an abort of the process.
   *
   * The check is harmless for the very first chttpsvr_start() call of a
   * server. No worker thread of this server can exist yet to be marked. */
  if (_chttpsvr_is_self_call(raw)) {
    _chttpsvr_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  chttpsvr_config_t default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  ccol_mutex_lock(raw->mutex);
  for (;;) {
    /* Exhaustive switch (no `default:` label), for the identical
     * `-Wswitch` reason chttpsvr_lifecycle_t's own field comment explains.
     * CHTTPSVR_LC_STOPPING backs off and retries below; CHTTPSVR_LC_IDLE
     * is the only value that falls through past this switch to the second
     * one below it. */
    switch (raw->lifecycle) {
      case CHTTPSVR_LC_STARTING:
      case CHTTPSVR_LC_RUNNING:
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 1 */
        return ccol_not_permitted;
      case CHTTPSVR_LC_STOPPING: {
        /* A concurrent _chttpsvr_stop_internal() call on this exact handle
         * can still be in flight. It comes from a different thread. One
         * caller that does a stop() and then a start() serially never
         * observes this. chttpsvr_stop() does not return until its own
         * teardown work is complete. That work includes the exit from this
         * state. A plain chttpsvr_stop() call can drive that stop,
         * and so can the first call of a _quiesce_server_once pass into it.
         *
         * The quiesce_state pass that the code checks below is different.
         * Its own SEPARATE, later teardown steps wait for
         * pending_resolve_count to reach 0 as their own precondition. The
         * _chttpsvr_stop_internal() function itself never waits on
         * pending_resolve_count anywhere in its body. It waits only on
         * listener_dispatch_pins. See the field comment of
         * chttpsvr_lifecycle_t for the spurious race of the EADDRINUSE class
         * that this state closes.
         *
         * This call therefore deliberately keeps its own resolve pin. The
         * quiesce_state wait below differs, because it must release the pin
         * first. Without that release it hits a real deadlock across two
         * condition variables, against the pending_resolve_count wait of
         * _quiesce_server_once. To hold the pin here cannot block the
         * forward progress of _chttpsvr_stop_internal(), because nothing
         * that it does waits on this counter. The pin also keeps raw itself
         * alive across this wait, so nothing has to resolve it again
         * afterward.
         *
         * The code uses a genuine condition variable wait here. The
         * resolve_cv broadcast of _chttpsvr_stop_internal() wakes it, right
         * after that function sets CHTTPSVR_LC_IDLE. The code does not use a
         * blind nanosleep and repoll at a fixed interval. This function also
         * performs the structurally identical quiesce_state and "currently
         * stopping" backoffs; see the comment of each one. Such a poll
         * measurably busy-loops thousands of times a second under load. This
         * branch has the identical structural shape, which is to release the
         * pin, sleep, resolve again and retry. It therefore carries the
         * identical risk.
         *
         * This exact wait can be reached while a _quiesce_server_once pass
         * is the one that drives _chttpsvr_stop_internal. The wake then
         * observes CHTTPSVR_LC_IDLE and falls through to the quiesce_state
         * switch below, on the next iteration of the loop. That switch
         * releases this pin correctly there before it waits further. Nothing
         * here holds the pin longer than that one extra, bounded loop
         * back. */
#ifdef RUNNING_UNIT_TESTS
        _start_stopping_wait_signal_fire_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        while (raw->lifecycle == CHTTPSVR_LC_STOPPING)
          ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
        continue;
      }
      case CHTTPSVR_LC_IDLE:
        break;
    }
    /* A _quiesce_server_once pass for this exact server can already be
     * running. Two things drive such a pass. The first is a concurrent
     * force-stop from chttpsvr_engine_stop(), through
     * _engine_force_stop_quiesce_all. That function reads raw straight out
     * of servers_bundler.servers[] and calls _quiesce_server_once directly.
     * It never goes through _chttpsvr_resolve or pending_resolve_count; see
     * the doc comment of that function. The second is a chttpsvr_destroy()
     * call of another thread that races this one.
     *
     * The _chttpsvr_stop_internal function leaves CHTTPSVR_LC_RUNNING. It is
     * only the very first step of that pass. It runs long before the
     * _drain_and_close_all_connections and _engine_release work of the pass
     * finishes, and that work can be slow. Only then does quiesce_state
     * reach CHTTPSVR_QS_QUIESCED. That pass never pins raw. The
     * pending_resolve_count wait that _quiesce_server_once does on ITS OWN
     * entry therefore cannot see the pin of this call either way. A
     * raw->lifecycle that reads CHTTPSVR_LC_IDLE above is therefore no proof
     * that no teardown is still in flight.
     *
     * The wait here is for any such pass in progress to fully finish. The
     * code does not go on to reset quiesce_state blindly below. That wait is
     * what closes the gap. Without it, this call can stomp that state out
     * from under the single-winner serialization of the pass in progress;
     * see the doc comment of _quiesce_server_once. Two outcomes follow. A
     * second, concurrent quiesce of the same raw can run beside the first.
     * That is a use-after-free with a destroyed mutex in use. A racing
     * chttpsvr_destroy() goes straight from the return of that second pass
     * into a free of raw. Or a later, unrelated rescan by
     * _engine_force_stop_quiesce_all silently quiesces a server. That server
     * is the very one that this call is in the middle of a restart of. The
     * rescan thereby kills it.
     *
     * The code must NOT wait on raw->quiesce_done_cv while it still holds
     * the resolve pin of this call. That deadlocks. The
     * pending_resolve_count wait of _quiesce_server_once must reach 0 before
     * that pass can do ANY of its real work. That work includes the work
     * that would eventually reach CHTTPSVR_QS_QUIESCED and broadcast this
     * condition variable. That wait counts the pin that this call still
     * holds. A pinned wait here therefore makes _quiesce_server_once wait
     * forever for a pin that only WE can release. We wait forever for a
     * signal that only IT can send. That is a genuine circular deadlock
     * across two condition variables, and not a rare corner case. An
     * entirely ordinary chttpsvr_stop() then chttpsvr_start() restart, which
     * races a concurrent chttpsvr_engine_stop(), reaches it.
     *
     * The code prevents that. It fully releases the pin before it backs off,
     * with the same _chttpsvr_resolve_unpin that every other exit path
     * already uses. It then resolves the original handle again from scratch,
     * after the pass in progress has plausibly finished.
     *
     * The second resolve, instead of a further touch of the same raw pointer
     * directly, is what makes this safe. raw itself can be freed
     * concurrently while this call holds no pin at all. One example is a
     * racing chttpsvr_destroy() that completes its own teardown and free,
     * which is now unblocked, while we are backed off here. The
     * generation-checked slot table then does one of two things. It hands
     * back the same, still-live raw, and the loop runs again. Or it
     * correctly reports the handle as gone, with ccol_invalid_args. It never
     * hands back a stale pointer.
     *
     * The switch below is exhaustive and has no `default:` label. The
     * `-Wswitch` reason is the identical one above. */
    switch (raw->quiesce_state) {
      case CHTTPSVR_QS_QUIESCING:
        /* This joins the exact same winner and loser protocol that the
         * CHTTPSVR_QS_QUIESCING and _QUIESCED branch of
         * _quiesce_server_once already uses. The code increments
         * quiesce_waiters before it waits. It decrements it and broadcasts
         * after it wakes. The tail wait of the winner for
         * quiesce_waiters == 0 therefore accounts for this call too. See the
         * doc comment of that function.
         *
         * The code does not use a blind poll at a fixed interval. This
         * function performs the structurally identical CHTTPSVR_LC_STOPPING
         * and "currently stopping" backoffs elsewhere. Such a poll
         * measurably busy-loops thousands of times a second. It also starves
         * the condition variable wakeup of the real teardown thread. That
         * wakeup then never wins the race against a much cheaper second pin.
         * See the "currently stopping" branch further below, which blocks on
         * a condition variable for the same reason.
         *
         * The code MUST increment quiesce_waiters here. It must do so in the
         * same critical section that just observed CHTTPSVR_QS_QUIESCING,
         * and strictly before it releases the resolve pin of this call
         * below. It must never do so after it takes raw->mutex again. An
         * increment after the release is a use-after-free. A release of the
         * pin first can at once unblock the pending_resolve_count wait of
         * _quiesce_server_once. The winner then runs its entire real
         * teardown, which is _chttpsvr_stop_internal,
         * _drain_and_close_all_connections and _engine_release. It reaches
         * its own tail check of "while quiesce_waiters > 0" before this call
         * ever gets back to raw->mutex. It finds quiesce_waiters still zero
         * and returns. __chttpsvr_destroy then goes straight to
         * ccol_mutex_destroy(raw->mutex) and a free of raw. Nothing is left
         * to protect the ccol_mutex_lock(raw->mutex) that this call makes
         * below. That lock then runs on memory that something already freed.
         *
         * An increment while the code still holds the lock that observed
         * CHTTPSVR_QS_QUIESCING closes this. The winner cannot move to
         * CHTTPSVR_QS_QUIESCED until this increment is already visible to
         * it, because that move itself needs raw->mutex. quiesce_waiters is
         * therefore provably nonzero for as long as the winner takes to
         * reach its own tail check. That holds whatever the real schedule of
         * the two threads is. It is what keeps raw alive across the
         * resolve_unpin call and the wait loop below.
         *
         * It is safe to wait directly on raw here, for exactly that reason.
         * The process-wide "currently stopping" condition below must resolve
         * again first, and this branch does not. The code still resolves
         * from the handle afterward, instead of a further direct use of this
         * same raw pointer. That matches every other backoff branch in this
         * function. The slot table, and not raw itself, is what proves
         * whether srv is still a live, startable handle by the time this
         * wait finishes. */
        raw->quiesce_waiters++;
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw);
#ifdef RUNNING_UNIT_TESTS
        _start_quiescing_unpinned_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
        ccol_mutex_lock(raw->mutex);
        while (raw->quiesce_state != CHTTPSVR_QS_QUIESCED)
          ccol_cond_var_wait(raw->quiesce_done_cv, raw->mutex);
        raw->quiesce_waiters--;
        ccol_cond_var_broadcast(raw->quiesce_done_cv);
        ccol_mutex_unlock(raw->mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        ccol_mutex_lock(raw->mutex);
        continue;
      case CHTTPSVR_QS_NOT_QUIESCED:
      case CHTTPSVR_QS_QUIESCED:
        break;
    }
    /* There is no `break;` here. The scope of this `for (;;)` loop extends
     * all the way to the end of the function. That is what lets the
     * "currently stopping" branch far below, inside the
     * `if (need_acquire) {...}` block, use `continue;` to go back to the
     * top. That scope affects no `return` statement anywhere in the rest of
     * this function. A `return` exits the function whatever the loop nesting
     * is. Control simply falls through to the claim below once there is
     * nothing left to check. */
    /* The code claims the state here, under the same lock and in the same
     * critical section as the checks above. A second, concurrent
     * chttpsvr_start() call on this same handle therefore cannot slip past
     * both checks before this one sets it. See the field comment of
     * chttpsvr_lifecycle_t for what that race would otherwise corrupt. It
     * gives a use-after-free of raw->tls_ctx, and it drops worker pools.
     * Every return point below resets this field to CHTTPSVR_LC_IDLE under
     * raw->mutex. */
    raw->lifecycle = CHTTPSVR_LC_STARTING;
    /* An earlier stop or quiesce cycle can have left quiesce_state at
     * CHTTPSVR_QS_QUIESCED. That cycle can be a chttpsvr_stop() plus
     * chttpsvr_start() restart. It can also be a chttpsvr_engine_stop()
     * force-stop that this server survived with no chttpsvr_destroy() call.
     * This server now legitimately starts fresh again. The
     * _quiesce_server_once function must therefore be ready to run its real
     * teardown work again, the next time something really destroys this
     * server.
     *
     * An unconditional reset is safe here. The loop above already guarantees
     * that no pass with quiesce_state == CHTTPSVR_QS_QUIESCING is still in
     * flight at this exact point. That guarantee holds under the same
     * raw->mutex critical section, which the code holds without a break. */
    raw->quiesce_state = CHTTPSVR_QS_NOT_QUIESCED;
    ccol_mutex_unlock(raw->mutex);

    /* Retired pool sets of earlier restarts whose drain finished are joined
     * here too, and not only by the sweep. */
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
    ctpool new_pool = ccol_create_cthread_pool_mp((size_t)nthreads, queue_cap,
                                                  raw->m_procs, &pool_err);
    if (!new_pool) {
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return ccol_not_enough_memory;
    }

    /* The thread count of reject_pool scales with the resolved size of
     * worker_pool. That size is nthreads above. It is already the value that
     * the code resolved from the CPU count when cfg->worker_thread_count is
     * 0 or less. It is not the raw config field, which can be 0 or less. The
     * count is not a fixed constant. _CHTTPSVR_REJECT_POOL_MIN_THREADS is
     * its floor. A server with one worker, or a few, therefore still gets
     * real concurrency for its rejection traffic. See the comment of that
     * constant for the reason.
     *
     * The queue is bounded, at _CHTTPSVR_REJECT_POOL_QUEUE_CAP. See the
     * field comment of reject_pool, and the comment of that constant, for
     * why. When this creation fails, the code must tear new_pool above down
     * too, and not leak it. */
    int reject_nthreads = nthreads / 2;
    if (reject_nthreads < _CHTTPSVR_REJECT_POOL_MIN_THREADS)
      reject_nthreads = _CHTTPSVR_REJECT_POOL_MIN_THREADS;
    char *reject_pool_err = NULL;
    ctpool new_reject_pool = ccol_create_cthread_pool_mp(
        (size_t)reject_nthreads, _CHTTPSVR_REJECT_POOL_QUEUE_CAP, raw->m_procs,
        &reject_pool_err);
    if (!new_reject_pool) {
      ctpool_shutdown_drain(new_pool);
      ctpool_destroy(new_pool);
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
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
      /* The slow-client settings read 0 as "the default", and turn off only
       * through their own named constants, so that a configuration that
       * never mentions them keeps every defense. The named constants are the
       * maximum values of their types, and they pass through unchanged as
       * the "off" or "no limit" state of the resolved field. */
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
      ccol_mutex_lock(raw->wait_mutex);
      raw->mem_admission_closed = false;
      ccol_mutex_unlock(raw->wait_mutex);
    }

    /* The fresh pools replace the pools of the previous run, if any, in one
     * critical section, so that a connection that stays open across the
     * stop always finds a pool. The previous pools drain in the background;
     * see _retire_pools. A restart therefore neither waits for, nor cuts
     * off, a request of the previous run that still runs. The settings of
     * this start are all stored above, before the swap: a request that
     * finds a new pool through srv->mutex therefore also sees the new
     * settings, and a first streaming request creates its pool with the new
     * streaming_thread_count and never with the old one. */
    ccol_mutex_lock(raw->mutex);
    _detached_pools_t previous = {raw->worker_pool, raw->reject_pool,
                                  raw->stream_pool};
    raw->worker_pool = new_pool;
    raw->reject_pool = new_reject_pool;
    raw->stream_pool = CTPOOL_INVALID;
    ccol_mutex_unlock(raw->mutex);
#ifdef RUNNING_UNIT_TESTS
    _start_hold_for_tests();
#endif
    _retire_pools(raw, previous);

    if (raw->tls_ctx) {
      ctls_ctx_release(raw->tls_ctx);
      raw->tls_ctx = NULL;
    }
    if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
      ctls_ctx_t *tls_ctx = ctls_ctx_new_mp(raw->m_procs, NULL);
      if (!tls_ctx) {
        _retire_current_pools(raw);
        ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 4 */
        return ccol_not_enough_memory;
      }
      if (ctls_ctx_cert_add(tls_ctx, NULL, cfg->tls->cert_path,
                            cfg->tls->key_path, NULL, NULL) != ccol_success) {
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 5 */
        return ccol_unexpected_failure;
      }
      /* The mode goes onto the context before the trust store, so the one
       * rebuild that ctls_ctx_trust() runs already builds with it. A strict
       * context, the default, needs no call at all. */
      if (cfg->tls->client_cert_optional &&
          ctls_ctx_peer_cert_optional(tls_ctx, true) != ccol_success) {
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 6 */
        return ccol_unexpected_failure;
      }
      if (cfg->tls->ca_bundle_path &&
          ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL) !=
              ccol_success) {
        /* The code must not silently read a CA bundle that it cannot read,
         * or that is malformed, as "no CA bundle configured". A successful
         * ctls_ctx_trust() call is what turns SSL_VERIFY_PEER on for the
         * context that it makes. That is the verification of the client
         * certificate for mutual TLS. To discard this failure starts the
         * server with TLS, but without the client-certificate enforcement
         * that the caller explicitly asked for. Nothing reports an error
         * anywhere. This matches the sibling ctls_ctx_cert_add failure a few
         * lines above exactly. */
        ctls_ctx_release(tls_ctx);
        _retire_current_pools(raw);
        ccol_mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        _chttpsvr_resolve_unpin(raw); /* exit 6 */
        return ccol_unexpected_failure;
      }
      raw->tls_ctx = tls_ctx;
    }

    /* The code registers raw here, BEFORE the engine-acquire attempt of this
     * call runs, and not only after that attempt succeeds. This call already
     * holds its resolve pin for its whole duration. That pin is
     * raw->pending_resolve_count, from the _chttpsvr_resolve at the top of
     * this function.
     *
     * A register of raw now therefore lets a concurrent
     * _engine_force_stop_quiesce_all pass of chttpsvr_engine_stop() find raw
     * at once. That pass quiesces only the servers that it can find in
     * servers_bundler.servers. It then correctly blocks on the pin of this
     * call; see the pending_resolve_count wait of _quiesce_server_once. It
     * blocks before it releases the shared reactor and possibly tears it
     * down. This holds even when that race lands in the narrow window before
     * the _engine_acquire() call below runs.
     *
     * A register only after _engine_acquire() succeeds leaves exactly that
     * window open. A concurrent force-stop that lands there finds raw
     * entirely absent from servers_bundler.servers. It completes its own
     * scan and believes that every server was already quiesced. It can then
     * tear the reactor down while this call is still about to acquire and
     * use it. That appears as a spurious ccol_unexpected_failure from this
     * call, where ccol_event_loop_add fails against a reactor that is
     * already gone. It is not a crash, but it is a real race that the code
     * can avoid.
     *
     * The _servers_register function is idempotent. An unconditional call to
     * it here is therefore safe for every caller. That holds whether or not
     * this call goes on to need a fresh engine reference. It also holds for
     * a restart that already registered raw on an earlier chttpsvr_start()
     * call.
     *
     * When _engine_acquire() below fails, the failure path undoes this
     * registration, and does not leave it dangling; see exit 7 below. A
     * server that is unregistered and holds no engine reference is the
     * correct state for a call that never finished its start.
     *
     * A registration that fails, which only a failed growth of the array
     * does, fails the start: a server outside the array is never swept and
     * never quiesced before an engine force-stop tears the reactor down. */
    if (!_servers_register(raw)) {
      _SRV_ENGINE_LOG(ccol_log_error,
                      "chttpsvr_start: the server registry cannot grow");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 6b */
      return ccol_not_enough_memory;
    }

    bool need_acquire;
    ccol_mutex_lock(raw->mutex);
    need_acquire = !raw->contributed_to_engine;
    if (need_acquire) raw->contributed_to_engine = true;
    ccol_mutex_unlock(raw->mutex);

    if (need_acquire) {
#ifdef RUNNING_UNIT_TESTS
      _engine_stopping_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */
      bool currently_stopping = false;
      ccol_retval_t engine_rc = _engine_acquire(&currently_stopping);
      if (currently_stopping) {
        /* A reap tears the shared reactor down at this moment.
         * chttpsvr_engine_stop() forces that reap. A chttpsvr_destroy() call
         * of some other server can also trigger it gracefully, by dropping
         * reactor_refs to 0. The _engine_acquire() call returned at once. It
         * did not block for that reap, and it never incremented
         * reactor_refs. See the doc comment of that function for the
         * deadlock that this closes. A block there, while this call still
         * holds its resolve pin, is what a _quiesce_server_once pass for
         * this exact server would wait on. The reap itself is stuck behind
         * that same pass.
         *
         * The code unwinds exactly as the engine_rc != ccol_success path
         * below does. This is deliberately the same template as exit 7. It
         * is not the _chttpsvr_undo_start_registration() helper that exits 8
         * and 9 use. Those exits run after _engine_acquire() already
         * succeeded and genuinely incremented reactor_refs. To undo them
         * means to really release a reference. This branch never incremented
         * it, so there is nothing to release. */
        ccol_mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        _servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        _retire_current_pools(raw);
        _chttpsvr_resolve_unpin(raw);
        /* This is a genuine condition variable wait. It matches the
         * quiesce_state and lifecycle checks above. A blind poll and retry
         * is "nanosleep(1ms), resolve again, continue". For this exact class
         * of backoff, it measurably busy-loops thousands of times a second
         * under load. See the comment of each one.
         *
         * A blind retry loop here also has a second problem. It can out-race
         * the thread of _quiesce_server_once for this exact server. That
         * thread needs a brief window to notice that pending_resolve_count
         * reached 0 and to make real progress. In that window the pin of
         * this call sits at 0 for a moment, immediately after the
         * _chttpsvr_resolve_unpin above. A retry that is instant registers
         * and pins again before the thread of the reaper is ever scheduled.
         * That is a genuine starvation bug, and not merely a theoretical
         * one.
         *
         * The wait is on srv_engine_bundler.stopped_cv. That is a
         * process-wide condition variable. The other two waits use
         * resolve_cv and quiesce_done_cv, which are scoped to raw. The
         * reason for the difference is that raw itself may already be freed
         * by the time the reap that this call waits on finishes. Nothing
         * scoped to raw is therefore safe to wait on here. This call ALREADY
         * fully released its own pin above. That pin is the one and only
         * thing that the reap could have been waiting on. A wait on a
         * process-wide condition variable, with no pin held, is therefore
         * safe. It carries no risk of a second deadlock against a pin that
         * only this call can release.
         *
         * The library broadcasts stopped_cv after the whole reap completes,
         * and not after the quiesce of this one server. That is the
         * condition that this wait needs. It is safe to wait on here
         * precisely because the code holds no pin across it. */
        ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
        ccol_mutex_lock(srv_engine_bundler.mutex);
        while (srv_engine_bundler.stopping)
          ccol_cond_var_wait(srv_engine_bundler.stopped_cv,
                             srv_engine_bundler.mutex);
        ccol_mutex_unlock(srv_engine_bundler.mutex);
        raw = _chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        ccol_mutex_lock(raw->mutex);
        continue;
      }
      if (engine_rc != ccol_success) {
        ccol_mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol_mutex_unlock(raw->mutex);
        /* Undo the speculative registration above. This call never acquired
         * an engine reference. raw must therefore not stay visible to
         * _engine_force_stop_quiesce_all as if it held one. This branch is
         * reachable only when need_acquire was true. That means that raw did
         * not already hold a reference, and that nothing had registered it,
         * before this call began. An unregister here can therefore never
         * undo a registration that an earlier, already successful
         * chttpsvr_start() call still depends on. */
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
      /* Two features of every registered server depend entirely on this
       * shared thread. Those are the enforcement of idle_timeout_ms and the
       * recovery of capacity for max_connections. To go on as if this call
       * had succeeded leaves both of them permanently broken for this
       * server, in silence. For the max_connections case it also leaves a
       * listener paused forever, with nothing left that can resume it, the
       * instant the server reaches capacity. chttpsvr_start() itself would
       * report success. This branch therefore fails exactly like the
       * _make_listen_socket failure a few lines below. It undoes whatever
       * this attempt already set up, and it reports a real error. */
      _SRV_ENGINE_LOG(ccol_log_error,
                      "idle-timeout sweep thread could not be started");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _chttpsvr_undo_start_registration(raw, need_acquire);
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 8 */
      return ccol_unexpected_failure;
    }

    bool is_unix = false;
    _chttpsvr_unix_bind_t unix_bind;
    int lfd =
        _make_listen_socket(cfg->host, cfg->port, cfg->enable_reuseport,
                            cfg->ipv6_only, &is_unix, &unix_bind, raw->m_procs);
    if (lfd < 0) {
      int listen_errno = errno;
      _SRV_ENGINE_LOG(
          ccol_log_error, "listen socket setup failed host=%s port=%u errno=%d",
          cfg->host ? cfg->host : "(any)", (unsigned)cfg->port, listen_errno);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      _retire_current_pools(raw);
      _chttpsvr_undo_start_registration(raw, need_acquire);
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 9 */
      return ccol_unexpected_failure;
    }

    /* The code publishes these fields BEFORE the ccol_event_loop_add call
     * below, and not after it. That call makes the registration live at
     * once. A connection can arrive in the window between the registration
     * and the write of these fields. Without this order, the library
     * dispatches such a connection to _listener_on_readable while it still
     * observes the default value of listen_fd before the start, which is -1.
     * See the field comment of listen_fd on struct chttpserver. The code
     * resets listen_fd back to -1 below when ccol_event_loop_add itself
     * fails. A failed start therefore never leaves listen_fd pointing at an
     * fd that this function is about to close. */
    atomic_store(&raw->listen_fd, lfd);
    atomic_store(&raw->is_unix_socket, is_unix);

    ccol_call_once(srv_parser_bundler.once, _init_parser_settings);
    char *reg_err = NULL;
    /* The registration holds one raw->lifetime_refs reference, which
     * _listener_on_removed releases; see the comment of that field. The code
     * takes it before the add, because the on_removed of a registration can
     * fire as soon as the registration exists (a teardown of the reactor).
     * The application reference that this call holds through its resolve
     * keeps the count above 0, so the release on a failed add never reaches
     * the final teardown. */
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
      ccol_mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol_mutex_unlock(raw->mutex);
      _chttpsvr_resolve_unpin(raw); /* exit 10 */
      return ccol_unexpected_failure;
    }

    ccol_mutex_lock(raw->mutex);
    raw->unix_bind = unix_bind;
    raw->listen_reg = lreg;
    raw->lifecycle = CHTTPSVR_LC_RUNNING;
    ccol_mutex_unlock(raw->mutex);

    /* raw was already registered above, right after the engine reference was
     * confirmed; nothing further to do here. */
    _chttpsvr_resolve_unpin(raw); /* exit 11 (success) */
    return ccol_success;
  } /* end of the for (;;) retry loop opened near the top of this function */
}

/* This is the real work of chttpsvr_stop. It works directly on a struct
 * chttpserver* that something already resolved.
 *
 * The work sits in its own function so that _quiesce_server_once can call it
 * directly. That function already holds a resolved raw pointer, which it got
 * from the registry of servers_bundler, and never from a chttpsvr handle at
 * all. It therefore does not go through the public chttpsvr_stop, which
 * resolves a handle.
 *
 * This split is critical, because chttpsvr is a value handle and not a
 * pointer. To pass a raw struct chttpserver* where a handle belongs fails to
 * compile. Worse, it can read the bit pattern of that pointer as a pair of a
 * slot index and a generation. This mirrors the split in
 * chttpclient.c between chttp_do_internal, which works on a struct
 * chttpclient*, and the public chttpclient_do wrapper, which resolves and
 * then calls. */
static void _chttpsvr_stop_internal(struct chttpserver *raw) {
  ccol_mutex_lock(raw->mutex);
  /* This switch is exhaustive and has no `default:` label. It is not a plain
   * `raw->lifecycle == CHTTPSVR_LC_RUNNING` comparison. The `-Wswitch`
   * reason is the identical one that the field comment of
   * chttpsvr_lifecycle_t explains. Every other reachable state correctly
   * resolves to was_started == false here. That includes a concurrent second
   * chttpsvr_stop() call that observes CHTTPSVR_LC_STOPPING. It also
   * includes a call that races a chttpsvr_start() in flight and observes
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
    /* The code sets this in the same critical section that leaves
     * CHTTPSVR_LC_RUNNING. The wait loop of chttpsvr_start() can therefore
     * never observe lifecycle == CHTTPSVR_LC_IDLE without having observed
     * CHTTPSVR_LC_STOPPING first. That holds for as long as the real
     * teardown work of this function below is still in flight. See the field
     * comment of chttpsvr_lifecycle_t for the race that this state
     * closes. */
    raw->lifecycle = CHTTPSVR_LC_STOPPING;
    atomic_store(&raw->listen_fd, -1);
    raw->listen_reg = CCOL_EVENT_REG_INVALID;
    raw->unix_bind.path = NULL;
    raw->unix_bind.dir_fd = -1;
  }
  ccol_mutex_unlock(raw->mutex);
  if (!was_started) return;

#ifdef RUNNING_UNIT_TESTS
  _stop_race_hook_wait_if_armed();
#endif /* RUNNING_UNIT_TESTS */

  /* After ccol_event_loop_remove returns, no NEW _listener_on_readable
   * dispatch can ever start for lreg. But that call does NOT wait for a
   * dispatch that is already in progress at the moment of the call. It never
   * takes the dispatch_lock of the registration. See the comment of
   * _ccol_event_loop_run_callback in cthreadcomm.c, and the doc comment of
   * ccol_event_loop_remove in cthreadcomm.h. The phrase "teardown is
   * deferred until any in-progress callback returns" there describes the
   * internal bookkeeping memory of ccol_event_loop. It is not a promise that
   * the calling thread here blocks for that callback.
   *
   * Without the wait below, a real race exists here. It is narrow, at a few
   * instructions. Those instructions load srv->listen_fd into a register and
   * then issue accept4() with it. A dispatch that is already past that load
   * can still be about to call accept4(). It uses the fd number that this
   * function is about to close. Some unrelated, concurrent open() in this
   * same process can then reuse that number.
   *
   * The code closes that race with a wait for listener_dispatch_pins to
   * reach 0. A dispatch that pinned before the clear of listen_fd above
   * holds this wait back until it returns. A dispatch that pins after that
   * clear compares the fd of its registration against listen_fd, finds a
   * mismatch, and never calls accept4(). The memory of raw is a separate
   * matter: the listener registration holds a raw->lifetime_refs reference
   * until its on_removed fires; see _listener_on_removed.
   *
   * That wait is safe here specifically. The _quiesce_server_once and
   * _chttpsvr_stop_internal functions must never wait on
   * servers_bundler_pins or pending_resolve_count; see the comment of each
   * field. listener_dispatch_pins differs. Only the reactor thread that runs
   * _listener_on_readable holds it. The thread that calls chttpsvr_stop(),
   * _quiesce_server_once or __chttpsvr_destroy never holds it. There is no
   * case where the thread that waits here is also the one that must run for
   * this counter to reach 0. No self-deadlock is therefore possible.
   *
   * The ccol_event_loop_remove() call already ran when this wait starts.
   * This wait can therefore only be for a dispatch that is live at that
   * exact moment, and never for a fresh one. There is at most one at a time.
   * The per-registration dispatch_lock of ccol_event_loop already guarantees
   * that the callback of a registration never runs concurrently with
   * itself. */
  if (lreg) ccol_event_loop_remove(srv_engine_bundler.reactor, lreg);
  ccol_mutex_lock(raw->mutex);
  while (atomic_load(&raw->listener_dispatch_pins) > 0)
    ccol_cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol_mutex_unlock(raw->mutex);
  close(lfd);
  _unix_bind_cleanup(&unix_bind, raw->m_procs);

  /* This write is unconditional, and not a switch. The code reaches this
   * point only on the path where was_started was already true. An early
   * `return` sits between this point and the false case. The retry loop of
   * chttpsvr_start() always backs off and retries for the whole time that
   * lifecycle == CHTTPSVR_LC_STOPPING. Nothing else can therefore change
   * raw->lifecycle away from CHTTPSVR_LC_STOPPING. That holds between the
   * claim at the top of the body of this function and this exit. A fork()
   * differs, because it can land at a genuinely arbitrary point with no such
   * guarantee; see the analogous fixup of _chttpsvr_atfork_release_impl.
   * This is one uninterrupted, synchronous call sequence with one writer
   * throughout.
   *
   * The code broadcasts resolve_cv. That is the same condition variable that
   * the listener_dispatch_pins wait of this function above already uses. A
   * concurrent chttpsvr_start() call that blocks in its own
   * CHTTPSVR_LC_STOPPING wait therefore wakes promptly; see the comment of
   * that branch. It does not discover this transition only on its next poll
   * tick. */
  ccol_mutex_lock(raw->mutex);
  raw->lifecycle = CHTTPSVR_LC_IDLE;
  ccol_cond_var_broadcast(raw->resolve_cv);
  ccol_mutex_unlock(raw->mutex);
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
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  clog old_user = srv_engine_bundler.user_log;
  clog old_fallback = CLOG_INVALID;
  srv_engine_bundler.user_log = derived;
  /* A running engine switches to the new logger at once. The fallback that
   * it ran with until now belongs to nobody else, so it closes too. */
  if (srv_engine_bundler.log) {
    if (srv_engine_bundler.log != old_user)
      old_fallback = srv_engine_bundler.log;
    srv_engine_bundler.log = derived;
  }
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  if (old_user) clog_close(old_user);
  if (old_fallback) clog_close(old_fallback);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs_t *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    ccol_mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    srv_engine_bundler.mprocs_storage = *mp;
    srv_engine_bundler.mprocs = &srv_engine_bundler.mprocs_storage;
  } else {
    srv_engine_bundler.mprocs = NULL;
  }
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

ccol_retval_t chttpsvr_set_engine_num_reactor_threads(size_t num_threads) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    ccol_mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  srv_engine_bundler.num_reactor_threads = num_threads;
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

/* This white-box test helper gives the reactor thread count that the library
 * really wired into the reactor that it created last. It is not part of the
 * public API. A gate keeps this symbol out of a production build of
 * libccollections.so. That matches the identical convention that
 * chttpclient.c already established for its own test helpers that show the
 * internal state of its engine. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_engine_num_reactor_threads_for_tests(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(srv_engine_bundler.mutex);
  size_t n = srv_engine_bundler.last_resolved_num_reactor_threads;
  ccol_mutex_unlock(srv_engine_bundler.mutex);
  return n;
}
#endif /* RUNNING_UNIT_TESTS */

/* This white-box test helper gives g_reject_task_run_count_for_tests; see
 * the comment of that variable. It says how many rejection responses really
 * ran on a thread of a reject_pool, across the whole process, since the
 * counter started at zero. A test reads it before and after its own window
 * and asserts on the difference. Other tests in the same process may also
 * cause rejections. It is not part of the public API. A gate keeps this
 * symbol out of a production build of libccollections.so. That matches the
 * identical convention that
 * _chttpsvr_engine_num_reactor_threads_for_tests already established just
 * above. */
#ifdef RUNNING_UNIT_TESTS
size_t _chttpsvr_reject_pool_task_count_for_tests(void) {
  return atomic_load(&g_reject_task_run_count_for_tests);
}
#endif /* RUNNING_UNIT_TESTS */

/* Resolves h to the struct chttpserver* under it, WITHOUT a pin. It does not
 * touch pending_resolve_count at all. It is a bare lookup in the slot table.
 * That is safe for tests specifically. The test code that calls it runs
 * synchronously, on one thread. There is no concurrent destroy to race in
 * the first place.
 *
 * Unlike _chttpsvr_resolve, there is no matching unpin call that a test has
 * to remember. Such a call is an easy gap to leave. A forgotten unpin leaves
 * pending_resolve_count permanently nonzero on that server. Every future
 * chttpsvr_destroy call against it then hangs in silence.
 *
 * It returns NULL under exactly the same conditions as _chttpsvr_resolve.
 * This mirrors the identical _chttpcli_resolve_for_tests of
 * chttpclient.c. */
#ifdef RUNNING_UNIT_TESTS
struct chttpserver *_chttpsvr_resolve_for_tests(chttpsvr h) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot_t *slot =
        (chttpsvr_slot_t *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

/* Reads how many slots the chttpsvr handle table holds now. A test can
 * therefore assert that a loop of creates and destroys reuses freed slots.
 * Without that reuse, the table grows without bound. This mirrors the
 * identical _chttpcli_slot_table_capacity_for_tests of chttpclient.c. */
size_t _chttpsvr_slot_table_capacity_for_tests(void) {
  ccol_call_once(chttpsvr_slot_table.once, _chttpsvr_slot_table_init_globals);
  ccol_mutex_lock(chttpsvr_slot_table.mutex);
  size_t n = cvector_elem_count(chttpsvr_slot_table.slots);
  ccol_mutex_unlock(chttpsvr_slot_table.mutex);
  return n;
}

/* Reads how many servers are registered now in servers_bundler.servers[].
 * That is the list that the driver loop of _engine_force_stop_quiesce_all
 * walks. A test can therefore confirm that the library genuinely
 * unregistered a server. One example is the child-side fixup of
 * _chttpsvr_atfork_release_impl. That fixup runs for a server that a fork()
 * caught in the middle of a teardown. See the doc comment of that function.
 * The test needs
 * no fresh chttpsvr_engine_stop() pass to observe that indirectly. */
size_t _chttpsvr_servers_bundler_count_for_tests(void) {
  ccol_call_once(srv_engine_bundler.once, _engine_globals_init);
  ccol_mutex_lock(servers_bundler.mutex);
  size_t n = servers_bundler.count;
  ccol_mutex_unlock(servers_bundler.mutex);
  return n;
}

/* This white-box test helper gives the resolved idle timeout, in
 * milliseconds, that a chttpsvr_start() call really stored. The code
 * combines the chttpsvr_config_t fields idle_timeout_us and read_timeout_us,
 * which are both microsecond counts, and converts the result into the
 * internal `unsigned` millisecond field through _cfg_us_to_ms.
 *
 * A test can therefore assert the conversion directly. A configured value of
 * UINT_MAX - 1 milliseconds or more must land on UINT_MAX - 1. It must never
 * silently wrap to 0, which is the "off" sentinel of this field, and never
 * to some other unrelated value. The test needs no wait through an
 * astronomically long real idle timeout window.
 *
 * It returns 0 for an invalid handle. That is the same answer as for a
 * server that it really resolved and whose idle timeout really is off. A
 * test that uses this accessor is expected to know already that h is
 * otherwise valid. */
unsigned _chttpsvr_idle_timeout_ms_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  return atomic_load(&raw->idle_timeout_ms);
}

/* This white-box test helper gives every duration that a chttpsvr_start()
 * call resolved from chttpsvr_config_t, in the milliseconds that the server
 * keeps, in this order: stream_read_timeout, max_body_read_duration,
 * response_write_timeout, max_response_write_duration,
 * max_header_read_duration, the idle timeout, min_transfer_rate_grace,
 * body_memory_wait_timeout and streaming_queue_timeout. A test can then pin
 * the conversion from microseconds, each default that a 0 selects, and the
 * internal form of CHTTPSVR_NO_DEADLINE, without waiting for any of them. It
 * returns false for an invalid handle. */
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

/* This white-box test helper gives the current in_flight_requests count. See
 * the comment of that field, and see _release_in_flight,
 * _conn_start_diverted and _conn_dispatch_reject, for what it tracks. Every
 * read and write of this field elsewhere in this file happens under
 * raw->mutex. This helper therefore reads it the same way, and not with a
 * bare load that nothing synchronizes.
 *
 * A test can poll with it for a genuine, bounded condition. That condition
 * is "the library really dispatched the request that this test just sent to
 * a worker thread". The test then needs no fixed sleep that guesses how long
 * the accept, the parse and the dispatch take on the machine that runs it.
 *
 * It returns -1 for an invalid handle. That value is distinct from any real
 * count, because a real count is never negative. */
int _chttpsvr_in_flight_requests_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return -1;
  ccol_mutex_lock(raw->mutex);
  int n = raw->in_flight_requests;
  ccol_mutex_unlock(raw->mutex);
  return n;
}

/* This white-box test helper gives listener_paused_for_resource_pressure
 * directly; see the comment of that field. A test must prove that the
 * library genuinely paused the listener. The listener can instead be merely
 * quiet. Its backlog then happened to run dry at the moment of the
 * observation. Such a test needs a deterministic check of the state. It must
 * not infer a pause from the ABSENCE of network activity over some window of
 * time.
 *
 * The real timer of the sweep thread of the idle timeout runs at about 1
 * second. It is not synchronized with the clock of any test. It can
 * legitimately resume the listener at any point. If the underlying condition
 * still persists, it can then pause the listener again at once. A window
 * that a test observes over the network is therefore an inherently flaky
 * signal for this one property.
 *
 * It returns false for an invalid handle. That is the same answer as for a
 * server that it really resolved and that is not paused now. A test that
 * uses this accessor is expected to know already that h is otherwise
 * valid. */
bool _chttpsvr_listener_paused_for_resource_pressure_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return false;
  return atomic_load(&raw->listener_paused_for_resource_pressure);
}

/* This is a white-box test hook only. It marks the calling thread as if that
 * thread were a worker thread of h now. That means a thread of the
 * worker_pool or the reject_pool of h. See the comment of
 * chttpsvr_worker_key_bundle above. It dispatches no
 * real task, and it needs no running listener or reactor at all. A test can
 * therefore exercise the self-call guard of chttpsvr_destroy()
 * deterministically and directly.
 *
 * This matters beyond mere convenience. To drive that guard with a real
 * end-to-end request needs chttpsvr_start() to succeed inside a forked child
 * of a test binary. The shared reactor of that binary already runs with real
 * OS threads. fork(2) does not duplicate threads into the child. That child
 * therefore holds a hollow reactor handle that nothing services. It has no
 * way to accept the connection that is meant to trigger the handler under
 * test.
 *
 * The hook exists for exactly that forked-child use, where the process ends
 * in an abort or an exit; see destroy_from_within_own_handler_is_fatal in
 * tests.c. There is no counterpart that unmarks the thread. A mark inside a
 * forked child that never returns has nothing left to leak into.
 *
 * It is a no-op when h does not resolve. A gate keeps both this function and
 * the visibility of chttpsvr_worker_key_bundle out of a production build. */
void _chttpsvr_mark_self_as_worker_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (raw && _chttpsvr_worker_key_ensure()) _chttpsvr_mark_worker_thread(raw);
}

/* White-box accessors of the slow-client state, for the tests. Each one
 * gives 0 or false for an invalid handle. */
size_t _chttpsvr_mem_in_use_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_in_use;
  ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

/* The number of connections that hold a reservation of body memory, and in
 * *exempt_out whether one of them runs past the limit under the progress
 * rule. */
size_t _chttpsvr_mem_holders_for_tests(chttpsvr h, bool *exempt_out) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (exempt_out) *exempt_out = false;
  if (!raw) return 0;
  ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_holders;
  if (exempt_out) *exempt_out = raw->mem_exempt_active;
  ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

/* For the request that req names, which a handler of a buffered route holds:
 * the reservation of body memory of its connection, and the capacity of its
 * body buffer. */
void _chttpsvr_req_body_memory_for_tests(chttpsvr_req *req, size_t *charged,
                                         size_t *held) {
  chttpsvr_conn_t *conn = req->conn;
  ccol_mutex_lock(conn->srv->wait_mutex);
  *charged = conn->mem_charged;
  ccol_mutex_unlock(conn->srv->wait_mutex);
  *held = conn->body.cap;
}

size_t _chttpsvr_mem_waiting_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_waiting;
  ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

size_t _chttpsvr_stream_queue_len_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  ccol_mutex_lock(raw->wait_mutex);
  size_t n = raw->sq_len;
  ccol_mutex_unlock(raw->wait_mutex);
  return n;
}

bool _chttpsvr_stream_pool_created_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return false;
  ccol_mutex_lock(raw->mutex);
  bool created = raw->stream_pool != CTPOOL_INVALID;
  ccol_mutex_unlock(raw->mutex);
  return created;
}

/* The number of tasks that the worker pool, or the streaming pool, runs at
 * this moment. A parked connection holds none. */
size_t _chttpsvr_pool_active_for_tests(chttpsvr h, bool streaming) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  ccol_mutex_lock(raw->mutex);
  ctpool pool = streaming ? raw->stream_pool : raw->worker_pool;
  ccol_mutex_unlock(raw->mutex);
  return pool ? ctpool_active_count(pool) : 0;
}

/* The number of connections in the idle list that are parked on a body
 * (kind 1) or on a response (kind 2). */
size_t _chttpsvr_parked_count_for_tests(chttpsvr h, int kind) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->parked_head; c; c = c->parked_next)
    if (c->park == (uint8_t)kind) n++;
  ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* What the connections parked on a response (kind 2) or lingering (kind 6)
 * still hold of their request: the capacity of the body buffer plus one for
 * each header field, and one each for a target, a decoded target, a query and
 * an array of parameter values. It is 0 when none of them retains anything. */
size_t _chttpsvr_parked_request_state_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->parked_head; c; c = c->parked_next) {
    if (c->park != _CONN_PARK_WRITE && c->park != _CONN_PARK_LINGER) continue;
    n += c->body.cap + c->hdr_count + (c->path != NULL) +
         (c->decoded_path != NULL) + (c->raw_query != NULL) +
         (c->matched_param_values != NULL);
  }
  ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* The number of connections in the idle list of h. */
size_t _chttpsvr_idle_count_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol_mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn_t *c = raw->idle_head; c; c = c->idle_next) n++;
  ccol_mutex_unlock(raw->idle_mutex);
  return n;
}

/* Runs one tick of the sweep for h at once, on the calling thread, and
 * returns how many connections that tick examined. The sweep thread keeps
 * running beside it; the claims of the lists make the two safe together. */
size_t _chttpsvr_sweep_now_for_tests(chttpsvr h) {
  struct chttpserver *raw = _chttpsvr_resolve(h);
  if (!raw) return 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  size_t visited = _sweep_server(raw, now);
  _chttpsvr_resolve_unpin(raw);
  return visited;
}
#endif /* RUNNING_UNIT_TESTS */

void chttpsvr_engine_stop(void) { _engine_force_stop(); }

void chttpsvr_engine_wait(void) {
  /* This is the self-call guard; see the comment of
   * _chttpsvr_is_any_worker_call. Take a request handler, or a middleware,
   * on ANY server that is registered now. A block here from such a handler
   * can deadlock the whole engine, and not only its own server. The
   * ctpool_shutdown_drain inside _engine_force_stop_quiesce_all has no
   * timeout. It can never finish the drain of the pool of that handler,
   * because this exact call stack is what would let the task return. The
   * library can therefore never tear the reactor down, and this wait can
   * never wake.
   *
   * This function has no ccol_retval_t of its own to report the misuse
   * through. Its documented contract is simply "blocks until the engine
   * exits". The code therefore treats a detected self-call exactly like the
   * identical guards of chttpsvr_destroy() and chttpsvr_start(). It makes a
   * loud, immediate ccol_fatal_err() call that names the real problem,
   * instead of a silent, permanent hang. */
  if (_chttpsvr_is_any_worker_call()) {
    ccol_fatal_err(
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
  /* root_router, and not routers[0]: a concurrent chttpsvr_subrouter() can
   * realloc raw->routers, and root_router needs no lock to read. */
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

  ccol_rw_lock_wrlock(raw->routes_lock);
  if (raw->router_count >= raw->router_cap) {
    if (raw->router_cap > (SIZE_MAX - 2) / 2 ||
        raw->router_cap * 2 + 2 > SIZE_MAX / sizeof(chttpsvr_router *)) {
      ccol_rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 2 */
      return NULL;
    }
    size_t nc = raw->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)_ccol_mem_realloc(
        raw->m_procs, raw->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      ccol_rw_lock_unlock(raw->routes_lock);
      _destroy_router(r, raw->m_procs);
      _chttpsvr_resolve_unpin(raw); /* exit 3 */
      return NULL;
    }
    raw->routers = nr;
    raw->router_cap = nc;
  }
  /* The code inserts the new router among the sub-routers. Those are the
   * entries at index 1 and up, and index 0 stays the root. The order is
   * descending by the count of prefix segments. That is the order in which
   * _find_route walks them. See the comment of that function for why the
   * code must try the most specific mount first.
   *
   * The shift stops at index 1, so nothing ever displaces the root. Mounts
   * of equal specificity keep their relative order of registration. That is
   * the only tie left to break. Two mounts of the same segment count are
   * either disjoint literals, or the identical prefix registered two times.
   * For disjoint literals, at most one of them can match a given path. */
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
  /* See the field comment of max_prefix_seg_count on struct chttpserver. The
   * code keeps that field up to date here. It does so under the same
   * write-lock critical section as the registration of the router itself.
   * This mirrors exactly how _router_add_route maintains
   * router->max_route_seg_count under the identical lock. */
  if (r->prefix_seg_count > raw->max_prefix_seg_count)
    raw->max_prefix_seg_count = r->prefix_seg_count;
  ccol_rw_lock_unlock(raw->routes_lock);
  _chttpsvr_resolve_unpin(raw); /* exit 4 (success) */
  return r;
}

/* This is the shared entry sequence for every chttpsvr_router_on,
 * _on_stream and _use call. It reads router->owner, which is always safe in
 * memory. See the comment of chttpsvr_router_shell_registry for why nothing
 * ever frees the shell that this field lives in. It then resolves and pins
 * that handle through the ordinary chttpsvr slot table.
 *
 * It returns NULL when something already destroyed the owning server. In
 * every other case it returns the resolved, pinned server. The caller MUST
 * then call _chttpsvr_resolve_unpin(raw) exactly once when it is done. */
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
  /* A repeated header name keeps every occurrence, in the order of arrival,
   * in conn->hdr_names and conn->hdr_values. The scan therefore runs
   * backward, so that the LAST occurrence wins. That matches the documented
   * behavior of this module for a header that arrives more than one time;
   * see streaming_repeated_header in tests.c. */
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
  /* See the identical guard of chttpsvr_req_read. It says why conn and
   * conn->matched_route are never really NULL through documented usage, and
   * why the code still checks them here as a defence. The
   * chttpsvr_req_method, _path, _header and _raw_query functions differ.
   * Each one dereferences req->conn unconditionally once it has ruled !req
   * out. The same "never really NULL in practice" argument applies equally
   * to all of them.
   *
   * The library never extracts the body of a streaming route into conn->body
   * in advance, as one contiguous, stable buffer. It does that for a
   * buffered route. For a streaming route, conn->body is a live cursor
   * instead, which is growbuf_t.pos. The chttpsvr_req_read() function drains
   * that cursor batch by batch, as bytes really arrive. The _on_body()
   * function compacts it back to empty lazily, on the arrival of the NEXT
   * batch. It does not compact it the moment the cursor is fully drained.
   *
   * Take a streaming handler that calls chttpsvr_req_read() at all and then
   * calls this function. Without this guard it gets back a pair of a pointer
   * and a length. That pair silently mixes two kinds of byte. The first kind
   * is the bytes that the library already delivered, before pos. The second
   * kind is the bytes from pos to len, which it has not delivered yet. Or
   * the length merely reflects the
   * internal accounting since the last compaction. It is then neither the
   * true total nor the true count of unread bytes. Nothing reports an error
   * at all.
   *
   * This function is documented for a buffered route only; see the doc
   * comment of chttpsvr_req_body in chttpserver.h. This mirrors the
   * symmetric rejection of a buffered route in chttpsvr_req_read exactly. A
   * mismatch of the route therefore always fails safe, with a clear NULL and
   * 0, on either accessor, and not on only one of them. */
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
  /* See the identical guard of chttpsvr_req_read. It says why conn and
   * conn->matched_route are never really NULL through documented usage, and
   * why the code still checks them here as a defence. The
   * chttpsvr_req_method, _path, _header and _raw_query functions differ.
   * Each one dereferences req->conn unconditionally once it has ruled !req
   * out. */
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

/* Says whether value is a Content-Length that a response may carry: one or
 * more ASCII digits (RFC 9110 SS8.6) whose number fits in a signed 64-bit
 * integer, which is the widest length that the parsers of Go, curl and this
 * library accept. */
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

/* The checks that every response header passes, whichever call sets it. */
static ccol_retval_t _resp_header_validate(const char *name,
                                           const char *value) {
  /* A field name must be a token that holds only tchar bytes, and it must
   * not be empty; see RFC 7230 SS3.2.6. It is not enough that it is not
   * empty and holds no CR or LF. A byte outside that set is not itself a way
   * to inject a CR or an LF. One example is a space, and another is a
   * literal ':'. But it still produces a wire line that is structurally
   * malformed, and that a strict parser downstream reads as ambiguous. The
   * name "X Foo: bar" puts "X Foo:bar:baz\r\n" on the wire. That is not a
   * genuine split into two fields.
   *
   * The check uses the identical tchar classifier that chttp1_parser.c
   * itself already applies to the header names of an incoming request. The
   * chttp_request_set_header of chttpclient.c applies it to the names of an
   * outgoing request. The code does not use a second character class, which
   * could diverge from that one. */
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  /* The _send_response() function writes the name and the value onto the
   * wire exactly as "name:value\r\n", with no further escaping. Take a
   * caller that reflects data that the request controls into a response
   * header. That data can be a query parameter, a path parameter, or an
   * echoed request header. An embedded CR or LF byte then lets whoever
   * controls that data inject any number of extra header lines. It also lets
   * them split the response into two. That is the classic HTTP response
   * splitting, which is also called CRLF injection.
   *
   * The code rejects such a byte outright here. Every call that sets a
   * response header goes through this function. The library does not leave
   * every caller to clean its own input. */
  if (strpbrk(value, "\r\n")) return ccol_invalid_args;
  /* The code rejects "Transfer-Encoding" outright. The
   * chttp_request_set_header of chttpclient.c does exactly the same on the
   * client side, for the identical reason. This server never transfer-codes
   * a response body. See the comment of _send_response: "this server never
   * uses chunked transfer-encoding for its own responses". To honor a
   * Transfer-Encoding header that a caller sets is therefore impossible.
   *
   * A header that reached the wire would also sit beside the Content-Length
   * header that this function computes automatically. That Content-Length
   * describes a body that nothing ever transfer-coded. The framing is then
   * ambiguous; see RFC 7230 SS3.3.3. An intermediary that honors
   * Transfer-Encoding over Content-Length misreads the boundary of the
   * message. That is exactly the response-splitting and desync hazard that
   * the handling of Connection and Content-Length in _send_response already
   * prevents. */
  if (strcasecmp(name, "transfer-encoding") == 0) return ccol_invalid_args;
  /* A Content-Length reaches the wire only on a response that sends no body
   * (see _send_response), and there it is the whole framing statement of the
   * response for any cache or client that reads it. A value that is not a
   * number would make that statement malformed. */
  if (strcasecmp(name, "content-length") == 0 &&
      !_resp_content_length_valid(value))
    return ccol_invalid_args;
  return ccol_success;
}

/* Says whether a response carries at most one field of this name, whatever
 * call sets it. _send_response decides the Connection field itself and
 * reads the Content-Length and the Date of a handler as single values. */
static bool _resp_header_is_singleton(const char *name) {
  return strcasecmp(name, "connection") == 0 ||
         strcasecmp(name, "content-length") == 0 ||
         strcasecmp(name, "date") == 0;
}

/* Appends one more field line. The caller validated name and value. */
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

/* Gives name the one value value. The first field of that name keeps its
 * place and the letter case of its name, and every later field of the same
 * name goes, so the order of the other fields stays the order of the calls
 * that set them. The caller validated name and value. */
static ccol_retval_t _resp_header_replace(chttpsvr_resp *resp, const char *name,
                                          const char *value) {
  ccol_memmgmt_procs_t *mp = resp->m_procs;
  size_t first = resp->header_count;
  for (size_t i = 0; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, name) == 0) {
      first = i;
      break;
    }
  }
  if (first == resp->header_count)
    return _resp_header_append(resp, name, value);

  /* The copy comes first, so a failed allocation leaves every field as it
   * was. */
  char *new_val = ccol_strdup(mp, value);
  if (!new_val) return ccol_not_enough_memory;
  _ccol_mem_free(mp, resp->headers[first].value);
  resp->headers[first].value = new_val;
  size_t kept = first + 1;
  for (size_t i = first + 1; i < resp->header_count; i++) {
    if (strcasecmp(resp->headers[i].name, name) == 0) {
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
