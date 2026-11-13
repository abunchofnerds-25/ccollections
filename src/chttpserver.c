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

// The GNU extensions give us accept4() and the other Linux socket calls.
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
#include <linux/sockios.h>
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

typedef struct chttpsvr_mw_node chttpsvr_mw_node;

// One middleware entry of a router's singly linked list. next is a plain
// pointer: Readers walk the list under the read lock of routes_lock and writers
// append under the write lock, so the lock orders the two sides.
struct chttpsvr_mw_node {
  chttpsvr_middleware_fn fn;
  void *ctx;
  chttpsvr_mw_node *next;
};

// A compiled route: The pattern split into segments, the names of its {name}
// segments, and the handler to call.
typedef struct chttpsvr_route {
  chttp_method method;
  char **segs;
  int seg_count;
  char **param_names;
  int param_count;
  chttpsvr_handler_fn fn;
  void *ctx;
  bool is_streaming;
} chttpsvr_route;

// A sub-router, and the root router of a server (prefix ""). The struct itself,
// the "shell", is never freed while a caller can hold a pointer to it, because
// chttpsvr_router_on, _on_stream and _use read owner before any resolve or pin
// can protect them. destroy_router frees only the contents (routes, mw nodes,
// prefix) and stores CHTTPSVR_NULL in owner. The shell of a root router goes
// away with its server, and after the registry's exit-time destructor has run
// every shell of a destroyed server is freed too.
struct chttpsvr_router {
  char *prefix;
  size_t prefix_len;
  // How many '/'-separated segments the prefix has (0 for the root and for
  // "/"). find_route orders the sub-routers by it, and it bounds the shared
  // decode cache through max_prefix_seg_count of the server.
  int prefix_seg_count;
  chttpsvr_mw_node *mw_head;
  chttpsvr_mw_node *mw_tail;
  int mw_count;
  // Each route is its own allocation, so a route pointer stays valid when the
  // array grows.
  chttpsvr_route **routes;
  size_t route_count;
  size_t route_cap;
  // The largest seg_count of this router's routes. Routes are only ever added,
  // so a running maximum is always right. It bounds how many segments of a
  // request path, which the client controls, the decode cache of find_route
  // splits.
  int max_route_seg_count;
  // The server's allocator serves only the contents of the router, never the
  // shell. The public router calls must resolve owner before they read srv.
  struct chttpserver *srv;
  ccol_memmgmt_procs *m_procs;
  // The handle of the owning server, or CHTTPSVR_NULL once that server is gone.
  // It's atomic because destroy_router writes it with no lock a reader holds.
  // shell_idx is the shell's position in the shell registry (SIZE_MAX when the
  // registry doesn't hold it), guarded by the registry mutex.
  _Atomic chttpsvr owner;
  size_t shell_idx;
};

// One response header in the flat headers array.
typedef struct {
  char *name;
  char *value;
} resp_header;

// The response a handler builds. The headers live in a flat array that grows to
// 2x+8, so a duplicate scan stays cache friendly and a header costs no node
// allocation.
struct chttpsvr_resp {
  int status_code;
  resp_header *headers;
  size_t header_count;
  size_t header_cap;
  char *body;
  size_t body_len;
  size_t body_cap;
  ccol_memmgmt_procs *m_procs;
};

// The query parameters, parsed on first use. Keys and values are URL-decoded
// copies that the struct owns.
typedef struct chttpsvr_qparams {
  char **keys;
  char **values;
  size_t count;
  size_t cap;
  ccol_memmgmt_procs *m_procs;
} chttpsvr_qparams;

// How many middleware steps one request may go through, global and router steps
// together. The per-request snapshot in dispatch_ctx is a fixed array of this
// size, so a request costs no allocation for it; a request that needs more gets
// a 500.
#define CHTTPSVR_MAX_MW 32

// The Retry-After value of every 503 the server sends on its own (full pools,
// streaming queue timeout, body memory wait timeout). Five seconds covers
// several sweep ticks, so a client that honours it doesn't walk straight back
// into the same saturation.
#define CHTTPSVR_RETRY_AFTER_SECONDS "5"

// Defaults of the slow-client settings of chttpsvr_config. 240 bytes a second
// after a five-second grace is the minimum data rate Kestrel and IIS use.
#define CHTTPSVR_DEFAULT_MIN_RATE_BPS 240u
#define CHTTPSVR_DEFAULT_RATE_GRACE_MS 5000u
#define CHTTPSVR_DEFAULT_PARTIAL_BODY_MEMORY ((size_t)256 * 1024 * 1024)
#define CHTTPSVR_DEFAULT_MEMORY_WAIT_MS 30000u
#define CHTTPSVR_DEFAULT_STREAM_QUEUE_TIMEOUT_MS 5000u

// A hard cap, in ms, on the total time of a write the library makes itself: A
// rejection response or the "100 Continue" interim line. It applies whatever
// max_response_write_duration_ms says, because that knob can be 0 and a peer
// reading one byte at a time could otherwise keep such a write, its descriptor
// and its parked state alive for ever. On a streaming route it also bounds the
// wait for the interim line inside chttpsvr_req_read().
#define CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS 2000

#ifdef RUNNING_UNIT_TESTS
// Test override of the internal write ceiling, so a client that reads a large
// rejection slowly (under valgrind, say) gets all of it. 0 means the default.
static _Atomic unsigned g_internal_write_ceiling_ms_for_tests;
// Sets the test override of the internal write ceiling to ms (0 goes back to
// the default). Tests that check what a parked rejection does, not how long it
// may last, need a higher ceiling so a slow reader gets the whole response. It
// stores into g_internal_write_ceiling_ms_for_tests.
void chttpsvr__set_internal_write_ceiling_ms_for_tests(unsigned ms) {
  atomic_store(&g_internal_write_ceiling_ms_for_tests, ms);
}
// Returns the internal write ceiling in ms. The rejection and interim-line
// writes take their total deadline from it, so tests can raise it. It reads
// g_internal_write_ceiling_ms_for_tests and returns
// CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS when that is 0.
static inline unsigned internal_write_ceiling_ms(void) {
  unsigned ms = atomic_load(&g_internal_write_ceiling_ms_for_tests);
  return ms ? ms : CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS;
}
#else
// Without the test hooks this takes no arguments and evaluates to
// CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS, so the callers of
// internal_write_ceiling_ms() build the same way in both configurations. It
// reads and changes no state.
#define internal_write_ceiling_ms() CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS
#endif

// The fewest threads reject_pool gets. chttpsvr_start sizes it as the larger of
// this and half of worker_pool's thread count, because every rejection (404,
// 405, 500, a 503 for a full pool) goes through it and a burst of unmatched
// routes needs real concurrency to drain.
#define CHTTPSVR_REJECT_POOL_MIN_THREADS 2

// The queue capacity of reject_pool. A bound keeps an endless flood of rejected
// connections from growing our own memory without limit. When the queue is
// full, conn_reject_via_pool closes the connection on the calling thread.
#define CHTTPSVR_REJECT_POOL_QUEUE_CAP 1024

// One middleware step of the per-request snapshot.
typedef struct {
  chttpsvr_middleware_fn fn;
  void *ctx;
} mw_entry;

// Middleware dispatch state. It lives inside chttpsvr_conn, so it costs no
// allocation and is reset for each request like the other scratch fields. The
// snapshot of fn and ctx pairs is copied under routes_lock.
typedef struct dispatch_ctx {
  struct chttpserver *srv;
  chttpsvr_router *router;
  chttpsvr_route *route;
  mw_entry mw_snap[CHTTPSVR_MAX_MW];
  int mw_count;
  int mw_idx;
} dispatch_ctx;

// A small growable buffer. A buffered route collects the whole body in it. A
// streaming route keeps in it the body bytes chttpsvr_req_read hasn't handed
// out yet; pos is how many of the len bytes it already gave.
typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  size_t pos;
} growbuf;

// Who owns a connection and what it waits for. In CONN_ST_DIVERTED a worker
// owns the fd and the reactor registration is paused, never removed, so the
// next request can resume it cheaply.
typedef enum {
  CONN_ST_TLS_HANDSHAKE,
  CONN_ST_READING_HEADERS,
  CONN_ST_DIVERTED,
  CONN_ST_CLOSING
} conn_state;

// One accepted connection. It lives across all the keep-alive requests of the
// connection, and conn_reset_for_request() clears the per-request scratch
// fields before each request. Either the reactor registration or a worker task
// owns it, never both at once.
typedef struct chttpsvr_conn {
  int fd;
  struct chttpserver *srv;
  ceventloop_reg reg;
  ctls_conn *tls;
  conn_state state;
  chttp1_parser parser;

  // Per-request scratch fields. path is the raw, percent-encoded path: Matching
  // splits on a literal '/' and decodes each segment on its own, so a %2F
  // inside a {param} stays one segment and a bad %XX in one segment is a 404,
  // not a 400. decoded_path is filled once a route matched and is what
  // chttpsvr_req_path() returns.
  chttp_method method;
  char *path;
  char *decoded_path;
  char *raw_query;
  char **hdr_names, **hdr_values;
  size_t hdr_count, hdr_cap;
  chttpsvr_router *matched_router;
  chttpsvr_route *matched_route;
  char **matched_param_values;
  dispatch_ctx dispatch;
  chttpsvr_resp resp;
  // allowed_methods feeds the Allow header of a 405. options_star marks an
  // "OPTIONS *" request, which the server answers itself.
  bool req_rejected;
  int reject_status;
  uint8_t allowed_methods;
  bool options_star;
  // interim_continue_sent keeps the "100 Continue" line from going out twice.
  // interim_write_failed means part of it went out, so anything written after
  // it would break the client's framing; the connection is closed instead.
  bool expects_continue;
  bool interim_continue_sent;
  bool interim_write_failed;
  // The body and why reading it stopped. body_alloc_failed is a real allocation
  // failure (ccol_not_enough_memory for a streaming route), transfer_aborted an
  // EOF or I/O error seen by chttpsvr_req_read(), and body_malformed a framing
  // error or a short body on a buffered route, which gets a 400 and not a 500.
  // chttpsvr_req_stream_error() checks body_too_large and body_alloc_failed
  // first.
  growbuf body;
  size_t body_bytes_seen;
  bool body_too_large;
  bool body_alloc_failed;
  bool transfer_aborted;
  bool body_malformed;

  // Bytes past the headers in the reactor's read buffer. That buffer is on the
  // reactor thread's stack, so conn_start_diverted copies them out, and the
  // worker frees them once its stream has taken them.
  char *carry_over;
  size_t carry_over_len;

  // Deadline of max_body_read_duration_ms.
  struct timespec read_deadline;
  bool read_deadline_set;
  bool deadline_exceeded;

  // Deadline of max_response_write_duration_ms, read by the real response, a
  // rejection and the interim line (the last two are also capped by the
  // internal write ceiling).
  struct timespec write_deadline;
  bool write_deadline_set;

  // Idle list links for the idle sweep; srv->idle_mutex guards them.
  struct timespec last_activity;
  struct chttpsvr_conn *idle_prev, *idle_next;
  bool in_idle_list;

  // Diverted list links, guarded by srv->diverted_mutex. A teardown finds the
  // connection of a stuck worker here and forces a shutdown(2) on it.
  struct chttpsvr_conn *diverted_prev, *diverted_next;
  bool in_diverted_list;

  // 1 until conn_close, plus 1 for every registration created for this
  // connection until its on_removed fires. A connection can go through several
  // registrations (a failed pause removes one and a later wait adds another),
  // so conn_free runs only when every one of them, and the close, has dropped
  // its reference.
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs *m_procs;

  // Start of the reactor-owned phase of the current request (handshake or first
  // header byte), for max_header_read_duration_ms. Written only while the
  // connection is out of the idle list, read by the sweep under
  // srv->idle_mutex.
  struct timespec header_phase_start;
  bool header_phase_active;

  // Slow-client state; for a buffered route a worker never waits on the socket.
  // divert_gate is 0 for an ordinary divert, SIZE_MAX for a streaming route,
  // and otherwise the Content-Length of a buffered body that has to reserve
  // memory.
  size_t divert_gate;

  // The CONN_PARK_* wait the connection is in while no thread owns it, written
  // before it's published to a list and read by the thread that claims it back.
  // park_pending is the wait a worker is about to enter. reg_dir is the
  // direction reg was last armed for, so conn_reg_modify skips a call that
  // changes nothing.
  uint8_t park;
  uint8_t park_pending;
  uint8_t reg_dir;

  // Rate floor and per-gap limit of a parked transfer, on slow_clock.
  // rate_paused_ms is time the server itself held the request back. rate_queued
  // is the unread byte count the sweep last saw while the receive low-water
  // mark was above 1. rcvlowat is the current SO_RCVLOWAT and rcvlowat_cap the
  // highest the buffer allows (read on the first park).
  bool rate_active;
  struct timespec rate_start;
  long long rate_paused_ms;
  uint64_t rate_bytes;
  struct timespec last_progress;
  size_t rate_queued;
  int rcvlowat;
  int rcvlowat_cap;

  // Partial-body memory. Once the connection holds a charge or waits,
  // srv->wait_mutex guards this group. mem_charged lasts until conn_drop_body
  // frees the body; mem_exempt means it went past the limit under the progress
  // rule; mem_need is what a waiter asks for.
  size_t mem_charged;
  size_t mem_need;
  bool mem_exempt;
  bool in_mem_wait;
  struct timespec mem_wait_start;
  struct chttpsvr_conn *mem_prev, *mem_next;

  // When a parked connection went to the worker pool, on CLOCK_MONOTONIC and
  // slow_clock. task_resume judges the limits at this instant, so time spent in
  // the pool's queue is charged to the server.
  struct timespec claim_mono;
  struct timespec claim_slow;

  // Streaming queue links and deadline; srv->wait_mutex guards them.
  bool in_stream_queue;
  bool sq_has_deadline;
  struct timespec sq_deadline;
  struct chttpsvr_conn *sq_prev, *sq_next;

  // A response whose write stopped on a full socket. wp_head holds the unsent
  // part of the header block and wp_body_pos how much body went out.
  // wp_wants_read means TLS needs to read first, wp_reject marks a rejection
  // (it ends in a close), and wp_interim marks the interim line of a buffered
  // route. wp_outq is the smallest SIOCOUTQ seen (SIZE_MAX if unknown); a
  // shrinking send queue counts as progress.
  bool wp_active;
  bool wp_keep_alive;
  bool wp_no_body;
  bool wp_wants_read;
  bool wp_reject;
  bool wp_interim;
  char *wp_head;
  size_t wp_head_len;
  size_t wp_head_pos;
  size_t wp_body_pos;
  size_t wp_outq;

  // Parked list links, guarded by srv->idle_mutex.
  struct chttpsvr_conn *parked_prev, *parked_next;
  bool in_parked_list;

  // End of a lingering close, on CLOCK_MONOTONIC.
  struct timespec linger_deadline;
} chttpsvr_conn;

// The waits of chttpsvr_conn.park. BODY, WRITE and LINGER sit on the parked
// list with the registration armed. The two memory waits sit on the memory-wait
// list and STREAM on the streaming queue, all three with the registration
// paused.
enum {
  CONN_PARK_NONE = 0,
  CONN_PARK_BODY = 1,
  CONN_PARK_WRITE = 2,
  CONN_PARK_MEM_START = 3,
  CONN_PARK_MEM_BODY = 4,
  CONN_PARK_STREAM = 5,
  CONN_PARK_LINGER = 6
};

// One chttpsvr_req_set_context() value. A pair holding a value never has a NULL
// key.
typedef struct {
  const void *key;
  void *value;
} chttpsvr_ctx_pair;

// The context values after the first one, in a block that doubles as it grows.
typedef struct {
  size_t count;
  size_t cap;
  chttpsvr_ctx_pair pairs[];
} chttpsvr_ctx_more;

// What a handler or middleware gets: A view over the connection plus the
// worker's own chttp1_stream, which exists only while one worker drives one
// request. The first context value is stored inline (key NULL while there is
// none); the rest go into ctx_more, which destroy_req_scratch frees.
struct chttpsvr_req {
  chttpsvr_conn *conn;
  chttp1_stream *stream;
  const char **param_names;
  chttpsvr_qparams *qparams;
  const char **qresult;
  size_t qresult_cap;
  ccol_memmgmt_procs *m_procs;
  chttpsvr_ctx_pair ctx_first;
  chttpsvr_ctx_more *ctx_more;
  bool qparams_attempted;
  bool qparams_parse_oom;
  bool qresult_oom;
};

// One slot of the handle table. A chttpsvr handle holds the slot index in its
// top 32 bits and the generation in the bottom 32. The generation goes up on
// every acquire, so a stale handle never matches a later server in the same
// slot and a double destroy is a fatal error instead of a use-after-free.
typedef struct {
  struct chttpserver *ptr;
  uint32_t generation;
  bool in_use;
} chttpsvr_slot;

// The handle slot table, set up once by chttpsvr_slot_table_init_globals. slots
// only grows, free_indices is a LIFO free list, and release_deferred says the
// exit-time destructor found a live server and left the release to the destroy
// of the last one.
static struct {
  ccol__mutex mutex;
  ccol__once_flag once;
  cvec slots;
  bool release_deferred;
  cvec free_indices;
} chttpsvr_slot_table = {0};

// Set when a thread enters chttpsvr_slot_table_init_globals. The exit-time
// destructor uses it to skip a process that never touched this module, because
// the init also builds the tables of clogger and cthreadpool, whose destructors
// may have run already.
static atomic_bool chttpsvr_slot_table_entered = false;

#if CCOL_FORK_SAFETY_REQUIRED
static void chttpsvr_atfork_prepare(void);
static void chttpsvr_atfork_release(void);
static void chttpsvr_atfork_child_release(void);
#endif

// Creates the slot table and registers the fork handlers. It runs once, through
// chttpsvr_slot_table.once, before any path creates or resolves a server,
// because every handle lookup needs the table. It sets
// chttpsvr_slot_table_entered, initializes the table's mutex and vectors, and
// ends the process if any of that fails. The handlers of cthreadcomm,
// cthreadpool and clogger are forced to register first: Prepare handlers run in
// reverse order, so ours runs first and takes our locks before theirs, which is
// the order every ordinary call nests them in (engine_acquire holds
// srv_engine_bundler.mutex while it creates the event loop and the logger).
// Left to the order of first use, the two orders can disagree and fork time
// sees a lock-order cycle.
static void chttpsvr_slot_table_init_globals(void) {
  atomic_store(&chttpsvr_slot_table_entered, true);
  if (ccol__mutex_init(chttpsvr_slot_table.mutex) != 0)
    ccol__fatal_err("chttpsvr slot table: failed to initialize mutex");
  chttpsvr_slot_table.slots = cvector_create(sizeof(chttpsvr_slot), NULL);
  if (!chttpsvr_slot_table.slots)
    ccol__fatal_err("chttpsvr slot table: failed to allocate slots vector");
  chttpsvr_slot_table.free_indices = cvector_create(sizeof(uint32_t), NULL);
  if (!chttpsvr_slot_table.free_indices)
    ccol__fatal_err(
        "chttpsvr slot table: failed to allocate free-index vector");
#if CCOL_FORK_SAFETY_REQUIRED
  cthreadcomm__ensure_atfork_registered_before_caller();
  cthrpool__ensure_atfork_registered_before_caller();
  clog__ensure_atfork_registered_before_caller();
  ccol__at_fork(chttpsvr_atfork_prepare, chttpsvr_atfork_release,
                chttpsvr_atfork_child_release);
#endif
}

#if defined(CCOL__ATFORK_AT_LOAD)
// Runs the slot table init, and so the fork handler registration, when the
// library loads. On systems that set CCOL__ATFORK_AT_LOAD a lazy
// pthread_atfork() can hang in a forked child, so the handlers must be
// registered before any thread starts. It goes through
// chttpsvr_slot_table.once, so a later lazy call does nothing.
__attribute__((constructor)) static void chttpsvr_atfork_at_load(void) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
}
#endif

// Start/stop state of a server; srv->mutex guards it. STARTING spans a whole
// chttpsvr_start() so a second concurrent start can't create pools and swap the
// TLS context under the first. STOPPING spans the removal and close of the old
// listener, so a start can't bind the same port while the old fd is open. Every
// reader uses a switch with no default, so -Wswitch catches a new value.
typedef enum {
  CHTTPSVR_LC_IDLE = 0,
  CHTTPSVR_LC_STARTING = 1,
  CHTTPSVR_LC_RUNNING = 2,
  CHTTPSVR_LC_STOPPING = 3,
} chttpsvr_lifecycle;

// The one-shot quiesce latch of quiesce_server_once(). It's separate from the
// lifecycle so the claim of a teardown doesn't depend on start/stop. A loser
// waits for QUIESCED, not QUIESCING, because chttpsvr__destroy frees the server
// right after the call returns. Only a later chttpsvr_start() resets it.
typedef enum {
  CHTTPSVR_QS_NOT_QUIESCED = 0,
  CHTTPSVR_QS_QUIESCING = 1,
  CHTTPSVR_QS_QUIESCED = 2,
} chttpsvr_quiesce_state;

// What a unix:// listener needs to remove its socket file. Every lookup goes
// through dir_fd, so a later chdir() can't move the cleanup to another file,
// and the name is unlinked only while dev and ino still name the socket we
// bound.
typedef struct {
  char *path;
  const char *base;
  int dir_fd;
  dev_t dev;
  ino_t ino;
} chttpsvr_unix_bind;

// The server. Lock order: srv->mutex before wait_mutex; idle_mutex,
// diverted_mutex and wait_mutex are each held only briefly and never across a
// dispatch.
struct chttpserver {
  // routers[0] is the root and the rest are the sub-routers, sorted by
  // prefix_seg_count from largest to smallest; routes_lock guards the array.
  // root_router is routers[0] copied at create time, read without the lock.
  // max_prefix_seg_count bounds the shared decode cache of find_route, so the
  // operator and not the client decides how much of a path gets decoded.
  chttpsvr_router **routers;
  chttpsvr_router *root_router;
  size_t router_count;
  size_t router_cap;
  int max_prefix_seg_count;
  cthrpool worker_pool;
  // Runs every rejection (middleware plus the courtesy write and close), so
  // that work stays off the reactor and doesn't compete with the full worker
  // pool that caused a 503.
  cthrpool reject_pool;
  ctls_ctx *tls_ctx;
  // in_flight_requests counts handlers that run; requests_done_cv wakes the
  // drain.
  ccol__mutex mutex;
  ccol__cond_var requests_done_cv;
  int in_flight_requests;
  // Three pin counters, all broadcast on resolve_cv, which chttpsvr__destroy
  // waits on before it frees the server. pending_resolve_count is held by every
  // public call for its whole body; the decrement and the broadcast are both
  // done under mutex. servers_bundler_pins is held by
  // engine_force_stop_quiesce_all and listener_dispatch_pins by one listener
  // dispatch; each has its own counter because quiesce_server_once and
  // chttpsvr_stop_internal wait on them from inside a call that holds a
  // pending_resolve_count pin. chttpsvr_stop_internal waits for
  // listener_dispatch_pins before it closes the listener fd.
  _Atomic size_t pending_resolve_count;
  _Atomic size_t servers_bundler_pins;
  _Atomic size_t listener_dispatch_pins;
  ccol__cond_var resolve_cv;
  ccol__rw_lock routes_lock;
  clog cl;

  // Read by the reactor on every accept without srv->mutex, hence atomic, and
  // set before the listener is registered. listen_fd is -1 while stopped.
  _Atomic int listen_fd;
  _Atomic bool is_unix_socket;
  // The unix socket file (path NULL for TCP), and the port and address
  // getsockname() gave at bind time. srv->mutex guards all of them.
  chttpsvr_unix_bind unix_bind;
  uint16_t bound_port;
  char bound_host[INET6_ADDRSTRLEN];
  ceventloop_reg listen_reg;
  // lifecycle, contributed_to_engine and engine_gen (the engine generation of
  // our reference) are guarded by srv->mutex.
  chttpsvr_lifecycle lifecycle;
  bool contributed_to_engine;
  uintptr_t engine_gen;
  // quiesce_done_cv is broadcast when quiesce_state reaches QUIESCED.
  // quiesce_waiters counts threads waiting for that; the winner waits for it to
  // reach 0 before it returns, because its caller destroys the mutex and the
  // condition variable right after, while a woken loser may still be
  // reacquiring the mutex.
  chttpsvr_quiesce_state quiesce_state;
  ccol__cond_var quiesce_done_cv;
  size_t quiesce_waiters;
  // True from the start of a teardown until a restart. Responses then close
  // their connections and new requests get a 503, so the drain waits only for
  // running handlers. A plain bool used through __atomic builtins, so the
  // response path pays a plain load.
  bool draining;

  // Settings that chttpsvr_start writes on every restart while connections that
  // survived the stop may read them, hence atomic. max_header_read_duration_ms
  // bounds the handshake plus the header read (0 is off).
  _Atomic unsigned stream_read_timeout_ms;
  _Atomic unsigned max_body_read_duration_ms;
  _Atomic unsigned response_write_timeout_ms;
  _Atomic unsigned max_response_write_duration_ms;
  _Atomic unsigned idle_timeout_ms;
  _Atomic unsigned max_header_read_duration_ms;
  _Atomic size_t max_body_size;
  _Atomic size_t max_header_bytes;
  _Atomic size_t max_connections;
  // Read only by the listener callback, which never runs while chttpsvr_start
  // writes it, so a plain bool is enough.
  bool enable_keepalive;
  _Atomic size_t current_connections;

  // Set when the listener pauses after a resource error of accept4() or a
  // failed allocation, and consumed by the sweep, which resumes it. Retrying
  // inline would block the shared reactor thread for as long as the pressure
  // lasts.
  _Atomic bool listener_paused_for_resource_pressure;

  // Rate limit of the unexpected accept4() failure log. Plain fields, since one
  // listener's dispatches never overlap.
  struct timespec last_accept_err_log;
  bool last_accept_err_log_set;

  // idle_mutex guards the idle list (reactor-owned connections waiting for
  // their next request) and the parked list (connections waiting with no thread
  // for body bytes or write room). Both are claimed through
  // idle_list_try_claim; the separate parked list lets the sweep skip idle
  // keep-alive connections.
  ccol__mutex idle_mutex;
  chttpsvr_conn *idle_head, *idle_tail;
  chttpsvr_conn *parked_head, *parked_tail;

  // Every connection a worker owns. wait_in_flight_bounded forces a shutdown(2)
  // on these after its graceful wait, so a peer that stalls under an infinite
  // timeout can't hang a destroy, a restart or chttpsvr_engine_wait().
  ccol__mutex diverted_mutex;
  chttpsvr_conn *diverted_head, *diverted_tail;

  // Our slot index, for chttpsvr_finish_destroy when it runs from on_removed
  // with no handle at hand.
  uint32_t self_slot_idx;

  // Works like lifetime_refs of chttpsvr_conn: 1 until chttpsvr__destroy, plus
  // 1 for every connection registration and listener registration until its
  // on_removed fires. chttpsvr_finish_destroy frees the server at 0. The free
  // is deferred, never waited for, because an on_removed can fire on the
  // engine's reaper long after chttpsvr__destroy returns, and blocking there
  // would deadlock against a concurrent reap.
  _Atomic int lifetime_refs;

  ccol_memmgmt_procs *m_procs;

  // Slow-client settings resolved by chttpsvr_start, atomic for the same
  // restart reason. UINT_MAX means the floor or the wait is off, SIZE_MAX no
  // memory limit or an unlimited queue, and stream_threads -1 means streaming
  // handlers share the worker pool.
  _Atomic unsigned min_rate_bps;
  _Atomic unsigned rate_grace_ms;
  _Atomic size_t mem_cap;
  _Atomic unsigned mem_wait_ms;
  _Atomic int stream_threads;
  _Atomic size_t stream_qcap;
  _Atomic unsigned stream_qtimeout_ms;

  // The streaming pool, created by the first streaming request and guarded by
  // srv->mutex. stream_gen changes, under wait_mutex, each time the pool goes
  // out of service, so a task of a retired pool stops taking requests.
  cthrpool stream_pool;
  _Atomic uint64_t stream_gen;

  // Pools a restart retired while old requests may run on them. Each drains on
  // its own thread; srv->mutex guards the list and a teardown waits for all of
  // them.
  struct chttpsvr_retired_pools *retired_pools;

  // wait_mutex guards the memory accounting, the memory-wait list and the
  // streaming queue. It's taken only when a body reserves memory or a request
  // enters or leaves the streaming queue, and never held across I/O, a dispatch
  // or another lock of this server. mem_exempt_active means one connection went
  // past the limit (see mem_admit); mem_admitting and mem_admit_again serialize
  // mem_admit without recursion.
  ccol__mutex wait_mutex;
  size_t mem_in_use;
  size_t mem_holders;
  size_t mem_blocked;
  bool mem_exempt_active;
  bool mem_admission_closed;
  bool mem_admitting;
  bool mem_admit_again;
  chttpsvr_conn *mem_head, *mem_tail;
  size_t mem_waiting;

  // The streaming queue, oldest first. sq_tokens counts the current run's tasks
  // draining it (at most the pool's thread count) and sq_unclaimed those that
  // haven't taken a request yet, so no extra task starts for a request one is
  // already coming for.
  chttpsvr_conn *sq_head, *sq_tail;
  size_t sq_len;
  size_t sq_tokens;
  size_t sq_unclaimed;
};

// Tells whether srv is in the draining phase of a teardown, so the response
// paths stop keeping connections alive. Reads srv->draining with a relaxed
// atomic load.
static inline bool srv_draining(const struct chttpserver *srv) {
  return __atomic_load_n(&srv->draining, __ATOMIC_RELAXED);
}

// Puts srv into the draining phase when a teardown starts, so no new handler
// starts and no connection is kept alive. Sets srv->draining with a relaxed
// atomic store.
static void srv_begin_draining(struct chttpserver *srv) {
  __atomic_store_n(&srv->draining, true, __ATOMIC_RELAXED);
}

// Resolves h to its server and pins it, so a public call can use the server
// without a concurrent destroy freeing it. Returns NULL for 0, for garbage, for
// a free or reused slot, and for every handle once the table was released at
// unload. On success the caller must call chttpsvr_resolve_unpin() once before
// each return of its own short function.
// It reads the slot table under chttpsvr_slot_table.mutex and increments
// raw->pending_resolve_count without taking raw->mutex. That's safe because the
// slot-release step of chttpsvr__destroy needs the table mutex too, so raw
// can't be freed while we hold it.
static struct chttpserver *chttpsvr_resolve(chttpsvr h) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot *slot =
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  if (raw) atomic_fetch_add(&raw->pending_resolve_count, 1);
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

// Drops the pin chttpsvr_resolve took, and wakes a destroy that waits for the
// pins. The decrement and the broadcast on raw->resolve_cv both happen under
// raw->mutex; a bare atomic decrement outside the lock lets the destroy free
// raw before the broadcast touches it.
static void chttpsvr_resolve_unpin(struct chttpserver *raw) {
  ccol__mutex_lock(raw->mutex);
  atomic_fetch_sub(&raw->pending_resolve_count, 1);
  ccol__cond_var_broadcast(raw->resolve_cv);
  ccol__mutex_unlock(raw->mutex);
}

// Gives srv a slot in the handle table and returns the handle built from it, so
// chttpsvr_create_mp can publish the fully built server. Returns 0 when memory
// runs out or the table was released at unload. Under chttpsvr_slot_table.mutex
// it reuses a free index or appends a slot, bumps the generation (skipping 0 on
// wraparound, so a live server never gets a handle equal to CHTTPSVR_NULL),
// marks the slot in use and stores the index in srv->self_slot_idx.
static chttpsvr chttpsvr_handle_slot_acquire(struct chttpserver *srv) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  if (!chttpsvr_slot_table.slots) {
    ccol__mutex_unlock(chttpsvr_slot_table.mutex);
    return 0;
  }
  uint32_t idx;
  chttpsvr_slot *slot;
  if (cvector_elem_count(chttpsvr_slot_table.free_indices) > 0) {
    cvector_pop_back(chttpsvr_slot_table.free_indices, &idx);
    slot = (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
  } else {
    chttpsvr_slot fresh = {0};
    if (cvector_push_back(chttpsvr_slot_table.slots, &fresh) != ccol_success) {
      ccol__mutex_unlock(chttpsvr_slot_table.mutex);
      return 0;
    }
    idx = (uint32_t)cvector_elem_count(chttpsvr_slot_table.slots) - 1;
    slot = (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
  }
  slot->generation++;
  if (slot->generation == 0) slot->generation++;
  slot->ptr = srv;
  slot->in_use = true;
  srv->self_slot_idx = idx;
  chttpsvr h = ((chttpsvr)idx << 32) | (chttpsvr)slot->generation;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return h;
}

// Tells whether the exit-time destructor has released the slot table, so a late
// create can fail cleanly; reads it under the table mutex.
static bool chttpsvr_slot_table_released(void) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  bool released = chttpsvr_slot_table.slots == NULL;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return released;
}

// A process-wide thread-local key that records which server, if any, the
// calling thread runs worker_pool or reject_pool tasks for. task_worker and
// reject_task set it once and nothing clears it, because cthrpool threads
// belong to one pool for their whole life and a restart builds new pools with
// new threads.
// It lets chttpsvr_destroy(), chttpsvr_start() and chttpsvr_engine_wait()
// refuse a call from a handler or middleware of the server (or engine) that
// would have to wait for that same handler. Without it such a call waits about
// a minute for in_flight_requests to reach 0 and then aborts inside
// cthreadpool.c, or deadlocks for ever if an engine stop races in.
// Creating the key can fail when the process holds PTHREAD_KEYS_MAX keys, and
// key 0 then belongs to some other component. ready records that creation
// worked, and nothing touches key without it. chttpsvr_start refuses to start a
// server without the key; the module destructor deletes it once no server is
// left (see cleanup_chttpsvr_slot_table).
static struct {
  ccol__thread_ls_key key;
  ccol__once_flag once;
  // Serializes creating and deleting key.
  ccol__mutex mutex;
  _Atomic bool ready;
} chttpsvr_worker_key_bundle = {0};

// Initializes the key bundle's mutex once; the worker key functions need it
// before they create or delete the key.
static void chttpsvr_worker_key_init_globals(void) {
  if (ccol__mutex_init(chttpsvr_worker_key_bundle.mutex) != 0)
    ccol__fatal_err("chttpsvr worker key: failed to initialize mutex");
}

// Creates the worker key if it doesn't exist yet and tells whether it exists.
// chttpsvr_start calls it and refuses to start a server without the key, so no
// worker thread ever runs unmarked. It creates the key under the bundle mutex
// and publishes ready with a release store; a failed creation is retried by the
// next start.
static bool chttpsvr_worker_key_ensure(void) {
  if (atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                           memory_order_acquire))
    return true;
  ccol__call_once(chttpsvr_worker_key_bundle.once,
                  chttpsvr_worker_key_init_globals);
  ccol__mutex_lock(chttpsvr_worker_key_bundle.mutex);
  if (!atomic_load(&chttpsvr_worker_key_bundle.ready) &&
      ccol__thread_ls_key_create(chttpsvr_worker_key_bundle.key, NULL) == 0)
    atomic_store_explicit(&chttpsvr_worker_key_bundle.ready, true,
                          memory_order_release);
  bool ready = atomic_load(&chttpsvr_worker_key_bundle.ready);
  ccol__mutex_unlock(chttpsvr_worker_key_bundle.mutex);
  return ready;
}

// Tells whether the worker key exists, so the mark and self-call checks never
// touch a key of another component. A thread can only mark itself when it does,
// so every reader takes a missing key as "not a worker".
static inline bool chttpsvr_worker_key_ready(void) {
  return atomic_load_explicit(&chttpsvr_worker_key_bundle.ready,
                              memory_order_acquire);
}

// Marks the calling thread as a permanent worker of srv's pools, which the
// self-call checks read later. task_worker and reject_task call it once at
// their top; it sets the thread-local worker key.
static void chttpsvr_mark_worker_thread(struct chttpserver *srv) {
  if (!chttpsvr_worker_key_ready()) return;
  ccol__thread_ls_set(chttpsvr_worker_key_bundle.key, (void *)srv);
}

// Tells whether the calling thread is a worker of srv's worker_pool or
// reject_pool, so chttpsvr_destroy() and chttpsvr_start() can refuse a call
// from a handler, middleware or reject task of srv that would wait for itself.
// Reads the thread-local worker key.
static bool chttpsvr_is_self_call(struct chttpserver *srv) {
  if (!chttpsvr_worker_key_ready()) return false;
  return ccol__thread_ls_get(chttpsvr_worker_key_bundle.key) == (void *)srv;
}

// Tells whether the calling thread is a worker of any server, so
// chttpsvr_engine_wait() can refuse a call from a handler. An engine stop
// drains the worker pool of every registered server with no timeout, so a
// handler of any server that waits for the engine would wait for itself and
// deadlock the whole engine. Reads the thread-local worker key.
static bool chttpsvr_is_any_worker_call(void) {
  if (!chttpsvr_worker_key_ready()) return false;
  return ccol__thread_ls_get(chttpsvr_worker_key_bundle.key) != NULL;
}

// The one ceventloop reactor of the process, shared by every server, with its
// refcount, reaper thread and settings. chttpclient has a reactor of its own,
// so nothing here coordinates with the client. mutex guards every field unless
// a member comment says otherwise.
static struct {
  // Written only under mutex, by engine_acquire and the reaper. Many places
  // read it with no lock; that's safe because such a reader always runs while
  // it (or its server) holds a live engine reference, which keeps the value
  // fixed between creation and reap, and thread create, join and the handle
  // pins order the writes against the readers. A reader that holds no engine
  // reference would need this field to be _Atomic.
  ceventloop reactor;
  size_t reactor_refs;
  ccol__mutex mutex;
  ccol__cond_var stopped_cv;
  ccol__once_flag once;
  // True while a reap is tearing the reactor down. reaper_thread is the reaper
  // spawn_reaper recorded, joinable until somebody joins it.
  bool stopping;
  ccol__thread_id reaper_thread;
  bool reaper_joinable;
  // The engine allocator: mprocs points at mprocs_storage, or is NULL for the
  // default. Like num_reactor_threads it's built into the reactor at creation,
  // so it can change only before the first start or after a full stop.
  ccol_memmgmt_procs mprocs_storage;
  ccol_memmgmt_procs *mprocs;
  // 0 picks the default of one reactor thread, which runs every callback inline
  // and is the faster choice for ordinary traffic. A positive value pins the
  // reactor to that many threads.
  size_t num_reactor_threads;
  // The thread count the last reactor was created with, for the tests.
  size_t last_resolved_num_reactor_threads;
  // The engine logger for the reactor thread's diagnostics. It's user_log when
  // the caller installed one, else a CLOG_FATAL fallback on fd 2 that
  // engine_acquire creates and the reaper closes. It's never CLOG_NULL while a
  // reactor exists, because clog has no guard for a NULL handle; engine_acquire
  // destroys the reactor again when the fallback can't be allocated.
  clog log;
  // The logger chttpsvr_set_engine_logger() derived, or CLOG_NULL. It outlives
  // every engine: Each start logs through it and each reaper leaves it open.
  // Only a later chttpsvr_set_engine_logger(), process exit or unload closes
  // it.
  clog user_log;
  // Set when the exit-time destructor couldn't close user_log because an engine
  // still ran; that engine's reaper closes it.
  bool close_user_log_at_stop;
  // Generation of the running reactor, or 0 when none runs. A server tags its
  // engine reference with it so a late release never touches a newer engine's
  // refcount. chttpsvr_engine_stop() reads it with one lock-free load, which is
  // async-signal-safe. Written under mutex; gen_counter hands out the values.
  _Atomic uintptr_t generation;
  uintptr_t gen_counter;
  // The process that created the current or last reactor. A fork() child
  // inherits the reactor but none of its threads; see
  // engine_forget_inherited_locked.
  pid_t reactor_pid;
} srv_engine_bundler = {0};

// The one idle-timeout sweep thread of the process. It walks the idle and
// parked lists of every started server. servers_bundler.mutex guards running
// and pid.
static struct {
  ccol__thread_id thread;
  bool running;
  // The process that started the thread. A fork() child that finds running set
  // by another process has no thread to join (see
  // idle_sweep_forget_inherited_locked).
  pid_t pid;
  // Atomic because idle_sweep_fn reads it in its loop with no lock. A plain
  // bool is a data race, and the compiler could cache the read so the thread
  // never stops and the join hangs.
  _Atomic bool stop_flag;
  // The sweep waits for its next tick on wake_cv (CLOCK_MONOTONIC). A stop sets
  // stop_flag under wake_mu and signals wake_cv, so the join doesn't sit out
  // the rest of a tick. wake_mu is a leaf lock.
  ccol__mutex wake_mu;
  ccol__cond_var wake_cv;
} idle_sweep_bundler = {0};

// The registry of started servers, a plain realloc'ed array. The sweep walks it
// and an engine stop quiesces every server in it.
static struct {
  ccol__mutex mutex;
  struct chttpserver **servers;
  size_t count;
  size_t capacity;
} servers_bundler = {0};

static void idle_sweep_stop_if_running(void);
static void engine_request_reaper_join(void);
static void quiesce_server_once(struct chttpserver *srv);
static bool engine_release_begin(uintptr_t gen);
static void close_idle_connections(struct chttpserver *srv,
                                   bool include_parked);
static void spawn_reaper(void);
static void chttpsvr_stop_internal(struct chttpserver *raw);
static void engine_globals_init(void);
#ifdef RUNNING_UNIT_TESTS
static void reaper_race_hook_wait_if_armed(void);
static void listener_dispatch_race_hook_wait_if_armed(bool at_entry);
static void quiesce_teardown_race_hook_wait_if_armed(void);
#endif

// Quiesces every server still in servers_bundler: Stop listening, drain, close
// connections, destroy the pools and drop the engine reference. The engine
// reaper calls it before it destroys the reactor, because after a
// chttpsvr_engine_stop() the servers may be fully live, and the reactor and the
// array would otherwise be freed under registrations that point into them. On
// the graceful path the list usually holds only servers whose quiesce runs on
// another thread (we wait for each as a loser) or that a concurrent start
// registered.
// It first sets srv->draining on every server and closes their idle
// connections, so none keeps serving keep-alive clients while the drains ahead
// of it run; that pass walks the array from the end, because an unregister
// moves the last element into the freed slot. The main loop reads servers[0]
// again on every pass, since quiesce_server_once unregisters the server, and
// ends when servers_bundler.count is 0.
// Each server is pinned through srv->servers_bundler_pins, taken under
// servers_bundler.mutex and dropped under srv->mutex with a broadcast, because
// a concurrent chttpsvr_destroy() could free it during the long quiesce.
// pending_resolve_count would self-deadlock, since quiesce_server_once waits
// for it to reach 0. Seeing the same server twice in a row means a concurrent
// restart registered it again, so we nap 1 ms instead of spinning.
static void engine_force_stop_quiesce_all(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++)
    srv_begin_draining(servers_bundler.servers[i]);
  size_t idle_pos = servers_bundler.count;
  ccol__mutex_unlock(servers_bundler.mutex);
  for (;;) {
    ccol__mutex_lock(servers_bundler.mutex);
    if (idle_pos > servers_bundler.count) idle_pos = servers_bundler.count;
    if (idle_pos == 0) {
      ccol__mutex_unlock(servers_bundler.mutex);
      break;
    }
    struct chttpserver *srv = servers_bundler.servers[--idle_pos];
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    ccol__mutex_unlock(servers_bundler.mutex);
    close_idle_connections(srv, false);
    ccol__mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    ccol__cond_var_broadcast(srv->resolve_cv);
    ccol__mutex_unlock(srv->mutex);
  }
  struct chttpserver *prev_unremoved = NULL;
  for (;;) {
    ccol__mutex_lock(servers_bundler.mutex);
    if (servers_bundler.count == 0) {
      ccol__mutex_unlock(servers_bundler.mutex);
      return;
    }
    struct chttpserver *srv = servers_bundler.servers[0];
    atomic_fetch_add(&srv->servers_bundler_pins, 1);
    ccol__mutex_unlock(servers_bundler.mutex);

#ifdef RUNNING_UNIT_TESTS
    reaper_race_hook_wait_if_armed();
#endif

    if (srv == prev_unremoved) {
      struct timespec ts = {0, 1000000L};
      nanosleep(&ts, NULL);
    }
    prev_unremoved = srv;
    quiesce_server_once(srv);
    ccol__mutex_lock(srv->mutex);
    atomic_fetch_sub(&srv->servers_bundler_pins, 1);
    ccol__cond_var_broadcast(srv->resolve_cv);
    ccol__mutex_unlock(srv->mutex);
  }
}

// Creates idle_sweep_bundler.wake_cv on CLOCK_MONOTONIC, the clock the sweep
// computes its deadlines with, so a stop can wake the sweep early. Returns 0 on
// success.
static int idle_sweep_init_wake_cv(void) {
  ccol__cond_var_attr cv_attr;
  if (ccol__cond_var_attr_init(cv_attr) != 0) return 1;
  int rc = ccol__cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
  if (rc == 0) rc = ccol__cond_var_init_ca(idle_sweep_bundler.wake_cv, cv_attr);
  ccol__cond_var_attr_destroy(cv_attr);
  return rc;
}

// Initializes the engine, registry and sweep locks once, before any engine
// function uses them; a failure is fatal. We leave SIGPIPE alone: Every plain
// write sends with MSG_NOSIGNAL through chttp1_stream, and every TLS write goes
// through the ctls socket BIO, which does the same.
static void engine_globals_init(void) {
  if (ccol__mutex_init(srv_engine_bundler.mutex) != 0)
    ccol__fatal_err("chttpserver engine: failed to initialize mutex");
  if (ccol__cond_var_init(srv_engine_bundler.stopped_cv) != 0)
    ccol__fatal_err(
        "chttpserver engine: failed to initialize condition variable");
  if (ccol__mutex_init(servers_bundler.mutex) != 0)
    ccol__fatal_err(
        "chttpserver engine: failed to initialize servers_bundler mutex");
  if (ccol__mutex_init(idle_sweep_bundler.wake_mu) != 0)
    ccol__fatal_err("chttpserver engine: failed to initialize the sweep mutex");
  if (idle_sweep_init_wake_cv() != 0)
    ccol__fatal_err(
        "chttpserver engine: failed to initialize the sweep condition "
        "variable");
}

// Logs a record through the engine logger with level_call (clog_info,
// clog_error, ...) and the format and arguments that follow, so any engine code
// can log without racing a logger swap. It's a statement and evaluates to
// nothing. It reads srv_engine_bundler.log under srv_engine_bundler.mutex and
// logs inside that critical section; copying the handle out and logging later
// is a use-after-free, because chttpsvr_set_engine_logger() swaps the logger
// under the mutex and closes the old one after unlocking. It does nothing while
// log is CLOG_NULL. It's a macro so the record names the real call site.
#define SRV_ENGINE_LOG(level_call, ...)                            \
  do {                                                             \
    ccol__call_once(srv_engine_bundler.once, engine_globals_init); \
    ccol__mutex_lock(srv_engine_bundler.mutex);                    \
    if (srv_engine_bundler.log)                                    \
      level_call(srv_engine_bundler.log, __VA_ARGS__);             \
    ccol__mutex_unlock(srv_engine_bundler.mutex);                  \
  } while (0)

// Joins the recorded reaper thread if it's still joinable, so its stack isn't
// kept for the life of the process, and clears reaper_joinable. The caller
// holds srv_engine_bundler.mutex and must know the reaper already cleared
// stopping, so it won't wait on this mutex.
static void join_reaper_if_needed_locked(void) {
  if (srv_engine_bundler.reaper_joinable) {
    ccol__thread_join(srv_engine_bundler.reaper_thread);
    srv_engine_bundler.reaper_joinable = false;
  }
}

// Tears down the shared engine; spawn_reaper runs it when the last reference
// goes or chttpsvr_engine_stop() asks. It quiesces every registered server
// first (a forced stop can find them live), then stops the sweep thread, which
// calls into the reactor, and only then destroys the reactor. The servers array
// is freed only when empty, because a start that registers during the teardown
// belongs to the next engine.
// Under srv_engine_bundler.mutex it clears reactor, generation and stopping,
// closes user_log when the exit destructor asked for it, and wakes
// chttpsvr_engine_wait(). The user_log's last record is logged inside the lock,
// since a concurrent chttpsvr_set_engine_logger() may close that logger right
// after; the fallback logger is taken out of log first, so its record and close
// happen after the unlock and a slow sink never holds the mutex.
// The last statement asks the stop watcher to join this thread, so an
// application that only destroys its last server doesn't leave it unjoined. It
// must come last and take no lock, because the watcher may join at once.
static void *engine_reaper_fn(void *arg) {
  (void)arg;
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ceventloop loop_to_destroy;
  ccol__mutex_lock(srv_engine_bundler.mutex);
  loop_to_destroy = srv_engine_bundler.reactor;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  engine_force_stop_quiesce_all();
  idle_sweep_stop_if_running();
  if (loop_to_destroy) ceventloop_destroy(loop_to_destroy);

  ccol__mutex_lock(servers_bundler.mutex);
  if (servers_bundler.count == 0) {
    free(servers_bundler.servers);
    servers_bundler.servers = NULL;
    servers_bundler.capacity = 0;
  }
  ccol__mutex_unlock(servers_bundler.mutex);

  ccol__mutex_lock(srv_engine_bundler.mutex);
  srv_engine_bundler.reactor = CEVENTLOOP_NULL;
  atomic_store(&srv_engine_bundler.generation, (uintptr_t)0);
  srv_engine_bundler.stopping = false;
  clog old_logger = srv_engine_bundler.log;
  srv_engine_bundler.log = CLOG_NULL;
  if (old_logger && old_logger == srv_engine_bundler.user_log) {
    clog_info(old_logger, "The http server reactor engine has been destroyed");
    old_logger = CLOG_NULL;
  }
  clog closing_user_log = CLOG_NULL;
  if (srv_engine_bundler.close_user_log_at_stop) {
    closing_user_log = srv_engine_bundler.user_log;
    srv_engine_bundler.user_log = CLOG_NULL;
    srv_engine_bundler.close_user_log_at_stop = false;
  }
  ccol__cond_var_broadcast(srv_engine_bundler.stopped_cv);
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  if (closing_user_log) clog_close(closing_user_log);

  if (old_logger) {
    clog_info(old_logger, "The http server reactor engine has been destroyed");
    clog_close(old_logger);
  }
  engine_request_reaper_join();
  return NULL;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: A delay in ms between creating the reaper and recording its
// thread, so the reaper can run to its end inside that window.
static _Atomic unsigned g_spawn_reaper_delay_ms_for_tests = 0;
void chttpsvr__set_spawn_reaper_delay_for_tests(unsigned ms) {
  atomic_store(&g_spawn_reaper_delay_ms_for_tests, ms);
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: While set, creating the reaper thread fails and spawn_reaper runs
// the reaper inline.
static _Atomic bool g_force_reaper_thread_create_fail_for_tests = false;
void chttpsvr__force_reaper_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_reaper_thread_create_fail_for_tests, force);
}
#endif

// Starts the engine reaper on its own thread, so chttpsvr_destroy() and the
// stop watcher return without waiting for the teardown. It creates the thread
// and records it in reaper_thread and reaper_joinable under
// srv_engine_bundler.mutex, which the reaper needs before it ends, so the
// reaper can't ask for its join, and no second reaper can start, before it's
// recorded.
// When the thread can't be created the reaper runs inline. It then waits for
// every quiesce in progress, so the caller must not be quiescing a registered
// server; quiesce_server_once spawns only after it unregisters.
static void spawn_reaper(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  ccol__thread_id reaper;
  bool reaper_create_fails = false;
#ifdef RUNNING_UNIT_TESTS
  reaper_create_fails =
      atomic_load(&g_force_reaper_thread_create_fail_for_tests);
#endif
  if (reaper_create_fails ||
      ccol__thread_create(reaper, engine_reaper_fn, NULL) != 0) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    engine_reaper_fn(NULL);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  unsigned delay_ms = atomic_load(&g_spawn_reaper_delay_ms_for_tests);
  if (delay_ms) {
    struct timespec nap = {(time_t)(delay_ms / 1000),
                           (long)(delay_ms % 1000) * 1000000L};
    nanosleep(&nap, NULL);
  }
#endif
  srv_engine_bundler.reaper_thread = reaper;
  srv_engine_bundler.reaper_joinable = true;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
}

static bool engine_stop_watcher_ensure_started_locked(void);
static void engine_forget_inherited_locked(void);

// Takes a reference on the shared engine for chttpsvr_start, creating the
// reactor and its logger when none runs. Returns ccol_success with the engine
// generation in *out_gen, ccol_not_enough_memory, or ccol_unexpected_failure
// when the stop watcher can't start (that engine would ignore
// chttpsvr_engine_stop()).
// *out_currently_stopping is always set. It's true only when stopping shows a
// reap in progress; then we return at once without a reference and the return
// value means nothing. We can't wait for the reap, because the calling
// chttpsvr_start() holds its resolve pin and the reap's quiesce of that server
// waits for that pin. The caller drops the pin and retries.
// Under srv_engine_bundler.mutex it forgets an engine inherited across fork(),
// starts the watcher, joins a finished reaper, and on creation sets reactor,
// reactor_pid, log and a new generation; it always increments reactor_refs on
// success. A logger that can't be allocated makes us destroy the new reactor
// synchronously under the mutex, which is safe because nothing the join waits
// for takes this mutex. The "created" record is logged under the lock, because
// a concurrent chttpsvr_set_engine_logger() could close the logger right after
// the unlock.
static ccol_retval engine_acquire(bool *out_currently_stopping,
                                  uintptr_t *out_gen) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  engine_forget_inherited_locked();
  *out_currently_stopping = false;
  if (!engine_stop_watcher_ensure_started_locked()) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    return ccol_unexpected_failure;
  }
  if (srv_engine_bundler.stopping) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    *out_currently_stopping = true;
    return ccol_unexpected_failure;
  }
  join_reaper_if_needed_locked();

  if (!srv_engine_bundler.reactor) {
    size_t nthreads = srv_engine_bundler.num_reactor_threads;
    if (nthreads == 0) nthreads = 1;
    srv_engine_bundler.last_resolved_num_reactor_threads = nthreads;
    char *err = NULL;
    srv_engine_bundler.reactor =
        ceventloop_create_mp(256, 4, nthreads, srv_engine_bundler.mprocs, &err);
    if (!srv_engine_bundler.reactor) {
      ccol__mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_enough_memory;
    }
    srv_engine_bundler.reactor_pid = getpid();

    if (!srv_engine_bundler.log && srv_engine_bundler.user_log)
      srv_engine_bundler.log = srv_engine_bundler.user_log;
    if (!srv_engine_bundler.log) {
      srv_engine_bundler.log = clog_create_from_fd_mp(
          2, CLOG_FATAL, NULL, srv_engine_bundler.mprocs);
      if (!srv_engine_bundler.log) {
        ceventloop_destroy(srv_engine_bundler.reactor);
        srv_engine_bundler.reactor = CEVENTLOOP_NULL;
        ccol__mutex_unlock(srv_engine_bundler.mutex);
        return ccol_not_enough_memory;
      }
      clog_set_field(srv_engine_bundler.log, "component", "http-server-engine");
    }

    clog_info(srv_engine_bundler.log,
              "New http server reactor engine has been created");
    if (++srv_engine_bundler.gen_counter == 0) srv_engine_bundler.gen_counter++;
    atomic_store(&srv_engine_bundler.generation,
                 srv_engine_bundler.gen_counter);
  }
  srv_engine_bundler.reactor_refs++;
  *out_gen = atomic_load(&srv_engine_bundler.generation);
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

// Drops an engine reference of generation gen, the first half of
// engine_release; quiesce_server_once calls it directly so it can spawn the
// reaper only after it unregisters. Returns true when this was the last
// reference, and then the caller must call spawn_reaper().
// Under srv_engine_bundler.mutex it decrements reactor_refs and, on the last
// one, sets stopping. A gen that isn't the running generation is ignored, so a
// late release can't take a newer engine's refcount. With stopping already set
// (a forced stop's quiesce pass) it never reports the last reference, so no
// second reaper starts.
static bool engine_release_begin(uintptr_t gen) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  bool should_reap = false;
  ccol__mutex_lock(srv_engine_bundler.mutex);
  if (gen == 0 || gen != atomic_load(&srv_engine_bundler.generation)) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    return false;
  }
  if (srv_engine_bundler.reactor_refs > 0) srv_engine_bundler.reactor_refs--;
  if (srv_engine_bundler.reactor_refs == 0 && srv_engine_bundler.reactor &&
      !srv_engine_bundler.stopping) {
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return should_reap;
}

// Drops an engine reference of a server that stops or fails to start, and
// starts the reaper when it was the last.
static void engine_release(uintptr_t gen) {
  if (engine_release_begin(gen)) spawn_reaper();
}

// Waits until no reactor exists and no reap runs, for chttpsvr_engine_wait(),
// then joins the reaper. It waits on stopped_cv under srv_engine_bundler.mutex
// and forgets an engine inherited across fork() first, so a child never waits
// for threads it doesn't have.
static void engine_wait_until_stopped(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  engine_forget_inherited_locked();
  while (srv_engine_bundler.reactor || srv_engine_bundler.stopping)
    ccol__cond_var_wait(srv_engine_bundler.stopped_cv,
                        srv_engine_bundler.mutex);
  join_reaper_if_needed_locked();
  ccol__mutex_unlock(srv_engine_bundler.mutex);
}

// Stops the engine of generation gen; the stop watcher calls it for
// chttpsvr_engine_stop(). It locks a mutex and may create a thread, so it runs
// only on an ordinary thread, never in a signal handler. Under
// srv_engine_bundler.mutex it zeroes reactor_refs and sets stopping, then
// spawns the reaper. It does nothing when no reactor runs, when gen isn't the
// running generation, or when stopping is set: A repeated or overlapping stop,
// such as a second SIGTERM, would otherwise start two reapers that destroy the
// same reactor, which is fatal.
static void engine_force_stop_now(uintptr_t gen) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  bool should_reap = false;
  ccol__mutex_lock(srv_engine_bundler.mutex);
  engine_forget_inherited_locked();
  if (srv_engine_bundler.reactor && !srv_engine_bundler.stopping &&
      gen == atomic_load(&srv_engine_bundler.generation)) {
    srv_engine_bundler.reactor_refs = 0;
    srv_engine_bundler.stopping = true;
    should_reap = true;
  }
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  if (should_reap) spawn_reaper();
}

// The stop watcher. chttpsvr_engine_stop() is documented as async-signal-safe,
// while the stop itself locks srv_engine_bundler.mutex and creates a thread. A
// signal that lands on a thread holding that mutex (a SIGTERM in the middle of
// chttpsvr_start(), say) would deadlock. So the stop call only does an atomic
// load, an atomic store and a semaphore post, and this always-running thread
// does the real work.
static struct {
  ccol__semaphore sem;
  // True once sem and thread are live; the only field chttpsvr_engine_stop()
  // reads.
  _Atomic bool ready;
  ccol__thread_id thread;
  // Guards the start attempts, under srv_engine_bundler.mutex. It's separate
  // from ready so a failed start can be retried later.
  bool started;
  _Atomic bool exit_requested;
  // Each kind of work has its own request flag, set before its post, so an
  // extra post is harmless and a join request is never taken for a stop.
  // stop_requested holds the generation a stop names, or 0, and the watcher
  // stops only that generation. The reaper sets join_reaper_requested as its
  // last act.
  _Atomic uintptr_t stop_requested;
  _Atomic bool join_reaper_requested;
  // The process that started the watcher thread, guarded by
  // srv_engine_bundler.mutex.
  pid_t pid;
} g_engine_stop_watcher = {0};

// Forgets the engine a fork() child inherited: The reactor handle, its
// references and generation, the fallback logger and the reaper and watcher
// flags, with none of their threads. Without this the child's servers would use
// a reactor with no poller and its waits would never end, so the child starts
// with no engine. It runs under srv_engine_bundler.mutex on every path that
// acquires, stops or waits for the engine, and decides by comparing reactor_pid
// and the watcher's pid with getpid(), so it works with
// CCOL_FORK_SAFETY_REQUIRED=0. It resets those fields and closes the inherited
// fallback logger, but leaves the inherited reactor itself alone, since its
// threads belong to the parent.
static void engine_forget_inherited_locked(void) {
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
  srv_engine_bundler.log = CLOG_NULL;
  if (inherited_log && inherited_log != srv_engine_bundler.user_log)
    clog_close(inherited_log);
  srv_engine_bundler.reactor = CEVENTLOOP_NULL;
  srv_engine_bundler.reactor_refs = 0;
  srv_engine_bundler.stopping = false;
  srv_engine_bundler.reaper_joinable = false;
  atomic_store(&srv_engine_bundler.generation, (uintptr_t)0);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Returns the process that created the running reactor, or 0, so a
// test can check that a fork() child runs a reactor of its own.
pid_t chttpsvr__engine_reactor_pid_for_tests(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  pid_t pid = srv_engine_bundler.reactor ? srv_engine_bundler.reactor_pid : 0;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return pid;
}
#endif

// Asks the watcher to join the reaper; the reaper calls it as its last act, so
// its thread is joined even when nobody waits for the engine. It always raises
// join_reaper_requested, even with no watcher, so the backstop in
// cleanup_engine_stop_watcher can find a request nobody serviced, and posts the
// semaphore when the watcher is ready.
static void engine_request_reaper_join(void) {
  atomic_store(&g_engine_stop_watcher.join_reaper_requested, true);
  if (atomic_load(&g_engine_stop_watcher.ready))
    ccol__semaphore_post(g_engine_stop_watcher.sem);
}

// Joins a finished reaper for the watcher and the exit destructor, so no
// returned thread keeps its stack mapped. The join under
// srv_engine_bundler.mutex is safe only because the reaper takes no lock after
// it clears stopping. With stopping set a new reap runs, which means the
// previous reaper was already joined, and the new one posts its own request.
static void engine_join_finished_reaper(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  if (!srv_engine_bundler.stopping) join_reaper_if_needed_locked();
  ccol__mutex_unlock(srv_engine_bundler.mutex);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Returns true while a spawned reaper thread is still unjoined, so a
// test can check that the watcher joins it.
bool chttpsvr__engine_reaper_unjoined_for_tests(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  bool unjoined = srv_engine_bundler.reaper_joinable;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return unjoined;
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: A delay in ms the watcher waits after each wakeup before it acts
// on a stop request.
static _Atomic unsigned g_engine_stop_watcher_delay_ms_for_tests = 0;
void chttpsvr__set_engine_stop_watcher_delay_for_tests(unsigned ms) {
  atomic_store(&g_engine_stop_watcher_delay_ms_for_tests, ms);
}
// Sleeps for the test delay above, if one is set, so a test can start an engine
// between a stop request and its handling.
static void engine_stop_watcher_delay_if_armed(void) {
  unsigned ms = atomic_load(&g_engine_stop_watcher_delay_ms_for_tests);
  if (!ms) return;
  struct timespec nap = {(time_t)(ms / 1000), (long)(ms % 1000) * 1000000L};
  nanosleep(&nap, NULL);
}
#endif

// The stop watcher thread. It exists so chttpsvr_engine_stop() can stay
// async-signal-safe while the stop itself needs a mutex and a new thread. It
// waits on the semaphore, then serves a pending reaper join and a pending stop
// request (each read and cleared with an atomic exchange), until exit_requested
// is set.
static void *engine_stop_watcher_fn(void *arg) {
  (void)arg;
  for (;;) {
    ccol__semaphore_wait(g_engine_stop_watcher.sem);
    if (atomic_load(&g_engine_stop_watcher.exit_requested)) return NULL;
    if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
      engine_join_finished_reaper();
#ifdef RUNNING_UNIT_TESTS
    engine_stop_watcher_delay_if_armed();
#endif
    uintptr_t stop_gen =
        atomic_exchange(&g_engine_stop_watcher.stop_requested, (uintptr_t)0);
    if (stop_gen != 0) engine_force_stop_now(stop_gen);
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: While set, creating the watcher thread fails.
static _Atomic bool g_force_watcher_thread_create_fail_for_tests = false;
void chttpsvr__force_watcher_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_watcher_thread_create_fail_for_tests, force);
}
#endif

// Starts the watcher thread if it isn't running and tells whether it runs, so
// engine_acquire can refuse to run an engine that chttpsvr_engine_stop()
// couldn't reach. The caller holds srv_engine_bundler.mutex on an ordinary
// thread. On success it sets started, pid and ready; a failure leaves them
// unset and is retried on every later call. A stop before the first engine
// start does nothing, and it's never replayed against a later start.
static bool engine_stop_watcher_ensure_started_locked(void) {
  if (g_engine_stop_watcher.started) return true;
  if (ccol__semaphore_init(g_engine_stop_watcher.sem, 0) != 0) return false;
  bool created = false;
#ifdef RUNNING_UNIT_TESTS
  if (!atomic_load(&g_force_watcher_thread_create_fail_for_tests))
#endif
    created = ccol__thread_create(g_engine_stop_watcher.thread,
                                  engine_stop_watcher_fn, NULL) == 0;
  if (!created) {
    ccol__semaphore_destroy(g_engine_stop_watcher.sem);
    return false;
  }
  g_engine_stop_watcher.started = true;
  g_engine_stop_watcher.pid = getpid();
  atomic_store(&g_engine_stop_watcher.ready, true);
  return true;
}

// At process exit, joins the watcher and any reaper it didn't get to, so
// valgrind sees no thread stack left behind, and closes the
// chttpsvr_set_engine_logger() logger. started and thread are written once and
// never reset, so reading them unlocked is safe. ready is cleared before the
// semaphore is destroyed, because a signal handler may still call
// chttpsvr_engine_stop() during exit. When an engine still runs, the logger
// stays open and close_user_log_at_stop asks that engine's reaper to close it.
__attribute__((destructor)) static void cleanup_engine_stop_watcher(void) {
  if (g_engine_stop_watcher.started) {
    atomic_store(&g_engine_stop_watcher.exit_requested, true);
    ccol__semaphore_post(g_engine_stop_watcher.sem);
    ccol__thread_join(g_engine_stop_watcher.thread);
    atomic_store(&g_engine_stop_watcher.ready, false);
    ccol__semaphore_destroy(g_engine_stop_watcher.sem);
  }
  if (atomic_exchange(&g_engine_stop_watcher.join_reaper_requested, false))
    engine_join_finished_reaper();

  if (srv_engine_bundler.user_log) {
    ccol__mutex_lock(srv_engine_bundler.mutex);
    clog user_log = CLOG_NULL;
    if (!srv_engine_bundler.log) {
      user_log = srv_engine_bundler.user_log;
      srv_engine_bundler.user_log = CLOG_NULL;
    } else {
      srv_engine_bundler.close_user_log_at_stop = true;
    }
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    if (user_log) clog_close(user_log);
  }
}

// Requests a stop of the running engine; it's the body of
// chttpsvr_engine_stop(), which a signal handler may call, so it uses only
// atomic operations and a semaphore post. It reads generation and the watcher's
// ready flag and does nothing when no engine runs or the watcher isn't up.
// Otherwise it stores the generation in stop_requested and wakes the watcher.
// The flag is set only inside the ready test, so a stale request never waits
// for some unrelated later post.
static void engine_force_stop(void) {
  uintptr_t gen = atomic_load(&srv_engine_bundler.generation);
  if (gen != 0 && atomic_load(&g_engine_stop_watcher.ready)) {
    atomic_store(&g_engine_stop_watcher.stop_requested, gen);
    ccol__semaphore_post(g_engine_stop_watcher.sem);
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Raises sig on this thread while it holds srv_engine_bundler.mutex,
// to prove that a signal handler which calls chttpsvr_engine_stop() never
// deadlocks. Such a handler deadlocks here if that call ever takes the mutex or
// creates a thread.
void chttpsvr__test_hold_engine_mutex_and_signal_self(int sig) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  raise(sig);
  ccol__mutex_unlock(srv_engine_bundler.mutex);
}
#endif

static void conn_close(chttpsvr_conn *conn);
static void conn_linger(chttpsvr_conn *conn);
static void conn_linger_step(chttpsvr_conn *conn);
static bool conn_request_bytes_may_remain(chttpsvr_conn *conn);
static void conn_reject_and_close(chttpsvr_conn *conn, bool run_middleware);
static void chttpsvr_finish_destroy(struct chttpserver *raw);
static void retired_pools_reap(struct chttpserver *srv, bool wait_all);

static void timespec_add_ms(struct timespec *ts, unsigned ms);
static void slow_clock(struct timespec *ts);
static void mem_release(chttpsvr_conn *conn);
static void sweep_parked_waits(struct chttpserver *srv,
                               struct timespec now_slow);
static bool parked_expired(struct chttpserver *srv, chttpsvr_conn *c,
                           struct timespec now, struct timespec now_slow);
static bool parked_needs_kernel_query(struct chttpserver *srv,
                                      const chttpsvr_conn *c,
                                      struct timespec now,
                                      struct timespec now_slow);
#ifdef RUNNING_UNIT_TESTS
// Test counters: How many kernel queries judged a parked connection, and how
// many of those the sweep made while holding idle_mutex (that must stay 0).
static _Atomic size_t g_parked_queries_for_tests = 0;
static _Atomic size_t g_parked_queries_locked_for_tests = 0;
static __thread bool g_sweep_holds_idle_mutex_for_tests = false;
// Counts one kernel query of a parked connection, and counts it again as locked
// when the sweep holds idle_mutex, for the test that checks the sweep queries
// outside that lock.
static void parked_query_note_for_tests(void) {
  atomic_fetch_add_explicit(&g_parked_queries_for_tests, 1,
                            memory_order_relaxed);
  if (g_sweep_holds_idle_mutex_for_tests)
    atomic_fetch_add_explicit(&g_parked_queries_locked_for_tests, 1,
                              memory_order_relaxed);
}
// Test hook: Reads the two counters above into *total and *locked.
void chttpsvr__parked_queries_for_tests(size_t *total, size_t *locked) {
  *total = atomic_load(&g_parked_queries_for_tests);
  *locked = atomic_load(&g_parked_queries_locked_for_tests);
}
#endif
static void conn_resume_parked(chttpsvr_conn *conn);
static bool conn_divert_gate(chttpsvr_conn *conn);
static ssize_t drain_body_socket_read(chttpsvr_conn *conn,
                                      chttp1_stream *stream, char *raw,
                                      size_t len);
static void send_response_park(chttpsvr_conn *conn, chttp1_stream *stream,
                               const char *head, size_t head_len,
                               size_t body_pos, bool keep_alive, bool no_body,
                               bool internal);
static void conn_park_write(chttpsvr_conn *conn, struct chttpserver *srv);
static bool conn_park_arm(chttpsvr_conn *conn, uint8_t kind);
static void conn_end_parked_write(chttpsvr_conn *conn);
static void conn_dispatch_reject(chttpsvr_conn *conn);
static void parked_unlink_locked(struct chttpserver *srv, chttpsvr_conn *c);
static void task_park_body(chttpsvr_conn *conn, struct chttpserver *srv,
                           chttp1_stream *stream, chttpsvr_req *req);

// Returns the next capacity of a realloc'ed pointer array (initial when cap is
// 0, else cap * 2), for the registries of this file that grow with plain
// realloc. It returns 0, which the callers treat like a failed realloc, when
// the doubling or the later multiplication by elem_size would overflow size_t.
// It touches no global state, so a test can drive it with a fake cap.
static size_t doubling_growth_cap(size_t cap, size_t elem_size,
                                  size_t initial) {
  if (cap == 0) return initial;
  if (cap > SIZE_MAX / 2 || cap * 2 > SIZE_MAX / elem_size) return 0;
  return cap * 2;
}

#ifdef RUNNING_UNIT_TESTS
// Returns doubling_growth_cap(cap, elem_size, initial). It exists so the tests
// can check the overflow-safe growth rule directly.
size_t chttpsvr__doubling_growth_cap_for_tests(size_t cap, size_t elem_size,
                                               size_t initial) {
  return doubling_growth_cap(cap, elem_size, initial);
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: The next servers_register() that has to add a server fails, as if
// the growth of the array failed. The tests use it to reach the failed-start
// cleanup of chttpsvr_start. The setter only stores the flag.
static _Atomic bool g_servers_register_fail_for_tests = false;
void chttpsvr__fail_next_servers_register_for_tests(void) {
  atomic_store(&g_servers_register_fail_for_tests, true);
}
#endif

// Adds srv to the server registry (servers_bundler.servers) unless it's already
// there. chttpsvr_start calls it so the idle sweep walks the server's
// connections and an engine force-stop quiesces the server; a server outside
// the registry gets neither. Starts and restarts both call it, so a server
// already in the array is skipped; a duplicate would stay behind as a dangling
// pointer after the destroy. Returns false when the array can't grow, and the
// start then fails. It takes servers_bundler.mutex, and can be the first thing
// in the process to touch that mutex, so it runs the engine globals init first.
static bool servers_register(struct chttpserver *srv) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      ccol__mutex_unlock(servers_bundler.mutex);
      return true;
    }
  }
  bool room = servers_bundler.count < servers_bundler.capacity;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_exchange(&g_servers_register_fail_for_tests, false)) {
    ccol__mutex_unlock(servers_bundler.mutex);
    return false;
  }
#endif
  if (!room) {
    size_t new_cap = doubling_growth_cap(servers_bundler.capacity,
                                         sizeof(struct chttpserver *), 8);
    struct chttpserver **nn = new_cap > 0
                                  ? (struct chttpserver **)realloc(
                                        servers_bundler.servers,
                                        new_cap * sizeof(struct chttpserver *))
                                  : NULL;
    if (!nn) {
      ccol__mutex_unlock(servers_bundler.mutex);
      return false;
    }
    servers_bundler.servers = nn;
    servers_bundler.capacity = new_cap;
  }
  servers_bundler.servers[servers_bundler.count++] = srv;
  ccol__mutex_unlock(servers_bundler.mutex);
  return true;
}

// Removes srv from the server registry by a swap with the last entry, and does
// nothing when srv isn't there. It's split out of servers_unregister so the
// fork child fixup, which already holds the inherited servers_bundler.mutex,
// can call it. The caller holds that mutex.
static void servers_unregister_locked(struct chttpserver *srv) {
  for (size_t i = 0; i < servers_bundler.count; i++) {
    if (servers_bundler.servers[i] == srv) {
      servers_bundler.servers[i] =
          servers_bundler.servers[servers_bundler.count - 1];
      servers_bundler.count--;
      break;
    }
  }
}

// Removes srv from the server registry under servers_bundler.mutex, for the
// destroy and for a failed start. It runs the engine globals init first, like
// every function that touches that mutex.
static void servers_unregister(struct chttpserver *srv) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  servers_unregister_locked(srv);
  ccol__mutex_unlock(servers_bundler.mutex);
}

// Takes raw out of the server registry and drops its engine reference, for a
// chttpsvr_start() that took both and then failed. Without it a failed start
// keeps the shared reactor alive and chttpsvr_engine_wait() blocks for ever.
// acquired_here must be the need_acquire of that start: A failed restart of a
// server that an earlier start registered keeps its registration and its
// reference, or the reactor could go away under its live connections. It reads
// and clears raw->contributed_to_engine and raw->engine_gen under raw->mutex
// and releases the engine after that lock. It may run while the start holds its
// resolve pin, because a concurrent quiesce_server_once waits for that pin
// before it does the same cleanup.
static void chttpsvr_undo_start_registration(struct chttpserver *raw,
                                             bool acquired_here) {
  if (!acquired_here) return;
  servers_unregister(raw);
  bool release = false;
  uintptr_t gen = 0;
  ccol__mutex_lock(raw->mutex);
  if (raw->contributed_to_engine) {
    raw->contributed_to_engine = false;
    release = true;
    gen = raw->engine_gen;
    raw->engine_gen = 0;
  }
  ccol__mutex_unlock(raw->mutex);
  if (release) engine_release(gen);
}

// The interval of the idle sweep, which bounds how late a timeout or a listener
// resume can come. The batch size is the stack buffer in which a teardown pass
// and mem_admit claim connections under a lock, so they can close or dispatch
// them outside it.
#define CHTTPSVR_IDLE_SWEEP_INTERVAL_MS 1000
#define CHTTPSVR_IDLE_CLOSE_BATCH 64

// Resumes the listener registration of srv once current_connections is below
// max_connections again. listener_on_readable pauses the listener at the cap so
// the reactor doesn't spin on a backlog it can't accept, and this lifts the
// pause. Only the sweep calls it: A resume straight from conn_free would touch
// srv after the decrement that drain_and_close_all_connections waits for, and
// the destroy could free srv under it. It reads srv->listen_reg under
// srv->mutex. A paused listener therefore resumes within one tick.
static void listener_resume_if_capacity_freed(struct chttpserver *srv) {
  size_t cap = atomic_load(&srv->max_connections);
  if (!cap || atomic_load(&srv->current_connections) >= cap) return;
  ccol__mutex_lock(srv->mutex);
  ceventloop_reg lreg = srv->listen_reg;
  ccol__mutex_unlock(srv->mutex);
  if (!lreg) return;
  ceventloop_resume(srv_engine_bundler.reactor, lreg);
}

// Resumes the listener registration of srv when it was paused because accept4()
// or an allocation after it ran out of resources. The pause stops the reactor
// from spinning on that failure, and the sweep calls this once per tick to
// retry. It clears listener_paused_for_resource_pressure atomically and reads
// srv->listen_reg under srv->mutex. It's kept apart from the max_connections
// resume because the two reasons for a pause are independent. A new pause can
// land between the exchange and the resume; the dispatch that made it sets the
// flag again, so the next tick repeats the cycle.
static void listener_resume_if_resource_pressure_cleared(
    struct chttpserver *srv) {
  if (!atomic_exchange(&srv->listener_paused_for_resource_pressure, false))
    return;
  ccol__mutex_lock(srv->mutex);
  ceventloop_reg lreg = srv->listen_reg;
  ccol__mutex_unlock(srv->mutex);
  if (!lreg) return;
  ceventloop_resume(srv_engine_bundler.reactor, lreg);
}

// Returns the nanoseconds from earlier to later, negative when later comes
// first, for the timeout checks.
static inline __attribute__((always_inline)) long long ns_between(
    struct timespec later, struct timespec earlier) {
  return (long long)(later.tv_sec - earlier.tv_sec) * 1000000000LL +
         (long long)(later.tv_nsec - earlier.tv_nsec);
}

// Returns ns as whole milliseconds rounded up, for remaining times that must
// never be cut short.
static inline __attribute__((always_inline)) long long ms_ceil_of_ns(
    long long ns) {
  return ns > 0 ? (ns + 999999LL) / 1000000LL : ns / 1000000LL;
}

// Tells whether at least idle_ms (non-zero) whole milliseconds lie between
// last_activity and the sweep's time stamp. The sweep uses it for the idle
// timeout, and conn_header_phase_expired for the header-phase limit. The sweep
// takes its stamp before it locks idle_mutex and idle_list_add on another
// thread can refresh last_activity after that, so the elapsed time can be
// negative; it's signed and a negative value never times out, where an unsigned
// compare would wrap and close a connection that just went idle. long long
// keeps it from overflowing on ILP32.
static bool conn_idle_timed_out(struct timespec now,
                                struct timespec last_activity,
                                unsigned idle_ms) {
  long long elapsed_ms = ns_between(now, last_activity) / 1000000LL;
  if (elapsed_ms < 0) return false;
  return (unsigned long long)elapsed_ms >= (unsigned long long)idle_ms;
}

// Tells whether conn has spent more than max_ms in the reactor phase of its
// request (TLS handshake plus header read), counted from header_phase_start.
// The sweep needs a total bound here because a peer that sends one header byte
// every few seconds keeps resetting last_activity and would otherwise hold its
// connection for ever. It's false while the phase clock isn't armed.
static bool conn_header_phase_expired(struct timespec now,
                                      const chttpsvr_conn *conn,
                                      unsigned max_ms) {
  if (!conn->header_phase_active) return false;
  return conn_idle_timed_out(now, conn->header_phase_start, max_ms);
}

// Starts the header phase clock of the current request (header_phase_start and
// header_phase_active of conn) unless it's already running. The reactor calls
// it at each handshake step and header read, and only the first call counts, so
// no later step moves the deadline that conn_header_phase_expired checks.
static void conn_header_phase_arm(chttpsvr_conn *conn,
                                  const struct timespec *now) {
  if (conn->header_phase_active) return;
  conn->header_phase_start = *now;
  conn->header_phase_active = true;
}

#ifdef RUNNING_UNIT_TESTS
// Returns conn_idle_timed_out(now, last_activity, idle_ms). The tests use it to
// check a last_activity later than the sweep's stamp without racing a live
// sweep.
bool chttpsvr__conn_idle_timed_out_for_tests(struct timespec now,
                                             struct timespec last_activity,
                                             unsigned idle_ms) {
  return conn_idle_timed_out(now, last_activity, idle_ms);
}
#endif

// Runs one sweep tick for srv: It resumes a paused listener, closes the idle
// connections past the idle or header-phase limit, and ends the parked
// connections that parked_expired judges expired (a parked body gets a 408,
// anything else is closed). It's the only place that enforces those limits,
// because these connections have no thread. idle_sweep_fn calls it with srv
// pinned, and it returns the number of connections it visited. Unlinking a
// connection from the idle or parked list under idle_mutex is the claim, so no
// dispatch owns it at the same time; the closes and rejects run after the lock,
// because conn_close reaches ceventloop_remove. One tick takes every expired
// connection with no cap. Parked connections whose verdict needs a kernel query
// are claimed under the lock, judged outside it and put back at the parked tail
// if they survive. It also runs sweep_parked_waits and reaps retired pools.
static size_t sweep_server(struct chttpserver *srv, struct timespec now) {
  listener_resume_if_capacity_freed(srv);
  listener_resume_if_resource_pressure_cleared(srv);

  unsigned idle_ms = atomic_load(&srv->idle_timeout_ms);
  unsigned hdr_ms = atomic_load(&srv->max_header_read_duration_ms);
  struct timespec now_slow;
  slow_clock(&now_slow);

  chttpsvr_conn *idle_close = NULL;
  chttpsvr_conn *parked_close = NULL;
  chttpsvr_conn *parked_reject = NULL;
  chttpsvr_conn *parked_query = NULL;
  size_t expired_n = 0, lingers_n = 0;
  size_t visited = 0;
  ccol__mutex_lock(srv->idle_mutex);
#ifdef RUNNING_UNIT_TESTS
  g_sweep_holds_idle_mutex_for_tests = true;
#endif
  chttpsvr_conn *c = (idle_ms || hdr_ms) ? srv->idle_head : NULL;
  while (c) {
    chttpsvr_conn *next = c->idle_next;
    visited++;
    if ((idle_ms && conn_idle_timed_out(now, c->last_activity, idle_ms)) ||
        (hdr_ms && conn_header_phase_expired(now, c, hdr_ms))) {
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
    chttpsvr_conn *next = c->parked_next;
    visited++;
    if (parked_needs_kernel_query(srv, c, now, now_slow)) {
      parked_unlink_locked(srv, c);
      c->parked_next = parked_query;
      parked_query = c;
    } else if (parked_expired(srv, c, now, now_slow)) {
      parked_unlink_locked(srv, c);
      if (c->park == CONN_PARK_BODY) {
        c->parked_next = parked_reject;
        parked_reject = c;
      } else {
        c->parked_next = parked_close;
        parked_close = c;
      }
      if (c->park == CONN_PARK_LINGER) lingers_n++;
      expired_n++;
    }
    c = next;
  }
#ifdef RUNNING_UNIT_TESTS
  g_sweep_holds_idle_mutex_for_tests = false;
#endif
  ccol__mutex_unlock(srv->idle_mutex);
  if (parked_query) {
    chttpsvr_conn *keep = NULL;
    while (parked_query) {
      chttpsvr_conn *q = parked_query;
      parked_query = q->parked_next;
      if (parked_expired(srv, q, now, now_slow)) {
        if (q->park == CONN_PARK_BODY) {
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
    ccol__mutex_lock(srv->idle_mutex);
    while (keep) {
      chttpsvr_conn *k = keep;
      keep = k->parked_next;
      k->parked_prev = srv->parked_tail;
      k->parked_next = NULL;
      if (srv->parked_tail) srv->parked_tail->parked_next = k;
      srv->parked_tail = k;
      if (!srv->parked_head) srv->parked_head = k;
      k->in_parked_list = true;
    }
    ccol__mutex_unlock(srv->idle_mutex);
  }
  if (expired_n > lingers_n) {
    SRV_ENGINE_LOG(clog_info, "timeout closing %zu connection(s)",
                   expired_n - lingers_n);
  }
  while (idle_close) {
    chttpsvr_conn *next = idle_close->idle_next;
    idle_close->idle_next = NULL;
    idle_close->park = CONN_PARK_NONE;
    conn_close(idle_close);
    idle_close = next;
  }
  while (parked_close) {
    chttpsvr_conn *next = parked_close->parked_next;
    parked_close->parked_next = NULL;
    parked_close->park = CONN_PARK_NONE;
    conn_close(parked_close);
    parked_close = next;
  }
  while (parked_reject) {
    chttpsvr_conn *r = parked_reject;
    parked_reject = r->parked_next;
    r->parked_next = NULL;
    r->park = CONN_PARK_NONE;
    r->req_rejected = true;
    r->reject_status = CHTTP_STATUS_REQUEST_TIMEOUT;
    conn_dispatch_reject(r);
  }

  sweep_parked_waits(srv, now_slow);
  if (expired_n) listener_resume_if_capacity_freed(srv);
  retired_pools_reap(srv, false);
  return visited;
}

#ifdef RUNNING_UNIT_TESTS
// Test counters of how the sweep thread stopped: Woken by the stop, or only
// after its tick wait ran out. g_idle_sweep_stop_ns_for_tests is the
// CLOCK_MONOTONIC instant of the last stop, guarded by
// idle_sweep_bundler.wake_mu. The getter reads both counters.
static _Atomic size_t g_idle_sweep_exits_woken_for_tests = 0;
static uint64_t g_idle_sweep_stop_ns_for_tests = 0;
static _Atomic size_t g_idle_sweep_exits_timed_out_for_tests = 0;
void chttpsvr__idle_sweep_exits_for_tests(size_t *woken, size_t *timed_out) {
  *woken = atomic_load(&g_idle_sweep_exits_woken_for_tests);
  *timed_out = atomic_load(&g_idle_sweep_exits_timed_out_for_tests);
}
#endif

// The body of the shared sweep thread: It calls sweep_server for every
// registered server once per tick until stop_flag is set. The idle,
// header-phase and parked limits and the listener resumes all depend on it. It
// waits on wake_cv until the tick deadline or a stop; the stop sets stop_flag
// under wake_mu, so it is never missed. Then it copies servers_bundler.servers
// and pins every server with servers_bundler_pins under servers_bundler.mutex,
// and sweeps after that mutex is released, so a slow tick doesn't hold up
// starts, destroys and engine stops of other servers. Each server is unpinned
// under its own mutex with a broadcast of resolve_cv. A failed snapshot
// allocation skips the tick.
static void *idle_sweep_fn(void *arg) {
  (void)arg;
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
#ifdef RUNNING_UNIT_TESTS
  bool stopped_after_timeout = false;
#endif
  while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
    struct timespec tick_deadline;
    clock_gettime(CLOCK_MONOTONIC, &tick_deadline);
    timespec_add_ms(&tick_deadline, CHTTPSVR_IDLE_SWEEP_INTERVAL_MS);
#ifdef RUNNING_UNIT_TESTS
    bool wait_timed_out = false;
#endif
    ccol__mutex_lock(idle_sweep_bundler.wake_mu);
    while (!atomic_load(&idle_sweep_bundler.stop_flag)) {
      if (ccol__cond_var_timedwait(idle_sweep_bundler.wake_cv,
                                   idle_sweep_bundler.wake_mu,
                                   tick_deadline) == ETIMEDOUT) {
#ifdef RUNNING_UNIT_TESTS
        uint64_t deadline_ns = (uint64_t)tick_deadline.tv_sec * 1000000000ULL +
                               (uint64_t)tick_deadline.tv_nsec;
        wait_timed_out =
            !atomic_load(&idle_sweep_bundler.stop_flag) ||
            g_idle_sweep_stop_ns_for_tests + 100000000ULL < deadline_ns;
#endif
        break;
      }
    }
    ccol__mutex_unlock(idle_sweep_bundler.wake_mu);
    if (atomic_load(&idle_sweep_bundler.stop_flag)) {
#ifdef RUNNING_UNIT_TESTS
      stopped_after_timeout = wait_timed_out;
#endif
      break;
    }

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    ccol__mutex_lock(servers_bundler.mutex);
    size_t n = servers_bundler.count;
    struct chttpserver **snapshot =
        n > 0 ? (struct chttpserver **)malloc(n * sizeof(struct chttpserver *))
              : NULL;
    if (n > 0 && !snapshot) {
      n = 0;
    } else {
      for (size_t i = 0; i < n; i++) {
        snapshot[i] = servers_bundler.servers[i];
        atomic_fetch_add(&snapshot[i]->servers_bundler_pins, 1);
      }
    }
    ccol__mutex_unlock(servers_bundler.mutex);

    for (size_t i = 0; i < n; i++) {
      struct chttpserver *srv = snapshot[i];
      sweep_server(srv, now);

      ccol__mutex_lock(srv->mutex);
      atomic_fetch_sub(&srv->servers_bundler_pins, 1);
      ccol__cond_var_broadcast(srv->resolve_cv);
      ccol__mutex_unlock(srv->mutex);
    }
    free(snapshot);
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(stopped_after_timeout
                       ? &g_idle_sweep_exits_timed_out_for_tests
                       : &g_idle_sweep_exits_woken_for_tests,
                   1);
#endif
  return NULL;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook that makes the next start of the sweep thread fail as if
// ccol__thread_create() had failed, so the tests can reach that failure of
// chttpsvr_start.
static _Atomic bool g_force_idle_sweep_thread_create_fail_for_tests = false;
void chttpsvr__force_idle_sweep_thread_create_fail_for_tests(bool force) {
  atomic_store(&g_force_idle_sweep_thread_create_fail_for_tests, force);
}
#endif
// Clears idle_sweep_bundler.running in a fork() child that inherited it,
// because the child has no sweep thread and must start its own instead of
// joining the parent's. The caller holds servers_bundler.mutex.
static void idle_sweep_forget_inherited_locked(void) {
  if (idle_sweep_bundler.running && idle_sweep_bundler.pid != getpid())
    idle_sweep_bundler.running = false;
}

// Starts the shared sweep thread unless it runs, for chttpsvr_start. Returns
// false only when the thread could not be created; the start then fails,
// because the idle limits and the max_connections recovery depend on this
// thread. It works under servers_bundler.mutex and sets
// idle_sweep_bundler.running, pid and the cleared stop_flag.
static bool idle_sweep_start_if_needed(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  idle_sweep_forget_inherited_locked();
  if (!idle_sweep_bundler.running) {
    atomic_store(&idle_sweep_bundler.stop_flag, false);
#ifdef RUNNING_UNIT_TESTS
    if (atomic_load(&g_force_idle_sweep_thread_create_fail_for_tests)) {
    } else
#endif
        if (ccol__thread_create(idle_sweep_bundler.thread, idle_sweep_fn,
                                NULL) == 0) {
      idle_sweep_bundler.running = true;
      idle_sweep_bundler.pid = getpid();
    }
  }
  bool ok = idle_sweep_bundler.running;
  ccol__mutex_unlock(servers_bundler.mutex);
  return ok;
}

// Stops and joins the sweep thread. The engine reaper calls it, since the
// thread serves every server of the engine and must not outlive it. Under
// servers_bundler.mutex it clears idle_sweep_bundler.running and, under
// wake_mu, sets stop_flag and signals wake_cv; the join runs after both locks
// are released.
static void idle_sweep_stop_if_running(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  bool was_running = false;
  ccol__thread_id t = {0};
  ccol__mutex_lock(servers_bundler.mutex);
  idle_sweep_forget_inherited_locked();
  if (idle_sweep_bundler.running) {
    ccol__mutex_lock(idle_sweep_bundler.wake_mu);
#ifdef RUNNING_UNIT_TESTS
    {
      struct timespec stop_ts;
      clock_gettime(CLOCK_MONOTONIC, &stop_ts);
      g_idle_sweep_stop_ns_for_tests =
          (uint64_t)stop_ts.tv_sec * 1000000000ULL + (uint64_t)stop_ts.tv_nsec;
    }
#endif
    atomic_store(&idle_sweep_bundler.stop_flag, true);
    ccol__cond_var_signal(idle_sweep_bundler.wake_cv);
    ccol__mutex_unlock(idle_sweep_bundler.wake_mu);
    t = idle_sweep_bundler.thread;
    was_running = true;
    idle_sweep_bundler.running = false;
  }
  ccol__mutex_unlock(servers_bundler.mutex);
  if (was_running) ccol__thread_join(t);
}

// Unlinks c from the parked list of srv and clears in_parked_list. Unlinking is
// what claims a parked connection, for the sweep, a dispatch or a close. The
// caller holds srv->idle_mutex.
static void parked_unlink_locked(struct chttpserver *srv, chttpsvr_conn *c) {
  if (c->parked_prev) c->parked_prev->parked_next = c->parked_next;
  if (c->parked_next) c->parked_next->parked_prev = c->parked_prev;
  if (srv->parked_head == c) srv->parked_head = c->parked_next;
  if (srv->parked_tail == c) srv->parked_tail = c->parked_prev;
  c->parked_prev = c->parked_next = NULL;
  c->in_parked_list = false;
}

#ifdef RUNNING_UNIT_TESTS
// Test counter of the ceventloop_modify calls conn_reg_modify makes, so a test
// can see that a keep-alive request makes no direction change.
static _Atomic size_t g_conn_reg_modify_calls_for_tests = 0;
size_t chttpsvr__conn_reg_modify_calls_for_tests(void) {
  return atomic_load(&g_conn_reg_modify_calls_for_tests);
}
#endif
// Points conn->reg at dir and returns the result of ceventloop_modify. It skips
// the call when conn->reg_dir already matches, which spares an ordinary
// keep-alive request a system call. Every direction change goes through here
// and every ceventloop_add of conn->reg sets reg_dir, so reg_dir always matches
// the registration; it updates reg_dir on success.
static inline ccol_retval conn_reg_modify(chttpsvr_conn *conn,
                                          cselect_dir dir) {
  if (conn->reg_dir == (uint8_t)dir) return ccol_success;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add_explicit(&g_conn_reg_modify_calls_for_tests, 1,
                            memory_order_relaxed);
#endif
  ccol_retval rv =
      ceventloop_modify(srv_engine_bundler.reactor, conn->reg, dir);
  if (rv == ccol_success) conn->reg_dir = (uint8_t)dir;
  return rv;
}

// Puts a parked connection at the tail of the parked list, so the sweep
// enforces its limits and a dispatch can claim it on readiness. It does nothing
// when conn is already there, and works under srv->idle_mutex.
static void parked_list_add(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->idle_mutex);
  if (!conn->in_parked_list) {
    conn->parked_prev = srv->parked_tail;
    conn->parked_next = NULL;
    if (srv->parked_tail) srv->parked_tail->parked_next = conn;
    srv->parked_tail = conn;
    if (!srv->parked_head) srv->parked_head = conn;
    conn->in_parked_list = true;
  }
  ccol__mutex_unlock(srv->idle_mutex);
}

// Puts conn at the tail of the idle list and refreshes last_activity, so the
// sweep can time it out and a dispatch or closer can claim it. It does nothing
// to the list when conn is already there, and works under srv->idle_mutex.
static void idle_list_add(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->idle_mutex);
  if (!conn->in_idle_list) {
    conn->idle_prev = srv->idle_tail;
    conn->idle_next = NULL;
    if (srv->idle_tail) srv->idle_tail->idle_next = conn;
    srv->idle_tail = conn;
    if (!srv->idle_head) srv->idle_head = conn;
    conn->in_idle_list = true;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  ccol__mutex_unlock(srv->idle_mutex);
}

// Takes conn out of the idle list, or out of the parked list, under
// srv->idle_mutex, when its owner keeps it for itself or closes it.
static void idle_list_remove(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
  } else if (conn->in_parked_list) {
    parked_unlink_locked(srv, conn);
  }
  ccol__mutex_unlock(srv->idle_mutex);
}

// Adds conn to the diverted registry of srv under diverted_mutex. A bounded
// teardown walks this registry to shutdown(2) the fd of a connection whose
// worker stays stuck in blocking I/O. conn_start_diverted calls it for every
// request handed to a worker, and conn_dispatch_reject for every rejection it
// writes. It does nothing when conn is already there.
static void diverted_list_add(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->diverted_mutex);
  if (!conn->in_diverted_list) {
    conn->diverted_prev = srv->diverted_tail;
    conn->diverted_next = NULL;
    if (srv->diverted_tail) srv->diverted_tail->diverted_next = conn;
    srv->diverted_tail = conn;
    if (!srv->diverted_head) srv->diverted_head = conn;
    conn->in_diverted_list = true;
  }
  ccol__mutex_unlock(srv->diverted_mutex);
}

// Removes conn from the diverted registry under diverted_mutex once no worker
// owns it, so a teardown leaves it alone. conn_free calls it, and so does the
// keep-alive tail that gives conn back to the reactor.
static void diverted_list_remove(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->diverted_mutex);
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
  ccol__mutex_unlock(srv->diverted_mutex);
}

// Calls shutdown(SHUT_RDWR) on the fd of every diverted connection, so a worker
// blocked in a read or write gets an I/O error and finishes the request
// normally. wait_in_flight_bounded calls it after the graceful wait runs out,
// with srv->mutex released; it walks the registry under diverted_mutex. It's
// shutdown() and not close(), because the worker owns the fd and a close here
// could let the number be reused under it. The result of shutdown() is ignored.
static void force_unblock_diverted_connections(struct chttpserver *srv) {
  ccol__mutex_lock(srv->diverted_mutex);
  for (chttpsvr_conn *c = srv->diverted_head; c; c = c->diverted_next)
    shutdown(c->fd, SHUT_RDWR);
  ccol__mutex_unlock(srv->diverted_mutex);
}

// Claims conn out of the idle or parked list under srv->idle_mutex. A dispatch,
// the sweep and a teardown all race for unowned connections, and without this
// claim a closer could free a connection while a reactor thread reads from it
// (on TLS a use-after-free of the SSL object). Returns true when the caller now
// owns conn alone, false when someone else has it and the caller must not touch
// conn. A dispatch can fire before the add that follows its ceventloop_add; its
// claim then fails, and level-triggered readiness reports the event again.
static bool idle_list_try_claim(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  bool claimed = false;
  ccol__mutex_lock(srv->idle_mutex);
  if (conn->in_idle_list) {
    if (conn->idle_prev) conn->idle_prev->idle_next = conn->idle_next;
    if (conn->idle_next) conn->idle_next->idle_prev = conn->idle_prev;
    if (srv->idle_head == conn) srv->idle_head = conn->idle_next;
    if (srv->idle_tail == conn) srv->idle_tail = conn->idle_prev;
    conn->idle_prev = conn->idle_next = NULL;
    conn->in_idle_list = false;
    claimed = true;
  } else if (conn->in_parked_list) {
    parked_unlink_locked(srv, conn);
    claimed = true;
  }
  ccol__mutex_unlock(srv->idle_mutex);
  return claimed;
}

// Closes every idle connection of srv, and the parked ones when include_parked
// is set. The destroy calls it after the listener is gone; without it a
// keep-alive connection the client never closed leaks with the server. Each
// connection is unlinked under idle_mutex in batches, the same claim as
// idle_list_try_claim, and closed after the lock is released.
static void close_idle_connections(struct chttpserver *srv,
                                   bool include_parked) {
  for (;;) {
    chttpsvr_conn *to_close[CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t to_close_n = 0;
    ccol__mutex_lock(srv->idle_mutex);
    chttpsvr_conn *c = srv->idle_head;
    while (c && to_close_n < CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn *next = c->idle_next;
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
           to_close_n < CHTTPSVR_IDLE_CLOSE_BATCH) {
      c = srv->parked_head;
      parked_unlink_locked(srv, c);
      c->park = CONN_PARK_NONE;
      to_close[to_close_n++] = c;
    }
    ccol__mutex_unlock(srv->idle_mutex);
    if (to_close_n == 0) break;
    for (size_t i = 0; i < to_close_n; i++) conn_close(to_close[i]);
  }
}

// Closes the idle and the parked connections of srv, for the teardown.
static void close_all_idle_connections(struct chttpserver *srv) {
  close_idle_connections(srv, true);
}

// Returns the value of a hex digit, or -1, for percent_decode.
static int hex_nibble(char c) {
  if (c >= '0' && c <= '9') return c - '0';
  if (c >= 'a' && c <= 'f') return c - 'a' + 10;
  if (c >= 'A' && c <= 'F') return c - 'A' + 10;
  return -1;
}

// Decodes RFC 3986 %XX escapes from src into dst, for request paths and query
// strings. '+' becomes a space when plus_as_space is set (query strings) and
// stays as it is otherwise (paths). It works in place with dst == src. Returns
// the decoded length, or -1 for a malformed escape or an escape that decodes to
// NUL: Every caller treats the result as a C string, so "foo%00bar" would
// otherwise compare equal to the route segment "foo".
static ssize_t percent_decode(char *dst, const char *src, bool plus_as_space) {
  size_t si = 0, di = 0;
  while (src[si]) {
    char c = src[si];
    if (c == '%') {
      char c1 = src[si + 1];
      if (!c1) return -1;
      int hi = hex_nibble(c1);
      char c2 = src[si + 2];
      int lo = hex_nibble(c2);
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

// Percent-decodes a path segment, where '+' is literal, for the route search.
static ssize_t decode_path_unsafe(char *dst, const char *src) {
  return percent_decode(dst, src, false);
}

// Percent-decodes a query string part, where '+' is a space, for the query
// parameters.
static ssize_t decode_url_unsafe(char *dst, const char *src) {
  return percent_decode(dst, src, true);
}

// Turns an absolute-form request target ("http://host/items") into the
// origin-form slice inside it. RFC 7230 section 5.3.2 makes a server accept
// that form, which clients send through a proxy, and without this it would
// route as four segments and get a 404. Only http and https are recognised,
// case-insensitively; the authority is dropped on purpose, since we route on
// the path alone and section 5.4 says to ignore Host here. *out and *out_len
// describe a slice of target. Returns true when a prefix was stripped, because
// an empty path then means "/".
static bool strip_absolute_form(const char *target, size_t target_len,
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

  size_t i = scheme_len;
  while (i < target_len && target[i] != '/' && target[i] != '?' &&
         target[i] != '#')
    i++;

  *out = target + i;
  *out_len = target_len - i;
  return true;
}

// The method parse_method returns for a method it doesn't know. It's outside
// the public enum, so no route can match it.
#define CHTTP_METHOD_UNKNOWN ((chttp_method)(CHTTP_ANY + 1))

// The largest body "OPTIONS *" may carry with the connection kept alive. A
// bigger body is discarded and the connection closes after the 200, as in Go's
// net/http.
#define CHTTPSVR_OPTIONS_STAR_MAX_BODY ((size_t)4096)

// Does nothing. It's the handler of options_star_route, the route that "OPTIONS
// *" matches; that route is never registered and only lets the worker path
// treat the request as a buffered route. task_worker answers the request
// itself, so this never runs.
static void options_star_unused_handler(chttpsvr_req *req, chttpsvr_resp *resp,
                                        void *ctx) {
  (void)req;
  (void)resp;
  (void)ctx;
}
static chttpsvr_route options_star_route = {.method = CHTTP_OPTIONS,
                                            .fn = options_star_unused_handler};

static void chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp);
static void task_worker(void *arg);
#ifdef RUNNING_UNIT_TESTS
// Test counter of the requests conn_start_diverted hands to the worker pool.
atomic_size_t chttpsvr__worker_submits_for_tests;
#endif
static void task_resume(void *arg);
static void reject_task(void *arg);
static void destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs *mp);
static void conn_on_readable(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg);
static void conn_on_writable(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg);
static void conn_on_error(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                          void *arg);
static void listener_on_readable(ceventloop loop, ceventloop_reg reg,
                                 cselectable *sel, void *arg);

// Splits a route pattern such as "/users/{id}" into segments and collects the
// names of its {name} segments, which route registration stores for the route
// search. The pattern must start with '/', must not end with one and must not
// hold "//". A name is letters, digits and '_', and the same name twice is
// refused, because chttpsvr_req_param would only ever return the first. On
// success the four out-parameters receive arrays allocated from mp (NULL and 0
// for "/"). Returns ccol_invalid_args or ccol_not_enough_memory and frees
// everything on failure.
static ccol_retval compile_pattern(const char *pattern, char ***segs_out,
                                   int *seg_count_out, char ***param_names_out,
                                   int *param_count_out,
                                   ccol_memmgmt_procs *mp) {
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
      (char **)ccol__mem_calloc(mp, (size_t)seg_count, sizeof(char *));
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
    segs[si] = (char *)ccol__mem_alloc(mp, len + 1);
    if (!segs[si]) {
      for (int j = 0; j < si; j++) ccol__mem_free(mp, segs[j]);
      ccol__mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    memcpy(segs[si], s, len);
    segs[si][len] = '\0';
    if (segs[si][0] == '{') {
      if (len < 3 || segs[si][len - 1] != '}') {
        for (int j = 0; j <= si; j++) ccol__mem_free(mp, segs[j]);
        ccol__mem_free(mp, segs);
        return ccol_invalid_args;
      }
      for (size_t ci = 1; ci < len - 1; ci++) {
        char c = segs[si][ci];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_')) {
          for (int j = 0; j <= si; j++) ccol__mem_free(mp, segs[j]);
          ccol__mem_free(mp, segs);
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
        (char **)ccol__mem_calloc(mp, (size_t)param_count, sizeof(char *));
    if (!param_names) {
      for (int j = 0; j < seg_count; j++) ccol__mem_free(mp, segs[j]);
      ccol__mem_free(mp, segs);
      return ccol_not_enough_memory;
    }
    int pi = 0;
    for (int i = 0; i < seg_count; i++) {
      if (segs[i][0] == '{') {
        size_t len = strlen(segs[i]);
        param_names[pi] = (char *)ccol__mem_alloc(mp, len - 1);
        if (!param_names[pi]) {
          for (int j = 0; j < pi; j++) ccol__mem_free(mp, param_names[j]);
          ccol__mem_free(mp, param_names);
          for (int j = 0; j < seg_count; j++) ccol__mem_free(mp, segs[j]);
          ccol__mem_free(mp, segs);
          return ccol_not_enough_memory;
        }
        memcpy(param_names[pi], segs[i] + 1, len - 2);
        param_names[pi][len - 2] = '\0';
        for (int j = 0; j < pi; j++) {
          if (strcmp(param_names[j], param_names[pi]) == 0) {
            for (int k = 0; k <= pi; k++) ccol__mem_free(mp, param_names[k]);
            ccol__mem_free(mp, param_names);
            for (int k = 0; k < seg_count; k++) ccol__mem_free(mp, segs[k]);
            ccol__mem_free(mp, segs);
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

// Frees the segments and parameter names of a compiled route, when the route or
// its router goes away.
static void free_route_data(chttpsvr_route *r, ccol_memmgmt_procs *mp) {
  for (int i = 0; i < r->seg_count; i++) ccol__mem_free(mp, r->segs[i]);
  ccol__mem_free(mp, r->segs);
  for (int i = 0; i < r->param_count; i++)
    ccol__mem_free(mp, r->param_names[i]);
  ccol__mem_free(mp, r->param_names);
}

// The outcome of a route search: A match, a path that matches with another
// method (405), an allocation failure, or no match (404).
typedef enum {
  ROUTE_MATCH_OK,
  ROUTE_MATCH_METHOD,
  ROUTE_MATCH_OOM,
  ROUTE_MATCH_NONE
} route_match;

// The result of find_route. router is the router of the match, or for a 405 or
// 404 the sub-router that owns the path and runs its middleware (NULL for the
// root). On a match the caller frees param_values. allowed_methods has one bit
// per method that matches the path, HEAD included where it falls back to GET,
// and feeds the Allow header of a 405.
typedef struct {
  chttpsvr_router *router;
  chttpsvr_route *route;
  char **param_values;
  route_match result;
  uint8_t allowed_methods;
} match_result;

// One raw segment of a request path and its percent-decoded value, decoded on
// first use and kept, so a router decodes each segment once however many routes
// try it. raw points into the path and isn't NUL-terminated. ok is meaningful
// once attempted is set and tells whether decoded holds a value.
typedef struct {
  const char *raw;
  size_t raw_len;
  bool attempted;
  bool ok;
  char *decoded;
  size_t decoded_len;
} seg_cache_entry;

// The segment cache of one path. count is the number of entries filled; see
// seg_cache_build for why it stands in for the real segment count.
typedef struct {
  seg_cache_entry *entries;
  size_t count;
  ccol_memmgmt_procs *mp;
} seg_cache;

// Splits sub_path on '/' into raw segments, without decoding them, and fills
// cache with at most cap of them. The route search shares the cache between all
// routes of a router so each segment is decoded once. It strips the leading
// slash and keeps empty segments exactly like the route walk; a trailing '/'
// counts as one more empty segment, so "/items/{id}" doesn't match "items/99/".
// cap must be larger than the seg_count of every route of the router, so count
// == seg_count is an exact test, and the cache size depends on the routes,
// which the operator controls, and not on the path, which the client controls.
// Returns 0, or -1 when out of memory.
static int seg_cache_build(const char *sub_path, size_t cap, seg_cache *cache,
                           ccol_memmgmt_procs *mp) {
  cache->entries = NULL;
  cache->count = 0;
  cache->mp = mp;
  if (cap == 0) return 0;

  const char *p = sub_path;
  if (*p == '/') p++;
  if (*p == '\0') return 0;

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

  seg_cache_entry *entries =
      (seg_cache_entry *)ccol__mem_calloc(mp, count, sizeof(seg_cache_entry));
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

// Frees the decoded values and the entries of the cache at the end of a route
// search.
static void seg_cache_free(seg_cache *cache) {
  if (!cache->entries) return;
  for (size_t i = 0; i < cache->count; i++)
    ccol__mem_free(cache->mp, cache->entries[i].decoded);
  ccol__mem_free(cache->mp, cache->entries);
  cache->entries = NULL;
  cache->count = 0;
}

// Returns the decoded value of segment idx, decoding it on the first call and
// keeping it in the cache, so the routes and prefixes that ask share one
// decode. NULL means the segment can't match anything: It's empty or its
// encoding is malformed. The cache owns the result until seg_cache_free;
// *len_out gets its length. *oom_out is set on an allocation failure. That
// failure is remembered like the other outcomes and never retried, which is
// safe only because every caller aborts the whole route search on it.
static const char *seg_cache_get(seg_cache *cache, size_t idx, size_t *len_out,
                                 bool *oom_out) {
  *oom_out = false;
  seg_cache_entry *e = &cache->entries[idx];
  if (e->attempted) {
    if (!e->ok) return NULL;
    *len_out = e->decoded_len;
    return e->decoded;
  }
  e->attempted = true;
  if (e->raw_len == 0) return NULL;

  char *decoded = (char *)ccol__mem_alloc(cache->mp, e->raw_len + 1);
  if (!decoded) {
    *oom_out = true;
    return NULL;
  }
  memcpy(decoded, e->raw, e->raw_len);
  decoded[e->raw_len] = '\0';
  ssize_t dlen = decode_path_unsafe(decoded, decoded);
  if (dlen < 0) {
    ccol__mem_free(cache->mp, decoded);
    return NULL;
  }
  decoded[(size_t)dlen] = '\0';
  e->ok = true;
  e->decoded = decoded;
  e->decoded_len = (size_t)dlen;
  *len_out = e->decoded_len;
  return decoded;
}

// Matches route against the shared segment cache, for find_route; a route with
// a different segment count is rejected before anything is decoded. On a match
// *pv_out (when given) gets a new array of copies of the parameter values,
// because the cache's values must stay intact for the next route. A NULL pv_out
// only probes whether the path matches, to tell a 405 from a 404. Returns 1 for
// a match, 0 for none and -1 when out of memory, which ends the whole search.
static int match_route_cached(seg_cache *cache, chttpsvr_route *route,
                              char ***pv_out, ccol_memmgmt_procs *mp) {
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
    const char *decoded = seg_cache_get(cache, (size_t)i, &dlen, &oom);
    if (!decoded) {
      if (oom) {
        if (pv) {
          for (int j = 0; j < param_idx; j++) ccol__mem_free(mp, pv[j]);
          ccol__mem_free(mp, pv);
        }
        return -1;
      }
      goto no_match;
    }

    if (route->segs[i][0] == '{') {
      if (pv_out) {
        if (!pv) {
          pv = (char **)ccol__mem_calloc(mp, (size_t)route->param_count,
                                         sizeof(char *));
          if (!pv) return -1;
        }
        char *val = (char *)ccol__mem_alloc(mp, dlen + 1);
        if (!val) {
          for (int j = 0; j < param_idx; j++) ccol__mem_free(mp, pv[j]);
          ccol__mem_free(mp, pv);
          return -1;
        }
        memcpy(val, decoded, dlen + 1);
        pv[param_idx++] = val;
      }
    } else {
      if (strcmp(decoded, route->segs[i]) != 0) goto no_match;
    }
  }
  if (pv_out) *pv_out = pv;
  return 1;

no_match:
  if (pv) {
    for (int j = 0; j < param_idx; j++) ccol__mem_free(mp, pv[j]);
    ccol__mem_free(mp, pv);
  }
  return 0;
}

// Decides whether router->prefix owns the request path in cache, so find_route
// can pick the sub-router whose routes and middleware handle the request. The
// cache is built once from the full path and shared between the sub-routers.
// Ownership is judged on the decoded path, as chttpsvr_req_path() reports it,
// so "/admin%2Fsecret" belongs to the "/admin" mount. Returns 1 when the prefix
// ends between raw segments (*sub_path_out gets the rest of the path), 2 when
// it ends inside a segment whose decoded value holds a '/' (the router answers
// 404), 0 when it doesn't own the path, and -1 when out of memory.
static int prefix_matches(seg_cache *cache, chttpsvr_router *router,
                          const char **sub_path_out) {
  const char *pp = router->prefix + 1;

  if (*pp == '\0') {
    if (cache->count != 0) return 0;
    *sub_path_out = "/";
    return 1;
  }

  size_t prem = router->prefix_len - 1;
  size_t idx = 0;
  for (;;) {
    if (idx >= cache->count) return 0;

    size_t dlen = 0;
    bool oom = false;
    const char *decoded = seg_cache_get(cache, idx, &dlen, &oom);
    if (oom) return -1;
    if (!decoded) return 0;

    if (dlen < prem) {
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
    return decoded[prem] == '/' ? 2 : 0;
  }
}

static void free_param_values(char **pv, int count, ccol_memmgmt_procs *mp);

// Finds the route that answers method on path. http1_on_headers_complete calls
// it for every request, and its result decides which handler runs and which
// middleware chain runs with it. The caller holds srv->routes_lock. It returns
// the matched router, route and decoded path parameter values (the caller owns
// the values), or a 404 or 405 result with the owning router, or
// ROUTE_MATCH_OOM.
// Precedence comes from how specific a mount prefix is, never from the order of
// registration. srv->routers keeps the root at index 0 and the sub-routers in
// descending prefix segment count, so the loop visits the sub-routers first and
// the root last. If the root went first, a pattern like "/{a}/{b}" would answer
// paths inside a mount and its middleware (an authenticator, say) would be
// skipped.
// The most specific mount that owns the path owns the request: Only routers on
// that same prefix may answer, and when none of their routes matches, the 404
// or 405 comes back with that owner as the router so its middleware runs for
// the rejection too.
// A HEAD request also matches a GET route (RFC 9110 SS9.3.2), but only after
// the whole router has been scanned, since an explicit HEAD or ANY route must
// win. A path that matched with the wrong method sets a bit in allowed_methods
// for the Allow header of the 405. Both segment caches are capped by operator
// settings (max_prefix_seg_count, max_route_seg_count), never by the number of
// segments in the client's path.
static match_result find_route(struct chttpserver *srv, const char *path,
                               chttp_method method) {
  unsigned allowed_methods = 0;

  seg_cache path_cache = {0};
  if (srv->router_count > 1) {
    if (seg_cache_build(path, (size_t)srv->max_prefix_seg_count + 1,
                        &path_cache, srv->m_procs) != 0)
      return (match_result){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
  }

  chttpsvr_router *owner = NULL;
  for (size_t visit = 0; visit < srv->router_count; visit++) {
    size_t ri = (visit + 1 < srv->router_count) ? visit + 1 : 0;
    chttpsvr_router *router = srv->routers[ri];

    const char *sub_path = NULL;
    if (router->prefix_len == 0) {
      if (owner) break;
      sub_path = path;
    } else {
      if (owner && router->prefix_seg_count < owner->prefix_seg_count) break;
      int pm = prefix_matches(&path_cache, router, &sub_path);
      if (pm < 0) {
        seg_cache_free(&path_cache);
        return (match_result){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
      }
      if (pm == 0) continue;
      if (!owner) owner = router;
      if (pm == 2) continue;
    }

    if (router->route_count == 0) continue;

    seg_cache cache;
    if (seg_cache_build(sub_path, (size_t)router->max_route_seg_count + 1,
                        &cache, srv->m_procs) != 0) {
      seg_cache_free(&path_cache);
      return (match_result){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
    }

    match_result found = {NULL, NULL, NULL, ROUTE_MATCH_NONE, 0};
    bool have_result = false;
    chttpsvr_route *head_fallback_route = NULL;
    char **head_fallback_pv = NULL;
    for (size_t i = 0; i < router->route_count; i++) {
      chttpsvr_route *route = router->routes[i];

      char **pv = NULL;
      bool method_ok = (route->method == CHTTP_ANY || route->method == method);
      bool head_ok =
          !method_ok && method == CHTTP_HEAD && route->method == CHTTP_GET;
      int ms = match_route_cached(
          &cache, route, (method_ok || head_ok) ? &pv : NULL, srv->m_procs);
      if (ms == -1) {
        found = (match_result){NULL, NULL, NULL, ROUTE_MATCH_OOM, 0};
        have_result = true;
        break;
      }
      if (ms == 0) continue;

      if (!method_ok) {
        if (head_ok) {
          if (!head_fallback_route) {
            head_fallback_route = route;
            head_fallback_pv = pv;
          } else {
            free_param_values(pv, route->param_count, srv->m_procs);
          }
          continue;
        }
        allowed_methods |= 1u << route->method;
        if (route->method == CHTTP_GET) allowed_methods |= 1u << CHTTP_HEAD;
        continue;
      }
      found = (match_result){router, route, pv, ROUTE_MATCH_OK, 0};
      have_result = true;
      break;
    }
    if (!have_result && head_fallback_route) {
      found = (match_result){router, head_fallback_route, head_fallback_pv,
                             ROUTE_MATCH_OK, 0};
      have_result = true;
      head_fallback_pv = NULL;
    }
    free_param_values(
        head_fallback_pv,
        head_fallback_route ? head_fallback_route->param_count : 0,
        srv->m_procs);
    seg_cache_free(&cache);
    if (have_result) {
      seg_cache_free(&path_cache);
      return found;
    }
  }
  seg_cache_free(&path_cache);
  if (allowed_methods)
    return (match_result){owner, NULL, NULL, ROUTE_MATCH_METHOD,
                          (uint8_t)allowed_methods};
  return (match_result){owner, NULL, NULL, ROUTE_MATCH_NONE, 0};
}

// Frees an array of count path parameter values and the array itself; NULL does
// nothing. find_route and every teardown of a request's state use it, because
// each matched route leaves such an array on the connection.
static void free_param_values(char **pv, int count, ccol_memmgmt_procs *mp) {
  if (!pv) return;
  for (int i = 0; i < count; i++) ccol__mem_free(mp, pv[i]);
  ccol__mem_free(mp, pv);
}

// Maps a request method token to its chttp_method. http1_on_request_line uses
// it to pick the method routes match on, and returns 501 for the
// CHTTP_METHOD_UNKNOWN it gives for any token outside the seven we implement.
static chttp_method parse_method(const char *s, size_t len) {
  if (len == 3 && memcmp(s, "GET", 3) == 0) return CHTTP_GET;
  if (len == 4 && memcmp(s, "POST", 4) == 0) return CHTTP_POST;
  if (len == 3 && memcmp(s, "PUT", 3) == 0) return CHTTP_PUT;
  if (len == 6 && memcmp(s, "DELETE", 6) == 0) return CHTTP_DELETE;
  if (len == 5 && memcmp(s, "PATCH", 5) == 0) return CHTTP_PATCH;
  if (len == 4 && memcmp(s, "HEAD", 4) == 0) return CHTTP_HEAD;
  if (len == 7 && memcmp(s, "OPTIONS", 7) == 0) return CHTTP_OPTIONS;
  return CHTTP_METHOD_UNKNOWN;
}

// Frees the headers and the body of resp and leaves it empty, so a keep-alive
// connection can reuse the same response object for its next request and a
// teardown leaks nothing.
static void destroy_resp(chttpsvr_resp *resp, ccol_memmgmt_procs *mp) {
  for (size_t i = 0; i < resp->header_count; i++) {
    ccol__mem_free(mp, resp->headers[i].name);
    ccol__mem_free(mp, resp->headers[i].value);
  }
  ccol__mem_free(mp, resp->headers);
  resp->headers = NULL;
  resp->header_count = 0;
  resp->header_cap = 0;
  ccol__mem_free(mp, resp->body);
  resp->body = NULL;
  resp->body_len = 0;
  resp->body_cap = 0;
}

// Frees what a request built lazily for its handler (the parsed query
// parameters, the query result and the extra context pairs), so nothing a
// handler asked for outlives the request.
static void destroy_req_scratch(chttpsvr_req *req) {
  ccol_memmgmt_procs *mp = req->m_procs;
  if (req->qparams) {
    chttpsvr_qparams *qp = req->qparams;
    for (size_t i = 0; i < qp->count; i++) {
      ccol__mem_free(mp, qp->keys[i]);
      ccol__mem_free(mp, qp->values[i]);
    }
    ccol__mem_free(mp, qp->keys);
    ccol__mem_free(mp, qp->values);
    ccol__mem_free(mp, qp);
  }
  if (req->qresult) ccol__mem_free(mp, req->qresult);
  if (req->ctx_more) {
    ccol__mem_free(mp, req->ctx_more);
    req->ctx_more = NULL;
  }
}

// Returns the reason phrase of a status code, from the IANA registry, for the
// status line that send_response writes. Clients ignore the phrase (RFC 9112
// SS4), so an unlisted code just gets "Unknown".
static const char *status_reason(int status) {
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

// Converts a chttpsvr_config duration from microseconds to the milliseconds the
// server keeps; chttpsvr_start uses it for every duration field. It rounds up,
// so a non-zero value never becomes the 0 that several fields read as "off" or
// "default", and it saturates at UINT_MAX - 1. UINT_MAX stays free because it's
// the internal form of CHTTPSVR_NO_DEADLINE.
static unsigned cfg_us_to_ms(uint64_t us) {
  uint64_t ms = ccol_us_to_ms_ceil(us);
  return ms >= (uint64_t)UINT_MAX ? UINT_MAX - 1u : (unsigned)ms;
}

// Converts like cfg_us_to_ms, for the fields that accept CHTTPSVR_NO_DEADLINE,
// which has to map to the internal UINT_MAX instead of being clamped.
static unsigned cfg_deadline_us_to_ms(uint64_t us) {
  return us == CHTTPSVR_NO_DEADLINE ? UINT_MAX : cfg_us_to_ms(us);
}

// Turns a configured timeout into the poll(2)-style timeout that
// chttp1_stream_read and chttp1_stream_write take. It's needed because the
// config reads 0 as "wait forever", while those calls read 0 as "don't wait at
// all" and fail before even one try, so 0 becomes -1. A value above INT_MAX is
// clamped to INT_MAX, because the int cast would wrap it negative and turn a
// long finite wait into "forever". Every configured timeout must pass through
// here before it reaches those two calls.
static int to_stream_timeout_ms(unsigned configured_timeout_ms) {
  if (configured_timeout_ms == 0) return -1;
  if (configured_timeout_ms > (unsigned)INT_MAX) return INT_MAX;
  return (int)configured_timeout_ms;
}

// A growable buffer for one response's header block. It starts on the stack,
// moves to the heap when the headers don't fit and then doubles. A fixed buffer
// would drop a valid response with many or long headers (Set-Cookie, CSP) and
// close the connection with nothing on the wire and nothing in the log. heap
// says whether buf has to be freed.
typedef struct {
  char *buf;
  size_t cap;
  size_t len;
  bool heap;
  ccol_memmgmt_procs *mp;
} head_builder;

// Grows b so it has room for needed_extra more bytes, moving it to the heap;
// the two append functions call it when a header doesn't fit. Returns false on
// overflow or a failed allocation, and b is left as it was then.
static bool head_builder_grow(head_builder *b, size_t needed_extra) {
  size_t min_cap = b->len + needed_extra;
  if (min_cap < b->len) return false;
  size_t new_cap = b->cap ? b->cap : 1;
  while (new_cap < min_cap) {
    if (new_cap > SIZE_MAX / 2) {
      new_cap = min_cap;
      break;
    }
    new_cap *= 2;
  }
  char *nb = (char *)ccol__mem_alloc(b->mp, new_cap);
  if (!nb) return false;
  memcpy(nb, b->buf, b->len);
  if (b->heap) ccol__mem_free(b->mp, b->buf);
  b->buf = nb;
  b->cap = new_cap;
  b->heap = true;
  return true;
}

// Appends fmt and its arguments to b, growing it as often as needed.
// send_response builds the status line and the header fields with it. It
// returns false only on a failed allocation or a vsnprintf encoding error. The
// format attribute moves the format string check to every call site, where fmt
// is a literal, and keeps Clang's -Wformat-nonliteral quiet on the inner
// vsnprintf.
static bool __attribute__((format(printf, 2, 3))) head_builder_append(
    head_builder *b, const char *fmt, ...) {
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
    if (!head_builder_grow(b, (size_t)n + 1)) return false;
  }
}

// Appends n bytes of s to b without any formatting, for the prebuilt Date line
// where vsnprintf would be wasted work. Returns false when b can't grow.
static bool head_builder_append_bytes(head_builder *b, const char *s,
                                      size_t n) {
  if (n > b->cap - b->len && !head_builder_grow(b, n)) return false;
  memcpy(b->buf + b->len, s, n);
  b->len += n;
  return true;
}

// Length of an RFC 9110 SS5.6.7 IMF-fixdate such as "Sun, 06 Nov 1994 08:49:37
// GMT". The Date cache and the Date line of send_response are sized from it.
#define CHTTPSVR_IMF_FIXDATE_LEN 29

// The last Date value a thread formatted and the second it stands for. The Date
// header changes once a second but goes on every response, so each thread
// formats it once a second and reuses it. The cache belongs to one thread and
// needs no lock and no atomic.
typedef struct {
  int64_t sec;
  char text[CHTTPSVR_IMF_FIXDATE_LEN + 1];
} chttpsvr_date_cache_state;

// Each thread's Date cache. It takes the same TLS model as the library's other
// thread-local fast paths: initial-exec, unless CMEMPOOL_DYNAMIC_TLS asks for
// the general model (see cmempool.c).
#if defined(CMEMPOOL_DYNAMIC_TLS) && CMEMPOOL_DYNAMIC_TLS
static __thread chttpsvr_date_cache_state chttpsvr_date_cache = {.sec =
                                                                     INT64_MIN};
#else
static __thread chttpsvr_date_cache_state chttpsvr_date_cache
    __attribute__((tls_model("initial-exec"))) = {.sec = INT64_MIN};
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: Any value other than INT64_MIN replaces the clock in
// chttpsvr_date_now, so a test can cross a second boundary without sleeping.
static _Atomic int64_t g_date_override_for_tests = INT64_MIN;
void chttpsvr__set_date_override_for_tests(int64_t sec) {
  atomic_store(&g_date_override_for_tests, sec);
}
#endif

// Writes v (0 to 99) as two decimal digits at out, for the fixed-width fields
// of format_imf_fixdate.
static void put2(char *out, int v) {
  out[0] = (char)('0' + v / 10);
  out[1] = (char)('0' + v % 10);
}

// Formats sec, seconds since the epoch, as a UTC IMF-fixdate into out
// (CHTTPSVR_IMF_FIXDATE_LEN bytes plus a NUL), for the Date header. Day and
// month names come from fixed tables, so the process locale that strftime()
// would read can't change the header, and gmtime_r() touches no shared state. A
// second that time_t can't hold, or one without a four-digit year, gives the
// epoch. It runs once a second per thread, so it stays out of line.
static __attribute__((noinline, cold)) void format_imf_fixdate(int64_t sec,
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
    memcpy(out, "Thu, 01 Jan 1970 00:00:00 GMT", CHTTPSVR_IMF_FIXDATE_LEN);
    out[CHTTPSVR_IMF_FIXDATE_LEN] = '\0';
    return;
  }
  int year = tm.tm_year + 1900;
  memcpy(out, days[tm.tm_wday], 3);
  out[3] = ',';
  out[4] = ' ';
  put2(out + 5, tm.tm_mday);
  out[7] = ' ';
  memcpy(out + 8, months[tm.tm_mon], 3);
  out[11] = ' ';
  put2(out + 12, year / 100);
  put2(out + 14, year % 100);
  out[16] = ' ';
  put2(out + 17, tm.tm_hour);
  out[19] = ':';
  put2(out + 20, tm.tm_min);
  out[22] = ':';
  put2(out + 23, tm.tm_sec);
  memcpy(out + 25, " GMT", 4);
  out[CHTTPSVR_IMF_FIXDATE_LEN] = '\0';
}

// Returns the Date value for the current second, which send_response puts on
// every response that doesn't carry its own. It reads this thread's
// chttpsvr_date_cache and formats it again only when the second has changed
// (g_date_override_for_tests replaces the clock in tests). The coarse real-time
// clock is read from the vDSO with no system call, and its resolution is far
// finer than a second.
static const char *chttpsvr_date_now(void) {
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
  chttpsvr_date_cache_state *c = &chttpsvr_date_cache;
  if (__builtin_expect(c->sec != sec, 0)) {
    format_imf_fixdate(sec, c->text);
    c->sec = sec;
  }
  return c->text;
}

// Frees b's buffer when it lives on the heap; every exit of send_response calls
// it so a grown header block never leaks.
static void head_builder_release(head_builder *b) {
  if (b->heap) ccol__mem_free(b->mp, b->buf);
}

// Counts down against a total-duration cap, at the given instant. It exists so
// a read or a write that waits many times is bounded in total, and not only per
// call. On the first call it sets *deadline max_dur ms after that instant and
// latches *deadline_set. Every call then lowers *timeout_ms_inout to the time
// left when that is shorter (a 0 there means no per-call bound yet, so the time
// left wins). Returns false once not a single nanosecond is left. The time left
// rounds up to whole milliseconds and is computed in long long, so a wait never
// ends early and a cap of UINT_MAX ms can't overflow a 32-bit long.
static inline __attribute__((always_inline)) bool shrink_timeout_at(
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
  long long remaining_ns = ns_between(*deadline, now);
  if (remaining_ns <= 0) return false;
  long long remaining_ms = ms_ceil_of_ns(remaining_ns);
  unsigned remaining_ms_clamped =
      (remaining_ms > (long long)UINT_MAX) ? UINT_MAX : (unsigned)remaining_ms;
  if (!*timeout_ms_inout || remaining_ms_clamped < *timeout_ms_inout)
    *timeout_ms_inout = remaining_ms_clamped;
  return true;
}

// Runs shrink_timeout_at at the current monotonic time. check_read_deadline and
// send_response both use it, so both total-duration caps work the same way.
// With max_dur 0 the cap is off and it returns true without touching anything,
// so a disabled cap costs one branch.
static inline __attribute__((always_inline)) bool
shrink_timeout_to_deadline_inl(struct timespec *deadline, bool *deadline_set,
                               unsigned max_dur, unsigned *timeout_ms_inout) {
  if (!max_dur) return true;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  return shrink_timeout_at(deadline, deadline_set, max_dur, now,
                           timeout_ms_inout);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Returns what shrink_timeout_at decides, and the timeout it shrinks
// to, for a deadline that is already set, so a test can check the rounding
// without a clock.
bool chttpsvr__shrink_timeout_at_for_tests(struct timespec deadline,
                                           struct timespec now,
                                           unsigned *timeout_ms_inout) {
  bool set = true;
  return shrink_timeout_at(&deadline, &set, 1, now, timeout_ms_inout);
}
#endif

// Out-of-line shrink_timeout_to_deadline_inl, for callers off the request
// body's path that don't need it inlined.
static bool shrink_timeout_to_deadline(struct timespec *deadline,
                                       bool *deadline_set, unsigned max_dur,
                                       unsigned *timeout_ms_inout) {
  return shrink_timeout_to_deadline_inl(deadline, deadline_set, max_dur,
                                        timeout_ms_inout);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: The next response_write_deadline_ok reports an expired deadline
// whatever the clock says, then disarms itself. A rejection's small header
// block always goes out in one write, so a test can't get a real expired
// deadline for it any other way.
static _Atomic bool g_force_next_response_write_deadline_expired_for_tests =
    false;
void chttpsvr__force_next_response_write_deadline_expired_for_tests(void) {
  atomic_store(&g_force_next_response_write_deadline_expired_for_tests, true);
}
#endif

// Applies the write deadline of conn to one write attempt and shrinks
// *call_timeout_ms to the time left. send_response, send_response_continue and
// write_interim_continue all call it before each write, so a slow reader can't
// keep a write going past its total cap. With apply_internal_ceiling false (a
// handler's response) the cap is the server's max_response_write_duration_ms,
// where 0 means no limit. With it true (a rejection or the interim 100 Continue
// line) the cap is the tighter of that value and
// CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS, so these small writes of our own are
// bounded even when the operator turned the cap off.
// Returns false once the deadline has passed. It reads
// srv->max_response_write_duration_ms on every call and sets
// conn->write_deadline and write_deadline_set on the first one. conn is never
// NULL.
static bool response_write_deadline_ok(chttpsvr_conn *conn,
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
      (configured == 0 || configured > internal_write_ceiling_ms()))
    effective_max_dur = internal_write_ceiling_ms();
  return shrink_timeout_to_deadline(&conn->write_deadline,
                                    &conn->write_deadline_set,
                                    effective_max_dur, call_timeout_ms);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: The next handler response write sends only its first n bytes and
// then acts as if the socket were full, so a test can end a partial write
// inside the head or the body and watch the parked rest resume. Disarms itself
// after one use.
static _Atomic size_t g_force_short_response_write_for_tests = 0;
void chttpsvr__force_short_response_write_for_tests(size_t n) {
  atomic_store(&g_force_short_response_write_for_tests, n);
}
#endif

// Serializes resp into an HTTP/1.1 response and writes the head and the body
// together, as one writev(2) or one TLS record when they fit. task_worker sends
// every handler response with it and conn_reject_and_close every rejection. It
// tries the write first and waits only once the socket is full, so an ordinary
// response costs one system call.
// keep_alive is the only source of the Connection header, and resp->body_len
// the only source of Content-Length where a body goes out, so the handler's
// values for those two never reach the wire. Otherwise the response could
// announce keep-alive while the caller closes, or a length other than what we
// send, and a kept-alive connection would lose its framing. A 1xx or
// out-of-range status goes out as 500. 204, 205, 304 and a HEAD request
// (suppress_body) carry no body. A 204 has no Content-Length, a 205 always says
// 0, and a 304 or a bodyless HEAD response carries the handler's Content-Length
// if it set one (RFC 9110 SS8.6). A HEAD answer to a rejection (is_reject)
// reports the length of the body its GET form would get. A Date header is added
// unless the handler set one.
// Returns false on an error, a timeout or a park, and the caller must then stop
// using the connection. Every write is bounded by timeout_ms and by
// response_write_deadline_ok on conn (is_reject switches on the internal
// ceiling). A stream with write_nonblocking set parks the unsent rest on conn
// instead of waiting (see send_response_park); the stack copy of the head is
// copied there.
static bool send_response(chttp1_stream *stream, chttpsvr_resp *resp,
                          bool keep_alive, unsigned timeout_ms,
                          bool suppress_body, chttpsvr_conn *conn,
                          bool is_reject) {
  char stack_buf[4096];
  head_builder hb = {.buf = stack_buf,
                     .cap = sizeof(stack_buf),
                     .len = 0,
                     .heap = false,
                     .mp = resp->m_procs};

  int status = (resp->status_code >= 200 && resp->status_code <= 999)
                   ? resp->status_code
                   : 500;
  bool no_body =
      suppress_body || status == 204 || status == 304 || status == 205;
  bool handler_date = false;
  const char *handler_length = NULL;

  if (!head_builder_append(&hb, "HTTP/1.1 %d %s\r\n", status,
                           status_reason(status))) {
    head_builder_release(&hb);
    return false;
  }

  for (size_t i = 0; i < resp->header_count; i++) {
    if (ccol_ascii_casecmp(resp->headers[i].name, "connection") == 0) continue;
    if (ccol_ascii_casecmp(resp->headers[i].name, "content-length") == 0) {
      handler_length = resp->headers[i].value;
      continue;
    }
    if (ccol_ascii_casecmp(resp->headers[i].name, "date") == 0)
      handler_date = true;
    if (!head_builder_append(&hb, "%s:%s\r\n", resp->headers[i].name,
                             resp->headers[i].value)) {
      head_builder_release(&hb);
      return false;
    }
  }

  if (!handler_date) {
    char date_line[5 + CHTTPSVR_IMF_FIXDATE_LEN + 2];
    memcpy(date_line, "date:", 5);
    memcpy(date_line + 5, chttpsvr_date_now(), CHTTPSVR_IMF_FIXDATE_LEN);
    date_line[5 + CHTTPSVR_IMF_FIXDATE_LEN] = '\r';
    date_line[6 + CHTTPSVR_IMF_FIXDATE_LEN] = '\n';
    if (!head_builder_append_bytes(&hb, date_line, sizeof(date_line))) {
      head_builder_release(&hb);
      return false;
    }
  }

  if (status != 204) {
    bool ok_len = true;
    bool handler_head = suppress_body && resp->body_len == 0 && !is_reject;
    if (__builtin_expect(status == 205, 0))
      ok_len = head_builder_append(&hb, "content-length:0\r\n");
    else if (__builtin_expect(status != 304 && !handler_head, 1))
      ok_len =
          head_builder_append(&hb, "content-length:%zu\r\n", resp->body_len);
    else if (handler_length)
      ok_len =
          head_builder_append(&hb, "content-length:%s\r\n", handler_length);
    if (!ok_len) {
      head_builder_release(&hb);
      return false;
    }
  }
  {
    const char *cval = keep_alive ? "keep-alive" : "close";
    if (!head_builder_append(&hb, "connection:%s\r\n", cval)) {
      head_builder_release(&hb);
      return false;
    }
  }
  if (!head_builder_append(&hb, "\r\n")) {
    head_builder_release(&hb);
    return false;
  }

  size_t blen = no_body ? 0 : resp->body_len;
  size_t total = hb.len + blen;
  size_t sent = 0;
  bool ok = true;
  while (sent < total) {
    unsigned call_timeout_ms = timeout_ms;
    if (conn &&
        !response_write_deadline_ok(conn, is_reject, &call_timeout_ms)) {
      ok = false;
      break;
    }
    size_t alen = sent < hb.len ? hb.len - sent : 0;
    size_t boff = sent > hb.len ? sent - hb.len : 0;
#ifdef RUNNING_UNIT_TESTS
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
      send_response_park(conn, stream, hb.buf + (hb.len - alen), alen, boff,
                         keep_alive, no_body, is_reject);
      ok = false;
      break;
    }
#endif
    ssize_t n = chttp1_stream_writev2(
        stream, hb.buf + (hb.len - alen), alen, blen ? resp->body + boff : NULL,
        blen - boff, to_stream_timeout_ms(call_timeout_ms));
    if (n <= 0) {
      if (n < 0 && stream->write_nonblocking && conn &&
          chttp1_stream_timed_out(stream))
        send_response_park(conn, stream, hb.buf + (hb.len - alen), alen, boff,
                           keep_alive, no_body, is_reject);
      ok = false;
      break;
    }
    sent += (size_t)n;
  }
  head_builder_release(&hb);
  return ok;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: With 0 < n < 25, the next write_interim_continue writes exactly n
// bytes of the line for real and then fails as if its deadline expired. Socket
// buffering gives no reliable way to get a real short write of 25 bytes.
// Disarms itself after one use.
static _Atomic size_t g_force_short_interim_write_bytes_for_tests = 0;
void chttpsvr__force_short_interim_write_for_tests(size_t n) {
  atomic_store(&g_force_short_interim_write_bytes_for_tests, n);
}
#endif
#ifdef RUNNING_UNIT_TESTS
// Test hook: The next interim line written without waiting sends its first n
// bytes (n may be 0) and then acts as if the socket were full, so a test can
// watch the rest wait in a parked connection. SIZE_MAX means not armed.
static _Atomic size_t g_force_interim_park_after_for_tests = SIZE_MAX;
void chttpsvr__force_interim_park_after_for_tests(size_t n) {
  atomic_store(&g_force_interim_park_after_for_tests, n);
}
#endif

// The interim 100 Continue response (RFC 7231 SS5.1.1) and its length, which
// write_interim_continue sends and counts against.
static const char interim_continue_line[] = "HTTP/1.1 100 Continue\r\n\r\n";
#define INTERIM_CONTINUE_LEN (sizeof(interim_continue_line) - 1)

// Writes the interim 100 Continue line for an Expect: 100-continue request,
// from task_worker for a buffered route and from chttpsvr_req_read for a
// streaming one; the client sends the body only after it sees this line. It
// loops on short writes like send_response, and every attempt goes through
// response_write_deadline_ok with the internal ceiling, so a peer that reads
// slowly can't hold a worker thread for ever.
// Returns 1 when the whole line is out. Returns -1 when a stream with
// write_nonblocking hit a full socket and the rest was parked on conn; the
// caller lets go of the connection and task_resume finishes it. Returns 0 on a
// failure or an expired deadline, also when copying the rest fails. A 0 can
// mean a truncated line is on the wire, so the caller must write nothing more
// and close.
// It sets conn->wp_interim when it parks, and clears conn->write_deadline_set
// on every exit. The deadline belongs to the response, and a leftover one would
// charge send_response for the whole body read in between.
static int write_interim_continue(chttpsvr_conn *conn, chttp1_stream *stream,
                                  unsigned wtimeout_ms) {
  const char *cont = interim_continue_line;
  const size_t clen = INTERIM_CONTINUE_LEN;
  size_t sent = 0;
#ifdef RUNNING_UNIT_TESTS
  size_t forced_short =
      atomic_exchange(&g_force_short_interim_write_bytes_for_tests, 0);
  if (forced_short > 0 && forced_short < clen) {
    ssize_t n = chttp1_stream_write(stream, cont, forced_short,
                                    to_stream_timeout_ms(wtimeout_ms));
    if (n > 0) sent = (size_t)n;
    conn->write_deadline_set = false;
    return sent == clen;
  }
  size_t forced_park = SIZE_MAX;
  if (stream->write_nonblocking)
    forced_park =
        atomic_exchange(&g_force_interim_park_after_for_tests, SIZE_MAX);
#endif
  while (sent < clen) {
    unsigned call_timeout_ms = wtimeout_ms;
    if (!response_write_deadline_ok(conn, true, &call_timeout_ms)) break;
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
                              to_stream_timeout_ms(call_timeout_ms));
    if (n > 0) sent += (size_t)n;
    if (!would_block && n > 0) continue;
    if (!would_block && n < 0 && stream->write_nonblocking &&
        chttp1_stream_timed_out(stream))
      would_block = true;
    if (would_block) {
      if (sent == clen) break;
      send_response_park(conn, stream, cont + sent, clen - sent, 0, true, true,
                         true);
      if (conn->wp_active && conn->wp_keep_alive) {
        conn->wp_interim = true;
        return -1;
      }
      conn->wp_active = false;
      ccol__mem_free(conn->m_procs, conn->wp_head);
      conn->wp_head = NULL;
    }
    break;
  }
  conn->write_deadline_set = false;
  return sent == clen;
}

// Runs the next middleware of the request's chain, or the route handler once
// the chain is used up. It's the next function every middleware receives, so a
// middleware passes the request on by calling it. It reads and advances
// req->conn->dispatch. A NULL route marks a rejected request (see
// conn_reject_and_close), and reaching the end of its chain means sending the
// rejection that resp already holds.
static void chttpsvr_next(chttpsvr_req *req, chttpsvr_resp *resp) {
  dispatch_ctx *ctx = &req->conn->dispatch;
  if (ctx->mw_idx < ctx->mw_count) {
    mw_entry entry = ctx->mw_snap[ctx->mw_idx++];
    entry.fn(req, resp, entry.ctx, chttpsvr_next);
    return;
  }
  if (ctx->route) ctx->route->fn(req, resp, ctx->route->ctx);
}

// Tells conn_reject_and_close whether a rejection with this status runs the
// middleware chain first. 404, 405 and 413 are ordinary client traffic that a
// rate limiter or an access log wants to see. A 500 comes from an allocation
// failure, a 503 from a full worker pool, and a 501 is decided before there is
// a request object, so none of those runs application code.
static bool reject_status_runs_middleware(int status) {
  return status == CHTTP_STATUS_NOT_FOUND ||
         status == CHTTP_STATUS_METHOD_NOT_ALLOWED ||
         status == CHTTP_STATUS_PAYLOAD_TOO_LARGE;
}

// Copies the effective middleware chain into dispatch: The root router's chain,
// then router's when it's a sub-router. The copy lets the chain run after
// srv->routes_lock is released, while routes or middleware can be added. router
// is the matched route's router, the sub-router that owns a 404 or 405 path, or
// NULL for a rejection from the root.
// Returns false when the chains hold more than CHTTPSVR_MAX_MW entries, which
// the caller answers with a 500. The caller holds srv->routes_lock.
// dispatch->route stays NULL and the caller sets it for a matched route; a NULL
// route keeps a rejected request away from every handler.
static bool snapshot_chain(struct chttpserver *srv, chttpsvr_router *router,
                           dispatch_ctx *dispatch) {
  int mc = 0;
  chttpsvr_mw_node *n;
  for (n = srv->root_router->mw_head; n && mc < CHTTPSVR_MAX_MW; n = n->next)
    dispatch->mw_snap[mc++] = (mw_entry){n->fn, n->ctx};
  bool overflow = (n != NULL);
  if (!overflow && router && router != srv->root_router) {
    for (n = router->mw_head; n && mc < CHTTPSVR_MAX_MW; n = n->next)
      dispatch->mw_snap[mc++] = (mw_entry){n->fn, n->ctx};
    overflow = (n != NULL);
  }
  dispatch->srv = srv;
  dispatch->router = router ? router : srv->root_router;
  dispatch->route = NULL;
  dispatch->mw_idx = 0;
  dispatch->mw_count = mc;
  return !overflow;
}

// Frees the body of the request that ended and gives back its body memory
// reservation in the same step. The reservation lives exactly as long as the
// buffer, so max_partial_body_memory bounds every body byte a buffered route
// holds, the one a running handler reads included. task_finish,
// conn_drop_request_state, conn_reset_for_request and conn_close call it. It
// clears conn->body and releases conn's charge on the server's shared body
// memory budget (mem_release), which can let a waiting request go ahead. A
// Content-Length body reserves memory before its buffer exists, so the release
// doesn't depend on the buffer.
static inline __attribute__((always_inline)) void conn_drop_body(
    chttpsvr_conn *conn) {
  if (conn->body.buf) {
    ccol__mem_free(conn->m_procs, conn->body.buf);
    memset(&conn->body, 0, sizeof(conn->body));
  }
  if (__builtin_expect(conn->mem_charged != 0 || conn->mem_exempt, 0))
    mem_release(conn);
}

// Frees what the request holds that nobody reads once its response is being
// written: The body, the path, the query, the header fields and the path
// parameter values. A connection calls it when it starts to wait for a slow
// reader or to linger, because such a wait holds no thread and no memory limit
// could cover what it keeps. Its body reservation goes back to the server's
// budget through conn_drop_body. matched_route stays, since routes live as long
// as the server.
static __attribute__((noinline, cold)) void conn_drop_request_state(
    chttpsvr_conn *conn) {
  ccol_memmgmt_procs *mp = conn->m_procs;
  conn_drop_body(conn);
  ccol__mem_free(mp, conn->path);
  conn->path = NULL;
  ccol__mem_free(mp, conn->decoded_path);
  conn->decoded_path = NULL;
  ccol__mem_free(mp, conn->raw_query);
  conn->raw_query = NULL;
  for (size_t i = 0; i < conn->hdr_count; i++) {
    ccol__mem_free(mp, conn->hdr_names[i]);
    ccol__mem_free(mp, conn->hdr_values[i]);
  }
  ccol__mem_free(mp, conn->hdr_names);
  ccol__mem_free(mp, conn->hdr_values);
  conn->hdr_names = conn->hdr_values = NULL;
  conn->hdr_count = conn->hdr_cap = 0;
  free_param_values(conn->matched_param_values,
                    conn->matched_route ? conn->matched_route->param_count : 0,
                    mp);
  conn->matched_param_values = NULL;
}

// Resets conn for its next request: It frees the previous request's state and
// sets every per-request field to its default. conn_create runs it for the
// first request and the keep-alive path between every two requests; without the
// free of the param values, every request but the last would leak them. The
// header-phase clock and the rate floor belong to one request, so they're
// switched off; a header clock left armed would charge the next request for the
// keep-alive gap.
// The parser is re-initialized in place, because a struct assignment would copy
// its 8 KiB line buffer every time. It reads the server's max_header_bytes and
// max_body_size: The parser's max_chunk_size_override is set to max_body_size,
// so one huge declared chunk is refused at once instead of tying up a worker
// while it waits for bytes that never come. A max_body_size of 0 leaves it at
// 0, which is also "no cap" there. The body reservation goes back to the server
// through conn_drop_body.
static void conn_reset_for_request(chttpsvr_conn *conn) {
  ccol_memmgmt_procs *mp = conn->m_procs;
  ccol__mem_free(mp, conn->path);
  conn->path = NULL;
  ccol__mem_free(mp, conn->decoded_path);
  conn->decoded_path = NULL;
  ccol__mem_free(mp, conn->raw_query);
  conn->raw_query = NULL;
  for (size_t i = 0; i < conn->hdr_count; i++) {
    ccol__mem_free(mp, conn->hdr_names[i]);
    ccol__mem_free(mp, conn->hdr_values[i]);
  }
  ccol__mem_free(mp, conn->hdr_names);
  ccol__mem_free(mp, conn->hdr_values);
  conn->hdr_names = conn->hdr_values = NULL;
  conn->hdr_count = conn->hdr_cap = 0;
  free_param_values(conn->matched_param_values,
                    conn->matched_route ? conn->matched_route->param_count : 0,
                    mp);
  conn->matched_router = NULL;
  conn->matched_route = NULL;
  conn->matched_param_values = NULL;
  memset(&conn->dispatch, 0, sizeof(conn->dispatch));
  destroy_resp(&conn->resp, mp);
  conn->resp.status_code = CHTTP_STATUS_OK;
  conn->resp.m_procs = mp;
  conn->req_rejected = false;
  conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
  conn->options_star = false;
  conn->expects_continue = false;
  conn->interim_continue_sent = false;
  conn->interim_write_failed = false;
  conn_drop_body(conn);
  conn->body_bytes_seen = 0;
  conn->body_too_large = false;
  conn->body_alloc_failed = false;
  conn->transfer_aborted = false;
  conn->body_malformed = false;
  conn->read_deadline_set = false;
  conn->deadline_exceeded = false;
  conn->write_deadline_set = false;
  conn->header_phase_active = false;
  conn->rate_active = false;

  const chttp1_settings *settings = conn->parser.settings;
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  size_t max_hdr_bytes = atomic_load(&conn->srv->max_header_bytes);
  if (max_hdr_bytes) {
    conn->parser.max_header_count_override = 0;
    conn->parser.max_total_header_bytes_override = max_hdr_bytes;
  }
  conn->parser.max_chunk_size_override =
      (uint64_t)atomic_load(&conn->srv->max_body_size);
}

// Allocates a connection for an accepted fd, so the reactor has somewhere to
// keep its parser and request state. It starts with one lifetime reference, the
// application's, and runs conn_reset_for_request so even the first request gets
// the per-request defaults; a calloc'ed status code of 0 would otherwise go out
// as 500. Returns NULL when the allocation fails.
static chttpsvr_conn *conn_create(struct chttpserver *srv, int fd,
                                  const chttp1_settings *settings) {
  chttpsvr_conn *conn =
      (chttpsvr_conn *)ccol__mem_calloc(srv->m_procs, 1, sizeof(*conn));
  if (!conn) return NULL;
  conn->fd = fd;
  conn->srv = srv;
  conn->m_procs = srv->m_procs;
  atomic_init(&conn->lifetime_refs, 1);
  chttp1_parser_init_request(&conn->parser, settings);
  conn->parser.data = conn;
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  conn_reset_for_request(conn);
  return conn;
}

// Frees a connection once its last lifetime reference is gone: It unlinks conn
// from the idle and diverted lists, destroys its TLS state, closes the fd and
// frees everything it owns. It can run on the reactor thread from
// conn_on_removed. The decrement of srv->current_connections comes last, after
// every access to conn: drain_and_close_all_connections takes 0 as proof that
// every connection is done and lets the server teardown free the routers and
// procs.
static void conn_free(chttpsvr_conn *conn) {
  if (!conn) return;
  struct chttpserver *srv = conn->srv;
  ccol_memmgmt_procs *mp = conn->m_procs;
  idle_list_remove(conn);
  diverted_list_remove(conn);
  if (conn->tls) ctls_conn_destroy(conn->tls);
  if (conn->fd >= 0) close(conn->fd);
  ccol__mem_free(mp, conn->path);
  ccol__mem_free(mp, conn->decoded_path);
  ccol__mem_free(mp, conn->raw_query);
  for (size_t i = 0; i < conn->hdr_count; i++) {
    ccol__mem_free(mp, conn->hdr_names[i]);
    ccol__mem_free(mp, conn->hdr_values[i]);
  }
  ccol__mem_free(mp, conn->hdr_names);
  ccol__mem_free(mp, conn->hdr_values);
  free_param_values(conn->matched_param_values,
                    conn->matched_route ? conn->matched_route->param_count : 0,
                    mp);
  destroy_resp(&conn->resp, mp);
  ccol__mem_free(mp, conn->body.buf);
  ccol__mem_free(mp, conn->carry_over);
  if (conn->wp_head) ccol__mem_free(mp, conn->wp_head);
  ccol__mem_free(mp, conn);

  atomic_fetch_sub(&srv->current_connections, 1);
}

// The on_removed handler of every conn->reg registration. It exists because
// ceventloop_remove can't promise synchronously that no dispatch touches conn
// without a lock cycle with the server's locks, so the free waits for this
// call. It fires once per registration, from the reactor's reclaim path. It
// drops one conn->lifetime_refs reference and frees conn on the last one, then
// always drops one srv->lifetime_refs reference and finishes the server's
// destroy on the last one, so chttpsvr__destroy never has to block for it. srv
// is read before conn may be freed.
static void conn_on_removed(void *arg) {
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  struct chttpserver *srv = conn->srv;
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) conn_free(conn);
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    chttpsvr_finish_destroy(srv);
}

// Closes conn from the application side; every path that ends a connection
// calls it, once per connection. It unlinks conn from the idle and diverted
// lists at once, so neither the sweep nor a shutdown drain can find it again,
// and removes its registration from the server reactor. The free happens later
// in conn_on_removed when an earlier registration holds a reference too. It
// drops the body at once so a waiting request can take its memory, then drops
// the application's lifetime reference.
static void conn_close(chttpsvr_conn *conn) {
  idle_list_remove(conn);
  diverted_list_remove(conn);

  if (conn->reg) {
    ceventloop_reg reg = conn->reg;
    conn->reg = CEVENTLOOP_REG_NULL;
    ceventloop_remove(srv_engine_bundler.reactor, reg);
  }

  conn->state = CONN_ST_CLOSING;
  conn_drop_body(conn);
  if (atomic_fetch_sub(&conn->lifetime_refs, 1) == 1) conn_free(conn);
}

// The parser callback for the request line, on the reactor thread. It records
// the method and stores the raw path (percent-encoded as it arrived) and the
// query on conn, which routing and the request accessors read later, and it
// refuses request lines that this server can't serve safely. Returns 1 to stop
// the parse when it rejects the request, always with conn->req_rejected and
// reject_status set, because conn_feed_bytes only answers a CHTTP1_USER abort
// with a response when they are set.
// A major version other than 1 gets 505, and a method we don't implement gets
// 501, since a CHTTP_ANY route would otherwise hand its handler a meaningless
// method. "OPTIONS *" is answered by the server itself; "*" with any other
// method is a 400. An absolute-form target is reduced to its origin-form part,
// and an empty path there means "/". Any other path that doesn't start with "/"
// is a 400, or "admin/secret" would reach the "/admin/secret" route while a
// middleware guarding "/admin" let it through. A failed allocation gets 500.
static int http1_on_request_line(chttp1_parser *p, const char *method,
                                 size_t method_len, const char *target,
                                 size_t target_len) {
  chttpsvr_conn *conn = (chttpsvr_conn *)p->data;

  if (p->http_major != 1) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_HTTP_VERSION_NOT_SUPPORTED;
    return 1;
  }

  conn->method = parse_method(method, method_len);

  if (conn->method == CHTTP_METHOD_UNKNOWN) {
    conn->req_rejected = true;
    conn->reject_status = (target_len == 1 && target[0] == '*')
                              ? CHTTP_STATUS_BAD_REQUEST
                              : CHTTP_STATUS_NOT_IMPLEMENTED;
    return 1;
  }

  if (target_len == 1 && target[0] == '*') {
    if (conn->method != CHTTP_OPTIONS) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
      return 1;
    }
    char *star = (char *)ccol__mem_alloc(conn->m_procs, 2);
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

  const char *eff_target = target;
  size_t eff_target_len = target_len;
  bool was_absolute =
      strip_absolute_form(target, target_len, &eff_target, &eff_target_len);

  const char *qmark = (const char *)memchr(eff_target, '?', eff_target_len);
  size_t path_raw_len = qmark ? (size_t)(qmark - eff_target) : eff_target_len;

  if (path_raw_len > 0 ? eff_target[0] != '/' : !was_absolute) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
    return 1;
  }

  bool empty_abs_path = (was_absolute && path_raw_len == 0);
  size_t path_alloc_len = empty_abs_path ? 1 : path_raw_len;
  char *path_raw = (char *)ccol__mem_alloc(conn->m_procs, path_alloc_len + 1);
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
    conn->raw_query = (char *)ccol__mem_alloc(conn->m_procs, qlen + 1);
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

// Parser callback for one header field of the request: It copies name and value
// into conn->hdr_names and conn->hdr_values, growing both arrays as needed.
// chttpsvr_req_header reads those arrays later. A failed allocation sets
// conn->req_rejected with a 500 status before returning 1, so the client gets
// an answer instead of a silent close.
// Only the header block reaches this function. init_parser_settings leaves
// on_trailer NULL, so a trailer field can't land in these arrays;
// chttpsvr_req_header scans them backwards, and a trailer there would override
// a header that an upstream proxy set on a request we already routed.
static int http1_on_header(chttp1_parser *p, const char *name, size_t name_len,
                           const char *value, size_t value_len) {
  chttpsvr_conn *conn = (chttpsvr_conn *)p->data;
  if (conn->hdr_count >= conn->hdr_cap) {
    size_t new_cap = doubling_growth_cap(conn->hdr_cap, sizeof(char *), 8);
    if (new_cap == 0) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    char **nn = (char **)ccol__mem_realloc(conn->m_procs, conn->hdr_names,
                                           new_cap * sizeof(char *));
    if (!nn) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_names = nn;
    char **nv = (char **)ccol__mem_realloc(conn->m_procs, conn->hdr_values,
                                           new_cap * sizeof(char *));
    if (!nv) {
      conn->req_rejected = true;
      conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
      return 1;
    }
    conn->hdr_values = nv;
    conn->hdr_cap = new_cap;
  }
  char *n = (char *)ccol__mem_alloc(conn->m_procs, name_len + 1);
  char *v = (char *)ccol__mem_alloc(conn->m_procs, value_len + 1);
  if (!n || !v) {
    ccol__mem_free(conn->m_procs, n);
    ccol__mem_free(conn->m_procs, v);
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

// Parser callback that runs once the request headers are complete. It decides
// what happens to the request: It rejects (sets conn->req_rejected and
// conn->reject_status, returns -1) a transfer coding other than chunked with
// 501, an unmatched path or method with 404 or 405, a declared Content-Length
// above max_body_size on a buffered route with 413, and any allocation failure
// with 500. Otherwise it stores the matched router, route, path parameter
// values and decoded path on conn and returns CHTTP1_HEADERS_DIVERT_BODY, so
// every request goes to a worker.
// It takes srv->routes_lock for reading around find_route and snapshot_chain,
// so the middleware chain is copied under the lock and a 404, 405 or 413 runs
// the chain of the router that owns the path. Streaming routes never get the
// 413 here, because their handler is always called and decides the answer
// itself. conn->divert_gate tells conn_start_diverted whether the request needs
// the streaming pool or a body memory reservation first. The decoded path is
// allocated before the hand-over, because chttpsvr_req_path promises a handler
// a non-NULL pointer.
static int http1_on_headers_complete(chttp1_parser *p) {
  chttpsvr_conn *conn = (chttpsvr_conn *)p->data;
  if (conn->req_rejected) return -1;

  if (chttp1_has_other_transfer_coding(p)) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_NOT_IMPLEMENTED;
    return -1;
  }

  struct chttpserver *srv = conn->srv;
  if (conn->options_star) {
    conn->matched_router = srv->root_router;
    conn->matched_route = &options_star_route;
    conn->expects_continue = chttp1_expects_continue(p);
    conn->divert_gate = 0;
    return CHTTP1_HEADERS_DIVERT_BODY;
  }
  ccol__rw_lock_rdlock(srv->routes_lock);
  match_result mr = find_route(srv, conn->path, conn->method);
  if (mr.result == ROUTE_MATCH_NONE || mr.result == ROUTE_MATCH_METHOD) {
    bool chain_ok = snapshot_chain(srv, mr.router, &conn->dispatch);
    ccol__rw_lock_unlock(srv->routes_lock);
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
    ccol__rw_lock_unlock(srv->routes_lock);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }

  dispatch_ctx *dispatch = &conn->dispatch;
  if (!snapshot_chain(srv, mr.router, dispatch)) {
    ccol__rw_lock_unlock(srv->routes_lock);
    dispatch->mw_count = 0;
    free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    return -1;
  }
  dispatch->route = mr.route;
  ccol__rw_lock_unlock(srv->routes_lock);

  size_t divert_gate = 0;
  if (mr.route->is_streaming) {
    if (atomic_load(&srv->stream_threads) >= 0) divert_gate = SIZE_MAX;
  } else if (chttp1_has_content_length(p)) {
    size_t limit = atomic_load(&srv->max_body_size);
    uint64_t declared = chttp1_declared_content_length(p);
    if (limit && declared > (uint64_t)limit) {
      free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
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

  size_t plen = strlen(conn->path);
  char *dp = (char *)ccol__mem_alloc(srv->m_procs, plen + 1);
  if (dp) {
    ssize_t dlen = decode_path_unsafe(dp, conn->path);
    if (dlen >= 0) {
      dp[(size_t)dlen] = '\0';
    } else {
      ccol__mem_free(srv->m_procs, dp);
      dp = NULL;
    }
  }
  if (!dp) {
    free_param_values(mr.param_values, mr.route->param_count, srv->m_procs);
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

  return CHTTP1_HEADERS_DIVERT_BODY;
}

// First buffer capacity of a Content-Length body: The 8 KiB that can come with
// the headers plus one 8 KiB read window of drain_body. It is also what such a
// body reserves before a worker reads it (see body_first_reserve).
#define CHTTPSVR_BODY_FIRST_CAP ((size_t)16384)

// Returns the capacity the body buffer of conn grows to from cap so it holds
// min_cap bytes. http1_on_body and drain_body_socket_read both use it, so a
// growth and its memory reservation agree. limit is the max_body_size of the
// request (0 for none).
// A buffered Content-Length body that arrived whole while body memory is
// limited gets its full declared length at once, since it holds no reservation.
// Any other Content-Length body doubles from CHTTPSVR_BODY_FIRST_CAP up to its
// declared length, so a client that declares a lot and sends little holds
// little memory. Other bodies double from 8 KiB up to limit. The doubling stops
// before it can wrap past SIZE_MAX. It reads conn->mem_charged,
// conn->mem_exempt and srv->mem_cap.
static size_t body_grow_target(const chttpsvr_conn *conn, size_t cap,
                               size_t min_cap, size_t limit) {
  if (min_cap <= cap) return cap;
  size_t ceiling = limit;
  size_t start = 8192;
  if (!conn->matched_route->is_streaming && !conn->options_star &&
      chttp1_has_content_length(&conn->parser)) {
    uint64_t rest = chttp1_declared_content_length(&conn->parser);
    size_t held = conn->body.len;
    size_t d =
        rest < (uint64_t)(SIZE_MAX - held) ? held + (size_t)rest : SIZE_MAX;
    if (d >= min_cap) {
      if (conn->mem_charged == 0 && !conn->mem_exempt &&
          atomic_load(&conn->srv->mem_cap) != SIZE_MAX)
        return d;
      if (!ceiling || d < ceiling) ceiling = d;
      start = CHTTPSVR_BODY_FIRST_CAP;
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

static void mem_trim(chttpsvr_conn *conn, size_t keep);
#ifdef RUNNING_UNIT_TESTS
static void body_growth_note_for_tests(const chttpsvr_conn *conn,
                                       size_t old_cap, size_t new_cap);
#endif

// Parser callback for each piece of request body. It appends the bytes to
// conn->body, which collects a buffered route's whole body or holds the bytes
// of a streaming route that chttpsvr_req_read hasn't taken yet, and it enforces
// max_body_size, which the parser knows nothing about. Returns 0, or 1 to stop
// the parse: conn->body_too_large is set for a body over the limit or one whose
// size would overflow size_t (possible on ILP32 for a long streaming body), and
// conn->body_alloc_failed for a real allocation failure.
// min_cap is checked for wraparound before it is compared with the capacity, or
// a wrapped sum would skip the growth and overflow the buffer in the memcpy.
// After a growth the body's memory reservation is trimmed to the new buffer,
// because the old block is gone.
static int http1_on_body(chttp1_parser *p, const char *at, size_t len) {
  chttpsvr_conn *conn = (chttpsvr_conn *)p->data;
  if (len > SIZE_MAX - conn->body_bytes_seen) {
    conn->body_too_large = true;
    return 1;
  }
  conn->body_bytes_seen += len;
  size_t body_limit = conn->options_star
                          ? CHTTPSVR_OPTIONS_STAR_MAX_BODY
                          : atomic_load(&conn->srv->max_body_size);
  if (body_limit && conn->body_bytes_seen > body_limit) {
    conn->body_too_large = true;
    return 1;
  }
  growbuf *b = &conn->body;
  if (conn->matched_route->is_streaming && b->pos == b->len && b->pos > 0)
    b->pos = b->len = 0;
  size_t min_cap = b->len + len;
  if (min_cap < b->len) {
    conn->body_too_large = true;
    return 1;
  }
  if (min_cap > b->cap) {
    size_t old_cap = b->cap;
    size_t new_cap = body_grow_target(conn, old_cap, min_cap, body_limit);
    char *nb = (char *)ccol__mem_realloc(conn->m_procs, b->buf, new_cap);
    if (!nb) {
      conn->body_alloc_failed = true;
      return 1;
    }
    b->buf = nb;
    b->cap = new_cap;
#ifdef RUNNING_UNIT_TESTS
    body_growth_note_for_tests(conn, old_cap, new_cap);
#endif
    if (__builtin_expect(conn->mem_charged > new_cap, 0))
      mem_trim(conn, new_cap);
  }
  memcpy(b->buf + b->len, at, len);
  b->len += len;
  return 0;
}

// Parser callback for the end of a message; the server needs nothing at that
// point.
static int http1_on_message_complete(chttp1_parser *p) {
  (void)p;
  return 0;
}

// The parser callbacks every server connection shares, filled once by
// init_parser_settings through the once flag.
static struct {
  chttp1_settings settings;
  ccol__once_flag once;
} srv_parser_bundler = {0};

// Fills srv_parser_bundler.settings with this file's callbacks; it runs once,
// before the first connection parses a request. on_trailer stays NULL on
// purpose, so no trailer field can reach the request's header array (see
// http1_on_header).
static void init_parser_settings(void) {
  chttp1_settings_init(&srv_parser_bundler.settings);
  srv_parser_bundler.settings.on_request_line = http1_on_request_line;
  srv_parser_bundler.settings.on_header = http1_on_header;
  srv_parser_bundler.settings.on_headers_complete = http1_on_headers_complete;
  srv_parser_bundler.settings.on_body = http1_on_body;
  srv_parser_bundler.settings.on_message_complete = http1_on_message_complete;
}

// The pools of a server, so srv_submit can find the current pool of the same
// kind again after a restart.
typedef enum {
  SRV_POOL_WORKER = 0,
  SRV_POOL_REJECT = 1,
  SRV_POOL_STREAM = 2
} srv_pool_kind;

static cthrpool stream_pool_get(struct chttpserver *srv);

// Starts a new run of the streaming pool after the caller (holding srv->mutex)
// has taken the old pool out of service. It resets the token counts and bumps
// stream_gen, which tells every task of the earlier run to stop taking
// requests. Both change under srv->wait_mutex, which every reader of sq_tokens
// holds.
static void stream_new_run_locked(struct chttpserver *srv) {
  ccol__mutex_lock(srv->wait_mutex);
  srv->sq_tokens = 0;
  srv->sq_unclaimed = 0;
  atomic_fetch_add(&srv->stream_gen, 1);
  ccol__mutex_unlock(srv->wait_mutex);
}

// Returns the current pool of kind k of srv, read under srv->mutex. The
// streaming pool is created on demand, as for any streaming request.
// srv_submit_retry uses it to find the pool that replaced a retired one.
static cthrpool srv_current_pool(struct chttpserver *srv, srv_pool_kind k) {
  if (k == SRV_POOL_STREAM) return stream_pool_get(srv);
  ccol__mutex_lock(srv->mutex);
  cthrpool p = k == SRV_POOL_WORKER ? srv->worker_pool : srv->reject_pool;
  ccol__mutex_unlock(srv->mutex);
  return p;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Holds the nth worker submit of conn_start_diverted, after it read
// the pool and before it submits, so a test can restart the server inside that
// window. retried counts the submits that srv_submit_retry moved to a newer
// pool.
static struct {
  _Atomic int countdown;
  _Atomic bool entered;
  _Atomic bool go;
  _Atomic size_t retried;
} g_divert_submit_hook;
// Test controls of g_divert_submit_hook: Arm it for the nth next submit, read
// the retry count, ask whether a submit is held, and let it go.
void chttpsvr__arm_divert_submit_hook_for_tests(int nth) {
  atomic_store(&g_divert_submit_hook.entered, false);
  atomic_store(&g_divert_submit_hook.go, false);
  atomic_store(&g_divert_submit_hook.countdown, nth);
}
size_t chttpsvr__submit_retried_for_tests(void) {
  return atomic_load(&g_divert_submit_hook.retried);
}
bool chttpsvr__divert_submit_hook_entered_for_tests(void) {
  return atomic_load(&g_divert_submit_hook.entered);
}
void chttpsvr__release_divert_submit_hook_for_tests(void) {
  atomic_store(&g_divert_submit_hook.go, true);
}
// Counts down the armed submit and holds the one it lands on until the test
// releases it, for at most about 20 seconds.
static void divert_submit_hook_wait_if_armed(void) {
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
#endif

// The cold half of srv_submit, for a task the first pool refused. If a restart
// replaced that pool after the caller read it, the refusal came from a retired,
// draining pool, so the task goes to the current pool of the same kind and a
// request that arrives during a restart isn't refused because of it. Returns
// true once a pool took the task, and false when the current pool itself
// refuses (a full queue) or there is none.
static __attribute__((noinline, cold)) bool srv_submit_retry(
    struct chttpserver *srv, srv_pool_kind k, cthrpool pool, void (*fn)(void *),
    void *arg) {
  for (;;) {
    cthrpool cur = srv_current_pool(srv, k);
    if (!cur || cur == pool) return false;
    pool = cur;
    if (cthrpool_try_submit(pool, fn, arg, NULL) == ccol_success) {
#ifdef RUNNING_UNIT_TESTS
      atomic_fetch_add(&g_divert_submit_hook.retried, 1);
#endif
      return true;
    }
  }
}

// Submits fn(arg) to pool, which the caller read from srv under srv->mutex,
// without blocking. If pool refuses, it retries on the current pool of kind k
// (see srv_submit_retry). Returns whether some pool took the task.
static inline __attribute__((always_inline)) bool srv_submit(
    struct chttpserver *srv, srv_pool_kind k, cthrpool pool, void (*fn)(void *),
    void *arg) {
  if (__builtin_expect(
          pool && cthrpool_try_submit(pool, fn, arg, NULL) == ccol_success, 1))
    return true;
  return srv_submit_retry(srv, k, pool, fn, arg);
}

// Decrements srv->in_flight_requests under srv->mutex and wakes every thread in
// drain_and_close_all_connections when it reaches zero. Every increment
// (conn_start_diverted, conn_dispatch_reject) needs exactly one call. Call it
// only once the connection is closed through conn_close or published back to
// the idle list; any earlier and the drain can wake, see zero and let
// chttpsvr__destroy free srv while this thread is finishing the connection.
static void release_in_flight(struct chttpserver *srv) {
  ccol__mutex_lock(srv->mutex);
  if (--srv->in_flight_requests == 0)
    ccol__cond_var_broadcast(srv->requests_done_cv);
  ccol__mutex_unlock(srv->mutex);
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: While it's set, every pause in conn_pause_reg takes the
// failed-pause path, and the counter counts the removals that path makes.
static _Atomic bool g_force_pause_fail_for_tests = false;
static _Atomic size_t g_pause_fallback_count_for_tests = 0;
// Test controls: Turn the forced pause failure on or off, and read how many
// removals it caused.
void chttpsvr__force_pause_fail_for_tests(bool force) {
  atomic_store(&g_force_pause_fail_for_tests, force);
}
size_t chttpsvr__pause_fallback_count_for_tests(void) {
  return atomic_load(&g_pause_fallback_count_for_tests);
}
#endif

// Pauses the reactor registration of conn while a thread owns the connection,
// so no readiness fires meanwhile; the caller owns conn. If the pause fails it
// removes the registration and clears conn->reg, and the connection's next wait
// (conn_park_arm or the keep-alive tail of task_tail) adds a registration of
// its own. Removing a registration that's already gone does nothing, and
// removing a live one reclaims it.
static void conn_pause_reg(chttpsvr_conn *conn) {
  if (!conn->reg) return;
  bool paused;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_pause_fail_for_tests))
    paused = false;
  else
#endif
    paused =
        ceventloop_pause(srv_engine_bundler.reactor, conn->reg) == ccol_success;
  if (!paused) {
    ceventloop_remove(srv_engine_bundler.reactor, conn->reg);
    conn->reg = CEVENTLOOP_REG_NULL;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_pause_fallback_count_for_tests, 1);
#endif
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: When set, the next rejection skips reject_pool and takes the
// inline answer of conn_reject_via_pool, as when that pool is full. It clears
// itself after one use.
static _Atomic bool g_force_reject_inline_for_tests = false;
// Test control: Arms or disarms the forced inline rejection.
void chttpsvr__force_reject_inline_for_tests(bool force) {
  atomic_store(&g_force_reject_inline_for_tests, force);
}
#endif

// Hands conn to reject_pool so its rejection response (status already in
// conn->reject_status) is written there, with the middleware, and never on the
// calling thread. Both rejection paths use it: The reactor's 404, 405 and 500
// through conn_dispatch_reject, and the 503 or 500 of conn_start_diverted. The
// caller must already have incremented srv->in_flight_requests.
// When reject_pool is gone (teardown), its bounded queue is full or the submit
// fails, it answers on the calling thread without middleware; that write never
// waits on the peer, so it's safe on the reactor and the sweep. Only then does
// it call release_in_flight, after the close; srv is read first because
// conn_reject_and_close may free or park conn.
static void conn_reject_via_pool(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->mutex);
  cthrpool reject_pool = srv->reject_pool;
  ccol__mutex_unlock(srv->mutex);

  bool inline_only = false;
#ifdef RUNNING_UNIT_TESTS
  inline_only = atomic_exchange(&g_force_reject_inline_for_tests, false);
#endif
  if (!inline_only &&
      srv_submit(srv, SRV_POOL_REJECT, reject_pool, reject_task, conn)) {
    return;
  }

  conn_reject_and_close(conn, false);
  release_in_flight(srv);
}

// Sends a connection rejected at headers-complete on the reactor thread (404,
// 405, 500 and the like) to reject_pool, so a slow-reading client can't stall
// the reactor with its rejection. It pauses the registration rather than
// removing it, because a parked response or a lingering close re-arms the same
// registration with a modify and a resume, which allocate nothing. It marks
// conn CONN_ST_DIVERTED, puts it on the diverted list, so a shutdown or restart
// can force it to unblock, and counts it in srv->in_flight_requests under
// srv->mutex. conn_free takes it off the list again on every close path.
static void conn_dispatch_reject(chttpsvr_conn *conn) {
  idle_list_remove(conn);
  conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  diverted_list_add(conn);
  ccol__mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  ccol__mutex_unlock(conn->srv->mutex);
  conn_reject_via_pool(conn);
}

// Hands a request whose headers are parsed to a worker. The reactor calls it
// with the bytes it read past the headers (leftover, in its stack buffer). It
// pauses the registration (cheaper than a remove and a re-add on every
// keep-alive request), marks conn diverted, puts it on the diverted list and
// counts it in srv->in_flight_requests under srv->mutex, where it also reads
// the worker pool and the draining flag.
// leftover is copied into conn->carry_over before the submit, because the
// buffer dies when the callback returns and a worker may start task_worker at
// once; writing the fields after the submit would race it and lose or leak the
// bytes. carry_over_len is 0 whenever carry_over is NULL. A draining server
// answers 503, a failed copy answers 500, and a full worker pool answers 503,
// all through conn_reject_via_pool. A request with a divert gate goes to
// conn_divert_gate first (streaming pool or body memory reservation).
static void conn_start_diverted(chttpsvr_conn *conn, const char *leftover,
                                size_t leftover_len) {
  idle_list_remove(conn);
  conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  diverted_list_add(conn);

  ccol__mutex_lock(conn->srv->mutex);
  conn->srv->in_flight_requests++;
  cthrpool pool = conn->srv->worker_pool;
  bool draining = srv_draining(conn->srv);
  ccol__mutex_unlock(conn->srv->mutex);

  if (__builtin_expect(draining, 0)) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    conn_reject_via_pool(conn);
    return;
  }

  char *carry = NULL;
  bool carry_alloc_failed = false;
  if (leftover_len > 0) {
    carry = (char *)ccol__mem_alloc(conn->m_procs, leftover_len);
    if (carry)
      memcpy(carry, leftover, leftover_len);
    else
      carry_alloc_failed = true;
  }
  conn->carry_over = carry;
  conn->carry_over_len = carry ? leftover_len : 0;

  if (carry_alloc_failed) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_INTERNAL_ERROR;
    conn_reject_via_pool(conn);
    return;
  }

  if (__builtin_expect(conn->divert_gate > leftover_len, 0) &&
      conn_divert_gate(conn))
    return;

#ifdef RUNNING_UNIT_TESTS
  divert_submit_hook_wait_if_armed();
#endif
  if (!srv_submit(conn->srv, SRV_POOL_WORKER, pool, task_worker, conn)) {
    ccol__mem_free(conn->m_procs, carry);
    conn->carry_over = NULL;
    conn->carry_over_len = 0;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;

    conn_reject_via_pool(conn);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&chttpsvr__worker_submits_for_tests, 1);
#endif
}

// Fills conn->decoded_path for a request rejected before routing set it, so a
// middleware running for the rejection can read the target through
// chttpsvr_req_path. A target with malformed percent-encoding is stored as it
// came in, since that's often why no route matched and an access log should
// show it. An allocation failure leaves the field NULL.
static void conn_set_rejected_decoded_path(chttpsvr_conn *conn) {
  if (conn->decoded_path || !conn->path) return;
  size_t plen = strlen(conn->path);
  char *dp = (char *)ccol__mem_alloc(conn->m_procs, plen + 1);
  if (!dp) return;
  ssize_t dlen = decode_path_unsafe(dp, conn->path);
  if (dlen >= 0)
    dp[(size_t)dlen] = '\0';
  else
    memcpy(dp, conn->path, plen + 1);
  conn->decoded_path = dp;
}

// Writes the Allow header value for the method mask into out (at least 64
// bytes), in the order GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS, separated
// by ", ". The 405 response of conn_reject_and_close uses it; the longest value
// is 44 bytes with the terminator.
static void format_allow_header(unsigned mask, char *out) {
  static const chttp_method order[] = {CHTTP_GET,    CHTTP_HEAD,   CHTTP_POST,
                                       CHTTP_PUT,    CHTTP_DELETE, CHTTP_PATCH,
                                       CHTTP_OPTIONS};
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

// Writes the rejection response for conn->reject_status and ends the
// connection. A 503 gets Retry-After and a 405 gets Allow. When run_middleware
// is true (only from reject_task on a reject_pool thread, whose fixed size
// bounds how much application code a flood of rejections runs) and the status
// takes a chain, the snapshot middleware runs with no route and no body,
// provided the decoded path could be allocated; otherwise the response goes out
// bare.
// The write is non-blocking with a duration ceiling. A full socket parks the
// connection as a write park, drops the request state and leaves the diverted
// list; if parking fails the connection closes. After a complete response it
// lingers when request bytes may remain unread, so the kernel's reset can't
// wipe out the response in the client, and closes otherwise.
static void conn_reject_and_close(chttpsvr_conn *conn, bool run_middleware) {
  chttpsvr_resp *resp = &conn->resp;
  resp->status_code = conn->reject_status;
  if (conn->reject_status == CHTTP_STATUS_SERVICE_UNAVAILABLE)
    (void)chttpsvr_resp_set_header(resp, "retry-after",
                                   CHTTPSVR_RETRY_AFTER_SECONDS);
  if (conn->reject_status == CHTTP_STATUS_METHOD_NOT_ALLOWED &&
      conn->allowed_methods) {
    char allow[64];
    format_allow_header(conn->allowed_methods, allow);
    (void)chttpsvr_resp_set_header(resp, "allow", allow);
  }
  bool wants_middleware = run_middleware && conn->dispatch.mw_count > 0 &&
                          reject_status_runs_middleware(conn->reject_status);
  if (wants_middleware) conn_set_rejected_decoded_path(conn);
  if (wants_middleware && conn->decoded_path) {
    conn->dispatch.route = NULL;
    conn->dispatch.mw_idx = 0;
    chttpsvr_req req;
    memset(&req, 0, sizeof(req));
    req.conn = conn;
    req.stream = NULL;
    req.param_names = NULL;
    req.m_procs = conn->m_procs;
    chttpsvr_next(&req, resp);
    destroy_req_scratch(&req);
  }
  chttp1_stream stream;
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                              conn->m_procs);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  stream.fd_nonblocking = true;
  stream.write_nonblocking = true;
  bool sent = send_response(&stream, resp, false, 0, conn->method == CHTTP_HEAD,
                            conn, true);
  chttp1_stream_release(&stream);
  if (!sent && conn->wp_active) {
    conn->wp_reject = true;
    conn_drop_request_state(conn);
    diverted_list_remove(conn);
    if (!conn_park_arm(conn, CONN_PARK_WRITE)) {
      conn->wp_reject = false;
      conn_end_parked_write(conn);
      conn_close(conn);
    }
    return;
  }
  if (sent && conn_request_bytes_may_remain(conn))
    conn_linger(conn);
  else
    conn_close(conn);
}

#ifdef RUNNING_UNIT_TESTS
// Test counter of rejections that ran on a reject_pool thread (the inline
// fallback adds nothing). It's process-wide, so a test reads the difference
// across its own window.
static _Atomic size_t g_reject_task_run_count_for_tests = 0;

// Test hook: Makes the first stream prepare of task_worker count as failed, so
// a test reaches the body_unavailable fallback that a real allocation failure
// there takes. A sized allocator failure can't reach it, because
// conn_start_diverted makes an allocation of the same size first.
static _Atomic bool g_force_stream_prepare_fail_for_tests = false;
// Test control: Arms or disarms the forced stream prepare failure.
void chttpsvr__force_stream_prepare_fail_for_tests(bool force) {
  atomic_store(&g_force_stream_prepare_fail_for_tests, force);
}

// Test hook: Once armed, chttpsvr_start blocks right after it has the engine
// reference and registered with servers_bundler, before any more reactor work,
// so a test can land a concurrent chttpsvr_engine_stop in that exact window. It
// fires once per arm. This and the other parking hooks below share one shape:
// armed, entered and go under a mutex and a condition variable, set up lazily
// through the once flag.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_start_race_hook = {0};

// Initialises the mutex and the condition variable of g_start_race_hook; a
// failure is fatal.
static void start_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_start_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_start_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_start_race_hook: Arm it, wait until a caller is parked in
// it, and release the caller.
void chttpsvr__arm_start_race_hook_for_tests(void) {
  ccol__call_once(g_start_race_hook.once, start_race_hook_init_globals);
  ccol__mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.armed = true;
  g_start_race_hook.entered = false;
  g_start_race_hook.go = false;
  ccol__mutex_unlock(g_start_race_hook.mutex);
}

void chttpsvr__wait_start_race_hook_entered_for_tests(void) {
  ccol__call_once(g_start_race_hook.once, start_race_hook_init_globals);
  ccol__mutex_lock(g_start_race_hook.mutex);
  while (!g_start_race_hook.entered)
    ccol__cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  ccol__mutex_unlock(g_start_race_hook.mutex);
}

void chttpsvr__release_start_race_hook_for_tests(void) {
  ccol__call_once(g_start_race_hook.once, start_race_hook_init_globals);
  ccol__mutex_lock(g_start_race_hook.mutex);
  g_start_race_hook.go = true;
  ccol__cond_var_broadcast(g_start_race_hook.cv);
  ccol__mutex_unlock(g_start_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. chttpsvr_start calls it, so a test can run its race in a
// fixed order.
static void start_race_hook_wait_if_armed(void) {
  ccol__call_once(g_start_race_hook.once, start_race_hook_init_globals);
  ccol__mutex_lock(g_start_race_hook.mutex);
  if (!g_start_race_hook.armed) {
    ccol__mutex_unlock(g_start_race_hook.mutex);
    return;
  }
  g_start_race_hook.armed = false;
  g_start_race_hook.entered = true;
  ccol__cond_var_broadcast(g_start_race_hook.cv);
  while (!g_start_race_hook.go)
    ccol__cond_var_wait(g_start_race_hook.cv, g_start_race_hook.mutex);
  ccol__mutex_unlock(g_start_race_hook.mutex);
}

// Test hook: Once armed, chttpsvr_start blocks after it set
// contributed_to_engine and before engine_acquire, the window where
// engine_acquire can see the engine stopping and return without waiting. A test
// uses it to exercise the retry of the "currently stopping" branch every time.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_engine_stopping_race_hook = {0};

// Initialises the mutex and the condition variable of
// g_engine_stopping_race_hook; a failure is fatal.
static void engine_stopping_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_engine_stopping_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_engine_stopping_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_engine_stopping_race_hook: Arm it, wait until a caller is
// parked in it, and release the caller.
void chttpsvr__arm_engine_stopping_race_hook_for_tests(void) {
  ccol__call_once(g_engine_stopping_race_hook.once,
                  engine_stopping_race_hook_init_globals);
  ccol__mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.armed = true;
  g_engine_stopping_race_hook.entered = false;
  g_engine_stopping_race_hook.go = false;
  ccol__mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void chttpsvr__wait_engine_stopping_race_hook_entered_for_tests(void) {
  ccol__call_once(g_engine_stopping_race_hook.once,
                  engine_stopping_race_hook_init_globals);
  ccol__mutex_lock(g_engine_stopping_race_hook.mutex);
  while (!g_engine_stopping_race_hook.entered)
    ccol__cond_var_wait(g_engine_stopping_race_hook.cv,
                        g_engine_stopping_race_hook.mutex);
  ccol__mutex_unlock(g_engine_stopping_race_hook.mutex);
}

void chttpsvr__release_engine_stopping_race_hook_for_tests(void) {
  ccol__call_once(g_engine_stopping_race_hook.once,
                  engine_stopping_race_hook_init_globals);
  ccol__mutex_lock(g_engine_stopping_race_hook.mutex);
  g_engine_stopping_race_hook.go = true;
  ccol__cond_var_broadcast(g_engine_stopping_race_hook.cv);
  ccol__mutex_unlock(g_engine_stopping_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. chttpsvr_start calls it, so a test can run its race in a
// fixed order.
static void engine_stopping_race_hook_wait_if_armed(void) {
  ccol__call_once(g_engine_stopping_race_hook.once,
                  engine_stopping_race_hook_init_globals);
  ccol__mutex_lock(g_engine_stopping_race_hook.mutex);
  if (!g_engine_stopping_race_hook.armed) {
    ccol__mutex_unlock(g_engine_stopping_race_hook.mutex);
    return;
  }
  g_engine_stopping_race_hook.armed = false;
  g_engine_stopping_race_hook.entered = true;
  ccol__cond_var_broadcast(g_engine_stopping_race_hook.cv);
  while (!g_engine_stopping_race_hook.go)
    ccol__cond_var_wait(g_engine_stopping_race_hook.cv,
                        g_engine_stopping_race_hook.mutex);
  ccol__mutex_unlock(g_engine_stopping_race_hook.mutex);
}

// Test hook: Once armed, chttpsvr_start blocks right after chttpsvr_resolve
// succeeded, so its resolve pin is live, and before any other check. A test
// uses it to place that pin ahead of the pending_resolve_count wait of a
// concurrent quiesce_server_once, which would deadlock if chttpsvr_start then
// waited for the quiesce while holding the pin.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_start_resolve_race_hook = {0};

// Initialises the mutex and the condition variable of
// g_start_resolve_race_hook; a failure is fatal.
static void start_resolve_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_start_resolve_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_start_resolve_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_start_resolve_race_hook: Arm it, wait until a caller is
// parked in it, and release the caller.
void chttpsvr__arm_start_resolve_race_hook_for_tests(void) {
  ccol__call_once(g_start_resolve_race_hook.once,
                  start_resolve_race_hook_init_globals);
  ccol__mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.armed = true;
  g_start_resolve_race_hook.entered = false;
  g_start_resolve_race_hook.go = false;
  ccol__mutex_unlock(g_start_resolve_race_hook.mutex);
}

void chttpsvr__wait_start_resolve_race_hook_entered_for_tests(void) {
  ccol__call_once(g_start_resolve_race_hook.once,
                  start_resolve_race_hook_init_globals);
  ccol__mutex_lock(g_start_resolve_race_hook.mutex);
  while (!g_start_resolve_race_hook.entered)
    ccol__cond_var_wait(g_start_resolve_race_hook.cv,
                        g_start_resolve_race_hook.mutex);
  ccol__mutex_unlock(g_start_resolve_race_hook.mutex);
}

void chttpsvr__release_start_resolve_race_hook_for_tests(void) {
  ccol__call_once(g_start_resolve_race_hook.once,
                  start_resolve_race_hook_init_globals);
  ccol__mutex_lock(g_start_resolve_race_hook.mutex);
  g_start_resolve_race_hook.go = true;
  ccol__cond_var_broadcast(g_start_resolve_race_hook.cv);
  ccol__mutex_unlock(g_start_resolve_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. chttpsvr_start calls it, so a test can run its race in a
// fixed order.
static void start_resolve_race_hook_wait_if_armed(void) {
  ccol__call_once(g_start_resolve_race_hook.once,
                  start_resolve_race_hook_init_globals);
  ccol__mutex_lock(g_start_resolve_race_hook.mutex);
  if (!g_start_resolve_race_hook.armed) {
    ccol__mutex_unlock(g_start_resolve_race_hook.mutex);
    return;
  }
  g_start_resolve_race_hook.armed = false;
  g_start_resolve_race_hook.entered = true;
  ccol__cond_var_broadcast(g_start_resolve_race_hook.cv);
  while (!g_start_resolve_race_hook.go)
    ccol__cond_var_wait(g_start_resolve_race_hook.cv,
                        g_start_resolve_race_hook.mutex);
  ccol__mutex_unlock(g_start_resolve_race_hook.mutex);
}

// Test hook: Once armed, chttpsvr_stop_internal blocks after it entered
// CHTTPSVR_LC_STOPPING and unlocked raw->mutex, and before it removes and
// closes the old listener, so a test can land a concurrent chttpsvr_start in
// the window that state guards.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_stop_race_hook = {0};

// Initialises the mutex and the condition variable of g_stop_race_hook; a
// failure is fatal.
static void stop_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_stop_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_stop_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_stop_race_hook: Arm it, wait until a caller is parked in
// it, and release the caller.
void chttpsvr__arm_stop_race_hook_for_tests(void) {
  ccol__call_once(g_stop_race_hook.once, stop_race_hook_init_globals);
  ccol__mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.armed = true;
  g_stop_race_hook.entered = false;
  g_stop_race_hook.go = false;
  ccol__mutex_unlock(g_stop_race_hook.mutex);
}

void chttpsvr__wait_stop_race_hook_entered_for_tests(void) {
  ccol__call_once(g_stop_race_hook.once, stop_race_hook_init_globals);
  ccol__mutex_lock(g_stop_race_hook.mutex);
  while (!g_stop_race_hook.entered)
    ccol__cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  ccol__mutex_unlock(g_stop_race_hook.mutex);
}

void chttpsvr__release_stop_race_hook_for_tests(void) {
  ccol__call_once(g_stop_race_hook.once, stop_race_hook_init_globals);
  ccol__mutex_lock(g_stop_race_hook.mutex);
  g_stop_race_hook.go = true;
  ccol__cond_var_broadcast(g_stop_race_hook.cv);
  ccol__mutex_unlock(g_stop_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. chttpsvr_stop_internal calls it, so a test can run its race
// in a fixed order.
static void stop_race_hook_wait_if_armed(void) {
  ccol__call_once(g_stop_race_hook.once, stop_race_hook_init_globals);
  ccol__mutex_lock(g_stop_race_hook.mutex);
  if (!g_stop_race_hook.armed) {
    ccol__mutex_unlock(g_stop_race_hook.mutex);
    return;
  }
  g_stop_race_hook.armed = false;
  g_stop_race_hook.entered = true;
  ccol__cond_var_broadcast(g_stop_race_hook.cv);
  while (!g_stop_race_hook.go)
    ccol__cond_var_wait(g_stop_race_hook.cv, g_stop_race_hook.mutex);
  ccol__mutex_unlock(g_stop_race_hook.mutex);
}

// Test signal for the CHTTPSVR_LC_STOPPING wait of chttpsvr_start: It tells a
// test that the call reached its wait, and never blocks. A parking hook would
// be unsafe there, because the caller holds raw->mutex, which
// chttpsvr_stop_internal needs to leave that state, so a slow release would
// wedge both sides of the race under test.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
} g_start_stopping_wait_signal = {0};

// Initialises the mutex and the condition variable of
// g_start_stopping_wait_signal; a failure is fatal.
static void start_stopping_wait_signal_init_globals(void) {
  if (ccol__mutex_init(g_start_stopping_wait_signal.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_start_stopping_wait_signal.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_start_stopping_wait_signal: Arm it, and wait until
// chttpsvr_start has fired it.
void chttpsvr__arm_start_stopping_wait_signal_for_tests(void) {
  ccol__call_once(g_start_stopping_wait_signal.once,
                  start_stopping_wait_signal_init_globals);
  ccol__mutex_lock(g_start_stopping_wait_signal.mutex);
  g_start_stopping_wait_signal.armed = true;
  g_start_stopping_wait_signal.entered = false;
  ccol__mutex_unlock(g_start_stopping_wait_signal.mutex);
}

void chttpsvr__wait_start_stopping_wait_signal_entered_for_tests(void) {
  ccol__call_once(g_start_stopping_wait_signal.once,
                  start_stopping_wait_signal_init_globals);
  ccol__mutex_lock(g_start_stopping_wait_signal.mutex);
  while (!g_start_stopping_wait_signal.entered)
    ccol__cond_var_wait(g_start_stopping_wait_signal.cv,
                        g_start_stopping_wait_signal.mutex);
  ccol__mutex_unlock(g_start_stopping_wait_signal.mutex);
}

// Fires the test signal of this point when it is armed. It disarms the signal,
// marks it entered and wakes the waiting test, without blocking. chttpsvr_start
// calls it, so a test knows the thread got there.
static void start_stopping_wait_signal_fire_if_armed(void) {
  ccol__call_once(g_start_stopping_wait_signal.once,
                  start_stopping_wait_signal_init_globals);
  ccol__mutex_lock(g_start_stopping_wait_signal.mutex);
  if (g_start_stopping_wait_signal.armed) {
    g_start_stopping_wait_signal.armed = false;
    g_start_stopping_wait_signal.entered = true;
    ccol__cond_var_broadcast(g_start_stopping_wait_signal.cv);
  }
  ccol__mutex_unlock(g_start_stopping_wait_signal.mutex);
}

// Test hook: Once armed, engine_force_stop_quiesce_all blocks after it pinned
// the first registered server through servers_bundler_pins and unlocked
// servers_bundler.mutex, and before quiesce_server_once. A test lands a
// concurrent chttpsvr_destroy on that server there, where only the pin keeps
// the server alive.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_reaper_race_hook = {0};

// Initialises the mutex and the condition variable of g_reaper_race_hook; a
// failure is fatal.
static void reaper_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_reaper_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_reaper_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_reaper_race_hook: Arm it, wait until a caller is parked in
// it, and release the caller.
void chttpsvr__arm_reaper_race_hook_for_tests(void) {
  ccol__call_once(g_reaper_race_hook.once, reaper_race_hook_init_globals);
  ccol__mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.armed = true;
  g_reaper_race_hook.entered = false;
  g_reaper_race_hook.go = false;
  ccol__mutex_unlock(g_reaper_race_hook.mutex);
}

void chttpsvr__wait_reaper_race_hook_entered_for_tests(void) {
  ccol__call_once(g_reaper_race_hook.once, reaper_race_hook_init_globals);
  ccol__mutex_lock(g_reaper_race_hook.mutex);
  while (!g_reaper_race_hook.entered)
    ccol__cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  ccol__mutex_unlock(g_reaper_race_hook.mutex);
}

void chttpsvr__release_reaper_race_hook_for_tests(void) {
  ccol__call_once(g_reaper_race_hook.once, reaper_race_hook_init_globals);
  ccol__mutex_lock(g_reaper_race_hook.mutex);
  g_reaper_race_hook.go = true;
  ccol__cond_var_broadcast(g_reaper_race_hook.cv);
  ccol__mutex_unlock(g_reaper_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. engine_force_stop_quiesce_all calls it, so a test can run
// its race in a fixed order.
static void reaper_race_hook_wait_if_armed(void) {
  ccol__call_once(g_reaper_race_hook.once, reaper_race_hook_init_globals);
  ccol__mutex_lock(g_reaper_race_hook.mutex);
  if (!g_reaper_race_hook.armed) {
    ccol__mutex_unlock(g_reaper_race_hook.mutex);
    return;
  }
  g_reaper_race_hook.armed = false;
  g_reaper_race_hook.entered = true;
  ccol__cond_var_broadcast(g_reaper_race_hook.cv);
  while (!g_reaper_race_hook.go)
    ccol__cond_var_wait(g_reaper_race_hook.cv, g_reaper_race_hook.mutex);
  ccol__mutex_unlock(g_reaper_race_hook.mutex);
}

// Test hook: Once armed, quiesce_server_once blocks after it won the quiesce
// (CHTTPSVR_QS_QUIESCING, pins drained, srv->mutex unlocked) and before any
// teardown work. A test forks there to exercise the child-side fixup of
// chttpsvr_atfork_release_impl for a server in that state.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_quiesce_teardown_race_hook = {0};

// Initialises the mutex and the condition variable of
// g_quiesce_teardown_race_hook; a failure is fatal.
static void quiesce_teardown_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_quiesce_teardown_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_quiesce_teardown_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test controls of g_quiesce_teardown_race_hook: Arm it, wait until a caller is
// parked in it, and release the caller.
void chttpsvr__arm_quiesce_teardown_race_hook_for_tests(void) {
  ccol__call_once(g_quiesce_teardown_race_hook.once,
                  quiesce_teardown_race_hook_init_globals);
  ccol__mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.armed = true;
  g_quiesce_teardown_race_hook.entered = false;
  g_quiesce_teardown_race_hook.go = false;
  ccol__mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void chttpsvr__wait_quiesce_teardown_race_hook_entered_for_tests(void) {
  ccol__call_once(g_quiesce_teardown_race_hook.once,
                  quiesce_teardown_race_hook_init_globals);
  ccol__mutex_lock(g_quiesce_teardown_race_hook.mutex);
  while (!g_quiesce_teardown_race_hook.entered)
    ccol__cond_var_wait(g_quiesce_teardown_race_hook.cv,
                        g_quiesce_teardown_race_hook.mutex);
  ccol__mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

void chttpsvr__release_quiesce_teardown_race_hook_for_tests(void) {
  ccol__call_once(g_quiesce_teardown_race_hook.once,
                  quiesce_teardown_race_hook_init_globals);
  ccol__mutex_lock(g_quiesce_teardown_race_hook.mutex);
  g_quiesce_teardown_race_hook.go = true;
  ccol__cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  ccol__mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

// Stops the calling thread at this race point for a test when the hook is
// armed. It disarms the hook, signals that it got there and waits until the
// test releases it. quiesce_server_once calls it, so a test can run its race in
// a fixed order.
static void quiesce_teardown_race_hook_wait_if_armed(void) {
  ccol__call_once(g_quiesce_teardown_race_hook.once,
                  quiesce_teardown_race_hook_init_globals);
  ccol__mutex_lock(g_quiesce_teardown_race_hook.mutex);
  if (!g_quiesce_teardown_race_hook.armed) {
    ccol__mutex_unlock(g_quiesce_teardown_race_hook.mutex);
    return;
  }
  g_quiesce_teardown_race_hook.armed = false;
  g_quiesce_teardown_race_hook.entered = true;
  ccol__cond_var_broadcast(g_quiesce_teardown_race_hook.cv);
  while (!g_quiesce_teardown_race_hook.go)
    ccol__cond_var_wait(g_quiesce_teardown_race_hook.cv,
                        g_quiesce_teardown_race_hook.mutex);
  ccol__mutex_unlock(g_quiesce_teardown_race_hook.mutex);
}

static void timespec_add_ms(struct timespec *ts, unsigned ms);

// Test hook: Once armed, it pauses the CHTTPSVR_QS_QUIESCING backoff of
// chttpsvr_start after it joined quiesce_waiters and released its resolve pin,
// and before it takes raw->mutex again. A test runs a quiesce and a destroy to
// the end around it to show raw survives; if quiesce_waiters were raised after
// the pin release, the quiesce could free raw in that window.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
  bool go;
} g_start_quiescing_unpinned_race_hook = {0};

// Initialises the mutex and the condition variable of
// g_start_quiescing_unpinned_race_hook; a failure is fatal. The condition
// variable uses CLOCK_MONOTONIC, because the bounded wait of the test computes
// its deadline on that clock.
static void start_quiescing_unpinned_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_start_quiescing_unpinned_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  ccol__cond_var_attr cv_attr;
  int cv_rc;
  if (ccol__cond_var_attr_init(cv_attr) == 0) {
    ccol__cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc = ccol__cond_var_init_ca(g_start_quiescing_unpinned_race_hook.cv,
                                   cv_attr);
    ccol__cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = ccol__cond_var_init(g_start_quiescing_unpinned_race_hook.cv);
  }
  if (cv_rc != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test control: Arms g_start_quiescing_unpinned_race_hook for the next
// chttpsvr_start that takes the backoff.
void chttpsvr__arm_start_quiescing_unpinned_race_hook_for_tests(void) {
  ccol__call_once(g_start_quiescing_unpinned_race_hook.once,
                  start_quiescing_unpinned_race_hook_init_globals);
  ccol__mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.armed = true;
  g_start_quiescing_unpinned_race_hook.entered = false;
  g_start_quiescing_unpinned_race_hook.go = false;
  ccol__mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

// Test control of g_start_quiescing_unpinned_race_hook: Waits until a paused
// chttpsvr_start() has reached the hook. Returns true once it has, and false
// after 10 seconds, so a regression that never reaches the hook fails the test
// instead of hanging the whole binary (the call runs on the main test thread).
// The test reaches the hook only after a reaper has claimed the server, which
// it learns from chttpsvr__wait_quiesce_claimed_signal_entered_for_tests.
bool chttpsvr__wait_start_quiescing_unpinned_race_hook_entered_for_tests(void) {
  ccol__call_once(g_start_quiescing_unpinned_race_hook.once,
                  start_quiescing_unpinned_race_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  timespec_add_ms(&deadline, 10000);
  ccol__mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  bool timed_out = false;
  while (!g_start_quiescing_unpinned_race_hook.entered && !timed_out) {
    timed_out =
        ccol__cond_var_timedwait(g_start_quiescing_unpinned_race_hook.cv,
                                 g_start_quiescing_unpinned_race_hook.mutex,
                                 deadline) == ETIMEDOUT;
  }
  bool entered = g_start_quiescing_unpinned_race_hook.entered;
  ccol__mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
  return entered;
}

// Test control of g_start_quiescing_unpinned_race_hook: Lets the paused
// chttpsvr_start() go on.
void chttpsvr__release_start_quiescing_unpinned_race_hook_for_tests(void) {
  ccol__call_once(g_start_quiescing_unpinned_race_hook.once,
                  start_quiescing_unpinned_race_hook_init_globals);
  ccol__mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  g_start_quiescing_unpinned_race_hook.go = true;
  ccol__cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  ccol__mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

// Holds chttpsvr_start() at the hook when a test armed it. It fires once, tells
// the waiting test it got there, and waits under the hook mutex until the test
// releases it.
static void start_quiescing_unpinned_race_hook_wait_if_armed(void) {
  ccol__call_once(g_start_quiescing_unpinned_race_hook.once,
                  start_quiescing_unpinned_race_hook_init_globals);
  ccol__mutex_lock(g_start_quiescing_unpinned_race_hook.mutex);
  if (!g_start_quiescing_unpinned_race_hook.armed) {
    ccol__mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
    return;
  }
  g_start_quiescing_unpinned_race_hook.armed = false;
  g_start_quiescing_unpinned_race_hook.entered = true;
  ccol__cond_var_broadcast(g_start_quiescing_unpinned_race_hook.cv);
  while (!g_start_quiescing_unpinned_race_hook.go)
    ccol__cond_var_wait(g_start_quiescing_unpinned_race_hook.cv,
                        g_start_quiescing_unpinned_race_hook.mutex);
  ccol__mutex_unlock(g_start_quiescing_unpinned_race_hook.mutex);
}

// Test hook: Once armed, the next quiesce_server_once() pass that claims a
// server signals right after it sets quiesce_state to CHTTPSVR_QS_QUIESCING. It
// only signals and never blocks. It fires while the pass holds srv->mutex, so
// its mutex is a leaf lock. A test that pauses a chttpsvr_start() with its pin
// held uses it to know the reaper claimed the server before it releases the
// paused call.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool entered;
} g_quiesce_claimed_signal = {0};

// Creates the mutex and condition variable of g_quiesce_claimed_signal. The
// condition variable uses CLOCK_MONOTONIC, because the bounded wait computes
// its deadline on that clock.
static void quiesce_claimed_signal_init_globals(void) {
  if (ccol__mutex_init(g_quiesce_claimed_signal.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  ccol__cond_var_attr cv_attr;
  int cv_rc;
  if (ccol__cond_var_attr_init(cv_attr) == 0) {
    ccol__cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
    cv_rc = ccol__cond_var_init_ca(g_quiesce_claimed_signal.cv, cv_attr);
    ccol__cond_var_attr_destroy(cv_attr);
  } else {
    cv_rc = ccol__cond_var_init(g_quiesce_claimed_signal.cv);
  }
  if (cv_rc != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Test control: Arms g_quiesce_claimed_signal for the next pass that claims a
// server.
void chttpsvr__arm_quiesce_claimed_signal_for_tests(void) {
  ccol__call_once(g_quiesce_claimed_signal.once,
                  quiesce_claimed_signal_init_globals);
  ccol__mutex_lock(g_quiesce_claimed_signal.mutex);
  g_quiesce_claimed_signal.armed = true;
  g_quiesce_claimed_signal.entered = false;
  ccol__mutex_unlock(g_quiesce_claimed_signal.mutex);
}

// Test control: Waits up to 10 seconds for the armed signal to fire and returns
// whether it did. It disarms the hook on the way out, so a missed signal can't
// fire later in an unrelated pass.
bool chttpsvr__wait_quiesce_claimed_signal_entered_for_tests(void) {
  ccol__call_once(g_quiesce_claimed_signal.once,
                  quiesce_claimed_signal_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  timespec_add_ms(&deadline, 10000);
  ccol__mutex_lock(g_quiesce_claimed_signal.mutex);
  bool timed_out = false;
  while (!g_quiesce_claimed_signal.entered && !timed_out) {
    timed_out = ccol__cond_var_timedwait(g_quiesce_claimed_signal.cv,
                                         g_quiesce_claimed_signal.mutex,
                                         deadline) == ETIMEDOUT;
  }
  bool entered = g_quiesce_claimed_signal.entered;
  g_quiesce_claimed_signal.armed = false;
  ccol__mutex_unlock(g_quiesce_claimed_signal.mutex);
  return entered;
}

// Fires g_quiesce_claimed_signal once if a test armed it; called by
// quiesce_server_once() right after it claims a server.
static void quiesce_claimed_signal_fire_if_armed(void) {
  ccol__call_once(g_quiesce_claimed_signal.once,
                  quiesce_claimed_signal_init_globals);
  ccol__mutex_lock(g_quiesce_claimed_signal.mutex);
  if (g_quiesce_claimed_signal.armed) {
    g_quiesce_claimed_signal.armed = false;
    g_quiesce_claimed_signal.entered = true;
    ccol__cond_var_broadcast(g_quiesce_claimed_signal.cv);
  }
  ccol__mutex_unlock(g_quiesce_claimed_signal.mutex);
}

// Test hook that blocks listener_on_readable() at one of two points picked by
// the arm call: Right after it pins srv through listener_dispatch_pins, or at
// its very first statement, before the pin. A test lands a concurrent
// chttpsvr_destroy() while a listener dispatch is really in flight and checks
// whether the destroy waits for the pin. It has the shape of
// g_reaper_race_hook.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  bool armed;
  bool at_entry;
  bool entered;
  bool go;
} g_listener_dispatch_race_hook = {0};

// Creates the mutex and condition variable of g_listener_dispatch_race_hook.
static void listener_dispatch_race_hook_init_globals(void) {
  if (ccol__mutex_init(g_listener_dispatch_race_hook.mutex) != 0)
    ccol__fatal_err("chttpsvr test hook: failed to initialize mutex");
  if (ccol__cond_var_init(g_listener_dispatch_race_hook.cv) != 0)
    ccol__fatal_err(
        "chttpsvr test hook: failed to initialize condition variable");
}

// Arms g_listener_dispatch_race_hook for one dispatch, at the entry point when
// at_entry is true and after the pin otherwise.
static void listener_dispatch_race_hook_arm(bool at_entry) {
  ccol__call_once(g_listener_dispatch_race_hook.once,
                  listener_dispatch_race_hook_init_globals);
  ccol__mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.armed = true;
  g_listener_dispatch_race_hook.at_entry = at_entry;
  g_listener_dispatch_race_hook.entered = false;
  g_listener_dispatch_race_hook.go = false;
  ccol__mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

// Test controls of g_listener_dispatch_race_hook: Arm it after the pin or at
// the entry, wait until a dispatch is held, and release it.
void chttpsvr__arm_listener_dispatch_race_hook_for_tests(void) {
  listener_dispatch_race_hook_arm(false);
}

void chttpsvr__arm_listener_dispatch_entry_race_hook_for_tests(void) {
  listener_dispatch_race_hook_arm(true);
}

void chttpsvr__wait_listener_dispatch_race_hook_entered_for_tests(void) {
  ccol__call_once(g_listener_dispatch_race_hook.once,
                  listener_dispatch_race_hook_init_globals);
  ccol__mutex_lock(g_listener_dispatch_race_hook.mutex);
  while (!g_listener_dispatch_race_hook.entered)
    ccol__cond_var_wait(g_listener_dispatch_race_hook.cv,
                        g_listener_dispatch_race_hook.mutex);
  ccol__mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

void chttpsvr__release_listener_dispatch_race_hook_for_tests(void) {
  ccol__call_once(g_listener_dispatch_race_hook.once,
                  listener_dispatch_race_hook_init_globals);
  ccol__mutex_lock(g_listener_dispatch_race_hook.mutex);
  g_listener_dispatch_race_hook.go = true;
  ccol__cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  ccol__mutex_unlock(g_listener_dispatch_race_hook.mutex);
}

// Holds listener_on_readable() when the hook is armed for this point (at_entry
// tells which one the caller is at). It fires once and waits under the hook
// mutex until the test releases it.
static void listener_dispatch_race_hook_wait_if_armed(bool at_entry) {
  ccol__call_once(g_listener_dispatch_race_hook.once,
                  listener_dispatch_race_hook_init_globals);
  ccol__mutex_lock(g_listener_dispatch_race_hook.mutex);
  if (!g_listener_dispatch_race_hook.armed ||
      g_listener_dispatch_race_hook.at_entry != at_entry) {
    ccol__mutex_unlock(g_listener_dispatch_race_hook.mutex);
    return;
  }
  g_listener_dispatch_race_hook.armed = false;
  g_listener_dispatch_race_hook.entered = true;
  ccol__cond_var_broadcast(g_listener_dispatch_race_hook.cv);
  while (!g_listener_dispatch_race_hook.go)
    ccol__cond_var_wait(g_listener_dispatch_race_hook.cv,
                        g_listener_dispatch_race_hook.mutex);
  ccol__mutex_unlock(g_listener_dispatch_race_hook.mutex);
}
#endif

// Task of reject_pool: Sends a rejection and closes the connection on one of
// that pool's threads, so a slow client can't stall the reactor. It releases
// the request's in_flight_requests slot only after the close completed (see
// release_in_flight). srv is read first, because conn_reject_and_close frees
// conn.
static void reject_task(void *arg) {
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  struct chttpserver *srv = conn->srv;
  chttpsvr_mark_worker_thread(srv);
  conn_reject_and_close(conn, true);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_reject_task_run_count_for_tests, 1);
#endif
  release_in_flight(srv);
}

// Feeds n new bytes into conn->parser and acts on the result. conn_pump calls
// it for bytes it read, and the keep-alive tail of task_worker calls it for the
// bytes of a pipelined request that are already off the wire, so no readiness
// event would bring them back. buf only needs to live for the call.
// It returns true when the parser needs more header bytes, and false when conn
// has been handed on (diverted to a worker, or sent to the reject path); the
// caller must not touch conn after false. A routing rejection (CHTTP1_USER)
// keeps the status http1_on_headers_complete chose, and a syntax error gets
// 400, because nothing after a syntax error can be framed as a request. Either
// way the answer goes through conn_dispatch_reject, so a slow reader never
// stalls the reactor, and the connection closes. It also arms the header-phase
// budget at the first byte of the request and refreshes conn->last_activity.
static bool conn_feed_bytes(chttpsvr_conn *conn, const char *buf, size_t n) {
  clock_gettime(CLOCK_MONOTONIC, &conn->last_activity);
  conn_header_phase_arm(conn, &conn->last_activity);
  chttp1_errno r = chttp1_parser_execute(&conn->parser, buf, n);

  if (r == CHTTP1_OK) return true;

  if (r == CHTTP1_HEADERS_ONLY || r == CHTTP1_PAUSED) {
    size_t consumed = chttp1_parser_consumed(&conn->parser);
    const char *leftover = buf + consumed;
    size_t leftover_len = n - consumed;

    conn_start_diverted(conn, leftover, leftover_len);
    return false;
  }

  if (!conn->req_rejected) {
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_BAD_REQUEST;
  }
  conn_dispatch_reject(conn);
  return false;
}

// Drives the reactor-owned part of a connection: The TLS handshake if there is
// one, then the reads of the header block, until the request is diverted to a
// worker or rejected. A parked connection goes to conn_linger_step or
// conn_resume_parked instead. The handshake arms the header-phase budget, so a
// peer that drips handshake bytes can't escape it.
// Whenever the socket has nothing (or the handshake wants more), it sets the
// registration to the direction it needs and puts conn back in the idle list,
// so the sweep and a destroy can see it and the next dispatch can claim it. On
// TLS the direction can be write even for a read, because OpenSSL may have to
// send first (see ctls_conn_wants_write). A new registration adds one to
// conn->lifetime_refs and srv->lifetime_refs at once, for the on_removed it
// fires later. A failed modify closes the connection, since a wrong interest
// would strand it. EINTR retries the read; an error or EOF closes the
// connection.
static void conn_pump(chttpsvr_conn *conn) {
  if (__builtin_expect(conn->park != CONN_PARK_NONE, 0)) {
    if (conn->park == CONN_PARK_LINGER)
      conn_linger_step(conn);
    else
      conn_resume_parked(conn);
    return;
  }
  if (conn->state == CONN_ST_TLS_HANDSHAKE) {
    if (!conn->header_phase_active) {
      struct timespec hs_now;
      clock_gettime(CLOCK_MONOTONIC, &hs_now);
      conn_header_phase_arm(conn, &hs_now);
    }
    ctls_handshake_result r = ctls_conn_handshake_step(conn->tls);
    if (r == CTLS_HANDSHAKE_ERROR) {
      SRV_ENGINE_LOG(clog_warn, "TLS handshake failed fd=%d", conn->fd);
      conn_close(conn);
      return;
    }
    if (r == CTLS_HANDSHAKE_WANT_READ || r == CTLS_HANDSHAKE_WANT_WRITE) {
      cselect_dir want =
          (r == CTLS_HANDSHAKE_WANT_WRITE) ? cselect_write : cselect_read;
      if (conn->reg) {
        if (conn_reg_modify(conn, want) != ccol_success) {
          conn_close(conn);
          return;
        }
      } else {
        char *err = NULL;
        conn->reg = ceventloop_add(
            srv_engine_bundler.reactor, cselectable_fd(conn->fd, want),
            (ceventloop_handlers){.on_readable = conn_on_readable,
                                  .on_writable = conn_on_writable,
                                  .on_error = conn_on_error,
                                  .on_removed = conn_on_removed},
            conn, &err);
        if (!conn->reg) {
          conn_close(conn);
          return;
        }
        conn->reg_dir = (uint8_t)want;
        atomic_fetch_add(&conn->lifetime_refs, 1);
        atomic_fetch_add(&conn->srv->lifetime_refs, 1);
      }
      idle_list_add(conn);
      return;
    }
    conn->state = CONN_ST_READING_HEADERS;
    if (conn->reg) {
      if (conn_reg_modify(conn, cselect_read) != ccol_success) {
        conn_close(conn);
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
      if (errno == EINTR) continue;
      if (errno == EWOULDBLOCK || errno == EAGAIN) {
        cselect_dir want = cselect_read;
        if (conn->tls && ctls_conn_wants_write(conn->tls)) want = cselect_write;
        if (!conn->reg) {
          char *err = NULL;
          conn->reg = ceventloop_add(
              srv_engine_bundler.reactor, cselectable_fd(conn->fd, want),
              (ceventloop_handlers){.on_readable = conn_on_readable,
                                    .on_writable = conn_on_writable,
                                    .on_error = conn_on_error,
                                    .on_removed = conn_on_removed},
              conn, &err);
          if (!conn->reg) {
            conn_close(conn);
            return;
          }
          conn->reg_dir = (uint8_t)want;
          atomic_fetch_add(&conn->lifetime_refs, 1);
          atomic_fetch_add(&conn->srv->lifetime_refs, 1);
        } else {
          if (conn_reg_modify(conn, want) != ccol_success) {
            conn_close(conn);
            return;
          }
        }
        idle_list_add(conn);
        return;
      }
      conn_close(conn);
      return;
    }
    if (n == 0) {
      conn_close(conn);
      return;
    }

    if (!conn_feed_bytes(conn, buf, (size_t)n)) return;
  }
}

// Reactor callbacks of a connection. Each one first claims conn out of the idle
// list with idle_list_try_claim, which keeps a dispatch and a concurrent closer
// (idle sweep or destroy) from using the same connection. A failed claim means
// someone else owns it, or its idle_list_add hasn't run yet and level-triggered
// epoll reports it again, so the callback returns without touching it. Readable
// and writable events pump the connection, an error closes it.
static void conn_on_readable(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  if (!idle_list_try_claim(conn)) return;
  conn_pump(conn);
}

static void conn_on_writable(ceventloop loop, ceventloop_reg reg,
                             cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  if (!idle_list_try_claim(conn)) return;
  conn_pump(conn);
}

static void conn_on_error(ceventloop loop, ceventloop_reg reg, cselectable *sel,
                          void *arg) {
  (void)loop;
  (void)reg;
  (void)sel;
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  if (!idle_list_try_claim(conn)) return;
  conn_close(conn);
}

// Slow clients. A worker never waits on a client's socket for a buffered route:
// a body read or a response write that would block parks the connection with no
// thread, a rate floor and gap limits close slow peers, body memory has a
// shared limit with a FIFO wait list, and streaming handlers run on their own
// bounded pool fed by a bounded queue. The sweep enforces every limit, and a
// teardown closes or answers every wait.
//
// The largest SO_RCVLOWAT a parked body asks for, which bounds how much the
// kernel buffers before it wakes the reactor.
#define CHTTPSVR_RCVLOWAT_LIMIT 16384

#ifdef RUNNING_UNIT_TESTS
// Test hooks for the slow-client code: A clock offset that only slow_clock adds
// (so a test can expire a limit without sleeping), process-wide counts of
// resume dispatches and parks (also per kind of park), and a switch that turns
// SO_RCVLOWAT off. The functions below read or set them.
static _Atomic long long g_slow_clock_offset_ms_for_tests = 0;
static _Atomic size_t g_resume_dispatch_count_for_tests = 0;
static _Atomic size_t g_park_count_for_tests = 0;
static _Atomic size_t g_park_kind_count_for_tests[8];
size_t chttpsvr__park_kind_count_for_tests(int kind) {
  return kind >= 0 && kind < 8 ? atomic_load(&g_park_kind_count_for_tests[kind])
                               : 0;
}
static _Atomic bool g_rcvlowat_disabled_for_tests = false;
void chttpsvr__advance_slow_clock_for_tests(long long ms) {
  atomic_fetch_add(&g_slow_clock_offset_ms_for_tests, ms);
}
size_t chttpsvr__resume_dispatch_count_for_tests(void) {
  return atomic_load(&g_resume_dispatch_count_for_tests);
}
size_t chttpsvr__park_count_for_tests(void) {
  return atomic_load(&g_park_count_for_tests);
}
void chttpsvr__set_rcvlowat_enabled_for_tests(bool enabled) {
  atomic_store(&g_rcvlowat_disabled_for_tests, !enabled);
}

// Test hook that holds a streaming-pool task just before it claims the next
// queued request (armed_at 1) or just after (armed_at 2), so a test can run the
// sweep in that window and see that exactly one of the two answers the request.
// release_gen is bumped by every release, and a held task waits for it to
// change, so an arm made after a release never takes the release back from a
// task that hasn't woken yet.
static struct {
  ccol__mutex mutex;
  ccol__cond_var cv;
  ccol__once_flag once;
  int armed_at;
  bool entered;
  unsigned release_gen;
} g_stream_claim_hook = {0};
// Creates the mutex and condition variable of g_stream_claim_hook.
static void stream_claim_hook_init_globals(void) {
  if (ccol__mutex_init(g_stream_claim_hook.mutex) != 0 ||
      ccol__cond_var_init(g_stream_claim_hook.cv) != 0)
    ccol__fatal_err("chttpsvr: stream claim hook init failed");
}
// Test controls of g_stream_claim_hook: Arm it at a point, wait up to
// timeout_ms for a task to be held there (returns whether one was), and release
// it.
void chttpsvr__arm_stream_claim_hook_for_tests(int where) {
  ccol__call_once(g_stream_claim_hook.once, stream_claim_hook_init_globals);
  ccol__mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.armed_at = where;
  g_stream_claim_hook.entered = false;
  ccol__mutex_unlock(g_stream_claim_hook.mutex);
}
bool chttpsvr__wait_stream_claim_hook_entered_for_tests(unsigned timeout_ms) {
  ccol__call_once(g_stream_claim_hook.once, stream_claim_hook_init_globals);
  struct timespec deadline;
  clock_gettime(CLOCK_REALTIME, &deadline);
  timespec_add_ms(&deadline, timeout_ms);
  ccol__mutex_lock(g_stream_claim_hook.mutex);
  while (!g_stream_claim_hook.entered)
    if (ccol__cond_var_timedwait(g_stream_claim_hook.cv,
                                 g_stream_claim_hook.mutex,
                                 deadline) == ETIMEDOUT)
      break;
  bool entered = g_stream_claim_hook.entered;
  ccol__mutex_unlock(g_stream_claim_hook.mutex);
  return entered;
}
void chttpsvr__release_stream_claim_hook_for_tests(void) {
  ccol__call_once(g_stream_claim_hook.once, stream_claim_hook_init_globals);
  ccol__mutex_lock(g_stream_claim_hook.mutex);
  g_stream_claim_hook.release_gen++;
  ccol__cond_var_broadcast(g_stream_claim_hook.cv);
  ccol__mutex_unlock(g_stream_claim_hook.mutex);
}
// Holds the calling streaming task when the hook is armed at where, until the
// next release.
static void stream_claim_hook_wait_if_armed(int where) {
  ccol__call_once(g_stream_claim_hook.once, stream_claim_hook_init_globals);
  ccol__mutex_lock(g_stream_claim_hook.mutex);
  if (g_stream_claim_hook.armed_at == where) {
    unsigned gen = g_stream_claim_hook.release_gen;
    g_stream_claim_hook.armed_at = 0;
    g_stream_claim_hook.entered = true;
    ccol__cond_var_broadcast(g_stream_claim_hook.cv);
    while (g_stream_claim_hook.release_gen == gen)
      ccol__cond_var_wait(g_stream_claim_hook.cv, g_stream_claim_hook.mutex);
  }
  ccol__mutex_unlock(g_stream_claim_hook.mutex);
}
#endif

// Reads the clock of every slow-client limit: CLOCK_MONOTONIC, plus the test
// offset in a test build.
static void slow_clock(struct timespec *ts) {
  clock_gettime(CLOCK_MONOTONIC, ts);
#ifdef RUNNING_UNIT_TESTS
  long long off = atomic_load(&g_slow_clock_offset_ms_for_tests);
  if (off > 0) timespec_add_ms(ts, (unsigned)off);
#endif
}

// Whole milliseconds from then to now, rounded down and signed (negative when
// another thread stamped then after now was read). A limit of n ms compared
// with >= is reached when n ms have passed, never sooner.
static long long ms_since(struct timespec now, struct timespec then) {
  return ns_between(now, then) / 1000000LL;
}

// The same span rounded up. Waits taken off a client's clock and the time left
// before a deadline use it, so a client is never charged for a fraction of a
// millisecond it didn't use.
static long long ms_since_ceil(struct timespec now, struct timespec then) {
  return ms_ceil_of_ns(ns_between(now, then));
}

#ifdef RUNNING_UNIT_TESTS
// Test access to ms_since and ms_since_ceil.
long long chttpsvr__ms_since_for_tests(struct timespec now,
                                       struct timespec then, bool ceil) {
  return ceil ? ms_since_ceil(now, then) : ms_since(now, then);
}
#endif

// Moves *ts ms milliseconds earlier.
static void timespec_sub_ms(struct timespec *ts, long long ms) {
  ts->tv_sec -= (time_t)(ms / 1000);
  ts->tv_nsec -= (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec < 0) {
    ts->tv_nsec += 1000000000L;
    ts->tv_sec -= 1;
  }
}

// Whether now is at or past when.
static bool timespec_reached(struct timespec now, struct timespec when) {
  return now.tv_sec > when.tv_sec ||
         (now.tv_sec == when.tv_sec && now.tv_nsec >= when.tv_nsec);
}

// Sets SO_RCVLOWAT on a connection that parks with an incomplete body, so the
// reactor wakes once a useful amount of body is queued and not for every small
// segment. The mark is the number of body bytes the framing still guarantees
// (chttp1_body_bytes_still_expected), capped at CHTTPSVR_RCVLOWAT_LIMIT, so a
// client that follows its framing always reaches it. It is 1 on a chunk-size
// line or in trailers, and 1 when OpenSSL holds part of a record, because the
// rest of that record needs fewer socket bytes than the plaintext it completes.
// It is also at most half of the receive buffer (read once into
// conn->rcvlowat_cap), because a buffer queues a bit less than its size and a
// mark at the size is never reached. conn->rcvlowat caches the value set, so an
// unchanged mark costs no system call.
static void conn_set_rcvlowat(chttpsvr_conn *conn) {
  int want = 1;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_rcvlowat_disabled_for_tests)) want = 0;
#endif
  if (want && !(conn->tls && ctls_conn_has_pending_input(conn->tls))) {
    uint64_t rem = chttp1_body_bytes_still_expected(&conn->parser);
    if (rem > 1)
      want = rem < (uint64_t)CHTTPSVR_RCVLOWAT_LIMIT ? (int)rem
                                                     : CHTTPSVR_RCVLOWAT_LIMIT;
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

// Puts the mark back to 1 once the body is done, so the next request or a write
// park wakes on its first byte.
static void conn_reset_rcvlowat(chttpsvr_conn *conn) {
  if (conn->rcvlowat > 1) {
    int one = 1;
    (void)setsockopt(conn->fd, SOL_SOCKET, SO_RCVLOWAT, &one, sizeof(one));
    conn->rcvlowat = 1;
  }
}

// Starts the phase the rate floor measures (a body read or a response write) at
// the current slow_clock, with bytes_so_far already counted.
static void rate_window_start(chttpsvr_conn *conn, uint64_t bytes_so_far) {
  slow_clock(&conn->rate_start);
  conn->last_progress = conn->rate_start;
  conn->rate_bytes = bytes_so_far;
  conn->rate_paused_ms = 0;
  conn->rate_queued = 0;
  conn->rate_active = true;
}

// The time of the current phase that counts against the client: Time since its
// start minus the time the server held it back.
static long long rate_elapsed_ms(const chttpsvr_conn *conn,
                                 struct timespec now) {
  return ms_since(now, conn->rate_start) - conn->rate_paused_ms;
}

// How long a phase that moved bytes stays above a floor of rate bytes per
// second: The grace period or the time those bytes buy, whichever is longer.
// Saturates instead of overflowing.
static long long rate_allowed_ms(unsigned rate, unsigned grace,
                                 uint64_t bytes) {
  uint64_t bought =
      bytes > UINT64_MAX / 1000u ? UINT64_MAX : bytes * 1000u / rate;
  if (bought > (uint64_t)LLONG_MAX) bought = (uint64_t)LLONG_MAX;
  return (long long)bought > (long long)grace ? (long long)bought
                                              : (long long)grace;
}

// Whether conn's current phase is below srv's rate floor, counting extra_bytes
// on top of the bytes it moved. A floor of UINT_MAX or no active phase never
// is.
static bool rate_below_floor(struct chttpserver *srv, const chttpsvr_conn *conn,
                             struct timespec now, uint64_t extra_bytes) {
  unsigned rate = atomic_load(&srv->min_rate_bps);
  if (rate == UINT_MAX || !conn->rate_active) return false;
  unsigned grace = atomic_load(&srv->rate_grace_ms);
  return rate_elapsed_ms(conn, now) >
         rate_allowed_ms(rate, grace, conn->rate_bytes + extra_bytes);
}

// The rate floor inside a streaming handler's blocking read. The first call
// starts the phase. Returns false when the body is already below the floor;
// otherwise it shrinks *timeout_ms_inout to the moment the floor would be
// crossed if no more bytes came (0 there means no bound yet, as in
// shrink_timeout_to_deadline).
static bool rate_floor_shrink(chttpsvr_conn *conn, unsigned *timeout_ms_inout) {
  struct chttpserver *srv = conn->srv;
  unsigned rate = atomic_load(&srv->min_rate_bps);
  if (rate == UINT_MAX) return true;
  if (!conn->rate_active) rate_window_start(conn, 0);
  struct timespec now;
  slow_clock(&now);
  long long left = rate_allowed_ms(rate, atomic_load(&srv->rate_grace_ms),
                                   conn->rate_bytes) -
                   rate_elapsed_ms(conn, now) + 1;
  if (left <= 0) return false;
  unsigned left_ms = left > (long long)UINT_MAX ? UINT_MAX : (unsigned)left;
  if (!*timeout_ms_inout || left_ms < *timeout_ms_inout)
    *timeout_ms_inout = left_ms;
  return true;
}

// Returns how many bytes conn's send queue still holds (not sent, or not
// acknowledged by the peer), read through SIOCOUTQ, FIONWRITE or SO_NWRITE
// depending on the system, or SIZE_MAX when the kernel doesn't report it.
static size_t conn_send_queue(const chttpsvr_conn *conn) {
#ifdef RUNNING_UNIT_TESTS
  parked_query_note_for_tests();
#endif
  int outq = 0;
#if defined(SIOCOUTQ)
  if (ioctl(conn->fd, SIOCOUTQ, &outq) != 0 || outq < 0) return SIZE_MAX;
#elif defined(FIONWRITE)
  if (ioctl(conn->fd, FIONWRITE, &outq) != 0 || outq < 0) return SIZE_MAX;
#elif defined(SO_NWRITE)
  socklen_t len = sizeof(outq);
  if (getsockopt(conn->fd, SOL_SOCKET, SO_NWRITE, &outq, &len) != 0 || outq < 0)
    return SIZE_MAX;
#else
  (void)conn;
  return SIZE_MAX;
#endif
  return (size_t)outq;
}

// Judges the gap and the rate floor of a parked response. The reactor only
// wakes such a connection when a large part of the send buffer has drained, so
// the wakeup doesn't measure a steady reader's progress; the send queue does. A
// queue that shrank since the park or the last tick resets the gap (it updates
// c->wp_outq and c->last_progress). For TCP the floor counts the bytes that
// reached the reader (accepted minus queued). For unix:// the kernel reports
// buffer memory instead of bytes, so the floor counts accepted bytes there.
// gap_hit says whether the gap had run out before the queue was read. The
// caller owns c, and calls it with no lock held (see
// parked_needs_kernel_query).
static bool parked_write_expired(struct chttpserver *srv, chttpsvr_conn *c,
                                 struct timespec now_slow, bool gap_hit) {
  unsigned rate = atomic_load(&srv->min_rate_bps);
  bool rate_on = rate != UINT_MAX && c->rate_active;
  if (!gap_hit && !rate_on) return false;
  size_t outq = conn_send_queue(c);
  if (outq == SIZE_MAX) return gap_hit || rate_below_floor(srv, c, now_slow, 0);
  if (outq < c->wp_outq) {
    c->wp_outq = outq;
    c->last_progress = now_slow;
    gap_hit = false;
  }
  if (!rate_on) return gap_hit;
  if (atomic_load(&srv->is_unix_socket))
    return gap_hit || rate_below_floor(srv, c, now_slow, 0);
  uint64_t delivered =
      c->rate_bytes > (uint64_t)outq ? c->rate_bytes - (uint64_t)outq : 0;
  return gap_hit ||
         rate_elapsed_ms(c, now_slow) >
             rate_allowed_ms(rate, atomic_load(&srv->rate_grace_ms), delivered);
}

// Whether parked_expired would have to ask the kernel about c before deciding:
// a parked response with the gap hit or the rate floor on, or a parked body
// that looks stalled while bytes may sit below its low-water mark. The sweep
// calls it with srv->idle_mutex held, and it makes no system call. A connection
// it returns true for is claimed and judged after the unlock, so kernel queries
// never run under the lock every dispatch and park takes.
static bool parked_needs_kernel_query(struct chttpserver *srv,
                                      const chttpsvr_conn *c,
                                      struct timespec now,
                                      struct timespec now_slow) {
  unsigned gap;
  if (c->park == CONN_PARK_BODY) {
    if (c->read_deadline_set && timespec_reached(now, c->read_deadline))
      return false;
    if (c->rcvlowat <= 1) return false;
    gap = atomic_load(&srv->stream_read_timeout_ms);
  } else if (c->park == CONN_PARK_WRITE) {
    if (c->write_deadline_set && timespec_reached(now, c->write_deadline))
      return false;
    gap = atomic_load(&srv->response_write_timeout_ms);
  } else {
    return false;
  }
  bool gap_hit = gap && ms_since(now_slow, c->last_progress) >= (long long)gap;
  if (c->park == CONN_PARK_WRITE)
    return gap_hit ||
           (atomic_load(&srv->min_rate_bps) != UINT_MAX && c->rate_active);
  return gap_hit || rate_below_floor(srv, c, now_slow, c->rate_queued);
}

// Whether a parked connection broke one of its limits. A parked body answers to
// the gap since its last byte, its total read deadline and the rate floor; a
// parked response to its gap, total write deadline and rate floor; a lingering
// connection to its linger deadline. The deadlines use now (CLOCK_MONOTONIC),
// the gap and floor use now_slow. For a body with a low-water mark above 1,
// bytes waiting below the mark count as progress (FIONREAD). The caller owns c
// (it claimed it, or it's the sweep under srv->idle_mutex with
// parked_needs_kernel_query false) and may update its rate state.
static bool parked_expired(struct chttpserver *srv, chttpsvr_conn *c,
                           struct timespec now, struct timespec now_slow) {
  unsigned gap;
  if (c->park == CONN_PARK_BODY) {
    if (c->read_deadline_set && timespec_reached(now, c->read_deadline))
      return true;
    gap = atomic_load(&srv->stream_read_timeout_ms);
  } else if (c->park == CONN_PARK_WRITE) {
    if (c->write_deadline_set && timespec_reached(now, c->write_deadline))
      return true;
    gap = atomic_load(&srv->response_write_timeout_ms);
  } else if (c->park == CONN_PARK_LINGER) {
    return timespec_reached(now, c->linger_deadline);
  } else {
    return false;
  }
  bool gap_hit = gap && ms_since(now_slow, c->last_progress) >= (long long)gap;
  if (c->park == CONN_PARK_WRITE)
    return parked_write_expired(srv, c, now_slow, gap_hit);
  bool rate_hit = rate_below_floor(srv, c, now_slow, c->rate_queued);
  if ((gap_hit || rate_hit) && c->rcvlowat > 1) {
#ifdef RUNNING_UNIT_TESTS
    parked_query_note_for_tests();
#endif
    int queued = 0;
    if (ioctl(c->fd, FIONREAD, &queued) == 0 && queued > 0 &&
        (size_t)queued > c->rate_queued) {
      c->rate_queued = (size_t)queued;
      c->last_progress = now_slow;
      gap_hit = false;
      rate_hit = rate_below_floor(srv, c, now_slow, c->rate_queued);
    }
  }
  return gap_hit || rate_hit;
}

// Records, on both clocks, when conn leaves a wait for the worker pool, so
// conn_uncharge_queue_wait can take the time in the pool queue off its limits.
static void conn_stamp_claim(chttpsvr_conn *conn) {
  clock_gettime(CLOCK_MONOTONIC, &conn->claim_mono);
  slow_clock(&conn->claim_slow);
}

// Takes the time conn spent in the worker pool's queue since its claim off
// every limit of the phase it resumes: The rate floor, the gap and the total
// read or write deadline. A client whose bytes already arrived is never charged
// for how long the server took to get to them.
static void conn_uncharge_queue_wait(chttpsvr_conn *conn, struct timespec now,
                                     struct timespec now_slow) {
  long long waited = ms_since_ceil(now_slow, conn->claim_slow);
  if (waited > 0) {
    unsigned w = waited > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited;
    conn->rate_paused_ms += waited;
    timespec_add_ms(&conn->last_progress, w);
  }
  long long waited_mono = ms_since_ceil(now, conn->claim_mono);
  if (waited_mono > 0) {
    unsigned w =
        waited_mono > (long long)UINT_MAX ? UINT_MAX : (unsigned)waited_mono;
    if (conn->read_deadline_set) timespec_add_ms(&conn->read_deadline, w);
    if (conn->write_deadline_set) timespec_add_ms(&conn->write_deadline, w);
  }
}

// Whether need more bytes fit under srv's body memory limit. mem_in_use can be
// above the limit while a connection runs past it under the progress rule,
// hence the guarded subtraction. The caller holds srv->wait_mutex.
static bool mem_fits_locked(struct chttpserver *srv, size_t need) {
  size_t cap = atomic_load(&srv->mem_cap);
  return srv->mem_in_use <= cap && need <= cap - srv->mem_in_use;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: The highest mem_in_use any server reached since the last reset,
// for the test of the progress rule's bound.
static _Atomic size_t g_mem_peak_for_tests = 0;
size_t chttpsvr__mem_peak_for_tests(void) {
  return atomic_load(&g_mem_peak_for_tests);
}
void chttpsvr__mem_peak_reset_for_tests(void) {
  atomic_store(&g_mem_peak_for_tests, 0);
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: Since the last reset, the most a reserved body buffer ever held
// beyond its reservation, with the old and new blocks of a growth counted
// together. body_growth_note_for_tests updates it at each growth.
static _Atomic size_t g_body_overshoot_for_tests = 0;
size_t chttpsvr__body_overshoot_for_tests(void) {
  return atomic_load(&g_body_overshoot_for_tests);
}
void chttpsvr__body_overshoot_reset_for_tests(void) {
  atomic_store(&g_body_overshoot_for_tests, 0);
}
static void body_growth_note_for_tests(const chttpsvr_conn *conn,
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

// Charges need bytes of body memory to c, counting c as a holder at its first
// charge. A charge above the whole limit counts as the limit, since such a
// request can only run under the progress rule anyway, and the sum saturates so
// mem_in_use never wraps (with max_body_size 0 a body can declare almost
// SIZE_MAX bytes). The caller holds srv->wait_mutex.
static void mem_grant_locked(struct chttpserver *srv, chttpsvr_conn *c,
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

// Reserves need more bytes for conn if they fit right now, and returns whether
// it did. A reservation never jumps ahead of a request already waiting, so
// small requests can't starve a big one; a connection exempt under the progress
// rule always gets it. Takes srv->wait_mutex. Its owner holds conn.
static bool mem_try_reserve(struct chttpserver *srv, chttpsvr_conn *conn,
                            size_t need) {
  ccol__mutex_lock(srv->wait_mutex);
  bool ok = conn->mem_exempt || (!srv->mem_head && mem_fits_locked(srv, need));
  if (ok) mem_grant_locked(srv, conn, need);
  ccol__mutex_unlock(srv->wait_mutex);
  return ok;
}

// Puts conn at the tail of the memory-wait list with park set to kind and need
// bytes wanted. It's the owner's last touch of conn, because from the unlock of
// srv->wait_mutex on an admission on another thread may take it.
static void mem_wait_enqueue(struct chttpserver *srv, chttpsvr_conn *conn,
                             size_t need, uint8_t kind) {
  slow_clock(&conn->mem_wait_start);
  conn->park = kind;
  conn->mem_need = need;
  ccol__mutex_lock(srv->wait_mutex);
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
  ccol__mutex_unlock(srv->wait_mutex);
}

// Unlinks c from the memory-wait list, which is what claims it, and updates the
// waiter counts. The caller holds srv->wait_mutex.
static void mem_wait_unlink_locked(struct chttpserver *srv, chttpsvr_conn *c) {
  if (c->mem_prev) c->mem_prev->mem_next = c->mem_next;
  if (c->mem_next) c->mem_next->mem_prev = c->mem_prev;
  if (srv->mem_head == c) srv->mem_head = c->mem_next;
  if (srv->mem_tail == c) srv->mem_tail = c->mem_prev;
  c->mem_prev = c->mem_next = NULL;
  c->in_mem_wait = false;
  srv->mem_waiting--;
  if (c->mem_charged) srv->mem_blocked--;
}

static void mem_admit(struct chttpserver *srv);

// Gives back conn's whole body memory reservation and ends its progress-rule
// exemption, then admits whatever the freed memory allows. conn_drop_body calls
// it as it frees the body (handler returned, request refused or connection
// closing). Takes srv->wait_mutex, and runs mem_admit after the unlock.
static __attribute__((noinline, cold)) void mem_release(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->wait_mutex);
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
  ccol__mutex_unlock(srv->wait_mutex);
  if (waiters) mem_admit(srv);
}

// Gives back the part of conn's reservation above keep bytes, which a body
// doesn't need once a growth freed the old block or while it's parked, and
// admits waiters if that freed anything. conn's owner calls it.
static __attribute__((noinline, cold)) void mem_trim(chttpsvr_conn *conn,
                                                     size_t keep) {
  struct chttpserver *srv = conn->srv;
  ccol__mutex_lock(srv->wait_mutex);
  bool freed = false;
  if (conn->mem_charged > keep) {
    srv->mem_in_use -= conn->mem_charged - keep;
    if (keep == 0) srv->mem_holders--;
    conn->mem_charged = keep;
    freed = true;
  }
  bool waiters = freed && srv->mem_head != NULL;
  ccol__mutex_unlock(srv->wait_mutex);
  if (waiters) mem_admit(srv);
}

// Once conn's body stopped arriving, trims its reservation to what the buffer
// really holds, because a short read leaves a reserved growth unused. A
// connection running under the progress rule may hold more and keeps it.
static inline __attribute__((always_inline)) void mem_settle(
    chttpsvr_conn *conn) {
  if (__builtin_expect(conn->mem_charged > conn->body.cap, 0))
    mem_trim(conn, conn->body.cap);
}

// Hands a waiter that mem_admit admitted to the worker pool. A request that
// waited before its body started goes to task_worker as if it had just
// diverted; a body that waited for more memory goes to task_resume, with the
// wait taken off its rate floor, gap and read deadline, since the wait was the
// server's time. It puts conn in the diverted list and counts it in
// in_flight_requests. A full pool answers 503, as no response byte has gone out
// yet.
static void mem_dispatch(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  void (*fn)(void *) = task_worker;
  if (conn->park == CONN_PARK_MEM_BODY) {
    struct timespec now;
    slow_clock(&now);
    long long waited = ms_since_ceil(now, conn->mem_wait_start);
    if (waited < 0) waited = 0;
    conn->rate_paused_ms += waited;
    conn->last_progress = now;
    if (conn->read_deadline_set)
      timespec_add_ms(&conn->read_deadline, waited > (long long)UINT_MAX
                                                ? UINT_MAX
                                                : (unsigned)waited);
    conn->park = CONN_PARK_BODY;
    fn = task_resume;
    conn_stamp_claim(conn);
  } else {
    conn->park = CONN_PARK_NONE;
  }
  conn->state = CONN_ST_DIVERTED;
  diverted_list_add(conn);
  ccol__mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  cthrpool pool = srv->worker_pool;
  ccol__mutex_unlock(srv->mutex);
  if (!srv_submit(srv, SRV_POOL_WORKER, pool, fn, conn)) {
    conn->park = CONN_PARK_NONE;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    conn_reject_via_pool(conn);
  }
}

// Admits waiters from the head of the memory-wait list, oldest first, as long
// as the head fits under the limit. When the head doesn't fit and every holder
// of a reservation is itself waiting, nothing could ever free memory, so the
// progress rule lets the head go past the limit until its body is freed; only
// one connection is exempt at a time (srv->mem_exempt_active).
// This can't deadlock: A holder outside the list is a body arriving, a refusal
// or a running handler, and each ends and frees its body, which calls this
// again; with no holder outside, the head goes through. The overshoot stays
// below twice max_body_size (or twice one declared length when that is 0). A
// handler that waits for another request is the one dependency the server can't
// see; that request waits until its memory deadline or until a handler returns.
// srv->wait_mutex is never held across a dispatch, and only one admission loop
// runs at a time: A call that finds one running sets mem_admit_again and
// returns.
static void mem_admit(struct chttpserver *srv) {
  ccol__mutex_lock(srv->wait_mutex);
  if (srv->mem_admitting) {
    srv->mem_admit_again = true;
    ccol__mutex_unlock(srv->wait_mutex);
    return;
  }
  srv->mem_admitting = true;
  for (;;) {
    chttpsvr_conn *batch[CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    srv->mem_admit_again = false;
    while (!srv->mem_admission_closed && srv->mem_head &&
           n < CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn *c = srv->mem_head;
      bool fits = mem_fits_locked(srv, c->mem_need);
      bool ahead = !fits && !srv->mem_exempt_active &&
                   srv->mem_holders == srv->mem_blocked;
      if (!fits && !ahead) break;
      mem_wait_unlink_locked(srv, c);
      mem_grant_locked(srv, c, c->mem_need);
      if (ahead) {
        c->mem_exempt = true;
        srv->mem_exempt_active = true;
      }
      batch[n++] = c;
    }
    if (n == 0 && !srv->mem_admit_again) break;
    ccol__mutex_unlock(srv->wait_mutex);
    for (size_t i = 0; i < n; i++) mem_dispatch(batch[i]);
    ccol__mutex_lock(srv->wait_mutex);
  }
  srv->mem_admitting = false;
  ccol__mutex_unlock(srv->wait_mutex);
}

// Stops mem_admit from dispatching, for a server's teardown.
static void mem_admission_close(struct chttpserver *srv) {
  ccol__mutex_lock(srv->wait_mutex);
  srv->mem_admission_closed = true;
  ccol__mutex_unlock(srv->wait_mutex);
}

// Closes every waiter of the memory-wait list for a server's teardown, claiming
// each one under srv->wait_mutex before it closes it outside the lock.
static void mem_wait_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn *batch[CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    ccol__mutex_lock(srv->wait_mutex);
    while (srv->mem_head && n < CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn *c = srv->mem_head;
      mem_wait_unlink_locked(srv, c);
      batch[n++] = c;
    }
    ccol__mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) {
      batch[i]->park = CONN_PARK_NONE;
      conn_close(batch[i]);
    }
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hooks of the streaming pool. g_stream_pool_threads_for_tests holds the
// thread count of the pool stream_pool_get created last (0 once taken). The
// start hold makes the next chttpsvr_start wait after it swapped in the new
// run's pools and released srv->mutex; the create hold makes the next
// stream_pool_get that builds a pool wait after it read the size. Both wait for
// the test's release, 10 seconds at most. The functions below arm, query and
// release them.
static _Atomic int g_stream_pool_threads_for_tests = 0;
static _Atomic bool g_start_hold_armed_for_tests = false;
static _Atomic bool g_start_hold_entered_for_tests = false;
static _Atomic bool g_start_hold_release_for_tests = false;

int chttpsvr__take_stream_pool_threads_for_tests(void) {
  return atomic_exchange(&g_stream_pool_threads_for_tests, 0);
}

static _Atomic bool g_stream_create_hold_armed_for_tests = false;
static _Atomic bool g_stream_create_hold_entered_for_tests = false;
static _Atomic bool g_stream_create_hold_release_for_tests = false;

static void stream_pool_create_hold_for_tests(void) {
  if (!atomic_exchange(&g_stream_create_hold_armed_for_tests, false)) return;
  atomic_store(&g_stream_create_hold_entered_for_tests, true);
  struct timespec ts = {0, 1000000L};
  for (int i = 0;
       i < 10000 && !atomic_load(&g_stream_create_hold_release_for_tests); i++)
    nanosleep(&ts, NULL);
}

void chttpsvr__arm_stream_create_hold_for_tests(void) {
  atomic_store(&g_stream_create_hold_entered_for_tests, false);
  atomic_store(&g_stream_create_hold_release_for_tests, false);
  atomic_store(&g_stream_create_hold_armed_for_tests, true);
}

bool chttpsvr__stream_create_hold_entered_for_tests(void) {
  return atomic_load(&g_stream_create_hold_entered_for_tests);
}

void chttpsvr__release_stream_create_hold_for_tests(void) {
  atomic_store(&g_stream_create_hold_armed_for_tests, false);
  atomic_store(&g_stream_create_hold_release_for_tests, true);
}

void chttpsvr__arm_start_hold_for_tests(void) {
  atomic_store(&g_start_hold_entered_for_tests, false);
  atomic_store(&g_start_hold_release_for_tests, false);
  atomic_store(&g_start_hold_armed_for_tests, true);
}

bool chttpsvr__start_hold_entered_for_tests(void) {
  return atomic_load(&g_start_hold_entered_for_tests);
}

void chttpsvr__release_start_hold_for_tests(void) {
  atomic_store(&g_start_hold_armed_for_tests, false);
  atomic_store(&g_start_hold_release_for_tests, true);
}

static void start_hold_for_tests(void) {
  if (!atomic_exchange(&g_start_hold_armed_for_tests, false)) return;
  atomic_store(&g_start_hold_entered_for_tests, true);
  struct timespec ts = {0, 1000000};
  for (int i = 0; i < 10000 && !atomic_load(&g_start_hold_release_for_tests);
       i++)
    nanosleep(&ts, NULL);
}
#endif

// Returns srv's streaming pool, creating it on the first streaming request
// after a start, so a server that never gets one starts no threads for it. The
// size is read with stream_gen under srv->mutex, and the pool is built outside
// the lock and installed only while the run is unchanged; a creator that loses
// the race destroys its pool, and one that saw a restart builds again with the
// new size. A server that isn't running or was just detached gets
// CTHRPOOL_NULL.
static cthrpool stream_pool_get(struct chttpserver *srv) {
  for (;;) {
    ccol__mutex_lock(srv->mutex);
    cthrpool pool = srv->stream_pool;
    bool running = srv->worker_pool != CTHRPOOL_NULL;
    uint64_t gen = atomic_load(&srv->stream_gen);
    int nthreads = atomic_load(&srv->stream_threads);
    ccol__mutex_unlock(srv->mutex);
    if (pool || !running) return pool;
    if (nthreads <= 0) return CTHRPOOL_NULL;

#ifdef RUNNING_UNIT_TESTS
    stream_pool_create_hold_for_tests();
#endif
    char *err = NULL;
    cthrpool fresh =
        cthrpool_create_mp((size_t)nthreads, 0, srv->m_procs, &err);
    if (!fresh) return CTHRPOOL_NULL;
    ccol__mutex_lock(srv->mutex);
    bool same_run = atomic_load(&srv->stream_gen) == gen;
    if (same_run && !srv->stream_pool && srv->worker_pool != CTHRPOOL_NULL) {
      srv->stream_pool = fresh;
      fresh = CTHRPOOL_NULL;
    }
    pool = srv->stream_pool;
    ccol__mutex_unlock(srv->mutex);
    if (fresh) {
      cthrpool_shutdown_drain(fresh);
      cthrpool_destroy(fresh);
      if (!same_run) continue;
    }
#ifdef RUNNING_UNIT_TESTS
    else if (pool)
      atomic_store(&g_stream_pool_threads_for_tests, nthreads);
#endif
    return pool;
  }
}

// Answers 503 to a request no pool can take (a streaming request the streaming
// pool refuses or a parked body the worker pool refuses). The connection
// already holds its in_flight_requests slot and is in the diverted list.
static void conn_reject_busy(chttpsvr_conn *conn) {
  conn->park = CONN_PARK_NONE;
  conn->req_rejected = true;
  conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
  conn_reject_via_pool(conn);
}

// Unlinks c from the streaming queue, which claims it. The caller holds
// srv->wait_mutex.
static void stream_unlink_locked(struct chttpserver *srv, chttpsvr_conn *c) {
  if (c->sq_prev) c->sq_prev->sq_next = c->sq_next;
  if (c->sq_next) c->sq_next->sq_prev = c->sq_prev;
  if (srv->sq_head == c) srv->sq_head = c->sq_next;
  if (srv->sq_tail == c) srv->sq_tail = c->sq_prev;
  c->sq_prev = c->sq_next = NULL;
  c->in_stream_queue = false;
  srv->sq_len--;
}

static void stream_token_task(void *arg);
static void stream_queue_flush(struct chttpserver *srv);

// What a stream_token_task receives: Its server, and the stream_gen of the run
// it was counted for in sq_tokens.
typedef struct {
  struct chttpserver *srv;
  uint64_t gen;
} stream_token_arg;

static bool stream_token_start_one(struct chttpserver *srv);

// Starts streaming tasks on the current run's pool while the queue holds more
// requests than the run has tasks on their way to it (sq_unclaimed), and fewer
// tasks are counted than the pool has threads, so a queued request costs at
// most one new task and the pool's own queue never holds more tasks than
// threads.
static void stream_token_start(struct chttpserver *srv) {
  while (stream_token_start_one(srv)) {
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Counts the streaming tasks submitted by every server of the
// process.
static _Atomic size_t g_stream_tokens_started_for_tests = 0;

size_t chttpsvr__stream_tokens_started_for_tests(void) {
  return atomic_load(&g_stream_tokens_started_for_tests);
}
#endif

// One step of stream_token_start: Counts and submits one task when the
// conditions above hold, and returns true when it did. The pool is read after
// stream_gen, so a task counted for one run never goes to an earlier run's
// pool. When no pool takes the task (a pool in shutdown, no running server, a
// failed allocation), it undoes its count if the run is unchanged; if no other
// task of that run is left, every queued request gets a 503, and if the run
// changed, the next run gets its own start. Takes srv->wait_mutex.
static bool stream_token_start_one(struct chttpserver *srv) {
  size_t nthreads = (size_t)atomic_load(&srv->stream_threads);
  ccol__mutex_lock(srv->wait_mutex);
  bool submit = srv->sq_head != NULL && srv->sq_unclaimed < srv->sq_len &&
                srv->sq_tokens < nthreads;
  uint64_t gen = atomic_load(&srv->stream_gen);
  if (submit) {
    srv->sq_tokens++;
    srv->sq_unclaimed++;
  }
  ccol__mutex_unlock(srv->wait_mutex);
  if (!submit) return false;
  cthrpool pool = stream_pool_get(srv);
  stream_token_arg *arg =
      pool ? (stream_token_arg *)ccol__mem_alloc(srv->m_procs, sizeof(*arg))
           : NULL;
  if (arg) {
    arg->srv = srv;
    arg->gen = gen;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_stream_tokens_started_for_tests, 1);
#endif
    if (srv_submit(srv, SRV_POOL_STREAM, pool, stream_token_task, arg))
      return true;
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_sub(&g_stream_tokens_started_for_tests, 1);
#endif
    ccol__mem_free(srv->m_procs, arg);
  }
  ccol__mutex_lock(srv->wait_mutex);
  bool same_run = atomic_load(&srv->stream_gen) == gen;
  if (same_run) {
    srv->sq_tokens--;
    srv->sq_unclaimed--;
  }
  bool orphaned = same_run && srv->sq_tokens == 0;
  ccol__mutex_unlock(srv->wait_mutex);
  if (orphaned)
    stream_queue_flush(srv);
  else if (!same_run)
    stream_token_start(srv);
  return false;
}

// Puts a streaming request into the streaming queue with its queue deadline and
// starts a task for it. The connection keeps its in_flight_requests slot, its
// place in the diverted list and its paused registration, so a teardown sees
// it, but it holds no thread. No pool or a full queue answers 503 at once; the
// call never blocks. From the unlock on, conn may already run on the streaming
// pool.
static void stream_enqueue(chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  cthrpool pool = stream_pool_get(srv);
  if (!pool) {
    conn_reject_busy(conn);
    return;
  }
  size_t qcap = atomic_load(&srv->stream_qcap);
  unsigned tmo = atomic_load(&srv->stream_qtimeout_ms);
  conn->park = CONN_PARK_STREAM;
  conn->sq_has_deadline = tmo != UINT_MAX;
  if (conn->sq_has_deadline) {
    slow_clock(&conn->sq_deadline);
    timespec_add_ms(&conn->sq_deadline, tmo);
  }

  ccol__mutex_lock(srv->wait_mutex);
  if (qcap != SIZE_MAX && srv->sq_len >= qcap) {
    ccol__mutex_unlock(srv->wait_mutex);
    conn_reject_busy(conn);
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
  ccol__mutex_unlock(srv->wait_mutex);
  stream_token_start(srv);
}

// One streaming-pool task: It takes the oldest queued request, runs it through
// task_worker and takes the next, until the queue is empty. Taking a request is
// a claim under srv->wait_mutex, as in the sweep, so one of the two answers it
// and never both; a request whose deadline passed while it waited gets a 503
// here. The thread counts as a worker of srv, so the calls a handler mustn't
// make on its own server are refused here too. A task whose run ended
// (stream_gen changed) stops and hands the queue to the current run, so the new
// thread count applies and the old pool can drain. The task leaves sq_unclaimed
// at its first claim and sq_tokens when it finds the queue empty.
static void stream_token_task(void *arg) {
  stream_token_arg a = *(stream_token_arg *)arg;
  struct chttpserver *srv = a.srv;
  ccol__mem_free(srv->m_procs, arg);
  chttpsvr_mark_worker_thread(srv);
  bool first = true;
  for (;;) {
#ifdef RUNNING_UNIT_TESTS
    stream_claim_hook_wait_if_armed(1);
#endif
    ccol__mutex_lock(srv->wait_mutex);
    if (atomic_load(&srv->stream_gen) != a.gen) {
      ccol__mutex_unlock(srv->wait_mutex);
      stream_token_start(srv);
      return;
    }
    chttpsvr_conn *c = srv->sq_head;
    if (first) srv->sq_unclaimed--;
    first = false;
    if (!c) {
      srv->sq_tokens--;
      ccol__mutex_unlock(srv->wait_mutex);
      return;
    }
    stream_unlink_locked(srv, c);
    ccol__mutex_unlock(srv->wait_mutex);
#ifdef RUNNING_UNIT_TESTS
    stream_claim_hook_wait_if_armed(2);
#endif
    struct timespec now;
    slow_clock(&now);
    if (c->sq_has_deadline && timespec_reached(now, c->sq_deadline)) {
      c->park = CONN_PARK_NONE;
      c->req_rejected = true;
      c->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
      conn_reject_and_close(c, false);
      release_in_flight(srv);
      continue;
    }
    c->park = CONN_PARK_NONE;
    task_worker(c);
  }
}

// Answers every queued streaming request with 503, claiming each under
// srv->wait_mutex first. A teardown calls it, and so does
// stream_token_start_one when the queue has lost every task that drains it.
static void stream_queue_flush(struct chttpserver *srv) {
  for (;;) {
    chttpsvr_conn *batch[CHTTPSVR_IDLE_CLOSE_BATCH];
    size_t n = 0;
    ccol__mutex_lock(srv->wait_mutex);
    while (srv->sq_head && n < CHTTPSVR_IDLE_CLOSE_BATCH) {
      chttpsvr_conn *c = srv->sq_head;
      stream_unlink_locked(srv, c);
      batch[n++] = c;
    }
    ccol__mutex_unlock(srv->wait_mutex);
    if (n == 0) return;
    for (size_t i = 0; i < n; i++) conn_reject_busy(batch[i]);
  }
}

// The part of a sweep tick that no idle list reaches: Requests waiting for body
// memory and streaming requests waiting for a thread. Every expired one is
// claimed out of its list under srv->wait_mutex, chained through that list's
// link field with no cap per tick, and answered with a 503 after the unlock. A
// memory waiter has no in_flight_requests slot, so it goes through
// conn_dispatch_reject, which takes one; a queued streaming request has one
// already. An expired memory waiter can unblock the progress rule, so mem_admit
// runs afterwards.
static void sweep_parked_waits(struct chttpserver *srv,
                               struct timespec now_slow) {
  unsigned mem_ms = atomic_load(&srv->mem_wait_ms);
  chttpsvr_conn *expired = NULL;
  bool any = false;
  ccol__mutex_lock(srv->wait_mutex);
  if (mem_ms != UINT_MAX) {
    chttpsvr_conn *c = srv->mem_head;
    while (c) {
      chttpsvr_conn *next = c->mem_next;
      if (ms_since(now_slow, c->mem_wait_start) >= (long long)mem_ms) {
        mem_wait_unlink_locked(srv, c);
        c->mem_next = expired;
        expired = c;
      }
      c = next;
    }
  }
  ccol__mutex_unlock(srv->wait_mutex);
  while (expired) {
    chttpsvr_conn *c = expired;
    expired = c->mem_next;
    c->mem_next = NULL;
    any = true;
    c->park = CONN_PARK_NONE;
    c->req_rejected = true;
    c->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    conn_dispatch_reject(c);
  }
  if (any) mem_admit(srv);

  ccol__mutex_lock(srv->wait_mutex);
  chttpsvr_conn *q = srv->sq_head;
  while (q) {
    chttpsvr_conn *next = q->sq_next;
    if (q->sq_has_deadline && timespec_reached(now_slow, q->sq_deadline)) {
      stream_unlink_locked(srv, q);
      q->sq_next = expired;
      expired = q;
    }
    q = next;
  }
  ccol__mutex_unlock(srv->wait_mutex);
  while (expired) {
    chttpsvr_conn *c = expired;
    expired = c->sq_next;
    c->sq_next = NULL;
    conn_reject_busy(c);
  }
}

// What a Content-Length body of declared bytes, of which carry came with the
// headers, reserves before a worker reads it: The capacity its buffer reaches
// under body_grow_target's rule to hold the carry-over plus one 8 KiB read,
// never more than declared. Every later growth is reserved as the bytes come in
// (see drain_body_socket_read).
static size_t body_first_reserve(size_t declared, size_t carry) {
  size_t need = carry > SIZE_MAX - 8192 ? SIZE_MAX : carry + 8192;
  if (need > declared) need = declared;
  size_t first = CHTTPSVR_BODY_FIRST_CAP;
  while (first < need) {
    if (first > SIZE_MAX / 2) return need;
    first *= 2;
  }
  return first < declared ? first : declared;
}

// The cold half of conn_start_diverted, for a request whose divert_gate is set:
// a streaming request (SIZE_MAX) goes to the streaming queue, and a body
// reserves its first buffer. Returns true when it took the connection over and
// false when the ordinary divert carries on. If the first buffer doesn't fit,
// the request leaves the diverted list, gives back its in_flight_requests slot
// and waits in the memory-wait list without a thread, and nobody reads its body
// or sends 100 Continue until memory frees up. The caller has counted the
// request, listed it as diverted, paused its registration and saved the
// carry-over.
static __attribute__((noinline, cold)) bool conn_divert_gate(
    chttpsvr_conn *conn) {
  size_t gate = conn->divert_gate;
  if (gate == SIZE_MAX) {
    stream_enqueue(conn);
    return true;
  }
  struct chttpserver *srv = conn->srv;
  size_t first = body_first_reserve(gate, conn->carry_over_len);
  if (mem_try_reserve(srv, conn, first)) return false;
  diverted_list_remove(conn);
  mem_wait_enqueue(srv, conn, first, CONN_PARK_MEM_START);
  release_in_flight(srv);
  mem_admit(srv);
  return true;
}

// The socket half of drain_body, once the carry-over is used up: Reads the
// socket without waiting. When the socket has nothing it sets
// conn->park_pending to CONN_PARK_BODY and returns -1, and the caller parks the
// connection. Before reading it makes sure the body memory reservation covers
// this read and any growth it causes (old and new block together); if that
// can't be reserved, it sets park_pending to CONN_PARK_MEM_BODY and returns -1
// without reading. The capacity it reserves for is clamped to max_body_size and
// to the declared Content-Length, so a legal body never waits for memory it
// won't use. The first call starts the rate floor's phase, and every byte read
// counts as progress.
static __attribute__((noinline, cold)) ssize_t drain_body_socket_read(
    chttpsvr_conn *conn, chttp1_stream *stream, char *raw, size_t len) {
  struct chttpserver *srv = conn->srv;
  if (!conn->rate_active) rate_window_start(conn, conn->body_bytes_seen);
  size_t cap = conn->body.cap;
  size_t limit = atomic_load(&srv->max_body_size);
  size_t need = conn->body.len + len;
  if (need < conn->body.len) need = SIZE_MAX;
  if (limit && need > limit) need = limit < cap ? cap : limit;
  if (chttp1_has_content_length(&conn->parser)) {
    uint64_t rest = chttp1_declared_content_length(&conn->parser);
    if (rest < (uint64_t)(need - conn->body.len)) {
      size_t d = conn->body.len + (size_t)rest;
      need = d < cap ? cap : d;
    }
  }
  if ((need > cap || conn->mem_charged < cap) && !conn->options_star &&
      atomic_load(&srv->mem_cap) != SIZE_MAX) {
    size_t target = body_grow_target(conn, cap, need, limit);
    size_t want = target <= cap             ? cap
                  : target > SIZE_MAX - cap ? SIZE_MAX
                                            : target + cap;
    if (conn->mem_charged < want) {
      size_t step = want - conn->mem_charged;
      if (!mem_try_reserve(srv, conn, step)) {
        conn->mem_need = step;
        conn->park_pending = CONN_PARK_MEM_BODY;
        return -1;
      }
    }
  }
  ssize_t n = chttp1_stream_read(stream, raw, len, 0);
  if (n > 0) {
    conn->rate_bytes += (uint64_t)n;
    conn->rate_queued = 0;
    slow_clock(&conn->last_progress);
  } else if (n < 0 && chttp1_stream_timed_out(stream)) {
    conn->park_pending = CONN_PARK_BODY;
  }
  return n;
}

// Parks conn: Arms its registration for the direction the park of the given
// kind needs (read for a body or a linger, the direction the write wants for a
// write park), sets conn->park and adds conn to the parked list. Every park
// path ends here, so a parked connection holds no thread while the reactor
// waits for it.
// The caller owns conn and doesn't touch it after a true return. A registration
// paused by the worker phase gets a modify and a resume; a connection that
// never had one (its first request was parsed right after the accept) gets a
// fresh ceventloop_add, which takes a lifetime reference on conn and on srv as
// every add does. Returns false, with conn in no list and park cleared, when
// the reactor refuses; the caller then closes conn.
static bool conn_park_arm(chttpsvr_conn *conn, uint8_t kind) {
  cselect_dir dir;
  if (kind == CONN_PARK_BODY) {
    dir = (conn->tls && ctls_conn_wants_write(conn->tls)) ? cselect_write
                                                          : cselect_read;
    conn_set_rcvlowat(conn);
  } else if (kind == CONN_PARK_LINGER) {
    dir = cselect_read;
    conn_reset_rcvlowat(conn);
  } else {
    dir = conn->wp_wants_read ? cselect_read : cselect_write;
    conn_reset_rcvlowat(conn);
    conn->wp_outq = conn_send_queue(conn);
  }
  conn->park = kind;
  conn->state = CONN_ST_READING_HEADERS;
  if (conn->reg) {
    if (conn_reg_modify(conn, dir) != ccol_success ||
        ceventloop_resume(srv_engine_bundler.reactor, conn->reg) !=
            ccol_success) {
      conn->park = CONN_PARK_NONE;
      return false;
    }
  } else {
    char *err = NULL;
    conn->reg = ceventloop_add(
        srv_engine_bundler.reactor, cselectable_fd(conn->fd, dir),
        (ceventloop_handlers){.on_readable = conn_on_readable,
                              .on_writable = conn_on_writable,
                              .on_error = conn_on_error,
                              .on_removed = conn_on_removed},
        conn, &err);
    if (!conn->reg) {
      conn->park = CONN_PARK_NONE;
      return false;
    }
    conn->reg_dir = (uint8_t)dir;
    atomic_fetch_add(&conn->lifetime_refs, 1);
    atomic_fetch_add(&conn->srv->lifetime_refs, 1);
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_park_count_for_tests, 1);
  if (kind < 8) atomic_fetch_add(&g_park_kind_count_for_tests[kind], 1);
#endif
  parked_list_add(conn);
  return true;
}

// Gives up, on the worker that was reading it, a connection whose body stopped
// arriving or that must wait for body memory (conn->park_pending says which).
// Afterwards the connection holds no thread and no in_flight_requests slot, and
// the reactor or a memory admission hands it to a worker again. Nothing in the
// stream needs keeping: The body reader only reads the socket once the
// carry-over is empty, and a pipelined request can only follow a complete body.
// A memory wait goes onto the server's memory wait queue and runs mem_admit. A
// body park trims the body reservation down to the buffer it holds (a body with
// no buffer yet keeps its first reservation) and arms the registration; if that
// fails the connection is closed. The in-flight slot is released only after
// conn is parked or closed.
static __attribute__((noinline, cold)) void task_park_body(
    chttpsvr_conn *conn, struct chttpserver *srv, chttp1_stream *stream,
    chttpsvr_req *req) {
  uint8_t kind = conn->park_pending;
  conn->park_pending = CONN_PARK_NONE;
  destroy_req_scratch(req);
  chttp1_stream_release(stream);
  diverted_list_remove(conn);
  if (kind == CONN_PARK_MEM_BODY) {
    mem_wait_enqueue(srv, conn, conn->mem_need, CONN_PARK_MEM_BODY);
    release_in_flight(srv);
    mem_admit(srv);
    return;
  }
  if (conn->mem_charged > conn->body.cap && conn->body.cap)
    mem_trim(conn, conn->body.cap);
  if (!conn_park_arm(conn, CONN_PARK_BODY)) {
    conn_close(conn);
    release_in_flight(srv);
    return;
  }
  release_in_flight(srv);
}

// Hands a parked connection that the reactor claimed on readiness to a free
// worker, which runs task_resume. It stamps the claim time first, so the queue
// wait isn't charged to the client, and pauses the registration so no more
// readiness fires while a worker owns the connection. It puts conn in the
// diverted list and counts it in srv->in_flight_requests (under srv->mutex).
// When the pool refuses, a parked body gets a 503, since nothing of its
// response is out yet, and a parked response, already partly sent, is closed.
static __attribute__((noinline, cold)) void conn_resume_parked(
    chttpsvr_conn *conn) {
  struct chttpserver *srv = conn->srv;
  conn_stamp_claim(conn);
  conn_pause_reg(conn);
  conn->state = CONN_ST_DIVERTED;
  diverted_list_add(conn);
  ccol__mutex_lock(srv->mutex);
  srv->in_flight_requests++;
  cthrpool pool = srv->worker_pool;
  ccol__mutex_unlock(srv->mutex);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_resume_dispatch_count_for_tests, 1);
#endif
  if (srv_submit(srv, SRV_POOL_WORKER, pool, task_resume, conn)) return;
  if (conn->park == CONN_PARK_WRITE) {
    conn->park = CONN_PARK_NONE;
    conn_close(conn);
    release_in_flight(srv);
    return;
  }
  conn_reject_busy(conn);
}

// Longest time a lingering close waits. When the server closes a connection
// while the client is still sending (often the body of a refused request), the
// kernel answers those bytes with a reset, and a reset can wipe the response
// out of the client's receive buffer. So the server shuts down its write side
// and reads and throws away what arrives, as nginx's lingering_close does. The
// bound is a time, because a byte budget smaller than a refused upload would
// close the connection under a client that sends the whole body before it reads
// the response.
#define CHTTPSVR_LINGER_MS 2000u
// Most bytes one dispatch of a lingering connection throws away; the rest waits
// for the next readiness report, so a fast sender can't hold the reactor
// thread.
#define CHTTPSVR_LINGER_STEP_BYTES ((size_t)256 * 1024)

#ifdef RUNNING_UNIT_TESTS
// Test hook: Counts the lingering closes that started, and its reader.
static _Atomic size_t g_linger_count_for_tests = 0;
size_t chttpsvr__linger_count_for_tests(void) {
  return atomic_load(&g_linger_count_for_tests);
}
#endif

// Tells whether conn's client may still send bytes the server hasn't read: The
// rest of a body, pipelined bytes in the carry-over, or bytes already queued in
// the socket (FIONREAD). The close paths use it to pick a lingering close over
// a plain one.
static bool conn_request_bytes_may_remain(chttpsvr_conn *conn) {
  if (chttp1_request_bytes_may_remain(&conn->parser)) return true;
  if (conn->carry_over_len != 0) return true;
  int queued = 0;
  return ioctl(conn->fd, FIONREAD, &queued) == 0 && queued > 0;
}

// Reads and throws away what the client sent, without waiting, up to
// CHTTPSVR_LINGER_STEP_BYTES per call. Returns true while the linger should go
// on (the socket is empty or the step budget is used up), and false once the
// client closed or the read failed.
static bool linger_discard(chttpsvr_conn *conn) {
  char buf[16384];
  size_t taken = 0;
  while (taken < CHTTPSVR_LINGER_STEP_BYTES) {
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

// Starts conn's lingering close in place of conn_close (see
// CHTTPSVR_LINGER_MS). The caller owns conn, has it in no list but the diverted
// list, and doesn't touch it again.
// It frees the request state, the response and the body reservation right away,
// since the real close can be up to CHTTPSVR_LINGER_MS away. A TLS connection
// sends its close_notify as its TLS state goes; nothing else is written. Then
// it shuts down the write side, sets conn->linger_deadline, discards what is
// already there and parks conn as CONN_PARK_LINGER, which holds no thread but
// still counts against max_connections. Any failure closes the connection at
// once.
static __attribute__((noinline, cold)) void conn_linger(chttpsvr_conn *conn) {
  conn_drop_request_state(conn);
  destroy_resp(&conn->resp, conn->m_procs);
  if (conn->tls) {
    ctls_conn_destroy(conn->tls);
    conn->tls = NULL;
  }
  if (shutdown(conn->fd, SHUT_WR) != 0) {
    conn_close(conn);
    return;
  }
  clock_gettime(CLOCK_MONOTONIC, &conn->linger_deadline);
  timespec_add_ms(&conn->linger_deadline, CHTTPSVR_LINGER_MS);
  if (!linger_discard(conn)) {
    conn_close(conn);
    return;
  }
  diverted_list_remove(conn);
  if (!conn_park_arm(conn, CONN_PARK_LINGER)) {
    conn_close(conn);
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_linger_count_for_tests, 1);
#endif
}

// Handles a lingering connection the reactor claimed on readiness: Throws away
// what arrived, then parks it again, or closes it once the client closed or
// conn->linger_deadline passed.
static __attribute__((noinline, cold)) void conn_linger_step(
    chttpsvr_conn *conn) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (linger_discard(conn) && !timespec_reached(now, conn->linger_deadline)) {
    parked_list_add(conn);
    return;
  }
  conn->park = CONN_PARK_NONE;
  conn_close(conn);
}

// Saves the state of a response whose write ran into a full socket, so
// task_continue_write can finish it later. head and head_len are the unsent
// part of the header block, which lives on send_response's stack and is copied;
// body_pos is how much of the body is out. internal marks one of the library's
// own writes (a rejection or the interim line), whose time limit is capped by
// CHTTPSVR_INTERNAL_WRITE_MAX_TOTAL_MS.
// It sets the wp_* fields and conn->wp_active, and moves pipelined bytes of a
// next request from the stream onto conn->carry_over, because the caller
// releases the stream; if those bytes can't be copied, the connection closes
// when the write ends instead of waiting for a request the client already sent.
// The write's rate window starts where the write started, taken from the write
// deadline when a total-duration bound is on. If the header copy fails
// wp_active stays false, and the caller closes the connection as for any failed
// write.
static __attribute__((noinline, cold)) void send_response_park(
    chttpsvr_conn *conn, chttp1_stream *stream, const char *head,
    size_t head_len, size_t body_pos, bool keep_alive, bool no_body,
    bool internal) {
  char *copy = NULL;
  if (head_len) {
    copy = (char *)ccol__mem_alloc(conn->m_procs, head_len);
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
                        !ctls_conn_wants_write((ctls_conn *)stream->tls);
  conn->wp_active = true;
  size_t lo_len = 0;
  char *lo = chttp1_stream_take_leftover(stream, &lo_len);
  if (lo) {
    ccol__mem_free(conn->m_procs, conn->carry_over);
    conn->carry_over = lo;
    conn->carry_over_len = lo_len;
  } else if (lo_len) {
    conn->wp_keep_alive = false;
  }
  rate_window_start(conn, body_pos);
  unsigned max_dur = atomic_load(&conn->srv->max_response_write_duration_ms);
  if (internal && (max_dur == 0 || max_dur > internal_write_ceiling_ms()))
    max_dur = internal_write_ceiling_ms();
  if (conn->write_deadline_set && max_dur) {
    struct timespec now_mono;
    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    long long run =
        (long long)max_dur - ms_since_ceil(conn->write_deadline, now_mono);
    if (run > 0) timespec_sub_ms(&conn->rate_start, run);
  }
}

// Parks a connection whose response ran into a full socket (see
// send_response_park) until the socket has room; the sweep applies the write
// limits and the rate floor meanwhile. It releases the in-flight slot after
// conn is parked, or after it is closed when the park fails.
static __attribute__((noinline, cold)) void conn_park_write(
    chttpsvr_conn *conn, struct chttpserver *srv) {
  diverted_list_remove(conn);
  if (!conn_park_arm(conn, CONN_PARK_WRITE)) {
    conn->wp_active = false;
    ccol__mem_free(conn->m_procs, conn->wp_head);
    conn->wp_head = NULL;
    conn_close(conn);
    release_in_flight(srv);
    return;
  }
  release_in_flight(srv);
}

// Drops the saved state of a parked response once its write has ended.
static void conn_end_parked_write(chttpsvr_conn *conn) {
  conn->wp_active = false;
  ccol__mem_free(conn->m_procs, conn->wp_head);
  conn->wp_head = NULL;
  conn->wp_head_len = conn->wp_head_pos = conn->wp_body_pos = 0;
}

// Writes the rest of a parked response without waiting, checking the write
// deadline before each write (the library's own writes keep their internal
// ceiling). It advances the wp_* positions, the rate byte count and
// conn->last_progress. Returns 1 once the whole response is out, 0 when the
// write failed or a write limit ran out, and -1 when the socket is full again;
// on -1 it records in conn->wp_wants_read whether TLS needs the socket readable
// first.
static int send_response_continue(chttp1_stream *stream, chttpsvr_conn *conn) {
  size_t blen = conn->wp_no_body ? 0 : conn->resp.body_len;
  for (;;) {
    size_t alen = conn->wp_head_len - conn->wp_head_pos;
    size_t brem = blen - conn->wp_body_pos;
    if (alen + brem == 0) return 1;
    unsigned call_timeout_ms = 0;
    if (!response_write_deadline_ok(conn, conn->wp_reject || conn->wp_interim,
                                    &call_timeout_ms))
      return 0;
    ssize_t n = chttp1_stream_writev2(
        stream, conn->wp_head ? conn->wp_head + conn->wp_head_pos : NULL, alen,
        brem ? conn->resp.body + conn->wp_body_pos : NULL, brem, 0);
    if (n <= 0) {
      if (n < 0 && chttp1_stream_timed_out(stream)) {
        conn->wp_wants_read =
            stream->tls && !ctls_conn_wants_write((ctls_conn *)stream->tls);
        return -1;
      }
      return 0;
    }
    size_t moved = (size_t)n;
    size_t in_head = moved < alen ? moved : alen;
    conn->wp_head_pos += in_head;
    conn->wp_body_pos += moved - in_head;
    conn->rate_bytes += (uint64_t)n;
    slow_clock(&conn->last_progress);
  }
}

// Worker thread: Body read, handler and response.
//
// What drain_body returns, and only then, when the body stopped arriving or
// must wait for memory and the caller has to park the connection instead of
// waiting.
#define CHTTPSVR_BODY_PARKED ccol_container_empty

static bool rate_floor_shrink(chttpsvr_conn *conn, unsigned *timeout_ms_inout);

// Shrinks *timeout_ms_inout so a body read doesn't wait past the body's
// max_body_read_duration_ms deadline. Returns false, and sets
// conn->deadline_exceeded, once that deadline has passed.
static bool check_read_deadline(chttpsvr_conn *conn,
                                unsigned *timeout_ms_inout) {
  unsigned max_dur = atomic_load(&conn->srv->max_body_read_duration_ms);
  if (!shrink_timeout_to_deadline(&conn->read_deadline,
                                  &conn->read_deadline_set, max_dur,
                                  timeout_ms_inout)) {
    conn->deadline_exceeded = true;
    return false;
  }
  return true;
}

// The same check as check_read_deadline, kept inline for drain_body, which runs
// for every buffered body.
static inline __attribute__((always_inline)) bool check_read_deadline_inl(
    chttpsvr_conn *conn, unsigned *timeout_ms_inout) {
  unsigned max_dur = atomic_load(&conn->srv->max_body_read_duration_ms);
  if (!shrink_timeout_to_deadline_inl(&conn->read_deadline,
                                      &conn->read_deadline_set, max_dur,
                                      timeout_ms_inout)) {
    conn->deadline_exceeded = true;
    return false;
  }
  return true;
}

// Reads a buffered route's whole request body on the calling worker and feeds
// it to the parser; http1_on_body collects it in conn->body. Streaming routes
// never come here, their handler reads with chttpsvr_req_read. The carry-over
// that came with the headers is read first, then the socket without waiting.
// Returns ccol_success, CHTTPSVR_BODY_PARKED when the caller must park
// (conn->park_pending set), ccol_timed_out, ccol_msg_too_large,
// ccol_not_enough_memory, or ccol_http_transfer_aborted; the caller maps these
// to a status. A truncated body or bad framing sets conn->body_malformed so it
// answers 400, never 500, and an oversized chunk sets conn->body_too_large.
// Bytes past the end of the message belong to a pipelined next request and go
// back onto the stream's carry-over; if that fails the parser is forced to
// close the connection, or the client would wait for an answer to a request
// whose bytes we lost.
static ccol_retval drain_body(chttpsvr_conn *conn, chttp1_stream *stream) {
  for (;;) {
    if (chttp1_parser_message_complete(&conn->parser)) break;
    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!check_read_deadline_inl(conn, &timeout_ms)) return ccol_timed_out;
    char raw[8192];
    ssize_t n = chttp1_stream_has_carry(stream)
                    ? chttp1_stream_read(stream, raw, sizeof(raw),
                                         to_stream_timeout_ms(timeout_ms))
                    : drain_body_socket_read(conn, stream, raw, sizeof(raw));
    if (n < 0) {
      if (conn->park_pending) return CHTTPSVR_BODY_PARKED;
      if (chttp1_stream_timed_out(stream)) return ccol_timed_out;
      return ccol_http_transfer_aborted;
    }
    if (n == 0) {
      conn->body_malformed = true;
      return ccol_http_transfer_aborted;
    }
    chttp1_errno r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(stream, raw + consumed,
                                            (size_t)n - consumed)) {
        chttp1_parser_force_close(&conn->parser);
      }
      break;
    }
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      if (r == CHTTP1_ERROR && !conn->body_too_large &&
          !conn->body_alloc_failed)
        conn->body_malformed = true;
      return conn->body_too_large      ? ccol_msg_too_large
             : conn->body_alloc_failed ? ccol_not_enough_memory
                                       : ccol_http_transfer_aborted;
    }
  }
  return ccol_success;
}

// Reads up to buflen bytes of a streaming route's request body into buf,
// waiting as the read timeout, the body duration bound and the rate floor
// allow. Returns the count read, 0 at the end of the body, and -1 on failure;
// chttpsvr_req_stream_error then tells why from the flags this sets on conn
// (deadline_exceeded, body_too_large, transfer_aborted). It fails for a NULL
// req, a buffered route, or a request with no matched route (a rejection a
// middleware sees), which also covers a req used after its handler returned.
// The first call sends the deferred 100 Continue when the client asked for it
// and a body is still to come, so a handler that rejects without reading never
// invites the upload. A failed or short interim write sets
// conn->interim_write_failed and transfer_aborted, and no response follows it.
// Bytes past the end of the message are pushed back onto the stream for
// task_tail, as in drain_body.
ssize_t chttpsvr_req_read(chttpsvr_req *req, void *buf, size_t buflen) {
  if (!req) return -1;
  if (buflen == 0) return 0;
  if (!buf) return -1;
  chttpsvr_conn *conn = req->conn;
  if (!conn || !conn->matched_route) return -1;
  if (!conn->matched_route->is_streaming) return -1;
  if (!req->stream) return -1;

  if (conn->expects_continue && !conn->interim_continue_sent) {
    conn->interim_continue_sent = true;
    if (!chttp1_parser_message_complete(&conn->parser)) {
      unsigned wtimeout_ms = atomic_load(&conn->srv->response_write_timeout_ms);
      if (write_interim_continue(conn, req->stream, wtimeout_ms) != 1) {
        conn->interim_write_failed = true;
        conn->transfer_aborted = true;
      }
    }
  }
  if (conn->interim_write_failed) return -1;

  for (;;) {
    growbuf *b = &conn->body;
    if (b->pos < b->len) {
      size_t avail = b->len - b->pos;
      size_t take = avail < buflen ? avail : buflen;
      memcpy(buf, b->buf + b->pos, take);
      b->pos += take;
      return (ssize_t)take;
    }
    if (chttp1_parser_message_complete(&conn->parser)) return 0;

    unsigned timeout_ms = atomic_load(&conn->srv->stream_read_timeout_ms);
    if (!check_read_deadline(conn, &timeout_ms)) return -1;
    if (!rate_floor_shrink(conn, &timeout_ms)) {
      conn->deadline_exceeded = true;
      return -1;
    }
    char raw[8192];
    ssize_t n = chttp1_stream_read(req->stream, raw, sizeof(raw),
                                   to_stream_timeout_ms(timeout_ms));
    if (n > 0) conn->rate_bytes += (uint64_t)n;
    if (n < 0) {
      if (chttp1_stream_timed_out(req->stream)) {
        conn->deadline_exceeded = true;
      } else {
        conn->transfer_aborted = true;
      }
      return -1;
    }
    if (n == 0) {
      conn->transfer_aborted = true;
      return -1;
    }
    chttp1_errno r = chttp1_parser_execute(&conn->parser, raw, (size_t)n);
    if (r == CHTTP1_USER || r == CHTTP1_ERROR) {
      if (chttp1_chunk_size_limit_exceeded(&conn->parser))
        conn->body_too_large = true;
      conn->transfer_aborted = true;
      return -1;
    }
    if (r == CHTTP1_PAUSED) {
      size_t consumed = chttp1_parser_consumed(&conn->parser);
      if ((size_t)n > consumed &&
          !chttp1_stream_push_back_leftover(req->stream, raw + consumed,
                                            (size_t)n - consumed)) {
        chttp1_parser_force_close(&conn->parser);
      }
    }
  }
}

// Tells a streaming handler why chttpsvr_req_read failed, from the flags the
// read set on the connection: ccol_timed_out, ccol_msg_too_large,
// ccol_not_enough_memory or ccol_http_transfer_aborted, in that priority, else
// ccol_success. Returns ccol_invalid_args for a NULL or detached req.
ccol_retval chttpsvr_req_stream_error(const chttpsvr_req *req) {
  if (!req || !req->conn) return ccol_invalid_args;
  chttpsvr_conn *conn = req->conn;
  if (conn->deadline_exceeded) return ccol_timed_out;
  if (conn->body_too_large) return ccol_msg_too_large;
  if (conn->body_alloc_failed) return ccol_not_enough_memory;
  if (conn->transfer_aborted) return ccol_http_transfer_aborted;
  return ccol_success;
}

// Finishes a request whose response went out or was given up: It frees the
// stream and the request scratch, then closes, lingers, parks a half-written
// response, or hands a kept-alive connection back to the reactor. task_worker,
// task_resume and the parked-write paths all end here; it is inlined so an
// ordinary request compiles as one function.
// Pipelined bytes of a next request, already off the wire, are taken from the
// stream and kept only on keep-alive. They are fed to the parser right here, as
// is any input that TLS holds inside the SSL object, because epoll would never
// report either. After conn_feed_bytes the connection may already be freed by
// another thread, so only srv (whose m_procs equals conn->m_procs) is used
// after that call. A kept-alive connection gets its registration forced back to
// the read direction (or write, when TLS needs it) and resumed, or a fresh add
// with both handlers when it never had one. srv->in_flight_requests is released
// last on every path, once conn is closed or back in the idle list, or the
// drain could free srv under us.
static inline __attribute__((always_inline)) void task_tail(
    chttpsvr_conn *conn, struct chttpserver *srv, chttp1_stream *stream,
    chttpsvr_req *req, bool prepared, bool keep_alive, bool response_out) {
  size_t reclaimed_len = 0;
  char *reclaimed =
      prepared ? chttp1_stream_take_leftover(stream, &reclaimed_len) : NULL;

  if (prepared) chttp1_stream_release(stream);
  destroy_req_scratch(req);

  if (!keep_alive) ccol__mem_free(conn->m_procs, reclaimed);

  if (!keep_alive) {
    if (__builtin_expect(conn->wp_active, 0)) {
      conn_drop_request_state(conn);
      conn_park_write(conn, srv);
      return;
    }
    if (__builtin_expect(response_out && (reclaimed_len != 0 ||
                                          conn_request_bytes_may_remain(conn)),
                         0))
      conn_linger(conn);
    else
      conn_close(conn);
    release_in_flight(srv);
    return;
  }

  diverted_list_remove(conn);
  conn_reset_for_request(conn);
  conn->state = CONN_ST_READING_HEADERS;

  if (reclaimed) {
    conn->carry_over = reclaimed;
    conn->carry_over_len = reclaimed_len;
  } else if (__builtin_expect(reclaimed_len != 0, 0)) {
    conn_close(conn);
    release_in_flight(srv);
    return;
  }

  if (conn->carry_over_len > 0) {
    char *lo = conn->carry_over;
    size_t lo_len = conn->carry_over_len;
    conn->carry_over = NULL;
    conn->carry_over_len = 0;
    ccol_memmgmt_procs *mp = srv->m_procs;
    bool need_more = conn_feed_bytes(conn, lo, lo_len);
    ccol__mem_free(mp, lo);
    if (!need_more) {
      release_in_flight(srv);
      return;
    }
  }

  cselect_dir tail_dir = cselect_read;
  while (conn->tls && ctls_conn_has_pending_input(conn->tls)) {
    char tls_buf[8192];
    ssize_t n = ctls_conn_read(conn->tls, tls_buf, sizeof(tls_buf));
    if (n < 0 && errno == EINTR) continue;
    if (n < 0 && (errno == EWOULDBLOCK || errno == EAGAIN)) {
      if (ctls_conn_wants_write(conn->tls)) tail_dir = cselect_write;
      break;
    }
    if (n <= 0) {
      conn_close(conn);
      release_in_flight(srv);
      return;
    }
    if (!conn_feed_bytes(conn, tls_buf, (size_t)n)) {
      release_in_flight(srv);
      return;
    }
  }

  if (conn->reg) {
    if (conn_reg_modify(conn, tail_dir) != ccol_success) {
      conn_close(conn);
      release_in_flight(srv);
      return;
    }
    if (ceventloop_resume(srv_engine_bundler.reactor, conn->reg) !=
        ccol_success) {
      conn_close(conn);
      release_in_flight(srv);
      return;
    }
  } else {
    char *err = NULL;
    conn->reg = ceventloop_add(
        srv_engine_bundler.reactor, cselectable_fd(conn->fd, tail_dir),
        (ceventloop_handlers){.on_readable = conn_on_readable,
                              .on_writable = conn_on_writable,
                              .on_error = conn_on_error,
                              .on_removed = conn_on_removed},
        conn, &err);
    if (!conn->reg) {
      conn_close(conn);
      release_in_flight(srv);
      return;
    }
    conn->reg_dir = (uint8_t)tail_dir;
    atomic_fetch_add(&conn->lifetime_refs, 1);
    atomic_fetch_add(&srv->lifetime_refs, 1);
  }
  idle_list_add(conn);
  release_in_flight(srv);
}

// Ends a request whose body is read: Frees the body, decides keep-alive
// (complete message, client allows it, body not too large, server not draining)
// and sends the response, then calls task_tail. A response never waits for a
// slow reader on this thread; a full socket parks the connection. Nothing is
// sent after a failed interim 100 Continue, because part of that line may be on
// the wire.
static inline __attribute__((always_inline)) void task_finish(
    chttpsvr_conn *conn, struct chttpserver *srv, chttp1_stream *stream,
    chttpsvr_req *req, bool prepared, bool aborted, unsigned write_timeout_ms) {
  conn_drop_body(conn);
  bool msg_fully_parsed =
      prepared && chttp1_parser_message_complete(&conn->parser);
  bool keep_alive = prepared && !aborted && msg_fully_parsed &&
                    chttp1_should_keep_alive(&conn->parser) &&
                    !conn->body_too_large && !srv_draining(srv);

  stream->write_nonblocking = true;
  bool sent = prepared && !conn->interim_write_failed &&
              send_response(stream, &conn->resp, keep_alive, write_timeout_ms,
                            conn->method == CHTTP_HEAD, conn, false);
  if (!sent) keep_alive = false;

  task_tail(conn, srv, stream, req, prepared, keep_alive, sent);
}

// Worker task for a newly diverted request: Reads the body of a buffered route,
// runs the middleware chain and the handler, and ends through task_finish. A
// request still queued when a teardown started (srv_draining) gets a 503 with
// Connection: close instead of a handler.
// If copying the carry-over fails the stream is prepared without it, so the
// client still gets an error response; the body is then lost and the handler
// doesn't run. A buffered route sends its 100 Continue here when a body is
// still expected; a streaming route defers it to chttpsvr_req_read, so its
// handler can reject without inviting the upload. "OPTIONS *" is answered with
// 200 and no handler, reading and discarding a body up to
// CHTTPSVR_OPTIONS_STAR_MAX_BODY. A body that stalls or must wait for memory
// parks the connection and the thread returns. A failed buffered body maps to
// 413, 408, 400 (malformed) or 500.
static void task_worker(void *arg) {
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  struct chttpserver *srv = conn->srv;
  chttpsvr_mark_worker_thread(srv);

  if (__builtin_expect(srv_draining(srv), 0)) {
    ccol__mem_free(conn->m_procs, conn->carry_over);
    conn->carry_over = NULL;
    conn->carry_over_len = 0;
    conn->req_rejected = true;
    conn->reject_status = CHTTP_STATUS_SERVICE_UNAVAILABLE;
    conn_reject_via_pool(conn);
    return;
  }

  chttp1_stream stream;
  bool prepared;
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_force_stream_prepare_fail_for_tests)) {
    prepared = false;
  } else
#endif
      if (conn->tls)
    prepared = chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls,
                                         conn->carry_over, conn->carry_over_len,
                                         conn->m_procs);
  else
    prepared = chttp1_stream_prepare(&stream, conn->fd, conn->carry_over,
                                     conn->carry_over_len, conn->m_procs);
  bool body_unavailable = !prepared;
  if (body_unavailable) {
    prepared =
        conn->tls
            ? chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                                        conn->m_procs)
            : chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  }
  stream.fd_nonblocking = true;
  ccol__mem_free(conn->m_procs, conn->carry_over);
  conn->carry_over = NULL;
  conn->carry_over_len = 0;

  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  bool options_star_body_refused =
      conn->options_star && chttp1_has_content_length(&conn->parser) &&
      chttp1_declared_content_length(&conn->parser) >
          (uint64_t)CHTTPSVR_OPTIONS_STAR_MAX_BODY;
  if (prepared && !body_unavailable && conn->expects_continue &&
      !conn->matched_route->is_streaming && !options_star_body_refused &&
      !chttp1_parser_message_complete(&conn->parser)) {
    stream.write_nonblocking = true;
    int ic = write_interim_continue(conn, &stream, write_timeout_ms);
    conn->interim_continue_sent = true;
    if (ic < 0) {
      chttp1_stream_release(&stream);
      conn_park_write(conn, srv);
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

  ccol_retval body_err = ccol_success;
  bool aborted = conn->interim_write_failed;
  if (!aborted && conn->options_star) {
    bool body_ok = prepared && !body_unavailable && !options_star_body_refused;
    if (body_ok) {
      ccol_retval star_err = drain_body(conn, &stream);
      if (star_err == CHTTPSVR_BODY_PARKED) {
        task_park_body(conn, srv, &stream, &req);
        return;
      }
      body_ok = (star_err == ccol_success);
    }
    conn->resp.status_code = CHTTP_STATUS_OK;
    aborted = !body_ok;
  } else if (!aborted) {
    if (body_unavailable) {
      body_err = ccol_not_enough_memory;
    } else if (prepared) {
      if (!conn->matched_route->is_streaming) {
        body_err = drain_body(conn, &stream);
        if (body_err != CHTTPSVR_BODY_PARKED) mem_settle(conn);
      }
    } else {
      body_err = ccol_not_enough_memory;
    }

    if (body_err != ccol_success &&
        (body_unavailable || !conn->matched_route->is_streaming)) {
      if (body_err == CHTTPSVR_BODY_PARKED) {
        task_park_body(conn, srv, &stream, &req);
        return;
      }
      conn->resp.status_code =
          (body_err == ccol_msg_too_large) ? CHTTP_STATUS_PAYLOAD_TOO_LARGE
          : (body_err == ccol_timed_out)   ? CHTTP_STATUS_REQUEST_TIMEOUT
          : conn->body_malformed           ? CHTTP_STATUS_BAD_REQUEST
                                           : CHTTP_STATUS_INTERNAL_ERROR;
      aborted = true;
    } else {
      chttpsvr_next(&req, &conn->resp);
    }
  }

  task_finish(conn, srv, &stream, &req, prepared, aborted, write_timeout_ms);
}

static void task_continue_write(chttpsvr_conn *conn, struct chttpserver *srv,
                                chttp1_stream *stream, bool expired);
static void task_continue_interim(chttpsvr_conn *conn, struct chttpserver *srv,
                                  bool expired);
static void task_read_body_and_finish(chttpsvr_conn *conn,
                                      struct chttpserver *srv,
                                      chttp1_stream *stream, bool expired);

// Worker task for a parked connection the reactor handed back (see
// conn_resume_parked). It judges the park's limits at the claim time, since the
// sweep only looks once a second, and takes the queue wait off the client's
// clocks. Then it continues a parked interim line (task_continue_interim), a
// parked response (task_continue_write), or a parked body
// (task_read_body_and_finish). The stream starts empty: A body only parks with
// an empty carry-over, and a parked response's pipelined bytes wait on the
// connection.
static void task_resume(void *arg) {
  chttpsvr_conn *conn = (chttpsvr_conn *)arg;
  struct chttpserver *srv = conn->srv;
  chttpsvr_mark_worker_thread(srv);
  uint8_t kind = conn->park;
  struct timespec now, now_slow;
  clock_gettime(CLOCK_MONOTONIC, &now);
  slow_clock(&now_slow);
  bool expired = parked_expired(srv, conn, conn->claim_mono, conn->claim_slow);
  if (!expired) conn_uncharge_queue_wait(conn, now, now_slow);
  conn->park = CONN_PARK_NONE;
  conn->rate_queued = 0;

  chttp1_stream stream;
  if (kind == CONN_PARK_WRITE && conn->wp_interim) {
    task_continue_interim(conn, srv, expired);
    return;
  }
  if (conn->tls)
    chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                              conn->m_procs);
  else
    chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  stream.fd_nonblocking = true;

  if (kind == CONN_PARK_WRITE) {
    task_continue_write(conn, srv, &stream, expired);
    return;
  }
  task_read_body_and_finish(conn, srv, &stream, expired);
}

// Reads the rest of a buffered body after a park or after its 100 Continue went
// out, runs the handler and ends the request through task_finish, as
// task_worker does. The caller prepared stream and gives it up here. expired
// means a park limit ran out, which answers 408. It parks again if the body
// stalls, and otherwise resets the receive low-water mark and settles the body
// reservation.
static void task_read_body_and_finish(chttpsvr_conn *conn,
                                      struct chttpserver *srv,
                                      chttp1_stream *stream, bool expired) {
  unsigned write_timeout_ms = atomic_load(&srv->response_write_timeout_ms);
  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  req.conn = conn;
  req.stream = stream;
  req.param_names = (const char **)conn->matched_route->param_names;
  req.m_procs = conn->m_procs;

  ccol_retval body_err = ccol_timed_out;
  if (!expired) body_err = drain_body(conn, stream);
  if (body_err == CHTTPSVR_BODY_PARKED) {
    task_park_body(conn, srv, stream, &req);
    return;
  }
  conn_reset_rcvlowat(conn);
  mem_settle(conn);

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
    chttpsvr_next(&req, &conn->resp);
  }
  task_finish(conn, srv, stream, &req, true, aborted, write_timeout_ms);
}

// Continues a parked response. A full socket parks the connection again. A
// finished response ends through task_tail with the keep-alive decision of the
// first write; a rejection's response ends like conn_reject_and_close, with a
// lingering close when the client may still be sending. expired gives up the
// write.
static void task_continue_write(chttpsvr_conn *conn, struct chttpserver *srv,
                                chttp1_stream *stream, bool expired) {
  stream->write_nonblocking = true;
  int r = expired ? 0 : send_response_continue(stream, conn);
  if (r < 0) {
    chttp1_stream_release(stream);
    conn_park_write(conn, srv);
    return;
  }
  bool keep_alive = r == 1 && conn->wp_keep_alive && !srv_draining(srv);
  conn_end_parked_write(conn);
  if (conn->wp_reject) {
    conn->wp_reject = false;
    chttp1_stream_release(stream);
    if (r == 1 && conn_request_bytes_may_remain(conn))
      conn_linger(conn);
    else
      conn_close(conn);
    release_in_flight(srv);
    return;
  }
  chttpsvr_req req;
  memset(&req, 0, sizeof(req));
  task_tail(conn, srv, stream, &req, true, keep_alive, r == 1);
}

// Continues a buffered request whose 100 Continue line ran into a full socket.
// The body bytes that came with the headers go into the stream first. Once the
// line is out the body is read and the request ends as in task_worker. If the
// socket is full again, the carry-over goes back onto conn and the connection
// parks again. If the line can't be finished (a limit ran out, the write
// failed, or a copy failed) the connection closes with no response, because
// part of the line may be on the wire.
static void task_continue_interim(chttpsvr_conn *conn, struct chttpserver *srv,
                                  bool expired) {
  chttp1_stream stream;
  bool carried;
  if (conn->tls)
    carried = chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls,
                                        conn->carry_over, conn->carry_over_len,
                                        conn->m_procs);
  else
    carried = chttp1_stream_prepare(&stream, conn->fd, conn->carry_over,
                                    conn->carry_over_len, conn->m_procs);
  if (!carried) {
    if (conn->tls)
      chttp1_stream_prepare_tls(&stream, conn->fd, conn->tls, NULL, 0,
                                conn->m_procs);
    else
      chttp1_stream_prepare(&stream, conn->fd, NULL, 0, conn->m_procs);
  }
  ccol__mem_free(conn->m_procs, conn->carry_over);
  conn->carry_over = NULL;
  conn->carry_over_len = 0;
  stream.fd_nonblocking = true;
  stream.write_nonblocking = true;
  int r = expired || !carried ? 0 : send_response_continue(&stream, conn);
  if (r < 0) {
    size_t lo_len = 0;
    char *lo = chttp1_stream_take_leftover(&stream, &lo_len);
    chttp1_stream_release(&stream);
    if (lo_len && !lo) {
      conn_end_parked_write(conn);
      conn->wp_interim = false;
      conn->interim_write_failed = true;
      chttpsvr_req req;
      memset(&req, 0, sizeof(req));
      task_tail(conn, srv, &stream, &req, false, false, false);
      return;
    }
    conn->carry_over = lo;
    conn->carry_over_len = lo_len;
    conn_park_write(conn, srv);
    return;
  }
  conn_end_parked_write(conn);
  conn->wp_interim = false;
  conn->write_deadline_set = false;
  conn->rate_active = false;
  if (r == 0) {
    conn->interim_write_failed = true;
    chttpsvr_req req;
    memset(&req, 0, sizeof(req));
    task_tail(conn, srv, &stream, &req, true, false, false);
    return;
  }
  task_read_body_and_finish(conn, srv, &stream, false);
}

// Listener: Accept and socket options.
//
// Shortest gap in seconds between two accept-failure log lines of one server,
// so a lasting failure can't flood the log.
#define CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC 5
// Most connections one listener dispatch accepts before it returns to the
// reactor.
#define CHTTPSVR_ACCEPT_BATCH_LIMIT 64u

// Tells whether an accept4() failure is about one pending connection rather
// than the listener, so the accept loop can go straight on to the next
// connection in the backlog. The list is ECONNABORTED plus the pending network
// errors and the firewall EPERM that accept(2) documents for Linux.
static bool accept_errno_is_transient(int e) {
  switch (e) {
    case ECONNABORTED:
    case EPROTO:
    case ENETDOWN:
    case ENETUNREACH:
    case ENOPROTOOPT:
    case EHOSTDOWN:
#ifdef ENONET
    case ENONET:
#endif
    case EHOSTUNREACH:
    case EOPNOTSUPP:
    case EPERM:
    case ETIMEDOUT:
#ifdef ENOSR
    case ENOSR:
#endif
    case ESOCKTNOSUPPORT:
    case EPROTONOSUPPORT:
      return true;
    default:
      return false;
  }
}

// Tells whether an accept4() failure comes from a process or system resource
// running out (EMFILE, ENFILE, ENOBUFS, ENOMEM), which a retry won't fix until
// something else frees it. It only labels the log line. The accept loop pauses
// the listener for every errno that isn't transient, because a level-triggered
// listen backlog would otherwise dispatch again and again; the idle sweep
// resumes it later, so the shared reactor thread never sleeps.
static bool accept_errno_is_resource_exhaustion(int e) {
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
// Test hooks that expose the two errno classifiers, since producing those
// errors for real depends on the environment.
bool chttpsvr__accept_errno_is_transient_for_tests(int e) {
  return accept_errno_is_transient(e);
}
bool chttpsvr__accept_errno_is_resource_exhaustion_for_tests(int e) {
  return accept_errno_is_resource_exhaustion(e);
}
#endif

// Rate limit for the accept loop's diagnostics: Returns true at most once per
// CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC for srv, and records the moment in
// srv->last_accept_err_log when it does. Only the listener's own dispatch
// touches those fields.
static bool listener_accept_err_log_gate(struct chttpserver *srv) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  if (srv->last_accept_err_log_set &&
      now.tv_sec - srv->last_accept_err_log.tv_sec <
          CHTTPSVR_ACCEPT_ERR_LOG_INTERVAL_SEC)
    return false;
  srv->last_accept_err_log = now;
  srv->last_accept_err_log_set = true;
  return true;
}

// Returns the text of err through the thread-safe strerror_r, in either form:
// the GNU one (glibc with _GNU_SOURCE) may return its own string and leave buf
// alone, the POSIX one fills buf.
static const char *errno_text(int err, char *buf, size_t len) {
#if defined(__GLIBC__) && defined(_GNU_SOURCE)
  return strerror_r(err, buf, len);
#else
  if (strerror_r(err, buf, len) != 0) snprintf(buf, len, "error %d", err);
  return buf;
#endif
}

// Logs an unexpected accept4() failure through the engine logger, rate limited
// by listener_accept_err_log_gate. The line marks the resource exhaustion
// cases, as a hint for the operator; anything else may be worth reporting as a
// bug. It uses errno_text because two listeners can log at the same moment on
// two reactor threads.
static void listener_log_accept_err_rate_limited(struct chttpserver *srv,
                                                 int err) {
  if (!listener_accept_err_log_gate(srv)) return;
  char errbuf[128];
  SRV_ENGINE_LOG(
      clog_warn, "accept() failed errno=%d (%s)%s", err,
      errno_text(err, errbuf, sizeof(errbuf)),
      accept_errno_is_resource_exhaustion(err) ? " [resource exhaustion]" : "");
}

#ifdef RUNNING_UNIT_TESTS
// Test counters for the two listener pauses: How often an allocation failure
// right after an accept paused a listener, and how often
// listener_pause_for_resource_pressure really paused one. The sweep clears the
// pause flag about once a second, so a test reads the difference of a counter
// over its own window instead of polling the flag.
static _Atomic size_t g_listener_alloc_failure_pause_count_for_tests = 0;
// Test hook: Gives the number of pauses after an allocation failure so far.
size_t chttpsvr__listener_alloc_failure_pause_count_for_tests(void) {
  return atomic_load(&g_listener_alloc_failure_pause_count_for_tests);
}

static _Atomic size_t g_listener_resource_pause_count_for_tests = 0;
// Test hook: Gives the number of successful resource-pressure pauses so far.
size_t chttpsvr__listener_resource_pause_count_for_tests(void) {
  return atomic_load(&g_listener_resource_pause_count_for_tests);
}
#endif

// Pauses srv's listener registration after accept failed for lack of a resource
// (or for any other reason the loop can't retry at once), so a backlog that
// can't be served doesn't make the level-triggered reactor call the accept loop
// again and again. It reads srv->listen_reg under srv->mutex. Only when the
// pause succeeds does it set srv->listener_paused_for_resource_pressure, which
// tells the idle sweep to resume the listener on a later tick. With no
// registration, or a failed pause (which only a concurrent remove could cause,
// and listener_dispatch_pins prevents that), it does nothing.
static void listener_pause_for_resource_pressure(struct chttpserver *srv) {
  ccol__mutex_lock(srv->mutex);
  ceventloop_reg lreg = srv->listen_reg;
  ccol__mutex_unlock(srv->mutex);
  if (!lreg) return;
  if (ceventloop_pause(srv_engine_bundler.reactor, lreg) == ccol_success) {
    atomic_store(&srv->listener_paused_for_resource_pressure, true);
#ifdef RUNNING_UNIT_TESTS
    atomic_fetch_add(&g_listener_resource_pause_count_for_tests, 1);
#endif
  }
}

// Handles a failed allocation right after a successful accept (conn_create or
// ctls_conn_create_server gave NULL): It logs what failed, through the same
// rate-limit gate as the accept errors, and pauses the listener. Without the
// pause a custom allocator that has run dry would make the loop accept, fail,
// close and repeat for as long as clients keep connecting.
static void listener_log_and_pause_for_alloc_failure(struct chttpserver *srv,
                                                     const char *what) {
  if (listener_accept_err_log_gate(srv))
    SRV_ENGINE_LOG(clog_warn, "%s failed after accept()", what);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_alloc_failure_pause_count_for_tests, 1);
#endif
  listener_pause_for_resource_pressure(srv);
}

#ifdef RUNNING_UNIT_TESTS
// Test counter of accepted unix:// connections whose socket buffers
// apply_accepted_socket_options raised, with its reader.
static _Atomic size_t g_accepted_sockbuf_raise_count_for_tests = 0;
size_t chttpsvr__accepted_sockbuf_raise_count_for_tests(void) {
  return atomic_load(&g_accepted_sockbuf_raise_count_for_tests);
}
#endif

// Sets the socket options of a freshly accepted connection. The fd is already
// non-blocking from accept, so no fcntl is needed. A TCP socket gets
// TCP_NODELAY and, when enable_keepalive is set, SO_KEEPALIVE; its buffer sizes
// stay as the kernel chose them, because setting SO_SNDBUF or SO_RCVBUF turns
// Linux's buffer autotuning off and caps the throughput of connections with a
// long round trip. A unix:// socket has no autotuning and can get small
// defaults, so both of its buffers are raised to at least 128 KiB.
static void apply_accepted_socket_options(int fd, bool is_unix,
                                          bool enable_keepalive) {
  int one = 1;
  if (!is_unix) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    if (enable_keepalive)
      setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    return;
  }
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_accepted_sockbuf_raise_count_for_tests, 1);
#endif
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
// Test counter of listener dispatches in the whole process. A test that holds a
// server at max_connections reads its difference over a window: A paused
// listener adds a few dispatches, a busy-looping one adds thousands.
static _Atomic size_t g_listener_dispatch_count_for_tests = 0;
size_t chttpsvr__listener_dispatch_count_for_tests(void) {
  return atomic_load(&g_listener_dispatch_count_for_tests);
}

// Test record of the most connections one dispatch of the accept loop accepted,
// across all servers since the last reset, which a test compares with
// CHTTPSVR_ACCEPT_BATCH_LIMIT. The three functions below read it, reset it and
// raise it with a compare-and-swap loop.
static _Atomic size_t g_listener_max_accepts_per_dispatch_for_tests = 0;
size_t chttpsvr__listener_max_accepts_per_dispatch_for_tests(void) {
  return atomic_load(&g_listener_max_accepts_per_dispatch_for_tests);
}
void chttpsvr__listener_max_accepts_per_dispatch_reset_for_tests(void) {
  atomic_store(&g_listener_max_accepts_per_dispatch_for_tests, 0);
}
static void listener_note_accepts_for_tests(size_t n) {
  size_t cur = atomic_load(&g_listener_max_accepts_per_dispatch_for_tests);
  while (n > cur &&
         !atomic_compare_exchange_weak(
             &g_listener_max_accepts_per_dispatch_for_tests, &cur, n)) {
  }
}

// Test hook state for a forced accept error. When the armed server is the one
// dispatching, its next accept result is thrown away (a real connection is
// closed unused) and replaced by a failure with the armed errno, then both
// fields are cleared. It names one server because the reactor is shared by the
// whole process, and another server's dispatch would otherwise take the forced
// errno. It lets a test check how the loop treats an errno that is hard to
// produce for real, such as EBADF or EINVAL.
static _Atomic(struct chttpserver *) g_force_next_accept_errno_for_tests_srv =
    NULL;
static _Atomic int g_force_next_accept_errno_for_tests_val = 0;
// Test hook: Arms the forced accept errno for the server behind h.
void chttpsvr__force_next_accept_errno_for_tests(chttpsvr h, int errno_val) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  atomic_store(&g_force_next_accept_errno_for_tests_val, errno_val);
  atomic_store(&g_force_next_accept_errno_for_tests_srv, raw);
}
#endif

// The accept loop of a listener. It accepts at most CHTTPSVR_ACCEPT_BATCH_LIMIT
// connections per dispatch and goes back to the reactor, so a flood of connects
// can't keep the single reactor thread from serving established connections;
// the level-triggered registration brings it back while the backlog isn't
// empty. Each accepted fd raises srv->current_connections, gets its socket
// options, a connection object and, with TLS, a server TLS object, and is
// pumped once.
// At max_connections it pauses the listener instead of returning, since a plain
// return would be dispatched again at once; it then checks the count again
// after the pause and resumes if a slot freed meanwhile, because the sweep's
// resume may have run just before the pause. Any accept error other than
// EAGAIN, EINTR or a per-connection transient one pauses the listener as well,
// and so does a failed allocation. The caller holds a listener_dispatch_pins
// pin, which keeps lfd open for the whole call.
static void listener_on_readable_impl(struct chttpserver *srv, int lfd) {
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_listener_dispatch_count_for_tests, 1);
  size_t accepted_for_tests = 0;
#endif

  for (unsigned batch = 0; batch < CHTTPSVR_ACCEPT_BATCH_LIMIT; batch++) {
#ifdef RUNNING_UNIT_TESTS
    listener_note_accepts_for_tests(accepted_for_tests);
#endif
    size_t cap = atomic_load(&srv->max_connections);
    if (cap && atomic_load(&srv->current_connections) >= cap) {
      ccol__mutex_lock(srv->mutex);
      ceventloop_reg lreg = srv->listen_reg;
      ccol__mutex_unlock(srv->mutex);
      if (lreg &&
          ceventloop_pause(srv_engine_bundler.reactor, lreg) == ccol_success) {
        if (atomic_load(&srv->current_connections) < cap)
          ceventloop_resume(srv_engine_bundler.reactor, lreg);
      }
      return;
    }

    struct sockaddr_storage ss;
    socklen_t slen = sizeof(ss);
    int cfd = ccol_accept_nb(lfd, (struct sockaddr *)&ss, &slen);
    int accept_errno = cfd < 0 ? errno : 0;
#ifdef RUNNING_UNIT_TESTS
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
#endif
    if (cfd < 0) {
      if (accept_errno == EWOULDBLOCK || accept_errno == EAGAIN) return;
      if (accept_errno == EINTR) continue;
      if (accept_errno_is_transient(accept_errno)) continue;
      listener_log_accept_err_rate_limited(srv, accept_errno);
      listener_pause_for_resource_pressure(srv);
      return;
    }
    atomic_fetch_add(&srv->current_connections, 1);
#ifdef RUNNING_UNIT_TESTS
    accepted_for_tests++;
#endif
    apply_accepted_socket_options(cfd, atomic_load(&srv->is_unix_socket),
                                  srv->enable_keepalive);

    ccol__call_once(srv_parser_bundler.once, init_parser_settings);
    chttpsvr_conn *conn = conn_create(srv, cfd, &srv_parser_bundler.settings);
    if (!conn) {
      close(cfd);
      atomic_fetch_sub(&srv->current_connections, 1);
      listener_log_and_pause_for_alloc_failure(srv, "connection allocation");
      return;
    }

    if (srv->tls_ctx) {
      conn->tls = ctls_conn_create_server(srv->tls_ctx, cfd, conn, NULL);
      if (!conn->tls) {
        conn_free(conn);
        listener_log_and_pause_for_alloc_failure(srv, "TLS connection setup");
        return;
      }
      conn->state = CONN_ST_TLS_HANDSHAKE;
    } else {
      conn->state = CONN_ST_READING_HEADERS;
    }
    conn_pump(conn);
  }
#ifdef RUNNING_UNIT_TESTS
  listener_note_accepts_for_tests(accepted_for_tests);
#endif
}

// The listener's on_readable callback. It pins srv through
// srv->listener_dispatch_pins for the whole run of listener_on_readable_impl
// and drops the pin last, broadcasting resolve_cv so chttpsvr_stop_internal's
// wait can go on. The pin and the read of srv->listen_fd happen under
// srv->mutex, which orders them against the stop that clears listen_fd and
// waits for the pins. A dispatch whose fd differs from listen_fd (the server
// stopped, or restarted on a new fd) returns at once: It must not accept on a
// listener it wasn't registered for. srv's memory itself stays valid through
// the lifetime_refs reference the registration holds.
static void listener_on_readable(ceventloop loop, ceventloop_reg reg,
                                 cselectable *sel, void *arg) {
  (void)loop;
  (void)reg;
  struct chttpserver *srv = (struct chttpserver *)arg;

#ifdef RUNNING_UNIT_TESTS
  listener_dispatch_race_hook_wait_if_armed(true);
#endif

  ccol__mutex_lock(srv->mutex);
  atomic_fetch_add(&srv->listener_dispatch_pins, 1);
  int lfd = atomic_load(&srv->listen_fd);
  ccol__mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  listener_dispatch_race_hook_wait_if_armed(false);
#endif

  if (lfd >= 0 && lfd == sel->fd) listener_on_readable_impl(srv, lfd);

  ccol__mutex_lock(srv->mutex);
  atomic_fetch_sub(&srv->listener_dispatch_pins, 1);
  ccol__cond_var_broadcast(srv->resolve_cv);
  ccol__mutex_unlock(srv->mutex);
}

// The listener registration's on_removed callback, which runs once no dispatch
// of it can still run. It drops the srv->lifetime_refs reference chttpsvr_start
// took for the registration and runs chttpsvr_finish_destroy when that was the
// last one.
static void listener_on_removed(void *arg) {
  struct chttpserver *srv = (struct chttpserver *)arg;
  if (atomic_fetch_sub(&srv->lifetime_refs, 1) == 1)
    chttpsvr_finish_destroy(srv);
}

// Listen sockets. A host that starts with this prefix names a Unix domain
// socket path.
#define CHTTPSVR_UNIX_PREFIX "unix://"

// Removes a unix:// listener's socket file and releases what ub holds (path,
// directory fd). The file is unlinked only while the name still points at the
// socket this server bound (same dev and ino), so a file someone else put there
// meanwhile stays. The gap between that check and the unlink can't be closed,
// since POSIX has no unlink by inode.
static void unix_bind_cleanup(chttpsvr_unix_bind *ub, ccol_memmgmt_procs *mp) {
  if (!ub->path) return;
  if (ub->dir_fd >= 0) {
    struct stat st;
    if (fstatat(ub->dir_fd, ub->base, &st, AT_SYMLINK_NOFOLLOW) == 0 &&
        S_ISSOCK(st.st_mode) && st.st_dev == ub->dev && st.st_ino == ub->ino)
      unlinkat(ub->dir_fd, ub->base, 0);
    close(ub->dir_fd);
  }
  ccol__mem_free(mp, ub->path);
  ub->path = NULL;
  ub->base = NULL;
  ub->dir_fd = -1;
}

// Decides, before the bind, what to do with a file that already exists at a
// unix:// path. It returns true when the bind may go ahead: Nothing is there,
// or a stale socket that no one listens on was found and removed (only while
// the name still points at the socket the probe saw). It returns false with
// errno set when the start must fail and leave the file alone: EEXIST for
// something that isn't a socket, so a configuration typo never deletes a
// regular file, and EADDRINUSE when a live listener answers. The probe is a
// non-blocking connect from a scratch socket; ECONNREFUSED means a socket file
// with no listener.
static bool unix_path_prepare(const struct sockaddr_un *addr) {
  struct stat st;
  if (lstat(addr->sun_path, &st) != 0) return errno == ENOENT;
  if (!S_ISSOCK(st.st_mode)) {
    SRV_ENGINE_LOG(clog_error,
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
    struct stat again;
    if (lstat(addr->sun_path, &again) == 0 && S_ISSOCK(again.st_mode) &&
        again.st_dev == st.st_dev && again.st_ino == st.st_ino)
      unlink(addr->sun_path);
    return true;
  }
  if (e == ENOENT) return true;
  if (e == 0 || e == EAGAIN || e == EINPROGRESS) {
    SRV_ENGINE_LOG(clog_error,
                   "unix socket path %s: another server already listens "
                   "on it",
                   addr->sun_path);
    e = EADDRINUSE;
  }
  errno = e;
  return false;
}

// Open flags of a unix:// path's directory descriptor. The descriptor only
// names the directory for the *at() calls, so O_PATH is enough where the system
// has it (no read permission needed); macOS doesn't.
#if defined(O_PATH)
#define UNIX_DIR_OPEN_FLAGS (O_PATH | O_DIRECTORY | O_CLOEXEC)
#else
#define UNIX_DIR_OPEN_FLAGS (O_RDONLY | O_DIRECTORY | O_CLOEXEC)
#endif

// Opens the directory that holds path, for chttpsvr_unix_bind.dir_fd, and
// points *base at the last component of path. It gives the fd, or -1 with errno
// set (EINVAL for a path that ends in a slash).
static int unix_open_dir(const char *path, const char **base) {
  const char *slash = strrchr(path, '/');
  if (!slash) {
    *base = path;
    return open(".", UNIX_DIR_OPEN_FLAGS);
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
  return open(dir, UNIX_DIR_OPEN_FLAGS);
}

#ifdef RUNNING_UNIT_TESTS
// Test record of the errno of the last failed listen socket setup, with its
// reader.
static _Atomic int g_last_listen_errno_for_tests = 0;
int chttpsvr__last_listen_errno_for_tests(void) {
  return atomic_load(&g_last_listen_errno_for_tests);
}
#endif

// Creates, binds and listens on a unix:// socket at path. It refuses an empty
// path (EINVAL) and one too long for sun_path (ENAMETOOLONG), handles an
// existing file through unix_path_prepare, and records the directory fd, the
// base name and the dev and ino of the new socket in *ub so that the stop
// removes only this socket. If the new socket can't be identified after bind,
// the start fails and the file stays as a stale socket that the next start
// replaces. It gives the listening fd, or -1 with errno set and nothing left
// allocated.
static int make_unix_listen_socket(const char *path, chttpsvr_unix_bind *ub,
                                   ccol_memmgmt_procs *mp) {
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

  chttpsvr_unix_bind b = {.path = NULL, .base = NULL, .dir_fd = -1};
  b.path = ccol_strdup(mp, path);
  if (!b.path) return -1;
  const char *base_in_cfg = NULL;
  b.dir_fd = unix_open_dir(path, &base_in_cfg);
  if (b.dir_fd < 0) {
    ccol__mem_free(mp, b.path);
    return -1;
  }
  b.base = b.path + (base_in_cfg - path);

  int fd = -1;
  if (unix_path_prepare(&addr)) fd = ccol_socket_nb(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) {
    close(b.dir_fd);
    ccol__mem_free(mp, b.path);
    return -1;
  }
  struct stat st;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
    close(fd);
    close(b.dir_fd);
    ccol__mem_free(mp, b.path);
    return -1;
  }
  if (fstatat(b.dir_fd, b.base, &st, AT_SYMLINK_NOFOLLOW) != 0 ||
      !S_ISSOCK(st.st_mode)) {
    close(fd);
    close(b.dir_fd);
    ccol__mem_free(mp, b.path);
    return -1;
  }
  b.dev = st.st_dev;
  b.ino = st.st_ino;
  if (listen(fd, SOMAXCONN) != 0) {
    close(fd);
    unix_bind_cleanup(&b, mp);
    return -1;
  }
  *ub = b;
  return fd;
}

// Creates, binds and listens on one TCP socket with SO_REUSEADDR, and
// SO_REUSEPORT when asked and available. v6only is -1 to keep the system's
// IPV6_V6ONLY default, or 0 or 1 to set it. It gives the fd, or -1 with errno
// set by the step that failed.
static int bind_tcp(int family, const struct sockaddr *sa, socklen_t salen,
                    bool enable_reuseport, int v6only) {
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

// Tells whether an IPv6 socket here can take IPv4 clients too; OpenBSD refuses
// to turn IPV6_V6ONLY off, so it can't. make_listen_socket asks it to decide
// whether to fall back to 0.0.0.0. When the probe socket fails for a reason
// other than missing IPv6 (no descriptors or memory left), it says yes, so the
// caller reports its own bind error instead of falling back.
static bool ipv6_dual_stack_supported(void) {
  int fd = ccol_socket_nb(AF_INET6, SOCK_STREAM, 0);
  if (fd < 0) return errno != EAFNOSUPPORT && errno != EPROTONOSUPPORT;
  int off = 0;
  bool ok = setsockopt(fd, IPPROTO_IPV6, IPV6_V6ONLY, &off, sizeof(off)) == 0;
  close(fd);
  return ok;
}

// Opens the listener that host names, following Go's net listener. NULL or ""
// gives one dual-stack socket on [::] (IPv6 only with ipv6_only), with a
// fallback to 0.0.0.0 when the host has no IPv6 or the system no dual-stack
// sockets. A literal address binds exactly that address, and "[addr]" binds the
// address inside the brackets (a bracketed name is refused). A name binds its
// first IPv4 address, else its first IPv6 one (the reverse with ipv6_only), and
// only that one, so its bind error is the answer. "unix://path" goes to
// make_unix_listen_socket.
// It sets *is_unix and fills *ub for a unix:// listener. It gives the fd, or -1
// with errno set; a name that doesn't resolve is logged with the resolver's
// message and gives EADDRNOTAVAIL.
static int make_listen_socket(const char *host, uint16_t port,
                              bool enable_reuseport, bool ipv6_only,
                              bool *is_unix, chttpsvr_unix_bind *ub,
                              ccol_memmgmt_procs *mp) {
  ub->path = NULL;
  ub->base = NULL;
  ub->dir_fd = -1;
  *is_unix = host && strncmp(host, CHTTPSVR_UNIX_PREFIX,
                             strlen(CHTTPSVR_UNIX_PREFIX)) == 0;
  if (*is_unix)
    return make_unix_listen_socket(host + strlen(CHTTPSVR_UNIX_PREFIX), ub, mp);

  int v6only_explicit = ipv6_only ? 1 : -1;
  if (!host || !*host) {
    struct sockaddr_in6 a6;
    memset(&a6, 0, sizeof(a6));
    a6.sin6_family = AF_INET6;
    a6.sin6_port = htons(port);
    a6.sin6_addr = in6addr_any;
    int fd = bind_tcp(AF_INET6, (struct sockaddr *)&a6, sizeof(a6),
                      enable_reuseport, ipv6_only ? 1 : 0);
    if (fd >= 0 || ipv6_only) return fd;
    int e = errno;
    if (e != EAFNOSUPPORT && e != EADDRNOTAVAIL && e != EPROTONOSUPPORT &&
        ipv6_dual_stack_supported()) {
      errno = e;
      return -1;
    }
    struct sockaddr_in a4;
    memset(&a4, 0, sizeof(a4));
    a4.sin_family = AF_INET;
    a4.sin_port = htons(port);
    a4.sin_addr.s_addr = htonl(INADDR_ANY);
    return bind_tcp(AF_INET, (struct sockaddr *)&a4, sizeof(a4),
                    enable_reuseport, -1);
  }

  char unbracketed[256];
  size_t host_len = strlen(host);
  bool bracketed = host[0] == '[';
  if (bracketed) {
    if (host_len < 3 || host[host_len - 1] != ']' ||
        host_len - 2 >= sizeof(unbracketed)) {
      SRV_ENGINE_LOG(clog_error, "listen host \"%s\" is malformed", host);
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
      SRV_ENGINE_LOG(clog_error,
                     "listen host \"%s\" does not resolve: errno=%d", host, e);
    else
      SRV_ENGINE_LOG(clog_error, "listen host \"%s\" does not resolve: %s",
                     host, gai != 0 ? gai_strerror(gai) : "no address");
    if (res) freeaddrinfo(res);
    errno = EADDRNOTAVAIL;
    return -1;
  }
  const int first_family = ipv6_only ? AF_INET6 : AF_INET;
  const int second_family = ipv6_only ? AF_INET : AF_INET6;
  struct addrinfo *pick = NULL;
  for (struct addrinfo *rp = res; rp && !pick; rp = rp->ai_next)
    if (rp->ai_family == first_family) pick = rp;
  for (struct addrinfo *rp = res; rp && !pick; rp = rp->ai_next)
    if (rp->ai_family == second_family) pick = rp;
  int fd = -1;
  if (pick) {
    fd = bind_tcp(pick->ai_family, pick->ai_addr, pick->ai_addrlen,
                  enable_reuseport, v6only_explicit);
  } else {
    errno = EAFNOSUPPORT;
  }
  int e = errno;
  freeaddrinfo(res);
  errno = e;
  return fd;
}

// Reads the port and numeric local address of the TCP listener fd for
// chttpsvr_bound_port() and chttpsvr_bound_address(); with port 0 the system
// chooses, so the socket is the only place to learn it from. It gives false
// with errno set when getsockname fails or the address family isn't IPv4 or
// IPv6.
static bool listen_sockname(int fd, uint16_t *port_out, char *host,
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

// The registry of router shells. A shell is the chttpsvr_router struct itself,
// while its contents (prefix, middleware list, routes) are separate
// allocations. A caller's chttpsvr_router pointer has no generation of its own,
// and the public router calls must read router->owner before any resolve can
// pin the server, so the shell is never freed while the process may still read
// it. destroy_router stores CHTTPSVR_NULL into owner and frees only the
// contents, so a reader sees a live handle or the sentinel, never freed memory.
// Shells come from the default allocator, since they outlive the server's
// allocator, and the registry lets the exit-time destructor free them once.
static struct {
  ccol__mutex mutex;
  ccol__once_flag once;
  // A plain realloc'd array of every shell still allocated.
  chttpsvr_router **shells;
  size_t count;
  size_t capacity;
  // Set once the exit-time destructor has run. From then on a server's destroy
  // frees all its shells, and the array goes with the last shell.
  bool finalized;
} chttpsvr_router_shell_registry = {0};

// Initializes the registry mutex, once; a failure is fatal.
static void chttpsvr_router_shell_registry_init_globals(void) {
  if (ccol__mutex_init(chttpsvr_router_shell_registry.mutex) != 0)
    ccol__fatal_err(
        "chttpsvr router shell registry: failed to initialize mutex");
}

// Adds the new shell r to the registry under its mutex and records its index in
// r->shell_idx. When the array can't grow, r gets SIZE_MAX and isn't tracked;
// the only cost is a still-reachable shell at exit.
static void chttpsvr_router_shell_register(chttpsvr_router *r) {
  ccol__call_once(chttpsvr_router_shell_registry.once,
                  chttpsvr_router_shell_registry_init_globals);
  ccol__mutex_lock(chttpsvr_router_shell_registry.mutex);
  if (chttpsvr_router_shell_registry.count ==
      chttpsvr_router_shell_registry.capacity) {
    size_t new_cap = doubling_growth_cap(
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
  r->shell_idx = SIZE_MAX;
  if (chttpsvr_router_shell_registry.count <
      chttpsvr_router_shell_registry.capacity) {
    r->shell_idx = chttpsvr_router_shell_registry.count;
    chttpsvr_router_shell_registry
        .shells[chttpsvr_router_shell_registry.count++] = r;
  }
  ccol__mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

// Takes r out of the registry by swapping the last shell into its place, and
// frees the array once the destructor has run and the registry is empty. The
// caller holds the registry mutex and frees r itself.
static void chttpsvr_router_shell_unregister_locked(chttpsvr_router *r) {
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

// Frees the shells of a destroyed server's routers that no caller can reach any
// more: Always the root router, which is never handed out, and every router
// once the exit-time destructor has run. A sub-router of a server destroyed
// earlier keeps its shell, so a stale pointer to it stays safe to pass. Every
// router has already gone through destroy_router. It takes the registry mutex.
static void chttpsvr_router_shells_release(chttpsvr_router **routers,
                                           size_t count,
                                           chttpsvr_router *root) {
  ccol__call_once(chttpsvr_router_shell_registry.once,
                  chttpsvr_router_shell_registry_init_globals);
  ccol__mutex_lock(chttpsvr_router_shell_registry.mutex);
  bool all = chttpsvr_router_shell_registry.finalized;
  for (size_t i = 0; i < count; i++) {
    chttpsvr_router *r = routers[i];
    if (r != root && !all) continue;
    chttpsvr_router_shell_unregister_locked(r);
    ccol__default_mem_free(r);
  }
  ccol__mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

// Frees at exit every shell whose owner is CHTTPSVR_NULL (its router was
// destroyed, or its registration failed half way), so valgrind reports no
// still-reachable shells. A shell of a server the application never destroyed
// stays, packed at the front of the array, because that server's threads keep
// running past this destructor and find_route reads the shell; the server's own
// destroy frees it later. It sets finalized and frees the array when nothing is
// kept. The once guard comes first because a process may never have created a
// server.
__attribute__((destructor)) static void cleanup_chttpsvr_router_shells(void) {
  ccol__call_once(chttpsvr_router_shell_registry.once,
                  chttpsvr_router_shell_registry_init_globals);
  ccol__mutex_lock(chttpsvr_router_shell_registry.mutex);
  size_t kept = 0;
  for (size_t i = 0; i < chttpsvr_router_shell_registry.count; i++) {
    chttpsvr_router *r = chttpsvr_router_shell_registry.shells[i];
    if (atomic_load(&r->owner) == CHTTPSVR_NULL) {
      r->shell_idx = SIZE_MAX;
      ccol__default_mem_free(r);
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
  ccol__mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

#if CCOL_FORK_SAFETY_REQUIRED
// The fork prepare handler. fork() copies only the calling thread, so a lock
// another thread holds would stay locked in the child for good; this takes
// every lock of the module first: The slot table, servers_bundler then
// srv_engine_bundler (one fixed order, so a future change that nests them has
// one order to follow), the router shell registry, the sweep's wake_mu leaf
// lock, and for every in_use server its mutex, idle_mutex, diverted_mutex,
// wait_mutex and routes_lock for write. Each registry's once guard runs first,
// so a fork before the module was used doesn't lock uninitialized memory. The
// pools, the reactor and the loggers are protected by their own modules'
// handlers.
static void chttpsvr_atfork_prepare(void) {
#ifdef RUNNING_UNIT_TESTS
  ccol__atfork_order_record(ccol__atfork_module_chttpserver);
#endif
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);

  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  ccol__mutex_lock(srv_engine_bundler.mutex);

  ccol__call_once(chttpsvr_router_shell_registry.once,
                  chttpsvr_router_shell_registry_init_globals);
  ccol__mutex_lock(chttpsvr_router_shell_registry.mutex);
  ccol__mutex_lock(idle_sweep_bundler.wake_mu);

  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot *slot =
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    ccol__mutex_lock(slot->ptr->mutex);
    ccol__mutex_lock(slot->ptr->idle_mutex);
    ccol__mutex_lock(slot->ptr->diverted_mutex);
    ccol__mutex_lock(slot->ptr->wait_mutex);
    ccol__rw_lock_wrlock(slot->ptr->routes_lock);
  }
}

// Releases every lock chttpsvr_atfork_prepare took; the parent and the child
// handlers share it. Plain mutexes are unlocked in both, but in the child
// routes_lock is initialized again, because a glibc rwlock's write lock tracks
// its owner's TID and an unlock from the child's new thread releases nothing.
// In the child it also clears state that names threads which don't exist there.
// Every server's pending_resolve_count, servers_bundler_pins and
// listener_dispatch_pins go to 0, since destroy waits on them without a limit
// (in_flight_requests stays: Its wait is bounded, and a reset could free a
// connection a worker really holds). A server caught in a teardown
// (CHTTPSVR_QS_QUIESCING) is marked quiesced and unregistered with the locked
// variant, as servers_bundler.mutex is held; retrying its half-done steps could
// free things twice, so what it held leaks. A lifecycle of STARTING or STOPPING
// goes back to IDLE, or the handle would be unusable; an orphaned listener fd
// may make a later start fail with EADDRINUSE. The stop watcher's flags and
// requests, reaper_joinable, the sweep's running flag and its condition
// variable, and srv_engine_bundler.stopping are reset, so a later start, stop
// or wait in the child neither joins a vanished thread nor waits for a
// broadcast that never comes.
static void chttpsvr_atfork_release_impl(bool is_child) {
  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  for (size_t i = 0; i < n; i++) {
    chttpsvr_slot *slot =
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, i);
    if (!slot->in_use) continue;
    struct chttpserver *srv = slot->ptr;

    if (is_child) {
      atomic_store(&srv->pending_resolve_count, (size_t)0);
      atomic_store(&srv->servers_bundler_pins, (size_t)0);
      atomic_store(&srv->listener_dispatch_pins, (size_t)0);
      if (ccol__rw_lock_reinit_in_child(srv->routes_lock) != 0)
        ccol__fatal_err(
            "chttpsvr atfork release: failed to reinit routes_lock");

      switch (srv->quiesce_state) {
        case CHTTPSVR_QS_QUIESCING: {
          srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
          srv->quiesce_waiters = 0;
          servers_unregister_locked(srv);
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
      ccol__rw_lock_unlock(srv->routes_lock);
    }
    ccol__mutex_unlock(srv->wait_mutex);
    ccol__mutex_unlock(srv->diverted_mutex);
    ccol__mutex_unlock(srv->idle_mutex);
    ccol__mutex_unlock(srv->mutex);
  }

  if (is_child) {
    g_engine_stop_watcher.started = false;
    atomic_store(&g_engine_stop_watcher.ready, false);
    atomic_store(&g_engine_stop_watcher.join_reaper_requested, false);
    atomic_store(&g_engine_stop_watcher.stop_requested, (uintptr_t)0);

    srv_engine_bundler.reaper_joinable = false;
    idle_sweep_bundler.running = false;
    if (idle_sweep_init_wake_cv() != 0)
      ccol__fatal_err(
          "chttpserver atfork child: failed to reinit the sweep condition "
          "variable");

    srv_engine_bundler.stopping = false;
  }

  ccol__mutex_unlock(idle_sweep_bundler.wake_mu);
  ccol__mutex_unlock(chttpsvr_router_shell_registry.mutex);
  ccol__mutex_unlock(servers_bundler.mutex);
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
}

// The fork parent handler: Releases the locks the prepare handler took.
static void chttpsvr_atfork_release(void) {
  chttpsvr_atfork_release_impl(false);
}

// The fork child handler: Releases the locks and resets the inherited state of
// every server and thread. It runs as part of fork()'s return, before any
// application code in the child can reach a server.
static void chttpsvr_atfork_child_release(void) {
  chttpsvr_atfork_release_impl(true);
}
#endif

// Counts the '/'-separated segments of a router prefix, which
// find_route uses to order routers by how specific their mount is. The prefix
// has no trailing or repeated slashes, so each '/' starts one segment; "" (the
// root) and "/" have none.
static int count_prefix_segments(const char *prefix) {
  size_t len = strlen(prefix);
  if (len <= 1) return 0;
  int count = 0;
  for (size_t i = 0; i < len; i++)
    if (prefix[i] == '/') count++;
  return count;
}

// Creates a router for srv with prefix (NULL means the root's ""), trailing
// slashes removed, and registers its shell. The shell comes from the default
// allocator because it outlives the server and mp (see
// chttpsvr_router_shell_registry); the prefix comes from mp. owner starts as
// CHTTPSVR_NULL. It gives NULL when an allocation fails.
static chttpsvr_router *create_router(struct chttpserver *srv,
                                      const char *prefix,
                                      ccol_memmgmt_procs *mp) {
  chttpsvr_router *r =
      (chttpsvr_router *)ccol__default_mem_calloc(1, sizeof(chttpsvr_router));
  if (!r) return NULL;
  r->prefix = ccol_strdup(mp, prefix ? prefix : "");
  if (!r->prefix) {
    ccol__default_mem_free(r);
    return NULL;
  }
  size_t plen = strlen(r->prefix);
  while (plen > 1 && r->prefix[plen - 1] == '/') r->prefix[--plen] = '\0';
  r->prefix_len = plen;
  r->prefix_seg_count = count_prefix_segments(r->prefix);
  r->srv = srv;
  r->m_procs = mp;
  atomic_store(&r->owner, CHTTPSVR_NULL);
  chttpsvr_router_shell_register(r);
  return r;
}

// Frees the contents of router r (middleware nodes, routes, prefix) with mp and
// leaves the shell allocated. It stores CHTTPSVR_NULL into r->owner first, so a
// reader that comes later sees the sentinel instead of racing the frees;
// chttpsvr_finish_destroy calls it only after the pending_resolve_count wait,
// so no reader is inside router_add_route or router_add_mw.
static void destroy_router(chttpsvr_router *r, ccol_memmgmt_procs *mp) {
  atomic_store(&r->owner, CHTTPSVR_NULL);
  chttpsvr_mw_node *mw = r->mw_head;
  while (mw) {
    chttpsvr_mw_node *next = mw->next;
    ccol__mem_free(mp, mw);
    mw = next;
  }
  r->mw_head = r->mw_tail = NULL;
  r->mw_count = 0;
  for (size_t i = 0; i < r->route_count; i++) {
    free_route_data(r->routes[i], mp);
    ccol__mem_free(mp, r->routes[i]);
  }
  ccol__mem_free(mp, r->routes);
  r->routes = NULL;
  r->route_count = r->route_cap = 0;
  ccol__mem_free(mp, r->prefix);
  r->prefix = NULL;
  r->srv = NULL;
}

// Compiles pattern and appends a route for method to router, taking
// srv->routes_lock for write around the append. A method above CHTTP_ANY is
// refused, since find_route keeps methods as bits of a mask. It returns
// ccol_success, ccol_invalid_args, the error of compile_pattern, or
// ccol_not_enough_memory, and on failure frees the route it built.
static ccol_retval router_add_route(chttpsvr_router *router,
                                    chttp_method method, const char *pattern,
                                    chttpsvr_handler_fn fn, void *ctx,
                                    bool is_streaming) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  if ((unsigned int)method > (unsigned int)CHTTP_ANY) return ccol_invalid_args;

  ccol_memmgmt_procs *mp = router->m_procs;

  chttpsvr_route *rt =
      (chttpsvr_route *)ccol__mem_alloc(mp, sizeof(chttpsvr_route));
  if (!rt) return ccol_not_enough_memory;
  memset(rt, 0, sizeof(*rt));
  rt->method = method;
  rt->fn = fn;
  rt->ctx = ctx;
  rt->is_streaming = is_streaming;

  ccol_retval rv = compile_pattern(pattern, &rt->segs, &rt->seg_count,
                                   &rt->param_names, &rt->param_count, mp);
  if (rv != ccol_success) {
    ccol__mem_free(mp, rt);
    return rv;
  }

  ccol__rw_lock_wrlock(router->srv->routes_lock);
  if (router->route_count >= router->route_cap) {
    if (router->route_cap > (SIZE_MAX - 4) / 2 ||
        router->route_cap * 2 + 4 > SIZE_MAX / sizeof(chttpsvr_route *)) {
      ccol__rw_lock_unlock(router->srv->routes_lock);
      free_route_data(rt, mp);
      ccol__mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    size_t new_cap = router->route_cap * 2 + 4;
    chttpsvr_route **nr = (chttpsvr_route **)ccol__mem_realloc(
        mp, router->routes, new_cap * sizeof(chttpsvr_route *));
    if (!nr) {
      ccol__rw_lock_unlock(router->srv->routes_lock);
      free_route_data(rt, mp);
      ccol__mem_free(mp, rt);
      return ccol_not_enough_memory;
    }
    router->routes = nr;
    router->route_cap = new_cap;
  }
  router->routes[router->route_count++] = rt;
  if (rt->seg_count > router->max_route_seg_count)
    router->max_route_seg_count = rt->seg_count;
  ccol__rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

// Appends middleware fn with its ctx to the end of router's chain; chttpsvr_use
// and chttpsvr_router_use call it. It gives ccol_invalid_args for a NULL router
// or fn, ccol_not_enough_memory when the node can't be allocated, and
// ccol_not_permitted once the router holds CHTTPSVR_MAX_MW entries. The node is
// allocated before the write lock of srv->routes_lock is taken, and the chain
// and mw_count change only under that lock, so a request that walks the chain
// under the read lock never sees a half-linked node.
static ccol_retval router_add_mw(chttpsvr_router *router,
                                 chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  ccol_memmgmt_procs *mp = router->m_procs;
  chttpsvr_mw_node *node =
      (chttpsvr_mw_node *)ccol__mem_alloc(mp, sizeof(chttpsvr_mw_node));
  if (!node) return ccol_not_enough_memory;
  node->fn = fn;
  node->ctx = ctx;
  node->next = NULL;

  ccol__rw_lock_wrlock(router->srv->routes_lock);
  if (router->mw_count >= CHTTPSVR_MAX_MW) {
    ccol__rw_lock_unlock(router->srv->routes_lock);
    ccol__mem_free(mp, node);
    return ccol_not_permitted;
  }
  if (!router->mw_tail) {
    router->mw_head = router->mw_tail = node;
  } else {
    router->mw_tail->next = node;
    router->mw_tail = node;
  }
  router->mw_count++;
  ccol__rw_lock_unlock(router->srv->routes_lock);
  return ccol_success;
}

// Query parameters

// Splits a raw query string at '&' into key and value pairs, URL-decodes both
// and stores them, in order, in a new chttpsvr_qparams allocated from mp, which
// goes to *qp_out. A pair without '=' gets an empty value and "=v" an empty
// key; an empty pair (as in "a=1&&b=2") is skipped, and so is a pair whose key
// or value doesn't decode. Short keys and values are copied through a stack
// buffer before decoding. It gives ccol_success, or ccol_not_enough_memory
// after freeing everything it built.
static ccol_retval parse_qparams(const char *raw_query,
                                 chttpsvr_qparams **qp_out,
                                 ccol_memmgmt_procs *mp) {
  chttpsvr_qparams *qp =
      (chttpsvr_qparams *)ccol__mem_calloc(mp, 1, sizeof(chttpsvr_qparams));
  if (!qp) return ccol_not_enough_memory;
  qp->m_procs = mp;

  const char *p = raw_query;
  while (p && *p) {
    const char *amp = strchr(p, '&');
    size_t pair_len = amp ? (size_t)(amp - p) : strlen(p);
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
      key_sp = (char *)ccol__mem_alloc(mp, key_raw_len + 1);
      kh = true;
      if (!key_sp) goto oom;
    }
    memcpy(key_sp, p, key_raw_len);
    key_sp[key_raw_len] = '\0';
    char *key_decoded = (char *)ccol__mem_alloc(mp, key_raw_len + 1);
    if (!key_decoded) {
      if (kh) ccol__mem_free(mp, key_sp);
      goto oom;
    }
    ssize_t kl = decode_url_unsafe(key_decoded, key_sp);
    if (kh) ccol__mem_free(mp, key_sp);
    if (kl < 0) {
      ccol__mem_free(mp, key_decoded);
      goto next;
    }
    key_decoded[(size_t)kl] = '\0';

    char *val_decoded = NULL;
    if (val_raw) {
      char val_src[512];
      char *val_sp = val_src;
      bool vh = false;
      if (val_raw_len + 1 > sizeof(val_src)) {
        val_sp = (char *)ccol__mem_alloc(mp, val_raw_len + 1);
        vh = true;
        if (!val_sp) {
          ccol__mem_free(mp, key_decoded);
          goto oom;
        }
      }
      memcpy(val_sp, val_raw, val_raw_len);
      val_sp[val_raw_len] = '\0';
      val_decoded = (char *)ccol__mem_alloc(mp, val_raw_len + 1);
      if (!val_decoded) {
        if (vh) ccol__mem_free(mp, val_sp);
        ccol__mem_free(mp, key_decoded);
        goto oom;
      }
      ssize_t vl = decode_url_unsafe(val_decoded, val_sp);
      if (vh) ccol__mem_free(mp, val_sp);
      if (vl < 0) {
        ccol__mem_free(mp, val_decoded);
        ccol__mem_free(mp, key_decoded);
        goto next;
      }
      val_decoded[(size_t)vl] = '\0';
    } else {
      val_decoded = ccol_strdup(mp, "");
      if (!val_decoded) {
        ccol__mem_free(mp, key_decoded);
        goto oom;
      }
    }

    if (qp->count >= qp->cap) {
      if (qp->cap > (SIZE_MAX - 8) / 2 ||
          qp->cap * 2 + 8 > SIZE_MAX / sizeof(char *)) {
        ccol__mem_free(mp, key_decoded);
        ccol__mem_free(mp, val_decoded);
        goto oom;
      }
      size_t nc = qp->cap * 2 + 8;
      char **nk = (char **)ccol__mem_realloc(mp, qp->keys, nc * sizeof(char *));
      if (!nk) {
        ccol__mem_free(mp, key_decoded);
        ccol__mem_free(mp, val_decoded);
        goto oom;
      }
      qp->keys = nk;
      char **nv =
          (char **)ccol__mem_realloc(mp, qp->values, nc * sizeof(char *));
      if (!nv) {
        ccol__mem_free(mp, key_decoded);
        ccol__mem_free(mp, val_decoded);
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
    ccol__mem_free(mp, qp->keys[i]);
    ccol__mem_free(mp, qp->values[i]);
  }
  ccol__mem_free(mp, qp->keys);
  ccol__mem_free(mp, qp->values);
  ccol__mem_free(mp, qp);
  return ccol_not_enough_memory;
}

// Gives the parsed query parameters of req, parsing conn->raw_query on the
// first call only and caching the result in req->qparams. A request with no
// query gets an empty set. When parsing fails for lack of memory it sets
// req->qparams_parse_oom, which chttpsvr_req_query_oom reads to tell a failed
// allocation from an absent key, and gives an empty set or NULL.
// req->qparams_attempted stops a second attempt after a failure.
static chttpsvr_qparams *ensure_qparams(chttpsvr_req *req) {
  chttpsvr_conn *conn = req->conn;
  if (req->qparams) return req->qparams;
  if (req->qparams_attempted) return NULL;
  req->qparams_attempted = true;

  if (!conn->raw_query || !*conn->raw_query) {
    chttpsvr_qparams *qp = (chttpsvr_qparams *)ccol__mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams));
    if (!qp) {
      req->qparams_parse_oom = true;
      return NULL;
    }
    qp->m_procs = req->m_procs;
    req->qparams = qp;
    return qp;
  }
  if (parse_qparams(conn->raw_query, &req->qparams, req->m_procs) !=
      ccol_success) {
    req->qparams_parse_oom = true;
    chttpsvr_qparams *qp = (chttpsvr_qparams *)ccol__mem_calloc(
        req->m_procs, 1, sizeof(chttpsvr_qparams));
    if (qp) {
      qp->m_procs = req->m_procs;
      req->qparams = qp;
      return qp;
    }
    return NULL;
  }
  return req->qparams;
}

// Public API

// Creates a stopped server: Its copy of mprocs, a logger derived from cl (or a
// fatal-only logger on stderr when cl is CLOG_NULL), its mutexes, condition
// variables and routes lock, the root router, and a slot in the handle table.
// It gives the new handle, or CHTTPSVR_NULL with *err_str set; every failure
// undoes what was already built. A create after the exit-time destructor
// released the slot table is refused before anything is allocated. A closed or
// stale parent logger is reported through err_str, because clog_derive would
// end the process. requests_done_cv uses CLOCK_MONOTONIC to match the deadline
// wait_in_flight_bounded builds. root->owner is set to the handle so the
// exit-time destructor can tell a live root router from a destroyed one.
chttpsvr chttpsvr_create_mp(ccol_memmgmt_procs *mprocs, clog cl,
                            char **err_str) {
  if (!ccol__verify_memmgmt_procs(mprocs, err_str)) return CHTTPSVR_NULL;

  if (chttpsvr_slot_table_released()) {
    if (err_str)
      *err_str = CCOL__ERR_STR(
          "the library already released its server table at unload");
    return CHTTPSVR_NULL;
  }

  struct chttpserver *srv = (struct chttpserver *)ccol__mem_calloc(
      mprocs, 1, sizeof(struct chttpserver));
  if (!srv) {
    if (err_str) *err_str = CCOL__ERR_STR("out of memory");
    return CHTTPSVR_NULL;
  }
  atomic_init(&srv->lifetime_refs, 1);

  if (mprocs) {
    srv->m_procs = (ccol_memmgmt_procs *)ccol__mem_alloc(
        mprocs, sizeof(ccol_memmgmt_procs));
    if (!srv->m_procs) {
      ccol__mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL__ERR_STR("out of memory");
      return CHTTPSVR_NULL;
    }
    memcpy(srv->m_procs, mprocs, sizeof(ccol_memmgmt_procs));
  }

  ccol_retval derive_rv = ccol_success;
  clog logger = cl ? clog__derive_checked(cl, &derive_rv)
                   : clog_create_from_fd_mp(2, CLOG_FATAL, NULL, mprocs);
  if (logger && cl) clog_set_field(logger, "component", "http-server");
  if (!logger) {
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str)
      *err_str = derive_rv == ccol_invalid_args
                     ? CCOL__ERR_STR("the parent logger is not open")
                     : CCOL__ERR_STR("failed to create server logger");
    return CHTTPSVR_NULL;
  }

  if (ccol__mutex_init(srv->mutex) != 0) {
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__mutex_init failed");
    return CHTTPSVR_NULL;
  }
  if (ccol__mutex_init(srv->idle_mutex) != 0) {
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__mutex_init failed");
    return CHTTPSVR_NULL;
  }
  if (ccol__mutex_init(srv->diverted_mutex) != 0) {
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__mutex_init failed");
    return CHTTPSVR_NULL;
  }
  if (ccol__mutex_init(srv->wait_mutex) != 0) {
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__mutex_init failed");
    return CHTTPSVR_NULL;
  }
  if (ccol__cond_var_init(srv->resolve_cv) != 0) {
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__cond_var_init failed");
    return CHTTPSVR_NULL;
  }

  {
    ccol__cond_var_attr cv_attr;
    int cv_rc = 0;
    if (ccol__cond_var_attr_init(cv_attr) == 0) {
      ccol__cond_var_attr_setclock(cv_attr, CLOCK_MONOTONIC);
      cv_rc = ccol__cond_var_init_ca(srv->requests_done_cv, cv_attr);
      ccol__cond_var_attr_destroy(cv_attr);
    } else {
      cv_rc = 1;
    }
    if (cv_rc != 0) {
      ccol__cond_var_destroy(srv->resolve_cv);
      ccol__mutex_destroy(srv->idle_mutex);
      ccol__mutex_destroy(srv->diverted_mutex);
      ccol__mutex_destroy(srv->wait_mutex);
      ccol__mutex_destroy(srv->mutex);
      clog_close(logger);
      ccol__mem_free(mprocs, srv->m_procs);
      ccol__mem_free(mprocs, srv);
      if (err_str) *err_str = CCOL__ERR_STR("ccol__cond_var_init failed");
      return CHTTPSVR_NULL;
    }
  }

  if (ccol__cond_var_init(srv->quiesce_done_cv) != 0) {
    ccol__cond_var_destroy(srv->requests_done_cv);
    ccol__cond_var_destroy(srv->resolve_cv);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__cond_var_init failed");
    return CHTTPSVR_NULL;
  }

  if (ccol__rw_lock_init(srv->routes_lock) != 0) {
    ccol__cond_var_destroy(srv->quiesce_done_cv);
    ccol__cond_var_destroy(srv->requests_done_cv);
    ccol__cond_var_destroy(srv->resolve_cv);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("ccol__rw_lock_init failed");
    return CHTTPSVR_NULL;
  }

  chttpsvr_router *root = create_router(srv, "", srv->m_procs);
  if (!root) {
    ccol__rw_lock_destroy(srv->routes_lock);
    ccol__cond_var_destroy(srv->quiesce_done_cv);
    ccol__cond_var_destroy(srv->requests_done_cv);
    ccol__cond_var_destroy(srv->resolve_cv);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("out of memory");
    return CHTTPSVR_NULL;
  }

  srv->routers = (chttpsvr_router **)ccol__mem_alloc(srv->m_procs,
                                                     sizeof(chttpsvr_router *));
  if (!srv->routers) {
    destroy_router(root, srv->m_procs);
    ccol__rw_lock_destroy(srv->routes_lock);
    ccol__cond_var_destroy(srv->quiesce_done_cv);
    ccol__cond_var_destroy(srv->requests_done_cv);
    ccol__cond_var_destroy(srv->resolve_cv);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    if (err_str) *err_str = CCOL__ERR_STR("out of memory");
    return CHTTPSVR_NULL;
  }
  srv->routers[0] = root;
  srv->root_router = root;
  srv->router_count = 1;
  srv->router_cap = 1;
  srv->cl = logger;
  atomic_store(&srv->listen_fd, -1);
  srv->lifecycle = CHTTPSVR_LC_IDLE;

  chttpsvr h = chttpsvr_handle_slot_acquire(srv);
  if (h == 0) {
    if (err_str) *err_str = CCOL__ERR_STR("failed to allocate server slot");
    ccol__mem_free(srv->m_procs, srv->routers);
    destroy_router(root, srv->m_procs);
    ccol__rw_lock_destroy(srv->routes_lock);
    ccol__cond_var_destroy(srv->quiesce_done_cv);
    ccol__cond_var_destroy(srv->requests_done_cv);
    ccol__cond_var_destroy(srv->resolve_cv);
    ccol__mutex_destroy(srv->idle_mutex);
    ccol__mutex_destroy(srv->diverted_mutex);
    ccol__mutex_destroy(srv->wait_mutex);
    ccol__mutex_destroy(srv->mutex);
    clog_close(logger);
    ccol__mem_free(mprocs, srv->m_procs);
    ccol__mem_free(mprocs, srv);
    return CHTTPSVR_NULL;
  }

  atomic_store(&root->owner, h);
  return h;
}

// The three thread pools of a server (worker, reject and the lazily created
// streaming pool), taken out of the server together. Every detach point needs
// all three, and one return value keeps a call site from detaching one and
// forgetting another.
typedef struct {
  cthrpool worker;
  cthrpool reject;
  cthrpool stream;
} detached_pools;

// How long wait_in_flight_bounded waits for in-flight requests:
// CHTTPSVR_GRACEFUL_WAIT_MS before it forces a shutdown(2) on every diverted
// connection, then CHTTPSVR_FORCE_UNBLOCK_GRACE_MS for those workers to unwind
// through their error paths.
#define CHTTPSVR_FORCE_UNBLOCK_GRACE_MS 5000
#define CHTTPSVR_GRACEFUL_WAIT_MS 30000

// Bound on drain_and_close_all_connections's wait for every connection of a
// server to be freed.
#define CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS 30000

#ifdef RUNNING_UNIT_TESTS
// Test overrides of the three bounds above, set by the two setters below. 0
// means the real value. They are process-wide and let a test run the escalation
// in well under a second.
static _Atomic unsigned g_wait_in_flight_graceful_ms_for_tests = 0;
static _Atomic unsigned g_wait_in_flight_grace_ms_for_tests = 0;
void chttpsvr__set_wait_in_flight_bounds_for_tests(unsigned graceful_ms,
                                                   unsigned grace_ms) {
  atomic_store(&g_wait_in_flight_graceful_ms_for_tests, graceful_ms);
  atomic_store(&g_wait_in_flight_grace_ms_for_tests, grace_ms);
}

static _Atomic unsigned g_drain_connections_wait_ms_for_tests = 0;
void chttpsvr__set_drain_connections_wait_ms_for_tests(unsigned ms) {
  atomic_store(&g_drain_connections_wait_ms_for_tests, ms);
}
#endif

// Adds ms milliseconds to *ts and normalizes tv_nsec; the bounded waits below
// use it to build their deadlines.
static void timespec_add_ms(struct timespec *ts, unsigned ms) {
  ts->tv_sec += (time_t)(ms / 1000);
  ts->tv_nsec += (long)(ms % 1000) * 1000000L;
  if (ts->tv_nsec >= 1000000000L) {
    ts->tv_nsec -= 1000000000L;
    ts->tv_sec += 1;
  }
}

// Waits for srv->in_flight_requests to reach 0 on requests_done_cv. After the
// graceful bound it calls force_unblock_diverted_connections, so a worker
// blocked for ever in a socket read or write (a timeout of 0 and a peer that
// stalls) gets an I/O error, and then waits one more grace period. Without
// that, the cthrpool_shutdown_drain that every caller runs next, which has no
// timeout, could hang on such a task. The caller holds srv->mutex; it is
// released around the shutdown pass, so no I/O happens under the lock. A
// request that remains after both waits is blocked in something this library
// can't reach, and the pool drain waits for it.
static void wait_in_flight_bounded(struct chttpserver *srv) {
  if (srv->in_flight_requests <= 0) return;

  unsigned graceful_ms = CHTTPSVR_GRACEFUL_WAIT_MS;
  unsigned grace_ms = CHTTPSVR_FORCE_UNBLOCK_GRACE_MS;
#ifdef RUNNING_UNIT_TESTS
  unsigned override_graceful_ms =
      atomic_load(&g_wait_in_flight_graceful_ms_for_tests);
  unsigned override_grace_ms =
      atomic_load(&g_wait_in_flight_grace_ms_for_tests);
  if (override_graceful_ms) graceful_ms = override_graceful_ms;
  if (override_grace_ms) grace_ms = override_grace_ms;
#endif

  struct timespec deadline;
  clock_gettime(CLOCK_MONOTONIC, &deadline);
  timespec_add_ms(&deadline, graceful_ms);
  bool timed_out = false;
  while (srv->in_flight_requests > 0) {
    if (ccol__cond_var_timedwait(srv->requests_done_cv, srv->mutex, deadline) ==
        ETIMEDOUT) {
      timed_out = true;
      break;
    }
  }
  if (!timed_out || srv->in_flight_requests <= 0) return;

  ccol__mutex_unlock(srv->mutex);
  force_unblock_diverted_connections(srv);
  ccol__mutex_lock(srv->mutex);
  if (srv->in_flight_requests <= 0) return;

  struct timespec grace_deadline;
  clock_gettime(CLOCK_MONOTONIC, &grace_deadline);
  timespec_add_ms(&grace_deadline, grace_ms);
  while (srv->in_flight_requests > 0) {
    if (ccol__cond_var_timedwait(srv->requests_done_cv, srv->mutex,
                                 grace_deadline) == ETIMEDOUT)
      break;
  }
}

// Drains and destroys each pool of pools that exists, streaming pool first.
// Each drain waits for every queued and running task of that pool.
static void destroy_detached_pools(detached_pools pools) {
  if (pools.stream) {
    cthrpool_shutdown_drain(pools.stream);
    cthrpool_destroy(pools.stream);
  }
  if (pools.worker) {
    cthrpool_shutdown_drain(pools.worker);
    cthrpool_destroy(pools.worker);
  }
  if (pools.reject) {
    cthrpool_shutdown_drain(pools.reject);
    cthrpool_destroy(pools.reject);
  }
}

// One set of pools that a restart took out of service. They drain on a thread
// of their own with no forced unblock, so a restart neither waits for nor cuts
// off a request of the previous run. The records form the list
// srv->retired_pools. Each task of these pools references srv, so every
// teardown of srv waits for all records through retired_pools_reap. Nothing
// reaches these pools through srv after the detach: Every submit reads the
// current pool under srv->mutex, and a submit that read an old pool lands
// before its drain or is refused like a full pool.
typedef struct chttpsvr_retired_pools {
  struct chttpsvr_retired_pools *next;
  struct chttpserver *srv;
  detached_pools pools;
  ccol__thread_id thread;
  // has_thread is set when a drain thread was started for this record; done is
  // set under srv->mutex once that thread has finished.
  bool has_thread;
  bool done;
} chttpsvr_retired_pools;

#ifdef RUNNING_UNIT_TESTS
// Test counters of retired pool sets that finished draining (above) and that
// started draining (below), with their readers.
static _Atomic size_t g_retired_drained_for_tests = 0;
size_t chttpsvr__retired_drained_for_tests(void) {
  return atomic_load(&g_retired_drained_for_tests);
}
#endif

#ifdef RUNNING_UNIT_TESTS
static _Atomic size_t g_retired_drain_started_for_tests = 0;
size_t chttpsvr__retired_drain_started_for_tests(void) {
  return atomic_load(&g_retired_drain_started_for_tests);
}
#endif

// The drain thread of a retired pool set: It drains and destroys the pools and
// then sets rec->done under srv->mutex, its last touch of srv. The creating
// thread holds srv->mutex until it has stored this thread in rec, so a reaper
// that sees done can always join it.
static void *retired_pools_drain_fn(void *arg) {
  chttpsvr_retired_pools *rec = (chttpsvr_retired_pools *)arg;
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_retired_drain_started_for_tests, 1);
#endif
  destroy_detached_pools(rec->pools);
#ifdef RUNNING_UNIT_TESTS
  atomic_fetch_add(&g_retired_drained_for_tests, 1);
#endif
  struct chttpserver *srv = rec->srv;
  ccol__mutex_lock(srv->mutex);
  rec->done = true;
  ccol__mutex_unlock(srv->mutex);
  return NULL;
}

// Starts the drain thread of rec and records it. The caller holds srv->mutex,
// which the thread takes as its last step, so rec->thread is stored before the
// thread can report itself done. A failed thread creation leaves has_thread
// false, and retired_pools_reap tries again.
static void retired_pools_spawn_locked(chttpsvr_retired_pools *rec) {
  ccol__thread_id t;
  if (ccol__thread_create(t, retired_pools_drain_fn, rec) == 0) {
    rec->thread = t;
    rec->has_thread = true;
  }
}

// Joins and frees every retired pool set of srv whose drain has finished, and
// starts a drain thread for any set without one. With wait_all it takes every
// set and waits for each; its caller is a teardown that has already ended or
// unblocked every request, so each drain ends, and a set without a thread is
// drained right here. The list is taken under srv->mutex and the joins and
// frees happen after it is released.
static void retired_pools_reap(struct chttpserver *srv, bool wait_all) {
  ccol__mutex_lock(srv->mutex);
  chttpsvr_retired_pools *keep = NULL, *finish = NULL;
  chttpsvr_retired_pools *r = srv->retired_pools;
  srv->retired_pools = NULL;
  while (r) {
    chttpsvr_retired_pools *next = r->next;
    if (!wait_all && !r->has_thread) retired_pools_spawn_locked(r);
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
  ccol__mutex_unlock(srv->mutex);
  while (finish) {
    chttpsvr_retired_pools *next = finish->next;
    if (finish->has_thread)
      ccol__thread_join(finish->thread);
    else
      destroy_detached_pools(finish->pools);
    ccol__mem_free(srv->m_procs, finish);
    finish = next;
  }
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: The worker pool the latest retire_pools call took out of service,
// and a reader that tells whether that pool's shutdown has started (after which
// submits to it are refused). A pool that doesn't resolve was destroyed, which
// counts as started. The caller must hold a task of that pool so it can't be
// destroyed while the reader looks at it.
static _Atomic cthrpool g_last_retired_worker_pool_for_tests = CTHRPOOL_NULL;

bool chttpsvr__retired_worker_shutdown_started_for_tests(void) {
  cthrpool h = atomic_load(&g_last_retired_worker_pool_for_tests);
  if (h == CTHRPOOL_NULL) return false;
  struct cthread_pool *pool = cthrpool__resolve_for_tests(h);
  if (!pool) return true;
  bool started = false;
  cthrpool__shutdown_state_for_tests(pool, &started, NULL, NULL);
  return started;
}
#endif

// Takes pools out of service without waiting: It allocates a
// chttpsvr_retired_pools record, starts its drain thread and links it into
// srv->retired_pools under srv->mutex. A set with no pool does nothing. When
// the record can't be allocated, the pools drain on the calling thread, again
// without a forced unblock.
static void retire_pools(struct chttpserver *srv, detached_pools pools) {
  if (!pools.worker && !pools.reject && !pools.stream) return;
#ifdef RUNNING_UNIT_TESTS
  if (pools.worker)
    atomic_store(&g_last_retired_worker_pool_for_tests, pools.worker);
#endif
  chttpsvr_retired_pools *rec =
      (chttpsvr_retired_pools *)ccol__mem_alloc(srv->m_procs, sizeof(*rec));
  if (!rec) {
    destroy_detached_pools(pools);
    return;
  }
  rec->srv = srv;
  rec->pools = pools;
  rec->has_thread = false;
  rec->done = false;
  ccol__mutex_lock(srv->mutex);
  retired_pools_spawn_locked(rec);
  rec->next = srv->retired_pools;
  srv->retired_pools = rec;
  ccol__mutex_unlock(srv->mutex);
}

// Detaches srv's current pools under srv->mutex, starts a new streaming run so
// no queued streaming request goes to them, and retires them. The failure paths
// of chttpsvr_start use it. With no worker pool there's no run, so the
// stream_token_start call gives a queued streaming request its 503 at once.
static void retire_current_pools(struct chttpserver *srv) {
  ccol__mutex_lock(srv->mutex);
  detached_pools out = {srv->worker_pool, srv->reject_pool, srv->stream_pool};
  srv->worker_pool = CTHRPOOL_NULL;
  srv->reject_pool = CTHRPOOL_NULL;
  srv->stream_pool = CTHRPOOL_NULL;
  stream_new_run_locked(srv);
  ccol__mutex_unlock(srv->mutex);
  retire_pools(srv, out);
  stream_token_start(srv);
}

// The drain step of a teardown: It gives every body-memory waiter and every
// queued streaming request its answer, closes the idle keep-alive connections
// (the server is draining, so they would get nothing more), waits for in-flight
// requests through wait_in_flight_bounded, and detaches the pools, all in one
// srv->mutex critical section with a final idle-close pass. Holding the lock
// from the wait to that pass keeps a keep-alive connection from being diverted
// again after the pass and leaking once the engine goes. Parked connections
// carry on, since they hold a request being served.
// It then polls, every millisecond up to CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS,
// until srv->current_connections reaches 0, repeating the idle-close pass. That
// count covers connections the idle list can't see (one in its handshake on the
// reactor, one a dispatch claimed and closes itself), which the reactor may be
// touching when srv is freed. A poll is used because a broadcast from every
// conn_free would cost the hot close path. It gives the detached pools to the
// caller.
static detached_pools drain_and_close_all_connections(struct chttpserver *srv) {
  mem_admission_close(srv);
  stream_queue_flush(srv);
  close_idle_connections(srv, false);
  ccol__mutex_lock(srv->mutex);
  wait_in_flight_bounded(srv);
  detached_pools out = {srv->worker_pool, srv->reject_pool, srv->stream_pool};
  srv->worker_pool = CTHRPOOL_NULL;
  srv->reject_pool = CTHRPOOL_NULL;
  srv->stream_pool = CTHRPOOL_NULL;
  stream_new_run_locked(srv);
  close_all_idle_connections(srv);
  mem_wait_flush(srv);
  ccol__mutex_unlock(srv->mutex);

  unsigned drain_wait_ms = CHTTPSVR_DRAIN_CONNECTIONS_WAIT_MS;
#ifdef RUNNING_UNIT_TESTS
  unsigned override_drain_wait_ms =
      atomic_load(&g_drain_connections_wait_ms_for_tests);
  if (override_drain_wait_ms) drain_wait_ms = override_drain_wait_ms;
#endif
  struct timespec poll_deadline;
  clock_gettime(CLOCK_MONOTONIC, &poll_deadline);
  timespec_add_ms(&poll_deadline, drain_wait_ms);
  while (atomic_load(&srv->current_connections) > 0) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    if (now.tv_sec > poll_deadline.tv_sec ||
        (now.tv_sec == poll_deadline.tv_sec &&
         now.tv_nsec >= poll_deadline.tv_nsec))
      break;
    struct timespec nap = {0, 1000000L};
    nanosleep(&nap, NULL);
    close_all_idle_connections(srv);
    mem_wait_flush(srv);
    stream_queue_flush(srv);
  }
  return out;
}

// The full teardown of a server, run exactly once: Stop listening, drain
// requests and connections, reap retired pools, drop the engine reference,
// destroy the pools, unregister from servers_bundler.servers, and start the
// engine reaper when that reference was the last. chttpsvr__destroy and the
// force-stop pass of chttpsvr_engine_stop (engine_force_stop_quiesce_all) can
// both call it, at the same time. The first caller claims it through
// srv->quiesce_state and sets draining; a later caller counts itself in
// quiesce_waiters and blocks until CHTTPSVR_QS_QUIESCED, because
// chttpsvr__destroy destroys srv->mutex and frees srv right after this returns.
// The winner first waits for pending_resolve_count to reach 0, so a
// chttpsvr_start that holds an engine reference can't go on using a reactor
// torn down under it. The drain comes before the engine release, since the
// reaper destroys the reactor. The unregister comes after the release, so a
// concurrent reap finds srv and waits for this quiesce, and the reaper starts
// only after it, because a reaper that runs inline would otherwise wait for
// this very thread. At the end the winner waits for quiesce_waiters to fall to
// 0, so no loser touches srv->mutex or quiesce_done_cv after its caller
// destroys them.
static void quiesce_server_once(struct chttpserver *srv) {
  ccol__mutex_lock(srv->mutex);
  switch (srv->quiesce_state) {
    case CHTTPSVR_QS_QUIESCING:
    case CHTTPSVR_QS_QUIESCED: {
      srv->quiesce_waiters++;
      while (srv->quiesce_state != CHTTPSVR_QS_QUIESCED)
        ccol__cond_var_wait(srv->quiesce_done_cv, srv->mutex);
      srv->quiesce_waiters--;
      ccol__cond_var_broadcast(srv->quiesce_done_cv);
      ccol__mutex_unlock(srv->mutex);
      return;
    }
    case CHTTPSVR_QS_NOT_QUIESCED:
      srv->quiesce_state = CHTTPSVR_QS_QUIESCING;
      srv_begin_draining(srv);
#ifdef RUNNING_UNIT_TESTS
      quiesce_claimed_signal_fire_if_armed();
#endif
      break;
  }
  while (atomic_load(&srv->pending_resolve_count) > 0)
    ccol__cond_var_wait(srv->resolve_cv, srv->mutex);
  ccol__mutex_unlock(srv->mutex);

#ifdef RUNNING_UNIT_TESTS
  quiesce_teardown_race_hook_wait_if_armed();
#endif

  chttpsvr_stop_internal(srv);

  detached_pools old_pools = drain_and_close_all_connections(srv);
  retired_pools_reap(srv, true);

  bool should_release_engine = false;
  uintptr_t engine_gen = 0;
  ccol__mutex_lock(srv->mutex);
  if (srv->contributed_to_engine) {
    srv->contributed_to_engine = false;
    should_release_engine = true;
    engine_gen = srv->engine_gen;
    srv->engine_gen = 0;
  }
  ccol__mutex_unlock(srv->mutex);
  bool should_reap = should_release_engine && engine_release_begin(engine_gen);

  destroy_detached_pools(old_pools);
  servers_unregister(srv);
  if (should_reap) spawn_reaper();

  ccol__mutex_lock(srv->mutex);
  srv->quiesce_state = CHTTPSVR_QS_QUIESCED;
  ccol__cond_var_broadcast(srv->quiesce_done_cv);
  while (srv->quiesce_waiters > 0)
    ccol__cond_var_wait(srv->quiesce_done_cv, srv->mutex);
  ccol__mutex_unlock(srv->mutex);
}

// Destroys the server behind srv. It resolves the handle and clears the slot's
// in_use flag in one critical section of the slot table, so a second destroy
// reaches the fatal stale-handle error; a call from a handler running on this
// server's own pools is fatal too, since its drain would wait for itself. It
// then waits for every pinned resolve, quiesces the server, waits for
// servers_bundler_pins and listener_dispatch_pins (a force-stop pass and a
// listener dispatch can hold a bare pointer to raw), and releases the TLS
// context. Last it drops the application's raw->lifetime_refs reference;
// whoever drops the last one, this call or a conn_on_removed, runs
// chttpsvr_finish_destroy.
void chttpsvr__destroy(chttpsvr srv) {
  if (!srv) return;

  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(srv >> 32);
  uint32_t gen = (uint32_t)(srv & 0xFFFFFFFFu);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot *slot = NULL;
  struct chttpserver *raw = NULL;
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot *s =
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (s->in_use && s->generation == gen) {
      slot = s;
      raw = s->ptr;
    }
  }
  if (!raw) {
    ccol__mutex_unlock(chttpsvr_slot_table.mutex);
    ccol__fatal_err(
        "chttpsvr_destroy: handle is stale or already destroyed "
        "(double-destroy / use-after-destroy of a chttpsvr handle)");
  }
  if (chttpsvr_is_self_call(raw)) {
    ccol__mutex_unlock(chttpsvr_slot_table.mutex);
    ccol__fatal_err(
        "chttpsvr_destroy: called from within a request handler (or "
        "middleware) running on this very server's own worker pool; "
        "destroying it here would deadlock waiting for this exact "
        "in-flight request to finish, then abort when the pool's own "
        "self-destroy guard fires. Destroy the server from a different "
        "thread instead, or call chttpsvr_engine_stop() (documented "
        "safe to call from within a handler) if a full shutdown is the "
        "actual goal.");
  }
  slot->in_use = false;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);

  ccol__mutex_lock(raw->mutex);
  while (atomic_load(&raw->pending_resolve_count) > 0)
    ccol__cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol__mutex_unlock(raw->mutex);

  quiesce_server_once(raw);

  ccol__mutex_lock(raw->mutex);
  while (atomic_load(&raw->servers_bundler_pins) > 0 ||
         atomic_load(&raw->listener_dispatch_pins) > 0)
    ccol__cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol__mutex_unlock(raw->mutex);

  if (raw->tls_ctx) {
    ctls_ctx_release(raw->tls_ctx);
    raw->tls_ctx = NULL;
  }

  if (atomic_fetch_sub(&raw->lifetime_refs, 1) == 1)
    chttpsvr_finish_destroy(raw);
}

#ifdef RUNNING_UNIT_TESTS
// Test hooks: A test names one server whose chttpsvr_finish_destroy it watches
// and asks whether that ran. The second counter records route teardowns that
// found a connection of the server allocated; it must stay 0, since such a
// connection could read its matched route.
static _Atomic(struct chttpserver *) g_finish_destroy_watch_for_tests = NULL;
static _Atomic size_t g_route_teardown_with_live_connections_for_tests = 0;
size_t chttpsvr__route_teardown_with_live_connections_for_tests(void) {
  return atomic_load(&g_route_teardown_with_live_connections_for_tests);
}
static _Atomic bool g_finish_destroy_watch_hit_for_tests = false;
void chttpsvr__watch_finish_destroy_for_tests(chttpsvr h) {
  atomic_store(&g_finish_destroy_watch_hit_for_tests, false);
  atomic_store(&g_finish_destroy_watch_for_tests,
               chttpsvr__resolve_for_tests(h));
}
bool chttpsvr__watched_finish_destroy_ran_for_tests(void) {
  return atomic_load(&g_finish_destroy_watch_hit_for_tests);
}
#endif

static void chttpsvr_release_slot_table_if_deferred_locked(void);

// The final teardown of a server, run by whoever drops the last
// raw->lifetime_refs reference (chttpsvr__destroy or conn_on_removed). It frees
// the routers and their route and middleware data here, because a connection a
// slow handler owns reads its matched route until its conn_free, and each such
// connection holds a reference. No router call can reach the routers either,
// since their owner handle doesn't resolve. It then destroys the locks, closes
// the logger and frees raw. The slot is released last, under the slot table
// mutex and fetched again by index (a concurrent create may have moved the slot
// array): ptr goes NULL, the generation moves past the stale handle's, and the
// index returns to the free list. When the exit-time destructor deferred the
// table release, this may be the call that does it.
static void chttpsvr_finish_destroy(struct chttpserver *raw) {
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&g_finish_destroy_watch_for_tests) == raw) {
    atomic_store(&g_finish_destroy_watch_for_tests, NULL);
    atomic_store(&g_finish_destroy_watch_hit_for_tests, true);
  }
#endif
#ifdef RUNNING_UNIT_TESTS
  if (atomic_load(&raw->current_connections) > 0)
    atomic_fetch_add(&g_route_teardown_with_live_connections_for_tests, 1);
#endif
  for (size_t i = 0; i < raw->router_count; i++) {
    destroy_router(raw->routers[i], raw->m_procs);
  }
  chttpsvr_router_shells_release(raw->routers, raw->router_count,
                                 raw->root_router);
  raw->root_router = NULL;
  ccol__mem_free(raw->m_procs, raw->routers);

  ccol__mutex_destroy(raw->mutex);
  ccol__mutex_destroy(raw->idle_mutex);
  ccol__mutex_destroy(raw->diverted_mutex);
  ccol__mutex_destroy(raw->wait_mutex);
  ccol__cond_var_destroy(raw->requests_done_cv);
  ccol__cond_var_destroy(raw->resolve_cv);
  ccol__cond_var_destroy(raw->quiesce_done_cv);
  ccol__rw_lock_destroy(raw->routes_lock);

  clog_close(raw->cl);
  raw->cl = CLOG_NULL;

  uint32_t idx = raw->self_slot_idx;
  ccol_memmgmt_procs *mp = raw->m_procs;
  ccol__mem_free(mp, raw);
  ccol__mem_free(mp, mp);

  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  chttpsvr_slot *slot2 =
      (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
  slot2->ptr = NULL;
  slot2->generation++;
  cvector_push_back(chttpsvr_slot_table.free_indices, &idx);
  chttpsvr_release_slot_table_if_deferred_locked();
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
}

// Tells whether slot belongs to a server, so the table must not be released
// under it. It reads ptr and not in_use, because in_use is cleared as the first
// step of chttpsvr__destroy, while ptr stays set until the final locked release
// of the slot, which indexes the table.
static bool chttpsvr_slot_live(const chttpsvr_slot *slot) {
  return slot->ptr != NULL;
}

#ifdef RUNNING_UNIT_TESTS
// Test hooks: The first asks chttpsvr_slot_live about the slot of h, the second
// sets that slot's in_use flag, so a test can build the window between the two
// fields without racing a real teardown.
bool chttpsvr__slot_live_for_tests(chttpsvr h) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  bool live = false;
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots))
    live = chttpsvr_slot_live(
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx));
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return live;
}

void chttpsvr__slot_set_in_use_for_tests(chttpsvr h, bool in_use) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  uint32_t idx = (uint32_t)(h >> 32);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots))
    ((chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx))->in_use =
        in_use;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
}
#endif

// Tells whether any slot of the table names a server. The caller holds the
// table's mutex and the table exists.
static bool chttpsvr_any_slot_live_locked(void) {
  size_t slot_count = cvector_elem_count(chttpsvr_slot_table.slots);
  for (size_t i = 0; i < slot_count; i++)
    if (chttpsvr_slot_live(
            (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, i)))
      return true;
  return false;
}

// Frees the slot table's arrays and sets their pointers to NULL, which every
// resolve, acquire and fork handler tests, so a later stale handle resolves to
// nothing and a create fails cleanly. It also deletes the worker key: With no
// server left no worker thread of any server is left, so a dlclose() doesn't
// leak the key. The caller holds the table's mutex, the table exists and no
// slot is live.
static void chttpsvr_release_slot_table_locked(void) {
  cvector__destroy(chttpsvr_slot_table.slots);
  cvector__destroy(chttpsvr_slot_table.free_indices);
  chttpsvr_slot_table.slots = NULL;
  chttpsvr_slot_table.free_indices = NULL;
  chttpsvr_slot_table.release_deferred = false;
  ccol__call_once(chttpsvr_worker_key_bundle.once,
                  chttpsvr_worker_key_init_globals);
  ccol__mutex_lock(chttpsvr_worker_key_bundle.mutex);
  if (atomic_exchange(&chttpsvr_worker_key_bundle.ready, false))
    ccol__thread_ls_key_delete(chttpsvr_worker_key_bundle.key);
  ccol__mutex_unlock(chttpsvr_worker_key_bundle.mutex);
}

// Does the table release the exit-time destructor deferred, once the last
// server is gone. The caller holds the table's mutex. It is out of line so the
// destroy path keeps its shape.
static __attribute__((noinline)) void
chttpsvr_release_slot_table_if_deferred_locked(void) {
  if (chttpsvr_slot_table.release_deferred && chttpsvr_slot_table.slots &&
      !chttpsvr_any_slot_live_locked())
    chttpsvr_release_slot_table_locked();
}

// Frees the slot table at process exit or unload so memtest doesn't report it,
// but only when no slot is live. A server the application hasn't destroyed may
// be used from a later destructor or atexit handler, so then the release is
// deferred to the destroy of the last server. A process that never entered the
// initialization returns at once, since initializing here would build the slot
// tables of clogger and cthreadpool after their own destructors ran.
__attribute__((destructor)) static void cleanup_chttpsvr_slot_table(void) {
  if (!atomic_load(&chttpsvr_slot_table_entered)) return;
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  if (chttpsvr_slot_table.slots) {
    if (chttpsvr_any_slot_live_locked())
      chttpsvr_slot_table.release_deferred = true;
    else
      chttpsvr_release_slot_table_locked();
  }
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
}

// Starts or restarts the server behind h with cfg (NULL means
// CHTTPSVR_CONFIG_DEFAULT). It refuses a cfg->tls without both cert_path and
// key_path, so a TLS setting never silently gives plain HTTP, and a negative
// streaming_thread_count other than CHTTPSVR_STREAMING_POOL_OFF. A call from
// one of the server's own handlers gives ccol_not_permitted, and a server that
// is starting or running too.
// Under raw->mutex it waits out a concurrent stop (keeping its pin) and a
// quiesce in progress (dropping its pin first, since the quiesce waits for
// pins, and counting itself in quiesce_waiters before the drop so raw stays
// alive), resolving h again after each wait. It then claims
// CHTTPSVR_LC_STARTING and resets quiesce_state and draining. It builds the new
// pools, stores the settings in milliseconds, swaps the pools in under
// srv->mutex and retires the previous run's, builds the TLS context, registers
// srv (before engine_acquire, so a concurrent force-stop finds it), takes an
// engine reference, starts the idle sweep, binds the listen socket and
// registers the listener with one extra lifetime_refs reference. listen_fd is
// published before that registration, since a connection can arrive at once.
// When a reap is in progress it unwinds, waits on srv_engine_bundler.stopped_cv
// with no pin held and retries. Every failure path undoes its steps, retires
// the new pools and sets the lifecycle back to idle.
ccol_retval chttpsvr_start(chttpsvr h, const chttpsvr_config *cfg) {
  if (cfg && cfg->tls && !(cfg->tls->cert_path && cfg->tls->key_path))
    return ccol_invalid_args;
  if (cfg && cfg->streaming_thread_count < CHTTPSVR_STREAMING_POOL_OFF)
    return ccol_invalid_args;

  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;

  if (!chttpsvr_worker_key_ensure()) {
    SRV_ENGINE_LOG(clog_error,
                   "chttpsvr_start: no thread-specific key is available "
                   "(the process holds PTHREAD_KEYS_MAX keys)");
    chttpsvr_resolve_unpin(raw);
    return ccol_unexpected_failure;
  }

#ifdef RUNNING_UNIT_TESTS
  start_resolve_race_hook_wait_if_armed();
#endif

  if (chttpsvr_is_self_call(raw)) {
    chttpsvr_resolve_unpin(raw);
    return ccol_not_permitted;
  }

  chttpsvr_config default_cfg = CHTTPSVR_CONFIG_DEFAULT;
  if (!cfg) cfg = &default_cfg;

  ccol__mutex_lock(raw->mutex);
  for (;;) {
    switch (raw->lifecycle) {
      case CHTTPSVR_LC_STARTING:
      case CHTTPSVR_LC_RUNNING:
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
        return ccol_not_permitted;
      case CHTTPSVR_LC_STOPPING: {
#ifdef RUNNING_UNIT_TESTS
        start_stopping_wait_signal_fire_if_armed();
#endif
        while (raw->lifecycle == CHTTPSVR_LC_STOPPING)
          ccol__cond_var_wait(raw->resolve_cv, raw->mutex);
        continue;
      }
      case CHTTPSVR_LC_IDLE:
        break;
    }
    switch (raw->quiesce_state) {
      case CHTTPSVR_QS_QUIESCING:
        raw->quiesce_waiters++;
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
#ifdef RUNNING_UNIT_TESTS
        start_quiescing_unpinned_race_hook_wait_if_armed();
#endif
        ccol__mutex_lock(raw->mutex);
        while (raw->quiesce_state != CHTTPSVR_QS_QUIESCED)
          ccol__cond_var_wait(raw->quiesce_done_cv, raw->mutex);
        raw->quiesce_waiters--;
        ccol__cond_var_broadcast(raw->quiesce_done_cv);
        ccol__mutex_unlock(raw->mutex);
        raw = chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        ccol__mutex_lock(raw->mutex);
        continue;
      case CHTTPSVR_QS_NOT_QUIESCED:
      case CHTTPSVR_QS_QUIESCED:
        break;
    }
    raw->lifecycle = CHTTPSVR_LC_STARTING;
    raw->quiesce_state = CHTTPSVR_QS_NOT_QUIESCED;
    __atomic_store_n(&raw->draining, false, __ATOMIC_RELAXED);
    ccol__mutex_unlock(raw->mutex);

    retired_pools_reap(raw, false);

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
    cthrpool new_pool = cthrpool_create_mp((size_t)nthreads, queue_cap,
                                           raw->m_procs, &pool_err);
    if (!new_pool) {
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    int reject_nthreads = nthreads / 2;
    if (reject_nthreads < CHTTPSVR_REJECT_POOL_MIN_THREADS)
      reject_nthreads = CHTTPSVR_REJECT_POOL_MIN_THREADS;
    char *reject_pool_err = NULL;
    cthrpool new_reject_pool = cthrpool_create_mp(
        (size_t)reject_nthreads, CHTTPSVR_REJECT_POOL_QUEUE_CAP, raw->m_procs,
        &reject_pool_err);
    if (!new_reject_pool) {
      cthrpool_shutdown_drain(new_pool);
      cthrpool_destroy(new_pool);
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    unsigned stream_read_ms = cfg_us_to_ms(cfg->stream_read_timeout_us);
    atomic_store(&raw->stream_read_timeout_ms, stream_read_ms);
    atomic_store(&raw->max_body_read_duration_ms,
                 cfg_us_to_ms(cfg->max_body_read_duration_us));
    atomic_store(&raw->response_write_timeout_ms,
                 cfg->response_write_timeout_us
                     ? cfg_us_to_ms(cfg->response_write_timeout_us)
                     : stream_read_ms);
    atomic_store(&raw->max_response_write_duration_ms,
                 cfg_us_to_ms(cfg->max_response_write_duration_us));
    atomic_store(&raw->max_header_read_duration_ms,
                 cfg_us_to_ms(cfg->max_header_read_duration_us));
    atomic_store(&raw->idle_timeout_ms,
                 cfg_us_to_ms(cfg->idle_timeout_us ? cfg->idle_timeout_us
                                                   : cfg->read_timeout_us));
    atomic_store(&raw->max_body_size, cfg->max_body_size);
    atomic_store(&raw->max_header_bytes, cfg->max_header_bytes);
    atomic_store(&raw->max_connections, cfg->max_connections);
    raw->enable_keepalive = cfg->enable_keepalive;
    {
      atomic_store(&raw->min_rate_bps, cfg->min_transfer_rate_bps
                                           ? cfg->min_transfer_rate_bps
                                           : CHTTPSVR_DEFAULT_MIN_RATE_BPS);
      atomic_store(&raw->rate_grace_ms,
                   cfg->min_transfer_rate_grace_us
                       ? cfg_us_to_ms(cfg->min_transfer_rate_grace_us)
                       : CHTTPSVR_DEFAULT_RATE_GRACE_MS);
      atomic_store(&raw->mem_cap, cfg->max_partial_body_memory
                                      ? cfg->max_partial_body_memory
                                      : CHTTPSVR_DEFAULT_PARTIAL_BODY_MEMORY);
      uint64_t mem_wait_us = cfg->body_memory_wait_timeout_us;
      atomic_store(&raw->mem_wait_ms, mem_wait_us
                                          ? cfg_deadline_us_to_ms(mem_wait_us)
                                          : CHTTPSVR_DEFAULT_MEMORY_WAIT_MS);
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
                       ? cfg_deadline_us_to_ms(cfg->streaming_queue_timeout_us)
                       : CHTTPSVR_DEFAULT_STREAM_QUEUE_TIMEOUT_MS);
      ccol__mutex_lock(raw->wait_mutex);
      raw->mem_admission_closed = false;
      ccol__mutex_unlock(raw->wait_mutex);
    }

    ccol__mutex_lock(raw->mutex);
    detached_pools previous = {raw->worker_pool, raw->reject_pool,
                               raw->stream_pool};
    raw->worker_pool = new_pool;
    raw->reject_pool = new_reject_pool;
    raw->stream_pool = CTHRPOOL_NULL;
    stream_new_run_locked(raw);
    ccol__mutex_unlock(raw->mutex);
#ifdef RUNNING_UNIT_TESTS
    start_hold_for_tests();
#endif
    retire_pools(raw, previous);
    stream_token_start(raw);

    if (raw->tls_ctx) {
      ctls_ctx_release(raw->tls_ctx);
      raw->tls_ctx = NULL;
    }
    if (cfg->tls && cfg->tls->cert_path && cfg->tls->key_path) {
      ctls_ctx *tls_ctx = ctls_ctx_new_mp(raw->m_procs, NULL);
      if (!tls_ctx) {
        retire_current_pools(raw);
        ccol__mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
        return ccol_not_enough_memory;
      }
      ccol_retval tls_rv = ctls_ctx_cert_add(tls_ctx, NULL, cfg->tls->cert_path,
                                             cfg->tls->key_path, NULL, NULL);
      if (tls_rv != ccol_success) {
        ctls_ctx_release(tls_ctx);
        retire_current_pools(raw);
        ccol__mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
        return tls_rv;
      }
      if (cfg->tls->client_cert_optional &&
          (tls_rv = ctls_ctx_peer_cert_optional(tls_ctx, true)) !=
              ccol_success) {
        ctls_ctx_release(tls_ctx);
        retire_current_pools(raw);
        ccol__mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
        return tls_rv;
      }
      if (cfg->tls->ca_bundle_path &&
          (tls_rv = ctls_ctx_trust(tls_ctx, cfg->tls->ca_bundle_path, NULL)) !=
              ccol_success) {
        ctls_ctx_release(tls_ctx);
        retire_current_pools(raw);
        ccol__mutex_lock(raw->mutex);
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        chttpsvr_resolve_unpin(raw);
        return tls_rv;
      }
      raw->tls_ctx = tls_ctx;
    }

    if (!servers_register(raw)) {
      SRV_ENGINE_LOG(clog_error,
                     "chttpsvr_start: the server registry cannot grow");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      retire_current_pools(raw);
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_not_enough_memory;
    }

    bool need_acquire;
    ccol__mutex_lock(raw->mutex);
    need_acquire = !raw->contributed_to_engine;
    if (need_acquire) raw->contributed_to_engine = true;
    ccol__mutex_unlock(raw->mutex);

    if (need_acquire) {
#ifdef RUNNING_UNIT_TESTS
      engine_stopping_race_hook_wait_if_armed();
#endif
      bool currently_stopping = false;
      uintptr_t engine_gen = 0;
      ccol_retval engine_rc = engine_acquire(&currently_stopping, &engine_gen);
      if (engine_rc == ccol_success) {
        ccol__mutex_lock(raw->mutex);
        raw->engine_gen = engine_gen;
        ccol__mutex_unlock(raw->mutex);
      }
      if (currently_stopping) {
        ccol__mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        retire_current_pools(raw);
        chttpsvr_resolve_unpin(raw);
        ccol__call_once(srv_engine_bundler.once, engine_globals_init);
        ccol__mutex_lock(srv_engine_bundler.mutex);
        while (srv_engine_bundler.stopping)
          ccol__cond_var_wait(srv_engine_bundler.stopped_cv,
                              srv_engine_bundler.mutex);
        ccol__mutex_unlock(srv_engine_bundler.mutex);
        raw = chttpsvr_resolve(h);
        if (!raw) return ccol_invalid_args;
        ccol__mutex_lock(raw->mutex);
        continue;
      }
      if (engine_rc != ccol_success) {
        clog_error(raw->cl,
                   engine_rc == ccol_not_enough_memory
                       ? "the shared reactor engine could not be created"
                       : "the engine-stop watcher thread could not be "
                         "started");
        ccol__mutex_lock(raw->mutex);
        raw->contributed_to_engine = false;
        raw->lifecycle = CHTTPSVR_LC_IDLE;
        ccol__mutex_unlock(raw->mutex);
        servers_unregister(raw);
        if (raw->tls_ctx) {
          ctls_ctx_release(raw->tls_ctx);
          raw->tls_ctx = NULL;
        }
        retire_current_pools(raw);
        chttpsvr_resolve_unpin(raw);
        return engine_rc;
      }
    }
#ifdef RUNNING_UNIT_TESTS
    start_race_hook_wait_if_armed();
#endif
    if (!idle_sweep_start_if_needed()) {
      SRV_ENGINE_LOG(clog_error,
                     "idle-timeout sweep thread could not be started");
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      retire_current_pools(raw);
      chttpsvr_undo_start_registration(raw, need_acquire);
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }

    bool is_unix = false;
    chttpsvr_unix_bind unix_bind;
    int lfd =
        make_listen_socket(cfg->host, cfg->port, cfg->enable_reuseport,
                           cfg->ipv6_only, &is_unix, &unix_bind, raw->m_procs);
    uint16_t bound_port = 0;
    char bound_host[INET6_ADDRSTRLEN] = "";
    if (lfd >= 0 && !is_unix &&
        !listen_sockname(lfd, &bound_port, bound_host, sizeof(bound_host))) {
      int e = errno;
      close(lfd);
      lfd = -1;
      errno = e;
    }
    if (lfd < 0) {
      int listen_errno = errno;
#ifdef RUNNING_UNIT_TESTS
      atomic_store(&g_last_listen_errno_for_tests, listen_errno);
#endif
      SRV_ENGINE_LOG(
          clog_error, "listen socket setup failed host=%s port=%u errno=%d",
          cfg->host ? cfg->host : "(any)", (unsigned)cfg->port, listen_errno);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      retire_current_pools(raw);
      chttpsvr_undo_start_registration(raw, need_acquire);
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }

    atomic_store(&raw->listen_fd, lfd);
    atomic_store(&raw->is_unix_socket, is_unix);

    ccol__call_once(srv_parser_bundler.once, init_parser_settings);
    char *reg_err = NULL;
    atomic_fetch_add(&raw->lifetime_refs, 1);
    ceventloop_reg lreg = ceventloop_add(
        srv_engine_bundler.reactor, cselectable_fd(lfd, cselect_read),
        (ceventloop_handlers){.on_readable = listener_on_readable,
                              .on_removed = listener_on_removed},
        raw, &reg_err);
    if (!lreg) {
      atomic_fetch_sub(&raw->lifetime_refs, 1);
      atomic_store(&raw->listen_fd, -1);
      close(lfd);
      unix_bind_cleanup(&unix_bind, raw->m_procs);
      if (raw->tls_ctx) {
        ctls_ctx_release(raw->tls_ctx);
        raw->tls_ctx = NULL;
      }
      retire_current_pools(raw);
      chttpsvr_undo_start_registration(raw, need_acquire);
      ccol__mutex_lock(raw->mutex);
      raw->lifecycle = CHTTPSVR_LC_IDLE;
      ccol__mutex_unlock(raw->mutex);
      chttpsvr_resolve_unpin(raw);
      return ccol_unexpected_failure;
    }

    ccol__mutex_lock(raw->mutex);
    raw->unix_bind = unix_bind;
    raw->bound_port = bound_port;
    memcpy(raw->bound_host, bound_host, sizeof(bound_host));
    raw->listen_reg = lreg;
    raw->lifecycle = CHTTPSVR_LC_RUNNING;
    ccol__mutex_unlock(raw->mutex);

    chttpsvr_resolve_unpin(raw);
    return ccol_success;
  }
}

// Stops a running server on an already resolved raw pointer: It closes the
// listener and goes back to the idle state, and it leaves open connections and
// pools alone. chttpsvr_stop() calls it after resolving a handle, and
// quiesce_server_once calls it on a pointer it got from the server registry.
// Under raw->mutex it moves lifecycle from RUNNING to STOPPING and takes
// listen_fd, listen_reg and unix_bind out of the server; any other state makes
// it return at once. After ceventloop_remove() it waits on resolve_cv until
// listener_dispatch_pins is 0, because a dispatch already in flight may be
// about to accept4() on the fd we close, and that number can be reused by an
// unrelated open(). Only reactor threads hold that pin, so the wait can't
// deadlock on itself. At the end it sets lifecycle to IDLE and broadcasts
// resolve_cv, which wakes a chttpsvr_start() waiting for the stop to finish.
static void chttpsvr_stop_internal(struct chttpserver *raw) {
  ccol__mutex_lock(raw->mutex);
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
  ceventloop_reg lreg = raw->listen_reg;
  chttpsvr_unix_bind unix_bind = raw->unix_bind;
  if (was_started) {
    raw->lifecycle = CHTTPSVR_LC_STOPPING;
    atomic_store(&raw->listen_fd, -1);
    raw->listen_reg = CEVENTLOOP_REG_NULL;
    raw->unix_bind.path = NULL;
    raw->unix_bind.dir_fd = -1;
  }
  ccol__mutex_unlock(raw->mutex);
  if (!was_started) return;

#ifdef RUNNING_UNIT_TESTS
  stop_race_hook_wait_if_armed();
#endif

  if (lreg) ceventloop_remove(srv_engine_bundler.reactor, lreg);
  ccol__mutex_lock(raw->mutex);
  while (atomic_load(&raw->listener_dispatch_pins) > 0)
    ccol__cond_var_wait(raw->resolve_cv, raw->mutex);
  ccol__mutex_unlock(raw->mutex);
  close(lfd);
  unix_bind_cleanup(&unix_bind, raw->m_procs);

  ccol__mutex_lock(raw->mutex);
  raw->lifecycle = CHTTPSVR_LC_IDLE;
  ccol__cond_var_broadcast(raw->resolve_cv);
  ccol__mutex_unlock(raw->mutex);
}

// Stops listening on h; see chttpsvr_stop_internal. A handle that doesn't
// resolve is ignored.
void chttpsvr_stop(chttpsvr h) {
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return;
  chttpsvr_stop_internal(raw);
  chttpsvr_resolve_unpin(raw);
}

// chttpsvr_bound_address() copies into a buffer of CHTTPSVR_BOUND_ADDRESS_SIZE
// bytes, so a unix:// address and every numeric address must fit in it.
_Static_assert(sizeof(CHTTPSVR_UNIX_PREFIX) - 1 +
                       sizeof(((struct sockaddr_un *)0)->sun_path) <=
                   CHTTPSVR_BOUND_ADDRESS_SIZE,
               "CHTTPSVR_BOUND_ADDRESS_SIZE must hold a unix:// address");
_Static_assert(INET6_ADDRSTRLEN <= CHTTPSVR_BOUND_ADDRESS_SIZE,
               "CHTTPSVR_BOUND_ADDRESS_SIZE must hold a numeric address");

// Tells in *running_out whether h is in the RUNNING state, read under
// raw->mutex, so a caller can check a server without starting or stopping it.
// It gives ccol_invalid_args for a NULL running_out or a handle that doesn't
// resolve.
ccol_retval chttpsvr_is_running(chttpsvr h, bool *running_out) {
  if (running_out) *running_out = false;
  if (!running_out) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol__mutex_lock(raw->mutex);
  *running_out = raw->lifecycle == CHTTPSVR_LC_RUNNING;
  ccol__mutex_unlock(raw->mutex);
  chttpsvr_resolve_unpin(raw);
  return ccol_success;
}

// Gives the port the listener of h is bound to, which a caller needs after
// binding port 0. It reads lifecycle, is_unix_socket and bound_port under
// raw->mutex and gives ccol_not_permitted when the server doesn't run or
// listens on a unix socket, ccol_invalid_args for a bad handle or a NULL
// port_out (*port_out is 0 then).
ccol_retval chttpsvr_bound_port(chttpsvr h, uint16_t *port_out) {
  if (port_out) *port_out = 0;
  if (!port_out) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = ccol_not_permitted;
  ccol__mutex_lock(raw->mutex);
  if (raw->lifecycle == CHTTPSVR_LC_RUNNING &&
      !atomic_load(&raw->is_unix_socket)) {
    *port_out = raw->bound_port;
    rv = ccol_success;
  }
  ccol__mutex_unlock(raw->mutex);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Writes the address the listener of h is bound to into buf ("unix://" plus the
// path for a unix socket, the numeric host otherwise), so a caller can log or
// advertise it. It reads the bound state under raw->mutex. Gives
// ccol_not_permitted when the server doesn't run, ccol_msg_too_large when buf
// is too short, and ccol_invalid_args for a bad handle or an empty buffer; buf
// holds an empty string on every failure.
ccol_retval chttpsvr_bound_address(chttpsvr h, char *buf, size_t buflen) {
  if (buf && buflen) buf[0] = '\0';
  if (!buf || !buflen) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = ccol_not_permitted;
  ccol__mutex_lock(raw->mutex);
  if (raw->lifecycle == CHTTPSVR_LC_RUNNING) {
    const char *prefix = "";
    const char *text = raw->bound_host;
    if (atomic_load(&raw->is_unix_socket)) {
      prefix = CHTTPSVR_UNIX_PREFIX;
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
  ccol__mutex_unlock(raw->mutex);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Installs the logger that the shared engine logs through, or with CLOG_NULL
// releases the installed one. A caller uses it to send engine diagnostics where
// its own logs go. It works under srv_engine_bundler.mutex: A new logger is a
// derived child of cl, stored in user_log, and a running engine switches to it
// at once (the fallback logger it ran with is closed). A release is refused
// with ccol_not_permitted while a reactor exists or a stop runs, because the
// engine logs through it. A closed or stale cl gives the code of
// clog__derive_checked and changes nothing.
ccol_retval chttpsvr_set_engine_logger(clog cl) {
  if (!cl) {
    ccol__call_once(srv_engine_bundler.once, engine_globals_init);
    ccol__mutex_lock(srv_engine_bundler.mutex);
    if (srv_engine_bundler.reactor || srv_engine_bundler.stopping) {
      ccol__mutex_unlock(srv_engine_bundler.mutex);
      return ccol_not_permitted;
    }
    clog old_user = srv_engine_bundler.user_log;
    srv_engine_bundler.user_log = CLOG_NULL;
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    if (old_user) clog_close(old_user);
    return ccol_success;
  }
  ccol_retval derive_rv = ccol_success;
  clog derived = clog__derive_checked(cl, &derive_rv);
  if (!derived) return derive_rv;
  clog_set_field(derived, "component", "http-server-engine");
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  clog old_user = srv_engine_bundler.user_log;
  clog old_fallback = CLOG_NULL;
  srv_engine_bundler.user_log = derived;
  if (srv_engine_bundler.log) {
    if (srv_engine_bundler.log != old_user)
      old_fallback = srv_engine_bundler.log;
    srv_engine_bundler.log = derived;
  }
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  if (old_user) clog_close(old_user);
  if (old_fallback) clog_close(old_fallback);
  return ccol_success;
}

// Sets the allocator the shared engine uses (NULL for the default), so a caller
// can account for the engine's memory. It copies *mp into
// srv_engine_bundler.mprocs_storage under the engine mutex. Gives
// ccol_invalid_args for a procs struct with a NULL function and
// ccol_not_permitted once a reactor exists.
ccol_retval chttpsvr_set_engine_mem_mgmt_procs(ccol_memmgmt_procs *mp) {
  if (mp && (!mp->malloc || !mp->free || !mp->calloc || !mp->realloc))
    return ccol_invalid_args;
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  if (mp) {
    srv_engine_bundler.mprocs_storage = *mp;
    srv_engine_bundler.mprocs = &srv_engine_bundler.mprocs_storage;
  } else {
    srv_engine_bundler.mprocs = NULL;
  }
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

// Sets how many reactor threads the next engine creates, so a caller can size
// the reactor before the first start. It writes
// srv_engine_bundler.num_reactor_threads under the engine mutex and gives
// ccol_not_permitted once a reactor exists.
ccol_retval chttpsvr_set_engine_num_reactor_threads(size_t num_threads) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  if (srv_engine_bundler.reactor) {
    ccol__mutex_unlock(srv_engine_bundler.mutex);
    return ccol_not_permitted;
  }
  srv_engine_bundler.num_reactor_threads = num_threads;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return ccol_success;
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: The reactor thread count that was wired into the reactor created
// last, read under the engine mutex, so a test can check how a configured count
// was resolved.
size_t chttpsvr__engine_num_reactor_threads_for_tests(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  size_t n = srv_engine_bundler.last_resolved_num_reactor_threads;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return n;
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: How many rejection responses ran on a reject pool thread in the
// whole process. A test compares it before and after its own window, since
// other tests can cause rejections too.
size_t chttpsvr__reject_pool_task_count_for_tests(void) {
  return atomic_load(&g_reject_task_run_count_for_tests);
}
#endif

#ifdef RUNNING_UNIT_TESTS
// Test hook: Resolves h to its server under the slot table mutex without a pin,
// so a test has no unpin to forget (a missed unpin would make every later
// destroy of that server hang). It gives NULL in the same cases as
// chttpsvr_resolve, a released table included.
struct chttpserver *chttpsvr__resolve_for_tests(chttpsvr h) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  if (h == 0) return NULL;
  uint32_t idx = (uint32_t)(h >> 32);
  uint32_t gen = (uint32_t)(h & 0xFFFFFFFFu);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  struct chttpserver *raw = NULL;
  if (chttpsvr_slot_table.slots &&
      idx < cvector_elem_count(chttpsvr_slot_table.slots)) {
    chttpsvr_slot *slot =
        (chttpsvr_slot *)cvector_at(chttpsvr_slot_table.slots, idx);
    if (slot->in_use && slot->generation == gen) raw = slot->ptr;
  }
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return raw;
}

// Test hook: How many slots the handle table holds, so a test can check that a
// create and destroy loop reuses freed slots instead of growing the table.
size_t chttpsvr__slot_table_capacity_for_tests(void) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  size_t n = chttpsvr_slot_table.slots
                 ? cvector_elem_count(chttpsvr_slot_table.slots)
                 : 0;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return n;
}

// Test hook: How many servers are registered with the engine, read under
// servers_bundler.mutex, so a test can see that a teardown or a fork fixup
// really unregistered a server.
size_t chttpsvr__servers_bundler_count_for_tests(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(servers_bundler.mutex);
  size_t n = servers_bundler.count;
  ccol__mutex_unlock(servers_bundler.mutex);
  return n;
}

// Test hook: The idle timeout in ms that chttpsvr_start() stored for h, so a
// test can check the conversion from microseconds (a huge value must saturate
// at UINT_MAX - 1, never wrap to 0, which means off) without waiting for it.
// Gives 0 for an invalid handle.
unsigned chttpsvr__idle_timeout_ms_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  return atomic_load(&raw->idle_timeout_ms);
}

// Test hook: Copies into out every duration chttpsvr_start() resolved for h, in
// ms, in the order the header lists, so a test can pin the conversion from
// microseconds, the defaults and the form of CHTTPSVR_NO_DEADLINE without
// waiting. Gives false for an invalid handle.
bool chttpsvr__resolved_ms_for_tests(chttpsvr h, unsigned out[9]) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
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

// Test hook: The in_flight_requests count of h, read under raw->mutex like
// everywhere else, so a test can wait until a request has really reached a
// worker instead of sleeping. Gives -1 for an invalid handle.
int chttpsvr__in_flight_requests_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return -1;
  ccol__mutex_lock(raw->mutex);
  int n = raw->in_flight_requests;
  ccol__mutex_unlock(raw->mutex);
  return n;
}

// Test hook: Marks the calling thread as a worker of h through the worker key,
// so a test in a forked child can reach the self-call guard of
// chttpsvr_destroy() without a live reactor (fork() copies none of its
// threads). There is no unmark, since the child ends right after. Does nothing
// when h doesn't resolve.
void chttpsvr__mark_self_as_worker_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (raw && chttpsvr_worker_key_ensure()) chttpsvr_mark_worker_thread(raw);
}

// Test hooks for the body-memory and streaming-queue state of h, so the
// slow-client tests can see how much memory is reserved, who holds it, who
// waits for it and what the streaming queue holds. Each reads its fields under
// raw->wait_mutex (the pool fields under raw->mutex) and gives 0 or false for
// an invalid handle. mem_holders also reports in *exempt_out whether one holder
// runs past the limit under the progress rule.
size_t chttpsvr__mem_in_use_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  ccol__mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_in_use;
  ccol__mutex_unlock(raw->wait_mutex);
  return n;
}

size_t chttpsvr__mem_holders_for_tests(chttpsvr h, bool *exempt_out) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (exempt_out) *exempt_out = false;
  if (!raw) return 0;
  ccol__mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_holders;
  if (exempt_out) *exempt_out = raw->mem_exempt_active;
  ccol__mutex_unlock(raw->wait_mutex);
  return n;
}

// Test hook: For the request a buffered handler holds, the body memory
// reservation of its connection (read under wait_mutex) and the capacity of its
// body buffer, so a test can check that the reservation covers the buffer.
void chttpsvr__req_body_memory_for_tests(chttpsvr_req *req, size_t *charged,
                                         size_t *held) {
  chttpsvr_conn *conn = req->conn;
  ccol__mutex_lock(conn->srv->wait_mutex);
  *charged = conn->mem_charged;
  ccol__mutex_unlock(conn->srv->wait_mutex);
  *held = conn->body.cap;
}

size_t chttpsvr__mem_waiting_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  ccol__mutex_lock(raw->wait_mutex);
  size_t n = raw->mem_waiting;
  ccol__mutex_unlock(raw->wait_mutex);
  return n;
}

size_t chttpsvr__stream_queue_len_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  ccol__mutex_lock(raw->wait_mutex);
  size_t n = raw->sq_len;
  ccol__mutex_unlock(raw->wait_mutex);
  return n;
}

bool chttpsvr__stream_pool_created_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return false;
  ccol__mutex_lock(raw->mutex);
  bool created = raw->stream_pool != CTHRPOOL_NULL;
  ccol__mutex_unlock(raw->mutex);
  return created;
}

// Test hook: How many tasks the worker pool, or the streaming pool, of h runs
// right now, so a test can check that a parked connection holds no thread.
size_t chttpsvr__pool_active_for_tests(chttpsvr h, bool streaming) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  ccol__mutex_lock(raw->mutex);
  cthrpool pool = streaming ? raw->stream_pool : raw->worker_pool;
  ccol__mutex_unlock(raw->mutex);
  return pool ? cthrpool_active_count(pool) : 0;
}

// Test hook: How many connections of h are parked on a body (kind 1) or on a
// response (kind 2), counted on the parked list under idle_mutex.
size_t chttpsvr__parked_count_for_tests(chttpsvr h, int kind) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol__mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn *c = raw->parked_head; c; c = c->parked_next)
    if (c->park == (uint8_t)kind) n++;
  ccol__mutex_unlock(raw->idle_mutex);
  return n;
}

// Test hook: A rough measure of what the connections parked on a response or
// lingering still hold (body capacity, one per header field, one per target,
// query and parameter array, plus the response for a lingering one), counted
// under idle_mutex. A test expects 0, because that state must be dropped when a
// write parks or lingers.
size_t chttpsvr__parked_request_state_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol__mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn *c = raw->parked_head; c; c = c->parked_next) {
    if (c->park != CONN_PARK_WRITE && c->park != CONN_PARK_LINGER) continue;
    n += c->body.cap + c->hdr_count + (c->path != NULL) +
         (c->decoded_path != NULL) + (c->raw_query != NULL) +
         (c->matched_param_values != NULL);
    if (c->park == CONN_PARK_LINGER)
      n += c->resp.body_cap + c->resp.header_count;
  }
  ccol__mutex_unlock(raw->idle_mutex);
  return n;
}

// Test hook: How many connections sit in the idle list of h, counted under
// idle_mutex.
size_t chttpsvr__idle_count_for_tests(chttpsvr h) {
  struct chttpserver *raw = chttpsvr__resolve_for_tests(h);
  if (!raw) return 0;
  size_t n = 0;
  ccol__mutex_lock(raw->idle_mutex);
  for (chttpsvr_conn *c = raw->idle_head; c; c = c->idle_next) n++;
  ccol__mutex_unlock(raw->idle_mutex);
  return n;
}

// Test hook: Runs one sweep tick for h on the calling thread with the clock
// read ahead_ms later, and gives how many connections it looked at. A test uses
// it to expire an idle limit without sleeping. The sweep thread keeps running;
// the claims of the lists make the two safe together.
size_t chttpsvr__sweep_ahead_for_tests(chttpsvr h, unsigned ahead_ms) {
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return 0;
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  timespec_add_ms(&now, ahead_ms);
  size_t visited = sweep_server(raw, now);
  chttpsvr_resolve_unpin(raw);
  return visited;
}

// Test hook: One sweep tick for h right away.
size_t chttpsvr__sweep_now_for_tests(chttpsvr h) {
  return chttpsvr__sweep_ahead_for_tests(h, 0);
}
#endif

// Asks the shared engine to stop every server; see engine_force_stop, which
// keeps it async-signal-safe so a signal handler can call it.
void chttpsvr_engine_stop(void) { engine_force_stop(); }

// Blocks until the shared engine has stopped and its reaper is joined, so
// main() can wait for a stop that a signal asked for. A call from a handler or
// middleware of any server is fatal: The engine stop drains every worker pool
// with no timeout, and this call would keep that drain from finishing.
void chttpsvr_engine_wait(void) {
  if (chttpsvr_is_any_worker_call()) {
    ccol__fatal_err(
        "chttpsvr_engine_wait: called from within a request handler (or "
        "middleware) running on some server's own worker pool; blocking "
        "here would deadlock the shared engine's shutdown, which can "
        "never finish draining this exact server's worker pool while "
        "this in-flight request is what would eventually let it return. "
        "Call chttpsvr_engine_wait() from a different thread instead.");
  }
  engine_wait_until_stopped();
}

// Adds a buffered route for method and pattern to the root router of h, so fn
// runs on the worker pool with the whole body in memory. It reads
// raw->root_router, which needs no lock, because a concurrent
// chttpsvr_subrouter() may realloc raw->routers. Gives ccol_invalid_args for a
// NULL pattern or fn or a bad handle, else the code of router_add_route.
ccol_retval chttpsvr_register_handler(chttpsvr h, chttp_method method,
                                      const char *pattern,
                                      chttpsvr_handler_fn fn, void *ctx) {
  if (!pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv =
      router_add_route(raw->root_router, method, pattern, fn, ctx, false);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Adds a streaming route to the root router of h, so fn runs on the streaming
// pool and reads the body with chttpsvr_req_read(). Same arguments and codes as
// chttpsvr_register_handler.
ccol_retval chttpsvr_register_streaming_handler(chttpsvr h, chttp_method method,
                                                const char *pattern,
                                                chttpsvr_handler_fn fn,
                                                void *ctx) {
  if (!pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv =
      router_add_route(raw->root_router, method, pattern, fn, ctx, true);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Adds fn as a global middleware of h (on its root router), so it runs before
// every handler of the server. Gives ccol_invalid_args for a NULL fn or a bad
// handle, else the code of router_add_mw.
ccol_retval chttpsvr_use(chttpsvr h, chttpsvr_middleware_fn fn, void *ctx) {
  if (!fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = router_add_mw(raw->root_router, fn, ctx);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Mounts a new sub-router of h at prefix and gives it, or NULL for a prefix
// that doesn't start with '/' or holds "//", a bad handle, or a failed
// allocation. A caller groups routes and middleware under a common path with
// it.
// Under the write side of raw->routes_lock it grows raw->routers when needed
// and inserts the router among the sub-routers (index 1 up; the root at index 0
// never moves) in descending order of prefix segment count, the order
// find_route tries them in; equal counts keep registration order. It also
// raises raw->max_prefix_seg_count. The router's owner field is set to h before
// it's visible.
chttpsvr_router *chttpsvr_subrouter(chttpsvr h, const char *prefix) {
  if (!prefix || prefix[0] != '/') return NULL;
  if (strstr(prefix, "//")) return NULL;

  struct chttpserver *raw = chttpsvr_resolve(h);
  if (!raw) return NULL;

  chttpsvr_router *r = create_router(raw, prefix, raw->m_procs);
  if (!r) {
    chttpsvr_resolve_unpin(raw);
    return NULL;
  }
  atomic_store(&r->owner, h);

  ccol__rw_lock_wrlock(raw->routes_lock);
  if (raw->router_count >= raw->router_cap) {
    if (raw->router_cap > (SIZE_MAX - 2) / 2 ||
        raw->router_cap * 2 + 2 > SIZE_MAX / sizeof(chttpsvr_router *)) {
      ccol__rw_lock_unlock(raw->routes_lock);
      destroy_router(r, raw->m_procs);
      chttpsvr_resolve_unpin(raw);
      return NULL;
    }
    size_t nc = raw->router_cap * 2 + 2;
    chttpsvr_router **nr = (chttpsvr_router **)ccol__mem_realloc(
        raw->m_procs, raw->routers, nc * sizeof(chttpsvr_router *));
    if (!nr) {
      ccol__rw_lock_unlock(raw->routes_lock);
      destroy_router(r, raw->m_procs);
      chttpsvr_resolve_unpin(raw);
      return NULL;
    }
    raw->routers = nr;
    raw->router_cap = nc;
  }
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
  if (r->prefix_seg_count > raw->max_prefix_seg_count)
    raw->max_prefix_seg_count = r->prefix_seg_count;
  ccol__rw_lock_unlock(raw->routes_lock);
  chttpsvr_resolve_unpin(raw);
  return r;
}

// Resolves and pins the server that owns router, for the chttpsvr_router_*
// calls. The router's shell is never freed, so its owner field is always safe
// to read. Gives NULL when the owner is gone; otherwise the caller must call
// chttpsvr_resolve_unpin() once.
static struct chttpserver *chttpsvr_router_reader_enter(
    chttpsvr_router *router) {
  chttpsvr owner = atomic_load(&router->owner);
  return chttpsvr_resolve(owner);
}

// Adds a buffered route to a sub-router while its server is pinned, so a
// destroyed server is refused with ccol_invalid_args instead of a write into
// freed memory.
ccol_retval chttpsvr_router_on(chttpsvr_router *router, chttp_method method,
                               const char *pattern, chttpsvr_handler_fn fn,
                               void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = router_add_route(router, method, pattern, fn, ctx, false);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Adds a streaming route to a sub-router; same rules as chttpsvr_router_on.
ccol_retval chttpsvr_router_on_stream(chttpsvr_router *router,
                                      chttp_method method, const char *pattern,
                                      chttpsvr_handler_fn fn, void *ctx) {
  if (!router || !pattern || !fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = router_add_route(router, method, pattern, fn, ctx, true);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Adds a middleware to a sub-router, which runs for every route mounted under
// its prefix; same rules as chttpsvr_router_on.
ccol_retval chttpsvr_router_use(chttpsvr_router *router,
                                chttpsvr_middleware_fn fn, void *ctx) {
  if (!router || !fn) return ccol_invalid_args;
  struct chttpserver *raw = chttpsvr_router_reader_enter(router);
  if (!raw) return ccol_invalid_args;
  ccol_retval rv = router_add_mw(router, fn, ctx);
  chttpsvr_resolve_unpin(raw);
  return rv;
}

// Gives the method of the request (CHTTP_GET for a NULL req).
chttp_method chttpsvr_req_method(const chttpsvr_req *req) {
  if (!req) return CHTTP_GET;
  return req->conn->method;
}

// Gives the percent-decoded path of the request, which the router matched
// against.
const char *chttpsvr_req_path(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->conn->decoded_path;
}

// The public digest buffer is filled by ctls, so both lengths must agree.
_Static_assert(CHTTPSVR_PEER_CERT_SHA256_LEN == CTLS_SHA256_LEN,
               "the public digest length must match the one of ctls");

// Tells whether the TLS client presented a certificate that the trust store
// verified, so a handler can refuse anonymous clients when client certificates
// are optional.
bool chttpsvr_req_peer_cert_verified(const chttpsvr_req *req) {
  if (!req) return false;
  return req->conn->tls && ctls_conn_peer_cert_verified(req->conn->tls);
}

// Gives the DER bytes of the verified client certificate and their length in
// *len_out, or NULL (and 0) when there is none, so a handler can identify the
// client. The bytes belong to the connection.
const void *chttpsvr_req_peer_cert_der(const chttpsvr_req *req,
                                       size_t *len_out) {
  if (len_out) *len_out = 0;
  if (!req || !req->conn->tls) return NULL;
  return ctls_conn_peer_cert_der(req->conn->tls, len_out);
}

// Writes the SHA-256 digest of the verified client certificate to out, so a
// handler can pin a client by fingerprint. Gives ccol_key_not_found when there
// is no verified certificate, ccol_not_enough_memory when the digest can't be
// computed, ccol_invalid_args for NULL arguments; out is zeroed on failure.
ccol_retval chttpsvr_req_peer_cert_sha256(
    const chttpsvr_req *req, unsigned char out[CHTTPSVR_PEER_CERT_SHA256_LEN]) {
  if (out) memset(out, 0, CHTTPSVR_PEER_CERT_SHA256_LEN);
  if (!req || !out) return ccol_invalid_args;
  if (!req->conn->tls || !ctls_conn_peer_cert_verified(req->conn->tls))
    return ccol_key_not_found;
  return ctls_conn_peer_cert_sha256(req->conn->tls, out)
             ? ccol_success
             : ccol_not_enough_memory;
}

// Gives the subject name of the verified client certificate, or NULL, so a
// handler can log or authorize by name.
const char *chttpsvr_req_peer_cert_subject(const chttpsvr_req *req) {
  if (!req || !req->conn->tls) return NULL;
  return ctls_conn_peer_cert_subject(req->conn->tls);
}

// Gives the value of the request header name, compared without letter case, or
// NULL. When a name repeats, every occurrence is kept in arrival order and the
// last one wins, so we scan backwards.
const char *chttpsvr_req_header(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn *conn = req->conn;
  for (size_t i = conn->hdr_count; i-- > 0;) {
    if (ccol_ascii_casecmp(conn->hdr_names[i], name) == 0)
      return conn->hdr_values[i];
  }
  return NULL;
}

// Gives the body of a buffered route and its length in *len_out, so a handler
// can read it as one stable buffer. A streaming route gets NULL and 0: Its
// conn->body is a live read cursor that chttpsvr_req_read() drains, and its
// pointer and length would mix delivered and undelivered bytes. conn and the
// matched route are checked as a defence, though documented use never gives
// NULL there.
const void *chttpsvr_req_body(const chttpsvr_req *req, size_t *len_out) {
  if (!req) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  chttpsvr_conn *conn = req->conn;
  if (!conn || !conn->matched_route || conn->matched_route->is_streaming) {
    if (len_out) *len_out = 0;
    return NULL;
  }
  if (len_out) *len_out = conn->body.len;
  return conn->body.buf;
}

// Gives the value of the named path parameter of the matched route (":name" in
// its pattern), or NULL, so a handler can read the parts of the path it routed
// on.
const char *chttpsvr_req_param(const chttpsvr_req *req, const char *name) {
  if (!req || !name) return NULL;
  chttpsvr_conn *conn = req->conn;
  if (!conn || !conn->matched_route) return NULL;
  int count = conn->matched_route->param_count;
  for (int i = 0; i < count; i++) {
    if (req->param_names[i] && strcmp(req->param_names[i], name) == 0) {
      return conn->matched_param_values[i];
    }
  }
  return NULL;
}

// Gives a NULL-terminated array of every value of the query key key and their
// number in *count_out, or NULL with 0 when the key is absent or memory runs
// out. A handler uses it for a key that may repeat. The query is parsed once
// per request (ensure_qparams); the array lives in req->qresult, which the next
// call reuses. A failed growth sets req->qresult_oom, which
// chttpsvr_req_query_oom() reports.
const char **chttpsvr_req_query(chttpsvr_req *req, const char *key,
                                size_t *count_out) {
  if (!req || !key) {
    if (count_out) *count_out = 0;
    return NULL;
  }

  chttpsvr_qparams *qp = ensure_qparams(req);
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

  if (n + 1 > req->qresult_cap) {
    const char **nr = (const char **)ccol__mem_realloc(
        req->m_procs, req->qresult, (n + 1) * sizeof(char *));
    if (!nr) {
      req->qresult_oom = true;
      if (count_out) *count_out = 0;
      return NULL;
    }
    req->qresult = nr;
    req->qresult_cap = n + 1;
  }
  size_t idx = 0;
  for (size_t i = 0; i < qp->count; i++) {
    if (strcmp(qp->keys[i], key) == 0) req->qresult[idx++] = qp->values[i];
  }
  req->qresult[idx] = NULL;
  if (count_out) *count_out = n;
  return req->qresult;
}

// Gives in *val_out the one value of the query key key, for a handler that
// expects a key at most once. Gives ccol_key_not_found when it's absent,
// ccol_key_already_present when it repeats, ccol_not_enough_memory when the
// query couldn't be parsed, ccol_invalid_args for NULL arguments; *val_out is
// NULL on every failure.
ccol_retval chttpsvr_req_query_one(chttpsvr_req *req, const char *key,
                                   const char **val_out) {
  if (!req || !key) {
    if (val_out) *val_out = NULL;
    return ccol_invalid_args;
  }

  chttpsvr_qparams *qp = ensure_qparams(req);
  if (!qp) {
    if (val_out) *val_out = NULL;
    return ccol_not_enough_memory;
  }
  if (req->qparams_parse_oom) {
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

// Gives the query string as it arrived, undecoded, or NULL when the target had
// none.
const char *chttpsvr_req_raw_query(const chttpsvr_req *req) {
  if (!req) return NULL;
  return req->conn->raw_query;
}

// Tells whether a query lookup of this request ran out of memory, so a handler
// can tell "absent" from "not enough memory" after a NULL from
// chttpsvr_req_query().
bool chttpsvr_req_query_oom(const chttpsvr_req *req) {
  if (!req) return false;
  return req->qresult_oom || req->qparams_parse_oom;
}

// Starting capacity of the overflow block of chttpsvr_req_set_context(), which
// doubles from there.
#define CHTTPSVR_CTX_MORE_FIRST_CAP 4u

// Stores value under the address key in the request, replacing the value of a
// key already there, so middleware can pass data to the handler. The first pair
// lives inside the request with no allocation; later pairs go into
// req->ctx_more, a doubling block from the server's allocator that the request
// scratch frees. Gives ccol_invalid_args for NULL arguments and
// ccol_not_enough_memory when the block can't grow.
ccol_retval chttpsvr_req_set_context(chttpsvr_req *req, const void *key,
                                     void *value) {
  if (!req || !key) return ccol_invalid_args;
  if (!req->ctx_first.key || req->ctx_first.key == key) {
    req->ctx_first.key = key;
    req->ctx_first.value = value;
    return ccol_success;
  }
  chttpsvr_ctx_more *more = req->ctx_more;
  size_t count = more ? more->count : 0;
  for (size_t i = 0; i < count; i++) {
    if (more->pairs[i].key == key) {
      more->pairs[i].value = value;
      return ccol_success;
    }
  }
  if (!more || count == more->cap) {
    size_t cap = more ? more->cap : 0;
    if (cap > (SIZE_MAX - sizeof(chttpsvr_ctx_more)) /
                  (2 * sizeof(chttpsvr_ctx_pair)))
      return ccol_not_enough_memory;
    size_t new_cap = cap ? cap * 2 : CHTTPSVR_CTX_MORE_FIRST_CAP;
    chttpsvr_ctx_more *grown = (chttpsvr_ctx_more *)ccol__mem_realloc(
        req->m_procs, more,
        sizeof(chttpsvr_ctx_more) + new_cap * sizeof(chttpsvr_ctx_pair));
    if (!grown) return ccol_not_enough_memory;
    if (!more) grown->count = 0;
    grown->cap = new_cap;
    req->ctx_more = more = grown;
  }
  more->pairs[more->count].key = key;
  more->pairs[more->count].value = value;
  more->count++;
  return ccol_success;
}

// Gives the value stored under key by chttpsvr_req_set_context(), or NULL.
void *chttpsvr_req_context(const chttpsvr_req *req, const void *key) {
  if (!req || !key) return NULL;
  if (req->ctx_first.key == key) return req->ctx_first.value;
  const chttpsvr_ctx_more *more = req->ctx_more;
  if (!more) return NULL;
  for (size_t i = 0; i < more->count; i++)
    if (more->pairs[i].key == key) return more->pairs[i].value;
  return NULL;
}

// Sets the status code of the response; send_response turns a code outside 200
// to 999 into 500.
void chttpsvr_resp_set_status(chttpsvr_resp *resp, int status_code) {
  if (resp) resp->status_code = status_code;
}

// Tells whether value is a Content-Length a response may carry: One or more
// ASCII digits (RFC 9110 section 8.6) whose number fits a signed 64-bit
// integer, the widest length that Go, curl and our own parser accept.
static bool resp_content_length_valid(const char *value) {
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

// Checks a response header before either setter stores it, because
// send_response writes name and value to the wire as they are. The name must be
// a non-empty token of tchar bytes (the classifier the parser uses), so it
// can't build a malformed line. The value may hold no control byte other than
// HTAB and no DEL, which stops CRLF injection when a handler reflects request
// data. Transfer-Encoding is refused, since the server never transfer-codes a
// body and the field would contradict its Content-Length, and a Content-Length
// must pass resp_content_length_valid. Gives ccol_success or ccol_invalid_args.
static ccol_retval resp_header_validate(const char *name, const char *value) {
  if (!*name) return ccol_invalid_args;
  for (const char *p = name; *p; p++) {
    if (!chttp1_is_tchar((unsigned char)*p)) return ccol_invalid_args;
  }
  for (const unsigned char *p = (const unsigned char *)value; *p; p++) {
    if ((*p < 0x20 && *p != '\t') || *p == 0x7F) return ccol_invalid_args;
  }
  if (ccol_ascii_casecmp(name, "transfer-encoding") == 0)
    return ccol_invalid_args;
  if (ccol_ascii_casecmp(name, "content-length") == 0 &&
      !resp_content_length_valid(value))
    return ccol_invalid_args;
  return ccol_success;
}

// Tells whether a response carries at most one field of this name:
// send_response decides Connection itself and reads a handler's Content-Length
// and Date as single values, so chttpsvr_resp_add_header replaces these instead
// of appending.
static bool resp_header_is_singleton(const char *name) {
  return ccol_ascii_casecmp(name, "connection") == 0 ||
         ccol_ascii_casecmp(name, "content-length") == 0 ||
         ccol_ascii_casecmp(name, "date") == 0;
}

// Appends one field line with copies of name and value, growing resp->headers
// when needed, for chttpsvr_resp_add_header and for a first set. The caller has
// validated both. Gives ccol_not_enough_memory and changes nothing when a
// growth or a copy fails.
static ccol_retval resp_header_append(chttpsvr_resp *resp, const char *name,
                                      const char *value) {
  ccol_memmgmt_procs *mp = resp->m_procs;
  if (resp->header_count >= resp->header_cap) {
    if (resp->header_cap > (SIZE_MAX - 8) / 2) return ccol_not_enough_memory;
    size_t new_cap = resp->header_cap * 2 + 8;
    if (new_cap > SIZE_MAX / sizeof(resp_header)) return ccol_not_enough_memory;
    resp_header *nh = (resp_header *)ccol__mem_realloc(
        mp, resp->headers, new_cap * sizeof(resp_header));
    if (!nh) return ccol_not_enough_memory;
    resp->headers = nh;
    resp->header_cap = new_cap;
  }

  char *n_name = ccol_strdup(mp, name);
  char *n_val = ccol_strdup(mp, value);
  if (!n_name || !n_val) {
    ccol__mem_free(mp, n_name);
    ccol__mem_free(mp, n_val);
    return ccol_not_enough_memory;
  }
  resp->headers[resp->header_count].name = n_name;
  resp->headers[resp->header_count].value = n_val;
  resp->header_count++;
  return ccol_success;
}

// Makes value the single value of name, for chttpsvr_resp_set_header and the
// singleton fields. The first field of that name keeps its place and the letter
// case of its name, later ones are dropped, and the other fields keep their
// order. The new value is copied first, so a failed allocation leaves every
// field as it was. The caller has validated name and value.
static ccol_retval resp_header_replace(chttpsvr_resp *resp, const char *name,
                                       const char *value) {
  ccol_memmgmt_procs *mp = resp->m_procs;
  size_t first = resp->header_count;
  for (size_t i = 0; i < resp->header_count; i++) {
    if (ccol_ascii_casecmp(resp->headers[i].name, name) == 0) {
      first = i;
      break;
    }
  }
  if (first == resp->header_count) return resp_header_append(resp, name, value);

  char *new_val = ccol_strdup(mp, value);
  if (!new_val) return ccol_not_enough_memory;
  ccol__mem_free(mp, resp->headers[first].value);
  resp->headers[first].value = new_val;
  size_t kept = first + 1;
  for (size_t i = first + 1; i < resp->header_count; i++) {
    if (ccol_ascii_casecmp(resp->headers[i].name, name) == 0) {
      ccol__mem_free(mp, resp->headers[i].name);
      ccol__mem_free(mp, resp->headers[i].value);
      continue;
    }
    resp->headers[kept++] = resp->headers[i];
  }
  resp->header_count = kept;
  return ccol_success;
}

// Validates name and value and makes value the only field of that name in the
// response, replacing earlier ones. Gives ccol_invalid_args for NULL or refused
// arguments and ccol_not_enough_memory when a copy fails.
ccol_retval chttpsvr_resp_set_header(chttpsvr_resp *resp, const char *name,
                                     const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_retval rv = resp_header_validate(name, value);
  if (rv != ccol_success) return rv;
  return resp_header_replace(resp, name, value);
}

// Validates name and value and appends one more field line, for a field that
// may repeat such as Set-Cookie. Connection, Content-Length and Date stay
// single-valued, so for them it replaces. Same codes as
// chttpsvr_resp_set_header.
ccol_retval chttpsvr_resp_add_header(chttpsvr_resp *resp, const char *name,
                                     const char *value) {
  if (!resp || !name || !value) return ccol_invalid_args;
  ccol_retval rv = resp_header_validate(name, value);
  if (rv != ccol_success) return rv;
  if (resp_header_is_singleton(name))
    return resp_header_replace(resp, name, value);
  return resp_header_append(resp, name, value);
}

// Appends len bytes of data to the response body, which send_response writes
// after the handler returns. The buffer grows to at least twice its size plus
// 256 bytes, so many small writes stay cheap. Gives ccol_invalid_args for a
// NULL resp or NULL data with a non-zero len, and ccol_not_enough_memory when
// the growth fails or would overflow; the body is unchanged then.
ccol_retval chttpsvr_resp_write(chttpsvr_resp *resp, const void *data,
                                size_t len) {
  if (!resp || (!data && len > 0)) return ccol_invalid_args;
  if (len == 0) return ccol_success;

  ccol_memmgmt_procs *mp = resp->m_procs;

  if (len > resp->body_cap - resp->body_len) {
    if (len > SIZE_MAX - resp->body_len) return ccol_not_enough_memory;
    size_t min_cap = resp->body_len + len;
    size_t new_cap = min_cap;
    if (resp->body_cap <= (SIZE_MAX - 256) / 2) {
      size_t doubled = resp->body_cap * 2 + 256;
      if (doubled > new_cap) new_cap = doubled;
    }
    char *nb = (char *)ccol__mem_realloc(mp, resp->body, new_cap);
    if (!nb) return ccol_not_enough_memory;
    resp->body = nb;
    resp->body_cap = new_cap;
  }
  memcpy(resp->body + resp->body_len, data, len);
  resp->body_len += len;
  return ccol_success;
}

// Appends the NUL-terminated str to the response body; see chttpsvr_resp_write.
ccol_retval chttpsvr_resp_write_str(chttpsvr_resp *resp, const char *str) {
  if (!resp || !str) return ccol_invalid_args;
  return chttpsvr_resp_write(resp, str, strlen(str));
}

// Formats with printf rules and appends the result to the response body. A
// result under 256 bytes is formatted on the stack; a longer one is formatted
// again into a heap buffer from the response's allocator. Gives
// ccol_invalid_args for NULL arguments or a format error,
// ccol_not_enough_memory when an allocation fails.
ccol_retval chttpsvr_resp_printf(chttpsvr_resp *resp, const char *fmt, ...) {
  if (!resp || !fmt) return ccol_invalid_args;

  ccol_memmgmt_procs *mp = resp->m_procs;
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

  char *heap_buf = (char *)ccol__mem_alloc(mp, (size_t)n + 1);
  if (!heap_buf) {
    va_end(ap2);
    return ccol_not_enough_memory;
  }
  int n2 = vsnprintf(heap_buf, (size_t)n + 1, fmt, ap2);
  va_end(ap2);
  if (n2 < 0) {
    ccol__mem_free(mp, heap_buf);
    return ccol_invalid_args;
  }
  ccol_retval rv = chttpsvr_resp_write(resp, heap_buf, (size_t)n2);
  ccol__mem_free(mp, heap_buf);
  return rv;
}

// Appends len bytes of JSON text to the body and sets Content-Type to
// application/json, so a handler that answers JSON needs one call. Gives
// ccol_invalid_args for NULL arguments or a len of 0, else the code of the
// write or the header set.
ccol_retval chttpsvr_resp_write_json(chttpsvr_resp *resp, const char *json,
                                     size_t len) {
  if (!resp || !json || len == 0) return ccol_invalid_args;
  ccol_retval rv = chttpsvr_resp_write(resp, json, len);
  if (rv != ccol_success) return rv;
  return chttpsvr_resp_set_header(resp, "content-type", "application/json");
}

#ifdef RUNNING_UNIT_TESTS
// Test hook: Runs the exit-time destructors of the module now (router shells,
// slot table, stop watcher), in a forked child, so the test can then destroy a
// server as a late application destructor would.
void chttpsvr__run_exit_cleanup_for_tests(void) {
  cleanup_chttpsvr_router_shells();
  cleanup_chttpsvr_slot_table();
  cleanup_engine_stop_watcher();
}

// Test hook: How many router shells the registry holds and whether its array is
// allocated, read under the registry mutex, so a test can see what a destroy
// and the exit cleanup free.
void chttpsvr__router_shell_registry_state_for_tests(size_t *count,
                                                     bool *array) {
  ccol__call_once(chttpsvr_router_shell_registry.once,
                  chttpsvr_router_shell_registry_init_globals);
  ccol__mutex_lock(chttpsvr_router_shell_registry.mutex);
  *count = chttpsvr_router_shell_registry.count;
  *array = chttpsvr_router_shell_registry.shells != NULL;
  ccol__mutex_unlock(chttpsvr_router_shell_registry.mutex);
}

// Test hook: true once the arrays of the slot table have been released, read
// under its mutex.
bool chttpsvr__slot_table_released_for_tests(void) {
  ccol__call_once(chttpsvr_slot_table.once, chttpsvr_slot_table_init_globals);
  ccol__mutex_lock(chttpsvr_slot_table.mutex);
  bool released = chttpsvr_slot_table.slots == NULL;
  ccol__mutex_unlock(chttpsvr_slot_table.mutex);
  return released;
}

// Test hook: true while a logger of chttpsvr_set_engine_logger() is installed,
// read under the engine mutex.
bool chttpsvr__engine_user_log_set_for_tests(void) {
  ccol__call_once(srv_engine_bundler.once, engine_globals_init);
  ccol__mutex_lock(srv_engine_bundler.mutex);
  bool set = srv_engine_bundler.user_log != CLOG_NULL;
  ccol__mutex_unlock(srv_engine_bundler.mutex);
  return set;
}
#endif
